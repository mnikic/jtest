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

/* Where inode INO is on disk: block *BLK of its inode table, at byte
   offset *OFF.  */
void jt_inode_location (ino_t ino, block_t *blk, size_t *off);

/* Copy LEN bytes from DATA to offset OFF of block B in the disk cache,
   paging B in first if it is evicted, and mark B dirty.  */
void jt_edit (block_t b, size_t off, const void *data, size_t len);

/* Copy block B as the disk cache has it (paging it in first) to OUT.  */
void jt_read (block_t b, void *out);

/* Ask the pager threads to write B out, keeping it resident (EVICT 0) or
   evicting it (EVICT 1).  While a journal runs it is always evicted, as
   ext2fs syncs no journaled block then (fakefs.c).  A clean block is not
   written.  With WAIT, wait until it is done.  */
void jt_pageout (block_t b, int evict, int wait);
/* An ext2_warning whose format contains this is not counted as one of
   jt_umount's warnings.  */
extern const char *jt_expected_warning;
/* Set by a test whose blocks only ever get newer versions (tests/
   common.h's pattern), none freed: a home write of an older version than
   one written home before stops the test (env/fakefs.c).  */
extern int jt_check_home_order;

/* Set one of the jt_*_hook pointers (or jt_on_release) while other
   threads may call it.  */
#define JT_SET_HOOK(hook, fn) __atomic_store_n (&(hook), (fn), __ATOMIC_RELEASE)

/* If set, called when ext2fs references a disk cache block inside
   diskfs_catch_exception (dino_ref, for instance); an error it returns
   makes the access fail as a failed page-in does on the Hurd.  */
extern int (*jt_ref_fail_hook) (block_t b);

/* Blocks whose latest version, in the disk cache or the directory
   pager, is not on disk: how many in *COUNT, and the first.  Journal
   blocks are not counted.  */
block_t jt_unwritten (size_t *count);
/* journal_check on the journal as the store now has it.  */
int jt_journal_check (void);
/* diskfs_readonly, and in *INHIBITED whether it was set while the protid
   RPCs were inhibited.  */
int jt_readonly (int *inhibited);
/* An RPC of the protid class: ports_inhibit_class_rpcs waits for those
   in flight, and new ones wait for its resume.  Not to be nested, nor
   called from jt_readonly's thread while in one.  */
void jt_rpc_begin (void);
void jt_rpc_end (void);
/* How many ports_inhibit_class_rpcs calls to come fail with EBUSY.  */
extern int jt_inhibit_busy;

/* The same for directory blocks, whose live copy is in a directory's
   file pager instead of the disk cache.  The file pager writes them
   through journal_store_write with DISK_CACHE 0.  Edit them inside a
   handle as dir.c does: journal_get_write_access_external, jt_dir_edit,
   then jt_dir_mark_dirty (or journal_release_access instead, if nothing
   was edited).  */
void jt_dir_edit (block_t b, size_t off, const void *data, size_t len);
void jt_dir_read (block_t b, void *out);
void jt_dir_pageout (block_t b, int evict, int wait);
/* jt_dir_pageout, taking with B the dirty blocks after it, N in all at
   most (RUN_MAX, 8, at most): one journal_store_write for the run, as
   file_pager_write_pages makes.  */
void jt_dir_pageout_run (block_t b, int n, int evict, int wait);
/* How many file pager page-outs wrote more than one block: they take
   the dirty blocks after the one asked for with them, half the time.  */
unsigned long jt_pageout_runs (void);

/* Drop block B's page from the file pager (FILE_PAGER 1), as truncating
   the file does, or from the disk cache (0), as ext2_new_block clearing
   its modified_global_blocks bit does: it is no longer written out, and
   its next page-in reads what is on disk.  A truncation waits for a
   write of the page in flight; the disk pager's goes on.  For a block
   that changes owner.  */
void jt_discard (block_t b, int file_pager);

/* The same, for file data in the file pager: not journaled, and
   written out by jt_sync_everything (directories are not, with a
   journal).  jt_dir_edit marks a block as a directory's, jt_file_edit as
   file data.  */
void jt_file_edit (block_t b, size_t off, const void *data, size_t len);
void jt_file_read (block_t b, void *out);
void jt_file_pageout (block_t b, int evict, int wait);

/* diskfs_sync_everything (WAIT) of pager.c with a journal, then the
   diskfs_set_hypermetadata (0, 0) of the sync thread: a handle in which
   jt_write_all_disknodes, if set, writes the inodes, committed; then
   every file pager's dirty pages written out except directories', and
   waited for with WAIT; then a store_sync with WAIT.  */
void jt_sync_everything (int wait);
/* jt_write_all_disknodes is inode.c's write_all_disknodes where that is
   linked in and the test sets none.  jt_sync_files_hook, if set, syncs
   other file pagers (env/fpager.c's) too.  */
