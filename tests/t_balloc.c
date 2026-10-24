/* The real block allocator, balloc.c, under the journal.

   Two threads each own a file of the image (pool0 and pool1), and grow
   and shrink it.  Each operation is one handle: it allocates blocks with
   ext2_new_block and puts them in the file's direct slots or in an
   indirect block (itself allocated and zeroed through the journal),
   frees blocks with ext2_free_blocks, and writes the inode (block
   pointers, i_blocks, i_size) through the journal.  The two inodes share
   an inode table block, so its edits are serialized.  Between
   operations the file pager writes the files' data home, as writeback
   mode does: 0xa5 bytes, invalid block numbers if a block still read as
   an indirect block got them.

   ext2_new_block and ext2_free_blocks do their own journaling: the block
   bitmap, the group descriptor and, through alloc_sync and
   diskfs_set_hypermetadata, the superblock.  Freed blocks stay busy
   until their free commits.

   A third thread keeps the log turning over: it reserves and commits
   the inode table blocks of the other groups, unchanged, so that old
   copies leave the log and only the home copy of a block is left; a
   busy block reused too early then shows.

   Allocations go through a model of getblk.c's ext2_alloc_block, with a
   preallocation window per file, in the form the allocator linked in
   has.  With prealloc-rsv's balloc.c (ext2_unreserve_blocks), the window
   is a reservation in memory, and every allocation, a hit included,
   goes through ext2_new_block; a file that uses up its window and goes
   on sequentially gets one twice as big.  With JT_OLD_PREALLOC=1 and an
   allocator that has ext2_free_unused_blocks, the window's blocks are
   marked in the bitmap up front, as before d786df4c5 turned that off
   with a journal: a crash then leaves them allocated and owned by no
   one, which this test must catch.  Otherwise no window, as
   shadow-intercept's getblk.c does with a journal.

   What must hold, after a replay: e2fsck -fn finds the image clean, so
   bitmaps, counts and inodes agree (jt_crash_check checks that); and
   each file holds the blocks it had after one of its operations, no
   older than one that stopped before a commit that returned.  */

#include <pthread.h>
#include <stddef.h>
#include "common.h"

/* t_balloc_full.c builds this test under its own name.  */
#ifndef T_NAME
#define T_NAME "t_balloc"
#endif

#define NT 2
#define NSLOT 16		/* 12 direct, 4 in the indirect block, which
				   is thus often freed.  */
#define OPS 150

struct fstate
{
  uint32_t slot[NSLOT];
  uint32_t ind;
};

/* Present only in the allocators that have them.  */
extern void ext2_unreserve_blocks (block_t, unsigned long)
  __attribute__ ((weak));
extern void ext2_free_unused_blocks (block_t, unsigned long)
  __attribute__ ((weak));

enum { WIN_NONE, WIN_RESERVED, WIN_OLD } win_mode;

struct window
{
  block_t block;		/* i_prealloc_block */
  block_t count;		/* i_prealloc_count */
  block_t size;			/* i_prealloc_size */
};
static struct window win[NT];
static unsigned long win_hits;

/* ext2_discard_prealloc.  */
static void
discard_window (int t)
{
  if (!win[t].count)
    return;
  if (win_mode == WIN_RESERVED)
    ext2_unreserve_blocks (win[t].block, win[t].count);
  else
    ext2_free_unused_blocks (win[t].block, win[t].count);
  win[t].count = 0;
}

/* ext2_alloc_block's choice of block, for file T.  */
static block_t
alloc_block (int t, block_t goal)
{
  struct window *w = &win[t];
  int sequential = goal == w->block || goal + 1 == w->block;

  if (win_mode == WIN_NONE)
    return ext2_new_block (goal, 0, 0, 0);

  if (w->count && sequential)
    {
      __atomic_add_fetch (&win_hits, 1, __ATOMIC_RELAXED);
      if (win_mode == WIN_OLD)
	{
	  /* The block is already marked allocated.  */
	  w->count--;
	  return w->block++;
	}
      goal = w->block;
    }
  else
    {
      discard_window (t);
      if (win_mode == WIN_RESERVED && sequential && w->size)
	w->size = w->size * 2 < 1024 ? w->size * 2 : 1024;
      else
	w->size = EXT2_DEFAULT_PREALLOC_BLOCKS;
    }
  return ext2_new_block (goal, w->size, &w->count, &w->block);
}

