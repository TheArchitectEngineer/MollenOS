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
 * Holds platform devices back from their drivers until every dependency that
 * needs a runtime owner has one.
 *
 * A driver cannot finish DevicePrepare while a clock, supply or other
 * dependency has no owner. Handing it the device anyway only produces a failed
 * prepare and a driver that gives up. So the device stays published but is not
 * matched with a driver. When the last missing owner registers, binding is
 * enabled from a separate job. A device whose owner never appears stays
 * published and unassigned for the life of deviced.
 *
 * Most waiting devices have no driver at all, for example the audio codec on
 * the Raspberry Pi 4. Their detail is only traced. Once the driver
 * configurations are known, PlatformWaitingReport names the waiting devices
 * that a known driver matches at NOTICE level, and logs one summary line.
 */

#include <ddk/utils.h>
#include <devices.h>
#include <discover.h>
#include <firmware/decode-log.h>
#include <os/usched/job.h>
#include <stdio.h>
#include <string.h>

#include "private.h"

static const char*
__KindName(
    _In_ enum FdtPlatformDependencyKind kind)
{
    switch (kind) {
        case FdtPlatformPower:  return "power domain";
        case FdtPlatformSupply: return "supply";
        case FdtPlatformClock:  return "clock";
        case FdtPlatformReset:  return "reset";
        case FdtPlatformPins:   return "pin state";
        case FdtPlatformPhy:    return "phy";
    }
    return "dependency";
}

static void
__WaitingLog(
    _In_ struct PlatformEntry*                 entry,
    _In_ const struct FdtPlatformDependencies* dependencies,
    _In_ uint32_t                              first,
    _In_ uint32_t                              missing,
    _In_ int                                   log)
{
    const struct FdtPlatformDependency* dependency = &dependencies->Entries[first];
    const struct FdtResources*          node = FdtTreeNode(entry->Bus->Tree, dependency->NodeOffset);
    const struct FdtResources*          state;
    char                                what[96];

    if (dependency->Kind == FdtPlatformPins) {
        // The argument of a pin entry is the state node, so name that node.
        state = FdtTreeNode(entry->Bus->Tree, dependency->Arguments[0]);
        snprintf(what, sizeof(what), "pin state %s of %s",
                 state != NULL ? state->Name : "?", node != NULL ? node->Name : "?");
    } else if (dependency->ArgumentCount != 0) {
        snprintf(what, sizeof(what), "%s %s %u",
                 __KindName(dependency->Kind), node != NULL ? node->Name : "?", dependency->Arguments[0]);
    } else {
        snprintf(what, sizeof(what), "%s %s",
                 __KindName(dependency->Kind), node != NULL ? node->Name : "?");
    }

    // Most waiting devices have no driver at all, so their detail is only
    // traced. A device that a known driver matches is worth an operator's
    // attention, see PlatformWaitingReport.
    if (log == PLATFORM_WAITING_LOG_NOTICE) {
        NOTICE("Platform: %s waits for %s before its driver can start (%u of %u dependencies have no owner)",
               entry->Firmware.Base.Identification.Description, what, missing, dependencies->Count);
    } else {
        TRACE("Platform: %s waits for %s before its driver can start (%u of %u dependencies have no owner)",
              entry->Firmware.Base.Identification.Description, what, missing, dependencies->Count);
    }
}

