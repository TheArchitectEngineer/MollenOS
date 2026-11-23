/**
 * Copyright 2017, Philip Meulengracht
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

#ifndef __DDK_INITRD_H__
#define __DDK_INITRD_H__

#include <ddk/ddkdefs.h>

// forward declare this to avoid any vafs includes
struct VaFsReaderConfiguration;

/**
 * @brief Initializes reader configuration with the OS initrd's aPLib codec.
 * Call before opening the image; descriptor blocks can already be compressed.
 *
 * @param[Out] configuration The reader configuration to initialize.
 */
DDKDECL(void,
InitrdConfigureVafsReader(
    _Out_ struct VaFsReaderConfiguration* configuration));

/**
 * @brief Maps the boot ramdisk (initrd) into the memory space of the caller.
 * The mapping can be released again with MemoryFree.
 *
 * @param bufferOut       Receives the address of the ramdisk.
 * @param bufferLengthOut Receives the size of the ramdisk in bytes.
 * @return OS_EOK on success, OS_EINVALPARAMS for a missing output pointer,
 *         or an error from the kernel.
 */
DDKDECL(oserr_t,
InitrdMap(
    _Out_ void**  bufferOut,
    _Out_ size_t* bufferLengthOut));

#endif //!__DDK_INITRD_H__
