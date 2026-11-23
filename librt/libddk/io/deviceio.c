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
 * Device I/O spaces. A driver registers the memory range, port range or pin of
 * its device with the kernel, acquires it, and then reads and writes the
 * registers through DeviceIoRead and DeviceIoWrite.
 */

#include <internal/_syscalls.h>
#include <ddk/io.h>
#include <ddk/utils.h>
#include <assert.h>

#if defined(__i386__) || defined(__amd64__)
// Port I/O instructions, implemented in arch/<arch>/_io.s
ASMDECL(uint8_t,  __readbyte(uint16_t Port));
ASMDECL(void,     __writebyte(uint16_t Port, uint8_t Value));
ASMDECL(uint16_t, __readword(uint16_t Port));
ASMDECL(void,     __writeword(uint16_t Port, uint16_t Value));
ASMDECL(uint32_t, __readlong(uint16_t Port));
ASMDECL(void,     __writelong(uint16_t Port, uint32_t Value));
#endif

oserr_t
DeviceIoCreateMemory(
    _In_ DeviceIo_t* ioSpace,
    _In_ uintptr_t   physicalBase,
    _In_ size_t      length)
{
    assert(ioSpace != NULL);
    if (ioSpace == NULL) {
        return OS_EINVALPARAMS;
    }

    // The virtual base stays zero until DeviceIoAcquire has mapped the range.
    ioSpace->Type                       = DeviceIoMemoryBased;
    ioSpace->Access.Memory.PhysicalBase = physicalBase;
    ioSpace->Access.Memory.VirtualBase  = 0;
    ioSpace->Access.Memory.Length       = length;
    return Syscall_IoSpaceRegister(ioSpace);
}

oserr_t
DeviceIoCreatePort(
    _In_ DeviceIo_t* ioSpace,
    _In_ uint16_t    port,
    _In_ size_t      length)
{
    assert(ioSpace != NULL);
    if (ioSpace == NULL) {
        return OS_EINVALPARAMS;
    }

    ioSpace->Type               = DeviceIoPortBased;
    ioSpace->Access.Port.Base   = port;
    ioSpace->Access.Port.Length = length;
    return Syscall_IoSpaceRegister(ioSpace);
}

oserr_t
DeviceIoCreatePin(
    _In_ DeviceIo_t* ioSpace,
    _In_ uint16_t    port,
    _In_ uint8_t     pin)
{
    assert(ioSpace != NULL);
    if (ioSpace == NULL) {
        return OS_EINVALPARAMS;
    }

    ioSpace->Type            = DeviceIoPinBased;
    ioSpace->Access.Pin.Port = port;
    ioSpace->Access.Pin.Pin  = pin;
    return Syscall_IoSpaceRegister(ioSpace);
}

oserr_t
DeviceIoDestroy(
    _In_ DeviceIo_t* ioSpace)
{
    assert(ioSpace != NULL);
    if (ioSpace == NULL) {
        return OS_EINVALPARAMS;
    }
    return Syscall_IoSpaceDestroy(ioSpace);
}

oserr_t
DeviceIoAcquire(
    _InOut_ DeviceIo_t* ioSpace)
{
    assert(ioSpace != NULL);
    if (ioSpace == NULL) {
        return OS_EINVALPARAMS;
    }
    return Syscall_IoSpaceAcquire(ioSpace);
}

oserr_t
DeviceIoMap(
    _In_  uuid_t id,
    _In_  size_t offset,
    _In_  size_t length,
    _Out_ void** mappingOut)
{
    return Syscall_IoSpaceMap(id, offset, length, mappingOut);
}

oserr_t
DeviceIoRelease(
    _InOut_ DeviceIo_t* ioSpace)
{
    assert(ioSpace != NULL);
    if (ioSpace == NULL) {
        return OS_EINVALPARAMS;
    }
    return Syscall_IoSpaceRelease(ioSpace);
}

// Reads one register of 1, 2, 4 or (on 64-bit systems) 8 bytes from mapped
// device memory. The pointers are volatile so the compiler performs exactly
// one access of the requested width.
static oserr_t
__ReadMemory(
    _In_  uintptr_t address,
    _In_  size_t    width,
    _Out_ size_t*   valueOut)
{
    switch (width) {
        case 1:
            *valueOut = *(volatile uint8_t*)address;
            return OS_EOK;
        case 2:
            *valueOut = *(volatile uint16_t*)address;
            return OS_EOK;
        case 4:
            *valueOut = *(volatile uint32_t*)address;
            return OS_EOK;
#if defined(__amd64__) || defined(__aarch64__)
        case 8:
            *valueOut = *(volatile uint64_t*)address;
            return OS_EOK;
#endif
        default:
            return OS_EINVALPARAMS;
    }
}

