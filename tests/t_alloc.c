/* Freed blocks, busy until their free commits, and reused as metadata or
   as file data.

   A model of balloc.c's allocator runs over a pool of blocks.  Each
   operation is one handle that allocates blocks as metadata (journaled
   through the disk cache) or as file data, edits metadata, and frees
   blocks, as free_blocks does: journal_record_freed_blocks, which
   revokes the copies in the log, and the block kept busy until
   ext2_release_busy_blocks hands it back.  With no free block, the
   allocator waits with journal_wait_freed_blocks, as ext2_new_block
   does; that only waits for a commit in flight, so a second thread keeps
   committing, as fsyncs of other files would.  Between operations the
   file pager writes file data home.

   A metadata block's page is dropped before its free, waiting for a
   write of it in flight, as ext2fs does; a block that changes owner has
   its old pages dropped (jt_discard).

   What must hold: after a replay, the metadata is that of one operation,
   no older than any whose commit returned or whose frees were handed
   back; and a block that is file data in that operation holds the last
   data written to it for that allocation.  An old journal copy of the
   block, from when it was metadata, must never be replayed or
   checkpointed over it.  File data goes home outside transactions
   (writeback mode), so a block whose allocation did not commit may hold
   any data; it is free again.  */

#include <pthread.h>
#include "common.h"

#define NP 40			/* Pool blocks.  */
#define OPS 400

enum role { FREE, META, DATA };

struct bstate
{
  uint8_t role;
  uint8_t busy;			/* Freed, its free not yet committed.  */
  uint32_t ver;			/* META: version.  */
  uint32_t epoch;		/* DATA: which allocation as data.  */
};

static block_t pool[NP];
static struct bstate now[NP];
static struct bstate snaps[OPS + 1][NP];
static size_t synced[OPS + 1];	/* Log length when op J's commit returned.  */
static int nops;
static uint32_t next_epoch = 1;
static uint32_t data_ver[NP];	/* Last data version written.  */

/* Busy blocks: the operation that freed them, and the log length when
   they were handed back.  */
static pthread_mutex_t alloc_lock = PTHREAD_MUTEX_INITIALIZER;
static int freed_by[NP];
struct release
{
  int op;
  size_t log;
};
static struct release releases[OPS * NP];
static size_t nreleases;
static unsigned long n_frees, n_reuses_data, n_reuses_meta, n_waits;

/* The other thread's commits: every operation done before one started is
   durable once it returned.  */
static int done_ops;		/* Operations whose handle is released.  */
static int stop_committer;
static struct release commits[100000];
static size_t ncommits;

static void *
committer (void *arg)
{
  (void) arg;
  while (!__atomic_load_n (&stop_committer, __ATOMIC_ACQUIRE))
    {
      int done = __atomic_load_n (&done_ops, __ATOMIC_ACQUIRE);
      diskfs_transaction_t *txn = diskfs_journal_start_transaction ();
      if (!txn)
	break;
      diskfs_journal_set_sync (txn);
      diskfs_journal_commit_transaction (txn);
      pthread_mutex_lock (&alloc_lock);
      if (ncommits < sizeof commits / sizeof commits[0])
	commits[ncommits++] = (struct release) { done, jt_log_count () };
      pthread_mutex_unlock (&alloc_lock);
      usleep (200);
    }
  return NULL;
}

static int
pool_index (block_t b)
{
  for (int k = 0; k < NP; k++)
    if (pool[k] == b)
      return k;
  return -1;
}

static void
on_release (block_t start, unsigned long count)
{
  size_t log = jt_log_count ();

  pthread_mutex_lock (&alloc_lock);
  for (unsigned long i = 0; i < count; i++)
    {
      int k = pool_index (start + i);
      CHECK (k >= 0, "released block %lu is not in the pool",
	     (unsigned long) start + i);
      CHECK (now[k].busy, "block %lu released twice",
	     (unsigned long) start + i);
      now[k].busy = 0;
      releases[nreleases++] = (struct release) { freed_by[k], log };
    }
  pthread_mutex_unlock (&alloc_lock);
}

static uint32_t
data_word (int k)
{
  return 0x80000000u | now[k].epoch << 12 | data_ver[k];
}

/* Fill BUF with file data: as pattern, with the data flag in the
   version word.  */
static void
data_pattern (void *buf, block_t b, uint32_t word)
{
  pattern (buf, b, word);
}

/* A free block, or -1.  ALLOC_LOCK is held.  */
static int
find_free (void)
{
  int start = rand () % NP;
  for (int i = 0; i < NP; i++)
    {
      int k = (start + i) % NP;
      if (now[k].role == FREE && !now[k].busy)
	return k;
    }
  return -1;
}

