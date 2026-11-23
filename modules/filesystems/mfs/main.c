/**
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
 */

// The trace lines of this file follow the file operations: mount, open,
// create, close, unlink and move. The boot test runners under testing/ look
// for some of them. FsReadEntry and FsWriteEntry log no trace line. On a
// serial line one trace line takes longer than a small request itself, so a
// line per request would slow every read and write down.
#define __TRACE
#define CACHE_SEGMENTED
#define __need_minmax
#include <ddk/utils.h>
#include <fs/common.h>
#include <os/types/file.h>
#include <os/handle.h>
#include <os/shm.h>
#include <os/memory.h>
#include <stdlib.h>
#include <string.h>
#include "mfs.h"

static const char* RootEntryName = "<root>";

// File specific operation handlers
oserr_t FsReadFromFile(FileSystemMFS_t*, MFSEntry_t*, uuid_t, void*, size_t, size_t, size_t*);
oserr_t FsWriteToFile(FileSystemMFS_t*, MFSEntry_t*, uuid_t, void*, size_t, size_t, size_t*);
oserr_t FsSeekInFile(FileSystemMFS_t*, MFSEntry_t*, uint64_t);

oserr_t FsChangeFileSize(FileSystemMFS_t*, MFSEntry_t*, uint64_t);

// Directory specific operation handlers
oserr_t FsReadFromDirectory(FileSystemMFS_t*, MFSEntry_t*, void*, size_t, size_t, size_t*);
oserr_t FsSeekInDirectory(FileSystemMFS_t*, MFSEntry_t*, uint64_t);

// Locking: each Fs* function that filed calls on a mounted filesystem takes
// mfs->Lock, calls a static function that does the work and releases the lock
// again (see FileSystemMFS_t in mfs.h). The static functions never take the
// lock, so they can call each other. A Fs* function never calls another Fs*
// function, as the lock is not recursive and the job would wait for itself.

static void MFSEntryDelete(MFSEntry_t* entry)
{
    if (entry == NULL) {
        return;
    }

    mstr_delete(entry->Name);
    free(entry);
}

static oserr_t
__OpenPath(
        _In_      FileSystemMFS_t* mfs,
        _In_      mstring_t*       path,
        _Out_Opt_ void**           dataOut)
{
    oserr_t     osStatus;
    MFSEntry_t* mfsEntry;
    TRACE("FsOpen=(path=%ms)", path);

    osStatus = MfsLocateRecord(
            mfs,
            &mfs->RootEntry,
            path,
            &mfsEntry
    );
    if (osStatus != OS_EOK) {
        return osStatus;
    }

    *dataOut = mfsEntry;
    return osStatus;
}

oserr_t
FsOpen(
        _In_      void*      instanceData,
        _In_      mstring_t* path,
        _Out_Opt_ void**     dataOut)
{
    FileSystemMFS_t* mfs = instanceData;
    oserr_t          oserr;

    usched_mtx_lock(&mfs->Lock);
    oserr = __OpenPath(mfs, path, dataOut);
    usched_mtx_unlock(&mfs->Lock);
    return oserr;
}

static oserr_t
__CreateEntry(
        _In_  FileSystemMFS_t* mfs,
        _In_  MFSEntry_t*      entry,
        _In_  mstring_t*       name,
        _In_  uint32_t         owner,
        _In_  uint32_t         flags,
        _In_  uint32_t         permissions,
        _Out_ void**           dataOut)
{
    oserr_t     osStatus;
    MFSEntry_t* result;
    TRACE("FsCreate(parent=%ms, name=%ms)", entry ? entry->Name : NULL, name);

    osStatus = MfsCreateRecord(
            mfs,
            entry,
            name,
            owner,
            flags,
            permissions,
            0,
            0,
            MFS_ENDOFCHAIN,
            0,
            &result
    );
    if (osStatus != OS_EOK) {
        return osStatus;
    }
    TRACE("FsCreate returned %u (0x%llx) %ms", osStatus, result, result->Name);
    *dataOut = result;
    return osStatus;
}

oserr_t
FsCreate(
        _In_  void*      instanceData,
        _In_  void*      data,
        _In_  mstring_t* name,
        _In_  uint32_t   owner,
        _In_  uint32_t   flags,
        _In_  uint32_t   permissions,
        _Out_ void**     dataOut)
{
    FileSystemMFS_t* mfs = instanceData;
    oserr_t          oserr;

    usched_mtx_lock(&mfs->Lock);
    oserr = __CreateEntry(
            mfs,
            (MFSEntry_t*)data,
            name,
            owner,
            flags,
            permissions,
            dataOut
    );
    usched_mtx_unlock(&mfs->Lock);
    return oserr;
}