// Writes one register of 1, 2, 4 or (on 64-bit systems) 8 bytes to mapped
// device memory. Bits of value above the width are dropped.
static oserr_t
__WriteMemory(
    _In_ uintptr_t address,
    _In_ size_t    width,
    _In_ size_t    value)
{
    switch (width) {
        case 1:
            *(volatile uint8_t*)address = (uint8_t)(value & 0xFF);
            return OS_EOK;
        case 2:
            *(volatile uint16_t*)address = (uint16_t)(value & 0xFFFF);
            return OS_EOK;
        case 4:
            *(volatile uint32_t*)address = (uint32_t)(value & 0xFFFFFFFF);
            return OS_EOK;
#if defined(__amd64__) || defined(__aarch64__)
        case 8:
            *(volatile uint64_t*)address = (uint64_t)value;
            return OS_EOK;
#endif
        default:
            return OS_EINVALPARAMS;
    }
}

#if defined(__i386__) || defined(__amd64__)
// Reads 1, 2 or 4 bytes from an x86 I/O port.
static oserr_t
__ReadPort(
    _In_  uint16_t port,
    _In_  size_t   width,
    _Out_ size_t*  valueOut)
{
    switch (width) {
        case 1:
            *valueOut = __readbyte(port);
            return OS_EOK;
        case 2:
            *valueOut = __readword(port);
            return OS_EOK;
        case 4:
            *valueOut = __readlong(port);
            return OS_EOK;
        default:
            return OS_EINVALPARAMS;
    }
}

// Writes 1, 2 or 4 bytes to an x86 I/O port. Bits of value above the width
// are dropped.
static oserr_t
__WritePort(
    _In_ uint16_t port,
    _In_ size_t   width,
    _In_ size_t   value)
{
    switch (width) {
        case 1:
            __writebyte(port, (uint8_t)(value & 0xFF));
            return OS_EOK;
        case 2:
            __writeword(port, (uint16_t)(value & 0xFFFF));
            return OS_EOK;
        case 4:
            __writelong(port, (uint32_t)(value & 0xFFFFFFFF));
            return OS_EOK;
        default:
            return OS_EINVALPARAMS;
    }
}
#endif

size_t
DeviceIoRead(
    _In_ DeviceIo_t* ioSpace,
    _In_ size_t      offset,
    _In_ size_t      length)
{
    size_t  value = 0;
    oserr_t status;

    assert(ioSpace != NULL);
    if (ioSpace == NULL) {
        return 0;
    }

    switch (ioSpace->Type) {
        case DeviceIoMemoryBased:
            assert((offset + length) <= ioSpace->Access.Memory.Length);
            assert(ioSpace->Access.Memory.VirtualBase != 0);
            status = __ReadMemory(ioSpace->Access.Memory.VirtualBase + offset, length, &value);
            break;

#if defined(__i386__) || defined(__amd64__)
        case DeviceIoPortBased:
            status = __ReadPort(LOWORD(ioSpace->Access.Port.Base) + LOWORD(offset), length, &value);
            break;
#endif

        default:
            status = OS_ENOTSUPPORTED;
            break;
    }

    // The value itself is the return value, so a failed read can only be
    // reported in the log. It reads as zero.
    if (status != OS_EOK) {
        ERROR("DeviceIoRead: io-space %u type %u, offset %zu, width %zu cannot be read (%u)",
              ioSpace->Id, ioSpace->Type, offset, length, status);
        return 0;
    }
    return value;
}

oserr_t
DeviceIoWrite(
    _In_ DeviceIo_t* ioSpace,
    _In_ size_t      offset,
    _In_ size_t      value,
    _In_ size_t      length)
{
    oserr_t status;

    assert(ioSpace != NULL);
    if (ioSpace == NULL) {
        return OS_EINVALPARAMS;
    }

    switch (ioSpace->Type) {
        case DeviceIoMemoryBased:
            assert((offset + length) <= ioSpace->Access.Memory.Length);
            assert(ioSpace->Access.Memory.VirtualBase != 0);
            status = __WriteMemory(ioSpace->Access.Memory.VirtualBase + offset, length, value);
            break;

#if defined(__i386__) || defined(__amd64__)
        case DeviceIoPortBased:
            status = __WritePort(LOWORD(ioSpace->Access.Port.Base) + LOWORD(offset), length, value);
            break;
#endif

        default:
            status = OS_ENOTSUPPORTED;
            break;
    }

    if (status != OS_EOK) {
        ERROR("DeviceIoWrite: io-space %u type %u, offset %zu, width %zu cannot be written (%u)",
              ioSpace->Id, ioSpace->Type, offset, length, status);
    }
    return status;
}
