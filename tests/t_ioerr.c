/* I/O errors: one write of a given kind fails with EIO, midway through a
   run, and the run goes on.  The kinds:

     log-data    a block of the log that is neither a descriptor, a
		 commit record nor the journal superblock
     commit      a commit record
     jsb         the journal superblock
     jsb-anchor  the journal superblock a commit into an empty log
		 writes, before the commit is durable: recovery would
		 not see the commit, which must then fail
     checkpoint  a home write by the journal: the flush after a commit,
		 as this test's log never fills (t_wrap fails the
		 checkpoint's)
     pager       a home write by a pager thread
     pager-run   a home write of several directory blocks at once by a
		 pager thread (file_pager_write_pages)

   The last NDIR blocks are a directory's: edited as dir.c does, through
   the file pager, and written out by it, often several at once.

   After the failure, there must be no stale page-in, and every crash
   image must hold one operation's state, no older than one whose commit
   returned.  By default the run covers every kind, each in a process
   of its own.

   log-data and commit abort a commit, and so does jsb when the write
   that fails is the superblock a commit into an empty log writes.  The journal must then stop: the
   file system goes read-only, every commit from then on returns the
   error, and the aborted transaction's shadows still serve reads (they
   are the only copy of pages the pager has evicted).  An operation whose
   commit failed is not taken as durable, and the unmounted journal must
   not be usable until e2fsck has seen it.  They need a journal.c whose
   diskfs_journal_commit_transaction returns the error, and are left out
   otherwise.  */

#include <sys/wait.h>
#include <unistd.h>

#include "common.h"
#include "jbd2_format.h"

#define NBLK 40
#define OPS 200
/* jsb and pager-run: operations after OPS, while no write of the kind
   has failed yet (see run).  */
#define EXTRA_OPS 100

static const char *kind;
static int matches, fail_at = 6, failed, aborts;
/* jsb-anchor: the log start the disk's journal superblock has.  */
static uint32_t disk_start;
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
  if (__atomic_load_n (&failed, __ATOMIC_ACQUIRE))
    return 0;
  if (!strcmp (kind, "log-data"))
    match = journal && !is_jsb && !is_hdr;
  else if (!strcmp (kind, "commit"))
    match = is_hdr && be32toh (w[1]) == JBD2_COMMIT_BLOCK;
  else if (!strcmp (kind, "jsb"))
    match = is_jsb;
  else if (!strcmp (kind, "jsb-anchor"))
    {
      /* The superblock a commit into an empty log writes: the log start
	 goes from 0 to the commit's.  */
      uint32_t start = is_jsb && len >= sizeof (journal_superblock_t)
	? be32toh (((const journal_superblock_t *) buf)->s_start) : 0;
      match = is_jsb && start
	&& __atomic_load_n (&disk_start, __ATOMIC_RELAXED) == 0;
      if (is_jsb && !(match && __atomic_load_n (&matches, __ATOMIC_RELAXED)
		      + 1 >= fail_at))
	__atomic_store_n (&disk_start, start, __ATOMIC_RELAXED);
    }
  else if (!strcmp (kind, "checkpoint"))
    match = !journal && !jt_on_pager_thread () && off >= 4096 * 16;
  else if (!strcmp (kind, "pager"))
    match = !journal && jt_on_pager_thread ();
  else if (!strcmp (kind, "pager-run"))
    match = !journal && jt_on_pager_thread () && len > 4096;
  else
    match = 0;
  if (!match || __atomic_add_fetch (&matches, 1, __ATOMIC_RELAXED) < fail_at)
    return 0;
  __atomic_store_n (&failed, 1, __ATOMIC_RELEASE);
  __atomic_store_n (&failed_at_log, jt_log_count (), __ATOMIC_RELAXED);
  return EIO;
}

static block_t blocks[NBLK];

#define NDIR 10

static int
is_dir (int k)
{
  return k >= NBLK - NDIR;
}

