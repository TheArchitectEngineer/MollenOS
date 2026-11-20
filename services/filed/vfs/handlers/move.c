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
 *
 * Move and copy of files (VFSNodeMove).
 *
 * A move runs in steps, and each step looks up the directory it needs and
 * releases it again, so at most one directory lock is held at a time. Two
 * directory locks at once can deadlock. Two moves in opposite directions
 * would each hold one directory and wait for the other. A lookup holds a
 * parent while it waits for a child, so a move that write locks the child and
 * then waits for the parent would wait for that lookup forever. A node can be
 * destroyed while its directory is unlocked, so no step keeps a node from an
 * earlier step. struct __MoveRequest keeps copies of what the steps share.
 *
 * Steps:
 * 1. __CheckTarget: the target directory must exist and the name must be free.
 * 2. __HandleSource: with the source directory write locked, the source is
 *    checked. Within one filesystem, the filesystem moves the file here and a
 *    move destroys the source node. For a move to another filesystem, the
 *    source file is opened in its filesystem.
 * 3. __AddTargetNode adds the node of the moved file to the target directory.
 *    For another filesystem, __CopyAcross creates the file there, copies the
 *    data and adds the node.
 * 4. A move to another filesystem then removes the source with VFSNodeUnlink.
 */

//#define __TRACE

#define __need_quantity
#include <ddk/utils.h>
#include <os/handle.h>
#include <os/grants.h>
#include <string.h>
#include <os/shm.h>
#include <vfs/vfs.h>

#include "../private.h"

/**
 * @brief What the steps of one move share.
 * SourceDirectoryPath, SourceName, TargetDirectoryPath and TargetName come from
 * the caller's paths, and are looked up in the caller's scope. SourcePath and
 * TargetPath are the paths inside the source and the target filesystem, as the
 * filesystem drivers take them. TargetVFS is the filesystem of the target
 * directory. It is only compared with other VFS pointers and never used, as
 * the filesystem can be destroyed once the target directory is released.
 * Flags, Permissions and Size are copied from the source node. A copy to
 * another filesystem sets Size to the number of bytes it copied.
 */
struct __MoveRequest {
    mstring_t*  SourceDirectoryPath;
    mstring_t*  SourceName;
    mstring_t*  TargetDirectoryPath;
    mstring_t*  TargetName;
    mstring_t*  SourcePath;
    mstring_t*  TargetPath;
    struct VFS* TargetVFS;
    uint32_t    Flags;
    uint32_t    Permissions;
    uint64_t    Size;
};

/**
 * @brief How the file gets to the target, decided in step 2.
 * MOVE_KIND_LOCAL:  source and target are in the same filesystem, which moves
 *                   or copies the file itself.
 * MOVE_KIND_ACROSS: the target is in another filesystem. filed creates the
 *                   file there and copies the data.
 */
enum __MoveKind {
    MOVE_KIND_LOCAL,
    MOVE_KIND_ACROSS
};

/**
 * @brief The source file of a move to another filesystem, open in its
 * filesystem driver. VFS is entered (VFSEnter) while Data is open, so the
 * filesystem and its driver stay valid without holding a node lock.
 */
struct __MoveSourceFile {
    struct VFS* VFS;
    void*       Data;
};

static mstring_t*
__CombineNodePath(
        _In_ struct VFSNode* directoryNode,
        _In_ mstring_t*      name)
{
    mstring_t* nodePath = VFSNodeMakePath(directoryNode, 1);
    mstring_t* combined;

    if (nodePath == NULL) {
        return NULL;
    }

    // Only the root path ends with a '/'. mstr_at(nodePath, -1) cannot be
    // used to find it, as it returns 0 for the last character of a string
    // that is one character long.
    if (__PathIsRoot(nodePath)) {
        combined = mstr_fmt("/%ms", name);
    } else {
        combined = mstr_fmt("%ms/%ms", nodePath, name);
    }
    mstr_delete(nodePath);
    return combined;
}

static void
__DestroyRequest(
        _In_ struct __MoveRequest* request)
{
    mstr_delete(request->SourceDirectoryPath);
    mstr_delete(request->SourceName);
    mstr_delete(request->TargetDirectoryPath);
    mstr_delete(request->TargetName);
    mstr_delete(request->SourcePath);
    mstr_delete(request->TargetPath);
}

