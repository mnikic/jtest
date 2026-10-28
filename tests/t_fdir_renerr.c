/* A rename of a directory that fails before it has locked the directory
   being moved, in libdiskfs's diskfs_rename_dir.

   diskfs_S_dir_rename unlocks the directory being moved (FNP) before it
   calls diskfs_rename_dir, which locks it only with its lookup of the
   source name.  The exits before that lookup must leave FNP's lock
   alone: the target lookup failing (EACCES, the target directory not
   writable) and the target existing with EXCL (EEXIST).

   U/D is renamed into RO, which refuses changes (jt_checkdirmod_hook),
   and onto T/X, an empty directory, with EXCL.

   1. Through diskfs_S_dir_rename: each fails with its error, D is still
      U's, and the link counts are unchanged.  FNP is free there, so
      unlocking it shows only under TSan ("unlock of an unlocked mutex").
   2. Through diskfs_rename_dir, while another thread holds D's lock, as
      a lookup in D would: each fails the same, and D's lock is still
      held afterwards.  Unlocking it took that thread's lock away, which
      shows in any build.

   After the unmount and in every crash image, e2fsck -fn is clean.  */

#include "files.h"
#include "fs_S.h"

static struct peropen po_root, po_u, po_ro, po_t;
static struct protid cred_root, cred_u, cred_ro, cred_t;
static ino_t ro_ino;

static error_t
deny_ro (io_statbuf_t *dir, io_statbuf_t *st)
{
  (void) st;
  return dir->st_ino == ro_ino ? EACCES : 0;
}

