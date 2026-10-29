/* Pager threads that start transactions.

   pager_unlock_page opens a handle on a pager thread to allocate a page's
   blocks.  The fault it serves can come from an RPC thread that holds a
   handle itself, such as an edit of a directory page.  If a commit is
   draining that RPC's transaction, the commit waits for the RPC, and the
   RPC for the pager; so a pager thread never waits in the journal: it
   joins the draining transaction (journal_join_transaction_locked), and
   journal_wait_freed_blocks returns at once for it.

   1. A holds a handle on T; a commit drains T (T_LOCKED) and waits for
      A.  A faults: the pager callback starts a transaction, which must
      be T, journals a block in it, and returns.  A finishes, and the
      commit completes with both blocks.
   2. A commit's log write is held in store_write.  On a pager thread,
      journal_wait_freed_blocks returns 0 at once; on an RPC thread it
      waits for the commit.
   3. RPC threads fault in the middle of their handles while another
      thread keeps committing.  The pager's block of each fault must land
      in the faulting operation's transaction: after a replay, each
      thread's own block and its pager block hold the same version.

   A deadlock trips a watchdog.  */

#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include "common.h"

static void
watchdog (int sig)
{
  static const char msg[] = "t_pager_txn: DEADLOCK: no progress for 60 s\n";
  (void) sig;
  if (write (2, msg, sizeof msg - 1) < 0)
    _exit (1);
  _exit (1);
}

/* ---------------------------------------------------------------- */
/* 1.  */

struct unlock
{
  block_t blk;
  uint32_t ver;
  diskfs_transaction_t *txn;	/* The transaction the pager joined.  */
};

/* pager_unlock_page: one handle for the page's allocation.  */
static void
unlock_page (void *arg)
{
  struct unlock *u = arg;

  u->txn = diskfs_journal_start_transaction ();
  CHECK (u->txn, "the pager thread got no transaction");
  journal_version (u->txn, u->blk, u->ver);
  diskfs_journal_stop_transaction (u->txn);
}

static diskfs_transaction_t *txn_t;
static size_t part1_returned_at = SIZE_MAX;

static void *
committer1 (void *arg)
{
  (void) arg;
  diskfs_transaction_t *txn = diskfs_journal_start_transaction ();
  CHECK (txn == txn_t, "the committer joined another transaction");
  diskfs_journal_commit_transaction (txn);
  part1_returned_at = jt_log_count ();
  return NULL;
}

static block_t p1_rpc, p1_pager;

static void
part1 (void)
{
  pthread_t c;
  struct unlock u = { .blk = p1_pager, .ver = 1 };

  txn_t = diskfs_journal_start_transaction ();
  journal_version (txn_t, p1_rpc, 1);
  pthread_create (&c, NULL, committer1, NULL);
  while (!jt_txn_locked (txn_t))
    sched_yield ();

  jt_pager_call (unlock_page, &u);
  CHECK (u.txn == txn_t, "the pager thread did not join the draining "
	 "transaction");

  diskfs_journal_stop_transaction (txn_t);
  pthread_join (c, NULL);
}

/* ---------------------------------------------------------------- */
/* 2.  */

static pthread_mutex_t hold_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t hold_cond = PTHREAD_COND_INITIALIZER;
static int hold_armed, hold_reached, hold_released;

