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
 * Link handling. The PHY (the chip that talks to the cable) reports whether
 * there is a link and at what speed. This file reads that result, tolerates a
 * few failed reads, and sets the MAC to the same speed and duplex mode.
 */

#ifndef __GENET_LINK_H__
#define __GENET_LINK_H__

#include "../common/netadapter.h"

DECL_STRUCT(GenetController);

// Time between PHY link reads.
#define GENET_LINK_POLL_INTERVAL_MS        1000U
// Failed PHY reads in a row that are reported as a down link before the read
// error is passed on. A single MDIO timeout must not close the session.
#define GENET_LINK_READ_FAILURE_LIMIT      3U
// Time between PHY link reads once the failure limit is reached. Each failed
// read can wait out the full MDIO timeout, so a dead PHY is read less often.
#define GENET_LINK_POLL_FAILED_INTERVAL_MS 10000U

/**
 * @brief Link state of one controller, protected by the adapter lock.
 * Applied is the link the MAC was last set to. It is all zero (down) after a
 * MAC reset. NextPoll is the monotonic time in milliseconds (DDKGetTimeMs) of
 * the next PHY link read.
 * ReadFailures counts failed PHY link reads in a row. A successful read sets
 * it back to zero.
 */
typedef struct GenetLink {
    struct PhyLink Applied;
    uint64_t       NextPoll;
    uint32_t       ReadFailures;
} GenetLink_t;

/**
 * @brief Set the MAC speed, duplex mode and link state from the PHY's result.
 * The PHY must have been initialized successfully. A down link only clears the
 * RGMII link bit. The MAC transmitter and receiver stay as they are, so frames
 * already in the TX ring can still finish. MAC traffic is switched on only
 * while the link is up and the DMA engines may be running.
 * @param controller GENET state protected by the adapter lock.
 * @param link PHY link state.
 * @return OS_EOK, OS_ENOTSUPPORTED for a speed the MAC cannot run, OS_EDEVFAULT
 *         if the command register does not read back, or a register error.
 */
__EXTERN oserr_t
GenetLinkApply(
    _In_ GenetController_t*    controller,
    _In_ const struct PhyLink* link);

/**
 * @brief Read the PHY link, allowing a few failed reads in a row. While fewer
 * than GENET_LINK_READ_FAILURE_LIMIT reads in a row have failed, a failed read
 * is logged and reported as a down link with OS_EOK. The MAC is not changed by
 * this function.
 * @param controller GENET state protected by the adapter lock.
 * @param link Receives the link. Zeroed (down) on any failure.
 * @return OS_EOK, or the PHY read error once the limit is reached.
 */
__EXTERN oserr_t
GenetLinkReadPhy(
    _In_  GenetController_t* controller,
    _Out_ struct PhyLink*    link);

/**
 * @brief Read the PHY link state and apply any change to the MAC. A failed
 * read is handled as GenetLinkReadPhy describes, and leaves the MAC as it is.
 * This is the ReadLink operation of the common adapter code.
 * @param device Adapter protected by the adapter lock.
 * @param link Receives the PHY link state. Zeroed (down) on any failure.
 * @return OS_EOK, the PHY read error once the failure limit is reached, or the
 *         error from applying the link to the MAC.
 */
__EXTERN oserr_t
GenetLinkRead(
    _In_  EthernetDevice_t* device,
    _Out_ struct PhyLink*   link);

/**
 * @brief Say how long to wait before the next PHY link read.
 * @param controller GENET state protected by the adapter lock.
 * @return GENET_LINK_POLL_FAILED_INTERVAL_MS once the failure limit is reached,
 *         otherwise GENET_LINK_POLL_INTERVAL_MS.
 */
__EXTERN uint32_t
GenetLinkPollInterval(
    _In_ const GenetController_t* controller);

#endif //!__GENET_LINK_H__
