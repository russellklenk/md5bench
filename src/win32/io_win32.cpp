/*/////////////////////////////////////////////////////////////////////////////
/// @summary Implements the Windows platform functions for the I/O module.
///////////////////////////////////////////////////////////////////////////80*/

/*////////////////
//   Includes   //
////////////////*/
#include <windows.h>
#include <ctype.h>
#include "io.hpp"

#ifdef _MSC_VER
    #include <intrin.h>
    #define PROCESSOR_MFENCE_READ             _mm_lfence()
    #define PROCESSOR_MFENCE_WRITE            _mm_sfence()
    #define PROCESSOR_MFENCE_READ_WRITE       _mm_mfence()
#endif

#ifdef __GNUC__
    #if (__GNUC__ > 4) || (__GNUC__ == 4 && __GNUC_MINOR__ >= 1)
        #define PROCESSOR_MFENCE_READ         __sync_synchronize()
        #define PROCESSOR_MFENCE_WRITE        __sync_synchronize()
        #define PROCESSOR_MFENCE_READ_WRITE   __sync_synchronize()
    #elif defined(__ppc__) || defined(__powerpc__) || defined(__PPC__)
        #define PROCESSOR_MFENCE_READ         asm volatile("sync":::"memory")
        #define PROCESSOR_MFENCE_WRITE        asm volatile("sync":::"memory")
        #define PROCESSOR_MFENCE_READ_WRITE   asm volatile("sync":::"memory")
    #elif defined(__i386__) || defined(__i486__) || defined(__i586__) || \
          defined(__i686__) || defined(__x86_64__)
        #define PROCESSOR_MFENCE_READ         asm volatile("lfence":::"memory")
        #define PROCESSOR_MFENCE_WRITE        asm volatile("sfence":::"memory")
        #define PROCESSOR_MFENCE_READ_WRITE   asm volatile("mfence":::"memory")
    #endif /* GNU C version */
#endif

/*/////////////////
//   Constants   //
/////////////////*/
/// @summary Define the maximum number of read buffers that have been processed
/// and returned to the read queue, and will be 'freed' at the end of the next 
/// read queue update operation. This value must be a non-zero power-of-two.
#ifndef IO_RDQ_MAX_RETURNS
#define IO_RDQ_MAX_RETURNS   256
#endif

/// @summary The scale used to convert from seconds into nanoseconds.
static uint64_t const SEC_TO_NANOSEC = 1000000000ULL;

/*///////////////
//   Globals   //
///////////////*/
/// @summary Store the results of our call to QueryPerformanceFrequency.
/// This value is set when the first read buffer is created.
static LARGE_INTEGER g_Frequency = {0};

/*//////////////////
//   Data Types   //
//////////////////*/
/// @summary MinGW doesn't define a bunch of these constants and structures.
#ifdef __GNUC__
#define METHOD_BUFFERED                    0
#define FILE_ANY_ACCESS                    0
#define FILE_DEVICE_MASS_STORAGE           0x0000002d

#ifndef QUOTA_LIMITS_HARDWS_MIN_ENABLE
#define QUOTA_LIMITS_HARDWS_MIN_ENABLE     0x00000001
#endif

#ifndef QUOTA_LIMITS_HARDWS_MAX_DISABLE
#define QUOTA_LIMITS_HARDWS_MAX_DISABLE    0x00000008
#endif

#ifndef CTL_CODE
#define CTL_CODE(DeviceType, Function, Method, Access ) (                     \
    ((DeviceType) << 16) | ((Access) << 14) | ((Function) << 2) | (Method)    \
)
#endif

#define IOCTL_STORAGE_BASE           FILE_DEVICE_MASS_STORAGE
#define IOCTL_STORAGE_QUERY_PROPERTY CTL_CODE(IOCTL_STORAGE_BASE, 0x0500, METHOD_BUFFERED, FILE_ANY_ACCESS)

typedef enum _STORAGE_PROPERTY_ID {
    StorageDeviceProperty = 0,
    StorageAdapterProperty,
    StorageDeviceIdProperty, 
    StorageDeviceUniqueIdProperty, 
    StorageDeviceWriteCacheProperty, 
    StorageMiniportProperty, 
    StorageAccessAlignmentProperty, 
    StorageDeviceSeekPenaltyProperty, 
    StorageDeviceTrimProperty, 
    StorageDeviceWriteAggregationProperty, 
    StorageDeviceTelemetryProperty, 
    StorageDeviceLBProvisioningProperty, 
    StorageDevicePowerProperty, 
    StorageDeviceCopyOffloadProperty, 
    StorageDeviceResiliencyPropery
} STORAGE_PROPERTY_ID, *PSTORAGE_PROPERTY_ID;

typedef enum _STORAGE_QUERY_TYPE {
    PropertyStandardQuery = 0,
    propertyExistsQuery, 
    PropertyMaskQuery, 
    PropertyQueryMaxDefined
} STORAGE_QUERY_TYPE, *PSTORAGE_QUERY_TYPE;

typedef struct _STORAGE_PROPERTY_QUERY {
    STORAGE_PROPERTY_ID PropertyId;
    STORAGE_QUERY_TYPE  QueryType;
    UCHAR               AdditionalParameters[1];
} STORAGE_PROPERTY_QUERY, *PSTORAGE_PROPERTY_QUERY;

typedef struct _STORAGE_ACCESS_ALIGNMENT_DESCRIPTOR {
    DWORD Version;
    DWORD Size;
    DWORD BytesPerCacheLine;
    DWORD BytesOffsetForCacheAlignment;
    DWORD BytesPerLogicalSector;
    DWORD BytesPerPhysicalSector;
    DWORD BytesOffsetForSectorAlignment;
} STORAGE_ACCESS_ALIGNMENT_DESCRIPTOR, *PSTORAGE_ACCESS_ALIGNMENT_DESCRIPTOR;

// the following are not available under MinGW. we will load kernel32.dll and 
// resolve these functions manually in io_create_rdbuffer().
typedef void (WINAPI *GetNativeSystemInfoFn)(SYSTEM_INFO*);
typedef BOOL (WINAPI *SetProcessWorkingSetSizeExFn)(HANDLE, SIZE_T, SIZE_T, DWORD);

