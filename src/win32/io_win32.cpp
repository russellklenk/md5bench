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
// The following functions are not available under MinGW, so kernel32.dll is  
// loaded and these functions will be resolved manually in io_create_rdq().
typedef void (WINAPI *GetNativeSystemInfoFn)(SYSTEM_INFO*);
typedef BOOL (WINAPI *SetProcessWorkingSetSizeExFn)(HANDLE, SIZE_T, SIZE_T, DWORD);
typedef BOOL (WINAPI *CancelIoExFn)(HANDLE, OVERLAPPED*);
typedef BOOL (WINAPI *GetOverlappedResultExFn)(HANDLE, OVERLAPPED*, DWORD*, DWORD, BOOL);

static CancelIoExFn                 CancelIoEx_Func                 = NULL;
static GetNativeSystemInfoFn        GetNativeSystemInfo_Func        = NULL;
static GetOverlappedResultExFn      GetOverlappedResultEx_Func      = NULL;
static SetProcessWorkingSetSizeExFn SetProcessWorkingSetSizeEx_Func = NULL;
static bool                         _ResolveKernelAPIs_             = true;
static LARGE_INTEGER                _Frequency_                     = {0};

/*//////////////////
//   Data Types   //
//////////////////*/
/// @summary MinGW doesn't define a bunch of these constants and structures.
#ifdef __GNUC__
#define METHOD_BUFFERED                    0
#define FILE_ANY_ACCESS                    0
#define FILE_DEVICE_MASS_STORAGE           0x0000002d

#ifndef ERROR_OFFSET_ALIGNMENT_VIOLATION
#define ERROR_OFFSET_ALIGNMENT_VIOLATION   0x00000147
#endif

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
#endif /* __GNUC__ */

