/**
 * Copyright 2011, Philip Meulengracht
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
 * Kernel Log
 * - All kernel messages, and the messages userspace writes with the Debug
 *   system call, go into one ring buffer. Each message is also printed to the
 *   serial port, and to the screen while screen output is turned on.
 * - When the buffer is full, the oldest messages are dropped to make room.
 * - Userspace reads the buffer with LogRead, by sequence number.
 */

#ifndef __LOGGING_INTERFACE__
#define __LOGGING_INTERFACE__

#include <os/osdefs.h>
#include <os/types/syslog.h>

// Buffer sizes in bytes. Both must be powers of two, so that a byte position
// can be turned into a buffer offset with a mask.
#define LOG_BOOT_BUFFER_SIZE (16 * 1024)
#define LOG_FULL_BUFFER_SIZE (128 * 1024)

/**
 * @brief Sets up the log in a static boot buffer of LOG_BOOT_BUFFER_SIZE bytes
 * and initializes the serial port, if the platform has one. This can be called
 * before the memory systems are initialized.
 */
KERNELAPI void KERNELABI
LogInitialize(void);

/**
 * @brief Moves the log into a heap buffer of LOG_FULL_BUFFER_SIZE bytes. All
 * messages in the boot buffer are kept, with the same sequence numbers. The
 * heap must be initialized. If the allocation fails, the log stays in the
 * boot buffer and a warning is logged.
 */
KERNELAPI void KERNELABI
LogInitializeFull(void);

/**
 * @brief Turns printing of messages to the screen on or off. Serial output
 * does not depend on this. Turning it on also prints any messages that are
 * still waiting, for example the ones logged before the serial port was set up.
 * Has no effect after LogReleaseScreen.
 *
 * @param enable Nonzero to print to the screen, zero to stop.
 */
KERNELAPI void KERNELABI
LogSetRenderMode(
    _In_ int enable);

/**
 * @brief Stops printing messages to the screen, because a userspace process
 * mapped the boot framebuffer and now draws the log itself. LogSetRenderMode
 * cannot turn the screen back on after this. Only LogEnterPanicMode does.
 *
 * Messages are still stored and printed to the serial port. When there is no
 * serial port, they are only stored, for the process to read with LogRead.
 */
KERNELAPI void KERNELABI
LogReleaseScreen(void);

/**
 * @brief Switches the log to panic mode. Call it when the system stops after
 * a fatal error. The mode cannot be left again.
 *
 * In panic mode the log never waits on a lock that may never be released.
 * Messages that have not been printed yet are printed right away, screen
 * output is turned on (also after LogReleaseScreen), and each new message is
 * printed before the call returns.
 */
KERNELAPI void KERNELABI
LogEnterPanicMode(void);

/**
 * @brief Formats a message, adds it to the log and prints it.
 *
 * Text longer than OSKERNELLOG_MESSAGE_MAX bytes is cut off. If the log is
 * full, the oldest messages are dropped to make room. This can be called from
 * any context, including interrupt handlers.
 *
 * @param level  Log level of the message.
 * @param format printf-style format, followed by its arguments.
 */
KERNELAPI void KERNELABI
LogAppendMessage(
    _In_ enum OSSysLogLevel level,
    _In_ const char*        format, ...);

/**
 * @brief Copies whole log records into a kernel buffer, starting at the record
 * with the given sequence number.
 *
 * Records are copied in order until the next one does not fit. Each copied
 * record has the OSKernelLogRecord_t layout, with Size set to the bytes it
 * takes in the buffer. If messages from the requested sequence number on were
 * already dropped, copying starts at the oldest message still in the log, and
 * result->Dropped tells how many were skipped.
 *
 * @param sequence   Sequence number of the first record to copy. Use 0 to start
 *                   with the oldest message, or result->NextSequence from the
 *                   previous read to continue.
 * @param buffer     Kernel buffer to copy into.
 * @param bufferSize Size of buffer in bytes. OSKERNELLOG_RECORD_MAX always fits
 *                   at least one record.
 * @param result     Receives the next sequence number, the number of dropped
 *                   messages and the number of bytes copied.
 * @return OS_EOK when records were copied or no new records exist,
 *         OS_EINVALPARAMS for a NULL argument or a sequence number newer than
 *         any message, or OS_EBUFFER if the next record does not fit in buffer.
 */
KERNELAPI oserr_t KERNELABI
LogRead(
    _In_  uint64_t                 sequence,
    _In_  void*                    buffer,
    _In_  size_t                   bufferSize,
    _Out_ OSKernelLogReadResult_t* result);

#endif // !__LOGGING_INTERFACE__