/* Hold the first write to the log once armed: the commit's.  */
static void
hold_hook (uint64_t off, size_t len, const void *data)
{
  (void) len;
  (void) data;
  /* A log write, which the commit makes with the journal lock dropped.
     Not the journal superblock: kjournald writes it when the tail moves,
     with the lock held, and holding that would keep every thread out of
     the journal, the RPC below in diskfs_journal_start_transaction until
     the commit is over.  */
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

static block_t p2_blk, p2_freed;
static int pager_waited = -1;

static void
pager_wait (void *arg)
{
  (void) arg;
  diskfs_transaction_t *txn = diskfs_journal_start_transaction ();
  pager_waited = journal_wait_freed_blocks (txn);
  diskfs_journal_stop_transaction (txn);
}

static void *
committer2 (void *arg)
{
  (void) arg;
  diskfs_transaction_t *txn = diskfs_journal_start_transaction ();
  journal_version (txn, p2_blk, 1);
  CHECK (journal_record_freed_blocks (txn, p2_freed, 1), "record a free");
  diskfs_journal_commit_transaction (txn);
  return NULL;
}

static int rpc_waited = -1, rpc_done;
static int pager_returned, released_late;

/* Let the held write go if the pager is still in the journal after 2 s:
   it waited, which the test reports.  */
static void *
releaser (void *arg)
{
  (void) arg;
  for (int i = 0; i < 200; i++)
    {
      if (__atomic_load_n (&pager_returned, __ATOMIC_ACQUIRE))
	return NULL;
      usleep (10 * 1000);
    }
  __atomic_store_n (&released_late, 1, __ATOMIC_RELEASE);
  pthread_mutex_lock (&hold_lock);
  hold_released = 1;
  pthread_cond_broadcast (&hold_cond);
  pthread_mutex_unlock (&hold_lock);
  return NULL;
}

static void *
rpc_wait (void *arg)
{
  (void) arg;
  diskfs_transaction_t *txn = diskfs_journal_start_transaction ();
  rpc_waited = journal_wait_freed_blocks (txn);
  __atomic_store_n (&rpc_done, 1, __ATOMIC_RELEASE);
  diskfs_journal_stop_transaction (txn);
  return NULL;
}

static void
part2 (void)
{
  pthread_t c, r;

  pthread_mutex_lock (&hold_lock);
  hold_armed = 1;
  pthread_mutex_unlock (&hold_lock);
  JT_SET_HOOK (jt_store_write_hook, hold_hook);
  pthread_create (&c, NULL, committer2, NULL);
  pthread_mutex_lock (&hold_lock);
  while (!hold_reached)
    pthread_cond_wait (&hold_cond, &hold_lock);
  pthread_mutex_unlock (&hold_lock);

  /* The commit is writing the log: the pager must not wait for it.  */
  pthread_t rl;
  pthread_create (&rl, NULL, releaser, NULL);
  jt_pager_call (pager_wait, NULL);
  __atomic_store_n (&pager_returned, 1, __ATOMIC_RELEASE);
  pthread_join (rl, NULL);
  CHECK (!released_late && pager_waited == 0,
	 "journal_wait_freed_blocks waited on a pager thread for the commit");

  /* An RPC thread does wait, until the commit has handed its frees
     back.  */
  pthread_create (&r, NULL, rpc_wait, NULL);
  usleep (100 * 1000);
  CHECK (!__atomic_load_n (&rpc_done, __ATOMIC_ACQUIRE),
	 "journal_wait_freed_blocks did not wait on an RPC thread");
  pthread_mutex_lock (&hold_lock);
  hold_released = 1;
  pthread_cond_broadcast (&hold_cond);
  pthread_mutex_unlock (&hold_lock);
  pthread_join (r, NULL);
  pthread_join (c, NULL);
  JT_SET_HOOK (jt_store_write_hook, NULL);
  CHECK (rpc_waited == 1, "journal_wait_freed_blocks returned %d on an "
	 "RPC thread", rpc_waited);
}

/* ---------------------------------------------------------------- */
/* 3.  */

#define NT 3
#define OPS 200

static block_t own[NT], pgblk[NT];
static unsigned long *seq[NT];
static unsigned long seq_counter;
static int stop_committer;

struct durable
{
  unsigned long seq;
  size_t log;
};
static pthread_mutex_t durable_lock = PTHREAD_MUTEX_INITIALIZER;
static struct durable durables[100000];
static size_t ndurables;
static unsigned long faults, joined_draining;

static void *
committer3 (void *arg)
{
  (void) arg;
  while (!__atomic_load_n (&stop_committer, __ATOMIC_ACQUIRE))
    {
      unsigned long before = __atomic_load_n (&seq_counter,
					       __ATOMIC_SEQ_CST);
      diskfs_transaction_t *txn = diskfs_journal_start_transaction ();
      diskfs_journal_commit_transaction (txn);
      pthread_mutex_lock (&durable_lock);
      if (ndurables < sizeof durables / sizeof durables[0])
	durables[ndurables++] = (struct durable) { before, jt_log_count () };
      pthread_mutex_unlock (&durable_lock);
      usleep (rand () % 300);
    }
  return NULL;
}

static void *
rpc_thread (void *arg)
{
  int t = (intptr_t) arg;

  for (uint32_t i = 1; i <= OPS; i++)
    {
      diskfs_transaction_t *txn = diskfs_journal_start_transaction ();
      struct unlock u = { .blk = pgblk[t], .ver = i };

      journal_version (txn, own[t], i);
      if (rand () % 2)
	sched_yield ();
      /* The fault, served while this handle is open.  */
      jt_pager_call (unlock_page, &u);
      CHECK (u.txn == txn, "thread %d: the pager's handle went to another "
	     "transaction", t);
      __atomic_add_fetch (&faults, 1, __ATOMIC_RELAXED);
      if (jt_txn_locked (txn))
	__atomic_add_fetch (&joined_draining, 1, __ATOMIC_RELAXED);
      if (rand () % 4 == 0)
	jt_pageout (own[t], rand () % 2, 0);
      diskfs_journal_stop_transaction (txn);
      seq[t][i] = __atomic_add_fetch (&seq_counter, 1, __ATOMIC_SEQ_CST);
    }
  return NULL;
}

static void
part3 (void)
{
  pthread_t threads[NT], c;

  for (int t = 0; t < NT; t++)
    seq[t] = calloc (OPS + 1, sizeof *seq[t]);
  pthread_create (&c, NULL, committer3, NULL);
  for (int t = 0; t < NT; t++)
    pthread_create (&threads[t], NULL, rpc_thread, (void *) (intptr_t) t);
  for (int t = 0; t < NT; t++)
    pthread_join (threads[t], NULL);
  __atomic_store_n (&stop_committer, 1, __ATOMIC_RELEASE);
  pthread_join (c, NULL);
}

/* ---------------------------------------------------------------- */

static long
read_version (FILE *f, block_t b, char *buf)
{
  CHECK (!fseeko (f, (off_t) b << log2_block_size, SEEK_SET)
	 && fread (buf, block_size, 1, f) == 1, "read block %u", b);
  return pattern_version (buf, b);
}

static const char *
check (size_t n, int torn, const char *img, void *arg)
{
  static char msg[256];
  FILE *f = fopen (img, "r");
  char *buf = malloc (block_size);
  const char *err = NULL;
  unsigned long durable_seq = 0;

  (void) torn;
  (void) arg;
  CHECK (f, "open %s", img);

  /* 1: the RPC's block and the pager's commit together.  */
  long a = read_version (f, p1_rpc, buf), p = read_version (f, p1_pager, buf);
  if (a != p)
    {
      snprintf (msg, sizeof msg, "part 1: the RPC's block holds v%ld, the "
		"pager's v%ld", a, p);
      err = msg;
    }
  else if (n >= part1_returned_at && a != 1)
    {
      snprintf (msg, sizeof msg, "part 1: lost after the commit returned");
      err = msg;
    }

  /* 3.  */
  for (size_t d = 0; d < ndurables; d++)
    if (durables[d].log <= n && durables[d].seq > durable_seq)
      durable_seq = durables[d].seq;
  for (int t = 0; t < NT && !err; t++)
    {
      long vo = read_version (f, own[t], buf);
      long vp = read_version (f, pgblk[t], buf);
      uint32_t need = 0;

      for (int i = 1; i <= OPS; i++)
	if (seq[t][i] && seq[t][i] <= durable_seq)
	  need = i;
      if (vo != vp)
	snprintf (msg, sizeof msg, "thread %d: its block holds v%ld, its "
		  "pager block v%ld: a fault's allocation went to another "
		  "transaction", t, vo, vp);
      else if (vo < (long) need)
	snprintf (msg, sizeof msg, "thread %d: recovered v%ld, but v%u had "
		  "stopped before a commit that returned", t, vo, need);
      else
	continue;
      err = msg;
    }
  fclose (f);
  free (buf);
  return err;
}

int
main (int argc, char **argv)
{
  const char *image = argc > 1 ? argv[1] : "build/base.img";
  block_t picked[4 + 2 * NT];
  unsigned long warnings;

  srand (argc > 2 ? atoi (argv[2]) : 1);
  signal (SIGALRM, watchdog);
  alarm (60);

  /* A callback can fault on a disk-cache block, which another pager
     thread serves; libpager adds threads as needed, the harness has a
     fixed number: one per RPC thread, and some to spare.  */
  jt_mount (image, NT + 3, 0);
  pick_free_blocks (picked, 4 + 2 * NT);
  p1_rpc = picked[0];
  p1_pager = picked[1];
  p2_blk = picked[2];
  p2_freed = picked[3];
  for (int t = 0; t < NT; t++)
    {
      own[t] = picked[4 + 2 * t];
      pgblk[t] = picked[5 + 2 * t];
    }

  part1 ();
  part2 ();
  part3 ();

  warnings = jt_umount ();
  alarm (0);
  CHECK (warnings == 0, "%lu journal warnings", warnings);
  CHECK (jt_stale_reads () == 0, "%lu stale page-ins", jt_stale_reads ());
  for (int t = 0; t < NT; t++)
    CHECK (pattern_version (jt_disk_block (own[t]), own[t]) == OPS
	   && pattern_version (jt_disk_block (pgblk[t]), pgblk[t]) == OPS,
	   "thread %d: the last versions are not home", t);

  size_t checked = jt_crash_check ("t_pager_txn", check, NULL);
  printf ("t_pager_txn: OK (%lu faults in handles, %lu while draining, "
	  "%zu writes, %zu crash images)\n", faults, joined_draining,
	  jt_log_count (), checked);
  return 0;
}
