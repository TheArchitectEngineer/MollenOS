/**
 * Copyright 2017, Philip Meulengracht
 *
 * This program is free software : you can redistribute it and / or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation ? , either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "internal/_file.h"

//#define __TRACE
#include <assert.h>
#include <os/notification_queue.h>
#include <os/handle.h>
#include <os/syslog.h>
#include <os/mollenos.h>
#include <ds/hashtable.h>
#include <errno.h>
#include <internal/_io.h>
#include <internal/_utils.h>
#include <io.h>
#include "private.h"
#include <stdlib.h>
#include <string.h>

// hashtable functions for g_stdioObjects
static uint64_t stdio_hash(const void* element);
static int      stdio_cmp(const void* element1, const void* element2);

static hashtable_t g_stdioObjects = { 0 };

hashtable_t* IODescriptors(void)
{
    return &g_stdioObjects;
}

extern stdio_ops_t g_fmemOps;
extern stdio_ops_t g_memstreamOps;
extern stdio_ops_t g_evtOps;
extern stdio_ops_t g_iosetOps;
extern stdio_ops_t g_fileOps;
extern stdio_ops_t g_pipeOps;
extern stdio_ops_t g_ipcOps;
extern stdio_ops_t g_netOps;

static oserr_t __null_read(stdio_handle_t*, void*, size_t, size_t*);
static oserr_t __null_write(stdio_handle_t*, const void*, size_t, size_t*);
static oserr_t __null_seek(stdio_handle_t*, int, off64_t, long long*);
static void    __null_close(stdio_handle_t*, int);

// Operations for NULL_SIGNATURE descriptors. A process that inherits no
// stdio gets stdin, stdout and stderr with this signature, and these streams
// go nowhere. Every descriptor gets a real operations table, so code that
// calls handle->Ops never has to check for a missing table. resize and ioctl
// stay NULL, and their callers report ENOTSUP like they do for other
// descriptors without them.
static stdio_ops_t g_stdioNullOps = {
    .read = __null_read,
    .write = __null_write,
    .seek = __null_seek,
    .close = __null_close
};

stdio_ops_t*
CRTSignatureOps(
        _In_ unsigned int signature)
{
    switch (signature) {
        case NULL_SIGNATURE: return &g_stdioNullOps;
        case FMEM_SIGNATURE: return &g_fmemOps;
        case MEMORYSTREAM_SIGNATURE: return &g_memstreamOps;
        case PIPE_SIGNATURE: return &g_pipeOps;
        case FILE_SIGNATURE: return &g_fileOps;
        case IPC_SIGNATURE: return &g_ipcOps;
        case EVENT_SIGNATURE: return &g_evtOps;
        case IOSET_SIGNATURE: return &g_iosetOps;
        case NET_SIGNATURE: return &g_netOps;
        default: {
            assert(0 && "unsupported io-descriptor signature");
        }
    }
    return NULL;
}

// A stream that goes nowhere has nothing to read, so a read is end of file.
static oserr_t
__null_read(
        _In_  stdio_handle_t* handle,
        _In_  void*           buffer,
        _In_  size_t          length,
        _Out_ size_t*         bytesRead)
{
    _CRT_UNUSED(handle);
    _CRT_UNUSED(buffer);
    _CRT_UNUSED(length);
    *bytesRead = 0;
    return OS_EOK;
}

// The data is dropped, but the write reports every byte as written. A write
// that failed would make printf in a service return an error, and a short
// write would make the stream code retry or mark the stream with an error.
static oserr_t
__null_write(
        _In_  stdio_handle_t* handle,
        _In_  const void*     buffer,
        _In_  size_t          length,
        _Out_ size_t*         bytesWritten)
{
    _CRT_UNUSED(handle);
    _CRT_UNUSED(buffer);
    *bytesWritten = length;
    return OS_EOK;
}

// The stream is always empty, so every position is 0. The text-mode read
// path calls seek without checking for it, which is why it is present.
static oserr_t
__null_seek(
        _In_  stdio_handle_t* handle,
        _In_  int             origin,
        _In_  off64_t         offset,
        _Out_ long long*      positionOut)
{
    _CRT_UNUSED(handle);
    _CRT_UNUSED(origin);
    _CRT_UNUSED(offset);
    *positionOut = 0;
    return OS_EOK;
}

// There is no context and no OS handle to release.
static void
__null_close(
        _In_ stdio_handle_t* handle,
        _In_ int             options)
{
    _CRT_UNUSED(handle);
    _CRT_UNUSED(options);
}

void
StdioConfigureStandardHandles(void)
{
    stdio_handle_t* stdoutHandle;
    stdio_handle_t* stdinHandle;
    stdio_handle_t* stderrHandle;
    int             status;
    // Make sure all default handles have been set for std. A process that
    // inherits no stdio gets NULL_SIGNATURE descriptors, whose operations drop
    // writes and read end of file. If the process wants to get output it must
    // reopen the stdout/stderr handles.
    stdoutHandle = stdio_handle_get(STDOUT_FILENO);
    if (!stdoutHandle) {
        status = stdio_handle_create(
                STDOUT_FILENO,
                0,
                __IO_TEXTMODE | __IO_NOINHERIT,
                NULL_SIGNATURE,
                0,
                &stdoutHandle
        );
        assert(status == 0);
    }

    stdinHandle = stdio_handle_get(STDIN_FILENO);
    if (!stdinHandle) {
        status = stdio_handle_create(
                STDIN_FILENO,
                0,
                __IO_TEXTMODE | __IO_NOINHERIT,
                NULL_SIGNATURE,
                0,
                &stdinHandle
        );
        assert(status == 0);
    }

    stderrHandle = stdio_handle_get(STDERR_FILENO);
    if (!stderrHandle) {
        status = stdio_handle_create(
                STDERR_FILENO,
                0,
                __IO_TEXTMODE | __IO_NOINHERIT,
                NULL_SIGNATURE,
                0,
                &stderrHandle
        );
        assert(status == 0);
    }

    stdio_handle_set_buffered(stdoutHandle, NULL, _IOWR, _IOLBF); // we buffer stdout as default
    stdio_handle_set_buffered(stdinHandle, NULL, _IORD, _IOLBF); // we also buffer stdint as default
    stdio_handle_set_buffered(stderrHandle, NULL, _IOWR, _IONBF);
}

// The descriptors are closed at exit in two steps. First the handles to close
// are collected while the table is enumerated, then they are closed one by
// one. A close removes its entry from g_stdioObjects, and a removal can shrink
// the table and free its storage, so no close may run from inside
// hashtable_enumerate.

/**
 * @brief Collects the handles that __close_io_descriptors closes. The handles
 * array has room for capacity entries, and count says how many are stored.
 */
