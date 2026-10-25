/* Entries in a directory that was removed, in libdiskfs's direnter and
   diskfs_rename_dir.

   A rename onto an empty directory (as rmdir does) clears it and drops
   its last link, but a port or working directory on it keeps the node.
   An entry made in it afterwards is in a directory nothing reaches: when
   the node goes, the file entered is left allocated with no name, which
   e2fsck reports as an unattached inode.

   The steps:
   1. Directories A and B in the root, a file f in the root.
   2. The test keeps A, as a port on it does, and renames B onto A, which
      removes A.
   3. In A: a create of g, a rename of f to it, a rename of the root's
      directory C to it.  Each must fail with ENOENT.
   After the unmount, e2fsck -fn is clean.

   With "lax" as the third argument, the results of 3. are printed, not
   checked, to see what e2fsck finds without the fix.  */

#include "files.h"
#include "fs_S.h"

static struct peropen po_root, po_a;
static struct protid cred_root, cred_a;

static struct node *
make_in (struct node *dp, struct protid *cr, const char *name, mode_t mode)
{
  struct node *np;
  struct dirstat *ds = alloca (diskfs_dirstat_size);
  diskfs_transaction_t *txn = diskfs_journal_start_transaction ();
  pthread_mutex_lock (&dp->lock);
  CHECK (diskfs_lookup (dp, (char *) name, CREATE, &np, ds, cr) == ENOENT,
	 "lookup %s", name);
  CHECK (!diskfs_create_node (dp, (char *) name, mode, &np, cr, ds),
	 "create %s", name);
  pthread_mutex_unlock (&dp->lock);
  pthread_mutex_unlock (&np->lock);
  CHECK (!jt_commit (txn), "commit %s", name);
  return np;
}

int
main (int argc, char **argv)
{
  const char *image = argc > 1 ? argv[1] : "build/balloc.img";
  struct node *a, *b, *c, *f, *np;
  struct dirstat *ds = alloca (diskfs_dirstat_size);
  error_t err;

  int lax = argc > 3 && !strcmp (argv[3], "lax");
#define EXPECT_ENOENT(what, err)					\
  do {									\
    if (lax)								\
      fprintf (stderr, "t_fdir_dead: %s: %s\n", what,			\
	       (err) ? strerror (err) : "done");			\
    else								\
      CHECK ((err) == ENOENT, "%s: %s", what,				\
	     (err) ? strerror (err) : "done");				\
  } while (0)

  jt_mount (image, 2, 0);
  jt_inode_globals ();
  files_init ();
  diskfs_root_node = root;
  cred_root = cred;
  cred_root.po = &po_root;
  po_root.np = root;

  /* 1.  */
  a = make_in (root, &cred_root, "A", S_IFDIR | 0755);
  b = make_in (root, &cred_root, "B", S_IFDIR | 0755);
  c = make_in (root, &cred_root, "C", S_IFDIR | 0755);
  f = make_in (root, &cred_root, "f", S_IFREG | 0644);
  cred_a = cred;
  cred_a.po = &po_a;
  po_a.np = a;

  /* 2.  */
  CHECK (!diskfs_S_dir_rename (&cred_root, "B", &cred_root, "A", 0),
	 "rename B onto A");
  CHECK (a->dn_stat.st_nlink == 0, "A has %d links after the rename",
	 (int) a->dn_stat.st_nlink);

  /* 3.  */
  diskfs_transaction_t *txn = diskfs_journal_start_transaction ();
  pthread_mutex_lock (&a->lock);
  err = diskfs_lookup (a, "g", CREATE, &np, ds, &cred_a);
  if (err == ENOENT)
    {
      err = diskfs_create_node (a, "g", S_IFREG | 0644, &np, &cred_a, ds);
      if (!err)
	diskfs_nput (np);
    }
  else
    diskfs_drop_dirstat (a, ds);
  pthread_mutex_unlock (&a->lock);
  diskfs_journal_stop_transaction (txn);
  EXPECT_ENOENT ("create in the removed A", err);

  err = diskfs_S_dir_rename (&cred_root, "f", &cred_a, "g", 0);
  EXPECT_ENOENT ("rename of a file into the removed A", err);
  err = diskfs_S_dir_rename (&cred_root, "C", &cred_a, "h", 0);
  EXPECT_ENOENT ("rename of a directory into the removed A", err);

  txn = diskfs_journal_start_transaction ();
  diskfs_nrele (a);
  diskfs_nrele (b);
  diskfs_nrele (c);
  diskfs_nrele (f);
  diskfs_journal_stop_transaction (txn);
  unsigned long warnings = jt_umount ();
  CHECK (warnings == 0, "%lu journal warnings", warnings);
  const char *out = "build/t_fdir_dead.img";
  jt_save_image (out);
  CHECK (jt_run ("e2fsck -fn %s >%s.fsck 2>&1", out, out) == 0,
	 "e2fsck -fn %s is not clean after the unmount (see %s.fsck)", out,
	 out);
  printf ("t_fdir_dead: OK (%lu warnings)\n", warnings);
  return 0;
}
