/* The rest of ext2fs and libdiskfs that the real getblk.c and truncate.c
   need.  Linked only into the tests that use them (see the Makefile),
   with env/nodeglue.c (nodes the harness builds) or env/inodeglue.c (the
   real inode.c).  Nodes have no file pager, so what only a pager reaches
   is not here, or aborts.  */

#include <stdlib.h>
#include <string.h>
#include "jtest.h"

unsigned long addr_per_block;
unsigned log2_stat_blocks_per_fs_block;
/* Weak: hyper.c's, where it is linked in.  */
__attribute__ ((weak)) vm_address_t zeroblock;
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

/* The globals ext2fs sets at mount, for what the harness mounted.  */
void
jt_ext2_globals (void)
{
  addr_per_block = block_size / sizeof (block_t);
  log2_stat_blocks_per_fs_block = log2_block_size - 9;
  if (!zeroblock)
    zeroblock = (vm_address_t) calloc (1, block_size);
}
