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
 * Wrapper for OSIOCTLREQUEST_FIRMWARE_NODE, and the checks and accessors for
 * the description it returns. The layout is described in ddk/firmware_node.h.
 *
 * Logging: every refused answer or description is logged once, with the
 * values that failed the check. An accessor logs only a malformed property.
 * An absent property is a normal answer and is not logged.
 */

#include <ddk/firmware_node.h>
#include <ddk/utils.h>
#include <os/device.h>
#include <stdlib.h>
#include <string.h>

// Size of the two 32-bit fields that start every record.
#define __RECORD_HEADER_SIZE 8U

static uint32_t
__NodeReadU32(
    _In_ const uint8_t* bytes)
{
    uint32_t value;

    // Records are 4-byte aligned inside the description, but the caller's
    // buffer may not be, so the value is copied out.
    memcpy(&value, bytes, sizeof(value));
    return value;
}

static uint32_t
__NodeReadBe32(
    _In_ const uint8_t* bytes)
{
    return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) |
           ((uint32_t)bytes[2] << 8) | (uint32_t)bytes[3];
}

static uint32_t
__RecordPadded(
    _In_ uint32_t length)
{
    return (length + 3U) & ~3U;
}

/**
 * @brief Check one record at offset and return the offset after it.
 * ValueLength is zero for a child record, whose second field is its flags.
 */
static oserr_t
__CheckRecord(
    _In_  const uint8_t* data,
    _In_  uint32_t       length,
    _In_  uint32_t       offset,
    _In_  int            isProperty,
    _Out_ uint32_t*      nextOut)
{
    const uint8_t* name;
    uint32_t       nameLength;
    uint32_t       valueLength = 0;
    uint32_t       left;
    uint32_t       size;

    left = length - offset;
    if (left < __RECORD_HEADER_SIZE) {
        ERROR("__CheckRecord: record at byte %u has %u bytes left, its header needs %u",
              offset, left, __RECORD_HEADER_SIZE);
        return OS_EPROTOCOL;
    }

    nameLength = __NodeReadU32(data + offset);
    if (isProperty) {
        valueLength = __NodeReadU32(data + offset + 4);
    }
    left -= __RECORD_HEADER_SIZE;

    // Compare against what is left before padding, so a huge length cannot
    // wrap around when it is rounded up.
    if (nameLength < 2 || nameLength > left || valueLength > left) {
        ERROR("__CheckRecord: record at byte %u has name length %u and value length %u, %u bytes left",
              offset, nameLength, valueLength, left);
        return OS_EPROTOCOL;
    }

    size = __RecordPadded(nameLength);
    if (size > left || __RecordPadded(valueLength) > left - size) {
        ERROR("__CheckRecord: record at byte %u with padding does not fit in %u bytes", offset, left);
        return OS_EPROTOCOL;
    }
    size += __RecordPadded(valueLength);

    // The name must be non-empty and end exactly at its terminating zero:
    // the first zero byte is the last byte of the name.
    name = data + offset + __RECORD_HEADER_SIZE;
    if (memchr(name, 0, nameLength) != name + nameLength - 1) {
        ERROR("__CheckRecord: record at byte %u has a name that does not end at byte %u of %u",
              offset, nameLength - 1, nameLength);
        return OS_EPROTOCOL;
    }

    *nextOut = offset + __RECORD_HEADER_SIZE + size;
    return OS_EOK;
}

static const char*
__RecordName(
    _In_ const DeviceFirmwareNode_t* node,
    _In_ uint32_t                    offset)
{
    return (const char*)node->Data + offset + __RECORD_HEADER_SIZE;
}

static uint32_t
__NextProperty(
    _In_ const DeviceFirmwareNode_t* node,
    _In_ uint32_t                    offset)
{
    uint32_t nameLength = __NodeReadU32(node->Data + offset);
    uint32_t valueLength = __NodeReadU32(node->Data + offset + 4);

    return offset + __RECORD_HEADER_SIZE + __RecordPadded(nameLength) + __RecordPadded(valueLength);
}

