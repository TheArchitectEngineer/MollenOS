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
 * Regular work on a controller: link polling, ring service and teardown
 * retries. See service.h.
 */

#include <ddk/utils.h>

#include "genet.h"

uint32_t
GenetControllerProcessInterval(
    _In_ const GenetController_t* controller)
{
    return controller->Interrupt.State == GENET_INTERRUPT_ENABLED ?
        GENET_MAINTENANCE_INTERVAL_MS : GENET_POLL_INTERVAL_MS;
}

void
GenetControllerProcessQueues(
    _InOut_ GenetController_t* controller)
{
    uint32_t causes;
    oserr_t  status;

    // Take the recorded causes before emptying the rings. A cause the handler
    // records after this point is kept for the next pass, so a frame that
    // arrives while we work always has an interrupt waiting for it.
    causes = GenetInterruptTakeCauses(controller);

    // Teardown can leave DMA running after a failed stop. A late event must
    // not resume packet processing or unmask causes while cleanup retries.
    if (controller->State != GENET_STATE_READY) {
        return;
    }
    EthernetPoll(&controller->Adapter);

    // After a stop the queues are not running, so the causes must stay masked.
    // The next start unmasks them.
    if (causes == 0 || controller->Queues.DmaState != GENET_DMA_STATE_MAY_BE_RUNNING) {
        return;
    }

    // Unmask only after the rings are empty. With a done threshold of one, the
    // card would otherwise interrupt for every frame while we are still working.
    status = GenetMacUnmaskDmaInterrupts(controller, causes);
    if (status != OS_EOK) {
        ERROR("GENET interrupt: unmasking causes 0x%x failed: %u", causes, status);
        EthernetFault(&controller->Adapter, status);
    }
}

void
GenetControllerProcessInterrupt(
    _InOut_ GenetController_t* controller)
{
    // Reset the event before taking the causes. If the handler runs after this
    // point, it signals again, so its causes always get a wake-up.
    GenetInterruptResetEvent(controller);
    GenetControllerProcessQueues(controller);
}

/**
 * Work out how long to wait after the given number of failed teardown
 * attempts in a row. The first failure waits
 * GENET_TEARDOWN_RETRY_INTERVAL_MS, and each further one doubles the wait up
 * to GENET_TEARDOWN_RETRY_MAX_INTERVAL_MS.
 */
static uint32_t
__TeardownRetryWait(
    _In_ uint32_t failures)
{
    uint32_t waitMs = GENET_TEARDOWN_RETRY_INTERVAL_MS;

    for (uint32_t i = 1; i < failures && waitMs < GENET_TEARDOWN_RETRY_MAX_INTERVAL_MS; i++) {
        waitMs *= 2;
    }
    return waitMs < GENET_TEARDOWN_RETRY_MAX_INTERVAL_MS ? waitMs : GENET_TEARDOWN_RETRY_MAX_INTERVAL_MS;
}

oserr_t
GenetControllerShutdown(
    _InOut_ GenetController_t* controller)
{
    GenetTeardownRetry_t* retry = &controller->Teardown;
    uint32_t              waitMs;
    oserr_t               status;

    status = GenetControllerDestroy(controller);
    if (status == OS_EOK) {
        if (retry->Failures != 0) {
            WARNING("GENET: teardown finished after %u failed attempts", retry->Failures);
        }
        retry->Failures = 0;
        return OS_EOK;
    }

    // The steps inside the teardown log what went wrong. Only the first
    // failure is an error here. Later ones repeat it, so they are warnings.
    if (retry->Failures < UINT32_MAX) {
        retry->Failures++;
    }
    waitMs = __TeardownRetryWait(retry->Failures);
    if (retry->Failures == 1) {
        ERROR("GENET: teardown failed (%u), trying again in %u ms", status, waitMs);
    } else {
        WARNING("GENET: teardown attempt %u failed (%u), trying again in %u ms",
            retry->Failures, status, waitMs);
    }
    retry->NextAttempt = DDKGetTimeMs(OSTimeSource_MONOTONIC) + waitMs;
    return status;
}

/**
 * Run a pass for a controller that is being torn down. Makes a teardown
 * attempt if one is due.
 * @return Milliseconds until the next attempt is due. After a successful
 *         teardown the value no longer matters, since the caller frees the
 *         controller.
 */
static uint32_t
__TeardownPass(
    _InOut_ GenetController_t* controller,
    _In_    uint64_t           now)
{
    oserr_t status;

    if (now >= controller->Teardown.NextAttempt) {
        // GenetControllerShutdown logs the failure and moves NextAttempt forward.
        status = GenetControllerShutdown(controller);
        if (status == OS_EOK) {
            return GENET_POLL_INTERVAL_MS;
        }
    }

    if (controller->Teardown.NextAttempt <= now) {
        return GENET_POLL_INTERVAL_MS;
    }
    return (uint32_t)(controller->Teardown.NextAttempt - now);
}

uint32_t
GenetControllerProcess(
    _InOut_ GenetController_t* controller)
{
    uint64_t now;

    now = DDKGetTimeMs(OSTimeSource_MONOTONIC);
    if (controller->State != GENET_STATE_READY) {
        return __TeardownPass(controller, now);
    }

    // EthernetReadConfiguration faults the session and logs when the read
    // fails, so its status needs no handling here.
    if (now >= controller->Link.NextPoll) {
        (void)EthernetReadConfiguration(&controller->Adapter);
        controller->Link.NextPoll = now + GenetLinkPollInterval(controller);
    }
    GenetLogProgress(controller);
    GenetControllerProcessQueues(controller);
    return GenetControllerProcessInterval(controller);
}
