/*/////////////////////////////////////////////////////////////////////////////
/// @summary Implements the platform-agnostic functions for the I/O module.
///////////////////////////////////////////////////////////////////////////80*/

/*////////////////
//   Includes   //
////////////////*/
#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include "io.hpp"

/*/////////////////
//   Constants   //
/////////////////*/
#ifdef _MSC_VER
#include <intrin.h>
#define  ROTATE_LEFT(x, n)    _lrotl((x), (n))
#endif

#ifdef __GNUC__
#define  ROTATE_LEFT(x, n)    (((x) << (n)) | ((x) >> (32-(n))))
#endif

#ifndef UTF8_TOUPPER
#define UTF8_TOUPPER(x)       (((x) >= 'a' && (x) <= 'z') ? ((x)-'a'+'A') : (x))
#endif

#ifndef UTF8_TOLOWER
#define UTF8_TOLOWER(x)       (((x) >= 'A' && (x) <= 'Z') ? ((x)-'A'+'a') : (x))
#endif

/*///////////////
//   Globals   //
///////////////*/
/// @summary Path storage for a file list grows in 1024 item increments after
/// it hits 1024 items in size; prior to that point, it doubles in size.
static size_t const PATH_GROW_LIMIT = 1024;

/// @summary Blob storage for a file list grows in 64KB chunks after it hits 
/// 64KB in size; prior to that point, it doubles in size.
static size_t const BLOB_GROW_LIMIT = 64 * 1024 * 1024;

/*///////////////////////
//   Local Functions   //
///////////////////////*/
/// @summary Retrieves the next UTF-8 codepoint from a string.
/// @param str Pointer to the start of the codepoint.
/// @param cp On return, this value stores the current codepoint.
/// @return A pointer to the start of the next codepoint.
static inline char const* next_codepoint(char const *str, uint32_t &cp)
{
    if ((str[0] & 0x80) == 0)
    {   // cp in [0x00000, 0x0007F], most likely case.
        cp = str[0];
        return str + 1;
    }
    if ((str[0] & 0xFF) >= 0xC2 &&   (str[0] & 0xFF) <= 0xDF && (str[1] & 0xC0) == 0x80)
    {   // cp in [0x00080, 0x007FF]
        cp = (str[0] & 0x1F) <<  6 | (str[1] & 0x3F);
        return str + 2;
    }
    if ((str[0] & 0xF0) == 0xE0 &&   (str[1] & 0xC0) == 0x80 && (str[2] & 0xC0) == 0x80)
    {   // cp in [0x00800, 0x0FFFF]
        cp = (str[0] & 0x0F) << 12 | (str[1] & 0x3F) << 6  |    (str[2] & 0x3F);
        return str + 3;
    }
    if ((str[0] & 0xFF) == 0xF0 &&   (str[1] & 0xC0) == 0x80 && (str[2] & 0xC0) == 0x80 && (str[3] & 0xC0) == 0x80)
    {   // cp in [0x10000, 0x3FFFF]
        cp = (str[1] & 0x3F) << 12 | (str[2] & 0x3F) << 6  |    (str[3] & 0x3F);
        return str + 4;
    }
    // else, invalid UTF-8 codepoint.
    cp = 0xFFFFFFFFU;
    return str + 1;
}

