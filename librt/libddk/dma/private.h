/**
 * Copyright 2023, Philip Meulengracht
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

#ifndef __DDK_DMA_PRIVATE_H__
#define __DDK_DMA_PRIVATE_H__

#include <ddk/dma.h>
#include <os/types/shm.h>
#include <os/types/dma_attachment.h>
#include <os/types/syscall.h>
#include <stdatomic.h>

// A buffer or a mapping created from a context. Its device segment list is kept
// here so the accessors can return it without asking the kernel again.
struct __DmaResource {
    // Next resource in the context's Pending list. Only used while on that list.
    struct __DmaResource* Next;
    // The context this resource was created from.
    struct DMAContext*    Context;
    // Kernel handle of the device mapping.
    uuid_t                Handle;
    uint32_t              SegmentCount;
    // Holds the first segment. A single segment list needs no allocation.
    struct DMASegment     InlineSegment;
    // Full segment list when there is more than one segment, otherwise NULL.
    struct DMASegment*    Segments;
    // SHM attachment that DMAAllocate made for the buffer. NULL for mappings,
    // because the caller already has its own attachment to the SHM buffer they map.
    SHMHandle_t*          Storage;
};

// Where a mapping is in its life. Only region mappings ever leave Mapped
// before they are freed, because only their unmap can be blocked half way.
enum __DmaMappingState {
    // The mapping can be used, and region sessions can be opened on it.
    DmaMappingStateMapped,
    // DMAUnmap has started on a region mapping. If releasing it fails, the
    // mapping stays in this state and no new region session can be opened
    // until the unmap is tried again and succeeds.
    DmaMappingStateUnmapping
};

// Tracks a DMA mapping created by DMAMapSHM, DMAMapSHMDirect, DMAMapSHMPersistent,
// or DMAMapSHMRegions. Setup and teardown calls on the mapping must be serialized.
// Individual region sessions serialize their own transfers independently.
struct DMAMapping {
    // Kernel handle and cached device segments shared with DMABuffer.
    struct __DmaResource   Resource;
    // DMA_MAP_* flags fixed when the mapping was created.
    unsigned int           Flags;
    // Mapped byte count, used to check streaming transfer ranges.
    size_t                 Length;
    // Widest data direction allowed for transfers on this mapping.
    enum DMADirection      Direction;
    // Number of open DMARegionSession_t objects referencing this mapping.
    // DMAUnmap refuses to run while any region session is still open.
    unsigned int           Sessions;
    // See enum __DmaMappingState.
    enum __DmaMappingState State;
};

// Where a context is in its life. A context only moves forward through these
// states, and is freed once DMAClose has released everything.
enum __DmaContextState {
    // The open succeeded. Buffers and mappings can be created.
    DmaContextStateOpen,
    // The reply to the binding request was lost during open, so it is unknown
    // whether the device service granted the binding. The context can not
    // create buffers or mappings. DMAClose sends the same request again to
    // learn the outcome before it releases anything.
    DmaContextStateBindingUnknown,
    // DMAClose has started, or the open failed and only cleanup is left. No
    // new buffers or mappings can be created.
    DmaContextStateClosing
};

// The driver's DMA access to one device. At most one context exists per device
// in this process. All contexts are linked in a registry guarded by a lock.
struct DMAContext {
    struct DMAContext*     Next;
    uuid_t                 Device;
    // Kernel handle of the open context. UUID_INVALID until the open succeeds.
    uuid_t                 Handle;
    // Identifies the binding request. If the reply is lost, the same ID is sent
    // again so the service returns the same binding instead of making a new one.
    uint64_t               RequestId;
    // Permission the device service gave this process to open a context.
    OSDmaBindingResult_t   Binding;
    // See enum __DmaContextState.
    enum __DmaContextState State;
    // Number of buffers and mappings from this context that are not released yet.
    // Resources waiting in Pending are included.
    atomic_uint            Children;
    // Resources whose creation failed and whose cleanup must be retried.
    struct __DmaResource*  Pending;
};

/**
 * @brief Fills in a new buffer or mapping from the result of its creation
 * syscall, and fetches the full segment list when there is more than one
 * segment. Implemented in resource.c.
 *
 * On failure the kernel mapping is released again and the resource is freed.
 * If that release fails as well, the resource is handed to __DmaDeferCleanup.
 * The caller must not touch the resource after a failure.
 *
 * @param resource Zeroed resource at the start of a DMABuffer or DMAMapping.
 *                 Storage must already be set for a buffer.
 * @param context  Context the resource was created from.
 * @param result   Handle and first segment returned by the kernel.
 * @return OS_EOK on success, OS_EOVERFLOW or OS_EOOM when the segment list
 *         cannot be allocated, or an error from the kernel.
 */
