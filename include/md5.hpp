/*/////////////////////////////////////////////////////////////////////////////
/// @summary Provides function signatures for the various MD5 implementations 
/// we are testing. Each has a differnt name.
///////////////////////////////////////////////////////////////////////////80*/

#ifndef MD5BENCH_MD5_HPP
#define MD5BENCH_MD5_HPP

/*////////////////
//   Includes   //
////////////////*/
#include <stddef.h>
#include <stdint.h>
#include "common.hpp"

/*////////////////////
//   Preprocessor   //
////////////////////*/
#ifndef MD5_C_API
#define MD5_C_API(return_type)    extern return_type LLCALL_C
#endif

/*//////////////////
//   Data Types   //
//////////////////*/

/*/////////////////
//   Functions   //
/////////////////*/
#ifdef __cplusplus
extern "C" {
#endif

/// @summary This is the reference implementation of the MD5 hash taken from 
/// https://www.ietf.org/rfc/rfc1321.txt, appendix A. See md5_ref.cpp.
/// @param dst16 An array of 16 bytes where the MD5 will be written.
/// @param src The source data buffer.
/// @param count The number of bytes to read from the source data buffer.
MD5_C_API(void) md5_ref(void *dst16, void const *src, size_t count);

/// @summary The reference MD5 with some simplifications (use stdlib memcpy, 
/// cast instead of recombining bytes since we're little-endian, etc.)
/// @param dst16 An array of 16 bytes where the MD5 will be written.
/// @param src The source data buffer.
/// @param count The number of bytes to read from the source data buffer.
MD5_C_API(void) md5_ref2(void *dst16, void const *src, size_t count);

#ifdef __cplusplus
}; // extern "C"
#endif

#endif /* !defined(MD5BENCH_MD5_HPP) */

