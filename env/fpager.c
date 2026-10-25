/* A file pager for the harness: the memory objects of ext2fs's nodes, as
   ext2fs/pager.c and libpager serve them, for code that maps a node's
   contents (dir.c, through diskfs_get_filemap and vm_map).

   A node's object is a memfd indexed by file offset, one page per block.
   Mappings of it (vm_map) fault as Mach's do: a page not resident is
   mapped without access, and a touch waits for a page-in on a pager
   thread; a resident page is read-only until it is written, when it
   becomes dirty (and, if the page-in found no block, a pager_unlock_page
   allocates one first).  A page-out resolves the page's block at that
   time, as file_pager_write_pages does, and writes it through
   journal_store_write; an eviction takes the page away from every
   mapping before it is copied.  The block of a page is resolved for each
   page-in and page-out, so it may move under a mapping, as when dir.c
   allocates the block for an EXTEND after mapping the directory.

   Departures from the Hurd: one block per page (ext2fs's are 4096 bytes
   anyway); at most FP_PAGES pages per node; the faulting thread waits in
   the SIGSEGV handler; a page-in of a page the harness evicted is
   checked against what that eviction wrote (jt_stale_reads).  */

#include <errno.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>

#include "jtest.h"

/* A broken assumption of the harness or the test.  */
#define FP_CHECK(cond, ...) \
  do { if (!(cond)) _ext2_panic (__func__, __VA_ARGS__); } while (0)

#define FP_PAGES 64
#define FP_MAX 64
#define FP_PORT_BASE 0x10000

struct fmap
{
  char *addr;
  size_t off, len;
  int maxprot;
  struct fmap *next;
};

struct fpager
{
  struct node *np;
  int fd;
  char *own;			/* The pager's own view.  */
  unsigned char resident[FP_PAGES], dirty[FP_PAGES], wlocked[FP_PAGES];
  unsigned char paging_in[FP_PAGES], paging_out[FP_PAGES];
  unsigned char pagein_wait[FP_PAGES], invalid[FP_PAGES];
  unsigned char snap_valid[FP_PAGES];
  char *snap;			/* What the last eviction wrote.  */
  struct fmap *maps;
  int queued;			/* Page-outs asked for, not yet done.  */
  int dir;			/* NP is a directory: diskfs_drop_node
				   clears its mode while pages go out.  */
};

static struct fpager *fpagers[FP_MAX];
static int nfpagers;
static pthread_mutex_t fp_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t fp_cond = PTHREAD_COND_INITIALIZER;
static unsigned long fp_stale, fp_pageins, fp_pageouts, fp_unlocks;

/* ext2glue.c's, with diskfs_catch_exception.  */
extern __thread struct disk_image_user *diskfs_exception_diu;

unsigned long
jt_fp_stale_reads (void)
{
  return __atomic_load_n (&fp_stale, __ATOMIC_RELAXED);
}

void
jt_fp_counts (unsigned long *pageins, unsigned long *pageouts,
	      unsigned long *unlocks)
{
  *pageins = fp_pageins;
  *pageouts = fp_pageouts;
  *unlocks = fp_unlocks;
}

static struct fpager *
fp_of_port (mach_port_t port)
{
  long i = (long) port - FP_PORT_BASE;
  return i >= 0 && i < nfpagers ? fpagers[i] : NULL;
}

static char *
own_page (struct fpager *fp, size_t page)
{
  return fp->own + (page << log2_block_size);
}

/* The protection of PAGE in a mapping allowing MAXPROT.  FP_LOCK held.  */
static int
page_prot (struct fpager *fp, size_t page, int maxprot)
{
  int prot;
  if (!fp->resident[page])
    prot = PROT_NONE;
  else if (fp->dirty[page] && !fp->wlocked[page])
    prot = PROT_READ | PROT_WRITE;
  else
    prot = PROT_READ;
  return prot & maxprot;
}

