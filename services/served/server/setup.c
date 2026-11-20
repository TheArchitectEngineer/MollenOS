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
 */

#define __TRACE

#include <ddk/utils.h>
#include <served/install.h>
#include <served/state.h>
#include <served/server.h>
#include <stdlib.h>

// Sets served up from /data. sys_file_event_storage_ready_invocation queues
// this job each time filed reports /data as mounted.
//
// A failed step logs its error and ends this job. served keeps running without
// applications, and the setup runs again from the start when filed reports
// /data once more. /data is on removable storage, so a failed step usually
// means the disk went away while served used it. Running the setup again is
// what already happens when a disk is put in again after a setup that worked:
// the paths are only created when missing, and StateLoad builds the state anew.
void served_server_setup_job(void* arguments, void* cancellationToken)
{
    _CRT_UNUSED(arguments);
    _CRT_UNUSED(cancellationToken);

    // Ensure server paths exists before doing anything. This doesn't
    // require any previous action, and does not hurt to ensure we do.
    oserr_t oserr = ServerEnsurePaths();
    if (oserr != OS_EOK) {
        ERROR("served_server_setup_job failed to create necessary paths: %u, waiting for /data to be mounted again",
              oserr);
        return;
    }

    // The first thing we want to determine is the system state. Check for the
    // presence of state.json, and determine the action from here. If
    // /data/served/state.json is not present, then we check for
    // /data/setup and launch the system install job.
    oserr = StateLoad();
    if (oserr != OS_EOK) {
        ERROR("served_server_setup_job failed to load server state: %u, waiting for /data to be mounted again",
              oserr);
        return;
    }

    StateLock();
    if (State()->FirstBoot) {
        InstallBundledApplications();
        State()->FirstBoot = false;
    }
    StateUnlock();

    // State was loaded, initialize served from state
    oserr = ServerLoad();
    if (oserr != OS_EOK) {
        ERROR("served_server_setup_job failed to initialize server: %u, waiting for /data to be mounted again",
              oserr);
        return;
    }
}
