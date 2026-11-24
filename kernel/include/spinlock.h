/**
 * MollenOS
 *
 * Copyright 2019, Philip Meulengracht
 *
 * This program is free software : you can redistribute it and / or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation ? , either version 3 of the License, or
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
 *
 * Synchronization (Irq Spinlocks)
 * - Spinlock implementation that also disables interrupts.
 */

#ifndef __VALI_IRQ_SPINLOCK_H__
#define __VALI_IRQ_SPINLOCK_H__

#include <os/osdefs.h>

typedef struct Spinlock {
    irqstate_t            IrqState;
    _Atomic(unsigned int) Current;
    _Atomic(unsigned int) Next;
} Spinlock_t;

#define OS_SPINLOCK_INIT { 0, 0, 0 }

/**
 * @brief
 * @param spinlock
 */
KERNELAPI void KERNELABI
SpinlockConstruct(
        _In_ Spinlock_t* spinlock);

/**
 * @brief
 * @param spinlock
 */
KERNELAPI void KERNELABI
SpinlockAcquire(
        _In_ Spinlock_t* spinlock);

/**
 * @brief
 * @param spinlock
 */
KERNELAPI void KERNELABI
SpinlockRelease(
        _In_ Spinlock_t* spinlock);

/**
 * @brief
 * @param spinlock
 */
KERNELAPI void KERNELABI
SpinlockAcquireIrq(
        _In_ Spinlock_t* spinlock);

/**
 * @brief Takes the lock and disables interrupts, but only if the lock is free
 * right now. It never waits for another holder.
 *
 * Use it where waiting could deadlock, for example while the system is
 * stopping after a fatal error and the holder may never release the lock.
 * Release a lock taken this way with SpinlockReleaseIrq.
 *
 * @param spinlock The lock to take.
 * @return OS_EOK if the lock was taken, or OS_EBUSY if it is held or another
 *         caller is waiting for it. Interrupts are left as they were on
 *         OS_EBUSY.
 */
KERNELAPI oserr_t KERNELABI
SpinlockTryAcquireIrq(
        _In_ Spinlock_t* spinlock);

/**
 * @brief
 * @param spinlock
 */
KERNELAPI void KERNELABI
SpinlockReleaseIrq(
        _In_ Spinlock_t* spinlock);

#endif //!__VALI_IRQ_SPINLOCK_H__