#endif

/// @summary Bitflags that can be set on a read queue.
enum io_rdq_flags_e
{
    IO_RDQ_FLAGS_NONE               = (0 << 0), /// Prefer synchronous, buffered I/O.
    IO_RDQ_FLAGS_ASYNC              = (1 << 0), /// Prefer asynchronous I/O.
    IO_RDQ_FLAGS_UNBUFFERED         = (1 << 1), /// Prefer unbuffered I/O.
};

/// @summary Bitflags that can be set on io_rdq_file_t::Status.
enum io_rdq_file_status_e
{
    IO_RDQ_FILE_NONE                = (0 << 0), /// No special status.
    IO_RDQ_FILE_PENDING_AIO         = (1 << 0), /// Async read pending.
    IO_RDQ_FILE_FINISHED            = (1 << 1), /// All required data has been read.
    IO_RDQ_FILE_ERROR               = (1 << 2), /// An error occurred, check OSError.
    IO_RDQ_FILE_CANCELLED           = (1 << 3), /// I/O operations were cancelled.
    IO_RDQ_FILE_EOF                 = (1 << 4), /// End-of-file was reported.
};

/// @summary Defines the state associated with a lock-free queue that is safe 
/// for concurrent access by a single reader and single writer. This basic 
/// queue does not support waiting on the queue state.
template <typename T>
struct io_srsw_fifo_t
{
    io_srsw_flq_t    Queue;         /// SRSW queue state & capacity. See io.hpp.
    T               *Store;         /// Fixed-size storage for queue items.
};

/// @summary There are several types of waitable queues defined that all 
/// operate in the same fashion and have the same basic characteristics, 
/// but store different data. Each is safe for a single concurrent reader 
/// and writer. Only one thread should be waiting on the queue. Any waiter
/// is subject to spurious wakeup, and must check the condition they are 
/// waiting on after being woken up (A.R.E = Auto Reset Event.)
template <typename T>
struct io_srsw_waitable_fifo_t
{
    HANDLE           Empty;         /// A.R.E. signaled when queue goes empty.
    HANDLE           NotEmpty;      /// A.R.E. signaled when queue goes not empty.
    HANDLE           NotFull;       /// A.R.E. signaled when queue goes not full.
    io_srsw_flq_t    Queue;         /// SRSW queue state & capacity. See io.hpp.
    T               *Store;         /// Fixed-size storage for queue items.
};

/// @summary Defines the state associated with an I/O read buffer manager. 
/// Memory is allocated in one large chunk, and then divided into fixed-size
/// pages which are an even multiple of the physical sector size of the primary
/// disk, and guaranteed to be at least as large as io_rdq_config_t::MaxReadSize.
/// The memory block is allocated using the system virtual memory manager, and
/// all pages are pinned in physical memory. For this reason, the maximum total 
/// buffer size should remain relatively small. This structure is not safe for 
/// concurrent access. All management is performed on the I/O thread.
struct io_rdbuffer_t
{
    size_t           TotalSize;     /// The total number of bytes allocated.
    size_t           PageSize;      /// The size of a single page, in bytes.
    size_t           AllocSize;     /// The number of pages per-allocation.
    void            *BaseAddress;   /// The base address of the committed range.
    size_t           FreeCount;     /// The number of unallocated AllocSize blocks.
    void           **FreeList;      /// Pointers to the start of each unallocated block.
};

/// @summary Stores the data associated with a single started, but not completed,
/// file read job. The file is read sequentially from beginning to end, possibly
/// skipping ranges of bytes, and is then closed. The job is completed when all
/// buffers have been returned.
struct io_rdq_file_t
{
    #define NR       IO_MAX_RANGES
    HANDLE           File;          /// The file handle, or INVALID_FILE_HANDLE.
    void            *TargetBuffer;  /// The target buffer for the current read, or NULL.
    size_t           SectorSize;    /// The physical sector size of the disk, in bytes.
    size_t           BytesRead;     /// The total number of bytes read so far.
    uint32_t         Status;        /// Combination of io_rdq_file_status_e.
    DWORD            OSError;       /// Any error code returned by the OS.
    OVERLAPPED       AIO;           /// Overlapped I/O state for the current read.
    uint32_t         RangeCount;    /// The number of ranges defined for the file.
    uint32_t         RangeIndex;    /// The zero-based index of the range being read.
    io_range_t       RangeList[NR]; /// Range data, converted to (Begin, End) pairs.
    io_returnq_t     ReturnQueue;   /// The SRSW FIFO for returning buffers from the PC.
    size_t           ReturnCount;   /// The number of returns pending for this file.
    size_t           BytesTotal;    /// The total number of bytes to be read for the whole job.
    size_t           FileSize;      /// The total size of the file, in bytes.
    uint64_t         NanosEnq;      /// Timestamp when job was added.
    uint64_t         NanosDeq;      /// Timestamp when job was started.
    uint64_t         NanosEnd;      /// Timestamp when job finished being read.
    #undef NR
};

