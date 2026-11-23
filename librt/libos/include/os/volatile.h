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
 * Reads and writes of memory that changes outside the control of the compiler,
 * such as device registers. The helpers perform one access of the exact width
 * and keep the compiler from reordering, splitting or merging it.
 */

#ifndef __OS_VOLATILE_H__
#define __OS_VOLATILE_H__

#include <os/osdefs.h>
#include <os/barrier.h>
#include <string.h>

#if defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) && \
        (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
#error Split 64-bit device I/O requires little-endian access
#endif

#define READ_VOLATILE(var) ({                                       \
        union { typeof(var) Value; char Data[1]; } ValueRep;         \
        ReadVolatileMemory(&(var), &ValueRep.Data[0], sizeof(var)); \
        ValueRep.Value;                                             \
    })

#define WRITE_VOLATILE(var, x) ({                                \
        union { typeof(var) Value; char Data[1]; } ValueRep =         \
            { .Value = (x) };                                      \
        WriteVolatileMemory(&(var), &ValueRep.Data[0], sizeof(var)); \
        ValueRep.Value;                                              \
    })

#define READ_VOLATILE_LOW_HIGH(var) ({                               \
        _Static_assert(sizeof(var) == sizeof(uint64_t),              \
                       "64-bit register required");                 \
        __ReadVolatileLowHigh64(&(var));                             \
    })

#define WRITE_VOLATILE_LOW_HIGH(var, x) ({                           \
        _Static_assert(sizeof(var) == sizeof(uint64_t),              \
                       "64-bit register required");                 \
        __WriteVolatileLowHigh64(&(var), (x));                       \
    })

_CODE_BEGIN

/**
 * @brief Reads Length bytes of volatile memory. Lengths of 1, 2, 4 and 8 are
 * done as a single access of that width. Any other length is copied between
 * two compiler barriers.
 *
 * @param Pointer Address to read from.
 * @param Out     Receives the bytes. Must be writable for Length bytes.
 * @param Length  Number of bytes to read.
 */
CRTDECL(void,
ReadVolatileMemory(
    _In_ const volatile void* Pointer,
    _In_ volatile void*       Out,
    _In_ size_t               Length));

/**
 * @brief Writes Length bytes to volatile memory. Lengths of 1, 2, 4 and 8 are
 * done as a single access of that width. Any other length is copied between
 * two compiler barriers.
 *
 * @param Pointer Address to write to.
 * @param Data    The bytes to write. Must be readable for Length bytes.
 * @param Length  Number of bytes to write.
 */
CRTDECL(void,
WriteVolatileMemory(
    _In_ volatile void* Pointer,
    _In_ void*          Data,
    _In_ size_t         Length));

/**
 * @brief Read a 64-bit device register as two 32-bit accesses, low word first.
 *
 * The register must be mapped little-endian device memory aligned to at least
 * four bytes. The reads are not atomic.
 *
 * @param Address Address of the low word.
 * @return The combined register value.
 */
static inline uint64_t
__ReadVolatileLowHigh64(
    _In_ const volatile void* Address)
{
    const volatile uint32_t* words = Address;
    uint32_t                 low;
    uint32_t                 high;

    ReadVolatileMemory(&words[0], &low, sizeof(low));
    rmb();
    ReadVolatileMemory(&words[1], &high, sizeof(high));
    return ((uint64_t)high << 32) | low;
}

/**
 * @brief Write a 64-bit device register as two 32-bit accesses, low word first.
 *
 * The register must be mapped little-endian device memory aligned to at least
 * four bytes. The writes are not atomic.
 *
 * @param Address Address of the low word.
 * @param Value Full register value.
 */
static inline void
__WriteVolatileLowHigh64(
    _In_ volatile void* Address,
    _In_ uint64_t       Value)
{
    volatile uint32_t* words = Address;
    uint32_t           low;
    uint32_t           high;

    low = (uint32_t)Value;
    high = (uint32_t)(Value >> 32);
    WriteVolatileMemory(&words[0], &low, sizeof(low));
    wmb();
    WriteVolatileMemory(&words[1], &high, sizeof(high));
}

_CODE_END

#endif //!__OS_VOLATILE_H__
