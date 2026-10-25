/* A rename of a directory onto a name its source's parent is below,
   against a lookup inside that name, in libdiskfs's diskfs_rename_dir.

   diskfs_rename_dir locks the source's parent FDP and the target
   directory TDP, then looks the target name up in TDP with both held,
   which locks the node it finds.  If that node is a directory above FDP,
   it is locked after its descendant FDP, while a lookup locks a directory
   before its children.

   The steps are forced:
   1. Directories: A in the root, B in A, C in B.
   2. Y locks A, as an RPC on A does before a lookup in it.
   3. X renames C (in B) to the root's name A: diskfs_S_dir_rename,
      which locks B and the root, and looks A up, waiting for A.
   4. Y looks B up in A, which locks B.
   Both must finish; the rename fails, as A is not empty.  */

#include <unistd.h>
#include "files.h"
#include "fs_S.h"

static struct node *a, *b, *c;
static struct peropen po_root, po_b;
static struct protid cred_root, cred_b;
static int x_done, y_done;
static error_t x_err;

static struct node *
mkdir_in (struct node *dp, struct protid *cr, const char *name)
{
  struct node *np;
  struct dirstat *ds = alloca (diskfs_dirstat_size);
  diskfs_transaction_t *txn = diskfs_journal_start_transaction ();
  pthread_mutex_lock (&dp->lock);
  CHECK (diskfs_lookup (dp, (char *) name, CREATE, &np, ds, cr) == ENOENT,
	 "lookup %s", name);
  CHECK (!diskfs_create_node (dp, (char *) name, S_IFDIR | 0755, &np, cr, ds),
	 "mkdir %s", name);
  pthread_mutex_unlock (&dp->lock);
  pthread_mutex_unlock (&np->lock);
  diskfs_journal_stop_transaction (txn);
  return np;
}

static void *
renamer (void *arg)
{
  (void) arg;
  x_err = diskfs_S_dir_rename (&cred_b, "c", &cred_root, "A", 0);
  __atomic_store_n (&x_done, 1, __ATOMIC_RELEASE);
  return NULL;
}

static void *
looker (void *arg)
{
  struct node *np;
  struct protid *cr = arg;
  diskfs_transaction_t *txn = diskfs_journal_start_transaction ();
  pthread_mutex_lock (&a->lock);
  /* 3. happens meanwhile.  */
  usleep (300000);
  error_t err = diskfs_lookup (a, "b", LOOKUP, &np, NULL, cr);
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
  static struct peropen po_a;
  static struct protid cred_a;
  pthread_t x, y;

  jt_mount (image, 2, 0);
  jt_inode_globals ();
  files_init ();
  diskfs_root_node = root;
  cred_root = cred;
  cred_root.po = &po_root;
  po_root.np = root;

  /* 1.  */
  a = mkdir_in (root, &cred_root, "A");
  cred_a = cred;
  cred_a.po = &po_a;
  po_a.np = a;
  b = mkdir_in (a, &cred_a, "b");
  cred_b = cred;
  cred_b.po = &po_b;
  po_b.np = b;
  c = mkdir_in (b, &cred_b, "c");

  /* 2. and 4.  */
  pthread_create (&y, NULL, looker, &cred_a);
  usleep (50000);
  /* 3.  */
  pthread_create (&x, NULL, renamer, NULL);

  for (int i = 0; i < 300 && !(__atomic_load_n (&x_done, __ATOMIC_ACQUIRE)
			       && __atomic_load_n (&y_done, __ATOMIC_ACQUIRE));
       i++)
    usleep (10000);
  if (!x_done || !y_done)
    {
      fprintf (stderr, "t_fdir_renlock: DEADLOCK: the rename %s, the lookup "
	       "in A %s\n", x_done ? "finished" : "did not finish",
	       y_done ? "finished" : "did not finish");
      if (getenv ("JT_WATCHDOG_GDB"))
	{
	  alarm (1);
	  pause ();
	}
      _exit (1);
    }
  pthread_join (x, NULL);
  pthread_join (y, NULL);
  diskfs_nrele (a);
  diskfs_nrele (b);
  diskfs_nrele (c);
  unsigned long warnings = jt_umount ();
  printf ("t_fdir_renlock: OK (the rename: %s; %lu warnings)\n",
	  x_err ? strerror (x_err) : "done", warnings);
  return 0;
}
