/* Fill the log many times over: wrap-around, forced checkpoints, and
   transactions that outgrow j_max_transaction_buffers while their handle
   is open.

   The base image's journal is the smallest mke2fs makes (1024 blocks),
   so a transaction may hold at most 256 blocks.  Operations write 1 to 40
   blocks out of a pool of 600, and every 50th writes 300 in one handle.
   Most end with a plain stop, leaving the commits to kjournald when a
   transaction is full; every 25th commits.  Then the journal is
   unmounted cleanly, and crash images are checked as in t_crash.

   This is the test whose log fills, so it is where checkpoints write
   blocks home.  Two more runs each fail one checkpoint write with EIO:
     ckpt         one while the operations go on: the checkpoint stops,
		  and a later one writes the block;
     ckpt-umount  every home write from the unmount on, the pagers'
		  too, as when the disk goes bad then: the unmount's
		  checkpoint fails, and leaves the journal dirty for a
		  replay, which must bring every block's last version
		  home.  If every committed block was home already, only
		  the pages left dirty fail, and the journal is clean.
   By default all three run, each in a process of its own.  */

#include <sys/wait.h>
#include <unistd.h>

#include "common.h"

#define NBLK 600
#define OPS 400

static const char *mode;
static int fail_armed, failed, umount_failures, umount_journal_failures;
static unsigned long matches, fail_at;
static int umount_fail;

static int
fail_hook (uint64_t off, size_t len, const void *buf)
{
  (void) len;
  (void) buf;
  if (!__atomic_load_n (&fail_armed, __ATOMIC_ACQUIRE)
      || __atomic_load_n (&failed, __ATOMIC_ACQUIRE)
      || jt_in_journal (off) || off < 4096 * 16
      || (!umount_fail && (jt_on_pager_thread () || !jt_checkpointing ())))
    return 0;
  if (__atomic_add_fetch (&matches, 1, __ATOMIC_RELAXED) < fail_at)
    return 0;
  /* At the unmount, they all fail.  A pager's, on a pager thread, can be
     of a page left dirty whose block a checkpoint already wrote home;
     only the journal's own home writes leave it something to replay.  */
  if (!umount_fail)
    __atomic_store_n (&failed, 1, __ATOMIC_RELEASE);
  else
    {
      __atomic_store_n (&umount_failures, 1, __ATOMIC_RELEASE);
      if (!jt_on_pager_thread ())
	__atomic_store_n (&umount_journal_failures, 1, __ATOMIC_RELEASE);
    }
  return EIO;
}

static int
run (const char *image, unsigned seed)
{
  block_t blocks[NBLK];
  static struct history h;
  unsigned long warnings;
  size_t checked, log_writes = 0, jblocks = 0;
  umount_fail = !strcmp (mode, "ckpt-umount");

  srand (seed);
  if (!strcmp (mode, "ckpt"))
    {
      fail_at = 100 + seed % 1000;
      fail_armed = 1;
    }
  else if (umount_fail)
    fail_at = 1;
  JT_SET_HOOK (jt_store_fail_hook, fail_hook);
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

  if (umount_fail)
    __atomic_store_n (&fail_armed, 1, __ATOMIC_RELEASE);
  warnings = jt_umount ();
  JT_SET_HOOK (jt_store_fail_hook, NULL);
  failed |= umount_failures;
  if (strcmp (mode, "plain"))
    {
      CHECK (failed, "no checkpoint write failed (%s)", mode);
      fprintf (stderr, "t_wrap %s: a checkpoint write failed; %lu "
	       "warnings\n", mode, warnings);
    }
  else
    CHECK (warnings == 0, "%lu journal warnings", warnings);
  CHECK (jt_stale_reads () == 0, "%lu stale page-ins", jt_stale_reads ());

  /* Every block's last version is home, after the replay of a journal
     the unmount left dirty.  */
  const char *img = "build/t_wrap-umount.img";
  jt_save_image (img);
  if (umount_fail)
    {
      /* With every committed block home before the unmount, only pages
	 left dirty failed, and the journal is rightly clean: the check of
	 the blocks below is what counts.  */
      CHECK (!umount_journal_failures
	     || jt_journal_check () == JOURNAL_NEEDS_REPLAY, "the unmount's "
	     "checkpoint failed, but the journal is clean");
      CHECK (jt_run ("e2fsck -E journal_only -y %s >%s.fsck 2>&1", img, img)
	     <= 1, "replay of %s", img);
    }
  FILE *f = fopen (img, "r");
  char *buf = malloc (block_size);
  CHECK (f, "open %s", img);
  for (int k = 0; k < NBLK; k++)
    {
      CHECK (!fseeko (f, (off_t) blocks[k] << log2_block_size, SEEK_SET)
	     && fread (buf, block_size, 1, f) == 1, "read %s", img);
      long v = pattern_version (buf, blocks[k]);
      if (v != h.ver[k])
	jt_trace_dump (blocks[k]);
      CHECK (v == h.ver[k], "block %u: version %ld on disk, %u expected",
	     blocks[k], v, h.ver[k]);
    }
  fclose (f);
  free (buf);
  unlink (img);

  /* The log really went round a few times.  */
  for (size_t i = 0; i < jt_log_count (); i++)
    if (jt_log_entry (i)->data && jt_in_journal (jt_log_entry (i)->off))
      log_writes++;
  for (uint64_t off = 0; off < (uint64_t) jt_nblocks () << log2_block_size;
       off += block_size)
    jblocks += jt_in_journal (off);
  CHECK (log_writes > 3 * jblocks, "only %zu log writes for a %zu-block "
	 "journal", log_writes, jblocks);

  char name[64];
  snprintf (name, sizeof name, strcmp (mode, "plain") ? "t_wrap-%s"
	    : "t_wrap", mode);
  checked = jt_crash_check (name, history_check, &h);
  printf ("t_wrap: OK (%s%s: %d operations, %zu writes, %.1f times round "
	  "the log, %zu crash images)\n", mode,
	  !umount_fail ? "" : umount_journal_failures
	  ? ", the unmount's checkpoint failed"
	  : ", only page-outs failed at the unmount",
	  OPS, jt_log_count (), (double) log_writes / jblocks, checked);
  return 0;
}

int
main (int argc, char **argv)
{
  const char *image = argc > 1 ? argv[1] : "build/base.img";
  unsigned seed = argc > 2 ? atoi (argv[2]) : 1;
  static const char *const all[] = { "plain", "ckpt", "ckpt-umount", NULL };
  const char *one[] = { argc > 3 ? argv[3] : NULL, NULL };
  const char *const *modes = argc > 3 ? one : all;
  int status;

  for (; *modes; modes++)
    {
      fflush (NULL);
      pid_t pid = fork ();
      CHECK (pid >= 0, "fork");
      if (pid == 0)
	{
	  mode = *modes;
	  exit (run (image, seed));
	}
      CHECK (waitpid (pid, &status, 0) == pid, "wait");
      if (!WIFEXITED (status) || WEXITSTATUS (status))
	return 1;
    }
  return 0;
}
