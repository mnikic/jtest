/* ext2_new_block on a full disk, with a node's alloc_lock held.

   balloc.c fears a deadlock: an RPC thread holds alloc_lock (from
   diskfs_grow) while ext2_new_block waits for the committing transaction
   C to give back the blocks it frees, and a pager thread that joined C
   (pager_unlock_page, whose start never waits) waits for that alloc_lock,
   so C never drains.  It also notes that when the busy blocks are the
   caller's own transaction's, nothing waits and the caller gets ENOSPC
   although the space comes back at the next commit.

   The locks are taken as in ext2fs: an RPC starts its handle first, then
   takes the node lock and alloc_lock (io-write.c, diskfs_grow); a pager
   thread starts its handle, marked as a pager, then takes alloc_lock
   (pager_unlock_page).  ALLOC_LOCK stands for one node's alloc_lock.

   The disk (full.img) is filled first, so every allocation finds only
   blocks still busy until their free commits.

   1. Drain first.  F frees blocks in C.  A pager handle on C waits; C's
      commit is asked for, and C drains (T_LOCKED), waiting for it.  The
      RPC thread A starts its handle: it must wait there, holding no lock,
      until C has drained.  The pager handle then takes ALLOC_LOCK and
      ends; A takes it, and its allocation waits for C's frees and gets a
      block.
   2. Lock first.  A's handle frees blocks in C, and A holds
      ALLOC_LOCK; a pager handle joins C and waits for ALLOC_LOCK.  C's
      commit is asked for.  A allocates: the busy blocks are its own
      transaction's, so it gets ENOSPC, as balloc.c says, and lets go;
      C commits.  After that, the blocks are free again.
   3. Threads at random: RPC threads and pager handles take ALLOC_LOCK
      and allocate or free, while commits come and go.

   No deadlock may happen (the watchdog), and once every block is freed
   and committed, e2fsck finds the image clean.  */

#include <pthread.h>
#include <unistd.h>

#include "common.h"

#define MAXOWN 64

static pthread_rwlock_t alloc_lock = PTHREAD_RWLOCK_INITIALIZER;

/* The blocks the test holds.  */
static pthread_mutex_t own_lock = PTHREAD_MUTEX_INITIALIZER;
static block_t own[MAXOWN];
static int nown;

static unsigned long n_alloc, n_enospc, n_free, n_pager;

static void
own_add (block_t b)
{
  pthread_mutex_lock (&own_lock);
  CHECK (nown < MAXOWN, "too many blocks");
  own[nown++] = b;
  pthread_mutex_unlock (&own_lock);
}

static block_t
own_take (void)
{
  block_t b = 0;

  pthread_mutex_lock (&own_lock);
  if (nown > 0)
    b = own[--nown];
  pthread_mutex_unlock (&own_lock);
  return b;
}

/* One allocation in the thread's handle.  Returns the block, or 0.  */
static block_t
alloc_one (void)
{
  block_t b = ext2_new_block (0, 0, 0, 0);

  if (b)
    {
      own_add (b);
      __atomic_add_fetch (&n_alloc, 1, __ATOMIC_RELAXED);
    }
  else
    __atomic_add_fetch (&n_enospc, 1, __ATOMIC_RELAXED);
  return b;
}

/* Free N of the test's blocks in the thread's handle.  */
static int
free_some (int n)
{
  int done = 0;

  for (; done < n; done++)
    {
      block_t b = own_take ();
      if (!b)
	break;
      ext2_free_blocks (b, 1);
      __atomic_add_fetch (&n_free, 1, __ATOMIC_RELAXED);
    }
  return done;
}

/* An RPC's handle that frees N blocks.  */
static void
rpc_free (int n)
{
  diskfs_transaction_t *txn = diskfs_journal_start_transaction ();
  CHECK (free_some (n) == n, "nothing to free");
  diskfs_journal_stop_transaction (txn);
}

/* The handle of a pager callback, as pager_unlock_page has it.  */
struct pager_job
{
  int (*go) (struct pager_job *);	/* When to take ALLOC_LOCK.  */
  diskfs_transaction_t *txn;
  int in_txn, locked, done;
  int release;				/* Set to let it go on.  */
  int alloc;				/* Allocate under the lock.  */
};

static pthread_mutex_t step_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t step_cond = PTHREAD_COND_INITIALIZER;

