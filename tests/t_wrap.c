/* Fill the log many times over: wrap-around, forced checkpoints, and
   transactions that outgrow j_max_transaction_buffers while their handle
   is open.

   The base image's journal is the smallest mke2fs makes (1024 blocks),
   so a transaction may hold at most 256 blocks.  Operations write 1 to 40
   blocks out of a pool of 600, and every 50th writes 300 in one handle.
   Most end with a plain stop, leaving the commits to kjournald when a
   transaction is full; every 25th commits.  Then the journal is
   unmounted cleanly, and crash images are checked as in t_crash.  */

#include "common.h"

#define NBLK 600
#define OPS 400

int
main (int argc, char **argv)
{
  const char *image = argc > 1 ? argv[1] : "build/base.img";
  block_t blocks[NBLK];
  static struct history h;
  unsigned long warnings;
  size_t checked, log_writes = 0, jblocks = 0;

  srand (argc > 2 ? atoi (argv[2]) : 1);
  jt_mount (image, 2, 0);
  pick_free_blocks (blocks, NBLK);
  history_init (&h, blocks, NBLK, OPS);

  for (int op = 1; op <= OPS; op++)
    {
      diskfs_transaction_t *txn = diskfs_journal_start_transaction ();
      int n = op % 50 == 0 ? 300 : 1 + rand () % 40;
      int sync = op % 25 == 0;

      CHECK (txn, "no transaction");
      for (int i = 0; i < n; i++)
	{
	  int k = rand () % NBLK;
	  history_write (&h, txn, k);
	  if (rand () % 8 == 0)
	    jt_pageout (blocks[k], rand () % 2, 0);
	}
      if (sync)
	diskfs_journal_commit_transaction (txn);
      else
	diskfs_journal_stop_transaction (txn);
      history_op_done (&h, sync);

      /* Committed blocks go home through the pager too.  */
      for (int i = rand () % 4; i > 0; i--)
	jt_pageout (blocks[rand () % NBLK], rand () % 2, 0);
    }

  warnings = jt_umount ();
  CHECK (warnings == 0, "%lu journal warnings", warnings);
  CHECK (jt_stale_reads () == 0, "%lu stale page-ins", jt_stale_reads ());

  /* Every block's last version is home.  */
  for (int k = 0; k < NBLK; k++)
    {
      long v = pattern_version (jt_disk_block (blocks[k]), blocks[k]);
      CHECK (v == h.ver[k], "block %u: version %ld on disk, %u expected",
	     blocks[k], v, h.ver[k]);
    }

  /* The log really went round a few times.  */
  for (size_t i = 0; i < jt_log_count (); i++)
    if (jt_log_entry (i)->data && jt_in_journal (jt_log_entry (i)->off))
      log_writes++;
  for (uint64_t off = 0; off < (uint64_t) jt_nblocks () << log2_block_size;
       off += block_size)
    jblocks += jt_in_journal (off);
  CHECK (log_writes > 3 * jblocks, "only %zu log writes for a %zu-block "
	 "journal", log_writes, jblocks);

  checked = jt_crash_check ("t_wrap", history_check, &h);
  printf ("t_wrap: OK (%d operations, %zu writes, %.1f times round the "
	  "log, %zu crash images)\n", OPS, jt_log_count (),
	  (double) log_writes / jblocks, checked);
  return 0;
}
