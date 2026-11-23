/**
 * MollenOS
 *
 * Copyright 2017, Philip Meulengracht
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
 * I/O Definitions & Structures
 * - This header describes the base io-structure, prototypes
 *   and functionality, refer to the individual things for descriptions
 */

#ifndef __DDK_IO_H__
#define __DDK_IO_H__

#include <ddk/ddkdefs.h>
#include <ddk/barrier.h>
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

typedef enum DeviceIoType {
    DeviceIoInvalid = 0,
    DeviceIoMemoryBased,    // Usually device memory range
    DeviceIoPortBased,      // Usually a port range
    DeviceIoPinBased        // Usually a port/pin combination
} DeviceIoType_t;

// Represents a device io communcation space
// that can be used by a driver to communcate with its physical device.
typedef struct DeviceIo {
    uuid_t              Id;
    DeviceIoType_t      Type;
    union {
        struct {
            uintptr_t   PhysicalBase;
            uintptr_t   VirtualBase;
            size_t      Length;
        } Memory;
        struct {
            uint16_t    Base;
            size_t      Length;
        } Port;
        struct {
            uint16_t    Port;
            uint8_t     Pin;
        } Pin;
    } Access;
} DeviceIo_t;

_CODE_BEGIN

void ReadVolatileMemory(
    _In_ const volatile void* Pointer,
    _In_ volatile void*       Out,
    _In_ size_t               Length);

void WriteVolatileMemory(
    _In_ volatile void* Pointer,
    _In_ void*          Data,
    _In_ size_t         Length);

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

/**
 * @brief Registers a range of device memory (MMIO) with the kernel as an I/O
 * space. The range is not mapped yet. Call AcquireDeviceIo to map it before
 * reading or writing registers.
 *
 * @param ioSpace      Receives the description and the identifier of the I/O space.
 * @param physicalBase Physical address of the first register.
 * @param length       Size of the range in bytes.
 * @return OS_EOK on success, OS_EINVALPARAMS for a missing ioSpace, or an error
 *         from the kernel. The kernel refuses a range that overlaps an I/O space
 *         that is already registered.
 */
DDKDECL(oserr_t,
CreateDeviceMemoryIo(
    _In_ DeviceIo_t* ioSpace,
    _In_ uintptr_t   physicalBase,
    _In_ size_t      length));

/**
 * @brief Registers a range of x86 I/O ports with the kernel as an I/O space.
 *
 * @param ioSpace Receives the description and the identifier of the I/O space.
 * @param port    Number of the first port.
 * @param length  Number of ports in the range.
 * @return OS_EOK on success, OS_EINVALPARAMS for a missing ioSpace, or an error
 *         from the kernel. The kernel refuses a range that overlaps an I/O space
 *         that is already registered.
 */
DDKDECL(oserr_t,
CreateDevicePortIo(
    _In_ DeviceIo_t* ioSpace,
    _In_ uint16_t    port,
    _In_ size_t      length));

/**
 * @brief Registers a single pin of a port with the kernel as an I/O space.
 * ReadDeviceIo and WriteDeviceIo do not support pin based I/O spaces.
 *
 * @param ioSpace Receives the description and the identifier of the I/O space.
 * @param port    Number of the port the pin belongs to.
 * @param pin     Number of the pin on that port.
 * @return OS_EOK on success, OS_EINVALPARAMS for a missing ioSpace, or an error
 *         from the kernel. The kernel refuses a pin that is already registered.
 */
DDKDECL(oserr_t,
CreateDevicePinIo(
    _In_ DeviceIo_t* ioSpace,
    _In_ uint16_t    port,
    _In_ uint8_t     pin));

/**
 * @brief Removes an I/O space that was registered with one of the create calls.
 * The range can not be used through this I/O space afterwards.
 *
 * @param ioSpace The registered I/O space.
 * @return OS_EOK on success, OS_EINVALPARAMS for a missing ioSpace, or an error
 *         from the kernel.
 */
DDKDECL(oserr_t,
DestroyDeviceIo(
    _In_ DeviceIo_t* ioSpace));

/**
 * @brief Takes a registered I/O space into use. Only one driver can have an
 * I/O space acquired at a time, which keeps two drivers from using the same
 * device. For device memory this maps the range and stores the address in
 * Access.Memory.VirtualBase.
 *
 * @param ioSpace The registered I/O space. Updated with the mapping on success.
 * @return OS_EOK on success, OS_EINVALPARAMS for a missing ioSpace, or an error
 *         from the kernel.
 */
