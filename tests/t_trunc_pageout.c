/* A page-out of an indirect block while trunc_indirect edits it.

   trunc_indirect clears the entries of an indirect block that a
   truncation frees, and frees their blocks as it goes.  The block must
   be reserved in the transaction before the first entry is cleared: if
   no transaction holds it, a page-out in between (the kernel can write
   the page at any moment) finds no hazard and writes the edit home
   before the transaction that frees the blocks has committed.  A crash
   then leaves the blocks marked in use with nothing pointing to them.

   The steps are forced:
   1. pool0 and pool1 grow a block at a time, alternately, out into
      their single indirect blocks, so that pool0's blocks are not
      contiguous; that commits, and pool0's indirect block is written
      home, its page then left dirty with no transaction holding it.
   2. pool0 is truncated to a few blocks into its single indirect block
      ("part"), or to its direct blocks, which frees the indirect block
      ("whole").  Its blocks are not contiguous, so ext2_free_blocks runs
      for each one inside trunc_indirect's loop, and references the block
      bitmap.  At the first such reference after the first entry to go
      was cleared, the indirect block is paged out, and waited for.
   The indirect block's home copy must still map the freed blocks then,
   and every crash image must be clean after the replay.  Each case runs
   in a process of its own; the third argument picks one.  */

#include <pthread.h>
#include <sys/wait.h>
#include <unistd.h>
#include "common.h"

#define GROW 64			/* Entries each file grows to.  */

static unsigned keep;		/* Entries of the indirect block kept.  */

static struct node *np0, *np1;
static block_t ind;
static block_t old_entry;
static int armed, paged_out;
static block_t home_entry;

static ino_t
lookup (const char *image, const char *name)
{
  char cmd[512], line[256];
  unsigned long found = 0;

  snprintf (cmd, sizeof cmd, "debugfs -R 'stat /%s' %s 2>/dev/null",
	    name, image);
  FILE *p = popen (cmd, "r");
  CHECK (p, "debugfs");
  while (fgets (line, sizeof line, p))
    if (sscanf (line, "Inode: %lu", &found) == 1)
      break;
  pclose (p);
  CHECK (found, "no /%s in %s", name, image);
  return found;
}

/* 2.  */
static void
ref_hook (block_t b)
{
  uint32_t buf[block_size / 4];

  if (!__atomic_load_n (&armed, __ATOMIC_ACQUIRE) || b == ind)
    return;
  jt_read (ind, buf);
  if (buf[keep])
    return;
  __atomic_store_n (&armed, 0, __ATOMIC_RELEASE);
  jt_pageout (ind, 1, 1);
  home_entry = le32toh (((const uint32_t *) jt_disk_block (ind))[keep]);
  paged_out = 1;
}

static void
grow (struct node *np, block_t b)
{
  block_t disk;

  CHECK (!ext2_getblk (np, b, 1, &disk), "ext2_getblk");
  ext2_discard_prealloc (np);
  np->dn_stat.st_size = (off_t) (b + 1) << log2_block_size;
  np->allocsize = np->dn_stat.st_size;
}

static const char *
check (size_t n, int torn, const char *img, void *arg)
{
  (void) n;
  (void) torn;
  (void) img;
  (void) arg;
  return NULL;
}

static int
run (const char *image)
{
  ino_t ino0 = lookup (image, "pool0"), ino1 = lookup (image, "pool1");

  jt_mount (image, 3, 0);
  np0 = jt_node_load (ino0);
  np1 = jt_node_load (ino1);

  /* 1.  */
  diskfs_transaction_t *txn = diskfs_journal_start_transaction ();
  pthread_rwlock_wrlock (&diskfs_node_disknode (np0)->alloc_lock);
  pthread_rwlock_wrlock (&diskfs_node_disknode (np1)->alloc_lock);
  for (block_t b = 0; b < EXT2_NDIR_BLOCKS + GROW; b++)
    {
      grow (np0, b);
      grow (np1, b);
    }
  pthread_rwlock_unlock (&diskfs_node_disknode (np1)->alloc_lock);
  pthread_rwlock_unlock (&diskfs_node_disknode (np0)->alloc_lock);
  diskfs_node_update (np0, 0);
  diskfs_node_update (np1, 0);
  CHECK (!diskfs_journal_commit_transaction (txn), "the grow's commit");

  ind = diskfs_node_disknode (np0)->info.i_data[EXT2_IND_BLOCK];
  CHECK (ind, "pool0 has no single indirect block");
  /* Home, so that its home copy is the one before the truncation.  Then
     dirty again with no transaction holding it, as when the checkpoint
     wrote it home from the log and the kernel has not written the page
     yet: the harness sees a page dirtied only when ext2fs records the
     poke, not when trunc_indirect writes into it before that.  */
  jt_pageout (ind, 1, 1);
  pokel_add (&diskfs_node_disknode (np0)->indir_pokel,
	     disk_cache_block_ref (ind), block_size);
  const uint32_t *e = jt_disk_block (ind);
  CHECK (le32toh (e[keep]) && le32toh (e[keep + 1]) != le32toh (e[keep]) + 1,
	 "pool0's blocks %u and %u are contiguous", le32toh (e[keep]),
	 le32toh (e[keep + 1]));
  old_entry = le32toh (e[keep]);

  /* 2.  */
  JT_SET_HOOK (jt_ref_hook, ref_hook);
  __atomic_store_n (&armed, 1, __ATOMIC_RELEASE);
  txn = diskfs_journal_start_transaction ();
  pthread_mutex_lock (&np0->lock);
  CHECK (!diskfs_truncate (np0, (off_t) (EXT2_NDIR_BLOCKS + keep)
			   << log2_block_size), "diskfs_truncate");
  pthread_mutex_unlock (&np0->lock);
  CHECK (!diskfs_journal_commit_transaction (txn), "the truncation's commit");
  JT_SET_HOOK (jt_ref_hook, NULL);
  CHECK (paged_out, "the indirect block was never paged out during the "
	 "truncation");
  CHECK (home_entry == old_entry, "the page-out wrote the truncation's "
	 "edit of indirect block %u home before its commit: entry %d is %u "
	 "there, %u expected", ind, keep, home_entry, old_entry);

  unsigned long warnings = jt_umount ();
  CHECK (warnings == 0, "%lu journal warnings", warnings);
  CHECK (jt_stale_reads () == 0, "%lu stale page-ins", jt_stale_reads ());
  size_t checked = jt_crash_check (keep ? "t_trunc_pageout-part"
				   : "t_trunc_pageout-whole", check, NULL);
  printf ("t_trunc_pageout: OK (%s: %zu writes, %zu crash images)\n",
	  keep ? "part" : "whole", jt_log_count (), checked);
  return 0;
}

int
main (int argc, char **argv)
{
  static const char *const names[] = { "part", "whole" };
  static const unsigned keeps[] = { 4, 0 };
  const char *image = argc > 1 ? argv[1] : "build/balloc.img";
  int status;

  for (int m = 0; m < 2; m++)
    {
      if (argc > 3 && strcmp (argv[3], names[m]))
	continue;
      fflush (NULL);
      pid_t pid = fork ();
      CHECK (pid >= 0, "fork");
      if (pid == 0)
	{
	  keep = keeps[m];
	  exit (run (image));
	}
      CHECK (waitpid (pid, &status, 0) == pid, "wait");
      if (!WIFEXITED (status) || WEXITSTATUS (status))
	return 1;
    }
  return 0;
}
