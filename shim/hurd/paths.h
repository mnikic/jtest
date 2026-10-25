/* jtest shim: <hurd/paths.h>, as much of it as the libdiskfs files the
   harness links use.  */
#ifndef JT_HURD_PATHS_H
#define JT_HURD_PATHS_H
#define _HURD "/hurd/"
#define _HURD_SYMLINK _HURD "symlink"
#define _HURD_CHRDEV _HURD "chrdev"
#define _HURD_BLKDEV _HURD "blkdev"
#define _HURD_FIFO _HURD "fifo"
#define _HURD_IFSOCK _HURD "ifsock"
#define _SERVERS_EXEC "/servers/exec"
#endif
