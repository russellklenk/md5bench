/*/////////////////////////////////////////////////////////////////////////////
/// @summary Provides an interface to the filesystem for performing I/O ops.
/// The implementation files are platform-specific, but assume that reads and
/// writes of aligned 32-bit values are atomic on the platform.
///////////////////////////////////////////////////////////////////////////80*/

#ifndef MD5BENCH_IO_HPP
#define MD5BENCH_IO_HPP

/*////////////////
//   Includes   //
////////////////*/
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <assert.h>
#include "common.hpp"

/*////////////////////
//   Preprocessor   //
////////////////////*/
#ifndef IO_C_API
#define IO_C_API(return_type)    extern return_type LLCALL_C
#endif

/*/////////////////
//   Constants   //
/////////////////*/
/// @summary A special timeout value indicating 'no timeout'. Blocking 
/// operations will never time out.
#ifndef IO_WAIT_FOREVER
#define IO_WAIT_FOREVER    0xFFFFFFFFU
#endif

/// @summary A special identifier meaning 'all active jobs'.
#ifndef IO_ALL_ACTIVE
#define IO_ALL_ACTIVE      0xFFFFFFFFU
#endif

/// @summary Define the maximum number of ranges that can be specified
/// for reading chunks of a file.
#ifndef IO_MAX_RANGES
#define IO_MAX_RANGES      16
#endif

/// @summary Define the minimum buffer size for a concurrent read queue.
/// On a system with 4KB pages, this is enough for 256 in-flight operations, 
/// assuming a maximum read size of 4KB.
#ifndef IO_MIN_BUFFER_SIZE
#define IO_MIN_BUFFER_SIZE 1024 * 1024
#endif

/// @summary Define the absolute minimum size of any I/O buffer, in bytes. 
/// This corresponds to the typical minimum sector size.
#ifndef IO_MIN_READ_SIZE
#define IO_MIN_READ_SIZE   512
#endif

/// @summary Specify IO_ONE_PAGE in io_rdq_config_t::MaxReadSize to limit the 
/// maximum read size to the size of one virtual memory manager page.
#define IO_ONE_PAGE        0

/// @summary Define the maximum number of files that can possibly be active in 
/// an io_rdq_t at any given time.
#define IO_MAX_FILES       64

/*////////////////////////////
//   Forward Declarations   //
////////////////////////////*/
struct io_rdq_t;                   /// Manages concurrent read operations.
struct io_rdop_t;                  /// A single successful read operation.
struct io_rdq_job_t;               /// Input parameters for a file read job.
struct io_rdq_result_t;            /// A completed file read job.

template <typename T>
struct io_srsw_fifo_t;             /// A SRSW-safe concurrent queue.

template <typename T>
struct io_srsw_waitable_fifo_t;    /// A SRSW-safe concurrent waitable queue.

typedef io_srsw_fifo_t<void*>                    io_returnq_t; /// SRSW FIFO for buffer returns.
typedef io_srsw_fifo_t<uint32_t>                 io_rdstopq_t; /// SRSW FIFO for cancelling active jobs.
typedef io_srsw_waitable_fifo_t<io_rdop_t>       io_rdopq_t;   /// SRSW FIFO for completed reads.
typedef io_srsw_waitable_fifo_t<io_rdq_job_t>    io_rdpendq_t; /// SRSW FIFO for submitting jobs.
typedef io_srsw_waitable_fifo_t<io_rdq_result_t> io_rddoneq_t; /// SRSW FIFO for completed jobs.

/*//////////////////
//   Data Types   //
//////////////////*/
/// @summary Status reported to the caller with a completed I/O request.
enum io_job_status_e
{
    IO_STATUS_UNKNOWN   = (0<<0),  /// The job status is unknown.
    IO_STATUS_SUCCESS   = (1<<0),  /// The job completed successfully, and the file is closed.
    IO_STATUS_EOF       = (1<<1),  /// End-of-file was reached, and the file is closed.
    IO_STATUS_ERROR     = (1<<2),  /// An error occurred, and the file is closed.
    IO_STATUS_CANCELLED = (1<<3)   /// The job was cancelled, and the file is closed.
};