// Writes the record of the entry when it changed, and frees the entry. The
// entry is freed also when the write fails.
static oserr_t
__CloseEntry(
        _In_ FileSystemMFS_t* mfs,
        _In_ MFSEntry_t*      entry)
{
    oserr_t osStatus = OS_EOK;
    TRACE("FsClose(%i)", entry->ActionOnClose);

    if (entry->ActionOnClose) {
        osStatus = MfsUpdateRecord(mfs, entry, entry->ActionOnClose);
    }
    mstr_delete(entry->Name);
    free(entry);
    return osStatus;
}

oserr_t
FsClose(
        _In_ void* instanceData,
        _In_ void* data)
{
    FileSystemMFS_t* mfs = instanceData;
    oserr_t          oserr;

    usched_mtx_lock(&mfs->Lock);
    oserr = __CloseEntry(mfs, (MFSEntry_t*)data);
    usched_mtx_unlock(&mfs->Lock);
    return oserr;
}

static oserr_t
__StatFileSystem(
        _In_ FileSystemMFS_t*  mfs,
        _In_ struct VFSStatFS* stat)
{
    stat->Label = mfs->Label;
    stat->MaxFilenameLength = MFS_FILERECORD_MAX_NAME;
    stat->BlockSize = mfs->SectorSize;
    stat->BlocksPerSegment = mfs->SectorsPerBucket;
    stat->SegmentsTotal = mfs->BucketsInMap;
    stat->SegmentsFree = mfs->MasterRecord.FreeBucket; // TODO we don't know how many free
    return OS_EOK;
}

// Label is not copied. It stays the same while the filesystem is mounted, so
// the caller may still read it after the lock is released.
oserr_t
FsStat(
        _In_ void*             instanceData,
        _In_ struct VFSStatFS* stat)
{
    FileSystemMFS_t* mfs = instanceData;
    oserr_t          oserr;

    usched_mtx_lock(&mfs->Lock);
    oserr = __StatFileSystem(mfs, stat);
    usched_mtx_unlock(&mfs->Lock);
    return oserr;
}

static oserr_t
__UnlinkPath(
        _In_ FileSystemMFS_t* mfs,
        _In_ mstring_t*       path)
{
    oserr_t     oserr;
    oserr_t     freeStatus;
    MFSEntry_t* mfsEntry;
    TRACE("FsUnlink(%ms)", path);

    oserr = MfsLocateRecord(
            mfs,
            &mfs->RootEntry,
            path,
            &mfsEntry);
    if (oserr != OS_EOK) {
        return oserr;
    }

    // The record is removed first, and the buckets are freed after it. A
    // bucket must never be on the free list while a record still points at
    // it, as the next allocation would give it to a second file.
    // - If the record write fails, nothing has changed. The file is still
    //   there with all its data, and the error is returned.
    // - If freeing the buckets fails, the record is already gone and nothing
    //   points at the buckets any more. They are lost: their space cannot be
    //   used again. This is logged, and OS_EOK is returned, because the file
    //   is removed. filed only drops its node for the file on OS_EOK, and
    //   would otherwise keep a node for a file that no longer exists.
    oserr = MfsUpdateRecord(mfs, mfsEntry, MFS_ACTION_DELETE);
    if (oserr != OS_EOK) {
        ERROR("FsUnlink failed to remove the record of %ms: %u", path, oserr);
        goto cleanup;
    }

    // A file that was never written has no buckets (StartBucket is
    // MFS_ENDOFCHAIN), and MfsFreeBuckets refuses a length of 0. Only the
    // record is removed then.
    if (mfsEntry->StartBucket != MFS_ENDOFCHAIN) {
        freeStatus = MfsFreeBuckets(mfs, mfsEntry->StartBucket, mfsEntry->StartLength);
        if (freeStatus != OS_EOK) {
            ERROR("FsUnlink removed %ms, but failed to free its buckets at %u (length %u): %u. "
                  "The buckets are lost",
                  path, mfsEntry->StartBucket, mfsEntry->StartLength, freeStatus);
        }
    }

cleanup:
    MFSEntryDelete(mfsEntry);
    return oserr;
}

oserr_t
FsUnlink(
        _In_ void*      instanceData,
        _In_ mstring_t* path)
{
    FileSystemMFS_t* mfs = instanceData;
    oserr_t          oserr;

    usched_mtx_lock(&mfs->Lock);
    oserr = __UnlinkPath(mfs, path);
    usched_mtx_unlock(&mfs->Lock);
    return oserr;
}

static oserr_t
__CreateLink(
        _In_ FileSystemMFS_t* mfs,
        _In_ MFSEntry_t*      entry,
        _In_ mstring_t*       linkName,
        _In_ mstring_t*       linkTarget,
        _In_ int              symbolic)
{
    // TODO implement MFS::Link
    return OS_ENOTSUPPORTED;
}

