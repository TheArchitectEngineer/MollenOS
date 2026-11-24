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
 * Delay helpers for usched waits. The waits take an absolute UTC time, and
 * these turn a delay in microseconds into that time.
 */

#include <errno.h>
#include <os/usched/job.h>
#include <os/usched/usched.h>
#include <time.h>

int
usched_deadline_after_us(
    _In_  uint64_t         microseconds,
    _Out_ struct timespec* until)
{
    struct timespec now;
    uint64_t        nanoseconds;

    if (until == NULL) {
        errno = EINVAL;
        return -1;
    }

    // timespec_get sets errno when it fails.
    if (timespec_get(&now, TIME_UTC) != TIME_UTC) {
        return -1;
    }

    // tv_nsec must stay below one second. Only the part of the delay below
    // one second is added to it, and the whole seconds of that sum are
    // carried into tv_sec together with the whole seconds of the delay.
    nanoseconds = (uint64_t)now.tv_nsec + (microseconds % USEC_PER_SEC) * NSEC_PER_USEC;
    until->tv_sec = now.tv_sec + (time_t)(microseconds / USEC_PER_SEC + nanoseconds / NSEC_PER_SEC);
    until->tv_nsec = (long)(nanoseconds % NSEC_PER_SEC);
    return 0;
}

int
usched_job_sleep_us(
    _In_ uint64_t microseconds)
{
    struct timespec until;

    // errno is already set by usched_deadline_after_us.
    if (usched_deadline_after_us(microseconds, &until) != 0) {
        return -1;
    }
    return usched_job_sleep(&until);
}
