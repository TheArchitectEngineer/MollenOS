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
 * General File System (MFS) Driver
 *  - Contains the implementation of the MFS driver for mollenos
 */

//#define __TRACE

#include <ddk/utils.h>
#include <fs/common.h>
#include <os/types/file.h>
#include <os/shm.h>
#include "mfs.h"
#include <stdlib.h>
#include <string.h>

unsigned int
MFSToNativeFlags(
    _In_ unsigned int flags)
{
    unsigned int nativeFlags = 0;
    if (flags & FILE_FLAG_DIRECTORY) {
        nativeFlags |= MFS_FILERECORD_DIRECTORY;
    } else if (flags & FILE_FLAG_LINK) {
        nativeFlags |= MFS_FILERECORD_LINK;
    }
    return nativeFlags;
}

void
MFSFromNativeFlags(
    _In_  FileRecord_t* fileRecord,
    _Out_ unsigned int* flags,
    _Out_ unsigned int* permissions)
{
    // Permissions are not really implemented
    *permissions = (FILE_PERMISSION_READ | FILE_PERMISSION_WRITE | FILE_PERMISSION_EXECUTE);
    *flags       = 0;

    if (fileRecord->Flags & MFS_FILERECORD_DIRECTORY) {
        *flags |= FILE_FLAG_DIRECTORY;
    } else if (fileRecord->Flags & MFS_FILERECORD_LINK) {
        *flags |= FILE_FLAG_LINK;
    }
}

void
MfsFileRecordToVfsFile(
        _In_ FileSystemMFS_t* mfs,
        _In_ FileRecord_t*    nativeEntry,
        _In_ MFSEntry_t*      mfsEntry)
{
    TRACE("MfsFileRecordToVfsFile()");

    // VfsEntry->Base.Descriptor.Id = ??
    mfsEntry->Name          = mstr_new_u8((const char*)&nativeEntry->Name[0]);
    mfsEntry->NativeFlags   = nativeEntry->Flags;
    mfsEntry->ActualSize    = nativeEntry->Size;
    mfsEntry->AllocatedSize = nativeEntry->AllocatedSize;
    mfsEntry->StartBucket   = nativeEntry->StartBucket;
    mfsEntry->StartLength   = nativeEntry->StartLength;

    // Convert flags to generic vfs flags and permissions
    MFSFromNativeFlags(nativeEntry,
                       &mfsEntry->Flags,
                       &mfsEntry->Permissions);

    // TODO Convert dates
    // VfsEntry->Base.DescriptorCreatedAt;
    // VfsEntry->Base.DescriptorModifiedAt;
    // VfsEntry->Base.DescriptorAccessedAt;
}

void
MFSEntryResetPosition(
        _In_ MFSEntry_t* entry)
{
    entry->Position           = 0;
    entry->BucketByteBoundary = 0;
    entry->DataBucketPosition = entry->StartBucket;
    entry->DataBucketLength   = entry->StartLength;

    // A record without buckets may still carry a length. The data bucket
    // length is 0 then, as there is nothing to read or write.
    if (entry->StartBucket == MFS_ENDOFCHAIN) {
        entry->DataBucketLength = 0;
    }
}