struct __iod_collect_context {
    unsigned int     excludeFlags;
    stdio_handle_t** handles;
    int              capacity;
    int              count;
};

/**
 * @brief Finds the open descriptor with the lowest number above after that is
 * not excluded. handle stays NULL when there is none.
 */
struct __iod_find_context {
    unsigned int    excludeFlags;
    int             after;
    stdio_handle_t* handle;
};

// A descriptor is closed unless it carries one of the excluded flags.
static int
__should_close(
        _In_ stdio_handle_t* handle,
        _In_ unsigned int    excludeFlags)
{
    if (handle == NULL) {
        return 0;
    }
    return !(excludeFlags && (handle->XTFlags & excludeFlags));
}

// A buffered stream is closed through fclose, which frees the stream and then
// closes the descriptor. Other descriptors are closed directly. A successful
// close frees the handle, so the descriptor number is read first. A failed
// close leaves the descriptor open and is logged.
static void
__close_handle(
        _In_ stdio_handle_t* handle)
{
    int iod = handle->IOD;
    int status;

    if (handle->Stream) {
        status = fclose(handle->Stream);
    } else {
        status = close(iod);
    }

    if (status != 0) {
        WARNING("__close_io_descriptors: closing descriptor %i failed: errno %i", iod, errno);
    }
}

