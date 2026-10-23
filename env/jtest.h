/* Host test harness for the ext2fs journal.

   journal.c runs unchanged against a fake ext2fs around it:

   - The store is the image held in memory.  Every store_write and
     store_sync is also appended to a write log, so that the image as it
     was after any prefix of the writes (a crash at that point) can be
     rebuilt afterwards.

   - The disk cache holds every block of the image.  A block is either
     resident or evicted.  Tests edit blocks only through jt_edit, which
     pages an evicted block back in first, as a fault would.

   - Pager threads serve page-outs (clean or evict) and page-ins through
     journal_store_write and journal_store_read, as the disk pager does.
     An evicted block keeps its bytes, and a page-in checks that the
     journal hands back exactly those bytes: a mismatch is a stale read.  */

#ifndef JTEST_H
#define JTEST_H

#include <stddef.h>
#include <stdint.h>
#include "ext2fs.h"
#include "journal.h"

/* jt_mount flags.  */
#define JT_QUIET	0x1	/* Do not print journal warnings.  */

/* Load IMAGE (an ext3 image made by mke2fs -t ext3 -o hurd) and start the
   journal on it, with NPAGERS pager threads.  Exits on failure.  */
void jt_mount (const char *image, int npagers, int flags);

/* Stop the journal as a clean unmount does, and the pager threads.
   Returns the number of journal warnings seen since jt_mount.  */
unsigned long jt_umount (void);

/* Stop the pager threads without stopping the journal: the state a crash
   leaves.  */
void jt_abandon (void);

/* Number of blocks in the image.  */
block_t jt_nblocks (void);

/* Whether block B is free in the image's block bitmap as loaded.  */
int jt_block_free (block_t b);

/* Copy LEN bytes from DATA to offset OFF of block B in the disk cache,
   paging B in first if it is evicted, and mark B dirty.  */
void jt_edit (block_t b, size_t off, const void *data, size_t len);

/* Copy block B as the disk cache has it (paging it in first) to OUT.  */
void jt_read (block_t b, void *out);

/* Ask the pager threads to write B out, keeping it resident (EVICT 0) or
   evicting it (EVICT 1).  A clean block is not written.  With WAIT, wait
   until it is done.  */
void jt_pageout (block_t b, int evict, int wait);

/* Write out every dirty block and wait, as sync_global (1) does.  */
void jt_sync_all (void);

/* Wait until the pager threads have served every request so far.  */
void jt_drain (void);

/* Number of page-ins whose data did not match the evicted bytes.  */
unsigned long jt_stale_reads (void);

/* Block B as the store (the "disk") has it right now.  */
const void *jt_disk_block (block_t b);

/* The write log.  Entry I is a write of LEN bytes at byte offset OFF,
   or a store_sync when DATA is NULL.  */
struct jt_write
{
  uint64_t off;
  size_t len;
  void *data;
};
size_t jt_log_count (void);
const struct jt_write *jt_log_entry (size_t i);

/* Write the image as it was after the first N log entries to PATH: the
   image as loaded plus those writes.  With TORN, the Nth write only
   reaches the disk for its first half.  */
void jt_build_image (size_t n, int torn, const char *path);

/* Write the current store to PATH.  */
void jt_save_image (const char *path);

/* Run CMD through the shell and return its exit status.  */
int jt_run (const char *fmt, ...) __attribute__ ((format (printf, 1, 2)));

/* Called with every range the journal hands back to the allocator, if
   set.  */
extern void (*jt_on_release) (block_t start, unsigned long count);

#endif