/* Set PAGE's protection in every mapping.  FP_LOCK held.  */
static void
apply_prot (struct fpager *fp, size_t page)
{
  size_t off = page << log2_block_size;
  for (struct fmap *m = fp->maps; m; m = m->next)
    if (off >= m->off && off < m->off + m->len)
      mprotect (m->addr + (off - m->off), block_size,
		page_prot (fp, page, m->maxprot));
}

/* ---------------------------------------------------------------- */
/* The pager's side, on pager threads.  */

struct fp_req
{
  struct fpager *fp;
  size_t page;
  int evict;
  error_t err;
  int done;
};

/* file_pager_read_page: the page's block is found under alloc_lock; a
   page past allocsize is EIO, one with no block is zeros, write-locked
   (pager_unlock_page allocates it on the first write).  */
static void
fp_pagein (void *arg)
{
  struct fp_req *r = arg;
  struct fpager *fp = r->fp;
  struct node *np = fp->np;
  size_t page = r->page;
  vm_offset_t off = page << log2_block_size;
  pthread_rwlock_t *lock = &diskfs_node_disknode (np)->alloc_lock;
  block_t blk = 0;
  void *buf = NULL;
  size_t len = 0;
  error_t err = 0;
  int wlock = 0;

  pthread_rwlock_rdlock (lock);
  if (off >= (vm_offset_t) np->allocsize)
    err = EIO;
  else
    {
      err = ext2_getblk (np, page, 0, &blk);
      if (err == EINVAL)
	{
	  blk = 0;
	  err = 0;
	}
    }
  if (!err && blk)
    {
      err = journal_store_read (blk, block_size, &buf, &len);
      if (!err && len != block_size)
	err = EIO;
    }
  pthread_rwlock_unlock (lock);
  if (!err && !blk)
    wlock = 1;

  pthread_mutex_lock (&fp_lock);
  if (!err)
    {
      if (blk)
	memcpy (own_page (fp, page), buf, block_size);
      else
	memset (own_page (fp, page), 0, block_size);
      char what[40];
      snprintf (what, sizeof what, "fp in ino %llu pg %zu",
		(unsigned long long) np->cache_id, page);
      if (blk)
	jt_trace (blk, what, own_page (fp, page));
      if (fp->snap_valid[page]
	  && memcmp (own_page (fp, page), fp->snap + off, block_size))
	{
	  __atomic_add_fetch (&fp_stale, 1, __ATOMIC_RELAXED);
	  ext2_warning ("jtest: STALE file page-in of inode %llu page %zu "
			"(block %u)", (unsigned long long) np->cache_id, page,
			blk);
	  if (blk)
	    {
	      jt_trace (blk, "  (evicted bytes were)", fp->snap + off);
	      jt_trace_dump (blk);
	    }
	}
      fp->snap_valid[page] = 0;
      fp->resident[page] = 1;
      fp->dirty[page] = 0;
      fp->wlocked[page] = wlock;
      fp->invalid[page] = 0;
    }
  fp->paging_in[page] = 0;
  r->err = err;
  r->done = 1;
  __atomic_add_fetch (&fp_pageins, 1, __ATOMIC_RELAXED);
  apply_prot (fp, page);
  pthread_cond_broadcast (&fp_cond);
  pthread_mutex_unlock (&fp_lock);
  if (buf)
    vm_deallocate (mach_task_self (), (vm_address_t) buf, len);
}

/* pager_unlock_page: a pager handle, alloc_lock for writing, and the
   page's blocks allocated.  */
static void
fp_unlock (void *arg)
{
  struct fp_req *r = arg;
  struct fpager *fp = r->fp;
  struct node *np = fp->np;
  struct disknode *dn = diskfs_node_disknode (np);
  block_t blk;

  diskfs_transaction_t *txn = diskfs_journal_start_transaction ();
  pthread_rwlock_wrlock (&dn->alloc_lock);
  error_t err = diskfs_catch_exception ();
  if (!err)
    {
      err = ext2_getblk (np, r->page, 1, &blk);
      diskfs_end_catch_exception ();
    }
  pthread_rwlock_unlock (&dn->alloc_lock);
  diskfs_journal_stop_transaction (txn);
  if (err == ENOSPC)
    ext2_warning ("This filesystem is out of space.");

  pthread_mutex_lock (&fp_lock);
  if (!err)
    fp->wlocked[r->page] = 0;
  r->err = err;
  r->done = 1;
  __atomic_add_fetch (&fp_unlocks, 1, __ATOMIC_RELAXED);
  pthread_cond_broadcast (&fp_cond);
  pthread_mutex_unlock (&fp_lock);
}