static void
set (int *flag)
{
  pthread_mutex_lock (&step_lock);
  *flag = 1;
  pthread_cond_broadcast (&step_cond);
  pthread_mutex_unlock (&step_lock);
}

static void
wait_for (int *flag)
{
  pthread_mutex_lock (&step_lock);
  while (!*flag)
    pthread_cond_wait (&step_cond, &step_lock);
  pthread_mutex_unlock (&step_lock);
}

static int
is_set (int *flag)
{
  pthread_mutex_lock (&step_lock);
  int v = *flag;
  pthread_mutex_unlock (&step_lock);
  return v;
}

static void
pager_fn (void *arg)
{
  struct pager_job *j = arg;

  j->txn = diskfs_journal_start_transaction ();
  set (&j->in_txn);
  wait_for (&j->release);
  pthread_rwlock_wrlock (&alloc_lock);
  set (&j->locked);
  if (j->alloc)
    alloc_one ();
  pthread_rwlock_unlock (&alloc_lock);
  diskfs_journal_stop_transaction (j->txn);
  __atomic_add_fetch (&n_pager, 1, __ATOMIC_RELAXED);
}

static void *
pager_thread (void *arg)
{
  jt_pager_call (pager_fn, arg);
  set (&((struct pager_job *) arg)->done);
  return NULL;
}

/* Ask for the commit of the running transaction, from a thread of its
   own: diskfs_journal_commit_transaction waits until it is done.  */
static void *
committer (void *arg)
{
  int *done = arg;
  diskfs_transaction_t *txn = diskfs_journal_start_transaction ();
  diskfs_journal_commit_transaction (txn);
  set (done);
  return NULL;
}

/* The RPC thread of step 1.  */
struct rpc_job
{
  int started, locked, done;
  block_t got;
};

static void *
rpc_alloc (void *arg)
{
  struct rpc_job *r = arg;
  diskfs_transaction_t *txn = diskfs_journal_start_transaction ();

  set (&r->started);
  pthread_rwlock_wrlock (&alloc_lock);
  set (&r->locked);
  r->got = alloc_one ();
  pthread_rwlock_unlock (&alloc_lock);
  diskfs_journal_stop_transaction (txn);
  set (&r->done);
  return NULL;
}

/* Take every free block, after the commit of the frees before: the disk
   is full, and nothing is busy.  Returns how many were taken.  */
static int
fill (void)
{
  diskfs_transaction_t *txn = diskfs_journal_start_transaction ();
  diskfs_journal_commit_transaction (txn);
  txn = diskfs_journal_start_transaction ();
  int n = 0;
  while (alloc_one ())
    n++;
  diskfs_journal_stop_transaction (txn);
  return n;
}

static void
drain_first (void)
{
  struct pager_job pj = { .alloc = 0 };
  struct rpc_job r = { 0 };
  pthread_t pt, ct, rt;
  int committed = 0;

  fill ();
  rpc_free (4);
  pthread_create (&pt, NULL, pager_thread, &pj);
  wait_for (&pj.in_txn);
  pthread_create (&ct, NULL, committer, &committed);
  while (!jt_txn_locked (pj.txn))
    sched_yield ();

  pthread_create (&rt, NULL, rpc_alloc, &r);
  usleep (100000);
  CHECK (!is_set (&r.started), "an RPC's handle started while the "
	 "commit drains");
  CHECK (!is_set (&r.locked), "the RPC took alloc_lock while the commit "
	 "drains");

  set (&pj.release);
  wait_for (&pj.done);
  wait_for (&r.done);
  pthread_join (rt, NULL);
  wait_for (&committed);
  pthread_join (ct, NULL);
  pthread_join (pt, NULL);
  CHECK (r.got, "the RPC got no block after the commit freed 4");
  printf ("t_balloc_lock: drain first: the RPC waited for the drain, then "
	  "for the frees, and got block %u\n", r.got);
}