oserr_t
DeviceFirmwareNodeParse(
    _In_  const void*           data,
    _In_  size_t                length,
    _Out_ DeviceFirmwareNode_t* node)
{
    struct DeviceFirmwareNodeHeader header;
    DeviceFirmwareNode_t            result;
    const uint8_t*                  bytes = data;
    uint32_t                        offset;
    uint32_t                        next;
    oserr_t                         status;

    if (data == NULL || node == NULL) {
        ERROR("DeviceFirmwareNodeParse: missing description %p or node %p", data, node);
        return OS_EINVALPARAMS;
    }

    if (length < sizeof(header) || length > DEVICE_FIRMWARE_NODE_MAX_LENGTH) {
        ERROR("DeviceFirmwareNodeParse: description has %zu bytes, expected %zu to %u",
              length, sizeof(header), DEVICE_FIRMWARE_NODE_MAX_LENGTH);
        return OS_EPROTOCOL;
    }

    memcpy(&header, data, sizeof(header));
    if (header.Version != DEVICE_FIRMWARE_NODE_VERSION || header.Length != length) {
        ERROR("DeviceFirmwareNodeParse: header has version %u and length %u, expected version %u and length %zu",
              header.Version, header.Length, DEVICE_FIRMWARE_NODE_VERSION, length);
        return OS_EPROTOCOL;
    }

    result = (DeviceFirmwareNode_t) {
        .Data = bytes,
        .Length = (uint32_t)length,
        .ParentAddressCells = header.ParentAddressCells,
        .ParentSizeCells = header.ParentSizeCells,
        .PropertyCount = header.PropertyCount,
        .ChildCount = header.ChildCount
    };

    // Every record is at least eight bytes, so a count the length cannot hold
    // is refused here and the loops below stay bounded.
    offset = sizeof(header);
    if (header.PropertyCount > (result.Length - offset) / __RECORD_HEADER_SIZE ||
        header.ChildCount > (result.Length - offset) / __RECORD_HEADER_SIZE) {
        ERROR("DeviceFirmwareNodeParse: %u properties and %u children cannot fit in %u bytes",
              header.PropertyCount, header.ChildCount, result.Length);
        return OS_EPROTOCOL;
    }

    for (uint32_t i = 0; i < header.PropertyCount; i++) {
        status = __CheckRecord(bytes, result.Length, offset, 1, &next);
        if (status != OS_EOK) {
            ERROR("DeviceFirmwareNodeParse: property %u at byte %u is malformed (%u)", i, offset, status);
            return status;
        }

        // A name that appears twice would make lookups depend on order.
        for (uint32_t earlier = sizeof(header); earlier < offset; earlier = __NextProperty(&result, earlier)) {
            if (!strcmp(__RecordName(&result, earlier), __RecordName(&result, offset))) {
                ERROR("DeviceFirmwareNodeParse: property %s appears twice, at bytes %u and %u",
                      __RecordName(&result, offset), earlier, offset);
                return OS_EPROTOCOL;
            }
        }
        offset = next;
    }

    result.ChildOffset = offset;
    for (uint32_t i = 0; i < header.ChildCount; i++) {
        status = __CheckRecord(bytes, result.Length, offset, 0, &next);
        if (status != OS_EOK) {
            ERROR("DeviceFirmwareNodeParse: child %u at byte %u is malformed (%u)", i, offset, status);
            return status;
        }
        offset = next;
    }

    // Nothing may follow the last record.
    if (offset != result.Length) {
        ERROR("DeviceFirmwareNodeParse: the records end at byte %u of %u", offset, result.Length);
        return OS_EPROTOCOL;
    }

    *node = result;
    return OS_EOK;
}

/**
 * @brief Check the chunk deviced answered for the given offset.
 * length is the description length the first answer reported.
 */
static oserr_t
__CheckChunk(
    _In_ uuid_t                                  device,
    _In_ const struct DeviceFirmwareNodeRequest* request,
    _In_ uint32_t                                offset,
    _In_ uint32_t                                length)
{
    if (request->Length != length) {
        ERROR("DeviceFirmwareNodeRead: device %u chunk at offset %u reports length %u, the first reported %u",
              device, offset, request->Length, length);
        return OS_EPROTOCOL;
    }

    // An empty chunk before the end would never finish the read.
    if (request->Count == 0 || request->Count > DEVICE_FIRMWARE_NODE_CHUNK_SIZE ||
        request->Count > length - offset) {
        ERROR("DeviceFirmwareNodeRead: device %u chunk at offset %u has %u bytes, %u of %u are left",
              device, offset, request->Count, length - offset, length);
        return OS_EPROTOCOL;
    }
    return OS_EOK;
}

