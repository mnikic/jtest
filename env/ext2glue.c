/* The rest of ext2fs and libdiskfs that the real getblk.c and truncate.c
   need, for nodes the harness builds (jt_node_load).  Linked only into
   the tests that use them (see the Makefile).  Such nodes have no file
   pager, so what only a pager reaches is not here, or aborts.  */

#include <stdlib.h>
#include <string.h>
#include "jtest.h"

unsigned long addr_per_block;
unsigned log2_stat_blocks_per_fs_block;
vm_address_t zeroblock;
pthread_spinlock_t node_to_page_lock;
__thread struct disk_image_user *diskfs_exception_diu;

static void __attribute__ ((constructor))
glue_init (void)
{
  pthread_spin_init (&node_to_page_lock, PTHREAD_PROCESS_PRIVATE);
}

static void __attribute__ ((noreturn))
not_here (const char *what)
{
  _ext2_panic (what, "not available in the harness: a node with a file "
	       "pager, or a truncation inside a block");
}

int
diskfs_check_readonly (void)
{
  return 0;
}

void
diskfs_file_update (struct node *np, int wait)
{
  diskfs_node_update (np, wait);
}

mach_port_t
diskfs_get_filemap (struct node *np, vm_prot_t prot)
{
  (void) np;
  (void) prot;
  not_here (__func__);
}

error_t
diskfs_node_rdwr (struct node *np, char *data, loff_t off,
		  mach_msg_type_number_t amt, int dir, struct protid *cred,
		  mach_msg_type_number_t *amtread)
{
  (void) np; (void) data; (void) off; (void) amt; (void) dir;
  (void) cred; (void) amtread;
  not_here (__func__);
}

kern_return_t
vm_map (task_t t, vm_address_t *a, vm_size_t s, vm_address_t m, boolean_t any,
	mach_port_t o, vm_offset_t off, boolean_t copy, vm_prot_t cur,
	vm_prot_t max, int inh)
{
  (void) t; (void) a; (void) s; (void) m; (void) any; (void) o; (void) off;
  (void) copy; (void) cur; (void) max; (void) inh;
  not_here (__func__);
}

kern_return_t
mach_port_deallocate (task_t t, mach_port_t p)
{
  (void) t;
  (void) p;
  return 0;
}

void
pager_change_attributes (struct pager *p, boolean_t may_cache,
			 memory_object_copy_strategy_t copy, int wait)
{
  (void) p; (void) may_cache; (void) copy; (void) wait;
}

void
pager_sync_some (struct pager *p, vm_address_t start, vm_size_t len,
		 int wait)
{
  (void) p; (void) start; (void) len; (void) wait;
}

void
ports_port_ref (void *port)
{
  (void) port;
}

void
ports_port_deref (void *port)
{
  (void) port;
}

/* libdiskfs's diskfs_node_update with ext2fs's write_node, for what the
   harness's nodes hold: size, i_blocks and the block pointers, written
   to the inode table under a reservation, in a handle of its own (nested
   in the caller's, as every path to a node update opens one).  */
void
diskfs_node_update (struct node *np, int wait)
{
  diskfs_transaction_t *txn = diskfs_journal_start_transaction ();
  struct disknode *dn = diskfs_node_disknode (np);
  block_t blk;
  size_t off;
  uint32_t size = htole32 (np->dn_stat.st_size);
  uint32_t blocks = htole32 (np->dn_stat.st_blocks);
  uint32_t ib[EXT2_N_BLOCKS];

  (void) wait;
  for (int i = 0; i < EXT2_N_BLOCKS; i++)
    ib[i] = htole32 (dn->info.i_data[i]);
  jt_inode_location (np->cache_id, &blk, &off);
  journal_get_write_access (txn, blk);
  jt_edit (blk, off + offsetof (struct ext2_inode, i_size), &size,
	   sizeof size);
  jt_edit (blk, off + offsetof (struct ext2_inode, i_blocks), &blocks,
	   sizeof blocks);
  jt_edit (blk, off + offsetof (struct ext2_inode, i_block), ib, sizeof ib);
  journal_mark_dirty (txn, blk);
  np->dn_stat_dirty = 0;
  diskfs_journal_stop_transaction (txn);
}

struct node *
jt_node_load (ino_t ino)
{
  struct node *np = calloc (1, sizeof (struct node)
			    + sizeof (struct disknode));
  struct disknode *dn = diskfs_node_disknode (np);
  struct ext2_inode di;
  char *buf = malloc (block_size);
  block_t blk;
  size_t off;

  addr_per_block = block_size / sizeof (block_t);
  log2_stat_blocks_per_fs_block = log2_block_size - 9;
  if (!zeroblock)
    zeroblock = (vm_address_t) calloc (1, block_size);

  jt_inode_location (ino, &blk, &off);
  jt_read (blk, buf);
  memcpy (&di, buf + off, sizeof di);
  free (buf);

  np->cache_id = ino;
  np->dn_stat.st_ino = ino;
  np->dn_stat.st_mode = le16toh (di.i_mode);
  np->dn_stat.st_size = le32toh (di.i_size);
  np->dn_stat.st_blocks = le32toh (di.i_blocks);
  np->allocsize = round_block (np->dn_stat.st_size);
  pthread_mutex_init (&np->lock, NULL);
  pthread_rwlock_init (&dn->alloc_lock, NULL);
  for (int i = 0; i < EXT2_N_BLOCKS; i++)
    dn->info.i_data[i] = le32toh (di.i_block[i]);
  dn->info.i_block_group = (ino - 1) / le32toh (sblock->s_inodes_per_group);
  return np;
}
