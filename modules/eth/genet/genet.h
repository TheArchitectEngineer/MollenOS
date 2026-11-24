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
 * State of one BCM2711 GENET v5 controller, and its creation and destruction.
 * Each part of the driver has its own header, and this file ties them
 * together:
 *
 * - wrapper.h    Pi 4 wiring around the MAC: port mode, RGMII, MDIO clock, reset line.
 * - mac.h        UniMAC reset, address, receive filters and interrupt masks.
 * - mdio.h       Register access to the PHY over the management bus.
 * - link.h       PHY link reads and the MAC speed and duplex settings.
 * - queues.h     Ring 16 setup, start and stop.
 * - transfer.h   Frames through the running queues.
 * - thresholds.h How many frames the card gathers before it interrupts.
 * - interrupt.h  The packet interrupt and its fast handler.
 * - service.h    Regular work: link polling, ring service, teardown retries.
 * - registers.h  Register map and register access.
 */

#ifndef __GENET_H__
#define __GENET_H__

#include <ddk/time.h>

#include "../common/mac.h"
#include "../common/netadapter.h"
#include "diagnostics.h"
#include "interrupt.h"
#include "link.h"
#include "mac.h"
#include "mdio.h"
#include "queues.h"
#include "registers.h"
#include "service.h"
#include "thresholds.h"
#include "transfer.h"
#include "wrapper.h"

/**
 * Controller lifetime, protected by the adapter lock. Creation advances from
 * GENET_STATE_NEW through GENET_STATE_PREPARED and GENET_STATE_MAPPED to
 * GENET_STATE_READY. Failed creation leaves the last completed stage for
 * GenetControllerDestroy. Registers stay mapped from GENET_STATE_MAPPED through
 * GENET_STATE_UNMAPPING, including a failed unmap. GENET_STATE_DESTROYED is
 * terminal.
 */
enum GenetState {
    GENET_STATE_NEW,
    GENET_STATE_PREPARED,
    GENET_STATE_MAPPED,
    GENET_STATE_READY,
    // Unpublished. Queue shutdown must succeed before memory can be freed.
    GENET_STATE_STOPPING,
    // Queue shutdown completed. Remaining resource releases may be retried.
    GENET_STATE_STOPPED,
    // The platform has stopped. Only the register mapping remains.
    GENET_STATE_UNMAPPING,
    GENET_STATE_DESTROYED
};

/**
 * @brief State of one BCM2711 GENET v5 controller. The adapter lock serializes access.
 * Adapter is the handle the common adapter code works with, and its
 * Controller field points back at this structure. Device is the platform
 * device description, which the module frees after the controller is
 * destroyed. Registers is the mapped register window. Configuration is what
 * firmware said about the MAC, the PHY and their wiring, and Mac is the
 * address in use.
 * Mdio and Bus give the common PHY code access to the management bus. Phy.Bus
 * is set when PHY initialization begins, including failed attempts. Cleanup
 * clears it after attempting PhyStop, once DMA has stopped.
 * Dma is the context the frame storage in Queues is allocated from.
 * State is the controller lifetime. Teardown holds the retry schedule once a
 * teardown attempt has failed.
 */
struct GenetController {
    EthernetDevice_t                   Adapter;
    Device_t*                          Device;
    DeviceIo_t                         Registers;
    struct DeviceEthernetConfiguration Configuration;
    uint8_t                            Mac[GENET_MAC_ADDRESS_SIZE];
    enum GenetState                    State;
    GenetWrapper_t                     Wrapper;
    GenetInterrupt_t                   Interrupt;
    GenetMdio_t                        Mdio;
    MdioBus_t                          Bus;
    PhyDevice_t                        Phy;
    GenetLink_t                        Link;
    struct DMAContext*                 Dma;
    GenetQueues_t                      Queues;
    GenetThresholds_t                  Thresholds;
    GenetTeardownRetry_t               Teardown;
#ifdef VALI_ETHERNET_DIAGNOSTICS
    GenetDiagnostics_t                 Diagnostics;
#endif
};

/**
 * @brief Check whether the controller still has its register mapping.
 * @param controller GENET state protected by the adapter lock.
 * @return true from successful mapping until successful unmapping.
 */
static inline bool
GenetRegistersMapped(
    _In_ const GenetController_t* controller)
{
    return controller->State >= GENET_STATE_MAPPED && controller->State <= GENET_STATE_UNMAPPING;
}

/**
 * @brief Set up a Pi 4 controller and publish its network adapter. The state
 * must start out zeroed. If this fails, the partial state is kept so
 * GenetControllerDestroy can clean it up, and the step that failed is logged.
 * @param controller GENET state protected by the adapter lock.
 * @param device Platform device description. It must stay valid until the
 *               controller is destroyed.
 * @return OS_EOK, OS_EBUSY if the controller has already left GENET_STATE_NEW,
 *         OS_ENOTSUPPORTED if the device is not a BCM2711 GENET v5, or the
 *         status of the setup step that failed.
 */
__EXTERN oserr_t
GenetControllerCreate(
    _InOut_ GenetController_t* controller,
    _In_    Device_t*          device);

/**
 * @brief Stop DMA and release all resources. On failure the state is kept so
 * the call can be retried, and it then continues from the stage that failed.
 * @param controller GENET state protected by the adapter lock.
 * @return OS_EOK once the controller has reached GENET_STATE_DESTROYED,
 *         otherwise the error of the step that has to be tried again.
 */
__EXTERN oserr_t
GenetControllerDestroy(
    _InOut_ GenetController_t* controller);

#endif //!__GENET_H__
