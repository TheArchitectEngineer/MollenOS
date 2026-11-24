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
 * Logging to the system log. OSSysLogWrite formats one message and hands it to
 * the kernel log. OSKernelLogRead reads the messages back. The macros below are what code normally uses. TRACE, ENTRY
 * and EXIT only produce output in a source file that defines __TRACE before
 * it includes this header.
 */

#ifndef __OS_SYSLOG_H__
#define __OS_SYSLOG_H__

#include <os/osdefs.h>
#include <os/types/syslog.h>

/* Global <always-on> definitions
 * These are enabled no matter which kind of debugging is enabled */
#define STR(str)               str
#define NOTICE(...)            OSSysLogWrite(OSSYSLOGLEVEL_TRACE, __VA_ARGS__)
#define DEBUG(...)             OSSysLogWrite(OSSYSLOGLEVEL_DEBUG, __VA_ARGS__)
#define WARNING(...)           OSSysLogWrite(OSSYSLOGLEVEL_WARNING, __VA_ARGS__)
#define WARNING_IF(cond, ...)  { if ((cond)) { OSSysLogWrite(OSSYSLOGLEVEL_WARNING, __VA_ARGS__); } }
#define WARNING_ONCE(id, ...)  { \
                                    static _Atomic(int) __gi_ ##id ## _trigger = 0; \
                                    if (!atomic_exchange(&__gi_ ##id ## _trigger, 1)) { \
                                        OSSysLogWrite(OSSYSLOGLEVEL_WARNING, __VA_ARGS__); \
                                    } \
                               }
#define ERROR(...)	           OSSysLogWrite(OSSYSLOGLEVEL_ERROR, __VA_ARGS__)
#define TODO(msg)              OSSysLogWrite(OSSYSLOGLEVEL_WARNING, "TODO: %s, line %d, %s", __FILE__, __LINE__, msg)

/**
 * Global <toggable> definitions
 * These can be turned on per-source file by pre-defining the __TRACE before inclusion
 */
#ifdef __TRACE
#include <time.h>
#define __TS_DIFF_MS(start, end) (((end.tv_sec - start.tv_sec) * MSEC_PER_SEC) + ((end.tv_nsec - start.tv_nsec) / NSEC_PER_MSEC))

#define TRACE(...) OSSysLogWrite(OSSYSLOGLEVEL_TRACE, __VA_ARGS__)
#define ENTRY(...) struct timespec start, end; OSSysLogWrite(OSSYSLOGLEVEL_TRACE, __VA_ARGS__); timespec_get(&start, TIME_MONOTONIC)
#define EXIT(msg)  timespec_get(&end, TIME_MONOTONIC); OSSysLogWrite(OSSYSLOGLEVEL_TRACE, msg " completed in %llu ms", __TS_DIFF_MS(start, end))
#else
#define TRACE(...)
#define ENTRY(...)
#define EXIT(msg)
#endif

_CODE_BEGIN

/**
 * @brief Formats a message and writes it to the system log.
 *
 * @param level  Log level of the message, one of the OSSYSLOGLEVEL_* values.
 * @param format printf-style format, followed by its arguments. The formatted
 *               message is cut off after OSKERNELLOG_MESSAGE_MAX
 *               characters.
 */
CRTDECL(void,
OSSysLogWrite(
    _In_ enum OSSysLogLevel level,
    _In_ const char*        format, ...));

/**
 * @brief Copies whole messages from the kernel log into buffer, oldest first,
 * starting at the message with the given sequence number. Each message is an
 * OSKernelLogRecord_t header followed by its text. Step to the next message by
 * adding the header's Size. The caller needs the syslog-read capability.
 *
 * @param sequence   Sequence number of the first message to copy. Use 0 for
 *                   the oldest message in the log, or result->NextSequence
 *                   from the previous call to continue.
 * @param buffer     Buffer that receives the messages.
 * @param bufferSize Size of buffer in bytes. At most 16 KiB is filled per call.
 *                   OSKERNELLOG_RECORD_MAX bytes always fit one message.
 * @param result     Receives the next sequence number, the number of messages
 *                   that were dropped from the log before they could be read,
 *                   and the number of bytes written to buffer.
 * @return OS_EOK when messages were copied or no new ones exist,
 *         OS_EBUFFER if the next message does not fit in buffer,
 *         OS_EPERMISSIONS without the syslog-read capability, or OS_EINVALPARAMS
 *         for bad arguments or a sequence number newer than any message.
 */
CRTDECL(oserr_t,
OSKernelLogRead(
    _In_  uint64_t                 sequence,
    _In_  void*                    buffer,
    _In_  size_t                   bufferSize,
    _Out_ OSKernelLogReadResult_t* result));

_CODE_END

#endif //!__OS_SYSLOG_H__
