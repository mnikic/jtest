/* The flush after a commit against a checkpoint writing an older copy.

   A checkpoint checks for a newer copy of each block before it writes
   the old one home, and a transaction that is still committing does not
   count: it is not durable yet.  If that transaction then commits and
   its flush writes its copy home before the checkpoint's write lands,
   the old copy lands over the new one, and both count as written: the
   new version is lost once the log moves past it.  The flush must wait
   for the checkpoint's write, as a pager write does.

   The steps are forced:
   1. T1 writes version 1 of B and commits; its copy waits in the
      checkpoint list.
   2. A checkpoint starts in another thread; its write of B is held.
   3. T2 writes version 2 of B, B's page goes out into T2's shadow, and
      T2 commits in another thread, so that its flush writes B home.
   4. The held write is let go.
   No home write of B may be older than one before it (the harness's
   home order check), version 2 must be home after the unmount, and every
   crash image must hold at least the version whose commit returned.  */

#include <pthread.h>
#include <unistd.h>
#include "common.h"

static block_t blk;
static size_t t1_at, t2_at = SIZE_MAX;

static pthread_mutex_t hold_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t hold_cond = PTHREAD_COND_INITIALIZER;
static int hold_reached, hold_released;

/* Hold the checkpoint's write of version 1 of B.  */
static void
hold_hook (uint64_t off, size_t len, const void *data)
{
  if (off != (uint64_t) blk << log2_block_size || len != block_size
      || pattern_version (data, blk) != 1 || jt_on_pager_thread ())
    return;
  pthread_mutex_lock (&hold_lock);
  hold_reached = 1;
  pthread_cond_broadcast (&hold_cond);
  while (!hold_released)
    pthread_cond_wait (&hold_cond, &hold_lock);
  pthread_mutex_unlock (&hold_lock);
}

static void *
checkpointer (void *arg)
{
  (void) arg;
  jt_checkpoint_now ();
  return NULL;
}

static void *
t2 (void *arg)
{
  (void) arg;
  diskfs_transaction_t *txn = diskfs_journal_start_transaction ();
  journal_version (txn, blk, 2);
  jt_pageout (blk, 1, 1);
  CHECK (!diskfs_journal_commit_transaction (txn), "T2's commit");
  __atomic_store_n (&t2_at, jt_log_count (), __ATOMIC_RELEASE);
  return NULL;
}

static const char *
check (size_t n, int torn, const char *img, void *arg)
{
  static char msg[128];
  char buf[block_size];
  FILE *f = fopen (img, "r");

  (void) torn;
  (void) arg;
  CHECK (f, "open %s", img);
  CHECK (!fseeko (f, (off_t) blk << log2_block_size, SEEK_SET)
	 && fread (buf, block_size, 1, f) == 1, "read");
  fclose (f);
  long v = pattern_version (buf, blk);
  long want = n >= t2_at ? 2 : n >= t1_at ? 1 : 0;
  if (v >= want)
    return NULL;
  snprintf (msg, sizeof msg, "block %u holds version %ld, but the commit "
	    "of version %ld had returned", blk, v, want);
  return msg;
}

int
main (int argc, char **argv)
{
  const char *image = argc > 1 ? argv[1] : "build/base.img";
  block_t picked[1];
  pthread_t c, t;

  jt_check_home_order = 1;
  jt_mount (image, 2, 0);
  pick_free_blocks (picked, 1);
  blk = picked[0];

  /* 1.  */
  diskfs_transaction_t *txn = diskfs_journal_start_transaction ();
  journal_version (txn, blk, 1);
  CHECK (!diskfs_journal_commit_transaction (txn), "T1's commit");
  t1_at = jt_log_count ();
  CHECK (jt_checkpoint_unwritten (blk), "B is home already");

  /* 2.  */
  JT_SET_HOOK (jt_store_write_hook, hold_hook);
  pthread_create (&c, NULL, checkpointer, NULL);
  pthread_mutex_lock (&hold_lock);
  while (!hold_reached)
    pthread_cond_wait (&hold_cond, &hold_lock);
  pthread_mutex_unlock (&hold_lock);

  /* 3.  */
  pthread_create (&t, NULL, t2, NULL);
  usleep (300000);

  /* 4.  */
  pthread_mutex_lock (&hold_lock);
  hold_released = 1;
  pthread_cond_broadcast (&hold_cond);
  pthread_mutex_unlock (&hold_lock);
  pthread_join (c, NULL);
  pthread_join (t, NULL);
  JT_SET_HOOK (jt_store_write_hook, NULL);

  unsigned long warnings = jt_umount ();
  CHECK (warnings == 0, "%lu journal warnings", warnings);
  CHECK (jt_stale_reads () == 0, "%lu stale page-ins", jt_stale_reads ());
  CHECK (pattern_version (jt_disk_block (blk), blk) == 2,
	 "block %u: version %ld home, 2 expected", blk,
	 pattern_version (jt_disk_block (blk), blk));
  size_t checked = jt_crash_check ("t_flush_order", check, NULL);
  printf ("t_flush_order: OK (%zu writes, %zu crash images)\n",
	  jt_log_count (), checked);
  return 0;
}
