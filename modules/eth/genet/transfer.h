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
 * Frame transfer through the running queues: accepting packets from the
 * client, finishing sent frames and handing over received frames. The queue
 * state and its setup are in queues.h.
 */

#ifndef __GENET_TRANSFER_H__
#define __GENET_TRANSFER_H__

#include "../common/netadapter.h"

// A transmit that makes no progress for this long, while the link is up, faults the session.
#define GENET_TX_STALL_TIMEOUT_MS 5000U

/**
 * @brief Take an accepted packet slot. A receive slot is put on the list of
 * slots waiting for a frame. A transmit slot is copied into a private buffer
 * and handed to the card. This is the Submit operation of the common adapter
 * code.
 * @param device Adapter protected by the adapter lock.
 * @param slot Accepted slot, with room already set aside for its completion.
 * @return OS_EOK, OS_EBUSY if the ring or the waiting list is full,
 *         OS_EINPROGRESS if it is unclear whether the card has seen the
 *         packet, or a register error that left the slot unused.
 */
__EXTERN oserr_t
GenetTransferSubmit(
    _In_ EthernetDevice_t* device,
    _In_ EthernetSlot_t*   slot);

/**
 * @brief Finish sent frames and hand over received frames, using the private
 * rings. One call handles at most one ring of frames in each direction. A
 * ring index the card could not have produced, a stalled transmit or a
 * register error faults the session. This is the Poll operation of the common
 * adapter code.
 * @param device Adapter protected by the adapter lock.
 */
__EXTERN void
GenetTransferPoll(
    _In_ EthernetDevice_t* device);

#endif //!__GENET_TRANSFER_H__
