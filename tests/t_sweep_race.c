/* A handle that joins while the stop-time sweep copies, and edits the
   block being copied.

   The sweep in journal_stop_transaction_locked raises t_active_threads
   and drops the journal lock to copy the disk cache into the shadows.
   The transaction is still T_RUNNING, so a new handle can join and edit
   a block the sweep is copying: the copy is torn.  That must not reach
   the log.  The editor's journal_get_write_access sets needs_copy again,
   and the last handle to stop sweeps again with no editor left.

   The steps are forced:
   1. S journals version 1 of block B, and stops: the sweep lists B.
   2. The sweep is held right before it copies B.
   3. A joins the transaction, reserves B, and writes the first half of
      version 2.
   4. The sweep copies B (half version 2, half version 1) and returns.
   5. A writes the second half, marks B dirty, and commits.
   Then B is home whole after a clean unmount, and no crash image holds a
   torn B.  */

#include <pthread.h>
#include "common.h"

static block_t blk;

static pthread_mutex_t hold_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t hold_cond = PTHREAD_COND_INITIALIZER;
static int hold_armed, hold_reached, hold_released;

static void
hold_hook (block_t b)
{
  if (b != blk)
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

static diskfs_transaction_t *txn_s;

/* S: journal version 1 of B, and stop.  As the last handle, the stop
   sweeps, and is held in the copy of B.  Handles are per thread, so S
   starts and stops in a thread of its own.  */
static void *
thread_s (void *arg)
{
  (void) arg;
  txn_s = diskfs_journal_start_transaction ();
  journal_version (txn_s, blk, 1);
  diskfs_journal_stop_transaction (txn_s);
  return NULL;
}

static size_t commit_returned_at;

static const char *
check (size_t n, int torn, const char *img, void *arg)
{
  static char msg[128];
  FILE *f = fopen (img, "r");
  char *buf = malloc (block_size);
  long v;

  (void) torn;
  (void) arg;
  CHECK (f, "open %s", img);
  CHECK (!fseeko (f, (off_t) blk << log2_block_size, SEEK_SET)
	 && fread (buf, block_size, 1, f) == 1, "read");
  fclose (f);
  v = pattern_version (buf, blk);
  free (buf);

  /* Versions 1 and 2 are in the same transaction: only 2 commits.  */
  if (v != 0 && v != 2)
    snprintf (msg, sizeof msg, "block %u holds %s", blk,
	      v < 0 ? "a torn copy" : "version 1, which never committed");
  else if (n >= commit_returned_at && v != 2)
    snprintf (msg, sizeof msg, "version 2 lost after its commit returned");
  else
    return NULL;
  return msg;
}

int
main (int argc, char **argv)
{
  const char *image = argc > 1 ? argv[1] : "build/base.img";
  diskfs_transaction_t *a;
  pthread_t sweeper;
  char *buf;
  unsigned long warnings;

  jt_mount (image, 2, 0);
  pick_free_blocks (&blk, 1);
  buf = malloc (block_size);
  commit_returned_at = SIZE_MAX;

  /* 1, 2.  S journals version 1 and stops; its sweep is held before the
     copy of B, keeping the transaction's count up.  */
  pthread_mutex_lock (&hold_lock);
  hold_armed = 1;
  pthread_mutex_unlock (&hold_lock);
  jt_ref_mapped_hook = hold_hook;
  pthread_create (&sweeper, NULL, thread_s, NULL);
  pthread_mutex_lock (&hold_lock);
  while (!hold_reached)
    pthread_cond_wait (&hold_cond, &hold_lock);
  pthread_mutex_unlock (&hold_lock);

  /* 3.  A joins, reserves B, and writes half of version 2.  */
  a = diskfs_journal_start_transaction ();
  CHECK (a == txn_s, "A did not join the transaction being swept");
  CHECK (!journal_get_write_access (a, blk), "reserve");
  pattern (buf, blk, 2);
  jt_edit (blk, 0, buf, block_size / 2);

  /* 4.  The sweep copies B, half old and half new, and returns.  */
  pthread_mutex_lock (&hold_lock);
  hold_released = 1;
  pthread_cond_broadcast (&hold_cond);
  pthread_mutex_unlock (&hold_lock);
  pthread_join (sweeper, NULL);
  jt_ref_mapped_hook = NULL;

  /* The race happened: the shadow holds the torn copy.  */
  CHECK (!jt_shadow (a, blk, buf), "no shadow");
  CHECK (((uint32_t *) buf)[1] == 2
	 && ((uint32_t *) buf)[block_size / 8 + 1] == 1,
	 "the sweep did not copy the half-edited block (v%u/v%u)",
	 ((uint32_t *) buf)[1], ((uint32_t *) buf)[block_size / 8 + 1]);
  pattern (buf, blk, 2);

  /* 5.  A finishes the edit and commits.  */
  jt_edit (blk, block_size / 2, buf + block_size / 2, block_size / 2);
  journal_mark_dirty (a, blk);
  diskfs_journal_commit_transaction (a);
  commit_returned_at = jt_log_count ();

  warnings = jt_umount ();
  CHECK (warnings == 0, "%lu journal warnings", warnings);
  CHECK (jt_stale_reads () == 0, "%lu stale page-ins", jt_stale_reads ());
  CHECK (pattern_version (jt_disk_block (blk), blk) == 2,
	 "block %u: version %ld home, 2 expected", blk,
	 pattern_version (jt_disk_block (blk), blk));

  size_t checked = jt_crash_check ("t_sweep_race", check, NULL);
  printf ("t_sweep_race: OK (%zu writes, %zu crash images)\n",
	  jt_log_count (), checked);
  free (buf);
  return 0;
}