/* file_pager_write_pages for one page: its block found under alloc_lock
   when it is written; a page past allocsize is not written.  With EVICT
   the page leaves every mapping before it is copied, as Mach takes it
   away; otherwise it stays, clean.  A page-in meanwhile waits, and gets
   what was written (PM_PAGEINWAIT).  */
static void fp_pageout_1 (void *arg);

static void
fp_pageout (void *arg)
{
  struct fpager *fp = ((struct fp_req *) arg)->fp;
  fp_pageout_1 (arg);
  pthread_mutex_lock (&fp_lock);
  fp->queued--;
  pthread_cond_broadcast (&fp_cond);
  pthread_mutex_unlock (&fp_lock);
}

/* Queue a page-out of FP; FP_LOCK held.  */
static struct fp_req *
pageout_req (struct fpager *fp, size_t page, int evict)
{
  struct fp_req *r = calloc (1, sizeof *r);
  r->fp = fp;
  r->page = page;
  r->evict = evict;
  fp->queued++;
  return r;
}

static void
fp_pageout_1 (void *arg)
{
  struct fp_req *r = arg;
  struct fpager *fp = r->fp;
  struct node *np = fp->np;
  size_t page = r->page;
  vm_offset_t off = page << log2_block_size;
  int evict = r->evict;
  char *buf = malloc (block_size);

  /* With a journal a directory's blocks are journaled: ext2fs syncs no
     directory's pager while it runs, and Mach's own page-outs evict.  */
  if (ext2_journal && fp->dir)
    evict = 1;

  pthread_mutex_lock (&fp_lock);
  /* Writes of a page are serialized (libpager).  A page being paged in is
     not resident: there is nothing to write, and waiting for the page-in
     here could take the pager thread it needs.  */
  while (fp->paging_out[page])
    pthread_cond_wait (&fp_cond, &fp_lock);
  if (!fp->resident[page] || !fp->dirty[page])
    {
      /* Mach drops a clean page without writing it.  */
      if (evict && fp->resident[page])
	{
	  fp->resident[page] = 0;
	  memcpy (fp->snap + off, own_page (fp, page), block_size);
	  fp->snap_valid[page] = 1;
	  apply_prot (fp, page);
	}
      pthread_mutex_unlock (&fp_lock);
      free (buf);
      free (r);
      return;
    }
  if (evict)
    fp->resident[page] = 0;
  fp->dirty[page] = 0;
  apply_prot (fp, page);
  memcpy (buf, own_page (fp, page), block_size);
  fp->paging_out[page] = 1;
  pthread_mutex_unlock (&fp_lock);

  pthread_rwlock_t *lock = &diskfs_node_disknode (np)->alloc_lock;
  error_t err = 0;
  size_t amount = block_size;
  pthread_rwlock_rdlock (lock);
  if (off < (vm_offset_t) np->allocsize)
    {
      block_t blk = 0;
      err = ext2_getblk (np, page, 0, &blk);
      /* pager_unlock_page allocated it before the page was written.  */
      assert_backtrace (err || blk);
      if (!err)
	{
	  char what[40];
	  snprintf (what, sizeof what, "fp out ino %llu pg %zu%s",
		    (unsigned long long) np->cache_id, page,
		    evict ? " evict" : "");
	  jt_trace (blk, what, buf);
	  jt_set_keeps_page (!evict);
	  err = journal_store_write (blk, block_size, buf, &amount, 0);
	  jt_set_keeps_page (0);
	}
    }
  pthread_rwlock_unlock (lock);
  if (err || amount != block_size)
    ext2_warning ("jtest: file page-out of inode %llu page %zu failed: %s",
		  (unsigned long long) np->cache_id, page,
		  strerror (err ? err : EIO));

  pthread_mutex_lock (&fp_lock);
  fp->paging_out[page] = 0;
  if (evict)
    {
      memcpy (fp->snap + off, buf, block_size);
      fp->snap_valid[page] = !err;
      if (err && !fp->pagein_wait[page])
	fp->invalid[page] = 1;
    }
  if (fp->pagein_wait[page])
    {
      /* memory_object_data_supply of what was written.  */
      memcpy (own_page (fp, page), buf, block_size);
      fp->resident[page] = 1;
      fp->pagein_wait[page] = 0;
      fp->snap_valid[page] = 0;
      apply_prot (fp, page);
    }
  __atomic_add_fetch (&fp_pageouts, 1, __ATOMIC_RELAXED);
  pthread_cond_broadcast (&fp_cond);
  pthread_mutex_unlock (&fp_lock);
  free (buf);
  free (r);
}

