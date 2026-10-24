/* The corners of the log format, against e2fsck's replay.

   escape        A metadata block that starts with the JBD2 magic number
		 is written to the log with its first word zeroed and the
		 tag's ESCAPE flag set; the replay puts the magic back.
		 While the commit writes such a block, the block is read
		 back after a clean eviction, and evicted dirty: the shadow
		 must still hold the magic.
   big           One transaction of 1100 blocks, half of them escaped,
		 takes three descriptor blocks; one that frees them all
		 takes two revoke blocks.  The blocks are then file data,
		 written home, which no replay may undo.
   escape-abort  A log write fails in the second of three descriptor
		 batches of an escaped transaction whose pages the pager
		 has handed over.  After the abort they are read back.
   oversize      A transaction larger than the log is refused before
		 any of it reaches the log.

   Every crash image must be clean for e2fsck -fn and hold whole
   transactions.  The default runs all four, each in a process of its
   own; a part's name as the third argument runs it alone.  big and
   escape-abort use build/big.img, whose journal holds 4096 blocks.  */

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include "common.h"
#include "jbd2_format.h"

#define BIG 1100
#define DATA_WORD 0x80000001u

static const char *big_image = "build/big.img";

/* Version V of block B, starting with the JBD2 magic number.  */
static void
mpattern (void *buf, block_t b, uint32_t v)
{
  pattern (buf, b, v);
  ((uint32_t *) buf)[0] = htobe32 (JBD2_MAGIC_NUMBER);
}

/* The version of B in BUF, written by mpattern if MAGIC, by pattern if
   not; -1 if neither (the magic missing, say).  */
static long
mversion (const void *buf, block_t b, int magic)
{
  uint32_t w[block_size / 4];

  memcpy (w, buf, block_size);
  if (magic && w[0] == htobe32 (JBD2_MAGIC_NUMBER))
    w[0] = b;
  else if (magic && pattern_version (w, b) != 0)
    return -1;
  return pattern_version (w, b);
}

static void
journal_mversion (diskfs_transaction_t *txn, block_t b, uint32_t v, int magic)
{
  char buf[block_size];

  CHECK (!journal_get_write_access (txn, b), "write access to %u", b);
  if (magic)
    mpattern (buf, b, v);
  else
    pattern (buf, b, v);
  jt_edit (b, 0, buf, block_size);
  journal_mark_dirty (txn, b);
}

/* Read block B of crash image IMG.  */
static void
image_block (FILE *f, block_t b, void *buf)
{
  CHECK (!fseeko (f, (off_t) b << log2_block_size, SEEK_SET)
	 && fread (buf, block_size, 1, f) == 1, "read block %u", b);
}

static int
is_header (const struct jt_write *w, uint32_t type)
{
  const journal_header_t *h = w->data;
  return w->data && jt_in_journal (w->off) && jt_journal_index (w->off) != 0
    && be32toh (h->h_magic) == JBD2_MAGIC_NUMBER
    && be32toh (h->h_blocktype) == type;
}

/* The descriptor blocks of the log from write FROM on: how many there
   are, and in FLAGS[i] the flags of the last tag found for BLOCKS[i] (or
   -1).  */
static int
scan_descriptors (size_t from, const block_t *blocks, int n, long *flags)
{
  int descriptors = 0;

  for (int i = 0; i < n; i++)
    flags[i] = -1;
  for (size_t i = from; i < jt_log_count (); i++)
    {
      const struct jt_write *w = jt_log_entry (i);
      if (!is_header (w, JBD2_DESCRIPTOR_BLOCK))
	continue;
      descriptors++;
      const journal_block_tag_t *t =
	(const journal_block_tag_t *) ((const char *) w->data
				       + sizeof (journal_header_t));
      for (;; t++)
	{
	  uint32_t f = be32toh (t->t_flags);
	  for (int k = 0; k < n; k++)
	    if (blocks[k] == be32toh (t->t_blocknr))
	      flags[k] = f;
	  if (f & JBD2_FLAG_LAST_TAG)
	    break;
	}
    }
  return descriptors;
}

/* ---------------------------------------------------------------- */
/* escape */

static block_t x_blk;
static size_t x_synced[8];
static int x_nver;
static enum { HOOK_NONE, HOOK_READ, HOOK_EVICT } x_hook;

/* While the commit writes X's escaped copy of version x_nver.  */
static void
escape_hook (uint64_t off, size_t len, const void *buf)
{
  const uint32_t *w = buf;

  if (x_hook == HOOK_NONE || !jt_in_journal (off) || len != block_size
      || w[0] != 0 || w[1] != (uint32_t) x_nver || w[2] != x_blk)
    return;
  if (x_hook == HOOK_READ)
    {
      static char got[4096];
      jt_read (x_blk, got);
    }
  else
    jt_pageout (x_blk, 1, 1);
  x_hook = HOOK_NONE;
}

