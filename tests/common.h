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

#endif
