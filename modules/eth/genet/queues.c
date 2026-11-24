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
 * Queue setup, start and stop for ring 16. See queues.h.
 *
 * Logging: each failure is logged once, where it is detected. A call into
 * another GENET function that logs its own failures is passed on as it is.
 */

#include <os/barrier.h>
#include <ddk/utils.h>
#include <string.h>

#include "genet.h"

// Time the stop waits between stopping the TX engine and stopping the RX
// engine, so frames already in flight can finish. The Linux driver waits
// 10 to 20 ms here (bcmgenet_dma_teardown).
#define GENET_STOP_DRAIN_US     10000U

// A DMA engine is asked to stop and then checked this many times, with this
// wait between two checks. Together that is 10 ms before the stop gives up.
#define GENET_DMA_STOP_CHECKS   1000U
#define GENET_DMA_STOP_CHECK_US 10U

// A CMake variable can set the default thresholds. Fail the build if a
// default would later be rejected by GenetThresholdsValidate.
_Static_assert(GENET_DEFAULT_TX_DONE_FRAMES >= 1 && GENET_DEFAULT_TX_DONE_FRAMES <= GENET_RING_COUNT,
    "TX done frames must be 1 up to the ring size");
_Static_assert(GENET_DEFAULT_RX_DONE_FRAMES <= GENET_RING_COUNT,
    "RX done frames must not exceed the ring size");
_Static_assert(GENET_DEFAULT_RX_TIMEOUT_US <= GENET_RX_TIMEOUT_MAX_US,
    "RX timeout is longer than the hardware counter holds");
_Static_assert(GENET_DEFAULT_RX_DONE_FRAMES == 1 || GENET_DEFAULT_RX_TIMEOUT_US != 0,
    "RX done frames other than one need a nonzero RX timeout");

oserr_t
GenetQueuesWriteDescriptor(
    _In_ GenetController_t* controller,
    _In_ uint32_t           base,
    _In_ uint32_t           index,
    _In_ uint64_t           address,
    _In_ uint32_t           flags)
{
    uint32_t offset = base + index * GENET_DESCRIPTOR_SIZE;
    oserr_t  status;

    // A failure is logged by the caller, which knows what the descriptor is for.
    status = GenetRegisterWrite(
        &controller->Registers,
        offset + GENET_DESCRIPTOR_ADDRESS_LOW,
        (uint32_t)address
    );
    if (status != OS_EOK) {
        return status;
    }

    status = GenetRegisterWrite(
        &controller->Registers,
        offset + GENET_DESCRIPTOR_ADDRESS_HIGH,
        address >> 32
    );
    if (status != OS_EOK) {
        return status;
    }

    return GenetRegisterWrite(
        &controller->Registers,
        offset + GENET_DESCRIPTOR_STATUS,
        flags
    );
}

static oserr_t
__ProgramReceiveTimeout(
    _In_ GenetController_t* controller)
{
    uint32_t offset = GENET_RX_DMA + GENET_DMA_RING16_TIMEOUT;
    uint32_t value;

    // Only the low 16 bits are ours to set. The card uses the rest of the
    // register, so those bits are left as they were.
    value = GenetRegisterRead(&controller->Registers, offset);
    value &= ~GENET_DMA_TIMEOUT_MASK;
    value |= GenetThresholdsTimeoutTicks(controller->Thresholds.RxTimeoutUs) & GENET_DMA_TIMEOUT_MASK;
    return GenetRegisterWrite(&controller->Registers, offset, value);
}

