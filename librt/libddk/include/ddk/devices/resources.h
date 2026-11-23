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

#ifndef __DDK_RESOURCES_H__
#define __DDK_RESOURCES_H__

#include <ddk/devices/device.h>
#include <ddk/io.h>

_CODE_BEGIN

#define DEVICE_MAX_REGISTERS 8

/**
 * @brief Identifies the PCI device that a device sits directly below, as
 * reported by the bus of the device. A VendorId of zero means there is no such
 * parent, or the bus does not know it. The values describe the parent only.
 * They say nothing about the device itself.
 */
struct DeviceParentPciIdentity {
    uint32_t VendorId;
    uint32_t ProductId;
    uint32_t Revision;
};

/**
 * @brief The register ranges of a device and the identity of its PCI parent.
 * Count gives the number of valid entries in Registers. ParentPci is all zero
 * when the bus cannot name a PCI parent.
 *
 * This is the layout of the OSIOCTLREQUEST_DEVICE_RESOURCES ioctl. When it
 * changes, deviced and every driver must be rebuilt together.
 */
struct DeviceResources {
    uint32_t                       Count;
    DeviceIo_t                     Registers[DEVICE_MAX_REGISTERS];
    struct DeviceParentPciIdentity ParentPci;
};

/**
 * The stages a driver moves its device through with DeviceSetStage.
 */
enum DeviceStage {
    // Turn on access to the registers of the device, together with the bus
    // and the power it depends on.
    DevicePrepare,
    // Let the device do DMA. Requested once the driver has set up its buffers
    // and interrupts.
    DeviceStart,
    // Turn DMA off again and shut the device down on its bus. Requested after
    // the driver has halted the device.
    DeviceStop
};

/**
 * @brief Asks the bus of the device for its register ranges. The ranges are
 * described, not mapped.
 *
 * @param device    ID of the registered device.
 * @param resources Receives the register ranges. Only written on success.
 * @return OS_EOK on success, OS_EINVALPARAMS for a missing output,
 *         OS_EPROTOCOL when the answer holds more than DEVICE_MAX_REGISTERS
 *         ranges, or the error from the bus.
 */
DDKDECL(oserr_t,
DeviceGetResources(
    _In_  uuid_t                  device,
    _Out_ struct DeviceResources* resources));

/**
 * @brief Moves the device to a new stage through its bus. Asking for the stage
 * the device is already in succeeds and changes nothing.
 *
 * @param device ID of the registered device.
 * @param stage  DevicePrepare, DeviceStart or DeviceStop.
 * @return OS_EOK on success, or the error from the bus. After a failure the
 *         driver still has to clean up what it set up itself.
 */
DDKDECL(oserr_t,
DeviceSetStage(
    _In_ uuid_t           device,
    _In_ enum DeviceStage stage));

_CODE_END

#endif //!__DDK_RESOURCES_H__
