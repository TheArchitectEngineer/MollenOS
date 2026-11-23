/**
 * MollenOS
 *
 * Copyright 2019, Philip Meulengracht
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
 *
 * Session Service (Protected) Definitions & Structures
 * - This header describes the base session-structure, prototypes
 *   and functionality, refer to the individual things for descriptions
 */

#ifndef __DDK_SERVICE_H__
#define __DDK_SERVICE_H__

#include <ddk/ddkdefs.h>

_CODE_BEGIN

// imported from gracht types.h, to avoid including that header
typedef struct gracht_server gracht_server_t;

struct ServiceStartupOptions {
    // Server is the handle to the gracht server itself. This is exposed to allow
    // services to register the protocols they support.
    gracht_server_t* Server;
    // ServerHandle is the handle of gracht server link. This is exposed to allow
    // services to share their handle with other procceses to allow communication.
    // While this is possible to discover with just the above Server handle, we add
    // it here for convenience.
    uuid_t ServerHandle;
};

// While we no longer hardcode these paths in the code when setting them, we still hardcode
// them when discovering. This means that we cannot remove these just yet, and instead they must
// match the service YAML paths.
#define SERVICE_DEVICE_PATH "/service/device"
#define SERVICE_USB_PATH "/service/usb"
#define SERVICE_PROCESS_PATH "/service/process"
#define SERVICE_FILE_PATH "/service/file"
#define SERVICE_NET_PATH "/service/net"
#define SERVICE_SYSLOG_PATH "/service/syslog"
#define SERVICE_SERVED_PATH "/service/serve"

/**
 * @brief Returns the handle of a system service. The handle is looked up by
 * the path of the service on the first call and reused after that.
 *
 * @return The handle of the service, or UUID_INVALID while the service has not
 *         registered its path yet.
 */
DDKDECL(uuid_t, ServiceGetDevice(void));
DDKDECL(uuid_t, ServiceGetUsb(void));
DDKDECL(uuid_t, ServiceGetProcess(void));
DDKDECL(uuid_t, ServiceGetFile(void));
DDKDECL(uuid_t, ServiceGetNet(void));
DDKDECL(uuid_t, ServiceGetSysLog(void));

/**
 * @brief Waits until the service has registered its handle path. The path is
 * looked up every 10 ms until it is found.
 *
 * @param timeout Longest time to wait, in milliseconds. Zero waits forever.
 * @return OS_EOK once the service is found, or OS_ETIMEOUT when it did not
 *         appear in time.
 */
DDKDECL(oserr_t,
ServiceWaitForDevice(
    _In_ size_t timeout));
DDKDECL(oserr_t,
ServiceWaitForUsb(
    _In_ size_t timeout));
DDKDECL(oserr_t,
ServiceWaitForProcess(
    _In_ size_t timeout));
DDKDECL(oserr_t,
ServiceWaitForFile(
    _In_ size_t timeout));
DDKDECL(oserr_t,
ServiceWaitForNet(
    _In_ size_t timeout));
DDKDECL(oserr_t,
ServiceWaitForSysLog(
    _In_ size_t timeout));

_CODE_END

#endif //!__DDK_SERVICE_H__