// Splits both paths into a directory and a name. The root has no name and
// cannot be moved, and nothing can be moved onto it.
static oserr_t
__PrepareRequest(
        _In_ const char*           cfrom,
        _In_ const char*           cto,
        _In_ struct __MoveRequest* request)
{
    mstring_t* fromPath;
    mstring_t* toPath;
    oserr_t    oserr;

    fromPath = mstr_path_new_u8(cfrom);
    if (fromPath == NULL) {
        return OS_EOOM;
    }

    toPath = mstr_path_new_u8(cto);
    if (toPath == NULL) {
        mstr_delete(fromPath);
        return OS_EOOM;
    }

    if (__PathIsRoot(fromPath)) {
        WARNING("VFSNodeMove the root cannot be moved");
        oserr = OS_EPERMISSIONS;
        goto cleanup;
    }

    if (__PathIsRoot(toPath)) {
        oserr = OS_EEXISTS;
        goto cleanup;
    }

    request->SourceDirectoryPath = mstr_path_dirname(fromPath);
    request->SourceName          = mstr_path_basename(fromPath);
    request->TargetDirectoryPath = mstr_path_dirname(toPath);
    request->TargetName          = mstr_path_basename(toPath);
    if (request->SourceDirectoryPath == NULL || request->SourceName == NULL ||
        request->TargetDirectoryPath == NULL || request->TargetName == NULL) {
        oserr = OS_EOOM;
        goto cleanup;
    }
    oserr = OS_EOK;

cleanup:
    mstr_delete(fromPath);
    mstr_delete(toPath);
    return oserr;
}

// Releases a directory that __LockSource or __LockTarget write locked.
static void
__UnlockDirectory(
        _In_ struct VFSNode* directory)
{
    usched_rwlock_w_demote(&directory->Lock);
    VFSNodePut(directory);
}

// Step 1. The target directory must exist, and the target name must be free
// in it. Sets TargetPath and TargetVFS.
static oserr_t
__CheckTarget(
        _In_ struct VFS*           vfs,
        _In_ struct __MoveRequest* request)
{
    struct VFSNode* directory;
    struct VFSNode* existing;
    oserr_t         oserr;

    oserr = VFSNodeGet(vfs, request->TargetDirectoryPath, 1, &directory);
    if (oserr != OS_EOK) {
        TRACE("__CheckTarget failed to find %ms: %u", request->TargetDirectoryPath, oserr);
        return oserr;
    }

    if (!__NodeIsDirectory(directory)) {
        oserr = OS_ENOTDIR;
        goto exit;
    }

    // VFSNodeFind loads the directory first if needed, so names that are only
    // on disk are found as well.
    oserr = VFSNodeFind(directory, request->TargetName, &existing);
    if (oserr == OS_EOK) {
        oserr = OS_EEXISTS;
        goto exit;
    }
    if (oserr != OS_ENOENT) {
        ERROR("__CheckTarget failed to load %ms: %u", request->TargetDirectoryPath, oserr);
        goto exit;
    }

    request->TargetPath = __CombineNodePath(directory, request->TargetName);
    if (request->TargetPath == NULL) {
        oserr = OS_EOOM;
        goto exit;
    }
    request->TargetVFS = directory->FileSystem;
    oserr = OS_EOK;

exit:
    VFSNodePut(directory);
    return oserr;
}

// Looks up the source directory, write locks it and then write locks the
// source node in it. The write lock on the node waits for anyone that still
// reads the node (a stat or an open that found it earlier). On success release
// the node lock first, and then the directory with __UnlockDirectory. On
// failure nothing is held.
static oserr_t
__LockSource(
        _In_  struct VFS*           vfs,
        _In_  struct __MoveRequest* request,
        _Out_ struct VFSNode**      directoryOut,
        _Out_ struct VFSNode**      nodeOut)
{
    struct VFSNode*    directory;
    struct __VFSChild* child;
    oserr_t            oserr;

    oserr = VFSNodeGet(vfs, request->SourceDirectoryPath, 1, &directory);
    if (oserr != OS_EOK) {
        TRACE("__LockSource failed to find %ms: %u", request->SourceDirectoryPath, oserr);
        return oserr;
    }

    // Loading promotes the reader lock by itself, so it must run before the
    // promote below. With the write lock held, its promote would wait for a
    // lock this job already holds.
    oserr = VFSNodeEnsureLoaded(directory);
    if (oserr != OS_EOK) {
        ERROR("__LockSource failed to load %ms: %u", request->SourceDirectoryPath, oserr);
        VFSNodePut(directory);
        return oserr;
    }

    usched_rwlock_w_promote(&directory->Lock);
    child = hashtable_get(
            &directory->Children,
            &(struct __VFSChild) { .Key = request->SourceName }
    );
    if (child == NULL) {
        __UnlockDirectory(directory);
        return OS_ENOENT;
    }

    usched_rwlock_w_lock(&child->Node->Lock);
    *directoryOut = directory;
    *nodeOut      = child->Node;
    return OS_EOK;
}

