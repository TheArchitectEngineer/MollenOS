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
 * The description of a device I/O space. Drivers register and use I/O spaces
 * through ddk/io.h, and the kernel keeps the same structure for each one.
 */

#ifndef __OS_TYPES_DEVICEIO_H__
#define __OS_TYPES_DEVICEIO_H__

#include <os/osdefs.h>

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

#endif //!__OS_TYPES_DEVICEIO_H__
