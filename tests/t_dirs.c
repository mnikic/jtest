/* Directory blocks, whose live copy is in a directory's file pager, in
   transactions with disk-cache metadata.

   Each thread owns K disk-cache blocks and D directory blocks.  Its
   operation I is one handle: it writes version I to some of its
   disk-cache blocks, and edits some of its directory blocks as dir.c
   does: journal_get_write_access_external before the edit, the edit in
   the file pager's copy, then journal_mark_dirty_external with the
   image.  Some reservations are given up with journal_release_access
   instead, with no edit, as when a lookup finds no room.  The file pager
   cleans and evicts directory blocks all along: before the edit, between
   the edit and the mark, and after.  Every fifth operation commits.

   After a replay, each thread's blocks must hold the state after one of
   its operations, no older than any operation that stopped before a
   commit that returned (see t_threads).  */

#include <pthread.h>
#include <sched.h>
#include "common.h"

#define NT 3
#define K 2			/* Disk-cache blocks per thread.  */
#define D 3			/* Directory blocks per thread.  */
#define NB (K + D)

static int ops = 150;
static block_t blocks[NT][NB];	/* K disk-cache blocks, then D directory
				   blocks.  */
static uint32_t *snaps[NT];	/* After operation I: snaps[T][I * NB ...].  */
static unsigned long *seq[NT];
static unsigned long seq_counter;

struct durable
{
  unsigned long seq;
  size_t log;
};
static pthread_mutex_t durable_lock = PTHREAD_MUTEX_INITIALIZER;
static struct durable *durables;
static size_t ndurables;

static unsigned long reservations, releases, marks;

static void
maybe_dir_pageout (block_t b)
{
  if (rand () % 3 == 0)
    jt_dir_pageout (b, rand () % 2, rand () % 4 == 0);
  if (rand () % 4 == 0)
    sched_yield ();
}

/* Edit directory block B in TXN to version V, or with GIVE_UP reserve it
   and give it up.  */
static void
dir_op (diskfs_transaction_t *txn, block_t b, uint32_t v, int give_up)
{
  char buf[block_size];

  jt_trace (b, give_up ? "reserve (to give up)" : "reserve", NULL);
  CHECK (!journal_get_write_access_external (txn, b), "reserve dir %u", b);
  __atomic_add_fetch (&reservations, 1, __ATOMIC_RELAXED);
  maybe_dir_pageout (b);
  if (give_up)
    {
      jt_trace (b, "release_access", NULL);
      journal_release_access (txn, b);
      __atomic_add_fetch (&releases, 1, __ATOMIC_RELAXED);
      return;
    }
  pattern (buf, b, v);
  /* In two halves, with the pager possibly writing the page between
     them: a torn edit must never commit.  */
  jt_dir_edit (b, 0, buf, block_size / 2);
  maybe_dir_pageout (b);
  jt_dir_edit (b, block_size / 2, buf + block_size / 2, block_size / 2);
  maybe_dir_pageout (b);
  CHECK (!jt_dir_mark_dirty (txn, b), "mark dir %u", b);
  __atomic_add_fetch (&marks, 1, __ATOMIC_RELAXED);
  maybe_dir_pageout (b);
}

