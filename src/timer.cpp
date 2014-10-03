/*/////////////////////////////////////////////////////////////////////////////
/// @summary Implements the interface to a high-resolution timer.
///////////////////////////////////////////////////////////////////////////80*/

/*////////////////
//   Includes   //
////////////////*/
#if defined(_WIN32) || defined(_WIN64) || defined(WINDOWS)
#define  LLTIMER_WINDOWS
#include <windows.h>
#endif

#ifdef __APPLE__
#define  LLTIMER_APPLE
#include <mach/mach.h>
#include <mach/mach_time.h>
#endif

#ifdef __linux__
#define  LLTIMER_LINUX
#include <time.h>
#endif

#include "timer.hpp"

/*/////////////////
//   Constants   //
/////////////////*/
/// The scale used to convert from seconds into nanoseconds.
static uint64_t const SEC_TO_NANOSEC = 1000000000ULL;

/*///////////////
//   Globals   //
///////////////*/
#ifdef LLTIMER_WINDOWS
/// @summary Store the results of our call to QueryPerformanceFrequency.
static LARGE_INTEGER             g_Frequency = {0};
#endif

#ifdef LLTIMER_APPLE
/// @summary Store the results of our call to mach_timebase_info.
static mach_timebase_info_data_t g_Frequency = {0};
#endif

/*///////////////////////
//   Local Functions   //
///////////////////////*/
#ifdef LLTIMER_WINDOWS
/// @summary Performs any one-time initialization for the Windows platforms.
/// We call QueryPerformanceFrequency() to retrieve the scale factor used to
/// convert a timestamp to seconds.
/// @return true if high-resolution timing is supported on this system.
static bool platform_init(void)
{
    if (QueryPerformanceFrequency(&g_Frequency))
        return true;
    else
        return false;
}

/// @summary Retrieve the current timestamp.
/// @return The current time value, in nanoseconds relative to some unspecified point.
static inline uint64_t platform_time(void)
{
    LARGE_INTEGER tsc = {0};
    LARGE_INTEGER tsf = g_Frequency;
    QueryPerformanceCounter(&tsc);
    return (SEC_TO_NANOSEC * uint64_t(tsc.QuadPart) / uint64_t(tsf.QuadPart));
}

/// @summary Performs any platform-specific cleanup for the timing system.
static void platform_close(void)
{
    /* empty */
}
#endif /* defined(LLTIMER_WINDOWS) */




#ifdef LLTIMER_APPLE
/// @summary Performs any one-time initialization for Mach-based platforms.
/// We call mach_timebase_info() to retrieve the scale factor used to convert
/// a timestamp to nanoseconds.
/// @return true if high-resolution timing services are available.
static bool platform_init(void)
{
    mach_timebase_info(&g_Frequency);
    return true;
}

/// @summary Retrieve the current timestamp.
/// @return The current time value, in nanoseconds relative to some unspecified point.
static inline uint64_t platform_time(void)
{
    return (mach_absolute_time() * g_Frequency.numer / g_Frequency.denom);
}

/// @summary Performs any platform-specific cleanup for the timing system.
static void platform_close(void)
{
    /* empty */
}
#endif




#ifdef LLTIMER_LINUX
/// @summary Performs any one-time initialization for Linux platforms. We 
/// check to ensure that CLOCK_MONOTONIC is available on the system.
/// @return true if high-resolution timing services are available.
static bool platform_init(void)
{
    struct timespec tsc;
    if (clock_gettime(CLOCK_MONOTONIC, &tsc) == 0)
        return true;
    else
        return false;
}

/// @summary Retrieve the current timestamp.
/// @return The current time value, in nanoseconds relative to some unspecified point.
static inline uint64_t platform_time(void)
{
    struct timespec tsc;
    clock_gettime(CLOCK_MONOTONIC, &tsc);
    return (SEC_TO_NANOSEC * uint64_t(tsc.tv_sec) + uint64_t(tsc.tv_nsec));
}

/// @summary Performs any platform-specific cleanup for the timing system.
static void platform_close(void)
{
    /* empty */
}
#endif

/*////////////////////////
//   Public Functions   //
////////////////////////*/
bool LLCALL_C time_service_open(void)
{
    return platform_init();
}

void LLCALL_C time_service_close(void)
{
    return platform_close();
}

uint64_t LLCALL_C time_service_read(void)
{
    return platform_time();
}

