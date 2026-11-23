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
 * Clock queries for drivers of platform devices.
 */

#ifndef __DDK_CLOCK_H__
#define __DDK_CLOCK_H__

#include <ddk/ddkdefs.h>

_CODE_BEGIN

/**
 * @brief Wire format of OSIOCTLREQUEST_CLOCK_RATE.
 * Index selects the clock by its position in the device's "clocks" property,
 * starting at zero. Reserved and Rate must be zero in the request. deviced
 * writes the rate in Hz to Rate.
 */
struct DeviceClockRequest {
    uint32_t Index;
    uint32_t Reserved;
    uint64_t Rate;
};

/**
 * @brief Read the rate of one clock of a prepared device.
 * DevicePrepare has started the clock. The rate comes from the clock
 * controller driver, or from the "clock-frequency" property of a fixed clock.
 * Use it to compute divisors, for example the SD card clock divisor.
 *
 * @param device Prepared device assigned to this driver.
 * @param index Position of the clock in the device's "clocks" property.
 * @param hz Receives the rate in Hz on success. Unchanged on failure.
 * @return OS_EOK on success.
 *         OS_EINVALPARAMS for a missing output pointer.
 *         OS_EPERMISSIONS when the caller is not the device's driver.
 *         OS_EBUSY before DevicePrepare.
 *         OS_ENOENT when the device has no clock at that position.
 *         OS_ENOTSUPPORTED when the clock's owner cannot report a rate.
 *         OS_ETIMEOUT, OS_ENOTCONNECTED or the driver's error when the clock
 *         controller driver failed to answer. A driver error of
 *         OS_ENOTSUPPORTED comes back as OS_EDEVFAULT.
 *         OS_EPROTOCOL when the request or its answer could not be transported.
 */
DDKDECL(oserr_t,
DeviceClockGetRate(
    _In_  uuid_t    device,
    _In_  uint32_t  index,
    _Out_ uint64_t* hz));

_CODE_END

#endif //!__DDK_CLOCK_H__
