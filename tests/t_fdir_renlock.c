/* A rename of a directory against a lookup, in the order of their locks
   in libdiskfs's diskfs_rename_dir.

   A lookup locks a directory before the child it finds, so a rename must
   take its locks from the top down too.  In each case X renames a
   directory in A or below, and Y holds A, as an RPC on A does before a
   lookup in it, then looks up a name in A:

   (default)  X renames A/b/c onto the root's name A (an ancestor of its
	      parent B): the target A must be locked before B, and the
	      rename fails, as A is not empty.  Y looks up b in A.
   up	      X moves A/b/c up to A/z: A must be locked before B, and the
	      move succeeds.  Y looks up b in A.
   file	      X renames A/c onto the root's name t, a file that is also
	      A's g: the file must not be locked before A, and the rename
	      fails, as a directory does not replace a file.  Y looks up g
	      in A, which locks the file.

   The case is the third argument.  diskfs_rename_dir finds the
   directories above the source's parent before it takes its locks;
   Y takes A once X has (linked with --wrap=diskfs_journal_start_transaction:
   the rename starts its transaction after that), and holds it while X
   takes its locks.  Both must finish, with the rename's result.  */

#include <unistd.h>
#include "files.h"
#include "fs_S.h"

enum { ANCESTOR, UP, FILE_TARGET } mode;

static struct node *a, *b, *c, *f;
static struct peropen po_root, po_a, po_b;
static struct protid cred_root, cred_a, cred_b;
static int x_done, y_done;
static error_t x_err;

/* X's thread, and how far it got; Y's hold of A.  */
static pthread_t x_thread;
static int x_starts;
static pthread_mutex_t step_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t step_cond = PTHREAD_COND_INITIALIZER;
static int y_go, y_holds;

diskfs_transaction_t *__real_diskfs_journal_start_transaction (void);

diskfs_transaction_t *
__wrap_diskfs_journal_start_transaction (void)
{
  /* X's first is diskfs_S_dir_rename's; its second, diskfs_rename_dir's,
     once the directories above the source's parent are known.  */
  if (pthread_equal (pthread_self (), x_thread) && ++x_starts == 2)
    {
      struct timespec until;
      clock_gettime (CLOCK_REALTIME, &until);
      until.tv_sec += 2;
      pthread_mutex_lock (&step_lock);
      y_go = 1;
      pthread_cond_broadcast (&step_cond);
      while (!y_holds
	     && pthread_cond_timedwait (&step_cond, &step_lock, &until) == 0)
	;
      pthread_mutex_unlock (&step_lock);
    }
  return __real_diskfs_journal_start_transaction ();
}

static struct node *
make_in (struct node *dp, struct protid *cr, const char *name, mode_t m)
{
  struct node *np;
  struct dirstat *ds = alloca (diskfs_dirstat_size);
  diskfs_transaction_t *txn = diskfs_journal_start_transaction ();
  pthread_mutex_lock (&dp->lock);
  CHECK (diskfs_lookup (dp, (char *) name, CREATE, &np, ds, cr) == ENOENT,
	 "lookup %s", name);
  CHECK (!diskfs_create_node (dp, (char *) name, m, &np, cr, ds),
	 "create %s", name);
  pthread_mutex_unlock (&dp->lock);
  pthread_mutex_unlock (&np->lock);
  diskfs_journal_stop_transaction (txn);
  return np;
}

/* A second name NAME in DP for NP, as dir_link makes it.  */
static void
link_in (struct node *dp, struct protid *cr, const char *name,
	 struct node *np)
{
  struct node *tnp;
  struct dirstat *ds = alloca (diskfs_dirstat_size);
  diskfs_transaction_t *txn = diskfs_journal_start_transaction ();
  pthread_mutex_lock (&dp->lock);
  CHECK (diskfs_lookup (dp, (char *) name, CREATE, &tnp, ds, cr) == ENOENT,
	 "lookup %s", name);
  pthread_mutex_lock (&np->lock);
  np->dn_stat.st_nlink++;
  np->dn_set_ctime = 1;
  diskfs_node_update (np, 0);
  CHECK (!diskfs_direnter (dp, name, np, ds, cr), "link %s", name);
  pthread_mutex_unlock (&np->lock);
  pthread_mutex_unlock (&dp->lock);
  diskfs_journal_stop_transaction (txn);
}

static void *
renamer (void *arg)
{
  (void) arg;
  switch (mode)
    {
    case ANCESTOR:
      x_err = diskfs_S_dir_rename (&cred_b, "c", &cred_root, "A", 0);
      break;
    case UP:
      x_err = diskfs_S_dir_rename (&cred_b, "c", &cred_a, "z", 0);
      break;
    case FILE_TARGET:
      x_err = diskfs_S_dir_rename (&cred_a, "c", &cred_root, "t", 0);
      break;
    }
  __atomic_store_n (&x_done, 1, __ATOMIC_RELEASE);
  return NULL;
}