// Checks that the source can be moved or copied. Only files are supported,
// as the copy to another filesystem copies file data. A move destroys the
// source node, so the node must not be open or be the source of a bind.
// The node must be write locked.
static oserr_t
__VerifySource(
        _In_ struct VFSNode* node,
        _In_ bool            copy)
{
    size_t handles;

    // Mount points and bind mounts are not files of this filesystem.
    if (!__NodeIsRegular(node)) {
        return OS_EPERMISSIONS;
    }

    if (__NodeIsDirectory(node)) {
        return OS_EISDIR;
    }

    if (!__NodeIsFile(node)) {
        return OS_ENOTSUPPORTED;
    }

    if (copy) {
        return OS_EOK;
    }

    // Closes change Handles without the node lock, under HandlesLock.
    usched_mtx_lock(&node->HandlesLock);
    handles = node->Handles.element_count;
    usched_mtx_unlock(&node->HandlesLock);
    if (handles != 0 || node->Mounts.element_count != 0) {
        return OS_EBUSY;
    }
    return OS_EOK;
}

// Step 2 within one filesystem. Runs with the source directory and the
// source node write locked. The filesystem moves or copies the file. A move
// then removes the source node, which __VerifySource checked is not in use.
// The node is unlocked here, also when it was destroyed.
static oserr_t
__MoveWithinFileSystem(
        _In_ struct VFSNode*       directory,
        _In_ struct VFSNode*       node,
        _In_ struct __MoveRequest* request,
        _In_ bool                  copy)
{
    struct VFS* vfs = directory->FileSystem;
    oserr_t     oserr;

    oserr = vfs->Interface->Operations.Move(
            vfs->Interface,
            vfs->Data,
            request->SourcePath,
            request->TargetPath,
            copy ? 1 : 0
    );
    if (oserr != OS_EOK) {
        TRACE("__MoveWithinFileSystem %ms to %ms failed: %u", request->SourcePath, request->TargetPath, oserr);
        usched_rwlock_w_unlock(&node->Lock);
        return oserr;
    }

    if (copy) {
        usched_rwlock_w_unlock(&node->Lock);
        return OS_EOK;
    }

    hashtable_remove(
            &directory->Children,
            &(struct __VFSChild) { .Key = node->Name }
    );
    VFSNodeDestroy(node);
    return OS_EOK;
}

// Step 2 for a move to another filesystem. Runs with the source directory
// write locked. Opens the source file and enters its VFS, so the file can be
// read once the directory is released. Undo with __CloseSourceFile.
static oserr_t
__OpenSourceFile(
        _In_  struct VFS*              vfs,
        _In_  struct __MoveRequest*    request,
        _Out_ struct __MoveSourceFile* sourceOut)
{
    oserr_t oserr;

    // The source directory is held, so the VFS is still allocated here.
    oserr = VFSEnter(vfs);
    if (oserr != OS_EOK) {
        return oserr;
    }

    oserr = vfs->Interface->Operations.Open(
            vfs->Interface,
            vfs->Data,
            request->SourcePath,
            &sourceOut->Data
    );
    if (oserr != OS_EOK) {
        ERROR("__OpenSourceFile failed to open %ms: %u", request->SourcePath, oserr);
        VFSLeave(vfs);
        return oserr;
    }
    sourceOut->VFS = vfs;
    return OS_EOK;
}

static void
__CloseSourceFile(
        _In_ struct __MoveSourceFile* source)
{
    oserr_t oserr;

    oserr = source->VFS->Interface->Operations.Close(
            source->VFS->Interface,
            source->VFS->Data,
            source->Data
    );
    if (oserr != OS_EOK) {
        WARNING("__CloseSourceFile failed to close the source file: %u", oserr);
    }
    VFSLeave(source->VFS);
}

