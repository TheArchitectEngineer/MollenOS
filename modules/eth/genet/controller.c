/**
 * Copyright 2026, Philip Meulengracht
 *
 * This program is free software : you can redistribute it and / or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
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
 * Controller creation and destruction. GenetCreate runs the setup steps in
 * order and records how far it got in GenetController_t.State. GenetDestroy
 * walks back from that state, and can be called again after a failure.
 *
 * Logging: each failure is logged once, where it is detected, with the device
 * ID where one is known. The GENET functions called from here log their own
 * failures, so only calls into the DDK and the common Ethernet code are logged
 * in this file.
 */

#include <ddk/utils.h>
#include <string.h>

#include "genet.h"

// Receive slots the client must have posted before the common code starts
// the queues. A frame that arrives while no slot is posted is dropped.
#define GENET_MIN_RX_SLOTS 4U

// The Pi 4 device tree gives GENET one register window.
#define GENET_REGISTER_WINDOW_COUNT 1U

static const struct EthernetOperations g_operations = {
    .Prepare = GenetQueuesPrepare,
    .Start = GenetQueuesStart,
    .Stop = GenetQueuesStop,
    .Submit = GenetTransferSubmit,
    .ReadLink = GenetLinkRead,
    .Poll = GenetTransferPoll
};

static const struct EthernetRegistrationConfig g_registrationConfig = {
    .MinRxSlots = GENET_MIN_RX_SLOTS
};

/**
 * Check that an address can be used as the MAC's own address. The lowest bit
 * of the first byte marks a group (multicast) address, and the all-zero
 * address is what an unpatched device tree holds.
 */
static bool
__ValidMac(
    _In_ const uint8_t* mac)
{
    return !(mac[0] & 1) && (mac[0] | mac[1] | mac[2] | mac[3] | mac[4] | mac[5]);
}

static oserr_t
__ReadMac(
    _InOut_ GenetController_t* controller)
{
    uint32_t high;
    uint32_t low;

    if (controller->Configuration.MacPresent) {
        memcpy(controller->Mac, controller->Configuration.Mac, GENET_MAC_ADDRESS_SIZE);
    } else {
        // The device tree may have no address. In that case the bootloader may
        // have left a usable one in UniMAC. Read it before the reset wipes it,
        // and never make up an address that several boards would share.
        high = GenetRegisterRead(&controller->Registers, GENET_MAC_ADDRESS0);
        low = GenetRegisterRead(&controller->Registers, GENET_MAC_ADDRESS1);

        controller->Mac[0] = high >> 24;
        controller->Mac[1] = high >> 16;
        controller->Mac[2] = high >> 8;
        controller->Mac[3] = high;
        controller->Mac[4] = low >> 8;
        controller->Mac[5] = low;
    }

    if (!__ValidMac(controller->Mac)) {
        ERROR("GENET %u: no usable MAC address from %s: %02x:%02x:%02x:%02x:%02x:%02x",
            controller->Device->Id,
            controller->Configuration.MacPresent ? "firmware" : "UniMAC",
            controller->Mac[0], controller->Mac[1], controller->Mac[2],
            controller->Mac[3], controller->Mac[4], controller->Mac[5]);
        return OS_ENOENT;
    }
    return OS_EOK;
}

/**
 * Read the firmware configuration, prepare the platform device and map the
 * register window. Moves the controller from GENET_STATE_NEW to
 * GENET_STATE_PREPARED and then GENET_STATE_MAPPED as each step succeeds.
 */
