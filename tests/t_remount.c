/* Mounting again on a log that holds an earlier mount's transactions.

   A first mount, in a child process, commits enough to fill the log
   several times over, and either unmounts cleanly or crashes partway,
   after which e2fsck -E journal_only replays the log as a boot would.  A
   second mount then works on that image, writing new versions of the
   same blocks.  The log still holds the first mount's transactions; the
   second mount's sequence numbers must carry on past them, so that a
   replay stops at the first stale block instead of applying it.

   Every crash image of the second mount must hold the state after one
   of its operations, starting from what the first mount left.  A
   replayed stale transaction shows as a block back at an older
   version.

   With transactions of random sizes, the two mounts' log layouts rarely
   line up, and a stale transaction would replay only if the block after
   the second mount's last commit were a descriptor of exactly the next
   sequence number.  The aligned variant makes them line up: both mounts
   commit one-block transactions one by one, three log blocks each, and
   the first does not wrap, so its transaction J sits where the second
   mount's J-th would.  A second mount that reused the first's sequence
   numbers would then replay the first's.

   JT_REMOUNT=clean, crashed or aligned runs one variant (default all).  */

#include <sys/wait.h>
#include <unistd.h>
#include "common.h"

#define NBLK 64
#define OPS_A 300
#define OPS_B 60

/* The first mount, in a child.  Writes the image it leaves to OUT.  */
enum variant { CLEAN, CRASHED, ALIGNED };
static const char *const names[] = { "clean", "crashed", "aligned" };
#define ALIGNED_OPS 100		/* 300 log blocks: no wrap.  */

static void
first_mount (const char *image, const char *out, enum variant var,
	     unsigned seed)
{
  block_t blocks[NBLK];
  uint32_t ver[NBLK] = { 0 };
  int crash = var == CRASHED;

  srand (seed);
  jt_mount (image, 2, JT_QUIET);
  pick_free_blocks (blocks, NBLK);
  for (int op = 1; op <= (var == ALIGNED ? ALIGNED_OPS : OPS_A); op++)
    {
      diskfs_transaction_t *txn = diskfs_journal_start_transaction ();
      if (var == ALIGNED)
	{
	  int k = rand () % NBLK;
	  journal_version (txn, blocks[k], ++ver[k]);
	  diskfs_journal_commit_transaction (txn);
	  continue;
	}
      for (int i = 1 + rand () % 20; i > 0; i--)
	{
	  int k = rand () % NBLK;
	  journal_version (txn, blocks[k], ++ver[k]);
	}
      if (op % 3 == 0)
	diskfs_journal_commit_transaction (txn);
      else
	diskfs_journal_stop_transaction (txn);
    }
  CHECK (jt_umount () == 0, "journal warnings in the first mount");

  if (!crash)
    jt_save_image (out);
  else
    {
      /* A crash two thirds of the way through, then the boot's replay.  */
      jt_build_image (jt_log_count () * 2 / 3, 0, out);
      CHECK ((jt_run ("e2fsck -E journal_only -y %s >/dev/null 2>&1", out)
	      & ~1) == 0, "replay of the first mount's crash");
    }
  CHECK (jt_run ("e2fsck -fn %s >/dev/null 2>&1", out) == 0,
	 "the first mount left %s unclean", out);
  exit (0);
}

static int
run (const char *image, enum variant var, unsigned seed)
{
  char mid[64], name[64];
  snprintf (mid, sizeof mid, "build/t_remount-%s.img", names[var]);
  snprintf (name, sizeof name, "t_remount-%s", names[var]);
  static block_t blocks[NBLK];
  static struct history h;
  pid_t pid;
  int status;

  fflush (NULL);
  pid = fork ();
  CHECK (pid >= 0, "fork");
  if (pid == 0)
    first_mount (image, mid, var, seed);
  CHECK (waitpid (pid, &status, 0) == pid && WIFEXITED (status)
	 && WEXITSTATUS (status) == 0, "the first mount failed");

  /* The second mount, on what the first left.  */
  srand (seed + 1);
  jt_mount (mid, 2, 0);
  pick_free_blocks (blocks, NBLK);
  history_init (&h, blocks, NBLK, OPS_B);
  for (int k = 0; k < NBLK; k++)
    {
      long v = pattern_version (jt_disk_block (blocks[k]), blocks[k]);
      CHECK (v >= 0, "block %u torn after the first mount", blocks[k]);
      h.ver[k] = v;
      h.snaps[k] = v;
    }

  for (int op = 1; op <= OPS_B; op++)
    {
      diskfs_transaction_t *txn = diskfs_journal_start_transaction ();
      for (int i = var == ALIGNED ? 1 : 1 + rand () % 10; i > 0; i--)
	{
	  int k = rand () % NBLK;
	  /* New versions well above the first mount's.  */
	  if (h.ver[k] < 100000)
	    h.ver[k] = 100000;
	  history_write (&h, txn, k);
	  if (var != ALIGNED && rand () % 5 == 0)
	    jt_pageout (blocks[k], rand () % 2, 0);
	}
      int sync = var == ALIGNED || op % 4 == 0;
      if (sync)
	diskfs_journal_commit_transaction (txn);
      else
	diskfs_journal_stop_transaction (txn);
      history_op_done (&h, sync);
    }
  CHECK (jt_umount () == 0, "journal warnings in the second mount");
  CHECK (jt_stale_reads () == 0, "stale page-ins");

  size_t checked = jt_crash_check (name, history_check, &h);
  printf ("t_remount: OK (%s first mount, %zu writes in the second, "
	  "%zu crash images)\n", names[var], jt_log_count (), checked);
  return 0;
}

int
main (int argc, char **argv)
{
  const char *image = argc > 1 ? argv[1] : "build/base.img";
  unsigned seed = argc > 2 ? atoi (argv[2]) : 1;
  const char *which = getenv ("JT_REMOUNT");
  int status;

  /* Each variant mounts in a process of its own.  */
  for (int var = CLEAN; var <= ALIGNED; var++)
    {
      if (which && strcmp (which, names[var]))
	continue;
      fflush (NULL);
      pid_t pid = fork ();
      CHECK (pid >= 0, "fork");
      if (pid == 0)
	exit (run (image, var, seed));
      CHECK (waitpid (pid, &status, 0) == pid, "wait");
      if (!WIFEXITED (status) || WEXITSTATUS (status))
	return 1;
    }
  return 0;
}
