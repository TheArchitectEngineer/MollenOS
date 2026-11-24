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
 * Frame transfer through the running ring 16 queues. See transfer.h.
 */

#include <os/barrier.h>
#include <ddk/utils.h>
#include <string.h>

#include "genet.h"

// Byte offset of the EtherType field in an Ethernet frame. It follows the
// destination and source addresses and is stored high byte first.
#define GENET_ETHERTYPE_OFFSET (2U * GENET_MAC_ADDRESS_SIZE)

static const uint8_t g_broadcastAddress[GENET_MAC_ADDRESS_SIZE] = {255, 255, 255, 255, 255, 255};

/**
 * Read the EtherType of a frame, for the diagnostic log. The frame must hold
 * a complete Ethernet header.
 */
static inline uint32_t
__FrameEtherType(
    _In_ const uint8_t* frame)
{
    return (uint32_t)frame[GENET_ETHERTYPE_OFFSET] << 8 | frame[GENET_ETHERTYPE_OFFSET + 1];
}

oserr_t
GenetTransferSubmit(
    _In_ EthernetDevice_t* device,
    _In_ EthernetSlot_t*   slot)
{
    GenetController_t*         controller = device->Controller;
    GenetQueues_t*             queues = &controller->Queues;
    GenetTransmitQueue_t*      transmit = &queues->Transmit;
    GenetPostedReceiveSlots_t* posted = &queues->Receive.PostedSlots;
    uint32_t                   index;
    uint32_t                   length;
    uint32_t                   packetLength;
    uint32_t                   offset;
    oserr_t                    status;

    // A receive slot only waits here until a frame arrives for it.
    if (EthernetPacketIsReceive(slot)) {
        if (posted->Count == GENET_RING_COUNT) {
            return OS_EBUSY;
        }
        index = (posted->Head + posted->Count) % GENET_RING_COUNT;
        posted->Slots[index] = slot;
        posted->Count++;
        return OS_EOK;
    }

    if ((uint16_t)(transmit->Producer - transmit->Consumer) == GENET_RING_COUNT) {
        return OS_EBUSY;
    }

    // Copy the frame into its private buffer. The TX buffers come after the
    // RX buffers in the storage. A frame below the minimum size is padded
    // with zeros, so no old buffer contents go out on the wire.
    index = transmit->Producer % GENET_RING_COUNT;
    offset = (GENET_RING_COUNT + index) * GENET_BUFFER_SIZE;
    packetLength = EthernetPacketLength(slot);
    length = packetLength < ETHERNET_MIN_FRAME_SIZE ? ETHERNET_MIN_FRAME_SIZE : packetLength;
    memcpy(queues->Storage.Data + offset, EthernetPacketBytes(slot), packetLength);
    memset(queues->Storage.Data + offset + packetLength, 0, length - packetLength);

    status = GenetQueuesWriteDescriptor(
        controller,
        GENET_TX_DESCRIPTORS,
        index,
        queues->Storage.Address + offset,
        (length << GENET_DESCRIPTOR_LENGTH_SHIFT) | GENET_PACKET_FLAGS |
        GENET_TX_QUEUE_TAG_DEFAULT | GENET_TX_APPEND_CRC
    );
    if (status != OS_EOK) {
        // The producer index has not moved, so the card never looks at this
        // descriptor and the slot stays unused.
        ERROR("GENET: writing TX descriptor %u failed: %u", index, status);
        return status;
    }

    // A frame queued on an empty ring starts the stall clock. On a busy ring
    // the clock runs from the last finished frame.
    if (transmit->Producer == transmit->Consumer) {
        transmit->ProgressAt = DDKGetTimeMs(OSTimeSource_MONOTONIC);
    }
    transmit->Slots[index] = slot;
    transmit->Producer++;
    dma_mb();

    status = GenetRegisterWrite(
        &controller->Registers,
        GENET_TX_RING + GENET_RING_SOFTWARE_INDEX,
        (uint16_t)(transmit->Producer + transmit->HardwareBase)
    );
    if (status != OS_EOK) {
        // The producer write may have reached the card even though the access
        // reported a failure. The caller cannot tell, so it gets OS_EINPROGRESS.
        ERROR("GENET: writing the TX producer index failed (%u), the frame may still be sent", status);
        return OS_EINPROGRESS;
    }

    GenetLogPacket(controller, GENET_EVENT_TX_QUEUED, length,
        __FrameEtherType(queues->Storage.Data + offset));
    return OS_EOK;
}

/**
 * Hand one good frame to the oldest waiting client slot. The frame is dropped
 * and counted if it is not addressed to us, or if no slot is waiting.
 */
