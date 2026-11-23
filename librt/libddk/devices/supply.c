/**
 * Copyright 2026, Philip Meulengracht
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
 * Wrappers for OSIOCTLREQUEST_SUPPLY_CONTROL.
 */

#include <ddk/devices/supply.h>
#include <os/device.h>
#include <string.h>

static oserr_t
__SupplyRequest(
    _In_    uuid_t                      device,
    _In_    const char*                 name,
    _In_    enum DeviceSupplyOperation  operation,
    _InOut_ struct DeviceSupplyRequest* request)
{
    size_t length;

    // Check the name here, so a driver bug shows up before the request
    // reaches deviced. deviced checks the name again.
    if (name == NULL) {
        return OS_EINVALPARAMS;
    }

    length = strnlen(name, DEVICE_SUPPLY_NAME_MAX);
    if (length == 0 || length == DEVICE_SUPPLY_NAME_MAX) {
        return OS_EINVALPARAMS;
    }

    // The bytes after the name stay zero, as the wire format requires.
    request->Operation = (uint32_t)operation;
    memcpy(&request->Name[0], name, length);
    return OSDeviceIOCtl(device, OSIOCTLREQUEST_SUPPLY_CONTROL, request, sizeof(*request));
}

oserr_t
DeviceSupplyManage(
    _In_ uuid_t      device,
    _In_ const char* name)
{
    struct DeviceSupplyRequest request = { 0 };

    return __SupplyRequest(device, name, DeviceSupplyOperationManage, &request);
}

oserr_t
DeviceSupplySetState(
    _In_ uuid_t      device,
    _In_ const char* name,
    _In_ int         on)
{
    struct DeviceSupplyRequest request = { .Value = on ? 1 : 0 };

    return __SupplyRequest(device, name, DeviceSupplyOperationSetState, &request);
}

oserr_t
DeviceSupplyGetVoltage(
    _In_  uuid_t      device,
    _In_  const char* name,
    _Out_ uint32_t*   microvolts)
{
    struct DeviceSupplyRequest request = { 0 };
    oserr_t                    status;

    if (microvolts == NULL) {
        return OS_EINVALPARAMS;
    }

    status = __SupplyRequest(device, name, DeviceSupplyOperationGetVoltage, &request);
    if (status != OS_EOK) {
        return status;
    }

    // A voltage of zero is never a valid answer for a working supply.
    if (request.Value == 0) {
        return OS_EPROTOCOL;
    }
    *microvolts = request.Value;
    return OS_EOK;
}

oserr_t
DeviceSupplySetVoltage(
    _In_ uuid_t      device,
    _In_ const char* name,
    _In_ uint32_t    microvolts)
{
    struct DeviceSupplyRequest request = { .Value = microvolts };

    if (microvolts == 0) {
        return OS_EINVALPARAMS;
    }
    return __SupplyRequest(device, name, DeviceSupplyOperationSetVoltage, &request);
}
