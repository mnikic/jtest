/* journal.c, unchanged, with read-only accessors to its internals that
   tests use to wait for a state.  JT_JOURNAL_C is the path of the
   journal.c under test.  */

#ifndef JT_JOURNAL_C
#define JT_JOURNAL_C "journal.c"	/* For editors; make sets it.  */
#endif
#include JT_JOURNAL_C

#include "jtest.h"

int
jt_txn_locked (diskfs_transaction_t *txn)
{
  int locked;

  JOURNAL_LOCK (ext2_journal);
  locked = txn->t_state == T_LOCKED;
  JOURNAL_UNLOCK (ext2_journal);
  return locked;
}
