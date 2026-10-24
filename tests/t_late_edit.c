/* An edit made without a reservation while a commit writes the log.

   trunc_indirect edits a block just before it reserves it.  If that
   block is in the committing transaction T, and T is already writing
   its shadows to the log (T_FLUSHING), a pager write of the page brings
   bytes T's shadow does not have and must not get.  The hazard check
   then reserves the block in the running transaction R itself and puts
   the page in R's shadow, so that the edit commits with R.

   The steps are forced, not left to chance:
   1. T reserves and edits M and N to version 1, and its commit starts;
      the first write to the log is held, with T_FLUSHING.
   2. M's page is edited to version 2 without a reservation, and evicted.
   3. A handle on R reserves M, as trunc_indirect does after its edit,
      and edits N to version 2.
   4. The log write is let go; T commits, then R.
   M's page-in after the eviction must be version 2, and so must M at
   home after the unmount.  Every crash image must hold M and N at one
   version: 0, 1 or 2 (T and R are each all or nothing), at least 1 once
   T's commit returned, and 2 once R's did.

   With "dir" as the third argument, M is a directory block, edited
   through the file pager as dir.c does; N stays a disk cache block.  By
   default both run, each in a process of its own.  */

#include <pthread.h>
#include <sys/wait.h>
#include <unistd.h>

#include "common.h"

static int dir_mode;
static block_t m_blk, n_blk;
static diskfs_transaction_t *txn_t;
static size_t t_returned_at, r_returned_at;

/* The held log write.  */
static pthread_mutex_t hold_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t hold_cond = PTHREAD_COND_INITIALIZER;
static int hold_armed, hold_reached, hold_released;

static void
hold_hook (uint64_t off, size_t len, const void *data)
{
  (void) len;
  (void) data;
  if (!jt_in_journal (off) || jt_journal_index (off) == 0)
    return;
  pthread_mutex_lock (&hold_lock);
  if (hold_armed)
    {
      hold_armed = 0;
      hold_reached = 1;
      pthread_cond_broadcast (&hold_cond);
      while (!hold_released)
	pthread_cond_wait (&hold_cond, &hold_lock);
    }
  pthread_mutex_unlock (&hold_lock);
}

static void m_version (diskfs_transaction_t *txn, uint32_t v);

/* Step 1: T, on a thread of its own, as handles are per thread.  */
static void *
committer (void *arg)
{
  (void) arg;
  txn_t = diskfs_journal_start_transaction ();
  m_version (txn_t, 1);
  journal_version (txn_t, n_blk, 1);
  CHECK (!diskfs_journal_commit_transaction (txn_t), "T's commit");
  t_returned_at = jt_log_count ();
  return NULL;
}

/* Version V of M, reserved and marked in TXN.  */
static void
m_version (diskfs_transaction_t *txn, uint32_t v)
{
  char buf[block_size];

  if (!dir_mode)
    {
      journal_version (txn, m_blk, v);
      return;
    }
  CHECK (!journal_get_write_access_external (txn, m_blk), "reserve M");
  pattern (buf, m_blk, v);
  jt_dir_edit (m_blk, 0, buf, block_size);
  CHECK (!jt_dir_mark_dirty (txn, m_blk), "mark M");
}

static long
image_version (FILE *f, block_t b)
{
  char buf[block_size];

  CHECK (!fseeko (f, (off_t) b << log2_block_size, SEEK_SET)
	 && fread (buf, block_size, 1, f) == 1, "read");
  return pattern_version (buf, b);
}

static const char *
check (size_t n, int torn, const char *img, void *arg)
{
  static char msg[160];
  FILE *f = fopen (img, "r");

  (void) torn;
  (void) arg;
  CHECK (f, "open %s", img);
  long vm = image_version (f, m_blk), vn = image_version (f, n_blk);
  fclose (f);

  long need = r_returned_at && n >= r_returned_at ? 2
    : t_returned_at && n >= t_returned_at ? 1 : 0;
  if (vm != vn || vm < 0 || vm > 2)
    snprintf (msg, sizeof msg, "M v%ld with N v%ld: a transaction is split",
	      vm, vn);
  else if (vm < need)
    snprintf (msg, sizeof msg, "M and N v%ld, but the commit of v%ld "
	      "had returned", vm, need);
  else
    return NULL;
  return msg;
}

