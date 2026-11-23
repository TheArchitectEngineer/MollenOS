/**
 * Copyright 2023, Philip Meulengracht
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
#include <os/shm.h>
#include <string.h>
#include "mfs.h"

oserr_t FsSeekInFile(FileSystemMFS_t*, MFSEntry_t*, uint64_t);

// Returns whether the data bucket of entry holds the byte at position. It uses
// the handle's copy of the run length, which may be shorter than the run is
// in the bucket map (see MFSEntry_t in mfs.h). A false answer only sends
// __SeekToBucketOfPosition to FsSeekInFile, which reads the length from the
// map, so a run that grew is still found.
static bool
__BucketHoldsPosition(
        _In_ FileSystemMFS_t* mfs,
        _In_ MFSEntry_t*      entry,
        _In_ uint64_t         position)
{
    size_t bucketSizeBytes = mfs->SectorsPerBucket * mfs->SectorSize;

    if (entry->DataBucketPosition == MFS_ENDOFCHAIN) {
        return false;
    }
    return position >= entry->BucketByteBoundary &&
           position < (entry->BucketByteBoundary + (entry->DataBucketLength * bucketSizeBytes));
}

// Makes the data bucket of entry the bucket that holds position. The read and
// write loops take the sector from the data bucket, and only step to the next
// bucket when the position is exactly at the end of the current one. The data
// bucket does not hold the position when:
// - The file had no buckets when it was created or opened, at its last seek,
//   or after a truncate to 0. The data bucket is then MFS_ENDOFCHAIN.
// - FsSeekInFile went past the last bucket of the file. It stops at that
//   bucket, and the position lies past its end. A write or a truncate may
//   since have given the file the bucket of the position.
// - The position is at the end of the data bucket, as the handle's copy of
//   the run length gives it. A write at the end of the file is the common
//   case. The run may have grown, or the file may have a new run after it.
// The seek is then done again. It walks the bucket map from the data bucket
// when the position lies at or after its start, and from the first bucket of
// the file otherwise (see FsSeekInFile). The caller makes sure the file has a
// bucket for the position: a read checks the size of the file, and a write
// calls MfsEnsureRecordSpace first. If the seek still finds no bucket, the
// entry and the bucket map do not agree. OS_EDEVFAULT is returned then, so
// nothing is read or written outside the file.
static oserr_t
__SeekToBucketOfPosition(
        _In_ FileSystemMFS_t* mfs,
        _In_ MFSEntry_t*      entry,
        _In_ uint64_t         position)
{
    oserr_t oserr;

    if (__BucketHoldsPosition(mfs, entry, position)) {
        return OS_EOK;
    }

    oserr = FsSeekInFile(mfs, entry, position);
    if (oserr != OS_EOK) {
        ERROR("__SeekToBucketOfPosition failed to seek to %u in %ms: %u", LODWORD(position), entry->Name, oserr);
        return oserr;
    }

    if (!__BucketHoldsPosition(mfs, entry, position)) {
        ERROR("__SeekToBucketOfPosition position %u lies past the buckets of %ms", LODWORD(position), entry->Name);
        return OS_EDEVFAULT;
    }
    return OS_EOK;
}

// Writes zeros from position up to end, but only within the data bucket of
// cursor, and returns the number of bytes cleared in bytesZeroedOut. A sector
// that also holds bytes before position or after end is read first, and only
// the bytes of the range are cleared in it. Whole sectors are written from a
// cleared transfer buffer, as many at a time as the bucket and the transfer
// buffer allow.
static oserr_t
__ZeroInBucket(
        _In_  FileSystemMFS_t* mfs,
        _In_  MFSEntry_t*      cursor,
        _In_  uint64_t         position,
        _In_  uint64_t         end,
        _Out_ size_t*          bytesZeroedOut)
{
    uint8_t* transferBuffer = (uint8_t*)SHMBuffer(&mfs->TransferBuffer);
    size_t   bufferSectors  = SHMBufferLength(&mfs->TransferBuffer) / mfs->SectorSize;
    size_t   sectorOffset   = (size_t)((position - cursor->BucketByteBoundary) % mfs->SectorSize);
    size_t   sectorIndex    = (size_t)((position - cursor->BucketByteBoundary) / mfs->SectorSize);
    size_t   sectorsLeft    = MFS_SECTORCOUNT(mfs, cursor->DataBucketLength) - sectorIndex;
    uint64_t sector         = MFS_GETSECTOR(mfs, cursor->DataBucketPosition) + sectorIndex;
    size_t   sectorCount;
    size_t   byteCount;
    size_t   sectorsTransferred;
    oserr_t  oserr;

    // The caller steps to the next bucket at the end of this one. A position
    // past the end would write outside the bucket, and one at the end would
    // clear nothing and never finish.
    if (sectorIndex >= MFS_SECTORCOUNT(mfs, cursor->DataBucketLength)) {
        ERROR("__ZeroInBucket position %u is not inside bucket %u", LODWORD(position), cursor->DataBucketPosition);
        return OS_EDEVFAULT;
    }

    if (sectorOffset != 0 || (end - position) < mfs->SectorSize) {
        sectorCount = 1;
        byteCount   = (size_t)MIN(end - position, (uint64_t)(mfs->SectorSize - sectorOffset));
        oserr = FSStorageRead(
                &mfs->Storage,
                mfs->TransferBuffer.ID,
                0,
                &(UInteger64_t) { .QuadPart = sector },
                sectorCount,
                &sectorsTransferred
        );
        if (oserr != OS_EOK) {
            ERROR("__ZeroInBucket failed to read sector %u: %u", LODWORD(sector), oserr);
            return oserr;
        }
        memset(transferBuffer + sectorOffset, 0, byteCount);
    } else {
        sectorCount = (size_t)((end - position) / mfs->SectorSize);
        sectorCount = MIN(sectorCount, sectorsLeft);
        sectorCount = MIN(sectorCount, bufferSectors);
        byteCount   = sectorCount * mfs->SectorSize;
        memset(transferBuffer, 0, byteCount);
    }

    oserr = FSStorageWrite(
            &mfs->Storage,
            mfs->TransferBuffer.ID,
            0,
            &(UInteger64_t) { .QuadPart = sector },
            sectorCount,
            &sectorsTransferred
    );
    if (oserr != OS_EOK) {
        ERROR("__ZeroInBucket failed to write %u sectors at sector %u: %u",
              LODWORD(sectorCount), LODWORD(sector), oserr);
        return oserr;
    }
    if (sectorsTransferred != sectorCount) {
        ERROR("__ZeroInBucket wrote %u of %u sectors at sector %u",
              LODWORD(sectorsTransferred), LODWORD(sectorCount), LODWORD(sector));
        return OS_EINCOMPLETE;
    }

    *bytesZeroedOut = byteCount;
    return OS_EOK;
}

// Writes zeros to the bytes of the file from start up to end.
//
// The rule: every byte below the size of the file (ActualSize) holds data
// that was written to the file, or zero. FsReadFromFile stops at the size, so
// the bytes past it are never returned. They may still hold data of a file
// that used the buckets before: new buckets are not cleared when they are
// allocated, and a write of part of a sector leaves the rest of the sector as
// it was. So when the size grows past bytes that were not written, those
// bytes are cleared here first:
// - FsWriteToFile, for a write that starts past the size. The range is from
//   the size to the start of the write.
// - FsChangeFileSize, when it makes the file larger. The range is from the
//   old size to the new size.
// Clearing only these ranges costs nothing for a write at the end of the
// file, which is the common case. Clearing every new bucket would write each
// appended byte twice.
//
// The caller has made sure the file has buckets up to end. A copy of the entry
// walks through the file, so the position and the data bucket of the handle
// do not change.
static oserr_t
__ZeroFileRange(
        _In_ FileSystemMFS_t* mfs,
        _In_ MFSEntry_t*      entry,
        _In_ uint64_t         start,
        _In_ uint64_t         end)
{
    size_t     bucketSizeBytes = mfs->SectorsPerBucket * mfs->SectorSize;
    uint64_t   position = start;
    MFSEntry_t cursor;
    size_t     bytesZeroed;
    oserr_t    oserr;

    TRACE("__ZeroFileRange(name=%ms, start=%u, end=%u)", entry->Name, LODWORD(start), LODWORD(end));

    memcpy(&cursor, entry, sizeof(MFSEntry_t));
    oserr = __SeekToBucketOfPosition(mfs, &cursor, start);
    if (oserr != OS_EOK) {
        ERROR("__ZeroFileRange failed to find the bucket of position %u: %u", LODWORD(start), oserr);
        return oserr;
    }

    while (position < end) {
        // Step to the next run of buckets at the end of the current one
        if (position == (cursor.BucketByteBoundary + (cursor.DataBucketLength * bucketSizeBytes))) {
            oserr = MFSAdvanceToNextBucket(mfs, &cursor);
            if (oserr != OS_EOK) {
                ERROR("__ZeroFileRange failed to get the bucket after %u: %u", cursor.DataBucketPosition, oserr);
                return oserr;
            }
        }

        oserr = __ZeroInBucket(mfs, &cursor, position, end, &bytesZeroed);
        if (oserr != OS_EOK) {
            ERROR("__ZeroFileRange failed to clear position %u of %ms: %u", LODWORD(position), entry->Name, oserr);
            return oserr;
        }
        position += bytesZeroed;
    }
    return OS_EOK;
}

oserr_t
FsReadFromFile(
        _In_  FileSystemMFS_t* mfs,
        _In_  MFSEntry_t*      entry,
        _In_  uuid_t           bufferHandle,
        _In_  void*            buffer,
        _In_  size_t           bufferOffset,
        _In_  size_t           unitCount,
        _Out_ size_t*          unitsRead)
{
    oserr_t  oserr    = OS_EOK;
    uint64_t position = entry->Position;
    size_t   bucketSizeBytes = mfs->SectorsPerBucket * mfs->SectorSize;
    size_t   bytesToRead     = unitCount;

    TRACE("FsReadFromFile(name=%ms, position=%u, length=%u)",
          entry->Name, LODWORD(entry->Position), LODWORD(unitCount));

    // Zero this first to indicate no bytes read
    *unitsRead = 0;

    // Sanitize the amount of bytes we want to read, cap it at bytes available
    if ((position + bytesToRead) > entry->ActualSize) {
        if (position >= entry->ActualSize) {
            return OS_EOK;
        }
        bytesToRead = (size_t)(entry->ActualSize - position);
    }

    if (entry->DataBucketPosition == MFS_ENDOFCHAIN) {
        // No buckets allocated for this file. Guard against this as there are two cases;
        // Case 1 - Newly created file, return OS_EOK;
        if (entry->ActualSize == 0) {
            return OS_EOK;
        } else {
            // Read from integrated data
            // TODO
        }
    }

    // The position is below the size of the file here, so the file has a
    // bucket for it. The data bucket of the handle may still be behind, for
    // example after a truncate gave a file without buckets its first ones.
    oserr = __SeekToBucketOfPosition(mfs, entry, position);
    if (oserr != OS_EOK) {
        ERROR("FsReadFromFile failed to find the bucket of position %u: %u", LODWORD(position), oserr);
        return oserr;
    }

    // Read the current sector, update index to where data starts
    // Keep reading consecutive after that untill all bytes requested have
    // been read
    while (bytesToRead) {
        // Calculate which bucket, then the sector offset
        // Then calculate how many sectors of the bucket we need to read
        uint64_t Sector       = MFS_GETSECTOR(mfs, entry->DataBucketPosition);    // Start-sector of current bucket
        uint64_t SectorOffset = position % mfs->SectorSize; // Byte-offset into the current sector
        size_t   SectorIndex  = (size_t)((position - entry->BucketByteBoundary) / mfs->SectorSize); // The sector-index into the current bucket
        size_t   SectorsLeft  = MFS_SECTORCOUNT(mfs, entry->DataBucketLength) - SectorIndex; // How many sectors are left in this bucket
        size_t   SectorCount;
        size_t   SectorsRead;
        size_t   ByteCount;
        
        // The buffer handle + offset that was selected for reading 
        uuid_t SelectedHandle = mfs->TransferBuffer.ID;
        size_t SelectedOffset = 0;
        
        // Calculate the sector index into bucket
        Sector += SectorIndex;

        // CASE 1: DIRECT READING INTO USER BUFFER
        // <Sector> now contains where we should start reading, and SectorOffset is
        // the byte offset into that first sector. This means if we request any number of bytes
        // we should also have room for that. So to read directly into user provided buffer, we MUST
        // ensure that <SectorOffset> is 0 and that we can read atleast one entire sector to avoid
        // any form for discarding of data.
        if (SectorOffset == 0 && bytesToRead >= mfs->SectorSize) {
            SectorCount    = bytesToRead / mfs->SectorSize;
            SelectedHandle = bufferHandle;
            SelectedOffset = bufferOffset;
        }
        
        // CASE 2: SINGLE READ INTO INTERMEDIATE BUFFER
        // We want this to happen when we can fit the entire read into our fs transfer buffer
        // that can act as an intermediate buffer. So calculate enough space for <BytesToRead>, with
        // room for <SectorOffset> and also the spill-over bytes thats left for the sector
        else if ((SectorOffset + bytesToRead + (mfs->SectorSize -
                                                ((SectorOffset + bytesToRead) % mfs->SectorSize))) <=
                SHMBufferLength(&mfs->TransferBuffer)) {
            SectorCount = DIVUP(bytesToRead, mfs->SectorSize);
            if (SectorOffset != 0 && (SectorOffset + bytesToRead > mfs->SectorSize)) {
                SectorCount++; // Take into account the extra sector we have to read
            }
        }

        // CASE 3: SINGLE READ INTO INTERMEDIATE BUFFER TO CORRECTLY ALIGN FOR CASE 1
        // Make sure we make a single read to align the file position to a sector offset
        // so we can directly read into the buffer
        // CASE 4: SINGLE READ INTO INTERMEDIATE BUFFER TO FINISH TRANSFER AFTER CASE 1
        // Read a single sector into the buffer to copy the remaining bytes
        else {
            // Just read a single sector, the ByteCount adjustor will automatically
            // adjust the number of bytes to read in this iteration
            SectorCount = 1;
        }

        // Adjust for bucket boundary
        SectorCount = MIN(SectorCount, SectorsLeft);
        if (SectorCount != 0) {
            // Adjust for number of bytes already consumed in the active sector
            ByteCount = MIN(bytesToRead, (SectorCount * mfs->SectorSize) - SectorOffset);
    
            // Ex pos 490 - length 50
            // SectorIndex = 0, SectorOffset = 490, SectorCount = 2 - ByteCount = 50 (Capacity 4096)
            // Ex pos 1109 - length 450
            // SectorIndex = 2, SectorOffset = 85, SectorCount = 2 - ByteCount = 450 (Capacity 4096)
            // Ex pos 490 - length 4000
            // SectorIndex = 0, SectorOffset = 490, SectorCount = 8 - ByteCount = 3606 (Capacity 4096)
            TRACE(" > sector %u (b-start %u, b-index %u), num-sectors %u, sector-byte-offset %u, bytecount %u",
                LODWORD(Sector), LODWORD(Sector) - SectorIndex, SectorIndex, SectorCount, LODWORD(SectorOffset), ByteCount);

            oserr = FSStorageRead(
                    &mfs->Storage,
                    SelectedHandle,
                    SelectedOffset,
                    &(UInteger64_t) { .QuadPart = Sector },
                    SectorCount,
                    &SectorsRead
            );
            if (oserr != OS_EOK) {
                ERROR("Failed to read sector");
                break;
            }
            
            // Adjust for how many sectors we actually read
            if (SectorCount != SectorsRead) {
                ByteCount = (mfs->SectorSize * SectorsRead) - SectorOffset;
            }
            
            // If we used the intermediate buffer for the transfer we now have to copy
            // <ByteCount> amount of bytes from <TransferBuffer> + <SectorOffset> to <Buffer> + <BufferOffset>
            if (SelectedHandle == mfs->TransferBuffer.ID) {
                memcpy(((uint8_t*)buffer + bufferOffset), ((uint8_t*)SHMBuffer(&mfs->TransferBuffer) + SectorOffset), ByteCount);
            }
            
            // Increament all read-state variables
            *unitsRead   += ByteCount;
            bufferOffset += ByteCount;
            position     += ByteCount;
            bytesToRead  -= ByteCount;
        }

        // Do we need to switch bucket?
        // We do if the position we have read to equals end of bucket
        if (position == (entry->BucketByteBoundary + (entry->DataBucketLength * bucketSizeBytes))) {
            oserr = MFSAdvanceToNextBucket(mfs, entry);
            if (oserr != OS_EOK) {
                if (oserr == OS_ENOENT) {
                    oserr = OS_EOK;
                }
                break;
            }
        }
    }

    // Store the new position we've calculated during the read operation.
    entry->Position = position;

    TRACE("[mfs] [read_file] bytes read %u/%u", *unitsRead, unitCount);
    return oserr;
}

oserr_t
FsWriteToFile(
        _In_  FileSystemMFS_t* mfs,
        _In_  MFSEntry_t*      entry,
        _In_  uuid_t           bufferHandle,
        _In_  void*            buffer,
        _In_  size_t           bufferOffset,
        _In_  size_t           unitCount,
        _Out_ size_t*          unitsWritten)
{
    uint64_t position        = entry->Position;
    size_t   bucketSizeBytes = mfs->SectorsPerBucket * mfs->SectorSize;
    size_t   bytesToWrite    = unitCount;
    size_t   accumOffset     = bufferOffset;
    oserr_t  oserr;

    TRACE("FsWriteToFile(name=%ms, position=%u, length=%u)",
          entry->Name, LODWORD(entry->Position), LODWORD(unitCount));

    // Set 0 to start out with, in case of errors we want to indicate correctly.
    *unitsWritten = 0;
    
    // We do not have the same boundary limits here as we do when reading, when we
    // write to a file we can do so untill we run out of space on the filesystem.
    oserr = MfsEnsureRecordSpace(mfs, entry, position + bytesToWrite);
    if (oserr != OS_EOK) {
        ERROR("FsWriteToFile: failed to ensure record space for write: %u", oserr);
        return oserr;
    }

    // A write that starts past the end of the file leaves the bytes between
    // the end and the position unwritten. They are cleared before the size
    // grows over them (see __ZeroFileRange).
    if (position > entry->ActualSize) {
        oserr = __ZeroFileRange(mfs, entry, entry->ActualSize, position);
        if (oserr != OS_EOK) {
            ERROR("FsWriteToFile: failed to clear the bytes from %u to %u: %u",
                  LODWORD(entry->ActualSize), LODWORD(position), oserr);
            return oserr;
        }
    }

    // MfsEnsureRecordSpace may just have given the file the bucket of the
    // position, while the data bucket of the handle is MFS_ENDOFCHAIN or the
    // last bucket a seek past the end stopped at (see
    // __SeekToBucketOfPosition). A write of 0 bytes needs no bucket.
    if (bytesToWrite != 0) {
        oserr = __SeekToBucketOfPosition(mfs, entry, position);
        if (oserr != OS_EOK) {
            ERROR("FsWriteToFile: failed to find the bucket of position %u: %u", LODWORD(position), oserr);
            return oserr;
        }
    }

    // Write in a loop to make sure we write all requested bytes
    while (bytesToWrite) {
        // Determine whether we need to switch bucket as the first step of writing. Do
        // this to avoid switching buckets when writing the last byte of a file. There will
        // always be enough space to write because of Ensure.
        TRACE("FsWriteToFile: position=0x%llx, boundary at 0x%llx",
              position, (entry->BucketByteBoundary + (entry->DataBucketLength * bucketSizeBytes)));
        if (position == (entry->BucketByteBoundary + (entry->DataBucketLength * bucketSizeBytes))) {
            oserr = MFSAdvanceToNextBucket(mfs, entry);
            if (oserr != OS_EOK) {
                ERROR("FsWriteToFile: failed to get next data bucket: %u", oserr);
                break;
            }
        }

        // Calculate which bucket, then the sector offset
        // Then calculate how many sectors of the bucket we need to read
        uint64_t bucketSector       = MFS_GETSECTOR(mfs, entry->DataBucketPosition);
        uint64_t bucketSectorOffset = (position - entry->BucketByteBoundary) % mfs->SectorSize;
        size_t   sectorIndex        = (size_t)((position - entry->BucketByteBoundary) / mfs->SectorSize);
        size_t   sectorsLeft        = MFS_SECTORCOUNT(mfs, entry->DataBucketLength) - sectorIndex;
        size_t   sectorCount;
        size_t   sectorsWritten;
        size_t   byteCount;

        TRACE("FsWriteToFile: position=0x%llx, entry->BucketByteBoundary=0x%llx",
              position, entry->BucketByteBoundary);
        TRACE("FsWriteToFile: bucketSector=0x%llx, sectorIndex=0x%" PRIxIN ", sectorOffset=0x%llx",
              bucketSector, bucketSectorOffset, sectorIndex);

        // The buffer handle + offset that was selected for writing 
        uuid_t SelectedHandle = mfs->TransferBuffer.ID;
        size_t SelectedOffset = 0;

        // Calculate the sector index into bucket
        bucketSector += sectorIndex;
        
        // CASE 1: WE CAN WRITE DIRECTLY FROM USER-BUFFER TO DISK
        // If <SectorOffset> is 0, this means we can write directly to the disk
        // from <Buffer> + <accumOffset>. We must also be able to write an entire
        // sector to avoid writing out of bounds from the buffer
        if (bucketSectorOffset == 0 && bytesToWrite >= mfs->SectorSize) {
            sectorCount    = bytesToWrite / mfs->SectorSize;
            SelectedHandle = bufferHandle;
            SelectedOffset = accumOffset;
            TRACE("FsWriteToFile: [case 1] direct transfer %" PRIuIN " bytes from user-buffer",
                  sectorCount * mfs->SectorSize);
        }
        
        // CASE 2: SECTOR-ALIGN BY READING-WRITE ONE SECTOR FIRST FOR CASE 1
        // If we are not aligned to a sector boundary in the file, we should start
        // out by writing the rest of the sector with a read-write combine. This
        // must happen by using the intermediate buffer
        // CASE 3: FINAL SECTOR WRITE BY USING INTERMEDIATE BUFFER
        else {
            TRACE("FsWriteToFile: [case 2]");
            sectorCount = 1;
        }

        // Adjust for bucket boundary
        sectorCount = MIN(sectorsLeft, sectorCount);
        TRACE("FsWriteToFile: sectors (adjusted)=0x%" PRIuIN, sectorCount);
        if (sectorCount != 0) {
            byteCount = MIN(bytesToWrite, (sectorCount * mfs->SectorSize) - bucketSectorOffset);
    
            // Ex pos 490 - length 50
            // SectorIndex = 0, SectorOffset = 490, SectorCount = 2 - ByteCount = 50 (Capacity 4096)
            // Ex pos 1109 - length 450
            // SectorIndex = 2, SectorOffset = 85, SectorCount = 2 - ByteCount = 450 (Capacity 4096)
            // Ex pos 490 - length 4000
            // SectorIndex = 0, SectorOffset = 490, SectorCount = 8 - ByteCount = 3606 (Capacity 4096)
            TRACE("FsWriteToFile: bytes to write (adjusted)=%" PRIuIN, byteCount);
    
            // First of all, calculate the bounds as we might need to read
            // in existing data - Start out by clearing our combination buffer
            if (SelectedHandle == mfs->TransferBuffer.ID) {
                TRACE("FsWriteToFile: write-combine");
                memset(
                        SHMBuffer(&mfs->TransferBuffer),
                        0,
                        SHMBufferLength(&mfs->TransferBuffer)
                );
                
                // CASE READ-WRITE: We do this to support appending to sectors and overwriting
                // bytes in a sector. This must occur when we either have a <SectorOffset> != 0
                // or when the <SectorOffset> == 0 and <ByteCount> is less than a sector.
                TRACE("FsWriteToFile: read sector=0x%llx, count=0x%" PRIuIN, bucketSector, sectorCount);
                oserr = FSStorageRead(
                        &mfs->Storage,
                        mfs->TransferBuffer.ID,
                        0,
                        &(UInteger64_t) { .QuadPart = bucketSector },
                        sectorCount,
                        &sectorsWritten
                );
                if (oserr != OS_EOK) {
                    ERROR("Failed to read sector %u for combination step", 
                        LODWORD(bucketSector));
                    break;
                }
                
                // Now perform the user copy operation where we overwrite some of the data
                // Copy from <Buffer> + <accumOffset> to <TransferBuffer> + <SectorOffset>
                TRACE("FsWriteToFile: copying 0x%" PRIuIN " bytes at offset=0x%" PRIuIN " into read buffer",
                      byteCount, bucketSectorOffset);
                memcpy(
                        ((uint8_t*)SHMBuffer(&mfs->TransferBuffer) + bucketSectorOffset),
                        ((uint8_t*)buffer + accumOffset),
                        byteCount
                );
            }
            
            // Write either the intermediate buffer or directly from user
            TRACE("FsWriteToFile: write sector=0x%llx, count=0x%" PRIuIN, bucketSector, sectorCount);
            oserr = FSStorageWrite(
                    &mfs->Storage,
                    SelectedHandle,
                    SelectedOffset,
                    &(UInteger64_t) { .QuadPart = bucketSector },
                    sectorCount,
                    &sectorsWritten
            );
            if (oserr != OS_EOK) {
                ERROR("Failed to write sector %u", LODWORD(bucketSector));
                break;
            }
            
            // Adjust for how many sectors we actually read
            if (sectorCount != sectorsWritten) {
                byteCount = (mfs->SectorSize * sectorsWritten) - bucketSectorOffset;
            }

            TRACE("FsWriteToFile: written %" PRIuIN " bytes", byteCount);
            *unitsWritten += byteCount;
            accumOffset   += byteCount;
            position      += byteCount;
            bytesToWrite  -= byteCount;
        }
    }

    // Store the new position we've calculated during the read operation.
    entry->Position = position;
    if (entry->Position > entry->ActualSize) {
        entry->ActualSize = entry->Position;
        entry->ActionOnClose = MFS_ACTION_UPDATE;
    }
    return oserr;
}

oserr_t
FsSeekInFile(
        _In_ FileSystemMFS_t* mfs,
        _In_ MFSEntry_t*      entry,
        _In_ uint64_t         absolutePosition)
{
    size_t      bucketSizeBytes = mfs->SectorsPerBucket * mfs->SectorSize;
    bucket_t    bucket;
    uint64_t    runStart;
    uint64_t    runEnd;
    MapRecord_t run;
    oserr_t     oserr;

    TRACE("FsSeekInFile(name=%ms, position=0x%x)", entry->Name, LODWORD(absolutePosition));

    // A file without buckets (StartBucket is MFS_ENDOFCHAIN) has no bucket the
    // position can point into. Any position is accepted and stored, also one
    // past 0, and the data bucket is set to MFS_ENDOFCHAIN:
    // - FsReadFromFile returns 0 bytes, as the position is at or past the end
    //   of the file.
    // - FsWriteToFile has MfsEnsureRecordSpace allocate buckets up to the
    //   position plus the bytes to write. It sees the MFS_ENDOFCHAIN data
    //   bucket and seeks again, which then finds the bucket of the position.
    //   The bytes before the position are cleared (see __ZeroFileRange).
    if (entry->StartBucket == MFS_ENDOFCHAIN) {
        entry->DataBucketPosition = MFS_ENDOFCHAIN;
        entry->DataBucketLength   = 0;
        entry->BucketByteBoundary = 0;
        entry->Position           = absolutePosition;
        return OS_EOK;
    }

    // Where the walk starts. The runs of the chain only link forward, so a
    // position before the data bucket is found by walking from the first
    // bucket. A position at or after the start of the data bucket is found by
    // walking on from the data bucket. This makes the common cases cheap: a
    // read or write that goes on where the last one ended, and a write at the
    // end of the file, which __SeekToBucketOfPosition sends here once the file
    // has a new run. Both cost one or two map lookups, however long the file is.
    if (entry->DataBucketPosition != MFS_ENDOFCHAIN && absolutePosition >= entry->BucketByteBoundary) {
        bucket   = entry->DataBucketPosition;
        runStart = entry->BucketByteBoundary;
    } else {
        bucket   = entry->StartBucket;
        runStart = 0;
    }

    // The length of each run, the first one too, is read from the bucket map.
    // The handle's copies may be shorter than the run is now (see MFSEntry_t
    // in mfs.h). The walk stops at the run that holds the position, or at the
    // last run when the position lies past the end of the file.
    while (1) {
        oserr = MFSBucketMapGetLengthAndLink(mfs, bucket, &run);
        if (oserr != OS_EOK) {
            ERROR("FsSeekInFile failed to get the run of bucket %u in %ms: %u", bucket, entry->Name, oserr);
            return OS_EDEVFAULT;
        }

        runEnd = runStart + ((uint64_t)run.Length * bucketSizeBytes);
        if (absolutePosition < runEnd) {
            break;
        }
        if (run.Link == MFS_ENDOFCHAIN) {
            WARNING("FsSeekInFile position %u lies past the last run of %ms, which ends at %u",
                    LODWORD(absolutePosition), entry->Name, LODWORD(runEnd));
            break;
        }
        bucket   = run.Link;
        runStart = runEnd;
    }

    MFSEntrySetDataRun(entry, bucket, run.Length, runStart);
    entry->Position = absolutePosition;
    return OS_EOK;
}

// Sets the size of the file to 0 and gives its buckets back to the free list.
// The record is written first, and the buckets are freed after it, as in
// FsUnlink. A bucket must never be on the free list while a record still
// points at it.
// - If the record write fails, entry gets its old values back. The file keeps
//   its buckets and its size, on the disk and in the handle, and the error is
//   returned.
// - If freeing the buckets fails, the record no longer points at them. They
//   are lost (their space cannot be used again). This is logged, and OS_EOK
//   is returned, as the file is empty as asked.
// The position of the handle does not change. filed keeps its own copy of it
// and does not seek before each write. The data bucket becomes
// MFS_ENDOFCHAIN, so the handle never points at the freed buckets. The next
// write gives the file new buckets and finds the bucket of the position in
// them, and the bytes before the position read as zero.
static oserr_t
__TruncateToZero(
        _In_ FileSystemMFS_t* mfs,
        _In_ MFSEntry_t*      entry)
{
    MFSEntry_t previous;
    oserr_t    oserr;

    memcpy(&previous, entry, sizeof(MFSEntry_t));
    entry->AllocatedSize = 0;
    entry->ActualSize    = 0;
    entry->StartBucket   = MFS_ENDOFCHAIN;
    entry->StartLength   = 0;

    oserr = MfsUpdateRecord(mfs, entry, MFS_ACTION_UPDATE);
    if (oserr != OS_EOK) {
        ERROR("FsChangeFileSize failed to write the record of %ms with size 0: %u", entry->Name, oserr);
        memcpy(entry, &previous, sizeof(MFSEntry_t));
        return oserr;
    }

    entry->DataBucketPosition = MFS_ENDOFCHAIN;
    entry->DataBucketLength   = 0;
    entry->BucketByteBoundary = 0;
    entry->ActionOnClose      = MFS_ACTION_UPDATE;

    // A file that was never written has no buckets to free
    if (previous.StartBucket == MFS_ENDOFCHAIN) {
        return OS_EOK;
    }

    oserr = MfsFreeBuckets(mfs, previous.StartBucket, previous.StartLength);
    if (oserr != OS_EOK) {
        ERROR("FsChangeFileSize emptied %ms, but failed to free its buckets at %u (length %u): %u. "
              "The buckets are lost",
              entry->Name, previous.StartBucket, previous.StartLength, oserr);
    }
    return OS_EOK;
}

// Called by FsTruncate in main.c, which holds mfs->Lock.
// A size of 0 frees every bucket (see __TruncateToZero). Any other size keeps
// every bucket of the file: MfsEnsureRecordSpace only adds buckets, so after
// a truncate to a smaller size the data bucket of the handle still points
// into the file. The bytes past the new size stay on the disk, and are
// cleared if the file grows over them again.
oserr_t
FsChangeFileSize(
        _In_ FileSystemMFS_t* mfs,
        _In_ MFSEntry_t*      entry,
        _In_ uint64_t         size)
{
    oserr_t oserr;
    TRACE("FsChangeFileSize(Name %ms, Size 0x%x)", entry->Name, LODWORD(size));

    if (size == 0) {
        return __TruncateToZero(mfs, entry);
    }

    oserr = MfsEnsureRecordSpace(mfs, entry, size);
    if (oserr != OS_EOK) {
        ERROR("FsChangeFileSize failed to get buckets for size %u of %ms: %u", LODWORD(size), entry->Name, oserr);
        return oserr;
    }

    // The bytes the file grows by were never written. They are cleared before
    // the size covers them (see __ZeroFileRange).
    if (size > entry->ActualSize) {
        oserr = __ZeroFileRange(mfs, entry, entry->ActualSize, size);
        if (oserr != OS_EOK) {
            ERROR("FsChangeFileSize failed to clear the bytes from %u to %u of %ms: %u",
                  LODWORD(entry->ActualSize), LODWORD(size), entry->Name, oserr);
            return oserr;
        }
    }

    // entry->modified = now
    entry->ActualSize    = size;
    entry->ActionOnClose = MFS_ACTION_UPDATE;
    return OS_EOK;
}
