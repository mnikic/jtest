/* Transactions whose only change is a free.

   journal_record_freed_blocks puts a revoke record and the freed extent
   in the transaction, and nothing else; free_blocks journals the bitmap
   too, but the journal must not depend on it.  Such a transaction must
   commit like any other: its revoke records reach the log, and its
   blocks come back to the allocator.

   1. T1 journals metadata block B (version 1) and commits.
   2. T2 only frees B, and commits: B must be handed back, and a revoke
      record for B must be in the log.
   3. B becomes file data, written home.
   4. T3 only frees block C, and stops without committing; the unmount
      must commit it and hand C back, and stop the journal cleanly.
   B must hold its data after a clean unmount, and in every crash image
   after it was written: T1's copy of B must never be replayed over it.  */

#include <pthread.h>
#include "common.h"
#include "jbd2_format.h"

static block_t b_blk, c_blk;
static int b_released, c_released;
static size_t data_written_at = SIZE_MAX;
#define DATA_WORD 0x80000001u

static void
on_release (block_t start, unsigned long count)
{
  if (b_blk >= start && b_blk < start + count)
    __atomic_store_n (&b_released, 1, __ATOMIC_RELEASE);
  if (c_blk >= start && c_blk < start + count)
    __atomic_store_n (&c_released, 1, __ATOMIC_RELEASE);
}

/* Whether the write log has a JBD2 revoke record for block B.  */
static int
revoke_logged (block_t b)
{
  for (size_t i = 0; i < jt_log_count (); i++)
    {
      const struct jt_write *w = jt_log_entry (i);
      if (!w->data || !jt_in_journal (w->off))
	continue;
      const journal_revoke_header_t *r = w->data;
      if (be32toh (r->r_header.h_magic) != JBD2_MAGIC_NUMBER
	  || be32toh (r->r_header.h_blocktype) != JBD2_REVOKE_BLOCK)
	continue;
      const uint32_t *e = (const uint32_t *) (r + 1);
      size_t n = (be32toh (r->r_count) - sizeof *r) / sizeof *e;
      for (size_t k = 0; k < n; k++)
	if (be32toh (e[k]) == b)
	  return 1;
    }
  return 0;
}

static const char *
check (size_t n, int torn, const char *img, void *arg)
{
  static char msg[128];
  FILE *f = fopen (img, "r");
  char *buf = malloc (block_size);
  long v;

  (void) torn;
  (void) arg;
  CHECK (f, "open %s", img);
  CHECK (!fseeko (f, (off_t) b_blk << log2_block_size, SEEK_SET)
	 && fread (buf, block_size, 1, f) == 1, "read");
  fclose (f);
  v = pattern_version (buf, b_blk);
  free (buf);
  /* A torn write of the data itself leaves it torn.  */
  if (n >= data_written_at && v != DATA_WORD
      && !(torn && n == data_written_at))
    {
      snprintf (msg, sizeof msg, "block %u holds %lx, not its data",
		b_blk, v);
      return msg;
    }
  return NULL;
}

int
main (int argc, char **argv)
{
  const char *image = argc > 1 ? argv[1] : "build/base.img";
  diskfs_transaction_t *txn;
  block_t picked[2];
  char *buf;
  unsigned long warnings;

  jt_mount (image, 2, 0);
  pick_free_blocks (picked, 2);
  b_blk = picked[0];
  c_blk = picked[1];
  jt_on_release = on_release;
  buf = malloc (block_size);

  /* 1.  */
  txn = diskfs_journal_start_transaction ();
  journal_version (txn, b_blk, 1);
  diskfs_journal_commit_transaction (txn);

  /* 2.  */
  txn = diskfs_journal_start_transaction ();
  CHECK (journal_record_freed_blocks (txn, b_blk, 1), "record B's free");
  diskfs_journal_commit_transaction (txn);
  CHECK (__atomic_load_n (&b_released, __ATOMIC_ACQUIRE),
	 "B was not handed back after its free was committed");
  CHECK (revoke_logged (b_blk), "no revoke record for B in the log");

  /* 3.  */
  jt_discard (b_blk, 0);
  jt_discard (b_blk, 1);
  pattern (buf, b_blk, DATA_WORD);
  jt_file_edit (b_blk, 0, buf, block_size);
  jt_file_pageout (b_blk, 1, 1);
  data_written_at = jt_log_count ();

  /* 4.  */
  txn = diskfs_journal_start_transaction ();
  CHECK (journal_record_freed_blocks (txn, c_blk, 1), "record C's free");
  diskfs_journal_stop_transaction (txn);

  warnings = jt_umount ();
  CHECK (warnings == 0, "%lu journal warnings", warnings);
  CHECK (__atomic_load_n (&c_released, __ATOMIC_ACQUIRE),
	 "C was not handed back by the unmount");
  CHECK (pattern_version (jt_disk_block (b_blk), b_blk) == DATA_WORD,
	 "block %u: %lx home, not its data", b_blk,
	 pattern_version (jt_disk_block (b_blk), b_blk));

  size_t checked = jt_crash_check ("t_free_only", check, NULL);
  printf ("t_free_only: OK (%zu writes, %zu crash images)\n",
	  jt_log_count (), checked);
  free (buf);
  return 0;
}
