/* The orphan list, on the real orphan.c, ialloc.c, inode.c and hyper.c,
   with libdiskfs's diskfs_create_node, node cache, references and drop.

   Threads create anonymous files, as dir_mkfile does (diskfs_create_node
   with no name: no links), grow them through ext2_getblk, truncate them
   through diskfs_truncate, and close them: the last reference drops the
   node, which truncates it, takes it off the orphan list and frees it.
   Some are still open at the unmount.

   Every crash image, replayed, must be clean for e2fsck -fn: the replay
   (e2fsck -E journal_only) also releases the files on the orphan list,
   and a file left off it shows as an inode in use without links.  The
   image of the unmount, and some crash images replayed by debugfs, which
   leaves the list alone, are mounted again for ext2_recover_orphan_list
   to release the files (t_orphan --recover IMAGE), and must then be
   clean too.  */

#include <pthread.h>
#include "common.h"

#define NTHREADS 3
#define OPS 80
#define OPEN_MAX 4
#define MAXB 200

static struct node *root;
static struct iouser user;
static struct protid cred = { .user = &user };
static unsigned long n_create, n_grow, n_trunc, n_close, n_enospc;

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
	      /* The last close: the node is dropped and freed.  */
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

/* Mount IMAGE and recover its orphan list, as ext2fs does at mount.  */
static int
recover (const char *image)
{
  jt_mount (image, 2, 0);
  jt_inode_globals ();
  ext2_recover_orphan_list ();
  jt_umount ();
  jt_save_image (image);
  /* It reports what it recovered as a warning; anything else is one.  */
  return 0;
}

int
main (int argc, char **argv)
{
  if (argc == 3 && !strcmp (argv[1], "--recover"))
    return recover (argv[2]);

  const char *image = argc > 1 ? argv[1] : "build/balloc.img";
  unsigned seed = argc > 2 ? atoi (argv[2]) : 1;
  static uid_t ids[1];
  static struct idvec uids = { ids, 1, 1 }, gids = { ids, 1, 1 };
  pthread_t t[NTHREADS];

  user.uids = &uids;
  user.gids = &gids;
  jt_mount (image, 2, 0);
  jt_inode_globals ();
  diskfs_transaction_t *txn = diskfs_journal_start_transaction ();
  CHECK (!diskfs_cached_lookup (EXT2_ROOT_INO, &root), "the root");
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

  size_t checked = jt_crash_check ("t_orphan", NULL, NULL);

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
      CHECK (jt_run ("%s --recover %s >%s.out 2>&1", argv[0], img, img) == 0
	     && jt_run ("grep 'warning' %s.out | grep -qv 'recovered [0-9]* "
			"orphan'", img) != 0,
	     "recovering the orphans of %s failed (see %s.out)", img, img);
      CHECK (jt_run ("e2fsck -fn %s >%s.fsck 2>&1", img, img) == 0,
	     "%s is not clean after ext2_recover_orphan_list (see %s.fsck)",
	     img, img);
      jt_run ("rm -f %s %s.out %s.fsck", img, img, img);
      recovered++;
    }

  printf ("t_orphan: OK (%lu files created, %lu grows, %lu truncations, "
	  "%lu closes, %lu ENOSPC, %zu writes, %zu crash images, %d "
	  "recovered by ext2fs, %d of them with orphans)\n", n_create, n_grow,
	  n_trunc, n_close, n_enospc, n_log, checked, recovered,
	  with_orphans);
  return 0;
}
