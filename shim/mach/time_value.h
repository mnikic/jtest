/* jtest shim: <mach/time_value.h>, enough for maptime.h.  */
#ifndef JTEST_MACH_TIME_VALUE_H
#define JTEST_MACH_TIME_VALUE_H
#include <mach.h>

struct time_value64
{
  int64_t seconds;
  int64_t nanoseconds;
};

typedef struct mapped_time_value
{
  integer_t seconds;
  integer_t microseconds;
  integer_t check_seconds;
  struct time_value64 time_value;
  int64_t check_seconds64;
  struct time_value64 uptime_value;
  int64_t check_upseconds64;
} mapped_time_value_t;
#endif
