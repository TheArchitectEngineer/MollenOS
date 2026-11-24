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
 * Time helpers for drivers: read a clock as one plain number for deadlines,
 * and busy-wait for short hardware delays. Built on OSGetTime and OSStall
 * from os/time.h.
 *
 * To sleep, or to wait on a usched condition with a timeout, use
 * usched_job_sleep_us and usched_deadline_after_us from the usched headers.
 */

#ifndef __DDK_TIME_H__
#define __DDK_TIME_H__

#include <ddk/ddkdefs.h>
#include <os/types/time.h>

_CODE_BEGIN

/**
 * @brief Reads a clock in whole microseconds. Use OSTimeSource_MONOTONIC for
 * deadlines and timeouts, as it counts from boot and never goes backwards.
 *
 * When the clock cannot be read, a warning is logged once and the largest
 * value read so far from the same source is returned again (zero before the
 * first good read). Time then seems to stand still, so a timeout ends later
 * than it should but never earlier.
 *
 * @param source Clock to read.
 * @return Time of the clock in microseconds.
 */
DDKDECL(uint64_t,
DDKGetTimeUs(
    _In_ enum OSTimeSource source));

/**
 * @brief Reads a clock in whole milliseconds, rounded down. Same clock and
 * same handling of read failures as DDKGetTimeUs.
 *
 * @param source Clock to read.
 * @return Time of the clock in milliseconds.
 */
DDKDECL(uint64_t,
DDKGetTimeMs(
    _In_ enum OSTimeSource source));

/**
 * @brief Busy-waits for at least the given time. The thread keeps its CPU in
 * the kernel (OSStall) for the whole wait, so use this only for short hardware
 * delays. The time is turned into nanoseconds in 64 bits, so any 32 bit
 * value is valid. A zero delay returns at once.
 *
 * @param microseconds Minimum time to wait in microseconds.
 * @return OS_EOK once the time has passed, or the error from OSStall.
 */
DDKDECL(oserr_t,
DDKStallUs(
    _In_ uint32_t microseconds));

_CODE_END
#endif //!__DDK_TIME_H__