static void
lock_first (void)
{
  struct pager_job pj = { .alloc = 0 };
  pthread_t pt, ct;
  int committed = 0;

  fill ();
  diskfs_transaction_t *txn = diskfs_journal_start_transaction ();
  CHECK (free_some (4) == 4, "nothing to free");
  pthread_rwlock_wrlock (&alloc_lock);

  pj.release = 1;
  pthread_create (&pt, NULL, pager_thread, &pj);
  wait_for (&pj.in_txn);
  CHECK (pj.txn == txn, "the pager handle is not on the RPC's transaction");
  pthread_create (&ct, NULL, committer, &committed);
  while (!jt_txn_locked (txn))
    sched_yield ();
  usleep (20000);
  CHECK (!is_set (&pj.locked), "the pager handle got alloc_lock");

  block_t b = ext2_new_block (0, 0, 0, 0);
  if (b)
    own_add (b);
  pthread_rwlock_unlock (&alloc_lock);
  diskfs_journal_stop_transaction (txn);

  wait_for (&pj.done);
  wait_for (&committed);
  pthread_join (ct, NULL);
  pthread_join (pt, NULL);

  /* The frees committed with C: the blocks are back.  */
  txn = diskfs_journal_start_transaction ();
  block_t after = alloc_one ();
  diskfs_journal_stop_transaction (txn);
  CHECK (after, "no block after the commit of the frees");
  CHECK (!b, "a block although only the caller's own frees were free");
  printf ("t_balloc_lock: lock first: ENOSPC with the caller's own "
	  "transaction's 4 frees busy, as balloc.c says; after their "
	  "commit, block %u\n", after);
}

/* Step 3.  */
static int stop_random;

static void *
rpc_worker (void *arg)
{
  unsigned seed = (unsigned) (uintptr_t) arg;

  while (!__atomic_load_n (&stop_random, __ATOMIC_ACQUIRE))
    {
      diskfs_transaction_t *txn = diskfs_journal_start_transaction ();
      pthread_rwlock_wrlock (&alloc_lock);
      if (rand_r (&seed) % 2)
	alloc_one ();
      else
	free_some (1 + rand_r (&seed) % 3);
      pthread_rwlock_unlock (&alloc_lock);
      diskfs_journal_stop_transaction (txn);
      if (rand_r (&seed) % 8 == 0)
	{
	  txn = diskfs_journal_start_transaction ();
	  diskfs_journal_commit_transaction (txn);
	}
    }
  return NULL;
}

static void *
pager_worker (void *arg)
{
  unsigned seed = (unsigned) (uintptr_t) arg;

  while (!__atomic_load_n (&stop_random, __ATOMIC_ACQUIRE))
    {
      struct pager_job pj = { .release = 1, .alloc = 1 };
      jt_pager_call (pager_fn, &pj);
      usleep (rand_r (&seed) % 500);
    }
  return NULL;
}

int
main (int argc, char **argv)
{
  const char *image = argc > 1 ? argv[1] : "build/full.img";
  unsigned seed = argc > 2 ? atoi (argv[2]) : 1;
  const char *out = "build/t_balloc_lock.img";

  jt_mount (image, 4, 0);

  int filled = fill ();
  diskfs_transaction_t *txn;
  CHECK (filled > 8, "only %d blocks were free", filled);
  n_alloc = n_enospc = 0;

  drain_first ();
  lock_first ();

  pthread_t rt[3], pt[2];
  for (int i = 0; i < 3; i++)
    pthread_create (&rt[i], NULL, rpc_worker, (void *) (uintptr_t)
		    (seed * 31 + i));
  for (int i = 0; i < 2; i++)
    pthread_create (&pt[i], NULL, pager_worker, (void *) (uintptr_t)
		    (seed * 37 + i));
  sleep (3);
  __atomic_store_n (&stop_random, 1, __ATOMIC_RELEASE);
  for (int i = 0; i < 3; i++)
    pthread_join (rt[i], NULL);
  for (int i = 0; i < 2; i++)
    pthread_join (pt[i], NULL);

  /* Give everything back.  */
  txn = diskfs_journal_start_transaction ();
  free_some (MAXOWN);
  diskfs_journal_commit_transaction (txn);

  unsigned long warnings = jt_umount ();
  CHECK (warnings == 0, "%lu journal warnings", warnings);
  jt_save_image (out);
  CHECK (jt_run ("e2fsck -fn %s >%s.fsck 2>&1", out, out) == 0,
	 "e2fsck -fn %s is not clean (see %s.fsck)", out, out);
  printf ("t_balloc_lock: OK (%d blocks free at the start; %lu "
	  "allocations, %lu ENOSPC, %lu frees, %lu pager handles)\n",
	  filled, n_alloc, n_enospc, n_free, n_pager);
  return 0;
}
