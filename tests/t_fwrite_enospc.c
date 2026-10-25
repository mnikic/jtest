/* io_write on a full disk whose only free blocks wait for a commit, with
   libdiskfs's io-write.c and rdwr-internal.c over the harness's file
   pager.

   Blocks freed in a transaction stay busy until it commits.  A write
   allocates its blocks in a pager thread (pager_unlock_page), whose
   handle joins the running transaction and never waits; when the only
   free blocks are that transaction's own frees, the allocation fails
   with ENOSPC, although they come back at the next commit.  io_write
   must try again once it has let go of its handle and the node lock
   (diskfs_journal_should_retry_alloc), as ext4 does.

   The disk (full.img) is filled first, by a file written a block at a
   time until ENOSPC, and committed.  Then:

   1. Own frees.  A handle truncates file F, freeing 8 blocks, and ends
      without a commit.  A write of 4 blocks to a new file H must
      succeed, and read back.
   2. Append.  The disk filled again, the same with file F2 (4 blocks)
      and an O_APPEND write of 4 blocks to file A, one block long: A
      must end 5 blocks long, the data at block 1, and the file pointer
      at the end.
   3. No space.  Everything committed and the disk filled again, a write
      of 4 more blocks to a new file must fail with ENOSPC, without
      waiting for anything.

   Each case starts from a full disk: the one before can leave blocks
   free, and a write that fits would not need the retry.

   After the unmount and in every crash image, e2fsck -fn is clean.  */

#include "files.h"
#include "io_S.h"

static struct peropen po_root;
static struct protid cred_root;

struct file
{
  struct node *np;
  struct peropen po;
  struct protid cred;
};

static void
make_file (struct file *f, const char *name, int openstat)
{
  struct dirstat *ds = alloca (diskfs_dirstat_size);
  diskfs_transaction_t *txn = diskfs_journal_start_transaction ();
  pthread_mutex_lock (&root->lock);
  CHECK (diskfs_lookup (root, (char *) name, CREATE, &f->np, ds, &cred_root)
	 == ENOENT, "lookup %s", name);
  CHECK (!diskfs_create_node (root, (char *) name, S_IFREG | 0644, &f->np,
			      &cred_root, ds), "create %s", name);
  pthread_mutex_unlock (&root->lock);
  pthread_mutex_unlock (&f->np->lock);
  CHECK (!jt_commit (txn), "commit %s", name);
  memset (&f->po, 0, sizeof f->po);
  f->po.np = f->np;
  f->po.openstat = O_READ | O_WRITE | openstat;
  f->cred = cred;
  f->cred.po = &f->po;
}

/* Fill BUF with blocks of FILL, block N of it with FILL + N.  */
static void
fill_blocks (char *buf, size_t blocks, int fill)
{
  for (size_t i = 0; i < blocks; i++)
    memset (buf + i * block_size, fill + (int) i, block_size);
}

static error_t
write_at (struct file *f, off_t off, size_t blocks, int fill,
	  vm_size_t *amt)
{
  char *buf = malloc (blocks * block_size);
  fill_blocks (buf, blocks, fill);
  *amt = 0;
  error_t err = diskfs_S_io_write (&f->cred, buf, blocks * block_size, off,
				   amt);
  free (buf);
  return err;
}

/* F's bytes at OFF, BLOCKS blocks of them, are pattern FILL.  */
static int
reads_back (struct file *f, off_t off, size_t blocks, int fill)
{
  char *want = malloc (blocks * block_size);
  char *got = malloc (blocks * block_size);
  mach_msg_type_number_t n;
  fill_blocks (want, blocks, fill);
  pthread_mutex_lock (&f->np->lock);
  error_t err = diskfs_node_rdwr (f->np, got, off, blocks * block_size, 0,
				  &f->cred, &n);
  pthread_mutex_unlock (&f->np->lock);
  int ok = !err && n == blocks * block_size
    && !memcmp (want, got, blocks * block_size);
  free (want);
  free (got);
  return ok;
}

/* Truncate F to nothing in a handle of its own, without a commit: its
   blocks are busy in the running transaction.  */
static void
free_file (struct file *f)
{
  diskfs_transaction_t *txn = diskfs_journal_start_transaction ();
  pthread_mutex_lock (&f->np->lock);
  CHECK (!diskfs_truncate (f->np, 0), "truncate");
  pthread_mutex_unlock (&f->np->lock);
  diskfs_journal_stop_transaction (txn);
}

static void
commit_all (void)
{
  CHECK (!jt_commit (diskfs_journal_start_transaction ()), "commit");
}

