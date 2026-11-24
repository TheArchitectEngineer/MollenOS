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
 * - Reads the kernel log and passes each message to the console.
 */

#include <os/syslog.h>
#include <os/usched/job.h>
#include <stdlib.h>
#include <string.h>
#include "console/console.h"
#include "reader.h"

// The kernel log has no way to wait for new messages, so the reader checks
// for them at this interval, in microseconds. 25 ms keeps the screen at most
// a frame or two behind the log, while an idle reader wakes 40 times a second.
#define __LOG_READER_POLL_US (25 * 1000)

/**
 * @brief Checks that a record fits inside the bytes that were read, and that
 * its Size and Length agree with each other.
 *
 * @param offset    Offset of the record in the buffer.
 * @param bytesRead Number of valid bytes in the buffer.
 * @param record    The record at offset. Only read when its header fits.
 */
static int
__RecordIsValid(
    _In_ size_t                     offset,
    _In_ size_t                     bytesRead,
    _In_ const OSKernelLogRecord_t* record)
{
    size_t available = bytesRead - offset;

    if (available < sizeof(OSKernelLogRecord_t)) {
        return 0;
    }

    if (record->Length > OSKERNELLOG_MESSAGE_MAX ||
        record->Size < sizeof(OSKernelLogRecord_t) + record->Length ||
        record->Size > available) {
        return 0;
    }
    return 1;
}

oserr_t
LogReaderCreate(
    _In_ LogReader_t* reader)
{
    if (reader == NULL) {
        return OS_EINVALPARAMS;
    }

    reader->NextSequence = 0;
    reader->Buffer       = malloc(LOG_READER_BUFFER_SIZE);
    if (reader->Buffer == NULL) {
        return OS_EOOM;
    }
    return OS_EOK;
}

void
LogReaderDestroy(
    _In_ LogReader_t* reader)
{
    if (reader == NULL) {
        return;
    }
    
    free(reader->Buffer);
    reader->Buffer = NULL;
}

oserr_t
LogReaderShowNext(
    _In_  LogReader_t* reader,
    _Out_ uint32_t*    recordCount)
{
    OSKernelLogReadResult_t result;
    char                    text[OSKERNELLOG_MESSAGE_MAX + 1];
    size_t                  offset = 0;
    oserr_t                 status;

    *recordCount = 0;
    status = OSKernelLogRead(reader->NextSequence, reader->Buffer, LOG_READER_BUFFER_SIZE, &result);
    if (status != OS_EOK) {
        return status;
    }

    // Move on first, so a malformed record below is not read again.
    reader->NextSequence = result.NextSequence;
    if (result.Dropped != 0) {
        ConsoleShowDropped(result.Dropped);
    }

    while (offset < result.BytesRead) {
        const OSKernelLogRecord_t* record = (const OSKernelLogRecord_t*)((const uint8_t*)reader->Buffer + offset);

        if (!__RecordIsValid(offset, result.BytesRead, record)) {
            return OS_EPROTOCOL;
        }

        // The text in a record is not zero terminated.
        memcpy(&text[0], (const uint8_t*)record + sizeof(OSKernelLogRecord_t), record->Length);
        text[record->Length] = '\0';
        ConsoleShowRecord(record, &text[0]);

        offset += record->Size;
        (*recordCount)++;
    }
    return OS_EOK;
}

/**
 * Flow:
 * 1. Read and show the next messages.
 * 2. Draw the rows that changed.
 * 3. With no new messages, sleep for the poll interval. Otherwise yield, so
 *    protocol calls on this thread still run while a large backlog is shown,
 *    and read again right away.
 */
void
LogReaderJob(
    _In_ void* argument,
    _In_ void* cancellationToken)
{
    LogReader_t* reader = argument;
    uint32_t     recordCount;
    oserr_t      status;

    while (!usched_is_cancelled(cancellationToken)) {
        status = LogReaderShowNext(reader, &recordCount);
        if (status != OS_EOK) {
            ERROR("LogReaderJob failed to read the kernel log at message %llu: %u",
                  (unsigned long long)reader->NextSequence, status);
            break;
        }

        ConsoleDraw();

        if (recordCount != 0) {
            usched_job_yield();
            continue;
        }

        if (usched_job_sleep_us(__LOG_READER_POLL_US) != 0) {
            ERROR("LogReaderJob failed to sleep between reads, stopping");
            break;
        }
    }

    LogReaderDestroy(reader);
    free(reader);
}