oserr_t
MfsUpdateRecord(
        _In_ FileSystemMFS_t* mfs,
        _In_ MFSEntry_t*      entry,
        _In_ int              action)
{
    oserr_t       oserr;
    FileRecord_t* record;
    size_t        sectorsTransferred;
    TRACE("MfsUpdateEntry(entry=%ms, action=%i)", entry->Name, action);

    // Read the stored data bucket where the record is
    oserr = FSStorageRead(
            &mfs->Storage,
            mfs->TransferBuffer.ID,
            0,
            &(UInteger64_t) { .QuadPart = MFS_GETSECTOR(mfs, entry->DirectoryBucket) },
            MFS_SECTORCOUNT(mfs, entry->DirectoryLength),
            &sectorsTransferred
    );
    if (oserr != OS_EOK) {
        ERROR("MfsUpdateEntry Failed to read bucket %u", entry->DirectoryBucket);
        goto Cleanup;
    }

    record = (FileRecord_t*)((uint8_t*)SHMBuffer(&mfs->TransferBuffer) + (sizeof(FileRecord_t) * entry->DirectoryIndex));

    // We have two over-all cases here, as create/modify share
    // some code, and that is delete as the second. If we delete
    // we zero out the entry and set the status to deleted
    if (action == MFS_ACTION_DELETE) {
        memset((void*)record, 0, sizeof(FileRecord_t));
    } else if (action == MFS_ACTION_RENAME) {
        // Only the name changes. The data, the sizes and the integrated data
        // stay, so a rename is this one write.
        char*  entryName = mstr_u8(entry->Name);
        size_t nameLength;

        if (entryName == NULL) {
            return OS_EOOM;
        }

        // The name is stored zero terminated.
        nameLength = strlen(entryName);
        if (nameLength >= sizeof(record->Name)) {
            ERROR("MfsUpdateEntry name %ms is too long for a record", entry->Name);
            free(entryName);
            return OS_EINVALPARAMS;
        }
        memset(&record->Name[0], 0, sizeof(record->Name));
        memcpy(&record->Name[0], entryName, nameLength);
        free(entryName);
    } else {
        // Now we have two sub cases, but create just needs some
        // extra updates otherwise they share
        if (action == MFS_ACTION_CREATE) {
            char* entryName = mstr_u8(entry->Name);
            if (entryName == NULL) {
                return OS_EOOM;
            }

            memset(&record->Integrated[0], 0, 512);
            memset(&record->Name[0], 0, 300);
            memcpy(&record->Name[0], entryName, strlen(entryName));
            free(entryName);
        }

        // Update stats that are modifiable
        record->Flags       = entry->NativeFlags | MFS_FILERECORD_INUSE;
        record->StartBucket = entry->StartBucket;
        record->StartLength = entry->StartLength;

        // Update modified / accessed dates

        // Update sizes
        record->Size          = entry->ActualSize;
        record->AllocatedSize = entry->AllocatedSize;
    }
    
    // Write the bucket back to the disk
    oserr = FSStorageWrite(
            &mfs->Storage,
            mfs->TransferBuffer.ID,
            0,
            &(UInteger64_t) { .QuadPart = MFS_GETSECTOR(mfs, entry->DirectoryBucket) },
            MFS_SECTORCOUNT(mfs, entry->DirectoryLength),
            &sectorsTransferred
    );
    if (oserr != OS_EOK) {
        ERROR("MfsUpdateEntry Failed to update bucket %u", entry->DirectoryBucket);
    }

    // Cleanup and exit
Cleanup:
    return oserr;
}

// Finds the last run of the bucket chain of entry. The walk starts at the data
// bucket of the handle, which is a run of the chain, so only the runs from the
// position on are visited. A write at the end of the file has its data bucket
// in the last run, and then the walk is one map lookup, however long the file
// is. A handle without a data bucket (MFS_ENDOFCHAIN, see
// MFSEntryResetPosition) walks from the first bucket of the file. The caller
// makes sure the file has buckets (StartBucket is not MFS_ENDOFCHAIN).
// The handle keeps no copy of the last bucket, as a copy goes stale when
// another handle of the same file adds buckets.
static oserr_t
__FindLastRun(
        _In_  FileSystemMFS_t* mfs,
        _In_  MFSEntry_t*      entry,
        _Out_ bucket_t*        lastBucketOut,
        _Out_ MapRecord_t*     lastRunOut)
{
    bucket_t    bucket = entry->DataBucketPosition;
    MapRecord_t run;
    oserr_t     oserr;

    if (bucket == MFS_ENDOFCHAIN) {
        bucket = entry->StartBucket;
    }

    while (1) {
        oserr = MFSBucketMapGetLengthAndLink(mfs, bucket, &run);
        if (oserr != OS_EOK) {
            ERROR("__FindLastRun failed to get the run of bucket %u in %ms: %u", bucket, entry->Name, oserr);
            return oserr;
        }
        if (run.Link == MFS_ENDOFCHAIN) {
            break;
        }
        bucket = run.Link;
    }

    *lastBucketOut = bucket;
    *lastRunOut    = run;
    return OS_EOK;
}

