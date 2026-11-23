/**
 * MollenOS
 *
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
 */

//#define __TRACE

#include <os/types/interrupt_group.h>
#include <ddk/types/interrupt-source.h>
#include <ddk/devices/bus.h>
#include <ddk/interrupts/interrupt.h>
#include <ddk/interrupts/pci.h>
#include <ddk/utils.h>
#include <os/device.h>
#include <internal/_syscalls.h>
#include <internal/_utils.h>
#include <stdlib.h>
#include <string.h>

#include "private.h"

// Sends one request to the bus driver of the device. A platform source is
// asked through the interrupt ioctl of the platform bus, and a PCI device
// through pci.c. Both follow the same order of requests, but each bus has its
// own ioctl format. The state of the interrupt is not changed here.
static oserr_t
__SendBusRequest(
    _In_ DeviceInterrupt_t*         interrupt,
    _In_ enum __InterruptBusRequest request)
{
    struct DeviceInterruptState*        state = interrupt->State;
    struct DeviceInterruptSourceRequest source = { .Allocation = state->Handle };
    oserr_t                             status;

    if (state->PlatformSource) {
        switch (request) {
            case InterruptBusRequestReserve:
                source.Operation = DeviceInterruptSourceReserve;
                source.Index = interrupt->SourceIndex;
                break;
            case InterruptBusRequestEnable:
                source.Operation = DeviceInterruptSourceEnable;
                break;
            case InterruptBusRequestDisable:
                source.Operation = DeviceInterruptSourceDisable;
                break;
            case InterruptBusRequestRelease:
                source.Operation = DeviceInterruptSourceRelease;
                break;
            default:
                ERROR("__SendBusRequest: device %u allocation %u unknown request %u",
                      interrupt->DeviceId, state->Handle, request);
                return OS_EINVALPARAMS;
        }

        status = OSDeviceIOCtl(
            interrupt->DeviceId,
            OSIOCTLREQUEST_INTERRUPT_CONTROL,
            &source,
            sizeof(source)
        );
    } else {
        status = __InterruptPciRequest(interrupt, request);
    }

    TRACE("__SendBusRequest(device=%u, allocation=%u, request=%u, platform=%d): %u",
          interrupt->DeviceId, state->Handle, request, state->PlatformSource, status);

    // OS_EBUSY is an expected answer to a reserve request. It is returned to
    // the driver without an error line.
    if (status != OS_EOK && status != OS_EBUSY) {
        ERROR("__SendBusRequest: device %u allocation %u request %u failed: %u",
              interrupt->DeviceId, state->Handle, request, status);
    }
    return status;
}

// Reserves the interrupts with the bus driver, and for a PCI device writes
// the vectors to the device afterwards.
static oserr_t
__ReserveWithBus(
    _InOut_ DeviceInterrupt_t* interrupt)
{
    struct DeviceInterruptState* state = interrupt->State;
    oserr_t                      status;

    // Mark the reservation as held before the request is sent. If the reply
    // is lost, the bus driver may have reserved the interrupts anyway, and
    // DeviceInterruptDestroy must then still send a release request. Only
    // OS_EBUSY promises that the bus driver changed nothing.
    state->BusReservation = InterruptBusReservationHeld;

    status = __SendBusRequest(interrupt, InterruptBusRequestReserve);
    if (status != OS_EOK) {
        if (status == OS_EBUSY) {
            state->BusReservation = InterruptBusReservationNone;
        }
        return status;
    }

    if (state->PlatformSource) {
        // The platform bus translates and programs the source as part of its
        // reserve request, so there is nothing left to program from here.
        return OS_EOK;
    }
    return __InterruptPciProgram(interrupt);
}

void
DeviceInterruptInitialize(
    _Out_ DeviceInterrupt_t* interrupt,
    _In_  Device_t*          device)
{
    const BusDevice_t* bus;

    // Initialization allocates nothing. A driver without a bus device can
    // therefore fill in the interrupt line itself before it allocates.
    memset(interrupt, 0, sizeof(DeviceInterrupt_t));
    interrupt->DeviceId = UUID_INVALID;
    interrupt->Line.Line = INTERRUPT_NONE;
    interrupt->Line.Pin = INTERRUPT_NONE;
    
    if (device != NULL) {
        interrupt->DeviceId = device->Id;
        if (device->Length == sizeof(BusDevice_t)) {
            bus = (const BusDevice_t*)device;
            interrupt->IsPci = bus->Type == BusTypePci;
            interrupt->Line.Line = bus->InterruptLine;
            interrupt->Line.Pin = bus->InterruptPin;
            interrupt->Line.AcpiConform = bus->InterruptAcpiConform;
        }
    }
}