static const char *
escape_check (size_t n, int torn, const char *img, void *arg)
{
  static char msg[128];
  char buf[block_size];
  FILE *f = fopen (img, "r");
  long v;
  int durable = 0;

  (void) torn;
  (void) arg;
  CHECK (f, "open %s", img);
  image_block (f, x_blk, buf);
  fclose (f);
  v = mversion (buf, x_blk, 1);
  for (int i = 1; i <= x_nver; i++)
    if (x_synced[i] <= n)
      durable = i;
  if (v < durable)
    {
      snprintf (msg, sizeof msg, "block %u holds version %ld, but %d had "
		"committed", x_blk, v, durable);
      return msg;
    }
  return NULL;
}

static int
run_escape (const char *image)
{
  diskfs_transaction_t *txn;
  long flags;

  jt_mount (image, 2, 0);
  char *buf = malloc (block_size);
  pick_free_blocks (&x_blk, 1);
  JT_SET_HOOK (jt_store_write_hook, escape_hook);

  for (x_nver = 1; x_nver <= 6; x_nver++)
    {
      size_t mark = jt_log_count ();

      txn = diskfs_journal_start_transaction ();
      journal_mversion (txn, x_blk, x_nver, 1);
      if (x_nver % 3 == 2)
	{
	  /* Handed to the journal and gone: a read during the commit
	     gets the shadow.  */
	  jt_pageout (x_blk, 1, 1);
	  x_hook = HOOK_READ;
	}
      else if (x_nver % 3 == 0)
	/* Still dirty: evicted during the commit.  */
	x_hook = HOOK_EVICT;
      CHECK (!jt_commit (txn), "commit");
      x_synced[x_nver] = jt_log_count ();
      CHECK (x_hook == HOOK_NONE, "the commit wrote no escaped copy of "
	     "version %d", x_nver);

      CHECK (scan_descriptors (mark, &x_blk, 1, &flags) >= 1
	     && flags >= 0 && (flags & JBD2_FLAG_ESCAPE),
	     "version %d of block %u is not tagged escaped", x_nver, x_blk);
      jt_read (x_blk, buf);
      CHECK (mversion (buf, x_blk, 1) == x_nver, "block %u reads back as "
	     "version %ld, not %d", x_blk, mversion (buf, x_blk, 1), x_nver);
    }
  x_nver--;
  JT_SET_HOOK (jt_store_write_hook, NULL);

  unsigned long warnings = jt_umount ();
  CHECK (jt_stale_reads () == 0, "%lu stale page-ins", jt_stale_reads ());
  CHECK (warnings == 0, "%lu journal warnings", warnings);
  size_t checked = jt_crash_check ("t_logfmt-escape", escape_check, NULL);
  printf ("t_logfmt: OK (escape: %d escaped commits, %zu crash images)\n",
	  x_nver, checked);
  return 0;
}

/* ---------------------------------------------------------------- */
/* big */

static block_t big[BIG];
static size_t t1_at, t2_at, data_at[BIG];

static const char *
big_check (size_t n, int torn, const char *img, void *arg)
{
  static char msg[160];
  char buf[block_size];
  FILE *f = fopen (img, "r");
  long first = -2;

  (void) torn;
  (void) arg;
  CHECK (f, "open %s", img);
  for (int i = 0; i < BIG; i++)
    {
      block_t b = big[i];
      image_block (f, b, buf);
      /* The crash tore this block's own data write, or lost it before
	 a flush.  */
      if (n >= data_at[i] && !jt_crash_on_disk (data_at[i] - 1))
	continue;
      if (n >= data_at[i])
	{
	  if (pattern_version (buf, b) != DATA_WORD)
	    {
	      snprintf (msg, sizeof msg, "block %u lost its file data", b);
	      fclose (f);
	      return msg;
	    }
	  continue;
	}
      long v = mversion (buf, b, i % 2 == 0);
      /* Before T1's commit returned, all of T1 or none of it.  Before
	 T2's, all of it.  After T2, a block is revoked or still T1's.  */
      int ok = n >= t2_at ? v == 0 || v == 1
	: n >= t1_at ? v == 1 : v == 0 || v == 1;
      if (ok && n < t1_at)
	{
	  if (first == -2)
	    first = v;
	  ok = v == first;
	}
      if (!ok)
	{
	  snprintf (msg, sizeof msg, "block %u (%s) holds version %ld at "
		    "write %zu (T1 done at %zu, T2 at %zu)", b,
		    i % 2 ? "plain" : "escaped", v, n, t1_at, t2_at);
	  fclose (f);
	  return msg;
	}
    }
  fclose (f);
  return NULL;
}