static int
alloc_block (diskfs_transaction_t *txn)
{
  int k, busy = 0;

  pthread_mutex_lock (&alloc_lock);
  k = find_free ();
  for (int i = 0; i < NP; i++)
    busy += now[i].busy;
  pthread_mutex_unlock (&alloc_lock);
  if (k < 0 && busy && journal_wait_freed_blocks (txn))
    {
      n_waits++;
      pthread_mutex_lock (&alloc_lock);
      k = find_free ();
      pthread_mutex_unlock (&alloc_lock);
    }
  return k;
}

static void
free_block (diskfs_transaction_t *txn, int k, int op)
{
  if (now[k].role == DATA)
    /* Truncation drops the file's page.  */
    jt_discard (pool[k], 1);
  else
    /* ext2fs drops a metadata block's page before it frees the block,
       and waits for a write of it in flight (truncate.c,
       flush_global_ptr), which must not land once the block has a new
       owner.  */
    pager_flush_some (diskfs_disk_pager,
		      (vm_address_t) pool[k] << log2_block_size, block_size, 1);
  CHECK (journal_record_freed_blocks (txn, pool[k], 1),
	 "cannot record the free of block %u", pool[k]);
  pthread_mutex_lock (&alloc_lock);
  now[k].role = FREE;
  now[k].busy = 1;
  freed_by[k] = op;
  pthread_mutex_unlock (&alloc_lock);
  n_frees++;
}

/* The latest write of file data to pool block K in the first N log
   entries that reached the disk, as its version word, or 0.  *SKIP is
   set if the crash tore a write of K after it.  */
static uint32_t
last_data_write (int k, size_t n, int *skip)
{
  *skip = 0;
  for (size_t i = n; i-- > 0;)
    {
      const uint32_t *d = jt_write_block (jt_log_entry (i), pool[k]);
      if (!d || !jt_crash_kept (i))
	continue;
      if (jt_crash_torn_at (i))
	{
	  *skip = 1;
	  return 0;
	}
      if (d[0] == pool[k] && (d[1] & 0x80000000u))
	return d[1];
    }
  return 0;
}

static const char *
check (size_t n, int torn, const char *img, void *arg)
{
  static char msg[512];
  FILE *f = fopen (img, "r");
  char *buf = malloc (block_size);
  long got[NP];
  uint32_t data[NP];
  int skip[NP], durable = 0, j;

  (void) torn;
  (void) arg;
  CHECK (f, "open %s", img);
  for (int k = 0; k < NP; k++)
    {
      CHECK (!fseeko (f, (off_t) pool[k] << log2_block_size, SEEK_SET)
	     && fread (buf, block_size, 1, f) == 1, "read");
      got[k] = pattern_version (buf, pool[k]);
      data[k] = last_data_write (k, n, &skip[k]);
    }
  fclose (f);
  free (buf);

  for (j = 1; j <= nops; j++)
    if (synced[j] <= n)
      durable = j;
  for (size_t r = 0; r < nreleases; r++)
    if (releases[r].log <= n && releases[r].op > durable)
      durable = releases[r].op;
  for (size_t c = 0; c < ncommits; c++)
    if (commits[c].log <= n && commits[c].op > durable)
      durable = commits[c].op;

  /* The newest operation whose state the image holds.  */
  for (j = nops; j >= durable; j--)
    {
      int k;
      for (k = 0; k < NP; k++)
	{
	  const struct bstate *st = &snaps[j][k];
	  if (skip[k])
	    continue;
	  uint32_t e = data[k] >> 12 & 0x7ffff;
	  if (st->role == META && got[k] != (long) st->ver)
	    break;
	  if (st->role == DATA && data[k] && e == st->epoch
	      && got[k] != (long) data[k])
	    /* The last data written to the block's current owner, lost.  */
	    break;
	}
      if (k == NP)
	return NULL;
    }

  /* Explain against the oldest state the image may hold.  */
  int len = snprintf (msg, sizeof msg, "no state from operation %d on "
		      "matches; against %d:", durable, durable);
  for (int k = 0; k < NP && len < (int) sizeof msg - 80; k++)
    {
      const struct bstate *st = &snaps[durable][k];
      if (skip[k])
	continue;
      if (st->role == META && got[k] != (long) st->ver)
	len += snprintf (msg + len, sizeof msg - len,
			 " block %u holds %lx, meta v%u expected;",
			 pool[k], got[k], st->ver);
      else if (st->role == DATA && data[k]
	       && (data[k] >> 12 & 0x7ffff) == st->epoch
	       && got[k] != (long) data[k])
	len += snprintf (msg + len, sizeof msg - len,
			 " block %u holds %lx, its data is %x;", pool[k],
			 got[k], data[k]);

    }
  return msg;
}

