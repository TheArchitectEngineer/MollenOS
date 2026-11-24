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
 * Boot Video Query
 * - Hands the boot video mode to userspace. The kernel does not draw on the
 *   screen. The syslog service does.
 */

#ifndef __CONSOLE_INTERFACE_H__
#define __CONSOLE_INTERFACE_H__

#include <os/osdefs.h>
#include <os/types/video.h>

/**
 * @brief Copies the boot video mode. Userspace reads it through the system
 * query call, so the syslog service knows how to draw on the framebuffer.
 *
 * @param videoDescriptor Receives the boot video mode.
 * @return OS_EOK on success, or OS_ENOTSUPPORTED for a NULL argument.
 */
KERNELAPI oserr_t KERNELABI
VideoQuery(
	_Out_ OSBootVideoDescriptor_t *videoDescriptor);

#endif //!__CONSOLE_INTERFACE_H__