static int
run_big (void)
{
  diskfs_transaction_t *txn;
  long *flags = malloc (BIG * sizeof *flags);
  size_t mark;
  int escaped = 0, plain = 0, revoke_blocks = 0, revoked = 0;

  jt_mount (big_image, 2, 0);
  char *buf = malloc (block_size);
  pick_free_blocks (big, BIG);

  /* T1: three descriptor blocks.  */
  mark = jt_log_count ();
  txn = diskfs_journal_start_transaction ();
  for (int i = 0; i < BIG; i++)
    journal_mversion (txn, big[i], 1, i % 2 == 0);
  CHECK (!jt_commit (txn), "commit T1");
  t1_at = jt_log_count ();
  int descriptors = scan_descriptors (mark, big, BIG, flags);
  CHECK (descriptors >= 3, "T1 took %d descriptor blocks", descriptors);
  for (int i = 0; i < BIG; i++)
    {
      CHECK (flags[i] >= 0, "no tag for block %u", big[i]);
      CHECK (!(flags[i] & JBD2_FLAG_ESCAPE) == !!(i % 2),
	     "block %u: escape flag %s", big[i],
	     flags[i] & JBD2_FLAG_ESCAPE ? "set" : "missing");
      if (flags[i] & JBD2_FLAG_ESCAPE)
	escaped++;
      else
	plain++;
    }

  /* T2: frees them all, two revoke blocks.  */
  mark = jt_log_count ();
  txn = diskfs_journal_start_transaction ();
  for (int i = 0; i < BIG; i++)
    CHECK (journal_record_freed_blocks (txn, big[i], 1), "free %u", big[i]);
  CHECK (!jt_commit (txn), "commit T2");
  t2_at = jt_log_count ();
  char *seen = calloc (BIG, 1);
  for (size_t i = mark; i < t2_at; i++)
    {
      const struct jt_write *w = jt_log_entry (i);
      if (!is_header (w, JBD2_REVOKE_BLOCK))
	continue;
      revoke_blocks++;
      const journal_revoke_header_t *r = w->data;
      const uint32_t *e = (const uint32_t *) (r + 1);
      size_t k = (be32toh (r->r_count) - sizeof *r) / sizeof *e;
      CHECK (be32toh (r->r_count) <= block_size, "r_count %u",
	     be32toh (r->r_count));
      for (size_t j = 0; j < k; j++)
	for (int m = 0; m < BIG; m++)
	  if (big[m] == be32toh (e[j]) && !seen[m])
	    {
	      seen[m] = 1;
	      revoked++;
	    }
    }
  CHECK (revoke_blocks >= 2, "T2 took %d revoke blocks", revoke_blocks);
  CHECK (revoked == BIG, "%d of %d blocks revoked", revoked, BIG);
  free (seen);
  free (flags);

  /* The blocks become file data.  */
  for (int i = 0; i < BIG; i++)
    {
      jt_discard (big[i], 0);
      jt_discard (big[i], 1);
      pattern (buf, big[i], DATA_WORD);
      jt_file_edit (big[i], 0, buf, block_size);
      jt_file_pageout (big[i], 1, 1);
      data_at[i] = jt_log_count ();
    }

  unsigned long warnings = jt_umount ();
  CHECK (jt_stale_reads () == 0, "%lu stale page-ins", jt_stale_reads ());
  CHECK (warnings == 0, "%lu journal warnings", warnings);
  size_t checked = jt_crash_check ("t_logfmt-big", big_check, NULL);
  printf ("t_logfmt: OK (big: %d descriptor blocks, %d escaped and %d "
	  "plain tags, %d revoke blocks, %zu crash images)\n", descriptors,
	  escaped, plain, revoke_blocks, checked);
  return 0;
}

/* ---------------------------------------------------------------- */
/* escape-abort */

static int data_writes, failed;

/* Fail the 600th log data block: in the second of three batches.  */
static int
abort_fail_hook (uint64_t off, size_t len, const void *buf)
{
  const uint32_t *w = buf;

  (void) len;
  if (failed || !jt_in_journal (off) || jt_journal_index (off) == 0
      || be32toh (w[0]) == JBD2_MAGIC_NUMBER)
    return 0;
  if (++data_writes < 600)
    return 0;
  failed = 1;
  return EIO;
}

static const char *
abort_check (size_t n, int torn, const char *img, void *arg)
{
  static char msg[128];
  char buf[block_size];
  FILE *f = fopen (img, "r");

  (void) n;
  (void) torn;
  (void) arg;
  CHECK (f, "open %s", img);
  for (int i = 0; i < BIG; i++)
    {
      image_block (f, big[i], buf);
      if (pattern_version (buf, big[i]) != 0)
	{
	  snprintf (msg, sizeof msg, "block %u of the aborted transaction "
		    "is not zero", big[i]);
	  fclose (f);
	  return msg;
	}
    }
  fclose (f);
  return NULL;
}