static void
__collect_entry(
        _In_ int         index,
        _In_ const void* element,
        _In_ void*       userContext)
{
    const struct stdio_object_entry* entry = element;
    struct __iod_collect_context*    context = userContext;
    _CRT_UNUSED(index);

    if (!__should_close(entry->handle, context->excludeFlags)) {
        return;
    }

    // The table cannot change during the enumeration, so it never holds more
    // entries than the count the list was sized from.
    assert(context->count < context->capacity);
    context->handles[context->count++] = entry->handle;
}

static void
__find_entry(
        _In_ int         index,
        _In_ const void* element,
        _In_ void*       userContext)
{
    const struct stdio_object_entry* entry = element;
    struct __iod_find_context*       context = userContext;
    _CRT_UNUSED(index);

    if (!__should_close(entry->handle, context->excludeFlags) || entry->id <= context->after) {
        return;
    }

    if (context->handle == NULL || entry->id < context->handle->IOD) {
        context->handle = entry->handle;
    }
}

// Closes the descriptors one at a time without a list. It is used when the
// list cannot be allocated. Each round looks up the lowest descriptor number
// above the one closed last, so a descriptor whose close fails and stays in
// the table is not tried again, and the loop ends after at most one round per
// descriptor.
static int
__close_io_descriptors_without_list(
        _In_ unsigned int excludeFlags)
{
    struct __iod_find_context context = {
            .excludeFlags = excludeFlags,
            .after        = -1
    };
    int filesClosed = 0;

    while (1) {
        context.handle = NULL;
        LOCK_FILES();
        hashtable_enumerate(&g_stdioObjects, __find_entry, &context);
        UNLOCK_FILES();
        if (context.handle == NULL) {
            break;
        }

        context.after = context.handle->IOD;
        __close_handle(context.handle);
        filesClosed++;
    }
    return filesClosed;
}

// Closes every descriptor that carries none of excludeFlags, and returns how
// many were closed. __CleanupSTDIO calls this twice at exit: first with
// __IO_PRIORITY excluded, then with nothing excluded to close the rest.
static int
__close_io_descriptors(
        _In_ unsigned int excludeFlags)
{
    struct __iod_collect_context context = {
        .excludeFlags = excludeFlags,
        .handles      = NULL,
        .capacity     = 0,
        .count        = 0
    };

    LOCK_FILES();
    if (g_stdioObjects.element_count == 0) {
        UNLOCK_FILES();
        return 0;
    }

    context.capacity = (int)g_stdioObjects.element_count;
    context.handles = malloc(sizeof(stdio_handle_t*) * g_stdioObjects.element_count);
    if (context.handles == NULL) {
        UNLOCK_FILES();
        ERROR("__close_io_descriptors: no memory for the list of %i descriptors, closing them one by one",
              context.capacity);
        return __close_io_descriptors_without_list(excludeFlags);
    }

    hashtable_enumerate(&g_stdioObjects, __collect_entry, &context);
    UNLOCK_FILES();

    // The file lock is not held across close() and fclose(), and neither of
    // them takes it. Each handle is still open here, because closing one
    // descriptor never closes another.
    for (int i = 0; i < context.count; i++) {
        __close_handle(context.handles[i]);
    }
    free(context.handles);
    return context.count;
}

static unsigned int
__oflags_to_xtflags(
        _In_ int oflags)
{
    unsigned int xtflags = 0;

    if (oflags & O_RDWR)             xtflags |= __IO_RW;
    else if (oflags & O_RDONLY)      xtflags |= __IO_READ;
    else if (oflags & O_WRONLY)      xtflags |= __IO_WRITE;
    if (oflags & O_APPEND)           xtflags |= __IO_APPEND;
    if (oflags & O_NOINHERIT)        xtflags |= __IO_NOINHERIT;

    if (oflags & O_TEXT)         xtflags |= __IO_TEXTMODE;
    else if (oflags & O_WTEXT)   xtflags |= __IO_WIDE;
    else if (oflags & O_U8TEXT)  xtflags |= __IO_UTF;
    else if (oflags & O_U16TEXT) xtflags |= __IO_UTF16;

    return xtflags;
}

