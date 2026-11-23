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
 * Lookup of the system services by their handle path. Each handle is looked up
 * once and cached for the rest of the process.
 */

//#define __TRACE

#include <internal/_syscalls.h>
#include <ddk/service.h>
#include <ddk/utils.h>
#include <os/threads.h>
#include <threads.h>

// Time between two lookups while waiting for a service to register itself.
#define SERVICE_POLL_INTERVAL_MS 10

enum __ServiceIndex {
    SERVICE_INDEX_DEVICE,
    SERVICE_INDEX_USB,
    SERVICE_INDEX_PROCESS,
    SERVICE_INDEX_FILE,
    SERVICE_INDEX_NET,
    SERVICE_INDEX_SYSLOG,

    SERVICE_INDEX_COUNT
};

// One system service that can be looked up. Handle stays UUID_INVALID until
// the first successful lookup, and is reused after that.
struct __Service {
    const char* Path;
    uuid_t      Handle;
};

static struct __Service g_services[SERVICE_INDEX_COUNT] = {
    [SERVICE_INDEX_DEVICE]  = { SERVICE_DEVICE_PATH,  UUID_INVALID },
    [SERVICE_INDEX_USB]     = { SERVICE_USB_PATH,     UUID_INVALID },
    [SERVICE_INDEX_PROCESS] = { SERVICE_PROCESS_PATH, UUID_INVALID },
    [SERVICE_INDEX_FILE]    = { SERVICE_FILE_PATH,    UUID_INVALID },
    [SERVICE_INDEX_NET]     = { SERVICE_NET_PATH,     UUID_INVALID },
    [SERVICE_INDEX_SYSLOG]  = { SERVICE_SYSLOG_PATH,  UUID_INVALID },
};

// Returns the cached handle of the service, or looks it up when it is not
// known yet. A service that has not registered its path yet is a normal
// answer during boot, so a failed lookup is only traced.
static uuid_t
__GetService(
    _In_ enum __ServiceIndex index)
{
    struct __Service* service = &g_services[index];
    uuid_t            handle;
    oserr_t           status;

    if (service->Handle != UUID_INVALID) {
        return service->Handle;
    }

    status = Syscall_HandleLookup(service->Path, &handle);
    if (status != OS_EOK) {
        TRACE("__GetService: lookup of %s failed (%u)", service->Path, status);
        return UUID_INVALID;
    }

    service->Handle = handle;
    return handle;
}

// Looks the service up every SERVICE_POLL_INTERVAL_MS until it is found. A
// timeout of zero waits forever. Any other timeout is given in milliseconds,
// and the last lookup happens once that much time has been slept.
static oserr_t
__WaitForService(
    _In_ enum __ServiceIndex index,
    _In_ size_t              timeout)
{
    size_t waited = 0;

    while (__GetService(index) == UUID_INVALID) {
        if (timeout != 0 && waited >= timeout) {
            WARNING("__WaitForService: %s did not appear within %zu ms", g_services[index].Path, timeout);
            return OS_ETIMEOUT;
        }

        thrd_sleep(&(struct timespec) { .tv_nsec = SERVICE_POLL_INTERVAL_MS * NSEC_PER_MSEC }, NULL);

        // Only count the time when there is a limit to compare it against,
        // so waiting forever can never overflow the counter.
        if (timeout != 0) {
            waited += SERVICE_POLL_INTERVAL_MS;
        }
    }
    return OS_EOK;
}

uuid_t ServiceGetDevice(void)
{
    return __GetService(SERVICE_INDEX_DEVICE);
}

uuid_t ServiceGetUsb(void)
{
    return __GetService(SERVICE_INDEX_USB);
}

uuid_t ServiceGetProcess(void)
{
    return __GetService(SERVICE_INDEX_PROCESS);
}

uuid_t ServiceGetFile(void)
{
    return __GetService(SERVICE_INDEX_FILE);
}

uuid_t ServiceGetNet(void)
{
    return __GetService(SERVICE_INDEX_NET);
}

uuid_t ServiceGetSysLog(void)
{
    return __GetService(SERVICE_INDEX_SYSLOG);
}

oserr_t
ServiceWaitForDevice(
    _In_ size_t timeout)
{
    return __WaitForService(SERVICE_INDEX_DEVICE, timeout);
}

oserr_t
ServiceWaitForUsb(
    _In_ size_t timeout)
{
    return __WaitForService(SERVICE_INDEX_USB, timeout);
}

oserr_t
ServiceWaitForProcess(
    _In_ size_t timeout)
{
    return __WaitForService(SERVICE_INDEX_PROCESS, timeout);
}

oserr_t
ServiceWaitForFile(
    _In_ size_t timeout)
{
    return __WaitForService(SERVICE_INDEX_FILE, timeout);
}

oserr_t
ServiceWaitForNet(
    _In_ size_t timeout)
{
    return __WaitForService(SERVICE_INDEX_NET, timeout);
}

oserr_t
ServiceWaitForSysLog(
    _In_ size_t timeout)
{
    return __WaitForService(SERVICE_INDEX_SYSLOG, timeout);
}
