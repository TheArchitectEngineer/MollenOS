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
 * Checks and conversion of the ring 16 done thresholds. See thresholds.h.
 */

#include <ddk/utils.h>

#include "registers.h"
#include "thresholds.h"

oserr_t
GenetThresholdsValidate(
    _In_ const GenetThresholds_t* thresholds,
    _In_ uint32_t                 ringSize)
{
    // A zero TX count is rejected by Linux too. A count above the ring size
    // is rejected here because the ring can never hold that many frames.
    if (thresholds->TxFrames == 0 || thresholds->TxFrames > ringSize) {
        ERROR("GENET thresholds: TX frames %u is outside 1..%u", thresholds->TxFrames, ringSize);
        return OS_EINVALPARAMS;
    }

    if (thresholds->RxFrames > ringSize) {
        ERROR("GENET thresholds: RX frames %u is outside 0..%u", thresholds->RxFrames, ringSize);
        return OS_EINVALPARAMS;
    }

    if (thresholds->RxTimeoutUs > GENET_RX_TIMEOUT_MAX_US) {
        ERROR("GENET thresholds: RX timeout %u us is above %u us",
            thresholds->RxTimeoutUs, GENET_RX_TIMEOUT_MAX_US);
        return OS_EINVALPARAMS;
    }

    // With a count of one, the count always fires first. Any other count needs
    // the timeout, so frames that never reach it still get delivered.
    if (thresholds->RxFrames != 1 && thresholds->RxTimeoutUs == 0) {
        ERROR("GENET thresholds: RX frames %u needs a nonzero RX timeout", thresholds->RxFrames);
        return OS_EINVALPARAMS;
    }
    return OS_EOK;
}

uint32_t
GenetThresholdsTimeoutTicks(
    _In_ uint32_t microseconds)
{
    uint64_t nanoseconds = (uint64_t)microseconds * 1000;

    return (uint32_t)((nanoseconds + GENET_DMA_TIMEOUT_TICK_NS - 1) / GENET_DMA_TIMEOUT_TICK_NS);
}
