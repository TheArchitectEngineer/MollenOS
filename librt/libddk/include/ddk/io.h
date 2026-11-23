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
#include <os/barrier.h>
#include <os/types/deviceio.h>
#include <os/volatile.h>
#include <string.h>

_CODE_BEGIN

/**
 * @brief Registers a range of device memory (MMIO) with the kernel as an I/O
 * space. The range is not mapped yet. Call DeviceIoAcquire to map it before
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
DeviceIoCreateMemory(
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
DeviceIoCreatePort(
    _In_ DeviceIo_t* ioSpace,
    _In_ uint16_t    port,
    _In_ size_t      length));

/**
 * @brief Registers a single pin of a port with the kernel as an I/O space.
 * DeviceIoRead and DeviceIoWrite do not support pin based I/O spaces.
 *
 * @param ioSpace Receives the description and the identifier of the I/O space.
 * @param port    Number of the port the pin belongs to.
 * @param pin     Number of the pin on that port.
 * @return OS_EOK on success, OS_EINVALPARAMS for a missing ioSpace, or an error
 *         from the kernel. The kernel refuses a pin that is already registered.
 */
DDKDECL(oserr_t,
DeviceIoCreatePin(
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
DeviceIoDestroy(
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
DeviceIoAcquire(
    _InOut_ DeviceIo_t* ioSpace));

/**
 * @brief Gives up an I/O space taken with DeviceIoAcquire, so another driver
 * can acquire it. For device memory the mapping is removed and
 * Access.Memory.VirtualBase is set to zero.
 *
 * @param ioSpace The acquired I/O space.
 * @return OS_EOK on success, OS_EINVALPARAMS for a missing ioSpace, or an error
 *         from the kernel.
 */
DDKDECL(oserr_t,
DeviceIoRelease(
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
DeviceIoMap(
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
DeviceIoRead(
    _In_ DeviceIo_t* ioSpace,
    _In_ size_t      offset,
    _In_ size_t      length));

/**
 * @brief Writes one register in an acquired I/O space. The supported widths
 * are the same as for DeviceIoRead.
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
DeviceIoWrite(
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
DeviceIoReadLowHigh64(
    _In_ DeviceIo_t* IoSpace,
    _In_ size_t     Offset)
{
    uint32_t low;
    uint32_t high;

    low = (uint32_t)DeviceIoRead(IoSpace, Offset, sizeof(low));
    rmb();
    high = (uint32_t)DeviceIoRead(IoSpace, Offset + sizeof(low), sizeof(high));
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
DeviceIoWriteLowHigh64(
    _In_ DeviceIo_t* IoSpace,
    _In_ size_t     Offset,
    _In_ uint64_t   Value)
{
    oserr_t oserr;

    oserr = DeviceIoWrite(IoSpace, Offset, (uint32_t)Value, sizeof(uint32_t));
    if (oserr != OS_EOK) {
        return oserr;
    }

    wmb();
    return DeviceIoWrite(
        IoSpace,
        Offset + sizeof(uint32_t),
        (uint32_t)(Value >> 32),
        sizeof(uint32_t));
}

_CODE_END

#endif //!__DDK_IO_H__