/// @summary Represents a growable list of UTF-8 file paths. Designed to support
/// load-in-place, so everything must be explicitly sized, and should generally 
/// store byte offsets instead of pointers within data buffers.
struct io_file_list_t
{
    uint32_t      PathCapacity;    /// The current capacity, in paths.
    uint32_t      PathCount;       /// The number of paths items currently in the list.
    uint32_t      BlobCapacity;    /// The current capacity of PathData, in bytes.
    uint32_t      BlobCount;       /// The number of bytes used in PathData.
    uint32_t      MaxPathBytes;    /// The maximum length of any path in the list, in bytes.
    uint32_t      TotalBytes;      /// The total number of bytes allocated.
    uint32_t     *HashList;        /// Hash values calculated for the paths in the list.
    uint32_t     *SizeList;        /// Length values (not including the zero byte) for each path.
    uint32_t     *PathOffset;      /// Offsets, in bytes, into PathData to the start of each path.
    char         *PathData;        /// Raw character data. PathList points into this blob.
};

/// @summary Defines the data associated with a fixed-size, lookaside queue. 
/// Lookaside means that the storage for items are stored and managed outside 
/// of the structure. The queue is safe for concurrent access by a single reader
/// and a single writer. Reads and writes of 32-bit values must be atomic. DO 
/// NOT DIRECTLY ACCESS THE FIELDS OF THIS STRUCTURE. This is used internally 
/// by several of the data types in the platform-specific portions of the I/O 
/// module, but this structure and its operations are cross-platform.
struct io_srsw_flq_t
{
    uint32_t      PushedCount;     /// Number of push operations performed.
    uint32_t      PoppedCount;     /// Number of pop operations performed.
    uint32_t      Capacity;        /// The queue capacity. Always a power-of-two.
};

/// @summary Describes a single portion of a file.
struct io_range_t
{
    uint64_t      Offset;          /// The byte offset of the start of the range.
    uint64_t      Amount;          /// The size of the range, in bytes.
};

/// @summary Describes the configuration for an I/O queue.
struct io_rdq_config_t
{
    io_rdopq_t   *DataQueue;       /// Where to write completed io_rdop_t for processing.
    io_rdpendq_t *PendingQueue;    /// Where to look for pending file processing jobs.
    io_rdstopq_t *CancelQueue;     /// Where to look for pending job cancellations.
    io_rddoneq_t *CompleteQueue;   /// Where to write completed job information.
    size_t        MaxConcurrent;   /// The maximum number of files opened concurrently.
    size_t        MaxBufferSize;   /// The maximum amount of buffer space that can be allocated.
    size_t        MaxReadSize;     /// The maximum number of bytes to read in a single operation.
    bool          Unbuffered;      /// true to use unbuffered I/O operations.
    bool          Asynchronous;    /// true to use asynchronous I/O operations.
};

/// @summary Describes a pending job to be submitted to a read queue.
struct io_rdq_job_t
{
    #define NR    IO_MAX_RANGES
    uint32_t      JobId;           /// A value identifying this job.
    char const   *Path;            /// The path of the file to read.
    uint64_t      EnqueueTime;     /// Nanosecond timestamp. Set to zero.
    size_t        RangeCount;      /// The number of ranges defined.
    io_range_t    RangeList[NR];   /// A description of each range to read.
    #undef NR
};

/// @summary Describes the results of a job completed by a read queue.
struct io_rdq_result_t
{
    uint32_t      JobId;           /// The job identifier.
    uint32_t      Status;          /// Combination of io_job_status_e.
    uint32_t      OSError;         /// Any error code returned by the OS.
    uint32_t      Reserved1;       /// Reserved for future use.
    uint64_t      BytesTotal;      /// The total number of bytes in the file.
    uint64_t      BytesJob;        /// The total number of bytes requested to be read.
    uint64_t      BytesRead;       /// The total number of bytes actually read.
    uint64_t      NanosWait;       /// The total number of nanoseconds the job was waiting.
    uint64_t      NanosRead;       /// The total number of nanoseconds spent doing I/O.
    uint64_t      NanosTotal;      /// The total number of nanoseconds to process the job.
};

