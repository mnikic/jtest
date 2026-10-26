/* A file pager's page-out of a run of directory blocks: one
   journal_store_write for several blocks, as file_pager_write_pages
   makes.  The first block goes through the hazard check; each block
   after it is written with it only if journal_claim_safe_block_locked
   can claim it (jb_is_flushing) in the transactions that still have to
   write it home.  Every claim must be given up once the write is done,
   or a checkpoint waits for it forever.

   Four cases, forced, each in a process of its own:

   committed: T holds directory blocks D0 and D1, and disk cache block
      X, whose page-out T took into its shadow.  T commits; the flush
      after the commit writes X home, and that write is held, with T
      still the committing transaction, T_COMMITTED.  A page-out of the
      run D0-D1 then writes both home: D1 is claimed from the committing
      transaction.

   checkpoint: T holds D0, D1 and D2 and commits; nothing wrote them
      home, so their copies wait in the checkpoint list.  A page-out of
      the run D0-D2 writes all three, D1 and D2 claimed from the
      checkpoint list.  T2 then takes them to version 2.

   stop: D0's page is dirty and no transaction holds it (its transaction
      went home from the log, and the kernel has not written the page
      out yet); D1 is held by the running transaction, whose handle has
      ended.  A page-out of the run D0-D1 writes D0, and that write is
      held while the journal stops: the stop commits D1 and writes it
      home.  The run then finds the journal gone, and writes D1 home as
      without a journal (journal_store_write).

   fail: T holds D0, D1 and D2 and commits, none of them home; then the
      running transaction T2 holds D1 at version 2.  A page-out of the
      run D0-D2 fails at D0's write, so the pager loses all three pages.
      D1 must still go into T2's shadow, and D0 and D2 be read back from
      T's copies: every page-in after the failure gets what was lost.

   Each time: one write for the run where it can be one, no claim left
   once the pagers are idle, nothing left for a checkpoint to write, the
   last version of every block home after the unmount, and every crash
   image holding at least the version whose commit had returned.  */

#include <pthread.h>
#include <sys/wait.h>
#include <unistd.h>

#include "common.h"

enum { COMMITTED, CHECKPOINT, STOP, FAIL, NMODES };
static const char *const mode_names[NMODES] = { "committed", "checkpoint",
						"stop", "fail" };
static int mode;

#define NRUN 3
static block_t d[NRUN], x_blk;

/* Crash images from log entry AT on must hold at least version V of the
   blocks of the run in MASK; RUN_NEED blocks of it are checked.  */
static struct
{
  size_t at;
  long v;
  unsigned mask;
} need[3];
static int nneed, run_need;

static void
add_need (long v, unsigned mask)
{
  need[nneed].at = jt_log_count ();
  need[nneed].v = v;
  need[nneed++].mask = mask;
}

/* The held write: block HOLD_BLK at version 1.  */
static block_t hold_blk;
static pthread_mutex_t hold_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t hold_cond = PTHREAD_COND_INITIALIZER;
static int hold_armed, hold_reached, hold_released;

/* What the store saw: the longest write starting at D0, and whether a
   pager thread wrote D1 alone.  */
static size_t run_write_len;
static int d1_by_pager;

static void
store_hook (uint64_t off, size_t len, const void *data)
{
  if (off == (uint64_t) d[0] << log2_block_size)
    {
      pthread_mutex_lock (&hold_lock);
      if (len > run_write_len)
	run_write_len = len;
      pthread_mutex_unlock (&hold_lock);
    }
  if (off == (uint64_t) d[1] << log2_block_size && jt_on_pager_thread ())
    __atomic_store_n (&d1_by_pager, 1, __ATOMIC_RELEASE);

  if (off != (uint64_t) hold_blk << log2_block_size
      || pattern_version (data, hold_blk) != 1)
    return;
  pthread_mutex_lock (&hold_lock);
  if (hold_armed)
    {
      hold_armed = 0;
      hold_reached = 1;
      pthread_cond_broadcast (&hold_cond);
      while (!hold_released)
	pthread_cond_wait (&hold_cond, &hold_lock);
    }
  pthread_mutex_unlock (&hold_lock);
}

