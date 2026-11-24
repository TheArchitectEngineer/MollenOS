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
 * BCM2711 GENET v5 register map and register access. Offsets are in bytes
 * from the start of the register window. Ring and DMA fields are relative to
 * the start of their own block. Every register is 32 bits wide.
 */

#ifndef __GENET_REGISTERS_H__
#define __GENET_REGISTERS_H__

#include <ddk/io.h>
#include <os/barrier.h>

#define GENET_REGISTER_SIZE 4U
// The register window must be at least this large to reach every register below.
#define GENET_REGISTER_WINDOW_SIZE 0x10000U

// Wrapper revision, port mode, reset and RGMII link control. The wrapper is the
// glue logic around the MAC.
#define GENET_SYS_REVISION_CTRL        0x000U
// Bits 27:24 hold the major revision. GENET v5 reports 6 here, as the Linux driver notes.
#define GENET_SYS_REVISION_MAJOR_SHIFT 24U
#define GENET_SYS_REVISION_MAJOR_MASK  0xfU
#define GENET_SYS_REVISION_MAJOR_V5    6U
#define GENET_SYS_PORT_CTRL            0x004U
// Port mode 3 routes the MAC to an external gigabit PHY, which is how the Pi 4 is wired.
#define GENET_SYS_PORT_MODE_EXT_GPHY   3U
#define GENET_SYS_RBUF_FLUSH_CTRL      0x008U
#define GENET_SYS_RBUF_FLUSH           (1U << 0)
#define GENET_SYS_MAC_RESET            (1U << 1)
#define GENET_RGMII_OOB_CTRL           0x08CU
// Tells the MAC that the PHY has a link. Linux clears only this bit when the link goes down.
#define GENET_RGMII_LINK               (1U << 4)
// Turns off the in-band (out-of-band status) link signalling. The PHY is read over MDIO instead.
#define GENET_RGMII_OOB_DISABLE        (1U << 5)
#define GENET_RGMII_MODE_ENABLE        (1U << 6)
// Set: the MAC adds no transmit clock delay. Cleared: the MAC adds it.
#define GENET_RGMII_ID_MODE_DISABLE    (1U << 16)

// Interrupt banks. Every source stays masked (switched off) except the ring 16
// packet causes in bank 0. Those are unmasked only while the queues run with
// interrupt delivery. A masked cause still shows up in the status register,
// and fires as soon as it is unmasked.
#define GENET_INTR0_STATUS      0x200U
#define GENET_INTR0_CLEAR       0x208U
#define GENET_INTR0_MASK_STATUS 0x20cU
#define GENET_INTR0_MASK_SET    0x210U
#define GENET_INTR0_MASK_CLEAR  0x214U
#define GENET_INTR1_MASK_SET    0x250U
#define GENET_INTR1_CLEAR       0x248U
#define GENET_INTR_ALL          UINT32_MAX

// Ring 16 raises these when it has handled as many frames as its done threshold.
#define GENET_INTR0_RX_DONE  (1U << 13)
#define GENET_INTR0_TX_DONE  (1U << 16)
#define GENET_INTR0_DMA_DONE (GENET_INTR0_RX_DONE | GENET_INTR0_TX_DONE)

// Receive and transmit FIFO (buffer) layout and energy saving control.
#define GENET_RBUF_CTRL             0x300U
#define GENET_RBUF_STATUS_64B       (1U << 0)
#define GENET_RBUF_ALIGN_2B         (1U << 1)
#define GENET_RBUF_CHECK_CTRL       0x314U
#define GENET_RBUF_L3_PARSE_DISABLE (1U << 5)
#define GENET_RBUF_ENERGY_CTRL      0x39CU
#define GENET_RBUF_TBUF_SIZE_CTRL   0x3B4U
#define GENET_RBUF_TBUF_SIZE_V5     1U
#define GENET_TBUF_CTRL             0x600U
#define GENET_TBUF_ENERGY_CTRL      0x614U
#define GENET_RX_STATUS_SIZE        64U
#define GENET_RX_ALIGNMENT_SIZE     2U
#define GENET_RX_PREFIX             (GENET_RX_STATUS_SIZE + GENET_RX_ALIGNMENT_SIZE)