static void *
looker (void *arg)
{
  struct node *np;
  (void) arg;

  pthread_mutex_lock (&step_lock);
  while (!y_go)
    pthread_cond_wait (&step_cond, &step_lock);
  pthread_mutex_unlock (&step_lock);

  diskfs_transaction_t *txn = diskfs_journal_start_transaction ();
  pthread_mutex_lock (&a->lock);
  pthread_mutex_lock (&step_lock);
  y_holds = 1;
  pthread_cond_broadcast (&step_cond);
  pthread_mutex_unlock (&step_lock);
  /* X takes its locks meanwhile.  */
  usleep (300000);
  error_t err = diskfs_lookup (a, mode == FILE_TARGET ? "g" : "b", LOOKUP,
			       &np, NULL, &cred_a);
  if (!err)
    diskfs_nput (np);
  pthread_mutex_unlock (&a->lock);
  diskfs_journal_stop_transaction (txn);
  __atomic_store_n (&y_done, 1, __ATOMIC_RELEASE);
  return NULL;
}

int
main (int argc, char **argv)
{
  const char *image = argc > 1 ? argv[1] : "build/balloc.img";
  const char *name = argc > 3 ? argv[3] : "ancestor";
  pthread_t y;

  mode = !strcmp (name, "up") ? UP
    : !strcmp (name, "file") ? FILE_TARGET : ANCESTOR;
  jt_mount (image, 2, 0);
  jt_inode_globals ();
  files_init ();
  diskfs_root_node = root;
  cred_root = cred;
  cred_root.po = &po_root;
  po_root.np = root;

  a = make_in (root, &cred_root, "A", S_IFDIR | 0755);
  cred_a = cred;
  cred_a.po = &po_a;
  po_a.np = a;
  if (mode == FILE_TARGET)
    {
      c = make_in (a, &cred_a, "c", S_IFDIR | 0755);
      f = make_in (a, &cred_a, "g", S_IFREG | 0644);
      link_in (root, &cred_root, "t", f);
    }
  else
    {
      b = make_in (a, &cred_a, "b", S_IFDIR | 0755);
      cred_b = cred;
      cred_b.po = &po_b;
      po_b.np = b;
      c = make_in (b, &cred_b, "c", S_IFDIR | 0755);
    }

  pthread_create (&y, NULL, looker, NULL);
  pthread_create (&x_thread, NULL, renamer, NULL);

  for (int i = 0; i < 300 && !(__atomic_load_n (&x_done, __ATOMIC_ACQUIRE)
			       && __atomic_load_n (&y_done, __ATOMIC_ACQUIRE));
       i++)
    usleep (10000);
  if (!x_done || !y_done)
    {
      fprintf (stderr, "t_fdir_renlock: DEADLOCK (%s): the rename %s, the "
	       "lookup in A %s\n", name,
	       x_done ? "finished" : "did not finish",
	       y_done ? "finished" : "did not finish");
      if (getenv ("JT_WATCHDOG_GDB"))
	{
	  alarm (1);
	  pause ();
	}
      _exit (1);
    }
  pthread_join (x_thread, NULL);
  pthread_join (y, NULL);
  CHECK (y_holds, "%s: Y never held A: the rename did not get to its "
	 "transaction", name);
  error_t want = mode == UP ? 0 : mode == FILE_TARGET ? ENOTDIR : ENOTEMPTY;
  CHECK (x_err == want, "%s: the rename gave %s, not %s", name,
	 x_err ? strerror (x_err) : "success",
	 want ? strerror (want) : "success");

  diskfs_transaction_t *txn = diskfs_journal_start_transaction ();
  diskfs_nrele (a);
  if (b)
    diskfs_nrele (b);
  diskfs_nrele (c);
  if (f)
    diskfs_nrele (f);
  diskfs_journal_stop_transaction (txn);
  unsigned long warnings = jt_umount ();
  CHECK (warnings == 0, "%lu journal warnings", warnings);
  const char *out = "build/t_fdir_renlock.img";
  jt_save_image (out);
  CHECK (jt_run ("e2fsck -fn %s >%s.fsck 2>&1", out, out) == 0,
	 "e2fsck -fn %s is not clean after the unmount (see %s.fsck)", out,
	 out);
  printf ("t_fdir_renlock: OK (%s: the rename: %s; %lu warnings)\n", name,
	  x_err ? strerror (x_err) : "done", warnings);
  return 0;
}
