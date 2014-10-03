/*/////////////////////////////////////////////////////////////////////////////
/// @summary Provides an interface to the filesystem for performing I/O ops.
/// The implementation files are platform-specific, but assumes that reads and
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

/// @summary Define the maximum number of ranges that can be specified
/// for reading chunks of a file.
#ifndef IO_MAX_RANGES
#define IO_MAX_RANGES      64
#endif

/*////////////////////////////
//   Forward Declarations   //
////////////////////////////*/
struct io_rdq_t;
struct io_rdlist_t;

/*//////////////////
//   Data Types   //
//////////////////*/
/// @summary Status reported to the caller with a completed I/O request.
enum io_status_e
{
    IO_STATUS_NONE  = (0 << 0), /// No status is being reported.
    IO_STATUS_EOF   = (1 << 0), /// End-of-file was reached, and the file is closed.
    IO_STATUS_ERROR = (1 << 1)  /// An error occurred, and the file is closed.
};

/// @summary Represents a growable list of UTF-8 file paths. Designed to support
/// load-in-place, so everything must be explicitly sized, and should generally 
/// store byte offsets instead of pointers within data buffers.
struct io_file_list_t
{
    uint32_t     PathCapacity;  /// The current capacity, in paths.
    uint32_t     PathCount;     /// The number of paths items currently in the list.
    uint32_t     BlobCapacity;  /// The current capacity of PathData, in bytes.
    uint32_t     BlobCount;     /// The number of bytes used in PathData.
    uint32_t     MaxPathBytes;  /// The maximum length of any path in the list, in bytes.
    uint32_t     TotalBytes;    /// The total number of bytes allocated.
    uint32_t    *HashList;      /// Hash values calculated for the paths in the list.
    uint32_t    *SizeList;      /// Length values (not including the zero byte) for each path.
    uint32_t    *PathOffset;    /// Offsets, in bytes, into PathData to the start of each path.
    char        *PathData;      /// Raw character data. PathList points into this blob.
};

/// @summary Represents a list of buffers used as the target of read operations.
/// Each buffer in the list has the same size and alignment.
struct io_rdbuf_list_t
{
    size_t       Alignment;     /// The allocation alignment for buffers in this list.
    size_t       BuffersTotal;  /// The total number of buffers (free and used).
    size_t       BuffersUsed;   /// The number of buffers currently in use.
    void       **BufferAddress; /// Pointers to the start of each buffer.
    size_t      *BufferAmounts; /// The number of bytes actually used in each buffer.
    size_t       BufferSize;    /// The size of each buffer, in bytes.
    size_t       BytesAllocated;/// The total number of bytes allocated.
    size_t       BytesUsed;     /// The total number of bytes actually in-use.
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
    uint32_t     PushedCount;   /// Number of push operations performed.
    uint32_t     PoppedCount;   /// Number of pop operations performed.
    uint32_t     Capacity;      /// The queue capacity. Always a power-of-two.
};

/// @summary Describes the configuration for an I/O queue.
struct io_rdq_config_t
{
    io_rdlist_t *ReadList;      /// Where to write completed reads.
    size_t       MaxConcurrent; /// The maximum number of files opened concurrently.
    size_t       MaxBufferSize; /// The maximum amount of buffer space that can be allocated.
    size_t       BufferSize;    /// The number of bytes to read in a single operation.
    bool         AsyncIo;       /// true to use asynchronous I/O operations.
};

/// @summary Describes a single portion of a file to be read.
struct io_range_t
{
    uint64_t     Offset;        /// The byte offset at which to begin reading.
    uint64_t     Amount;        /// The number of bytes to read.
};