/// @summary Describes the result of a successful read operation. The read may
/// be only a partial read; each read operation is at most the number of bytes
/// specified by the io_rdq_config_t::MaxReadSize field. The data can be queued
/// for later processing. When processing has completed, you must call:
/// io_rdq_complete_dp(io_read->ReturnQueue, io_read->DataBuffer).
/// Instances of io_read_t are placed in the read list identified by the 
/// io_rdq_config_t::DataQueue field.
struct io_rdop_t
{
    uint32_t      Id;              /// The application-defined ID associated with the file.
    uint32_t      DataAmount;      /// The maximum number of bytes to read from DataBuffer.
    void         *DataBuffer;      /// Pointer to the first available byte.
    io_returnq_t *ReturnQueue;     /// The queue to which the buffer should be returned.
    uint64_t      FileOffset;      /// The byte offset within the file at which the read started.
};

/*/////////////////
//   Functions   //
/////////////////*/
#ifdef __cplusplus
extern "C" {
#endif

/// @summary Atomically writes a 32-bit unsigned integer value to a given address.
/// Ensure that this function is not inlined by the compiler.
/// @param address The address to write to. This address must be 32-bit aligned.
/// @param value The value to write to address.
IO_C_API(void) io_atomic_write_uint32_aligned(uintptr_t address, uint32_t value);

/// @summary Atomically writes a pointer-sized value to a given address. 
/// Ensure that this function is not inlined by the compiler.
/// @param address The address to write to. This address must be aligned to the pointer size.
/// @param value The value to write to address.
IO_C_API(void) io_atomic_write_pointer_aligned(uintptr_t address, uintptr_t value);

/// @summary Atomically reads a 32-bit unsigned integer value from a given address.
/// Ensure that this function is not inlined by the compiler.
/// @param address The address to write to. This address must be 32-bit aligned.
/// @return The value read from the specified address.
IO_C_API(uint32_t) io_atomic_read_uint32_aligned(uintptr_t address);

/// @summary Calculates a 32-bit hash value for a path string. Case is ignored,
/// and forward and backslashes are treated as equivalent.
/// @param path A NULL-terminated UTF-8 path string.
/// @param out_end On return, points to one byte past the zero byte.
/// @return The hash of the specified string.
IO_C_API(uint32_t) io_hash_path(char const *path, char const **out_end);

/// @summary Allocates resources for and initializes a new file list.
/// @param list The file list to initialize.
/// @param capacity The initial capacity, in number of paths.
/// @param path_bytes The total number of bytes to allocate for path data.
/// @return true if the file list was initialized successfully.
IO_C_API(bool) io_create_file_list(io_file_list_t *list, size_t capacity, size_t path_bytes);

/// @summary Releases resources associated with a file list.
/// @param list The file list to delete.
IO_C_API(void) io_delete_file_list(io_file_list_t *list);

/// @summary Ensures that the file list has a specified minimum capacity, and if not, grows.
/// @param list The file list to check and possibly grow.
/// @param capacity The minimum number of paths that can be stored.
/// @param path_bytes The minimum number of bytes for storing path data.
/// @return true if the file list has the specified capacity.
IO_C_API(bool) io_ensure_file_list(io_file_list_t *list, size_t capacity, size_t path_bytes);

/// @summary Appends an item to the file list, growing it if necessary.
/// @param list The file list to modify.
/// @param path A NULL-terminated UTF-8 file path to append.
IO_C_API(void) io_append_file_list(io_file_list_t *list, char const *path);

/// @summary Resets a file list to empty without freeing any resources.
/// @param list The file list to reset.
IO_C_API(void) io_clear_file_list (io_file_list_t *list);

/// @summary Retrieves a path string from a file list.
/// @param list The file list to query.
/// @param index The zero-based index of the path to retrieve.
/// @return A pointer to the start of the path string, or NULL.
IO_C_API(char const*) io_file_list_path(io_file_list_t const *list, size_t index);

/// @summary Searches for a given hash value within the file list.
/// @param list The file list to search.
/// @param hash The 32-bit unsigned integer hash of the search path.
/// @param start The zero-based starting index of the search.
/// @param out_index On return, this location is updated with the index of the
/// item within the list. This index is valid until the list is modified.
/// @return true if the item was found in the list.
IO_C_API(bool) io_search_file_list_hash(io_file_list_t const *list, uint32_t hash, size_t start, size_t *out_index);

/// @summary Locates a specific path within the file list.
/// @param list The file list to search.
/// @param path A NULL-terminated UTF-8 file path to search for.
/// @param out_index On return, this location is updated with the index of the
/// item within the list. This index is valid until the list is modified.
/// @return true if the item was found in the list.
IO_C_API(bool) io_search_file_list_path(io_file_list_t const *list, char const *path, size_t *out_index);

/// @summary Verifies a file list, ensuring there are no hash collisions. This
/// operation is O(n^2) and thus expensive for large lists.
/// @param list The file list to verify.
/// @return true if the list contains no hash collisions.
IO_C_API(bool) io_verify_file_list(io_file_list_t const *list);

/// @summary Pretty-prints a file list to a buffered stream.
/// @param fp The output stream. This is typically stdout or stderr.
/// @param list The file list to format and write to the output stream.
IO_C_API(void) io_format_file_list(FILE *fp, io_file_list_t const *list);

/// @summary Enumerates all files under a directory that match a particular filter.
/// The implementation of this function is platform-specific.
/// @param dest The file list to populate with the results. The list is not cleared by the call.
/// @param path The path to search. If this is an empty string, the CWD is searched.
/// @param filter The filter string used to accept filenames. The filter '*' accepts everything.
/// @param recurse Specify true to recurse into subdirectories.
/// @return true if enumeration completed without error.
IO_C_API(bool) io_enumerate_files(io_file_list_t *dest, char const *path, char const *filter, bool recurse);

/// @summary Allocates resources for and initializes a concurrent read queue.
/// @param config Options used to specify the queue behavior. This structure 
/// will be updated with the actual configuration values used.
/// @return A pointer to the new queue, or NULL.
IO_C_API(io_rdq_t*) io_create_rdq(io_rdq_config_t &config);

/// @summary Frees resources associated with a concurrent read queue. The queue
/// should be idle with no pending jobs prior to being freed. To ensure that 
/// this is the case, call io_rdq_cancel_all(), io_rdq_poll() [one or more times],
/// and either io_rdq_poll_idle() or io_rdq_wait_idle(), depending on your design.
/// @param rdq The read queue to delete.
IO_C_API(void) io_delete_rdq(io_rdq_t *rdq);

/// @summary Determines whether the queue is idle, meaning that there are no 
/// pending jobs, no active jobs, and no jobs waiting on buffers to be returned.
/// This is typically used by either the job manager or the completion monitor.
/// @param rdq The read queue to poll for status.
/// @return true if the queue is idle.
IO_C_API(bool) io_rdq_poll_idle(io_rdq_t *rdq);

/// @summary Block the calling thread until the queue becomes idle, meaning 
/// that there are no pending jobs, no active jobs, and no jobs waiting on 
/// buffers to be returned. This is typically used by either the job manager 
/// or the completion monitor.
/// @param rdq The read queue to wait on.
/// @param timeout_ms The maximum amount of time to wait, in milliseconds. 
/// Specify IO_WAIT_FOREVER to block indefinitely.
/// @return true if the queue has become idle, or false if the timeout interval
/// elapsed or an error occurred. 
IO_C_API(bool) io_rdq_wait_idle(io_rdq_t *rdq, uint32_t timeout_ms);

/// @summary Cancels all pending and active jobs. Note that there may still be
/// jobs waiting for buffers to be returned; if this is the case, the queue will
/// report as not idle after the next poll cycle. This is typically used by the job manager.
/// @param rdq The target queue.
IO_C_API(void) io_rdq_cancel_all(io_rdq_t *rdq);

/// @summary Cancels a single specific job on the next poll cycle. This is 
/// typically used by the job manager.
/// @param rdq The target queue.
/// @param id The identifier of the job to cancel, as was specified on the job
/// definition passed to io_rdq_submit() used to add the job.
IO_C_API(void) io_rdq_cancel_one(io_rdq_t *rdq, uint32_t id);

/// @summary Submits a new job to the pending queue of a concurrent read queue.
/// @param rdq The target queue.
/// @param job A description of the file operation.
/// @return true if the job was added to the pending queue, or false if the 
/// pending queue is full and the job could not be submitted.
IO_C_API(bool) io_rdq_submit(io_rdq_t *rdq, io_rdq_job_t const &job);

#ifdef __cplusplus
}; // extern "C"
#endif