// Links are not supported yet. The lock is still taken, so a later
// implementation that reads the disk is already covered by it.
oserr_t
FsLink(
        _In_ void*      instanceData,
        _In_ void*      data,
        _In_ mstring_t* linkName,
        _In_ mstring_t* linkTarget,
        _In_ int        symbolic)
{
    FileSystemMFS_t* mfs = instanceData;
    oserr_t          oserr;

    usched_mtx_lock(&mfs->Lock);
    oserr = __CreateLink(mfs, (MFSEntry_t*)data, linkName, linkTarget, symbolic);
    usched_mtx_unlock(&mfs->Lock);
    return oserr;
}

static oserr_t
__ReadLinkPath(
        _In_ FileSystemMFS_t* mfs,
        _In_ mstring_t*       path,
        _In_ mstring_t**      pathOut)
{
    // TODO implement MFS::ReadLink
    return OS_ENOTSUPPORTED;
}

// Links are not supported yet. The lock is taken for the same reason as in
// FsLink.
oserr_t
FsReadLink(
        _In_ void*       instanceData,
        _In_ mstring_t*  path,
        _In_ mstring_t** pathOut)
{
    FileSystemMFS_t* mfs = instanceData;
    oserr_t          oserr;

    usched_mtx_lock(&mfs->Lock);
    oserr = __ReadLinkPath(mfs, path, pathOut);
    usched_mtx_unlock(&mfs->Lock);
    return oserr;
}

// Looks up path and returns whether it exists. entryOut is set when it does.
// A missing path is OS_EOK with MFS_SEARCH_MISSING. Any other error is a
// failed disk read (or no memory), and is returned as it is.
static oserr_t
__FindPath(
        _In_  FileSystemMFS_t*      mfs,
        _In_  mstring_t*            path,
        _Out_ MFSEntry_t**          entryOut,
        _Out_ enum MFSSearchResult* searchResultOut)
{
    oserr_t oserr;

    *entryOut = NULL;
    oserr = MfsFindRecord(mfs, &mfs->RootEntry, path, entryOut, searchResultOut);
    if (oserr != OS_EOK) {
        ERROR("FsMove failed to look up %ms: %u", path, oserr);
    }
    return oserr;
}

// Sets insideOut when path is directory, or lies inside it. A directory
// cannot be moved into itself, because nothing would be left pointing at it.
// The walk goes up one directory at a time and ends at the root, or at a
// path whose directory name is the path itself.
static oserr_t
__PathIsInside(
        _In_  mstring_t* path,
        _In_  mstring_t* directory,
        _Out_ bool*      insideOut)
{
    mstring_t* current;
    mstring_t* parent;
    bool       done = false;

    *insideOut = false;
    current = mstr_clone(path);
    if (current == NULL) {
        return OS_EOOM;
    }

    while (!done) {
        if (mstr_icmp(current, directory) == 0) {
            *insideOut = true;
            break;
        }
        if (__PathIsRoot(current)) {
            break;
        }

        parent = mstr_path_dirname(current);
        if (parent == NULL) {
            mstr_delete(current);
            return OS_EOOM;
        }
        done = mstr_icmp(parent, current) == 0;
        mstr_delete(current);
        current = parent;
    }
    mstr_delete(current);
    return OS_EOK;
}

// The source and the target are in the same directory: the source record is
// renamed in place. This is one write, so after a failure the file still has
// its old name.
static oserr_t
__RenameInPlace(
        _In_ FileSystemMFS_t* mfs,
        _In_ MFSEntry_t*      source,
        _In_ mstring_t*       targetName)
{
    mstring_t* sourceName = source->Name;
    oserr_t    oserr;

    source->Name = targetName;
    oserr = MfsUpdateRecord(mfs, source, MFS_ACTION_RENAME);
    source->Name = sourceName;
    if (oserr != OS_EOK) {
        ERROR("FsMove failed to rename %ms to %ms: %u", sourceName, targetName, oserr);
    }
    return oserr;
}

