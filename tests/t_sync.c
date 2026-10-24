/* diskfs_sync_everything at any moment.

   The sync thread calls diskfs_sync_everything every 30 seconds, but for
   the journal it can come at any time: in the middle of other handles,
   while a commit drains, while the pager serves a fault.  With a
   journal it commits the inodes (write_all_disknodes) in a handle of
   its own, then writes out the file pagers' dirty pages except the
   directories', then flushes the store when asked to wait
   (jt_sync_everything).

   1. A sync starts while a commit drains a transaction an RPC thread
      still holds: the sync waits for the next transaction, and finishes
      once the RPC's handle is released.
   2. RPC threads run operations while a sync thread syncs at random
      moments, waiting or not.  Each operation of thread T is one handle:
      it writes version I to its two disk-cache blocks and to its
      directory block (through the file pager, as dir.c does), and
      sometimes faults, the pager callback journaling version I of a
      fourth block; it updates its in-core inode, which only the sync
      writes, to its inode block.  After the handle, it writes version I
      of one of its file data blocks, not journaled, and leaves it dirty.

   3. A sync with nothing to write, while a commit holding an operation
      that stopped before the sync is still writing the log (the write
      is held in store_write): the sync's own transaction is empty, and
      it must still not return before that commit is on disk.

   What must hold, after a replay:
   - each thread's four journaled blocks hold one version: no operation,
     fault or sync splits a transaction;
   - every operation that stopped before a sync, or a commit, that
     returned is there;
   - each inode block holds at least what the last sync that returned
     wrote to it;
   - each file data block holds at least what was written to it before
     a waiting sync that returned started.
   A deadlock trips a watchdog.  */

#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include "common.h"

static void
watchdog (int sig)
{
  static const char msg[] = "t_sync: DEADLOCK: no progress for 120 s\n";
  (void) sig;
  if (write (2, msg, sizeof msg - 1) < 0)
    _exit (1);
  _exit (1);
}

#define NT 3
#define NDATA 3
#define OPS 150
#define NJ 4			/* Journaled blocks per thread.  */

/* Thread T's blocks: NJ journaled in its operations (two disk-cache,
   the directory block, the one its faults journal), its inode block,
   and its file data blocks.  */
static block_t jblk[NT][NJ], iblk[NT], dblk[NT][NDATA];
enum { J_META0, J_META1, J_DIR, J_FAULT };

static unsigned long seq_counter;
static unsigned long seq[NT][OPS + 2];
static unsigned long seq_extra;		/* Part 3's operation.  */
static uint32_t incore[NT];		/* In-core inode version.  */
static pthread_mutex_t node_lock[NT];	/* The node locks.  */
static uint32_t data_written[NT][NDATA];	/* Last version, once written.  */

/* Every sync or commit that returned: what it made durable, from log
   length LOG on.  */
struct durable
{
  size_t log;
  unsigned long seq;		/* Operations stopped by then.  */
  int has_inode, has_data;
  uint32_t inode[NT];		/* Inode versions it wrote.  */
  uint32_t data[NT][NDATA];	/* File data written before it started.  */
};
static pthread_mutex_t durable_lock = PTHREAD_MUTEX_INITIALIZER;
static struct durable *durables;
static size_t ndurables, maxdurables;
static unsigned long n_syncs, n_wait_syncs, n_faults;

static void
add_durable (struct durable *d)
{
  pthread_mutex_lock (&durable_lock);
  CHECK (ndurables < maxdurables, "too many syncs");
  durables[ndurables++] = *d;
  pthread_mutex_unlock (&durable_lock);
}

/* The test's write_all_disknodes: each dirty node, under its lock.  A
   sync with nothing dirty commits an empty transaction: the commit
   must then still wait for the one in flight, which may hold the
   caller's earlier operations.  */
static uint32_t captured[NT];
static uint32_t written[NT];		/* What the inode block has.  */

static void
write_nodes (diskfs_transaction_t *txn)
{
  for (int t = 0; t < NT; t++)
    {
      pthread_mutex_lock (&node_lock[t]);
      captured[t] = incore[t];
      if (incore[t] != written[t])
	{
	  journal_version (txn, iblk[t], incore[t]);
	  written[t] = incore[t];
	}
      pthread_mutex_unlock (&node_lock[t]);
    }
}

