/* Files for the tests on the real inode.c, ialloc.c, orphan.c and
   hyper.c (t_orphan, t_rwcycle): anonymous files that threads create,
   grow, truncate, name and unname in the root directory, and close; and
   the check of a crash image's orphan list.

   The harness has no dir.c: the names are written into the root's first
   directory block here, and the node steps are dir_link's and
   dir_unlink's, copied: a link from no links takes the file off the
   orphan list, an unlink to none puts it on.  JT_TREE_DIR_LINK=1 copies
   the dir_link before b3335f38e, which left it there.  */

#ifndef JTEST_FILES_H
#define JTEST_FILES_H

#include <pthread.h>
#include <unistd.h>
#include "common.h"

#define OPEN_MAX 4
#define MAXB 200

static struct node *root;
static block_t root_blk;	/* The root's first directory block.  */
static int tree_dir_link;
static struct iouser user;
static struct protid cred = { .user = &user };
static unsigned long n_create, n_grow, n_trunc, n_close, n_enospc;
static unsigned long n_link, n_unlink;

/* One thread's files.  A file KEPT open across a switch to read-only
   was open read-only then: it is only named, unnamed and closed.  */
struct files
{
  struct node *open[OPEN_MAX];
  unsigned char kept[OPEN_MAX];
  int nopen;
  unsigned seed;
  int ops;
};

/* After the mount: the root, and a user to create files as.  */
static void
files_init (void)
{
  static uid_t ids[1];
  static struct idvec uids = { ids, 1, 1 }, gids = { ids, 1, 1 };

  user.uids = &uids;
  user.gids = &gids;
  tree_dir_link = getenv ("JT_TREE_DIR_LINK") != NULL;
  diskfs_transaction_t *txn = diskfs_journal_start_transaction ();
  CHECK (!diskfs_cached_lookup (EXT2_ROOT_INO, &root), "the root");
  root_blk = diskfs_node_disknode (root)->info.i_data[0];
  pthread_mutex_unlock (&root->lock);
  diskfs_journal_stop_transaction (txn);
}

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

/* dir_link, or dir_unlink if NP has its name.  Locks the directory
   first, as they do.  */
static void
file_link (diskfs_transaction_t *txn, struct node *np)
{
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

/* One operation on F's files, in a handle of its own.  */
static void
file_op (struct files *f)
{
  int r = rand_r (&f->seed) % 10;
  diskfs_transaction_t *txn = diskfs_journal_start_transaction ();

  if (f->nopen == 0 || (r < 3 && f->nopen < OPEN_MAX))
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
	  f->kept[f->nopen] = 0;
	  f->open[f->nopen++] = np;
	  __atomic_add_fetch (&n_create, 1, __ATOMIC_RELAXED);
	}
    }
  else
    {
      int k = rand_r (&f->seed) % f->nopen;
      struct node *np = f->open[k];
      block_t len;

      if (f->kept[k])
	r = r < 5 ? 6 : 9;
      pthread_mutex_lock (&np->lock);
      len = np->dn_stat.st_size >> log2_block_size;
      if (r < 6 && len < MAXB)
	{
	  /* Written to: blocks allocated as the pager does.  */
	  block_t to = len + 1 + rand_r (&f->seed) % 40, b;
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
	  pthread_mutex_unlock (&np->lock);
	  file_link (txn, np);
	}
      else if (r < 8)
	{
	  block_t to = len ? rand_r (&f->seed) % len : 0;
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
	  f->nopen--;
	  f->open[k] = f->open[f->nopen];
	  f->kept[k] = f->kept[f->nopen];
	  __atomic_add_fetch (&n_close, 1, __ATOMIC_RELAXED);
	}
    }
  if (rand_r (&f->seed) % 4 == 0)
    CHECK (!jt_commit (txn), "commit");
  else
    diskfs_journal_stop_transaction (txn);
}

/* A thread's F->ops operations on its files F.  */
static void *
files_worker (void *arg)
{
  struct files *f = arg;

  for (int op = 0; op < f->ops; op++)
    file_op (f);
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

/* A crash image IMG (jt_crash_fsck): its list as debugfs replays it,
   then the usual replay and check.  */
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

#endif