static void
__Receive(
    _In_ EthernetDevice_t* device,
    _In_ const uint8_t*    frame,
    _In_ uint32_t          length)
{
    GenetController_t*         controller = device->Controller;
    GenetPostedReceiveSlots_t* posted = &controller->Queues.Receive.PostedSlots;
    EthernetSlot_t*            slot;

    // Check the destination address in software as well. The hardware filters
    // are the only ones set up, and this guards against a frame slipping through.
    if (memcmp(frame, controller->Mac, GENET_MAC_ADDRESS_SIZE) &&
        memcmp(frame, g_broadcastAddress, GENET_MAC_ADDRESS_SIZE)) {
        GenetLogPacket(controller, GENET_EVENT_RX_REJECTED, length, 0);
        EthernetReportReceiveDrop(device, EthernetReceiveFiltered);
        return;
    }

    if (!posted->Count) {
        GenetLogPacket(controller, GENET_EVENT_RX_NO_BUFFER, length, 0);
        EthernetReportReceiveDrop(device, EthernetReceiveNoBuffer);
        return;
    }

    slot = posted->Slots[posted->Head];
    posted->Slots[posted->Head] = NULL;
    posted->Head = (posted->Head + 1) % GENET_RING_COUNT;
    posted->Count--;
    memcpy(EthernetPacketBytes(slot), frame, length);
    GenetLogPacket(controller, GENET_EVENT_RX_DELIVERED, length, __FrameEtherType(frame));
    EthernetComplete(device, slot, OS_EOK, length);
}

static oserr_t
__PollTransmit(
    _In_ EthernetDevice_t* device)
{
    GenetController_t*    controller = device->Controller;
    GenetTransmitQueue_t* transmit = &controller->Queues.Transmit;
    EthernetSlot_t*       slot;
    uint32_t              hardwareIndex;
    uint16_t              completed;
    uint16_t              count;
    uint32_t              index;
    uint32_t              length;
    uint64_t              now;

    hardwareIndex = GenetRegisterRead(
        &controller->Registers,
        GENET_TX_RING + GENET_RING_HARDWARE_INDEX
    );
    completed = (uint16_t)((hardwareIndex & GENET_RING_INDEX_MASK) - transmit->HardwareBase);

    // The card cannot finish more descriptors than we have handed to it.
    count = (uint16_t)(completed - transmit->Consumer);
    if (count > (uint16_t)(transmit->Producer - transmit->Consumer)) {
        ERROR("GENET: the TX ring index %u is %u frames ahead, more than were queued", completed, count);
        return OS_EPROTOCOL;
    }
    dma_mb();

    for (unsigned i = 0; i < count; i++) {
        index = transmit->Consumer % GENET_RING_COUNT;
        slot = transmit->Slots[index];
        transmit->Slots[index] = NULL;
        transmit->Consumer++;
        length = EthernetPacketLength(slot);
        GenetLogPacket(controller, GENET_EVENT_TX_COMPLETED, length, 0);
        EthernetComplete(device, slot, OS_EOK, length);
    }

    // Finished frames, and a down link, restart the stall clock. With the link
    // down the MAC keeps sending, so the ring should still drain, but nobody
    // has confirmed on a Pi 4 how fast it does that. A pulled cable must not
    // fault the session, so the stall check only counts time with the link up.
    now = DDKGetTimeMs(OSTimeSource_MONOTONIC);
    if (count != 0 || !controller->Link.Applied.Carrier) {
        transmit->ProgressAt = now;
    }

    if (transmit->Consumer != transmit->Producer &&
        now - transmit->ProgressAt > GENET_TX_STALL_TIMEOUT_MS) {
        ERROR("GENET: no TX progress for %u ms, producer=%u consumer=%u hardware=0x%08x",
            GENET_TX_STALL_TIMEOUT_MS, transmit->Producer, transmit->Consumer, hardwareIndex);
        return OS_ETIMEOUT;
    }
    return OS_EOK;
}

/**
 * Report frames the MAC dropped because the receive ring was full. The card
 * keeps this count in the upper half of its index register, so the loop that
 * walks the descriptors never sees it.
 */
static oserr_t
__ReportReceiveDiscards(
    _In_ EthernetDevice_t* device,
    _In_ uint32_t          hardwareIndex)
{
    GenetController_t*   controller = device->Controller;
    GenetReceiveQueue_t* receive = &controller->Queues.Receive;
    uint32_t             discards;
    oserr_t              status;

    discards = (hardwareIndex >> GENET_RING_DISCARD_SHIFT) & GENET_RING_DISCARD_MASK;
    if (discards <= receive->ReportedDiscards) {
        receive->ReportedDiscards = (uint16_t)discards;
        return OS_EOK;
    }

    for (uint32_t i = receive->ReportedDiscards; i < discards; i++) {
        GenetLogPacket(controller, GENET_EVENT_RX_RING_FULL, 0, 0);
        EthernetReportReceiveDrop(device, EthernetReceiveNoBuffer);
    }
    receive->ReportedDiscards = (uint16_t)discards;

    // Clear the counter well before it can overflow.
    if (discards < GENET_RING_DISCARD_RESET_THRESHOLD) {
        return OS_EOK;
    }
    status = GenetRegisterWrite(
        &controller->Registers,
        GENET_RX_RING + GENET_RING_HARDWARE_INDEX,
        0
    );
    if (status != OS_EOK) {
        ERROR("GENET: clearing the RX ring-full drop counter failed: %u", status);
        return status;
    }
    receive->ReportedDiscards = 0;
    return OS_EOK;
}