static void
sync_and_record (int wait)
{
  struct durable d = { 0 };

  d.seq = __atomic_load_n (&seq_counter, __ATOMIC_SEQ_CST);
  if (wait)
    for (int t = 0; t < NT; t++)
      for (int k = 0; k < NDATA; k++)
	d.data[t][k] = __atomic_load_n (&data_written[t][k],
					__ATOMIC_ACQUIRE);
  jt_sync_everything (wait);
  d.log = jt_log_count ();
  d.has_inode = 1;
  memcpy (d.inode, captured, sizeof d.inode);
  d.has_data = wait;
  add_durable (&d);
  n_syncs++;
  n_wait_syncs += wait;
}

/* ---------------------------------------------------------------- */
/* 1.  */

static diskfs_transaction_t *txn_t;
static int s_done;

static void *
committer1 (void *arg)
{
  (void) arg;
  diskfs_transaction_t *txn = diskfs_journal_start_transaction ();
  CHECK (txn == txn_t, "the committer joined another transaction");
  diskfs_journal_commit_transaction (txn);
  return NULL;
}

static void *
syncer1 (void *arg)
{
  (void) arg;
  sync_and_record (1);
  __atomic_store_n (&s_done, 1, __ATOMIC_RELEASE);
  return NULL;
}

static void
part1 (void)
{
  pthread_t c, s;

  txn_t = diskfs_journal_start_transaction ();
  journal_version (txn_t, jblk[0][J_META0], 0);
  pthread_create (&c, NULL, committer1, NULL);
  while (!jt_txn_locked (txn_t))
    sched_yield ();

  pthread_create (&s, NULL, syncer1, NULL);
  usleep (100 * 1000);
  CHECK (!__atomic_load_n (&s_done, __ATOMIC_ACQUIRE),
	 "the sync finished while the RPC still held its handle");
  diskfs_journal_stop_transaction (txn_t);
  pthread_join (c, NULL);
  pthread_join (s, NULL);
}

/* ---------------------------------------------------------------- */
/* 2.  */

struct unlock
{
  block_t blk;
  uint32_t ver;
  diskfs_transaction_t *txn;
};

static void
unlock_page (void *arg)
{
  struct unlock *u = arg;

  u->txn = diskfs_journal_start_transaction ();
  journal_version (u->txn, u->blk, u->ver);
  diskfs_journal_stop_transaction (u->txn);
}

static void
dir_version (diskfs_transaction_t *txn, block_t b, uint32_t v)
{
  char buf[block_size];

  CHECK (!journal_get_write_access_external (txn, b), "reserve dir");
  pattern (buf, b, v);
  jt_dir_edit (b, 0, buf, block_size);
  CHECK (!jt_dir_mark_dirty (txn, b), "mark dir");
}

static void *
rpc_thread (void *arg)
{
  int t = (intptr_t) arg;
  char buf[block_size];

  for (uint32_t i = 1; i <= OPS; i++)
    {
      int sync = i % 15 == 0;
      unsigned long before = 0;
      diskfs_transaction_t *txn = diskfs_journal_start_transaction ();

      pthread_mutex_lock (&node_lock[t]);
      journal_version (txn, jblk[t][J_META0], i);
      if (rand () % 2)
	sched_yield ();
      dir_version (txn, jblk[t][J_DIR], i);
      journal_version (txn, jblk[t][J_META1], i);
      {
	struct unlock u = { .blk = jblk[t][J_FAULT], .ver = i };
	if (rand () % 3 == 0)
	  {
	    jt_pager_call (unlock_page, &u);
	    CHECK (u.txn == txn, "the fault went to another transaction");
	    __atomic_add_fetch (&n_faults, 1, __ATOMIC_RELAXED);
	  }
	else
	  journal_version (txn, jblk[t][J_FAULT], i);
      }
      /* Not every operation dirties the inode.  */
      if (rand () % 3 == 0)
	incore[t] = i;
      pthread_mutex_unlock (&node_lock[t]);
      if (rand () % 4 == 0)
	jt_pageout (jblk[t][J_META0], rand () % 2, 0);

      if (sync)
	{
	  before = __atomic_add_fetch (&seq_counter, 1, __ATOMIC_SEQ_CST);
	  seq[t][i] = before;
	  diskfs_journal_commit_transaction (txn);
	  struct durable d = { .log = jt_log_count (), .seq = before };
	  add_durable (&d);
	}
      else
	{
	  diskfs_journal_stop_transaction (txn);
	  seq[t][i] = __atomic_add_fetch (&seq_counter, 1, __ATOMIC_SEQ_CST);
	}

      /* File data: written to the page, left dirty.  */
      int k = rand () % NDATA;
      pattern (buf, dblk[t][k], 0x80000000u | i);
      jt_file_edit (dblk[t][k], 0, buf, block_size);
      __atomic_store_n (&data_written[t][k], i, __ATOMIC_RELEASE);
      if (rand () % 8 == 0)
	jt_file_pageout (dblk[t][k], rand () % 2, 0);
      usleep (rand () % 300);
    }
  return NULL;
}

