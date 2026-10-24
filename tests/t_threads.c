/* Several RPC threads share transactions while kjournald and the threads
   themselves commit.

   Each thread T owns K blocks.  Its operation I is one handle: it writes
   version I to all of its blocks, in random order with yields in between,
   part of it under a nested start, and records I in its slot of a shared
   block, edited under a lock as the allocator edits a bitmap.  Every
   tenth operation ends with a commit.  So after a replay, for each
   thread, its blocks and its slot must all hold the same version: no
   operation is split between transactions.

   A commit also makes durable every operation, of any thread, that had
   stopped before the commit started.  Crash images are checked against
   that: each operation gets a sequence number when its handle is
   released, and each commit records the last sequence number before it
   and the log length once it returned.  */

#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#include "common.h"

/* t_logpress.c builds this test bigger, under its own name.  */
#ifndef NT
#define NT 4
#define K 3
#define T_NAME "t_threads"
#endif
/* The blocks of thread T, at most KMAX.  */
#ifndef KT
#define KT(t) K
#define KMAX K
#endif

static int ops = 150;
static block_t own[NT][KMAX], shared_block;
static pthread_mutex_t shared_lock = PTHREAD_MUTEX_INITIALIZER;

/* SEQ[T][I]: the sequence number of thread T's operation I.  */
static unsigned long seq_counter;
static unsigned long *seq[NT];

struct durable
{
  unsigned long seq;		/* Operations up to this one... */
  size_t log;			/* ...are durable from here on.  */
};
static pthread_mutex_t durable_lock = PTHREAD_MUTEX_INITIALIZER;
static struct durable *durables;
static size_t ndurables;

static void
maybe_yield (void)
{
  if (rand () % 4 == 0)
    sched_yield ();
  if (rand () % 64 == 0)
    usleep (rand () % 200);
}

static void *
rpc_thread (void *arg)
{
  int t = (intptr_t) arg;

  for (uint32_t i = 1; i <= (uint32_t) ops; i++)
    {
      diskfs_transaction_t *txn = diskfs_journal_start_transaction ();
      diskfs_transaction_t *inner = NULL;
      int order[KMAX], sync = i % 10 == 0;

      CHECK (txn, "no transaction");
      for (int k = 0; k < KT (t); k++)
	order[k] = k;
      for (int k = KT (t) - 1; k > 0; k--)
	{
	  int j = rand () % (k + 1), x = order[k];
	  order[k] = order[j];
	  order[j] = x;
	}

      for (int k = 0; k < KT (t); k++)
	{
	  if (k == 1)
	    {
	      inner = diskfs_journal_start_transaction ();
	      CHECK (inner == txn, "a nested start got another transaction");
	    }
	  journal_version (txn, own[t][order[k]], i);
	  if (rand () % 6 == 0)
	    jt_pageout (own[t][order[k]], rand () % 2, 0);
	  maybe_yield ();
	}
      diskfs_journal_stop_transaction (inner);

      pthread_mutex_lock (&shared_lock);
      CHECK (!journal_get_write_access (txn, shared_block), "shared block");
      jt_edit (shared_block, t * sizeof i, &i, sizeof i);
      journal_mark_dirty (txn, shared_block);
      pthread_mutex_unlock (&shared_lock);
      maybe_yield ();

      if (sync)
	{
	  unsigned long before;

	  /* This operation is released by the commit itself.  */
	  before = __atomic_add_fetch (&seq_counter, 1, __ATOMIC_SEQ_CST);
	  seq[t][i] = before;
	  diskfs_journal_commit_transaction (txn);
	  pthread_mutex_lock (&durable_lock);
	  durables[ndurables++] = (struct durable) { before, jt_log_count () };
	  pthread_mutex_unlock (&durable_lock);
	}
      else
	{
	  diskfs_journal_stop_transaction (txn);
	  seq[t][i] = __atomic_add_fetch (&seq_counter, 1, __ATOMIC_SEQ_CST);
	}
    }
  return NULL;
}

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
  uint32_t slots[NT];
  unsigned long durable_seq = 0;
  const char *err = NULL;

  (void) torn;
  (void) arg;
  CHECK (f, "open %s", img);
  CHECK (!fseeko (f, (off_t) shared_block << log2_block_size, SEEK_SET)
	 && fread (slots, sizeof slots, 1, f) == 1, "read shared block");

  for (size_t d = 0; d < ndurables; d++)
    if (durables[d].log <= n && durables[d].seq > durable_seq)
      durable_seq = durables[d].seq;

  for (int t = 0; t < NT && !err; t++)
    {
      uint32_t need = 0;

      for (int k = 0; k < KT (t) && !err; k++)
	{
	  long v = read_version (f, own[t][k], buf);
	  if (v != slots[t])
	    {
	      snprintf (msg, sizeof msg, "thread %d: block %u holds version "
			"%ld, its slot %u", t, own[t][k], v, slots[t]);
	      err = msg;
	    }
	}
      for (int i = 1; i <= ops; i++)
	if (seq[t][i] <= durable_seq)
	  need = i;
      if (!err && slots[t] < need)
	{
	  snprintf (msg, sizeof msg, "thread %d: recovered operation %u, "
		    "but %u had stopped before a commit that returned",
		    t, slots[t], need);
	  err = msg;
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
  pthread_t threads[NT];
  static block_t picked[NT * KMAX + 1];
  int npicked = 0;
  unsigned long warnings;
  size_t checked;

  srand (argc > 2 ? atoi (argv[2]) : 1);
  if (getenv ("JT_OPS"))
    ops = atoi (getenv ("JT_OPS"));
  jt_mount (image, 3, 0);
  for (int t = 0; t < NT; t++)
    npicked += KT (t);
  pick_free_blocks (picked, npicked + 1);
  shared_block = picked[npicked];
  for (int t = 0; t < NT; t++)
    {
      static int next;
      memcpy (own[t], picked + next, KT (t) * sizeof own[t][0]);
      next += KT (t);
      seq[t] = calloc (ops + 1, sizeof *seq[t]);
    }
  durables = calloc (NT * ops, sizeof *durables);

  for (int t = 0; t < NT; t++)
    pthread_create (&threads[t], NULL, rpc_thread, (void *) (intptr_t) t);
  for (int t = 0; t < NT; t++)
    pthread_join (threads[t], NULL);

  warnings = jt_umount ();
  CHECK (warnings == 0, "%lu journal warnings", warnings);
  CHECK (jt_stale_reads () == 0, "%lu stale page-ins", jt_stale_reads ());

#ifdef T_PRESSURE
  CHECK (jt_forced_checkpoints () > 0, "the log never filled up: no "
	 "commit had to checkpoint for space");
#endif
  checked = jt_crash_check (T_NAME, check, NULL);
  printf (T_NAME ": OK (%d threads x %d operations of %d blocks, %lu "
	  "forced checkpoints, %zu writes, %zu crash images)\n", NT, ops, K,
	  jt_forced_checkpoints (), jt_log_count (), checked);
  return 0;
}
