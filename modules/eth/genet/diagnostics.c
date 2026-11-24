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
 * Optional GENET diagnostics. See diagnostics.h.
 */

#include "genet.h"

#ifdef VALI_ETHERNET_DIAGNOSTICS
// Log names of the packet events, in the order of GenetPacketEvent.
static const char* g_packetEvents[GENET_EVENT_COUNT] = {
    "TX queued", "TX completed", "RX received", "RX delivered",
    "RX filtered", "RX no-buffer", "RX invalid", "RX ring full"
};

void
GenetLogPacket(
    _In_ GenetController_t*    controller,
    _In_ enum GenetPacketEvent event,
    _In_ uint32_t              length,
    _In_ uint32_t              detail)
{
    uint64_t count;

    count = ++controller->Diagnostics.Packets[event];
    if (count <= GENET_LOGGED_EVENTS) {
        ETHERNET_LOG("GENET %s: count=%llu bytes=%u detail=0x%08x",
            g_packetEvents[event], (unsigned long long)count, length, detail);
    }
}

void
GenetLogInterrupt(
    _In_ GenetController_t* controller,
    _In_ uint32_t           causes)
{
    uint64_t count;

    count = ++controller->Diagnostics.Interrupts;
    if (count <= GENET_LOGGED_EVENTS) {
        ETHERNET_LOG("GENET interrupt: count=%llu causes=0x%08x",
            (unsigned long long)count, causes);
    }
}

void
GenetLogLinkStatus(
    _In_ GenetController_t* controller,
    _In_ oserr_t            status)
{
    if (status == controller->Diagnostics.LinkStatus) {
        return;
    }
    ETHERNET_LOG("GENET PHY poll status=%u", status);
    controller->Diagnostics.LinkStatus = status;
}

void
GenetLogProgress(
    _In_ GenetController_t* controller)
{
    GenetDiagnostics_t* diagnostics = &controller->Diagnostics;
    uint64_t            now;

    now = DDKGetTimeMs(OSTimeSource_MONOTONIC);
    if (now < diagnostics->NextReport) {
        return;
    }
    diagnostics->NextReport = now + GENET_PROGRESS_INTERVAL_MS;
    ETHERNET_LOG("GENET progress: carrier=%u speed=%u queues=%u TX-sw=%u/%u RX-sw=%u posted=%u TX-hw=%u RX-hw=%u",
        controller->Link.Applied.Carrier, controller->Link.Applied.Speed,
        controller->Queues.DmaState, controller->Queues.Transmit.Producer,
        controller->Queues.Transmit.Consumer, controller->Queues.Receive.DmaConsumer,
        controller->Queues.Receive.PostedSlots.Count,
        GenetRegisterRead(&controller->Registers,
            GENET_TX_RING + GENET_RING_HARDWARE_INDEX) & GENET_RING_INDEX_MASK,
        GenetRegisterRead(&controller->Registers,
            GENET_RX_RING + GENET_RING_HARDWARE_INDEX) & GENET_RING_INDEX_MASK);
    ETHERNET_LOG("GENET totals: TX-queued=%llu completed=%llu RX-received=%llu delivered=%llu filtered=%llu no-buffer=%llu invalid=%llu ring-full=%llu interrupts=%llu",
        (unsigned long long)diagnostics->Packets[GENET_EVENT_TX_QUEUED],
        (unsigned long long)diagnostics->Packets[GENET_EVENT_TX_COMPLETED],
        (unsigned long long)diagnostics->Packets[GENET_EVENT_RX_RECEIVED],
        (unsigned long long)diagnostics->Packets[GENET_EVENT_RX_DELIVERED],
        (unsigned long long)diagnostics->Packets[GENET_EVENT_RX_REJECTED],
        (unsigned long long)diagnostics->Packets[GENET_EVENT_RX_NO_BUFFER],
        (unsigned long long)diagnostics->Packets[GENET_EVENT_RX_ERROR],
        (unsigned long long)diagnostics->Packets[GENET_EVENT_RX_RING_FULL],
        (unsigned long long)diagnostics->Interrupts);
}
#endif
