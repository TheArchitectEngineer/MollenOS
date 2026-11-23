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
 * Wrappers for OSIOCTLREQUEST_CONTROLLER_ENABLE and OSIOCTLREQUEST_CONTROLLER_DISABLE.
 */

#include <ddk/controller.h>
#include <os/device.h>

static int
__ValidKinds(
    _In_ uint32_t kinds)
{
    return kinds != 0 && (kinds & ~(DEVICE_CONTROLLER_GPIO | DEVICE_CONTROLLER_CLOCK)) == 0;
}

oserr_t
DeviceControllerEnable(
    _In_ uuid_t   device,
    _In_ uint32_t kinds)
{
    struct DeviceControllerRequest request = { .Kinds = kinds };

    // Reject an empty or unknown mask here, so a driver bug shows up before
    // the request reaches deviced. deviced checks the mask again.
    if (!__ValidKinds(kinds)) {
        return OS_EINVALPARAMS;
    }

    return OSDeviceIOCtl(
        device,
        OSIOCTLREQUEST_CONTROLLER_ENABLE,
        &request,
        sizeof(struct DeviceControllerRequest)
    );
}

oserr_t
DeviceControllerDisable(
    _In_ uuid_t   device,
    _In_ uint32_t kinds)
{
    struct DeviceControllerRequest request = { .Kinds = kinds };

    if (!__ValidKinds(kinds)) {
        return OS_EINVALPARAMS;
    }

    return OSDeviceIOCtl(
        device,
        OSIOCTLREQUEST_CONTROLLER_DISABLE,
        &request,
        sizeof(struct DeviceControllerRequest)
    );
}