// Step 2. Write locks the source directory and the source, checks the
// source, and either moves it within its filesystem or opens it for a copy
// to another filesystem. kindOut tells which. sourceOut is only set for
// MOVE_KIND_ACROSS.
static oserr_t
__HandleSource(
        _In_  struct VFS*              vfs,
        _In_  struct __MoveRequest*    request,
        _In_  bool                     copy,
        _Out_ enum __MoveKind*         kindOut,
        _Out_ struct __MoveSourceFile* sourceOut)
{
    struct VFSNode* directory;
    struct VFSNode* node;
    oserr_t         oserr;

    oserr = __LockSource(vfs, request, &directory, &node);
    if (oserr != OS_EOK) {
        return oserr;
    }

    oserr = __VerifySource(node, copy);
    if (oserr != OS_EOK) {
        TRACE("__HandleSource %ms cannot be moved: %u", request->SourceName, oserr);
        goto unlock;
    }

    request->SourcePath = VFSNodeMakePath(node, 1);
    if (request->SourcePath == NULL) {
        oserr = OS_EOOM;
        goto unlock;
    }
    request->Flags       = node->Stats.Flags;
    request->Permissions = node->Stats.Permissions;
    request->Size        = node->Stats.Size;

    if (directory->FileSystem == request->TargetVFS) {
        *kindOut = MOVE_KIND_LOCAL;
        oserr = __MoveWithinFileSystem(directory, node, request, copy);
        __UnlockDirectory(directory);
        return oserr;
    }

    *kindOut = MOVE_KIND_ACROSS;
    oserr = __OpenSourceFile(directory->FileSystem, request, sourceOut);

unlock:
    usched_rwlock_w_unlock(&node->Lock);
    __UnlockDirectory(directory);
    return oserr;
}

// Looks up the target directory again and write locks it. It must still
// belong to the filesystem that step 1 found. On success release it with
// __UnlockDirectory. On failure nothing is held.
static oserr_t
__LockTarget(
        _In_  struct VFS*           vfs,
        _In_  struct __MoveRequest* request,
        _Out_ struct VFSNode**      directoryOut)
{
    struct VFSNode* directory;
    oserr_t         oserr;

    oserr = VFSNodeGet(vfs, request->TargetDirectoryPath, 1, &directory);
    if (oserr != OS_EOK) {
        return oserr;
    }

    if (directory->FileSystem != request->TargetVFS) {
        VFSNodePut(directory);
        return OS_ENOENT;
    }

    usched_rwlock_w_promote(&directory->Lock);
    *directoryOut = directory;
    return OS_EOK;
}

// Adds the node of the new file. The target directory must be write locked.
static oserr_t
__AddNode(
        _In_ struct VFSNode*       directory,
        _In_ struct __MoveRequest* request)
{
    struct VFSNode* node;

    // A node already exists when the directory was loaded from disk after
    // the file was created there.
    if (hashtable_get(&directory->Children, &(struct __VFSChild) { .Key = request->TargetName }) != NULL) {
        return OS_EOK;
    }

    // VFSNodeChildNew copies the name.
    return VFSNodeChildNew(directory->FileSystem, directory, &(struct VFSStat) {
            .Name = request->TargetName,
            .Size = request->Size,
            .Owner = 0,
            .Flags = request->Flags,
            .Permissions = request->Permissions
    }, &node);
}

// Step 3 within one filesystem. The file is already moved on disk.
static oserr_t
__AddTargetNode(
        _In_ struct VFS*           vfs,
        _In_ struct __MoveRequest* request)
{
    struct VFSNode* directory;
    oserr_t         oserr;

    oserr = __LockTarget(vfs, request, &directory);
    if (oserr != OS_EOK) {
        return oserr;
    }

    oserr = __AddNode(directory, request);
    __UnlockDirectory(directory);
    return oserr;
}

