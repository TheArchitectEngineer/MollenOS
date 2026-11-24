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
 * Optional GENET diagnostics: packet and interrupt counters, a log of the
 * first few events of each kind, and a summary every few seconds. Everything
 * here compiles to nothing unless VALI_ETHERNET_DIAGNOSTICS is set.
 */

#ifndef __GENET_DIAGNOSTICS_H__
#define __GENET_DIAGNOSTICS_H__

#include <os/osdefs.h>
#include <stdint.h>

#include "../common/diagnostics.h"

DECL_STRUCT(GenetController);

// Number of events of each kind that are written to the log. Later ones are
// only counted, so a busy link cannot flood the log.
#define GENET_LOGGED_EVENTS 8U

// Time between two summaries of the totals and the queue state.
#define GENET_PROGRESS_INTERVAL_MS 5000U

/** The steps a packet goes through. Each one has its own counter. */
enum GenetPacketEvent {
    GENET_EVENT_TX_QUEUED,
    GENET_EVENT_TX_COMPLETED,
    GENET_EVENT_RX_RECEIVED,
    GENET_EVENT_RX_DELIVERED,
    GENET_EVENT_RX_REJECTED,
    GENET_EVENT_RX_NO_BUFFER,
    GENET_EVENT_RX_ERROR,
    GENET_EVENT_RX_RING_FULL,
    GENET_EVENT_COUNT
};

/**
 * @brief Diagnostic totals. They keep counting across queue restarts. Packet
 * contents are never recorded.
 * Packets has one counter per GenetPacketEvent. Interrupts counts wake-ups
 * that carried a cause. NextReport is the monotonic time in milliseconds
 * (DDKGetTimeMs) of the next summary. LinkStatus is the result of the last PHY link read, kept so only
 * changes are logged.
 */
typedef struct GenetDiagnostics {
    uint64_t Packets[GENET_EVENT_COUNT];
    uint64_t Interrupts;
    uint64_t NextReport;
    oserr_t  LinkStatus;
} GenetDiagnostics_t;

#ifdef VALI_ETHERNET_DIAGNOSTICS
/**
 * @brief Count a packet event. Only the first GENET_LOGGED_EVENTS of each kind are logged.
 * @param controller GENET state protected by the adapter lock.
 * @param event Which step of packet handling this is.
 * @param length Frame length in bytes, or the length the card reported for a rejected descriptor.
 * @param detail Descriptor status or EtherType, depending on the event.
 */
__EXTERN void
GenetLogPacket(
    _In_ GenetController_t*    controller,
    _In_ enum GenetPacketEvent event,
    _In_ uint32_t              length,
    _In_ uint32_t              detail);

/**
 * @brief Count a handled interrupt. Only the first GENET_LOGGED_EVENTS are logged.
 * @param controller GENET state protected by the adapter lock.
 * @param causes GENET_INTR0_* bits the fast handler reported.
 */
__EXTERN void
GenetLogInterrupt(
    _In_ GenetController_t* controller,
    _In_ uint32_t           causes);

/**
 * @brief Log the result of a PHY link read, but only when it differs from the
 * result of the read before it.
 * @param controller GENET state protected by the adapter lock.
 * @param status Result of the PHY link read.
 */
__EXTERN void
GenetLogLinkStatus(
    _In_ GenetController_t* controller,
    _In_ oserr_t            status);

/**
 * @brief Log the totals and the queue state, at most once every GENET_PROGRESS_INTERVAL_MS.
 * @param controller GENET state with mapped registers, protected by the adapter lock.
 */
__EXTERN void
GenetLogProgress(
    _In_ GenetController_t* controller);
#else
// Arguments are not evaluated when diagnostics are disabled.
#define GenetLogPacket(controller, event, length, detail) ((void)0)
#define GenetLogInterrupt(controller, causes)             ((void)0)
#define GenetLogLinkStatus(controller, status)            ((void)0)
#define GenetLogProgress(controller)                      ((void)0)
#endif

#endif //!__GENET_DIAGNOSTICS_H__