oserr_t
DeviceInterruptSelectSource(
    _InOut_ DeviceInterrupt_t* interrupt,
    _In_    uint32_t           index)
{
    if (interrupt == NULL || interrupt->IsPci) {
        return OS_EINVALPARAMS;
    }

    if (interrupt->State != NULL) {
        return OS_EBUSY;
    }

    // The index is only stored here. The platform bus checks it during
    // allocation, and nothing is reserved on the hardware before that.
    interrupt->SourceIndex = index;
    TRACE("DeviceInterruptSelectSource(device=%u, source=%u)", interrupt->DeviceId, index);
    return OS_EOK;
}

static oserr_t
__AllocateVectors(
    _InOut_ DeviceInterrupt_t* interrupt)
{
    interrupt->Vectors = calloc(interrupt->Count, sizeof(DeviceInterruptVector_t));
    if (interrupt->Vectors == NULL) {
        ERROR("__AllocateVectors: device %u failed to allocate %u vectors", interrupt->DeviceId, interrupt->Count);
        return OS_EOOM;
    }

    for (uint32_t i = 0; i < interrupt->Count; i++) {
        interrupt->Vectors[i].AffinityMask = UINT32_MAX;
        interrupt->Vectors[i].ResourceTable.HandleResource = UUID_INVALID;
    }
    return OS_EOK;
}

static oserr_t
__ReserveSourceGroup(
    _In_  DeviceInterrupt_t*      interrupt,
    _In_  uint32_t                minimum,
    _Out_ InterruptGroupResult_t* result,
    _Out_ int*                    platformSource)
{
    struct DeviceInterruptSourceQuery source = {
        .Index = interrupt->SourceIndex
    };
    
    InterruptGroupRequest_t request = {
        .Kind = InterruptSourceLine,
        .Line = interrupt->Line,
        .Flags = interrupt->Flags,
        .Minimum = 1,
        .Optimal = 1
    };
    oserr_t status;

    if (minimum != 1) {
        return OS_ENOTSUPPORTED;
    }

    if (interrupt->DeviceId != UUID_INVALID && interrupt->Line.Line == INTERRUPT_NONE) {
        // The device has no interrupt line of its own, so ask the platform
        // bus what its firmware source at SourceIndex is. The answer is either
        // a fixed line or a message (MSI) from an interrupt controller.
        status = OSDeviceIOCtl(
            interrupt->DeviceId,
            OSIOCTLREQUEST_INTERRUPT_QUERY,
            &source,
            sizeof(source)
        );
        if (status != OS_EOK) {
            WARNING("__ReserveSourceGroup: device %u source %u query failed: %u",
                    interrupt->DeviceId, interrupt->SourceIndex, status);
            return status;
        }

        if (source.Index != interrupt->SourceIndex) {
            ERROR("__ReserveSourceGroup: device %u asked for source %u, got an answer for source %u",
                  interrupt->DeviceId, interrupt->SourceIndex, source.Index);
            return OS_EPROTOCOL;
        }

        if (source.Kind != InterruptSourceFixedLine && source.Kind != InterruptSourceMessage) {
            ERROR("__ReserveSourceGroup: device %u source %u has unknown kind %u",
                  interrupt->DeviceId, interrupt->SourceIndex, source.Kind);
            return OS_EPROTOCOL;
        }

        request.Kind = source.Kind;
        request.ControllerID = source.ControllerID;
        request.Line = source.Line;
        
        *platformSource = 1;
    } else if (interrupt->SourceIndex != 0) {
        return OS_EINVALPARAMS;
    }
    return Syscall_InterruptAllocate(&request, result);
}

