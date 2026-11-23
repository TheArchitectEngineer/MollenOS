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
 *  - Missing implementations:
 *    - Journaling
 *    - Encryptiong (AES)
 *      - Versioning
 *      - Inline-data
 *      - Switch to B+ trees for metadata
 */

#ifndef _MFS_H_
#define _MFS_H_

#include <ddk/filesystem.h>
#include <os/mollenos.h>
#include <os/types/handle.h>
#include <os/types/shm.h>
#include <os/usched/mutex.h>
#include <ds/mstring.h>

/**
 * MFS Definitions and Utilities
 * Contains magic constant values and utility macros for conversion
 */
#define MFS_ENDOFCHAIN                0xFFFFFFFF
#define MFS_SECTORCOUNT(mfs, bucket) ((mfs)->SectorsPerBucket * (bucket))
#define MFS_GETSECTOR(mfs, bucket)   ((mfs)->ReservedSectorCount + MFS_SECTORCOUNT(mfs, bucket))
#define MFS_ROOTSIZE                  8
#define MFS_DIRECTORYEXPANSION        4

#define MFS_ACTION_NONE     0x0
#define MFS_ACTION_UPDATE   0x1
#define MFS_ACTION_CREATE   0x2
#define MFS_ACTION_DELETE   0x3
#define MFS_ACTION_RENAME   0x4 // Only the name of the record changes

PACKED_TYPESTRUCT(BootRecord, {
    uint8_t  JumpCode[3];

    // Header
    uint32_t Magic;
    uint8_t  Version;
    uint8_t  Flags;

    // Disk Information
    uint8_t  MediaType;
    uint16_t SectorSize;
    uint16_t SectorsPerTrack;
    uint16_t HeadsPerCylinder;
    uint64_t SectorCount;
    
    // MFS Information
    uint16_t ReservedSectors;
    uint16_t SectorsPerBucket;
    uint64_t MasterRecordSector;
    uint64_t MasterRecordMirror;

    uint8_t  BootCode[468];    //512 - 44
});

/**
 * MFS Magic Value 
 * The signature value that must be present in BootRecord::Magic
 */
#define MFS_BOOTRECORD_MAGIC 0x3153464D // 1SFM

/**
 * MFS Boot-Record flags
 * The possible values that can be present in BootRecord::Flags
 */
#define MFS_BOOTRECORD_SYSDRIVE  0x1  // Boot Partition
#define MFS_BOOTRECORD_ENCRYPTED 0x2  // Partition is protected by encryption

typedef uint32_t bucket_t;

/**
 * @brief Partition metadata stored in two locations on disk for recovery.
 */
PACKED_TYPESTRUCT(MasterRecord, {
    uint32_t Magic;
    uint32_t Flags;
    uint32_t Checksum;           // Checksum of the master-record
    uint8_t  PartitionName[64];

    bucket_t FreeBucket;         // Pointer to first free index
    bucket_t RootIndex;          // Pointer to root directory
    bucket_t BadBucketIndex;     // Pointer to list of bad buckets
    bucket_t JournalIndex;       // Pointer to journal file

    uint64_t MapSector;          // Start sector of bucket-map
    uint64_t MapSize;            // Size of bucket map
});

#define MFS_MASTERRECORD_SYSTEM_DRIVE 0x1  // Partition is system drive
#define MFS_MASTERRECORD_DATA_DRIVE   0x2  // Partition is system data drive
#define MFS_MASTERRECORD_USER_DRIVE   0x4  // Partition is system user drive
#define MFS_MASTERRECORD_HIDDEN       0x8  // Partition is marked as hidden
#define MFS_MASTERRECORD_DIRTY        0x10 // Journal must be checked on startup

/**
 * @brief Bucket length and link to the next bucket in the chain.
 *
 * Read Map[Link] to get the next bucket's length. For bucket 0, the length is
 * HIDWORD(Map[0]) and the link is LODWORD(Map[0]). A link of 0xFFFFFFFF ends the chain.
 */
PACKED_TYPESTRUCT(MapRecord, {
    bucket_t Link;
    uint32_t Length;
});

/* The file-time structure
 * Keeps track of the last time records were modified */
PACKED_TYPESTRUCT(DateTimeRecord, {
    // Date Information
    uint16_t Year;
    uint8_t  Month;
    uint8_t  Day;

    // Time Information
    uint8_t  Hour;
    uint8_t  Minute;
    uint8_t  Second;
    uint8_t  MilliSeconds; // In the interval of 4 (20 = 80 milliseconds, 249 = 996 milliseconds)
});

/* The file-versioning structure
 * Contains a copy of a file somewhere in time (36 Bytes) */
