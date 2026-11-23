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
 * Lookup of the system services. Every service registers its handle under a
 * fixed path, and these calls turn the path into the handle that requests are
 * sent to.
 */

#ifndef __OS_SERVICES_LOOKUP_H__
#define __OS_SERVICES_LOOKUP_H__

#include <os/osdefs.h>

_CODE_BEGIN

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
CRTDECL(uuid_t, OSServiceGetDevice(void));
CRTDECL(uuid_t, OSServiceGetUsb(void));
CRTDECL(uuid_t, OSServiceGetProcess(void));
CRTDECL(uuid_t, OSServiceGetFile(void));
CRTDECL(uuid_t, OSServiceGetNet(void));
CRTDECL(uuid_t, OSServiceGetSysLog(void));

/**
 * @brief Waits until the service has registered its handle path. The path is
 * looked up every 10 ms until it is found.
 *
 * @param timeout Longest time to wait, in milliseconds. Zero waits forever.
 * @return OS_EOK once the service is found, or OS_ETIMEOUT when it did not
 *         appear in time.
 */
CRTDECL(oserr_t,
OSServiceWaitForDevice(
    _In_ size_t timeout));
CRTDECL(oserr_t,
OSServiceWaitForUsb(
    _In_ size_t timeout));
CRTDECL(oserr_t,
OSServiceWaitForProcess(
    _In_ size_t timeout));
CRTDECL(oserr_t,
OSServiceWaitForFile(
    _In_ size_t timeout));
CRTDECL(oserr_t,
OSServiceWaitForNet(
    _In_ size_t timeout));
CRTDECL(oserr_t,
OSServiceWaitForSysLog(
    _In_ size_t timeout));

_CODE_END

#endif //!__OS_SERVICES_LOOKUP_H__