oserr_t
__InterruptAllocate(
    _InOut_ DeviceInterrupt_t* interrupt,
    _In_    uint32_t           minimum,
    _In_    uint32_t           optimal,
    _In_    uint32_t           pciModes,
    _Out_   uint32_t*          countOut)
{
    InterruptGroupResult_t       result;
    struct DeviceInterruptState* state;
    oserr_t                      status;
    oserr_t                      cleanupStatus;

    if (countOut == NULL) {
        return OS_EINVALPARAMS;
    }

    *countOut = 0;
    
    if (interrupt == NULL || minimum == 0 || minimum > optimal) {
        return OS_EINVALPARAMS;
    }
    
    if (interrupt->Flags & ~INTERRUPT_EXCLUSIVE) {
        return OS_EINVALPARAMS;
    }
    
    if (interrupt->State != NULL) {
        return OS_EBUSY;
    }
    TRACE("__InterruptAllocate(device=%u, source=%u, count=%u/%u, pci=%u)",
          interrupt->DeviceId, interrupt->SourceIndex, minimum, optimal, interrupt->IsPci);

    // Allocate the state before anything is reserved. If undoing a failed
    // allocation fails as well, the state is what lets the driver call
    // DeviceInterruptDestroy again to finish the cleanup. calloc leaves it in
    // the Preparing phase, with delivery stopped and no bus reservation.
    state = calloc(1, sizeof(struct DeviceInterruptState));
    if (state == NULL) {
        ERROR("__InterruptAllocate: device %u state allocation failed", interrupt->DeviceId);
        return OS_EOOM;
    }

    if (interrupt->IsPci) {
        status = __InterruptPciReserve(interrupt, minimum, optimal, pciModes, &result, &state->PciMode);
    } else {
        status = __ReserveSourceGroup(interrupt, minimum, &result, &state->PlatformSource);
    }
    if (status != OS_EOK) {
        TRACE("__InterruptAllocate: device %u reservation failed: %u", interrupt->DeviceId, status);
        free(state);
        return status;
    }
    
    state->Handle = result.Handle;
    interrupt->State = state;
    interrupt->Count = result.Count;
    interrupt->Kind = result.Kind;

    status = __AllocateVectors(interrupt);
    if (status != OS_EOK) {
        goto rollback;
    }
    
    // A legacy line that the driver or the PCI configuration supplied needs
    // nothing from a bus driver. Platform sources and messages (MSI, MSI-X)
    // must be reserved and programmed by the bus driver of the device.
    if (state->PlatformSource || result.Kind == InterruptSourceMessage) {
        status = __ReserveWithBus(interrupt);
        if (status != OS_EOK) {
            goto rollback;
        }
    }

    state->Phase = InterruptPhaseReady;

    *countOut = result.Count;
    return OS_EOK;

rollback:
    cleanupStatus = DeviceInterruptDestroy(interrupt);
    if (cleanupStatus != OS_EOK) {
        ERROR("__InterruptAllocate: device %u cleanup failed: %u (setup failed with %u), "
              "state is kept for another Destroy",
              interrupt->DeviceId, cleanupStatus, status);
    }
    return status;
}

oserr_t
DeviceInterruptAllocate(
    _InOut_ DeviceInterrupt_t* interrupt,
    _In_    uint32_t           min,
    _In_    uint32_t           optimal,
    _Out_   uint32_t*          countOut)
{
    // Drivers that call this accept whichever kind of interrupt the bus picks.
    // A PCI driver that needs MSI or MSI-X uses DeviceInterruptAllocatePci.
    return __InterruptAllocate(
        interrupt,
        min,
        optimal,
        INTERRUPT_STRATEGY_ANY,
        countOut
    );
}

oserr_t
DeviceInterruptGetMessage(
    _In_  DeviceInterrupt_t*        interrupt,
    _In_  uint32_t                  index,
    _Out_ DeviceInterruptMessage_t* messageOut)
{
    DeviceInterruptMessage_t message;
    oserr_t                  status;

    if (interrupt == NULL || interrupt->State == NULL || messageOut == NULL) {
        return OS_EINVALPARAMS;
    }

    if (index >= interrupt->Count) {
        return OS_EINVALPARAMS;
    }

    if (interrupt->State->Phase != InterruptPhaseReady) {
        return OS_EBUSY;
    }

    status = Syscall_InterruptGetMessage(interrupt->State->Handle, index, &message);
    if (status == OS_EOK) {
        *messageOut = message;
    }
    return status;
}