extern void (*jt_write_all_disknodes) (diskfs_transaction_t *txn);
extern void (*jt_sync_files_hook) (int wait);

/* A thread doing jt_sync_everything every up to MAX_US microseconds,
   waiting one time in four, as libdiskfs's periodic sync does every few
   seconds; and its end, which returns how many syncs it did.  */
void jt_periodic_sync_start (unsigned max_us, unsigned seed);
unsigned long jt_periodic_sync_stop (void);

/* journal_mark_dirty_external of directory block B, as the file pager's
   mapping has it.  */
error_t jt_dir_mark_dirty (diskfs_transaction_t *txn, block_t b);

/* Run FN (ARG) on a pager thread and wait for it, as a fault waits for
   pager_unlock_page: the thread is marked as a pager thread
   (journal_thread_set_pager), so a transaction FN starts never waits.  */
void jt_pager_call (void (*fn) (void *), void *arg);
/* The same, without waiting for FN; jt_drain waits for it.  */
void jt_pager_call_async (void (*fn) (void *), void *arg);
/* Hooks for env/fpager.c: a SIGSEGV at ADDR, with the signal's ucontext,
   returns 1 if it was one of its mappings and is handled; called by
   jt_umount before the journal stops; the page-out that follows is one
   that keeps the page (pager_write_keeps_page) or not.  */
extern int (*jt_fault_hook) (void *addr, void *uctx);
extern void (*jt_umount_hook) (void);
/* If set, called with each node diskfs_node_norefs frees, first.  */
extern void (*jt_norefs_hook) (struct node *np);
/* If set, the watchdog calls it before it reports the threads: for a
   test to print what it knows (not async-signal-safe; the test is stuck
   anyway).  */
extern void (*jt_deadlock_hook) (void);
/* env/dirglue.c: if set, fshelp_checkdirmod's answer, for a test to
   refuse changes to a directory (EACCES) as for another user.  */
extern error_t (*jt_checkdirmod_hook) (io_statbuf_t *dir, io_statbuf_t *st);
void jt_set_keeps_page (int keeps);

/* env/fpager.c, a file pager for nodes, where it is linked in.  Write
   page PAGE of NP's pager out, evicting it with EVICT, and wait with
   WAIT; sync every pager (what jt_umount calls); the page-ins that did
   not get what an eviction wrote; pager activity.  */
void jt_fp_pageout (struct node *np, size_t page, int evict, int wait);
void jt_fp_sync_all (void);
unsigned long jt_fp_stale_reads (void);
void jt_fp_counts (unsigned long *pageins, unsigned long *pageouts,
		   unsigned long *unlocks);

/* Write out every dirty block of the directory pager and the disk cache,
   keeping them, and wait: as without a journal, or when it stops.  */
void jt_sync_all (void);

/* Wait until the pager threads have served every request so far.  */
void jt_drain (void);

/* Record WHAT happened to block B for the trace printed on a stale
   page-in; DATA, if not NULL, is an image of the block.  */
void jt_trace (block_t b, const char *what, const void *data);
/* The protid that a libdiskfs RPC handed back as PORT (dir_lookup's
   retry_port), or NULL; and its release, with its peropen.  */
struct protid *jt_protid_of_port (mach_port_t port);
void jt_protid_release (mach_port_t port);

/* Print the trace's events of block B, oldest first.  */
void jt_trace_dump (block_t b);

/* Number of page-ins that failed, as libpager fails the page-in of a
   page whose eviction write failed.  */
unsigned long jt_pager_errors (void);

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
/* Block B's data in write W, which can be of several blocks, or NULL if
   W does not write B.  */
const void *jt_write_block (const struct jt_write *w, block_t b);

/* Print log entries FROM to TO, decoded: syncs, log blocks (JBD2
   headers, descriptor tags) and home writes (their first two words, which
   for the tests' blocks are the block number and version).  */
void jt_dump_log (FILE *f, size_t from, size_t to);
/* The journal's view of its log: head, tail, free space and its
   transactions, for a report.  */
void jt_journal_state (FILE *f);
/* The journal's buffers of block B, for a trace event (see
   journal_peek.c).  */
void jt_block_holders (block_t b, char *out, size_t len);
/* Checkpoint every transaction in the checkpoint list now.  */
void jt_checkpoint_now (void);

/* Write the image as it was after the first N log entries to PATH: the
   image as loaded plus those writes.  With TORN, the Nth write only
   reaches the disk for its first half.  */
void jt_build_image (size_t n, int torn, const char *path);

/* Whether byte offset OFF of the store is in the journal's blocks.  */
int jt_in_journal (uint64_t off);

/* The journal block index of byte offset OFF (0 for the journal
   superblock), or -1 if it is not in the journal.  */
long jt_journal_index (uint64_t off);

