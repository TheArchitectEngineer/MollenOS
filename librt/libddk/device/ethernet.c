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
 * Wrappers for OSIOCTLREQUEST_ETHERNET_CONFIG and OSIOCTLREQUEST_ETHERNET_PHY_RESET.
 * An ethernet driver reads the PHY setup that firmware describes, and asks
 * the bus of its device to reset the PHY.
 */

#include <ddk/ethernet.h>
#include <os/device.h>

oserr_t
DeviceGetEthernetConfiguration(
    _In_  uuid_t                              device,
    _Out_ struct DeviceEthernetConfiguration* configuration)
{
    struct DeviceEthernetConfiguration result = { 0 };
    oserr_t                            status;

    if (configuration == NULL) {
        return OS_EINVALPARAMS;
    }

    // Firmware configuration can be queried before preparing the hardware.
    // Keep the result local until both the request and reply checks succeed.
    status = OSDeviceIOCtl(
        device,
        OSIOCTLREQUEST_ETHERNET_CONFIG,
        &result,
        sizeof(struct DeviceEthernetConfiguration)
    );
    if (status != OS_EOK) {
        return status;
    }

    // Do not pass an unknown layout, invalid MDIO address or unsupported mode
    // to the driver, where it could lead to programming the wrong PHY settings.
    if (result.Version != 1 || result.PhyAddress > 31 ||
        result.Mode < DeviceEthernetMii || result.Mode > DeviceEthernetRgmiiTxId) {
        return OS_EPROTOCOL;
    }

    *configuration = result;
    return OS_EOK;
}

oserr_t
DeviceResetEthernetPhy(
    _In_ uuid_t device)
{
    // The wire request requires a zero word even though reset has no arguments.
    // GPIO selection and pulse timing come from the validated firmware data.
    uint32_t reserved = 0;

    return OSDeviceIOCtl(
        device,
        OSIOCTLREQUEST_ETHERNET_PHY_RESET,
        &reserved,
        sizeof(uint32_t)
    );
}
