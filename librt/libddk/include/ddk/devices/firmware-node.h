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
 * The firmware description of a driver's own device node.
 *
 * deviced hands a driver the device tree node of its device as one packed
 * description: every property of the node (name and raw value bytes), the
 * cell counts its parent uses for "reg", and the name and state of each
 * direct child. The driver decodes the bindings it knows itself, so deviced
 * needs no decoder per device class. Nothing here touches hardware, so the
 * description can be read before DevicePrepare.
 *
 * Transport: one OSIOCTLREQUEST_FIRMWARE_NODE request carries at most
 * DEVICE_FIRMWARE_NODE_CHUNK_SIZE bytes, because a device request must fit in
 * one IPC message (2048 bytes by default) and deviced copies the request
 * body onto its stack. DeviceFirmwareNodeRead asks for the description in
 * chunks until it has all of it. The tree never changes while deviced runs,
 * so every chunk comes from the same description.
 *
 * Layout of a description. Header fields and record fields are 32-bit values
 * in the byte order of the machine. Property values are the raw firmware
 * bytes, so their cells are big-endian. Every record starts at a multiple of
 * four bytes.
 *
 *   struct DeviceFirmwareNodeHeader
 *   PropertyCount property records:
 *     uint32_t NameLength   name bytes including the terminating zero
 *     uint32_t ValueLength  value bytes
 *     name, zero padded to a multiple of four bytes
 *     value, zero padded to a multiple of four bytes
 *   ChildCount child records:
 *     uint32_t NameLength   name bytes including the terminating zero
 *     uint32_t Flags        DEVICE_FIRMWARE_CHILD_* bits
 *     name, zero padded to a multiple of four bytes
 */

#ifndef __DDK_FIRMWARE_NODE_H__
#define __DDK_FIRMWARE_NODE_H__

#include <ddk/ddkdefs.h>

_CODE_BEGIN

// Layout version of the description described here.
#define DEVICE_FIRMWARE_NODE_VERSION    1U

// Description bytes one request carries.
#define DEVICE_FIRMWARE_NODE_CHUNK_SIZE 1024U

// Largest description deviced hands out and a driver accepts. Real device
// nodes are a few hundred bytes. The limit bounds the memory a broken tree or
// a broken answer can make a driver allocate.
#define DEVICE_FIRMWARE_NODE_MAX_LENGTH 32768U

// Child record flags.
#define DEVICE_FIRMWARE_CHILD_ENABLED 0x1U // the child's "status" allows use
#define DEVICE_FIRMWARE_CHILD_HAS_REG 0x2U // the child has a "reg" property

/**
 * @brief Wire format of OSIOCTLREQUEST_FIRMWARE_NODE.
 *
 * The driver sets Offset to the first description byte it wants and Reserved
 * to zero. deviced writes the length of the whole description to Length, the
 * number of bytes it copied to Count, and the bytes to Data. Count is
 * DEVICE_FIRMWARE_NODE_CHUNK_SIZE, or less for the last chunk.
 */
struct DeviceFirmwareNodeRequest {
    uint32_t Offset;
    uint32_t Reserved;
    uint32_t Length;
    uint32_t Count;
    uint8_t  Data[DEVICE_FIRMWARE_NODE_CHUNK_SIZE];
};

/**
 * @brief First bytes of a description.
 * ParentAddressCells and ParentSizeCells are the "#address-cells" and
 * "#size-cells" of the node's parent, which give the size of one "reg" entry.
 */
struct DeviceFirmwareNodeHeader {
    uint32_t Version;
    uint32_t Length;
    uint32_t ParentAddressCells;
    uint32_t ParentSizeCells;
    uint32_t PropertyCount;
    uint32_t ChildCount;
};

/**
 * @brief A checked description, ready for the accessors below.
 *
 * Data holds Length bytes that DeviceFirmwareNodeParse has checked, so the
 * accessors never read outside it. ChildOffset is where the first child
 * record starts. A node from DeviceFirmwareNodeRead owns Data and is
 * released with DeviceFirmwareNodeDestroy. A node from
 * DeviceFirmwareNodeParse points into the caller's buffer.
 */
typedef struct DeviceFirmwareNode {
    const uint8_t* Data;
    uint32_t       Length;
    uint32_t       ParentAddressCells;
    uint32_t       ParentSizeCells;
    uint32_t       PropertyCount;
    uint32_t       ChildCount;
    uint32_t       ChildOffset;
} DeviceFirmwareNode_t;

/**
 * @brief One direct child of the node. Name points into the description.
 */
typedef struct DeviceFirmwareNodeChild {
    const char* Name;
    uint32_t    Flags;
} DeviceFirmwareNodeChild_t;