/* ---------------------------------------------------------------- */
/* The faulting side.  */

/* Fault in PAGE of FP, for writing if WRITE.  Returns 0 or the error the
   page-in or the unlock gave.  Called from the SIGSEGV handler.  */
static error_t
fault (struct fpager *fp, size_t page, int write)
{
  struct fp_req r = { .fp = fp, .page = page };
  error_t err = 0;

  pthread_mutex_lock (&fp_lock);
  while (!fp->resident[page] && !err)
    {
      if (fp->invalid[page])
	err = EIO;
      else if (fp->paging_out[page])
	{
	  fp->pagein_wait[page] = 1;
	  pthread_cond_wait (&fp_cond, &fp_lock);
	}
      else if (fp->paging_in[page])
	pthread_cond_wait (&fp_cond, &fp_lock);
      else
	{
	  fp->paging_in[page] = 1;
	  r.done = 0;
	  pthread_mutex_unlock (&fp_lock);
	  jt_pager_call (fp_pagein, &r);
	  pthread_mutex_lock (&fp_lock);
	  err = r.err;
	}
    }
  if (!err && write && fp->wlocked[page])
    {
      pthread_mutex_unlock (&fp_lock);
      r.done = 0;
      jt_pager_call (fp_unlock, &r);
      pthread_mutex_lock (&fp_lock);
      err = r.err;
    }
  if (!err && write)
    fp->dirty[page] = 1;
  if (!err)
    apply_prot (fp, page);
  pthread_mutex_unlock (&fp_lock);
  return err;
}

static int
fp_fault_hook (void *addr, void *uctx)
{
  char *a = addr;
  struct fpager *fp = NULL;
  size_t off = 0;
  int maxprot = 0;

  pthread_mutex_lock (&fp_lock);
  for (int i = 0; i < nfpagers && !fp; i++)
    for (struct fmap *m = fpagers[i]->maps; m; m = m->next)
      if (a >= m->addr && a < m->addr + m->len)
	{
	  fp = fpagers[i];
	  off = m->off + (a - m->addr);
	  maxprot = m->maxprot;
	  break;
	}
  pthread_mutex_unlock (&fp_lock);
  if (!fp)
    return 0;

  /* x86: bit 1 of the page fault error code is set for a write.  */
  int write = (((ucontext_t *) uctx)->uc_mcontext.gregs[REG_ERR] & 2) != 0;
  if (write && !(maxprot & PROT_WRITE))
    return 0;			/* A real fault.  */
  error_t err = fault (fp, off >> log2_block_size, write);
  if (err)
    {
      /* As the Hurd reports a failed page-in or unlock to a thread inside
	 diskfs_catch_exception; outside of one, it is fatal.  */
      if (diskfs_exception_diu)
	longjmp (diskfs_exception_diu->env, err);
      return 0;
    }
  return 1;
}

