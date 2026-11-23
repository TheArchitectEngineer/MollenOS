/**
 * Copyright 2022, Philip Meulengracht
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
 */

//#define __TRACE
#define __need_minmax
#include <ddk/utils.h>
#include <fs/common.h>
#include <os/types/file.h>
#include <os/shm.h>
#include "mfs.h"
#include <stdlib.h>
#include <string.h>

// TODO:
//  - Implement bucket map transactions
//     * BucketMapStartTransaction
//     * BucketMapCommitTransaction
//     * BucketMapAbortTransaction
//  - Abstract the storage medium, multiple mediums
//  - Expand buckets s to 64 bit.

// Writes the in-memory master record to both copies on disk. The error of the
// first write that failed is returned. The secondary copy goes first, so a
// failure while the primary is written leaves the primary at its old value.
static oserr_t
__UpdateMasterRecords(
        _In_ FileSystemMFS_t* mfs)
{
    size_t  sectorsTransferred;
    oserr_t oserr;
    void*   buffer = SHMBuffer(&mfs->TransferBuffer);
    TRACE("__UpdateMasterRecords()");

    memset(buffer, 0, mfs->SectorSize);
    memcpy(buffer, &mfs->MasterRecord, sizeof(MasterRecord_t));

    // Flush the secondary copy first, so we can detect failures
    oserr = FSStorageWrite(
            &mfs->Storage,
            mfs->TransferBuffer.ID,
            0,
            &(UInteger64_t) { .QuadPart = mfs->MasterRecordMirrorSector},
            1,
            &sectorsTransferred
    );
    if (oserr != OS_EOK) {
        ERROR("Failed to write (secondary) master-record to disk: %u", oserr);
        return oserr;
    }

    // Flush the primary copy
    oserr = FSStorageWrite(
            &mfs->Storage,
            mfs->TransferBuffer.ID,
            0,
            &(UInteger64_t) { .QuadPart = mfs->MasterRecordSector},
            1,
            &sectorsTransferred
    );
    if (oserr != OS_EOK) {
        ERROR("Failed to write (primary) master-record to disk: %u", oserr);
    }
    return oserr;
}

oserr_t
MfsZeroBucket(
        _In_ FileSystemMFS_t* mfs,
        _In_ uint32_t         bucket,
        _In_ size_t           count)
{
    size_t i;
    size_t sectorsTransferred;
    TRACE("MfsZeroBucket(bucket=%u, count=%u)", bucket, count);

    memset(SHMBuffer(&mfs->TransferBuffer), 0, SHMBufferLength(&mfs->TransferBuffer));
    for (i = 0; i < count; i++) {
        uint64_t sector = MFS_GETSECTOR(mfs, bucket + i);
        oserr_t  oserr;

        oserr = FSStorageWrite(
                &mfs->Storage,
                mfs->TransferBuffer.ID,
                0,
                &(UInteger64_t) { .QuadPart = sector },
                mfs->SectorsPerBucket,
                &sectorsTransferred
        );
        if (oserr != OS_EOK) {
            ERROR("MfsZeroBucket failed to write bucket to disk");
            return oserr;
        }
    }
    return OS_EOK;
}

oserr_t
MFSBucketMapGetLengthAndLink(
        _In_  FileSystemMFS_t* mfs,
        _In_  bucket_t         bucket,
        _Out_ MapRecord_t*     link)
{
    TRACE("MFSBucketMapGetLengthAndLink(bucket=%u)", bucket);
    if (bucket >= mfs->BucketsInMap) {
        return OS_EINVALPARAMS;
    }

    link->Link   = mfs->BucketMap[(bucket * 2)];
    link->Length = mfs->BucketMap[(bucket * 2) + 1];
    TRACE("... link %u, length %u", link->Link, link->Length);
    return OS_EOK;
}