static int stop_syncer;

static void *
syncer (void *arg)
{
  (void) arg;
  while (!__atomic_load_n (&stop_syncer, __ATOMIC_ACQUIRE))
    {
      usleep (rand () % 100);
      sync_and_record (rand () % 2);
    }
  return NULL;
}

static void
part2 (void)
{
  pthread_t threads[NT], s;

  pthread_create (&s, NULL, syncer, NULL);
  for (int t = 0; t < NT; t++)
    pthread_create (&threads[t], NULL, rpc_thread, (void *) (intptr_t) t);
  for (int t = 0; t < NT; t++)
    pthread_join (threads[t], NULL);
  __atomic_store_n (&stop_syncer, 1, __ATOMIC_RELEASE);
  pthread_join (s, NULL);
}

/* ---------------------------------------------------------------- */
/* 3.  */

static pthread_mutex_t hold_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t hold_cond = PTHREAD_COND_INITIALIZER;
static int hold_armed, hold_reached, hold_released;

/* Hold the first write to the log once armed: the commit's.  */
static void
hold_hook (uint64_t off, size_t len, const void *data)
{
  (void) len;
  (void) data;
  if (!jt_in_journal (off))
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

static void *
committer3 (void *arg)
{
  (void) arg;
  diskfs_transaction_t *txn = diskfs_journal_start_transaction ();
  diskfs_journal_commit_transaction (txn);
  return NULL;
}

static int s3_done;

static void *
syncer3 (void *arg)
{
  (void) arg;
  sync_and_record (0);
  __atomic_store_n (&s3_done, 1, __ATOMIC_RELEASE);
  return NULL;
}

static void
part3 (void)
{
  pthread_t c, s;
  diskfs_transaction_t *txn;
  uint32_t v = OPS + 1;

  /* Nothing dirty for the sync to write.  */
  jt_sync_everything (1);

  /* 1: an operation of thread 0, stopped.  */
  txn = diskfs_journal_start_transaction ();
  pthread_mutex_lock (&node_lock[0]);
  journal_version (txn, jblk[0][J_META0], v);
  dir_version (txn, jblk[0][J_DIR], v);
  journal_version (txn, jblk[0][J_META1], v);
  journal_version (txn, jblk[0][J_FAULT], v);
  pthread_mutex_unlock (&node_lock[0]);
  diskfs_journal_stop_transaction (txn);
  seq_extra = __atomic_add_fetch (&seq_counter, 1, __ATOMIC_SEQ_CST);

  /* 2: its commit, held at the log.  */
  pthread_mutex_lock (&hold_lock);
  hold_armed = 1;
  pthread_mutex_unlock (&hold_lock);
  jt_store_write_hook = hold_hook;
  pthread_create (&c, NULL, committer3, NULL);
  pthread_mutex_lock (&hold_lock);
  while (!hold_reached)
    pthread_cond_wait (&hold_cond, &hold_lock);
  pthread_mutex_unlock (&hold_lock);

  /* 3: the sync must wait for that commit.  */
  pthread_create (&s, NULL, syncer3, NULL);
  usleep (100 * 1000);
  int early = __atomic_load_n (&s3_done, __ATOMIC_ACQUIRE);
  pthread_mutex_lock (&hold_lock);
  hold_released = 1;
  pthread_cond_broadcast (&hold_cond);
  pthread_mutex_unlock (&hold_lock);
  pthread_join (s, NULL);
  pthread_join (c, NULL);
  jt_store_write_hook = NULL;
  CHECK (!early, "the sync returned while the commit of an operation "
	 "stopped before it was still writing the log");
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
  uint32_t need_inode[NT] = { 0 }, need_data[NT][NDATA] = { { 0 } };

  (void) arg;
  CHECK (f, "open %s", img);
  for (size_t d = 0; d < ndurables; d++)
    {
      const struct durable *du = &durables[d];
      if (du->log > n)
	continue;
      if (du->seq > durable_seq)
	durable_seq = du->seq;
      for (int t = 0; t < NT; t++)
	{
	  if (du->has_inode && du->inode[t] > need_inode[t])
	    need_inode[t] = du->inode[t];
	  for (int k = 0; du->has_data && k < NDATA; k++)
	    if (du->data[t][k] > need_data[t][k])
	      need_data[t][k] = du->data[t][k];
	}
    }

  for (int t = 0; t < NT && !err; t++)
    {
      long v[NJ];
      uint32_t need = 0;

      for (int j = 0; j < NJ; j++)
	v[j] = read_version (f, jblk[t][j], buf);
      for (int i = 1; i <= OPS; i++)
	if (seq[t][i] && seq[t][i] <= durable_seq)
	  need = i;
      if (t == 0 && seq_extra && seq_extra <= durable_seq)
	need = OPS + 1;
      for (int j = 1; j < NJ && !err; j++)
	if (v[j] != v[0])
	  {
	    snprintf (msg, sizeof msg, "thread %d: its blocks hold versions "
		      "%ld %ld %ld %ld: a transaction is split", t, v[0], v[1],
		      v[2], v[3]);
	    err = msg;
	  }
      if (!err && v[0] < (long) need)
	{
	  snprintf (msg, sizeof msg, "thread %d: recovered operation %ld, but "
		    "%u had stopped before a sync or commit that returned",
		    t, v[0], need);
	  err = msg;
	}

      long vi = read_version (f, iblk[t], buf);
      if (!err && vi < (long) need_inode[t])
	{
	  snprintf (msg, sizeof msg, "thread %d: inode block holds v%ld, "
		    "but a sync that returned wrote v%u", t, vi, need_inode[t]);
	  err = msg;
	}

      for (int k = 0; k < NDATA && !err; k++)
	{
	  /* A torn write of the data itself leaves it torn.  */
	  if (torn && n > 0 && jt_log_entry (n - 1)->data
	      && jt_log_entry (n - 1)->off == (uint64_t) dblk[t][k]
	      << log2_block_size)
	    continue;
	  long vd = read_version (f, dblk[t][k], buf);
	  long got = vd > 0 ? vd & 0x7fffffff : vd;
	  if (got < (long) need_data[t][k])
	    {
	      snprintf (msg, sizeof msg, "thread %d: file data block %u holds "
			"v%ld, but v%u was written before a waiting sync that "
			"returned", t, dblk[t][k], got, need_data[t][k]);
	      err = msg;
	    }
	}
    }
  fclose (f);
  free (buf);
  return err;
}

int
main (int argc, char **argv)
{
  const char *image = argc > 1 ? argv[1] : "build/base.img";
  block_t picked[NT * (NJ + 1 + NDATA)];
  unsigned long warnings;

  srand (argc > 2 ? atoi (argv[2]) : 1);
  signal (SIGALRM, watchdog);
  alarm (120);
  maxdurables = 100000;
  durables = calloc (maxdurables, sizeof *durables);
  for (int t = 0; t < NT; t++)
    pthread_mutex_init (&node_lock[t], NULL);

  jt_mount (image, NT + 3, 0);
  pick_free_blocks (picked, NT * (NJ + 1 + NDATA));
  for (int t = 0, p = 0; t < NT; t++)
    {
      for (int j = 0; j < NJ; j++)
	jblk[t][j] = picked[p++];
      iblk[t] = picked[p++];
      for (int k = 0; k < NDATA; k++)
	dblk[t][k] = picked[p++];
    }
  jt_write_all_disknodes = write_nodes;

  part1 ();
  part2 ();
  part3 ();

  warnings = jt_umount ();
  alarm (0);
  CHECK (warnings == 0, "%lu journal warnings", warnings);
  CHECK (jt_stale_reads () == 0, "%lu stale page-ins", jt_stale_reads ());

  size_t checked = jt_crash_check ("t_sync", check, NULL);
  printf ("t_sync: OK (%lu syncs, %lu waiting, %lu faults, %zu writes, "
	  "%zu crash images)\n", n_syncs, n_wait_syncs, n_faults,
	  jt_log_count (), checked);
  return 0;
}
