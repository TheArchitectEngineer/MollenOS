/**
 * Copyright 2022, Philip Meulengracht
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
 * The system log. Messages go to the kernel log, the same as with
 * OSSysLogWrite. The syslog service reads them from the kernel log and draws
 * them on the screen, so it never needs to receive them directly.
 */

#include <os/services/syslog.h>
#include <os/syslog.h>
#include <stdarg.h>
#include <stdio.h>

void
OSSystemLog(
        _In_ enum OSSysLogLevel level,
        _In_ const char*        fmt, ...)
{
    va_list args;
    char    buffer[OSKERNELLOG_MESSAGE_MAX + 1];
    int     written;

    // vsnprintf cuts a long message off and always ends it with a zero byte.
    va_start(args, fmt);
    written = vsnprintf(&buffer[0], sizeof(buffer), fmt, args);
    va_end(args);
    if (written <= 0) {
        return;
    }
    OSSysLogWrite(level, "%s", &buffer[0]);
}
