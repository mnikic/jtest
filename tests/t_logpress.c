/* t_threads under log pressure: 8 threads each rewrite their 40 blocks
   in every operation, in the base image's 1024-block log, the smallest
   mke2fs makes.  The handles open at once outgrow a transaction's 256
   blocks, the log goes round many times, and as the same blocks are
   edited again and again, the flush after a commit leaves many to the
   checkpoints: commits wait for room (journal_ensure_commit_space_locked
   forces a checkpoint), while kjournald writes the tail and pagers write
   blocks home.  What must hold is what t_threads checks, that the log
   did fill up, and that nothing hangs (the watchdog).

   The last thread owns 400 blocks: each of its operations is one handle
   bigger than what a forced checkpoint makes room for, so the commit
   empties the log first.  */

#define NT 8
#define K 40
#define KT(t) ((t) == NT - 1 ? 400 : K)
#define KMAX 400
#define T_NAME "t_logpress"
#define T_PRESSURE 1
#include "t_threads.c"
