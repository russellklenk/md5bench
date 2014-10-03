/*/////////////////////////////////////////////////////////////////////////////
/// @summary Provides an interface to a high-resolution timer.
///////////////////////////////////////////////////////////////////////////80*/

#ifndef MD5BENCH_TIMER_HPP
#define MD5BENCH_TIMER_HPP

/*////////////////
//   Includes   //
////////////////*/
#include <stddef.h>
#include <stdint.h>
#include "common.hpp"

/*////////////////////
//   Preprocessor   //
////////////////////*/
#ifndef TIMER_C_API
#define TIMER_C_API(return_type)    extern return_type LLCALL_C
#endif

/*/////////////////
//   Functions   //
/////////////////*/
#ifdef __cplusplus
extern "C" {
#endif

/// @summary Initializes the timing services. This needs to be called once 
/// prior to calling any of the other functions in this module. Typically 
/// this will gather information about the clock frequency and so on.
/// @return true if high resolution timing is available.
TIMER_C_API(bool) time_service_open(void);

/// @summary Frees resources associated with the timing services. This needs
/// to be called once just before the application terminates.
TIMER_C_API(void) time_service_close(void);

/// @summary Retrieves a timestamp value with nanosecond resolution.
/// @return The current timestamp, in nanoseconds.
TIMER_C_API(uint64_t) time_service_read(void);

#ifdef __cplusplus
}; // extern "C"
#endif

/*/////////////////////////
//   Inlined Functions   //
/////////////////////////*/
/// @summary Calculates the time elapsed between two timestamps.
/// @param a...b The timestamp values.
/// @return The time elapsed between the timestamps, in nanoseconds. This value 
/// will be the same regardless of whether you call duration(a, b) or duration(b, a).
static inline uint64_t duration(uint64_t a, uint64_t b)
{
    if (a < b) return (b - a);
    else return (a - b);
}

/// @summary Converts a nanosecond time value to seconds.
/// @param nanosec The input time value, in nanoseconds.
/// @return The time value in seconds.
static inline double seconds(uint64_t nanosec)
{
    return double(nanosec) / 1000000000.0;
}

#endif /* !defined(MD5BENCH_TIMER_HPP) */