/*////////////////////////
//   Inline Functions   //
////////////////////////*/
/// @summary Clears or initializes a SRSW fixed lookaside queue to empty.
/// @param srswq The queue to initialize.
/// @param capacity The queue capacity. This must be a power-of-two.
static inline void io_srsw_flq_clear(io_srsw_flq_t &srswq, uint32_t capacity)
{
    // the assumption is that either:
    // 1. this is only done once before the queue is used, or
    // 2. there will be some higher-level sync point that's clearing the queue
    //    so additional synchronization doesn't need to be performed here.
    assert((capacity & (capacity-1)) == 0); // capacity is a power-of-two.
    srswq.PushedCount = 0;
    srswq.PoppedCount = 0;
    srswq.Capacity    = capacity;
}

/// @summary Retrieves the number of items currently available in a SRSW fixed 
/// lookaside queue. Do not pop more than the number of items returned by this call.
/// @param srswq The queue to query.
static inline uint32_t io_srsw_flq_count(io_srsw_flq_t &srswq)
{
    uintptr_t pushed_cnt_addr = (uintptr_t) &srswq.PushedCount;
    uintptr_t popped_cnt_addr = (uintptr_t) &srswq.PoppedCount;
    uint32_t  pushed_cnt      = io_atomic_read_uint32_aligned(pushed_cnt_addr);
    uint32_t  popped_cnt      = io_atomic_read_uint32_aligned(popped_cnt_addr);
    return (pushed_cnt - popped_cnt); // unsigned; don't need to worry about overflow.
}

