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
/// @summary Define the maximum number of completed, but unprocessed read
/// operations. This value must be a power-of-two, greater than zero.
#ifndef IO_RDLIST_CAPACITY
#define IO_RDLIST_CAPACITY   128
#endif

/// @summary Define the maximum number of read buffers that have been processed
/// and returned to the read queue, and will be 'freed' at the end of the next 
/// read queue update operation.
#ifndef IO_RDQ_MAX_RETURNS
#define IO_RDQ_MAX_RETURNS   512
#endif

/*///////////////
//   Globals   //
///////////////*/

/*//////////////////
//   Data Types   //
//////////////////*/
/// @summary MinGW doesn't define a bunch of these constants and structures.
#ifdef __GNUC__
#define METHOD_BUFFERED              0
#define FILE_ANY_ACCESS              0
#define FILE_DEVICE_MASS_STORAGE     0x0000002d

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
#endif

/// @summary Defines the data associated with a set of completed read operations.
struct io_rdlist_t
{
    HANDLE           Available;     /// Manual Reset Event signaled when reads available.
    io_srsw_flq_t    OpQueue;       /// SRSW queue of available read operations.
    io_read_t        OpStore[IO_RDLIST_CAPACITY]; /// available read operations.
};

/// @summary Defines the data associated with a single file being actively read.
struct io_rdfile_t
{
    size_t           Size;          /// Total size of the file, in bytes.
    size_t          *TargetAmount;  /// The current target buffer fill amount, or NULL.
    void            *TargetBuffer;  /// The current target buffer, or NULL.
    HANDLE           File;          /// HANDLE returned by CreateFileExA.
    OVERLAPPED       AIO;           /// Asynchronous I/O operation state.
};

/// @summary Defines the data associated with a read queue for concurrent read operations.
struct io_rdq_t
{
    size_t           FileCapacity;  /// The maximum number of concurrently active files. Fixed.
    size_t           FileCount;     /// The number of currently active files.
    uint32_t        *FileIds;       /// Application identifiers for the active files. Fixed length.
    io_rdfile_t     *FileInfo;      /// Internal I/O state for the active files. Fixed length.
    HANDLE          *WaitHandles;   /// Array of OVERLAPPED::hEvent for active files. Fixed length.
    size_t           PoolCapacity;  /// The maximum number of read buffer pools.
    size_t           PoolCount;     /// The number of active read buffer pools.
    size_t           MaxBufferSize; /// The maximum allowable size of all read buffers.
    size_t           BufferSize;    /// The default size of a single read buffer.
    io_rdbuf_list_t *ReadBuffers;   /// List of internal read buffer pools, one pool per-alignment.
    io_rdlist_t     *ReadList;      /// Application managed queue for completed read ops.
    uint32_t         ReturnCount;   /// The number of buffers waiting to be recycled.
    void            *Returns[IO_RDQ_MAX_RETURNS]; /// Returned buffers to be recycled at end-of-update.
    HANDLE           Available;     /// Manual Reset Event signaled when one or more slots available.
    HANDLE           Empty;         /// Manual Reset Event signaled when all slots are available.
};

/*///////////////////////
//   Local Functions   //
///////////////////////*/
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

io_rdlist_t* LLCALL_C io_create_read_list(void)
{
    io_rdlist_t *rdlist = (io_rdlist_t*) malloc(sizeof(io_rdlist_t));
    if (rdlist)
    {   // auto reset event, nonsignaled, unsharable, local to process.
        rdlist->Available = CreateEvent(NULL, FALSE, FALSE, NULL);
        io_srsw_flq_clear(rdlist->OpQueue, IO_RDLIST_CAPACITY);
    }
    return rdlist;
}

void LLCALL_C io_delete_read_list(io_rdlist_t *rdlist)
{
    if (rdlist)
    {
        if (rdlist->Available != INVALID_HANDLE_VALUE)
        {
            CloseHandle(rdlist->Available);
            rdlist->Available  = INVALID_HANDLE_VALUE;
        }
        free(rdlist);
    }
}

bool LLCALL_C io_wait_read_list(io_rdlist_t *rdlist, uint32_t timeout_ms)
{
    DWORD   result  = WaitForSingleObjectEx(rdlist->Available, timeout_ms, TRUE);
    return (result == WAIT_OBJECT_0);
}

size_t LLCALL_C io_read_list_available(io_rdlist_t *rdlist)
{
    return io_srsw_flq_count(rdlist->OpQueue);
}

void LLCALL_C io_read_list_get(io_rdlist_t *rdlist, io_read_t *op_out)
{
    uint32_t index = io_srsw_flq_next_pop(rdlist->OpQueue);
    *op_out  = rdlist->OpStore[index];
    PROCESSOR_MFENCE_READ;
    io_srsw_flq_pop(rdlist->OpQueue);
}

void LLCALL_C io_flush_read_list(io_rdlist_t *rdlist)
{
    io_srsw_flq_clear(rdlist->OpQueue, IO_RDLIST_CAPACITY);
}