static void *
rpc_thread (void *arg)
{
  int t = (intptr_t) arg;
  uint32_t ver[NB] = { 0 };

  for (uint32_t i = 1; i <= (uint32_t) ops; i++)
    {
      diskfs_transaction_t *txn = diskfs_journal_start_transaction ();
      int sync = i % 5 == 0;

      CHECK (txn, "no transaction");
      /* At least one disk-cache block, so every operation changes the
	 thread's state.  */
      for (int k = 0; k < K; k++)
	if (k == 0 || rand () % 2)
	  {
	    ver[k] = i;
	    journal_version (txn, blocks[t][k], i);
	    if (rand () % 6 == 0)
	      jt_pageout (blocks[t][k], rand () % 2, 0);
	  }
      for (int d = K; d < NB; d++)
	if (rand () % 3)
	  {
	    int give_up = rand () % 5 == 0;
	    dir_op (txn, blocks[t][d], i, give_up);
	    if (!give_up)
	      ver[d] = i;
	  }
      memcpy (snaps[t] + (size_t) i * NB, ver, sizeof ver);

      for (int d = K; d < NB; d++)
	jt_trace (blocks[t][d], sync ? "op end: commit" : "op end: stop",
		  NULL);
      if (sync)
	{
	  unsigned long before = __atomic_add_fetch (&seq_counter, 1,
						     __ATOMIC_SEQ_CST);
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

static const char *
check (size_t n, int torn, const char *img, void *arg)
{
  static char msg[512];
  FILE *f = fopen (img, "r");
  char *buf = malloc (block_size);
  unsigned long durable_seq = 0;
  const char *err = NULL;

  (void) torn;
  (void) arg;
  CHECK (f, "open %s", img);
  for (size_t d = 0; d < ndurables; d++)
    if (durables[d].log <= n && durables[d].seq > durable_seq)
      durable_seq = durables[d].seq;

  for (int t = 0; t < NT && !err; t++)
    {
      long got[NB];
      int need = 0, j;

      for (int k = 0; k < NB; k++)
	{
	  CHECK (!fseeko (f, (off_t) blocks[t][k] << log2_block_size,
			  SEEK_SET)
		 && fread (buf, block_size, 1, f) == 1, "read");
	  got[k] = pattern_version (buf, blocks[t][k]);
	}
      for (int i = 1; i <= ops; i++)
	if (seq[t][i] <= durable_seq)
	  need = i;

      for (j = ops; j >= 0; j--)
	{
	  int k;
	  for (k = 0; k < NB && got[k] == snaps[t][(size_t) j * NB + k]; k++)
	    ;
	  if (k == NB)
	    break;
	}

      if (j < 0 || j < need)
	{
	  int len = snprintf (msg, sizeof msg, "thread %d: ", t);
	  if (j < 0)
	    len += snprintf (msg + len, sizeof msg - len,
			     "the blocks match no operation:");
	  else
	    len += snprintf (msg + len, sizeof msg - len,
			     "recovered operation %d, but %d had stopped "
			     "before a commit that returned:", j, need);
	  for (int k = 0; k < NB; k++)
	    len += snprintf (msg + len, sizeof msg - len, " %s %u v%ld",
			     k < K ? "meta" : "dir", blocks[t][k], got[k]);
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
  block_t picked[NT * NB];
  unsigned long warnings;
  size_t checked;
  char *buf;

  srand (argc > 2 ? atoi (argv[2]) : 1);
  if (getenv ("JT_OPS"))
    ops = atoi (getenv ("JT_OPS"));
  jt_mount (image, 3, 0);
  pick_free_blocks (picked, NT * NB);
  for (int t = 0; t < NT; t++)
    {
      memcpy (blocks[t], picked + t * NB, sizeof blocks[t]);
      snaps[t] = calloc ((size_t) (ops + 1) * NB, sizeof *snaps[t]);
      seq[t] = calloc (ops + 1, sizeof *seq[t]);
    }
  durables = calloc (NT * ops, sizeof *durables);

  for (int t = 0; t < NT; t++)
    pthread_create (&threads[t], NULL, rpc_thread, (void *) (intptr_t) t);
  for (int t = 0; t < NT; t++)
    pthread_join (threads[t], NULL);

  /* The file pager has every directory block's last version, while the
     pager threads still run to fault evicted ones in.  */
  buf = malloc (block_size);
  for (int t = 0; t < NT; t++)
    for (int k = K; k < NB; k++)
      {
	jt_dir_read (blocks[t][k], buf);
	CHECK (pattern_version (buf, blocks[t][k])
	       == snaps[t][(size_t) ops * NB + k],
	       "dir block %u: the file pager has version %ld, not %u",
	       blocks[t][k], pattern_version (buf, blocks[t][k]),
	       snaps[t][(size_t) ops * NB + k]);
      }
  free (buf);

  warnings = jt_umount ();
  CHECK (warnings == 0, "%lu journal warnings", warnings);
  CHECK (jt_stale_reads () == 0, "%lu stale page-ins", jt_stale_reads ());

  /* Every block's last version is home, directory blocks included.  */
  for (int t = 0; t < NT; t++)
    for (int k = 0; k < NB; k++)
      {
	uint32_t want = snaps[t][(size_t) ops * NB + k];
	long v = pattern_version (jt_disk_block (blocks[t][k]), blocks[t][k]);
	CHECK (v == want, "thread %d %s block %u: version %ld on disk, %u "
	       "expected", t, k < K ? "meta" : "dir", blocks[t][k], v, want);
      }

  checked = jt_crash_check ("t_dirs", check, NULL);
  printf ("t_dirs: OK (%d threads x %d operations, %lu dir reservations, "
	  "%lu marked, %lu given up, %zu writes, %zu crash images)\n",
	  NT, ops, reservations, marks, releases, jt_log_count (), checked);
  return 0;
}
