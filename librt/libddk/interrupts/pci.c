/**
 * Copyright 2026, Philip Meulengracht
 *
 * This program is free software : you can redistribute it and / or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
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

#include <ddk/interrupts/pci.h>
#include <ddk/utils.h>
#include <internal/_syscalls.h>
#include <os/device.h>
#include <string.h>

#include "private.h"

oserr_t
__InterruptPciReserve(
    _In_  DeviceInterrupt_t*      interrupt,
    _In_  uint32_t                minimum,
    _In_  uint32_t                optimal,
    _In_  uint32_t                modes,
    _Out_ InterruptGroupResult_t* result,
    _Out_ uint32_t*               selectedMode)
{
    const uint32_t preference[] = {
        INTERRUPT_STRATEGY_MSIX,
        INTERRUPT_STRATEGY_MSI,
        INTERRUPT_STRATEGY_INTx
    };

    struct PciInterruptSupport support = { 0 };
    InterruptGroupRequest_t    request;
    oserr_t                    status = OS_ENOTSUPPORTED;

    if (modes == 0 || (modes & ~INTERRUPT_STRATEGY_ANY)) {
        return OS_EINVALPARAMS;
    }

    if (modes & (INTERRUPT_STRATEGY_MSI | INTERRUPT_STRATEGY_MSIX)) {
        status = OSDeviceIOCtl(
            interrupt->DeviceId,
            OSIOCTLREQUEST_PCI_INTERRUPT_QUERY,
            &support,
            sizeof(support)
        );
        if (status != OS_EOK && status != OS_ENOTSUPPORTED) {
            ERROR("__InterruptPciReserve: device %u capability query failed: %u", interrupt->DeviceId, status);
            return status;
        }
        if (status == OS_ENOTSUPPORTED) {
            memset(&support, 0, sizeof(support));
        }
    }
    
    modes &= support.Strategies | INTERRUPT_STRATEGY_INTx;

    // Try the modes from best to worst and keep the first one the kernel can
    // reserve. One MSI message and one MSI-X vector are the same thing to the
    // kernel, and only differ in how the device is programmed later. Only
    // OS_ENOTSUPPORTED and OS_EBUSY move on to the next mode. Any other error
    // is returned, so a real failure is never hidden by falling back to INTx.
    status = OS_ENOTSUPPORTED;
    for (uint32_t i = 0; i < sizeof(preference) / sizeof(preference[0]); i++) {
        uint32_t capacity;
        uint32_t mode = preference[i];
        
        if (!(modes & mode)) {
            continue;
        }

        capacity = mode == INTERRUPT_STRATEGY_MSIX ? support.MsixCapacity : 1;
        if (capacity < minimum) {
            continue;
        }
        
        memset(&request, 0, sizeof(request));
        request.Flags = interrupt->Flags;
        request.Minimum = minimum;
        request.Optimal = optimal < capacity ? optimal : capacity;
        
        if (mode == INTERRUPT_STRATEGY_INTx) {
            request.Kind = InterruptSourceLine;
            request.Line = interrupt->Line;
        } else {
            request.Kind = InterruptSourceMessage;
            request.ControllerID = support.ControllerID;
        }
        
        status = Syscall_InterruptAllocate(&request, result);
        TRACE("__InterruptPciReserve: device %u mode 0x%x reservation: %u", interrupt->DeviceId, mode, status);
        if (status == OS_EOK) {
            *selectedMode = mode;
            NOTICE("PCI IRQ selected: device=%u mode=%s allocation=%u count=%u controller=%u INTx-line=%i",
                   interrupt->DeviceId,
                   mode == INTERRUPT_STRATEGY_MSIX ? "MSI-X" :
                       (mode == INTERRUPT_STRATEGY_MSI ? "MSI" : "INTx"),
                   result->Handle, result->Count, request.ControllerID, interrupt->Line.Line);
            return OS_EOK;
        }
        
        if (status != OS_ENOTSUPPORTED && status != OS_EBUSY) {
            ERROR("__InterruptPciReserve: device %u mode 0x%x reservation failed: %u",
                  interrupt->DeviceId, mode, status);
            return status;
        }
    }
    return status;
}

oserr_t
__InterruptPciRequest(
    _In_ DeviceInterrupt_t*         interrupt,
    _In_ enum __InterruptBusRequest request)
{
    struct PciInterruptRequest pciRequest = {
        .Allocation = interrupt->State->Handle,
        .Strategy = interrupt->State->PciMode
    };

    // Translate the request into the operation codes of the PCI interrupt
    // ioctl. interrupt.c has already recorded the state change it expects, and
    // logs a failed request.
    switch (request) {
        case InterruptBusRequestReserve:
            pciRequest.Operation = PciInterruptReserve;
            pciRequest.Count = interrupt->Count;
            break;
        case InterruptBusRequestEnable:
            pciRequest.Operation = PciInterruptEnable;
            break;
        case InterruptBusRequestDisable:
            pciRequest.Operation = PciInterruptDisable;
            break;
        case InterruptBusRequestRelease:
            pciRequest.Operation = PciInterruptRelease;
            break;
        default:
            return OS_EINVALPARAMS;
    }

    return OSDeviceIOCtl(
        interrupt->DeviceId,
        OSIOCTLREQUEST_PCI_INTERRUPT,
        &pciRequest,
        sizeof(pciRequest)
    );
}

oserr_t
__InterruptPciProgram(
    _In_ DeviceInterrupt_t* interrupt)
{
    struct PciInterruptRequest request = {
        .Operation = PciInterruptProgram,
        .Allocation = interrupt->State->Handle,
        .Strategy = interrupt->State->PciMode
    };

    DeviceInterruptMessage_t     message;
    struct DeviceInterruptState* state = interrupt->State;
    oserr_t                      status;

    // Ask the kernel for the address and data of each vector and have the PCI
    // bus write them to the device. If a write fails or its reply is lost, the
    // reservation with the PCI bus is still held. The caller releases it
    // through DeviceInterruptDestroy.
    for (uint32_t i = 0; i < interrupt->Count; i++) {
        status = Syscall_InterruptGetMessage(state->Handle, i, &message);
        if (status != OS_EOK) {
            ERROR("__InterruptPciProgram: allocation %u message %u query failed: %u", state->Handle, i, status);
            return status;
        }

        request.Index = i;
        request.AddressLow = message.AddressLow;
        request.AddressHigh = message.AddressHigh;
        request.Data = message.Data;
        
        status = OSDeviceIOCtl(
            interrupt->DeviceId,
            OSIOCTLREQUEST_PCI_INTERRUPT,
            &request,
            sizeof(request)
        );
        if (status != OS_EOK) {
            ERROR("__InterruptPciProgram: device %u allocation %u PCI vector %u programming failed: %u",
                  interrupt->DeviceId, state->Handle, i, status);
            return status;
        }
        NOTICE("PCI IRQ programmed: device=%u allocation=%u vector=%u address=0x%08x%08x data=0x%08x",
               interrupt->DeviceId, state->Handle, i, message.AddressHigh, message.AddressLow, message.Data);
    }
    return OS_EOK;
}

oserr_t
DeviceInterruptAllocatePci(
    _InOut_ DeviceInterrupt_t* interrupt,
    _In_    uint32_t           min,
    _In_    uint32_t           optimal,
    _In_    uint32_t           modes,
    _Out_   uint32_t*          countOut)
{
    if (countOut != NULL) {
        *countOut = 0;
    }

    if (interrupt == NULL || !interrupt->IsPci) {
        return OS_EINVALPARAMS;
    }
    
    return __InterruptAllocate(interrupt, min, optimal, modes, countOut);
}

uint32_t
DeviceInterruptGetPciMode(
    _In_ const DeviceInterrupt_t* interrupt)
{
    // The chosen mode is kept in the private state, so the public interrupt
    // structure stays free of PCI details.
    return interrupt->State == NULL ? 0 : interrupt->State->PciMode;
}