// The target is in another directory. A record for the target name is
// created there first, pointing at the same data as the source. Only once it
// is written is the source record removed. The steps:
// 1. Create the target record. Before that write the target directory may
//    get new buckets (a bucket map, a master record and a zero write each).
//    If any of those fail, the source is untouched and only allocated
//    buckets may be lost.
// 2. Remove the source record. If that fails, the target record is removed
//    again, so the file keeps its old name. If that write fails as well,
//    both records point at the same data. That is logged, because removing
//    one of the two later frees data the other one still uses.
static oserr_t
__MoveToDirectory(
        _In_ FileSystemMFS_t* mfs,
        _In_ MFSEntry_t*      source,
        _In_ mstring_t*       to,
        _In_ mstring_t*       targetName)
{
    enum MFSSearchResult searchResult;
    MFSEntry_t*          directory = NULL;
    MFSEntry_t*          target = NULL;
    mstring_t*           directoryPath;
    oserr_t              oserr;
    oserr_t              undoStatus;

    directoryPath = mstr_path_dirname(to);
    if (directoryPath == NULL) {
        return OS_EOOM;
    }

    oserr = __FindPath(mfs, directoryPath, &directory, &searchResult);
    if (oserr != OS_EOK) {
        goto cleanup;
    }
    if (searchResult == MFS_SEARCH_MISSING) {
        oserr = OS_ENOENT;
        goto cleanup;
    }
    if (!(directory->NativeFlags & MFS_FILERECORD_DIRECTORY)) {
        oserr = OS_ENOTDIR;
        goto cleanup;
    }

    oserr = MfsCreateRecord(
            mfs,
            directory,
            targetName,
            source->Owner,
            source->Flags,
            source->Permissions,
            source->AllocatedSize,
            source->ActualSize,
            source->StartBucket,
            source->StartLength,
            &target
    );
    if (oserr != OS_EOK) {
        ERROR("FsMove failed to create the record of %ms: %u", to, oserr);
        goto cleanup;
    }

    oserr = MfsUpdateRecord(mfs, source, MFS_ACTION_DELETE);
    if (oserr != OS_EOK) {
        ERROR("FsMove failed to remove the record of %ms: %u", source->Name, oserr);
        undoStatus = MfsUpdateRecord(mfs, target, MFS_ACTION_DELETE);
        if (undoStatus != OS_EOK) {
            ERROR("FsMove failed to remove the new record of %ms: %u. %ms and %ms now share their data",
                  to, undoStatus, source->Name, to);
        }
    }

cleanup:
    MFSEntryDelete(target);
    MFSEntryDelete(directory);
    mstr_delete(directoryPath);
    return oserr;
}

static oserr_t
__MovePath(
        _In_ FileSystemMFS_t* mfs,
        _In_ mstring_t*       from,
        _In_ mstring_t*       to,
        _In_ int              copy)
{
    enum MFSSearchResult searchResult;
    MFSEntry_t*          source = NULL;
    MFSEntry_t*          existing = NULL;
    mstring_t*           fromDirectory = NULL;
    mstring_t*           toDirectory = NULL;
    mstring_t*           targetName = NULL;
    bool                 inside;
    oserr_t              oserr;
    TRACE("FsMove(from=%ms, to=%ms, copy=%i)", from, to, copy);

    // A copy needs the data copied to new buckets, and MFSCloneBucketData
    // cannot do that yet. Refuse it before anything is changed.
    if (copy) {
        return OS_ENOTSUPPORTED;
    }

    // The root has no record that could move, and nothing can replace it.
    if (__PathIsRoot(from)) {
        return OS_EPERMISSIONS;
    }
    if (__PathIsRoot(to)) {
        return OS_EEXISTS;
    }

    oserr = __FindPath(mfs, from, &source, &searchResult);
    if (oserr != OS_EOK) {
        goto cleanup;
    }
    if (searchResult == MFS_SEARCH_MISSING) {
        oserr = OS_ENOENT;
        goto cleanup;
    }

    // Names compare without case, so a target that differs from the source
    // only in case is found here as the source itself.
    oserr = __FindPath(mfs, to, &existing, &searchResult);
    if (oserr != OS_EOK) {
        goto cleanup;
    }
    if (searchResult == MFS_SEARCH_FOUND) {
        oserr = OS_EEXISTS;
        goto cleanup;
    }

    if (source->NativeFlags & MFS_FILERECORD_DIRECTORY) {
        oserr = __PathIsInside(to, from, &inside);
        if (oserr != OS_EOK) {
            goto cleanup;
        }
        if (inside) {
            oserr = OS_EINVALPARAMS;
            goto cleanup;
        }
    }

    fromDirectory = mstr_path_dirname(from);
    toDirectory   = mstr_path_dirname(to);
    targetName    = mstr_path_basename(to);
    if (fromDirectory == NULL || toDirectory == NULL || targetName == NULL) {
        oserr = OS_EOOM;
        goto cleanup;
    }

    if (mstr_icmp(fromDirectory, toDirectory) == 0) {
        oserr = __RenameInPlace(mfs, source, targetName);
    } else {
        oserr = __MoveToDirectory(mfs, source, to, targetName);
    }

cleanup:
    MFSEntryDelete(source);
    MFSEntryDelete(existing);
    mstr_delete(fromDirectory);
    mstr_delete(toDirectory);
    mstr_delete(targetName);
    return oserr;
}

oserr_t
FsMove(
        _In_ void*      instanceData,
        _In_ mstring_t* from,
        _In_ mstring_t* to,
        _In_ int        copy)
{
    FileSystemMFS_t* mfs = instanceData;
    oserr_t          oserr;

    usched_mtx_lock(&mfs->Lock);
    oserr = __MovePath(mfs, from, to, copy);
    usched_mtx_unlock(&mfs->Lock);
    return oserr;
}

