/* What the real inode.c, ialloc.c, orphan.c and hyper.c, and libdiskfs's
   node cache, references and drop, need from the rest of ext2fs and the
   Hurd.  Linked, with env/ext2glue.c, into the tests that use them (see
   the Makefile).  Nodes have no file pager, no translator and no
   extended attributes.  */

#include <stdlib.h>
#include <string.h>
#include <maptime.h>
#include "jtest.h"

unsigned long inodes_per_block;
uint16_t global_inode_size;
unsigned long frag_size;
unsigned long frags_per_block;
unsigned long db_per_group;
unsigned long next_generation;
int use_xattr_translator_records;
int _diskfs_noatime;
int _diskfs_relatime;
static struct mapped_time_value mtime;
volatile struct mapped_time_value *diskfs_mtime = &mtime;

void
jt_inode_globals (void)
{
  jt_ext2_globals ();
  /* hyper.c's pointers into the disk cache.  With 4K blocks it keeps
     no map of modified blocks (allocate_mod_map).  */
  map_hypermetadata ();
  free (modified_global_blocks);
  modified_global_blocks = NULL;
  global_inode_size = le32toh (sblock->s_rev_level)
    ? le16toh (sblock->s_inode_size) : EXT2_GOOD_OLD_INODE_SIZE;
  inodes_per_block = block_size / global_inode_size;
  frag_size = block_size;
  frags_per_block = 1;
  db_per_group = 1;
  /* As Mach keeps it: maptime_read retries until the check matches.  */
  mtime.seconds = mtime.check_seconds = time (NULL);
  mtime.time_value.seconds = mtime.check_seconds64 = mtime.seconds;
}

/* No extended attributes: no xattr block, no translator record.  */
error_t
ext2_get_xattr (struct node *np, const char *name, char *value, size_t *len)
{
  (void) np; (void) name; (void) value; (void) len;
  return ENODATA;
}

error_t
ext2_set_xattr (struct node *np, const char *name, const char *value,
		size_t len, int flags)
{
  (void) np; (void) name; (void) value; (void) len; (void) flags;
  return EOPNOTSUPP;
}

error_t
ext2_free_xattr_block (struct node *np)
{
  (void) np;
  return 0;
}

/* A node's pokel of indirect blocks: with a journal they are journaled,
   and pokel_add (env/fakefs.c) marks the pages dirty.  */
void
pokel_init (struct pokel *pokel, struct pager *pager, void *image)
{
  (void) pager;
  (void) image;
  memset (pokel, 0, sizeof *pokel);
  pthread_spin_init (&pokel->lock, PTHREAD_PROCESS_PRIVATE);
}

void
pokel_finalize (struct pokel *pokel)
{
  (void) pokel;
}

void
pokel_flush (struct pokel *pokel)
{
  (void) pokel;
}

void
pokel_inherit (struct pokel *pokel, struct pokel *from)
{
  (void) pokel;
  (void) from;
}

/* No file pagers.  */
void
drop_pager_softrefs (struct node *node)
{
  (void) node;
}

void
allow_pager_softrefs (struct node *node)
{
  (void) node;
}

void
flush_node_pager (struct node *node)
{
  (void) node;
}

/* No translators, no conch.  */
void
fshelp_transbox_init (struct transbox *transbox, pthread_mutex_t *lock,
		      void *cookie)
{
  memset (transbox, 0, sizeof *transbox);
  transbox->lock = lock;
  transbox->cookie = cookie;
}

void
fshelp_drop_transbox (struct transbox *transbox)
{
  (void) transbox;
}

void
iohelp_initialize_conch (struct conch *c, pthread_mutex_t *lock)
{
  memset (c, 0, sizeof *c);
  c->lock = lock;
}

/* No directories: diskfs_create_node reaches these only for a name or
   a directory.  */
static void __attribute__ ((noreturn))
no_dirs (const char *what)
{
  _ext2_panic (what, "no directories in the harness");
}

error_t
diskfs_init_dir (struct node *np, struct node *parent, struct protid *cred)
{
  (void) np; (void) parent; (void) cred;
  no_dirs (__func__);
}

error_t
diskfs_clear_directory (struct node *np, struct node *parent,
			struct protid *cred)
{
  (void) np; (void) parent; (void) cred;
  no_dirs (__func__);
}

error_t
diskfs_direnter (struct node *dp, const char *name, struct node *np,
		 struct dirstat *ds, struct protid *cred)
{
  (void) dp; (void) name; (void) np; (void) ds; (void) cred;
  no_dirs (__func__);
}

error_t
diskfs_drop_dirstat (struct node *dp, struct dirstat *ds)
{
  (void) dp; (void) ds;
  no_dirs (__func__);
}

/* hyper.c's diskfs_readonly_changed, never called here.  */
error_t
store_set_flags (struct store *s, int flags)
{
  (void) s; (void) flags;
  return 0;
}

error_t
store_clear_flags (struct store *s, int flags)
{
  (void) s; (void) flags;
  return 0;
}

void
ext2_start_journal (void)
{
  _ext2_panic (__func__, "not available in the harness");
}
