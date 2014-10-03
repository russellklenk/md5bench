/*/////////////////////////////////////////////////////////////////////////////
/// @summary Abstract away compiler differences, define some useful macros, and
/// include some definitions required everywhere.
/// @author Russell Klenk (contact@russellklenk.com)
///////////////////////////////////////////////////////////////////////////80*/

#ifndef LL_COMMON_HPP
#define LL_COMMON_HPP

/*///////////////
//   Defines   //
///////////////*/
#define LLPLATFORM_UNKNOWN           0
#define LLPLATFORM_IOS               1
#define LLPLATFORM_ANDROID           2
#define LLPLATFORM_WIN32             3
#define LLPLATFORM_WINRT             4
#define LLPLATFORM_WP8               5
#define LLPLATFORM_OSX               6
#define LLPLATFORM_LINUX             7

#define LLCOMPILER_UNKNOWN           0
#define LLCOMPILER_GNUC              1
#define LLCOMPILER_MSVC              2

/// @summary The current target platform.
#define LLTARGET_PLATFORM            LLPLATFORM_UNKNOWN

/// @summary The current target compiler. Note that LLCOMPILER_GNUC
/// is also specified when building with clang.
#define LLTARGET_COMPILER            LLCOMPILER_UNKNOWN

/// @summary Compiler detection - Microsoft Visual C++.
#ifdef _MSC_VER
    #undef  LLTARGET_COMPILER
    #define LLTARGET_COMPILER        LLCOMPILER_MSVC
#endif

/// @summary Compiler detection - GNU C/C++, CLANG and Intel C/C++.
#ifdef __GNUC__
    #undef  LLTARGET_COMPILER
    #define LLTARGET_COMPILER        LLCOMPILER_GNUC
#endif

/// @summary Platform detection - Apple OSX and iOS.
#ifdef __APPLE__
    #if TARGET_OS_IPHONE || TARGET_IPHONE_SIMULATOR
        #undef  LLTARGET_PLATFORM
        #define LLTARGET_PLATFORM    LLPLATFORM_IOS
        #define LLCALL_C
    #else
        #undef  LLTARGET_PLATFORM
        #define LLTARGET_PLATFORM    LLPLATFORM_OSX
        #define LLCALL_C
    #endif
#endif

/// @summary Platform detection - Android.
#ifdef ANDROID
    #undef  LLTARGET_PLATFORM
    #define LLTARGET_PLATFORM        LLPLATFORM_ANDROID
    #define LLCALL_C
#endif

/// @summary Platform detection - Linux.
#ifdef __linux__
    #undef  LLTARGET_PLATFORM
    #define LLTARGET_PLATFORM        LLPLATFORM_LINUX
    #define LLCALL_C
#endif

/// @summary Platform detection - Windows desktop, surface and phone.
#if defined(_WIN32) || defined(_WIN64) || defined(WINDOWS)
    #if defined(_WIN32) && !defined(_WIN64)
        #define LLCALL_C             __cdecl
    #else
        #define LLCALL_C             
    #endif
    #if defined(WINRT)
        #undef  LLTARGET_PLATFORM
        #define LLTARGET_PLATFORM    LLPLATFORM_WINRT
    #elif defined(WP8)
        #undef  LLTARGET_PLATFORM
        #define LLTARGET_PLATFORM    LLPLATFORM_WP8
    #else
        #undef  LLTARGET_PLATFORM
        #define LLTARGET_PLATFORM    LLPLATFORM_WIN32
    #endif
#endif

/// @summary Force a compiler error if we can't detect the target compiler.
#if LLTARGET_COMPILER == LLCOMPILER_UNKNOWN
    #error Unable to detect the target compiler.
#endif

/// @summary Force a compiler error if we can't detect the target platform.
#if LLTARGET_PLATFORM == LLPLATFORM_UNKNOWN
    #error Unable to detect the target platform.
#endif

/// @summary Force a compiler error if Visual C++ 2012 or newer is not available.
#if LLTARGET_COMPILER == LLCOMPILER_MSVC
    #if _MSC_VER < 1700
        #error Visual C++ 2012 or later is required.
    #endif