static oserr_t
__InitializeRing(
    _In_ GenetController_t* controller,
    _In_ uint32_t           base,
    _In_ bool               receive)
{
    // thresholds.h explains what the done threshold means for each direction.
    uint32_t doneFrames = receive ? controller->Thresholds.RxFrames : controller->Thresholds.TxFrames;
    oserr_t  status;

    const GenetRegisterSetting_t settings[] = {
        {GENET_RING_HARDWARE_POINTER_LOW, 0},
        {GENET_RING_HARDWARE_POINTER_HIGH, 0},
        {GENET_RING_HARDWARE_INDEX, 0},
        {GENET_RING_SOFTWARE_INDEX, 0},
        {GENET_RING_BUFFER_SIZE, (GENET_RING_COUNT << GENET_RING_COUNT_SHIFT) | GENET_BUFFER_SIZE},
        {GENET_RING_START_ADDRESS_LOW, 0},
        {GENET_RING_START_ADDRESS_HIGH, 0},
        // Descriptor addresses count 32-bit words, not bytes. The end address
        // is the last word that belongs to the ring.
        {GENET_RING_END_ADDRESS_LOW, GENET_RING_COUNT * GENET_DESCRIPTOR_WORDS - 1U},
        {GENET_RING_END_ADDRESS_HIGH, 0},
        {GENET_RING_DONE_THRESHOLD, doneFrames & GENET_RING_DONE_THRESHOLD_MASK},
        {GENET_RING_FLOW_CONTROL, receive ? GENET_RING_RX_FLOW_THRESHOLDS : 0},
        {GENET_RING_SOFTWARE_POINTER_LOW, 0},
        {GENET_RING_SOFTWARE_POINTER_HIGH, 0},

        // Configure ring 16 before its buffers are enabled. The DMA engine itself
        // stays stopped.
        {GENET_RING_REGISTER_SIZE + GENET_DMA_RING_CONFIG, GENET_DMA_DEFAULT_RING_CONFIG},
        {GENET_RING_REGISTER_SIZE + GENET_DMA_BURST_SIZE, GENET_DMA_BURST_LENGTH},
        {GENET_RING_REGISTER_SIZE + GENET_DMA_CONTROL, GENET_DMA_DEFAULT_RING_ENABLE}
    };

    status = GenetRegisterWriteList(
        &controller->Registers,
        base,
        settings,
        sizeof(settings) / sizeof(settings[0])
    );
    if (status != OS_EOK) {
        ERROR("GENET: setting up the %s ring failed: %u", receive ? "RX" : "TX", status);
        return status;
    }

    if (!receive) {
        return OS_EOK;
    }

    status = __ProgramReceiveTimeout(controller);
    if (status != OS_EOK) {
        ERROR("GENET: writing the RX ring timeout failed: %u", status);
    }
    return status;
}

/**
 * Line our counters up with the card's ring indices. Ring setup writes zero to
 * the hardware index registers, as the Linux driver does. The U-Boot driver
 * notes that these writes do not always take on this chip, and starts from
 * whatever the card reports instead (rx_ring_init and tx_ring_init). This does
 * the same, so a leftover index from the bootloader or an earlier run cannot
 * look like a burst of finished frames. Our counters still start at zero,
 * because the descriptor positions restart at zero when the ring pointers are
 * written. Only the values written to and read from the index registers are
 * moved by the base.
 */