/* ---------------------------------------------------------------- */
/* What ext2fs and libdiskfs call.  */

/* NP is being freed.  ext2fs's pager holds a light reference on its node
   until it is gone, so by now every mapping is gone, and so are its pages
   (written back by a sync or the journal, or discarded by truncate):
   there is nothing to write.  Wait for page-outs in flight, and detach
   the pager from NP.  */
static void
fp_node_freed (struct node *np)
{
  pthread_mutex_lock (&fp_lock);
  for (int i = 0; i < nfpagers; i++)
    if (fpagers[i]->np == np)
      {
	struct fpager *fp = fpagers[i];
	for (;;)
	  {
	    int busy = fp->queued;
	    for (size_t pg = 0; pg < FP_PAGES && !busy; pg++)
	      busy = fp->paging_in[pg] || fp->paging_out[pg];
	    if (!busy)
	      break;
	    pthread_cond_wait (&fp_cond, &fp_lock);
	  }
	FP_CHECK (!fp->maps, "inode %llu freed while mapped",
		  (unsigned long long) np->cache_id);
	fp->np = NULL;
	FP_CHECK (!ftruncate (fp->fd, 0)
		  && !ftruncate (fp->fd, (off_t) FP_PAGES * block_size),
		  "memfd");
	break;
      }
  pthread_mutex_unlock (&fp_lock);
}

static struct fpager *
fpager_of (struct node *np, int create)
{
  for (int i = 0; i < nfpagers; i++)
    if (fpagers[i]->np == np)
      return fpagers[i];
  if (!create)
    return NULL;
  /* The pager of a node that was freed is gone: a new node, which can
     have the old one's address, gets a pager of its own.  */
  for (int i = 0; i < nfpagers; i++)
    if (!fpagers[i]->np)
      {
	struct fpager *fp = fpagers[i];
	int fd = fp->fd;
	char *own = fp->own, *snap = fp->snap;
	memset (fp, 0, sizeof *fp);
	fp->np = np;
	fp->dir = S_ISDIR (np->dn_stat.st_mode);
	fp->fd = fd;
	fp->own = own;
	fp->snap = snap;
	return fp;
      }
  FP_CHECK (nfpagers < FP_MAX, "too many file pagers");
  struct fpager *fp = calloc (1, sizeof *fp);
  fp->np = np;
  fp->dir = S_ISDIR (np->dn_stat.st_mode);
  fp->fd = memfd_create ("jtest file pager", 0);
  FP_CHECK (fp->fd >= 0 && !ftruncate (fp->fd, (off_t) FP_PAGES * block_size),
	 "memfd");
  fp->own = mmap (NULL, (size_t) FP_PAGES * block_size,
		  PROT_READ | PROT_WRITE, MAP_SHARED, fp->fd, 0);
  FP_CHECK (fp->own != MAP_FAILED, "mmap");
  fp->snap = calloc (FP_PAGES, block_size);
  if (!jt_fault_hook)
    {
      jt_fault_hook = fp_fault_hook;
      jt_umount_hook = jt_fp_sync_all;
      jt_norefs_hook = fp_node_freed;
    }
  fpagers[nfpagers++] = fp;
  return fp;
}

mach_port_t
diskfs_get_filemap (struct node *np, vm_prot_t prot)
{
  (void) prot;
  pthread_mutex_lock (&fp_lock);
  struct fpager *fp = fpager_of (np, 1);
  int i = 0;
  while (fpagers[i] != fp)
    i++;
  pthread_mutex_unlock (&fp_lock);
  return (mach_port_t) (FP_PORT_BASE + i);
}

