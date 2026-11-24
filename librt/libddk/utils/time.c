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
 * Time helpers for drivers, over OSGetTime and OSStall.
 */

#include <ddk/time.h>
#include <ddk/utils.h>
#include <os/spinlock.h>
#include <os/time.h>

// One slot per enum OSTimeSource value. OSTimeSource_UTC is the last one.
#define __TIME_SOURCE_COUNT (OSTimeSource_UTC + 1)

// The last value read from each clock, in microseconds. A failed read returns
// it again. A spinlock guards it, since i386 has no lock-free 64 bit atomics.
static spinlock_t g_lastReadLock = _SPN_INITIALIZER_NP;
static uint64_t   g_lastReadUs[__TIME_SOURCE_COUNT];

// Stores a good read. Threads can store their reads in a different order
// than they made them, so a smaller value never replaces a larger one. On a
// monotonic clock the stored value then never goes backwards.
static void
__StoreLastRead(
    _In_ enum OSTimeSource source,
    _In_ uint64_t          microseconds)
{
    spinlock_acquire(&g_lastReadLock);
    if (microseconds > g_lastReadUs[source]) {
        g_lastReadUs[source] = microseconds;
    }
    spinlock_release(&g_lastReadLock);
}

static uint64_t
__LoadLastRead(
    _In_ enum OSTimeSource source)
{
    uint64_t microseconds;

    spinlock_acquire(&g_lastReadLock);
    microseconds = g_lastReadUs[source];
    spinlock_release(&g_lastReadLock);
    return microseconds;
}

uint64_t
DDKGetTimeUs(
    _In_ enum OSTimeSource source)
{
    OSTimestamp_t timestamp;
    uint64_t      microseconds;
    oserr_t       oserr;

    // The source indexes g_lastReadUs, so an unknown source must stop here.
    if ((unsigned int)source >= __TIME_SOURCE_COUNT) {
        ERROR("DDK: reading the time failed, clock %u does not exist", source);
        return 0;
    }

    oserr = OSGetTime(source, &timestamp);
    if (oserr != OS_EOK) {
        WARNING_ONCE(ddk_time_read,
            "DDK: reading clock %u failed (%u), the last value read is used until it works",
            source, oserr);
        return __LoadLastRead(source);
    }

    // A negative time has no unsigned microsecond count, so it is handled as
    // a failed read.
    if (timestamp.Seconds < 0 || timestamp.Nanoseconds < 0) {
        WARNING_ONCE(ddk_time_negative,
            "DDK: clock %u returned a negative time, the last value read is used until it is positive",
            source);
        return __LoadLastRead(source);
    }

    microseconds = (uint64_t)timestamp.Seconds * USEC_PER_SEC +
                   (uint64_t)timestamp.Nanoseconds / NSEC_PER_USEC;
    __StoreLastRead(source, microseconds);
    return microseconds;
}

uint64_t
DDKGetTimeMs(
    _In_ enum OSTimeSource source)
{
    return DDKGetTimeUs(source) / USEC_PER_MSEC;
}

oserr_t
DDKStallUs(
    _In_ uint32_t microseconds)
{
    UInteger64_t duration;

    // OSStall refuses a zero duration, but a zero delay has nothing to wait for.
    if (microseconds == 0) {
        return OS_EOK;
    }

    // OSStall takes nanoseconds. The cast keeps large delays from wrapping
    // around in 32 bits.
    duration.QuadPart = (uint64_t)microseconds * NSEC_PER_USEC;
    return OSStall(&duration);
}
