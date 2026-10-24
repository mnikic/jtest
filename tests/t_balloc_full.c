/* t_balloc on a nearly full image: full.img, whose filler file takes
   all but a few dozen of the free blocks.  The two files then keep
   running out of space, and ext2_new_block finds every group full but
   for blocks still busy until their free commits: it waits for the
   commit freeing them, when another thread's transaction is committing,
   and retries.  When none is, the caller gets ENOSPC.  What must hold is
   what t_balloc checks, and that no allocation hangs (the watchdog).  */

#define T_NAME "t_balloc_full"
#include "t_balloc.c"