/**
 * Check the status word the card wrote in front of a received frame. A good
 * frame fills exactly one buffer (start and end flags both set), carries no
 * error bit, and has a length between an Ethernet header and a full frame.
 * The length the card reports includes the GENET_RX_PREFIX bytes in front.
 */
static bool
__ReceivedFrameIsValid(
    _In_ uint32_t frameStatus,
    _In_ uint32_t length)
{
    if ((frameStatus & GENET_PACKET_FLAGS) != GENET_PACKET_FLAGS) {
        return false;
    }
    if (frameStatus & GENET_RX_ERRORS) {
        return false;
    }
    return length >= GENET_RX_PREFIX + ETHERNET_HEADER_SIZE &&
           length <= GENET_RX_PREFIX + ETHERNET_FRAME_SIZE;
}

static oserr_t
__PollReceive(
    _In_ EthernetDevice_t* device)
{
    GenetController_t*   controller = device->Controller;
    GenetQueues_t*       queues = &controller->Queues;
    GenetReceiveQueue_t* receive = &queues->Receive;
    uint8_t*             buffer;
    uint32_t             hardwareIndex;
    uint16_t             completed;
    uint16_t             count;
    uint32_t             index;
    uint32_t             frameStatus;
    uint32_t             length;
    oserr_t              status;

    hardwareIndex = GenetRegisterRead(
        &controller->Registers,
        GENET_RX_RING + GENET_RING_HARDWARE_INDEX
    );
    status = __ReportReceiveDiscards(device, hardwareIndex);
    if (status != OS_EOK) {
        return status;
    }

    // The card can only be ahead by the size of its ring, no matter how many
    // client slots are waiting.
    completed = (uint16_t)((hardwareIndex & GENET_RING_INDEX_MASK) - receive->HardwareBase);
    count = (uint16_t)(completed - receive->DmaConsumer);
    if (count > GENET_RING_COUNT) {
        ERROR("GENET: the RX ring index %u is %u frames ahead, more than the ring holds", completed, count);
        return OS_EPROTOCOL;
    }
    dma_mb();

    for (unsigned i = 0; i < count; i++) {
        // The card writes a status word at the start of the buffer. Its upper
        // half is the frame length and its lower half holds the flags.
        index = receive->DmaConsumer % GENET_RING_COUNT;
        buffer = queues->Storage.Data + index * GENET_BUFFER_SIZE;
        memcpy(&frameStatus, buffer, sizeof(frameStatus));
        length = frameStatus >> GENET_DESCRIPTOR_LENGTH_SHIFT;
        if (__ReceivedFrameIsValid(frameStatus, length)) {
            GenetLogPacket(controller, GENET_EVENT_RX_RECEIVED, length - GENET_RX_PREFIX,
                __FrameEtherType(buffer + GENET_RX_PREFIX));
            __Receive(device, buffer + GENET_RX_PREFIX, length - GENET_RX_PREFIX);
        } else {
            GenetLogPacket(controller, GENET_EVENT_RX_ERROR, length, frameStatus);
            EthernetReportReceiveDrop(device, EthernetReceiveError);
        }
        receive->DmaConsumer++;
    }
    dma_mb();

    // Hand the handled descriptors back to the card.
    status = GenetRegisterWrite(
        &controller->Registers,
        GENET_RX_RING + GENET_RING_SOFTWARE_INDEX,
        (uint16_t)(receive->DmaConsumer + receive->HardwareBase)
    );
    if (status != OS_EOK) {
        ERROR("GENET: writing the RX consumer index failed: %u", status);
    }
    return status;
}

void
GenetTransferPoll(
    _In_ EthernetDevice_t* device)
{
    GenetController_t* controller = device->Controller;
    oserr_t            status;

    if (controller->Queues.DmaState != GENET_DMA_STATE_MAY_BE_RUNNING) {
        return;
    }

    // Both polls log what went wrong. The session is faulted here, because
    // the rings can no longer be trusted after either failure.
    status = __PollTransmit(device);
    if (status != OS_EOK) {
        EthernetFault(device, status);
        return;
    }

    status = __PollReceive(device);
    if (status != OS_EOK) {
        EthernetFault(device, status);
    }
}