// Returns the vector when its handler and resources may be changed. That is
// only the case while delivery is fully stopped, because a bound handler in
// the kernel may be using them.
static oserr_t
__GetEditableVector(
    _In_  DeviceInterrupt_t*        interrupt,
    _In_  uint32_t                  index,
    _Out_ DeviceInterruptVector_t** vectorOut)
{
    if (interrupt == NULL || interrupt->State == NULL || index >= interrupt->Count) {
        return OS_EINVALPARAMS;
    }

    // StopPending has no handler bound in the kernel, but the bus driver may
    // still deliver. Changes have to wait until that stop has finished too.
    if (interrupt->State->Phase != InterruptPhaseReady ||
        interrupt->State->Delivery != InterruptDeliveryStopped) {
        return OS_EBUSY;
    }

    *vectorOut = &interrupt->Vectors[index];
    return OS_EOK;
}

oserr_t
DeviceInterruptSetAffinity(
    _InOut_ DeviceInterrupt_t* interrupt,
    _In_    uint32_t           index,
    _In_    uint32_t           affinityMask)
{
    DeviceInterruptVector_t* vector;
    oserr_t                  status;

    // Only the mask that allows every CPU is accepted. Routing is left to the
    // platform, so a narrower mask would be stored without being applied.
    status = __GetEditableVector(interrupt, index, &vector);
    if (status != OS_EOK) {
        return status;
    }

    if (affinityMask == 0) {
        return OS_EINVALPARAMS;
    }

    if (affinityMask != UINT32_MAX) {
        return OS_ENOTSUPPORTED;
    }
    
    vector->AffinityMask = affinityMask;
    return OS_EOK;
}

oserr_t
DeviceInterruptSetHandler(
    _InOut_ DeviceInterrupt_t* interrupt,
    _In_    uint32_t           index,
    _In_    InterruptHandler_t handler)
{
    DeviceInterruptVector_t* vector;
    oserr_t                  status;

    // The handler is only stored here. It is bound in the kernel by Enable,
    // once the driver has described all of its resources.
    status = __GetEditableVector(interrupt, index, &vector);
    if (status != OS_EOK) {
        return status;
    }

    if (handler == NULL) {
        return OS_EINVALPARAMS;
    }
    
    vector->ResourceTable.Handler = handler;
    return OS_EOK;
}

oserr_t
DeviceInterruptSetDescriptor(
    _InOut_ DeviceInterrupt_t* interrupt,
    _In_    uint32_t           index,
    _In_    int                descriptor)
{
    DeviceInterruptVector_t* vector;
    uuid_t                   handle;
    oserr_t                  status;

    // The kernel signals an OS handle, so the file descriptor of the driver
    // is translated to its handle here.
    status = __GetEditableVector(interrupt, index, &vector);
    if (status != OS_EOK) {
        return status;
    }

    if (descriptor < 0) {
        return OS_EINVALPARAMS;
    }
    
    handle = GetNativeHandle(descriptor);
    if (handle == UUID_INVALID) {
        return OS_EINVALPARAMS;
    }
    
    vector->ResourceTable.HandleResource = handle;
    return OS_EOK;
}

oserr_t
DeviceInterruptAddIoResource(
    _InOut_ DeviceInterrupt_t* interrupt,
    _In_    uint32_t           index,
    _In_    DeviceIo_t*        ioSpace)
{
    DeviceInterruptVector_t* vector;
    oserr_t                  status;

    // Resources are appended and never moved, because fast interrupt handlers
    // refer to them by their index in the table.
    status = __GetEditableVector(interrupt, index, &vector);
    if (status != OS_EOK) {
        return status;
    }

    if (ioSpace == NULL) {
        return OS_EINVALPARAMS;
    }
    
    for (unsigned int i = 0; i < INTERRUPT_MAX_IO_RESOURCES; i++) {
        if (vector->ResourceTable.IoResources[i] == NULL) {
            vector->ResourceTable.IoResources[i] = ioSpace;
            return OS_EOK;
        }
    }
    return OS_EOOM;
}