/// @summary Calculates the number of items by which to grow a dynamic list.
/// @param value The current capacity.
/// @param limit The number of items beyond which the capacity stops doubling.
/// @param min_value The minimum acceptable capacity.
static inline size_t grow_size(size_t value, size_t limit, size_t min_value)
{
    size_t new_value = 0;

    if (value >= limit)
        new_value = value + limit;
    else
        new_value = value * 2;

    return new_value >= min_value ? new_value : min_value;
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

/// @summary Returns an address aligned to a given power-of-two value.
/// @param address The address to align.
/// @param pow2 The power-of-two alignment.
/// @return The input address, rounded up to the nearest even multiple of pow2.
template <typename T>
static inline T* align_to(T* address, size_t pow2)
{
    assert((pow2 & (pow2-1)) == 0);
    return (T*) ((address == NULL) ? pow2 : ((((uintptr_t)address) + (pow2-1)) & ~(pow2-1)));
}

/// @summary Allocates memory using malloc with alignment.
/// @param size_in_bytes The amount of memory being requested.
/// @param alignment The desired power-of-two alignment of the returned address.
/// @param actual On return, stores the number of bytes actually allocated.
/// @return A pointer to a memory block whose address is an even integer multiple
/// of alignment, and whose size is at least size_in_bytes, or NULL.
static void* allocate_aligned(size_t size_in_bytes, size_t alignment, size_t &actual)
{
    size_t   total_size = size_in_bytes + sizeof(uintptr_t); // allocate enough extra to store the base address
    size_t   alloc_size = align_up(total_size, alignment);   // allocate enough extra to properly align
    uint8_t *mem_ptr    = (uint8_t*) malloc(alloc_size);     // allocate the raw memory block
    uint8_t *aln_ptr    = align_to(mem_ptr, alignment);      // calculate the aligned address
    uint8_t *base       = aln_ptr - sizeof(uintptr_t);       // where to store the address returned by malloc
    *(uintptr_t*)base   = (uintptr_t) mem_ptr;               // store the address returned by malloc
    actual = alloc_size;
    return aln_ptr;
}

/// @summary Frees a memory block allocated using allocate_aligned().
/// @param address The address returned by allocate_aligned().
static void free_aligned(void *address)
{
    if (address != NULL)
    {
        uint8_t *aln_ptr  = (uint8_t*) address;              // the aligned address returned to the caller
        uint8_t *base     = aln_ptr - sizeof(uintptr_t);     // where we stored the address returned by malloc
        void    *mem_ptr  = (void*) *(uintptr_t*) base;      // the address returned by malloc
        free(mem_ptr);
    }
}

/*////////////////////////
//   Public Functions   //
////////////////////////*/
void LLCALL_C io_atomic_write_uint32_aligned(uintptr_t address, uint32_t value)
{
    assert((address & 0x03) == 0);                  // assert address is 32-bit aligned
    uint32_t *p  = (uint32_t*) address;
    *p = value;
}

void LLCALL_C io_atomic_write_pointer_aligned(uintptr_t address, uintptr_t value)
{
    assert((address & (sizeof(uintptr_t)-1)) == 0); // assert address is pointer-size aligned
    uintptr_t *p = (uintptr_t*) address;
    *p = value;
}

uint32_t LLCALL_C io_atomic_read_uint32_aligned(uintptr_t address)
{
    assert((address & 0x03) == 0);
    volatile uint32_t *p = (uint32_t*) address;
    return (*p);
}

uint32_t LLCALL_C io_hash_path(char const *path, char const **out_end)
{
    if (path == NULL)
    {
        if (out_end) *out_end = NULL;
        return 0;
    }

    uint32_t    cp   = 0;
    uint32_t    cp2  = 0;
    uint32_t    cp3  = 0;
    uint32_t    hash = 0;
    char const *iter = next_codepoint(path, cp);
    while (cp != 0)
    {
        cp2    = UTF8_TOUPPER(cp);
        cp3    = cp != '\\' ? cp2 : '/';
        hash   = ROTATE_LEFT(hash, 7) + cp3;
        iter   = next_codepoint(iter, cp);
    }
    if (out_end)
    {   
       *out_end = iter + 1;
    }
    return hash;
}

bool LLCALL_C io_create_file_list(io_file_list_t *list, size_t capacity, size_t path_bytes)
{
    if (list)
    {
        list->PathCapacity = uint32_t(capacity);
        list->PathCount    = 0;
        list->BlobCapacity = uint32_t(path_bytes);
        list->BlobCount    = 0;
        list->MaxPathBytes = 0;
        list->TotalBytes   = 0;
        list->HashList     = NULL;
        list->SizeList     = NULL;
        list->PathOffset   = NULL;
        list->PathData     = NULL;
        if (capacity > 0)
        {
            list->HashList    = (uint32_t *) malloc(capacity * sizeof(uint32_t));
            list->SizeList    = (uint32_t *) malloc(capacity * sizeof(uint32_t));
            list->PathOffset  = (uint32_t *) malloc(capacity * sizeof(uint32_t));
            list->TotalBytes +=  uint32_t(capacity * sizeof(uint32_t) * 3);
        }
        if (path_bytes > 0)
        {
            list->PathData    = (char*)  malloc(path_bytes * sizeof(char));
            list->TotalBytes +=  uint32_t(path_bytes * sizeof(char));
        }
        return true;
    }
    else return false;
}

void LLCALL_C io_delete_file_list(io_file_list_t *list)
{
    if (list)
    {
        if (list->PathData   != NULL) free(list->PathData);
        if (list->PathOffset != NULL) free(list->PathOffset);
        if (list->SizeList   != NULL) free(list->SizeList);
        if (list->HashList   != NULL) free(list->HashList);
        list->PathCapacity    = 0;
        list->PathCount       = 0;
        list->BlobCapacity    = 0;
        list->BlobCount       = 0;
        list->MaxPathBytes    = 0;
        list->TotalBytes      = 0;
        list->HashList        = NULL;
        list->SizeList        = NULL;
        list->PathOffset      = NULL;
        list->PathData        = NULL;
    }
}

bool LLCALL_C io_ensure_file_list(io_file_list_t *list, size_t capacity, size_t path_bytes)
{
    if (list)
    {
        if (list->PathCapacity >= capacity && list->BlobCapacity >= path_bytes)
        {
            // the list already meets the specified capacity; nothing to do.
            return true;
        }
        if (list->PathCapacity < capacity)
        {
            uint32_t *hash     = (uint32_t*) realloc(list->HashList  , capacity * sizeof(uint32_t));
            uint32_t *size     = (uint32_t*) realloc(list->SizeList  , capacity * sizeof(uint32_t));
            uint32_t *offset   = (uint32_t*) realloc(list->PathOffset, capacity * sizeof(uint32_t));
            if (hash   != NULL)  list->HashList    = hash;
            if (size   != NULL)  list->SizeList    = size;
            if (offset != NULL)  list->PathOffset  = offset;
            if (offset == NULL || size == NULL || hash == NULL)
            {
                return false;
            }
            list->TotalBytes  += uint32_t((capacity - list->PathCapacity) * sizeof(uint32_t) * 3);
            list->PathCapacity = uint32_t (capacity);
        }
        if (list->BlobCapacity < path_bytes)
        {
            char *blob = (char*) realloc(list->PathData, path_bytes * sizeof(char));
            if (blob  != NULL)   list->PathData = blob;
            else return false;
            list->TotalBytes  += uint32_t((path_bytes - list->BlobCapacity) * sizeof(char));
            list->BlobCapacity = uint32_t (path_bytes);
        }
        return true;
    }
    else return false;
}

void LLCALL_C io_append_file_list(io_file_list_t *list, char const *path)
{
    if (list->PathCount == list->PathCapacity)
    {
        // need to grow the list of path attributes.
        size_t new_items = grow_size(list->PathCapacity, PATH_GROW_LIMIT, list->PathCapacity + 1);
        io_ensure_file_list(list, new_items, list->BlobCapacity);
    }

    char const  *endp = NULL;
    uint32_t     hash = io_hash_path(path, &endp);
    size_t       nb   = endp  - path + 1;
    if (list->BlobCount + nb >= list->BlobCapacity)
    {
        size_t new_bytes = grow_size(list->BlobCapacity, BLOB_GROW_LIMIT, list->BlobCount + nb);
        io_ensure_file_list(list, list->PathCapacity, new_bytes);
    }

    size_t  index = list->PathCount;
    uint32_t size = uint32_t(nb-1);
    // append the basic path properties to the list:
    list->HashList  [index] = hash;
    list->SizeList  [index] = size;
    list->PathOffset[index] = list->BlobCount;
    // intern the path string data (including zero byte):
    memcpy(&list->PathData[list->BlobCount], path, nb);
    list->BlobCount += uint32_t(nb);
    list->PathCount += 1;
    if (nb > list->MaxPathBytes)
    {
        // @note: includes the zero byte.
        list->MaxPathBytes = uint32_t(nb);
    }
}

void LLCALL_C io_clear_file_list(io_file_list_t *list)
{
    list->PathCount    = 0;
    list->BlobCount    = 0;
    list->MaxPathBytes = 0;
    list->TotalBytes   = 0;
}

char const* LLCALL_C io_file_list_path(io_file_list_t const *list, size_t index)
{
    assert(index < list->PathCount);
    return &list->PathData[list->PathOffset[index]];
}

bool LLCALL_C io_search_file_list_hash(io_file_list_t const *list, uint32_t hash, size_t start, size_t *out_index)
{
    size_t   const  hash_count = list->PathCount;
    uint32_t const *hash_list  = list->HashList;
    for (size_t i = start; i < hash_count; ++i)
    {
        if (hash_list[i] == hash)
        {
            *out_index = i;
            return true;
        }
    }
    return false;
}

bool LLCALL_C io_search_file_list_path(io_file_list_t const *list, char const *path, size_t *out_index)
{
    char const *end = NULL;
    uint32_t   hash = io_hash_path(path, &end);
    return io_search_file_list_hash(list, hash, 0, out_index);
}

bool LLCALL_C io_verify_file_list(io_file_list_t const *list)
{
    size_t   const  hash_count = list->PathCount;
    uint32_t const *hash_list  = list->HashList;
    for (size_t i = 0; i < hash_count; ++i)
    {
        uint32_t path_hash = hash_list[i];
        size_t   num_hash  = 0; // number of items with hash path_hash
        for (size_t j = 0; j < hash_count; ++j)
        {
            if (path_hash == hash_list[j])
            {
                if (++num_hash > 1)
                    return false;
            }
        }
    }
    return true;
}

void LLCALL_C io_format_file_list(FILE *fp, io_file_list_t const *list)
{
    fprintf(fp, " Index | Hash     | Length | Offset | Path\n");
    fprintf(fp, "-------+----------+--------+--------+-------------------------------------------\n");
    for (size_t i = 0; i < list->PathCount; ++i)
    {
        fprintf(fp, " %5u | %08X | %6u | %6u | %s\n", 
                unsigned(i), 
                list->HashList[i], 
                list->SizeList[i], 
                list->PathOffset[i], 
                io_file_list_path(list, i));
    }
    fprintf(fp, "\n");
}