oserr_t
__DmaPrepareResource(
    _In_ struct __DmaResource*            resource,
    _In_ struct DMAContext*               context,
    _In_ const struct OSDmaMappingResult* result);

/**
 * @brief Releases the kernel mapping of a resource, detaches its SHM storage
 * if it has one, and frees the resource. Implemented in resource.c.
 *
 * Each step is recorded once it has succeeded. When a step fails the resource
 * is kept, and a second call continues with the step that failed.
 *
 * @param resource Resource at the start of a DMABuffer or DMAMapping.
 * @return OS_EOK when the resource is freed, or the error of the failed step.
 */
oserr_t
__DmaRelease(
    _In_ struct __DmaResource* resource);

/**
 * @brief Returns the device segments of a resource. The list belongs to the
 * resource and stays valid until the resource is released. Implemented in
 * resource.c.
 *
 * @param resource Resource that __DmaPrepareResource has filled in.
 * @param countOut Receives the number of segments.
 * @return The first segment of the list.
 */
const struct DMASegment*
__DmaSegments(
    _In_  const struct __DmaResource* resource,
    _Out_ uint32_t*                   countOut);

/**
 * @brief Shared implementation of every DMAMapSHM call. Implemented in
 * mapping.c, and also used by DMAMapSHMRegions in region.c.
 *
 * @param context      Open context of the device.
 * @param buffer       SHM buffer to map.
 * @param offset       Byte offset of the range inside the buffer.
 * @param length       Length of the range in bytes. Must not be zero.
 * @param direction    Widest data direction the mapping allows.
 * @param flags        DMA_MAP_* flags that select the kind of mapping.
 * @param requirements Optional limits of the device, or NULL.
 * @param mappingOut   Receives the mapping. Must point to NULL on entry.
 * @return OS_EOK on success, OS_EINVALPARAMS for a bad argument, OS_EBUSY when
 *         mappingOut is in use or the context is not open, or an error from
 *         the kernel.
 */
oserr_t
__DmaMapSHM(
    _In_    struct DMAContext*               context,
    _In_    const OSHandle_t*                buffer,
    _In_    size_t                           offset,
    _In_    size_t                           length,
    _In_    enum DMADirection                direction,
    _In_    unsigned int                     flags,
    _In_    const struct DMAMapRequirements* requirements,
    _InOut_ struct DMAMapping**              mappingOut);

/**
 * @brief Saves a resource whose creation failed and whose cleanup failed too.
 * Implemented in context.c, which holds the lock of the context registry.
 * DMAClose retries the cleanup later. Without this the kernel handle or the SHM
 * attachment would be lost.
 *
 * @param resource Resource that was never returned to the driver. The context takes
 * it over and frees it once cleanup succeeds.
 */
void
__DmaDeferCleanup(
    _In_ struct __DmaResource* resource);

/**
 * @brief Retries the cleanup of every resource saved by __DmaDeferCleanup.
 * Implemented in resource.c.
 * The context registry lock must be held.
 *
 * @param context Context to clean up. The caller must make sure nothing else is
 * creating resources on it or closing it.
 * @return OS_EOK, or the first cleanup error. Resources that still fail stay
 * on the list for the next try.
 */
oserr_t
__DmaCollect(
    _In_ struct DMAContext* context);

#endif //!__DDK_DMA_PRIVATE_H__
