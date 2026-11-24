/**
 * Copyright 2016, Philip Meulengracht
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

#include <arch/output.h>
#include <console.h>
#include <string.h>

oserr_t
VideoQuery(
	_Out_ OSBootVideoDescriptor_t* videoDescriptor)
{
	if (videoDescriptor == NULL || VideoGetTerminal() == NULL) {
		return OS_ENOTSUPPORTED;
	}

	memcpy(videoDescriptor, &VideoGetTerminal()->Info, sizeof(OSBootVideoDescriptor_t));
	return OS_EOK;
}
