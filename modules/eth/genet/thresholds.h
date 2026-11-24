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
 * Interrupt thresholds for ring 16. The card does not interrupt the CPU for
 * every single frame. Each ring raises its "done" cause only after a set
 * number of frames, which is called its done threshold. The values below
 * decide how many frames that is, and how long a received frame may wait for
 * the others. Linux calls this interrupt coalescing, and the values start out
 * the same as there. To change them, set the VALI_GENET_* CMake variables
 * (which define the GENET_DEFAULT_* macros), or fill in GenetThresholds_t
 * before the queues are prepared.
 */

#ifndef __GENET_THRESHOLDS_H__
#define __GENET_THRESHOLDS_H__

#include <ddk/ddkdefs.h>
#include <stdint.h>

/**
 * Frames sent before the TX ring raises its done cause. The card also raises
 * it as soon as the ring runs empty, and it has no timeout register for
 * sending. A single frame is therefore reported right away, while a busy ring
 * raises one cause per ten frames. Linux uses ten.
 */
#ifndef GENET_DEFAULT_TX_DONE_FRAMES
#define GENET_DEFAULT_TX_DONE_FRAMES 10U
#endif

/**
 * Frames received before the RX ring raises its done cause. One means an
 * interrupt for every frame, as in Linux. This costs little. The service masks
 * (switches off) the cause while it empties the ring, so on a busy ring many
 * frames already share one interrupt. Raise the number only if measurements
 * show the interrupts are too expensive, and keep the timeout below set. Zero
 * lets the timeout alone decide.
 */
#ifndef GENET_DEFAULT_RX_DONE_FRAMES
#define GENET_DEFAULT_RX_DONE_FRAMES 1U
#endif

/**
 * How long a received frame may wait, in microseconds, when the frame count
 * has not been reached. The card counts time in steps of 8.192 us and the
 * value is rounded up, so the default of 50 becomes 7 steps (57 us). When the
 * frame count is one, the count always fires first. Linux still sets the
 * timeout in that case, so this driver does too.
 */
#ifndef GENET_DEFAULT_RX_TIMEOUT_US
#define GENET_DEFAULT_RX_TIMEOUT_US 50U
#endif

/**
 * Longest timeout the card's 16-bit step counter can hold. This counts each
 * 8.192 us step as 8 us and adds one, which stays just below the true limit.
 * Linux uses the same limit.
 */
#define GENET_RX_TIMEOUT_MAX_US ((0xffffU * 8U) + 1U)

/**
 * @brief Done thresholds for the ring 16 queues.
 * TxFrames must be at least one. RxFrames is either zero (only the timeout
 * decides) or at least one. RxTimeoutUs must be nonzero unless RxFrames is
 * one, because otherwise frames that never reach the count would wait for the
 * next poll. Neither frame count may be larger than the ring, since the ring
 * could never hold that many frames.
 */
typedef struct GenetThresholds {
    uint32_t TxFrames;
    uint32_t RxFrames;
    uint32_t RxTimeoutUs;
} GenetThresholds_t;

/**
 * @brief Fill in the thresholds from the GENET_DEFAULT_* macros.
 * @param thresholds Receives the default values.
 */
static inline void
GenetThresholdsDefaults(
    _Out_ GenetThresholds_t* thresholds)
{
    thresholds->TxFrames = GENET_DEFAULT_TX_DONE_FRAMES;
    thresholds->RxFrames = GENET_DEFAULT_RX_DONE_FRAMES;
    thresholds->RxTimeoutUs = GENET_DEFAULT_RX_TIMEOUT_US;
}

/**
 * @brief Check the values against the rules listed on GenetThresholds_t.
 * Logs the first rule that fails.
 * @param thresholds Values to check.
 * @param ringSize Number of descriptors (frame slots) in each ring.
 * @return OS_EOK, or OS_EINVALPARAMS. The log names the field that failed.
 */
__EXTERN oserr_t
GenetThresholdsValidate(
    _In_ const GenetThresholds_t* thresholds,
    _In_ uint32_t                 ringSize);

/**
 * @brief Convert a receive timeout to the card's step count.
 * @param microseconds Timeout in microseconds, at most GENET_RX_TIMEOUT_MAX_US.
 * @return Number of 8.192 us steps, rounded up so the wait is never shorter than asked.
 */
__EXTERN uint32_t
GenetThresholdsTimeoutTicks(
    _In_ uint32_t microseconds);

#endif //!__GENET_THRESHOLDS_H__
