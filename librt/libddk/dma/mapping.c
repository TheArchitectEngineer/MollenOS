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
 * DMA mappings of SHM buffers. Covers the mapping calls, the transfers on a
 * streaming mapping, the uses of a persistent mapping, and DMAUnmap. Region
 * mappings are created in region.c.
 */

#include <ddk/utils.h>
#include <ddk/dma/region.h>
#include <os/shm.h>
#include <internal/_shm.h>
#include <os/types/syscall.h>
#include <stdlib.h>

#include "private.h"

_Static_assert(offsetof(struct DMAMapping, Resource) == 0, "Resource release frees the mapping");

oserr_t
__DMAMapSHM(
    _In_    struct DMAContext*               context,
    _In_    const OSHandle_t*                buffer,
    _In_    size_t                           offset,
    _In_    size_t                           length,
    _In_    enum DMADirection                direction,
    _In_    unsigned int                     flags,
    _In_    const struct DMAMapRequirements* requirements,
    _InOut_ struct DMAMapping**              mappingOut)
{
    struct OSDmaMapParameters parameters = {
        .Offset = offset,
        .Length = length,
        .Direction = direction,
        .Flags = flags
    };
    struct OSDmaMappingResult result = { 0 };
    struct DMAMapping*        mapping;
    oserr_t                   status;

    if (context == NULL || buffer == NULL || mappingOut == NULL || length == 0) {
        return OS_EINVALPARAMS;
    }

    if (buffer->Type != OSHANDLE_SHM || buffer->ID == UUID_INVALID) {
        return OS_EINVALPARAMS;
    }

    if (direction < DMAToDevice || direction > DMABidirectional) {
        return OS_EINVALPARAMS;
    }

    if (*mappingOut != NULL || context->State != DMAContextStateOpen) {
        return OS_EBUSY;
    }

    if (requirements != NULL) {
        parameters.Requirements = *requirements;
    }

    // The kernel keeps its own reference to the original storage, so mapping an
    // existing SHM range needs no extra userspace attachment, CPU mapping or RPC.
    mapping = calloc(1, sizeof(struct DMAMapping));
    if (mapping == NULL) {
        return OS_EOOM;
    }

    status = Syscall_DmaMapShm(context->Handle, buffer->ID, &parameters, &result);
    if (status != OS_EOK) {
        // A streaming request for the original pages only (DMAMapSHMDirect) is
        // refused with OS_ENOENT when the device cannot reach them, and with
        // OS_ENOTSUPPORTED when they break the cache or alignment rules. That
        // is an expected answer. DMAMapSHMChunk falls back to a bounce mapping
        // on it, so it is only traced. The kernel logs the first bounce
        // mapping of each context.
        if (flags == DMA_MAP_REQUIRE_DIRECT && (status == OS_ENOENT || status == OS_ENOTSUPPORTED)) {
            TRACE("DMAMapSHM: device %u, buffer %u, range %zu+%zu cannot use the original pages (%u)",
                context->Device, buffer->ID, offset, length, status);
        } else {
            WARNING("DMAMapSHM: device %u, buffer %u, range %zu+%zu failed (%u)",
                context->Device, buffer->ID, offset, length, status);
        }
        free(mapping);
        return status;
    }

    status = __DMAPrepareResource(&mapping->Resource, context, &result);
    if (status != OS_EOK) {
        return status;
    }

    mapping->Flags = flags;
    mapping->Direction = direction;
    mapping->Length = length;
    *mappingOut = mapping;
    return OS_EOK;
}

oserr_t
DMAMapSHM(
    _In_    struct DMAContext*               context,
    _In_    const OSHandle_t*                buffer,
    _In_    size_t                           offset,
    _In_    size_t                           length,
    _In_    enum DMADirection                direction,
    _In_    const struct DMAMapRequirements* requirements,
    _InOut_ struct DMAMapping**              mappingOut)
{
    return __DMAMapSHM(
        context,
        buffer,
        offset,
        length,
        direction,
        0,
        requirements,
        mappingOut
    );
}

oserr_t
DMAMapSHMDirect(
    _In_    struct DMAContext*               context,
    _In_    const OSHandle_t*                buffer,
    _In_    size_t                           offset,
    _In_    size_t                           length,
    _In_    enum DMADirection                direction,
    _In_    const struct DMAMapRequirements* requirements,
    _InOut_ struct DMAMapping**              mappingOut)
{
    return __DMAMapSHM(
        context,
        buffer,
        offset,
        length,
        direction,
        DMA_MAP_REQUIRE_DIRECT,
        requirements,
        mappingOut
    );
}

oserr_t
DMAMapSHMPersistent(
    _In_    struct DMAContext*               context,
    _In_    const OSHandle_t*                buffer,
    _In_    size_t                           offset,
    _In_    size_t                           length,
    _In_    enum DMADirection                direction,
    _In_    const struct DMAMapRequirements* requirements,
    _InOut_ struct DMAMapping**              mappingOut)
{
    return __DMAMapSHM(
        context,
        buffer,
        offset,
        length,
        direction,
        DMA_MAP_REQUIRE_DIRECT | DMA_MAP_PERSISTENT,
        requirements,
        mappingOut
    );
}

