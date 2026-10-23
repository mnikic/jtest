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
#include <stdio.h>
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

/* The same for directory blocks, whose live copy is in a directory's
   file pager instead of the disk cache.  The file pager writes them
   through journal_store_write with DISK_CACHE 0.  Edit them inside a
   handle as dir.c does: journal_get_write_access_external, jt_dir_edit,
   then jt_dir_mark_dirty (or journal_release_access instead, if nothing
   was edited).  */
void jt_dir_edit (block_t b, size_t off, const void *data, size_t len);
void jt_dir_read (block_t b, void *out);
void jt_dir_pageout (block_t b, int evict, int wait);

/* Drop block B's page from the file pager (FILE_PAGER 1), as truncating
   the file does, or from the disk cache (0), as ext2_new_block clearing
   its modified_global_blocks bit does: it is no longer written out, and
   its next page-in reads what is on disk.  A truncation waits for a
   write of the page in flight; the disk pager's goes on.  For a block
   that changes owner.  */
void jt_discard (block_t b, int file_pager);

/* journal_mark_dirty_external of directory block B, as the file pager's
   mapping has it.  */
error_t jt_dir_mark_dirty (diskfs_transaction_t *txn, block_t b);

/* Run FN (ARG) on a pager thread and wait for it, as a fault waits for
   pager_unlock_page: the thread is marked as a pager thread
   (journal_thread_set_pager), so a transaction FN starts never waits.  */
void jt_pager_call (void (*fn) (void *), void *arg);

/* Write out every dirty block of the directory pager and the disk cache
   and wait, as diskfs_sync_everything does.  */
void jt_sync_all (void);

/* Wait until the pager threads have served every request so far.  */
void jt_drain (void);

/* Record WHAT happened to block B for the trace printed on a stale
   page-in; DATA, if not NULL, is an image of the block.  */
void jt_trace (block_t b, const char *what, const void *data);

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

/* Print log entries FROM to TO, decoded: syncs, log blocks (JBD2
   headers, descriptor tags) and home writes (their first two words, which
   for the tests' blocks are the block number and version).  */
void jt_dump_log (FILE *f, size_t from, size_t to);

/* Write the image as it was after the first N log entries to PATH: the
   image as loaded plus those writes.  With TORN, the Nth write only
   reaches the disk for its first half.  */
void jt_build_image (size_t n, int torn, const char *path);

/* Whether byte offset OFF of the store is in the journal's blocks.  */
int jt_in_journal (uint64_t off);

/* Checks the image IMG that a crash after the first N log entries leaves
   (with the last one torn if TORN), once e2fsck has replayed it.  Returns
   NULL if it is right, or what is wrong.  */
typedef const char *(*jt_crash_check_fn) (size_t n, int torn,
					  const char *img, void *arg);

/* For crash points spread over the write log, each also with its last
   write torn: rebuild the image, replay its journal (e2fsck -E
   journal_only, which repairs nothing), require e2fsck -fn to find it
   clean, then call CHECK.  JT_CRASH_POINTS sets how
   many points (default 300, 0 for every one); JT_JOBS how many run at
   once (default: one per CPU); JT_TMP where the images go (default
   /dev/shm); JT_NO_CRASH skips the checks.  On the first failure, reports it, keeps the crashed image
   as build/NAME-crash-N.img and exits.  Returns the number of images
   checked.  Call it after jt_umount or jt_abandon.  */
size_t jt_crash_check (const char *name, jt_crash_check_fn check, void *arg);

/* Write the current store to PATH.  */
void jt_save_image (const char *path);

/* Run CMD through the shell and return its exit status.  */
int jt_run (const char *fmt, ...) __attribute__ ((format (printf, 1, 2)));

/* Whether TXN is draining for a commit (T_LOCKED).  */
int jt_txn_locked (diskfs_transaction_t *txn);

/* Copy the shadow of block B in TXN to OUT.  ENOENT if TXN does not
   hold B.  */
int jt_shadow (diskfs_transaction_t *txn, block_t b, void *out);

/* Called before every store_write of LEN bytes from BUF at byte offset
   OFF, if set, outside the journal lock.  Tests use it to hold a write.  */
extern void (*jt_store_write_hook) (uint64_t off, size_t len,
				    const void *buf);

/* Whether a transaction in the checkpoint list still has to write block
   B home: a buffer for B not marked written.  */
int jt_checkpoint_unwritten (block_t b);

/* Called on every disk_cache_block_ref_mapped of block B, if set: the
   stop-time sweep calls it, unlocked, right before it copies B.  */
extern void (*jt_ref_mapped_hook) (block_t b);

/* Called with every range the journal hands back to the allocator, if
   set.  */
extern void (*jt_on_release) (block_t start, unsigned long count);

#endif
