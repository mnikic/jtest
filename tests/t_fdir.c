/* A directory's blocks through the harness's file pager (env/fpager.c),
   as dir.c reaches them: mapped with diskfs_get_filemap and vm_map, and
   journaled with journal_get_write_access_external and
   journal_mark_dirty_external.

   1. The root directory grows by NB blocks in one handle, as dir.c's
      EXTEND does: diskfs_grow, then each block allocated with
      ext2_getblk before it is written.  The last block is written before
      it is allocated instead: its page-in finds no block, and the write
      fault allocates it (pager_unlock_page).  Each block is an empty
      directory block, at version 1.
   2. Threads then each journal the next version of some of the blocks,
      in a handle with the root locked, through a mapping made for the
      operation.  Pages are evicted at random and read back, and some
      operations commit.
   After the unmount every block's last version is home; no page-in got
   other than what the eviction before it wrote; and every crash image is
   clean (e2fsck -fn) and holds the blocks as one operation left them, no
   older than one whose commit returned.

   A block's version: an empty directory block (one unused entry, the
   length of the block), then the version pattern of tests/common.h after
   its 8 bytes.  */

#include "files.h"

#define NB 6
#define NT 2
#define OPS 400
#define DIRENT_HDR 8

static block_t blocks[NB];
static size_t base_page;	/* The root's first new block.  */
static struct history h;		/* Under the root's lock.  */

static long
dir_version (const void *buf, block_t b)
{
  const uint32_t *w = buf;
  int zero = 1;
  for (size_t i = 0; i < block_size / 4 && zero; i++)
    zero = !w[i];
  if (zero)
    return 0;
  if (w[0] != 0 || (w[1] & 0xffff) != block_size)
    return -1;
  uint32_t v = w[3];
  for (size_t i = DIRENT_HDR / 4; i < block_size / 4; i += 2)
    if (w[i] != b || w[i + 1] != v)
      return -1;
  return v;
}

static void
dir_block (char *p, block_t b, uint32_t v)
{
  uint32_t *w = (uint32_t *) p;
  w[0] = 0;
  w[1] = block_size;		/* rec_len; name_len and type 0.  */
  for (size_t i = DIRENT_HDR / 4; i < block_size / 4; i += 2)
    {
      w[i] = b;
      w[i + 1] = v;
    }
}

static char *
map_root (void)
{
  vm_address_t a;
  mach_port_t obj = diskfs_get_filemap (root, VM_PROT_READ | VM_PROT_WRITE);
  CHECK (!vm_map (mach_task_self (), &a,
		  (base_page + NB) << log2_block_size, 0, 1, obj, 0, 0,
		  VM_PROT_READ | VM_PROT_WRITE, VM_PROT_READ | VM_PROT_WRITE,
		  0), "vm_map the root");
  return (char *) a;
}

/* Version V of block K, journaled in TXN through MAP.  */
static void
write_block (diskfs_transaction_t *txn, char *map, int k, uint32_t v)
{
  char *p = map + ((base_page + k) << log2_block_size);
  CHECK (!journal_get_write_access_external (txn, blocks[k]),
	 "reserve block %u", blocks[k]);
  dir_block (p, blocks[k], v);
  CHECK (!journal_mark_dirty_external (txn, blocks[k], p), "mark %u",
	 blocks[k]);
}

static void
grow (void)
{
  struct disknode *dn = diskfs_node_disknode (root);
  diskfs_transaction_t *txn = diskfs_journal_start_transaction ();
  pthread_mutex_lock (&root->lock);
  base_page = root->dn_stat.st_size >> log2_block_size;
  CHECK (!diskfs_grow (root, (base_page + NB) << log2_block_size, &cred),
	 "grow the root");
  char *map = map_root ();

  /* The last block: written first, allocated by the write fault.  */
  char *last = map + ((base_page + NB - 1) << log2_block_size);
  last[DIRENT_HDR] = 1;
  pthread_rwlock_rdlock (&dn->alloc_lock);
  CHECK (!ext2_getblk (root, base_page + NB - 1, 0, &blocks[NB - 1])
	 && blocks[NB - 1], "the write fault allocated no block");
  pthread_rwlock_unlock (&dn->alloc_lock);

  for (int k = 0; k < NB; k++)
    {
      if (k < NB - 1)
	{
	  /* dirblock_get_write_access, as for an EXTEND.  */
	  pthread_rwlock_wrlock (&dn->alloc_lock);
	  CHECK (!ext2_getblk (root, base_page + k, 1, &blocks[k]),
		 "allocate block %d", k);
	  pthread_rwlock_unlock (&dn->alloc_lock);
	}
      write_block (txn, map, k, 1);
    }
  root->dn_stat.st_size = (base_page + NB) << log2_block_size;
  root->dn_set_mtime = root->dn_set_ctime = 1;
  diskfs_node_update (root, 0);
  munmap (map, (base_page + NB) << log2_block_size);
  pthread_mutex_unlock (&root->lock);
  for (int k = 0; k < NB; k++)
    h.ver[k] = 1;
  CHECK (!jt_commit (txn), "commit the growth");
  history_op_done (&h, 1);
}

