/* jtest shim: what ext2fs/dir.c uses of it.  */
#include <mach.h>
#include <string.h>

/* A memory fault inside hurd_safe_* is caught, as the Hurd reports it:
   the harness's file pager longjmps to diskfs_catch_exception instead,
   so a fault inside this one is not caught here.  */
#ifndef EKERN_MEMORY_ERROR
#define EKERN_MEMORY_ERROR 0x7f00
#endif

static inline error_t
hurd_safe_memset (void *dest, int byte, size_t nbytes)
{
  memset (dest, byte, nbytes);
  return 0;
}