/// @summary Defines all of the state necessary to represent a concurrent read queue.
/// The various io_rd*q_t queues are created and managed by the job manager. The rdq 
/// will either wait/poll or publish to them as appropriate. Jobs may be either 
/// pending (<= PendingQueue), actively being read (Active*), waiting on buffers 
/// to be returned (Finish*), or completed (=> CompleteQueue). Only the queues and 
/// the Idle status event are shared; everything else is touched only from I/O poll.
struct io_rdq_t
{
    uint32_t         Flags;         /// Combination of io_rdq_flags_e.
    HANDLE           Idle;          /// Manual Reset Event, signaled when idle.
    io_rdstopq_t    *CancelQueue;   /// Where to look for job cancellations.
    io_rdpendq_t    *PendingQueue;  /// Where to look for new jobs.
    size_t           ActiveCapacity;/// Maximum number of files opened concurrently.
    size_t           ActiveCount;   /// Current number of files opened.
    uint32_t        *ActiveJobIds;  /// List of active job IDs; FileCount are valid.
    io_rdq_file_t   *ActiveJobList; /// List of active job state; FileCount are valid.
    HANDLE          *ActiveJobWait; /// List of active job wait handles; FileCount are valid.
    size_t const     MaxReadSize;   /// Maximum number of bytes to read per-op.
    io_rdbuffer_t    BufferManager; /// Management state for the I/O buffer pool.
    io_rdopq_t      *DataQueue;     /// Where to write io_rdop_t.
    io_rddoneq_t    *CompleteQueue; /// Where to write io_job_result_t.
    size_t           FinishCapacity;/// Actual size of FinishJob* arrays, in items.
    size_t           FinishCount;   /// Current number of jobs waiting on returns.
    uint32_t        *FinishJobIds;  /// List of job IDs waiting on returns; FinishCount valid.
    io_rdq_file_t   *FinishJobList; /// List of job state waiting on returns; FinishCount valid.
};

/*///////////////////////
//   Local Functions   //
///////////////////////*/
/// @summary Create a new SRSW concurrent queue with the specified capacity.
/// @param fifo The queue to initialize.
/// @param capacity The queue capacity. This must be a non-zero power-of-two.
/// @return true if the queue was created.
template <typename T>
static bool io_create_srsw_fifo(io_srsw_fifo_t<T> *fifo, uint32_t capacity)
{
    // ensure we have a valid fifo and that the capacity is a power-of-two.
    // the capacity being a non-zero power-of-two is a requirement for correct
    // functioning of the queue.
    if ((fifo != NULL) && (capacity > 0) && ((capacity & (capacity-1) == 0)))
    {
        io_srsw_flq_clear(fifo->Queue, capacity);
        fifo->Store     = (T*) malloc( capacity * sizeof(T) );
        return true;
    }
    else return false;
}

/// @summary Frees resources associated with a SRSW concurrent queue.
/// @param fifo The queue to delete.
template <typename T>
static void io_delete_srsw_fifo(io_srsw_fifo_t<T> *fifo)
{
    if (fifo != NULL)
    {
        if (fifo->Store != NULL)
        {
            free(fifo->Store);
            fifo->Store = NULL;
        }
        io_srsw_flq_clear(fifo->Queue, fifo->Queue.Capacity);
    }
}

/// @summary Flushes a SRSW concurrent queue. This operation should only be 
/// performed after coordination between the producer and the consumer; only
/// one should be accessing the queue at the time.
/// @param fifo The queue to flush.
template <typename T>
static void io_flush_srsw_fifo(io_srsw_fifo_t<T> *fifo)
{
    io_srsw_flq_clear(fifo->Queue, fifo->Queue.Capacity);
}

/// @summary Retrieves the number of items 'currently' in the queue.
/// @param fifo The queue to query.
/// @return The number of items in the queue at the instant of the call.
template <typename T>
static inline size_t io_srsw_fifo_count(io_srsw_fifo_t<T> *fifo)
{
    return io_srsw_flq_count(fifo->Queue);
}

/// @summary Determines whether the queue is 'currently' empty.
/// @param fifo The queue to query.
/// @return true if the queue contains zero items at the instant of the call.
template <typename T>
static inline bool io_srsw_fifo_is_empty(io_srsw_fifo_t<T> *fifo)
{
    return io_srsw_flq_empty(fifo->Queue);
}

/// @summary Determines whether the queue is 'currently' full.
/// @param fifo The queue to query.
/// @return true if the queue is full at the instant of the call.
template <typename T>
static inline bool io_srsw_fifo_is_full(io_srsw_fifo_t<T> *fifo)
{
    return io_srsw_flq_full(fifo->Queue);
}

/// @summary Enqueues an item.
/// @param fifo The destination queue.
/// @param item The item to enqueue. This must be a POD type.
/// @return true if the item was enqueued, or false if the queue is at capacity.
template <typename T>
bool io_srsw_fifo_put(io_srsw_fifo_t<T> *fifo, T const &item)
{
    uint32_t count = io_srsw_flq_count(fifo->Queue) + 1;
    if (count <= fifo->Queue.Capacity)
    {
        uint32_t    index  = io_srsw_flq_next_push(fifo->Queue);
        fifo->Store[index] = item;
        PROCESSOR_MFENCE_WRITE;
        io_srsw_flq_push(fifo->Queue);
        return true;
    }
    return false;
}

/// @summary Dequeues an item.
/// @param fifo The source queue.
/// @param item On return, the dequeued item is copied here.
/// @return true if an item was dequeued, or false if the queue is empty.
template <typename T>
bool io_srsw_fifo_get(io_srsw_fifo_t<T> *fifo, T &item)
{
    uint32_t count = io_srsw_flq_count(fifo->Queue);
    if (count > 0)
    {
        uint32_t index = io_srsw_flq_next_pop(fifo->Queue);
        item = fifo->Store[index];
        PROCESSOR_MFENCE_READ;
        io_srsw_flq_pop(fifo->Queue);
        return true;
    }
    return false;
}

/// @summary Create a new waitable SRSW concurrent queue with the specified capacity.
/// @param fifo The queue to initialize.
/// @param capacity The queue capacity. This must be a non-zero power-of-two.
/// @return true if the queue was created.
template <typename T>
static bool io_create_srsw_waitable_fifo(io_srsw_waitable_fifo_t<T> *fifo, uint32_t capacity)
{
    // ensure we have a valid fifo and that the capacity is a power-of-two.
    // the capacity being a non-zero power-of-two is a requirement for correct
    // functioning of the queue.
    if ((fifo != NULL) && (capacity > 0) && ((capacity & (capacity-1) == 0)))
    {
        fifo->Empty     = CreateEvent(NULL, FALSE, TRUE , NULL);
        fifo->NotEmpty  = CreateEvent(NULL, FALSE, FALSE, NULL);
        fifo->NotFull   = CreateEvent(NULL, FALSE, TRUE , NULL);
        io_srsw_flq_clear(fifo->Queue, capacity);
        fifo->Store     = (T*) malloc( capacity * sizeof(T) );
        return true;
    }
    else return false;
}

