/**
 * Copyright, Philip Meulengracht
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
 *
 * The part that DMA buffers and DMA mappings share. Both hold a kernel handle
 * and the list of device segments, and both are released the same way.
 */

#include <ddk/utils.h>
#include <os/shm.h>
#include <internal/_shm.h>
#include <os/types/syscall.h>
#include <stdlib.h>

#include "private.h"

oserr_t
__DMARelease(
    _In_ struct __DMAResource* resource)
{
    oserr_t status;

    // Unmap first, so the storage cannot go away while the device mapping
    // still exists. Each step that succeeded is recorded before the next one
    // runs. A second call then neither unmaps twice nor loses the SHM
    // attachment when the detach failed.
    if (resource->Handle != UUID_INVALID) {
        status = Syscall_ResourceRelease(resource->Handle);
        if (status != OS_EOK) {
            WARNING("__DMARelease: mapping %u release failed (%u), the resource is kept", resource->Handle, status);
            return status;
        }
        resource->Handle = UUID_INVALID;
    }

    if (resource->Storage != NULL) {
        status = Syscall_SHMDetach(resource->Storage);
        if (status != OS_EOK) {
            WARNING("__DMARelease: buffer %u detach failed (%u), the resource is kept",
                    resource->Storage->ID, status);
            return status;
        }
    }

    atomic_fetch_sub(&resource->Context->Children, 1);
    free(resource->Segments);
    free(resource);
    return OS_EOK;
}

oserr_t
__DMACollect(
    _In_ struct DMAContext* context)
{
    struct __DMAResource* resource;
    struct __DMAResource* next;
    oserr_t               status;

    // Only failed creations end up on this list, so transfers never walk it.
    // Save the next link before the release, which frees the resource.
    while (context->Pending != NULL) {
        resource = context->Pending;
        next = resource->Next;

        status = __DMARelease(resource);
        if (status != OS_EOK) {
            return status;
        }
        context->Pending = next;
    }
    return OS_EOK;
}

oserr_t
__DMAPrepareResource(
    _In_ struct __DMAResource*            resource,
    _In_ struct DMAContext*               context,
    _In_ const struct OSDmaMappingResult* result)
{
    uint32_t count = 0;
    size_t   bytes;
    oserr_t  status;

    resource->Context = context;
    resource->Handle = result->Handle;
    resource->SegmentCount = result->SegmentCount;
    resource->InlineSegment = result->InlineSegment;
    atomic_fetch_add(&context->Children, 1);

    // One segment already arrived with creation. This path uses no additional
    // allocations or syscalls and covers contiguous controller buffers/rings.
    if (resource->SegmentCount == 1) {
        return OS_EOK;
    }

    bytes = (size_t)resource->SegmentCount * sizeof(*resource->Segments);
    if (resource->SegmentCount == 0 || bytes / sizeof(*resource->Segments) != resource->SegmentCount) {
        status = OS_EOVERFLOW;
        goto rollback;
    }

    resource->Segments = malloc(bytes);
    if (resource->Segments == NULL) {
        status = OS_EOOM;
        goto rollback;
    }

    // Fetch the segment list once, here. The resource is only given to the
    // driver after this succeeds, so the segment accessors cannot fail. Building
    // descriptors and resubmitting periodic transfers never hit a setup error.
    status = Syscall_DmaGetSegments(
        resource->Handle,
        resource->Segments,
        resource->SegmentCount,
        &count
    );

    if (status == OS_EOK && count != resource->SegmentCount) {
        status = OS_EUNKNOWN;
    }

    if (status == OS_EOK) {
        return OS_EOK;
    }

rollback:
    // No driver has seen this mapping yet, so hardware cannot be using it. If
    // cleanup fails, keep the resource on the context so DMAClose can retry it.
    WARNING("DMA: segment setup failed (%u); rolling back mapping %u", status, resource->Handle);
    if (__DMARelease(resource) != OS_EOK) {
        __DMADeferCleanup(resource);
    }
    return status;
}

const struct DMASegment*
__DMASegments(
    _In_  const struct __DMAResource* resource,
    _Out_ uint32_t*                   countOut)
{
    // Both the inline segment and the segment array are set up before the
    // resource is handed out, and never change after that. Returning a pointer
    // into the resource avoids a copy or allocation per descriptor.
    *countOut = resource->SegmentCount;
    return resource->SegmentCount == 1 ? &resource->InlineSegment : resource->Segments;
}
