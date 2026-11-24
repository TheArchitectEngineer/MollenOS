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
 * Console
 * - Shows kernel log messages on the boot framebuffer. Messages are written
 *   to a text grid, and ConsoleDraw draws the rows that changed.
 * - Only the log reader job calls ConsoleShow* and ConsoleDraw. Inhibit and
 *   uninhibit come from protocol calls, which may run on another thread, so
 *   they only change an atomic state that ConsoleDraw reads.
 */

#ifndef __SYSLOGD_CONSOLE_H__
#define __SYSLOGD_CONSOLE_H__

#include <os/osdefs.h>
#include <os/types/syslog.h>

/**
 * @brief Maps the boot framebuffer and clears it. Needs the boot-video
 * capability.
 *
 * @return OS_EOK on success, OS_ENOTSUPPORTED when text cannot be drawn in the
 *         boot video mode (nothing is mapped then), or the error
 *         from querying or mapping the framebuffer or allocating the text grid.
 */
__EXTERN oserr_t
ConsoleInitialize(void);

/**
 * @brief Writes one message to the console as a line in the format the kernel
 * uses on serial: "<milliseconds> [<level>-<core>-<thread>] <text>". Trace
 * messages are not shown, because they would scroll the screen too fast.
 *
 * @param record Header of the message, as returned by OSKernelLogRead.
 * @param text   Zero terminated message text.
 */
__EXTERN void
ConsoleShowRecord(
    _In_ const OSKernelLogRecord_t* record,
    _In_ const char*                text);

/**
 * @brief Writes a line saying that count messages were dropped from the
 * kernel log before they could be shown.
 */
__EXTERN void
ConsoleShowDropped(
    _In_ uint64_t count);

/**
 * @brief Draws the rows that changed since the last draw. Does nothing while
 * the console is inhibited. After an uninhibit it clears the screen and draws
 * every row.
 */
__EXTERN void
ConsoleDraw(void);

/**
 * @brief Stops drawing, for example while another program shows something on
 * the screen. Messages are still written to the text grid.
 */
__EXTERN void
ConsoleInhibit(void);

/**
 * @brief Starts drawing again. The next ConsoleDraw redraws the whole screen.
 */
__EXTERN void
ConsoleUninhibit(void);

#endif //!__SYSLOGD_CONSOLE_H__