static oserr_t
__ReadEntry(
        _In_  FileSystemMFS_t* mfs,
        _In_  MFSEntry_t*      entry,
        _In_  uuid_t           bufferHandle,
        _In_  void*            buffer,
        _In_  size_t           bufferOffset,
        _In_  size_t           unitCount,
        _Out_ size_t*          unitsRead)
{
    if (entry->Flags & FILE_FLAG_DIRECTORY) {
        return FsReadFromDirectory(mfs, entry, buffer, bufferOffset, unitCount, unitsRead);
    }
    else {
        return FsReadFromFile(mfs, entry, bufferHandle, buffer, bufferOffset, unitCount, unitsRead);
    }
}

// The lock is held also when the data goes straight to the caller's buffer.
// The read may still use the transfer buffer for the first and last sector,
// and it follows the bucket map to find the next bucket of the file.
oserr_t
FsRead(
        _In_  void*   instanceData,
        _In_  void*   data,
        _In_  uuid_t  bufferHandle,
        _In_  void*   buffer,
        _In_  size_t  bufferOffset,
        _In_  size_t  unitCount,
        _Out_ size_t* unitsRead)
{
    FileSystemMFS_t* mfs = instanceData;
    oserr_t          oserr;

    usched_mtx_lock(&mfs->Lock);
    oserr = __ReadEntry(
            mfs,
            (MFSEntry_t*)data,
            bufferHandle,
            buffer,
            bufferOffset,
            unitCount,
            unitsRead
    );
    usched_mtx_unlock(&mfs->Lock);
    return oserr;
}

static oserr_t
__WriteEntry(
        _In_  FileSystemMFS_t* mfs,
        _In_  MFSEntry_t*      entry,
        _In_  uuid_t           bufferHandle,
        _In_  void*            buffer,
        _In_  size_t           bufferOffset,
        _In_  size_t           unitCount,
        _Out_ size_t*          unitsWritten)
{
    if (!(entry->Flags & FILE_FLAG_DIRECTORY)) {
        return FsWriteToFile(mfs, entry, bufferHandle, buffer, bufferOffset, unitCount, unitsWritten);
    }
    return OS_EINVALPARAMS;
}

// The lock is held for the same reasons as in FsRead. A write can also give
// the file new buckets, which changes the bucket map and the master record.
oserr_t
FsWrite(
        _In_  void*   instanceData,
        _In_  void*   data,
        _In_  uuid_t  bufferHandle,
        _In_  void*   buffer,
        _In_  size_t  bufferOffset,
        _In_  size_t  unitCount,
        _Out_ size_t* unitsWritten)
{
    FileSystemMFS_t* mfs = instanceData;
    oserr_t          oserr;

    usched_mtx_lock(&mfs->Lock);
    oserr = __WriteEntry(
            mfs,
            (MFSEntry_t*)data,
            bufferHandle,
            buffer,
            bufferOffset,
            unitCount,
            unitsWritten
    );
    usched_mtx_unlock(&mfs->Lock);
    return oserr;
}

static oserr_t
__SeekEntry(
        _In_  FileSystemMFS_t* mfs,
        _In_  MFSEntry_t*      entry,
        _In_  uint64_t         absolutePosition,
        _Out_ uint64_t*        absolutePositionOut)
{
    oserr_t osStatus;

    if (entry->Flags & FILE_FLAG_DIRECTORY) {
        osStatus = FsSeekInDirectory(mfs, entry, absolutePosition);
    } else {
        osStatus = FsSeekInFile(mfs, entry, absolutePosition);
    }
    if (osStatus == OS_EOK) {
        *absolutePositionOut = entry->Position;
    }
    return osStatus;
}

// A seek reads the bucket map in memory, which a create or a write on another
// handle may be changing.
oserr_t
FsSeek(
        _In_  void*     instanceData,
        _In_  void*     data,
        _In_  uint64_t  absolutePosition,
        _Out_ uint64_t* absolutePositionOut)
{
    FileSystemMFS_t* mfs = instanceData;
    oserr_t          oserr;

    usched_mtx_lock(&mfs->Lock);
    oserr = __SeekEntry(mfs, (MFSEntry_t*)data, absolutePosition, absolutePositionOut);
    usched_mtx_unlock(&mfs->Lock);
    return oserr;
}

oserr_t
FsTruncate(
        _In_ void*    instanceData,
        _In_ void*    data,
        _In_ uint64_t size)
{
    FileSystemMFS_t* mfs = instanceData;
    oserr_t          oserr;

    usched_mtx_lock(&mfs->Lock);
    oserr = FsChangeFileSize(mfs, (MFSEntry_t*)data, size);
    usched_mtx_unlock(&mfs->Lock);
    return oserr;
}