// UniMAC (the MAC block) configuration, address and counters.
#define GENET_COMMAND              0x808U
#define GENET_COMMAND_TX_ENABLE    (1U << 0)
#define GENET_COMMAND_RX_ENABLE    (1U << 1)
#define GENET_COMMAND_TRAFFIC      (GENET_COMMAND_TX_ENABLE | GENET_COMMAND_RX_ENABLE)
#define GENET_COMMAND_SPEED_SHIFT  2U
#define GENET_COMMAND_SPEED_100    (1U << GENET_COMMAND_SPEED_SHIFT)
#define GENET_COMMAND_SPEED_1000   (2U << GENET_COMMAND_SPEED_SHIFT)
#define GENET_COMMAND_HALF_DUPLEX  (1U << 10)
#define GENET_COMMAND_RESET        (1U << 13)
#define GENET_COMMAND_PAUSE_IGNORE ((1U << 8) | (1U << 28))
#define GENET_MAC_ADDRESS0         0x80cU
#define GENET_MAC_ADDRESS1         0x810U
#define GENET_MAX_FRAME_LENGTH     0x814U
#define GENET_EEE_CTRL             0x864U
#define GENET_TX_FLUSH_CTRL        0xb34U
#define GENET_TX_FLUSH             (1U << 0)
#define GENET_MIB_CTRL             0xd80U
#define GENET_MIB_RESET_RX         (1U << 0)
#define GENET_MIB_RESET_RUNT       (1U << 1)
#define GENET_MIB_RESET_TX         (1U << 2)

// UniMAC MDIO block. The firmware must describe it at this offset with this length.
// A transaction is started by writing the command register with the busy bit
// set. The card clears the busy bit when the transaction is done, and sets the
// failed bit if a read got no answer from the PHY. The low 16 bits carry the data.
#define GENET_MDIO_COMMAND             0xe14U
#define GENET_MDIO_WINDOW_SIZE         8U
#define GENET_MDIO_BUSY                (1U << 29)
#define GENET_MDIO_FAILED              (1U << 28)
#define GENET_MDIO_OPERATION_READ      (2U << 26)
#define GENET_MDIO_OPERATION_WRITE     (1U << 26)
#define GENET_MDIO_OPERATION_MASK      (3U << 26)
#define GENET_MDIO_PHY_SHIFT           21U
#define GENET_MDIO_REGISTER_SHIFT      16U
#define GENET_MDIO_DATA_MASK           0xffffU
#define GENET_MDIO_CONFIG              0xe18U
// Selects the Clause 22 register access format.
#define GENET_MDIO_CONFIG_CLAUSE22     (1U << 0)
#define GENET_MDIO_CLOCK_DIVIDER_SHIFT 4U
#define GENET_MDIO_CLOCK_DIVIDER_MASK  (0x3fU << GENET_MDIO_CLOCK_DIVIDER_SHIFT)
// The BCM2711 reference clock runs at 250 MHz. Dividing it by 2 * (49 + 1)
// gives the 2.5 MHz management bus clock (MDC).
#define GENET_MDIO_CLOCK_DIVIDER_PI4   49U
// Set: no preamble before each transaction. The driver keeps the preamble.
#define GENET_MDIO_SUPPRESS_PREAMBLE   (1U << 12)

// MAC destination filters. Each address takes a 16-bit register first, then a 32-bit one.
#define GENET_MDF_CTRL                0xe50U
#define GENET_MDF_ADDRESS_HIGH(index) (0xe54U + (index) * 8U)
#define GENET_MDF_ADDRESS_LOW(index)  (0xe58U + (index) * 8U)
#define GENET_MDF_ENABLE(index)       (1U << (16U - (index)))
#define GENET_MDF_BROADCAST_HIGH      0xffffU
#define GENET_MDF_BROADCAST_LOW       UINT32_MAX
#define GENET_HFB_CTRL                0xfc00U
#define GENET_HFB_ENABLE_HIGH         0xfc04U
#define GENET_HFB_ENABLE_LOW          0xfc08U