oserr_t
MFSBucketMapSetLinkAndLength(
        _In_ FileSystemMFS_t* mfs,
        _In_ bucket_t         bucket,
        _In_ bucket_t         link,
        _In_ uint32_t         length,
        _In_ bool             updateLength)
{
    uint8_t* bufferOffset;
    size_t   sectorOffset;
    size_t   sectorsTransferred;
    uint32_t previousLink;
    uint32_t previousLength;
    oserr_t  oserr;
    TRACE("MFSBucketMapSetLinkAndLength(bucket=%u, link=%u, length=%u)",
          bucket, link, length);

    // Update in-memory map first, as the map sector is written from it. The
    // old values are restored if the write fails, so the map in memory keeps
    // matching the map on disk.
    previousLink = mfs->BucketMap[(bucket * 2)];
    previousLength = mfs->BucketMap[(bucket * 2) + 1];
    mfs->BucketMap[(bucket * 2)] = link;
    if (updateLength) {
        mfs->BucketMap[(bucket * 2) + 1] = length;
    }

    // Calculate which sector that is dirty now
    sectorOffset = bucket / mfs->BucketsPerSectorInMap;

    // Calculate offset into buffer
    bufferOffset = (uint8_t*)mfs->BucketMap;
    bufferOffset += (sectorOffset * mfs->SectorSize);

    // Copy a sector's worth of data into the buffer
    memcpy(SHMBuffer(&mfs->TransferBuffer), bufferOffset, mfs->SectorSize);

    // Flush buffer to disk
    oserr = FSStorageWrite(
            &mfs->Storage,
            mfs->TransferBuffer.ID,
            0,
            &(UInteger64_t) { .QuadPart = mfs->MasterRecord.MapSector + sectorOffset },
            1,
            &sectorsTransferred
    );
    if (oserr != OS_EOK) {
        ERROR("Failed to update the given map-sector %u on disk: %u",
              LODWORD(mfs->MasterRecord.MapSector + sectorOffset), oserr);
        mfs->BucketMap[(bucket * 2)] = previousLink;
        mfs->BucketMap[(bucket * 2) + 1] = previousLength;
        return oserr;
    }
    return OS_EOK;
}

// Checks that the free list holds at least bucketCount buckets. It only reads
// the map, so MFSBucketMapAllocate can refuse an allocation before it writes
// anything. Returns OS_EINCOMPLETE when the free list is too short, or the
// error of the map lookup that failed (OS_EINVALPARAMS for a free list head of
// MFS_ENDOFCHAIN, a disk without free buckets).
static oserr_t
__FreeListHoldsBuckets(
        _In_ FileSystemMFS_t* mfs,
        _In_ size_t           bucketCount)
{
    bucket_t    freeBucket   = mfs->MasterRecord.FreeBucket;
    size_t      bucketsFound = 0;
    MapRecord_t freeRun;
    oserr_t     oserr;

    while (bucketsFound < bucketCount) {
        oserr = MFSBucketMapGetLengthAndLink(mfs, freeBucket, &freeRun);
        if (oserr != OS_EOK) {
            ERROR("__FreeListHoldsBuckets failed to get the free run at bucket %u: %u", freeBucket, oserr);
            return oserr;
        }

        bucketsFound += freeRun.Length;
        if (bucketsFound < bucketCount && freeRun.Link == MFS_ENDOFCHAIN) {
            WARNING("__FreeListHoldsBuckets has %u free buckets, %u were asked for",
                    LODWORD(bucketsFound), LODWORD(bucketCount));
            return OS_EINCOMPLETE;
        }
        freeBucket = freeRun.Link;
    }
    return OS_EOK;
}

// Stores one run of a new allocation in the map: its link and its length. The
// map entry is only written when it does not hold these values already. A free
// run that is handed out whole and is not the last run of the allocation still
// links to the next free run, which is the next run of the allocation, so it
// needs no write.
static oserr_t
__StoreAllocatedRun(
        _In_ FileSystemMFS_t* mfs,
        _In_ bucket_t         bucket,
        _In_ bucket_t         link,
        _In_ uint32_t         length)
{
    MapRecord_t current;
    oserr_t     oserr;

    oserr = MFSBucketMapGetLengthAndLink(mfs, bucket, &current);
    if (oserr != OS_EOK) {
        ERROR("__StoreAllocatedRun failed to get the run of bucket %u: %u", bucket, oserr);
        return oserr;
    }
    if (current.Link == link && current.Length == length) {
        return OS_EOK;
    }

    oserr = MFSBucketMapSetLinkAndLength(mfs, bucket, link, length, true);
    if (oserr != OS_EOK) {
        ERROR("__StoreAllocatedRun failed to store run %u (link %u, length %u): %u", bucket, link, length, oserr);
    }
    return oserr;
}