static FileSystemMFS_t*
__FileSystemMFSNew(
        _In_ struct VFSStorageParameters* storageParameters,
        _In_ StorageDescriptor_t*         storageStats)
{
    FileSystemMFS_t* mfs;
    oserr_t          oserr;

    mfs = (FileSystemMFS_t*)malloc(sizeof(FileSystemMFS_t));
    if (!mfs) {
        return NULL;
    }
    memset(mfs, 0, sizeof(FileSystemMFS_t));
    usched_mtx_init(&mfs->Lock, USCHED_MUTEX_PLAIN);

    // Store the disk geometry used to calculate transfer offsets and sizes.
    memcpy(&mfs->Storage, storageParameters, sizeof(struct VFSStorageParameters));
    mfs->SectorSize = storageStats->SectorSize;

    // Create a generic transfer buffer for us to use
    oserr = SHMCreate(
        &(SHM_t) {
            .Flags = SHM_COMMIT | SHM_CLEAN,
            .Size = ((storageStats->SectorSize + MemoryPageSize() - 1) / MemoryPageSize()) * MemoryPageSize(),
            .Access = SHM_ACCESS_READ | SHM_ACCESS_WRITE
        },
        &mfs->TransferBuffer
    );
    if (oserr != OS_EOK) {
        free(mfs);
        return NULL;
    }
    return mfs;
}

// Frees the mount and everything it holds. FsInitialize calls this when it
// fails part way, so Label, RootEntry.Name and BucketMap may still be NULL.
// __FileSystemMFSNew cleared the structure, and mstr_delete and free do
// nothing for NULL.
static void __FileSystemMFSDelete(
        _In_ FileSystemMFS_t* mfs)
{
    OSHandleDestroy(&mfs->TransferBuffer);
    mstr_delete(mfs->Label);
    mstr_delete(mfs->RootEntry.Name);
    free(mfs->BucketMap);
    free(mfs);
}

static oserr_t __ParseBootRecord(
        _In_ FileSystemMFS_t* mfs,
        _In_ BootRecord_t*    bootRecord)
{
    if (bootRecord->Magic != MFS_BOOTRECORD_MAGIC) {
        ERROR("__ParseBootRecord invalid boot-record signature (0x%x, expected 0x%x)",
              bootRecord->Magic, MFS_BOOTRECORD_MAGIC);
        return OS_EINVALPARAMS;
    }
    TRACE("Fs-Version: %u", bootRecord->Version);

    // Store some data from the boot-record
    mfs->Version                  = (int)bootRecord->Version;
    mfs->Flags                    = (unsigned int)bootRecord->Flags;
    mfs->MasterRecordSector       = bootRecord->MasterRecordSector;
    mfs->MasterRecordMirrorSector = bootRecord->MasterRecordMirror;
    mfs->SectorsPerBucket         = bootRecord->SectorsPerBucket;
    mfs->ReservedSectorCount      = bootRecord->ReservedSectors;

    // Bucket entries are 64 bit (8 bytes) in map
    mfs->BucketsPerSectorInMap = mfs->SectorSize / sizeof(struct MapRecord);
    return OS_EOK;
}

static oserr_t
__ResizeTransferBuffer(
        _In_ FileSystemMFS_t* mfs)
{
    OSHandleDestroy(&mfs->TransferBuffer);

    // Create the transfer buffer used for subsequent filesystem operations.
    return SHMCreate(
        &(SHM_t) {
                .Flags = SHM_COMMIT | SHM_CLEAN,
                .Size = (((size_t)mfs->SectorsPerBucket * mfs->SectorSize * MFS_ROOTSIZE +
                            MemoryPageSize() - 1) / MemoryPageSize()) * MemoryPageSize(),
                .Access = SHM_ACCESS_READ | SHM_ACCESS_WRITE
        },
        &mfs->TransferBuffer
    );
}

static oserr_t __ValidateMasterRecord(
        _In_ MasterRecord_t*  masterRecord)
{
    if (masterRecord->Magic != MFS_BOOTRECORD_MAGIC) {
        ERROR("__ValidateMasterRecord invalid master-record signature (0x%x, expected 0x%x)",
              masterRecord->Magic, MFS_BOOTRECORD_MAGIC);
        return OS_EINVALPARAMS;
    }
    return OS_EOK;
}

