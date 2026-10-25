/* A rename of a directory that fails after it has changed "..", in
   libdiskfs's diskfs_rename_dir.

   The rename sets the moved directory's ".." to its new parent (step 2),
   then enters its new name there (step 3).  When step 3 fails, ".." and
   the link counts must be as they were: the directory stays where it
   was.

   1. ENOSPC.  The disk (full.img) is filled, then directory T with long
      names until one more needs a new block.  A rename of directory D
      from the root into T, with a name as long, needs that block: it
      fails with ENOSPC, D's ".." is still the root, and the link counts
      of the root and T are unchanged.
   2. EMLINK.  With diskfs_link_max at 3, a rename of directory A/D2 to
      the root fails with EMLINK, as D2 has 2 links: A is not left
      locked, and D2's ".." is still A.

   After the unmount and in every crash image, e2fsck -fn is clean.
   With "emlink" as the third argument, only 2. runs.  */

#include "files.h"
#include "fs_S.h"
#include "io_S.h"

static struct peropen po_root, po_a, po_t;
static struct protid cred_root, cred_a, cred_t;

static struct node *
make_in (struct node *dp, struct protid *cr, const char *name, mode_t mode,
	 error_t *errp)
{
  struct node *np = NULL;
  struct dirstat *ds = alloca (diskfs_dirstat_size);
  diskfs_transaction_t *txn = diskfs_journal_start_transaction ();
  pthread_mutex_lock (&dp->lock);
  error_t err = diskfs_lookup (dp, (char *) name, CREATE, &np, ds, cr);
  CHECK (err == ENOENT, "lookup %s: %s", name, strerror (err));
  err = diskfs_create_node (dp, (char *) name, mode, &np, cr, ds);
  pthread_mutex_unlock (&dp->lock);
  if (!err)
    pthread_mutex_unlock (&np->lock);
  CHECK (!jt_commit (txn), "commit %s", name);
  if (errp)
    *errp = err;
  else
    CHECK (!err, "create %s: %s", name, strerror (err));
  return err ? NULL : np;
}

/* The node that NAME in DP is, by its inode number.  */
static ino_t
ino_of (struct node *dp, struct protid *cr, const char *name)
{
  struct node *np;
  pthread_mutex_lock (&dp->lock);
  error_t err = diskfs_lookup (dp, (char *) name, LOOKUP, &np, NULL, cr);
  pthread_mutex_unlock (&dp->lock);
  CHECK (!err, "lookup %s: %s", name, strerror (err));
  ino_t ino = np->cache_id;
  if (np == dp)
    diskfs_nrele (np);
  else
    diskfs_nput (np);
  return ino;
}

/* A name of 200 characters, ending in N.  */
static const char *
long_name (int n)
{
  static char buf[256];
  memset (buf, 'x', 196);
  snprintf (buf + 196, sizeof buf - 196, "%04d", n);
  return buf;
}

int
main (int argc, char **argv)
{
  const char *image = argc > 1 ? argv[1] : "build/full.img";
  error_t err;

  jt_expected_warning = "out of space";
  jt_mount (image, 2, 0);
  jt_inode_globals ();
  files_init ();
  diskfs_root_node = root;
  cred_root = cred;
  cred_root.po = &po_root;
  po_root.np = root;

  struct node *t = make_in (root, &cred_root, "T", S_IFDIR | 0755, NULL);
  struct node *d = make_in (root, &cred_root, "D", S_IFDIR | 0755, NULL);
  struct node *a = make_in (root, &cred_root, "A", S_IFDIR | 0755, NULL);
  cred_t = cred;
  cred_t.po = &po_t;
  po_t.np = t;
  cred_a = cred;
  cred_a.po = &po_a;
  po_a.np = a;
  struct node *d2 = make_in (a, &cred_a, "D2", S_IFDIR | 0755, NULL);

  /* The disk, a block at a time.  */
  struct node *g = make_in (root, &cred_root, "G", S_IFREG | 0644, NULL);
  struct peropen po_g = { .np = g, .openstat = O_READ | O_WRITE };
  struct protid cred_g = cred;
  cred_g.po = &po_g;
  char *block = calloc (1, block_size);
  size_t filled;
  for (filled = 0;; filled++)
    {
      vm_size_t amt;
      err = diskfs_S_io_write (&cred_g, block, block_size,
			       filled * block_size, &amt);
      if (err)
	break;
    }
  free (block);
  CHECK (err == ENOSPC, "filling the disk: %s", strerror (err));

  /* T, until one more name needs a block.  */
  int names;
  for (names = 0;; names++)
    {
      struct node *np = make_in (t, &cred_t, long_name (names),
				 S_IFREG | 0644, &err);
      if (err)
	break;
      diskfs_nrele (np);
    }
  CHECK (err == ENOSPC, "filling T: %s", strerror (err));
  CHECK (!jt_commit (diskfs_journal_start_transaction ()), "commit");

  /* 1.  */
  if (argc > 3 && !strcmp (argv[3], "emlink"))
    goto emlink;
  nlink_t root_links = root->dn_stat.st_nlink, t_links = t->dn_stat.st_nlink;
  err = diskfs_S_dir_rename (&cred_root, "D", &cred_t, long_name (names), 0);
  CHECK (err == ENOSPC, "ENOSPC: the rename gave %s",
	 err ? strerror (err) : "success");
  CHECK (ino_of (d, &cred_root, "..") == root->cache_id,
	 "ENOSPC: D's \"..\" is inode %llu, not the root",
	 (unsigned long long) ino_of (d, &cred_root, ".."));
  CHECK (root->dn_stat.st_nlink == root_links
	 && t->dn_stat.st_nlink == t_links,
	 "ENOSPC: the root has %d links, T %d; they had %d and %d",
	 (int) root->dn_stat.st_nlink, (int) t->dn_stat.st_nlink,
	 (int) root_links, (int) t_links);
  CHECK (ino_of (root, &cred_root, "D") == d->cache_id,
	 "ENOSPC: D is not in the root");

  /* 2.  */
 emlink:;
  int link_max = diskfs_link_max;
  diskfs_link_max = 3;
  err = diskfs_S_dir_rename (&cred_a, "D2", &cred_root, "D2", 0);
  diskfs_link_max = link_max;
  CHECK (err == EMLINK, "EMLINK: the rename gave %s",
	 err ? strerror (err) : "success");
  CHECK (!pthread_mutex_trylock (&a->lock), "EMLINK: A was left locked");
  pthread_mutex_unlock (&a->lock);
  CHECK (ino_of (d2, &cred_a, "..") == a->cache_id,
	 "EMLINK: D2's \"..\" is not A");

  diskfs_transaction_t *txn = diskfs_journal_start_transaction ();
  diskfs_nrele (t);
  diskfs_nrele (d);
  diskfs_nrele (a);
  diskfs_nrele (d2);
  diskfs_nrele (g);
  diskfs_journal_stop_transaction (txn);
  unsigned long warnings = jt_umount ();
  CHECK (warnings == 0, "%lu journal warnings", warnings);
  const char *out = "build/t_fdir_renfail.img";
  jt_save_image (out);
  CHECK (jt_run ("e2fsck -fn %s >%s.fsck 2>&1", out, out) == 0,
	 "e2fsck -fn %s is not clean after the unmount (see %s.fsck)", out,
	 out);
  size_t checked = jt_crash_check ("t_fdir_renfail", NULL, NULL);
  printf ("t_fdir_renfail: OK (%zu blocks filled, %d names in T; %zu crash "
	  "images)\n", filled, names, checked);
  return 0;
}