// Copies the data of the open source file to the open target file, through
// one SHM buffer of 1 MB. copiedOut receives the number of bytes copied.
static oserr_t
__TransferFile(
    _In_  struct VFS* sourceVFS,
    _In_  void*       sourceFile,
    _In_  struct VFS* targetVFS,
    _In_  void*       targetFile,
    _Out_ uint64_t*   copiedOut)
{
    OSHandle_t    shm;
    SHMTransfer_t source = { .RangeMode = SHMTransferRangeFull };
    SHMTransfer_t target = { .RangeMode = SHMTransferRangeSubrange };
    size_t        read;
    size_t        written;
    oserr_t       status;
    oserr_t       revokeStatus;

    GrantTokenOptions_t options = {
        .ResourceType = GrantTokenResourceTypeSHM,
        .TargetType = GrantTokenTargetTypeAny,
        .TargetGlobalHandleID = UUID_INVALID,
        .Scope = GrantTokenScopeRead | GrantTokenScopeWrite,
        .Data.SHM = {
            .Offset = 0,
            .Length = MB(1)
        }
    };

    status = SHMCreate(
        &(SHM_t) {
            .Flags = SHM_COMMIT | SHM_CLEAN,
            .Size = MB(1),
            .Access = SHM_ACCESS_READ | SHM_ACCESS_WRITE
        },
        &shm
    );
    if (status != OS_EOK) {
        return status;
    }
    *copiedOut = 0;

    status = OSGrantTokenCreate(shm.ID, &options, &source.Token);
    if (status != OS_EOK) {
        goto cleanup;
    }

    options.Scope = GrantTokenScopeRead;
    status = OSGrantTokenCreate(shm.ID, &options, &target.Token);
    if (status != OS_EOK) {
        goto cleanup;
    }

    while (1) {
        memset(SHMBuffer(&shm), 0, MB(1));

        read = 0;

        status = sourceVFS->Interface->Operations.Read(
            sourceVFS->Interface,
            sourceVFS->Data,
            sourceFile,
            &source,
            &read
        );
        if (status != OS_EOK || read == 0) {
            break;
        }

        if (read > MB(1)) {
            status = OS_EPROTOCOL;
            break;
        }

        target.RelativeOffset = 0;
        target.RequestedLength = (uint32_t)read;
        while (target.RequestedLength != 0) {
            written = 0;
            status = targetVFS->Interface->Operations.Write(
                targetVFS->Interface,
                targetVFS->Data,
                targetFile,
                &target,
                &written
            );
            if (status != OS_EOK) {
                goto cleanup;
            }

            if (written == 0 || written > target.RequestedLength) {
                status = OS_EPROTOCOL;
                goto cleanup;
            }
            target.RelativeOffset += (uint32_t)written;
            target.RequestedLength -= (uint32_t)written;
            *copiedOut += written;
        }
    }

cleanup:
    if (target.Token.ControlHandleID != UUID_INVALID) {
        revokeStatus = OSGrantTokenRevoke(target.Token.ControlHandleID);
        if (revokeStatus != OS_EOK) {
            WARNING("__TransferFile failed to revoke the target token: %u", revokeStatus);
        }
    }
    if (source.Token.ControlHandleID != UUID_INVALID) {
        revokeStatus = OSGrantTokenRevoke(source.Token.ControlHandleID);
        if (revokeStatus != OS_EOK) {
            WARNING("__TransferFile failed to revoke the source token: %u", revokeStatus);
        }
    }
    OSHandleDestroy(&shm);
    return status;
}

