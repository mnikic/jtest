/* Crash at every point of a run, and check what e2fsck recovers.

   Operations write new versions of free blocks, each committed with
   diskfs_journal_commit_transaction, while the pager cleans and evicts
   them; then the journal is unmounted cleanly, so that the run also
   covers the checkpoints and the final superblock writes.  Every crash
   image must hold the state after one operation, no older than the last
   whose commit had returned (see struct history).  */

#include "common.h"

#define NBLK 16
#define OPS 60

int
main (int argc, char **argv)
{
  const char *image = argc > 1 ? argv[1] : "build/base.img";
  block_t blocks[NBLK];
  static struct history h;
  unsigned long warnings;
  size_t checked;

  srand (argc > 2 ? atoi (argv[2]) : 1);
  jt_check_home_order = 1;
  jt_mount (image, 2, 0);
  pick_free_blocks (blocks, NBLK);
  history_init (&h, blocks, NBLK, OPS);

  for (int op = 0; op < OPS; op++)
    {
      diskfs_transaction_t *txn = diskfs_journal_start_transaction ();
      int n = 1 + rand () % 6;

      CHECK (txn, "no transaction");
      for (int i = 0; i < n; i++)
	{
	  int k = rand () % NBLK;
	  history_write (&h, txn, k);
	  if (rand () % 3 == 0)
	    jt_pageout (blocks[k], rand () % 2, 0);
	}
      diskfs_journal_commit_transaction (txn);
      history_op_done (&h, 1);

      /* Let committed blocks go home through the pager too.  */
      if (rand () % 4 == 0)
	jt_pageout (blocks[rand () % NBLK], rand () % 2, 0);
    }

  warnings = jt_umount ();
  CHECK (warnings == 0, "%lu journal warnings", warnings);
  CHECK (jt_stale_reads () == 0, "%lu stale page-ins", jt_stale_reads ());

  checked = jt_crash_check ("t_crash", history_check, &h);
  printf ("t_crash: OK (%d commits, %zu writes, %zu crash images)\n",
	  OPS, jt_log_count (), checked);
  return 0;
}