// Takes bucketCount buckets from the free list, and returns the first run of
// them in allocatedRecord (Link is its first bucket, Length its length). The
// buckets are linked into a chain in the map that ends with MFS_ENDOFCHAIN.
//
// Free runs are taken from the head of the free list. When a free run starts at
// the bucket right after the run before it, the two become one run of the
// chain. So the chain never has two runs where the second starts at the bucket
// right after the first ends. This matters because MFS reads and writes a file
// one storage request per run. A free list of one-bucket runs that lie next to
// each other, as freeing a file grown in small steps leaves it, is handed out
// as one run.
//
// First the free list is checked to hold enough buckets. If it does not,
// nothing is written and OS_EINCOMPLETE is returned. Then the map is written in
// this order, one map sector write each, and the master record last:
// 1. Runs of the chain that are complete, in chain order. A run is complete
//    when the next free run does not follow it on disk. Its map entry gets the
//    first bucket of the next run as link, and the summed length. A run made
//    of one whole free run already has these values and is not written.
// 2. When the last free run taken is longer than needed, it is split. The map
//    entry of the bucket after the part taken gets the rest of the free run:
//    its old link and the remaining length. This bucket lies inside a free run
//    until the master record is written, so nothing reads the entry before.
// 3. The last run of the chain: link MFS_ENDOFCHAIN and its length.
// 4. The master record, with the new free list head.
// What is on disk after a failed write, with the old free list head still in
// the master record:
// - In step 1: the free list holds the same buckets as before, some of its
//   runs joined. Nothing is lost.
// - In step 2 or 3: as step 1. A failed step 3 also keeps the split rest
//   unused, as it lies inside the free run it came from.
// - In step 4: the free list is now the chain itself, as it ends with the
//   last run of the chain. The buckets of the chain stay free. The split rest
//   and the free runs after the chain are on no list (lost space).
// No bucket is handed out twice: the master record points at the split rest
// only after its entry was written. MFSBucketMapSetLinkAndLength restores the
// in-memory map entry when its write fails, and the in-memory free list head
// only changes when the master record was written, so memory matches the disk
// after any failure. The error of the failed write is returned.
oserr_t
MFSBucketMapAllocate(
        _In_ FileSystemMFS_t* mfs,
        _In_ size_t           bucketCount,
        _In_ MapRecord_t*     allocatedRecord)
{
    bucket_t    freeBucket;
    bucket_t    nextFreeBucket;
    bucket_t    previousFreeBucket;
    bucket_t    runBucket   = MFS_ENDOFCHAIN;
    uint32_t    runLength   = 0;
    size_t      bucketsLeft = bucketCount;
    uint32_t    bucketsTaken;
    MapRecord_t freeRun;
    oserr_t     oserr;

    if (bucketCount == 0) {
        ERROR("MFSBucketMapAllocate was asked for 0 buckets");
        return OS_EINVALPARAMS;
    }

    oserr = __FreeListHoldsBuckets(mfs, bucketCount);
    if (oserr != OS_EOK) {
        return oserr;
    }

    allocatedRecord->Link   = mfs->MasterRecord.FreeBucket;
    allocatedRecord->Length = 0;

    freeBucket = mfs->MasterRecord.FreeBucket;
    while (bucketsLeft) {
        oserr = MFSBucketMapGetLengthAndLink(mfs, freeBucket, &freeRun);
        if (oserr != OS_EOK) {
            ERROR("MFSBucketMapAllocate failed to get the free run at bucket %u: %u", freeBucket, oserr);
            return oserr;
        }
        bucketsTaken   = (uint32_t)MIN(bucketsLeft, (size_t)freeRun.Length);
        nextFreeBucket = freeRun.Link;

        // A free run that follows the current run on disk makes it longer.
        // Otherwise the current run is complete (step 1), and a new run starts.
        if (runBucket != MFS_ENDOFCHAIN && runBucket + runLength == freeBucket) {
            runLength += bucketsTaken;
        } else {
            if (runBucket != MFS_ENDOFCHAIN) {
                oserr = __StoreAllocatedRun(mfs, runBucket, freeBucket, runLength);
                if (oserr != OS_EOK) {
                    ERROR("MFSBucketMapAllocate failed to store run %u: %u", runBucket, oserr);
                    return oserr;
                }
                if (allocatedRecord->Length == 0) {
                    allocatedRecord->Length = runLength;
                }
            }
            runBucket = freeBucket;
            runLength = bucketsTaken;
        }

        // Split the free run when only a part of it is taken (step 2)
        if (bucketsTaken < freeRun.Length) {
            nextFreeBucket = freeBucket + bucketsTaken;
            oserr = MFSBucketMapSetLinkAndLength(
                    mfs,
                    nextFreeBucket,
                    freeRun.Link,
                    freeRun.Length - bucketsTaken,
                    true
            );
            if (oserr != OS_EOK) {
                ERROR("MFSBucketMapAllocate failed to store the rest of free run %u at bucket %u: %u",
                      freeBucket, nextFreeBucket, oserr);
                return oserr;
            }
        }

        bucketsLeft -= bucketsTaken;
        freeBucket   = nextFreeBucket;
    }

    // The last run of the chain (step 3)
    oserr = __StoreAllocatedRun(mfs, runBucket, MFS_ENDOFCHAIN, runLength);
    if (oserr != OS_EOK) {
        ERROR("MFSBucketMapAllocate failed to store the last run %u: %u", runBucket, oserr);
        return oserr;
    }
    if (allocatedRecord->Length == 0) {
        allocatedRecord->Length = runLength;
    }

    // The new free list head (step 4). freeBucket is the bucket after the
    // last one taken: the split rest, or the free run after the last one.
    previousFreeBucket = mfs->MasterRecord.FreeBucket;
    mfs->MasterRecord.FreeBucket = freeBucket;
    oserr = __UpdateMasterRecords(mfs);
    if (oserr != OS_EOK) {
        ERROR("MFSBucketMapAllocate failed to store the new free bucket %u: %u", freeBucket, oserr);
        mfs->MasterRecord.FreeBucket = previousFreeBucket;
    }
    return oserr;
}

