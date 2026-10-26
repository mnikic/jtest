/* libdiskfs's node cache: two races of diskfs_cached_lookup_context.

   lastref: a lookup of a node while its last hard reference goes.

   diskfs_nput drops the last hard reference with the node locked, and
   _diskfs_lastref, for a node with no links, calls
   diskfs_try_dropping_softrefs, which takes nodecache_lock for writing
   to take the node out of the cache.  A lookup of the same inode takes
   nodecache_lock for reading, finds the node, and diskfs_nref, seeing
   the hard count go from 0 to 1, locks the node while it still holds
   nodecache_lock.  Each then waits for the other.  A lookup by inode
   number comes for an unlinked file from a directory entry read just
   before the unlink, or from fsys_getfile.

   The steps are forced, with diskfs_set_node_times wrapped (the first
   call of _diskfs_lastref):
   1. A file is created without a name, as dir_mkfile does: no links,
      one hard reference.
   2. X closes it (diskfs_nput), and is held in _diskfs_lastref: the
      node is locked, its hard count 0, and it is still in the cache.
   3. Y looks the inode up (diskfs_cached_lookup).
   4. X goes on.
   Both must finish.

   lostrace: two lookups of the same inode, not cached.  Both miss and
   make a node; the one that inserts second finds the other's, and drops
   its own with diskfs_nput while it holds nodecache_lock for writing.
   That node was never read: its dn_stat is what malloc gave.  The steps,
   with diskfs_make_node_alloc wrapped:
   1. A looks inode 13 (pool1, a file of the image) up, misses, makes
      its node, and is held.
   2. B looks it up, misses, makes, inserts and reads its node, and keeps
      it.
   3. A goes on: it finds B's node, and drops its own.
   Both must finish, and e2fsck must find the image clean, inode 13
   still in use.  With JT_ZERO_STAT=1, A's node's dn_stat is zeroed, as
   fresh memory is.

   readerr: a lookup whose read of the inode fails.  The node is in the
   cache by then, with the cache's light reference; the error path takes
   it out of the cache and releases the lookup's reference.  The page-in
   of the inode's block fails as on the Hurd (jt_ref_fail_hook):
   1. A lookup of inode 13 gets EIO.
   2. The node it made must be freed (diskfs_node_norefs is wrapped).
   3. A lookup of inode 13 reads it again, and finds it.

   readwait: a lookup that waits for a node whose read then fails.
   1. X's lookup of inode 13 is held in its read, its node in the cache
      already.
   2. Y looks inode 13 up, finds that node, and waits for its lock.
   3. X's read fails.
   Y must get inode 13 as it is on disk (it reads it again), or the
   error, not the node that was never read.

   The scenario is the third argument; by default all run, each in a
   process of its own.  */

#include <sys/wait.h>
#include <unistd.h>
#include "files.h"

static struct node *victim;
static ino_t inum;
static int armed, x_in, x_done, y_done;
static error_t y_err;

/* lostrace.  */
static ino_t lost_ino = 13;
static int mk_armed, a_made, b_done, a_done;
static error_t a_err;

/* readerr.  */
static int record_made;
static struct node *made, *watched;
static block_t fail_blk;
static unsigned long watched_freed;

void __real_diskfs_node_norefs (struct node *np);

void
__wrap_diskfs_node_norefs (struct node *np)
{
  if (np == __atomic_load_n (&watched, __ATOMIC_ACQUIRE))
    __atomic_add_fetch (&watched_freed, 1, __ATOMIC_RELAXED);
  __real_diskfs_node_norefs (np);
}

static int
fail_inode_block (block_t b)
{
  return b == fail_blk ? EIO : 0;
}

/* readwait: X's read is held, then fails once.  */
static int rw_held, rw_go;

static int
hold_then_fail (block_t b)
{
  if (b != fail_blk || __atomic_exchange_n (&rw_held, 1, __ATOMIC_ACQ_REL))
    return 0;
  while (!__atomic_load_n (&rw_go, __ATOMIC_ACQUIRE))
    usleep (1000);
  return EIO;
}

static error_t rw_x_err;

static void *
rw_lookup_x (void *arg)
{
  struct node *np;
  (void) arg;
  diskfs_transaction_t *txn = diskfs_journal_start_transaction ();
  rw_x_err = diskfs_cached_lookup (13, &np);
  if (!rw_x_err)
    diskfs_nput (np);
  diskfs_journal_stop_transaction (txn);
  return NULL;
}

