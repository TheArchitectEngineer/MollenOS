/**
 * Copyright 2023, Philip Meulengracht
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
 */

#ifndef __OS_MEMORY_SPACE_H__
#define __OS_MEMORY_SPACE_H__

#include <os/osdefs.h>
#include <os/types/memory.h>

_CODE_BEGIN

// Where and how OSMemorySpaceCreateMapping places a mapping in the target
// memory space. Flags takes the MEMORY_* values of os/types/memory.h.
struct MemoryMappingParameters {
    uintptr_t    VirtualAddress;
    size_t       Length;
    unsigned int Flags;
};

/**
 * @brief Creates a new memory space that can be used to create new mappings, and manipulate existing mappings.
 *
 * @param flags             Memory-space creation flags.
 * @param msHandleOut       Receives the handle of the memory space on success. Unchanged on failure.
 * @param securityHandleOut Receives the security-group ID on success. Unchanged on failure.
 * @return OS_EOK on success, OS_EINVALPARAMS for a missing output pointer,
 *         or an error from the kernel.
 */
CRTDECL(oserr_t,
OSMemorySpaceCreate(
    _In_  unsigned int flags,
    _Out_ uuid_t*      msHandleOut,
    _Out_ uuid_t*      securityHandleOut));

/**
 * @brief Retrieves the memory space that is currently running for the thread handle.
 *
 * @param thread    Handle of the thread to look up.
 * @param handleOut Receives the handle of the memory space on success.
 * @return OS_EOK on success, OS_EINVALPARAMS for a missing output pointer,
 *         or an error from the kernel.
 */
CRTDECL(oserr_t,
OSMemorySpaceGetForThread(
    _In_  uuid_t  thread,
    _Out_ uuid_t* handleOut));

/**
 * @brief Creates a new memory mapping in the memory space. The new memory is also
 * mapped into the memory space of the caller, so the caller can fill it.
 *
 * @param handle     Handle of the memory space to create the mapping in.
 * @param parameters Address, length and flags of the mapping.
 * @param addressOut Receives the address the caller can use to access the memory.
 * @return OS_EOK on success, OS_EINVALPARAMS for a missing pointer,
 *         or an error from the kernel.
 */
CRTDECL(oserr_t,
OSMemorySpaceCreateMapping(
    _In_  uuid_t                          handle,
    _In_  struct MemoryMappingParameters* parameters,
    _Out_ void**                          addressOut));

_CODE_END

#endif //!__OS_MEMORY_SPACE_H__
