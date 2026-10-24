/* The orphan list, on the real orphan.c, ialloc.c, inode.c and hyper.c,
   with libdiskfs's diskfs_create_node, node cache, references and drop.

   Threads create anonymous files, as dir_mkfile does (diskfs_create_node
   with no name: no links), grow them through ext2_getblk, truncate them
   through diskfs_truncate, give them a name in the root directory and
   take it away again, and close them: the last reference to a file
   without links drops the node, which truncates it, takes it off the
   orphan list and frees it.  Some are still open at the unmount.  The
   harness has no dir.c: the names are written into the root's first
   directory block here, and the node steps are dir_link's and
   dir_unlink's, copied: a link from no links takes the file off the
   orphan list, an unlink to none puts it on.  JT_TREE_DIR_LINK=1 copies
   the tree's dir_link, which leaves it there.

   Every crash image must hold an orphan list of files without links
   only, when debugfs replays it (which leaves the list alone), and must
   be clean for e2fsck -fn when e2fsck replays it (which also releases
   the files on the list): a file with no links left off the list shows
   as an inode in use without links.

   The image of the unmount, and some crash images replayed by debugfs,
   are mounted again for ext2_recover_orphan_list to release the files
   (t_orphan --recover IMAGE), and must then be clean too.  Every other
   one is mounted read-only first, as the root is, and made writable
   after: diskfs_readonly_changed must recover them then
   (t_orphan --recover-ro IMAGE).  */

#include <pthread.h>
#include <unistd.h>
#include "common.h"

#define NTHREADS 3
#define OPS 80
#define OPEN_MAX 4
#define MAXB 200

static struct node *root;
static block_t root_blk;	/* The root's first directory block.  */
static int tree_dir_link;
static struct iouser user;
static struct protid cred = { .user = &user };
static unsigned long n_create, n_grow, n_trunc, n_close, n_enospc;
static unsigned long n_link, n_unlink;

/* Add (INO nonzero) or remove the name of inode INO / OLD in the root's
   first block, as diskfs_direnter and diskfs_dirremove would.  The root
   is locked.  */
static void
dir_name (diskfs_transaction_t *txn, ino_t ino, ino_t old)
{
  char name[16];
  unsigned char *buf = malloc (block_size);
  size_t len = snprintf (name, sizeof name, "f%lu",
			 (unsigned long) (ino ? ino : old));
  size_t need = EXT2_DIR_REC_LEN (len), off = 0, prev = SIZE_MAX;
  int done = 0;

  CHECK (!journal_get_write_access_external (txn, root_blk), "reserve");
  jt_dir_read (root_blk, buf);
  while (off < block_size && !done)
    {
      struct ext2_dir_entry *e = (struct ext2_dir_entry *) (buf + off);
      size_t rec = le16toh (e->rec_len);
      size_t used = e->inode ? EXT2_DIR_REC_LEN (le16toh (e->name_len)) : 0;

      if (ino && rec - used >= need)
	{
	  /* Split the room after this entry off for the new one.  */
	  if (used)
	    {
	      e->rec_len = htole16 (used);
	      e = (struct ext2_dir_entry *) (buf + off + used);
	      rec -= used;
	    }
	  e->inode = htole32 (ino);
	  e->rec_len = htole16 (rec);
	  e->name_len = htole16 (len);
	  memcpy (e->name, name, len);
	  done = 1;
	}
      else if (!ino && e->inode == htole32 (old)
	       && le16toh (e->name_len) == len && !memcmp (e->name, name, len))
	{
	  if (prev != SIZE_MAX)
	    {
	      struct ext2_dir_entry *p = (struct ext2_dir_entry *) (buf + prev);
	      p->rec_len = htole16 (le16toh (p->rec_len) + rec);
	    }
	  else
	    e->inode = 0;
	  done = 1;
	}
      prev = off;
      off += rec;
    }
  CHECK (done, "%s %s in the root", ino ? "no room for" : "no", name);
  jt_dir_edit (root_blk, 0, buf, block_size);
  CHECK (!jt_dir_mark_dirty (txn, root_blk), "mark the root");
  free (buf);
}

