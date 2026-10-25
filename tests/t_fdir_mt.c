/* t_fdir_ops with four threads making the root's entries at once: dir.c
   under concurrent handles, commits and page-outs of the directory's
   blocks, the root's lock ordering the operations.  */

#define NT 4
#define OPS 150
#define T_NAME "t_fdir_mt"
#include "t_fdir_ops.c"