static ino_t inos[NT];
static block_t itb[NT];		/* Inode table block.  */
static size_t ioff[NT];		/* Offset in it.  */
static struct fstate now[NT];
static struct fstate snaps[NT][OPS + 1];
static unsigned long seq[NT][OPS + 1];
static unsigned long seq_counter;
static pthread_mutex_t inode_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned long n_alloc, n_free, n_enospc;
static block_t last_freed[NT];	/* Goals for the other thread.  */
static block_t last_freed_ind[NT];
static int stop_filler;
static unsigned long filler_commits;

/* Reserve, and commit unchanged, inode table blocks of groups 1 on:
   their inodes are unused.  */
static void *
filler (void *arg)
{
  (void) arg;
  while (!__atomic_load_n (&stop_filler, __ATOMIC_ACQUIRE))
    {
      diskfs_transaction_t *txn = diskfs_journal_start_transaction ();
      for (unsigned long g = 1; g < groups_count; g++)
	{
	  block_t first = le32toh (group_desc (g)->bg_inode_table);
	  for (unsigned long i = 0; i < itb_per_group; i++)
	    {
	      CHECK (!journal_get_write_access (txn, first + i), "filler");
	      journal_mark_dirty (txn, first + i);
	    }
	}
      diskfs_journal_commit_transaction (txn);
      filler_commits++;
    }
  return NULL;
}

struct durable
{
  unsigned long seq;
  size_t log;
};
static pthread_mutex_t durable_lock = PTHREAD_MUTEX_INITIALIZER;
static struct durable durables[OPS * NT + 1];
static size_t ndurables;

static ino_t
lookup (const char *image, const char *name)
{
  char cmd[512], line[256];
  unsigned long ino = 0;

  snprintf (cmd, sizeof cmd, "debugfs -R 'stat /%s' %s 2>/dev/null",
	    name, image);
  FILE *p = popen (cmd, "r");
  CHECK (p, "debugfs");
  while (fgets (line, sizeof line, p))
    if (sscanf (line, "Inode: %lu", &ino) == 1)
      break;
  pclose (p);
  CHECK (ino, "no /%s in %s", name, image);
  return ino;
}

static void
edit_word (diskfs_transaction_t *txn, block_t b, size_t off, uint32_t v)
{
  uint32_t le = htole32 (v);
  CHECK (!journal_get_write_access (txn, b), "reserve %u", b);
  jt_edit (b, off, &le, sizeof le);
  journal_mark_dirty (txn, b);
}

/* Write file T's inode: block pointers, i_blocks and i_size.  */
static void
write_inode (diskfs_transaction_t *txn, int t)
{
  struct fstate *f = &now[t];
  uint32_t blocks = f->ind ? 1 : 0, size = 0;
  uint32_t ib[EXT2_N_BLOCKS] = { 0 };

  for (int s = 0; s < NSLOT; s++)
    if (f->slot[s])
      {
	blocks++;
	size = (s + 1) * block_size;
      }
  for (int s = 0; s < EXT2_NDIR_BLOCKS; s++)
    ib[s] = htole32 (f->slot[s]);
  ib[EXT2_IND_BLOCK] = htole32 (f->ind);

  uint32_t le_size = htole32 (size), le_blocks = htole32 (blocks
							  * (block_size / 512));
  pthread_mutex_lock (&inode_lock);
  CHECK (!journal_get_write_access (txn, itb[t]), "reserve the inode");
  jt_edit (itb[t], ioff[t] + offsetof (struct ext2_inode, i_size),
	   &le_size, sizeof le_size);
  jt_edit (itb[t], ioff[t] + offsetof (struct ext2_inode, i_blocks),
	   &le_blocks, sizeof le_blocks);
  jt_edit (itb[t], ioff[t] + offsetof (struct ext2_inode, i_block),
	   ib, sizeof ib);
  journal_mark_dirty (txn, itb[t]);
  pthread_mutex_unlock (&inode_lock);
}