static void
wait_hold (void)
{
  struct timespec until;

  clock_gettime (CLOCK_REALTIME, &until);
  until.tv_sec += 30;
  pthread_mutex_lock (&hold_lock);
  while (!hold_reached
	 && pthread_cond_timedwait (&hold_cond, &hold_lock, &until) == 0)
    ;
  int reached = hold_reached;
  pthread_mutex_unlock (&hold_lock);
  CHECK (reached, "the write of block %u was never made", hold_blk);
}

static void
release_hold (void)
{
  pthread_mutex_lock (&hold_lock);
  hold_released = 1;
  pthread_cond_broadcast (&hold_cond);
  pthread_mutex_unlock (&hold_lock);
}

/* Version V of directory block B, reserved and marked in TXN, as dir.c
   edits one.  */
static void
dir_version (diskfs_transaction_t *txn, block_t b, uint32_t v)
{
  char buf[block_size];

  CHECK (!journal_get_write_access_external (txn, b), "reserve %u", b);
  pattern (buf, b, v);
  jt_dir_edit (b, 0, buf, block_size);
  CHECK (!jt_dir_mark_dirty (txn, b), "mark %u", b);
}

/* NRUN contiguous free blocks.  */
static void
pick_free_run (block_t *out)
{
  block_t b = jt_nblocks () / 2;

  for (int n = 0; n < NRUN; b++)
    {
      CHECK (b < jt_nblocks (), "no %d free blocks in a row", NRUN);
      n = jt_block_free (b) ? n + 1 : 0;
      if (n == NRUN)
	for (int i = 0; i < NRUN; i++)
	  out[i] = b - (NRUN - 1) + i;
    }
}

static const char *
check (size_t n, int torn, const char *img, void *arg)
{
  static char msg[160];
  char buf[block_size];
  FILE *f = fopen (img, "r");

  (void) torn;
  (void) arg;
  CHECK (f, "open %s", img);
  for (int i = 0; i < run_need; i++)
    {
      long want = 0;
      for (int k = 0; k < nneed; k++)
	if (n >= need[k].at && need[k].mask & (1u << i) && need[k].v > want)
	  want = need[k].v;
      if (!want)
	continue;
      CHECK (!fseeko (f, (off_t) d[i] << log2_block_size, SEEK_SET)
	     && fread (buf, block_size, 1, f) == 1, "read");
      long v = pattern_version (buf, d[i]);
      if (v < want)
	{
	  fclose (f);
	  snprintf (msg, sizeof msg, "block %u holds version %ld, but the "
		    "commit of version %ld had returned", d[i], v, want);
	  return msg;
	}
    }
  fclose (f);
  return NULL;
}

/* committed: T, on a thread of its own, as handles are per thread.  */
static void *
committer (void *arg)
{
  (void) arg;
  diskfs_transaction_t *txn = diskfs_journal_start_transaction ();
  dir_version (txn, d[0], 1);
  dir_version (txn, d[1], 1);
  journal_version (txn, x_blk, 1);
  /* Into T's shadow: the flush after the commit writes it home.  */
  jt_pageout (x_blk, 1, 1);
  CHECK (!diskfs_journal_commit_transaction (txn), "T's commit");
  return NULL;
}

static void
run_committed (void)
{
  pthread_t c;

  run_need = 2;
  hold_blk = x_blk;
  hold_armed = 1;
  pthread_create (&c, NULL, committer, NULL);
  wait_hold ();

  /* T is T_COMMITTED, its flush held at X.  */
  jt_dir_pageout_run (d[0], 2, 1, 1);
  CHECK (run_write_len == 2 * block_size, "the run went home in pieces "
	 "(the longest write at %u: %zu bytes)", d[0], run_write_len);

  release_hold ();
  pthread_join (c, NULL);
  add_need (1, 3);
}

