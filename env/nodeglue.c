/* Nodes the harness builds itself, for the real getblk.c and truncate.c
   without the real inode.c: jt_node_load, and a diskfs_node_update for
   what such nodes hold.  */

#include <stdlib.h>
#include <string.h>
#include "jtest.h"

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

  jt_ext2_globals ();

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