PACKED_TYPESTRUCT(VersionRecord, {
    DateTimeRecord_t Timestamp;      // 0x00 - Timestamp of this version
    uint32_t         StartBucket;    // 0x08 - First data bucket
    uint32_t         StartLength;    // 0x0C - Length of first data bucket
    uint64_t         Size;           // 0x10 - Size of data (Set size if sparse)
    uint64_t         AllocatedSize;  // 0x18 - Actual size allocated
    uint32_t         SparseMap;      // 0x20 - Bucket of sparse-map
});

/* The file-record structure
 * Describes a record contained in a directory which can consist of multiple types, 
 * with the common types being both directories and files, and file-links */
PACKED_TYPESTRUCT(FileRecord, {
    uint32_t         Flags;                // 0x00 - Record Flags
    bucket_t         StartBucket;        // 0x04 - First data bucket
    uint32_t         StartLength;        // 0x08 - Length of first data bucket
    uint32_t         RecordChecksum;        // 0x0C - Checksum of record excluding this entry + inline data
    uint64_t         DataChecksum;        // 0x10 - Checksum of data

    // DateTime Records (8 bytes each, 64 bit)
    DateTimeRecord_t CreatedAt;            // 0x18 - Created timestamp
    DateTimeRecord_t ModifiedAt;            // 0x20 - Last modified timestamp
    DateTimeRecord_t AccessedAt;            // 0x28 - Last accessed timestamp
    
    uint64_t         Size;                // 0x30 - Size of data (Set size if sparse)
    uint64_t         AllocatedSize;        // 0x38 - Actual size allocated
    uint32_t         SparseMap;            // 0x40 - Bucket of sparse-map

    uint8_t          Name[300];            // 0x44 - Record name (150 UTF16)
    
    // Versioning Support
    VersionRecord_t  Versions[4];        // 0x170 - Record Versions

    // Inline Data Support
    uint8_t          Integrated[512];    // 0x200
});

#define MFS_FILERECORD_MAX_NAME 300

/* MFS FileRecord-Flags Definitions
 * Contains constants and bitfield definitions for FileRecord::Flags */
#define MFS_FILERECORD_FILE             0x0        
#define MFS_FILERECORD_DIRECTORY        0x1
#define MFS_FILERECORD_LINK             0x2
#define MFS_FILERECORD_RESERVED         0x3
#define MFS_FILERECORD_TYPE(Flags)      ((Flags) & 0x3)

#define MFS_FILERECORD_SECURITY         0x4         // User must possess the right key to unlock
#define MFS_FILERECORD_SYSTEM           0x8         // Readable, nothing else
#define MFS_FILERECORD_HIDDEN           0x10        // Don't show
#define MFS_FILERECORD_CHAINED          0x20        // Means all buckets are adjacent
#define MFS_FILERECORD_LOCKED           0x40        // File is deep-locked

#define MFS_FILERECORD_VERSIONED        0x10000000  // Record is versioned
#define MFS_FILERECORD_INLINE           0x20000000  // Inline data is present
#define MFS_FILERECORD_SPARSE           0x40000000  // Record-sparse map is in use
#define MFS_FILERECORD_INUSE            0x80000000  // Record is in use

typedef struct MFSBucketMap {
    // Cache the master record for updates to the free bucket pointer.
    MasterRecord_t MasterRecord;

    // The map has room for every bucket, including buckets joined into chains.
    uint32_t BucketCount;

    // Cache the entire bucket map at mount time to speed up lookups.
    MapRecord_t* Records;
} MFSBucketMap_t;

typedef struct MFSEntry {
    mstring_t* Name;
    uint32_t   Owner;
    uint32_t   Permissions;
    uint32_t   Flags;

    uint32_t   NativeFlags;
    int        ActionOnClose;

    // Allocated bytes on disk. ActualSize is the file length.
    uint64_t AllocatedSize;
    uint64_t ActualSize;

    // The initial bucket and length of that bucket
    bucket_t StartBucket;
    uint32_t StartLength;

    // The bucket of the directory where this file resides,
    // and the length of the bucket. We also store the file
    // descriptor index into this bucket.
    bucket_t DirectoryBucket;
    uint32_t DirectoryLength;
    size_t   DirectoryIndex;

    // Current position for this file handle. We store the bucket
    // and the length of that bucket.
    uint64_t Position;
    bucket_t DataBucketPosition;
    uint32_t DataBucketLength;
    uint64_t BucketByteBoundary;  // Support variadic bucket sizes
} MFSEntry_t;

