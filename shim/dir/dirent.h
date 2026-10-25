/* jtest shim, for ext2fs/dir.c only: the Hurd's struct dirent, which has
   d_namlen (glibc's sysdeps/mach/hurd/bits/dirent.h).  The host's
   dirent.h is still used for everything else.  */
#ifndef JT_HURD_DIRENT_H
#define JT_HURD_DIRENT_H
#include_next <dirent.h>
#define dirent hurd_dirent
struct hurd_dirent
{
  ino_t d_fileno;
  unsigned short int d_reclen;
  unsigned char d_type;
  unsigned char d_namlen;
  char d_name[1];
};
#endif
