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
 * Logging to the system log. The NOTICE, WARNING, ERROR and TRACE macros in
 * os/syslog.h end up here.
 */

#include <internal/_syscalls.h>
#include <os/syslog.h>
#include <stdarg.h>
#include <stdio.h>

void
OSSysLogWrite(
    _In_ enum OSSysLogLevel level,
    _In_ const char*        format, ...)
{
    va_list args;
    char    buffer[256];

    // A message longer than the buffer is cut off. vsnprintf always ends it
    // with a zero byte.
    va_start(args, format);
    vsnprintf(&buffer[0], sizeof(buffer) - 1, format, args);
    va_end(args);

    Syscall_Debug(level, &buffer[0]);
}
