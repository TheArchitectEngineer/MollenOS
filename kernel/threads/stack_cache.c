/**
 * Copyright 2026, Philip Meulengracht
 *
 * This program is free software : you can redistribute it and / or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation ? , either version 3 of the License, or
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
 *
 * Kernel stack cache. The calling rules are in stack_cache.h.
 *
 * The cache is a list of stacks, last in first out. A cached stack is not
 * used by anyone, so the list entry is stored in the top bytes of the stack
 * itself. Giving and taking a stack never allocates memory.
 */

#define __MODULE "stack_cache"
//#define __TRACE

#include <debug.h>
#include <spinlock.h>
#include <stack_cache.h>
#include <threading.h>

/**
 * List entry stored at the top of a cached stack, just below the address
 * returned as the stack top.
 */
struct __CachedStack {
    struct __CachedStack* Next;
};

static Spinlock_t            g_stackCacheLock = OS_SPINLOCK_INIT;
static struct __CachedStack* g_cachedStacks   = NULL;
static unsigned int          g_cachedCount    = 0;

static struct __CachedStack*
__EntryFromTop(
    _In_ uintptr_t stackTop)
{
    return (struct __CachedStack*)(stackTop - sizeof(struct __CachedStack));
}

static uintptr_t
__TopFromEntry(
    _In_ struct __CachedStack* entry)
{
    return (uintptr_t)entry + sizeof(struct __CachedStack);
}

oserr_t
StackCacheAllocate(
    _In_  size_t     stackSize,
    _Out_ uintptr_t* stackTopOut)
{
    struct __CachedStack* entry;

    if (stackTopOut == NULL) {
        return OS_EINVALPARAMS;
    }

    // Only one stack size is cached, so any other size is never found here.
    if (stackSize != THREADING_KERNEL_STACK_SIZE) {
        return OS_ENOENT;
    }

    // The IRQ variant is used because ThreadFork may run with interrupts
    // disabled, and the lock must never be held when an interrupt arrives.
    SpinlockAcquireIrq(&g_stackCacheLock);
    entry = g_cachedStacks;
    if (entry != NULL) {
        g_cachedStacks = entry->Next;
        g_cachedCount--;
    }
    SpinlockReleaseIrq(&g_stackCacheLock);

    if (entry == NULL) {
        TRACE("StackCacheAllocate cache is empty");
        return OS_ENOENT;
    }

    *stackTopOut = __TopFromEntry(entry);
    TRACE("StackCacheAllocate top=0x%" PRIxIN, *stackTopOut);
    return OS_EOK;
}

oserr_t
StackCacheRelease(
    _In_ uintptr_t stackTop,
    _In_ size_t    stackSize)
{
    struct __CachedStack* entry;
    oserr_t               status = OS_EOK;

    if (stackTop == 0) {
        return OS_EINVALPARAMS;
    }

    if (stackSize != THREADING_KERNEL_STACK_SIZE) {
        return OS_ENOTSUPPORTED;
    }

    // Check the limit and link the stack under the same lock, so concurrent
    // callers can never push the count above KERNEL_STACK_CACHE_LIMIT.
    entry = __EntryFromTop(stackTop);
    SpinlockAcquireIrq(&g_stackCacheLock);
    if (g_cachedCount < KERNEL_STACK_CACHE_LIMIT) {
        entry->Next    = g_cachedStacks;
        g_cachedStacks = entry;
        g_cachedCount++;
    } else {
        status = OS_EBUSY;
    }
    SpinlockReleaseIrq(&g_stackCacheLock);

    TRACE("StackCacheRelease top=0x%" PRIxIN " status=%u", stackTop, status);
    return status;
}
