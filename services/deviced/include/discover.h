/**
 * Copyright 2021, Philip Meulengracht
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
 *
 * Device Manager
 * - Implementation of the device manager in the operating system.
 *   Keeps track of devices, their loaded drivers and bus management.
 */

#ifndef __DISCOVER_H__
#define __DISCOVER_H__

#include <os/osdefs.h>
#include <ds/mstring.h>
#include <ds/list.h>
#include <os/types/capabilities.h>

struct DriverProduct {
    element_t ListHeader;
    uint32_t  Id;
};

struct DriverVendor {
    element_t ListHeader;
    uint32_t  Id;
    list_t    Products;
};

struct DriverResource {
    element_t ListHeader;
    int       Type;
    uintptr_t Base;
    size_t    Length;
};

struct DriverCompatible {
    element_t ListHeader;
    char*     Name;
};

// Most clock routes one driver manifest may declare.
#define DRIVER_MAX_CLOCK_ROUTES           4

// Most entries of "controller-compatibles" one driver manifest may declare.
#define DRIVER_MAX_CONTROLLER_COMPATIBLES 4

// Longest compatible string of the controller keys, including its terminator.
#define DRIVER_CONTROLLER_COMPATIBLE_MAX  64

/**
 * @brief One entry of the manifest key "clock-routes".
 *
 * Consumers name a clock as cell Clock of the enabled firmware node whose
 * compatible list holds Compatible. The driver controls the same clock as its
 * own clock number ControllerClock.
 */
struct DriverClockRoute {
    char     Compatible[DRIVER_CONTROLLER_COMPATIBLE_MAX];
    uint32_t Clock;
    uint32_t ControllerClock;
};

/**
 * @brief The manifest keys deviced uses when the driver enables its device as
 * a GPIO or clock controller (DeviceControllerEnable).
 *
 * Compatibles holds CompatibleCount entries of "controller-compatibles": when
 * there are any, a child of a client node is a controller node of the device
 * only when its compatible list holds one of them. ClockRoutes holds
 * ClockRouteCount entries of "clock-routes". Both live in the trusted
 * manifest, so a driver can never name nodes or clocks itself.
 */
struct DriverControllerManifest {
    uint32_t                CompatibleCount;
    char                    Compatibles[DRIVER_MAX_CONTROLLER_COMPATIBLES][DRIVER_CONTROLLER_COMPATIBLE_MAX];
    uint32_t                ClockRouteCount;
    struct DriverClockRoute ClockRoutes[DRIVER_MAX_CLOCK_ROUTES];
};

/**
 * @brief Driver matching data, resources and requested initial capabilities
 * parsed from trusted installed driver configuration.
 * The configuration owns the entries in Vendors, Compatibles and Resources,
 * including the product lists and compatible strings. Controller holds the
 * keys "controller-compatibles" and "clock-routes".
 */
struct DriverConfiguration {
    Capabilities_t                  Capabilities;
    uint32_t                        Class;
    uint32_t                        Subclass;
    list_t                          Vendors;
    list_t                          Compatibles;
    list_t                          Resources;
    struct DriverControllerManifest Controller;
};

/**
 * @brief Device identifiers used to select a driver. IsPlatform selects firmware
 * compatible matching or numeric bus matching. Compatibles must remain valid
 * while the identification is used for matching.
 */
struct DriverIdentification {
    // Set when this identity came from a firmware-enumerated PlatformDevice_t
    // (for example, a device described by ACPI or a device tree). Such devices
    // are identified by Compatibles; their numeric bus identifiers and class
    // fields may be unset and must not be used as a fallback.
    int         IsPlatform;

    // For platform devices, this is the bounded, NUL-separated compatible
    // string list supplied by firmware, in preference order. The length counts
    // bytes in the complete list, including each string terminator.
    const char* Compatibles;
    size_t      CompatibleLength;

    // Non-platform devices are matched by vendor/product identifiers or by
    // the class/subclass pair. A zero vendor ID or an all-zero class pair means
    // that identity was not supplied.
    uint32_t    VendorId;
    uint32_t    ProductId;

    uint32_t    Class;
    uint32_t    Subclass;
};

/**
 * @brief Checks whether a driver's settings match a device.
 * A positive score means the driver matches; a lower score means a better
 * match. For platform devices, the score is the position of the first
 * compatible string supported by the driver, with firmware's first entry
 * preferred. For non-platform devices, a vendor/product match scores 1;
 * otherwise the exact class/subclass pair may match with score 2. A score of
 * zero means no match or malformed platform-compatible data.
 *
 * @param configuration The settings that describe which devices the driver supports.
 * @param identification The identifiers and class information for the device.
 * @return A positive match score, or zero if the driver does not match.
 */