static void
op_end (diskfs_transaction_t *txn, unsigned *seed)
{
  if (rand_r (seed) % 4 == 0)
    CHECK (!jt_commit (txn), "commit");
  else
    diskfs_journal_stop_transaction (txn);
}

static void *
worker (void *arg)
{
  unsigned seed = (unsigned) (uintptr_t) arg;
  struct node *open[OPEN_MAX] = { 0 };
  int nopen = 0;

  for (int op = 0; op < OPS; op++)
    {
      int r = rand_r (&seed) % 10;
      diskfs_transaction_t *txn = diskfs_journal_start_transaction ();

      if (nopen == 0 || (r < 3 && nopen < OPEN_MAX))
	{
	  /* dir_mkfile.  */
	  struct node *np;
	  pthread_mutex_lock (&root->lock);
	  error_t err = diskfs_create_node (root, NULL, S_IFREG | 0644, &np,
					    &cred, NULL);
	  pthread_mutex_unlock (&root->lock);
	  if (err)
	    {
	      CHECK (err == ENOSPC, "create: %s", strerror (err));
	      __atomic_add_fetch (&n_enospc, 1, __ATOMIC_RELAXED);
	    }
	  else
	    {
	      pthread_mutex_unlock (&np->lock);
	      open[nopen++] = np;
	      __atomic_add_fetch (&n_create, 1, __ATOMIC_RELAXED);
	    }
	}
      else
	{
	  int k = rand_r (&seed) % nopen;
	  struct node *np = open[k];
	  block_t len;

	  pthread_mutex_lock (&np->lock);
	  len = np->dn_stat.st_size >> log2_block_size;
	  if (r < 6 && len < MAXB)
	    {
	      /* Written to: blocks allocated as the pager does.  */
	      block_t to = len + 1 + rand_r (&seed) % 40, b;
	      if (to > MAXB)
		to = MAXB;
	      pthread_rwlock_wrlock (&diskfs_node_disknode (np)->alloc_lock);
	      for (b = len; b < to; b++)
		{
		  block_t disk;
		  error_t err = ext2_getblk (np, b, 1, &disk);
		  if (err)
		    {
		      CHECK (err == ENOSPC, "ext2_getblk: %s", strerror (err));
		      __atomic_add_fetch (&n_enospc, 1, __ATOMIC_RELAXED);
		      break;
		    }
		}
	      pthread_rwlock_unlock (&diskfs_node_disknode (np)->alloc_lock);
	      if (b > len)
		{
		  np->dn_stat.st_size = (off_t) b << log2_block_size;
		  np->allocsize = np->dn_stat.st_size;
		  np->dn_set_mtime = 1;
		  np->dn_stat_dirty = 1;
		}
	      diskfs_node_update (np, 0);
	      pthread_mutex_unlock (&np->lock);
	      __atomic_add_fetch (&n_grow, 1, __ATOMIC_RELAXED);
	    }
	  else if (r == 6)
	    {
	      /* dir_link, or dir_unlink if it has its name.  Locks the
		 directory first, as they do.  */
	      pthread_mutex_unlock (&np->lock);
	      pthread_mutex_lock (&root->lock);
	      pthread_mutex_lock (&np->lock);
	      if (np->dn_stat.st_nlink == 0)
		{
		  np->dn_stat.st_nlink++;
		  np->dn_set_ctime = 1;
		  if (!tree_dir_link && np->dn_stat.st_nlink == 1)
		    diskfs_orphan_del (np);
		  diskfs_node_update (np, 0);
		  dir_name (txn, np->cache_id, 0);
		  __atomic_add_fetch (&n_link, 1, __ATOMIC_RELAXED);
		}
	      else
		{
		  dir_name (txn, 0, np->cache_id);
		  np->dn_stat.st_nlink--;
		  np->dn_set_ctime = 1;
		  if (np->dn_stat.st_nlink == 0)
		    diskfs_orphan_add (np);
		  diskfs_node_update (np, 0);
		  __atomic_add_fetch (&n_unlink, 1, __ATOMIC_RELAXED);
		}
	      pthread_mutex_unlock (&np->lock);
	      pthread_mutex_unlock (&root->lock);
	    }
	  else if (r < 8)
	    {
	      block_t to = len ? rand_r (&seed) % len : 0;
	      CHECK (!diskfs_truncate (np, (off_t) to << log2_block_size),
		     "diskfs_truncate");
	      pthread_mutex_unlock (&np->lock);
	      __atomic_add_fetch (&n_trunc, 1, __ATOMIC_RELAXED);
	    }
	  else
	    {
	      /* The last close: without links, the node is dropped and
		 freed.  */
	      diskfs_nput (np);
	      open[k] = open[--nopen];
	      __atomic_add_fetch (&n_close, 1, __ATOMIC_RELAXED);
	    }
	}
      op_end (txn, &seed);
    }
  /* The rest stay open through the unmount.  */
  return NULL;
}

