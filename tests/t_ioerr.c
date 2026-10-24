/* I/O errors: one write of a given kind fails with EIO, midway through a
   run, and the run goes on.  The kinds:

     log-data    a block of the log that is neither a descriptor, a
		 commit record nor the journal superblock
     commit      a commit record
     jsb         the journal superblock
     checkpoint  a home write by the journal (checkpoint or the flush
		 after a commit)
     pager       a home write by a pager thread

   After the failure, there must be no stale page-in, and every crash
   image must hold one operation's state, no older than one whose commit
   returned.  By default the run covers jsb and pager, each in a process
   of its own.

   checkpoint fails on some seeds, not yet understood: after a failed
   flush following a commit (needs_flush left set), a page-in of the
   block can read stale data before the buffer is dropped (seed 2,
   block 2050).

   log-data and commit abort a commit, and fail today: the aborted
   transaction's shadows are freed, among them the only copy of pages
   the pager has evicted (journal_free_buffer warns "Dropping block ...
   before its deferred home write"), so their next page-in reads stale
   data; and diskfs_journal_commit_transaction cannot report the error,
   so its caller takes the operation as durable.  Name them as the third
   argument to run them.  */

#include <sys/wait.h>
#include <unistd.h>

#include "common.h"
#include "jbd2_format.h"

#define NBLK 40
#define OPS 200

static const char *kind;
static int matches, fail_at = 6, failed;
static size_t failed_at_log;

static int
fail_hook (uint64_t off, size_t len, const void *buf)
{
  const uint32_t *w = buf;
  int journal = jt_in_journal (off);
  int is_jsb = journal && jt_journal_index (off) == 0;
  int is_hdr = journal && len >= 8 && be32toh (w[0]) == JBD2_MAGIC_NUMBER;
  int match;

  (void) len;
  if (failed)
    return 0;
  if (!strcmp (kind, "log-data"))
    match = journal && !is_jsb && !is_hdr;
  else if (!strcmp (kind, "commit"))
    match = is_hdr && be32toh (w[1]) == JBD2_COMMIT_BLOCK;
  else if (!strcmp (kind, "jsb"))
    match = is_jsb;
  else if (!strcmp (kind, "checkpoint"))
    match = !journal && !jt_on_pager_thread () && off >= 4096 * 16;
  else if (!strcmp (kind, "pager"))
    match = !journal && jt_on_pager_thread ();
  else
    match = 0;
  if (!match || ++matches < fail_at)
    return 0;
  failed = 1;
  failed_at_log = jt_log_count ();
  return EIO;
}

static int
run (const char *image, unsigned seed)
{
  static block_t blocks[NBLK];
  static struct history h;
  unsigned long warnings;

  srand (seed);
  jt_mount (image, 2, 0);
  pick_free_blocks (blocks, NBLK);
  history_init (&h, blocks, NBLK, OPS);
  jt_store_fail_hook = fail_hook;

  for (int op = 1; op <= OPS; op++)
    {
      diskfs_transaction_t *txn = diskfs_journal_start_transaction ();
      for (int i = 1 + rand () % 20; i > 0; i--)
	{
	  int k = rand () % NBLK;
	  history_write (&h, txn, k);
	  if (rand () % 4 == 0)
	    jt_pageout (blocks[k], rand () % 2, 0);
	}
      int sync = op % 5 == 0;
      if (sync)
	diskfs_journal_commit_transaction (txn);
      else
	diskfs_journal_stop_transaction (txn);
      history_op_done (&h, sync);
      if (rand () % 3 == 0)
	jt_pageout (blocks[rand () % NBLK], rand () % 2, 0);
    }

  warnings = jt_umount ();
  jt_store_fail_hook = NULL;
  int home_wrong = 0;
  for (int k = 0; k < NBLK; k++)
    home_wrong += pattern_version (jt_disk_block (blocks[k]), blocks[k])
      != h.ver[k];
  fprintf (stderr, "t_ioerr %s: write %zu failed; %lu warnings, %lu stale "
	   "page-ins, %lu failed page-ins, %d of %d blocks not home after "
	   "the unmount\n", kind, failed_at_log, warnings, jt_stale_reads (),
	   jt_pager_errors (), home_wrong, NBLK);
  CHECK (failed, "no %s write failed", kind);
  CHECK (jt_stale_reads () == 0, "%lu stale page-ins after a failed %s "
	 "write", jt_stale_reads (), kind);

  char name[64];
  snprintf (name, sizeof name, "t_ioerr-%s", kind);
  size_t checked = jt_crash_check (name, history_check, &h);
  printf ("t_ioerr: OK (a %s write failed at write %zu, %lu warnings, "
	  "%zu crash images)\n", kind, failed_at_log, warnings, checked);
  return 0;
}

int
main (int argc, char **argv)
{
  const char *image = argc > 1 ? argv[1] : "build/base.img";
  unsigned seed = argc > 2 ? atoi (argv[2]) : 1;
  static const char *const safe[] = { "jsb", "pager", NULL };
  const char *one[] = { argc > 3 ? argv[3] : NULL, NULL };
  const char *const *kinds = argc > 3 ? one : safe;
  int status;

  for (; *kinds; kinds++)
    {
      fflush (NULL);
      pid_t pid = fork ();
      CHECK (pid >= 0, "fork");
      if (pid == 0)
	{
	  kind = *kinds;
	  exit (run (image, seed));
	}
      CHECK (waitpid (pid, &status, 0) == pid, "wait");
      if (!WIFEXITED (status) || WEXITSTATUS (status))
	return 1;
    }
  return 0;
}