static oserr_t
__MapDevice(
    _InOut_ GenetController_t* controller)
{
    struct DeviceResources resources;
    uuid_t                 deviceId = controller->Device->Id;
    oserr_t                status;

    ETHERNET_LOG("GENET init: firmware configuration device=%u", deviceId);
    status = DeviceEthernetGetConfiguration(deviceId, &controller->Configuration);
    if (status != OS_EOK) {
        ERROR("GENET %u: reading the firmware Ethernet configuration failed: %u", deviceId, status);
        return status;
    }

    ETHERNET_LOG("GENET firmware: mode=%u PHY-address=%u PHY-node=%u MDIO-node=%u MDIO-offset=0x%x length=%u max-speed=%u",
        controller->Configuration.Mode, controller->Configuration.PhyAddress,
        controller->Configuration.PhyNode, controller->Configuration.MdioNode,
        controller->Configuration.MdioOffset, controller->Configuration.MdioLength,
        controller->Configuration.MaxSpeed);
    ETHERNET_LOG("GENET reset: GPIO-present=%u controller=%u pin=%u flags=0x%x assert-us=%u deassert-us=%u",
        controller->Configuration.Reset.GpioPresent, controller->Configuration.Reset.Controller,
        controller->Configuration.Reset.Pin, controller->Configuration.Reset.Flags,
        controller->Configuration.Reset.AssertUs, controller->Configuration.Reset.DeassertUs);

    // The platform code turns on clocks and power and takes the device out of
    // reset. No register may be touched before this.
    ETHERNET_LOG("GENET init: platform prepare");
    status = DeviceSetStage(deviceId, DevicePrepare);
    if (status != OS_EOK) {
        ERROR("GENET %u: preparing the platform device failed: %u", deviceId, status);
        return status;
    }
    controller->State = GENET_STATE_PREPARED;

    ETHERNET_LOG("GENET init: resources");
    status = DeviceGetResources(deviceId, &resources);
    if (status != OS_EOK) {
        ERROR("GENET %u: reading the device resources failed: %u", deviceId, status);
        return status;
    }
    if (resources.Count != GENET_REGISTER_WINDOW_COUNT) {
        ERROR("GENET %u: the device has %u register windows, expected %u",
            deviceId, resources.Count, GENET_REGISTER_WINDOW_COUNT);
        return OS_ENOTSUPPORTED;
    }
    controller->Registers = resources.Registers[0];

    ETHERNET_LOG("GENET init: map registers");
    status = DeviceIoAcquire(&controller->Registers);
    if (status != OS_EOK) {
        ERROR("GENET %u: mapping the register window failed: %u", deviceId, status);
        return status;
    }
    controller->State = GENET_STATE_MAPPED;
    return OS_EOK;
}

/**
 * Bring the MAC to a known, stopped state: wrapper settings, reset line, MAC
 * address, DMA engines off and a full MAC reset. Requires GENET_STATE_MAPPED.
 */
static oserr_t
__ResetMac(
    _InOut_ GenetController_t* controller)
{
    oserr_t status;

    ETHERNET_LOG("GENET init: integration");
    status = GenetWrapperConfigurePhy(
        &controller->Wrapper,
        &controller->Registers,
        &controller->Configuration
    );
    if (status != OS_EOK) {
        return status;
    }

    // The UniMAC registers cannot be used while the wrapper holds the MAC in
    // reset, and firmware may have left it there.
    status = GenetWrapperReleaseReset(&controller->Wrapper, &controller->Registers);
    if (status != OS_EOK) {
        return status;
    }

    // The address has to be read before the MAC reset below, which wipes the
    // one the bootloader may have left in UniMAC.
    ETHERNET_LOG("GENET init: MAC address");
    status = __ReadMac(controller);
    if (status != OS_EOK) {
        return status;
    }
    ETHERNET_LOG("GENET MAC: %02x:%02x:%02x:%02x:%02x:%02x source=%s",
        controller->Mac[0], controller->Mac[1], controller->Mac[2], controller->Mac[3],
        controller->Mac[4], controller->Mac[5],
        controller->Configuration.MacPresent ? "firmware" : "UniMAC");

    // The bootloader may have left the DMA engines running. They must be off
    // before the MAC is reset.
    ETHERNET_LOG("GENET init: stop DMA");
    status = GenetQueuesStop(&controller->Adapter);
    if (status != OS_EOK) {
        return status;
    }

    ETHERNET_LOG("GENET init: MAC reset");
    return GenetMacInitialize(controller);
}

/**
 * Reset the PHY through firmware, then find and configure it over MDIO.
 * Requires a reset MAC, since the MDIO block is part of it.
 */