/* A crash after the first N log entries, on a disk that had not written
   all of those since the last flush (store_sync): for VARIANT 0, none
   is lost (the default); for another, some, picked at random from N and
   VARIANT, and maybe one of the others torn.  jt_build_image of N then
   builds that image, until the next call.  Returns how many are lost.  */
size_t jt_crash_reorder (size_t n, unsigned variant);
/* Whether log entry I is on the disk of the crash set up.  */
int jt_crash_kept (size_t i);
/* Whether log entry I is the write torn in the image jt_build_image
   built last; whether it is a write on that disk whole.  */
int jt_crash_torn_at (size_t i);
int jt_crash_on_disk (size_t i);
/* Which writes were lost, for a report.  */
void jt_crash_describe (char *buf, size_t len);

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
   /dev/shm); JT_NO_CRASH skips the checks.  JT_REORDER=R adds R
   reordered crashes per point (jt_crash_reorder); CHECK then sees TORN
   as 0, and asks jt_crash_kept which writes are there.  On the first failure, reports it, keeps the crashed image
   as build/NAME-crash-N.img and exits.  Returns the number of images
   checked.  Call it after jt_umount or jt_abandon.  */
size_t jt_crash_check (const char *name, jt_crash_check_fn check, void *arg);
/* If set, run instead of the replay and e2fsck -fn on each crash image
   IMG, as the crash left it, with its output to OUT: NULL if the image is
   right, or what is wrong.  CHECK may then be NULL.  */
extern const char *(*jt_crash_fsck) (const char *img, const char *out);

/* Write the current store to PATH.  */
void jt_save_image (const char *path);

/* Run CMD through the shell and return its exit status.  */
int jt_run (const char *fmt, ...) __attribute__ ((format (printf, 1, 2)));

/* Whether a checkpoint (journal_flush_checkpoints_locked) is at work:
   the home writes the journal makes then are mostly its.  */
int jt_checkpointing (void);
/* How many blocks of the journal's transactions are claimed for a home
   write (jb_is_flushing), or -1 while a commit or a checkpoint is at
   work, which claim blocks for their own writes.  With the pagers
   drained too (jt_drain), a claim is one nothing will release.  */
long jt_flush_claims (void);
/* TXN's number, and the last transaction whose freed blocks went back
   to the allocator (with all before it).  */
uint32_t jt_txn_tid (diskfs_transaction_t *txn);
/* How many commits had to checkpoint to make room in the log.  */
/* The checkpoints a commit forced, or JT_UNKNOWN in a tree without the
   journal's counters.  */
#define JT_UNKNOWN ((unsigned long) -1)
unsigned long jt_forced_checkpoints (void);
uint32_t jt_released_tid (void);

/* Whether TXN is draining for a commit (T_LOCKED).  */
int jt_txn_locked (diskfs_transaction_t *txn);

/* Copy the shadow of block B in TXN to OUT.  ENOENT if TXN does not
   hold B.  */
int jt_shadow (diskfs_transaction_t *txn, block_t b, void *out);

/* Called before every store_write of LEN bytes from BUF at byte offset
   OFF, if set, outside the journal lock.  Tests use it to hold a write.  */
extern void (*jt_store_write_hook) (uint64_t off, size_t len,
				    const void *buf);

/* Called after every store_read of LEN bytes at byte offset OFF, if
   set, outside any lock: a test holds a read there.  */
extern void (*jt_store_read_hook) (uint64_t off, size_t len);

/* Record a journal that was started (see env/fakefs.c).  */
void jt_journal_started (void *journal);
/* jt_journal_state for a journal ext2_journal may no longer point to.  */
void jt_journal_state_of (FILE *f, void *journal);

/* With env/ext2glue.c: a real node for inode INO, from the disk cache,
   with no file pager.  */
struct node *jt_node_load (ino_t ino);
/* With env/ext2glue.c: set the globals ext2fs computes at mount.  */
void jt_ext2_globals (void);
/* With env/inodeglue.c: the same, for inode.c and the rest too.  */
void jt_inode_globals (void);

/* Whether the calling thread is one of the harness's pager threads.  */
int jt_on_pager_thread (void);

/* Called before every store_write, if set: a nonzero return is the
   error the write fails with, and nothing of it reaches the disk.  */
extern int (*jt_store_fail_hook) (uint64_t off, size_t len,
				  const void *buf);

/* Whether a transaction in the checkpoint list still has to write block
   B home: a buffer for B not marked written.  */
int jt_checkpoint_unwritten (block_t b);

/* Called on every disk_cache_block_ref of block B, if set, before the
   block is faulted in, with whatever locks the caller holds.  */
extern void (*jt_ref_hook) (block_t b);

/* Called on every disk_cache_block_ref_mapped of block B, if set: the
   stop-time sweep calls it, unlocked, right before it copies B.  */
extern void (*jt_ref_mapped_hook) (block_t b);

/* Called with every range the journal hands back to the allocator, if
   set.  */
extern void (*jt_on_release) (block_t start, unsigned long count);

#endif