/**
 * @brief Ask deviced for the whole description, one chunk per request.
 * On success dataOut receives an allocation of lengthOut bytes. On failure
 * nothing is left allocated.
 */
static oserr_t
__ReadDescription(
    _In_  uuid_t                            device,
    _In_  struct DeviceFirmwareNodeRequest* request,
    _Out_ uint8_t**                         dataOut,
    _Out_ uint32_t*                         lengthOut)
{
    uint8_t* data = NULL;
    uint32_t length = 0;
    uint32_t offset = 0;
    oserr_t  status;

    // The first answer gives the length. Every later chunk must report the
    // same length and continue exactly where the previous one ended.
    do {
        memset(request, 0, sizeof(struct DeviceFirmwareNodeRequest));
        request->Offset = offset;
        status = OSDeviceIOCtl(device, OSIOCTLREQUEST_FIRMWARE_NODE, request, sizeof(struct DeviceFirmwareNodeRequest));
        if (status != OS_EOK) {
            ERROR("DeviceFirmwareNodeRead: device %u request at offset %u failed (%u)", device, offset, status);
            goto error;
        }

        if (data == NULL) {
            length = request->Length;
            if (length < sizeof(struct DeviceFirmwareNodeHeader) || length > DEVICE_FIRMWARE_NODE_MAX_LENGTH) {
                ERROR("DeviceFirmwareNodeRead: device %u reports length %u, expected %zu to %u",
                      device, length, sizeof(struct DeviceFirmwareNodeHeader), DEVICE_FIRMWARE_NODE_MAX_LENGTH);
                status = OS_EPROTOCOL;
                goto error;
            }

            data = malloc(length);
            if (data == NULL) {
                ERROR("DeviceFirmwareNodeRead: device %u description of %u bytes cannot be allocated",
                      device, length);
                status = OS_EOOM;
                goto error;
            }
        }

        status = __CheckChunk(device, request, offset, length);
        if (status != OS_EOK) {
            goto error;
        }

        memcpy(data + offset, &request->Data[0], request->Count);
        offset += request->Count;
    } while (offset < length);

    *dataOut = data;
    *lengthOut = length;
    return OS_EOK;

error:
    free(data);
    return status;
}

oserr_t
DeviceFirmwareNodeRead(
    _In_  uuid_t                device,
    _Out_ DeviceFirmwareNode_t* node)
{
    struct DeviceFirmwareNodeRequest* request;
    uint8_t*                          data;
    uint32_t                          length;
    oserr_t                           status;

    if (node == NULL) {
        ERROR("DeviceFirmwareNodeRead: device %u has no output node", device);
        return OS_EINVALPARAMS;
    }

    // The request holds a whole chunk, too large for a small driver stack.
    request = malloc(sizeof(struct DeviceFirmwareNodeRequest));
    if (request == NULL) {
        ERROR("DeviceFirmwareNodeRead: device %u request cannot be allocated", device);
        return OS_EOOM;
    }

    status = __ReadDescription(device, request, &data, &length);
    free(request);
    if (status != OS_EOK) {
        return status;
    }

    status = DeviceFirmwareNodeParse(data, length, node);
    if (status != OS_EOK) {
        ERROR("DeviceFirmwareNodeRead: device %u description of %u bytes is malformed (%u)",
              device, length, status);
        free(data);
        return status;
    }
    return OS_EOK;
}

void
DeviceFirmwareNodeDestroy(
    _In_ DeviceFirmwareNode_t* node)
{
    if (node == NULL) {
        return;
    }

    free((void*)node->Data);
    memset(node, 0, sizeof(DeviceFirmwareNode_t));
}

const uint8_t*
DeviceFirmwareNodeProperty(
    _In_      const DeviceFirmwareNode_t* node,
    _In_      const char*                 name,
    _Out_Opt_ uint32_t*                   length)
{
    uint32_t offset = sizeof(struct DeviceFirmwareNodeHeader);

    if (node == NULL || node->Data == NULL || name == NULL) {
        return NULL;
    }

    for (uint32_t i = 0; i < node->PropertyCount; i++) {
        if (!strcmp(__RecordName(node, offset), name)) {
            uint32_t nameLength = __NodeReadU32(node->Data + offset);

            if (length != NULL) {
                *length = __NodeReadU32(node->Data + offset + 4);
            }
            return node->Data + offset + __RECORD_HEADER_SIZE + __RecordPadded(nameLength);
        }
        offset = __NextProperty(node, offset);
    }
    return NULL;
}