/// @summary Checks whether a SRSW fixed lookaside queue is full. Check this 
/// before pushing an item into the queue.
/// @param srswq The queue to query.
/// @return true if the queue is full.
static inline bool io_srsw_flq_full(io_srsw_flq_t &srswq)
{
    return (io_srsw_flq_count(srswq) == srswq.Capacity);
}

/// @summary Checks whether a SRSW fixed lookaside queue is empty. Check this 
/// before popping an item from the queue.
/// @param srswq The queue to query.
/// @return true if the queue is empty.
static inline bool io_srsw_flq_empty(io_srsw_flq_t &srswq)
{
    return (io_srsw_flq_count(srswq) == 0);
}

/// @summary Gets the index the next push operation will write to. This must be
/// called only by the producer prior to calling io_srsw_flq_push(). 
static inline uint32_t io_srsw_flq_next_push(io_srsw_flq_t &srswq)
{
    uintptr_t pushed_cnt_addr = (uintptr_t) &srswq.PushedCount;
    uint32_t  pushed_cnt      = io_atomic_read_uint32_aligned(pushed_cnt_addr);
    return (pushed_cnt & (srswq.Capacity - 1));
}

/// @summary Implements a push operation in a SRSW fixed lookaside queue. This 
/// must be called only from the producer.
/// @param srswq The queue to update.
static inline void io_srsw_flq_push(io_srsw_flq_t &srswq)
{
    uintptr_t pushed_cnt_addr = (uintptr_t) &srswq.PushedCount;
    uint32_t  pushed_cnt      = io_atomic_read_uint32_aligned(pushed_cnt_addr) + 1;
    io_atomic_write_uint32_aligned(pushed_cnt_addr, pushed_cnt);
}

/// @summary Gets the index the next pop operation will read from. This must be 
/// called only by the consumer prior to popping an item from the queue.
static inline uint32_t io_srsw_flq_next_pop(io_srsw_flq_t &srswq)
{
    uintptr_t popped_cnt_addr = (uintptr_t) &srswq.PoppedCount;
    uint32_t  popped_cnt      = io_atomic_read_uint32_aligned(popped_cnt_addr);
    return (popped_cnt & (srswq.Capacity - 1));
}

/// @summary Implements a pop operation in a SRSW fixed lookaside queue. This must
/// be called only from the consumer against a non-empty queue.
/// @param srswq The queue to update.
static inline void io_srsw_flq_pop(io_srsw_flq_t &srswq)
{
    uintptr_t popped_cnt_addr = (uintptr_t) &srswq.PoppedCount;
    uint32_t  popped_cnt      = io_atomic_read_uint32_aligned(popped_cnt_addr) + 1;
    io_atomic_write_uint32_aligned(popped_cnt_addr, popped_cnt);
}

#endif /* !defined(MD5BENCH_IO_HPP) */