static void *
worker (void *arg)
{
  unsigned seed = (unsigned) (uintptr_t) arg;
  char buf[4096];

  for (int op = 0; op < OPS; op++)
    {
      diskfs_transaction_t *txn = diskfs_journal_start_transaction ();
      pthread_mutex_lock (&root->lock);
      char *map = map_root ();
      int n = 2 + rand_r (&seed) % 3;
      for (int i = 0; i < n; i++)
	{
	  int k = rand_r (&seed) % NB;
	  write_block (txn, map, k, ++h.ver[k]);
	}
      int sync = rand_r (&seed) % 6 == 0;
      munmap (map, (base_page + NB) << log2_block_size);
      /* The operations are in the order they hold the root.  */
      history_op_done (&h, 0);
      int j = h.nops;
      pthread_mutex_unlock (&root->lock);
      if (sync)
	{
	  CHECK (!jt_commit (txn), "commit");
	  /* Durable from here on.  */
	  __atomic_store_n (&h.synced[j], jt_log_count (), __ATOMIC_RELEASE);
	}
      else
	diskfs_journal_stop_transaction (txn);

      /* Memory pressure, and a reader.  Blocks must go home often enough
	 for transactions to leave the log: until then the replay writes
	 a block's last committed copy over whatever is home.  */
      for (int e = rand_r (&seed) % 3; e > 0; e--)
	jt_fp_pageout (root, base_page + rand_r (&seed) % NB, 1, 0);
      if (rand_r (&seed) % 3 == 0)
	{
	  txn = diskfs_journal_start_transaction ();
	  pthread_mutex_lock (&root->lock);
	  map = map_root ();
	  memcpy (buf, map + ((base_page + rand_r (&seed) % NB)
			      << log2_block_size), sizeof buf);
	  munmap (map, (base_page + NB) << log2_block_size);
	  pthread_mutex_unlock (&root->lock);
	  diskfs_journal_stop_transaction (txn);
	}
    }
  return NULL;
}

int
main (int argc, char **argv)
{
  const char *image = argc > 1 ? argv[1] : "build/balloc.img";
  unsigned seed = argc > 2 ? atoi (argv[2]) : 1;
  pthread_t t[NT];

  jt_mount (image, 3, 0);
  jt_inode_globals ();
  files_init ();
  CHECK (block_size == 4096, "4096-byte blocks expected");
  history_init (&h, blocks, NB, 1 + NT * OPS);
  h.version = dir_version;

  grow ();
  for (int i = 0; i < NT; i++)
    pthread_create (&t[i], NULL, worker, (void *) (uintptr_t) (seed * 31 + i));
  for (int i = 0; i < NT; i++)
    pthread_join (t[i], NULL);

  unsigned long warnings = jt_umount ();
  CHECK (warnings == 0, "%lu journal warnings", warnings);
  CHECK (jt_fp_stale_reads () == 0, "%lu stale file page-ins",
	 jt_fp_stale_reads ());
  for (int k = 0; k < NB; k++)
    CHECK (dir_version (jt_disk_block (blocks[k]), blocks[k]) == h.ver[k],
	   "block %u: version %ld home, %u expected", blocks[k],
	   dir_version (jt_disk_block (blocks[k]), blocks[k]), h.ver[k]);
  jt_save_image ("build/t_fdir.img");
  CHECK (jt_run ("e2fsck -fn build/t_fdir.img >build/t_fdir.fsck 2>&1") == 0,
	 "e2fsck -fn is not clean after the unmount (see build/t_fdir.fsck)");

  unsigned long pi, po, un;
  jt_fp_counts (&pi, &po, &un);
  size_t checked = jt_crash_check ("t_fdir", history_check, &h);
  printf ("t_fdir: OK (%d blocks, %d threads x %d operations, %lu page-ins, "
	  "%lu page-outs, %lu unlocks, %zu writes, %zu crash images)\n", NB,
	  NT, OPS, pi, po, un, jt_log_count (), checked);
  return 0;
}
