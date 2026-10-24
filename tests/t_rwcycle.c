/* Writable, read-only, writable again: the real diskfs_readonly_changed
   both ways, on a filesystem whose files are in use (tests/files.h).

   Threads work on their files for a round, and between rounds the
   filesystem goes read-only as diskfs_set_readonly does it, with no
   handle open (the RPCs are inhibited): diskfs_sync_everything's commit
   of the inodes, the superblock, then diskfs_readonly_changed (1), which
   stops the journal.  The files stay open; those without links stay on
   the orphan list.  Then it becomes writable again
   (diskfs_readonly_changed (0)): a new journal starts on the log the old
   one emptied, and the orphan list is left alone, as its files are still
   open.  Files kept open across a switch are only named, unnamed and
   closed after it, as if open read-only.

   While read-only: the journal is stopped, its log is empty with no
   error, needs_recovery is clear, and every block's latest version is
   home.  ext2fs's view of the disk cache is read-only then, so a write
   to it faults.  After: the new journal's transactions are numbered past
   every transaction in the log, so that a replay cannot take an old one
   for a new one.

   During each switch a reader keeps evicting the blocks the files live
   in and reading them back, as faults on files being read go on while
   the RPCs are inhibited, and every read of the store is held a while:
   page-ins are in flight when the journal stops.  journal_store_read
   locks the journal it started with again after the read, so the
   stopped journal must still be there (cbb7243b3).  Every crash image, on either side of a switch, must
   hold an orphan list of files without links and be clean after the
   replay (tests/files.h).  */

#include "files.h"
#include "jbd2_format.h"

#define NTHREADS 3
#define ROUNDS 3
#define OPS 40

/* The highest transaction number in a log block among writes FROM to
   TO.  */
static uint32_t
max_tid (size_t from, size_t to)
{
  uint32_t max = 0;

  for (size_t i = from; i < to; i++)
    {
      const struct jt_write *w = jt_log_entry (i);
      const journal_header_t *h = w->data;
      if (!w->data || !jt_in_journal (w->off) || jt_journal_index (w->off) == 0
	  || be32toh (h->h_magic) != JBD2_MAGIC_NUMBER)
	continue;
      uint32_t type = be32toh (h->h_blocktype);
      if ((type == JBD2_DESCRIPTOR_BLOCK || type == JBD2_COMMIT_BLOCK
	   || type == JBD2_REVOKE_BLOCK) && be32toh (h->h_sequence) > max)
	max = be32toh (h->h_sequence);
    }
  return max;
}

/* The lowest transaction number in a log block from write FROM on, or
   0.  */
static uint32_t
min_tid (size_t from)
{
  uint32_t min = 0;

  for (size_t i = from; i < jt_log_count (); i++)
    {
      const struct jt_write *w = jt_log_entry (i);
      const journal_header_t *h = w->data;
      if (!w->data || !jt_in_journal (w->off) || jt_journal_index (w->off) == 0
	  || be32toh (h->h_magic) != JBD2_MAGIC_NUMBER
	  || be32toh (h->h_blocktype) == JBD2_SUPERBLOCK_V2
	  || be32toh (h->h_blocktype) == JBD2_SUPERBLOCK_V1)
	continue;
      if (!min || be32toh (h->h_sequence) < min)
	min = be32toh (h->h_sequence);
    }
  return min;
}

/* The reader, during a switch.  */
static int switching, reader_stop;
static unsigned long n_reads, n_held;

static void
hold_read (uint64_t off, size_t len)
{
  (void) off;
  (void) len;
  /* A page-in on a pager thread.  */
  if (__atomic_load_n (&switching, __ATOMIC_ACQUIRE) && jt_on_pager_thread ())
    {
      __atomic_add_fetch (&n_held, 1, __ATOMIC_RELAXED);
      usleep (2000);
    }
}

static void *
reader (void *arg)
{
  char *buf = malloc (block_size);
  unsigned seed = (unsigned) (uintptr_t) arg;

  while (!__atomic_load_n (&reader_stop, __ATOMIC_ACQUIRE))
    {
      unsigned long g = rand_r (&seed) % groups_count;
      const struct ext2_group_desc *gd = group_desc (g);
      block_t b;
      switch (rand_r (&seed) % 3)
	{
	case 0:
	  b = le32toh (gd->bg_block_bitmap);
	  break;
	case 1:
	  b = le32toh (gd->bg_inode_bitmap);
	  break;
	default:
	  b = le32toh (gd->bg_inode_table) + rand_r (&seed) % itb_per_group;
	}
      jt_pageout (b, 1, 1);
      jt_read (b, buf);
      __atomic_add_fetch (&n_reads, 1, __ATOMIC_RELAXED);
    }
  free (buf);
  return NULL;
}

