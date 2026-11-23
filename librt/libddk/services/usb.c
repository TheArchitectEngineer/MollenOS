/**
 * MollenOS
 *
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

#include <ddk/service.h>
#include <ddk/devices/usb.h>
#include <ddk/convert.h>
#include <gracht/link/vali.h>
#include <ddk/utils.h>
#include <internal/_utils.h>
#include <signal.h>
#include <sys_usb_service_client.h>

extern int __crt_get_server_iod(void);

oserr_t
UsbControllerRegister(
    _In_ Device_t*              device,
    _In_ enum USBControllerKind type,
    _In_ int                    portCount)
{
    struct vali_link_message msg          = VALI_MSG_INIT_HANDLE(ServiceGetUsb());
    uuid_t                   serverHandle = GetNativeHandle(__crt_get_server_iod());
    struct sys_device        protoDevice;
    oserr_t                  oserr;
    int                      status;

    oserr = to_sys_device(device, &protoDevice);
    if (oserr != OS_EOK) {
        ERROR("UsbControllerRegister: device description could not be converted: %u", oserr);
        return oserr;
    }

    status = sys_usb_register_controller(
        GetGrachtClient(), &msg.base,
        serverHandle,
        &protoDevice,
        (int)type,
        (int)portCount
    );
    sys_device_destroy(&protoDevice);
    if (status != 0) {
        ERROR("UsbControllerRegister: device %u send failed: %i", device->Id, status);
        return OS_EUNKNOWN;
    }
    return OS_EOK;
}

void
UsbControllerUnregister(
    _In_ uuid_t deviceID)
{
    struct vali_link_message msg = VALI_MSG_INIT_HANDLE(ServiceGetUsb());
    int                      status;

    status = sys_usb_unregister_controller(
        GetGrachtClient(), &msg.base,
        deviceID
    );
    if (status != 0) {
        // This function cannot return an error, so a lost connection to the
        // USB service is reported to the driver as a broken pipe.
        ERROR("UsbControllerUnregister: device %u send failed: %i", deviceID, status);
        raise(SIGPIPE);
    }
}

oserr_t
UsbHubRegister(
    _In_ UsbDevice_t* usbDevice,
    _In_ int          portCount,
    _In_ uint16_t     characteristics)
{
    struct vali_link_message msg          = VALI_MSG_INIT_HANDLE(ServiceGetUsb());
    uuid_t                   serverHandle = GetNativeHandle(__crt_get_server_iod());
    int                      status;
    oserr_t                  hubStatus;

    status = sys_usb_register_hub(
        GetGrachtClient(), &msg.base,
        usbDevice->DeviceContext.hub_device_id,
        usbDevice->Base.Id,
        serverHandle,
        portCount,
        characteristics
    );
    if (status != 0) {
        ERROR("UsbHubRegister: hub %u send failed: %i", usbDevice->Base.Id, status);
        return OS_EUNKNOWN;
    }

    status = gracht_client_await(
        GetGrachtClient(), &msg.base,
        GRACHT_AWAIT_ASYNC
    );
    if (status != 0) {
        ERROR("UsbHubRegister: hub %u wait failed: %i", usbDevice->Base.Id, status);
        return OS_EUNKNOWN;
    }

    status = sys_usb_register_hub_result(
        GetGrachtClient(), &msg.base,
        &hubStatus
    );
    if (status != 0) {
        ERROR("UsbHubRegister: hub %u decode failed: %i", usbDevice->Base.Id, status);
        return OS_EUNKNOWN;
    }

    if (hubStatus != OS_EOK) {
        WARNING("UsbHubRegister: hub %u registration rejected: %u", usbDevice->Base.Id, hubStatus);
    }
    return hubStatus;
}

oserr_t
UsbHubUnregister(
    _In_ uuid_t deviceId)
{
    struct vali_link_message msg = VALI_MSG_INIT_HANDLE(ServiceGetUsb());
    int                      status;

    status = sys_usb_unregister_hub(GetGrachtClient(), &msg.base, deviceId);
    if (status != 0) {
        ERROR("UsbHubUnregister: hub %u send failed: %i", deviceId, status);
        return OS_EUNKNOWN;
    }
    return OS_EOK;
}

oserr_t
UsbEventPort(
    _In_ uuid_t  DeviceId,
    _In_ uint8_t PortAddress)
{
    struct vali_link_message msg = VALI_MSG_INIT_HANDLE(ServiceGetUsb());
    int                      status;

    status = sys_usb_port_event(
        GetGrachtClient(),
        &msg.base,
        DeviceId,
        PortAddress
    );
    if (status != 0) {
        ERROR("UsbEventPort: device %u port %u send failed: %i", DeviceId, PortAddress, status);
        return OS_EUNKNOWN;
    }
    return OS_EOK;
}

oserr_t
UsbPortError(
    _In_ uuid_t  deviceId,
    _In_ uint8_t portAddress)
{
    struct vali_link_message msg = VALI_MSG_INIT_HANDLE(ServiceGetUsb());
    int                      status;

    status = sys_usb_port_error(GetGrachtClient(), &msg.base, deviceId, portAddress);
    if (status != 0) {
        ERROR("UsbPortError: device %u port %u send failed: %i", deviceId, portAddress, status);
        return OS_EUNKNOWN;
    }
    return OS_EOK;
}

oserr_t
UsbQueryControllerCount(
    _Out_ int* ControllerCount)
{
    struct vali_link_message msg = VALI_MSG_INIT_HANDLE(ServiceGetUsb());
    int                      count;
    int                      status;

    if (ControllerCount == NULL) {
        return OS_EINVALPARAMS;
    }

    status = sys_usb_get_controller_count(GetGrachtClient(), &msg.base);
    if (status != 0) {
        ERROR("UsbQueryControllerCount: send failed: %i", status);
        return OS_EUNKNOWN;
    }

    status = gracht_client_await(GetGrachtClient(), &msg.base, GRACHT_AWAIT_ASYNC);
    if (status != 0) {
        ERROR("UsbQueryControllerCount: wait failed: %i", status);
        return OS_EUNKNOWN;
    }

    status = sys_usb_get_controller_count_result(GetGrachtClient(), &msg.base, &count);
    if (status != 0) {
        ERROR("UsbQueryControllerCount: decode failed: %i", status);
        return OS_EUNKNOWN;
    }

    // The caller's count is only written once the whole request has succeeded.
    *ControllerCount = count;
    return OS_EOK;
}

oserr_t
UsbQueryController(
    _In_ int                    Index,
    _In_ USBControllerDevice_t* Controller)
{
    struct vali_link_message msg = VALI_MSG_INIT_HANDLE(ServiceGetUsb());
    uint32_t                 controllerLength = sizeof(struct USBControllerDevice);
    int                      status;

    if (Controller == NULL) {
        return OS_EINVALPARAMS;
    }

    status = sys_usb_get_controller(GetGrachtClient(), &msg.base, Index);
    if (status != 0) {
        ERROR("UsbQueryController: controller %i send failed: %i", Index, status);
        return OS_EUNKNOWN;
    }

    status = gracht_client_await(GetGrachtClient(), &msg.base, GRACHT_AWAIT_ASYNC);
    if (status != 0) {
        ERROR("UsbQueryController: controller %i wait failed: %i", Index, status);
        return OS_EUNKNOWN;
    }

    status = sys_usb_get_controller_result(
        GetGrachtClient(),
        &msg.base,
        (uint8_t*)Controller,
        &controllerLength
    );
    if (status != 0) {
        ERROR("UsbQueryController: controller %i decode failed: %i", Index, status);
        return OS_EUNKNOWN;
    }

    // Ensure that the returned controller structure has the expected size
    if (controllerLength != sizeof(struct USBControllerDevice)) {
        ERROR("UsbQueryController: controller %i returned %u bytes, expected %zu",
              Index, controllerLength, sizeof(struct USBControllerDevice));
        return OS_EPROTOCOL;
    }
    return OS_EOK;
}