__EXTERN unsigned int
DmDriverMatchScore(
    _In_ const struct DriverConfiguration*  configuration,
    _In_ const struct DriverIdentification* identification);

/**
 * @brief Checks whether any known driver configuration matches a device.
 * Nothing is started or queued. Takes the driver list lock, so the caller
 * must not hold a lock that the driver list code takes after it.
 *
 * @param identification The identifiers used to find a match.
 * @return Nonzero when at least one known driver matches.
 */
__EXTERN int
DmDiscoverHasDriver(
    _In_ const struct DriverIdentification* identification);

/**
 * @brief Copy the controller keys from the manifest of the driver with this
 * service endpoint.
 *
 * The endpoint is the one a device record holds for its assigned driver
 * (DmDeviceGetDriver). Only drivers that reported their endpoint are found.
 * Takes only a lock that is never held while another lock is taken, so it may
 * be called with the DMA lock and a bus lock held. The manifest of a started
 * driver never changes, so the copy stays correct.
 *
 * @param driverHandle Service endpoint of the driver.
 * @param manifest Receives the copy. Unchanged on failure.
 * @return OS_EOK, OS_EINVALPARAMS for a missing pointer, or OS_ENOENT when no
 *         started driver has this endpoint.
 */
__EXTERN oserr_t
DmDiscoverControllerManifest(
    _In_  uuid_t                           driverHandle,
    _Out_ struct DriverControllerManifest* manifest);

/**
 * @brief Removes a device from the device lists maintained for drivers.
 * Call this after stopping programs that use the device. This function only
 * clears the driver's tracking information; it does not stop those programs.
 *
 * @param deviceId The ID of the device to remove from the driver lists.
 */
__EXTERN void
DmDiscoverForgetDevice(
    _In_ uuid_t deviceId);

/**
 * @brief Initializes discovery and queues ramdisk discovery and device matching.
 * Call once before using the discovery list. Both jobs run asynchronously.
 */
__EXTERN void
DmDiscoverInitialize(void);

/**
 * @brief Adds a driver to the list of drivers that can be matched with devices.
 * Discovery must be initialized. The path is copied, and discovery owns
 * driverConfig on success. If copying the path fails, driverConfig is destroyed.
 * Failure to allocate the driver entry leaves driverConfig with the caller.
 *
 * @param driverPath The path to the driver's program.
 * @param driverConfig The settings that describe which devices the driver supports.
 * @return OS_EOK if the driver was added, or an error code if it could not be added.
 */
__EXTERN oserr_t
DmDiscoverAddDriver(
    _In_ mstring_t*                  driverPath,
    _In_ struct DriverConfiguration* driverConfig);

/**
 * @brief Removes a driver from the list of drivers that can be matched with devices.
 * A driver that has started loading is kept in the list.
 *
 * @param driverPath The path of the driver to remove.
 * @return OS_EOK if the driver is known, or OS_ENOENT if no driver has this path.
 */
__EXTERN oserr_t
DmDiscoverRemoveDriver(
    _In_ mstring_t* driverPath);

/**
 * @brief Finds the best matching driver for a device and starts or connects it.
 *
 * @param deviceId The ID of the device to find a driver for.
 * @param deviceIdentification The identifiers and class information used to find a match.
 * @return OS_EOK if a matching driver was started or connected, or an error code
 *         if the device cannot be matched or the driver cannot be started or connected.
 */
__EXTERN oserr_t
DmDiscoverFindDriver(
    _In_ uuid_t                       deviceId,
    _In_ struct DriverIdentification* deviceIdentification);

/**
 * @brief Accept readiness only from the security group returned by spawn.
 * The caller must obtain principal from kernel-authenticated transport metadata.
 * Duplicate notifications are harmless; conflicting identities or endpoints fail.
 *
 * @param principal Full kernel-authenticated IPC principal.
 * @param driverId Driver entry selected during discovery.
 * @param driverHandle Service endpoint used only to route messages.
 */
__EXTERN void
DmHandleNotify(
    _In_ uint64_t principal,
    _In_ uuid_t   driverId,
    _In_ uuid_t   driverHandle);

#endif //!__DISCOVER_H__
