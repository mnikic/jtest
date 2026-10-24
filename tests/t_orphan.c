/* The orphan list, on the real orphan.c, ialloc.c, inode.c and hyper.c,
   with libdiskfs's diskfs_create_node, node cache, references and drop.

   Threads create anonymous files, as dir_mkfile does (diskfs_create_node
   with no name: no links), grow them through ext2_getblk, truncate them
   through diskfs_truncate, give them a name in the root directory and
   take it away again, and close them (tests/files.h): the last
   reference to a file without links drops the node, which truncates it,
   takes it off the orphan list and frees it.  Some are still open at the
   unmount.

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

#include "files.h"

#define NTHREADS 3
#define OPS 80

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
  static struct files files[NTHREADS];
  pthread_t t[NTHREADS];

  jt_mount (image, 2, 0);
  jt_inode_globals ();
  files_init ();

  for (int i = 0; i < NTHREADS; i++)
    {
      files[i].seed = seed * 7919 + i;
      files[i].ops = OPS;
      pthread_create (&t[i], NULL, files_worker, &files[i]);
    }
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
