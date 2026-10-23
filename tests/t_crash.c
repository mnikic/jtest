/* Crash at every point of a run, and check what e2fsck recovers.

   Transactions write new versions of free blocks, each committed with
   diskfs_journal_commit_transaction, while the pager cleans and evicts
   them; then the journal is unmounted cleanly, so that the run also
   covers the checkpoints and the final superblock writes.  For each
   prefix of the write log (a crash right after it, optionally with the
   last write torn), the image is rebuilt, e2fsck -fy replays the journal,
   and e2fsck -fn must find it clean.  The blocks must then hold exactly
   the state after some commit K: no transaction half there, nothing that
   was not committed.  K must be at least the last commit that had
   returned before the crash.

   JT_CRASH_POINTS sets how many prefixes are checked (default 300, 0 for
   all of them).  */

#include <fcntl.h>
#include <unistd.h>
#include "common.h"

#define NBLK 16
#define ROUNDS 60

static block_t blocks[NBLK];
static uint32_t snap[ROUNDS + 1][NBLK];	/* Versions after commit K.  */
static size_t snap_log[ROUNDS + 1];	/* Log length when commit K returned.  */

/* The index of the commit whose state IMG holds, -1 if none.  */
static int
recovered_commit (const char *img, int verbose)
{
  int fd = open (img, O_RDONLY);
  long got[NBLK];
  char buf[block_size];

  CHECK (fd >= 0, "open %s", img);
  for (int i = 0; i < NBLK; i++)
    {
      CHECK (pread (fd, buf, block_size, (off_t) blocks[i] << log2_block_size)
	     == block_size, "read");
      got[i] = pattern_version (buf, blocks[i]);
    }
  close (fd);

  for (int k = ROUNDS; k >= 0; k--)
    {
      int i;
      for (i = 0; i < NBLK && got[i] == snap[k][i]; i++)
	;
      if (i == NBLK)
	return k;
    }
  if (verbose)
    for (int i = 0; i < NBLK; i++)
      fprintf (stderr, "  block %u: version %ld\n", blocks[i], got[i]);
  return -1;
}

int
main (int argc, char **argv)
{
  const char *image = argc > 1 ? argv[1] : "build/base.img";
  const char *img = "build/t_crash.img";
  const char *env = getenv ("JT_CRASH_POINTS");
  uint32_t ver[NBLK] = { 0 };
  size_t points, step, n_log, checked = 0;
  unsigned long warnings;

  srand (argc > 2 ? atoi (argv[2]) : 1);
  jt_mount (image, 2, 0);
  pick_free_blocks (blocks, NBLK);
  snap_log[0] = 0;

  for (int k = 1; k <= ROUNDS; k++)
    {
      diskfs_transaction_t *txn = diskfs_journal_start_transaction ();
      int n = 1 + rand () % 6;

      CHECK (txn, "no transaction");
      for (int i = 0; i < n; i++)
	{
	  int b = rand () % NBLK;
	  journal_version (txn, blocks[b], ++ver[b]);
	  if (rand () % 3 == 0)
	    jt_pageout (blocks[b], rand () % 2, 0);
	}
      diskfs_journal_commit_transaction (txn);
      memcpy (snap[k], ver, sizeof ver);
      snap_log[k] = jt_log_count ();

      /* Let committed blocks go home through the pager too.  */
      if (rand () % 4 == 0)
	jt_pageout (blocks[rand () % NBLK], rand () % 2, 0);
    }

  warnings = jt_umount ();
  CHECK (warnings == 0, "%lu journal warnings", warnings);
  CHECK (jt_stale_reads () == 0, "%lu stale page-ins", jt_stale_reads ());

  n_log = jt_log_count ();
  points = env ? strtoul (env, NULL, 0) : 300;
  step = points && n_log > points ? n_log / points : 1;

  for (size_t n = 0; n <= n_log; n += step)
    for (int torn = 0; torn < 2; torn++)
      {
	int durable = 0, k, fsck;

	if (torn && (n == 0 || !jt_log_entry (n - 1)->data))
	  continue;
	while (durable < ROUNDS && snap_log[durable + 1] <= n)
	  durable++;

	jt_build_image (n, torn, img);
	fsck = jt_run ("e2fsck -fy %s >build/t_crash.fsck1 2>&1", img);
	CHECK ((fsck & ~1) == 0, "crash after write %zu%s: e2fsck -fy exit %d "
	       "(see build/t_crash.fsck1)", n, torn ? " (torn)" : "", fsck);
	fsck = jt_run ("e2fsck -fn %s >build/t_crash.fsck2 2>&1", img);
	CHECK (fsck == 0, "crash after write %zu%s: not clean after replay "
	       "(see build/t_crash.fsck2)", n, torn ? " (torn)" : "");

	k = recovered_commit (img, 1);
	CHECK (k >= 0, "crash after write %zu%s: the blocks match no commit",
	       n, torn ? " (torn)" : "");
	CHECK (k >= durable, "crash after write %zu%s: recovered commit %d, "
	       "but commit %d had returned", n, torn ? " (torn)" : "",
	       k, durable);
	checked++;
      }

  printf ("t_crash: OK (%d commits, %zu writes, %zu crash images)\n",
	  ROUNDS, n_log, checked);
  return 0;
}