/* The orphan list of crash image IMG, which debugfs replayed: inodes
   without links only, and a chain that ends.  */
static const char *
check_list (const char *img)
{
  static char msg[128];
  struct ext2_super_block sb;
  struct ext2_inode di;
  FILE *f = fopen (img, "r");
  int steps = 0;

  CHECK (f, "open %s", img);
  CHECK (!fseeko (f, SBLOCK_OFFS, SEEK_SET) && fread (&sb, sizeof sb, 1, f)
	 == 1, "read the superblock of %s", img);
  for (uint32_t ino = le32toh (sb.s_last_orphan); ino;
       ino = le32toh (di.i_dtime))
    {
      block_t blk;
      size_t off;
      if (ino > le32toh (sb.s_inodes_count) || ++steps > 1000)
	{
	  snprintf (msg, sizeof msg, "the orphan list is corrupt at inode "
		    "%u", ino);
	  fclose (f);
	  return msg;
	}
      jt_inode_location (ino, &blk, &off);
      CHECK (!fseeko (f, ((off_t) blk << log2_block_size) + off, SEEK_SET)
	     && fread (&di, sizeof di, 1, f) == 1, "read inode %u", ino);
      if (di.i_links_count)
	{
	  snprintf (msg, sizeof msg, "inode %u is on the orphan list with "
		    "%u links", ino, le16toh (di.i_links_count));
	  fclose (f);
	  return msg;
	}
    }
  fclose (f);
  return NULL;
}

/* A crash image IMG: its list as debugfs replays it, then the usual
   replay and check.  */
static const char *
crash_fsck (const char *img, const char *out)
{
  static char msg[128];
  char dbg[300];
  const char *err;

  snprintf (dbg, sizeof dbg, "%s.dbg", img);
  CHECK (jt_run ("cp %s %s && debugfs -w -R journal_run %s >/dev/null 2>&1",
		 img, dbg, dbg) == 0, "debugfs replay of %s", img);
  err = check_list (dbg);
  unlink (dbg);
  if (err)
    return err;
  if (jt_run ("e2fsck -E journal_only -y %s >%s 2>&1", img, out) & ~1)
    return "e2fsck -E journal_only failed";
  int status = jt_run ("e2fsck -fn %s >%s 2>&1", img, out);
  if (status)
    {
      snprintf (msg, sizeof msg, "e2fsck -fn exit %d after the replay",
		status);
      return msg;
    }
  return NULL;
}

/* Mount IMAGE and recover its orphan list, as ext2fs does at mount, or
   with READONLY, at the mount, which must leave it, and when the
   filesystem becomes writable.  */
static int
recover (const char *image, int readonly)
{
  jt_mount (image, 2, 0);
  jt_inode_globals ();
  diskfs_readonly = readonly;
  uint32_t head = sblock->s_last_orphan;
  ext2_recover_orphan_list ();
  if (readonly)
    {
      CHECK (sblock->s_last_orphan == head, "orphans recovered read-only");
      /* diskfs_set_readonly (0).  */
      diskfs_readonly = 0;
      diskfs_readonly_changed (0);
    }
  jt_umount ();
  jt_save_image (image);
  /* It reports what it recovered as a warning; anything else is one.  */
  return 0;
}