oserr_t
MfsFreeBuckets(
        _In_ FileSystemMFS_t* mfs,
        _In_ uint32_t         startBucket,
        _In_ uint32_t         startLength)
{
    uint32_t    previousBucket;
    MapRecord_t mapRecord;

    TRACE("MfsFreeBuckets(Bucket %u, Length %u)", startBucket, startLength);

    if (startLength == 0) {
        return OS_EUNKNOWN;
    }

    // Essentially there is two algorithms we can deploy here
    // The quick one - Which is just to add the allocated bucket list
    // to the free and set the last allocated to point to the first free
    // OR there is the slow one that makes sure that buckets are <in order> as
    // they get freed, and gets inserted or extended correctly. This will reduce
    // fragmentation by A LOT
    mapRecord.Link = startBucket;

    // Start by iterating to the last bucket
    previousBucket = MFS_ENDOFCHAIN;
    while (mapRecord.Link != MFS_ENDOFCHAIN) {
        previousBucket = mapRecord.Link;
        if (MFSBucketMapGetLengthAndLink(mfs, mapRecord.Link, &mapRecord) != OS_EOK) {
            ERROR("Failed to retrieve the next bucket-link");
            return OS_EUNKNOWN;
        }
    }

    // If there was no allocated buckets to start with then do nothing. The
    // in-memory free list head is only changed when the master record was
    // written, as in MFSBucketMapAllocate.
    if (previousBucket != MFS_ENDOFCHAIN) {
        bucket_t previousFreeBucket = mfs->MasterRecord.FreeBucket;

        // Ok, so now update the pointer to free list
        oserr_t oserr = MFSBucketMapSetLinkAndLength(
                mfs,
                previousBucket,
                mfs->MasterRecord.FreeBucket,
                0,
                false
        );
        if (oserr != OS_EOK) {
            ERROR("Failed to update the next bucket-link");
            return oserr;
        }
        mfs->MasterRecord.FreeBucket = startBucket;
        oserr = __UpdateMasterRecords(mfs);
        if (oserr != OS_EOK) {
            ERROR("MfsFreeBuckets failed to store the new free bucket %u: %u", startBucket, oserr);
            mfs->MasterRecord.FreeBucket = previousFreeBucket;
        }
        return oserr;
    }
    return OS_EOK;
}
