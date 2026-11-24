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
 * Frame queues: setup, start and stop. The card moves frames through rings,
 * which are fixed-size lists of descriptors, and a descriptor points at one
 * frame buffer. This driver uses ring 16 in each direction. This file owns
 * the queue state and the control path. Moving frames through the running
 * queues is in transfer.h.
 */

#ifndef __GENET_QUEUES_H__
#define __GENET_QUEUES_H__

#include "../common/mac.h"
#include "../common/netadapter.h"

DECL_STRUCT(GenetController);

// Descriptors in each ring, and the size of the frame buffer behind each one.
#define GENET_RING_COUNT   32U
#define GENET_BUFFER_SIZE  2048U
// Private frame storage: one ring of RX buffers followed by one ring of TX buffers.
#define GENET_STORAGE_SIZE (2U * GENET_RING_COUNT * GENET_BUFFER_SIZE)

/**
 * @brief Client slots, indexed by position in the transmit ring.
 * Producer is the count of descriptors handed to the card since the ring was
 * set up. Consumer is the count the card has finished with. Both are 16-bit
 * counters that wrap around to zero after 65535, and both start at zero, so
 * Producer % GENET_RING_COUNT is also the descriptor position.
 * HardwareBase is the card's TX consumer index when the ring was set up. The
 * card's index registers hold HardwareBase plus our counters.
 * ProgressAt is the time of the last sign of progress: a finished frame, a
 * frame queued on an empty ring, or a poll while the link was down. The stall
 * check measures from it.
 */
typedef struct GenetTransmitQueue {
    EthernetSlot_t* Slots[GENET_RING_COUNT];
    uint64_t        ProgressAt;
    uint16_t        Producer;
    uint16_t        Consumer;
    uint16_t        HardwareBase;
} GenetTransmitQueue_t;

/**
 * @brief First-in, first-out list of client slots waiting for received frames.
 * Head is the oldest waiting slot and Count is how many are waiting. These
 * numbers are separate from the ring's descriptor positions.
 */
typedef struct GenetPostedReceiveSlots {
    EthernetSlot_t* Slots[GENET_RING_COUNT];
    uint32_t        Head;
    uint32_t        Count;
} GenetPostedReceiveSlots_t;

/**
 * @brief Waiting client slots plus our progress through the receive ring.
 * DmaConsumer counts the descriptors we have handled since the ring was set
 * up, including dropped frames, and wraps around after 65535. It starts at
 * zero, so DmaConsumer % GENET_RING_COUNT is also the descriptor position.
 * HardwareBase is the card's RX producer index when the ring was set up. The
 * card's index registers hold HardwareBase plus DmaConsumer.
 * ReportedDiscards is the part of the card's ring-full drop counter that has
 * already been reported, so each drop is reported only once.
 */
typedef struct GenetReceiveQueue {
    GenetPostedReceiveSlots_t PostedSlots;
    uint16_t                  DmaConsumer;
    uint16_t                  HardwareBase;
    uint16_t                  ReportedDiscards;
} GenetReceiveQueue_t;

/**
 * Whether the DMA engines may be running. GENET_DMA_STATE_MAY_BE_RUNNING is
 * set before the first enable write, because the card may start even if the
 * write reports a failure. GENET_DMA_STATE_STOPPED is set only after both
 * engines have been confirmed stopped.
 */
enum GenetDmaState {
    GENET_DMA_STATE_STOPPED,
    GENET_DMA_STATE_MAY_BE_RUNNING
};

/**
 * @brief Frame queues of one controller.
 * The card reads and writes private memory that only this driver uses. Client
 * shared memory is touched only when a frame is accepted for sending or handed
 * back on receive, and the frame is copied at that moment.
 */
typedef struct GenetQueues {
    struct EthernetDma   Storage;
    GenetTransmitQueue_t Transmit;
    GenetReceiveQueue_t  Receive;
    enum GenetDmaState   DmaState;
} GenetQueues_t;

/**
 * @brief Set up the stopped ring 16 queues and their private frame buffers.
 * Stops the queues, resets the MAC, allocates the frame storage on first use
 * and programs both rings. This is the Prepare operation of the common adapter
 * code.
 * @param device Adapter protected by the adapter lock.
 * @return OS_EOK, OS_EINVALPARAMS for thresholds the card cannot use,
 *         or the first stop, reset, allocation or register error.
 */
__EXTERN oserr_t
GenetQueuesPrepare(
    _In_ EthernetDevice_t* device);

/**
 * @brief Start the prepared queues. Turns on MAC traffic if the link is up.
 * This is the Start operation of the common adapter code.
 * @param device Adapter protected by the adapter lock.
 * @return OS_EOK, or the first register or PHY read error. The DMA engines
 *         may be running even if this fails.
 */
__EXTERN oserr_t
GenetQueuesStart(
    _In_ EthernetDevice_t* device);

/**
 * @brief Stop the queues. The order follows the Linux driver: MAC receiver off,
 * TX DMA off, a short wait for frames in flight, RX DMA off, and the MAC
 * transmitter off last. Both DMA engines must be confirmed off before the
 * queue state is cleared. Also safe on whatever state the bootloader left.
 * This is the Stop operation of the common adapter code.
 * @param device Adapter protected by the adapter lock.
 * @return OS_EOK once both engines are confirmed stopped, or when the
 *         registers are not mapped. Otherwise the error of the step that
 *         failed, and the queue state is kept for another attempt.
 */
__EXTERN oserr_t
GenetQueuesStop(
    _In_ EthernetDevice_t* device);

/**
 * @brief Write one descriptor: the frame buffer address first, then the
 * length and flags word, which is the word the card acts on.
 * @param controller GENET state protected by the adapter lock.
 * @param base GENET_RX_DESCRIPTORS or GENET_TX_DESCRIPTORS.
 * @param index Descriptor position in the ring, below GENET_RING_COUNT.
 * @param address Device address of the frame buffer.
 * @param flags Length and flags word. Zero for an RX descriptor.
 * @return Register write status.
 */
__EXTERN oserr_t
GenetQueuesWriteDescriptor(
    _In_ GenetController_t* controller,
    _In_ uint32_t           base,
    _In_ uint32_t           index,
    _In_ uint64_t           address,
    _In_ uint32_t           flags);

#endif //!__GENET_QUEUES_H__
