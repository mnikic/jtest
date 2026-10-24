/* An aborted commit under the file workload (tests/files.h), on the real
   inode.c, ialloc.c, orphan.c and hyper.c.

   A write of a log block fails partway through, with files open, nodes
   changed and files without links on the orphan list.  The commit
   aborts: from then on every commit fails with EIO, and kjournald makes
   the filesystem read-only with the RPCs inhibited, while the threads
   go on.  After that every operation but a close fails with EROFS; a
   close drops the node read-only, which writes it (write_node on an
   aborted journal) and leaves it on the orphan list.  The translator
   then goes away without stopping the journal, as diskfs_shutdown does
   read-only.

   What must hold: no stale page-in, also when every block the files
   live in is evicted and read back after the abort (the aborted
   transaction's shadows are the only copy of some), read-only set with
   no RPC in flight,
   a journal that ext2fs refuses to mount before e2fsck (s_errno), and
   every crash image an orphan list of files without links, clean after
   the replay.  */

#include "files.h"

#define NTHREADS 3
#define OPS 120

static int matches, failed, fail_at = 120;
static size_t failed_at;

/* Fail the FAIL_AT-th log block that is not a descriptor, a commit
   record or the journal superblock.  */
static int
fail_hook (uint64_t off, size_t len, const void *buf)
{
  const uint32_t *w = buf;

  if (failed || !jt_in_journal (off) || jt_journal_index (off) == 0
      || (len >= 8 && be32toh (w[0]) == 0xC03B3998U))
    return 0;
  if (++matches < fail_at)
    return 0;
  failed = 1;
  failed_at = jt_log_count ();
  return EIO;
}

int
main (int argc, char **argv)
{
  const char *image = argc > 1 ? argv[1] : "build/balloc.img";
  unsigned seed = argc > 2 ? atoi (argv[2]) : 1;
  static struct files files[NTHREADS];
  pthread_t t[NTHREADS];
  int ro, inhibited;

  jt_mount (image, 2, 0);
  jt_inode_globals ();
  files_init ();
  files_eio_ok = 1;
  jt_store_fail_hook = fail_hook;

  for (int i = 0; i < NTHREADS; i++)
    {
      files[i].seed = seed * 7919 + i;
      files[i].ops = OPS;
      pthread_create (&t[i], NULL, files_worker, &files[i]);
    }
  for (int i = 0; i < NTHREADS; i++)
    pthread_join (t[i], NULL);

  CHECK (failed, "no log write failed");
  for (int waited = 0; !(ro = jt_readonly (&inhibited)) && waited < 150;
       waited++)
    usleep (100000);
  CHECK (ro, "not read-only after the abort");
  CHECK (inhibited, "read-only, but not with the RPCs inhibited");

  /* Every bitmap and inode table block, and the root's, evicted and read
     back.  */
  char *buf = malloc (block_size);
  int reread = 0;
  for (unsigned long g = 0; g < groups_count; g++)
    {
      const struct ext2_group_desc *gd = group_desc (g);
      block_t first = le32toh (gd->bg_inode_table);
      block_t blocks[2] = { le32toh (gd->bg_block_bitmap),
			    le32toh (gd->bg_inode_bitmap) };
      for (int i = 0; i < 2; i++, reread++)
	{
	  jt_pageout (blocks[i], 1, 1);
	  jt_read (blocks[i], buf);
	}
      for (unsigned long i = 0; i < itb_per_group; i++, reread++)
	{
	  jt_pageout (first + i, 1, 1);
	  jt_read (first + i, buf);
	}
    }
  jt_dir_pageout (root_blk, 1, 1);
  jt_dir_read (root_blk, buf);
  free (buf);
  CHECK (jt_stale_reads () == 0, "%lu stale page-ins", jt_stale_reads ());

  /* diskfs_shutdown, read-only: no sync, the journal left as it is.  */
  jt_abandon ();
  jt_store_fail_hook = NULL;
  CHECK (jt_journal_check () == JOURNAL_NEEDS_REPLAY,
	 "the aborted journal could be mounted before e2fsck");

  jt_crash_fsck = crash_fsck;
  size_t checked = jt_crash_check ("t_abortfiles", NULL, NULL);
  jt_crash_fsck = NULL;

  int open_orphans = 0;
  for (int i = 0; i < NTHREADS; i++)
    for (int k = 0; k < files[i].nopen; k++)
      open_orphans += files[i].open[k]->dn_stat.st_nlink == 0;
  printf ("t_abortfiles: OK (log write %zu failed; %lu files created, "
	  "%lu touched, %lu closes, %lu of them read-only, %lu EROFS, %lu "
	  "commits failed, %d orphans still open; %zu writes, %zu crash "
	  "images, %d blocks read back)\n", failed_at, n_create, n_touch,
	  n_close, n_ro_close, n_erofs, n_eio, open_orphans, jt_log_count (),
	  checked, reread + 1);
  return 0;
}
