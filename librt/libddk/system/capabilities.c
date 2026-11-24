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
 * Capabilities. Grants capabilities to a memory space, and translates the
 * capability names used in service and driver manifests.
 */

#include <internal/_syscalls.h>
#include <ddk/capabilities.h>
#include <os/osdefs.h>
#include <string.h>

oserr_t
CapabilityGrant(
    _In_ Capabilities_t capabilities,
    _In_ uuid_t         memorySpaceHandle)
{
    return Syscall_CapabilityGrant(capabilities, memorySpaceHandle);
}

static struct {
    const char*    name;
    Capabilities_t capability;
} g_capabilityStrings[] = {
    { "grant-control",           CAPABILITY_GRANT_CAPABILITY },
    { "firmware-access",         CAPABILITY_FIRMWARE_ACCESS },
    { "ramdisk-access",          CAPABILITY_RAMDISK_ACCESS },
    { "deviceio-access",         CAPABILITY_DEVICEIO_ACCESS },
    { "deviceio-management",     CAPABILITY_DEVICEIO_MANAGEMENT },
    { "interrupt-control",       CAPABILITY_INTERRUPT_CONTROL },
    { "memoryspace-management",  CAPABILITY_MEMORYSPACE_MANAGEMENT },
    { "syslog-read",             CAPABILITY_SYSLOG_READ },
    { "boot-video",              CAPABILITY_BOOT_VIDEO },
};

Capabilities_t
CapabilityParse(
    _In_ const char* capabilityString)
{
    if (capabilityString == NULL) {
        return 0;
    }

    for (size_t i = 0; i < SIZEOF_ARRAY(g_capabilityStrings); i++) {
        if (strcmp(g_capabilityStrings[i].name, capabilityString) == 0) {
            return g_capabilityStrings[i].capability;
        }
    }
    return 0; // Invalid capability string
}
