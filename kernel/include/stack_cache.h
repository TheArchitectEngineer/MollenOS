/**
 * Copyright 2026, Philip Meulengracht
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
 * Kernel stack cache
 * - Keeps the kernel stacks of destroyed threads mapped, so the next thread
 *   can use one without mapping a new stack. Unmapping a kernel stack sends
 *   a memory synchronization interrupt to every other running core (MSSync),
 *   and asynchronous system calls create and destroy a thread for each wait.
 */

#ifndef __VALI_STACK_CACHE_H__
#define __VALI_STACK_CACHE_H__

#include <os/osdefs.h>

/**
 * Maximum number of stacks the cache keeps. When the cache is full, a freed
 * stack is unmapped as before. 64 stacks of 32 KiB (AArch64) keep 2 MiB mapped.
 */
#define KERNEL_STACK_CACHE_LIMIT 64

/**
 * @brief Takes a kernel stack from the cache.
 *
 * Only stacks of THREADING_KERNEL_STACK_SIZE are cached. A cached stack is
 * mapped in the global kernel range with every page committed, and its guard
 * page below the stack is still unmapped. Its contents are left over from the
 * previous thread, so the caller must set up any context it needs.
 *
 * @param stackSize Size of the wanted stack in bytes, without the guard page.
 * @param stackTopOut Receives the address just above the stack on success.
 * @return OS_EOK on success, OS_ENOENT if the cache has no stack of this size
 *         (the caller then maps a new stack), or OS_EINVALPARAMS if stackTopOut
 *         is NULL.
 */
KERNELAPI oserr_t KERNELABI
StackCacheAllocate(
        _In_  size_t     stackSize,
        _Out_ uintptr_t* stackTopOut);

/**
 * @brief Gives the kernel stack of a destroyed thread to the cache.
 *
 * Precondition: no core runs on the stack anymore. ThreadingFinish guarantees
 * this before a thread can be destroyed. The stack must be a fully committed
 * kernel stack, mapped the way the architecture's stack allocation maps a
 * THREADING_CONTEXT_LEVEL0 stack. The cache writes its list entry into the top
 * bytes of the stack.
 *
 * @param stackTop Address just above the stack.
 * @param stackSize Size of the stack in bytes, without the guard page.
 * @return OS_EOK if the cache keeps the stack. Otherwise the stack is unchanged
 *         and the caller must unmap it: OS_EBUSY if the cache is full,
 *         OS_ENOTSUPPORTED if stacks of this size are not cached, or
 *         OS_EINVALPARAMS if stackTop is 0.
 */
KERNELAPI oserr_t KERNELABI
StackCacheRelease(
        _In_ uintptr_t stackTop,
        _In_ size_t    stackSize);

#endif //!__VALI_STACK_CACHE_H__
