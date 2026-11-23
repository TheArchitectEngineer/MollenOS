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
 * Access to the video mode and framebuffer that the bootloader set up.
 */

#include <internal/_syscalls.h>
#include <ddk/video.h>
#include <os/mollenos.h>

oserr_t
BootVideoQuery(
    _In_ OSBootVideoDescriptor_t* descriptor)
{
    size_t bytesQueried;

    return OSSystemQuery(
        OSSYSTEMQUERY_BOOTVIDEOINFO,
        descriptor,
        sizeof(OSBootVideoDescriptor_t),
        &bytesQueried
    );
}

void*
BootVideoMapFramebuffer(void)
{
    void*   framebuffer;
    oserr_t oserr;

    oserr = Syscall_MapBootFramebuffer(&framebuffer);
    if (oserr != OS_EOK) {
        return NULL;
    }
    return framebuffer;
}