static oserr_t
__ParseAndProcessMasterRecord(
        _In_ FileSystemMFS_t* mfs)
{
    uint8_t* bMap;
    uint64_t bytesRead;
    uint64_t bytesLeft;
    oserr_t  oserr;
    size_t   i, imax;
    size_t   sectorsTransferred;

    TRACE("Partition-name: %s", &mfs->MasterRecord.PartitionName[0]);
    mfs->Label = mstr_new_u8((const char*)&mfs->MasterRecord.PartitionName[0]);
    if (mfs->Label == NULL) {
        ERROR("__ParseAndProcessMasterRecord failed to allocate the partition label");
        return OS_EOOM;
    }

    // Parse the master record
    TRACE("Partition flags: 0x%x", mfs->MasterRecord.Flags);
    TRACE("Caching bucket-map (Sector %u - Size %u Bytes)",
          LODWORD(mfs->MasterRecord.MapSector),
          LODWORD(mfs->MasterRecord.MapSize));

    // Calculate the number of entries in the bucket map.
    mfs->BucketsInMap = mfs->MasterRecord.MapSize / sizeof(struct MapRecord);
    mfs->BucketMap    = (uint32_t*)malloc((size_t)mfs->MasterRecord.MapSize + mfs->SectorSize);
    if (mfs->BucketMap == NULL) {
        return OS_EOOM;
    }

    // Load map
    bMap      = (uint8_t*)mfs->BucketMap;
    bytesLeft = mfs->MasterRecord.MapSize;
    bytesRead = 0;
    i         = 0;
    imax      = DIVUP(bytesLeft, (mfs->SectorsPerBucket * mfs->SectorSize));

#ifdef CACHE_SEGMENTED
    while (bytesLeft) {
        uint64_t mapSector    = mfs->MasterRecord.MapSector + (i * mfs->SectorsPerBucket);
        size_t   transferSize = MIN((mfs->SectorsPerBucket * mfs->SectorSize), (size_t)bytesLeft);
        size_t   sectorCount  = DIVUP(transferSize, mfs->SectorSize);

        oserr = FSStorageRead(
                &mfs->Storage,
                mfs->TransferBuffer.ID,
                0,
                &(UInteger64_t) { .QuadPart = mapSector },
                sectorCount,
                &sectorsTransferred
        );
        if (oserr != OS_EOK) {
            ERROR("Failed to read sector 0x%x (map) into cache", LODWORD(mapSector));
            return oserr;
        }

        if (sectorsTransferred != sectorCount) {
            ERROR("Read %u sectors instead of %u from sector %u [bytesleft %u]",
                  LODWORD(sectorsTransferred), LODWORD(sectorCount),
                  LODWORD(mfs->Storage.SectorStart.QuadPart + mapSector),
                  LODWORD(bytesLeft));
            return OS_EDEVFAULT;
        }

        // Reset buffer position to 0 and read the data into the map
        memcpy(bMap, SHMBuffer(&mfs->TransferBuffer), transferSize);
        bytesLeft -= transferSize;
        bytesRead += transferSize;
        bMap      += transferSize;
        i++;
        if (i == (imax / 4) || i == (imax / 2) || i == ((imax / 4) * 3)) {
            TRACE("Cached %u/%u bytes of sector-map", LODWORD(bytesRead), LODWORD(mfs->MasterRecord.MapSize));
        }
    }
    TRACE("Bucket map was cached");
#else
    DMABuffer_t     mapInfo;
    OSHandle_t      mapAttachment;
    uint64_t        mapSector   = Mfs->MasterRecord.MapSector + (i * Mfs->SectorsPerBucket);
    size_t          sectorCount = DIVUP((size_t)Mfs->MasterRecord.MapSize,
        Descriptor->Disk.descriptor.SectorSize);

    mapInfo.name     = "mfs_mapbuffer";
    mapInfo.length   = (size_t)Mfs->MasterRecord.MapSize + mfs->SectorSize;
    mapInfo.capacity = (size_t)Mfs->MasterRecord.MapSize + mfs->SectorSize;
    mapInfo.flags    = DMA_PERSISTANT;
    DmaInfo.type     = DMA_TYPE_DRIVER_32;

    Status = dma_export(bMap, &mapInfo, &mapAttachment);
    if (Status != OS_EOK) {
        ERROR("[mfs] [init] failed to export buffer for sector-map");
        goto Error;
    }

    if (MfsReadSectors(Descriptor, mapAttachment.handle, 0,
            mapSector, sectorCount, &SectorsTransferred) != OS_EOK) {
        ERROR("[mfs] [init] failed to read sector 0x%x (map) into cache", LODWORD(mapSector));
        goto Error;
    }

    if (SectorsTransferred != sectorCount) {
        ERROR("[mfs] [init] read %u sectors instead of %u from sector %u",
            LODWORD(SectorsTransferred), LODWORD(sectorCount), LODWORD(mapSector));
        goto Error;
    }

    dma_detach(&mapAttachment);
#endif
    return OS_EOK;
}

static oserr_t __InitializeRootEntry(
        _In_ FileSystemMFS_t* mfs)
{
    // __FileSystemMFSNew clears the structure. Set the nonzero defaults here.
    mfs->RootEntry.Name = mstr_new_u8(RootEntryName);
    if (mfs->RootEntry.Name == NULL) {
        ERROR("__InitializeRootEntry failed to allocate the name of the root");
        return OS_EOOM;
    }
    mfs->RootEntry.Owner = UUID_INVALID;
    mfs->RootEntry.Permissions = FILE_PERMISSION_READ | FILE_PERMISSION_OWNER_WRITE;
    mfs->RootEntry.Flags = FILE_FLAG_DIRECTORY;
    mfs->RootEntry.ActionOnClose = MFS_ACTION_NONE;
    mfs->RootEntry.StartBucket = mfs->MasterRecord.RootIndex;
    mfs->RootEntry.StartLength = MFS_ROOTSIZE;
    mfs->RootEntry.DirectoryBucket = MFS_ENDOFCHAIN;
    MFSEntryResetPosition(&mfs->RootEntry);
    return OS_EOK;
}

