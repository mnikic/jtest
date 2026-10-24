/* jtest shim: <hurd/port.h>, for the struct libdiskfs/priv.h names.  */
#ifndef JTEST_HURD_PORT_H
#define JTEST_HURD_PORT_H

#include <mach.h>

struct hurd_port
{
  mach_port_t port;
};

#endif
