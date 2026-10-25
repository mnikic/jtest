/* jtest shim: the MIG server header for <hurd/io.defs>, as much of it
   as the libdiskfs files the harness links use.  */
#ifndef JT_IO_S_H
#define JT_IO_S_H
kern_return_t diskfs_S_io_write (struct protid *cred, const_data_t data,
				 mach_msg_type_number_t datalen,
				 off_t offset, vm_size_t *amt);
#endif