oserr_t
DeviceFirmwareNodeFlag(
    _In_  const DeviceFirmwareNode_t* node,
    _In_  const char*                 name,
    _Out_ uint32_t*                   present)
{
    uint32_t length;

    if (present == NULL) {
        ERROR("DeviceFirmwareNodeFlag: property %s has no output", name != NULL ? name : "(null)");
        return OS_EINVALPARAMS;
    }

    if (DeviceFirmwareNodeProperty(node, name, &length) == NULL) {
        *present = 0;
        return OS_EOK;
    }

    // A boolean property has no value. One that carries a value is not what
    // the binding describes.
    if (length != 0) {
        ERROR("DeviceFirmwareNodeFlag: property %s has %u bytes, expected none", name, length);
        return OS_EINVALPARAMS;
    }
    *present = 1;
    return OS_EOK;
}

oserr_t
DeviceFirmwareNodeU32(
    _In_  const DeviceFirmwareNode_t* node,
    _In_  const char*                 name,
    _Out_ uint32_t*                   value)
{
    const uint8_t* bytes;
    uint32_t       length;

    if (value == NULL) {
        ERROR("DeviceFirmwareNodeU32: property %s has no output", name != NULL ? name : "(null)");
        return OS_EINVALPARAMS;
    }

    bytes = DeviceFirmwareNodeProperty(node, name, &length);
    if (bytes == NULL) {
        return OS_ENOENT;
    }

    if (length != 4) {
        ERROR("DeviceFirmwareNodeU32: property %s has %u bytes, expected 4", name, length);
        return OS_EINVALPARAMS;
    }
    *value = __NodeReadBe32(bytes);
    return OS_EOK;
}

oserr_t
DeviceFirmwareNodeStringIndex(
    _In_  const DeviceFirmwareNode_t* node,
    _In_  const char*                 name,
    _In_  const char*                 entry,
    _Out_ uint32_t*                   index,
    _Out_ uint32_t*                   count)
{
    const uint8_t* names;
    const uint8_t* end;
    uint32_t       length;
    uint32_t       offset = 0;
    uint32_t       entries = 0;
    uint32_t       selected = UINT32_MAX;

    if (entry == NULL || index == NULL || count == NULL) {
        ERROR("DeviceFirmwareNodeStringIndex: property %s has a missing entry or output",
              name != NULL ? name : "(null)");
        return OS_EINVALPARAMS;
    }

    names = DeviceFirmwareNodeProperty(node, name, &length);
    if (names == NULL) {
        return OS_ENOENT;
    }

    while (offset < length) {
        end = memchr(names + offset, 0, length - offset);
        if (end == NULL || end == names + offset) {
            ERROR("DeviceFirmwareNodeStringIndex: property %s has an empty or unterminated entry at byte %u",
                  name, offset);
            return OS_EINVALPARAMS;
        }

        if (!strcmp((const char*)names + offset, entry)) {
            if (selected != UINT32_MAX) {
                ERROR("DeviceFirmwareNodeStringIndex: property %s holds %s twice", name, entry);
                return OS_EINVALPARAMS;
            }
            selected = entries;
        }
        offset = (uint32_t)(end - names) + 1;
        entries++;
    }

    *index = selected;
    *count = entries;
    return selected == UINT32_MAX ? OS_ENOENT : OS_EOK;
}

oserr_t
DeviceFirmwareNodeChild(
    _In_  const DeviceFirmwareNode_t* node,
    _In_  uint32_t                    index,
    _Out_ DeviceFirmwareNodeChild_t*  child)
{
    uint32_t offset;

    if (node == NULL || node->Data == NULL || child == NULL) {
        ERROR("DeviceFirmwareNodeChild: missing node %p or child %p", node, child);
        return OS_EINVALPARAMS;
    }

    if (index >= node->ChildCount) {
        return OS_ENOENT;
    }

    // Child records have no value, so each one is its header and its name.
    offset = node->ChildOffset;
    for (uint32_t i = 0; i < index; i++) {
        offset += __RECORD_HEADER_SIZE + __RecordPadded(__NodeReadU32(node->Data + offset));
    }

    child->Name = __RecordName(node, offset);
    child->Flags = __NodeReadU32(node->Data + offset + 4);
    return OS_EOK;
}