static block_t
random_goal (void)
{
  return le32toh (sblock->s_first_data_block)
    + rand () % (le32toh (sblock->s_blocks_count)
		 - le32toh (sblock->s_first_data_block));
}

static void *
owner (void *arg)
{
  int t = (intptr_t) arg;
  struct fstate *f = &now[t];
  char *zero = calloc (1, block_size);
  char *data = malloc (block_size);

  memset (data, 0xa5, block_size);

  for (int op = 1; op <= OPS; op++)
    {
      diskfs_transaction_t *txn = diskfs_journal_start_transaction ();
      int sync = op % 10 == 0;

      /* Allocate.  */
      for (int i = rand () % 4; i > 0; i--)
	{
	  /* Mostly appends, as files are written: the next slot after
	     the last one used, so that windows are hit and grow.  */
	  int s = rand () % NSLOT;
	  if (rand () % 4)
	    {
	      int last = -1;
	      for (int k = 0; k < NSLOT; k++)
		if (f->slot[k])
		  last = k;
	      s = last + 1 < NSLOT ? last + 1 : s;
	    }
	  if (f->slot[s])
	    continue;
	  /* Half of the time, the block the other thread freed last: taking
	     it before its free commits is what the busy rule forbids.  */
	  block_t other = __atomic_load_n (&last_freed[1 - t], __ATOMIC_RELAXED);
	  block_t other_ind = __atomic_load_n (&last_freed_ind[1 - t],
					       __ATOMIC_RELAXED);
	  int r = rand () % 4;
	  block_t goal = other_ind && r == 0 ? other_ind
	    : other && r == 1 ? other
	    : s > 0 && f->slot[s - 1] ? f->slot[s - 1] + 1 : random_goal ();
	  if (s >= EXT2_NDIR_BLOCKS && !f->ind)
	    {
	      block_t ind = alloc_block (t, goal);
	      if (!ind)
		{
		  __atomic_add_fetch (&n_enospc, 1, __ATOMIC_RELAXED);
		  break;
		}
	      CHECK (!journal_get_write_access (txn, ind), "reserve ind");
	      jt_edit (ind, 0, zero, block_size);
	      journal_mark_dirty (txn, ind);
	      f->ind = ind;
	      __atomic_add_fetch (&n_alloc, 1, __ATOMIC_RELAXED);
	    }
	  block_t b = alloc_block (t, goal);
	  if (!b)
	    {
	      __atomic_add_fetch (&n_enospc, 1, __ATOMIC_RELAXED);
	      break;
	    }
	  /* A new owner: ext2_new_block cleared its modified bit.  */
	  jt_discard (b, 0);
	  jt_discard (b, 1);
	  f->slot[s] = b;
	  /* Its data goes home soon: writeback mode does not wait for the
	     allocation to commit.  */
	  jt_file_edit (b, 0, data, block_size);
	  jt_file_pageout (b, 1, 0);
	  if (s >= EXT2_NDIR_BLOCKS)
	    edit_word (txn, f->ind, (s - EXT2_NDIR_BLOCKS) * 4, b);
	  __atomic_add_fetch (&n_alloc, 1, __ATOMIC_RELAXED);
	}

      /* Free, as a truncation, which drops the window.  */
      if (rand () % 3 == 0)
	discard_window (t);
      for (int i = rand () % 3; i > 0; i--)
	{
	  int s = rand () % NSLOT;
	  if (!f->slot[s])
	    continue;
	  /* Truncation drops the file's page of the block.  */
	  jt_discard (f->slot[s], 1);
	  ext2_free_blocks (f->slot[s], 1);
	  __atomic_store_n (&last_freed[t], f->slot[s], __ATOMIC_RELAXED);
	  if (s >= EXT2_NDIR_BLOCKS)
	    edit_word (txn, f->ind, (s - EXT2_NDIR_BLOCKS) * 4, 0);
	  f->slot[s] = 0;
	  __atomic_add_fetch (&n_free, 1, __ATOMIC_RELAXED);
	}
      if (f->ind)
	{
	  int any = 0;
	  for (int s = EXT2_NDIR_BLOCKS; s < NSLOT; s++)
	    any |= f->slot[s] != 0;
	  if (!any)
	    {
	      ext2_free_blocks (f->ind, 1);
	      __atomic_store_n (&last_freed_ind[t], f->ind, __ATOMIC_RELAXED);
	      f->ind = 0;
	      __atomic_add_fetch (&n_free, 1, __ATOMIC_RELAXED);
	    }
	}

      write_inode (txn, t);
      if (rand () % 4 == 0)
	jt_pageout (itb[t], rand () % 2, 0);
      snaps[t][op] = *f;

      if (sync)
	{
	  unsigned long before = __atomic_add_fetch (&seq_counter, 1,
						     __ATOMIC_SEQ_CST);
	  seq[t][op] = before;
	  diskfs_journal_commit_transaction (txn);
	  pthread_mutex_lock (&durable_lock);
	  durables[ndurables++] = (struct durable) { before, jt_log_count () };
	  pthread_mutex_unlock (&durable_lock);
	}
      else
	{
	  diskfs_journal_stop_transaction (txn);
	  seq[t][op] = __atomic_add_fetch (&seq_counter, 1, __ATOMIC_SEQ_CST);
	}

      /* The file pager writes some data home, outside any handle.  */
      for (int s = 0; s < NSLOT; s++)
	if (f->slot[s] && rand () % 4 == 0)
	  {
	    jt_file_edit (f->slot[s], 0, data, block_size);
	    jt_file_pageout (f->slot[s], rand () % 2, 0);
	  }
    }
  /* The last close gives the window back.  */
  diskfs_transaction_t *txn = diskfs_journal_start_transaction ();
  discard_window (t);
  diskfs_journal_stop_transaction (txn);
  free (zero);
  free (data);
  return NULL;
}