static struct node *
make_in (struct node *dp, struct protid *cr, const char *name, mode_t mode)
{
  struct node *np = NULL;
  struct dirstat *ds = alloca (diskfs_dirstat_size);
  diskfs_transaction_t *txn = diskfs_journal_start_transaction ();
  pthread_mutex_lock (&dp->lock);
  error_t err = diskfs_lookup (dp, (char *) name, CREATE, &np, ds, cr);
  CHECK (err == ENOENT, "lookup %s: %s", name, strerror (err));
  err = diskfs_create_node (dp, (char *) name, mode, &np, cr, ds);
  pthread_mutex_unlock (&dp->lock);
  CHECK (!err, "create %s: %s", name, strerror (err));
  pthread_mutex_unlock (&np->lock);
  CHECK (!jt_commit (txn), "commit %s", name);
  return np;
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

/* The holder of D's lock in 2.  */
static struct node *held;
static pthread_mutex_t step_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t step_cond = PTHREAD_COND_INITIALIZER;
static int holding, release;

static void *
holder (void *arg)
{
  (void) arg;
  pthread_mutex_lock (&held->lock);
  pthread_mutex_lock (&step_lock);
  holding = 1;
  pthread_cond_broadcast (&step_cond);
  while (!release)
    pthread_cond_wait (&step_cond, &step_lock);
  pthread_mutex_unlock (&step_lock);
  pthread_mutex_unlock (&held->lock);
  return NULL;
}

static struct node *u, *d, *ro, *t, *x;

/* D, U and the target directory TDP are as they were.  */
static void
check_unchanged (const char *what, struct node *tdp, nlink_t u_links,
		 nlink_t tdp_links)
{
  CHECK (ino_of (u, &cred_u, "D") == d->cache_id, "%s: D is not in U", what);
  CHECK (ino_of (d, &cred_u, "..") == u->cache_id,
	 "%s: D's \"..\" is not U", what);
  CHECK (u->dn_stat.st_nlink == u_links
	 && tdp->dn_stat.st_nlink == tdp_links,
	 "%s: U has %d links, the target %d; they had %d and %d", what,
	 (int) u->dn_stat.st_nlink, (int) tdp->dn_stat.st_nlink,
	 (int) u_links, (int) tdp_links);
}

/* 1: TDP/TONAME through the RPC, expecting EXPECT.  */
static void
rpc_case (const char *what, struct protid *tocred, struct node *tdp,
	  const char *toname, int excl, error_t expect)
{
  nlink_t u_links = u->dn_stat.st_nlink, tdp_links = tdp->dn_stat.st_nlink;
  error_t err = diskfs_S_dir_rename (&cred_u, "D", tocred, toname, excl);
  CHECK (err == expect, "%s: the rename gave %s, not %s", what,
	 err ? strerror (err) : "success", strerror (expect));
  check_unchanged (what, tdp, u_links, tdp_links);
  CHECK (!pthread_mutex_trylock (&d->lock), "%s: D was left locked", what);
  pthread_mutex_unlock (&d->lock);
}

/* 2: the same, through diskfs_rename_dir while another thread holds
   D's lock.  */
static void
held_case (const char *what, struct protid *tocred, struct node *tdp,
	   const char *toname, int excl, error_t expect)
{
  pthread_t th;
  held = d;
  holding = release = 0;
  pthread_create (&th, NULL, holder, NULL);
  pthread_mutex_lock (&step_lock);
  while (!holding)
    pthread_cond_wait (&step_cond, &step_lock);
  pthread_mutex_unlock (&step_lock);

  nlink_t u_links = u->dn_stat.st_nlink, tdp_links = tdp->dn_stat.st_nlink;
  /* diskfs_S_dir_rename's state at the call: a reference on D, not its
     lock, and a journal handle (a synchronous mount writes U's inode in
     the lookups).  */
  diskfs_nref (d);
  diskfs_transaction_t *txn = diskfs_journal_start_transaction ();
  error_t err = diskfs_rename_dir (u, d, "D", tdp, toname, &cred_u, tocred,
				   excl);
  diskfs_journal_stop_transaction (txn);
  diskfs_nrele (d);
  CHECK (err == expect, "%s (held): the rename gave %s, not %s", what,
	 err ? strerror (err) : "success", strerror (expect));
  CHECK (pthread_mutex_trylock (&d->lock) == EBUSY,
	 "%s (held): the rename unlocked D, which another thread held", what);

  pthread_mutex_lock (&step_lock);
  release = 1;
  pthread_cond_broadcast (&step_cond);
  pthread_mutex_unlock (&step_lock);
  pthread_join (th, NULL);
  check_unchanged (what, tdp, u_links, tdp_links);
}

int
main (int argc, char **argv)
{
  const char *image = argc > 1 ? argv[1] : "build/balloc.img";

  jt_mount (image, 2, 0);
  jt_inode_globals ();
  files_init ();
  diskfs_root_node = root;
  cred_root = cred;
  cred_root.po = &po_root;
  po_root.np = root;

  u = make_in (root, &cred_root, "U", S_IFDIR | 0755);
  ro = make_in (root, &cred_root, "RO", S_IFDIR | 0755);
  t = make_in (root, &cred_root, "T", S_IFDIR | 0755);
  cred_u = cred;
  cred_u.po = &po_u;
  po_u.np = u;
  cred_ro = cred;
  cred_ro.po = &po_ro;
  po_ro.np = ro;
  cred_t = cred;
  cred_t.po = &po_t;
  po_t.np = t;
  d = make_in (u, &cred_u, "D", S_IFDIR | 0755);
  x = make_in (t, &cred_t, "X", S_IFDIR | 0755);

  ro_ino = ro->cache_id;
  jt_checkdirmod_hook = deny_ro;

  /* 1.  */
  rpc_case ("EACCES", &cred_ro, ro, "D", 0, EACCES);
  rpc_case ("EEXIST", &cred_t, t, "X", 1, EEXIST);

  /* 2.  */
  held_case ("EACCES", &cred_ro, ro, "D", 0, EACCES);
  held_case ("EEXIST", &cred_t, t, "X", 1, EEXIST);

  jt_checkdirmod_hook = NULL;

  diskfs_transaction_t *txn = diskfs_journal_start_transaction ();
  diskfs_nrele (u);
  diskfs_nrele (d);
  diskfs_nrele (ro);
  diskfs_nrele (t);
  diskfs_nrele (x);
  diskfs_journal_stop_transaction (txn);
  unsigned long warnings = jt_umount ();
  CHECK (warnings == 0, "%lu journal warnings", warnings);
  const char *out = "build/t_fdir_renerr.img";
  jt_save_image (out);
  CHECK (jt_run ("e2fsck -fn %s >%s.fsck 2>&1", out, out) == 0,
	 "e2fsck -fn %s is not clean after the unmount (see %s.fsck)", out,
	 out);
  size_t checked = jt_crash_check ("t_fdir_renerr", NULL, NULL);
  printf ("t_fdir_renerr: OK (EACCES and EEXIST, free and held; %zu crash "
	  "images)\n", checked);
  return 0;
}