static oserr_t
__StartPhy(
    _InOut_ GenetController_t* controller)
{
    uuid_t  deviceId = controller->Device->Id;
    oserr_t status;

    ETHERNET_LOG("GENET init: firmware PHY reset");
    status = DeviceEthernetResetPhy(deviceId);
    if (status != OS_EOK) {
        ERROR("GENET %u: resetting the PHY through firmware failed: %u", deviceId, status);
        return status;
    }

    ETHERNET_LOG("GENET init: MDIO");
    controller->Mdio.Registers = &controller->Registers;
    status = MdioInitialize(
        &controller->Bus,
        &g_genetMdioOperations,
        &controller->Mdio,
        controller->Configuration.PhyAddress
    );
    if (status != OS_EOK) {
        ERROR("GENET %u: PHY address %u is not a valid MDIO address: %u",
            deviceId, controller->Configuration.PhyAddress, status);
        return status;
    }

    ETHERNET_LOG("GENET init: PHY setup");
    status = PhyInitialize(
        &controller->Phy,
        &controller->Bus,
        &controller->Configuration
    );
    if (status != OS_EOK) {
        ERROR("GENET %u: setting up the PHY at address %u failed: %u",
            deviceId, controller->Configuration.PhyAddress, status);
    }
    return status;
}

/**
 * Register the packet interrupt. Polling can still move packets, so a missing
 * interrupt is not fatal. Whatever was set up is undone first, because a
 * half-installed handler must not be left behind.
 * @return OS_EOK with or without an interrupt, or the error from undoing a
 *         partial setup.
 */
static oserr_t
__RegisterInterrupt(
    _InOut_ GenetController_t* controller)
{
    oserr_t status;

    ETHERNET_LOG("GENET init: packet interrupt");
    status = GenetInterruptRegister(controller);
    if (status == OS_EOK) {
        return OS_EOK;
    }

    WARNING("GENET %u: packet interrupt unavailable (%u), polling every %u ms",
        controller->Device->Id, status, GENET_POLL_INTERVAL_MS);
    return GenetInterruptUnregister(controller);
}

oserr_t
GenetCreate(
    _InOut_ GenetController_t* controller,
    _In_    Device_t*          device)
{
    oserr_t status;

    if (controller->State != GENET_STATE_NEW) {
        ERROR("GENET %u: the controller has already been set up", device->Id);
        return OS_EBUSY;
    }

    controller->Device = device;
    controller->Adapter.Controller = controller;
    controller->Adapter.Operations = &g_operations;
    GenetThresholdsDefaults(&controller->Thresholds);

    if (!GenetWrapperMatchesDevice(device)) {
        ERROR("GENET %u: the device is not a %s", device->Id, GENET_COMPATIBLE_BCM2711);
        return OS_ENOTSUPPORTED;
    }

    // The order below is fixed by the hardware. Registers need a prepared
    // platform device. The PHY is reached through the MAC's MDIO block, so the
    // MAC is reset first. The platform wants interrupt handlers ready before
    // the device starts, and the adapter is published last, once everything
    // it can call is in place.
    status = __MapDevice(controller);
    if (status != OS_EOK) {
        return status;
    }

    status = __ResetMac(controller);
    if (status != OS_EOK) {
        return status;
    }

    status = __StartPhy(controller);
    if (status != OS_EOK) {
        return status;
    }

    ETHERNET_LOG("GENET init: DMA context");
    status = DMAOpen(device->Id, GENET_DMA_ADDRESS_MASK, &controller->Dma);
    if (status != OS_EOK) {
        ERROR("GENET %u: opening the DMA context failed: %u", device->Id, status);
        return status;
    }

    status = __RegisterInterrupt(controller);
    if (status != OS_EOK) {
        return status;
    }

    status = DeviceSetStage(device->Id, DeviceStart);
    if (status != OS_EOK) {
        ERROR("GENET %u: starting the platform device failed: %u", device->Id, status);
        return status;
    }

    status = EthernetRegister(
        &controller->Adapter,
        device->Id,
        controller->Mac,
        &g_registrationConfig
    );
    if (status != OS_EOK) {
        ERROR("GENET %u: publishing the network adapter failed: %u", device->Id, status);
        return status;
    }

    controller->State = GENET_STATE_READY;
    ETHERNET_LOG("GENET init: ready, netadapter published; waiting for link and client queues");
    return OS_EOK;
}

