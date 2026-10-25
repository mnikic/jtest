/* jtest shim: the MIG server header for <hurd/fs.defs>, as much of it
   as the libdiskfs files the harness links use.  */
#ifndef JT_FS_S_H
#define JT_FS_S_H
#ifndef JT_CONST_STRING_T
#define JT_CONST_STRING_T
typedef const char *const_string_t;
#endif
kern_return_t diskfs_S_dir_rename (struct protid *fromcred,
				   const_string_t fromname,
				   struct protid *tocred,
				   const_string_t toname, int excl);
#endif