int
main (int argc, char **argv)
{
  const char *image = argc > 1 ? argv[1] : "build/base.img";
  char *buf;
  unsigned long warnings;

  srand (argc > 2 ? atoi (argv[2]) : 1);
  jt_mount (image, 3, 0);
  pick_free_blocks (pool, NP);
  JT_SET_HOOK (jt_on_release, on_release);
  buf = malloc (block_size);
  pthread_t ct;
  pthread_create (&ct, NULL, committer, NULL);

  for (int op = 1; op <= OPS; op++)
    {
      diskfs_transaction_t *txn = diskfs_journal_start_transaction ();
      int sync = op % 10 == 0;

      CHECK (txn, "no transaction");

      /* Allocate.  */
      for (int i = rand () % 3; i > 0; i--)
	{
	  int k = alloc_block (txn);
	  if (k < 0)
	    break;
	  /* The new owner's pages start afresh.  */
	  jt_discard (pool[k], 0);
	  jt_discard (pool[k], 1);
	  pthread_mutex_lock (&alloc_lock);
	  if (rand () % 2)
	    {
	      now[k].role = META;
	      now[k].ver = op;
	      n_reuses_meta++;
	    }
	  else
	    {
	      now[k].role = DATA;
	      now[k].epoch = next_epoch++;
	      data_ver[k] = 0;
	      n_reuses_data++;
	    }
	  pthread_mutex_unlock (&alloc_lock);
	  if (now[k].role == META)
	    journal_version (txn, pool[k], op);
	}

      /* Edit metadata.  */
      for (int k = 0; k < NP; k++)
	if (now[k].role == META && rand () % 4 == 0)
	  {
	    now[k].ver = op;
	    journal_version (txn, pool[k], op);
	    if (rand () % 4 == 0)
	      jt_pageout (pool[k], rand () % 2, 0);
	  }

      /* Free; sometimes a block journaled in this very operation.  */
      for (int i = rand () % 3; i > 0; i--)
	{
	  int k = rand () % NP;
	  if (now[k].role != FREE)
	    free_block (txn, k, op);
	}

      /* on_release clears busy bits under alloc_lock.  */
      pthread_mutex_lock (&alloc_lock);
      memcpy (snaps[op], now, sizeof now);
      pthread_mutex_unlock (&alloc_lock);
      nops = op;
      if (sync)
	diskfs_journal_commit_transaction (txn);
      else
	diskfs_journal_stop_transaction (txn);
      synced[op] = sync ? jt_log_count () : SIZE_MAX;
      __atomic_store_n (&done_ops, op, __ATOMIC_RELEASE);

      /* The file pager writes file data home, outside any handle.  */
      for (int k = 0; k < NP; k++)
	if (now[k].role == DATA && rand () % 3 == 0)
	  {
	    data_ver[k]++;
	    data_pattern (buf, pool[k], data_word (k));
	    jt_file_edit (pool[k], 0, buf, block_size);
	    jt_file_pageout (pool[k], rand () % 2, rand () % 2);
	  }
    }

  __atomic_store_n (&stop_committer, 1, __ATOMIC_RELEASE);
  pthread_join (ct, NULL);

  /* diskfs_shutdown syncs the file pager before the journal stops.  */
  warnings = jt_umount ();
  CHECK (warnings == 0, "%lu journal warnings", warnings);
  CHECK (jt_stale_reads () == 0, "%lu stale page-ins", jt_stale_reads ());

  for (int k = 0; k < NP; k++)
    {
      long v = pattern_version (jt_disk_block (pool[k]), pool[k]);
      if (now[k].role == META)
	CHECK (v == now[k].ver, "meta block %u: v%ld home, v%u expected",
	       pool[k], v, now[k].ver);
      else if (now[k].role == DATA && data_ver[k])
	CHECK (v == data_word (k), "data block %u: %lx home, %x expected",
	       pool[k], v, data_word (k));
    }

  size_t checked = jt_crash_check ("t_alloc", check, NULL);
  printf ("t_alloc: OK (%d operations, %lu frees, %lu reused as metadata, "
	  "%lu as data, %lu allocator waits, %zu writes, %zu crash images)\n",
	  OPS, n_frees, n_reuses_meta, n_reuses_data, n_waits,
	  jt_log_count (), checked);
  free (buf);
  return 0;
}