static int
run_escape_abort (void)
{
  diskfs_transaction_t *txn;
  int wrong = 0;

  jt_inhibit_busy = 0;
  jt_mount (big_image, 2, 0);
  char *buf = malloc (block_size);
  pick_free_blocks (big, BIG);

  txn = diskfs_journal_start_transaction ();
  for (int i = 0; i < BIG; i++)
    {
      journal_mversion (txn, big[i], 1, 1);
      jt_pageout (big[i], 1, 0);
    }
  jt_drain ();
  JT_SET_HOOK (jt_store_fail_hook, abort_fail_hook);
  error_t err = jt_commit (txn);
  JT_SET_HOOK (jt_store_fail_hook, NULL);
  CHECK (failed, "no log write failed");
  CHECK (err == EIO, "the commit returned %s", strerror (err));

  for (int i = 0; i < BIG; i++)
    {
      jt_read (big[i], buf);
      wrong += mversion (buf, big[i], 1) != 1;
    }
  CHECK (jt_stale_reads () == 0 && !wrong, "%lu stale page-ins, %d blocks "
	 "read back wrong after the abort", jt_stale_reads (), wrong);

  jt_umount ();
  size_t checked = jt_crash_check ("t_logfmt-escape-abort", abort_check,
				   NULL);
  printf ("t_logfmt: OK (escape-abort: log write %d failed, %zu crash "
	  "images)\n", data_writes, checked);
  return 0;
}

/* ---------------------------------------------------------------- */
/* oversize */

static int log_fd = -1;

/* Report every log block but the journal superblock.  */
static void
oversize_hook (uint64_t off, size_t len, const void *buf)
{
  (void) len;
  (void) buf;
  if (log_fd >= 0 && jt_in_journal (off) && jt_journal_index (off) != 0)
    CHECK (write (log_fd, "w", 1) == 1, "pipe");
}

static int
run_oversize (const char *image)
{
  const char *log = "build/t_logfmt-oversize.log";
  int fds[2], status;
  char c;
  size_t written = 0;

  CHECK (!pipe (fds), "pipe");
  fflush (NULL);
  pid_t pid = fork ();
  CHECK (pid >= 0, "fork");
  if (pid == 0)
    {
      static block_t blocks[BIG];
      close (fds[0]);
      CHECK (freopen (log, "w", stderr), "%s", log);
      jt_mount (image, 2, 0);
      pick_free_blocks (blocks, BIG);
      JT_SET_HOOK (jt_store_write_hook, oversize_hook);
      log_fd = fds[1];
      diskfs_transaction_t *txn = diskfs_journal_start_transaction ();
      for (int i = 0; i < BIG; i++)
	journal_version (txn, blocks[i], 1);
      jt_commit (txn);
      _exit (3);
    }
  close (fds[1]);
  while (read (fds[0], &c, 1) == 1)
    written++;
  CHECK (waitpid (pid, &status, 0) == pid, "wait");
  CHECK (WIFSIGNALED (status) && WTERMSIG (status) == SIGABRT,
	 "the oversize commit did not stop the translator (status %x; see "
	 "%s)", status, log);
  CHECK (jt_run ("grep -q 'Use a larger journal' %s", log) == 0,
	 "no refusal in %s", log);
  CHECK (written == 0, "%zu log blocks written for the oversize "
	 "transaction", written);
  printf ("t_logfmt: OK (oversize: refused, nothing in the log)\n");
  return 0;
}

/* ---------------------------------------------------------------- */

int
main (int argc, char **argv)
{
  const char *image = argc > 1 ? argv[1] : "build/base.img";
  static const char *const parts[] =
    { "escape", "big", "escape-abort", "oversize", NULL };
  int status;

  for (int p = 0; parts[p]; p++)
    {
      if (argc > 3 && strcmp (argv[3], parts[p]))
	continue;
      fflush (NULL);
      pid_t pid = fork ();
      CHECK (pid >= 0, "fork");
      if (pid == 0)
	exit (p == 0 ? run_escape (image) : p == 1 ? run_big ()
	      : p == 2 ? run_escape_abort () : run_oversize (image));
      CHECK (waitpid (pid, &status, 0) == pid, "wait");
      if (WIFSIGNALED (status))
	fprintf (stderr, "t_logfmt: %s died of signal %d\n", parts[p],
		 WTERMSIG (status));
      if (!WIFEXITED (status) || WEXITSTATUS (status))
	return 1;
    }
  return 0;
}
