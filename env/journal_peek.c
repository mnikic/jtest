/* journal.c, unchanged, with read-only accessors to its internals that
   tests use to wait for a state.  JT_JOURNAL_C is the path of the
   journal.c under test.  */

#ifndef JT_JOURNAL_C
#define JT_JOURNAL_C "journal.c"	/* For editors; make sets it.  */
#endif
#include JT_JOURNAL_C

#include "jtest.h"

static long
claims_in (diskfs_transaction_t *txn)
{
  size_t iter = 0;
  journal_buffer_t *jb;
  long n = 0;

  if (!txn)
    return 0;
  while ((jb = journal_map_iterate (&txn->t_buffer_map, &iter)) != NULL)
    n += jb->jb_is_flushing != 0;
  return n;
}

long
jt_flush_claims (void)
{
  journal_t *j = ext2_journal;
  long n;

  if (!j)
    return 0;
  JOURNAL_LOCK (j);
  if (j->j_checkpointing || j->j_committing_transaction)
    n = -1;
  else
    {
      n = claims_in (j->j_running_transaction);
      for (diskfs_transaction_t *t = j->j_checkpoint_list; t;
	   t = t->t_checkpoint_next)
	n += claims_in (t);
    }
  JOURNAL_UNLOCK (j);
  return n;
}

int
jt_checkpointing (void)
{
  journal_t *j = ext2_journal;

  return j && __atomic_load_n (&j->j_checkpointing, __ATOMIC_ACQUIRE);
}

int
jt_txn_locked (diskfs_transaction_t *txn)
{
  int locked;

  JOURNAL_LOCK (ext2_journal);
  locked = txn->t_state == T_LOCKED;
  JOURNAL_UNLOCK (ext2_journal);
  return locked;
}

int
jt_shadow (diskfs_transaction_t *txn, block_t b, void *out)
{
  journal_buffer_t *jb;

  JOURNAL_LOCK (ext2_journal);
  jb = journal_map_lookup (&txn->t_buffer_map, b);
  if (jb)
    memcpy (out, jb->jb_shadow_data, block_size);
  JOURNAL_UNLOCK (ext2_journal);
  return jb ? 0 : ENOENT;
}

int
jt_checkpoint_unwritten (block_t b)
{
  int unwritten = 0;

  JOURNAL_LOCK (ext2_journal);
  for (diskfs_transaction_t *txn = ext2_journal->j_checkpoint_list;
       txn && !unwritten; txn = txn->t_checkpoint_next)
    {
      journal_buffer_t *jb = journal_map_lookup (&txn->t_buffer_map, b);
      unwritten = jb && !jb->jb_is_written;
    }
  JOURNAL_UNLOCK (ext2_journal);
  return unwritten;
}