/// @summary Describes the result of a read operation.
struct io_read_t
{
    uint64_t     RangeOffset;   /// The byte offset of the start of the current range.
    uint64_t     FileOffset;    /// The byte offset within the file at which the read started.
    size_t       Amount;        /// The amount of data available in Buffer, in bytes.
    void        *Buffer;        /// Pointer to the fiirst available byte.
    uint32_t     Status;        /// One of io_status_e.
    uint32_t     Id;            /// The application-defined ID associated with the file.
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

/// @summary Creates and initializes a new read buffer list.
/// @param list The read buffer list to initialize.
/// @param buffer_align A power-of-two value specifying the desired buffer alignment.
/// @param buffer_size The usable size of each buffer, in bytes.
/// @param capacity The number of buffers to pre-allocate.
/// @return true if the read buffer list was created successfully.
IO_C_API(bool) io_create_rdbuf_list(io_rdbuf_list_t *list, size_t buffer_align, size_t buffer_size, size_t capacity);

/// @summary Frees resources associated with a read buffer list.
/// @param list The read buffer list to delete.
IO_C_API(void) io_delete_rdbuf_list(io_rdbuf_list_t *list);

/// @summary Returns all allocated buffers to the free list.
/// @param list The read buffer list to flush.
IO_C_API(void) io_flush_rdbuf_list(io_rdbuf_list_t *list);

/// @summary Retrieves a buffer from a read buffer list. If no buffers are 
/// currently available, a new buffer is allocated and returned. Note that 
/// this function is not safe for concurrent access by multiple threads.
/// @param list The read buffer list servicing the request.
/// @param out_amount_addr On return, this points to the address storing the 
/// amount used in the buffer. You may read from and write to this location to
/// update the amount used in the buffer.
/// @return A pointer to the buffer, or NULL.
IO_C_API(void*) io_rdbuf_list_get(io_rdbuf_list_t *list, size_t *&out_amount_addr);

/// @summary Returns a buffer to a read buffer list, making it available for use.
/// Note that this function is not safe for concurrent access by multiple threads.
/// @param list The read buffer list that returned the buffer.
/// @param buffer The buffer returned by io_rdbuf_list_get(list, ...).
IO_C_API(void) io_rdbuf_list_put(io_rdbuf_list_t *list, void *buffer);

/// @summary Creates a new, empty list for storing completed read operations. 
/// The list is safe for concurrent access by a single reader and single writer.
/// @return A pointer to the new read list, or NULL.
IO_C_API(io_rdlist_t*) io_create_read_list(void);

/// @summary Frees resources associated with a read operation list.
/// @param rdlist The read list to delete.
IO_C_API(void) io_delete_read_list(io_rdlist_t *rdlist);

/// @summary Waits for one or more completed read operations to become available.
/// This function should be called only from the consumer. It does not  guarantee
/// that read operations have completed, so be sure to check by calling io_read_list_available() 
/// to get the number that have actually completed.
/// @param rdlist The read list to wait on.
/// @param timeout_ms The maximum number of milliseconds to wait.
/// @return true if at least one completed read operation is available, or false 
/// if the wait operation timeout interval elapsed or an error occurred.
IO_C_API(bool) io_wait_read_list(io_rdlist_t *rdlist, uint32_t timeout_ms);

/// @summary Retrieves the number of completed read operations. This function 
/// should be called only from the processing thread.
/// @param rdlist The read list to query.
/// @return The number of completed read operations available to retrieve.
IO_C_API(size_t) io_read_list_available(io_rdlist_t *rdlist);

/// @summary Retrieves information about a completed read operation. This 
/// function should be called only from the consumer, and in the case where 
/// the consumer knows that io_read_list_available(rdlist) > 0.
/// @param rdlist The read list to query.
/// @param out_op Information about the completed operation is copied here.
IO_C_API(void) io_read_list_get(io_rdlist_t *rdlist, io_read_t *out_op);

/// @summary Sets the number of completed read operations to zero. This function
/// should be called only from the consumer.
/// @param rdlist The read list to flush.
IO_C_API(void) io_flush_read_list(io_rdlist_t *rdlist);

/// @summary Appends a completed read operation to a read list. This function 
/// should be called only by the producer.
/// @param rdlist The read list to post the I/O operation to.
/// @param op Information about the completed operation.
IO_C_API(void) io_read_list_put(io_rdlist_t *rdlist, io_read_t const &op);

/// @summary Create a new concurrent file read queue. The read queue manages 
/// opening a set of files, reading them sequentially from beginning to end,
/// placing the read data into a list of data to process, and then closing the
/// file once all data has been processed. The design allows for maximum overlap
/// between I/O and processing of the data. The read queue is safe for a single
/// concurrent producer (calls io_rdq_add(), etc.) and a single consumer that
/// calls io_[wait|poll]_rdq_io().
/// @param config Configuration parameters for the queue.
/// @return A pointer to the initialized queue, or NULL.
IO_C_API(io_rdq_t*) io_create_rdq(io_rdq_config_t const *config);

/// @summary Delete a concurrent file read queue. All pending I/O operations 
/// are cancelled, all files are closed, all buffers are deleted, and the 
/// results list is flushed.
/// @param rdq The read queue to delete.
IO_C_API(void) io_delete_rdq(io_rdq_t *rdq);

/// @summary Block the calling thread until a file slot becomes available.
/// @param rdq The read queue to wait on.
/// @param timeout_ms The maximum number of milliseconds to wait.
/// @return true if a file slot became available, or false if the timeout 
/// interval elapsed or an error occurred.
IO_C_API(bool) io_wait_rdq_available(io_rdq_t *rdq, uint32_t timeout_ms);

/// @summary Block the calling thread until all active jobs have been completed.
/// @param rdq The read queue to wait on.
/// @param timeout_ms The maximum number of milliseconds to wait.
/// @return true if all active jobs have been completed, or false if the timeout
/// interval elapsed or an error occurred.
IO_C_API(bool) io_wait_rdq_empty(io_rdq_t *rdq, uint32_t timeout_ms);

/// @summary Block the calling thread until at least one pending I/O operation
/// has completed, and schedule any new I/O operations. 
/// @param rdq The read queue to wait on.
/// @param timeout_ms The maximum number of milliseconds to wait.
/// @return true if an I/O operation completed, or false if the timeout interval
/// elapsed or an error occurred.
IO_C_API(bool) io_wait_rdq_io(io_rdq_t *rdq, uint32_t timeout_ms);

/// @summary Synchronously poll a read queue to update the state of all pending
/// I/O operations and schedule any new I/O operations. This lets you schedule
/// the I/O start and completion within some larger process.
/// @param rdq The read queue to update.
IO_C_API(void) io_poll_rdq_io(io_rdq_t *rdq);

/// @summary Queues a single file to be read. The file is opened synchronously 
/// and the first read operation is scheduled and started.
/// @param rdq The read queue to manage the operation.
/// @param path The path of the file to read.
/// @param id An application-defined identifier associated with the file.
/// @param range_list A list of offset/size pairs specifying the portions of 
/// the file to read, or specify NULL to read the entire file from beginning to end.
/// @param range_count The number of items in range_list. At most IO_MAX_RANGES.
/// @param out_size On return, stores the total size of the file, in bytes.
/// @return true if the file was added to the queue.
IO_C_API(bool) io_rdq_add(io_rdq_t *rdq, char const *path, uint32_t id, io_range_t const *range_list, size_t range_count, uint64_t *out_size);

/// @summary Queries a read queue for the number of available file slots.
/// @param rdq The read queue to query.
/// @return The number of available file slots.
IO_C_API(size_t) io_rdq_available(io_rdq_t *rdq);

/// @summary Cancels all pending I/O operations on a file, and closes the file.
/// @param rdq The read queue managing the file.
/// @param id The ID associated with the file to close.
IO_C_API(void) io_rdq_cancel(io_rdq_t *rdq, uint32_t id);

/// @summary Cancels all pending I/O operations and closes all files.
/// @param rdq The target read queue.
IO_C_API(void) io_rdq_cancel_all(io_rdq_t *rdq);

/// @summary Signals the completion of processing on the data returned by a 
/// read operation, and returns the buffer to the read queue for reuse. This 
/// function must be called by the processor for every completed read operation.
/// @param rdq The read queue that reported the read operation.
/// @param id The unique identifier of the file from which the data was read.
/// @param buffer The buffer returned as part of the io_read_t.
IO_C_API(void) io_rdq_complete(io_rdq_t *rdq, uint32_t id, void *buffer);

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