oserr_t
DeviceInterruptAddMemoryResource(
    _InOut_ DeviceInterrupt_t* interrupt,
    _In_    uint32_t           index,
    _In_    uintptr_t          address,
    _In_    size_t             length,
    _In_    unsigned int       flags)
{
    DeviceInterruptVector_t*       vector;
    FastInterruptMemoryResource_t* resource;
    oserr_t                        status;

    // An empty range, or one that wraps around the end of the address space,
    // is refused here instead of being passed on to the kernel.
    status = __GetEditableVector(interrupt, index, &vector);
    if (status != OS_EOK) {
        return status;
    }

    if (address == 0 || length == 0) {
        return OS_EINVALPARAMS;
    }

    if (length > UINTPTR_MAX - address || (flags & ~INTERRUPT_RESOURCE_DISABLE_CACHE)) {
        return OS_EINVALPARAMS;
    }
    
    for (unsigned int i = 0; i < INTERRUPT_MAX_MEMORY_RESOURCES; i++) {
        resource = &vector->ResourceTable.MemoryResources[i];
        if (resource->Address == 0) {
            resource->Address = address;
            resource->Length = length;
            resource->Flags = flags;
            return OS_EOK;
        }
    }
    return OS_EOOM;
}

// Records that a stop has started. From here on Enable must not trust an
// earlier confirmed delivery, and has to finish the stop before it binds again.
static void
__BeginStop(
    _InOut_ struct DeviceInterruptState* state)
{
    if (state->Delivery == InterruptDeliveryStopped) {
        state->Delivery = InterruptDeliveryStopPending;
    } else if (state->Delivery == InterruptDeliveryEnabled) {
        state->Delivery = InterruptDeliveryKernelEnabled;
    }
}

// Disables delivery in the kernel and removes the bound handlers. The caller
// has either stopped the bus driver already, or never enabled it. This is the
// only place that returns delivery to Stopped, and only after the kernel has
// confirmed that the handlers have finished and are unbound.
static oserr_t
__StopKernel(
    _InOut_ struct DeviceInterruptState* state)
{
    oserr_t status;

    __BeginStop(state);
    if (state->Delivery != InterruptDeliveryStopPending) {
        // Handlers are bound, or may be after a failed bind.
        status = Syscall_InterruptDisable(state->Handle);
        if (status != OS_EOK) {
            ERROR("__StopKernel: allocation %u stop failed: %u, cleanup is still needed",
                  state->Handle, status);
            return status;
        }
    }

    state->Delivery = InterruptDeliveryStopped;
    return OS_EOK;
}

oserr_t
DeviceInterruptEnable(
    _InOut_ DeviceInterrupt_t* interrupt)
{
    InterruptBindRequest_t       request;
    struct DeviceInterruptState* state;
    oserr_t                      status;
    oserr_t                      cleanupStatus;

    TRACE("DeviceInterruptEnable(interrupt=%p)", interrupt);
    if (interrupt == NULL || interrupt->State == NULL) {
        WARNING("DeviceInterruptEnable: missing interrupt or allocation");
        return OS_EINVALPARAMS;
    }

    state = interrupt->State;
    if (state->Phase != InterruptPhaseReady) {
        TRACE("DeviceInterruptEnable: allocation %u phase %u is not ready",
              state->Handle, state->Phase);
        return OS_EBUSY;
    }

    // An earlier bind, stop or bus request may have failed half way. Finish
    // that stop first, so binding always starts from the Stopped state.
    if (state->Delivery != InterruptDeliveryStopped && state->Delivery != InterruptDeliveryEnabled) {
        status = DeviceInterruptDisable(interrupt);
        if (status != OS_EOK) {
            return status;
        }
    }

    if (state->Delivery == InterruptDeliveryEnabled) {
        return OS_EOK;
    }

    for (uint32_t i = 0; i < interrupt->Count; i++) {
        if (interrupt->Vectors[i].ResourceTable.Handler == NULL) {
            WARNING("DeviceInterruptEnable: allocation %u index %u has no handler", state->Handle, i);
            return OS_EINVALPARAMS;
        }
    }

    // Move to Bound before the first bind call. Even a bind that fails may
    // leave handlers mapped, so the kernel stop is needed from here on.
    state->Delivery = InterruptDeliveryBound;
    for (uint32_t i = 0; i < interrupt->Count; i++) {
        request.Index = i;
        request.Resources = interrupt->Vectors[i].ResourceTable;

        status = Syscall_InterruptBind(state->Handle, &request);
        if (status != OS_EOK) {
            ERROR("DeviceInterruptEnable: allocation %u bind index %u failed: %u",
                  state->Handle, i, status);
            goto stopKernel;
        }
    }

    status = Syscall_InterruptEnable(state->Handle);
    if (status != OS_EOK) {
        ERROR("DeviceInterruptEnable: allocation %u kernel enable failed: %u", state->Handle, status);
        goto stopKernel;
    }

    state->Delivery = InterruptDeliveryKernelEnabled;

    if (state->BusReservation == InterruptBusReservationHeld) {
        status = __SendBusRequest(interrupt, InterruptBusRequestEnable);
        if (status != OS_EOK) {
            // The bus driver may have enabled the device even though the reply
            // was lost. Disable stops the bus driver first and only then
            // removes the handlers, so no interrupt arrives without a handler.
            cleanupStatus = DeviceInterruptDisable(interrupt);
            if (cleanupStatus != OS_EOK) {
                ERROR("DeviceInterruptEnable: allocation %u stop after a failed enable failed: %u",
                      state->Handle, cleanupStatus);
            }
            return status;
        }
    }

    // The kernel and the bus driver have both confirmed delivery, so a second
    // Enable can return right away.
    state->Delivery = InterruptDeliveryEnabled;
    if (interrupt->IsPci) {
        NOTICE("PCI IRQ enabled: device=%u allocation=%u mode=0x%x count=%u",
               interrupt->DeviceId, state->Handle, state->PciMode, interrupt->Count);
    }
    return OS_EOK;

stopKernel:
    // The bus driver was never enabled, so only the kernel has to be stopped.
    // The first error is the one the driver needs. If the stop fails as well,
    // delivery stays in a state that makes the next Enable or Disable finish it.
    cleanupStatus = __StopKernel(state);
    if (cleanupStatus != OS_EOK) {
        ERROR("DeviceInterruptEnable: allocation %u stop after a failed enable failed: %u",
              state->Handle, cleanupStatus);
    }
    return status;
}