static int y_found;
static mode_t y_mode;
static nlink_t y_nlink;
static error_t y_rw_err;

static void *
rw_lookup_y (void *arg)
{
  struct node *np;
  (void) arg;
  diskfs_transaction_t *txn = diskfs_journal_start_transaction ();
  y_rw_err = diskfs_cached_lookup (13, &np);
  if (!y_rw_err)
    {
      y_mode = np->dn_stat.st_mode;
      y_nlink = np->dn_stat.st_nlink;
      diskfs_nput (np);
    }
  diskfs_journal_stop_transaction (txn);
  __atomic_store_n (&y_found, 1, __ATOMIC_RELEASE);
  return NULL;
}

struct node *__real_diskfs_make_node_alloc (size_t size);

/* ext2fs's diskfs_user_make_node allocates the node with this, after the
   cache missed and before the node is locked and inserted.  */
struct node *
__wrap_diskfs_make_node_alloc (size_t size)
{
  struct node *np = __real_diskfs_make_node_alloc (size);
  if (np && __atomic_load_n (&record_made, __ATOMIC_ACQUIRE))
    {
      __atomic_store_n (&made, np, __ATOMIC_RELEASE);
      __atomic_store_n (&watched, np, __ATOMIC_RELEASE);
    }
  if (np && __atomic_exchange_n (&mk_armed, 0, __ATOMIC_ACQ_REL))
    {
      if (getenv ("JT_ZERO_STAT"))
	memset (&np->dn_stat, 0, sizeof np->dn_stat);
      __atomic_store_n (&a_made, 1, __ATOMIC_RELEASE);
      while (!__atomic_load_n (&b_done, __ATOMIC_ACQUIRE))
	usleep (1000);
    }
  return np;
}

static void *
lookup_a (void *arg)
{
  struct node *np;
  (void) arg;
  diskfs_transaction_t *txn = diskfs_journal_start_transaction ();
  a_err = diskfs_cached_lookup (lost_ino, &np);
  if (!a_err)
    diskfs_nput (np);
  diskfs_journal_stop_transaction (txn);
  __atomic_store_n (&a_done, 1, __ATOMIC_RELEASE);
  return NULL;
}

void __real_diskfs_set_node_times (struct node *np);

void
__wrap_diskfs_set_node_times (struct node *np)
{
  if (np == __atomic_load_n (&victim, __ATOMIC_ACQUIRE)
      && __atomic_exchange_n (&armed, 0, __ATOMIC_ACQ_REL))
    {
      __atomic_store_n (&x_in, 1, __ATOMIC_RELEASE);
      /* Y looks the inode up meanwhile.  */
      usleep (300000);
    }
  __real_diskfs_set_node_times (np);
}

static void *
closer (void *arg)
{
  struct node *np = arg;
  diskfs_transaction_t *txn = diskfs_journal_start_transaction ();
  pthread_mutex_lock (&np->lock);
  diskfs_nput (np);
  diskfs_journal_stop_transaction (txn);
  __atomic_store_n (&x_done, 1, __ATOMIC_RELEASE);
  return NULL;
}

static void *
looker (void *arg)
{
  struct node *np;
  (void) arg;
  diskfs_transaction_t *txn = diskfs_journal_start_transaction ();
  y_err = diskfs_cached_lookup (inum, &np);
  if (!y_err)
    diskfs_nput (np);
  diskfs_journal_stop_transaction (txn);
  __atomic_store_n (&y_done, 1, __ATOMIC_RELEASE);
  return NULL;
}

static void
stuck (const char *what)
{
  fprintf (stderr, "t_nodecache: DEADLOCK: %s\n", what);
  /* The watchdog's backtraces, if JT_WATCHDOG_GDB is set.  */
  if (getenv ("JT_WATCHDOG_GDB"))
    {
      alarm (1);
      pause ();
    }
  _exit (1);
}