static int
run (const char *image)
{
  block_t picked[2];
  pthread_t c;

  jt_mount (image, 2, 0);
  char buf[block_size];		/* Known once mounted.  */
  jt_expected_warning = "changed without a reservation";
  pick_free_blocks (picked, 2);
  m_blk = picked[0];
  n_blk = picked[1];
  JT_SET_HOOK (jt_store_write_hook, hold_hook);

  /* 1.  T holds M and N at version 1; its commit writes the log, held.  */
  pthread_mutex_lock (&hold_lock);
  hold_armed = 1;
  pthread_mutex_unlock (&hold_lock);
  pthread_create (&c, NULL, committer, NULL);
  pthread_mutex_lock (&hold_lock);
  while (!hold_reached)
    pthread_cond_wait (&hold_cond, &hold_lock);
  pthread_mutex_unlock (&hold_lock);

  /* 2.  The edit without a reservation, and the page-out.  */
  pattern (buf, m_blk, 2);
  if (dir_mode)
    {
      jt_dir_edit (m_blk, 0, buf, block_size);
      jt_dir_pageout (m_blk, 1, 1);
    }
  else
    {
      jt_edit (m_blk, 0, buf, block_size);
      jt_pageout (m_blk, 1, 1);
    }

  /* 3.  The reservation the edit was for, in R, which also takes N.  */
  diskfs_transaction_t *txn_r = diskfs_journal_start_transaction ();
  CHECK (txn_r && txn_r != txn_t, "no handle on the next transaction");
  if (dir_mode)
    {
      CHECK (!journal_get_write_access_external (txn_r, m_blk), "reserve M");
      CHECK (!jt_dir_mark_dirty (txn_r, m_blk), "mark M");
      jt_dir_read (m_blk, buf);
    }
  else
    {
      CHECK (!journal_get_write_access (txn_r, m_blk), "reserve M");
      journal_mark_dirty (txn_r, m_blk);
      jt_read (m_blk, buf);
    }
  CHECK (pattern_version (buf, m_blk) == 2, "M's page-in after the "
	 "eviction holds v%ld, not the edit (v2)", pattern_version (buf, m_blk));
  journal_version (txn_r, n_blk, 2);

  /* 4.  T commits, then R.  */
  pthread_mutex_lock (&hold_lock);
  hold_released = 1;
  pthread_cond_broadcast (&hold_cond);
  pthread_mutex_unlock (&hold_lock);
  pthread_join (c, NULL);
  JT_SET_HOOK (jt_store_write_hook, NULL);
  CHECK (!diskfs_journal_commit_transaction (txn_r), "R's commit");
  r_returned_at = jt_log_count ();

  unsigned long warnings = jt_umount ();
  CHECK (warnings == 0, "%lu journal warnings", warnings);
  CHECK (jt_stale_reads () == 0, "%lu stale page-ins", jt_stale_reads ());
  long vm = pattern_version (jt_disk_block (m_blk), m_blk);
  long vn = pattern_version (jt_disk_block (n_blk), n_blk);
  CHECK (vm == 2 && vn == 2, "M v%ld and N v%ld home after the unmount, "
	 "v2 expected", vm, vn);

  size_t checked = jt_crash_check (dir_mode ? "t_late_edit-dir"
				   : "t_late_edit", check, NULL);
  printf ("t_late_edit: OK (%s: %zu writes, %zu crash images)\n",
	  dir_mode ? "directory block" : "disk cache block", jt_log_count (),
	  checked);
  return 0;
}

int
main (int argc, char **argv)
{
  const char *image = argc > 1 ? argv[1] : "build/base.img";
  int status;

  for (int mode = 0; mode < 2; mode++)
    {
      if (argc > 3 && mode != !strcmp (argv[3], "dir"))
	continue;
      fflush (NULL);
      pid_t pid = fork ();
      CHECK (pid >= 0, "fork");
      if (pid == 0)
	{
	  dir_mode = mode;
	  exit (run (image));
	}
      CHECK (waitpid (pid, &status, 0) == pid, "wait");
      if (!WIFEXITED (status) || WEXITSTATUS (status))
	return 1;
    }
  return 0;
}