/**
 * @brief Read the firmware description of a device's own node from deviced.
 *
 * Sends OSIOCTLREQUEST_FIRMWARE_NODE until the whole description has arrived,
 * then checks it with DeviceFirmwareNodeParse. Works before DevicePrepare.
 *
 * @param device Platform device assigned to this driver.
 * @param node Receives the checked description. Unchanged on failure.
 *             Release it with DeviceFirmwareNodeDestroy.
 * @return OS_EOK on success.
 *         OS_EINVALPARAMS for a missing output pointer, or a node deviced
 *         finds malformed.
 *         OS_ENOENT when the node is disabled.
 *         OS_ENOTSUPPORTED when the device has no firmware node, or the
 *         description is larger than DEVICE_FIRMWARE_NODE_MAX_LENGTH.
 *         OS_EPERMISSIONS when the caller is not the device's driver.
 *         OS_EOOM when the description cannot be allocated.
 *         OS_EPROTOCOL when an answer is malformed, changes between chunks or
 *         could not be transported.
 */
DDKDECL(oserr_t,
DeviceFirmwareNodeRead(
    _In_  uuid_t                device,
    _Out_ DeviceFirmwareNode_t* node));

/**
 * @brief Release a description from DeviceFirmwareNodeRead.
 *
 * @param node Description to release. Its fields are cleared. NULL and a
 *             cleared node are ignored.
 */
DDKDECL(void,
DeviceFirmwareNodeDestroy(
    _In_ DeviceFirmwareNode_t* node));

/**
 * @brief Check a description and fill in a node that points into it.
 *
 * Checks the version, the length, every record against the end of the
 * buffer, that every name is non-empty and ends at its terminating zero, and
 * that no property name appears twice.
 *
 * @param data Description bytes. Must stay valid while node is used.
 * @param length Number of bytes in data.
 * @param node Receives the checked description. Unchanged on failure.
 * @return OS_EOK, OS_EINVALPARAMS for a missing pointer, or OS_EPROTOCOL for
 *         a malformed description.
 */
DDKDECL(oserr_t,
DeviceFirmwareNodeParse(
    _In_  const void*           data,
    _In_  size_t                length,
    _Out_ DeviceFirmwareNode_t* node));

/**
 * @brief Find a property by name.
 *
 * @param node Checked description.
 * @param name Property name, such as "bus-width".
 * @param length Receives the value length in bytes when found. May be NULL.
 * @return The raw value bytes, or NULL when the node has no such property.
 *         A property without a value returns a non-NULL pointer and length 0.
 */
DDKDECL(const uint8_t*,
DeviceFirmwareNodeProperty(
    _In_      const DeviceFirmwareNode_t* node,
    _In_      const char*                 name,
    _Out_Opt_ uint32_t*                   length));

/**
 * @brief Read a boolean property, which is present or absent and has no value.
 *
 * @param node Checked description.
 * @param name Property name, such as "non-removable".
 * @param present Receives 1 when the property is present, 0 when absent.
 *                Unchanged on failure.
 * @return OS_EOK, or OS_EINVALPARAMS when the property carries a value.
 */
DDKDECL(oserr_t,
DeviceFirmwareNodeFlag(
    _In_  const DeviceFirmwareNode_t* node,
    _In_  const char*                 name,
    _Out_ uint32_t*                   present));

/**
 * @brief Read a property that holds exactly one 32-bit cell.
 *
 * @param node Checked description.
 * @param name Property name, such as "max-frequency".
 * @param value Receives the cell in host byte order. Unchanged on failure.
 * @return OS_EOK, OS_ENOENT when the property is absent, or OS_EINVALPARAMS
 *         when its length is not four bytes.
 */
DDKDECL(oserr_t,
DeviceFirmwareNodeU32(
    _In_  const DeviceFirmwareNode_t* node,
    _In_  const char*                 name,
    _Out_ uint32_t*                   value));

/**
 * @brief Find the position of one entry in a string list property, such as
 * "host" in "reg-names".
 *
 * The whole list is read, so an empty entry, a missing terminator or a
 * repeated match is found even after the requested entry.
 *
 * @param node Checked description.
 * @param name Property name.
 * @param entry String to look for.
 * @param index Receives the zero-based position.
 * @param count Receives the number of entries in the list.
 * @return OS_EOK when the entry occurs once. OS_ENOENT when the property is
 *         absent (outputs unchanged), or when the list is valid but does not
 *         hold the entry (index is UINT32_MAX and count is set).
 *         OS_EINVALPARAMS for a malformed list or a repeated entry. Outputs
 *         are unchanged then.
 */
DDKDECL(oserr_t,
DeviceFirmwareNodeStringIndex(
    _In_  const DeviceFirmwareNode_t* node,
    _In_  const char*                 name,
    _In_  const char*                 entry,
    _Out_ uint32_t*                   index,
    _Out_ uint32_t*                   count));

/**
 * @brief Read one direct child of the node, in firmware order.
 *
 * @param node Checked description.
 * @param index Zero-based child position.
 * @param child Receives the child. Unchanged on failure.
 * @return OS_EOK, or OS_ENOENT when index is ChildCount or more.
 */
DDKDECL(oserr_t,
DeviceFirmwareNodeChild(
    _In_  const DeviceFirmwareNode_t* node,
    _In_  uint32_t                    index,
    _Out_ DeviceFirmwareNodeChild_t*  child));

_CODE_END

#endif //!__DDK_FIRMWARE_NODE_H__