static int
lostrace (void)
{
  struct node *np;
  pthread_t a;
  const char *out = "build/t_nodecache-lostrace.img";

  if (getenv ("JT_INUM"))
    lost_ino = atoi (getenv ("JT_INUM"));
  __atomic_store_n (&mk_armed, 1, __ATOMIC_RELEASE);
  pthread_create (&a, NULL, lookup_a, NULL);
  while (!__atomic_load_n (&a_made, __ATOMIC_ACQUIRE))
    usleep (1000);

  diskfs_transaction_t *txn = diskfs_journal_start_transaction ();
  CHECK (!diskfs_cached_lookup (lost_ino, &np), "B's lookup");
  pthread_mutex_unlock (&np->lock);
  diskfs_journal_stop_transaction (txn);
  CHECK (np->dn_stat.st_nlink > 0, "inode 12 has no links");
  __atomic_store_n (&b_done, 1, __ATOMIC_RELEASE);

  for (int i = 0; i < 300 && !__atomic_load_n (&a_done, __ATOMIC_ACQUIRE);
       i++)
    usleep (10000);
  if (!__atomic_load_n (&a_done, __ATOMIC_ACQUIRE))
    stuck ("the lookup that lost the race did not finish");
  pthread_join (a, NULL);

  txn = diskfs_journal_start_transaction ();
  pthread_mutex_lock (&np->lock);
  diskfs_nput (np);
  diskfs_journal_stop_transaction (txn);
  unsigned long warnings = jt_umount ();
  jt_save_image (out);
  CHECK (jt_run ("e2fsck -fn %s >%s.fsck 2>&1", out, out) == 0,
	 "e2fsck -fn %s is not clean (see %s.fsck)", out, out);
  printf ("t_nodecache: OK (lostrace%s: the losing lookup %s; %lu "
	  "warnings)\n", getenv ("JT_ZERO_STAT") ? ", zeroed stat" : "",
	  a_err ? strerror (a_err) : "got the node", warnings);
  return 0;
}

static int
readerr (void)
{
  struct node *np;
  ino_t ino = 13;

  /* 1.  */
  fail_blk = dino_block (ino);
  __atomic_store_n (&record_made, 1, __ATOMIC_RELEASE);
  JT_SET_HOOK (jt_ref_fail_hook, fail_inode_block);
  diskfs_transaction_t *txn = diskfs_journal_start_transaction ();
  error_t err = diskfs_cached_lookup (ino, &np);
  diskfs_journal_stop_transaction (txn);
  JT_SET_HOOK (jt_ref_fail_hook, NULL);
  __atomic_store_n (&record_made, 0, __ATOMIC_RELEASE);
  CHECK (err == EIO, "the lookup with the failed read: %s", strerror (err));
  struct node *mnp = __atomic_load_n (&made, __ATOMIC_ACQUIRE);
  CHECK (mnp, "no node was made");

  /* 2.  */
  unsigned long freed = __atomic_load_n (&watched_freed, __ATOMIC_ACQUIRE);
  struct references r = { 0, 0 };
  if (!freed)
    /* Not freed, so still there to look at.  */
    refcounts_references (&mnp->refcounts, &r);

  /* 3.  */
  txn = diskfs_journal_start_transaction ();
  CHECK (!diskfs_cached_lookup (ino, &np), "the lookup after the failure");
  diskfs_nput (np);
  diskfs_journal_stop_transaction (txn);

  unsigned long warnings = jt_umount ();
  CHECK (freed, "the node whose read failed was never freed: %u hard and "
	 "%u weak references left, and the cache does not have it",
	 (unsigned) r.hard, (unsigned) r.weak);
  printf ("t_nodecache: OK (readerr: the node was freed; %lu warnings)\n",
	  warnings);
  return 0;
}

