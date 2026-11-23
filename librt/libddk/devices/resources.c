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
 * Wrappers for OSIOCTLREQUEST_DEVICE_RESOURCES and OSIOCTLREQUEST_DEVICE_SET_STAGE.
 * A driver asks the bus of its device for the register ranges, and tells it
 * when the device is prepared, started and stopped.
 */

#include <ddk/devices/resources.h>
#include <os/device.h>

oserr_t
DeviceGetResources(
    _In_  uuid_t                  device,
    _Out_ struct DeviceResources* resources)
{
    struct DeviceResources result = { 0 };
    oserr_t                status;

    if (resources == NULL) {
        return OS_EINVALPARAMS;
    }

    // Query into temporary storage so a transport error or invalid reply does
    // not replace the caller's existing resource description.
    status = OSDeviceIOCtl(
        device,
        OSIOCTLREQUEST_DEVICE_RESOURCES,
        &result,
        sizeof(struct DeviceResources)
    );
    if (status != OS_EOK) {
        return status;
    }
    
    // The returned count must fit the fixed array before a driver uses it.
    if (result.Count > DEVICE_MAX_REGISTERS) {
        return OS_EPROTOCOL;
    }
    
    *resources = result;
    return OS_EOK;
}

oserr_t
DeviceSetStage(
    _In_ uuid_t           device,
    _In_ enum DeviceStage stage)
{
    return OSDeviceIOCtl(
        device,
        OSIOCTLREQUEST_DEVICE_SET_STAGE,
        &stage,
        sizeof(enum DeviceStage)
    );
}
