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
 * PHY link reads and the MAC speed, duplex and link settings. See link.h.
 */

#include <ddk/utils.h>
#include <string.h>

#include "genet.h"

oserr_t
GenetLinkApply(
    _In_ GenetController_t*    controller,
    _In_ const struct PhyLink* link)
{
    uint32_t mode = GENET_COMMAND_PAUSE_IGNORE;
    uint32_t command;
    uint32_t oob;
    oserr_t  status;

    // PhyLink.Speed is in Mbit/s. The MAC runs at 10, 100 or 1000, and 10 is
    // the mode with no speed bit set.
    if (link->Carrier) {
        if (link->Speed == 1000) {
            mode |= GENET_COMMAND_SPEED_1000;
        } else if (link->Speed == 100) {
            mode |= GENET_COMMAND_SPEED_100;
        } else if (link->Speed != 10) {
            ERROR("GENET link: speed %u Mbit/s is not supported", link->Speed);
            return OS_ENOTSUPPORTED;
        }
        if (!link->FullDuplex) {
            mode |= GENET_COMMAND_HALF_DUPLEX;
        }
    }

    // A down link only clears the RGMII link bit below. The transmitter and
    // receiver keep their current setting, as in the Linux driver
    // (bcmgenet_mii_setup). Frames already in the TX ring can then still
    // finish, instead of waiting for a link that may never come back. While
    // the queues are stopped, the stop and the MAC reset have already
    // switched MAC traffic off.
    command = GenetRegisterRead(&controller->Registers, GENET_COMMAND);
    if (link->Carrier && (command & ~GENET_COMMAND_TRAFFIC) != mode) {
        // The speed or duplex mode really changes. Linux writes the new mode
        // without switching traffic off first. This driver turns MAC traffic
        // off for a moment, so no frame is sent or received half in the old
        // mode and half in the new one.
        status = GenetRegisterWrite(&controller->Registers, GENET_COMMAND, mode);
        if (status != OS_EOK) {
            ERROR("GENET link: writing the MAC mode 0x%08x failed: %u", mode, status);
            return status;
        }

        status = DDKStallUs(GENET_MAC_COMMAND_SETTLE_US);
        if (status != OS_EOK) {
            ERROR("GENET link: waiting after the mode change failed: %u", status);
            return status;
        }
    }

    oob = GenetRegisterRead(&controller->Registers, GENET_RGMII_OOB_CTRL);
    oob &= ~GENET_RGMII_LINK;
    if (link->Carrier) {
        oob |= GENET_RGMII_LINK;
    }

    status = GenetRegisterWrite(&controller->Registers, GENET_RGMII_OOB_CTRL, oob);
    if (status != OS_EOK) {
        ERROR("GENET link: writing the RGMII link bit failed: %u", status);
        return status;
    }

    // MAC traffic goes on only while the link is up and the DMA engines may
    // be running. Without running queues, start switches it on later.
    if (link->Carrier) {
        command = mode;
        if (controller->Queues.DmaState == GENET_DMA_STATE_MAY_BE_RUNNING) {
            command |= GENET_COMMAND_TRAFFIC;
        }

        status = GenetRegisterWrite(&controller->Registers, GENET_COMMAND, command);
        if (status != OS_EOK) {
            ERROR("GENET link: writing the MAC command 0x%08x failed: %u", command, status);
            return status;
        }

        if (GenetRegisterRead(&controller->Registers, GENET_COMMAND) != command) {
            ERROR("GENET link: the MAC command does not read back as 0x%08x", command);
            return OS_EDEVFAULT;
        }
    }

    ETHERNET_LOG("GENET link applied: carrier=%u speed=%u duplex=%s queues=%u COMMAND=0x%08x OOB=0x%08x",
        link->Carrier, link->Speed, link->FullDuplex ? "full" : "half",
        controller->Queues.DmaState, command, oob);
    controller->Link.Applied = *link;
    return OS_EOK;
}

oserr_t
GenetLinkReadPhy(
    _In_  GenetController_t* controller,
    _Out_ struct PhyLink*    link)
{
    oserr_t status;

    status = PhyReadLink(&controller->Phy, link);
    GenetLogLinkStatus(controller, status);

    if (status == OS_EOK) {
        if (controller->Link.ReadFailures != 0) {
            WARNING("GENET: the PHY link can be read again after %u failed reads",
                controller->Link.ReadFailures);
        }
        controller->Link.ReadFailures = 0;
        return OS_EOK;
    }

    // PhyReadLink already clears the link on failure. Clear it here too, so
    // the caller never sees a partial result.
    memset(link, 0, sizeof(*link));
    if (controller->Link.ReadFailures < UINT32_MAX) {
        controller->Link.ReadFailures++;
    }

    if (controller->Link.ReadFailures < GENET_LINK_READ_FAILURE_LIMIT) {
        WARNING("GENET: reading the PHY link failed (%u), failure %u of %u, reporting the link as down",
            status, controller->Link.ReadFailures, GENET_LINK_READ_FAILURE_LIMIT);
        return OS_EOK;
    }

    // Log the limit once. Later failures repeat the same error.
    if (controller->Link.ReadFailures == GENET_LINK_READ_FAILURE_LIMIT) {
        ERROR("GENET: reading the PHY link failed %u times in a row (%u), reading it every %u ms from now on",
            GENET_LINK_READ_FAILURE_LIMIT, status, GENET_LINK_POLL_FAILED_INTERVAL_MS);
    }
    return status;
}

uint32_t
GenetLinkPollInterval(
    _In_ const GenetController_t* controller)
{
    return controller->Link.ReadFailures >= GENET_LINK_READ_FAILURE_LIMIT ?
        GENET_LINK_POLL_FAILED_INTERVAL_MS : GENET_LINK_POLL_INTERVAL_MS;
}

oserr_t
GenetLinkRead(
    _In_  EthernetDevice_t* device,
    _Out_ struct PhyLink*   link)
{
    GenetController_t* controller = device->Controller;
    oserr_t            status;

    status = GenetLinkReadPhy(controller, link);
    if (status != OS_EOK) {
        return status;
    }

    // A failed read that is still within the limit reports a down link, but
    // it says nothing about the cable. Leave the MAC as it is. If the link
    // really is up, the next good read matches the applied link and nothing
    // has to change.
    if (controller->Link.ReadFailures != 0) {
        return OS_EOK;
    }

    if (memcmp(link, &controller->Link.Applied, sizeof(*link))) {
        status = GenetLinkApply(controller, link);
        if (status != OS_EOK) {
            memset(link, 0, sizeof(*link));
            return status;
        }
    }
    return OS_EOK;
}