/* The rest of the disk, a block at a time, into G after its *FILLED
   blocks, and committed.  */
static void
fill (struct file *g, size_t *filled)
{
  vm_size_t amt;
  error_t err;
  for (;; ++*filled)
    {
      err = write_at (g, *filled * block_size, 1, 0x40, &amt);
      if (err)
	break;
    }
  CHECK (err == ENOSPC, "filling: %s", strerror (err));
  commit_all ();
}

int
main (int argc, char **argv)
{
  const char *image = argc > 1 ? argv[1] : "build/full.img";
  struct file f, f2, a, g, h, n;
  vm_size_t amt;
  error_t err;

  /* The pager says so at each ENOSPC.  */
  jt_expected_warning = "out of space";
  jt_mount (image, 2, 0);
  jt_inode_globals ();
  files_init ();
  diskfs_root_node = root;
  cred_root = cred;
  cred_root.po = &po_root;
  po_root.np = root;

  make_file (&f, "F", 0);
  make_file (&f2, "F2", 0);
  make_file (&a, "A", O_APPEND);
  make_file (&g, "G", 0);
  CHECK (!write_at (&f, 0, 8, 0x10, &amt) && amt == 8 * block_size,
	 "write F");
  CHECK (!write_at (&f2, 0, 4, 0x20, &amt) && amt == 4 * block_size,
	 "write F2");
  CHECK (!write_at (&a, -1, 1, 0x30, &amt) && amt == block_size, "write A");
  commit_all ();

  size_t filled = 0;
  fill (&g, &filled);
  CHECK (filled > 0, "the disk was full already");

  /* 1.  */
  make_file (&h, "H", 0);
  free_file (&f);
  err = write_at (&h, 0, 4, 0x50, &amt);
  CHECK (!err && amt == 4 * block_size, "own frees: the write of 4 blocks "
	 "gave %s, %lu bytes", err ? strerror (err) : "success",
	 (unsigned long) amt);
  CHECK (reads_back (&h, 0, 4, 0x50), "own frees: H does not read back");

  /* 2.  Case 1 left 4 blocks free.  */
  commit_all ();
  fill (&g, &filled);
  free_file (&f2);
  err = write_at (&a, -1, 4, 0x60, &amt);
  CHECK (!err && amt == 4 * block_size, "append: the write of 4 blocks "
	 "gave %s, %lu bytes", err ? strerror (err) : "success",
	 (unsigned long) amt);
  CHECK (a.np->dn_stat.st_size == 5 * (off_t) block_size,
	 "append: A is %lld bytes, not 5 blocks",
	 (long long) a.np->dn_stat.st_size);
  CHECK (a.po.filepointer == 5 * (off_t) block_size,
	 "append: the file pointer is at %lld, not 5 blocks",
	 (long long) a.po.filepointer);
  CHECK (reads_back (&a, 0, 1, 0x30) && reads_back (&a, block_size, 4, 0x60),
	 "append: A does not read back");

  /* 3.  */
  commit_all ();
  fill (&g, &filled);
  make_file (&n, "N", 0);
  struct timespec t0, t1;
  clock_gettime (CLOCK_MONOTONIC, &t0);
  err = write_at (&n, 0, 4, 0x70, &amt);
  clock_gettime (CLOCK_MONOTONIC, &t1);
  CHECK (err == ENOSPC, "no space: the write gave %s",
	 err ? strerror (err) : "success");
  double secs = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
  CHECK (secs < 1, "no space: the write took %.1f s", secs);

  diskfs_transaction_t *txn = diskfs_journal_start_transaction ();
  struct file *all[] = { &f, &f2, &a, &g, &h, &n };
  for (size_t i = 0; i < sizeof all / sizeof *all; i++)
    diskfs_nrele (all[i]->np);
  diskfs_journal_stop_transaction (txn);
  unsigned long warnings = jt_umount ();
  CHECK (warnings == 0, "%lu journal warnings", warnings);
  const char *out = "build/t_fwrite_enospc.img";
  jt_save_image (out);
  CHECK (jt_run ("e2fsck -fn %s >%s.fsck 2>&1", out, out) == 0,
	 "e2fsck -fn %s is not clean after the unmount (see %s.fsck)", out,
	 out);
  size_t checked = jt_crash_check ("t_fwrite_enospc", NULL, NULL);
  printf ("t_fwrite_enospc: OK (%zu blocks filled; %zu writes, %zu crash "
	  "images)\n", filled, jt_log_count (), checked);
  return 0;
}
