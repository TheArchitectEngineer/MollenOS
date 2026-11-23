/**
 * MollenOS
 *
 * Copyright 2020, Philip Meulengracht
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
 * Debugger support. Gives a debugger access to the memory of another thread.
 */

#ifndef __DDK_DEBUG_H__
#define __DDK_DEBUG_H__

#include <ddk/ddkdefs.h>

/**
 * @brief Maps two pages of the memory of another thread into the memory space
 * of the caller, starting at the page that holds address. A debugger uses this
 * to read the stack of a crashed thread. The mapping can be released with
 * MemoryFree. Requires the memory space management capability.
 *
 * @param threadHandle Handle of the thread to read from.
 * @param address      Address in the memory space of that thread, usually its
 *                     stack pointer.
 * @param topOfStack   Receives the end of the mapping. Memory from pointerOut
 *                     up to this address can be read.
 * @param pointerOut   Receives the address in the caller that matches address.
 * @return OS_EOK on success, OS_EINVALPARAMS for a missing output pointer,
 *         or an error from the kernel.
 */
DDKDECL(oserr_t,
DebugMapThreadMemory(
    _In_  uuid_t    threadHandle,
    _In_  uintptr_t address,
    _Out_ void**    topOfStack,
    _Out_ void**    pointerOut));

#endif //!__DDK_DEBUG_H__