static oserr_t
__AlignRingIndices(
    _In_ GenetController_t* controller)
{
    GenetQueues_t* queues = &controller->Queues;
    uint32_t       transmitIndex;
    uint32_t       receiveIndex;
    oserr_t        status;

    transmitIndex = GenetRegisterRead(&controller->Registers, GENET_TX_RING + GENET_RING_HARDWARE_INDEX);
    receiveIndex = GenetRegisterRead(&controller->Registers, GENET_RX_RING + GENET_RING_HARDWARE_INDEX);
    queues->Transmit.HardwareBase = (uint16_t)(transmitIndex & GENET_RING_INDEX_MASK);
    queues->Receive.HardwareBase = (uint16_t)(receiveIndex & GENET_RING_INDEX_MASK);

    // The upper half of the RX index counts ring-full drops. Drops the card
    // counted before this run are not reported as new ones.
    queues->Receive.ReportedDiscards =
        (uint16_t)((receiveIndex >> GENET_RING_DISCARD_SHIFT) & GENET_RING_DISCARD_MASK);
    if (transmitIndex == 0 && receiveIndex == 0) {
        return OS_EOK;
    }

    // This has never been seen on a Pi 4, so say so when it happens.
    WARNING("GENET: ring indices kept their old values after setup (TX=0x%08x RX=0x%08x), using them as the start",
        transmitIndex, receiveIndex);
    status = GenetRegisterWrite(
        &controller->Registers,
        GENET_TX_RING + GENET_RING_SOFTWARE_INDEX,
        queues->Transmit.HardwareBase
    );
    if (status != OS_EOK) {
        ERROR("GENET: writing the TX producer index 0x%x failed: %u", queues->Transmit.HardwareBase, status);
        return status;
    }

    status = GenetRegisterWrite(
        &controller->Registers,
        GENET_RX_RING + GENET_RING_SOFTWARE_INDEX,
        queues->Receive.HardwareBase
    );
    if (status != OS_EOK) {
        ERROR("GENET: writing the RX consumer index 0x%x failed: %u", queues->Receive.HardwareBase, status);
        return status;
    }
    return OS_EOK;
}

oserr_t
GenetQueuesPrepare(
    _In_ EthernetDevice_t* device)
{
    GenetController_t* controller = device->Controller;
    GenetQueues_t*     queues = &controller->Queues;
    uint32_t           i;
    oserr_t            status;

    // Reject threshold values the card cannot use, before touching the hardware.
    // GenetThresholdsValidate logs the rule that failed.
    status = GenetThresholdsValidate(&controller->Thresholds, GENET_RING_COUNT);
    if (status != OS_EOK) {
        return status;
    }

    // The engines must be off before the MAC is reset, and the MAC must be
    // reset before the rings are written, because the reset flushes the FIFOs
    // the rings feed. Both steps log their own failures.
    status = GenetQueuesStop(device);
    if (status != OS_EOK) {
        return status;
    }

    status = GenetMacInitialize(controller);
    if (status != OS_EOK) {
        return status;
    }

    // The storage is kept across restarts and freed when the controller is destroyed.
    if (queues->Storage.Buffer == NULL) {
        status = EthernetDmaAllocate(controller->Dma, GENET_STORAGE_SIZE, &queues->Storage);
        if (status != OS_EOK) {
            ERROR("GENET: allocating %u bytes of frame storage failed: %u", GENET_STORAGE_SIZE, status);
            return status;
        }
    }

    memset(queues->Storage.Data, 0, GENET_STORAGE_SIZE);

    // Every RX descriptor gets its buffer now. TX descriptors are written as
    // frames are queued.
    for (i = 0; i < GENET_RING_COUNT; i++) {
        status = GenetQueuesWriteDescriptor(
            controller,
            GENET_RX_DESCRIPTORS,
            i,
            queues->Storage.Address + i * GENET_BUFFER_SIZE,
            0
        );
        if (status != OS_EOK) {
            ERROR("GENET: writing RX descriptor %u failed: %u", i, status);
            return status;
        }
    }

    status = __InitializeRing(controller, GENET_RX_RING, true);
    if (status != OS_EOK) {
        return status;
    }

    status = __InitializeRing(controller, GENET_TX_RING, false);
    if (status != OS_EOK) {
        return status;
    }

    status = __AlignRingIndices(controller);
    if (status != OS_EOK) {
        return status;
    }

    ETHERNET_LOG("GENET queues prepared: ring=16 entries=%u buffer-bytes=%u DMA=0x%llx",
        GENET_RING_COUNT, GENET_BUFFER_SIZE, (unsigned long long)queues->Storage.Address);
    ETHERNET_LOG("GENET thresholds: TX-frames=%u RX-frames=%u RX-timeout-us=%u ticks=%u",
        controller->Thresholds.TxFrames, controller->Thresholds.RxFrames,
        controller->Thresholds.RxTimeoutUs,
        GenetThresholdsTimeoutTicks(controller->Thresholds.RxTimeoutUs));
    return OS_EOK;
}