static void
to_readonly (void)
{
  /* diskfs_set_readonly (1), the RPCs inhibited: diskfs_sync_everything
     commits the inodes first.  */
  diskfs_transaction_t *txn = diskfs_journal_start_transaction ();
  write_all_disknodes ();
  CHECK (!jt_commit (txn), "commit the inodes");
  diskfs_set_hypermetadata (1, 1);
  diskfs_readonly = 1;
  diskfs_readonly_changed (1);

  CHECK (!ext2_journal, "the journal still runs read-only");
  CHECK (jt_journal_check () == JOURNAL_USABLE, "the log is not empty, or "
	 "has an error, read-only");
  const struct ext2_super_block *sb = (const struct ext2_super_block *)
    ((const char *) jt_disk_block (0) + SBLOCK_OFFS);
  CHECK (!(sb->s_feature_incompat
	   & htole32 (EXT3_FEATURE_INCOMPAT_RECOVER)),
	 "needs_recovery still set on disk read-only");
  size_t n;
  block_t b = jt_unwritten (&n);
  CHECK (n == 0, "%zu blocks not home read-only, the first %u", n, b);
}

static void
to_writable (void)
{
  /* diskfs_set_readonly (0).  */
  diskfs_readonly = 0;
  diskfs_readonly_changed (0);
  CHECK (ext2_journal, "no journal writable again");
}

int
main (int argc, char **argv)
{
  const char *image = argc > 1 ? argv[1] : "build/balloc.img";
  unsigned seed = argc > 2 ? atoi (argv[2]) : 1;
  static struct files files[NTHREADS];
  size_t restart_at[ROUNDS];
  uint32_t last_tid[ROUNDS];
  pthread_t t[NTHREADS];
  int kept_orphans = 0;

  jt_mount (image, 2, 0);
  jt_inode_globals ();
  files_init ();
  JT_SET_HOOK (jt_store_read_hook, hold_read);
  /* hyper.c's ext2fs_clean is set by get_hypermetadata, which the
     harness does not run.  */
  jt_expected_warning = "UNCLEANED FILESYSTEM NOW WRITABLE";

  for (int i = 0; i < NTHREADS; i++)
    {
      files[i].seed = seed * 7919 + i;
      files[i].ops = OPS;
    }
  for (int round = 0; round < ROUNDS; round++)
    {
      for (int i = 0; i < NTHREADS; i++)
	pthread_create (&t[i], NULL, files_worker, &files[i]);
      for (int i = 0; i < NTHREADS; i++)
	pthread_join (t[i], NULL);
      if (round == ROUNDS - 1)
	break;

      for (int i = 0; i < NTHREADS; i++)
	{
	  memset (files[i].kept, 1, sizeof files[i].kept);
	  for (int k = 0; k < files[i].nopen; k++)
	    kept_orphans += files[i].open[k]->dn_stat.st_nlink == 0;
	}
      pthread_t rd;
      __atomic_store_n (&reader_stop, 0, __ATOMIC_RELEASE);
      __atomic_store_n (&switching, 1, __ATOMIC_RELEASE);
      pthread_create (&rd, NULL, reader, (void *) (uintptr_t) (seed + round));
      usleep (5000);
      to_readonly ();
      last_tid[round] = max_tid (0, jt_log_count ());
      restart_at[round] = jt_log_count ();
      to_writable ();
      usleep (5000);
      __atomic_store_n (&switching, 0, __ATOMIC_RELEASE);
      __atomic_store_n (&reader_stop, 1, __ATOMIC_RELEASE);
      pthread_join (rd, NULL);
    }

  unsigned long warnings = jt_umount ();
  CHECK (warnings == 0, "%lu journal warnings", warnings);
  CHECK (jt_stale_reads () == 0, "%lu stale page-ins", jt_stale_reads ());

  for (int round = 0; round < ROUNDS - 1; round++)
    {
      uint32_t first = min_tid (restart_at[round]);
      CHECK (!first || first > last_tid[round], "the journal restarted at "
	     "transaction %u, but the log had reached %u", first,
	     last_tid[round]);
    }

  jt_crash_fsck = crash_fsck;
  size_t checked = jt_crash_check ("t_rwcycle", NULL, NULL);
  jt_crash_fsck = NULL;

  printf ("t_rwcycle: OK (%d read-only switches with %d orphans open and "
	  "%lu page-ins, %lu of them held; %lu files created, %lu links, %lu "
	  "unlinks, %lu closes, %zu writes, %zu crash images)\n", ROUNDS - 1,
	  kept_orphans, n_reads, n_held, n_create, n_link, n_unlink, n_close,
	  jt_log_count (), checked);
  return 0;
}