static stdio_handle_t*
__descriptor_new(
        _In_  int          iod,
        _In_  int          ioFlags,
        _In_  unsigned int wxFlags,
        _In_  unsigned int signature,
        _In_  void*        opsCtx)
{
    stdio_handle_t* handle;

    handle = (stdio_handle_t*)malloc(sizeof(stdio_handle_t));
    if (!handle) {
        return NULL;
    }
    memset(handle, 0, sizeof(stdio_handle_t));

    handle->IOD     = iod;
    handle->Signature = signature;
    handle->XTFlags = __IO_OPEN | wxFlags | __oflags_to_xtflags(ioFlags);
    handle->Ops = CRTSignatureOps(signature);
    handle->OpsContext = opsCtx;
    handle->Peek[0] = '\n';
    handle->Peek[1] = '\n';
    handle->Peek[2] = '\n';
    return handle;
}

int stdio_handle_create(
        _In_  int              iod,
        _In_  int              ioFlags,
        _In_  unsigned int     xtFlags,
        _In_  unsigned int     signature,
        _In_  void*            opsCtx,
        _Out_ stdio_handle_t** handleOut)
{
    stdio_handle_t* handle;
    int             allocatedIOD;

    // the bitmap allocator handles both cases if we want to allocate a specific
    // or just the first free fd
    allocatedIOD = stdio_bitmap_allocate(iod);
    if (allocatedIOD == -1) {
        _set_errno(EMFILE);
        return -1;
    }

    handle = __descriptor_new(
            allocatedIOD,
            ioFlags,
            xtFlags,
            signature,
            opsCtx
    );
    if (handle == NULL) {
        stdio_bitmap_free(allocatedIOD);
        _set_errno(ENOMEM);
        return -1;
    }

    hashtable_set(
            &g_stdioObjects,
            &(struct stdio_object_entry) {
            .id = allocatedIOD,
            .handle = handle
        }
    );
    if (stdio_handle_get(allocatedIOD) == NULL) {
        stdio_bitmap_free(allocatedIOD);
        free(handle);
        _set_errno(ENOMEM);
        return -1;
    }
    *handleOut = handle;
    return EOK;
}

int
stdio_handle_clone(
    _In_  stdio_handle_t*  handle,
    _Out_ stdio_handle_t** handleOut)
{
    stdio_handle_t* duplicated;
    unsigned int    flags;
    oserr_t         oserr;
    int             status;

    if (handle == NULL || handleOut == NULL) {
        _set_errno(EINVAL);
        return -1;
    }

    // Copies of these descriptors share a registered handle across local
    // aliases. Other kinds keep their existing context-close policy until
    // their lifecycle is migrated.
    flags = handle->XTFlags;
    if (!handle->Shared) {
        flags |= __IO_PERSISTANT;
    }

    status = stdio_handle_create(-1, 0, flags, handle->Signature, NULL, &duplicated);
    if (status) {
        return status;
    }

    duplicated->Shared = handle->Shared;

    // The copy is registered first, so a failed reference only has to undo
    // the registration.
    if (handle->OSHandle.ID != UUID_INVALID) {
        oserr = OSHandlesAcquire(&handle->OSHandle);
        if (oserr != OS_EOK) {
            stdio_handle_delete(duplicated);
            return OsErrToErrNo(oserr);
        }
        stdio_handle_set_handle(duplicated, &handle->OSHandle);
    }

    *handleOut = duplicated;
    return 0;
}

void stdio_handle_set_shared(
    _In_ stdio_handle_t* handle)
{
    if (!handle) {
        return;
    }

    handle->Shared = true;
}

int stdio_handle_set_handle(
    _In_ stdio_handle_t* handle,
    _In_ OSHandle_t*     osHandle)
{
    if (!handle) {
        return EBADF;
    }

    memcpy(&handle->OSHandle, osHandle, sizeof(OSHandle_t));
    return EOK;
}

