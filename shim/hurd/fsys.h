/* jtest shim: <hurd/fsys.h>, as much of it as the libdiskfs files the
   harness links use: no translator runs in the harness.  */
#ifndef JT_HURD_FSYS_H
#define JT_HURD_FSYS_H
#include <mach.h>
kern_return_t fsys_goaway (mach_port_t fsys, int flags);
#endif
