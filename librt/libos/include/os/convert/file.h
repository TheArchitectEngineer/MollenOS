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
 * Conversions between the file and storage types of libos and the structures
 * of the gracht file protocol.
 */

#ifndef __OS_CONVERT_FILE_H__
#define __OS_CONVERT_FILE_H__

#include <os/types/file.h>
#include <os/types/storage.h>
#include <os/types/time.h>
#include <string.h>

#include <sys_file_service.h>

static void from_sys_timestamp(struct sys_timestamp* in, OSTimestamp_t* out)
{
    out->Seconds = in->tv_sec;
    out->Nanoseconds = in->tv_nsec;
}

static void to_sys_timestamp(OSTimestamp_t* in, struct sys_timestamp* out)
{
    out->tv_sec = in->Seconds;
    out->tv_nsec = in->Nanoseconds;
}

static void from_sys_disk_descriptor(struct sys_disk_descriptor* in, OSStorageDescriptor_t* out)
{
    size_t len = strnlen(in->serial, sizeof(out->SerialNumber) - 1);

    out->Id = 0;
    out->Flags = (unsigned int)in->flags;
    out->SectorSize = in->geometry.sector_size;
    out->SectorsTotal.QuadPart = in->geometry.sectors_total;

    memcpy(&out->SerialNumber[0], in->serial, len);
    out->SerialNumber[len] = 0;
}

static void from_sys_filesystem_descriptor(struct sys_filesystem_descriptor* in, OSFileSystemDescriptor_t* out)
{
    size_t len = strnlen(in->serial, sizeof(out->SerialNumber) - 1);

    out->Id = in->id;
    out->Flags = in->flags;
    out->BlockSize = in->block_size;
    out->BlocksPerSegment = in->blocks_per_segment;
    out->MaxFilenameLength = in->max_filename_length;
    out->SegmentsFree.QuadPart = in->segments_free;
    out->SegmentsTotal.QuadPart = in->segments_total;

    memcpy(&out->SerialNumber[0], in->serial, len);
    out->SerialNumber[len] = 0;
}

static void from_sys_file_descriptor(struct sys_file_descriptor* in, OSFileDescriptor_t* out)
{
    out->Id = in->id;
    out->StorageId = in->storageId;
    out->Flags = (unsigned int)in->flags;
    out->Permissions = (unsigned int)in->permissions;
    out->Size.QuadPart = in->size;

    from_sys_timestamp(&in->created, &out->CreatedAt);
    from_sys_timestamp(&in->accessed, &out->AccessedAt);
    from_sys_timestamp(&in->modified, &out->ModifiedAt);
}

static void to_sys_file_descriptor(OSFileDescriptor_t* in, struct sys_file_descriptor* out)
{
    out->id = in->Id;
    out->storageId = in->StorageId;
    out->flags = (enum sys_file_flags)in->Flags;
    out->permissions = (enum sys_file_permissions)in->Permissions;
    out->size = in->Size.QuadPart;

    to_sys_timestamp(&in->CreatedAt, &out->created);
    to_sys_timestamp(&in->AccessedAt, &out->accessed);
    to_sys_timestamp(&in->ModifiedAt, &out->modified);
}

#endif //!__OS_CONVERT_FILE_H__