int
PlatformWaitingCheckLocked(
    _In_ struct PlatformEntry* entry,
    _In_ int                   log)
{
    struct FdtPlatformDependencies dependencies;
    uint32_t                       missing = 0;
    uint32_t                       first = 0;
    oserr_t                        status;

    // A plan that does not decode keeps the earlier behaviour: the device is
    // given to its driver, and DevicePrepare reports the decoding error. This
    // check only decides, so it decodes quietly. The errors appear once, when
    // the driver prepares the device.
    FdtDecodeQuietBegin();
    status = FdtPlatformDependencies(entry->Bus->Tree, entry->Firmware.FirmwareNode, &dependencies);
    FdtDecodeQuietEnd();
    if (status != OS_EOK) {
        return 0;
    }

    for (uint32_t i = 0; i < dependencies.Count; i++) {
        if (dependencies.Entries[i].Fixed) {
            continue;
        }
        if (PlatformDependencyFindOwner(entry->Bus, &dependencies.Entries[i]) != NULL) {
            continue;
        }
        if (missing == 0) {
            first = i;
        }
        missing++;
    }

    if (missing != 0 && log != PLATFORM_WAITING_LOG_NONE) {
        __WaitingLog(entry, &dependencies, first, missing, log);
    }
    return missing != 0;
}

static void
__PlatformBindWaiting(
    _In_ void* argument,
    _In_ void* cancellationToken)
{
    uuid_t  deviceId = (uuid_t)(uintptr_t)argument;
    oserr_t status;

    _CRT_UNUSED(cancellationToken);

    // Runs without any lock held. Binding takes the DMA and registry locks
    // and may start the driver.
    status = DmDeviceEnableDriverBinding(deviceId);
    if (status != OS_EOK) {
        WARNING("__PlatformBindWaiting: device %u could not be bound (%u)", deviceId, status);
    }
}

void
PlatformWaitingReleaseLocked(
    _In_ struct PlatformBus* bus)
{
    struct PlatformEntry* entry;
    uuid_t                job;

    for (entry = bus->Entries; entry != NULL; entry = entry->Next) {
        if (!entry->Waiting || PlatformWaitingCheckLocked(entry, PLATFORM_WAITING_LOG_NONE)) {
            continue;
        }

        entry->Waiting = 0;

        // A device that is still being published has no ID yet. Its publisher
        // sees the cleared flag and enables binding itself.
        if (entry->DeviceId == UUID_INVALID) {
            continue;
        }

        NOTICE("Platform: %s has every dependency owner, binding its driver",
               entry->Firmware.Base.Identification.Description);
        job = usched_job_queue(__PlatformBindWaiting, (void*)(uintptr_t)entry->DeviceId);
        if (job == UUID_INVALID) {
            ERROR("PlatformWaitingReleaseLocked: device %u binding job could not be queued",
                  entry->DeviceId);
        }
    }
}

void
PlatformWaitingReport(
    _In_ struct PlatformBus* bus)
{
    struct PlatformEntry*       entry;
    struct DriverIdentification identification;
    char                        compatibles[PLATFORM_DEVICE_MAX_COMPATIBLES];
    uint32_t                    waiting = 0;
    uint32_t                    matched = 0;
    int                         held;

    // Entries are added when the bus is created and freed only when it is
    // destroyed, so the list can be walked without the lock. Driver matching
    // takes the driver list lock, which must not be taken under Bus->Lock.
    for (entry = bus->Entries; entry != NULL; entry = entry->Next) {
        usched_mtx_lock(&bus->Lock);
        held = entry->Waiting && !entry->IsBus;
        if (held) {
            memcpy(compatibles, entry->Firmware.Compatibles, entry->Firmware.CompatibleLength);
        }
        usched_mtx_unlock(&bus->Lock);
        if (!held) {
            continue;
        }

        waiting++;
        memset(&identification, 0, sizeof(identification));
        identification.IsPlatform = 1;
        identification.Compatibles = compatibles;
        identification.CompatibleLength = entry->Firmware.CompatibleLength;
        if (!DmDiscoverHasDriver(&identification)) {
            continue;
        }

        // Log the detail again at NOTICE level, if the device still waits.
        matched++;
        usched_mtx_lock(&bus->Lock);
        if (entry->Waiting) {
            PlatformWaitingCheckLocked(entry, PLATFORM_WAITING_LOG_NOTICE);
        }
        usched_mtx_unlock(&bus->Lock);
    }

    if (waiting != 0) {
        NOTICE("Platform: %u devices wait for dependency owners, %u of them have a matching driver",
               waiting, matched);
    }
}