kern_return_t
vm_map (task_t t, vm_address_t *a, vm_size_t s, vm_address_t mask,
	boolean_t anywhere, mach_port_t obj, vm_offset_t off, boolean_t copy,
	vm_prot_t cur, vm_prot_t max, int inh)
{
  (void) t; (void) mask; (void) anywhere; (void) copy; (void) max;
  (void) inh;
  struct fpager *fp = fp_of_port (obj);
  FP_CHECK (fp, "vm_map of an object that is not a file pager's");
  FP_CHECK (off % block_size == 0 && s % block_size == 0
	 && off + s <= (size_t) FP_PAGES * block_size,
	 "vm_map of %zu bytes at %zu: past the harness's file pager",
	 (size_t) s, (size_t) off);
  char *p = mmap (NULL, s, PROT_NONE, MAP_SHARED, fp->fd, off);
  if (p == MAP_FAILED)
    return ENOMEM;
  struct fmap *m = malloc (sizeof *m);
  m->addr = p;
  m->off = off;
  m->len = s;
  m->maxprot = (cur & VM_PROT_READ ? PROT_READ : 0)
    | (cur & VM_PROT_WRITE ? PROT_WRITE : 0);
  pthread_mutex_lock (&fp_lock);
  m->next = fp->maps;
  fp->maps = m;
  for (size_t pg = off >> log2_block_size;
       pg < (off + s) >> log2_block_size; pg++)
    mprotect (p + ((pg << log2_block_size) - off), block_size,
	      page_prot (fp, pg, m->maxprot));
  pthread_mutex_unlock (&fp_lock);
  *a = (vm_address_t) p;
  return 0;
}

/* ext2fs's truncate, with a real pager, flushes the pages past the new
   size before it frees their blocks (force_delayed_copies): they are
   dropped, dirty or not, after any write of them in flight (linked with
   --wrap=diskfs_truncate).  */
error_t __real_diskfs_truncate (struct node *np, off_t length);

error_t
__wrap_diskfs_truncate (struct node *np, off_t length)
{
  pthread_mutex_lock (&fp_lock);
  struct fpager *fp = fpager_of (np, 0);
  if (fp)
    for (size_t pg = (length + block_size - 1) >> log2_block_size;
	 pg < FP_PAGES; pg++)
      {
	while (fp->paging_out[pg] || fp->paging_in[pg])
	  pthread_cond_wait (&fp_cond, &fp_lock);
	if (fp->resident[pg])
	  memset (own_page (fp, pg), 0, block_size);
	fp->resident[pg] = 0;
	fp->dirty[pg] = 0;
	fp->snap_valid[pg] = 0;
	apply_prot (fp, pg);
      }
  pthread_mutex_unlock (&fp_lock);
  return __real_diskfs_truncate (np, length);
}

/* dir.c unmaps with munmap (linked with --wrap=munmap).  */
int __real_munmap (void *addr, size_t len);

int
__wrap_munmap (void *addr, size_t len)
{
  pthread_mutex_lock (&fp_lock);
  for (int i = 0; i < nfpagers; i++)
    for (struct fmap **mp = &fpagers[i]->maps; *mp; mp = &(*mp)->next)
      if ((*mp)->addr == addr)
	{
	  struct fmap *m = *mp;
	  FP_CHECK (m->len == len, "a partial munmap of a file pager mapping");
	  *mp = m->next;
	  free (m);
	  goto out;
	}
 out:
  pthread_mutex_unlock (&fp_lock);
  return __real_munmap (addr, len);
}

/* libdiskfs's diskfs_node_rdwr, through a mapping as _diskfs_rdwr_internal
   does with pager_memcpy.  Reads stop at the end of the file; a write
   past it grows the file first.  */
