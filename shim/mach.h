/* Fake Mach for building ext2fs pieces on a Linux host.
   Only the types and calls that the headers and journal.c touch.  */

#ifndef JTEST_MACH_H
#define JTEST_MACH_H

#include <stddef.h>
#include <stdint.h>
#include <errno.h>
#include <sys/types.h>

typedef int kern_return_t;
typedef unsigned int natural_t;
typedef int integer_t;
typedef int boolean_t;
typedef uintptr_t vm_offset_t;
typedef uintptr_t vm_address_t;
typedef uintptr_t vm_size_t;
typedef int vm_prot_t;
typedef natural_t mach_port_t;
typedef mach_port_t mach_port_name_t;
typedef mach_port_t task_t;
typedef mach_port_t thread_t;
typedef mach_port_t memory_object_t;
typedef mach_port_t memory_object_control_t;
typedef mach_port_t mach_port_seqno_t;
typedef natural_t mach_port_right_t;
typedef natural_t mach_port_mscount_t;
typedef natural_t mach_msg_type_name_t;
typedef natural_t mach_msg_type_number_t;
typedef integer_t mach_msg_id_t;
typedef natural_t mach_msg_seqno_t;
typedef int error_t;
typedef int memory_object_copy_strategy_t;

#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif

#define KERN_SUCCESS 0

/* Port right dispositions and send errors, for libdiskfs's dir_lookup
   and file_set_translator; the harness has no IPC.  */
#define MACH_MSG_TYPE_MOVE_SEND 17
#define MACH_MSG_TYPE_COPY_SEND 19
#define MACH_MSG_TYPE_MAKE_SEND 20
#define MACH_SEND_INVALID_DEST 0x10000003
#define MIG_SERVER_DIED -308
#define MACH_PORT_NULL ((mach_port_t) 0)
#define MACH_PORT_DEAD ((mach_port_t) ~0)
#define MACH_PORT_VALID(p) ((p) != MACH_PORT_NULL && (p) != MACH_PORT_DEAD)

#define VM_PROT_NONE 0
#define VM_PROT_READ 1
#define VM_PROT_WRITE 2
#define VM_PROT_EXECUTE 4
#define VM_PROT_ALL 7

typedef struct
{
  natural_t msgh_bits;
  natural_t msgh_size;
  mach_port_t msgh_remote_port;
  mach_port_t msgh_local_port;
  natural_t msgh_seqno;
  integer_t msgh_id;
} mach_msg_header_t;

typedef struct
{
  integer_t seconds;
  integer_t microseconds;
} time_value_t;

extern vm_size_t vm_page_size;

static inline mach_port_t mach_task_self (void) { return 1; }

kern_return_t vm_allocate (task_t, vm_address_t *, vm_size_t, boolean_t);
kern_return_t vm_deallocate (task_t, vm_address_t, vm_size_t);

/* For truncate.c: only reached for a node with a file pager, which the
   harness's nodes do not have.  */
#define MEMORY_OBJECT_COPY_NONE 0
#define MEMORY_OBJECT_COPY_DELAY 2
#define round_page(x) \
  ((((vm_offset_t) (x)) + vm_page_size - 1) & ~(vm_page_size - 1))
#define trunc_page(x) (((vm_offset_t) (x)) & ~(vm_page_size - 1))
kern_return_t vm_map (task_t, vm_address_t *, vm_size_t, vm_address_t,
		      boolean_t, mach_port_t, vm_offset_t, boolean_t,
		      vm_prot_t, vm_prot_t, int);
kern_return_t mach_port_deallocate (task_t, mach_port_t);

#endif
