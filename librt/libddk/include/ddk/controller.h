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
 * Lets a driver's device work as a GPIO controller or clock controller for
 * other devices.
 */

#ifndef __DDK_CONTROLLER_H__
#define __DDK_CONTROLLER_H__

#include <ddk/ddkdefs.h>

_CODE_BEGIN

// The device works as a GPIO controller. deviced sends GPIO controller
// requests (contracts/gpio.gr) to its driver.
#define DEVICE_CONTROLLER_GPIO  0x1U

// The device works as a clock controller. deviced sends clock controller
// requests (contracts/clock.gr) to its driver.
#define DEVICE_CONTROLLER_CLOCK 0x2U

/**
 * @brief Wire format of OSIOCTLREQUEST_CONTROLLER_ENABLE and
 * OSIOCTLREQUEST_CONTROLLER_DISABLE.
 * Kinds is a mask of DEVICE_CONTROLLER_GPIO and DEVICE_CONTROLLER_CLOCK. The
 * request carries no node or line numbers. deviced finds the GPIO controller
 * node and clock controller nodes from the device's own firmware description,
 * so a driver can only act for hardware that sits behind its device.
 */
struct DeviceControllerRequest {
    uint32_t Kinds;
};

/**
 * @brief Tell deviced that this device now works as a GPIO controller, a clock
 * controller or both, so deviced may send its driver requests.
 *
 * Call this after DevicePrepare succeeded and after the driver's GPIO or clock
 * controller server is ready to answer. deviced may send requests before this
 * function returns, when it hands a waiting device to its driver.
 *
 * How deviced finds the controller nodes behind the device, from standard
 * device tree properties and the driver's manifest:
 * - A client node is an enabled node whose "mboxes" property names this
 *   device. Its enabled direct children are the candidates. When the manifest
 *   lists "controller-compatibles", a candidate must have one of them.
 * - GPIO: the device's own node if it has a "gpio-controller" property.
 *   Otherwise the candidate with a "gpio-controller" property.
 * - Clock: the device's own node if it has a "#clock-cells" property.
 *   Otherwise the candidate with a "#clock-cells" property. Then the
 *   manifest's "clock-routes": clocks that consumers name through another
 *   node, while this driver controls them under its own number.
 * Two candidates of one kind are ambiguous and fail the call.
 *
 * While enabled, the driver must answer every GPIO and clock controller
 * request without calling deviced. deviced waits for the answer with its
 * platform bus lock held. DeviceStop on the controller's device fails with
 * OS_EBUSY until every kind is disabled again.
 *
 * Enabling a kind that is already enabled fails with OS_EEXISTS. Another kind
 * can be enabled later with a second call.
 *
 * @param device Device assigned to this driver. It must be prepared.
 * @param kinds DEVICE_CONTROLLER_GPIO, DEVICE_CONTROLLER_CLOCK or both.
 * @return OS_EOK when deviced accepted every requested kind.
 *         OS_EINVALPARAMS for an empty or unknown kinds mask.
 *         OS_EPERMISSIONS when the caller is not the device's driver.
 *         OS_EBUSY before DevicePrepare, or while an earlier controller of this
 *         device, ended by deviced, still has users.
 *         OS_EEXISTS when a requested kind is already enabled, or another
 *         device already acts for the same controller node.
 *         OS_ENOENT when the firmware description has no controller of a
 *         requested kind behind this device.
 *         OS_ENOTSUPPORTED for a controller description deviced cannot use,
 *         such as a "#gpio-cells" value other than 2, two candidates of one
 *         kind, or a clock route that matches two nodes.
 */
DDKDECL(oserr_t,
DeviceControllerEnable(
    _In_ uuid_t   device,
    _In_ uint32_t kinds));

/**
 * @brief Tell deviced that this device no longer works as a GPIO controller,
 * clock controller or both.
 * Disable every kind before stopping the device or exiting. deviced also ends
 * the controller by itself when the device is removed, or when a request fails
 * because the driver is gone. Later requests then fail with OS_ENOTCONNECTED
 * inside deviced instead of reaching the driver.
 *
 * Nothing changes when the call fails.
 *
 * @param device Controller's device.
 * @param kinds DEVICE_CONTROLLER_GPIO, DEVICE_CONTROLLER_CLOCK or both.
 * @return OS_EOK when every requested kind was disabled.
 *         OS_EINVALPARAMS for an empty or unknown kinds mask.
 *         OS_EBUSY while another device still uses a clock or GPIO line of a
 *         requested kind. Stop those users first and retry.
 *         OS_ENOENT when a requested kind is not enabled.
 *         OS_EPERMISSIONS when the caller is not the device's driver.
 */
DDKDECL(oserr_t,
DeviceControllerDisable(
    _In_ uuid_t   device,
    _In_ uint32_t kinds));

_CODE_END

#endif //!__DDK_CONTROLLER_H__
