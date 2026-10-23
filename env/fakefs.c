/* The fake ext2fs around journal.c.  See jtest.h.  */

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "jtest.h"
#include "jbd2_format.h"

/* ---------------------------------------------------------------- */
/* ext2fs and libdiskfs globals that journal.c uses.  */

struct journal *ext2_journal;
unsigned int block_size;
unsigned int log2_block_size;
unsigned log2_dev_blocks_per_fs_block;
unsigned long groups_count;
unsigned int sblock_block = 1;
struct ext2_super_block *sblock;
struct ext2_group_desc *group_desc_image;
struct pokel global_pokel;
int diskfs_readonly;
vm_size_t vm_page_size = 4096;

void *disk_cache;
store_offset_t disk_cache_size;
int disk_cache_blocks;
struct disk_cache_info *disk_cache_info;
hurd_ihash_t disk_cache_bptr;
pthread_mutex_t disk_cache_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t disk_cache_reassociation = PTHREAD_COND_INITIALIZER;

static struct store fake_store;
struct store *store = &fake_store;

void (*jt_on_release) (block_t start, unsigned long count);

static int jt_flags;
static unsigned long jt_warnings;

/* ---------------------------------------------------------------- */
/* Reporting.  */

void
ext2_warning (const char *fmt, ...)
{
  va_list ap;

  __atomic_add_fetch (&jt_warnings, 1, __ATOMIC_RELAXED);
  if (jt_flags & JT_QUIET)
    return;
  va_start (ap, fmt);
  fputs ("ext2fs: warning: ", stderr);
  vfprintf (stderr, fmt, ap);
  fputc ('\n', stderr);
  va_end (ap);
}

void
_ext2_panic (const char *func, const char *fmt, ...)
{
  va_list ap;

  va_start (ap, fmt);
  fprintf (stderr, "ext2fs: panic: %s: ", func);
  vfprintf (stderr, fmt, ap);
  fputc ('\n', stderr);
  va_end (ap);
  abort ();
}

void
__assert_fail_backtrace (const char *assertion, const char *file,
			 unsigned int line, const char *function)
{
  fprintf (stderr, "%s:%u: %s: Assertion '%s' failed.\n",
	   file, line, function, assertion);
  abort ();
}

void
__assert_perror_fail_backtrace (int errnum, const char *file,
				unsigned int line, const char *function)
{
  fprintf (stderr, "%s:%u: %s: Unexpected error: %s.\n",
	   file, line, function, strerror (errnum));
  abort ();
}

/* ---------------------------------------------------------------- */
/* Mach.  */