// Creates the file in the target directory, which must be write locked, and
// copies the data from the source. A failed copy removes the file again, so a
// failed move leaves only the source.
static oserr_t
__CreateCopy(
        _In_ struct VFSNode*          directory,
        _In_ struct __MoveRequest*    request,
        _In_ struct __MoveSourceFile* source)
{
    struct VFS*           vfs = directory->FileSystem;
    struct VFSOperations* ops = &vfs->Interface->Operations;
    mstring_t*            directoryPath;
    void*                 directoryData;
    void*                 fileData;
    oserr_t               oserr;
    oserr_t               closeStatus;
    oserr_t               unlinkStatus;

    directoryPath = VFSNodeMakePath(directory, 1);
    if (directoryPath == NULL) {
        return OS_EOOM;
    }

    oserr = ops->Open(vfs->Interface, vfs->Data, directoryPath, &directoryData);
    mstr_delete(directoryPath);
    if (oserr != OS_EOK) {
        ERROR("__CreateCopy failed to open the target directory: %u", oserr);
        return oserr;
    }

    oserr = ops->Create(
            vfs->Interface,
            vfs->Data,
            directoryData,
            request->TargetName,
            0,
            request->Flags,
            request->Permissions,
            &fileData
    );
    closeStatus = ops->Close(vfs->Interface, vfs->Data, directoryData);
    if (closeStatus != OS_EOK) {
        WARNING("__CreateCopy failed to close the target directory: %u", closeStatus);
    }
    if (oserr != OS_EOK) {
        ERROR("__CreateCopy failed to create %ms: %u", request->TargetPath, oserr);
        return oserr;
    }

    // A file that fails to close may not have all its data written, so that
    // fails the copy as well. The node of the copy gets the size that was
    // copied, which is the size on disk.
    oserr = __TransferFile(source->VFS, source->Data, vfs, fileData, &request->Size);
    closeStatus = ops->Close(vfs->Interface, vfs->Data, fileData);
    if (closeStatus != OS_EOK) {
        WARNING("__CreateCopy failed to close %ms: %u", request->TargetPath, closeStatus);
        if (oserr == OS_EOK) {
            oserr = closeStatus;
        }
    }
    if (oserr == OS_EOK) {
        return OS_EOK;
    }

    ERROR("__CreateCopy failed to copy the data to %ms: %u", request->TargetPath, oserr);
    unlinkStatus = ops->Unlink(vfs->Interface, vfs->Data, request->TargetPath);
    if (unlinkStatus != OS_EOK) {
        ERROR("__CreateCopy failed to remove the incomplete copy %ms: %u", request->TargetPath, unlinkStatus);
    }
    return oserr;
}

// Step 3 for a move to another filesystem. The target name is checked again,
// as it may have been taken since step 1.
static oserr_t
__CopyAcross(
        _In_ struct VFS*              vfs,
        _In_ struct __MoveRequest*    request,
        _In_ struct __MoveSourceFile* source)
{
    struct VFSNode* directory;
    oserr_t         oserr;

    oserr = __LockTarget(vfs, request, &directory);
    if (oserr != OS_EOK) {
        return oserr;
    }

    if (hashtable_get(&directory->Children, &(struct __VFSChild) { .Key = request->TargetName }) != NULL) {
        oserr = OS_EEXISTS;
        goto unlock;
    }

    oserr = __CreateCopy(directory, request, source);
    if (oserr != OS_EOK) {
        goto unlock;
    }

    oserr = __AddNode(directory, request);
    if (oserr != OS_EOK) {
        ERROR("__CopyAcross failed to add the node of %ms: %u", request->TargetPath, oserr);
    }

unlock:
    __UnlockDirectory(directory);
    return oserr;
}

oserr_t VFSNodeMove(struct VFS* vfs, const char* cfrom, const char* cto, bool copy)
{
    struct __MoveRequest    request = { 0 };
    struct __MoveSourceFile source;
    enum __MoveKind         kind;
    oserr_t                 oserr;
    TRACE("VFSNodeMove(from=%s, to=%s, copy=%i)", cfrom, cto, copy);

    oserr = __PrepareRequest(cfrom, cto, &request);
    if (oserr != OS_EOK) {
        goto cleanup;
    }

    oserr = __CheckTarget(vfs, &request);
    if (oserr != OS_EOK) {
        goto cleanup;
    }

    oserr = __HandleSource(vfs, &request, copy, &kind, &source);
    if (oserr != OS_EOK) {
        goto cleanup;
    }

    // Within one filesystem the file is already moved, only the node of the
    // target is missing. A failure here leaves the file moved on disk, but
    // not visible until the directory is loaded again.
    if (kind == MOVE_KIND_LOCAL) {
        oserr = __AddTargetNode(vfs, &request);
        if (oserr != OS_EOK) {
            ERROR("VFSNodeMove moved %s to %s, but failed to add its node: %u", cfrom, cto, oserr);
        }
        goto cleanup;
    }

    oserr = __CopyAcross(vfs, &request, &source);
    __CloseSourceFile(&source);
    if (oserr != OS_EOK || copy) {
        goto cleanup;
    }

    // The copy is complete, now the source goes. If it was opened since step
    // 2, the unlink fails and both files remain.
    oserr = VFSNodeUnlink(vfs, cfrom);
    if (oserr != OS_EOK) {
        ERROR("VFSNodeMove copied %s to %s, but failed to remove the source: %u", cfrom, cto, oserr);
    }

cleanup:
    __DestroyRequest(&request);
    return oserr;
}
