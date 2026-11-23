/* MollenOS
 *
 * Copyright 2011 - 2017, Philip Meulengracht
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
 * Video Definitions & Structures
 * - This header describes the video-structure, prototypes
 *   and functionality, refer to the individual things for descriptions
 */

#ifndef __DDK_VIDEO_H__
#define __DDK_VIDEO_H__

#include <ddk/ddkdefs.h>
#include <os/types/video.h>

_CODE_BEGIN

/**
 * @brief Reads the video mode that the bootloader set up.
 *
 * @param descriptor Receives the boot video mode on success.
 * @return OS_EOK on success, or an error from the kernel.
 */
DDKDECL(oserr_t,
BootVideoQuery(
    _In_ OSBootVideoDescriptor_t* descriptor));

/**
 * @brief Maps the framebuffer of the boot video mode into the memory space of
 * the caller, so the caller can draw to the screen directly.
 *
 * @return The address of the framebuffer, or NULL when it could not be mapped.
 */
DDKDECL(void*,
BootVideoMapFramebuffer(void));

_CODE_END
#endif //!__DDK_VIDEO_H__