oserr_t
GenetQueuesStart(
    _In_ EthernetDevice_t* device)
{
    GenetController_t* controller = device->Controller;
    struct PhyLink     link;
    oserr_t            status;

    ETHERNET_LOG("GENET queues start");

    dma_mb();

    // Note that DMA may be running before either enable write can reach the
    // card. Cleanup must then confirm the engines stopped.
    controller->Queues.DmaState = GENET_DMA_STATE_MAY_BE_RUNNING;
    status = GenetRegisterWrite(
        &controller->Registers,
        GENET_RX_DMA + GENET_DMA_CONTROL,
        GENET_DMA_DEFAULT_RING_ENABLE | GENET_DMA_ENABLE
    );
    if (status != OS_EOK) {
        ERROR("GENET start: enabling the RX DMA engine failed: %u", status);
        return status;
    }

    status = GenetRegisterWrite(
        &controller->Registers,
        GENET_TX_DMA + GENET_DMA_CONTROL,
        GENET_DMA_DEFAULT_RING_ENABLE | GENET_DMA_ENABLE
    );
    if (status != OS_EOK) {
        ERROR("GENET start: enabling the TX DMA engine failed: %u", status);
        return status;
    }

    // Both engines are running now. Unmasking last keeps the interrupt handler
    // from seeing queues that are only half started.
    status = GenetMacUnmaskDmaInterrupts(controller, GENET_INTR0_DMA_DONE);
    if (status != OS_EOK) {
        ERROR("GENET start: unmasking the packet interrupt causes failed: %u", status);
        return status;
    }

    // A read that fails within the limit gives a down link. Apply it anyway,
    // so the applied link no longer matches a link that was applied before
    // the start, and the next good read switches MAC traffic on. Both calls
    // log their own failures.
    status = GenetLinkReadPhy(controller, &link);
    if (status != OS_EOK) {
        return status;
    }
    return GenetLinkApply(controller, &link);
}

/**
 * Stop one DMA engine and wait until the card reports that it stopped.
 * @param base GENET_RX_DMA or GENET_TX_DMA.
 * @param name "RX" or "TX", for the log.
 * @return OS_EOK once the engine is off, OS_ETIMEOUT if it is still running
 *         after GENET_DMA_STOP_CHECKS checks, or a register or delay error.
 */
static oserr_t
__StopDma(
    _In_ GenetController_t* controller,
    _In_ uint32_t           base,
    _In_ const char*        name)
{
    uint32_t value;
    oserr_t  status;

    ETHERNET_LOG("GENET DMA stop: base=0x%x control read begin", base);
    value = GenetRegisterRead(&controller->Registers, base + GENET_DMA_CONTROL);

    ETHERNET_LOG("GENET DMA stop: base=0x%x control=0x%08x disable write begin", base, value);
    status = GenetRegisterWrite(
        &controller->Registers,
        base + GENET_DMA_CONTROL,
        value & ~GENET_DMA_ENABLE
    );
    if (status != OS_EOK) {
        ERROR("GENET stop: clearing the %s DMA enable bit failed: %u", name, status);
        return status;
    }

    ETHERNET_LOG("GENET DMA stop: base=0x%x status poll begin", base);
    for (unsigned i = 0; i < GENET_DMA_STOP_CHECKS; i++) {
        if (GenetRegisterRead(&controller->Registers, base + GENET_DMA_STATUS) & GENET_DMA_DISABLED) {
            // The engine is off. Clear the ring enable bits as well.
            status = GenetRegisterWrite(&controller->Registers, base + GENET_DMA_CONTROL, 0);
            if (status != OS_EOK) {
                ERROR("GENET stop: clearing the %s DMA control register failed: %u", name, status);
            }
            return status;
        }

        if (i == 0) {
            ETHERNET_LOG("GENET DMA stop: base=0x%x not idle, delay begin", base);
        }

        status = DDKStallUs(GENET_DMA_STOP_CHECK_US);
        if (status != OS_EOK) {
            ERROR("GENET stop: waiting for the %s DMA engine failed: %u", name, status);
            return status;
        }
    }

    ERROR("GENET stop: the %s DMA engine did not stop, control=0x%08x status=0x%08x",
        name, GenetRegisterRead(&controller->Registers, base + GENET_DMA_CONTROL),
        GenetRegisterRead(&controller->Registers, base + GENET_DMA_STATUS));
    return OS_ETIMEOUT;
}

