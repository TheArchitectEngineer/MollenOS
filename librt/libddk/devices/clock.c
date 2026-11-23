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
 * Wrapper for OSIOCTLREQUEST_CLOCK_RATE.
 */

#include <ddk/devices/clock.h>
#include <os/device.h>

oserr_t
DeviceClockGetRate(
    _In_  uuid_t    device,
    _In_  uint32_t  index,
    _Out_ uint64_t* hz)
{
    struct DeviceClockRequest request = { .Index = index };
    oserr_t                   status;

    if (hz == NULL) {
        return OS_EINVALPARAMS;
    }

    status = OSDeviceIOCtl(
        device,
        OSIOCTLREQUEST_CLOCK_RATE,
        &request,
        sizeof(struct DeviceClockRequest)
    );
    if (status != OS_EOK) {
        return status;
    }

    // A running clock never has a rate of zero. Treat it as a broken answer
    // instead of letting the driver divide by it.
    if (request.Rate == 0) {
        return OS_EPROTOCOL;
    }
    *hz = request.Rate;
    return OS_EOK;
}