const struct DMASegment*
DMAMappingSegments(
    _In_  const struct DMAMapping* mapping,
    _Out_ uint32_t*                countOut)
{
    return __DMASegments(&mapping->Resource, countOut);
}

static oserr_t
__DMAWait(
    _In_ struct DMAMapping*   mapping,
    _In_ enum SHMAccessKind   kind,
    _In_ size_t               offset,
    _In_ size_t               length,
    _In_ enum SHMWaitMode     mode,
    _In_ const OSTimestamp_t* deadline)
{
    struct SHMAccessBeginParameters parameters = {
        .Kind = kind,
        .Offset = offset,
        .Length = length
    };

    return __SHMAccessWait(mapping->Resource.Handle, &parameters, mode, deadline);
}

static bool
__RangeValid(
    _In_ const struct DMAMapping* mapping,
    _In_ size_t                   offset,
    _In_ size_t                   length)
{
    return length != 0 && offset <= mapping->Length && length <= mapping->Length - offset;
}

oserr_t
DMABeginTransfer(
    _In_ struct DMAMapping*   mapping,
    _In_ size_t               offset,
    _In_ size_t               length,
    _In_ enum SHMWaitMode     mode,
    _In_ const OSTimestamp_t* deadline)
{
    oserr_t status;

    // Streaming transfer calls only apply to single-transfer mappings.
    // Persistent mappings use DMABeginUse/DMAEndUse, and region mappings use
    // DMARegionSession with DMARegionUseTryBegin/DMARegionUseEnd.
    if (mapping == NULL || (mapping->Flags & (DMA_MAP_PERSISTENT | DMA_MAP_REGIONS)) != 0) {
        return OS_EINVALPARAMS;
    }

    if (!__RangeValid(mapping, offset, length)) {
        return OS_EINVALPARAMS;
    }

    // Coherent devices go through the same call. The kernel still checks access
    // conflicts and ordering, even when no cache work or bounce copy is needed.
    status = __DMAWait(mapping, DMAAccessTransfer, offset, length, mode, deadline);

    if (status != OS_EOK) {
        WARNING("DMABeginTransfer: mapping %u failed (%u)", mapping->Resource.Handle, status);
    }
    return status;
}

oserr_t
DMAEndTransfer(
    _In_ struct DMAMapping* mapping,
    _In_ size_t             completedLength)
{
    oserr_t status;

    // Streaming transfer calls only apply to single-transfer mappings.
    // Persistent mappings use DMABeginUse/DMAEndUse, and region mappings use
    // DMARegionSession with DMARegionUseTryBegin/DMARegionUseEnd.
    if (mapping == NULL || (mapping->Flags & (DMA_MAP_PERSISTENT | DMA_MAP_REGIONS)) != 0) {
        return OS_EINVALPARAMS;
    }

    // The completed length is always counted from the start of the range given
    // to begin. Zero ends a cancelled transfer without copying stale bounce
    // bytes, and a short receive leaves the rest of the range untouched. If end
    // fails, the transfer stays active with the same range.
    status = Syscall_ShmAccessEnd(
        mapping->Resource.Handle,
        DMAAccessTransfer,
        completedLength
    );
    if (status != OS_EOK) {
        WARNING("DMAEndTransfer: mapping %u failed (%u)", mapping->Resource.Handle, status);
    }
    return status;
}

oserr_t
DMAUnmap(
    _InOut_ struct DMAMapping** mapping)
{
    oserr_t status;

    if (mapping == NULL || *mapping == NULL) {
        return OS_EOK;
    }

    // Region sessions read from this mapping's segment list, so every session
    // must be closed before the mapping can be torn down.
    if ((*mapping)->Sessions != 0) {
        return OS_EBUSY;
    }

    // Stop new region sessions from opening if releasing the mapping fails and
    // has to be retried later.
    if (((*mapping)->Flags & DMA_MAP_REGIONS) != 0) {
        (*mapping)->State = DMAMappingStateUnmapping;
    }

    status = __DMARelease(&(*mapping)->Resource);
    if (status == OS_EOK) {
        *mapping = NULL;
    }
    return status;
}

oserr_t
DMABeginUse(
    _In_    struct DMAMapping*   mapping,
    _InOut_ struct DMAUse*       use,
    _In_    enum SHMWaitMode     mode,
    _In_    const OSTimestamp_t* deadline)
{
    oserr_t status;

    if (mapping == NULL || use == NULL || (mapping->Flags & DMA_MAP_PERSISTENT) == 0) {
        return OS_EINVALPARAMS;
    }
    if (use->Mapping != NULL) {
        return OS_EBUSY;
    }

    status = __DMAWait(mapping, DMAAccessUse, 0, 0, mode, deadline);

    if (status == OS_EOK) {
        use->Mapping = mapping;
    }
    return status;
}

oserr_t
DMAEndUse(
    _InOut_ struct DMAUse* use)
{
    struct DMAMapping* mapping;
    oserr_t            status;

    if (use == NULL || use->Mapping == NULL) {
        return OS_EINVALPARAMS;
    }

    mapping = use->Mapping;

    // Leave the token active on failure. The caller can then retry, and unmap
    // keeps refusing while hardware may still be using the storage.
    status = Syscall_ShmAccessEnd(mapping->Resource.Handle, DMAAccessUse, 0);
    if (status == OS_EOK) {
        use->Mapping = NULL;
    }
    return status;
}
