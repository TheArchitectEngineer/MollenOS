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
 * GENET module entry points. This file owns the one controller the module
 * supports, the poll worker that calls into service.c, and the handlers the
 * module runtime and the device manager call. It takes the adapter lock
 * around every call into the driver. What the driver does on each pass, and
 * how long the worker sleeps, is decided in service.c.
 */

#include <ctt_driver_service_server.h>
#include <ctt_netadapter_service.h>
#include <sys_device_service_client.h>
#include <ddk/convert.h>
#include <ddk/time.h>
#include <ddk/utils.h>
#include <os/time.h>
#include <os/usched/job.h>
#include <errno.h>
#include <ioset.h>
#include <stdlib.h>

#include "genet.h"

// Time between teardown attempts while the module unloads. Unloading must not
// finish before the DMA engines are confirmed stopped, so it keeps trying.
// The steps inside a teardown log their own failures, so this stays at a
// second to keep the log readable.
#define GENET_UNLOAD_RETRY_INTERVAL_MS 1000U
// Failed unload attempts between two log lines. With the interval above that
// is one line about every ten seconds.
#define GENET_UNLOAD_LOG_INTERVAL 10U

// The one controller this module drives, protected by the adapter lock. NULL
// until a device is registered and again once it is destroyed.
static GenetController_t* g_controller;
static uuid_t             g_worker = UUID_INVALID;

/**
 * Free a device description made by from_sys_device, which copies every
 * identification string.
 */
static void
__FreeDescriptor(
    _In_ Device_t* device)
{
    free(device->Identification.Description);
    free(device->Identification.Manufacturer);
    free(device->Identification.Product);
    free(device->Identification.Revision);
    free(device->Identification.Serial);
    free(device);
}

/**
 * Free a destroyed controller and its device description. The caller must
 * hold the adapter lock, and the controller must be in GENET_STATE_DESTROYED.
 */
static void
__ReleaseController(void)
{
    __FreeDescriptor(g_controller->Device);
    free(g_controller);
    g_controller = NULL;
}

/**
 * Tear down and free the controller, if there is one. The caller must hold
 * the adapter lock.
 * @return OS_EOK when no controller is left. Otherwise the teardown error,
 *         and the controller is kept so the call can be made again.
 */
static oserr_t
__Destroy(void)
{
    oserr_t status;

    if (g_controller == NULL) {
        return OS_EOK;
    }

    // GenetControllerDestroy logs the step that failed.
    status = GenetControllerDestroy(g_controller);
    if (status != OS_EOK) {
        return status;
    }
    __ReleaseController();
    return OS_EOK;
}

/**
 * Block the calling thread for the given time. The kernel scheduler runs
 * other threads meanwhile. If the clock or the sleep fails, it falls back to
 * DDKStallUs, which busy-waits in the kernel.
 */
static void
__SleepMilliseconds(
    _In_ uint32_t milliseconds)
{
    OSTimestamp_t deadline;
    oserr_t       status;

    status = OSGetTime(OSTimeSource_UTC, &deadline);
    if (status == OS_EOK) {
        OSTimestampAddNsec(&deadline, &deadline, (int64_t)milliseconds * NSEC_PER_MSEC);
        status = OSSleep(&deadline, NULL);
    }
    if (status == OS_EOK) {
        return;
    }

    WARNING("GENET: sleeping for %u ms failed (%u), busy-waiting instead", milliseconds, status);
    status = DDKStallUs(milliseconds * USEC_PER_MSEC);
    if (status != OS_EOK) {
        ERROR("GENET: busy-waiting for %u ms failed: %u", milliseconds, status);
    }
}

/**
 * Run one pass of the poll worker. The caller must hold the adapter lock.
 * @return Milliseconds until the next pass.
 */
static uint32_t
__PollOnce(void)
{
    uint32_t periodMs;

    // Without a controller there is nothing to wake up for, so the period
    // does not matter then.
    if (g_controller == NULL) {
        return GENET_POLL_INTERVAL_MS;
    }

    // A pass can finish a teardown that failed earlier. The controller must
    // then be freed here, before anything else can reach it.
    periodMs = GenetControllerProcess(g_controller);
    if (g_controller->State == GENET_STATE_DESTROYED) {
        __ReleaseController();
    }
    return periodMs;
}

static void
__Poll(
    _In_ void* unused,
    _In_ void* cancellation)
{
    uint32_t periodMs;
    (void)unused;

    while (!usched_is_cancelled(cancellation)) {
        EthernetLock();
        periodMs = __PollOnce();
        EthernetUnlock();

        // The job sleep fails only when the UTC clock cannot be read. Block
        // the thread for the period then, so the worker does not spin.
        if (usched_job_sleep_us((uint64_t)periodMs * USEC_PER_MSEC) != 0) {
            WARNING_ONCE(genet_poll_sleep, "GENET: the poll worker could not sleep (errno %i), blocking the thread instead",
                errno);
            __SleepMilliseconds(periodMs);
        }
    }
}

oserr_t
OnLoad(void)
{
    oserr_t status;

    status = EthernetServiceInitialize();
    if (status != OS_EOK) {
        ERROR("GENET: setting up the common Ethernet service failed: %u", status);
        return status;
    }

    if (gracht_server_register_protocol(EthernetServer(), &ctt_driver_server_protocol)) {
        ERROR("GENET: registering the driver protocol failed");
        return OS_EUNKNOWN;
    }

    g_worker = usched_job_queue(__Poll, NULL);
    if (g_worker == UUID_INVALID) {
        ERROR("GENET: starting the poll worker failed");
        return OS_EOOM;
    }
    return OS_EOK;
}

