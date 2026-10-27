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

unsigned long
jt_forced_checkpoints (void)
{
#ifdef JRNL_STAT_GET
  return JRNL_STAT_GET (journal_stat_forced_checkpoints);
#else
  /* A tree without the journal's counters.  */
  return JT_UNKNOWN;
#endif
}

uint32_t
jt_txn_tid (diskfs_transaction_t *txn)
{
  return txn->t_tid;
}

uint32_t
jt_released_tid (void)
{
  JOURNAL_LOCK (ext2_journal);
  uint32_t tid = ext2_journal->j_released_tid;
  JOURNAL_UNLOCK (ext2_journal);
  return tid;
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

static void
txn_state (FILE *f, const char *what, diskfs_transaction_t *t)
{
  fprintf (f, "  %s tid %u: log start %u, state %d, %zu blocks, "
	   "%u outstanding\n", what, t->t_tid, t->t_log_start,
	   (int) t->t_state, t->t_buffer_map.size, t->t_outstanding_io);
}

/* What the journal thinks of its log, for a report.  Read without the
   journal's lock, which the caller may hold: the values can be torn.  */
void
jt_journal_state (FILE *f)
{
  journal_t *j = ext2_journal;

  if (!j)
    {
      fprintf (f, "jtest: no journal\n");
      return;
    }
  fprintf (f, "jtest: journal: head %u, tail %u, on disk %u, free %u "
	   "(log %u-%u), next tid %u, checkpointing %d, tail writing %d\n",
	   j->j_head, j->j_tail, j->j_disk_tail, j->j_free, j->j_first,
	   j->j_last, j->j_transaction_sequence, j->j_checkpointing,
	   j->j_tail_writing);
  for (diskfs_transaction_t *t = j->j_checkpoint_list; t;
       t = t->t_checkpoint_next)
    txn_state (f, "checkpoint", t);
  for (diskfs_transaction_t *t = j->j_retired_list; t;
       t = t->t_checkpoint_next)
    txn_state (f, "retired", t);
  if (j->j_committing_transaction)
    txn_state (f, "committing", j->j_committing_transaction);
  if (j->j_running_transaction)
    txn_state (f, "running", j->j_running_transaction);
}

static uint32_t
shadow_hash (const journal_buffer_t *jb)
{
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < block_size; i++)
    h = (h ^ ((const unsigned char *) jb->jb_shadow_data)[i]) * 16777619u;
  return h;
}

static size_t
holder (char *out, size_t len, const char *kind, diskfs_transaction_t *t,
	block_t b)
{
  journal_buffer_t *jb = t ? journal_map_lookup (&t->t_buffer_map, b) : NULL;
  if (!jb || len < 2)
    return 0;
  int n = snprintf (out, len, " %s%u:%s%s%s%s%s%s%s#%08x", kind, t->t_tid,
		    jb->needs_flush ? "f" : "", jb->jb_is_written ? "w" : "",
		    jb->jb_is_flushing ? "c" : "", jb->needs_copy ? "x" : "",
		    jb->jb_sweeping ? "s" : "", jb->jb_external ? "e" : "",
		    jb->jb_awaiting_data ? "a" : "", shadow_hash (jb));
  return n < 0 ? 0 : (size_t) n < len ? (size_t) n : len - 1;
}

/* The journal's buffers of block B, for the trace: each transaction
   holding it (K checkpoint list, C committing, R running, A aborted),
   its tid, flags (f needs_flush, w written, c claimed, x needs_copy,
   s sweeping, e external, a awaiting data) and its shadow's hash.  Only
   if the journal's lock is free: a caller may hold it.  */
void
jt_block_holders (block_t b, char *out, size_t len)
{
  journal_t *j = ext2_journal;

  out[0] = 0;
  if (!j)
    return;
  if (pthread_mutex_trylock (&j->j_state_lock))
    {
      snprintf (out, len, " ?");
      return;
    }
  /* Of the checkpoint list: those not written yet, and the two newest;
     the others are only counted.  */
  size_t n = 0, held = 0, skipped = 0;
  for (diskfs_transaction_t *t = j->j_checkpoint_list; t;
       t = t->t_checkpoint_next)
    held += journal_map_lookup (&t->t_buffer_map, b) != NULL;
  size_t k = 0;
  for (diskfs_transaction_t *t = j->j_checkpoint_list; t;
       t = t->t_checkpoint_next)
    {
      journal_buffer_t *jb = journal_map_lookup (&t->t_buffer_map, b);
      if (!jb)
	continue;
      k++;
      if (jb->jb_is_written && k + 2 <= held)
	{
	  skipped++;
	  continue;
	}
      if (skipped)
	{
	  int m = snprintf (out + n, len - n, " (%zu written)", skipped);
	  n += m > 0 && (size_t) m < len - n ? (size_t) m : 0;
	  skipped = 0;
	}
      n += holder (out + n, len - n, "K", t, b);
    }
  n += holder (out + n, len - n, "C", j->j_committing_transaction, b);
  n += holder (out + n, len - n, "R", j->j_running_transaction, b);
  n += holder (out + n, len - n, "A", j->j_aborted_transaction, b);
  pthread_mutex_unlock (&j->j_state_lock);
}