/* File T's state in the image F.  */
static void
read_file (FILE *f, int t, struct fstate *st)
{
  struct ext2_inode di;
  uint32_t *ind = malloc (block_size);

  memset (st, 0, sizeof *st);
  CHECK (!fseeko (f, ((off_t) itb[t] << log2_block_size) + ioff[t], SEEK_SET)
	 && fread (&di, sizeof di, 1, f) == 1, "read the inode");
  for (int s = 0; s < EXT2_NDIR_BLOCKS; s++)
    st->slot[s] = le32toh (di.i_block[s]);
  st->ind = le32toh (di.i_block[EXT2_IND_BLOCK]);
  if (st->ind)
    {
      CHECK (!fseeko (f, (off_t) st->ind << log2_block_size, SEEK_SET)
	     && fread (ind, block_size, 1, f) == 1, "read the indirect block");
      for (int s = EXT2_NDIR_BLOCKS; s < NSLOT; s++)
	st->slot[s] = le32toh (ind[s - EXT2_NDIR_BLOCKS]);
    }
  free (ind);
}

static const char *
check (size_t n, int torn, const char *img, void *arg)
{
  static char msg[256];
  FILE *f = fopen (img, "r");
  unsigned long durable_seq = 0;
  const char *err = NULL;

  (void) torn;
  (void) arg;
  CHECK (f, "open %s", img);
  for (size_t d = 0; d < ndurables; d++)
    if (durables[d].log <= n && durables[d].seq > durable_seq)
      durable_seq = durables[d].seq;

  /* The superblock's free count is that of the groups: balloc.c changes
     them in the same transaction.  e2fsck does not check it.  */
  struct ext2_super_block sb;
  CHECK (!fseeko (f, SBLOCK_OFFS, SEEK_SET) && fread (&sb, sizeof sb, 1, f) == 1,
	 "read the superblock");
  unsigned long sum = 0;
  for (unsigned long g = 0; g < groups_count; g++)
    {
      struct ext2_group_desc gd;
      CHECK (!fseeko (f, ((off_t) group_desc_block << log2_block_size)
		      + g * sizeof gd, SEEK_SET)
	     && fread (&gd, sizeof gd, 1, f) == 1, "read a group descriptor");
      sum += le16toh (gd.bg_free_blocks_count);
    }
  if (sum != le32toh (sb.s_free_blocks_count))
    {
      snprintf (msg, sizeof msg, "the superblock counts %u free blocks, "
		"the groups %lu", le32toh (sb.s_free_blocks_count), sum);
      err = msg;
    }

  for (int t = 0; t < NT && !err; t++)
    {
      struct fstate got;
      int need = 0, j;

      read_file (f, t, &got);
      for (int op = 1; op <= OPS; op++)
	if (seq[t][op] <= durable_seq)
	  need = op;
      for (j = OPS; j >= 0; j--)
	if (!memcmp (&got, &snaps[t][j], sizeof got))
	  break;
      if (j < 0)
	snprintf (msg, sizeof msg, "file %d holds blocks of no operation", t);
      else if (j < need)
	snprintf (msg, sizeof msg, "file %d: recovered operation %d, but "
		  "%d had stopped before a commit that returned", t, j, need);
      else
	continue;
      err = msg;
    }
  fclose (f);
  return err;
}