static void
run_checkpoint (void)
{
  diskfs_transaction_t *txn;

  run_need = 3;
  txn = diskfs_journal_start_transaction ();
  for (int i = 0; i < 3; i++)
    dir_version (txn, d[i], 1);
  CHECK (!diskfs_journal_commit_transaction (txn), "T's commit");
  add_need (1, 7);
  for (int i = 0; i < 3; i++)
    CHECK (jt_checkpoint_unwritten (d[i]), "block %u is home already",
	   d[i]);

  jt_dir_pageout_run (d[0], 3, 1, 1);
  CHECK (run_write_len == 3 * block_size, "the run went home in pieces "
	 "(the longest write at %u: %zu bytes)", d[0], run_write_len);
  for (int i = 0; i < 3; i++)
    CHECK (!jt_checkpoint_unwritten (d[i]), "block %u was written home, "
	   "but its checkpoint copy is not marked written", d[i]);

  txn = diskfs_journal_start_transaction ();
  for (int i = 0; i < 3; i++)
    dir_version (txn, d[i], 2);
  CHECK (!diskfs_journal_commit_transaction (txn), "T2's commit");
  add_need (2, 7);
}

static int fail_armed;

static int
fail_d0 (uint64_t off, size_t len, const void *buf)
{
  (void) len;
  (void) buf;
  if (off == (uint64_t) d[0] << log2_block_size
      && __atomic_exchange_n (&fail_armed, 0, __ATOMIC_ACQ_REL))
    return EIO;
  return 0;
}

static void
run_fail (void)
{
  diskfs_transaction_t *txn;
  char buf[block_size];
  static const long want[3] = { 1, 2, 1 };

  run_need = 3;
  jt_expected_warning = "page-out of blocks";
  txn = diskfs_journal_start_transaction ();
  for (int i = 0; i < 3; i++)
    dir_version (txn, d[i], 1);
  CHECK (!diskfs_journal_commit_transaction (txn), "T's commit");
  add_need (1, 7);
  for (int i = 0; i < 3; i++)
    CHECK (jt_checkpoint_unwritten (d[i]), "block %u is home already",
	   d[i]);
  txn = diskfs_journal_start_transaction ();
  dir_version (txn, d[1], 2);
  diskfs_journal_stop_transaction (txn);

  __atomic_store_n (&fail_armed, 1, __ATOMIC_RELEASE);
  JT_SET_HOOK (jt_store_fail_hook, fail_d0);
  jt_dir_pageout_run (d[0], 3, 1, 1);
  JT_SET_HOOK (jt_store_fail_hook, NULL);
  CHECK (!__atomic_load_n (&fail_armed, __ATOMIC_ACQUIRE),
	 "the write of block %u never failed", d[0]);

  for (int i = 0; i < 3; i++)
    {
      jt_dir_read (d[i], buf);
      CHECK (pattern_version (buf, d[i]) == want[i], "block %u read back "
	     "as version %ld after the failed page-out, %ld lost",
	     d[i], pattern_version (buf, d[i]), want[i]);
    }
  CHECK (jt_stale_reads () == 0, "%lu stale page-ins after the failed "
	 "page-out", jt_stale_reads ());

  txn = diskfs_journal_start_transaction ();
  CHECK (!diskfs_journal_commit_transaction (txn), "T2's commit");
  add_need (2, 2);
  txn = diskfs_journal_start_transaction ();
  for (int i = 0; i < 3; i++)
    dir_version (txn, d[i], 3);
  CHECK (!diskfs_journal_commit_transaction (txn), "T3's commit");
  add_need (3, 7);
}

static void *
stopper (void *arg)
{
  (void) arg;
  diskfs_journal_shutdown ();
  return NULL;
}