/// @summary Frees resources associated with a waitable SRSW concurrent queue.
/// @param fifo The queue to delete.
template <typename T>
static void io_delete_srsw_fifo(io_srsw_waitable_fifo_t<T> *fifo)
{
    if (fifo != NULL)
    {
        if (fifo->NotFull  != INVALID_HANDLE_VALUE)
        {
            CloseHandle(fifo->NotFull);
            fifo->NotFull   = INVALID_HANDLE_VALUE;
        }
        if (fifo->NotEmpty != INVALID_HANDLE_VALUE)
        {
            CloseHandle(fifo->NotEmpty);
            fifo->NotEmpty  = INVALID_HANDLE_VALUE;
        }
        if (fifo->Empty != INVALID_HANDLE_VALUE)
        {
            CloseHandle(fifo->Empty);
            fifo->Empty  = INVALID_HANDLE_VALUE;
        }
        if (fifo->Store != NULL)
        {
            free(fifo->Store);
            fifo->Store = NULL;
        }
        io_srsw_flq_clear(fifo->Queue, fifo->Queue.Capacity);
    }
}

/// @summary Flushes a SRSW concurrent queue. This operation should only be 
/// performed after coordination between the producer and the consumer; only
/// one should be accessing the queue at the time.
/// @param fifo The queue to flush.
template <typename T>
static void io_flush_srsw_fifo(io_srsw_waitable_fifo_t<T> *fifo)
{
    io_srsw_flq_clear(fifo->Queue, fifo->Queue.Capacity);
    SetEvent(fifo->NotFull);
    SetEvent(fifo->Empty);
}

/// @summary Retrieves the number of items 'currently' in the queue.
/// @param fifo The queue to query.
/// @return The number of items in the queue at the instant of the call.
template <typename T>
static inline size_t io_srsw_fifo_count(io_srsw_waitable_fifo_t<T> *fifo)
{
    return io_srsw_flq_count(fifo->Queue);
}

/// @summary Determines whether the queue is 'currently' empty.
/// @param fifo The queue to query.
/// @return true if the queue contains zero items at the instant of the call.
template <typename T>
static inline bool io_srsw_fifo_is_empty(io_srsw_waitable_fifo_t<T> *fifo)
{
    return io_srsw_flq_empty(fifo->Queue);
}

/// @summary Determines whether the queue is 'currently' full.
/// @param fifo The queue to query.
/// @return true if the queue is full at the instant of the call.
template <typename T>
static inline bool io_srsw_fifo_is_full(io_srsw_waitable_fifo_t<T> *fifo)
{
    return io_srsw_flq_full(fifo->Queue);
}

/// @summary Blocks the calling thread until the queue reaches an empty state,
/// or the specified timeout interval has elapsed. The caller must check the 
/// current state of the queue using io_srsw_fifo_is_empty(fifo) after being 
/// woken up, as the queue may no longer be empty.
/// @param fifo The queue to wait on.
/// @param timeout_ms The maximum number of milliseconds to wait.
/// @return true if the queue has reached an empty state, or false if the 
/// timeout interval has elapsed or an error has occurred.
template <typename T>
bool io_srsw_fifo_wait_empty(io_srsw_waitable_fifo_t<T> *fifo, uint32_t timeout_ms)
{
    DWORD   result  = WaitForSingleObjectEx(fifo->Empty, timeout_ms, TRUE);
    return (result == WAIT_OBJECT_0);
}

/// @summary Blocks the calling thread until the queue reaches a non-empty 
/// state, or the specified timeout interval has elapsed. The caller must check
/// the current state of the queue using io_srsw_fifo_count(fifo) after being 
/// woken up, as the queue may no longer be non-empty.
/// @param fifo The queue to wait on.
/// @param timeout_ms The maximum number of milliseconds to wait.
/// @return true if the queue has reached a non-empty state, or false if the 
/// timeout interval has elapsed or an error has occurred.
template <typename T>
bool io_srsw_fifo_wait_not_empty(io_srsw_waitable_fifo_t<T> *fifo, uint32_t timeout_ms)
{
    DWORD   result  = WaitForSingleObjectEx(fifo->NotEmpty, timeout_ms, TRUE);
    return (result == WAIT_OBJECT_0);
}

/// @summary Blocks the calling thread until the queue reaches a non-full 
/// state, or the specified timeout interval has elapsed. The caller must check
/// the current state of the queue using io_srsw_fifo_is_full(fifo) after being 
/// woken up, as the queue may no longer be non-full.
/// @param fifo The queue to wait on.
/// @param timeout_ms The maximum number of milliseconds to wait.
/// @return true if the queue has reached a non-full state, or false if the 
/// timeout interval has elapsed or an error has occurred.
template <typename T>
bool io_srsw_fifo_wait_not_full(io_srsw_waitable_fifo_t<T> *fifo, uint32_t timeout_ms)
{
    DWORD   result  = WaitForSingleObjectEx(fifo->NotFull, timeout_ms, TRUE);
    return (result == WAIT_OBJECT_0);
}

/// @summary Enqueues an item, if the queue is not full, and signals the 
/// 'not-empty' and possibly the 'not-full' events.
/// @param fifo The destination queue.
/// @param item The item to enqueue. This must be a POD type.
/// @return true if the item was enqueued, or false if the queue is at capacity.
template <typename T>
bool io_srsw_fifo_put(io_srsw_waitable_fifo_t<T> *fifo, T const &item)
{
    uint32_t count = io_srsw_flq_count(fifo->Queue) + 1;
    if (count <= fifo->Queue.Capacity)
    {
        uint32_t    index  = io_srsw_flq_next_push(fifo->Queue);
        fifo->Store[index] = item;
        PROCESSOR_MFENCE_WRITE;
        io_srsw_flq_push(fifo->Queue);
        SetEvent(fifo->NotEmpty);
        if (count < fifo->Queue.Capacity)
            SetEvent(fifo->NotFull);
        return true;
    }
    return false;
}

