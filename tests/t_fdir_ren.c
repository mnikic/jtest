/* Renames across directories, with the real libdiskfs and dir.c on the
   harness's file pager: diskfs_S_dir_rename for files and directories
   (diskfs_rename_dir, with its checkpath and the rewrite of "..").

   Four directories are made under the root, named from the same set (diskfs_create_node with a
   name, which runs diskfs_init_dir).  Threads then make operations at
   once, with names from a small set so that they collide:
     create   a file in a directory, as dir_mkfile and dir_link do in one
	      (diskfs_create_node with a name);
     unlink   a name, as dir-unlink.c does (directories are left alone);
     link     a file of one directory under a name of another, or the
	      same, through diskfs_S_dir_link: files get more names;
     rename   a name of one directory to a name of another, or the same,
	      through diskfs_S_dir_rename: files move or replace files,
	      directories move, and nest into each other unless that would
	      put one inside itself;
     move     one of the four directories, from where it was last seen,
	      to a name in any of them: a move into itself or its children
	      is refused (checkpath), one onto an empty directory replaces
	      it (diskfs_clear_directory), onto another fails.
   Each call returns 0 or an error the operation may give (the name is
   gone, taken, or a directory where a file is expected); pages of the
   directories are evicted at random; some operations commit.

   Now and then a thread locks a directory and checks a name in libdiskfs's
   name cache against the directory (namecache.h); once the threads are
   done, every name of the set and ".." in every directory.

   After the unmount and in every crash image, e2fsck -fn is clean: link
   counts, "..", the directories reachable, no entry to a free inode.
   Which names the threads' races leave is not predicted: a crash image
   is checked for consistency, not against a model.  */

#include "files.h"
#include "fs_S.h"
#include "namecache.h"

#define NDIRS 4
#define NT 4
#define OPS 200
#define NNAMES 12

/* The root and the directories under it: their nodes stay (directories
   are moved, never removed), each with a protid and peropen, as an RPC on
   its port has.  */
static struct node *dirs[NDIRS + 1];
static struct peropen pos[NDIRS + 1];
static struct protid creds[NDIRS + 1];
static unsigned long n_create, n_unlink, n_rename, n_dir_rename, n_err;

/* Where each directory was last moved to, for picking one to move: its
   parent (an index into DIRS) and its name.  Threads race, so it can be
   out of date; a rename of a directory that has moved since gives
   ENOENT.  */
static pthread_mutex_t where_lock = PTHREAD_MUTEX_INITIALIZER;
static int where_dir[NDIRS + 1];
static char where_name[NDIRS + 1][16];

static void
name_of (char *buf, int n)
{
  snprintf (buf, 16, "n%02d", n);
}

static int
allowed (error_t err)
{
  return !err || err == ENOENT || err == EEXIST || err == EINVAL
    || err == ENOTEMPTY || err == EISDIR || err == ENOTDIR;
}

static void
op_create (int d, const char *name)
{
  struct node *dp = dirs[d], *np;
  struct dirstat *ds = alloca (diskfs_dirstat_size);
  pthread_mutex_lock (&dp->lock);
  error_t err = diskfs_lookup (dp, (char *) name, CREATE, &np, ds, &creds[d]);
  if (err == ENOENT)
    {
      err = diskfs_create_node (dp, name, S_IFREG | 0644, &np, &creds[d],
				ds);
      if (!err)
	diskfs_nput (np);
      __atomic_add_fetch (&n_create, 1, __ATOMIC_RELAXED);
    }
  else
    {
      diskfs_drop_dirstat (dp, ds);
      if (!err)
	{
	  diskfs_nput (np);
	  err = EEXIST;
	}
    }
  pthread_mutex_unlock (&dp->lock);
  CHECK (allowed (err), "create: %s", strerror (err));
}

/* dir-unlink.c's steps, for a file.  */
static void
op_unlink (int d, const char *name)
{
  struct node *dp = dirs[d], *np;
  struct dirstat *ds = alloca (diskfs_dirstat_size);
  pthread_mutex_lock (&dp->lock);
  error_t err = diskfs_lookup (dp, (char *) name, REMOVE, &np, ds, &creds[d]);
  if (err)
    {
      diskfs_drop_dirstat (dp, ds);
      pthread_mutex_unlock (&dp->lock);
      CHECK (allowed (err), "unlink lookup: %s", strerror (err));
      return;
    }
  if (S_ISDIR (np->dn_stat.st_mode))
    {
      diskfs_drop_dirstat (dp, ds);
      diskfs_nput (np);
      pthread_mutex_unlock (&dp->lock);
      return;
    }
  err = diskfs_dirremove (dp, np, name, ds);
  diskfs_node_update (dp, 0);
  CHECK (!err, "dirremove: %s", strerror (err));
  np->dn_stat.st_nlink--;
  np->dn_set_ctime = 1;
  if (np->dn_stat.st_nlink == 0)
    diskfs_orphan_add (np);
  diskfs_node_update (np, 0);
  diskfs_nput (np);
  pthread_mutex_unlock (&dp->lock);
  __atomic_add_fetch (&n_unlink, 1, __ATOMIC_RELAXED);
}