// Ring 16 is the default ring. It takes every frame that no filter has claimed.
// Descriptor memory holds 256 entries.
#define GENET_DEFAULT_RING            16U
#define GENET_RX_DESCRIPTORS          0x2000U
#define GENET_TX_DESCRIPTORS          0x4000U
#define GENET_DESCRIPTOR_WORDS        3U
#define GENET_DESCRIPTOR_SIZE         (GENET_DESCRIPTOR_WORDS * GENET_REGISTER_SIZE)
#define GENET_DESCRIPTOR_STATUS       0x00U
#define GENET_DESCRIPTOR_ADDRESS_LOW  0x04U
#define GENET_DESCRIPTOR_ADDRESS_HIGH 0x08U
#define GENET_DESCRIPTOR_LENGTH_SHIFT 16U
#define GENET_DESCRIPTOR_SOP          (1U << 13)
#define GENET_DESCRIPTOR_EOP          (1U << 14)
#define GENET_PACKET_FLAGS            (GENET_DESCRIPTOR_SOP | GENET_DESCRIPTOR_EOP)
#define GENET_TX_APPEND_CRC           (1U << 6)
#define GENET_TX_QUEUE_TAG_SHIFT      7U
#define GENET_TX_QUEUE_TAG_DEFAULT    (63U << GENET_TX_QUEUE_TAG_SHIFT)
// RX error bits: overflow, bad CRC (checksum), receive error, frame that does not
// end on a whole byte, and frame that is too long.
#define GENET_RX_ERRORS               0x1fU

#define GENET_RX_RING                      0x3000U
#define GENET_TX_RING                      0x5000U
#define GENET_RING_REGISTER_SIZE           0x40U
// The card moves the RX write pointer (producer) and the TX read pointer (consumer).
#define GENET_RING_HARDWARE_POINTER_LOW    0x00U
#define GENET_RING_HARDWARE_POINTER_HIGH   0x04U
#define GENET_RING_HARDWARE_INDEX          0x08U
// The upper half of the RX hardware index counts frames the MAC dropped because
// the ring was full. Writing zero clears it. The driver clears it at 75% of its
// range, as Linux does, so it can never overflow.
#define GENET_RING_DISCARD_SHIFT           16U
#define GENET_RING_DISCARD_MASK            0xffffU
#define GENET_RING_DISCARD_RESET_THRESHOLD 0xc000U
// The driver moves the RX consumer and the TX producer.
#define GENET_RING_SOFTWARE_INDEX          0x0cU
#define GENET_RING_BUFFER_SIZE             0x10U
#define GENET_RING_COUNT_SHIFT             16U
#define GENET_RING_START_ADDRESS_LOW       0x14U
#define GENET_RING_START_ADDRESS_HIGH      0x18U
#define GENET_RING_END_ADDRESS_LOW         0x1cU
#define GENET_RING_END_ADDRESS_HIGH        0x20U
#define GENET_RING_DONE_THRESHOLD          0x24U
// Nine bits. The number of frames to finish before the ring raises its done cause.
#define GENET_RING_DONE_THRESHOLD_MASK     0x1ffU
// Offset 0x28 holds the RX pause (XON/XOFF) thresholds, or the TX flow period.
#define GENET_RING_FLOW_CONTROL            0x28U
#define GENET_RING_XOFF_THRESHOLD_SHIFT    16U
#define GENET_RING_RX_FLOW_THRESHOLDS      ((5U << GENET_RING_XOFF_THRESHOLD_SHIFT) | 16U)
// Starting value of the RX read pointer and the TX write pointer.
#define GENET_RING_SOFTWARE_POINTER_LOW    0x2cU
#define GENET_RING_SOFTWARE_POINTER_HIGH   0x30U
#define GENET_RING_INDEX_MASK              0xffffU

