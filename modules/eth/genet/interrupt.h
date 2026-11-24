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
 * GENET packet interrupt. A small fast handler switches off (masks) the ring
 * 16 causes, writes them down and wakes the module loop. This file sets the
 * interrupt up, takes it down and hands the recorded causes to the service
 * code in service.h, which empties the rings and switches the causes back on.
 */

#ifndef __GENET_INTERRUPT_H__
#define __GENET_INTERRUPT_H__

#include <ddk/interrupts/interrupt.h>
#include <os/osdefs.h>
#include <stdatomic.h>
#include <stdbool.h>

DECL_STRUCT(GenetController);

/**
 * Index of the firmware interrupt that carries the bank 0 causes (INTRL2_0).
 * The Pi 4 device tree lists this one first. Bank 1 serves the other rings,
 * which this driver does not use.
 */
#define GENET_PACKET_INTERRUPT_INDEX 0U

/**
 * Memory the fast handler shares with the module. The kernel makes this range
 * available to the handler, so it must stay valid until the interrupt is
 * destroyed. Causes holds the GENET_INTR0_* bits the handler has masked but the
 * module has not yet picked up. Failures counts register writes and event
 * signals that failed inside the handler. The handler cannot log, so the
 * module reports the count for it.
 */
typedef struct GenetInterruptShared {
    atomic_uint Causes;
    atomic_uint Failures;
} GenetInterruptShared_t;

/**
 * GENET_INTERRUPT_EVENT_READY means the descriptor is open and in the module's
 * I/O set. The DDK Source tracks any partial handler setup.
 * GENET_INTERRUPT_ENABLED follows successful delivery setup. A failed destroy
 * preserves the state and descriptor for retry.
 */
enum GenetInterruptState {
    GENET_INTERRUPT_NONE,
    GENET_INTERRUPT_EVENT_READY,
    GENET_INTERRUPT_ENABLED
};

/** Packet interrupt resources and setup state, protected by the adapter lock. */
typedef struct GenetInterrupt {
    DeviceInterrupt_t        Source;
    GenetInterruptShared_t   Shared;
    int                      EventDescriptor;
    enum GenetInterruptState State;
} GenetInterrupt_t;

/**
 * @brief Fast handler for the packet causes. Runs inside the kernel's interrupt
 * handling, so it must stay short.
 * Masks and clears every enabled cause that is set, which makes the interrupt
 * line go quiet again. It then passes the packet causes on through the shared
 * memory and the event. Resource 0 supplies the register window and the shared
 * memory.
 * @param interruptTable Kernel functions for register access and event signaling.
 * @param context Resource table prepared for this handler.
 * @return IRQSTATUS_HANDLED if a cause was pending, otherwise IRQSTATUS_NOT_HANDLED.
 */
__EXTERN irqstatus_t
GenetFastInterrupt(
    _In_ InterruptFunctionTable_t* interruptTable,
    _In_ void*                     context);

/**
 * @brief Set up the packet interrupt and start delivering it to the module loop.
 * Creates the event descriptor, adds it to the loop's I/O set, installs the
 * fast handler and turns on delivery. The card's causes stay masked until the
 * queues start. If a step fails, whatever was set up is left for
 * GenetInterruptUnregister to clean up.
 * @param controller GENET state with mapped registers, protected by the adapter lock.
 * @return OS_EOK, or the first setup error.
 */
__EXTERN oserr_t
GenetInterruptRegister(
    _InOut_ GenetController_t* controller);

/**
 * @brief Mask the card's causes, stop delivery and give the interrupt back.
 * Safe to call when nothing was registered. If a step fails, the remaining
 * state is kept so the call can be repeated. Call it before the register
 * window is unmapped and before the platform device is stopped.
 * @param controller GENET state protected by the adapter lock.
 * @return OS_EOK, or the error that needs another attempt.
 */
__EXTERN oserr_t
GenetInterruptUnregister(
    _InOut_ GenetController_t* controller);

/**
 * @brief Check if an I/O set event belongs to the packet interrupt.
 * @param controller GENET state protected by the adapter lock.
 * @param descriptor Descriptor reported by the I/O set.
 * @return true if the descriptor is the registered event descriptor.
 */
__EXTERN bool
GenetInterruptIsEvent(
    _In_ const GenetController_t* controller,
    _In_ int                      descriptor);

/**
 * @brief Take the packet causes the fast handler has recorded and set the
 * record back to zero. A cause the handler records after this call is kept
 * for the next one. Also reports the register writes and event signals that
 * failed inside the handler, which cannot log for itself.
 * @param controller GENET state protected by the adapter lock.
 * @return GENET_INTR0_* bits the handler masked since the last call. Zero if
 *         there are none, or if interrupts are not in use.
 */
__EXTERN uint32_t
GenetInterruptTakeCauses(
    _InOut_ GenetController_t* controller);

/**
 * @brief Set the event descriptor back to "not signaled" after a wake-up.
 * Call it before GenetInterruptTakeCauses. If the handler runs after this
 * point it signals again, so its causes always get a wake-up. A failure is
 * logged and not returned, because the causes are picked up either way.
 * @param controller GENET state with a registered packet interrupt, protected
 *                   by the adapter lock.
 */
__EXTERN void
GenetInterruptResetEvent(
    _InOut_ GenetController_t* controller);

#endif //!__GENET_INTERRUPT_H__
