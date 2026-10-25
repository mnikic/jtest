/* jtest shim: the <hurd/hurd_types.h> types the ext2fs headers name.  */
#ifndef JTEST_HURD_TYPES_H
#define JTEST_HURD_TYPES_H

#include <mach.h>
#include <sys/stat.h>
#include <sys/statfs.h>

typedef mach_port_t file_t;
typedef mach_port_t fsys_t;
typedef mach_port_t auth_t;
typedef mach_port_t io_t;
typedef mach_port_t exec_startup_t;
typedef char *data_t;
typedef const char *const_data_t;
typedef int *intarray_t;
typedef const int *const_intarray_t;
typedef mach_port_t *portarray_t;
typedef const mach_port_t *const_portarray_t;
typedef loff_t *off_array_t;
typedef const loff_t *const_off_array_t;
typedef char string_t[1024];
typedef struct stat io_statbuf_t;
typedef struct statfs fsys_statfsbuf_t;

/* The Hurd's open modes, which libdiskfs keeps in a peropen's openstat;
   Linux's <fcntl.h> has no O_READ or O_WRITE.  */
#ifndef O_READ
#define O_READ 0x0001
#endif
#ifndef O_WRITE
#define O_WRITE 0x0002
#endif
/* And the rest of the Hurd's own, on bits Linux's do not use.  */
#ifndef O_EXEC
#define O_EXEC 0x0004
#endif
#define O_SHLOCK 0x08000000
#define O_NOLINK 0x10000000
#define O_NOTRANS 0x20000000
#define O_EXLOCK 0x40000000
#define O_HURD (~0)

/* <hurd/hurd_types.h>'s translator flags.  */
#define FS_TRANS_FORCE 0x00000001
#define FS_TRANS_EXCL 0x00000002
#define FS_TRANS_SET 0x00000004
#define FS_TRANS_ORPHAN 0x00000008

typedef enum retry_type
{
  FS_RETRY_NORMAL = 1,
  FS_RETRY_REAUTH = 2,
  FS_RETRY_MAGICAL = 3,
} retry_type;

enum file_storage_class
{
  STORAGE_OTHER,
  STORAGE_DEVICE,
  STORAGE_HURD_FILE,
  STORAGE_NETWORK,
  STORAGE_MEMORY,
  STORAGE_TASK,
  STORAGE_ZERO,
  STORAGE_CONCAT,
  STORAGE_INTERLEAVE,
  STORAGE_LAYER,
  STORAGE_REMAP,
  STORAGE_COPY,
};

enum dir_changed_type
{
  DIR_CHANGED_NULL,		/* Always sent first for sync.  */
  DIR_CHANGED_NEW,		/* Specified name has been added.  */
  DIR_CHANGED_UNLINK,		/* Specified name has been removed.  */
  DIR_CHANGED_RENUMBER,		/* Name has been the target of rename.  */
};
typedef enum dir_changed_type dir_changed_type_t;

/* Types for fs_notify.defs:file_changed call: */
enum file_changed_type
{
  FILE_CHANGED_NULL,		/* Always sent first for sync.  */
  FILE_CHANGED_WRITE,		/* File data has been written.  */
  FILE_CHANGED_EXTEND,		/* File has grown.  */
  FILE_CHANGED_TRUNCATE,	/* File has been truncated.  */
  FILE_CHANGED_META,		/* Stat information has changed, and none
				   of the previous three apply.  Not sent
				   for changes in node times.  */
};
typedef enum file_changed_type file_changed_type_t;

#endif