oserr_t
GenetQueuesStop(
    _In_ EthernetDevice_t* device)
{
    GenetController_t* controller = device->Controller;
    GenetQueues_t*     queues = &controller->Queues;
    uint32_t           command;
    oserr_t            status;

    if (!GenetRegistersMapped(controller)) {
        return OS_EOK;
    }

    // Setup may have failed while releasing the wrapper reset. Cleanup has to
    // finish that step before it can use the UniMAC command register.
    if (controller->Wrapper.Reset != GENET_WRAPPER_RESET_RELEASED) {
        status = GenetWrapperReleaseReset(&controller->Wrapper, &controller->Registers);
        if (status != OS_EOK) {
            return status;
        }
    }

    // Silence the interrupt before the engines stop, so the handler cannot run
    // while the queues are being taken down.
    status = GenetMacMaskInterrupts(controller);
    if (status != OS_EOK) {
        ERROR("GENET stop: masking the interrupt causes failed: %u", status);
        return status;
    }

    // The order below follows the Linux driver (bcmgenet_netif_stop and
    // bcmgenet_dma_teardown). The transmitter must stay on until the TX engine
    // has stopped. With frames still queued and the transmitter off, the TX
    // engine may never report that it stopped.
    //
    // Step 1: switch the receiver off. This also runs during creation, on
    // whatever the bootloader left in the command register, so only the
    // receive enable bit is cleared here.
    command = GenetRegisterRead(&controller->Registers, GENET_COMMAND);
    ETHERNET_LOG("GENET DMA stop: UniMAC disable write begin");
    status = GenetRegisterWrite(
        &controller->Registers,
        GENET_COMMAND,
        command & ~GENET_COMMAND_RX_ENABLE
    );
    if (status != OS_EOK) {
        ERROR("GENET stop: switching the MAC receiver off failed: %u", status);
        return status;
    }

    // Step 2: stop the TX engine and wait until it reports that it stopped.
    ETHERNET_LOG("GENET DMA stop: UniMAC disable write complete");
    status = __StopDma(controller, GENET_TX_DMA, "TX");
    if (status != OS_EOK) {
        return status;
    }

    // Step 3: give frames in flight time to finish.
    status = DDKStallUs(GENET_STOP_DRAIN_US);
    if (status != OS_EOK) {
        ERROR("GENET stop: waiting for frames in flight failed: %u", status);
        return status;
    }

    // Step 4: stop the RX engine.
    status = __StopDma(controller, GENET_RX_DMA, "RX");
    if (status != OS_EOK) {
        return status;
    }

    // Step 5: switch the transmitter off. This writes the plain value, which
    // also clears any speed and enable bits the bootloader left.
    status = GenetRegisterWrite(
        &controller->Registers,
        GENET_COMMAND,
        GENET_COMMAND_PAUSE_IGNORE
    );
    if (status != OS_EOK) {
        ERROR("GENET stop: switching the MAC transmitter off failed: %u", status);
        return status;
    }
    dma_mb();

    // Both engines are off, so the card no longer uses the frame storage and
    // every slot reference can be dropped. The storage itself is kept.
    ETHERNET_LOG("GENET queues stopped: DMA idle confirmed");
    queues->DmaState = GENET_DMA_STATE_STOPPED;
    memset(&queues->Transmit, 0, sizeof(queues->Transmit));
    memset(&queues->Receive, 0, sizeof(queues->Receive));
    return OS_EOK;
}
