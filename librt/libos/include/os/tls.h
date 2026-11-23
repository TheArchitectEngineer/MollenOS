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
 * Access to the reserved thread-local slots. Each thread has a small table of
 * values that the C library and the threading code use, for example for the
 * thread control block. On x86 the table is reached through the GS segment,
 * and on AArch64 through the TPIDR register.
 */

#ifndef __OS_TLS_H__
#define __OS_TLS_H__

#include <os/osdefs.h>

#ifndef TESTING
#if defined(i386) || defined(__i386__)
#define TLS_VALUE uint32_t
#define TLS_READ(offset, value)  __asm { __asm mov ebx, [offset] __asm mov eax, gs:[ebx] __asm mov [value], eax }
#define TLS_WRITE(offset, value) __asm { __asm mov ebx, [offset] __asm mov eax, [value] __asm mov gs:[ebx], eax }
#elif defined(amd64) || defined(__amd64__)
#define TLS_VALUE uint64_t
#define TLS_READ(offset, value)  __asm { __asm mov rbx, [offset] __asm mov rax, gs:[rbx] __asm mov [value], rax }
#define TLS_WRITE(offset, value) __asm { __asm mov rbx, [offset] __asm mov rax, [value] __asm mov gs:[rbx], rax }
#elif defined(__aarch64__)
#define TLS_VALUE uint64_t
// The slot table of user code is reached through TPIDR_EL0. The slots belong
// to the execution unit, and slot 0 selects the thread or job that is running.
// The kernel keeps its own table in TPIDR_EL1. It must never read TPIDR_EL0
// for this, because user code controls that register.
#if defined(LIBC_KERNEL)
#define __VALI_TLS_REGISTER "tpidr_el1"
#else
#define __VALI_TLS_REGISTER "tpidr_el0"
#endif
#define TLS_READ(offset, value) do {                                  \
    uintptr_t __vali_slots;                                          \
    __asm__ volatile("mrs %0, " __VALI_TLS_REGISTER                    \
                     : "=r"(__vali_slots) : : "memory");              \
    (value) = *(volatile TLS_VALUE*)(__vali_slots + (offset));         \
} while (0)
#define TLS_WRITE(offset, value) do {                                 \
    uintptr_t __vali_slots;                                          \
    __asm__ volatile("mrs %0, " __VALI_TLS_REGISTER                    \
                     : "=r"(__vali_slots) : : "memory");              \
    *(volatile TLS_VALUE*)(__vali_slots + (offset)) = (value);         \
} while (0)
#else
#error "Implement rw for tls for this architecture"
#endif //defined(i386) || defined(__i386__)
#else  //TESTING
#define TLS_VALUE int
#define TLS_READ(offset, value) (value = offset)
#define TLS_WRITE(offset, value) (value = offset)
#endif //!TESTING

/**
 * @brief Read from the TLS register index. On the X86 architecture this is done by using
 * the GS register.
 */
SERVICEAPI size_t SERVICEABI
__get_reserved(
        _In_ size_t tlsIndex)
{
    TLS_VALUE tlsValue  = 0;
    size_t    tlsOffset = (tlsIndex * sizeof(TLS_VALUE));
    TLS_READ(tlsOffset, tlsValue);
    return (size_t)tlsValue;
}

/**
 * @brief Write to the TLS register index. On the X86 architecture this is done by using
 * the GS register.
 */
SERVICEAPI void SERVICEABI
__set_reserved(
        _In_ size_t    tlsIndex,
        _In_ TLS_VALUE tlsValue)
{
    size_t tlsOffset = (tlsIndex * sizeof(TLS_VALUE));
    TLS_WRITE(tlsOffset, tlsValue);
}

#endif //!__OS_TLS_H__
