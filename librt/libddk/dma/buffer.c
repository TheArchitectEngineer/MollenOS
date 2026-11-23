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
 * DMA buffers. Memory that DMAAllocate gets for the device and that stays
 * shared between the driver and the device until DMAFree.
 */

#include <ddk/utils.h>
#include <os/shm.h>
#include <internal/_shm.h>
#include <os/types/syscall.h>
#include <stdlib.h>

#include "private.h"

struct DMABuffer {
    struct __DmaResource Resource;
    SHMHandle_t          Storage;
};

_Static_assert(offsetof(struct DMABuffer, Resource) == 0, "Resource release frees the buffer");

oserr_t
DMAAllocate(
    _In_    struct DMAContext*            context,
    _In_    const struct DMARequirements* requirements,
    _InOut_ struct DMABuffer**            bufferOut)
{
    struct OSDmaAllocationResult result = { 0 };
    struct DMABuffer*            buffer;
    oserr_t                      status;

    if (context == NULL || requirements == NULL || bufferOut == NULL) {
        return OS_EINVALPARAMS;
    }

    if (*bufferOut != NULL || context->State != DmaContextStateOpen) {
        return OS_EBUSY;
    }

    // Allocate the local buffer object before entering the kernel. The SHM
    // attachment is embedded directly because this memory is never shared on
    // its own. Wrapping it in an OSHandle would cost another heap allocation
    // and registry entry.
    buffer = calloc(1, sizeof(struct DMABuffer));
    if (buffer == NULL) {
        return OS_EOOM;
    }

    status = Syscall_DmaAllocate(context->Handle, requirements, &result);
    if (status != OS_EOK) {
        WARNING("DMAAllocate: device %u, %zu bytes failed (%u)", context->Device, requirements->Length, status);
        free(buffer);
        return status;
    }

    buffer->Storage = result.Buffer;
    buffer->Resource.Storage = &buffer->Storage;

    status = __DmaPrepareResource(&buffer->Resource, context, &result.Mapping);
    if (status != OS_EOK) {
        return status;
    }

    TRACE("DMAAllocate: device %u, %zu bytes, %u segments",
        context->Device, buffer->Storage.Length, buffer->Resource.SegmentCount);
    *bufferOut = buffer;
    return OS_EOK;
}

void*
DMABufferData(
    _In_ const struct DMABuffer* buffer)
{
    return buffer->Storage.Buffer;
}

size_t
DMABufferSize(
    _In_ const struct DMABuffer* buffer)
{
    return buffer->Storage.Length;
}

const struct DMASegment*
DMABufferSegments(
    _In_  const struct DMABuffer* buffer,
    _Out_ uint32_t*               countOut)
{
    return __DmaSegments(&buffer->Resource, countOut);
}

oserr_t
DMAFree(
    _InOut_ struct DMABuffer** buffer)
{
    oserr_t status;

    if (buffer == NULL || *buffer == NULL) {
        return OS_EOK;
    }

    // Hardware quiescence is the driver's responsibility. Unlike streaming
    // mappings this memory is continuously shared and has no handoff state.
    status = __DmaRelease(&(*buffer)->Resource);
    if (status == OS_EOK) {
        *buffer = NULL;
    }
    return status;
}
