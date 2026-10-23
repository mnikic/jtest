/* jtest shim: <hurd.h>.  */
#ifndef JTEST_HURD_H
#define JTEST_HURD_H
#include <mach.h>
#include <hurd/hurd_types.h>

/* The real one catches faults on DEST or SRC.  The harness has no
   pager-backed memory, so a plain copy is the same thing.  */
#include <string.h>
static inline error_t
hurd_safe_copyin (void *dest, const void *src, size_t size)
{
  memcpy (dest, src, size);
  return 0;
}
#endif