kern_return_t
vm_allocate (task_t task, vm_address_t *addr, vm_size_t size, boolean_t anywhere)
{
  void *p = mmap (NULL, size, PROT_READ | PROT_WRITE,
		  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  (void) task;
  (void) anywhere;
  if (p == MAP_FAILED)
    return ENOMEM;
  *addr = (vm_address_t) p;
  return 0;
}

kern_return_t
vm_deallocate (task_t task, vm_address_t addr, vm_size_t size)
{
  (void) task;
  if (size)
    munmap ((void *) addr, size);
  return 0;
}

/* ---------------------------------------------------------------- */
/* The store: the image in memory, and the log of writes to it.  */

static block_t *jnode_map;
static size_t jnode_map_len;
static unsigned char *in_journal;	/* By block.  */

static char *store_mem;		/* The "disk".  */
static char *store_base;	/* The image as loaded.  */
static size_t store_bytes;

static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
static struct jt_write *log_entries;
static size_t log_n, log_alloc;

static void
log_append (uint64_t off, const void *data, size_t len)
{
  void *copy = NULL;

  if (data)
    {
      copy = malloc (len);
      memcpy (copy, data, len);
    }
  if (log_n == log_alloc)
    {
      log_alloc = log_alloc ? 2 * log_alloc : 1024;
      log_entries = realloc (log_entries, log_alloc * sizeof *log_entries);
    }
  log_entries[log_n++] = (struct jt_write) { off, len, copy };
}

error_t
store_write (struct store *s, store_offset_t addr, const void *buf,
	     size_t len, size_t *amount)
{
  uint64_t off = (uint64_t) addr << s->log2_block_size;

  if (off + len > store_bytes)
    return EIO;
  pthread_mutex_lock (&log_lock);
  memcpy (store_mem + off, buf, len);
  log_append (off, buf, len);
  pthread_mutex_unlock (&log_lock);
  *amount = len;
  return 0;
}

/* Like a device store: the data always comes back in a new buffer, which
   the caller frees with vm_deallocate or munmap.  */
error_t
store_read (struct store *s, store_offset_t addr, size_t amount,
	    void **buf, size_t *len)
{
  uint64_t off = (uint64_t) addr << s->log2_block_size;
  vm_address_t p;

  if (off >= store_bytes)
    return EIO;
  if (off + amount > store_bytes)
    amount = store_bytes - off;
  if (vm_allocate (mach_task_self (), &p, amount, 1))
    return ENOMEM;
  pthread_mutex_lock (&log_lock);
  memcpy ((void *) p, store_mem + off, amount);
  pthread_mutex_unlock (&log_lock);
  *buf = (void *) p;
  *len = amount;
  return 0;
}

error_t
store_sync (struct store *s)
{
  (void) s;
  pthread_mutex_lock (&log_lock);
  log_append (0, NULL, 0);
  pthread_mutex_unlock (&log_lock);
  return 0;
}

size_t
jt_log_count (void)
{
  return log_n;
}

const struct jt_write *
jt_log_entry (size_t i)
{
  return &log_entries[i];
}

const void *
jt_disk_block (block_t b)
{
  return store_mem + ((size_t) b << log2_block_size);
}

void
jt_dump_log (FILE *f, size_t from, size_t to)
{
  if (to > log_n)
    to = log_n;
  for (size_t i = from; i < to; i++)
    {
      const struct jt_write *w = &log_entries[i];
      block_t b = w->off >> log2_block_size;
      const uint32_t *d = w->data;

      if (!d)
	{
	  fprintf (f, "%6zu  sync\n", i);
	  continue;
	}
      fprintf (f, "%6zu  block %6u", i, b);
      if (w->len != block_size || w->off % block_size)
	fprintf (f, " +%" PRIu64 " len %zu", w->off % block_size, w->len);
      if (in_journal && in_journal[b])
	{
	  size_t j = 0;
	  while (jnode_map[j] != b)
	    j++;
	  fprintf (f, "  log %4zu", j);
	  if (be32toh (d[0]) == JBD2_MAGIC_NUMBER)
	    {
	      static const char *const types[] =
		{ "?", "descriptor", "commit", "sb v1", "sb v2", "revoke" };
	      uint32_t t = be32toh (d[1]);
	      fprintf (f, "  %s", t < 6 ? types[t] : "?");
	      if (t == JBD2_SUPERBLOCK_V1 || t == JBD2_SUPERBLOCK_V2)
		{
		  const journal_superblock_t *sb = w->data;
		  fprintf (f, " s_sequence %u s_start %u",
			   be32toh (sb->s_sequence), be32toh (sb->s_start));
		}
	      else
		fprintf (f, " tid %u", be32toh (d[2]));
	      if (t == JBD2_DESCRIPTOR_BLOCK)
		{
		  const journal_block_tag_t *tag
		    = (const void *) ((const char *) d + sizeof (journal_header_t));
		  fprintf (f, " ->");
		  for (; (const char *) (tag + 1) <= (const char *) d + block_size;
	       tag++)
		    {
		      uint32_t flags = be32toh (tag->t_flags);
		      fprintf (f, " %u", be32toh (tag->t_blocknr));
		      if (!(flags & JBD2_FLAG_SAME_UUID))
			tag = (const void *) ((const char *) tag + 16);
		      if (flags & JBD2_FLAG_LAST_TAG)
			break;
		    }
		}
	    }
	  else
	    fprintf (f, "  data %08x %08x", d[0], d[1]);
	}
      else
	fprintf (f, "  home %08x %08x", d[0], d[1]);
      fputc ('\n', f);
    }
}

static void
write_file (const char *path, const char *data, size_t len)
{
  int fd = open (path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0 || write (fd, data, len) != (ssize_t) len || close (fd))
    {
      perror (path);
      exit (2);
    }
}

void
jt_build_image (size_t n, int torn, const char *path)
{
  char *img = malloc (store_bytes);

  memcpy (img, store_base, store_bytes);
  for (size_t i = 0; i < n && i < log_n; i++)
    {
      const struct jt_write *w = &log_entries[i];
      size_t len = w->len;
      if (!w->data)
	continue;
      if (torn && i == n - 1)
	len /= 2;
      memcpy (img + w->off, w->data, len);
    }
  write_file (path, img, store_bytes);
  free (img);
}

void
jt_save_image (const char *path)
{
  pthread_mutex_lock (&log_lock);
  write_file (path, store_mem, store_bytes);
  pthread_mutex_unlock (&log_lock);
}

/* ---------------------------------------------------------------- */
/* The disk cache and the pager.

   Every block of the image has a slot.  PAGE_LOCK covers the residency
   and dirty bits and the bytes of non-resident blocks; edits of resident
   blocks by tests also take it, so that a page-out copies a whole edit.
   The journal's sweep reads the slots without it, as it reads the real
   disk cache without faulting.  */

static pthread_mutex_t page_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t page_in_done = PTHREAD_COND_INITIALIZER;
static unsigned char *resident, *dirty, *paging_in;
/* As libpager's PM_PAGINGOUT and PM_PAGEINWAIT: a block being written
   out, and one whose page-in waits for that write to supply it.  */
static unsigned char *paging_out, *pagein_wait;
static pthread_cond_t page_out_done = PTHREAD_COND_INITIALIZER;
static unsigned long stale_reads;

enum req_kind { REQ_CLEAN, REQ_EVICT, REQ_PAGEIN, REQ_EXIT };

struct req
{
  enum req_kind kind;
  block_t block;
  int wait;
  int done;
  struct req *next;
};

static pthread_mutex_t req_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t req_ready = PTHREAD_COND_INITIALIZER;
static pthread_cond_t req_done = PTHREAD_COND_INITIALIZER;
static struct req *req_head, **req_tail = &req_head;
static pthread_t *pagers;
static int npagers_running;
static unsigned long in_flight;	/* Requests queued or being served.  */

static __thread int keeps_page;

int
pager_write_keeps_page (void)
{
  return keeps_page;
}

static char *
slot (block_t b)
{
  return (char *) disk_cache + ((size_t) b << log2_block_size);
}

/* Queue a request; with WAIT, wait for it and free it.  */
static void
submit (enum req_kind kind, block_t b, int wait)
{
  struct req *r = calloc (1, sizeof *r);

  r->kind = kind;
  r->block = b;
  r->wait = wait;
  pthread_mutex_lock (&req_lock);
  if (kind != REQ_EXIT)
    in_flight++;
  *req_tail = r;
  req_tail = &r->next;
  if (kind == REQ_EXIT)
    pthread_cond_broadcast (&req_ready);
  else
    pthread_cond_signal (&req_ready);
  if (wait)
    {
      while (!r->done)
	pthread_cond_wait (&req_done, &req_lock);
      free (r);
    }
  pthread_mutex_unlock (&req_lock);
}

/* Like libpager's data-return: writes of a block are serialized, and a
   page-in that comes while the block is being written out waits, and
   gets the data being written.  */
static void
serve_pageout (block_t b, int evict)
{
  char *buf = malloc (block_size);
  size_t amount = 0;
  error_t err;

  pthread_mutex_lock (&page_lock);
  while (paging_out[b])
    pthread_cond_wait (&page_out_done, &page_lock);
  if (!resident[b] || !dirty[b])
    {
      /* Mach drops a clean page without writing it.  */
      if (evict && resident[b])
	resident[b] = 0;
      pthread_mutex_unlock (&page_lock);
      free (buf);
      return;
    }
  memcpy (buf, slot (b), block_size);
  dirty[b] = 0;
  if (evict)
    resident[b] = 0;
  paging_out[b] = 1;
  pthread_mutex_unlock (&page_lock);

  keeps_page = !evict;
  err = journal_store_write (b, block_size, buf, &amount, 1);
  if (err || amount != block_size)
    ext2_warning ("jtest: page-out of block %u failed: %s", b,
		  strerror (err ? err : EIO));

  pthread_mutex_lock (&page_lock);
  paging_out[b] = 0;
  if (pagein_wait[b])
    {
      /* memory_object_data_supply of what was written.  */
      memcpy (slot (b), buf, block_size);
      resident[b] = 1;
      pagein_wait[b] = 0;
      pthread_cond_broadcast (&page_in_done);
    }
  pthread_cond_broadcast (&page_out_done);
  pthread_mutex_unlock (&page_lock);
  free (buf);
}

static void
serve_pagein (block_t b)
{
  void *buf = NULL;
  size_t len = 0;
  error_t err;

  pthread_mutex_lock (&page_lock);
  if (resident[b] || paging_in[b] || pagein_wait[b])
    {
      pthread_mutex_unlock (&page_lock);
      return;
    }
  if (paging_out[b])
    {
      /* The write will supply it.  */
      pagein_wait[b] = 1;
      pthread_mutex_unlock (&page_lock);
      return;
    }
  paging_in[b] = 1;
  pthread_mutex_unlock (&page_lock);

  err = journal_store_read (b, block_size, &buf, &len);
  if (err || len != block_size)
    _ext2_panic (__func__, "page-in of block %u failed: %s", b,
		 strerror (err ? err : EIO));

  pthread_mutex_lock (&page_lock);
  /* The slot still holds what was evicted, and nothing edits an evicted
     block: the journal must return exactly that.  */
  if (memcmp (buf, slot (b), block_size))
    {
      stale_reads++;
      ext2_warning ("jtest: STALE page-in of block %u", b);
    }
  memcpy (slot (b), buf, block_size);
  resident[b] = 1;
  paging_in[b] = 0;
  pthread_cond_broadcast (&page_in_done);
  pthread_mutex_unlock (&page_lock);
  munmap (buf, len);
}

static void *
pager_thread (void *arg)
{
  (void) arg;
  journal_thread_set_pager (1);
  for (;;)
    {
      struct req *r;

      pthread_mutex_lock (&req_lock);
      while (!req_head)
	pthread_cond_wait (&req_ready, &req_lock);
      r = req_head;
      if (r->kind == REQ_EXIT)
	{
	  /* Leave it for the other pager threads.  */
	  pthread_mutex_unlock (&req_lock);
	  return NULL;
	}
      req_head = r->next;
      if (!req_head)
	req_tail = &req_head;
      pthread_mutex_unlock (&req_lock);

      switch (r->kind)
	{
	case REQ_CLEAN:
	  serve_pageout (r->block, 0);
	  break;
	case REQ_EVICT:
	  serve_pageout (r->block, 1);
	  break;
	case REQ_PAGEIN:
	  serve_pagein (r->block);
	  break;
	case REQ_EXIT:
	  break;
	}

      pthread_mutex_lock (&req_lock);
      in_flight--;
      if (r->wait)
	r->done = 1;
      else
	free (r);
      pthread_cond_broadcast (&req_done);
      pthread_mutex_unlock (&req_lock);
    }
}

/* Make B resident; PAGE_LOCK is held, and is dropped while waiting.  */
static void
fault_locked (block_t b)
{
  while (!resident[b])
    {
      if (!paging_in[b] && !pagein_wait[b])
	{
	  pthread_mutex_unlock (&page_lock);
	  submit (REQ_PAGEIN, b, 1);
	  pthread_mutex_lock (&page_lock);
	}
      else
	pthread_cond_wait (&page_in_done, &page_lock);
    }
}

void
jt_edit (block_t b, size_t off, const void *data, size_t len)
{
  assert_backtrace (off + len <= block_size);
  pthread_mutex_lock (&page_lock);
  fault_locked (b);
  memcpy (slot (b) + off, data, len);
  dirty[b] = 1;
  pthread_mutex_unlock (&page_lock);
}

void
jt_read (block_t b, void *out)
{
  pthread_mutex_lock (&page_lock);
  fault_locked (b);
  memcpy (out, slot (b), block_size);
  pthread_mutex_unlock (&page_lock);
}

void
jt_pageout (block_t b, int evict, int wait)
{
  submit (evict ? REQ_EVICT : REQ_CLEAN, b, wait);
}

void
jt_sync_all (void)
{
  block_t n = jt_nblocks ();

  for (block_t b = 0; b < n; b++)
    if (dirty[b])
      submit (REQ_CLEAN, b, 0);
  jt_drain ();
}

void
jt_drain (void)
{
  pthread_mutex_lock (&req_lock);
  while (in_flight)
    pthread_cond_wait (&req_done, &req_lock);
  pthread_mutex_unlock (&req_lock);
}

unsigned long
jt_stale_reads (void)
{
  return stale_reads;
}

/* ---------------------------------------------------------------- */
/* ext2fs functions that journal.c calls.  */

void *
disk_cache_block_ref_mapped (block_t block)
{
  return slot (block);
}

void
_disk_cache_block_deref (void *ptr)
{
  (void) ptr;
}

/* TESTING ONLY in ext2fs: evict BLOCK without waiting.  */
void
ext2_test_evict_block (block_t block)
{
  pthread_mutex_lock (&req_lock);
  int running = npagers_running;
  pthread_mutex_unlock (&req_lock);
  if (running)
    submit (REQ_EVICT, block, 0);
}

void
pokel_sync (struct pokel *pokel, int wait)
{
  (void) pokel;
  (void) wait;
  jt_sync_all ();
}

void
ext2_release_busy_blocks (block_t block, unsigned long count)
{
  if (jt_on_release)
    jt_on_release (block, count);
}

static struct node *jnode;
static int jnode_refs;

error_t
ext2_getblk (struct node *node, block_t block, int create, block_t *disk_block)
{
  if (node != jnode || create || block >= jnode_map_len)
    return EINVAL;
  *disk_block = jnode_map[block];
  return 0;
}

void
diskfs_nref (struct node *np)
{
  (void) np;
  __atomic_add_fetch (&jnode_refs, 1, __ATOMIC_RELAXED);
}

void
diskfs_nrele (struct node *np)
{
  (void) np;
  __atomic_sub_fetch (&jnode_refs, 1, __ATOMIC_RELAXED);
}

/* Write the superblock in the disk cache home, as hyper.c does for the
   needs_recovery flag.  */
static void
write_sblock (void)
{
  size_t amount;
  char *blk = slot (boffs_block (SBLOCK_OFFS));

  pthread_mutex_lock (&page_lock);
  store_write (store, SBLOCK_OFFS >> store->log2_block_size,
	       blk + SBLOCK_OFFS % block_size, SBLOCK_SIZE, &amount);
  pthread_mutex_unlock (&page_lock);
  store_sync (store);
}

void
ext2_clear_needs_recovery (void)
{
  sblock->s_feature_incompat &= ~htole32 (EXT3_FEATURE_INCOMPAT_RECOVER);
  write_sblock ();
}

/* ---------------------------------------------------------------- */
/* Mount and unmount.  */

block_t
jt_nblocks (void)
{
  return store_bytes >> log2_block_size;
}

int
jt_block_free (block_t b)
{
  unsigned long per = le32toh (sblock->s_blocks_per_group);
  block_t first = le32toh (sblock->s_first_data_block);
  unsigned long g, bit;
  const unsigned char *bitmap;

  if (b < first || b >= le32toh (sblock->s_blocks_count))
    return 0;
  g = (b - first) / per;
  bit = (b - first) % per;
  bitmap = (const unsigned char *) store_base
    + ((size_t) le32toh (group_desc (g)->bg_block_bitmap) << log2_block_size);
  return !(bitmap[bit / 8] & (1 << (bit % 8)));
}

/* Add the data blocks of the indirect block IND, of depth DEPTH, to the
   journal map.  */
static void
map_indirect (block_t ind, int depth)
{
  const uint32_t *p = (const uint32_t *) (store_mem
					  + ((size_t) ind << log2_block_size));
  for (size_t i = 0; i < block_size / 4 && p[i]; i++)
    if (depth == 0)
      jnode_map[jnode_map_len++] = le32toh (p[i]);
    else
      map_indirect (le32toh (p[i]), depth - 1);
}

static void
load_journal_inode (void)
{
  ino_t ino = le32toh (sblock->s_journal_inum);
  unsigned long ipg = le32toh (sblock->s_inodes_per_group);
  size_t isize = le32toh (sblock->s_rev_level) ? le16toh (sblock->s_inode_size) : 128;
  struct ext2_group_desc *gd = group_desc ((ino - 1) / ipg);
  struct ext2_inode *di = (struct ext2_inode *)
    (store_mem + ((size_t) le32toh (gd->bg_inode_table) << log2_block_size)
     + ((ino - 1) % ipg) * isize);

  if (le32toh (di->i_flags) & 0x80000)	/* EXT4_EXTENTS_FL */
    _ext2_panic (__func__, "the journal inode uses extents; "
		 "make the image with mke2fs -t ext3");

  jnode = calloc (1, sizeof *jnode);
  jnode->allocsize = le32toh (di->i_size);
  jnode_map = malloc ((jnode->allocsize / block_size + 1) * sizeof *jnode_map);
  for (int i = 0; i < EXT2_NDIR_BLOCKS && di->i_block[i]; i++)
    jnode_map[jnode_map_len++] = le32toh (di->i_block[i]);
  for (int d = 0; d < 3; d++)
    if (di->i_block[EXT2_IND_BLOCK + d])
      map_indirect (le32toh (di->i_block[EXT2_IND_BLOCK + d]), d);

  in_journal = calloc (store_bytes >> log2_block_size, 1);
  for (size_t i = 0; i < jnode_map_len; i++)
    in_journal[jnode_map[i]] = 1;
}

int
jt_in_journal (uint64_t off)
{
  return in_journal[off >> log2_block_size];
}

static void
load_image (const char *path)
{
  int fd = open (path, O_RDONLY);
  struct stat st;

  if (fd < 0 || fstat (fd, &st))
    {
      perror (path);
      exit (2);
    }
  store_bytes = st.st_size;
  store_mem = malloc (store_bytes);
  store_base = malloc (store_bytes);
  if (read (fd, store_base, store_bytes) != (ssize_t) store_bytes)
    {
      perror (path);
      exit (2);
    }
  close (fd);
  memcpy (store_mem, store_base, store_bytes);

  fake_store.block_size = 512;
  fake_store.log2_block_size = 9;
  fake_store.size = store_bytes;
}

void
jt_mount (const char *image, int npagers, int flags)
{
  struct ext2_super_block *sb;
  block_t nblocks;

  jt_flags = flags;
  load_image (image);

  sb = (struct ext2_super_block *) (store_mem + SBLOCK_OFFS);
  if (le16toh (sb->s_magic) != EXT2_SUPER_MAGIC
      || !(le32toh (sb->s_feature_compat) & EXT3_FEATURE_COMPAT_HAS_JOURNAL))
    _ext2_panic (__func__, "%s is not an ext3 image", image);
  log2_block_size = 10 + le32toh (sb->s_log_block_size);
  block_size = 1 << log2_block_size;
  log2_dev_blocks_per_fs_block = log2_block_size - fake_store.log2_block_size;
  groups_count = (le32toh (sb->s_blocks_count) - le32toh (sb->s_first_data_block)
		  + le32toh (sb->s_blocks_per_group) - 1)
    / le32toh (sb->s_blocks_per_group);

  /* The disk cache maps every block, all of them resident.  */
  nblocks = store_bytes >> log2_block_size;
  disk_cache_size = (store_offset_t) nblocks << log2_block_size;
  disk_cache_blocks = nblocks;
  disk_cache = aligned_alloc (vm_page_size, disk_cache_size);
  memcpy (disk_cache, store_mem, disk_cache_size);
  disk_cache_info = calloc (nblocks, sizeof *disk_cache_info);
  hurd_ihash_create (&disk_cache_bptr, HURD_IHASH_NO_LOCP);
  for (block_t b = 0; b < nblocks; b++)
    {
      disk_cache_info[b].block = b;
      disk_cache_info[b].flags = DC_INCORE;
      hurd_ihash_add (disk_cache_bptr, b, slot (b));
    }
  resident = malloc (nblocks);
  memset (resident, 1, nblocks);
  dirty = calloc (nblocks, 1);
  paging_in = calloc (nblocks, 1);
  paging_out = calloc (nblocks, 1);
  pagein_wait = calloc (nblocks, 1);

  sblock = (struct ext2_super_block *) (slot (boffs_block (SBLOCK_OFFS))
					+ SBLOCK_OFFS % block_size);
  group_desc_image = (struct ext2_group_desc *) slot (group_desc_block);

  load_journal_inode ();
  if (journal_check (jnode) != JOURNAL_USABLE)
    _ext2_panic (__func__, "the journal of %s is not usable "
		 "(needs replay, or unsupported features)", image);

  pagers = calloc (npagers, sizeof *pagers);
  for (int i = 0; i < npagers; i++)
    pthread_create (&pagers[i], NULL, pager_thread, NULL);
  pthread_mutex_lock (&req_lock);
  npagers_running = npagers;
  pthread_mutex_unlock (&req_lock);

  ext2_journal = journal_create (jnode);
  sblock->s_feature_incompat |= htole32 (EXT3_FEATURE_INCOMPAT_RECOVER);
  write_sblock ();
}

static void
stop_pagers (void)
{
  int n = npagers_running;

  submit (REQ_EXIT, 0, 0);
  for (int i = 0; i < n; i++)
    pthread_join (pagers[i], NULL);
  pthread_mutex_lock (&req_lock);
  npagers_running = 0;
  /* The exit request, and whatever was queued after it.  */
  while (req_head)
    {
      struct req *r = req_head;
      req_head = r->next;
      free (r);
    }
  req_tail = &req_head;
  pthread_mutex_unlock (&req_lock);
  free (pagers);
  pagers = NULL;
}

unsigned long
jt_umount (void)
{
  diskfs_journal_shutdown ();
  stop_pagers ();
  return jt_warnings;
}

void
jt_abandon (void)
{
  stop_pagers ();
}

/* ---------------------------------------------------------------- */

int
jt_run (const char *fmt, ...)
{
  char *cmd;
  va_list ap;
  int status;

  va_start (ap, fmt);
  if (vasprintf (&cmd, fmt, ap) < 0)
    exit (2);
  va_end (ap);
  status = system (cmd);
  free (cmd);
  return WIFEXITED (status) ? WEXITSTATUS (status) : -1;
}
