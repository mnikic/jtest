/* Link wraps for the directory tests that put the life of a block in
   the trace: allocations, frees, and dir.c's calls into the journal.  */

#include <stdio.h>
#include "jtest.h"

block_t __real_ext2_new_block (block_t goal, block_t prealloc_goal,
			       block_t *prealloc_count, block_t *prealloc_block);
void __real_ext2_free_blocks (block_t block, unsigned long count);
error_t __real_journal_get_write_access_external (diskfs_transaction_t *txn,
						  block_t b);
error_t __real_journal_mark_dirty_external (diskfs_transaction_t *txn,
					    block_t b, const void *data);
void __real_journal_release_access (diskfs_transaction_t *txn, block_t b);

block_t
__wrap_ext2_new_block (block_t goal, block_t prealloc_goal,
		       block_t *prealloc_count, block_t *prealloc_block)
{
  block_t b = __real_ext2_new_block (goal, prealloc_goal, prealloc_count,
				     prealloc_block);
  if (b)
    jt_trace (b, "allocated", NULL);
  return b;
}

void
__wrap_ext2_free_blocks (block_t block, unsigned long count)
{
  for (unsigned long i = 0; i < count; i++)
    jt_trace (block + i, "freed", NULL);
  __real_ext2_free_blocks (block, count);
}

error_t
__wrap_journal_get_write_access_external (diskfs_transaction_t *txn,
					  block_t b)
{
  jt_trace (b, "dir get_write_access", NULL);
  return __real_journal_get_write_access_external (txn, b);
}

error_t
__wrap_journal_mark_dirty_external (diskfs_transaction_t *txn, block_t b,
				    const void *data)
{
  jt_trace (b, "dir mark_dirty", data);
  return __real_journal_mark_dirty_external (txn, b, data);
}

void
__wrap_journal_release_access (diskfs_transaction_t *txn, block_t b)
{
  jt_trace (b, "dir release_access", NULL);
  __real_journal_release_access (txn, b);
}
