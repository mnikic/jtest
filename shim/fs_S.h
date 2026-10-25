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
kern_return_t diskfs_S_dir_mkdir (struct protid *dircred,
				  const_string_t name, mode_t mode);
kern_return_t diskfs_S_dir_link (struct protid *dircred,
				 struct protid *filecred,
				 const_string_t name, int excl);
kern_return_t diskfs_S_dir_lookup (struct protid *dircred,
				   const_string_t filename, int flags,
				   mode_t mode, retry_type *do_retry,
				   string_t retry_name,
				   mach_port_t *retry_port,
				   mach_msg_type_name_t *retry_port_type);
kern_return_t diskfs_S_file_set_translator (struct protid *cred,
					    int passive_flags,
					    int active_flags,
					    int killtrans_flags,
					    const_data_t passive,
					    mach_msg_type_number_t passivelen,
					    mach_port_t active);
#endif
