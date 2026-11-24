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
 * Log Reader
 * - Reads the kernel log with OSKernelLogRead, from the oldest message on,
 *   and passes each message to the console.
 */

#ifndef __SYSLOGD_READER_H__
#define __SYSLOGD_READER_H__

#include <os/osdefs.h>

// Bytes read from the kernel log per call. This is the most the kernel fills
// in one call.
#define LOG_READER_BUFFER_SIZE (16 * 1024)

/**
 * @brief Where the reader is in the kernel log.
 * NextSequence is the sequence number of the next message to read. Buffer
 * holds LOG_READER_BUFFER_SIZE bytes and receives the records of one read.
 */
typedef struct LogReader {
    uint64_t NextSequence;
    void*    Buffer;
} LogReader_t;

/**
 * @brief Sets up a reader that starts at the oldest message in the log.
 *
 * @return OS_EOK on success, OS_EINVALPARAMS for NULL, or OS_EOOM when the
 *         buffer could not be allocated.
 */
__EXTERN oserr_t
LogReaderCreate(
    _In_ LogReader_t* reader);

/**
 * @brief Frees the buffer of a reader.
 */
__EXTERN void
LogReaderDestroy(
    _In_ LogReader_t* reader);

/**
 * @brief Reads the next messages from the kernel log and shows them on the
 * console. Messages that were dropped from the log before they could be read
 * are reported with ConsoleShowDropped. Does not draw.
 *
 * @param reader      Reader to continue from.
 * @param recordCount Receives the number of messages read. Zero means there
 *                    were no new messages.
 * @return OS_EOK on success, the error from OSKernelLogRead, or OS_EPROTOCOL
 *         when a record in the buffer is malformed. Messages before the
 *         malformed record are still shown, and NextSequence still moves on.
 */
__EXTERN oserr_t
LogReaderShowNext(
    _In_  LogReader_t* reader,
    _Out_ uint32_t*    recordCount);

/**
 * @brief Job that shows new kernel log messages until it is cancelled or a
 * read fails. Queue it with usched_job_queue.
 *
 * @param argument          The LogReader_t to use. The job destroys it and
 *                          frees it with free() when it ends.
 * @param cancellationToken Token of the job.
 */
__EXTERN void
LogReaderJob(
    _In_ void* argument,
    _In_ void* cancellationToken);

#endif //!__SYSLOGD_READER_H__
