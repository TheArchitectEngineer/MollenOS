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

#ifndef __DDK_INTERRUPT_PRIVATE_H__
#define __DDK_INTERRUPT_PRIVATE_H__

#include <ddk/interrupts/interrupt.h>
#include <ddk/types/interrupt-group.h>

/**
 * Requests sent to the bus driver that programs the interrupts of the device.
 * This is the PCI bus for PCI devices, and the platform bus for devices that
 * are described by firmware. interrupt.c and pci.c translate each request into
 * the ioctl format of that bus.
 */
enum __InterruptBusRequest {
    // Set aside the interrupts on the device, without starting delivery.
    InterruptBusRequestReserve,
    // Let the device deliver its interrupts.
    InterruptBusRequestEnable,
    // Stop the device from delivering interrupts.
    InterruptBusRequestDisable,
    // Give back what Reserve set aside.
    InterruptBusRequestRelease
};

/**
 * Where an allocation is between DeviceInterruptAllocate and the end of
 * DeviceInterruptDestroy. It only moves forward.
 */
enum __InterruptPhase {
    // Allocation is still running. The interrupt can not be enabled yet.
    InterruptPhasePreparing,
    // Allocation succeeded. Handlers and resources can be set, and delivery
    // can be enabled and disabled.
    InterruptPhaseReady,
    // DeviceInterruptDestroy has started. Only Disable and Destroy are allowed
    // from here, even after a Disable succeeds.
    InterruptPhaseDestroying
};

/**
 * Whether the bus driver has set interrupts aside for this allocation.
 */
enum __InterruptBusReservation {
    // The bus driver holds nothing for this allocation.
    InterruptBusReservationNone,
    // The bus driver holds a reservation, or may hold one. This is set before
    // the reserve request is sent, because a lost reply does not tell whether
    // the bus driver made the reservation. It is only cleared when the bus
    // driver answers OS_EBUSY to the reserve request, which means it changed
    // nothing, or when a release request succeeds.
    InterruptBusReservationHeld
};

/**
 * How far delivery has been set up, and whether a stop still has to finish.
 *
 * Every state except Stopped and Enabled means that cleanup is still needed.
 * DeviceInterruptEnable finishes that cleanup before it binds handlers again,
 * and handlers or resources can only be changed in the Stopped state.
 */
enum __InterruptDelivery {
    // No handler is bound in the kernel and no stop is waiting to finish.
    InterruptDeliveryStopped,
    // No handler is bound in the kernel, but a stop has started and has not
    // finished. This happens when the bus driver fails a disable request.
    InterruptDeliveryStopPending,
    // Handlers are bound in the kernel, or some may be left bound after a
    // failed bind. The kernel has not enabled delivery.
    InterruptDeliveryBound,
    // The kernel has enabled delivery, but the bus driver has not confirmed
    // its side. Either its enable request is not answered yet, or a stop has
    // started and has not finished.
    InterruptDeliveryKernelEnabled,
    // The kernel and the bus driver have both confirmed delivery.
    InterruptDeliveryEnabled
};

/**
 * Bookkeeping for one interrupt allocation. It is created by
 * DeviceInterruptAllocate and freed by DeviceInterruptDestroy. When a cleanup
 * step fails the structure is kept, so the driver can call Disable or Destroy
 * again to finish the cleanup.
 */
struct DeviceInterruptState {
    // Kernel handle of the allocation.
    uuid_t                         Handle;
    // See enum __InterruptPhase.
    enum __InterruptPhase          Phase;
    // See enum __InterruptDelivery.
    enum __InterruptDelivery       Delivery;
    // See enum __InterruptBusReservation.
    enum __InterruptBusReservation BusReservation;
    // The INTERRUPT_STRATEGY_* value chosen for a PCI device. Only pci.c
    // reads it. Zero for every other device.
    uint32_t                       PciMode;
    // Nonzero when the interrupt was looked up by its firmware source index.
    // Bus requests then go to the platform bus instead of the PCI bus.
    int                            PlatformSource;
};

/**
 * @brief Shared allocation path of DeviceInterruptAllocate and
 * DeviceInterruptAllocatePci. Reserves the interrupts with the kernel and the
 * bus driver, and undoes both when a step fails.
 *
 * @param interrupt Initialized interrupt without an allocation.
 * @param minimum   Lowest number of vectors the driver can work with.
 * @param optimal   Number of vectors the driver would like, at least minimum.
 * @param pciModes  INTERRUPT_STRATEGY_* values the driver accepts. Only used
 *                  for PCI devices.
 * @param countOut  Receives the number of vectors on success, and zero on
 *                  failure.
 * @return OS_EOK on success. On failure the error of the failed step is
 *         returned. If undoing the allocation failed as well, the state is
 *         kept on the interrupt so DeviceInterruptDestroy can be called again.
 */
oserr_t
__InterruptAllocate(
    _InOut_ DeviceInterrupt_t* interrupt,
    _In_    uint32_t           minimum,
    _In_    uint32_t           optimal,
    _In_    uint32_t           pciModes,
    _Out_   uint32_t*          countOut);

/**
 * @brief Picks MSI-X, MSI or the legacy interrupt line for a PCI device and
 * reserves the vectors in the kernel. Nothing is requested from the PCI bus
 * yet, and delivery stays off.
 *
 * @param interrupt    PCI interrupt. Supplies the device and the legacy line.
 * @param minimum      Lowest number of vectors the driver can work with.
 * @param optimal      Number of vectors the driver would like.
 * @param modes        INTERRUPT_STRATEGY_* values the driver accepts.
 * @param result       Receives the kernel reservation on success.
 * @param selectedMode Receives the chosen INTERRUPT_STRATEGY_* value on success.
 * @return OS_EOK on success, or the error from the query or the reservation.
 */
oserr_t
__InterruptPciReserve(
    _In_  DeviceInterrupt_t*      interrupt,
    _In_  uint32_t                minimum,
    _In_  uint32_t                optimal,
    _In_  uint32_t                modes,
    _Out_ InterruptGroupResult_t* result,
    _Out_ uint32_t*               selectedMode);

/**
 * @brief Sends one bus request to the PCI bus for an allocated PCI interrupt.
 * The state of the interrupt is not changed.
 *
 * @param interrupt Allocated PCI interrupt with a chosen mode.
 * @param request   Request to send.
 * @return OS_EOK on success, or the error from the PCI bus or the transport.
 */
oserr_t
__InterruptPciRequest(
    _In_ DeviceInterrupt_t*         interrupt,
    _In_ enum __InterruptBusRequest request);

/**
 * @brief Writes the address and data of every reserved vector to the PCI
 * device. Must be called after the reserve request succeeded and before
 * delivery is enabled.
 *
 * @param interrupt PCI interrupt whose reservation with the PCI bus succeeded.
 * @return OS_EOK on success, or the error from the kernel query or the PCI
 *         bus. On failure the caller keeps the reservation and releases it
 *         through DeviceInterruptDestroy.
 */
oserr_t
__InterruptPciProgram(
    _In_ DeviceInterrupt_t* interrupt);

#endif //!__DDK_INTERRUPT_PRIVATE_H__