/// @summary Dequeues an item, if the queue is not empty, and signals the 
/// 'not-full' and either the 'empty' or 'not-empty' event.
/// @param fifo The source queue.
/// @param item On return, the dequeued item is copied here.
/// @return true if an item was dequeued, or false if the queue is empty.
template <typename T>
bool io_srsw_fifo_get(io_srsw_waitable_fifo_t<T> *fifo, T &item)
{
    uint32_t count = io_srsw_flq_count(fifo->Queue);
    if (count > 0)
    {
        uint32_t index = io_srsw_flq_next_pop(fifo->Queue);
        item = fifo->Store[index];
        PROCESSOR_MFENCE_READ;
        io_srsw_flq_pop(fifo->Queue);
        SetEvent(fifo->NotFull);
        if (count == 1) SetEvent(fifo->Empty);
        else SetEvent(fifo->NotEmpty);
        return true;
    }
    return false;
}

/// @summary Find the end of a volume and directory information portion of a path.
/// @param path The path string to search.
/// @param out_pathlen On return, indicates the number of bytes in the volume and
/// directory information of the path string. If the input path has no volume or
/// directory information, this value will be set to zero.
/// @param out_strlen On return, indicates the number of bytes in the input path,
/// not including the trailing zero byte.
/// @return A pointer to one past the last volume or directory separator, if present;
/// otherwise, the input pointer path.
static char const* pathend(char const *path, size_t &out_pathlen, size_t &out_strlen)
{
    if (path == NULL)
    {
        out_pathlen = 0;
        out_strlen  = 0;
        return path;
    }

    char        ch   = 0;
    char const *last = path;
    char const *iter = path;
    while ((ch = *iter++) != 0)
    {
        if (ch == ':' || ch == '\\' || ch == '/')
            last = iter;
    }
    out_strlen  = size_t(iter - path - 1);
    out_pathlen = size_t(last - path);
    return last;
}

/// @summary Find the extension part of a filename or path string.
/// @param path The path string to search; ideally just the filename portion.
/// @param out_extlen On return, indicates the number of bytes of extension information.
/// @return A pointer to the first character of the extension. Check the value of
/// out_extlen to be sure that there is extension information.
static char const* extpart(char const *path, size_t &out_extlen)
{
    if (path == NULL)
    {
        out_extlen = 0;
        return path;
    }

    char        ch    = 0;
    char const *last  = path;
    char const *iter  = path;
    while ((ch = *iter++) != 0)
    {
        if (ch == '.')
            last = iter;
    }
    if (last != path)
    {   // we found an extension separator somewhere in the input path.
        // @note: this also filters out the case of ex. path = '.gitignore'.
        out_extlen = size_t(iter - last - 1);
    }
    else
    {
        // no extension part is present in the input path.
        out_extlen = 0;
    }
    return last;
}

/// @summary Perform string matching with support for wildcards. The match is not case-sensitive.
/// @param str The string to check.
/// @param filter The filter string, which may contain wildcards '?' and '*'.
/// The '?' character matches any string except an empty string, while '*'
/// matches any string including the empty string.
/// @return true if str matches the filter pattern.
static bool match(char const *str, char const *filter)
{   // http://www.codeproject.com/Articles/188256/A-Simple-Wildcard-Matching-Function
    char ch = 0;
    while ((ch = *filter) != 0)
    {
        if (ch == '?')
        {
            if (*str == 0) return false;
            ++filter;
            ++str;
        }
        else if (ch == '*')
        {
            if (match(str, filter + 1))
                return true;
            if (*str && match(str + 1, filter))
                return true;
            return false;
        }
        else
        {   // standard comparison of two characters, ignoring case.
            if (toupper(*str++) != toupper(*filter++))
                return false;
        }
    }
    return (*str == 0 && *filter == 0);
}

/// @summary Retrieve the physical sector size for a block-access device.
/// @param file The handle to an open file on the device.
/// @return The size of a physical sector on the specified device.
static size_t physical_sector_size(HANDLE file)
{   // http://msdn.microsoft.com/en-us/library/ff800831(v=vs.85).aspx
    // for structure STORAGE_ACCESS_ALIGNMENT
    // Vista and Server 2008+ only - XP not supported.
    size_t const DefaultPhysicalSectorSize    = 4096;
    STORAGE_ACCESS_ALIGNMENT_DESCRIPTOR desc;
    STORAGE_PROPERTY_QUERY              query;
    memset(&desc , 0, sizeof(desc));
    memset(&query, 0, sizeof(query));

    query.QueryType  = PropertyStandardQuery;
    query.PropertyId = StorageAccessAlignmentProperty;
    DWORD bytes = 0;
    BOOL result = DeviceIoControl(
        file, 
        IOCTL_STORAGE_QUERY_PROPERTY, 
        &query, sizeof(query), 
        &desc , sizeof(desc), 
        &bytes, NULL);
    if (!result)
    {
        return DefaultPhysicalSectorSize;
    }
    else return desc.BytesPerPhysicalSector;
}

/// @summary Rounds a size up to the nearest even multiple of a given power-of-two.
/// @param size The size value to round up.
/// @param pow2 The power-of-two alignment.
/// @return The input size, rounded up to the nearest even multiple of pow2.
static inline size_t align_up(size_t size, size_t pow2)
{
    assert((pow2 & (pow2-1)) == 0);
    return (size == 0) ? pow2 : ((size + (pow2-1)) & ~(pow2-1));
}

/// @summary Clamps a value to a given maximum.
/// @param size The size value to clamp.
/// @param limit The upper-bound to clamp to.
/// @return The smaller of size and limit.
static inline size_t clamp_to(size_t size, size_t limit)
{
    return (size > limit) ? limit : size;
}

