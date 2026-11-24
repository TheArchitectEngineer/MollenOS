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
 * UniMAC reset, address, receive filters and interrupt masks. See mac.h.
 */

#include <ddk/utils.h>
#include <string.h>

#include "genet.h"

oserr_t
GenetMacInitialize(
    _In_ GenetController_t* controller)
{
    const uint8_t* mac = controller->Mac;
    const GenetRegisterSetting_t settings[] = {
        // Reset the MAC itself. The command register must rest before its next write.
        {GENET_COMMAND, GENET_COMMAND_RESET},
        GENET_REGISTER_WAIT(GENET_MAC_COMMAND_SETTLE_US),

        // Clear out old FIFO contents before any new descriptor buffers are
        // assigned. Each flush bit is held for a moment and then cleared.
        {GENET_TX_FLUSH_CTRL, GENET_TX_FLUSH},
        GENET_REGISTER_WAIT(GENET_MAC_FLUSH_SETTLE_US),
        {GENET_TX_FLUSH_CTRL, 0},
        {GENET_SYS_RBUF_FLUSH_CTRL, GENET_SYS_RBUF_FLUSH},
        GENET_REGISTER_WAIT(GENET_MAC_FLUSH_SETTLE_US),
        {GENET_SYS_RBUF_FLUSH_CTRL, 0},
        GENET_REGISTER_WAIT(GENET_MAC_FLUSH_SETTLE_US),

        // Mask the interrupts first, then clear whatever is already pending, in each bank.
        {GENET_INTR0_MASK_SET, GENET_INTR_ALL},
        {GENET_INTR0_CLEAR, GENET_INTR_ALL},
        {GENET_INTR1_MASK_SET, GENET_INTR_ALL},
        {GENET_INTR1_CLEAR, GENET_INTR_ALL},

        // Received frames get a 64-byte status block and two padding bytes in front.
        // Sent frames are plain.
        {GENET_RBUF_CTRL, GENET_RBUF_STATUS_64B | GENET_RBUF_ALIGN_2B},
        {GENET_RBUF_CHECK_CTRL, GENET_RBUF_L3_PARSE_DISABLE},
        {GENET_RBUF_TBUF_SIZE_CTRL, GENET_RBUF_TBUF_SIZE_V5},
        {GENET_TBUF_CTRL, 0},

        // Turn off energy-efficient Ethernet (EEE) in both FIFOs and the MAC.
        {GENET_RBUF_ENERGY_CTRL, 0},
        {GENET_TBUF_ENERGY_CTRL, 0},
        {GENET_EEE_CTRL, 0},

        // Reset the statistics counters, then set the frame size limit and the MAC address.
        {GENET_MIB_CTRL, GENET_MIB_RESET_RX | GENET_MIB_RESET_RUNT | GENET_MIB_RESET_TX},
        {GENET_MIB_CTRL, 0},
        {GENET_MAX_FRAME_LENGTH, ETHERNET_FRAME_SIZE + ETHERNET_FCS_SIZE},
        {GENET_MAC_ADDRESS0,
            (uint32_t)mac[0] << 24 | (uint32_t)mac[1] << 16 | (uint32_t)mac[2] << 8 | mac[3]},
        {GENET_MAC_ADDRESS1, (uint32_t)mac[4] << 8 | mac[5]},

        // Turn off the hardware frame filter. Frames that no filter claims go to ring 16.
        {GENET_HFB_CTRL, 0},
        {GENET_HFB_ENABLE_HIGH, 0},
        {GENET_HFB_ENABLE_LOW, 0},

        // Destination filters 0 and 1 accept broadcast frames and frames for our MAC address.
        {GENET_MDF_ADDRESS_HIGH(0), GENET_MDF_BROADCAST_HIGH},
        {GENET_MDF_ADDRESS_LOW(0), GENET_MDF_BROADCAST_LOW},
        {GENET_MDF_ADDRESS_HIGH(1), (uint32_t)mac[0] << 8 | mac[1]},
        {GENET_MDF_ADDRESS_LOW(1),
            (uint32_t)mac[2] << 24 | (uint32_t)mac[3] << 16 | (uint32_t)mac[4] << 8 | mac[5]},
        {GENET_MDF_CTRL, GENET_MDF_ENABLE(0) | GENET_MDF_ENABLE(1)},

        // Let the MAC rest before the command write that ends this function.
        GENET_REGISTER_WAIT(GENET_MAC_COMMAND_SETTLE_US)
    };
    oserr_t status;

    // The order matters. The wrapper reset comes first, because the UniMAC
    // command register cannot be used while the wrapper holds the MAC in
    // reset. The MAC reset and the FIFO flushes follow, and only then are the
    // settings written, since the reset would wipe them.
    status = GenetWrapperAssertReset(&controller->Wrapper, &controller->Registers);
    if (status != OS_EOK) {
        return status;
    }

    status = GenetWrapperReleaseReset(&controller->Wrapper, &controller->Registers);
    if (status != OS_EOK) {
        return status;
    }

    status = GenetRegisterWriteList(
        &controller->Registers,
        0,
        settings,
        sizeof(settings) / sizeof(settings[0])
    );
    if (status != OS_EOK) {
        ERROR("GENET MAC: the reset and setup sequence failed: %u", status);
        return status;
    }

    // The reset dropped the speed and duplex mode, so no link is applied now.
    // The command value below leaves the transmitter and receiver off.
    memset(&controller->Link.Applied, 0, sizeof(controller->Link.Applied));
    status = GenetRegisterWrite(
        &controller->Registers,
        GENET_COMMAND,
        GENET_COMMAND_PAUSE_IGNORE
    );
    if (status != OS_EOK) {
        ERROR("GENET MAC: writing the idle command value failed: %u", status);
    }
    return status;
}

oserr_t
GenetMacMaskInterrupts(
    _In_ GenetController_t* controller)
{
    if (controller->Interrupt.State != GENET_INTERRUPT_ENABLED) {
        return OS_EOK;
    }
    return GenetRegisterWrite(&controller->Registers, GENET_INTR0_MASK_SET, GENET_INTR_ALL);
}

oserr_t
GenetMacUnmaskDmaInterrupts(
    _In_ GenetController_t* controller,
    _In_ uint32_t           causes)
{
    if (controller->Interrupt.State != GENET_INTERRUPT_ENABLED) {
        return OS_EOK;
    }
    return GenetRegisterWrite(
        &controller->Registers,
        GENET_INTR0_MASK_CLEAR,
        causes & GENET_INTR0_DMA_DONE
    );
}