DDKDECL(oserr_t,
AcquireDeviceIo(
    _InOut_ DeviceIo_t* ioSpace));

/**
 * @brief Gives up an I/O space taken with AcquireDeviceIo, so another driver
 * can acquire it. For device memory the mapping is removed and
 * Access.Memory.VirtualBase is set to zero.
 *
 * @param ioSpace The acquired I/O space.
 * @return OS_EOK on success, OS_EINVALPARAMS for a missing ioSpace, or an error
 *         from the kernel.
 */
DDKDECL(oserr_t,
ReleaseDeviceIo(
    _InOut_ DeviceIo_t* ioSpace));

/**
 * @brief Map a registered MMIO subrange without acquiring its exclusive claim.
 *
 * Bus providers use this for structures such as MSI-X tables in a BAR also used
 * by a driver. The caller must keep the registration alive until MemoryFree
 * releases this uncached device mapping. This does not check device access.
 * To release it, round the returned address down to MemoryPageSize() and pass
 * that address and length plus the removed page offset to MemoryFree().
 *
 * @param id Registered memory I/O identifier.
 * @param offset Byte offset within the registered region.
 * @param length Nonzero byte length of the mapping.
 * @param mappingOut Receives the address corresponding to offset.
 * @return OS_EOK on success, or an error for an invalid range or failed mapping.
 */
DDKDECL(oserr_t,
MapDeviceIo(
    _In_  uuid_t id,
    _In_  size_t offset,
    _In_  size_t length,
    _Out_ void** mappingOut));

/**
 * @brief Reads one register from an acquired I/O space.
 *
 * Device memory supports widths of 1, 2 and 4 bytes, and 8 bytes on 64-bit
 * systems. Port ranges support 1, 2 and 4 bytes and only exist on x86.
 *
 * @param ioSpace The acquired I/O space.
 * @param offset  Byte offset of the register inside the I/O space.
 * @param length  Width of the register in bytes.
 * @return The register value. A read that can not be done is logged and
 *         returns zero.
 */
DDKDECL(size_t,
ReadDeviceIo(
    _In_ DeviceIo_t* ioSpace,
    _In_ size_t      offset,
    _In_ size_t      length));

/**
 * @brief Writes one register in an acquired I/O space. The supported widths
 * are the same as for ReadDeviceIo.
 *
 * @param ioSpace The acquired I/O space.
 * @param offset  Byte offset of the register inside the I/O space.
 * @param value   Value to write. Bits above the register width are dropped.
 * @param length  Width of the register in bytes.
 * @return OS_EOK on success, OS_EINVALPARAMS for a missing ioSpace or a width
 *         that is not supported, or OS_ENOTSUPPORTED for an I/O space type that
 *         can not be written.
 */
DDKDECL(oserr_t,
WriteDeviceIo(
    _In_ DeviceIo_t* ioSpace,
    _In_ size_t      offset,
    _In_ size_t      value,
    _In_ size_t      length));

/**
 * @brief Read a 64-bit device I/O register as two 32-bit accesses, low word first.
 *
 * @param IoSpace Device I/O space containing the register.
 * @param Offset Byte offset of the low word.
 * @return The combined register value.
 */
static inline uint64_t
__ReadDeviceIoLowHigh64(
    _In_ DeviceIo_t* IoSpace,
    _In_ size_t     Offset)
{
    uint32_t low;
    uint32_t high;

    low = (uint32_t)ReadDeviceIo(IoSpace, Offset, sizeof(low));
    rmb();
    high = (uint32_t)ReadDeviceIo(IoSpace, Offset + sizeof(low), sizeof(high));
    return ((uint64_t)high << 32) | low;
}

/**
 * @brief Write a 64-bit device I/O register as two 32-bit accesses, low word first.
 *
 * @param IoSpace Device I/O space containing the register.
 * @param Offset Byte offset of the low word.
 * @param Value Full register value.
 * @return OS_EOK on success, or the error from either 32-bit write.
 */
static inline oserr_t
__WriteDeviceIoLowHigh64(
    _In_ DeviceIo_t* IoSpace,
    _In_ size_t     Offset,
    _In_ uint64_t   Value)
{
    oserr_t oserr;

    oserr = WriteDeviceIo(IoSpace, Offset, (uint32_t)Value, sizeof(uint32_t));
    if (oserr != OS_EOK) {
        return oserr;
    }

    wmb();
    return WriteDeviceIo(
        IoSpace,
        Offset + sizeof(uint32_t),
        (uint32_t)(Value >> 32),
        sizeof(uint32_t));
}

_CODE_END

#endif //!__DDK_IO_H__