void LLCALL_C io_read_list_put(io_rdlist_t *rdlist, io_read_t const &op)
{
    uint32_t count = io_srsw_flq_count(rdlist->OpQueue) + 1;
    if (count < IO_RDLIST_CAPACITY)
    {
        uint32_t index = io_srsw_flq_next_push(rdlist->OpQueue);
        rdlist->OpStore[index] = op;
        PROCESSOR_MFENCE_WRITE;
        io_srsw_flq_push(rdlist->OpQueue);
        SetEvent(rdlist->Available);
    }
}

io_rdq_t* LLCALL_C io_create_rdq(io_rdq_config_t const *config)
{
    if (config == NULL)             return NULL;
    if (config->ReadList == NULL)   return NULL;
    if (config->MaxConcurrent == 0) return NULL;
    if (config->MaxBufferSize == 0) return NULL;
    if (config->BufferSize == 0)    return NULL;

    io_rdq_t *rdq = (io_rdq_t*) malloc(sizeof(io_rdq_t));
    if (rdq)
    {
        rdq->FileCapacity  = config->MaxConcurrent;
        rdq->FileCount     = 0;
        rdq->FileIds       = (uint32_t   *) malloc(config->MaxConcurrent * sizeof(uint32_t));
        rdq->FileInfo      = (io_rdfile_t*) malloc(config->MaxConcurrent * sizeof(io_rdfile_t));
        rdq->WaitHandles   = (HANDLE     *) malloc(config->MaxConcurrent * sizeof(HANDLE));
        rdq->PoolCapacity  = 0;
        rdq->PoolCount     = 0;
        rdq->MaxBufferSize = config->MaxBufferSize;
        rdq->BufferSize    = config->BufferSize;
        rdq->ReadBuffers   = NULL;
        rdq->ReadList      = config->ReadList;
        rdq->ReturnCount   = 0;
        rdq->Available     = CreateEvent(NULL, TRUE, TRUE, NULL);
        rdq->Empty         = CreateEvent(NULL, TRUE, TRUE, NULL);
        for (size_t i = 0; i < config->MaxConcurrent; ++i)
        {
            rdq->FileIds[i]  = 0xFFFFFFFFU;
            io_rdfile_t &rdf = rdq->FileInfo[i];
            rdf.Size         = 0;
            rdf.TargetAmount = NULL;
            rdf.TargetBuffer = NULL;
            rdf.File         = INVALID_HANDLE_VALUE;
            if (config->AsyncIo)
            {
                rdf.AIO.Internal     = 0;
                rdf.AIO.InternalHigh = 0;
                rdf.AIO.Offset       = 0;
                rdf.AIO.OffsetHigh   = 0;
                rdf.AIO.hEvent       = CreateEvent(NULL, TRUE, FALSE, NULL);
            }
            else
            {
                rdf.AIO.Internal     = 0;
                rdf.AIO.InternalHigh = 0;
                rdf.AIO.Offset       = 0;
                rdf.AIO.OffsetHigh   = 0;
                rdf.AIO.hEvent       = INVALID_HANDLE_VALUE;
            }
        }
        return rdq;
    }
    else return NULL;
}

void LLCALL_C io_delete_rdq(io_rdq_t *rdq)
{
    if (rdq)
    {
        if (rdq->FileCapacity)
        {
            for (size_t i = 0; i < rdq->FileCapacity; ++i)
            {
                io_rdfile_t &rdf = rdq->FileInfo[i];
                if (rdf.AIO.hEvent != INVALID_HANDLE_VALUE)
                {
                    if (rdf.File  != INVALID_HANDLE_VALUE)
                        CancelIo(rdf.File);

                    CloseHandle(rdf.AIO.hEvent);
                    rdf.AIO.hEvent = INVALID_HANDLE_VALUE;
                }
                if (rdf.File != INVALID_HANDLE_VALUE)
                {
                    CloseHandle(rdf.File);
                    rdf.File  = INVALID_HANDLE_VALUE;
                }
            }
            if (rdq->ReadList)
            {
                io_flush_read_list(rdq->ReadList);
            }
            for (size_t i = 0; i < rdq->PoolCapacity; ++i)
            {
                io_delete_rdbuf_list(&rdq->ReadBuffers[i]);
            }
        }
        if (rdq->Available != INVALID_HANDLE_VALUE)
        {
            CloseHandle(rdq->Available);
        }
        if (rdq->Empty != INVALID_HANDLE_VALUE)
        {
            CloseHandle(rdq->Empty);
        }
        if (rdq->ReadBuffers != NULL) free(rdq->ReadBuffers);
        if (rdq->WaitHandles != NULL) free(rdq->WaitHandles);
        if (rdq->FileInfo    != NULL) free(rdq->FileInfo);
        if (rdq->FileIds     != NULL) free(rdq->FileIds);
        rdq->FileCapacity = 0;
        rdq->FileCount    = 0;
        rdq->FileIds      = NULL;
        rdq->FileInfo     = NULL;
        rdq->WaitHandles  = NULL;
        rdq->PoolCapacity = 0;
        rdq->PoolCount    = 0;
        rdq->ReadBuffers  = NULL;
        rdq->ReadList     = NULL;
        rdq->ReturnCount  = 0;
        rdq->Available    = INVALID_HANDLE_VALUE;
        free(rdq);
    }
}