/**
 * Teardown stage for GENET_STATE_STOPPED: release everything except the
 * register mapping. Each call below does nothing when its work is already
 * done, so a retry may repeat the calls that succeeded on an earlier attempt.
 */
static oserr_t
__ReleaseResources(
    _InOut_ GenetController_t* controller)
{
    uuid_t  deviceId = controller->Device->Id;
    oserr_t status;

    // Stop interrupt delivery before releasing memory or the register window.
    status = GenetInterruptUnregister(controller);
    if (status != OS_EOK) {
        return status;
    }

    status = EthernetCleanup(&controller->Adapter);
    if (status != OS_EOK) {
        ERROR("GENET %u: releasing the adapter state failed: %u", deviceId, status);
        return status;
    }

    // DMA has stopped, so an unreachable PHY must not block freeing memory.
    if (controller->Phy.Bus != NULL) {
        status = PhyStop(&controller->Phy);
        if (status != OS_EOK) {
            WARNING("GENET %u: PHY isolation failed after DMA stopped: %u", deviceId, status);
        }
        controller->Phy.Bus = NULL;
    }

    status = DMAFree(&controller->Queues.Storage.Buffer);
    if (status != OS_EOK) {
        ERROR("GENET %u: freeing the frame storage failed: %u", deviceId, status);
        return status;
    }

    status = DMAClose(&controller->Dma);
    if (status != OS_EOK) {
        ERROR("GENET %u: closing the DMA context failed: %u", deviceId, status);
        return status;
    }

    status = GenetWrapperRestorePhy(&controller->Wrapper);
    if (status != OS_EOK) {
        return status;
    }

    // The platform refuses to stop a device that still has an interrupt
    // allocated, which is why the interrupt went first.
    status = DeviceSetStage(deviceId, DeviceStop);
    if (status != OS_EOK) {
        ERROR("GENET %u: stopping the platform device failed: %u", deviceId, status);
    }
    return status;
}

oserr_t
GenetDestroy(
    _InOut_ GenetController_t* controller)
{
    oserr_t status;

    // Before mapping, no MAC, PHY or interrupt resources can exist.
    if (controller->State == GENET_STATE_NEW) {
        controller->State = GENET_STATE_DESTROYED;
        return OS_EOK;
    }
    if (controller->State == GENET_STATE_PREPARED) {
        status = DeviceSetStage(controller->Device->Id, DeviceStop);
        if (status != OS_EOK) {
            ERROR("GENET %u: stopping the platform device failed: %u", controller->Device->Id, status);
            return status;
        }
        controller->State = GENET_STATE_DESTROYED;
        return OS_EOK;
    }

    // The stages below fall through into each other. A stage that fails
    // returns with its state kept, and the next call starts from it again.
    if (controller->State == GENET_STATE_READY) {
        EthernetUnregister(&controller->Adapter);
        controller->State = GENET_STATE_STOPPING;
    } else if (controller->State == GENET_STATE_MAPPED) {
        controller->State = GENET_STATE_STOPPING;
    }

    if (controller->State == GENET_STATE_STOPPING) {
        // The wrapper records its register window only after checking the
        // hardware. If creation failed before that, the MAC and DMA registers
        // were never used and must not be touched now.
        if (GenetWrapperIsVerified(&controller->Wrapper)) {
            status = EthernetQueuesReset(&controller->Adapter);
            if (status != OS_EOK) {
                ERROR("GENET %u: stopping the queues failed: %u", controller->Device->Id, status);
                return status;
            }
        }
        controller->State = GENET_STATE_STOPPED;
    }

    if (controller->State == GENET_STATE_STOPPED) {
        // Queue shutdown is complete and must not repeat during retries.
        status = __ReleaseResources(controller);
        if (status != OS_EOK) {
            return status;
        }
        controller->State = GENET_STATE_UNMAPPING;
    }

    if (controller->State == GENET_STATE_UNMAPPING) {
        status = DeviceIoRelease(&controller->Registers);
        if (status != OS_EOK) {
            ERROR("GENET %u: unmapping the register window failed: %u", controller->Device->Id, status);
            return status;
        }
        controller->State = GENET_STATE_DESTROYED;
        controller->Wrapper.Reset = GENET_WRAPPER_RESET_UNKNOWN;
    }
    return OS_EOK;
}