static void
op_rename (int fd, const char *fromname, int td, const char *toname)
{
  /* Count the directory renames: look at what FROMNAME is.  */
  struct node *np;
  int isdir = 0;
  pthread_mutex_lock (&dirs[fd]->lock);
  if (!diskfs_lookup (dirs[fd], (char *) fromname, LOOKUP, &np, NULL, &creds[fd]))
    {
      isdir = S_ISDIR (np->dn_stat.st_mode);
      diskfs_nput (np);
    }
  pthread_mutex_unlock (&dirs[fd]->lock);

  error_t err = diskfs_S_dir_rename (&creds[fd], fromname, &creds[td],
				     toname, 0);
  CHECK (allowed (err), "rename: %s", strerror (err));
  if (err)
    __atomic_add_fetch (&n_err, 1, __ATOMIC_RELAXED);
  else if (isdir)
    {
      __atomic_add_fetch (&n_dir_rename, 1, __ATOMIC_RELAXED);
      pthread_mutex_lock (&where_lock);
      for (int i = 1; i <= NDIRS; i++)
	if (where_dir[i] == fd && !strcmp (where_name[i], fromname))
	  {
	    where_dir[i] = td;
	    strcpy (where_name[i], toname);
	  }
      pthread_mutex_unlock (&where_lock);
    }
  __atomic_add_fetch (&n_rename, 1, __ATOMIC_RELAXED);
}

static unsigned long n_link;

/* Link FROMNAME of directory FD, if it is a file, as TONAME in TD.  */
static void
op_link (int fd, const char *fromname, int td, const char *toname)
{
  struct node *np;
  pthread_mutex_lock (&dirs[fd]->lock);
  error_t err = diskfs_lookup (dirs[fd], (char *) fromname, LOOKUP, &np,
			       NULL, &creds[fd]);
  pthread_mutex_unlock (&dirs[fd]->lock);
  if (err)
    return;
  int file = S_ISREG (np->dn_stat.st_mode);
  pthread_mutex_unlock (&np->lock);
  if (file)
    {
      struct peropen po = { .np = np };
      struct protid fc = cred;
      fc.po = &po;
      err = diskfs_S_dir_link (&creds[td], &fc, toname, 1);
      CHECK (allowed (err) || err == EMLINK, "link: %s", strerror (err));
      if (!err)
	__atomic_add_fetch (&n_link, 1, __ATOMIC_RELAXED);
    }
  /* The last reference can drop the node: in a handle.  */
  diskfs_transaction_t *txn = diskfs_journal_start_transaction ();
  diskfs_nrele (np);
  diskfs_journal_stop_transaction (txn);
}

/* Move directory I to directory TD under name TONAME.  */
static void
op_move_dir (int i, int td, const char *toname)
{
  char from[16];
  pthread_mutex_lock (&where_lock);
  int fd = where_dir[i];
  strcpy (from, where_name[i]);
  pthread_mutex_unlock (&where_lock);
  op_rename (fd, from, td, toname);
}

static void *
worker (void *arg)
{
  unsigned seed = (unsigned) (uintptr_t) arg;
  char a[16], b[16];

  for (int op = 0; op < OPS; op++)
    {
      int r = rand_r (&seed) % 10;
      int d = rand_r (&seed) % (NDIRS + 1), e = rand_r (&seed) % (NDIRS + 1);
      name_of (a, rand_r (&seed) % NNAMES);
      name_of (b, rand_r (&seed) % NNAMES);
      /* diskfs_S_dir_rename starts its own handle; the others are one
	 handle each, as an RPC is.  */
      if (r < 2)
	op_move_dir (1 + rand_r (&seed) % NDIRS, e, b);
      else if (r < 4)
	op_rename (d, a, e, b);
      else if (r == 4)
	op_link (d, a, e, b);
      else
	{
	  diskfs_transaction_t *txn = diskfs_journal_start_transaction ();
	  if (r < 7)
	    op_create (d, a);
	  else
	    op_unlink (d, a);
	  if (rand_r (&seed) % 5 == 0)
	    CHECK (!jt_commit (txn), "commit");
	  else
	    diskfs_journal_stop_transaction (txn);
	}
      if (rand_r (&seed) % 3 == 0)
	{
	  int c = rand_r (&seed) % (NDIRS + 1);
	  name_of (a, rand_r (&seed) % NNAMES);
	  pthread_mutex_lock (&dirs[c]->lock);
	  const char *wrong = nc_check (dirs[c], a, &creds[c]);
	  pthread_mutex_unlock (&dirs[c]->lock);
	  CHECK (!wrong, "%s", wrong);
	}
      for (int x = rand_r (&seed) % 3; x > 0; x--)
	{
	  struct node *dp = dirs[rand_r (&seed) % (NDIRS + 1)];
	  pthread_mutex_lock (&dp->lock);
	  size_t pages = (dp->dn_stat.st_size >> log2_block_size) + 1;
	  pthread_mutex_unlock (&dp->lock);
	  jt_fp_pageout (dp, rand_r (&seed) % pages, 1, 0);
	}
    }
  return NULL;
}

