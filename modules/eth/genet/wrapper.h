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
 *
 * BCM2711 wrapper. The wrapper is the glue logic the chip puts around the
 * UniMAC block: the port mode, the RGMII wiring to the PHY, the MDIO clock and
 * the MAC reset line. This file holds everything that is specific to how the
 * Pi 4 wires up GENET, so the MAC and queue code stay free of board details.
 */

#ifndef __GENET_WRAPPER_H__
#define __GENET_WRAPPER_H__

#include <ddk/devices/resources.h>
#include <ddk/devices/ethernet.h>
#include <stdbool.h>

// Device tree compatible string of the one integration this driver supports.
#define GENET_COMPATIBLE_BCM2711 "brcm,bcm2711-genet-v5"

// Time the wrapper reset line is held, and the time the MAC gets after the
// line is released before its registers are used.
#define GENET_WRAPPER_RESET_SETTLE_US 10U

/**
 * Last wrapper setting write attempted. Each stage includes the earlier
 * writes, and restore walks back to GENET_WRAPPER_UNCHANGED after the MAC and
 * DMA have stopped.
 */
enum GenetWrapperState {
    GENET_WRAPPER_UNCHANGED,
    GENET_WRAPPER_PORT_CHANGED,
    GENET_WRAPPER_OOB_CHANGED,
    GENET_WRAPPER_MDIO_CHANGED
};

/**
 * Whether the wrapper reset is known to be released. Firmware may leave the
 * wrapper in reset, and the UniMAC command register must not be used until it
 * is released. The state is separate from the controller lifetime, because a
 * queue reset may fail before the release, and stopping must then retry the
 * release first.
 */
enum GenetWrapperResetState {
    GENET_WRAPPER_RESET_UNKNOWN,
    GENET_WRAPPER_RESET_RELEASED
};

/**
 * @brief Wrapper state for one controller.
 * Registers is NULL until GenetWrapperConfigurePhy has checked that the window
 * belongs to a GENET v5, and NULL again once the settings are restored. Port,
 * Oob and Mdio hold the original register values, kept until every changed
 * register is restored. State says how far the setting writes got, and Reset
 * says whether the MAC reset line is released.
 */
typedef struct GenetWrapper {
    DeviceIo_t*                 Registers;
    uint32_t                    Port;
    uint32_t                    Oob;
    uint32_t                    Mdio;
    enum GenetWrapperState      State;
    enum GenetWrapperResetState Reset;
} GenetWrapper_t;

/**
 * @brief Check whether the register window has passed the GENET v5 checks.
 * Cleanup uses this to decide if the MAC and DMA registers may be touched.
 * @param wrapper Wrapper state protected by the adapter lock.
 * @return true from a successful hardware check until the settings are restored.
 */
static inline bool
GenetWrapperIsVerified(
    _In_ const GenetWrapper_t* wrapper)
{
    return wrapper->Registers != NULL;
}

/**
 * @brief Check that the device is the BCM2711 GENET v5 this driver supports.
 * @param device Device description that has already been validated.
 * @return true if the device lists GENET_COMPATIBLE_BCM2711.
 */
__EXTERN bool
GenetWrapperMatchesDevice(
    _In_ const Device_t* device);

/**
 * @brief Set up the RGMII link to the external PHY, after the platform code has
 * prepared the device.
 * @param wrapper Zeroed wrapper state. It keeps the original settings, used to
 *                undo the changes later. Keep it even if this call fails.
 * @param registers Mapped GENET window. Must stay valid until the restore succeeds.
 * @param configuration Firmware configuration for this MAC.
 * @return OS_EOK, OS_EBUSY if the wrapper is already configured,
 *         OS_ENOTSUPPORTED for a board setup this driver does not handle, or a
 *         register error.
 */
__EXTERN oserr_t
GenetWrapperConfigurePhy(
    _InOut_ GenetWrapper_t*                           wrapper,
    _In_    DeviceIo_t*                               registers,
    _In_    const struct DeviceEthernetConfiguration* configuration);

/**
 * @brief Put the original wrapper settings back. Call it after the MAC and DMA
 * have stopped.
 * @param wrapper The state saved by GenetWrapperConfigurePhy. A failed restore can be retried.
 * @return Register write status.
 */
__EXTERN oserr_t
GenetWrapperRestorePhy(
    _InOut_ GenetWrapper_t* wrapper);

/**
 * @brief Put the MAC into reset through the wrapper and hold it there for
 * GENET_WRAPPER_RESET_SETTLE_US. Follow it with GenetWrapperReleaseReset.
 * @param wrapper Wrapper state. Reset becomes GENET_WRAPPER_RESET_UNKNOWN.
 * @param registers Mapped and checked GENET v5 register window.
 * @return Register write or delay status.
 */
__EXTERN oserr_t
GenetWrapperAssertReset(
    _InOut_ GenetWrapper_t* wrapper,
    _In_    DeviceIo_t*     registers);

/**
 * @brief Release the wrapper reset that firmware or GenetWrapperAssertReset
 * may have left on, before the first access to the UniMAC command register.
 * The MAC address registers are not reset.
 * @param wrapper Wrapper state. Reset becomes GENET_WRAPPER_RESET_RELEASED on success.
 * @param registers Mapped and checked GENET v5 register window.
 * @return Register write or delay status.
 */
__EXTERN oserr_t
GenetWrapperReleaseReset(
    _InOut_ GenetWrapper_t* wrapper,
    _In_    DeviceIo_t*     registers);

#endif //!__GENET_WRAPPER_H__