/**
 * @brief Mounted filesystem geometry, transfer buffer and cached disk records.
 *
 * Lock: every operation on a mount (FsOpen, FsCreate, FsRead and the others
 * in main.c) holds Lock from start to end. Each request from filed runs as its
 * own job, and a job lets other jobs run each time it waits for the disk. One
 * mount has one TransferBuffer, so without the lock a second operation could
 * read into the buffer while the first one still has data in it. Lock protects:
 * - TransferBuffer.
 * - BucketMap and MasterRecord in memory.
 * - The read, change and write steps on a directory bucket. A record is added
 *   by reading the bucket, changing it in the buffer and writing it back. The
 *   lock must cover all three steps, so it is held from the start of the
 *   operation to its end. A lock around each single disk transfer would leave
 *   gaps between the steps.
 * The cost: operations on one mount run one at a time. This includes file
 *   reads and writes that move data straight to the caller's buffer. Other
 *   mounts are not held up.
 * Lock is plain (not recursive). Only the outer Fs* functions take it, and
 *   none of them calls another one. FsInitialize does not take it, as filed
 *   gets the mount only when FsInitialize returns. FsDestroy only checks that
 *   it is free (see FsDestroy in main.c).
 */
typedef struct FileSystemMFS {
    struct usched_mtx           Lock;
    mstring_t*                  Label;
    unsigned int                Flags;
    int                         Version;
    size_t                      SectorsPerBucket;
    OSHandle_t                  TransferBuffer;
    struct VFSStorageParameters Storage;
    size_t                      SectorSize;
    uint16_t                    ReservedSectorCount;
    uint64_t                    MasterRecordSector;
    uint64_t                    MasterRecordMirrorSector;
    size_t                      BucketsPerSectorInMap;
    size_t                      BucketsInMap;

    // Cached resources
    uint32_t*      BucketMap;
    MasterRecord_t MasterRecord;
    MFSEntry_t     RootEntry;
} FileSystemMFS_t;

/**
 * @brief
 * @param mfs
 * @param bucketCount
 * @param allocatedRecord
 * @return
 */
extern oserr_t
MFSBucketMapAllocate(
        _In_  FileSystemMFS_t* mfs,
        _In_  size_t           bucketCount,
        _Out_ MapRecord_t*     allocatedRecord);

/**
 * @brief
 * @param mfs
 * @param bucket
 * @param link
 * @return
 */
extern oserr_t
MFSBucketMapGetLengthAndLink(
        _In_ FileSystemMFS_t* mfs,
        _In_ bucket_t         bucket,
        _In_ MapRecord_t*     link);

/**
 * @brief
 * @param mfs
 * @param bucket
 * @param link
 * @param length
 * @param updateLength
 * @return
 */
extern oserr_t
MFSBucketMapSetLinkAndLength(
        _In_ FileSystemMFS_t* mfs,
        _In_ bucket_t         bucket,
        _In_ bucket_t         link,
        _In_ uint32_t         length,
        _In_ bool             updateLength);

/**
 * @brief
 * @param mfs
 * @param entry
 * @return
 */
extern oserr_t
MFSAdvanceToNextBucket(
        _In_ FileSystemMFS_t* mfs,
        _In_ MFSEntry_t*      entry);

/* MfsZeroBucket
 * Wipes the given bucket and count with zero values
 * useful for clearing clusters of sectors */
extern oserr_t
MfsZeroBucket(
        _In_ FileSystemMFS_t* mfs,
        _In_ uint32_t         bucket,
        _In_ size_t           count);

/* MfsFreeBuckets
 * Frees an entire chain of buckets that has been allocated for 
 * a file-record */
extern oserr_t
MfsFreeBuckets(
        _In_ FileSystemMFS_t* mfs,
        _In_ uint32_t         startBucket,
        _In_ uint32_t         startLength);

/**
 * @brief
 * @param mfs
 * @param sourceBucket
 * @param sourceLength
 * @param destinationBucket
 * @param destinationLength
 * @return
 */
extern oserr_t
MFSCloneBucketData(
        _In_ FileSystemMFS_t* mfs,
        _In_ uint32_t         sourceBucket,
        _In_ uint32_t         sourceLength,
        _In_ uint32_t         destinationBucket,
        _In_ uint32_t         destinationLength);

/* MfsEnsureRecordSpace
 * Ensures that the given record has the space neccessary for the required data. */
extern oserr_t
MfsEnsureRecordSpace(
        _In_ FileSystemMFS_t* mfs,
        _In_ MFSEntry_t*      entry,
        _In_ uint64_t         spaceRequired);

/**
 * @brief Sets the position of an entry to the start of its data. The data
 * bucket becomes the first bucket of the entry (StartBucket and StartLength).
 * An entry without buckets gets the data bucket MFS_ENDOFCHAIN with a length
 * of 0. FsWriteToFile and FsReadFromFile see MFS_ENDOFCHAIN and look up the
 * bucket of the position once the file has buckets. FsReadFromDirectory reads
 * from the data bucket as it is. Every MFSEntry_t that is made for a record
 * (open, create, the root) must be set up this way.
 * @param entry Entry to update. StartBucket and StartLength must be set.
 */
