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
 * Types shared between the kernel log and the programs that write to it or
 * read from it.
 */

#ifndef __OS_TYPES_SYSLOG_H__
#define __OS_TYPES_SYSLOG_H__

#include <stdint.h>

enum OSSysLogLevel {
    OSSYSLOGLEVEL_TRACE,
    OSSYSLOGLEVEL_DEBUG,
    OSSYSLOGLEVEL_WARNING,
    OSSYSLOGLEVEL_ERROR,
};

/**
 * Longest message text the kernel log stores, in bytes, not counting the zero
 * byte. Longer messages are cut off at this length.
 */
#define OSKERNELLOG_MESSAGE_MAX 255

/**
 * @brief Header of one message in the kernel log.
 *
 * The message text follows right after the header. It is Length bytes long
 * and does not end with a zero byte. Size covers the header, the text and the
 * unused bytes after the text that keep the next header aligned to 8 bytes.
 * Step from one record to the next by adding Size to the record address.
 *
 * Every message gets a Sequence number one higher than the message before it.
 * A reader uses it to ask for the messages after the last one it has seen, and
 * to notice when older messages were dropped before it could read them.
 */
typedef struct OSKernelLogRecord {
    uint32_t Size;
    uint16_t Length;
    uint8_t  Level;     // enum OSSysLogLevel
    uint8_t  Reserved;
    uint32_t CoreID;
    uint32_t ThreadID;
    uint64_t Sequence;
    uint64_t Timestamp; // Nanoseconds since the system timer started.
} OSKernelLogRecord_t;

/**
 * Most bytes a single record can take: the header, the longest text, and up
 * to 7 bytes that align the next record. A read buffer of this size always has
 * room for at least one record.
 */
#define OSKERNELLOG_RECORD_MAX \
    ((sizeof(OSKernelLogRecord_t) + OSKERNELLOG_MESSAGE_MAX + 7) & ~(sizeof(uint64_t) - 1))

/**
 * @brief Result of reading from the kernel log.
 *
 * NextSequence is the sequence number to pass to the next read. Dropped counts
 * the messages that were removed from the log before they could be read,
 * because newer messages needed the space. BytesRead is the number of bytes of
 * records written to the read buffer.
 */
typedef struct OSKernelLogReadResult {
    uint64_t NextSequence;
    uint64_t Dropped;
    uint32_t BytesRead;
    uint32_t Reserved;
} OSKernelLogReadResult_t;

#endif //!__OS_TYPES_SYSLOG_H__