#endif

/// @summary Indicate a C-callable API function.
#ifndef LLC_API
#define LLC_API(return_type)         extern return_type LLCALL_C
#endif

/// @summary Indicate the start of a multi-line macro.
#ifndef LLMLMACRO_BEGIN
#define LLMLMACRO_BEGIN              do {
#endif

/// @summary Indicate the end of a multi-line macro.
#ifndef LLMLMACRO_END
    #ifdef _MSC_VER
        #define LLMLMACRO_END                                                 \
            __pragma(warning(push))                                           \
            __pragma(warning(disable:4127))                                   \
            } while (0)                                                       \
            __pragma(warning(pop))
    #else
        #define LLMLMACRO_END                                                 \
            } while (0)
    #endif /* defined(_MSC_VER) */
#endif /* !defined(LLMLMACRO_END) */

/// @summary Internal macro used to suppress warnings about unused variables.
#ifndef LLUNUSED_X
    #ifdef _MSC_VER
        #define LLUNUSED_X(x)        (x)
    #else
        #define LLUNUSED_X(x)        (void)sizeof((x))
    #endif
#endif /* !defined(LLUNUSED_X) */

/// @summary Used to suppress warnings about unused arguments.
#ifndef UNUSED_ARG
#define UNUSED_ARG(x)                                                         \
    LLMLMACRO_BEGIN                                                           \
    LLUNUSED_X(x);                                                            \
    LLMLMACRO_END
#endif /* !defined(UNUSED_ARG) */

/// @summary Used to suppress warnings about unused local variables.
#ifndef UNUSED_LOCAL
#define UNUSED_LOCAL(x)                                                       \
    LLMLMACRO_BEGIN                                                           \
    LLUNUSED_X(x);                                                            \
    LLMLMACRO_END
#endif /* !defined(UNUSED_LOCAL) */

/// @summary Wrap compiler differences used to control structure alignment.
#ifndef LLALIGNMENT_DEFINED
    #ifdef _MSC_VER
        #define LLALIGN_BEGIN(_al)  __declspec(align(_al))
        #define LLALIGN_END(_al)
        #define LLALIGN_OF(_type)   __alignof(_type)
        #define LLALIGNMENT_DEFINED
    #endif /* defined(_MSC_VER) */
    #ifdef __GNUC__
        #define LLALIGN_BEGIN(_al)
        #define LLALIGN_END(_al)    __attribute__((aligned(_al)))
        #define LLALIGN_OF(_type)   __alignof__(_type)
        #define LLALIGNMENT_DEFINED
    #endif /* defined(__GNUC__) */
#endif /* !defined(LLALIGNMENT_DEFINED) */

/// @summary Wrap compiler differences used to force inlining of a function.
#ifndef LLFORCE_INLINE
    #ifdef _MSC_VER
        #define LLFORCE_INLINE       __forceinline
        #define force_inline         __forceinline
    #endif /* defined(_MSC_VER) */
    #ifdef __GNUC__
        #define LLFORCE_INLINE       __attribute__((always_inline))
        #define force_inline         __attribute__((always_inline))
    #endif /* defined(__GNUC__) */
#endif /* !defined(LLFORCE_INLINE) */

/// @summary Wrap compiler differences used to specify lack of aliasing.
/// Note that we can't #define restrict __restrict because MSVC will blow up - 
/// it uses __decltype(restrict) in several of its standard library headers.
#ifndef LLRESTRICT
    #ifdef _MSC_VER
        #define LLRESTRICT           __restrict
        #define llrestrict           __restrict
    #endif /* defined(_MSC_VER) */
    #ifdef __GNUC__
        #define LLRESTRICT           __restrict
        #define llrestrict           __restrict
    #endif /* defined(__GNUC__) */
#endif /*!defined(LLRESTRICT) */

/*////////////////
//   Includes   //
////////////////*/
#include <stddef.h>
#include <stdint.h>

#endif /* !defined(LL_COMMON_HPP) */

