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
 * Syslog Service
 * - Shows the kernel log on the boot framebuffer. The kernel itself only
 *   prints to serial and never draws on the screen.
 * - Messages are not sent to this service. Programs write them to the kernel
 *   log, and a reader job reads them from there.
 * - On systems without a 32 bit framebuffer nothing is drawn, and the
 *   service only answers protocol calls.
 */

#include <ddk/service.h>
#include <gracht/link/vali.h>
#include <internal/_utils.h>
#include <os/syslog.h>
#include <os/usched/job.h>
#include <stdlib.h>
#include "console/console.h"
#include "reader.h"

#include <sys_log_service_server.h>

static oserr_t
__StartReader(void)
{
    LogReader_t* reader;
    oserr_t      status;

    reader = malloc(sizeof(LogReader_t));
    if (reader == NULL) {
        return OS_EOOM;
    }

    status = LogReaderCreate(reader);
    if (status != OS_EOK) {
        free(reader);
        return status;
    }

    // The job owns the reader from here and frees it when it ends.
    if (usched_job_queue(LogReaderJob, reader) == UUID_INVALID) {
        LogReaderDestroy(reader);
        free(reader);
        return OS_EOOM;
    }
    return OS_EOK;
}

/**
 * Flow:
 * 1. Register the protocol, so the service answers calls even when there is
 *    nothing to draw on.
 * 2. Map the framebuffer. When this fails, nothing is drawn.
 * 3. Start the reader, which shows the whole kernel log from the oldest
 *    message on, and then keeps up with new ones.
 */
void ServiceInitialize(
    _In_ struct ServiceStartupOptions* startupOptions)
{
    oserr_t status;

    gracht_server_register_protocol(startupOptions->Server, &sys_log_server_protocol);

    status = ConsoleInitialize();
    if (status == OS_ENOTSUPPORTED) {
        DEBUG("syslogd: no 32 bit boot framebuffer, the log is not drawn");
        return;
    } else if (status != OS_EOK) {
        ERROR("syslogd: failed to set up the console: %u", status);
        return;
    }

    status = __StartReader();
    if (status != OS_EOK) {
        ERROR("syslogd: failed to start the log reader: %u", status);
    }
}

// Boot progress reporting is part of the protocol, but there is no splash
// screen to show it on yet.
void sys_log_report_stage_invocation(struct gracht_message* message, const char* stage, const int steps)
{
    _CRT_UNUSED(message);
    _CRT_UNUSED(stage);
    _CRT_UNUSED(steps);
}

void sys_log_report_progress_invocation(struct gracht_message* message, const char* progress)
{
    _CRT_UNUSED(message);
    _CRT_UNUSED(progress);
}

void sys_log_inhibit_invocation(struct gracht_message* message)
{
    _CRT_UNUSED(message);
    ConsoleInhibit();
}

void sys_log_uninhibit_invocation(struct gracht_message* message)
{
    _CRT_UNUSED(message);
    ConsoleUninhibit();
}