extern void
MFSEntryResetPosition(
        _In_ MFSEntry_t* entry);

/* MfsUpdateRecord
 * Conveniance function for updating a given file on
 * the disk, not data related to file, but the metadata */
extern oserr_t
MfsUpdateRecord(
        _In_ FileSystemMFS_t* mfs,
        _In_ MFSEntry_t*      entry,
        _In_ int              action);

// Returns whether path is the root of the filesystem, "/".
static inline bool __PathIsRoot(mstring_t* path) {
    if (mstr_at(path, 0) == U'/' && mstr_len(path) == 1) {
        return true;
    }
    return false;
}

/**
 * The outcome of a record search that could read every directory it needed.
 * MFS_SEARCH_FOUND: an entry with the name exists, and the result entry describes it.
 * MFS_SEARCH_MISSING: no entry has the name. For a search that may expand the
 *                     directory, the result entry's Directory* members describe
 *                     a free record.
 */
enum MFSSearchResult {
    MFS_SEARCH_FOUND,
    MFS_SEARCH_MISSING
};

/**
 * @brief Looks up a path, starting from the given directory. A missing path is
 * reported through searchResultOut, so it is never mixed up with a disk error.
 * A storage driver that lost its disk fails reads with OS_ENOENT.
 * @param mfs Mounted filesystem.
 * @param directory Directory the path starts in.
 * @param path Path to look up. "/" gives a copy of the root entry.
 * @param entryOut Receives a new entry for the record when it was found. Free
 *                 it with mstr_delete(entry->Name) and free(entry).
 * @param searchResultOut Receives MFS_SEARCH_FOUND or MFS_SEARCH_MISSING when
 *                        OS_EOK is returned. A directory on the way that has
 *                        no buckets yet makes the path missing.
 * @return OS_EOK when the search finished, OS_ENOTDIR when a part of the path
 *         before the last is not a directory, OS_EINVALPARAMS for an empty
 *         path, OS_EOOM, or the error of the disk read that failed.
 */
extern oserr_t
MfsFindRecord(
        _In_  FileSystemMFS_t*      mfs,
        _In_  MFSEntry_t*           directory,
        _In_  mstring_t*            path,
        _Out_ MFSEntry_t**          entryOut,
        _Out_ enum MFSSearchResult* searchResultOut);

/**
 * @brief Locates a path, starting from the given directory. The same as
 * MfsFindRecord, but a missing path is returned as OS_ENOENT. Callers that
 * must tell a missing path from a failed disk use MfsFindRecord.
 * @param mfs
 * @param directory
 * @param path
 * @param entryOut
 * @return
 */
extern oserr_t
MfsLocateRecord(
        _In_  FileSystemMFS_t* mfs,
        _In_  MFSEntry_t*      directory,
        _In_  mstring_t*       path,
        _Out_ MFSEntry_t**     entryOut);

/**
 * @brief
 * @param vfsCommonData
 * @param entry
 * @param name
 * @param owner
 * @param flags
 * @param permissions
 * @param entryOut
 * @return
 */
extern oserr_t
MfsCreateRecord(
        _In_  FileSystemMFS_t* mfs,
        _In_  MFSEntry_t*      entry,
        _In_  mstring_t*       name,
        _In_  uint32_t         owner,
        _In_  uint32_t         flags,
        _In_  uint32_t         permissions,
        _In_  uint64_t         allocatedSize,
        _In_  uint64_t         size,
        _In_  uint32_t         startBucket,
        _In_  uint32_t         startLength,
        _Out_ MFSEntry_t**     entryOut);

/**
 * @brief Converts the generic vfs options/permissions to the native mfs representation.
 * @param flags
 * @return
 */
extern unsigned int
MFSToNativeFlags(
    _In_ unsigned int flags);

/**
 * @brief Converts the native MFS file flags into the generic vfs options/permissions.
 * @param fileRecord
 * @param flags
 * @param permissions
 */
extern void
MFSFromNativeFlags(
    _In_  FileRecord_t* fileRecord,
    _Out_ unsigned int* flags,
    _Out_ unsigned int* permissions);

/* MfsFileRecordToVfsFile
 * Converts a native MFS file record into the generic vfs representation. */
extern void
MfsFileRecordToVfsFile(
        _In_ FileSystemMFS_t* mfs,
        _In_ FileRecord_t*    nativeEntry,
        _In_ MFSEntry_t*      mfsEntry);

#endif //!_MFS_H_