void
OnUnload(void)
{
    oserr_t  status;
    uint32_t attempts;
    int      exitCode;

    // usched_job_join always writes the exit code, so it needs a variable even
    // though the value is not used. The join fails only for a job ID that the
    // scheduler does not know. Teardown must still stop the hardware, so the
    // failure is logged and unloading goes on.
    if (g_worker != UUID_INVALID) {
        usched_job_cancel(g_worker);
        if (usched_job_join(g_worker, &exitCode) != 0) {
            WARNING("GENET: waiting for the poll worker to end failed");
        }
        g_worker = UUID_INVALID;
    }

    // A PCI device can be told to stop accessing memory, but this MAC cannot.
    // Do not exit and free its DMA storage until the hardware is confirmed
    // stopped, or the card could write into memory we no longer own. The
    // wait has no end, so it is logged when it starts and then now and then.
    for (attempts = 0; ; attempts++) {
        EthernetLock();
        status = __Destroy();
        EthernetUnlock();
        if (status == OS_EOK) {
            break;
        }

        if (attempts == 0) {
            ERROR("GENET unload: teardown failed (%u), waiting for the DMA engines to stop", status);
        } else if (attempts % GENET_UNLOAD_LOG_INTERVAL == 0) {
            WARNING("GENET unload: still waiting for the DMA engines to stop after %u attempts (%u)",
                attempts + 1, status);
        }
        __SleepMilliseconds(GENET_UNLOAD_RETRY_INTERVAL_MS);
    }

    if (attempts != 0) {
        WARNING("GENET unload: teardown finished after %u failed attempts", attempts);
    }
}

oserr_t
OnEvent(
    _In_ struct ioset_event* event)
{
    if (event == NULL || !(event->events & IOSETSYN)) {
        return OS_ENOENT;
    }

    // Every event other than the packet interrupt belongs to the gracht server.
    // By the time a queued event is handled, the controller may be gone or its
    // descriptor may already be closed.
    EthernetLock();
    if (g_controller == NULL || !GenetInterruptIsEvent(g_controller, event->data.iod)) {
        EthernetUnlock();
        return OS_ENOENT;
    }
    GenetControllerProcessInterrupt(g_controller);
    EthernetUnlock();

    // The event is ours, so report it as handled. The interrupt descriptor
    // must never go to gracht, which would treat it as an IPC link.
    return OS_EOK;
}

void
ctt_driver_register_device_invocation(
    _In_ struct gracht_message*   message,
    _In_ const struct sys_device* description)
{
    Device_t* device;
    oserr_t   status;
    (void)message;

    // Only platform devices can be this MAC. Other descriptions are not ours.
    if (description->content_type != SYS_DEVICE_CONTENT_PLATFORM) {
        return;
    }

    device = from_sys_device(description);
    if (device == NULL) {
        ERROR("GENET: converting a platform device description failed");
        return;
    }

    EthernetLock();
    if (g_controller != NULL) {
        WARNING("GENET: ignoring device %u, the driver supports one controller and already has device %u",
            device->Id, g_controller->Device->Id);
        __FreeDescriptor(device);
        EthernetUnlock();
        return;
    }

    g_controller = calloc(1, sizeof(GenetController_t));
    if (g_controller == NULL) {
        ERROR("GENET: allocating the controller for device %u failed", device->Id);
        __FreeDescriptor(device);
        EthernetUnlock();
        return;
    }

    // A failed creation leaves partial state behind. Tear it down right away.
    // If that fails too, the poll worker tries again later.
    status = GenetControllerCreate(g_controller, device);
    if (status != OS_EOK) {
        ERROR("GENET: setting up device %u failed: %u", device->Id, status);
        status = GenetControllerShutdown(g_controller);
        if (status == OS_EOK) {
            __ReleaseController();
        }
    }
    EthernetUnlock();
}

void
ctt_driver_get_device_protocols_invocation(
    _In_ struct gracht_message* message,
    _In_ uuid_t                 deviceId)
{
    EthernetLock();
    if (EthernetFindDevice(deviceId) != NULL) {
        ctt_driver_event_device_protocol_single(
            EthernetServer(), message->client,
            deviceId,
            "netadapter",
            SERVICE_CTT_NETADAPTER_ID
        );
    }
    EthernetUnlock();
}

void
ctt_driver_ioctl_invocation(
    _In_ struct gracht_message* message,
    _In_ uuid_t                 deviceId,
    _In_ unsigned int           request,
    _In_ const uint8_t*         data,
    _In_ uint32_t               count)
{
    (void)deviceId;
    (void)request;
    (void)data;
    (void)count;
    ctt_driver_ioctl_response(message, NULL, 0, OS_ENOTSUPPORTED);
}

oserr_t
OnUnregister(
    _In_ Device_t* device)
{
    oserr_t status = OS_ENOENT;

    // If the teardown fails here, the controller leaves GENET_STATE_READY and
    // the poll worker keeps trying.
    EthernetLock();
    if (g_controller && g_controller->Device->Id == device->Id) {
        status = __Destroy();
    }
    EthernetUnlock();
    return status;
}

// Events of the device service client. The generated client code calls these
// handlers, so they must exist. This driver has no use for the events.
void
sys_device_event_protocol_device_invocation(
    _In_ gracht_client_t* client,
    _In_ uuid_t           deviceId,
    _In_ uuid_t           driverId,
    _In_ uint8_t          protocolId)
{
    (void)client;
    (void)deviceId;
    (void)driverId;
    (void)protocolId;
}

void
sys_device_event_device_update_invocation(
    _In_ gracht_client_t* client,
    _In_ uuid_t           deviceId,
    _In_ uint8_t          connected)
{
    (void)client;
    (void)deviceId;
    (void)connected;
}