/// @summary Redirect a call to SetProcessWorkingSetSizeEx to SetProcessWorkingSetSize
/// on systems that don't support the Ex version.
/// @param process A handle to the process, or GetCurrentProcess() pseudo-handle.
/// @param minimum The minimum working set size, in bytes.
/// @param maximum The maximum working set size, in bytes.
/// @param flags Ignored. See MSDN for SetProcessWorkingSetSizeEx.
static BOOL WINAPI SetProcessWorkingSetSizeEx_Fallback(HANDLE process, SIZE_T minimum, SIZE_T maximum, DWORD /*flags*/)
{
    return SetProcessWorkingSetSize(process, minimum, maximum);
}

/// @summary Performs any one-time initialization for the Windows platforms.
/// We call QueryPerformanceFrequency() to retrieve the scale factor used to
/// convert a timestamp to seconds. The high-resolution timer is supported on
/// all systems Windows XP and later.
/// @return true if high-resolution timing is supported on this system.
static bool io_time_init(void)
{
    static bool _InitHighResTimer_ = true;
    static bool _InitTimerResult_  = false;
    if (_InitHighResTimer_)
    {
        if (QueryPerformanceFrequency(&g_Frequency))
        {
            _InitTimerResult_  = true;
            _InitHighResTimer_ = false;
        }
        else
        {
            _InitTimerResult_  = false;
            _InitHighResTimer_ = false;
        }
    }
    return _InitTimerResult_;
}

/// @summary Retrieve the current timestamp.
/// @return The current time value, in nanoseconds.
static inline uint64_t io_timestamp(void)
{
    LARGE_INTEGER tsc = {0};
    LARGE_INTEGER tsf = g_Frequency;
    QueryPerformanceCounter(&tsc);
    return (SEC_TO_NANOSEC * uint64_t(tsc.QuadPart) / uint64_t(tsf.QuadPart));
}

