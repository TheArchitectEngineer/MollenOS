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
 * Maps the boot ramdisk (initrd) into the memory space of the caller.
 */

#include <internal/_syscalls.h>
#include <ddk/utils.h>

oserr_t
DdkUtilsMapRamdisk(
    _Out_ void**  bufferOut,
    _Out_ size_t* bufferLengthOut)
{
    if (!bufferOut || !bufferLengthOut) {
        return OS_EINVALPARAMS;
    }
    return Syscall_MapRamdisk(bufferOut, bufferLengthOut);
}