oserr_t
DeviceInterruptDisable(
    _InOut_ DeviceInterrupt_t* interrupt)
{
    struct DeviceInterruptState* state;
    oserr_t                      status;

    TRACE("DeviceInterruptDisable(interrupt=%p)", interrupt);
    if (interrupt == NULL) {
        WARNING("DeviceInterruptDisable: missing interrupt");
        return OS_EINVALPARAMS;
    }

    state = interrupt->State;
    if (state == NULL) {
        return OS_EOK;
    }

    // Record the stop before the first request. If the bus driver fails, the
    // interrupt must not look fully enabled or fully stopped afterwards. This
    // holds for every kind of interrupt, also lines without a bus driver.
    __BeginStop(state);
    if (state->BusReservation == InterruptBusReservationHeld) {
        status = __SendBusRequest(interrupt, InterruptBusRequestDisable);
        if (status != OS_EOK) {
            return status;
        }
    }
    return __StopKernel(state);
}

oserr_t
DeviceInterruptDestroy(
    _InOut_ DeviceInterrupt_t* interrupt)
{
    oserr_t status;

    TRACE("DeviceInterruptDestroy(interrupt=%p)", interrupt);
    if (interrupt != NULL && interrupt->State != NULL) {
        // Destroy can not be undone. When it fails half way, parts of the
        // kernel or bus reservation may already be gone, so the interrupt can
        // only be destroyed again. A plain Disable does not change the phase.
        interrupt->State->Phase = InterruptPhaseDestroying;
    }

    // On any failure below the handle and the state are kept, so the driver
    // can call Destroy again.
    status = DeviceInterruptDisable(interrupt);
    if (status != OS_EOK) {
        return status;
    }

    if (interrupt->State == NULL) {
        return OS_EOK;
    }
    
    // The bus driver gives its reservation back before the kernel does, as
    // the bus reservation refers to the kernel allocation.
    if (interrupt->State->BusReservation == InterruptBusReservationHeld) {
        status = __SendBusRequest(interrupt, InterruptBusRequestRelease);
        if (status != OS_EOK) {
            return status;
        }
        interrupt->State->BusReservation = InterruptBusReservationNone;
    }

    status = Syscall_InterruptRelease(interrupt->State->Handle);
    if (status != OS_EOK) {
        ERROR("DeviceInterruptDestroy: Syscall_InterruptRelease allocation %u failed: %u, state is kept",
              interrupt->State->Handle, status);
        return status;
    }


    free(interrupt->Vectors);
    free(interrupt->State);
    interrupt->State = NULL;
    interrupt->Vectors = NULL;
    interrupt->Count = 0;
    interrupt->Kind = InterruptSourceInvalid;
    return OS_EOK;
}