/// @summary Bitflags that can be set on a read queue.
enum io_rdq_flags_e
{
    IO_RDQ_FLAGS_NONE               = (0 << 0), /// Prefer synchronous, buffered I/O.
    IO_RDQ_FLAGS_ASYNC              = (1 << 0), /// Prefer asynchronous I/O.
    IO_RDQ_FLAGS_UNBUFFERED         = (1 << 1), /// Prefer unbuffered I/O.
    IO_RDQ_FLAGS_CANCEL_ALL         = (1 << 2), /// Cancel all active and pending jobs.
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
    uint64_t         BytesRead;     /// The total number of bytes read so far.
    uint32_t         Status;        /// Combination of io_rdq_file_status_e.
    DWORD            OSError;       /// Any error code returned by the OS.
    uint32_t         RangeCount;    /// The number of ranges defined for the file.
    uint32_t         RangeIndex;    /// The zero-based index of the range being read.
    io_range_t       RangeList[NR]; /// Range data, converted to (Begin, End) pairs.
    io_returnq_t    *ReturnQueue;   /// The SRSW FIFO for returning buffers from the PC.
    size_t           ReturnCount;   /// The number of returns pending for this file.
    uint64_t         BytesTotal;    /// The total number of bytes to be read for the whole job.
    uint64_t         FileSize;      /// The total size of the file, in bytes.
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
    size_t           OverflowCount; /// The number of valid items in DataOverflow.
    io_rdop_t       *DataOverflow;  /// ActiveCount io_rdop_t that couldn't be delivered.
    io_rdstopq_t    *CancelQueue;   /// Where to look for job cancellations.
    io_rdpendq_t    *PendingQueue;  /// Where to look for new jobs.
    size_t           ActiveCapacity;/// Maximum number of files opened concurrently.
    size_t           ActiveCount;   /// Current number of files opened.
    uint32_t        *ActiveJobIds;  /// List of active job IDs; FileCount are valid.
    io_rdq_file_t   *ActiveJobList; /// List of active job state; FileCount are valid.
    OVERLAPPED      *ActiveJobAIO;  /// List of active job async I/O state; FileCount are valid.
    HANDLE          *ActiveJobWait; /// List of active job wait handles; FileCount are valid.
    size_t           MaxReadSize;   /// Maximum number of bytes to read per-op.
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
    if ((fifo != NULL) && (capacity > 0) && ((capacity & (capacity-1)) == 0))
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
    if ((fifo != NULL) && (capacity > 0) && ((capacity & (capacity-1)) == 0))
    {
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
        if (count > 1) SetEvent(fifo->NotEmpty);
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

/// @summary Calculate a power-of-two value greater than or equal to a given value.
/// @param The input value, which may or may not be a power of two.
/// @param min The minimum power-of-two value, which must also be non-zero.
/// @return A power-of-two that is greater than or equal to value.
static inline uint32_t pow2_ge(uint32_t value, uint32_t min)
{
    assert((min > 0));
    assert((min & (min - 1)) == 0);
    uint32_t x = min;
    while (x < value)
        x  <<= 1;
    return x;
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
        if (QueryPerformanceFrequency(&_Frequency_))
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
    LARGE_INTEGER tsf = _Frequency_;
    QueryPerformanceCounter(&tsc);
    return (SEC_TO_NANOSEC * uint64_t(tsc.QuadPart) / uint64_t(tsf.QuadPart));
}

/// @summary Redirect a call to CancelIoEx to CancelIo on systems that don't support the Ex version.
/// @param file The handle of the file whose I/O is being cancelled.
/// @param aio Ignored. See MSDN for CancelIoEx.
/// @return See MSDN for CancelIo.
static BOOL WINAPI CancelIoEx_Fallback(HANDLE file, OVERLAPPED* /*aio*/)
{
    return CancelIo(file);
}

/// @summary Redirect a call to GetNativeSystemInfo to GetSystemInfo.
/// @param sys_info The SYSTEM_INFO structure to populate.
static void WINAPI GetNativeSystemInfo_Fallback(SYSTEM_INFO *sys_info)
{
    GetSystemInfo(sys_info);
}

/// @summary Redirect a call to GetOverlappedResultEx to GetOverlapped result 
/// on systems that don't support the Ex version. 
/// @param file The file handle associated with the I/O operation.
/// @param aio The OVERLAPPED structure representing the I/O operation.
/// @param bytes_transferred On return, this location stores the number of bytes transferred,
/// @param timeout Specify 0 to check the status and return immediately. Any non-zero value 
/// will cause the call to block until status information is available.
/// @param alertable Ignored. See MSDN for GetOverlappedResultEx.
static BOOL WINAPI GetOverlappedResultEx_Fallback(HANDLE file, OVERLAPPED *aio, DWORD *bytes_transferred, DWORD timeout, BOOL /*alertable*/)
{
    // for our use case, timeout will always be 0 (= don't wait).
    return GetOverlappedResult(file, aio, bytes_transferred, timeout ? TRUE : FALSE);
}

/// @summary Redirect a call to SetProcessWorkingSetSizeEx to SetProcessWorkingSetSize
/// on systems that don't support the Ex version.
/// @param process A handle to the process, or GetCurrentProcess() pseudo-handle.
/// @param minimum The minimum working set size, in bytes.
/// @param maximum The maximum working set size, in bytes.
/// @param flags Ignored. See MSDN for SetProcessWorkingSetSizeEx.
/// @return See MSDN for SetProcessWorkingSetSize.
static BOOL WINAPI SetProcessWorkingSetSizeEx_Fallback(HANDLE process, SIZE_T minimum, SIZE_T maximum, DWORD /*flags*/)
{
    return SetProcessWorkingSetSize(process, minimum, maximum);
}

/// @summary Loads function entry points that may not be available at compile 
/// time with some build environments.
static void resolve_kernel_apis(void)
{
    if (_ResolveKernelAPIs_)
    {   // it's a safe assumption that kernel32.dll is mapped into our process
        // address space already, and will remain mapped for the duration of execution.
        // note that some of these APIs are Vista/WS2008+ only, so make sure that we 
        // have an acceptable fallback in each case to something available earlier.
        HMODULE kernel = GetModuleHandleA("kernel32.dll");
        if (kernel != NULL)
        {
            CancelIoEx_Func                 = (CancelIoExFn)                 GetProcAddress(kernel, "CancelIoEx");
            GetNativeSystemInfo_Func        = (GetNativeSystemInfoFn)        GetProcAddress(kernel, "GetNativeSystemInfo");
            GetOverlappedResultEx_Func      = (GetOverlappedResultExFn)      GetProcAddress(kernel, "GetOverlappedResultEx");
            SetProcessWorkingSetSizeEx_Func = (SetProcessWorkingSetSizeExFn) GetProcAddress(kernel, "SetProcessWorkingSetSizeEx");
        }
        // fallback if any of these APIs are not available.
        if (CancelIoEx_Func                 == NULL) CancelIoEx_Func = CancelIoEx_Fallback;
        if (GetNativeSystemInfo_Func        == NULL) GetNativeSystemInfo_Func = GetNativeSystemInfo_Fallback;
        if (GetOverlappedResultEx_Func      == NULL) GetOverlappedResultEx_Func = GetOverlappedResultEx_Fallback;
        if (SetProcessWorkingSetSizeEx_Func == NULL) SetProcessWorkingSetSizeEx_Func = SetProcessWorkingSetSizeEx_Fallback;
        _ResolveKernelAPIs_ = false;
        io_time_init();
    }
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

/// @summary Allocates storage for a new return queue and initializes it to empty.
/// @return The return queue instance, or NULL.
static io_returnq_t* io_create_returnq(void)
{   // return queues are explicitly allocated on the heap.
    // there exists one return queue for each started, but non-completed job.
    // the same return queue is used whether the job is active or waiting to finish.
    io_returnq_t *rq = (io_returnq_t*) malloc(sizeof(io_returnq_t));
    if (rq != NULL)
    {
        if (io_create_srsw_fifo(rq, IO_RDQ_MAX_RETURNS) == false)
        {   // the necessary memory could not be allocated.
            free(rq); rq = NULL;
        }
    }
    return rq;
}

/// @summary Frees the resources associated with a return queue. Return queues
/// are deleted at the time of job completion.
/// @param rq The return queue to delete.
/// @return The function always returns NULL.
static io_returnq_t* io_delete_returnq(io_returnq_t *rq)
{
    if (rq != NULL)
    {
        io_delete_srsw_fifo(rq);
        free(rq);
    }
    return NULL;
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
    if (rdf->File  != INVALID_HANDLE_VALUE)
    {
        CloseHandle(rdf->File);
        rdf->File   = INVALID_HANDLE_VALUE;
    }
    // expectation -- rdf->ReturnCount should be zero.
    // delete the return queue; it will not be accessed anymore.
    rdf->ReturnQueue  = io_delete_returnq(rdf->ReturnQueue);

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
        enanos  = rdf->NanosEnd;
    }
    if (rdf->Status & IO_RDQ_FILE_CANCELLED)
    {   // the job was intentionally cancelled.
        status |= IO_STATUS_CANCELLED;
        enanos  = rdf->NanosEnd;
    }
    if (rdf->Status & IO_RDQ_FILE_EOF)
    {   // the entire contents of the file was read.
        status |= IO_STATUS_EOF;
        enanos  = rdf->NanosEnd;
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

/// @summary Processes any pending buffer returns for a job.
/// @param rdq The concurrent read queue that owns the job.
/// @param rdf The active or finish-status job.
static void io_process_returns(io_rdq_t *rdq, io_rdq_file_t *rdf)
{
    size_t count  = 0;
    void  *buffer = NULL;
    while (io_srsw_fifo_get(rdf->ReturnQueue, buffer))
    {
        io_rdbuffer_put(&rdq->BufferManager, buffer);
        count++;
    }
    rdf->ReturnCount -= count;
    assert(rdf->ReturnCount >= 0);
}

/// @summary Move a single active job to the finished job list. The job status 
/// is updated prior to calling this function. Typically, the file handle is 
/// also closed prior to calling this function.
/// @param rdq The read queue managing the job.
/// @param active_index The zero-based index of the job to move from the active
/// list to the finished list.
/// @return true if the job was removed from the active list and moved to finished.
static bool io_finish(io_rdq_t *rdq, size_t active_index)
{
    if (rdq->FinishCount   ==  rdq->FinishCapacity)
    {   // the capacity of the finished job list needs to be increased.
        size_t         cap  =  rdq->FinishCapacity + rdq->ActiveCapacity;
        uint32_t      *ids  = (uint32_t*)      realloc(rdq->FinishJobIds , cap * sizeof(uint32_t));
        io_rdq_file_t *jobs = (io_rdq_file_t*) realloc(rdq->FinishJobList, cap * sizeof(io_rdq_file_t));
        if (ids  != NULL) rdq->FinishJobIds  = ids;
        if (jobs != NULL) rdq->FinishJobList = jobs;
        if (ids  != NULL && jobs != NULL) rdq->FinishCapacity = cap;
        else return false;
    }

    // this job will not be needing any pending I/O buffer.
    // @todo: this should not be done for normal jobs. should it 
    // be done for cancelled jobs? investigate.
    /*if (rdq->ActiveJobList[active_index].TargetBuffer != NULL)
    {   // return the target buffer to the pool.
        io_rdbuffer_put(&rdq->BufferManager, rdq->ActiveJobList[active_index].TargetBuffer);
        rdq->ActiveJobList[active_index].TargetBuffer  = NULL;
    }*/

    // copy the job from the active list to the finished list.
    size_t             finish_index  = rdq->FinishCount;
    rdq->FinishJobIds [finish_index] = rdq->ActiveJobIds [active_index];
    rdq->FinishJobList[finish_index] = rdq->ActiveJobList[active_index];
    rdq->FinishCount++;

    // remove the job from the active list, swapping the last active job 
    // into its place (the active_index slot.) note that we can be sure 
    // that since this is called from io_rdq_poll(), we are *NOT* actively
    // waiting on anything in rdq->ActiveJobWait, so it's safe to modify.
    size_t last_active  = rdq->ActiveCount - 1;
    if    (last_active != active_index)
    {
        rdq->ActiveJobIds [active_index] = rdq->ActiveJobIds [last_active];
        rdq->ActiveJobList[active_index] = rdq->ActiveJobList[last_active];
        rdq->ActiveJobAIO [active_index] = rdq->ActiveJobAIO [last_active];
        rdq->ActiveJobWait[active_index] = rdq->ActiveJobWait[last_active];
    }
    // @note: io_cull_finished_jobs(), which calls this function, decrements
    // rdq->ActiveCount if this function returns true, but not if it returns false.
    return true;
}

/// @summary Go through the list of active jobs, looking for any with a 
/// closed file handle, and move them to the finished list.
/// @param rdq The read queue to update.
static void io_cull_active_jobs(io_rdq_t *rdq)
{
    for (size_t i = 0; i < rdq->ActiveCount; /* empty */)
    {   // process any pending buffer returns.
        io_process_returns(rdq, &rdq->ActiveJobList[i]);
        if (rdq->ActiveJobList[i].File == INVALID_HANDLE_VALUE)
        {
            if (io_finish(rdq, i))
            {   // this job has been moved to the finished list.
                // it will be further inspected during io_finish_jobs().
                rdq->ActiveCount--;
            }
            else ++i; // memory allocation failed. hold the slot for now.
        }
        else ++i; // this job is still active.
    }
}

/// @summary Go through the list of finished jobs, looking for any with a 
/// pending return count of zero. Complete those jobs, and then remove them
/// from the set of finished jobs.
/// @param rdq The read queue to update.
static void io_finish_jobs(io_rdq_t *rdq)
{
    for (size_t i = 0; i < rdq->FinishCount; /* empty */)
    {   // process any pending buffer returns.
        io_process_returns(rdq, &rdq->FinishJobList[i]);
        if (rdq->FinishJobList[i].ReturnCount == 0)
        {   // move the job to the completion queue, and then swap the job at
            // the end of the finish list into the place of the finished job.
            io_complete_job(rdq, &rdq->FinishJobList[i], rdq->FinishJobIds[i]);

            if (i + 1 != rdq->FinishCount)
            {
                size_t last = rdq->FinishCount - 1;
                rdq->FinishJobIds [i] = rdq->FinishJobIds [last];
                rdq->FinishJobList[i] = rdq->FinishJobList[last];
            }
            rdq->FinishCount--;  // the FinishCount is decremented, but i 
            // is not incremented, since we need to check the job we just
            // swapped into slot i.
        }
        else ++i;  // this job is still waiting on buffer returns.
    }
    size_t nactive  = rdq->ActiveCount + rdq->FinishCount;
    size_t npending = io_srsw_fifo_count(rdq->PendingQueue);
    if (nactive + npending == 0)
        SetEvent(rdq->Idle);
}

/// @summary Processes all pending cancellations against a read queue.
/// @param rdq The read queue being updated.
static void io_process_cancellations(io_rdq_t *rdq)
{
    bool did_signal_idle  = false;
    if (rdq->Flags & IO_RDQ_FLAGS_CANCEL_ALL)
    {
        // the only potential change to flags that could be made outside of 
        // the io_rdq_poll() call is to set the cancel all status, which 
        // would not happen, since we're processing that now. clear the status.
        uintptr_t flags_addr  = (uintptr_t)&rdq->Flags;
        uint32_t  new_flags   = rdq->Flags & ~IO_RDQ_FLAGS_CANCEL_ALL;
        io_atomic_write_uint32_aligned(flags_addr, new_flags);
        
        // move jobs from pending to completed.
        size_t const npending = io_srsw_fifo_count(rdq->PendingQueue);
        for (size_t  i = 0; i < npending; ++i)
        {
            io_rdq_job_t job;
            if (io_srsw_fifo_get(rdq->PendingQueue, job))
            {   // immediately complete this job.
                io_rdq_file_t rdf;
                rdf.File         = INVALID_HANDLE_VALUE;
                rdf.TargetBuffer = NULL;
                rdf.SectorSize   = 0;
                rdf.BytesRead    = 0;
                rdf.Status       = IO_RDQ_FILE_CANCELLED;
                rdf.OSError      = ERROR_SUCCESS;
                rdf.RangeCount   = (uint32_t) job.RangeCount;
                rdf.RangeIndex   = 0;
                rdf.ReturnQueue  = NULL;
                rdf.ReturnCount  = 0;
                rdf.BytesTotal   = 0;
                rdf.FileSize     = 0;
                rdf.NanosEnq     = job.EnqueueTime;
                rdf.NanosDeq     = io_timestamp();
                rdf.NanosEnd     = rdf.NanosDeq;
                for (size_t i = 0;  i < job.RangeCount; ++i)
                {   // do calculate an accurate total job bytes count.
                    rdf.BytesTotal   += job.RangeList[i].Amount;
                }   // if there are no jobs, the BytesTotal remains 0.
                io_complete_job(rdq, &rdf, job.JobId);
            }
        }

        // cancel all active jobs. these must go through the transition 
        // from active->finish and finish->completed. jobs with pending 
        // buffer returns will remain in the finish list.
        size_t const nactive = rdq->ActiveCount;
        for (size_t i = 0; i < nactive; ++i)
        {
            io_rdq_file_t  &rdf = rdq->ActiveJobList[i];
            if (rdf.Status & IO_RDQ_FILE_PENDING_AIO)
            {   // cancel pending AIO and block until it's done.
                DWORD nbt = 0;
                CancelIoEx_Func(rdf.File, &rdq->ActiveJobAIO[i]);
                GetOverlappedResult(rdf.File, &rdq->ActiveJobAIO[i], &nbt, TRUE);
            }
            CloseHandle(rdf.File);
            rdf.File      = INVALID_HANDLE_VALUE;
            rdf.Status   |= IO_RDQ_FILE_CANCELLED;
            rdf.NanosEnd  = io_timestamp();
        }
        io_cull_active_jobs(rdq);
        io_finish_jobs(rdq);

        // flush all pending overflows and return their associated buffers.
        size_t const noverflow = rdq->OverflowCount;
        for (size_t  i = 0;  i < noverflow; ++i)
        {
            io_rdbuffer_put(&rdq->BufferManager, rdq->DataOverflow[i].DataBuffer);
        }
        rdq->OverflowCount = 0;

        // at this point, all we might have remaining is jobs waiting on 
        // buffers to be returned. signal that we are idle if necessary.
        if (rdq->FinishCount == 0)
        {
            did_signal_idle = true;
            SetEvent(rdq->Idle);
        }
    }

    // process the cancellation queue. any items to be cancelled here may 
    // have the cancellation completed asynchronously, if they have pending AIO.
    size_t const ncancel = io_srsw_fifo_count(rdq->CancelQueue);
    for (size_t  i = 0;  i < ncancel; ++i)
    {
        uint32_t id;
        if (io_srsw_fifo_get(rdq->CancelQueue, id))
        {   // find this job in the active job list.
            size_t const nactive = rdq->ActiveCount;
            uint32_t const  *ids = rdq->ActiveJobIds;
            for (size_t j = 0; j < nactive; ++j)
            {
                if (ids[j] == id)
                {
                    io_rdq_file_t &rdf = rdq->ActiveJobList[j];
                    OVERLAPPED    *aio =&rdq->ActiveJobAIO [j];
                    if (rdf.Status & IO_RDQ_FILE_PENDING_AIO)
                    {   // cancel the pending I/O, but don't wait for it to 
                        // finish. the completion will be picked up on a later
                        // poll cycle. the file handle needs to remain open.
                        CancelIoEx_Func(rdf.File, aio);
                    }
                    else
                    {   // synchronous I/O, or no pending AIO, can be cancelled
                        // and completed immediately. close the file handle.
                        CloseHandle(rdf.File);
                        rdf.File     = INVALID_HANDLE_VALUE;
                        rdf.NanosEnd = io_timestamp();
                    }
                    if (rdq->OverflowCount)
                    {   // flush any pending overflow for the cancelled item.
                        for (size_t k = 0; k < rdq->OverflowCount; ++k)
                        {
                            if (rdq->DataOverflow[k].DataBuffer == rdf.TargetBuffer)
                            {   // swap the last overflow into slot 'k'.
                                rdq->DataOverflow[k] = rdq->DataOverflow[--rdq->OverflowCount];
                                rdf.ReturnCount--;
                                break;
                            }
                        }
                    }
                    rdf.Status |= IO_RDQ_FILE_CANCELLED;
                    break;
                }
            }
        }
    }

    // move any cancelled jobs from active to finish, and then 
    // complete any finished jobs. jobs pending buffer returns 
    // remain in the finished list.
    io_cull_active_jobs(rdq);
    io_finish_jobs(rdq);

    // signal the idle event if there are no active or pending
    // jobs, and we didn't signal the event processing cancel all.
    size_t nactive  = rdq->ActiveCount + rdq->FinishCount;
    size_t npending = io_srsw_fifo_count(rdq->PendingQueue);
    if (nactive + npending == 0 && !did_signal_idle)
        SetEvent(rdq->Idle);
}

/// @summary Attempts to publish any overflow data buffers to the data queue. 
/// The poll cycle cannot execute anything except for cancel operations until
/// all overflow operations have been dispatched to the data queue.
/// @param rdq The read queue to flush.
/// @return The number of unqueued read operations remaining.
static size_t io_flush_overflow(io_rdq_t *rdq)
{
    while (rdq->OverflowCount > 0)
    {
        if (io_srsw_fifo_put(rdq->DataQueue, rdq->DataOverflow[0]))
        {   // swap the last item in the overflow list into slot 0.
            // there can't be more than one pending read per active file,
            // so reads will still complete in order as we can't start 
            // the next read until the overflow read has been queued.
            rdq->DataOverflow[0] = rdq->DataOverflow[--rdq->OverflowCount];
        }
        else break; // the queue is full; no point in continuing.
    }
    return rdq->OverflowCount;
}

/// @summary Prepares an active job, opening the file, setting timestamps, etc.
/// @param rdq The read queue managing the job.
/// @param rdf The job state data to modify.
/// @param job The job description.
/// @param oserr On return, this is set to the value returned by GetLastError(),
/// if the function returns false; otherwise, it is set to ERROR_SUCCESS.
static bool io_prepare_job(io_rdq_t *rdq, io_rdq_file_t *rdf, io_rdq_job_t const &job, DWORD &oserr)
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
        oserr = GetLastError();
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
            oserr = ERROR_OFFSET_ALIGNMENT_VIOLATION;
            CloseHandle(file);
            return false;
        }
    }

    // create a new FIFO for buffer returns from the processing coordinator.
    io_returnq_t *returnq = io_create_returnq();
    if (returnq == NULL)
    {   // a return queue is required for correct functioning.
        oserr = ERROR_OUTOFMEMORY;
        CloseHandle(file);
        return false;
    }

    // everything looks good, so initialize the state.
    rdf->File             = file;
    rdf->TargetBuffer     = NULL;
    rdf->SectorSize       = ssize;
    rdf->BytesRead        = 0;
    rdf->Status           = IO_RDQ_FILE_NONE;
    rdf->OSError          = ERROR_SUCCESS;
    rdf->RangeCount       = (uint32_t) job.RangeCount;
    rdf->RangeIndex       = 0;
    rdf->ReturnQueue      = returnq;
    rdf->ReturnCount      = 0;
    rdf->BytesTotal       = 0;
    rdf->FileSize         = fsize;
    rdf->NanosEnq         = job.EnqueueTime;
    rdf->NanosEnd         = 0;
    for (size_t i = 0;  i < job.RangeCount; ++i)
    {   // convert ranges to (Begin, End) and calculate total number of bytes to read.
        rdf->RangeList[i].Offset = job.RangeList[i].Offset;
        rdf->RangeList[i].Amount = job.RangeList[i].Offset + job.RangeList[i].Amount;
        rdf->BytesTotal         += job.RangeList[i].Amount;
    }
    if  (job.RangeCount == 0)
    {   // the job consists of the entire file.
        rdf->RangeList[0].Offset = 0;
        rdf->RangeList[0].Amount = fsize;
        rdf->BytesTotal          = fsize;
        rdf->RangeCount          = 1;
    }
    oserr = ERROR_SUCCESS;
    return true;
}

/// @summary Implements the I/O update and schedule loop for synchronous I/O.
/// This is where we actually read data into target buffers, close files, and 
/// publish read operations to the read list.
/// @param rdq The I/O read queue being polled.
static void io_rdq_poll_sync(io_rdq_t *rdq)
{
    size_t const  mbread = rdq->BufferManager.AllocSize; // max bytes to read
    size_t const  nfiles = rdq->ActiveCount;
    for (size_t i = 0; i < nfiles; ++i)
    {
        io_rdq_file_t &rdf = rdq->ActiveJobList[i];
        io_range_t    &rng = rdf.RangeList[rdf.RangeIndex];
        DWORD          amt = (DWORD) (rng.Amount - rng.Offset);
        DWORD          nbt = 0;
        BOOL           res = FALSE;

        if (rdq->Flags & IO_RDQ_FLAGS_UNBUFFERED)
        {   // unbuffered I/O must be performed in multiples of the sector size.
            // otherwise, there are no restrictions on the amount being read.
            amt = (DWORD) align_up(amt, rdf.SectorSize);
        }

        // clamp the read amount to the buffer size.
        amt = (DWORD) clamp_to(amt, mbread);
        rdf.TargetBuffer  = io_rdbuffer_get(&rdq->BufferManager);
        if (rdf.TargetBuffer == NULL)
        {   // no buffers are available right now. try again next poll.
            continue;
        }

        // schedule the next read and possibly complete.
        res = ReadFile(rdf.File, rdf.TargetBuffer, amt, &nbt, NULL);
        if (res && nbt > 0)
        {   // nbt bytes of data was read from the file.
            io_rdop_t rop;
            rop.Id           = rdq->ActiveJobIds[i];
            rop.DataAmount   = nbt;
            rop.DataBuffer   = rdf.TargetBuffer;
            rop.ReturnQueue  = rdf.ReturnQueue;
            rop.FileOffset   = rng.Offset;
            rdf.BytesRead   += nbt;
            rng.Offset      += nbt;
            if (rng.Offset  >= rng.Amount)
            {   // this range has been fully processed.
                // unbuffered I/O might read more data than required.
                rdf.RangeIndex++;
            }
            if (rdf.RangeIndex == rdf.RangeCount)
            {   // we've finished reading data for this file.
                CloseHandle(rdf.File);
                rdf.File      = INVALID_HANDLE_VALUE;
                rdf.Status    = IO_RDQ_FILE_FINISHED;
                rdf.NanosEnd  = io_timestamp();
            }
            // post the successful read to the data queue.
            if (io_srsw_fifo_put(rdq->DataQueue, rop))
            {   // the read operation was successfully enqueued.
                rdf.ReturnCount++;
            }
            else
            {   // the data queue is full. try again at the start of the next 
                // io_rdq_poll() cycle. this causes a hard stall. the return 
                // count is incremented in anticipation of successful enqueue.
                rdq->DataOverflow[rdq->OverflowCount++] = rop;
                rdf.ReturnCount++;
            }
        }
        else
        {   // an error has occurred, or EOF was reached.
            DWORD err = GetLastError();
            if (err  == ERROR_SUCCESS || err == ERROR_HANDLE_EOF)
            {   // end of file was reached.
                rdf.Status  |= IO_RDQ_FILE_FINISHED | IO_RDQ_FILE_EOF;
                rdf.NanosEnd = io_timestamp();
            }
            else
            {   // an actual error has occurred.
                rdf.Status  |= IO_RDQ_FILE_ERROR;
                rdf.OSError  = err;
                rdf.NanosEnd = io_timestamp();
            }
            CloseHandle(rdf.File);
            rdf.File = INVALID_HANDLE_VALUE;
        }
    }
}

/// @summary Implements the I/O poll and schedule loop for asynchronous I/O queues.
/// This is where we actually read data into target buffers, close files, and 
/// publish read operations to the read list.
/// @param rdq The I/O read queue being polled.
static void io_rdq_poll_async(io_rdq_t *rdq)
{
    size_t const  mbread = rdq->BufferManager.AllocSize; // max bytes to read
    size_t const  nfiles = rdq->ActiveCount;
    for (size_t i = 0; i < nfiles; ++i)
    {
        io_rdq_file_t &rdf = rdq->ActiveJobList[i];
        OVERLAPPED    *aio =&rdq->ActiveJobAIO [i];
        DWORD          err = ERROR_SUCCESS;
        DWORD          nbt = 0;
        BOOL           res = FALSE;
        bool   schedule_io = false;

        if (rdf.Status & IO_RDQ_FILE_PENDING_AIO)
        {   // poll the status of the active read against this file.
            // specify a wait of zero so that we just poll instead of wait.
            res = GetOverlappedResultEx_Func(rdf.File, aio, &nbt, 0, TRUE);
            if (res && nbt > 0)
            {   // the read operation has completed successfully.
                io_rdop_t   rop;
                io_range_t &rng  = rdf.RangeList[rdf.RangeIndex];
                rop.Id           = rdq->ActiveJobIds[i];
                rop.DataAmount   = nbt;
                rop.DataBuffer   = rdf.TargetBuffer;
                rop.ReturnQueue  = rdf.ReturnQueue;
                rop.FileOffset   = rng.Offset;
                rdf.BytesRead   += nbt;
                rng.Offset      += nbt;
                if (rng.Offset  >= rng.Amount)
                {   // this range has been fully processed.
                    // unbuffered I/O might read more data than required.
                    rdf.RangeIndex++;
                }
                if (rdf.RangeIndex == rdf.RangeCount)
                {   // we've finished reading data for this file.
                    CloseHandle(rdf.File);
                    rdf.File      = INVALID_HANDLE_VALUE;
                    rdf.Status    = IO_RDQ_FILE_FINISHED;
                    rdf.NanosEnd  = io_timestamp();
                    schedule_io   = false;
                }
                else schedule_io  = true;

                // post the successful read to the data queue.
                if (io_srsw_fifo_put(rdq->DataQueue, rop))
                {   // the read operation was successfully enqueued.
                    rdf.ReturnCount++;
                }
                else
                {   // the data queue is full. try again at the start of the next 
                    // io_rdq_poll() cycle. this causes a hard stall. the return 
                    // count is incremented in anticipation of successful enqueue.
                    rdq->DataOverflow[rdq->OverflowCount++] = rop;
                    rdf.ReturnCount++;
                }

                // clear the AIO pending status; the next I/O might complete 
                // synchronously for all we know...
                rdf.Status &= ~IO_RDQ_FILE_PENDING_AIO;
            }
            else
            {
                if ((err = GetLastError()) == ERROR_IO_INCOMPLETE)
                {   // the read operation is still in-progress; no error.
                    schedule_io = false;
                }
                else if (err == ERROR_OPERATION_ABORTED)
                {   // the entire job was aborted. clean up.
                    CloseHandle(rdf.File);
                    rdf.File      = INVALID_HANDLE_VALUE;
                    rdf.Status   |= IO_RDQ_FILE_CANCELLED;
                    rdf.NanosEnd  = io_timestamp();
                    schedule_io   = false;
                }
                else if (err == ERROR_HANDLE_EOF)
                {   // end-of-file was reached. clean up.
                    CloseHandle(rdf.File);
                    rdf.File     = INVALID_HANDLE_VALUE;
                    rdf.Status  |= IO_RDQ_FILE_FINISHED | IO_RDQ_FILE_EOF;
                    rdf.NanosEnd = io_timestamp();
                    schedule_io  = false;
                }
                else
                {   // an error occurred while processing the request.
                    CloseHandle(rdf.File);
                    rdf.File     = INVALID_HANDLE_VALUE;
                    rdf.Status  |= IO_RDQ_FILE_ERROR;
                    rdf.OSError  = err;
                    rdf.NanosEnd = io_timestamp();
                    schedule_io  = false;
                }
            }
        }
        else
        {   // the previous read operation completed synchronously.
            // there is no outstanding read operation, so schedule one.
            schedule_io = true;
        }

        if (schedule_io)
        {   // schedule the next I/O operation. we need to be careful because
            // the I/O operation could actually complete synchronously. see:
            // 'Asynchronous Disk I/O Appears as Synchronous' located at 
            // http://support.microsoft.com/kb/156932
            io_range_t &rng = rdf.RangeList[rdf.RangeIndex];
            DWORD       amt = (DWORD) (rng.Amount - rng.Offset);
            BOOL        res = FALSE;

            if (rdq->Flags & IO_RDQ_FLAGS_UNBUFFERED)
            {   // unbuffered I/O must be performed in multiples of the sector size.
                // otherwise, there are no restrictions on the amount being read.
                amt = (DWORD) align_up(amt, rdf.SectorSize);
            }

            // clamp the read amount to the buffer size.
            amt = (DWORD) clamp_to(amt, mbread);
            rdf.TargetBuffer  = io_rdbuffer_get(&rdq->BufferManager);
            if (rdf.TargetBuffer == NULL)
            {   // no buffers are available right now. try again next poll.
                continue;
            }

            // set up the OVERLAPPED structure for the request.
            aio->Internal     = 0;
            aio->InternalHigh = 0;
            aio->Offset       = uint32_t((rng.Offset & 0x00000000FFFFFFFFULL) >>  0);
            aio->OffsetHigh   = uint32_t((rng.Offset & 0xFFFFFFFF00000000ULL) >> 32);

            // submit the read. this may complete synchronously or asynchronously.
            res = ReadFile(rdf.File, rdf.TargetBuffer, amt, &nbt, aio);
            if (res && nbt  > 0)
            {   // the read operation completed synchronously.
                io_rdop_t rop;
                rop.Id           = rdq->ActiveJobIds[i];
                rop.DataAmount   = nbt;
                rop.DataBuffer   = rdf.TargetBuffer;
                rop.ReturnQueue  = rdf.ReturnQueue;
                rop.FileOffset   = rng.Offset;
                rdf.BytesRead   += nbt;
                rng.Offset      += nbt;
                if (rng.Offset  >= rng.Amount)
                {   // this range has been fully processed.
                    // unbuffered I/O might read more data than required.
                    rdf.RangeIndex++;
                }
                if (rdf.RangeIndex == rdf.RangeCount)
                {   // we've finished reading data for this file.
                    CloseHandle(rdf.File);
                    rdf.File      = INVALID_HANDLE_VALUE;
                    rdf.Status    = IO_RDQ_FILE_FINISHED;
                    rdf.NanosEnd  = io_timestamp();
                }
                // post the successful read to the data queue.
                if (io_srsw_fifo_put(rdq->DataQueue, rop))
                {   // the read operation was successfully enqueued.
                    rdf.ReturnCount++;
                }
                else
                {   // the data queue is full. try again at the start of the next 
                    // io_rdq_poll() cycle. this causes a hard stall. the return 
                    // count is incremented in anticipation of successful enqueue.
                    rdq->DataOverflow[rdq->OverflowCount++] = rop;
                    rdf.ReturnCount++;
                }

                // don't try and poll the status on the next update.
                rdf.Status &= ~IO_RDQ_FILE_PENDING_AIO;
            }
            else
            {   // either an error occurred, or the read operation was queued.
                if ((err = GetLastError()) == ERROR_IO_PENDING)
                {   // the request will be completed asynchronously.
                    rdf.Status  |= IO_RDQ_FILE_PENDING_AIO;
                }
                else if (err == ERROR_HANDLE_EOF)
                {   // end-of-file was reached. clean up.
                    CloseHandle(rdf.File);
                    rdf.File     = INVALID_HANDLE_VALUE;
                    rdf.Status  |= IO_RDQ_FILE_FINISHED | IO_RDQ_FILE_EOF;
                    rdf.NanosEnd = io_timestamp();
                }
                else
                {   // an error occurred while processing the request.
                    CloseHandle(rdf.File);
                    rdf.File     = INVALID_HANDLE_VALUE;
                    rdf.Status  |= IO_RDQ_FILE_ERROR;
                    rdf.OSError  = err;
                    rdf.NanosEnd = io_timestamp();
                }
            }
        }
    }
}

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

io_rdpendq_t* LLCALL_C io_create_jobq(size_t capacity)
{
    uint32_t      nitem = (uint32_t     ) capacity;
    io_rdpendq_t *pendq = (io_rdpendq_t*) malloc(sizeof(io_rdpendq_t));
    if (io_create_srsw_waitable_fifo(pendq, pow2_ge(nitem, 1)))
    {   // the queue was created successfully.
        return pendq;
    }
    else
    {   // the queue could not be initialized.
        if (pendq != NULL) free(pendq);
        return NULL;
    }
}

void LLCALL_C io_delete_jobq(io_rdpendq_t *pendq)
{
    if (pendq != NULL)
    {
        io_delete_srsw_fifo(pendq);
        free(pendq);
    }
}

bool LLCALL_C io_jobq_wait_not_full(io_rdpendq_t *pendq, uint32_t timeout_ms)
{
    return io_srsw_fifo_wait_not_full(pendq, timeout_ms);
}

bool LLCALL_C io_jobq_wait_not_empty(io_rdpendq_t *pendq, uint32_t timeout_ms)
{
    return io_srsw_fifo_wait_not_empty(pendq, timeout_ms);
}

io_rddoneq_t* LLCALL_C io_create_completionq(size_t capacity)
{
    uint32_t      nitem = (uint32_t     ) capacity;
    io_rddoneq_t *doneq = (io_rddoneq_t*) malloc(sizeof(io_rddoneq_t));
    if (io_create_srsw_waitable_fifo(doneq, pow2_ge(nitem, 1)))
    {   // the queue was created successfully.
        return doneq;
    }
    else
    {   // the queue could not be initialized.
        if (doneq != NULL) free(doneq);
        return NULL;
    }
}

void LLCALL_C io_delete_completionq(io_rddoneq_t *doneq)
{
    if (doneq != NULL)
    {
        io_delete_srsw_fifo(doneq);
        free(doneq);
    }
}

bool LLCALL_C io_completionq_wait_not_full(io_rddoneq_t *doneq, uint32_t timeout_ms)
{
    return io_srsw_fifo_wait_not_full(doneq, timeout_ms);
}

bool LLCALL_C io_completionq_wait_not_empty(io_rddoneq_t *doneq, uint32_t timeout_ms)
{
    return io_srsw_fifo_wait_not_empty(doneq, timeout_ms);
}

bool LLCALL_C io_completionq_get(io_rddoneq_t *doneq, io_rdq_result_t &result)
{
    return io_srsw_fifo_get(doneq, result);
}

io_rdstopq_t* LLCALL_C io_create_cancelq(size_t capacity)
{
    uint32_t      nitem   = (uint32_t     ) capacity;
    io_rdstopq_t *cancelq = (io_rdstopq_t*) malloc(sizeof(io_rdstopq_t));
    if (io_create_srsw_fifo(cancelq, pow2_ge(nitem, 1)))
    {   // the queue was created successfully.
        return cancelq;
    }
    else
    {   // the queue could not be initialized.
        if (cancelq != NULL) free(cancelq);
        return NULL;
    }
}

void LLCALL_C io_delete_cancelq(io_rdstopq_t *cancelq)
{
    if (cancelq != NULL)
    {
        io_delete_srsw_fifo(cancelq);
        free(cancelq);
    }
}

io_rdopq_t* LLCALL_C io_create_dataq(size_t capacity)
{
    uint32_t    nitem = (uint32_t   ) capacity;
    io_rdopq_t *dataq = (io_rdopq_t*) malloc(sizeof(io_rdopq_t));
    if (io_create_srsw_waitable_fifo(dataq, pow2_ge(nitem, 1)))
    {   // the queue was created successfully.
        return dataq;
    }
    else
    {   // the queue could not be initialized.
        if (dataq != NULL) free(dataq);
        return NULL;
    }
}

void LLCALL_C io_delete_dataq(io_rdopq_t *dataq)
{
    if (dataq != NULL)
    {
        io_delete_srsw_fifo(dataq);
        free(dataq);
    }
}

bool LLCALL_C io_dataq_wait_not_empty(io_rdopq_t *dataq, uint32_t timeout_ms)
{
    return io_srsw_fifo_wait_not_empty(dataq, timeout_ms);
}

bool LLCALL_C io_dataq_get(io_rdopq_t *dataq, io_rdop_t &op)
{
    return io_srsw_fifo_get(dataq, op);
}

io_rdq_t* LLCALL_C io_create_rdq(io_rdq_config_t &config)
{
    // ensure that our required kernel32 entry points are resolved.
    resolve_kernel_apis();

    // get the page size on this system.
    SYSTEM_INFO sysinfo = {0};
    GetNativeSystemInfo_Func(&sysinfo);
    size_t page_size    = sysinfo.dwPageSize;

    // validate the queue configuration.
    if (config.DataQueue     == NULL)  return NULL;
    if (config.PendingQueue  == NULL)  return NULL;
    if (config.CancelQueue   == NULL)  return NULL;
    if (config.CompleteQueue == NULL)  return NULL;
    if (config.MaxConcurrent  < 1)
        config.MaxConcurrent  = 1;
    if (config.MaxConcurrent  > IO_MAX_FILES)
        config.MaxConcurrent  = IO_MAX_FILES;
    if (config.MaxBufferSize  < IO_MIN_BUFFER_SIZE)
        config.MaxBufferSize  = IO_MIN_BUFFER_SIZE;
    if (config.MaxReadSize   == IO_ONE_PAGE)
        config.MaxReadSize    = page_size;
    if (config.MaxReadSize    < IO_MIN_READ_SIZE)
        config.MaxReadSize    = IO_MIN_READ_SIZE;

    io_rdq_t *rdq = (io_rdq_t*) malloc(sizeof(io_rdq_t));
    rdq->Flags    =  IO_RDQ_FLAGS_NONE;
    if (config.Unbuffered)   rdq->Flags |= IO_RDQ_FLAGS_UNBUFFERED;
    if (config.Asynchronous) rdq->Flags |= IO_RDQ_FLAGS_ASYNC;

    rdq->Idle           = CreateEvent(NULL, TRUE, TRUE, NULL); // manual reset; signaled.
    rdq->OverflowCount  = 0;
    rdq->DataOverflow   = (io_rdop_t    *) malloc(config.MaxConcurrent * sizeof(io_rdop_t));
    rdq->CancelQueue    = config.CancelQueue;
    rdq->PendingQueue   = config.PendingQueue;
    rdq->ActiveCapacity = config.MaxConcurrent;
    rdq->ActiveCount    = 0;
    rdq->ActiveJobIds   = (uint32_t     *) malloc(config.MaxConcurrent * sizeof(uint32_t));
    rdq->ActiveJobList  = (io_rdq_file_t*) malloc(config.MaxConcurrent * sizeof(io_rdq_file_t));
    rdq->ActiveJobAIO   = (OVERLAPPED   *) malloc(config.MaxConcurrent * sizeof(OVERLAPPED));
    rdq->ActiveJobWait  = (HANDLE       *) malloc(config.MaxConcurrent * sizeof(HANDLE));
    rdq->MaxReadSize    = config.MaxReadSize;
    rdq->DataQueue      = config.DataQueue;
    rdq->CompleteQueue  = config.CompleteQueue;
    rdq->FinishCapacity = config.MaxConcurrent;
    rdq->FinishCount    = 0;
    rdq->FinishJobIds   = (uint32_t     *) malloc(config.MaxConcurrent * sizeof(uint32_t));
    rdq->FinishJobList  = (io_rdq_file_t*) malloc(config.MaxConcurrent * sizeof(io_rdq_file_t));
    io_create_rdbuffer(&rdq->BufferManager, config.MaxBufferSize, config.MaxReadSize);
    if (config.Asynchronous)
    {   // initialize the OVERLAPPED structures with a manual reset event.
        for (size_t i = 0; i < config.MaxConcurrent; ++i)
        {
            rdq->ActiveJobAIO[i].Internal     = 0;
            rdq->ActiveJobAIO[i].InternalHigh = 0;
            rdq->ActiveJobAIO[i].Offset       = 0;
            rdq->ActiveJobAIO[i].OffsetHigh   = 0;
            rdq->ActiveJobAIO[i].hEvent       = CreateEvent(NULL, TRUE, FALSE, NULL);
        }
    }
    else
    {   // initialize the OVERLAPPED structures to zero.
        for (size_t i = 0; i < config.MaxConcurrent; ++i)
        {
            rdq->ActiveJobAIO[i].Internal     = 0;
            rdq->ActiveJobAIO[i].InternalHigh = 0;
            rdq->ActiveJobAIO[i].Offset       = 0;
            rdq->ActiveJobAIO[i].OffsetHigh   = 0;
            rdq->ActiveJobAIO[i].hEvent       = NULL;
        }
    }

    // update the config to let the caller know what they actually got.
    config.MaxBufferSize = rdq->BufferManager.TotalSize;
    config.MaxReadSize   = rdq->BufferManager.AllocSize;
    return rdq;
}

void LLCALL_C io_delete_rdq(io_rdq_t *rdq)
{
    if (rdq != NULL)
    {
        size_t nreturns  = rdq->FinishCount;
        size_t nactive   = rdq->ActiveCount;
        size_t npending  = rdq->PendingQueue != NULL ? io_srsw_fifo_count(rdq->PendingQueue) : 0;
        assert(nreturns == 0 && nactive == 0 && npending == 0);
        UNUSED_LOCAL(nreturns);
        UNUSED_LOCAL(nactive );
        UNUSED_LOCAL(npending);

        if (rdq->ActiveJobAIO != NULL && (rdq->Flags & IO_RDQ_FLAGS_ASYNC))
        {   // clean up the manual reset events allocated to the AIOs.
            for (size_t i = 0; i < rdq->ActiveCount; ++i)
            {
                if (rdq->ActiveJobAIO[i].hEvent != NULL)
                {
                    CloseHandle(rdq->ActiveJobAIO[i].hEvent);
                    rdq->ActiveJobAIO[i].hEvent  = NULL;
                }
            }
        }
        io_delete_rdbuffer(&rdq->BufferManager);
        if (rdq->FinishJobList != NULL) free(rdq->FinishJobList);
        if (rdq->FinishJobIds  != NULL) free(rdq->FinishJobIds);
        if (rdq->ActiveJobWait != NULL) free(rdq->ActiveJobWait);
        if (rdq->ActiveJobAIO  != NULL) free(rdq->ActiveJobAIO);
        if (rdq->ActiveJobList != NULL) free(rdq->ActiveJobList);
        if (rdq->ActiveJobIds  != NULL) free(rdq->ActiveJobIds);
        if (rdq->DataOverflow  != NULL) free(rdq->DataOverflow);
        if (rdq->Idle != INVALID_HANDLE_VALUE) CloseHandle(rdq->Idle);

        rdq->Flags          = IO_RDQ_FLAGS_NONE;
        rdq->Idle           = INVALID_HANDLE_VALUE;
        rdq->OverflowCount  = 0;
        rdq->DataOverflow   = NULL;
        rdq->CancelQueue    = NULL;
        rdq->PendingQueue   = NULL;
        rdq->ActiveCapacity = 0;
        rdq->ActiveCount    = 0;
        rdq->ActiveJobIds   = NULL;
        rdq->ActiveJobList  = NULL;
        rdq->ActiveJobAIO   = NULL;
        rdq->ActiveJobWait  = NULL;
        rdq->DataQueue      = NULL;
        rdq->CompleteQueue  = NULL;
        rdq->FinishCapacity = 0;
        rdq->FinishCount    = 0;
        rdq->FinishJobIds   = NULL;
        rdq->FinishJobList  = NULL;
        free(rdq);
    }
}

bool LLCALL_C io_rdq_poll_idle(io_rdq_t *rdq)
{
    size_t const noverflow = rdq->OverflowCount;
    size_t const nactive   = rdq->ActiveCount;
    size_t const nfinish   = rdq->FinishCount;
    size_t const npending  = io_srsw_fifo_count(rdq->PendingQueue);
    return (noverflow == 0 && nactive == 0 && nfinish == 0 && npending == 0);
}

bool LLCALL_C io_rdq_wait_idle(io_rdq_t *rdq, uint32_t timeout_ms)
{
    DWORD   result  = WaitForSingleObjectEx(rdq->Idle, timeout_ms, TRUE);
    return (result == WAIT_OBJECT_0);
}

void LLCALL_C io_rdq_cancel_all(io_rdq_t *rdq)
{
    uintptr_t address = (uintptr_t) &rdq->Flags;
    uint32_t  flags   = rdq->Flags | IO_RDQ_FLAGS_CANCEL_ALL;
    io_atomic_write_uint32_aligned(address, flags);
}

void LLCALL_C io_rdq_cancel_one(io_rdq_t *rdq, uint32_t id)
{
    io_srsw_fifo_put(rdq->CancelQueue, id);
}

bool LLCALL_C io_rdq_submit(io_rdq_t *rdq, io_rdq_job_t const &job)
{   // note: potential issue here with the lifetime of job.Path.
    // do we copy the string and free it on job dequeue?
    io_rdq_job_t job_copy = job;
    job_copy.EnqueueTime  = io_timestamp();
    if (job_copy.Path == NULL) return false;
    if (job_copy.RangeCount > IO_MAX_RANGES)
        job_copy.RangeCount = IO_MAX_RANGES;

    return io_srsw_fifo_put(rdq->PendingQueue, job_copy);
}

bool LLCALL_C io_rdq_wait_io(io_rdq_t *rdq, uint32_t timeout_ms)
{
    if ((rdq->Flags & IO_RDQ_FLAGS_ASYNC) == 0)
    {   // this is not an async queue, so we can always begin a new poll.
        return true;
    }
    if (rdq->ActiveCount == 0)
    {   // there are no active files, so begin a new poll to see if there
        // are any jobs in the pending queue.
        return true;
    }
    if (rdq->OverflowCount > 0)
    {   // there are overflow buffers pending, so spin as fast as we can 
        // to try and flush the overflow queue and return to normal operation.
        return true;
    }
    // at this point, we have active jobs and are allowed to perform async I/O.
    // note that we could be woken up because of a queued APC also.
    DWORD n      = (DWORD) rdq->ActiveCount;
    DWORD ok_min = WAIT_OBJECT_0;
    DWORD ok_max = WAIT_OBJECT_0    + n;
    DWORD ab_min = WAIT_ABANDONED_0;
    DWORD ab_max = WAIT_ABANDONED_0 + n;
    DWORD result = WaitForMultipleObjectsEx(n, rdq->ActiveJobWait, FALSE, timeout_ms, TRUE);
    if  (result >= ok_min && result < ok_max) return true;  // An event was signaled.
    if  (result >= ab_min && result < ab_max) return false; // One or more handles was closed?
    if  (result == WAIT_IO_COMPLETION)        return true;  // An APC was queued.
    if  (result == WAIT_TIMEOUT)              return false; // The timeout interval elapsed.
    if  (result == WAIT_FAILED)               return false; // Generic error.
    return false;
}

void LLCALL_C io_rdq_poll(io_rdq_t *rdq)
{
    // process any pending cancellations. if a CANCEL_ALL operation is 
    // pending, then this may block until all active jobs have been cancelled.
    io_process_cancellations(rdq);

    // attempt to push any queued read operations (at most, rdq->ActiveCapacity)
    // to the DataQueue for processing. if there are any queued read operations 
    // remaining in the overflow buffer, we cannot continue with the poll cycle.
    if (io_flush_overflow(rdq))
    {   // there are still buffers in the overflow queue.
        return;
    }

    // dequeue and start pending jobs until we've reached capacity, or 
    // no pending jobs remain in the pending job queue.
    bool          reset_idle = false;
    size_t const nconcurrent = rdq->ActiveCapacity;
    while (rdq->ActiveCount  < nconcurrent)
    {
        io_rdq_job_t job;
        if (io_srsw_fifo_get(rdq->PendingQueue, job) == false)
        {   // there are no more pending jobs in the queue.
            break;
        }

        // we popped a pending job from the queue.
        if (reset_idle == false)
        {   // the I/O manager is no longer idle.
            ResetEvent(rdq->Idle);
            reset_idle = true;
        }

        // attempt to open the file and allocate resource for the job.
        DWORD          oserr =  ERROR_SUCCESS;
        uint64_t  start_time =  io_timestamp();
        size_t  active_index =  rdq->ActiveCount;
        io_rdq_file_t   *rdf = &rdq->ActiveJobList[active_index];
        if (io_prepare_job(rdq, rdf, job, oserr) == false)
        {   // fill out an io_rdq_file_t representing the failed operation.
            io_rdq_file_t rdt;
            rdt.File         = INVALID_HANDLE_VALUE;
            rdt.TargetBuffer = NULL;
            rdt.SectorSize   = 0;
            rdt.BytesRead    = 0;
            rdt.Status       = IO_RDQ_FILE_ERROR;
            rdt.OSError      = oserr;
            rdt.RangeCount   = 0;
            rdt.RangeIndex   = 0;
            rdt.ReturnQueue  = NULL;
            rdt.ReturnCount  = 0;
            rdt.BytesTotal   = 0;
            rdt.FileSize     = 0;
            rdt.NanosEnq     = job.EnqueueTime;
            rdt.NanosDeq     = start_time;
            rdt.NanosEnd     = start_time;
            for (size_t i = 0; i < job.RangeCount; ++i)
            {
                rdt.BytesTotal  += job.RangeList[i].Amount;
            }
            io_complete_job(rdq, &rdt, job.JobId);
            continue;
        }

        // the job was initialized, so put it into active status.
        // note that io_prepare_job wrote into rdq->ActiveJobList[active_index].
        rdf->NanosDeq     = start_time;
        rdq->ActiveJobIds [active_index] = job.JobId;
        rdq->ActiveJobWait[active_index] = rdq->ActiveJobAIO[active_index].hEvent;
        rdq->ActiveCount++;
    }

    // update and schedule I/O operations against the active job list.
    if (rdq->Flags & IO_RDQ_FLAGS_ASYNC) io_rdq_poll_async(rdq);
    else io_rdq_poll_sync(rdq);

    // perform job cleanup, move active->finished and remove finished.
    // this may update rdq->ActiveCount and signal rdq->Idle.
    io_cull_active_jobs(rdq);
    io_finish_jobs(rdq);
}

bool LLCALL_C io_return_buffer(io_returnq_t *returnq, void *buffer)
{
    return io_srsw_fifo_put(returnq, buffer);
}

