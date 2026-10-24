/* A pager home write that races a reservation in a draining transaction.

   A pager write passes the hazard check and drops the journal lock for
   the I/O.  Meanwhile a thread with a handle on the transaction a commit
   is draining (T_LOCKED) reserves the block.  The home write held data
   from before the reservation, so its notification must not mark the
   new buffer written; journal_mark_dirty_external asserts that it is
   not.

   The steps are forced, not left to chance:
   1. Directory block B is committed in T0, so that a pager write of it
      goes home instead of being intercepted.
   2. Thread A opens a handle on T and journals a disk-cache block.
   3. Thread C commits; T drains (T_LOCKED), waiting for A.
   4. The file pager evicts B; the write is held in store_write, after
      the hazard check.
   5. A reserves B in T.
   6. The write is let go, and its notification comes in.  A edits B,
      which faults it back in: the edit waits for the write.
   7. A hands the journal B's image, and stops; the commit finishes.
   Then the edit must be home after a clean unmount, and in every crash
   image after the commit returned.  */

#include <pthread.h>
#include "common.h"

static block_t meta, dir;
static diskfs_transaction_t *txn_t;

/* The held write.  */
static pthread_mutex_t hold_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t hold_cond = PTHREAD_COND_INITIALIZER;
static int hold_armed, hold_reached, hold_released;

static void
hold_hook (uint64_t off, size_t len, const void *data)
{
  uint64_t start = (uint64_t) dir << log2_block_size;

  (void) data;
  if (off + len <= start || off >= start + block_size)
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

static size_t commit_returned_at;

static void *
committer (void *arg)
{
  (void) arg;
  diskfs_transaction_t *txn = diskfs_journal_start_transaction ();
  CHECK (txn == txn_t, "the committer joined another transaction");
  diskfs_journal_commit_transaction (txn);
  commit_returned_at = jt_log_count ();
  return NULL;
}

static void
dir_version (diskfs_transaction_t *txn, uint32_t v)
{
  char buf[block_size];

  CHECK (!journal_get_write_access_external (txn, dir), "reserve");
  pattern (buf, dir, v);
  jt_dir_edit (dir, 0, buf, block_size);
  CHECK (!jt_dir_mark_dirty (txn, dir), "mark");
}

static const char *
check (size_t n, int torn, const char *img, void *arg)
{
  static char msg[128];
  FILE *f = fopen (img, "r");
  char *buf = malloc (block_size);
  long vd, vm;

  (void) torn;
  (void) arg;
  CHECK (f, "open %s", img);
  CHECK (!fseeko (f, (off_t) dir << log2_block_size, SEEK_SET)
	 && fread (buf, block_size, 1, f) == 1, "read");
  vd = pattern_version (buf, dir);
  CHECK (!fseeko (f, (off_t) meta << log2_block_size, SEEK_SET)
	 && fread (buf, block_size, 1, f) == 1, "read");
  vm = pattern_version (buf, meta);
  fclose (f);
  free (buf);

  /* T0 wrote dir v1; T wrote meta v1 and dir v2, together.  */
  if (!((vm == 0 && (vd == 0 || vd == 1)) || (vm == 1 && vd == 2)))
    snprintf (msg, sizeof msg, "meta v%ld with dir v%ld", vm, vd);
  else if (commit_returned_at && n >= commit_returned_at && vd != 2)
    snprintf (msg, sizeof msg, "dir v%ld after T's commit returned", vd);
  else
    return NULL;
  return msg;
}

int
main (int argc, char **argv)
{
  const char *image = argc > 1 ? argv[1] : "build/base.img";
  block_t picked[2];
  pthread_t c;
  diskfs_transaction_t *txn;
  unsigned long warnings;

  jt_mount (image, 2, 0);
  pick_free_blocks (picked, 2);
  meta = picked[0];
  dir = picked[1];
  JT_SET_HOOK (jt_store_write_hook, hold_hook);

  /* 1.  B in a committed transaction, its page dirty.  */
  txn = diskfs_journal_start_transaction ();
  dir_version (txn, 1);
  diskfs_journal_commit_transaction (txn);

  /* 2.  A's handle on T.  */
  txn_t = diskfs_journal_start_transaction ();
  journal_version (txn_t, meta, 1);

  /* 3.  T drains for C's commit.  */
  pthread_create (&c, NULL, committer, NULL);
  while (!jt_txn_locked (txn_t))
    sched_yield ();

  /* 4.  The file pager writes B home, and is held there.  */
  pthread_mutex_lock (&hold_lock);
  hold_armed = 1;
  pthread_mutex_unlock (&hold_lock);
  jt_dir_pageout (dir, 0, 0);
  pthread_mutex_lock (&hold_lock);
  while (!hold_reached)
    pthread_cond_wait (&hold_cond, &hold_lock);
  pthread_mutex_unlock (&hold_lock);

  /* 5.  A reserves B in the draining T.  */
  CHECK (jt_txn_locked (txn_t), "T is no longer draining");
  char buf[block_size];
  CHECK (!journal_get_write_access_external (txn_t, dir), "reserve in T");

  /* 6.  The held write finishes, and the journal is told of it; A's
     edit.  */
  pthread_mutex_lock (&hold_lock);
  hold_released = 1;
  pthread_cond_broadcast (&hold_cond);
  pthread_mutex_unlock (&hold_lock);
  pattern (buf, dir, 2);
  jt_dir_edit (dir, 0, buf, block_size);
  jt_drain ();
  JT_SET_HOOK (jt_store_write_hook, NULL);

  /* 7.  The image, and the end of A's handle; C's commit goes on.  */
  CHECK (!jt_dir_mark_dirty (txn_t, dir), "mark in T");
  diskfs_journal_stop_transaction (txn_t);
  pthread_join (c, NULL);

  warnings = jt_umount ();
  CHECK (warnings == 0, "%lu journal warnings", warnings);
  CHECK (jt_stale_reads () == 0, "%lu stale page-ins", jt_stale_reads ());
  CHECK (pattern_version (jt_disk_block (dir), dir) == 2,
	 "dir block: version %ld home, 2 expected",
	 pattern_version (jt_disk_block (dir), dir));
  CHECK (pattern_version (jt_disk_block (meta), meta) == 1,
	 "meta block: version %ld home, 1 expected",
	 pattern_version (jt_disk_block (meta), meta));

  size_t checked = jt_crash_check ("t_notify_race", check, NULL);
  printf ("t_notify_race: OK (%zu writes, %zu crash images)\n",
	  jt_log_count (), checked);
  return 0;
}