int stdio_handle_set_buffered(
    _In_ stdio_handle_t* handle,
    _In_ FILE*           stream,
    _In_ uint16_t        flags,
    _In_ uint8_t         bufferMode)
{
    if (!handle) {
        return EBADFD;
    }
    
    if (!stream) {
        stream = (FILE*)malloc(sizeof(FILE));
        if (!stream) {
            return -1;
        }

        // TODO move to construct function
        memset(stream, 0, sizeof(FILE));
        usched_mtx_init(&stream->Lock, USCHED_MUTEX_RECURSIVE);
    }
    
    // Reset the stream structure
    stream->IOD        = handle->IOD;
    stream->Flags      = flags;
    stream->BufferMode = bufferMode;
    stream->Base       = NULL;
    stream->Current    = NULL;
    stream->BufferSize = 0;
    stream->BytesValid = 0;
    stream->_charbuf   = 0;
    stream->_tmpfname  = NULL;
    
    // associate the stream object
    handle->Stream = stream;
    return 0;
}

void stdio_handle_delete(stdio_handle_t* handle)
{
    if (!handle) {
        return;
    }

    hashtable_remove(
        &g_stdioObjects,
        &(struct stdio_object_entry) {
            .id = handle->IOD
        }
    );
    stdio_bitmap_free(handle->IOD);
    free(handle);
}

int stdio_handle_activity(stdio_handle_t* handle , int activity)
{
    oserr_t oserr = OSNotificationQueuePost(&handle->OSHandle, activity);
    if (oserr != OS_EOK) {
        OsErrToErrNo(oserr);
        return -1;
    }
    return 0;
}

void
stdio_handle_set_flags(
        _In_ stdio_handle_t* handle,
        _In_ unsigned int    xtFlags)
{
    if (!handle) {
        return;
    }
    handle->XTFlags |= xtFlags;
}

unsigned int
stdio_handle_signature(
        _In_ stdio_handle_t* handle)
{
    if (!handle) {
        return 0;
    }
    return handle->Signature;
}

FILE*
stdio_handle_stream(
        _In_ stdio_handle_t* handle)
{
    if (!handle) {
        return NULL;
    }
    return handle->Stream;
}

int
stdio_handle_iod(
        _In_ stdio_handle_t* handle)
{
    if (!handle) {
        return -1;
    }
    return handle->IOD;
}

stdio_handle_t* stdio_handle_get(int iod)
{
    struct stdio_object_entry* result;

    result = hashtable_get(&g_stdioObjects, &(struct stdio_object_entry) { .id = iod });
    if (result != NULL) {
        return result->handle;
    }
    return NULL;
}

FILE* __get_std_handle(int n)
{
    // Only allow direct lookup of STD* descriptors
    switch (n) {
        case STDOUT_FILENO:
        case STDIN_FILENO:
        case STDERR_FILENO: {
            stdio_handle_t* handle = stdio_handle_get(n);
            if (handle != NULL) {
                return handle->Stream;
            }
        } break;
        default: {
            break;
        }
    }
    return NULL;
}

hashtable_t* stdio_get_handles(void)
{
    return &g_stdioObjects;
}

uuid_t GetNativeHandle(int iod)
{
    stdio_handle_t* handle = stdio_handle_get(iod);
    if (!handle) {
        return UUID_INVALID;
    }
    return handle->OSHandle.ID;
}

static uint64_t stdio_hash(const void* element)
{
    const struct stdio_object_entry* entry = element;
    return (uint64_t)entry->id;
}

static int stdio_cmp(const void* element1, const void* element2)
{
    const struct stdio_object_entry* entry1 = element1;
    const struct stdio_object_entry* entry2 = element2;
    return entry1->id == entry2->id ? 0 : -1;
}

void __CleanupSTDIO(void)
{
    // flush all file buffers and close handles
    io_buffer_flush_all(0xFFFFU);

    // close all handles that are not marked _PRIO, and then lastly
    // close the _PRIO handles
    __close_io_descriptors(__IO_PRIORITY);
    __close_io_descriptors(0);
}

void StdioInitialize(void)
{
    // initialize the hashtable of handles
    hashtable_construct(
        &g_stdioObjects,
        0,
        sizeof(struct stdio_object_entry),
        stdio_hash, stdio_cmp
    );

    // initialize subsystems for stdio
    stdio_bitmap_initialize();

    // register the cleanup handler
    atexit(__CleanupSTDIO);
}
