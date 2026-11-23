/**
 * Copyright 2017, Philip Meulengracht
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
 */

#include <ddk/convert.h>
#include <ddk/service.h>
#include <ddk/utils.h>
#include <gracht/link/vali.h>
#include <internal/_utils.h>

#include <sys_device_service_client.h>

uuid_t
RegisterDevice(
    _In_ Device_t*    device,
    _In_ unsigned int flags)
{
    struct vali_link_message msg = VALI_MSG_INIT_HANDLE(GetDeviceService());
    struct sys_device        sysDevice;
    oserr_t                  oserr;
    uuid_t                   id;
    int                      status;

    oserr = to_sys_device(device, &sysDevice);
    if (oserr != OS_EOK) {
        ERROR("RegisterDevice: device description could not be converted: %u", oserr);
        return UUID_INVALID;
    }

    // The description is copied into the message while it is sent, so it can
    // be freed right after the send, whether the send worked or not.
    status = sys_device_register(GetGrachtClient(), &msg.base, &sysDevice, flags);
    sys_device_destroy(&sysDevice);
    if (status != 0) {
        ERROR("RegisterDevice: send failed: %i", status);
        return UUID_INVALID;
    }

    status = gracht_client_await(GetGrachtClient(), &msg.base, GRACHT_AWAIT_ASYNC);
    if (status != 0) {
        ERROR("RegisterDevice: wait failed: %i", status);
        return UUID_INVALID;
    }

    status = sys_device_register_result(GetGrachtClient(), &msg.base, &oserr, &id);
    if (status != 0) {
        ERROR("RegisterDevice: decode failed: %i", status);
        return UUID_INVALID;
    }

    if (oserr != OS_EOK) {
        WARNING("RegisterDevice: registration rejected: %u", oserr);
        return UUID_INVALID;
    }

    // The id is only stored once the device service has confirmed the device.
    device->Id = id;
    return id;
}

oserr_t
UnregisterDevice(
    _In_ uuid_t deviceId)
{
    struct vali_link_message msg = VALI_MSG_INIT_HANDLE(GetDeviceService());
    oserr_t                  oserr = OS_EPROTOCOL;
    int                      status;

    status = sys_device_unregister(GetGrachtClient(), &msg.base, deviceId);
    if (status != 0) {
        WARNING("UnregisterDevice: device %u send failed: %i", deviceId, status);
        return OS_EPROTOCOL;
    }

    status = gracht_client_await(GetGrachtClient(), &msg.base, GRACHT_AWAIT_ASYNC);
    if (status != 0) {
        WARNING("UnregisterDevice: device %u wait failed: %i", deviceId, status);
        return OS_EPROTOCOL;
    }

    status = sys_device_unregister_result(GetGrachtClient(), &msg.base, &oserr);
    if (status != 0) {
        WARNING("UnregisterDevice: device %u decode failed: %i", deviceId, status);
        return OS_EPROTOCOL;
    }

    if (oserr != OS_EOK) {
        WARNING("UnregisterDevice: device %u removal rejected: %u", deviceId, oserr);
    }
    return oserr;
}

oserr_t
IoctlDeviceEx(
    _In_    uuid_t       deviceId,
    _In_    int          direction,
    _In_    unsigned int offset,
    _InOut_ size_t*      value,
    _In_    size_t       width)
{
    struct vali_link_message msg = VALI_MSG_INIT_HANDLE(GetDeviceService());
    oserr_t                  oserr = OS_EPROTOCOL;
    size_t                   decoded;
    int                      status;

    if (value == NULL) {
        return OS_EINVALPARAMS;
    }

    decoded = *value;

    status = sys_device_ioctlex(GetGrachtClient(), &msg.base, deviceId, direction, offset, decoded, width);
    if (status != 0) {
        WARNING("IoctlDeviceEx: device %u send failed: %i", deviceId, status);
        return OS_EPROTOCOL;
    }

    status = gracht_client_await(GetGrachtClient(), &msg.base, GRACHT_AWAIT_ASYNC);
    if (status != 0) {
        WARNING("IoctlDeviceEx: device %u wait failed: %i", deviceId, status);
        return OS_EPROTOCOL;
    }

    status = sys_device_ioctlex_result(GetGrachtClient(), &msg.base, &oserr, &decoded);
    if (status != 0) {
        WARNING("IoctlDeviceEx: device %u decode failed: %i", deviceId, status);
        return OS_EPROTOCOL;
    }

    if (oserr != OS_EOK) {
        WARNING("IoctlDeviceEx: device %u access rejected: %u", deviceId, oserr);
        return oserr;
    }

    // The caller's value is only written once the whole request has succeeded.
    // Every failure above returns before this point and leaves it unchanged.
    *value = decoded;
    return OS_EOK;
}