static void
print_dirs (void)
{
  for (int d = 0; d <= NDIRS; d++)
    fprintf (stderr, "t_fdir_ren: dirs[%d] = %p, inode %lu, last seen as "
	     "%s in dirs[%d]\n", d, (void *) dirs[d],
	     (unsigned long) dirs[d]->cache_id, d ? where_name[d] : "/",
	     d ? where_dir[d] : 0);
}

int
main (int argc, char **argv)
{
  const char *image = argc > 1 ? argv[1] : "build/balloc.img";
  unsigned seed = argc > 2 ? atoi (argv[2]) : 1;
  pthread_t t[NT];

  jt_mount (image, 3, 0);
  jt_inode_globals ();
  files_init ();
  diskfs_root_node = root;
  jt_deadlock_hook = print_dirs;

  dirs[0] = root;
  diskfs_nref (root);
  for (int d = 0; d <= NDIRS; d++)
    {
      creds[d] = cred;
      creds[d].po = &pos[d];
    }
  pos[0].np = root;

  /* The directories, as dir_mkdir makes them.  */
  for (int d = 1; d <= NDIRS; d++)
    {
      char name[16];
      struct node *np;
      struct dirstat *ds = alloca (diskfs_dirstat_size);
      /* Names of the same set: renames move them, and collide.  */
      name_of (name, d - 1);
      diskfs_transaction_t *txn = diskfs_journal_start_transaction ();
      pthread_mutex_lock (&root->lock);
      CHECK (diskfs_lookup (root, name, CREATE, &np, ds, &creds[0]) == ENOENT,
	     "lookup %s", name);
      CHECK (!diskfs_create_node (root, name, S_IFDIR | 0755, &np, &creds[0],
				  ds), "mkdir %s", name);
      pthread_mutex_unlock (&root->lock);
      pthread_mutex_unlock (&np->lock);
      dirs[d] = np;
      pos[d].np = np;
      where_dir[d] = 0;
      strcpy (where_name[d], name);
      CHECK (!jt_commit (txn), "commit mkdir");
    }

  /* The periodic sync, every few milliseconds, unless
     JT_NO_PERIODIC_SYNC (JT_PERIODIC_SYNC_US: at most that long apart,
     default 2000): it writes every cached node, each under its
     lock, in a handle of its own, and commits.  */
  int psync = !getenv ("JT_NO_PERIODIC_SYNC");
  if (psync)
    jt_periodic_sync_start (getenv ("JT_PERIODIC_SYNC_US")
			    ? atoi (getenv ("JT_PERIODIC_SYNC_US")) : 2000, seed);
  for (int i = 0; i < NT; i++)
    pthread_create (&t[i], NULL, worker, (void *) (uintptr_t) (seed * 31 + i));
  for (int i = 0; i < NT; i++)
    pthread_join (t[i], NULL);
  unsigned long syncs = psync ? jt_periodic_sync_stop () : 0;

  /* The name cache, with nothing running.  */
  for (int d = 0; d <= NDIRS; d++)
    {
      char n[16];
      pthread_mutex_lock (&dirs[d]->lock);
      for (int i = 0; i <= NNAMES; i++)
	{
	  if (i < NNAMES)
	    name_of (n, i);
	  else
	    strcpy (n, "..");
	  const char *wrong = nc_check (dirs[d], n, &creds[d]);
	  CHECK (!wrong, "after the threads: %s", wrong);
	}
      pthread_mutex_unlock (&dirs[d]->lock);
    }

  /* The last references can write the inodes: in a handle, as an RPC's
     release is.  */
  diskfs_transaction_t *txn = diskfs_journal_start_transaction ();
  for (int d = 0; d <= NDIRS; d++)
    diskfs_nrele (dirs[d]);
  diskfs_journal_stop_transaction (txn);
  unsigned long warnings = jt_umount ();
  CHECK (warnings == 0, "%lu journal warnings", warnings);
  CHECK (jt_fp_stale_reads () == 0, "%lu stale file page-ins",
	 jt_fp_stale_reads ());
  const char *out = "build/t_fdir_ren.img";
  jt_save_image (out);
  CHECK (jt_run ("e2fsck -fn %s >%s.fsck 2>&1", out, out) == 0,
	 "e2fsck -fn %s is not clean after the unmount (see %s.fsck)", out,
	 out);

  size_t checked = jt_crash_check ("t_fdir_ren", NULL, NULL);
  printf ("t_fdir_ren: OK (%d threads; %lu creates, %lu unlinks, %lu "
	  "renames, %lu of directories, %lu refused, %lu links; %lu syncs; "
	  "%lu name cache checks, %lu hits, %lu negative; %zu writes, %zu "
	  "crash images)\n", NT, n_create, n_unlink, n_rename, n_dir_rename,
	  n_err, n_link, syncs, nc_checks, nc_hits, nc_negative,
	  jt_log_count (), checked);
  return 0;
}