// Adds the new buckets to the end of the chain of entry. The new buckets start
// at newRun.Link, and their first run has newRun.Length buckets.
// - When the first new run starts at the bucket right after the last run
//   (lastBucket + lastRun.Length == newRun.Link), the last run is made longer
//   instead. MFS reads and writes a file one storage request per run, so a
//   file that grows in small steps would otherwise be read in small requests
//   for good. The map entry of the last run gets the summed length and the
//   link of the first new run. The map entry of newRun.Link now lies inside a
//   run and is not read again.
// - Otherwise the last run links to the first new run.
// It is one map write either way. If it fails, the map and the entry stay as
// they were, and the new buckets are in no file and not on the free list
// (lost space).
// The entry keeps two copies of run lengths that are changed here as well:
// StartLength when the last run is the first run (written to the record at
// close), and DataBucketLength when the handle is in the last run.
static oserr_t
__AppendRun(
        _In_ FileSystemMFS_t* mfs,
        _In_ MFSEntry_t*      entry,
        _In_ MapRecord_t*     newRun)
{
    bucket_t    lastBucket;
    MapRecord_t lastRun;
    MapRecord_t firstNewRun;
    uint32_t    joinedLength;
    oserr_t     oserr;

    oserr = __FindLastRun(mfs, entry, &lastBucket, &lastRun);
    if (oserr != OS_EOK) {
        ERROR("__AppendRun failed to find the last run of %ms: %u", entry->Name, oserr);
        return oserr;
    }

    if (lastBucket + lastRun.Length != newRun->Link) {
        TRACE("__AppendRun: linking run %u of %ms to new run %u", lastBucket, entry->Name, newRun->Link);
        oserr = MFSBucketMapSetLinkAndLength(mfs, lastBucket, newRun->Link, 0, false);
        if (oserr != OS_EOK) {
            ERROR("__AppendRun failed to link run %u of %ms to run %u: %u",
                  lastBucket, entry->Name, newRun->Link, oserr);
        }
        return oserr;
    }

    // The new run follows the last run on disk. Its link is the rest of the
    // new buckets, or MFS_ENDOFCHAIN.
    oserr = MFSBucketMapGetLengthAndLink(mfs, newRun->Link, &firstNewRun);
    if (oserr != OS_EOK) {
        ERROR("__AppendRun failed to get the run of new bucket %u: %u", newRun->Link, oserr);
        return oserr;
    }

    joinedLength = lastRun.Length + newRun->Length;
    TRACE("__AppendRun: run %u of %ms grows to %u buckets", lastBucket, entry->Name, joinedLength);
    oserr = MFSBucketMapSetLinkAndLength(mfs, lastBucket, firstNewRun.Link, joinedLength, true);
    if (oserr != OS_EOK) {
        ERROR("__AppendRun failed to grow run %u of %ms to %u buckets: %u",
              lastBucket, entry->Name, joinedLength, oserr);
        return oserr;
    }

    if (entry->StartBucket == lastBucket) {
        entry->StartLength = joinedLength;
    }
    if (entry->DataBucketPosition == lastBucket) {
        entry->DataBucketLength = joinedLength;
    }
    return OS_EOK;
}

oserr_t
MfsEnsureRecordSpace(
        _In_ FileSystemMFS_t* mfs,
        _In_ MFSEntry_t*      entry,
        _In_ uint64_t         spaceRequired)
{
    size_t      bucketSizeBytes = mfs->SectorsPerBucket * mfs->SectorSize;
    size_t      sectorCount;
    size_t      bucketCount;
    MapRecord_t newRun;
    oserr_t     oserr;
    TRACE("MfsEnsureRecordSpace(%u)", LODWORD(spaceRequired));

    if (spaceRequired <= entry->AllocatedSize) {
        return OS_EOK;
    }

    // Calculate the number of sectors, then number of buckets
    sectorCount = (size_t)(DIVUP((spaceRequired - entry->AllocatedSize), mfs->SectorSize));
    bucketCount = DIVUP(sectorCount, mfs->SectorsPerBucket);

    oserr = MFSBucketMapAllocate(mfs, bucketCount, &newRun);
    if (oserr != OS_EOK) {
        ERROR("MfsEnsureRecordSpace failed to allocate %u buckets for %ms: %u", bucketCount, entry->Name, oserr);
        return OS_EDEVFAULT;
    }

    if (entry->StartBucket == MFS_ENDOFCHAIN) {
        // The file had no buckets. The record gets the first new run when the
        // handle is closed.
        TRACE("MfsEnsureRecordSpace: initializing record %ms to start=0x%x, length=0x%x",
              entry->Name, newRun.Link, newRun.Length);
        entry->StartBucket = newRun.Link;
        entry->StartLength = newRun.Length;
    } else {
        oserr = __AppendRun(mfs, entry, &newRun);
        if (oserr != OS_EOK) {
            ERROR("MfsEnsureRecordSpace failed to add %u buckets to %ms: %u", bucketCount, entry->Name, oserr);
            return OS_EDEVFAULT;
        }
    }

    entry->AllocatedSize += (bucketCount * bucketSizeBytes);
    entry->ActionOnClose  = MFS_ACTION_UPDATE;
    return OS_EOK;
}

