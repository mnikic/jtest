/* The real getblk.c and truncate.c under the journal.

   A file of the image (pool0) grows through ext2_getblk, appending or
   leaving holes, out to its double indirect blocks, and shrinks through
   diskfs_truncate to block boundaries, often just around the boundaries
   between direct, single and double indirect blocks.  Its blocks are
   allocated and freed by the real balloc.c, the indirect blocks
   journaled, the inode written by diskfs_node_update.  A filler thread
   keeps the log turning over, as in t_balloc.

   After each operation, the file's block map must be what the test
   expects: a truncation to L keeps every block below L where it was,
   and maps none from L on.  e2fsck alone would not see a truncation
   that freed the wrong blocks consistently.

   What must hold, after a replay: e2fsck -fn finds the image clean
   (jt_crash_check), and the inode holds the size, block count and block
   pointers of one operation, no older than one whose commit returned.  */

#include <pthread.h>
#include "common.h"

#define OPS 160

struct istate
{
  uint32_t size, blocks;
  uint32_t ib[EXT2_N_BLOCKS];
};

static struct node *np;
static ino_t ino;
static block_t itb;
static size_t ioff;
static struct istate snaps[OPS + 1];
static size_t synced[OPS + 1];
static unsigned long n_grow, n_trunc, n_enospc;
#define MAXB (12 + 1024 + 256)
static block_t model[MAXB];		/* Physical block of each, or 0.  */
static block_t longest;

/* The file's map as ext2_getblk has it, against the model.  */
static void
check_map (int op)
{
  pthread_rwlock_rdlock (&diskfs_node_disknode (np)->alloc_lock);
  for (block_t b = 0; b < MAXB; b++)
    {
      block_t disk = 0;
      error_t err = ext2_getblk (np, b, 0, &disk);
      CHECK (!err || err == EINVAL, "ext2_getblk (%u): %s", b,
	     strerror (err));
      if (err)
	disk = 0;
      CHECK (disk == model[b], "after operation %d: block %u of the file "
	     "maps to %u, %u expected", op, b, disk, model[b]);
    }
  pthread_rwlock_unlock (&diskfs_node_disknode (np)->alloc_lock);
}

static ino_t
lookup (const char *image, const char *name)
{
  char cmd[512], line[256];
  unsigned long found = 0;

  snprintf (cmd, sizeof cmd, "debugfs -R 'stat /%s' %s 2>/dev/null",
	    name, image);
  FILE *p = popen (cmd, "r");
  CHECK (p, "debugfs");
  while (fgets (line, sizeof line, p))
    if (sscanf (line, "Inode: %lu", &found) == 1)
      break;
  pclose (p);
  CHECK (found, "no /%s in %s", name, image);
  return found;
}

static void
snapshot (struct istate *st)
{
  st->size = np->dn_stat.st_size;
  st->blocks = np->dn_stat.st_blocks;
  for (int i = 0; i < EXT2_N_BLOCKS; i++)
    st->ib[i] = diskfs_node_disknode (np)->info.i_data[i];
}

/* Block counts just around the direct / single / double boundaries.  */
static block_t
interesting_length (block_t max)
{
  static const block_t marks[] = { 0, 12, 12 + 1024 };
  block_t m = marks[rand () % 3] + (rand () % 5) - 2;
  if (rand () % 3 == 0)
    m = rand () % (max + 1);
  if ((long) m < 0)
    m = 0;
  return m > max ? max : m;
}

static int stop_filler;

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
    }
  return NULL;
}

static const char *
check (size_t n, int torn, const char *img, void *arg)
{
  static char msg[256];
  FILE *f = fopen (img, "r");
  struct ext2_inode di;
  struct istate got;
  int durable = 0, j;

  (void) torn;
  (void) arg;
  CHECK (f, "open %s", img);
  CHECK (!fseeko (f, ((off_t) itb << log2_block_size) + ioff, SEEK_SET)
	 && fread (&di, sizeof di, 1, f) == 1, "read the inode");
  fclose (f);
  got.size = le32toh (di.i_size);
  got.blocks = le32toh (di.i_blocks);
  for (int i = 0; i < EXT2_N_BLOCKS; i++)
    got.ib[i] = le32toh (di.i_block[i]);

  for (j = 1; j <= OPS; j++)
    if (synced[j] <= n)
      durable = j;
  for (j = OPS; j >= 0; j--)
    if (!memcmp (&got, &snaps[j], sizeof got))
      break;
  if (j >= durable)
    return NULL;
  if (j < 0)
    snprintf (msg, sizeof msg, "the inode (size %u, %u sectors) matches no "
	      "operation", got.size, got.blocks);
  else
    snprintf (msg, sizeof msg, "recovered operation %d, but the commit of "
	      "%d had returned", j, durable);
  return msg;
}

