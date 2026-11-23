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
 * Supply control for drivers of platform devices.
 *
 * A supply is a regulator that a device tree node names with a property that
 * ends in "-supply", such as "vmmc-supply". The driver names a supply by that
 * property name without the "-supply" suffix, so "vmmc" for "vmmc-supply".
 * deviced keeps all supply state. It counts the devices that need a shared
 * supply, waits out settling and start-up times, and releases everything when
 * the device is stopped.
 *
 * By default DevicePrepare switches every supply of the device on. A driver
 * that wants to run a supply itself calls DeviceSupplyManage for it before
 * DevicePrepare. DevicePrepare then only reserves that supply and leaves its
 * state alone.
 *
 * Common return values of every call in this file:
 * OS_EPERMISSIONS when the caller is not the driver assigned to the device.
 * OS_EINVALPARAMS for a missing or too long name, or another invalid argument.
 * OS_ENOENT when the device has no supply of that name.
 * OS_EPROTOCOL when the request or its answer could not be transported.
 */

#ifndef __DDK_SUPPLY_H__
#define __DDK_SUPPLY_H__

#include <ddk/ddkdefs.h>

_CODE_BEGIN

// Size of the name field of a supply request, including the terminating zero.
// A name can be at most 31 characters long.
#define DEVICE_SUPPLY_NAME_MAX 32

// Operations of OSIOCTLREQUEST_SUPPLY_CONTROL.
enum DeviceSupplyOperation {
    // The driver runs this supply itself. Value must be zero.
    DeviceSupplyOperationManage = 1,

    // Switch the supply on (Value 1) or off (Value 0).
    DeviceSupplyOperationSetState,

    // Read the present voltage. deviced writes it to Value in microvolts.
    DeviceSupplyOperationGetVoltage,

    // Select the voltage in Value, in microvolts.
    DeviceSupplyOperationSetVoltage
};

/**
 * @brief Wire format of OSIOCTLREQUEST_SUPPLY_CONTROL.
 * Operation is a DeviceSupplyOperation. Value is the argument or the answer,
 * as the operation describes. Name is the supply name, zero terminated, and
 * every byte after the terminator must be zero. deviced writes the answer back
 * into the same structure.
 */
struct DeviceSupplyRequest {
    uint32_t Operation;
    uint32_t Value;
    char     Name[DEVICE_SUPPLY_NAME_MAX];
};

/**
 * @brief Tell deviced that the driver runs this supply itself.
 * DevicePrepare then reserves the supply but does not switch it on or off.
 * The choice is kept until the device is stopped and its driver changes, or
 * the device is removed. A later DevicePrepare by the same driver honours it
 * again. Call it once per supply.
 *
 * @param device Device assigned to this driver. It must not be prepared.
 * @param name Supply name without the "-supply" suffix, such as "vmmc".
 * @return OS_EOK when deviced recorded the choice.
 *         OS_EBUSY when the device is already prepared. Stop it first.
 *         Otherwise one of the common return values.
 */
DDKDECL(oserr_t,
DeviceSupplyManage(
    _In_ uuid_t      device,
    _In_ const char* name));

/**
 * @brief Switch a supply of a prepared device on or off.
 * Works on every supply of the device, also on the ones DevicePrepare
 * switched on by default. Switching on waits out the regulator's start-up or
 * settling time, and the off-on delay since the last switch-off, before it
 * returns.
 *
 * A supply shared with other devices goes off only when no device needs it
 * any more. Switching off then only drops this device's need, and the call
 * still returns OS_EOK.
 *
 * @param device Prepared device assigned to this driver.
 * @param name Supply name without the "-supply" suffix.
 * @param on Nonzero to switch on, zero to switch off.
 * @return OS_EOK when the supply is in the requested state, or stays on for
 *         another device.
 *         OS_EBUSY before DevicePrepare.
 *         OS_ENOTSUPPORTED when the regulator cannot be switched off, because
 *         it has no enable line or is marked always on. Nothing changes.
 *         OS_ETIMEOUT, OS_ENOTCONNECTED, OS_EDEVFAULT or another error when
 *         the GPIO controller behind the regulator failed. The state is
 *         unchanged. A failed GPIO request never gives OS_ENOTSUPPORTED:
 *         deviced passes that code from the GPIO controller as OS_EDEVFAULT.
 *         Otherwise one of the common return values.
 */
DDKDECL(oserr_t,
DeviceSupplySetState(
    _In_ uuid_t      device,
    _In_ const char* name,
    _In_ int         on));

/**
 * @brief Read the present voltage of a supply of a prepared device.
 *
 * @param device Prepared device assigned to this driver.
 * @param name Supply name without the "-supply" suffix.
 * @param microvolts Receives the voltage on success. Unchanged on failure.
 * @return OS_EOK on success.
 *         OS_EBUSY before DevicePrepare.
 *         OS_ENOTSUPPORTED when the firmware description gives no voltage.
 *         Another error, never OS_ENOTSUPPORTED, when the GPIO controller
 *         behind the regulator failed.
 *         Otherwise one of the common return values.
 */
DDKDECL(oserr_t,
DeviceSupplyGetVoltage(
    _In_  uuid_t      device,
    _In_  const char* name,
    _Out_ uint32_t*   microvolts));

/**
 * @brief Select an exact voltage for a supply of a prepared device.
 * The regulator's firmware description must list that voltage, for example in
 * the "states" table of a "regulator-gpio" node. The call waits out the
 * regulator's settling time before it returns.
 *
 * @param device Prepared device assigned to this driver.
 * @param name Supply name without the "-supply" suffix.
 * @param microvolts Voltage in microvolts. Must not be zero.
 * @return OS_EOK when the supply now has that voltage.
 *         OS_EBUSY before DevicePrepare.
 *         OS_ENOTSUPPORTED when the regulator cannot produce that voltage.
 *         Another error, never OS_ENOTSUPPORTED, when the GPIO controller
 *         behind the regulator failed.
 *         Otherwise one of the common return values.
 */
DDKDECL(oserr_t,
DeviceSupplySetVoltage(
    _In_ uuid_t      device,
    _In_ const char* name,
    _In_ uint32_t    microvolts));

_CODE_END

#endif //!__DDK_SUPPLY_H__
