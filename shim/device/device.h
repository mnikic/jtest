/* jtest shim: <device/device.h>.  */
#include <mach.h>
typedef mach_port_t device_t;
#define D_SUCCESS 0
#define D_IO_ERROR 2500
#define D_WOULD_BLOCK 2501
#define D_NO_SUCH_DEVICE 2502
#define D_ALREADY_OPEN 2503
#define D_DEVICE_DOWN 2504
#define D_INVALID_OPERATION 2505
#define D_INVALID_RECNUM 2506
#define D_INVALID_SIZE 2507
#define D_NO_MEMORY 2508
#define D_READ_ONLY 2509