/// @summary Allocates buffer space for I/O read buffers associated with a 
/// concurrent read queue. The entire requested amount of buffer space is 
/// allocated and pinned in memory. This function is *NOT* re-entrant.
/// @param rdbuf The buffer manager to initialize.
/// @param total_size The minimum number of bytes of buffer space to allocate. 
/// The actual amount allocated will be a multiple of the rounded alloc_size.
/// @param alloc_size The number of bytes allocated to a single I/O buffer. This
/// value will be rounded up to the nearest even multiple of the system page size.
/// @return true if the buffer space was successfully allocated.
static bool io_create_rdbuffer(io_rdbuffer_t *rdbuf, size_t total_size, size_t alloc_size)
{
    static GetNativeSystemInfoFn        GetNativeSystemInfo_Func        = NULL;
    static SetProcessWorkingSetSizeExFn SetProcessWorkingSetSizeEx_Func = NULL;
    static bool _ResolveVistaAPIs_ = true;

    if (_ResolveVistaAPIs_)
    {
        HMODULE kernel = GetModuleHandle("kernel32.dll");
        if (kernel != NULL)
        {
            GetNativeSystemInfo_Func        = (GetNativeSystemInfoFn)        GetProcAddress(kernel, "GetNativeSystemInfo");
            SetProcessWorkingSetSizeEx_Func = (SetProcessWorkingSetSizeExFn) GetProcAddress(kernel, "SetProcessWorkingSetSizeEx");
        }
        // fallback if either of these APIs are not available.
        if (GetNativeSystemInfo_Func        == NULL) GetNativeSystemInfo_Func        = GetSystemInfo;
        if (SetProcessWorkingSetSizeEx_Func == NULL) SetProcessWorkingSetSizeEx_Func = SetProcessWorkingSetSizeEx_Fallback;
        _ResolveVistaAPIs_ = false;
    }

    SYSTEM_INFO sysinfo = {0};
    GetNativeSystemInfo_Func(&sysinfo);
    
    // round the allocation size up to an even multiple of the page size.
    // round the total size up to an even multiple of the allocation size.
    size_t page_size = sysinfo.dwPageSize;
    alloc_size       = align_up(alloc_size, page_size);
    total_size       = align_up(total_size, alloc_size);
    size_t nallocs   = total_size / alloc_size;

    // in order to lock the entire allocated region in physical memory, we
    // might need to increase the size of the process' working set. this is
    // Vista and Windows Server 2003+ only, and requires that the process 
    // be running as (at least) a Power User or Administrator.
    HANDLE process   = GetCurrentProcess();
    SIZE_T min_wss   = 0;
    SIZE_T max_wss   = 0;
    DWORD  wss_flags = QUOTA_LIMITS_HARDWS_MIN_ENABLE | QUOTA_LIMITS_HARDWS_MAX_DISABLE;
    GetProcessWorkingSetSize(process, &min_wss, &max_wss);
    min_wss += total_size;
    max_wss += total_size;
    if (!SetProcessWorkingSetSizeEx_Func(process, min_wss, max_wss, wss_flags))
    {   // the minimum working set size could not be set.
        return false;
    }

    // reserve and commit the entire region, and then pin it in physical memory.
    // this prevents the buffers from being paged out during normal execution.
    void  *baseaddr  = VirtualAlloc(NULL, total_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (baseaddr == NULL)
    {   // the requested amount of memory could not be allocated.
        return false;
    }
    if (!VirtualLock(baseaddr, total_size))
    {   // the pages could not be pinned in physical memory.
        VirtualFree(baseaddr, 0, MEM_RELEASE);
        return false;
    }

    void **freelist  = (void**) malloc(nallocs * sizeof(void*));
    if (freelist == NULL)
    {   // the requested memory could not be allocated.
        VirtualUnlock(baseaddr , total_size);
        VirtualFree(baseaddr, 0, MEM_RELEASE);
        return false;
    }

    // at this point, everything that could have failed has succeeded.
    // set the fields of rdbuf and initialize the free list.
    rdbuf->TotalSize   = total_size;
    rdbuf->PageSize    = page_size;
    rdbuf->AllocSize   = alloc_size;
    rdbuf->BaseAddress = baseaddr;
    rdbuf->FreeCount   = nallocs;
    rdbuf->FreeList    = freelist;
    uint8_t *buf_it    = (uint8_t*) baseaddr;
    for (size_t i = 0; i < nallocs; ++i)
    {
        freelist[i] = buf_it;
        buf_it     += alloc_size;
    }
    return true;
}

/// @summary Frees resources associated with I/O buffers for a concurrent read queue.
/// @param The I/O buffer manager to delete.
static void io_delete_rdbuffer(io_rdbuffer_t *rdbuf)
{
    if (rdbuf->FreeList    != NULL) free(rdbuf->FreeList);
    if (rdbuf->BaseAddress != NULL)
    {
        VirtualUnlock(rdbuf->BaseAddress , rdbuf->TotalSize);
        VirtualFree(rdbuf->BaseAddress, 0, MEM_RELEASE);
    }
    rdbuf->BaseAddress = NULL;
    rdbuf->FreeCount   = 0;
    rdbuf->FreeList    = NULL;
}

/// @summary Returns all allocated I/O buffers to the free list.
/// @param The I/O buffer manager to flush.
static void io_flush_rdbuffer(io_rdbuffer_t *rdbuf)
{
    size_t const nallocs = rdbuf->TotalSize / rdbuf->AllocSize;
    size_t const allocsz = rdbuf->AllocSize;
    uint8_t       *bufit = (uint8_t*) rdbuf->BaseAddress;
    void         **freel = rdbuf->FreeList;
    for (size_t i = 0; i < nallocs; ++i)
    {
        freel[i]  = bufit;
        bufit    += allocsz;
    }
    rdbuf->FreeCount = nallocs;
}

/// @summary Allocates a logical I/O buffer.
/// @param rdbuf The I/O buffer manager to allocate from.
/// @return A pointer to the buffer, or NULL.
static void* io_rdbuffer_get(io_rdbuffer_t *rdbuf)
{
    if (rdbuf->FreeCount > 0) return rdbuf->FreeList[--rdbuf->FreeCount];
    else return NULL;
}

/// @summary Returns a logical I/O buffer to the free list.
/// @param rdbuf The I/O buffer manager that owns addr.
/// @param addr The address returned by a previous call to io_rdbuffer_get().
static void io_rdbuffer_put(io_rdbuffer_t *rdbuf, void *addr)
{
    rdbuf->FreeList[rdbuf->FreeCount++] = addr;
}

/// @summary Prepares an active job, opening the file, setting timestamps, etc.
/// @param rdq The read queue managing the job.
/// @param rdf The job state data to modify.
/// @param job The job description.
static bool io_prepare_job(io_rdq_t *rdq, io_rdq_file_t *rdf, io_rdq_job_t const &job)
{
    HANDLE   file  = INVALID_HANDLE_VALUE;
    DWORD    flags = FILE_FLAG_SEQUENTIAL_SCAN;
    size_t   ssize = 0;
    uint64_t fsize = 0;

    if (rdq->Flags & IO_RDQ_FLAGS_UNBUFFERED)
    {   // use unbuffered (direct) I/O and bypass the cache manager.
        flags &= ~FILE_FLAG_SEQUENTIAL_SCAN;
        flags |=  FILE_FLAG_NO_BUFFERING;
    }
    if (rdq->Flags & IO_RDQ_FLAGS_ASYNC)
    {   // use overlapped I/O. some operations may still complete 
        // synchronously. overlapped I/O may be buffered or unbuffered.
        flags |=  FILE_FLAG_OVERLAPPED;
    }

    file = CreateFileA(job.Path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, flags, NULL);
    if (file == INVALID_HANDLE_VALUE)
    {   // the file could not be opened, so fail.
        return false;
    }

    // get the physical sector size of the disk on which the file
    // resides, and also determine the current size of the file.
    LARGE_INTEGER distance = {0};
    LARGE_INTEGER filepos  = {0};
    SetFilePointerEx(file, distance, &filepos, FILE_END);
    SetFilePointerEx(file, distance,  NULL   , FILE_BEGIN);
    ssize = physical_sector_size(file);
    fsize = (uint64_t) filepos.QuadPart;

    // the base address must be aligned to the physical sector size,
    // and the buffer allocation size must be an even multiple of 
    // the physical sector size - these are requirements for direct I/O.
    // we use VirtualAlloc to allocate the I/O buffers, so these 
    // requirements should be met - this is more of a sanity check.
    uintptr_t baseaddr = (uintptr_t) rdq->BufferManager.BaseAddress;
    size_t    allocsz  = (size_t   ) rdq->BufferManager.AllocSize;
    if ((baseaddr & (ssize - 1)) != 0 || (allocsz & (ssize - 1)) != 0)
    {   // if we are using direct I/O, these requirements MUST be met.
        if((flags & FILE_FLAG_NO_BUFFERING) != 0 && (ssize > 0))
        {   // ...but they are not, so fail.
            CloseHandle(file);
            return false;
        }
    }

    // everything looks good, so initialize the state.
    rdf->File             = file;
    rdf->TargetBuffer     = NULL;
    rdf->SectorSize       = ssize;
    rdf->BytesRead        = 0;
    rdf->Status           = IO_RDQ_FILE_NONE;
    rdf->OSError          = ERROR_SUCCESS;
    rdf->AIO.Internal     = 0;
    rdf->AIO.InternalHigh = 0;
    rdf->AIO.Offset       = 0;
    rdf->AIO.OffsetHigh   = 0;
    rdf->RangeCount       = job.RangeCount;
    rdf->RangeIndex       = 0;
    rdf->ReturnCount      = 0;
    rdf->BytesTotal       = 0;
    rdf->FileSize         = fsize;
    rdf->NanosEnq         = job.EnqueueTime;
    rdf->NanosDeq         = io_timestamp();
    rdf->NanosEnd         = 0;
    io_create_srsw_fifo(&rdf->ReturnQueue, IO_RDQ_MAX_RETURNS);
    for (size_t i = 0;  i < job.RangeCount; ++i)
    {   // convert ranges to (Begin, End) and calculate total number of bytes to read.
        rdf->RangeList[i].Offset = job.RangeList[i].Offset;
        rdf->RangeList[i].Amount = job.RangeList[i].Offset + job.RangeList[i].Amount;
        rdf->BytesTotal         += job.RangeList[i].Offset + job.RangeList[i].Amount;
    }
    if  (job.RangeCount == 0)
    {   // the job consists of the entire file.
        rdf->RangeList[0].Offset = 0;
        rdf->RangeList[0].Amount = fsize;
        rdf->BytesTotal          = fsize;
    }
    return true;
}

/// @summary Calculates statistics and posts a job result to the completion queue 
/// for a job that was not cancelled. This function should be called only when 
/// the job is not waiting for any more buffer returns (it is truly complete.)
/// The caller must perform maintenence on any internal lists (Active*, Finish*).
/// @param rdq The read queue managing the job.
/// @param rdf The job state. The Status, BytesRead, BytesTotal, ReturnCount, 
/// FileSize, OSError, NanosEnq, NanosDeq, and NanosEnd fields must be valid. 
/// The file handle will be closed if it is currently open.
/// @param id The application identifier for the job.
static void io_complete_job(io_rdq_t *rdq, io_rdq_file_t *rdf, uint32_t id)
{
    // expectation -- rdf->ReturnCount should be zero.
    if (rdf->File  != INVALID_HANDLE_VALUE)
    {
        CloseHandle(rdf->File);
        rdf->File   = INVALID_HANDLE_VALUE;
    }

    // get the timestamp of job competion (nnanos) and determine job status.
    uint64_t nnanos = io_timestamp();
    uint64_t enanos = nnanos;
    uint32_t status = IO_STATUS_UNKNOWN;
    if (rdf->Status & IO_RDQ_FILE_FINISHED)
    {   // all ranges were read successfully.
        status |= IO_STATUS_SUCCESS;
        enanos  = rdf->NanosEnd;
    }
    if (rdf->Status & IO_RDQ_FILE_ERROR)
    {   // an error was encountered. rdf->OSError is valid.
        status |= IO_STATUS_ERROR;
    }
    if (rdf->Status & IO_RDQ_FILE_CANCELLED)
    {   // the job was intentionally cancelled.
        status |= IO_STATUS_CANCELLED;
    }
    if (rdf->Status & IO_RDQ_FILE_EOF)
    {   // the entire contents of the file was read.
        status |= IO_STATUS_EOF;
    }

    // populate the job result and post it to the completion queue.
    io_rdq_result_t res;
    res.JobId       = id;
    res.Status      = status;
    res.OSError     = rdf->OSError;
    res.Reserved1   = 0;
    res.BytesTotal  = rdf->FileSize;
    res.BytesJob    = rdf->BytesTotal;
    res.BytesRead   = rdf->BytesRead;
    res.NanosWait   = rdf->NanosDeq - rdf->NanosEnq;
    res.NanosRead   = enanos - rdf->NanosDeq;
    res.NanosTotal  = enanos - rdf->NanosEnq;
    io_srsw_fifo_put(rdq->CompleteQueue, res);
}

// io_poll_rdq(...)
// 1. process all items in the cancellation queue
// 2. while rdq->ActiveCount < rdq->ActiveCapacity, pop from pending queue
// 3. poll state of all pending I/Os, schedule next I/Os, etc.
// 4. move jobs from active->finish
// 5. move jobs from finish->complete

/*////////////////////////
//   Public Functions   //
////////////////////////*/
bool LLCALL_C io_enumerate_files(io_file_list_t *dest, char const *path, char const *filter, bool recurse)
{
    size_t  dir_len    = strlen(path);
    char   *pathbuf    = (char*) malloc(dir_len + 1 + MAX_PATH + 1); // <path>\<result>0
    char   *filterbuf  = (char*) malloc(dir_len + 3);                // <path>\*0

    // generate a filter string of the form <path>\* so that we enumerate
    // all files and directories under the specified path, even if 'path'
    // doesn't directly contain any files matching the extension filter.
    strncpy(filterbuf, path, dir_len);
    if (path[dir_len - 1] != '\\' && path[dir_len - 1] != '/')
    {
        filterbuf[dir_len + 0] = '\\';
        filterbuf[dir_len + 1] = '*';
        filterbuf[dir_len + 2] = '\0';
    }
    else
    {
        filterbuf[dir_len + 0] = '*';
        filterbuf[dir_len + 1] = '\0';
    }

    // open the find for all files and directories directly under 'path'.
    FINDEX_INFO_LEVELS  l = FINDEX_INFO_LEVELS (1); // FindExInfoBasic
    FINDEX_SEARCH_OPS   s = FINDEX_SEARCH_OPS  (0); // FindExSearchNameMatch
    WIN32_FIND_DATAA info = {0};
    HANDLE       find_obj = FindFirstFileExA(filterbuf, l, &info, s, NULL, 0);
    if (find_obj != INVALID_HANDLE_VALUE)
    {
        strncpy(pathbuf, path, dir_len);
        if (path[dir_len - 1] != '\\' && path[dir_len - 1] != '/')
        {
            // append a trailing '\\'.
            pathbuf[dir_len++] = '\\';
        }
        do
        {
            if (0 == strcmp(info.cFileName, "."))
                continue;
            if (0 == strcmp(info.cFileName, ".."))
                continue;

            strcpy(&pathbuf[dir_len], info.cFileName);
            if (recurse && (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
            {
                // recurse into the subdirectory.
                io_enumerate_files(dest, pathbuf, filter, true);
            }

            if ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 &&
                (info.dwFileAttributes & FILE_ATTRIBUTE_DEVICE   ) == 0 &&
                (info.dwFileAttributes & FILE_ATTRIBUTE_TEMPORARY) == 0 &&
                (info.dwFileAttributes & FILE_ATTRIBUTE_VIRTUAL  ) == 0)
            {
                // this is a regular file. if it passes the filter, add it.
                if (match(info.cFileName, filter))
                    io_append_file_list(dest, pathbuf);
            }
        } while (FindNextFileA(find_obj, &info));
        FindClose(find_obj);
        free(filterbuf);
        free(pathbuf);
        return true;
    }
    free(filterbuf);
    free(pathbuf);
    return false;
}