/* The next version of block K, in TXN.  */
static void
op_write (struct history *h, diskfs_transaction_t *txn, int k)
{
  char buf[block_size];
  block_t b = blocks[k];

  if (!is_dir (k))
    {
      history_write (h, txn, k);
      return;
    }
  CHECK (!journal_get_write_access_external (txn, b), "reserve %u", b);
  pattern (buf, b, ++h->ver[k]);
  jt_dir_edit (b, 0, buf, block_size);
  CHECK (!jt_dir_mark_dirty (txn, b), "mark %u", b);
}

static void
op_pageout (int k, int evict)
{
  if (is_dir (k))
    jt_dir_pageout (blocks[k], evict, 0);
  else
    jt_pageout (blocks[k], evict, 0);
}

static int
run (const char *image, unsigned seed)
{
  static struct history h;
  unsigned long warnings, n_failed_commits = 0;

  aborts = !strcmp (kind, "log-data") || !strcmp (kind, "commit")
    || !strcmp (kind, "jsb-anchor");
  /* The log is empty only now and then: the second time.  */
  if (!strcmp (kind, "jsb-anchor"))
    fail_at = 2;
  /* Each commit writes many log blocks: fail one about as far in as the
     sixth commit record.  */
  if (!strcmp (kind, "log-data"))
    fail_at = 200;
  /* Runs are fewer: a page-out takes the next blocks only half the time,
     and only if they are dirty too.  */
  if (!strcmp (kind, "pager-run"))
    fail_at = 2;
  CHECK (!aborts || JT_COMMIT_RETURNS, "%s needs a "
	 "diskfs_journal_commit_transaction that returns its error", kind);
  jt_inhibit_busy = 1;
  srand (seed);
  jt_mount (image, 2, 0);
  pick_free_blocks (blocks, NBLK);
  history_init (&h, blocks, NBLK, OPS + EXTRA_OPS);
  JT_SET_HOOK (jt_store_fail_hook, fail_hook);

  /* The superblock is written when the tail moves, as page-outs and
     checkpoints let transactions go, and how often depends on timing:
     fewer times than fail_at in OPS operations on some machines.  Then
     jsb goes on, each operation committed and the caches synced, which
     writes their blocks home and moves the tail, until one fails.  So
     does pager-run: a page-out of several pages goes home in pieces
     while some of its blocks are in a transaction that takes edits, so
     each operation there edits two neighbouring directory blocks,
     commits them, and pages them out as one run.  */
  int is_jsb = !strncmp (kind, "jsb", 3);
  int extend = is_jsb || !strcmp (kind, "pager-run");
  for (int op = 1;
       op <= OPS || (extend && op <= OPS + EXTRA_OPS
		     && !__atomic_load_n (&failed, __ATOMIC_ACQUIRE));
       op++)
    {
      diskfs_transaction_t *txn = diskfs_journal_start_transaction ();
      /* After an abort, edit only the first half of the blocks: the
	 others stay in the aborted transaction alone, and their evictions
	 must not go home.  */
      int seen = aborts && __atomic_load_n (&failed, __ATOMIC_ACQUIRE);
      /* After a failed write of directory blocks, leave them alone: a
	 write of them would release a claim the failure left.  */
      int no_dirs = !strcmp (kind, "pager-run")
	&& __atomic_load_n (&failed, __ATOMIC_ACQUIRE);
      for (int i = 1 + rand () % 20; i > 0; i--)
	{
	  int k = rand () % (seen ? NBLK / 2 : no_dirs ? NBLK - NDIR : NBLK);
	  op_write (&h, txn, k);
	  if (rand () % 4 == 0)
	    op_pageout (k, rand () % 2);
	}
      if (seen)
	{
	  /* Evict one of them and read it back.  */
	  static char buf[4096];
	  int k = NBLK / 2 + rand () % (NBLK / 2);
	  op_pageout (k, 1);
	  if (is_dir (k))
	    jt_dir_read (blocks[k], buf);
	  else
	    jt_read (blocks[k], buf);
	}
      int extra = op > OPS;
      int sync = op % 5 == 0 || extra;
      int run_k = -1;
      if (extra && !is_jsb)
	for (int k = NBLK - NDIR; k + 1 < NBLK && run_k < 0; k++)
	  if (blocks[k + 1] == blocks[k] + 1)
	    run_k = k;
      if (run_k >= 0)
	{
	  op_write (&h, txn, run_k);
	  op_write (&h, txn, run_k + 1);
	}
      int was_failed = __atomic_load_n (&failed, __ATOMIC_ACQUIRE);
      error_t err = 0;
      if (sync)
	err = jt_commit (txn);
      else
	diskfs_journal_stop_transaction (txn);
      /* A jsb write that failed was the superblock of a commit into an
	 empty log, if the journal aborted: recovery would not see that
	 commit, so it fails like a failed log write, here or in
	 kjournald.  From then on the run is one of the aborts.  */
      if (is_jsb && !aborts && __atomic_load_n (&failed, __ATOMIC_ACQUIRE)
	  && journal_aborted ())
	aborts = 1;
      /* Once the failed write is seen, the running transaction is after
	 the aborted one.  */
      if (sync && aborts && was_failed)
	CHECK (err == EIO, "a commit after the abort returned %s",
	       strerror (err));
      if (sync && !aborts)
	CHECK (!err, "commit: %s", strerror (err));
      if (err)
	n_failed_commits++;
      history_op_done (&h, sync && !err);
      if (run_k >= 0 && !err)
	jt_dir_pageout_run (blocks[run_k], 2, 0, 1);
      if (extra)
	{
	  jt_sync_all ();
	  /* kjournald writes the tail the sync moved.  */
	  usleep (20000);
	}
      if (rand () % 3 == 0)
	op_pageout (rand () % (no_dirs ? NBLK - NDIR : NBLK), rand () % 2);
    }

  if (is_jsb && !aborts && __atomic_load_n (&failed, __ATOMIC_ACQUIRE)
      && journal_aborted ())
    aborts = 1;

  /* Every claim a home write made is released, written or not.  */
  long claims;
  for (int tries = 0; tries < 1000; tries++)
    {
      jt_drain ();
      if ((claims = jt_flush_claims ()) >= 0)
	break;
      usleep (1000);
    }
  CHECK (claims == 0, "%ld blocks still claimed for home writes after a "
	 "failed %s write", claims, kind);

  /* kjournald makes the filesystem read-only, with the RPCs inhibited;
     the first try finds them inhibited already, and it tries again on
     its next tick.  */
  int ro, inhibited, waited = 0;
  while (!(ro = jt_readonly (&inhibited)) && aborts && waited++ < 150)
    usleep (100000);
  if (aborts)
    {
      CHECK (ro, "not read-only after the %s write failed", kind);
      CHECK (inhibited, "read-only, but not with the RPCs inhibited");
    }
  else
    CHECK (!ro, "read-only after a failed %s write", kind);
  warnings = jt_umount ();
  JT_SET_HOOK (jt_store_fail_hook, NULL);
  jt_save_image ("build/t_ioerr.img");
  /* An aborted journal must not be mounted again before e2fsck.  */
  if (aborts)
    CHECK (jt_journal_check () == JOURNAL_NEEDS_REPLAY,
	   "the journal of an unmount after an abort is usable");
  int home_wrong = 0;
  for (int k = 0; k < NBLK; k++)
    home_wrong += pattern_version (jt_disk_block (blocks[k]), blocks[k])
      != h.ver[k];
  fprintf (stderr, "t_ioerr %s: write %zu failed; %lu warnings, %lu stale "
	   "page-ins, %lu failed page-ins, %lu failed commits, %d of %d "
	   "blocks not home after the unmount, %lu page-outs of several "
	   "blocks\n", kind, failed_at_log, warnings, jt_stale_reads (),
	   jt_pager_errors (), n_failed_commits, home_wrong, NBLK,
	   jt_pageout_runs ());
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
  static const char *const all[] =
    { "jsb", "pager", "pager-run", "checkpoint", "log-data", "commit",
      "jsb-anchor", NULL };
  static const char *const no_aborts[] =
    { "jsb", "pager", "pager-run", "checkpoint", NULL };
  const char *one[] = { argc > 3 ? argv[3] : NULL, NULL };
  const char *const *kinds = argc > 3 ? one
    : JT_COMMIT_RETURNS ? all : no_aborts;
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