int
main (int argc, char **argv)
{
  if (argc == 3 && !strcmp (argv[1], "--recover"))
    return recover (argv[2], 0);
  if (argc == 3 && !strcmp (argv[1], "--recover-ro"))
    return recover (argv[2], 1);

  const char *image = argc > 1 ? argv[1] : "build/balloc.img";
  unsigned seed = argc > 2 ? atoi (argv[2]) : 1;
  static uid_t ids[1];
  static struct idvec uids = { ids, 1, 1 }, gids = { ids, 1, 1 };
  pthread_t t[NTHREADS];

  user.uids = &uids;
  user.gids = &gids;
  tree_dir_link = getenv ("JT_TREE_DIR_LINK") != NULL;
  jt_mount (image, 2, 0);
  jt_inode_globals ();
  diskfs_transaction_t *txn = diskfs_journal_start_transaction ();
  CHECK (!diskfs_cached_lookup (EXT2_ROOT_INO, &root), "the root");
  root_blk = diskfs_node_disknode (root)->info.i_data[0];
  pthread_mutex_unlock (&root->lock);
  diskfs_journal_stop_transaction (txn);

  for (int i = 0; i < NTHREADS; i++)
    pthread_create (&t[i], NULL, worker,
		    (void *) (uintptr_t) (seed * 7919 + i));
  for (int i = 0; i < NTHREADS; i++)
    pthread_join (t[i], NULL);

  unsigned long warnings = jt_umount ();
  CHECK (warnings == 0, "%lu journal warnings", warnings);
  CHECK (jt_stale_reads () == 0, "%lu stale page-ins", jt_stale_reads ());

  jt_crash_fsck = crash_fsck;
  size_t checked = jt_crash_check ("t_orphan", NULL, NULL);
  jt_crash_fsck = NULL;

  /* The unmount's image, and some crash images: ext2fs releases the
     orphans.  */
  int recovered = 0, with_orphans = 0;
  size_t n_log = jt_log_count ();
  for (int i = 1; i <= 8; i++)
    {
      char img[64];
      size_t n = n_log * i / 8;
      snprintf (img, sizeof img, "build/t_orphan-recover-%zu.img", n);
      jt_build_image (n, 0, img);
      CHECK (jt_run ("debugfs -w -R journal_run %s >/dev/null 2>&1", img)
	     == 0, "replay of %s", img);
      with_orphans += jt_run ("dumpe2fs -h %s 2>/dev/null | grep -q "
			      "'^First orphan inode'", img) == 0;
      const char *how = i % 2 ? "--recover-ro" : "--recover";
      CHECK (jt_run ("%s %s %s >%s.out 2>&1", argv[0], how, img, img) == 0
	     && jt_run ("grep 'warning' %s.out | grep -v 'recovered [0-9]* "
			"orphan\\|orphan inodes on readonly\\|UNCLEANED' | "
			"grep -q .", img) != 0,
	     "%s %s failed (see %s.out)", how, img, img);
      CHECK (jt_run ("e2fsck -fn %s >%s.fsck 2>&1", img, img) == 0,
	     "%s is not clean after ext2_recover_orphan_list (see %s.fsck)",
	     img, img);
      jt_run ("rm -f %s %s.out %s.fsck", img, img, img);
      recovered++;
    }

  printf ("t_orphan: OK (%lu files created, %lu grows, %lu truncations, "
	  "%lu links, %lu unlinks, %lu closes, %lu ENOSPC, %zu writes, %zu "
	  "crash images, %d recovered by ext2fs, %d of them with orphans)\n",
	  n_create, n_grow, n_trunc, n_link, n_unlink, n_close, n_enospc,
	  n_log, checked, recovered, with_orphans);
  return 0;
}
