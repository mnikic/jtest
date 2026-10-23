/* Journal versions of free blocks in many transactions while the pager
   cleans and evicts them, unmount cleanly, and check that every block's
   last version reached home, that no page-in read stale data, and that
   e2fsck finds the image clean.  */

#include "common.h"

#define NBLK 64
#define ROUNDS 2000

int
main (int argc, char **argv)
{
  block_t blocks[NBLK];
  uint32_t ver[NBLK] = { 0 };
  const char *image = argc > 1 ? argv[1] : "build/base.img";
  const char *out = "build/t_basic.img";
  unsigned seed = argc > 2 ? atoi (argv[2]) : 1;
  unsigned long warnings;

  srand (seed);
  jt_mount (image, 2, 0);
  pick_free_blocks (blocks, NBLK);

  for (int round = 0; round < ROUNDS; round++)
    {
      diskfs_transaction_t *txn = diskfs_journal_start_transaction ();
      int n = 1 + rand () % 8;

      CHECK (txn, "no transaction");
      for (int i = 0; i < n; i++)
	{
	  int k = rand () % NBLK;
	  journal_version (txn, blocks[k], ++ver[k]);
	  if (rand () % 4 == 0)
	    jt_pageout (blocks[k], rand () % 2, 0);
	}

      if (round % 50 == 49)
	diskfs_journal_commit_transaction (txn);
      else
	diskfs_journal_stop_transaction (txn);
    }

  warnings = jt_umount ();
  CHECK (warnings == 0, "%lu journal warnings", warnings);
  CHECK (jt_stale_reads () == 0, "%lu stale page-ins", jt_stale_reads ());

  for (int k = 0; k < NBLK; k++)
    {
      long v = pattern_version (jt_disk_block (blocks[k]), blocks[k]);
      CHECK (v == ver[k], "block %u: version %ld on disk, %u expected",
	     blocks[k], v, ver[k]);
    }

  jt_save_image (out);
  CHECK (jt_run ("e2fsck -fn %s >/dev/null 2>&1", out) == 0,
	 "e2fsck -fn %s is not clean", out);

  printf ("t_basic: OK (%d transactions, %zu writes)\n", ROUNDS,
	  jt_log_count ());
  return 0;
}
