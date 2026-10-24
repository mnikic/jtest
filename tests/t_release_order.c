/* The frees of two commits go back to the allocator in order.

   A commit hands its freed blocks back after it has finished, with the
   journal lock dropped (journal_forget_freed_blocks), and the next
   commit can run meanwhile.  j_released_tid says that the frees of every
   transaction up to it are back, and journal_wait_freed_blocks waits on
   it.  If the next commit's release finishes first and the older one
   then sets j_released_tid back, a thread waiting for the newer frees
   waits for good: in t_balloc_lock, with a node's alloc_lock held, and
   its own transaction's commit waiting for it.

   The steps are forced:
   1. T1 frees B1 and commits on a thread of its own; the release of B1
      is held (jt_on_release).
   2. T2 frees B2 and commits on another thread.
   3. The release of B1 is let go.
   Then the frees of T1 and of T2 are back, and j_released_tid is T2's.
   Before that, it must never say T2's frees are back while T1's are
   not.
   4. T3 frees B3 and commits; its release is held.  A thread W with a
      handle on the next transaction asks to wait for freed blocks, as
      ext2_new_block does on a full disk: T3 has committed, but its frees
      are not back, so W must wait, not give up (which would be ENOSPC).
      The release is let go, and W has waited.  */

#include <pthread.h>
#include <unistd.h>
#include "common.h"

static block_t b1, b2, b3, held;
static int b2_back;

static pthread_mutex_t hold_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t hold_cond = PTHREAD_COND_INITIALIZER;
static int hold_reached, hold_released;

static void
on_release (block_t start, unsigned long count)
{
  if (b2 >= start && b2 < start + count)
    __atomic_store_n (&b2_back, 1, __ATOMIC_RELEASE);
  if (!(held >= start && held < start + count))
    return;
  pthread_mutex_lock (&hold_lock);
  hold_reached = 1;
  pthread_cond_broadcast (&hold_cond);
  while (!hold_released)
    pthread_cond_wait (&hold_cond, &hold_lock);
  pthread_mutex_unlock (&hold_lock);
}

/* A transaction that frees B, and commits.  */
static void *
free_and_commit (void *arg)
{
  block_t b = (block_t) (uintptr_t) arg;
  diskfs_transaction_t *txn = diskfs_journal_start_transaction ();
  CHECK (journal_record_freed_blocks (txn, b, 1), "record the free of %u",
	 b);
  diskfs_journal_commit_transaction (txn);
  return NULL;
}

/* W of step 1.  */
static int w_done, w_waited;

static void *
waiter (void *arg)
{
  (void) arg;
  diskfs_transaction_t *txn = diskfs_journal_start_transaction ();
  int waited = journal_wait_freed_blocks (txn);
  diskfs_journal_stop_transaction (txn);
  __atomic_store_n (&w_waited, waited, __ATOMIC_RELEASE);
  __atomic_store_n (&w_done, 1, __ATOMIC_RELEASE);
  return NULL;
}

int
main (int argc, char **argv)
{
  const char *image = argc > 1 ? argv[1] : "build/base.img";
  block_t picked[3];
  pthread_t t1, t2, w;

  jt_mount (image, 2, 0);
  pick_free_blocks (picked, 3);
  b1 = picked[0];
  b2 = picked[1];
  b3 = picked[2];
  held = b1;
  JT_SET_HOOK (jt_on_release, on_release);

  /* 1.  */
  uint32_t before = jt_released_tid ();
  pthread_create (&t1, NULL, free_and_commit, (void *) (uintptr_t) b1);
  pthread_mutex_lock (&hold_lock);
  while (!hold_reached)
    pthread_cond_wait (&hold_cond, &hold_lock);
  pthread_mutex_unlock (&hold_lock);
  uint32_t tid1 = before + 1;

  /* 2.  */
  pthread_create (&t2, NULL, free_and_commit, (void *) (uintptr_t) b2);
  usleep (200000);
  uint32_t mid = jt_released_tid ();
  CHECK (mid == before, "the frees up to transaction %u are said to be "
	 "back while T1's (%u) are held", mid, tid1);

  /* 3.  */
  pthread_mutex_lock (&hold_lock);
  hold_released = 1;
  pthread_cond_broadcast (&hold_cond);
  pthread_mutex_unlock (&hold_lock);
  pthread_join (t1, NULL);
  pthread_join (t2, NULL);
  uint32_t after = jt_released_tid ();
  CHECK (__atomic_load_n (&b2_back, __ATOMIC_ACQUIRE), "B2 is not back");
  CHECK (after == tid1 + 1, "j_released_tid is %u after T1 (%u) and T2 "
	 "(%u) released their frees", after, tid1, tid1 + 1);

  /* 4.  */
  pthread_t t3;
  pthread_mutex_lock (&hold_lock);
  hold_reached = hold_released = 0;
  held = b3;
  pthread_mutex_unlock (&hold_lock);
  pthread_create (&t3, NULL, free_and_commit, (void *) (uintptr_t) b3);
  pthread_mutex_lock (&hold_lock);
  while (!hold_reached)
    pthread_cond_wait (&hold_cond, &hold_lock);
  pthread_mutex_unlock (&hold_lock);
  pthread_create (&w, NULL, waiter, NULL);
  usleep (100000);
  CHECK (!__atomic_load_n (&w_done, __ATOMIC_ACQUIRE), "a wait for freed "
	 "blocks returned (waited: %d) while T3's, committed, were held",
	 __atomic_load_n (&w_waited, __ATOMIC_ACQUIRE));
  pthread_mutex_lock (&hold_lock);
  hold_released = 1;
  pthread_cond_broadcast (&hold_cond);
  pthread_mutex_unlock (&hold_lock);
  pthread_join (t3, NULL);
  pthread_join (w, NULL);
  CHECK (w_waited, "the wait for freed blocks says it did not wait");

  unsigned long warnings = jt_umount ();
  CHECK (warnings == 0, "%lu journal warnings", warnings);
  printf ("t_release_order: OK (T1 %u and T2 %u released in order)\n",
	  tid1, tid1 + 1);
  return 0;
}
