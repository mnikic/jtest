/* Helpers shared by the tests: blocks to scribble on, and versioned
   block contents that can be told apart after a replay.  */

#ifndef JTEST_COMMON_H
#define JTEST_COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "jtest.h"

#define CHECK(cond, ...)						\
  do {									\
    if (!(cond))							\
      {									\
	fprintf (stderr, "%s:%d: CHECK failed: %s: ", __FILE__, __LINE__, \
		 #cond);						\
	fprintf (stderr, __VA_ARGS__);					\
	fputc ('\n', stderr);						\
	exit (1);							\
      }									\
  } while (0)

/* Fill BUF (one block) with version V of block B.  */
static inline void
pattern (void *buf, block_t b, uint32_t v)
{
  uint32_t *w = buf;
  for (size_t i = 0; i < block_size / 4; i += 2)
    {
      w[i] = b;
      w[i + 1] = v;
    }
}

/* The version of block B that BUF holds, or -1 if it is not a whole
   version of B (torn, or another block's).  Version 0 is the block as
   mke2fs left it: zeros.  */
static inline long
pattern_version (const void *buf, block_t b)
{
  const uint32_t *w = buf;
  int zero = 1;

  for (size_t i = 0; i < block_size / 4 && zero; i++)
    zero = !w[i];
  if (zero)
    return 0;
  for (size_t i = 0; i < block_size / 4; i += 2)
    if (w[i] != b || w[i + 1] != w[1])
      return -1;
  return w[1];
}

/* Pick N free blocks, spread out from the middle of the image.  */
static inline void
pick_free_blocks (block_t *out, int n)
{
  block_t b = jt_nblocks () / 2;
  for (int i = 0; i < n; b++)
    {
      CHECK (b < jt_nblocks (), "not enough free blocks");
      if (jt_block_free (b))
	out[i++] = b;
    }
}

/* Journal and write version V of block B in TXN.  */
static inline void
journal_version (diskfs_transaction_t *txn, block_t b, uint32_t v)
{
  char buf[block_size];

  CHECK (!journal_get_write_access (txn, b), "write access to %u", b);
  pattern (buf, b, v);
  jt_edit (b, 0, buf, block_size);
  journal_mark_dirty (txn, b);
}

/* The versions of NBLK blocks after each of a sequence of operations,
   for checking crash images.  Operation J is one handle, and JBD2 puts
   whole handles in a transaction, so after a replay the blocks must hold
   the state after some operation J: no operation half there.  J must be
   at least the last operation whose sync commit had returned before the
   crash.  */
struct history
{
  int nblk;
  const block_t *blocks;
  uint32_t *ver;		/* The versions now.  */
  uint32_t *snaps;		/* After operation J: snaps[J * nblk ...].  */
  size_t *synced;		/* Log length when J's sync commit returned,
				   or SIZE_MAX.  */
  /* What reads a block's version, if not pattern_version.  */
  long (*version) (const void *buf, block_t b);
  int nops, maxops;
};

static inline void
history_init (struct history *h, const block_t *blocks, int nblk, int maxops)
{
  h->nblk = nblk;
  h->blocks = blocks;
  h->ver = calloc (nblk, sizeof *h->ver);
  h->snaps = calloc ((size_t) (maxops + 1) * nblk, sizeof *h->snaps);
  h->synced = malloc ((maxops + 1) * sizeof *h->synced);
  h->synced[0] = 0;
  h->nops = 0;
  h->maxops = maxops;
}

/* Journal the next version of block K of H in TXN.  */
static inline void
history_write (struct history *h, diskfs_transaction_t *txn, int k)
{
  journal_version (txn, h->blocks[k], ++h->ver[k]);
}

/* Record the end of an operation; SYNCED if it ended with a commit that
   has returned.  */
static inline void
history_op_done (struct history *h, int synced)
{
  CHECK (h->nops < h->maxops, "too many operations");
  h->nops++;
  memcpy (h->snaps + (size_t) h->nops * h->nblk, h->ver,
	  h->nblk * sizeof *h->ver);
  h->synced[h->nops] = synced ? jt_log_count () : SIZE_MAX;
}

/* diskfs_journal_commit_transaction, and what it returns: 0 while the
   tree's returns nothing.  */
#define JT_COMMIT_RETURNS \
  !__builtin_types_compatible_p \
    (__typeof__ (diskfs_journal_commit_transaction (NULL)), void)
static inline error_t
jt_commit (diskfs_transaction_t *txn)
{
  return __builtin_choose_expr (JT_COMMIT_RETURNS,
				diskfs_journal_commit_transaction (txn),
				(diskfs_journal_commit_transaction (txn), 0));
}

/* A jt_crash_check_fn for a struct history.  */
static inline const char *
history_check (size_t n, int torn, const char *img, void *arg)
{
  static char msg[256];
  struct history *h = arg;
  long *got = calloc (h->nblk, sizeof *got);
  char *buf = malloc (block_size);
  FILE *f = fopen (img, "r");
  int durable = 0, j;

  (void) torn;
  CHECK (f, "open %s", img);
  for (int i = 0; i < h->nblk; i++)
    {
      CHECK (!fseeko (f, (off_t) h->blocks[i] << log2_block_size, SEEK_SET)
	     && fread (buf, block_size, 1, f) == 1, "read %s", img);
      got[i] = h->version ? h->version (buf, h->blocks[i])
	: pattern_version (buf, h->blocks[i]);
    }
  fclose (f);
  free (buf);

  for (j = 1; j <= h->nops; j++)
    if (h->synced[j] <= n)
      durable = j;

  for (j = h->nops; j >= 0; j--)
    {
      int i;
      const uint32_t *snap = h->snaps + (size_t) j * h->nblk;
      for (i = 0; i < h->nblk && got[i] == snap[i]; i++)
	;
      if (i == h->nblk)
	break;
    }

  if (j < 0)
    {
      int bad = 0;
      while (bad < h->nblk && got[bad] >= 0)
	bad++;
      if (bad < h->nblk)
	snprintf (msg, sizeof msg, "block %u is torn or garbage",
		  h->blocks[bad]);
      else
	{
	  /* Report the closest operation, and how the blocks differ.  */
	  int best = 0, best_diff = h->nblk + 1, len;
	  for (int o = 0; o <= h->nops; o++)
	    {
	      int diff = 0;
	      const uint32_t *snap = h->snaps + (size_t) o * h->nblk;
	      for (int i = 0; i < h->nblk; i++)
		diff += got[i] != snap[i];
	      if (diff <= best_diff)
		best = o, best_diff = diff;
	    }
	  len = snprintf (msg, sizeof msg, "the blocks match no operation "
			  "(a transaction is half there); closest: after "
			  "operation %d, but", best);
	  const uint32_t *snap = h->snaps + (size_t) best * h->nblk;
	  for (int i = 0, shown = 0; i < h->nblk && shown < 4; i++)
	    if (got[i] != snap[i] && len < (int) sizeof msg)
	      {
		/* The operation that wrote the version found.  */
		int w = 0;
		for (int o = 1; o <= h->nops; o++)
		  if (h->snaps[(size_t) o * h->nblk + i] == got[i]
		      && h->snaps[(size_t) (o - 1) * h->nblk + i] != got[i])
		    w = o;
		len += snprintf (msg + len, sizeof msg - len,
				 " block %u v%ld (op %d) not v%u;",
				 h->blocks[i], got[i], w, snap[i]);
		shown++;
	      }
	}
    }
  else if (j < durable)
    snprintf (msg, sizeof msg, "recovered the state after operation %d, "
	      "but the commit of operation %d had returned", j, durable);
  free (got);
  return j < 0 || j < durable ? msg : NULL;
}

#endif
