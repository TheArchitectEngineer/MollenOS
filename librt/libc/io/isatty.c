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

#include <errno.h>
#include <io.h>
#include <internal/_io.h>

int isatty(int fd)
{
    stdio_handle_t* handle = stdio_handle_get(fd);

    // A descriptor that is not open is not a terminal. isatty answers 0 and
    // sets errno, as marktty does for the same case. Returning EBADF itself
    // made the answer non-zero, which callers read as "is a terminal".
    if (!handle) {
        errno = EBADF;
        return 0;
    }
    return (handle->XTFlags & __IO_TTY) != 0;
}
