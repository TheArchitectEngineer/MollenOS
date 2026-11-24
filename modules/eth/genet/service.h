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
 * Regular work on a controller, run from the module's poll worker and from
 * its interrupt wake-ups: reading the link, emptying the rings, unmasking the
 * packet causes, and retrying a teardown that failed. All timing decisions of
 * the driver are made here, so main.c only has to sleep for the time it is
 * told.
 */

#ifndef __GENET_SERVICE_H__
#define __GENET_SERVICE_H__

#include <os/osdefs.h>

DECL_STRUCT(GenetController);

// Time between passes when polling is the only way to find packets.
#define GENET_POLL_INTERVAL_MS 10U

// Time between passes while interrupts deliver packets. The pass still reads
// the PHY link, checks for stuck transmits, and empties the rings in case an
// interrupt got lost.
#define GENET_MAINTENANCE_INTERVAL_MS 100U

// Time before a failed controller teardown is tried again. Each attempt can
// stall in the DMA stop with the adapter lock held, and the steps it runs log
// their own failures. So the wait doubles after every failed attempt, up to
// GENET_TEARDOWN_RETRY_MAX_INTERVAL_MS.
#define GENET_TEARDOWN_RETRY_INTERVAL_MS     1000U
#define GENET_TEARDOWN_RETRY_MAX_INTERVAL_MS 30000U

/**
 * @brief Retry schedule of a teardown that failed, protected by the adapter lock.
 * Failures counts failed attempts in a row and decides how long the next wait
 * is. NextAttempt is the monotonic time in milliseconds (DDKGetTimeMs)
 * before which the poll worker makes no new attempt. Zeroed storage means no attempt has failed yet.
 */
typedef struct GenetTeardownRetry {
    uint64_t NextAttempt;
    uint32_t Failures;
} GenetTeardownRetry_t;

/**
 * @brief Run one pass of the poll worker for a controller.
 * A ready controller gets its PHY link read when that is due, and its rings
 * emptied. A controller that has left GENET_STATE_READY gets a teardown
 * attempt when one is due. If the pass leaves the controller in
 * GENET_STATE_DESTROYED, the caller must free it and must not pass it again.
 * @param controller GENET state protected by the adapter lock.
 * @return Milliseconds to wait before the next pass.
 */
__EXTERN uint32_t
GenetControllerProcess(
    _InOut_ GenetController_t* controller);

/**
 * @brief Handle a wake-up from the fast interrupt handler.
 * Resets the event, then calls GenetControllerProcessQueues. If the event arrives after
 * its causes were already taken, the only cost is one extra pass over the
 * rings.
 * @param controller GENET state with a registered packet interrupt, protected
 *                   by the adapter lock.
 */
__EXTERN void
GenetControllerProcessInterrupt(
    _InOut_ GenetController_t* controller);

/**
 * @brief Empty both rings, then unmask the causes the fast handler masked.
 * The causes are taken first, so one that the handler masks while the rings
 * are being emptied is kept for the next call. Unmasking last stops the card
 * from interrupting for every frame while the rings are worked. If the unmask
 * fails, the adapter is marked as faulted, because those causes would
 * otherwise stay silent until the queues restart. Safe to call without
 * interrupts, where it only empties the rings. Does nothing to the rings once
 * the controller has left GENET_STATE_READY.
 * @param controller GENET state protected by the adapter lock.
 */
__EXTERN void
GenetControllerProcessQueues(
    _InOut_ GenetController_t* controller);

/**
 * @brief Say how long a ready controller waits between two passes.
 * @param controller GENET state protected by the adapter lock.
 * @return GENET_MAINTENANCE_INTERVAL_MS with interrupts, else GENET_POLL_INTERVAL_MS.
 */
__EXTERN uint32_t
GenetControllerProcessInterval(
    _In_ const GenetController_t* controller);

/**
 * @brief Try once to tear down the controller, and schedule the next attempt
 * if this one fails. The wait before the next attempt doubles after every
 * failure. Each failure is logged, and so is a success that follows failures.
 * @param controller GENET state protected by the adapter lock.
 * @return OS_EOK once the controller is destroyed and can be freed, otherwise
 *         the error from GenetControllerDestroy.
 */
__EXTERN oserr_t
GenetControllerShutdown(
    _InOut_ GenetController_t* controller);

#endif //!__GENET_SERVICE_H__