static void
run_stop (void)
{
  char buf[block_size];
  pthread_t s;

  run_need = 2;
  /* D0: dirty, in no transaction.  */
  pattern (buf, d[0], 1);
  jt_dir_edit (d[0], 0, buf, block_size);
  /* D1: in the running transaction, not committed.  */
  diskfs_transaction_t *txn = diskfs_journal_start_transaction ();
  dir_version (txn, d[1], 1);
  diskfs_journal_stop_transaction (txn);

  hold_blk = d[0];
  hold_armed = 1;
  jt_dir_pageout_run (d[0], 2, 1, 0);
  wait_hold ();

  pthread_create (&s, NULL, stopper, NULL);
  for (int i = 0; i < 3000 && __atomic_load_n (&ext2_journal,
					       __ATOMIC_ACQUIRE); i++)
    usleep (10000);
  CHECK (!__atomic_load_n (&ext2_journal, __ATOMIC_ACQUIRE),
	 "the journal did not stop while a page-out wrote a block no "
	 "transaction holds");
  /* The stop committed D1.  D0 has no journal to keep it whole: a crash
     before a flush can tear or lose its write, as any written-back
     block's.  It is checked at home after the unmount.  */
  add_need (1, 2);

  release_hold ();
  jt_drain ();
  pthread_join (s, NULL);
  CHECK (run_write_len == block_size, "D0 went home with D1 (%zu bytes)",
	 run_write_len);
  CHECK (__atomic_load_n (&d1_by_pager, __ATOMIC_ACQUIRE),
	 "the page-out did not write D1 home once the journal stopped");
}

static int
run (const char *image)
{
  block_t picked[1];

  jt_mount (image, 2, 0);
  pick_free_run (d);
  /* Not next to the run.  */
  pick_free_blocks (picked, 1);
  x_blk = picked[0] < d[0] || picked[0] > d[NRUN - 1] ? picked[0]
    : d[NRUN - 1] + 2;
  CHECK (jt_block_free (x_blk), "block %u is not free", x_blk);
  JT_SET_HOOK (jt_store_write_hook, store_hook);

  if (mode == COMMITTED)
    run_committed ();
  else if (mode == CHECKPOINT)
    run_checkpoint ();
  else if (mode == STOP)
    run_stop ();
  else
    run_fail ();

  jt_drain ();
  long claims = jt_flush_claims ();
  CHECK (claims <= 0, "%ld claims left with the pagers idle", claims);

  unsigned long warnings = jt_umount ();
  JT_SET_HOOK (jt_store_write_hook, NULL);
  CHECK (warnings == 0, "%lu journal warnings", warnings);
  CHECK (jt_stale_reads () == 0, "%lu stale page-ins", jt_stale_reads ());
  long last = need[nneed - 1].v;
  for (int i = 0; i < run_need; i++)
    CHECK (pattern_version (jt_disk_block (d[i]), d[i]) == last,
	   "block %u: version %ld home, %ld expected", d[i],
	   pattern_version (jt_disk_block (d[i]), d[i]), last);

  char name[64];
  snprintf (name, sizeof name, "t_run_claims-%s", mode_names[mode]);
  size_t checked = jt_crash_check (name, check, NULL);
  printf ("t_run_claims: OK (%s: %zu writes, %zu crash images)\n",
	  mode_names[mode], jt_log_count (), checked);
  return 0;
}

int
main (int argc, char **argv)
{
  const char *image = argc > 1 ? argv[1] : "build/base.img";
  int status;

  for (int m = 0; m < NMODES; m++)
    {
      if (argc > 3 && strcmp (argv[3], mode_names[m]))
	continue;
      fflush (NULL);
      pid_t pid = fork ();
      CHECK (pid >= 0, "fork");
      if (pid == 0)
	{
	  mode = m;
	  exit (run (image));
	}
      CHECK (waitpid (pid, &status, 0) == pid, "wait");
      if (!WIFEXITED (status) || WEXITSTATUS (status))
	return 1;
    }
  return 0;
}