oserr_t
MFSCloneBucketData(
        _In_ FileSystemMFS_t* mfs,
        _In_ uint32_t         sourceBucket,
        _In_ uint32_t         sourceLength,
        _In_ uint32_t         destinationBucket,
        _In_ uint32_t         destinationLength)
{
    TRACE("MFSCloneBucketData()");
    return OS_ENOTSUPPORTED;
}

void
MFSEntrySetDataRun(
        _In_ MFSEntry_t* entry,
        _In_ bucket_t    bucket,
        _In_ uint32_t    length,
        _In_ uint64_t    byteBoundary)
{
    entry->DataBucketPosition = bucket;
    entry->DataBucketLength   = length;
    entry->BucketByteBoundary = byteBoundary;
    if (bucket == entry->StartBucket) {
        entry->StartLength = length;
    }
}

// Called when the position of entry is at the end of its data bucket, as the
// handle's copy of the run length (DataBucketLength) gives it. The map entry
// of the run is read first:
// - The run is longer in the map than in the copy. Another handle of the same
//   file, or this handle before a crash, made it longer (see __AppendRun in
//   this file). The position still lies in the run, so the handle stays and
//   only takes the new length. Stepping on would add the map's length to
//   BucketByteBoundary and skip the bytes the run grew by.
// - The run has no link. OS_ENOENT is returned, and the handle is unchanged.
// - Otherwise the handle steps to the linked run.
oserr_t
MFSAdvanceToNextBucket(
        _In_ FileSystemMFS_t* mfs,
        _In_ MFSEntry_t*      entry)
{
    size_t      bucketSizeBytes = mfs->SectorsPerBucket * mfs->SectorSize;
    MapRecord_t currentRun;
    MapRecord_t nextRun;
    oserr_t     oserr;

    oserr = MFSBucketMapGetLengthAndLink(mfs, entry->DataBucketPosition, &currentRun);
    if (oserr != OS_EOK) {
        ERROR("MFSAdvanceToNextBucket failed to get the run of bucket %u: %u", entry->DataBucketPosition, oserr);
        return OS_EDEVFAULT;
    }
    TRACE("MFSAdvanceToNextBucket: bucket 0x%x, link=0x%x, length=0x%x",
          entry->DataBucketPosition, currentRun.Link, currentRun.Length);

    if (currentRun.Length > entry->DataBucketLength) {
        MFSEntrySetDataRun(entry, entry->DataBucketPosition, currentRun.Length, entry->BucketByteBoundary);
        return OS_EOK;
    }

    if (currentRun.Link == MFS_ENDOFCHAIN) {
        return OS_ENOENT;
    }

    oserr = MFSBucketMapGetLengthAndLink(mfs, currentRun.Link, &nextRun);
    if (oserr != OS_EOK) {
        ERROR("MFSAdvanceToNextBucket failed to get the run of bucket %u: %u", currentRun.Link, oserr);
        return OS_EDEVFAULT;
    }
    TRACE("MFSAdvanceToNextBucket: bucket 0x%x, link=0x%x, length=0x%x",
          currentRun.Link, nextRun.Link, nextRun.Length);

    MFSEntrySetDataRun(
            entry,
            currentRun.Link,
            nextRun.Length,
            entry->BucketByteBoundary + (currentRun.Length * bucketSizeBytes)
    );
    return OS_EOK;
}
