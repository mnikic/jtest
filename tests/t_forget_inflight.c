/* A block freed while a home write of its old contents is in flight.

   T1 journals metadata block B and commits; its copy of B waits in the
   checkpoint list.  A page-out of B then writes it home, and claims
   T1's copy for that write (jb_is_flushing).  T2 frees B and commits:
   journal_forget_freed_blocks retires T1's copy, and must wait for that
   write before it hands B back, or B's next owner could write B and have
   the old contents land over its own.  t_forget_race has the old write
   start after the forget; here it is already in flight.

   The steps are forced:
   1. T1 journals B (version 1) and commits.
   2. A page-out of B starts; its write is held in store_write.
   3. Another thread: T2 frees B, journals a stand-in for the block
      bitmap, as free_blocks does, and commits.  While the write is
      held, B must not be handed back.
   4. The write is let go: T2's commit finishes, and B is handed back.
   5. T3 journals B (version 3) for its new owner; a page-out puts it in
      T3's shadow; T3 commits.
   Then version 3 must be home, and in every crash image after T3's
   commit returned.  */

#include <pthread.h>
#include <unistd.h>
#include "common.h"

static block_t blk, bitmap;
static size_t t3_returned_at = SIZE_MAX;
static int released, t2_done;

static pthread_mutex_t hold_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t hold_cond = PTHREAD_COND_INITIALIZER;
static int hold_reached, hold_released;

/* Hold the write of version 1 of B: T1's old contents.  */
static void
hold_hook (uint64_t off, size_t len, const void *data)
{
  if (off != (uint64_t) blk << log2_block_size || len != block_size
      || pattern_version (data, blk) != 1)
    return;
  pthread_mutex_lock (&hold_lock);
  hold_reached = 1;
  pthread_cond_broadcast (&hold_cond);
  while (!hold_released)
    pthread_cond_wait (&hold_cond, &hold_lock);
  pthread_mutex_unlock (&hold_lock);
}

static void
on_release (block_t start, unsigned long count)
{
  if (blk >= start && blk < start + count)
    __atomic_store_n (&released, 1, __ATOMIC_RELEASE);
}

/* 3.  */
static void *
t2 (void *arg)
{
  (void) arg;
  diskfs_transaction_t *txn = diskfs_journal_start_transaction ();
  CHECK (journal_record_freed_blocks (txn, blk, 1), "record the free");
  journal_version (txn, bitmap, 1);
  diskfs_journal_commit_transaction (txn);
  __atomic_store_n (&t2_done, 1, __ATOMIC_RELEASE);
  return NULL;
}

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
  if (n >= t3_returned_at && v != 3)
    {
      snprintf (msg, sizeof msg, "block %u holds version %ld after T3's "
		"commit returned", blk, v);
      return msg;
    }
  return NULL;
}

int
main (int argc, char **argv)
{
  const char *image = argc > 1 ? argv[1] : "build/base.img";
  diskfs_transaction_t *txn;
  block_t picked[2];
  pthread_t t;

  jt_mount (image, 2, 0);
  pick_free_blocks (picked, 2);
  blk = picked[0];
  bitmap = picked[1];
  JT_SET_HOOK (jt_on_release, on_release);
  JT_SET_HOOK (jt_store_write_hook, hold_hook);

  /* 1.  */
  txn = diskfs_journal_start_transaction ();
  journal_version (txn, blk, 1);
  diskfs_journal_commit_transaction (txn);
  CHECK (jt_checkpoint_unwritten (blk), "T1's copy of B is home already");

  /* 2.  */
  jt_pageout (blk, 0, 0);
  struct timespec until;
  clock_gettime (CLOCK_REALTIME, &until);
  until.tv_sec += 30;
  pthread_mutex_lock (&hold_lock);
  while (!hold_reached
	 && pthread_cond_timedwait (&hold_cond, &hold_lock, &until) == 0)
    ;
  pthread_mutex_unlock (&hold_lock);
  CHECK (hold_reached, "the page-out never wrote B");

  /* 3.  */
  pthread_create (&t, NULL, t2, NULL);
  usleep (300000);
  CHECK (!__atomic_load_n (&released, __ATOMIC_ACQUIRE),
	 "B was handed back while a write of its old contents was in "
	 "flight");
  int waited = !__atomic_load_n (&t2_done, __ATOMIC_ACQUIRE);

  /* 4.  */
  pthread_mutex_lock (&hold_lock);
  hold_released = 1;
  pthread_cond_broadcast (&hold_cond);
  pthread_mutex_unlock (&hold_lock);
  pthread_join (t, NULL);
  CHECK (__atomic_load_n (&released, __ATOMIC_ACQUIRE),
	 "B was not handed back after its free committed");

  /* 5.  */
  txn = diskfs_journal_start_transaction ();
  journal_version (txn, blk, 3);
  jt_pageout (blk, 0, 1);
  diskfs_journal_commit_transaction (txn);
  t3_returned_at = jt_log_count ();

  unsigned long warnings = jt_umount ();
  JT_SET_HOOK (jt_store_write_hook, NULL);
  CHECK (warnings == 0, "%lu journal warnings", warnings);
  CHECK (jt_stale_reads () == 0, "%lu stale page-ins", jt_stale_reads ());
  CHECK (pattern_version (jt_disk_block (blk), blk) == 3,
	 "block %u: version %ld home, 3 expected", blk,
	 pattern_version (jt_disk_block (blk), blk));

  size_t checked = jt_crash_check ("t_forget_inflight", check, NULL);
  printf ("t_forget_inflight: OK (T2's commit %s for the write; %zu "
	  "writes, %zu crash images)\n", waited ? "waited" : "did not wait",
	  jt_log_count (), checked);
  return 0;
}
