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
 * UniMAC setup. UniMAC is the MAC block inside GENET, the part that builds
 * and reads Ethernet frames. This file resets it, programs its address and
 * receive filters, and switches the interrupt causes of bank 0 on and off.
 */

#ifndef __GENET_MAC_H__
#define __GENET_MAC_H__

#include <os/osdefs.h>

DECL_STRUCT(GenetController);

// Bytes in an Ethernet MAC address.
#define GENET_MAC_ADDRESS_SIZE      6U

// Time the MAC needs after a write to its command register that resets it or
// changes the speed and duplex mode, before the next command write.
#define GENET_MAC_COMMAND_SETTLE_US 2U

// Time a FIFO flush bit is held, and the time the FIFO gets after the bit is
// cleared again.
#define GENET_MAC_FLUSH_SETTLE_US   10U

/**
 * @brief Reset a stopped MAC and flush its FIFOs, then set up the MAC address
 * and the receive filters. Every interrupt cause is left masked, and the
 * applied link is cleared, because the reset drops the speed and duplex mode.
 * @param controller GENET state protected by the adapter lock. The DMA engines
 *                   must be stopped and Mac must hold the address to program.
 * @return OS_EOK, or the first register write or delay error.
 */
__EXTERN oserr_t
GenetMacInitialize(
    _In_ GenetController_t* controller);

/**
 * @brief Mask (switch off) every interrupt cause. Does nothing while interrupts
 * are not in use. Call it before stopping the DMA engines, so no cause can fire
 * during the stop.
 * @param controller GENET state protected by the adapter lock.
 * @return Register write status.
 */
__EXTERN oserr_t
GenetMacMaskInterrupts(
    _In_ GenetController_t* controller);

/**
 * @brief Unmask (switch on) the ring 16 packet causes. Does nothing while
 * interrupts are not in use. A cause that is already set fires right away.
 * @param controller GENET state protected by the adapter lock.
 * @param causes GENET_INTR0_* bits to unmask. Bits that are not packet causes are ignored.
 * @return Register write status.
 */
__EXTERN oserr_t
GenetMacUnmaskDmaInterrupts(
    _In_ GenetController_t* controller,
    _In_ uint32_t           causes);

#endif //!__GENET_MAC_H__