int
main (int argc, char **argv)
{
  const char *image = argc > 1 ? argv[1] : "build/balloc.img";
  const char *out = "build/t_trunc.img";
  const block_t max = MAXB;
  unsigned long warnings;
  pthread_t ft;

  srand (argc > 2 ? atoi (argv[2]) : 1);
  ino = lookup (image, "pool0");
  jt_mount (image, 3, 0);
  jt_inode_location (ino, &itb, &ioff);
  np = jt_node_load (ino);
  snapshot (&snaps[0]);
  synced[0] = 0;
  pthread_create (&ft, NULL, filler, NULL);

  for (int op = 1; op <= OPS; op++)
    {
      diskfs_transaction_t *txn = diskfs_journal_start_transaction ();
      block_t len = np->dn_stat.st_size >> log2_block_size;
      int sync = op % 5 == 0;

      if (rand () % 5 < 3 && len < max)
	{
	  /* Grow, as diskfs_grow and pager_unlock_page do, sometimes past
	     a hole.  */
	  block_t from = len + (rand () % 5 == 0 ? rand () % 40 : 0);
	  block_t to = from + 1 + rand () % 150;
	  if (to > max)
	    to = max;
	  pthread_rwlock_wrlock (&diskfs_node_disknode (np)->alloc_lock);
	  for (block_t b = from; b < to; b++)
	    {
	      block_t disk;
	      error_t err = ext2_getblk (np, b, 1, &disk);
	      if (err)
		{
		  CHECK (err == ENOSPC, "ext2_getblk: %s", strerror (err));
		  n_enospc++;
		  to = b;
		  break;
		}
	      model[b] = disk;
	    }
	  if (to > longest)
	    longest = to;
	  pthread_rwlock_unlock (&diskfs_node_disknode (np)->alloc_lock);
	  if (to > len)
	    {
	      np->dn_stat.st_size = (off_t) to << log2_block_size;
	      np->allocsize = np->dn_stat.st_size;
	    }
	  diskfs_node_update (np, 0);
	  n_grow++;
	}
      else
	{
	  block_t to = interesting_length (len);
	  pthread_mutex_lock (&np->lock);
	  CHECK (!diskfs_truncate (np, (off_t) to << log2_block_size),
		 "diskfs_truncate");
	  pthread_mutex_unlock (&np->lock);
	  for (block_t b = to; b < MAXB; b++)
	    model[b] = 0;
	  n_trunc++;
	}
      snapshot (&snaps[op]);
      check_map (op);
      if (sync)
	diskfs_journal_commit_transaction (txn);
      else
	diskfs_journal_stop_transaction (txn);
      synced[op] = sync ? jt_log_count () : SIZE_MAX;
    }
  __atomic_store_n (&stop_filler, 1, __ATOMIC_RELEASE);
  pthread_join (ft, NULL);

  warnings = jt_umount ();
  CHECK (warnings == 0, "%lu journal warnings", warnings);
  CHECK (jt_stale_reads () == 0, "%lu stale page-ins", jt_stale_reads ());
  jt_save_image (out);
  CHECK (jt_run ("e2fsck -fn %s >build/t_trunc.fsck 2>&1", out) == 0,
	 "e2fsck -fn %s is not clean after the unmount (see "
	 "build/t_trunc.fsck)", out);

  size_t checked = jt_crash_check ("t_trunc", check, NULL);
  printf ("t_trunc: OK (%lu grows, %lu truncations, %lu ENOSPC, longest "
	  "%u blocks, %zu writes, %zu crash images)\n", n_grow, n_trunc,
	  n_enospc, longest, jt_log_count (), checked);
  return 0;
}