// The DMA engine registers come right after the last ring's register block.
#define GENET_RX_DMA                  (GENET_RX_RING + GENET_RING_REGISTER_SIZE)
#define GENET_TX_DMA                  (GENET_TX_RING + GENET_RING_REGISTER_SIZE)
#define GENET_DMA_RING_CONFIG         0x00U
#define GENET_DMA_CONTROL             0x04U
#define GENET_DMA_STATUS              0x08U
#define GENET_DMA_BURST_SIZE          0x0cU
// RX only. Raises the done cause when frames are waiting but the done threshold
// has not been reached. The TX engine has no timeout register. The low 16 bits
// count steps of 8.192 us (a 125 MHz clock divided by 1024). The upper bits
// belong to the hardware and must be left as they are.
#define GENET_DMA_RING16_TIMEOUT      0x6cU
#define GENET_DMA_TIMEOUT_MASK        0xffffU
#define GENET_DMA_TIMEOUT_TICK_NS     8192U
#define GENET_DMA_DEFAULT_RING_CONFIG (1U << GENET_DEFAULT_RING)
#define GENET_DMA_DEFAULT_RING_ENABLE (1U << (GENET_DEFAULT_RING + 1U))
#define GENET_DMA_ENABLE              (1U << 0)
#define GENET_DMA_DISABLED            (1U << 0)
#define GENET_DMA_BURST_LENGTH        8U

// The card addresses 40 bits of memory. DMA buffers must lie below this limit.
#define GENET_DMA_ADDRESS_BITS 40U
#define GENET_DMA_ADDRESS_MASK ((1ULL << GENET_DMA_ADDRESS_BITS) - 1U)

/**
 * @brief One step in a list that is run in array order. A step is either a
 * register write or a wait.
 * For a write, Offset is relative to the base passed to GenetRegisterWriteList
 * and Value is the value to write. For a wait, Offset is
 * GENET_REGISTER_WAIT_OFFSET and Value is the time in microseconds. Use
 * GENET_REGISTER_WAIT to build a wait step.
 */
typedef struct GenetRegisterSetting {
    uint32_t Offset;
    uint32_t Value;
} GenetRegisterSetting_t;

// No register lives at this offset, so it can mark the wait steps of a list.
#define GENET_REGISTER_WAIT_OFFSET UINT32_MAX
// A list step that waits the given time, for registers that need time to take effect.
#define GENET_REGISTER_WAIT(microseconds) {GENET_REGISTER_WAIT_OFFSET, (microseconds)}

/**
 * @brief Read a 32-bit GENET register. Memory barriers keep the access in order
 * with the accesses around it.
 * @param registers Mapped GENET window.
 * @param offset Byte offset of the register inside the window. Must be 4-byte aligned.
 * @return Register contents.
 */
static inline uint32_t
GenetRegisterRead(
    _In_ DeviceIo_t* registers,
    _In_ size_t      offset)
{
    uint32_t value;

    mb();
    value = (uint32_t)DeviceIoRead(registers, offset, GENET_REGISTER_SIZE);
    mb();
    return value;
}

/**
 * @brief Write a 32-bit GENET register. Memory barriers keep the access in order
 * with the accesses around it.
 * @param registers Mapped GENET window.
 * @param offset Byte offset of the register inside the window. Must be 4-byte aligned.
 * @param value Value to write.
 * @return Register access status. The hardware may have changed even if this fails.
 */
static inline oserr_t
GenetRegisterWrite(
    _In_ DeviceIo_t* registers,
    _In_ size_t      offset,
    _In_ uint32_t    value)
{
    oserr_t status;

    mb();
    status = DeviceIoWrite(registers, offset, value, GENET_REGISTER_SIZE);
    mb();
    return status;
}

/**
 * @brief Run a list of register writes and waits in array order. Stops at the
 * first failure and logs the step that failed.
 * @param registers Mapped GENET window.
 * @param base Byte offset added to the Offset of every write in the list. Zero
 *             for registers given by their offset in the window.
 * @param settings Steps to run, first entry first.
 * @param count Number of entries in settings.
 * @return OS_EOK, or the first register write or delay error. Steps before
 *         the failed one have been run.
 */
__EXTERN oserr_t
GenetRegisterWriteList(
    _In_ DeviceIo_t*                   registers,
    _In_ uint32_t                      base,
    _In_ const GenetRegisterSetting_t* settings,
    _In_ size_t                        count);

#endif //!__GENET_REGISTERS_H__