int
main (int argc, char **argv)
{
  const char *image = argc > 1 ? argv[1] : "build/balloc.img";
  const char *out = "build/" T_NAME ".img";
  pthread_t threads[NT];
  unsigned long warnings;

  srand (argc > 2 ? atoi (argv[2]) : 1);
  if (ext2_unreserve_blocks)
    win_mode = WIN_RESERVED;
  else if (getenv ("JT_OLD_PREALLOC") && ext2_free_unused_blocks)
    win_mode = WIN_OLD;
  inos[0] = lookup (image, "pool0");
  inos[1] = lookup (image, "pool1");
  jt_mount (image, 3, 0);
  for (int t = 0; t < NT; t++)
    jt_inode_location (inos[t], &itb[t], &ioff[t]);

  pthread_t ft;
  pthread_create (&ft, NULL, filler, NULL);
  for (int t = 0; t < NT; t++)
    pthread_create (&threads[t], NULL, owner, (void *) (intptr_t) t);
  for (int t = 0; t < NT; t++)
    pthread_join (threads[t], NULL);
  __atomic_store_n (&stop_filler, 1, __ATOMIC_RELEASE);
  pthread_join (ft, NULL);

  warnings = jt_umount ();
  CHECK (warnings == 0, "%lu journal warnings", warnings);
  CHECK (jt_stale_reads () == 0, "%lu stale page-ins", jt_stale_reads ());
  jt_save_image (out);
  CHECK (jt_run ("e2fsck -fn %s >%s.fsck 2>&1", out, out) == 0,
	 "e2fsck -fn %s is not clean after the unmount (see %s.fsck)", out,
	 out);

  size_t checked = jt_crash_check (T_NAME, check, NULL);
  printf (T_NAME ": OK (%s windows, %d threads x %d operations, "
	  "%lu allocations, %lu window hits, %lu frees, %lu ENOSPC, "
	  "%lu filler commits, %zu writes, %zu crash images)\n",
	  win_mode == WIN_RESERVED ? "reserved" : win_mode == WIN_OLD
	  ? "bitmap" : "no", NT, OPS, n_alloc, win_hits, n_free, n_enospc,
	  filler_commits, jt_log_count (), checked);
  return 0;
}
