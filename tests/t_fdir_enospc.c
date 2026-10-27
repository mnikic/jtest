/* Directory operations on a full disk whose only free blocks wait for a
   commit, with libdiskfs's dir_mkdir, dir_link, dir_rename, dir_lookup
   and file_set_translator over the real dir.c.

   Blocks freed in a transaction stay busy until it commits, and an
   operation whose own transaction freed them cannot wait for that commit
   under its handle: it fails with ENOSPC, although the blocks come back
   at the commit.  Each RPC must try again once it has let go of its
   handle and node locks (diskfs_retry_enospc).

   Directories W0..W5 are made, and file X, and then the disk (full.img)
   is filled.  Each W is filled with links to X under long names until
   one more name needs a block.  Each case below first truncates a file
   of its own, in a handle that ends without a commit, freeing exactly
   the blocks the operation needs, and then:

   0. mkdir W0/name: a block for W0's new name, one for the directory.
   1. link X as W1/name.
   2. rename file Y in the root to W2/name.
   3. rename directory Z in the root to W3/name: its first try fails
      after it has pointed Z's ".." to W3, which it must undo.
   4. dir_lookup of W4/name from the root, with O_CREAT: a new file.  A
      path with a slash, which the lookup cuts in place.
   5. file_set_translator of a symlink to a 100-character target on file
      S: the target needs a block.

   Each must succeed, and leave what it made.  After the unmount and in
   every crash image, e2fsck -fn is clean.  */

#include "files.h"
#include "fs_S.h"
#include "io_S.h"

#define NW 6

struct dir
{
  struct node *np;
  struct peropen po;
  struct protid cred;
};

static struct dir rootd;

static void
open_dir (struct dir *d, struct node *np, int openstat)
{
  d->np = np;
  memset (&d->po, 0, sizeof d->po);
  d->po.np = np;
  d->po.openstat = O_READ | openstat;
  d->cred = cred;
  d->cred.po = &d->po;
}

static struct node *
make_in (struct dir *d, const char *name, mode_t mode)
{
  struct node *np;
  struct dirstat *ds = alloca (diskfs_dirstat_size);
  diskfs_transaction_t *txn = diskfs_journal_start_transaction ();
  pthread_mutex_lock (&d->np->lock);
  CHECK (diskfs_lookup (d->np, (char *) name, CREATE, &np, ds, &d->cred)
	 == ENOENT, "lookup %s", name);
  CHECK (!diskfs_create_node (d->np, (char *) name, mode, &np, &d->cred, ds),
	 "create %s", name);
  pthread_mutex_unlock (&d->np->lock);
  pthread_mutex_unlock (&np->lock);
  CHECK (!jt_commit (txn), "commit %s", name);
  return np;
}

static void
write_blocks (struct node *np, size_t blocks)
{
  struct dir f;
  open_dir (&f, np, O_WRITE);
  char *buf = calloc (blocks, block_size);
  vm_size_t amt;
  CHECK (!diskfs_S_io_write (&f.cred, buf, blocks * block_size, 0, &amt)
	 && amt == blocks * block_size, "write %zu blocks", blocks);
  free (buf);
}

/* Truncate NP to nothing in a handle of its own, without a commit.  */
static void
free_blocks_of (struct node *np)
{
  diskfs_transaction_t *txn = diskfs_journal_start_transaction ();
  pthread_mutex_lock (&np->lock);
  CHECK (!diskfs_truncate (np, 0), "truncate");
  pthread_mutex_unlock (&np->lock);
  diskfs_journal_stop_transaction (txn);
}

static void
commit_all (void)
{
  CHECK (!jt_commit (diskfs_journal_start_transaction ()), "commit");
}

/* A name of 250 characters for case CASE, number N.  */
static const char *
name_for (int c, int n)
{
  static char buf[256];
  memset (buf, 'a' + c, 244);
  snprintf (buf + 244, sizeof buf - 244, "%06d", n);
  return buf;
}

