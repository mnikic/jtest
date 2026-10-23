/* An old copy of a freed block, checkpointed while the block's next owner
   commits.

   T1 journals metadata block B and commits; its copy of B waits in the
   checkpoint list.  T2 frees B and commits, and B goes back to the
   allocator.  T3 journals B again for its new owner.  While T3 runs,
   T1's checkpoint does not count T3 as newer, so it may write its old
   copy of B home; if that write is still in flight when T3 commits and
   writes its own copy home, the old one can land last, and both are then
   taken as written: B stays stale at home, with no crash at all.
   journal_forget_freed_blocks prevents it by retiring T1's copy when T2
   commits, before B is handed back.

   The steps are forced:
   1. T1 journals B (version 1) and commits.
   2. T2 frees B, and journals a stand-in for the block bitmap, as
      free_blocks does; it commits, and B is released.
   3. Filler transactions fill more than half the log, so that kjournald
      checkpoints in the background.  Its write of T1's copy of B, if it
      makes one, is held in store_write.
   4. T3 journals B (version 3); a page-out puts B in T3's shadow; T3
      commits, and its flush writes version 3 home.
   5. The held write is let go.
   Then version 3 must be home, and in every crash image after T3's
   commit returned.  */

#include <pthread.h>
#include "common.h"

#define NFILL 180		/* Blocks per filler transaction.  */
#define FILLERS 3

static block_t blk, bitmap, fill[NFILL];
static size_t t3_returned_at = SIZE_MAX;
static int released;

static pthread_mutex_t hold_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t hold_cond = PTHREAD_COND_INITIALIZER;
static int hold_reached, hold_released;

/* Hold a write of version 1 of B: only T1's old copy holds it.  */
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
  block_t picked[NFILL + 2];
  unsigned long warnings;
  int held;

  jt_mount (image, 2, 0);
  pick_free_blocks (picked, NFILL + 2);
  blk = picked[0];
  bitmap = picked[1];
  memcpy (fill, picked + 2, sizeof fill);
  jt_on_release = on_release;
  jt_store_write_hook = hold_hook;

  /* 1.  */
  txn = diskfs_journal_start_transaction ();
  journal_version (txn, blk, 1);
  diskfs_journal_commit_transaction (txn);

  /* 2.  */
  txn = diskfs_journal_start_transaction ();
  CHECK (journal_record_freed_blocks (txn, blk, 1), "record the free");
  journal_version (txn, bitmap, 1);
  diskfs_journal_commit_transaction (txn);
  CHECK (__atomic_load_n (&released, __ATOMIC_ACQUIRE),
	 "B was not handed back after its free committed");

  /* 3.  */
  for (int f = 0; f < FILLERS; f++)
    {
      txn = diskfs_journal_start_transaction ();
      for (int i = 0; i < NFILL; i++)
	journal_version (txn, fill[i], f + 1);
      diskfs_journal_commit_transaction (txn);
    }
  /* If T1's copy of B is still to be written, kjournald's checkpoint
     comes to it first, as T1 is the oldest: wait until it is held.  */
  held = jt_checkpoint_unwritten (blk);
  if (held)
    {
      struct timespec until;
      clock_gettime (CLOCK_REALTIME, &until);
      until.tv_sec += 30;
      pthread_mutex_lock (&hold_lock);
      while (!hold_reached
	     && pthread_cond_timedwait (&hold_cond, &hold_lock, &until) == 0)
	;
      pthread_mutex_unlock (&hold_lock);
      CHECK (hold_reached, "kjournald never checkpointed T1's copy of B");
    }

  /* 4.  */
  txn = diskfs_journal_start_transaction ();
  journal_version (txn, blk, 3);
  jt_pageout (blk, 0, 1);
  diskfs_journal_commit_transaction (txn);
  t3_returned_at = jt_log_count ();

  /* 5.  */
  pthread_mutex_lock (&hold_lock);
  hold_released = 1;
  pthread_cond_broadcast (&hold_cond);
  pthread_mutex_unlock (&hold_lock);

  warnings = jt_umount ();
  jt_store_write_hook = NULL;
  CHECK (warnings == 0, "%lu journal warnings", warnings);
  CHECK (jt_stale_reads () == 0, "%lu stale page-ins", jt_stale_reads ());
  CHECK (pattern_version (jt_disk_block (blk), blk) == 3,
	 "block %u: version %ld home, 3 expected%s", blk,
	 pattern_version (jt_disk_block (blk), blk),
	 held ? " (T1's old copy was written over it)" : "");

  size_t checked = jt_crash_check ("t_forget_race", check, NULL);
  printf ("t_forget_race: OK (%s, %zu writes, %zu crash images)\n",
	  held ? "an old copy was held" : "the old copy was retired",
	  jt_log_count (), checked);
  return 0;
}