oserr_t
FsInitialize(
        _In_  struct VFSStorageParameters* storageParameters,
        _Out_ void**                       instanceData)
{
    FileSystemMFS_t*    mfs;
    oserr_t             oserr;
    size_t              sectorsTransferred;
    StorageDescriptor_t storageStats;

    TRACE("FsInitialize()");

    // Get the geometry of the disk, we need to it to calculate various offsets on
    // the storage medium.
    oserr = FSStorageStat(storageParameters, &storageStats);
    if (oserr != OS_EOK) {
        return oserr;
    }

    mfs = __FileSystemMFSNew(storageParameters, &storageStats);
    if (mfs == NULL) {
        ERROR("FsInitialize Failed to allocate memory for the fileystem");
        return OS_EOOM;
    }

    // Read the boot-sector
    oserr = FSStorageRead(
            storageParameters,
            mfs->TransferBuffer.ID,
            0,
            &(UInteger64_t) { .QuadPart = 0 },
            1,
            &sectorsTransferred
    );
    if (oserr != OS_EOK) {
        ERROR("FsInitialize Failed to read mfs boot-sector record");
        goto error_exit;
    }

    oserr = __ParseBootRecord(
            mfs,
            (BootRecord_t*)SHMBuffer(&mfs->TransferBuffer)
    );
    if (oserr != OS_EOK) {
        ERROR("FsInitialize failed to parse the boot record");
        goto error_exit;
    }

    // Read the master-record
    oserr = FSStorageRead(
            storageParameters,
            mfs->TransferBuffer.ID,
            0,
            &(UInteger64_t) { .QuadPart = mfs->MasterRecordSector },
            1,
            &sectorsTransferred
    );
    if (oserr != OS_EOK) {
        ERROR("FsInitialize failed to read the master record");
        goto error_exit;
    }

    // Validate and save the master record before resizing the buffer that contains it.
    oserr = __ValidateMasterRecord((MasterRecord_t*)SHMBuffer(&mfs->TransferBuffer));
    if (oserr != OS_EOK) {
        ERROR("FsInitialize failed to read the master record");
        goto error_exit;
    }
    memcpy(&mfs->MasterRecord, SHMBuffer(&mfs->TransferBuffer), sizeof(MasterRecord_t));

    oserr = __ResizeTransferBuffer(mfs);
    if (oserr != OS_EOK) {
        ERROR("FsInitialize failed to allocate memory for resized transfer buffer");
        goto error_exit;
    }

    oserr = __ParseAndProcessMasterRecord(mfs);
    if (oserr != OS_EOK) {
        ERROR("FsInitialize failed to read the master record");
        goto error_exit;
    }

    oserr = __InitializeRootEntry(mfs);
    if (oserr != OS_EOK) {
        ERROR("FsInitialize failed to set up the root directory");
        goto error_exit;
    }
    *instanceData = mfs;
    return OS_EOK;

error_exit:
    __FileSystemMFSDelete(mfs);
    return oserr;
}

// filed only destroys a mount after every operation on it has ended.
// VFSDestroy (services/filed/vfs/vfs.c) stops new operations and waits for the
// running ones before the driver's Destroy is sent. So the lock is normally
// free here. If it is not, an operation is still running and would use the
// mount after it was freed. The mount is then kept and OS_EBUSY returned.
// filed logs the error and forgets the mount, so its memory is lost, but
// nothing is freed while it is in use.
// trylock does not wait, so no other job can be queued on the lock when it
// succeeds. The lock is released again before the free, and this job does not
// give way to other jobs between the two, so nobody can take it in between.
oserr_t
FsDestroy(
        _In_ void*         instanceData,
        _In_ unsigned int  unmountFlags)
{
    FileSystemMFS_t* fileSystem = instanceData;
    if (instanceData == NULL) {
        return OS_EINVALPARAMS;
    }

    if (usched_mtx_trylock(&fileSystem->Lock) != 0) {
        ERROR("FsDestroy an operation is still running on the filesystem, it is not freed");
        return OS_EBUSY;
    }

    // Which kind of unmount is it?
    if (!(unmountFlags & 0x1)) {
        // Flush everything
        // @todo
    }

    usched_mtx_unlock(&fileSystem->Lock);
    __FileSystemMFSDelete(fileSystem);
    return OS_EOK;
}