error_t
diskfs_node_rdwr (struct node *np, char *data, loff_t off,
		  mach_msg_type_number_t amt, int dir, struct protid *cred,
		  mach_msg_type_number_t *amtread)
{
  error_t err = 0;
  if (dir)
    {
      if (off + amt > np->allocsize)
	err = diskfs_grow (np, off + amt, cred);
      if (err)
	return err;
      if (off + amt > np->dn_stat.st_size)
	{
	  np->dn_stat.st_size = off + amt;
	  np->dn_set_ctime = np->dn_set_mtime = 1;
	}
    }
  else if (off >= np->dn_stat.st_size)
    amt = 0;
  else if (off + amt > np->dn_stat.st_size)
    amt = np->dn_stat.st_size - off;
  if (amtread)
    *amtread = amt;
  if (amt == 0)
    return 0;

  vm_offset_t start = off & ~((loff_t) block_size - 1);
  vm_size_t len = ((off + amt + block_size - 1) & ~((loff_t) block_size - 1))
    - start;
  vm_address_t map;
  mach_port_t obj = diskfs_get_filemap (np, dir ? VM_PROT_READ | VM_PROT_WRITE
					: VM_PROT_READ);
  err = vm_map (mach_task_self (), &map, len, 0, 1, obj, start, 0,
		dir ? VM_PROT_READ | VM_PROT_WRITE : VM_PROT_READ,
		VM_PROT_READ | VM_PROT_WRITE, 0);
  if (err)
    return err;
  err = diskfs_catch_exception ();
  if (!err)
    {
      if (dir)
	memcpy ((char *) map + (off - start), data, amt);
      else
	memcpy (data, (char *) map + (off - start), amt);
      diskfs_end_catch_exception ();
    }
  __wrap_munmap ((void *) map, len);
  return err;
}

/* ext2fs's diskfs_grow (pager.c): allocsize grows to whole blocks, and
   only blocks of a page made writable already are allocated now; the
   others are allocated by pager_unlock_page, or by dir.c for an
   EXTEND.  */
error_t
diskfs_grow (struct node *np, loff_t size, struct protid *cred)
{
  (void) cred;
  struct disknode *dn = diskfs_node_disknode (np);
  if (size <= np->allocsize)
    return 0;
  pthread_rwlock_wrlock (&dn->alloc_lock);
  loff_t new_size = (size + block_size - 1) & ~((loff_t) block_size - 1);
  FP_CHECK (new_size <= (loff_t) FP_PAGES * block_size,
	 "a file past the harness's file pager");
  np->allocsize = new_size;
  pthread_rwlock_unlock (&dn->alloc_lock);
  return 0;
}

/* ---------------------------------------------------------------- */
/* For tests.  */

/* Write page PAGE of NP's pager out, evicting it with EVICT (with a
   journal a directory's always are), and wait with WAIT.  */
void
jt_fp_pageout (struct node *np, size_t page, int evict, int wait)
{
  pthread_mutex_lock (&fp_lock);
  struct fpager *fp = fpager_of (np, 0);
  struct fp_req *r = fp && page < FP_PAGES ? pageout_req (fp, page, evict)
    : NULL;
  pthread_mutex_unlock (&fp_lock);
  if (!r)
    return;
  if (wait)
    jt_pager_call (fp_pageout, r);
  else
    jt_pager_call_async (fp_pageout, r);
}

/* pager_shutdown of every file pager: its dirty pages written, kept.  */
void
jt_fp_sync_all (void)
{
  for (int i = 0; i < nfpagers; i++)
    for (size_t pg = 0; pg < FP_PAGES; pg++)
      {
	pthread_mutex_lock (&fp_lock);
	struct fp_req *r = NULL;
	if (fpagers[i]->np
	    && (fpagers[i]->dirty[pg] || fpagers[i]->paging_out[pg]))
	  r = pageout_req (fpagers[i], pg, 0);
	pthread_mutex_unlock (&fp_lock);
	if (r)
	  jt_pager_call (fp_pageout, r);
      }
}

/* dir.c allocates with mmap (0, ..., MAP_ANON, 0, 0), which the Hurd
   takes as a private mapping, and Linux refuses without MAP_PRIVATE
   (linked with --wrap=mmap).  */
void *__real_mmap (void *addr, size_t len, int prot, int flags, int fd,
		   off_t off);

void *
__wrap_mmap (void *addr, size_t len, int prot, int flags, int fd, off_t off)
{
  if ((flags & MAP_ANONYMOUS) && !(flags & (MAP_PRIVATE | MAP_SHARED)))
    {
      flags |= MAP_PRIVATE;
      fd = -1;
    }
  return __real_mmap (addr, len, prot, flags, fd, off);
}