static int
readwait (void)
{
  pthread_t x, y;

  /* 1.  */
  fail_blk = dino_block (13);
  JT_SET_HOOK (jt_ref_fail_hook, hold_then_fail);
  pthread_create (&x, NULL, rw_lookup_x, NULL);
  while (!__atomic_load_n (&rw_held, __ATOMIC_ACQUIRE))
    usleep (1000);

  /* 2.  */
  pthread_create (&y, NULL, rw_lookup_y, NULL);
  usleep (200000);
  CHECK (!__atomic_load_n (&y_found, __ATOMIC_ACQUIRE), "the second lookup "
	 "did not wait for the node being read");

  /* 3.  */
  __atomic_store_n (&rw_go, 1, __ATOMIC_RELEASE);
  for (int i = 0; i < 300 && !__atomic_load_n (&y_found, __ATOMIC_ACQUIRE);
       i++)
    usleep (10000);
  if (!__atomic_load_n (&y_found, __ATOMIC_ACQUIRE))
    stuck ("the second lookup did not finish");
  pthread_join (x, NULL);
  pthread_join (y, NULL);
  JT_SET_HOOK (jt_ref_fail_hook, NULL);
  CHECK (rw_x_err == EIO, "the lookup whose read failed: %s",
	 strerror (rw_x_err));
  CHECK (y_rw_err || (S_ISREG (y_mode) && (y_mode & 07777) == 0666
		      && y_nlink == 1),
	 "the second lookup got a node that was not read: mode %o, %u links",
	 (unsigned) y_mode, (unsigned) y_nlink);

  unsigned long warnings = jt_umount ();
  printf ("t_nodecache: OK (readwait: the second lookup %s; %lu "
	  "warnings)\n", y_rw_err ? strerror (y_rw_err)
	  : "read the inode again", warnings);
  return 0;
}

static int
lastref (void)
{
  struct node *np;
  pthread_t x, y;

  /* 1.  */
  diskfs_transaction_t *txn = diskfs_journal_start_transaction ();
  pthread_mutex_lock (&root->lock);
  CHECK (!diskfs_create_node (root, NULL, S_IFREG | 0644, &np, &cred, NULL),
	 "create");
  pthread_mutex_unlock (&root->lock);
  inum = np->cache_id;
  pthread_mutex_unlock (&np->lock);
  diskfs_journal_stop_transaction (txn);
  CHECK (np->dn_stat.st_nlink == 0, "the new file has links");

  /* 2.  */
  __atomic_store_n (&victim, np, __ATOMIC_RELEASE);
  __atomic_store_n (&armed, 1, __ATOMIC_RELEASE);
  pthread_create (&x, NULL, closer, np);
  while (!__atomic_load_n (&x_in, __ATOMIC_ACQUIRE))
    usleep (1000);

  /* 3., and 4. when the wrapper lets X go.  */
  pthread_create (&y, NULL, looker, NULL);
  for (int i = 0; i < 300 && !(__atomic_load_n (&x_done, __ATOMIC_ACQUIRE)
			       && __atomic_load_n (&y_done, __ATOMIC_ACQUIRE));
       i++)
    usleep (10000);
  if (!__atomic_load_n (&x_done, __ATOMIC_ACQUIRE)
      || !__atomic_load_n (&y_done, __ATOMIC_ACQUIRE))
    stuck (x_done ? "the lookup did not finish"
	   : y_done ? "the close did not finish"
	   : "neither the close nor the lookup finished");
  pthread_join (x, NULL);
  pthread_join (y, NULL);

  unsigned long warnings = jt_umount ();
  printf ("t_nodecache: OK (lastref: the lookup %s; %lu warnings)\n",
	  y_err ? strerror (y_err) : "found the node", warnings);
  return 0;
}

int
main (int argc, char **argv)
{
  const char *image = argc > 1 ? argv[1] : "build/balloc.img";
  /* TSan does not follow the longjmp of a failed page-in (its interceptor
     asserts), so readerr is left out under it.  */
  static const char *const all[] = { "lastref", "lostrace",
#ifndef __SANITIZE_THREAD__
				     "readerr", "readwait",
#endif
				     NULL };
  const char *one[] = { argc > 3 ? argv[3] : NULL, NULL };
  const char *const *modes = argc > 3 ? one : all;
  int status, failed = 0;

  for (; *modes; modes++)
    {
      fflush (NULL);
      pid_t pid = fork ();
      CHECK (pid >= 0, "fork");
      if (pid == 0)
	{
	  jt_mount (image, 2, 0);
	  jt_inode_globals ();
	  files_init ();
	  exit (!strcmp (*modes, "lastref") ? lastref ()
		: !strcmp (*modes, "lostrace") ? lostrace ()
		: !strcmp (*modes, "readerr") ? readerr () : readwait ());
	}
      CHECK (waitpid (pid, &status, 0) == pid, "wait");
      if (!WIFEXITED (status) || WEXITSTATUS (status))
	{
	  fprintf (stderr, "t_nodecache: %s failed\n", *modes);
	  failed = 1;
	}
    }
  return failed;
}
