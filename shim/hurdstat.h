/* jtest shim, included first in every file (see the Makefile): the
   Hurd's struct stat, which has fields Linux's lacks (st_gen, st_author,
   st_flags, ...) and is part of struct node.  glibc's own is renamed out
   of the way, along with stat (); the harness does not use the host's
   stat, fstat or lstat.  The same for struct statfs.  */
#ifndef JTEST_HURDSTAT_H
#define JTEST_HURDSTAT_H

#define stat jt_linux_stat
#include <sys/stat.h>
#undef stat

#include <stdint.h>
#include <time.h>

/* As in the Hurd's <bits/stat.h>.  */
struct stat
{
  int st_fstype;
  uint64_t st_fsid;
  ino_t st_ino;
  unsigned int st_gen;
  dev_t st_rdev;
  mode_t st_mode;
  nlink_t st_nlink;
  uid_t st_uid;
  gid_t st_gid;
  off_t st_size;
  struct timespec st_atim;
  struct timespec st_mtim;
  struct timespec st_ctim;
  blksize_t st_blksize;
  blkcnt_t st_blocks;
  uid_t st_author;
  unsigned int st_flags;
};
#define st_dev st_fsid

#define S_ISPARE   020000000000
#define S_ITRANS   000070000000
#define S_IROOT    000040000000
#define S_IPTRANS  000010000000
#define S_IATRANS  000020000000
#define S_IMMAP0   000100000000
#define S_IUSEUNK  000200000000
#define S_IUNKNOWN 000007000000
#define S_IUNKSHIFT 12

#define FSTYPE_EXT2FS 0x00000006

/* And its struct statfs, which ext2fs fills in.  */
#define statfs jt_linux_statfs
#include <sys/statfs.h>
#undef statfs

struct statfs
{
  unsigned int f_type;
  unsigned long f_bsize;
  unsigned long f_blocks;
  unsigned long f_bfree;
  unsigned long f_bavail;
  unsigned long f_files;
  unsigned long f_ffree;
  uint64_t f_fsid;
  unsigned long f_namelen;
  unsigned long f_favail;
  unsigned long f_frsize;
  unsigned long f_flag;
  unsigned int f_spare[3];
};

/* The Hurd's static initializer: x86-64 glibc's spin lock is unlocked at
   1 (it locks with lock decl).  */
#define PTHREAD_SPINLOCK_INITIALIZER 1

#endif
