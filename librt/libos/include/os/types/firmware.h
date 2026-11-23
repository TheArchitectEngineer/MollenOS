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
 * Types of the firmware table calls. They describe which firmware
 * descriptions (ACPI, DeviceTree) the system has and identify one table.
 */

#ifndef __OS_TYPES_FIRMWARE_H__
#define __OS_TYPES_FIRMWARE_H__

#include <os/osdefs.h>

enum OSFirmwareSource {
    OSFIRMWARE_NONE       = 0x0,
    OSFIRMWARE_ACPI       = 0x1,
    OSFIRMWARE_DEVICETREE = 0x2
};

typedef struct OSFirmwareInfo {
    // Bitmask of enum OSFirmwareSource values present on this system
    unsigned int Available;
    // The source the kernel used to describe the platform
    unsigned int Primary;
    struct {
        // (FADT major << 4) | FADT minor, matches ACPI_VERSION_*
        uint8_t  Revision;
        uint8_t  Century;
        uint16_t IaBootFlags;
        uint16_t ArmBootFlags;
    } Acpi;
    struct {
        uint32_t Size;
    } DeviceTree;
} OSFirmwareInfo_t;

typedef struct OSFirmwareTableKey {
    unsigned int Source;
    // ACPI table signature, ignored for the DeviceTree
    char         Signature[4];
    // Zero-based instance for tables that may repeat (SSDT), must be 0 for the DeviceTree
    unsigned int Instance;
} OSFirmwareTableKey_t;

typedef struct OSFirmwareTable {
    size_t   Length;
    // ACPI header revision, or the FDT format version
    uint32_t Revision;
} OSFirmwareTable_t;

#endif //!__OS_TYPES_FIRMWARE_H__