/* NAME in D is a node of type TYPE; returns its inode number.  */
static ino_t
check_entry (struct dir *d, const char *name, mode_t type, const char *what)
{
  struct node *np;
  pthread_mutex_lock (&d->np->lock);
  error_t err = diskfs_lookup (d->np, (char *) name, LOOKUP, &np, NULL,
			       &d->cred);
  pthread_mutex_unlock (&d->np->lock);
  CHECK (!err, "%s: the new name is not there: %s", what, strerror (err));
  CHECK ((np->dn_stat.st_mode & S_IFMT) == type, "%s: the new name is of "
	 "type %o", what, np->dn_stat.st_mode & S_IFMT);
  ino_t ino = np->cache_id;
  diskfs_nput (np);
  return ino;
}

static void
report (const char *what, error_t err)
{
  CHECK (!err, "%s: %s", what, strerror (err));
}

int
main (int argc, char **argv)
{
  const char *image = argc > 1 ? argv[1] : "build/full.img";
  struct dir w[NW];
  struct node *f[NW];
  error_t err;

  jt_expected_warning = "out of space";
  jt_mount (image, 2, 0);
  jt_inode_globals ();
  files_init ();
  diskfs_root_node = root;
  open_dir (&rootd, root, O_WRITE);

  struct node *x = make_in (&rootd, "X", S_IFREG | 0644);
  struct node *y = make_in (&rootd, "Y", S_IFREG | 0644);
  struct node *z = make_in (&rootd, "Z", S_IFDIR | 0755);
  struct node *s = make_in (&rootd, "S", S_IFREG | 0644);
  for (int i = 0; i < NW; i++)
    {
      char name[16];		/* Room for any int: W%d.  */
      snprintf (name, sizeof name, "W%d", i);
      open_dir (&w[i], make_in (&rootd, name, S_IFDIR | 0755), O_WRITE);
      snprintf (name, sizeof name, "F%d", i);
      f[i] = make_in (&rootd, name, S_IFREG | 0644);
      write_blocks (f[i], i == 0 ? 2 : 1);
    }
  struct dir xd;
  open_dir (&xd, x, 0);
  commit_all ();

  /* The disk, a block at a time.  */
  struct node *g = make_in (&rootd, "G", S_IFREG | 0644);
  struct dir gd;
  open_dir (&gd, g, O_WRITE);
  char *block = calloc (1, block_size);
  size_t filled;
  for (filled = 0;; filled++)
    {
      vm_size_t amt;
      err = diskfs_S_io_write (&gd.cred, block, block_size,
			       filled * block_size, &amt);
      if (err)
	break;
    }
  free (block);
  CHECK (err == ENOSPC, "filling the disk: %s", strerror (err));

  /* Each W, until one more name needs a block.  */
  int names[NW];
  for (int i = 0; i < NW; i++)
    {
      for (names[i] = 0;; names[i]++)
	{
	  err = diskfs_S_dir_link (&w[i].cred, &xd.cred,
				   name_for (i, names[i]), 1);
	  if (err)
	    break;
	}
      CHECK (err == ENOSPC, "filling W%d: %s", i, strerror (err));
    }
  commit_all ();

  /* 0.  */
  const char *name = name_for (0, names[0]);
  free_blocks_of (f[0]);
  report ("mkdir", diskfs_S_dir_mkdir (&w[0].cred, name, 0755));
  ino_t made = check_entry (&w[0], name, S_IFDIR, "mkdir");

  /* 1.  */
  commit_all ();
  name = name_for (1, names[1]);
  free_blocks_of (f[1]);
  report ("link", diskfs_S_dir_link (&w[1].cred, &xd.cred, name, 1));
  CHECK (check_entry (&w[1], name, S_IFREG, "link") == x->cache_id,
	 "link: the new name is not X");

  /* 2.  */
  commit_all ();
  name = name_for (2, names[2]);
  free_blocks_of (f[2]);
  report ("file rename", diskfs_S_dir_rename (&rootd.cred, "Y", &w[2].cred,
					      name, 0));
  CHECK (check_entry (&w[2], name, S_IFREG, "file rename") == y->cache_id,
	 "file rename: the new name is not Y");

  /* 3.  */
  commit_all ();
  name = name_for (3, names[3]);
  free_blocks_of (f[3]);
  report ("directory rename", diskfs_S_dir_rename (&rootd.cred, "Z",
						   &w[3].cred, name, 0));
  CHECK (check_entry (&w[3], name, S_IFDIR, "directory rename")
	 == z->cache_id, "directory rename: the new name is not Z");
  struct dir zd;
  open_dir (&zd, z, 0);
  CHECK (check_entry (&zd, "..", S_IFDIR, "directory rename")
	 == w[3].np->cache_id, "directory rename: Z's \"..\" is not W3");

  /* 4.  */
  commit_all ();
  name = name_for (4, names[4]);
  char path[sizeof "W4/" + 256];
  snprintf (path, sizeof path, "W4/%s", name);
  free_blocks_of (f[4]);
  retry_type retry;
  string_t retry_name;
  mach_port_t port = MACH_PORT_NULL;
  mach_msg_type_name_t port_type;
  report ("create", diskfs_S_dir_lookup (&rootd.cred, path,
					 O_CREAT | O_READ | O_WRITE, 0644,
					 &retry, retry_name, &port,
					 &port_type));
  struct protid *pi = jt_protid_of_port (port);
  CHECK (pi && S_ISREG (pi->po->np->dn_stat.st_mode),
	 "create: no new file came back");
  CHECK (check_entry (&w[4], name, S_IFREG, "create")
	 == pi->po->np->cache_id, "create: the new name is not the new file");

  /* 5.  */
  commit_all ();
  free_blocks_of (f[5]);
  char target[101];
  memset (target, 't', 100);
  target[100] = '\0';
  char passive[sizeof "/hurd/symlink" + sizeof target];
  memcpy (passive, "/hurd/symlink", sizeof "/hurd/symlink");
  memcpy (passive + sizeof "/hurd/symlink", target, sizeof target);
  struct dir sd;
  open_dir (&sd, s, O_WRITE);
  report ("symlink", diskfs_S_file_set_translator (&sd.cred, FS_TRANS_SET, 0,
						   0, passive,
						   sizeof passive,
						   MACH_PORT_NULL));
  CHECK (S_ISLNK (s->dn_stat.st_mode) && s->dn_stat.st_size == 100,
	 "symlink: S is mode %o, %lld bytes", s->dn_stat.st_mode,
	 (long long) s->dn_stat.st_size);

  diskfs_transaction_t *txn = diskfs_journal_start_transaction ();
  jt_protid_release (port);
  for (int i = 0; i < NW; i++)
    {
      diskfs_nrele (w[i].np);
      diskfs_nrele (f[i]);
    }
  diskfs_nrele (x);
  diskfs_nrele (y);
  diskfs_nrele (z);
  diskfs_nrele (s);
  diskfs_nrele (g);
  diskfs_journal_stop_transaction (txn);
  unsigned long warnings = jt_umount ();
  CHECK (warnings == 0, "%lu journal warnings", warnings);
  const char *out = "build/t_fdir_enospc.img";
  jt_save_image (out);
  CHECK (jt_run ("e2fsck -fn %s >%s.fsck 2>&1", out, out) == 0,
	 "e2fsck -fn %s is not clean after the unmount (see %s.fsck)", out,
	 out);
  size_t checked = jt_crash_check ("t_fdir_enospc", NULL, NULL);
  printf ("t_fdir_enospc: OK (%zu blocks filled; directory %llu made; %zu "
	  "crash images)\n", filled, (unsigned long long) made, checked);
  return 0;
}
