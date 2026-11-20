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
 */

//#define __TRACE

#include <ddk/utils.h>
#include <os/handle.h>
#include <os/grants.h>
#include <os/shm.h>
#include <vfs/vfs.h>
#include <stdlib.h>
#include <string.h>
#include "private.h"

static mstring_t g_rootToken   = mstr_const("/");
static mstring_t g_dotToken    = mstr_const(".");
static mstring_t g_dotdotToken = mstr_const("..");

mstring_t* VFSNodeMakePath(struct VFSNode* node, int local)
{
    struct VFSNode* i;
    int             tokenCount = 0;
    mstring_t**     tokens;
    TRACE("VFSNodeMakePath(node=%ms, local=%i)", node ? node->Name : NULL, local);

    if (node == NULL) {
        return NULL;
    }

    i = node;
    do {
        tokenCount++;
        if (local && !mstr_cmp(&g_rootToken, i->Name)) {
            break;
        }
        i = i->Parent;
    } while (i);

    // No need to do any combining if we only have one token. The traversal
    // cursor may already be NULL after walking past a root node.
    if (tokenCount == 1) {
        return mstr_clone(node->Name);
    }

    tokens = malloc(sizeof(mstring_t*) * tokenCount);
    if (tokens == NULL) {
        return NULL;
    }
    int index = 0;
    i = node;
    while (index < tokenCount) {
        tokens[tokenCount - index - 1] = i->Name;
        i = i->Parent;
        index++;
    }

    mstring_t* path = mstr_path_tokens_join(tokens, tokenCount);
    free(tokens);
    return path;
}

static void __ToVFSStat(struct VFSDirectoryEntry* in, struct VFSStat* out)
{
    const char* name = (const char*)((uint8_t*)in + sizeof(struct VFSDirectoryEntry));
    const char* link = (const char*)((uint8_t*)in + sizeof(struct VFSDirectoryEntry) + in->NameLength);

    memset(out, 0, sizeof(struct VFSStat));
    if (in->NameLength) {
        out->Name = mstr_new_u8(name);
    }
    if (in->LinkLength) {
        out->LinkTarget = mstr_new_u8(link);
    }
    out->Owner = in->UserID;
    out->Permissions = in->Permissions;
    out->Flags = in->Flags;
    out->Size = in->Size;

    out->Accessed.Seconds = in->Accessed.Seconds;
    out->Accessed.Nanoseconds = in->Accessed.Nanoseconds;
    out->Modified.Seconds = in->Modified.Seconds;
    out->Modified.Nanoseconds = in->Modified.Nanoseconds;
    out->Created.Seconds = in->Created.Seconds;
    out->Created.Nanoseconds = in->Created.Nanoseconds;
}

static void __CleanupVFSStat(struct VFSStat* stats)
{
    mstr_delete(stats->Name);
    mstr_delete(stats->LinkTarget);
}

static oserr_t __ParseEntries(struct VFSNode* node, void* buffer, size_t length)
{
    uint8_t*        i = buffer;
    size_t          bytesAvailable = length;
    struct VFSNode* result;

    while (bytesAvailable) {
        struct VFSDirectoryEntry* entry = (struct VFSDirectoryEntry*)i;
        size_t                    entryLength = sizeof(struct VFSDirectoryEntry) + entry->NameLength + entry->LinkLength;
        oserr_t                   osStatus;
        struct VFSStat            stats;

        // An entry that already has a node was created through the VFS before
        // this directory was loaded (VFSNodeCreateChild). That node is kept, as
        // it may have open handles or something mounted on it. A new node
        // would replace it in Children and leave it unreachable.
        __ToVFSStat(entry, &stats);
        if (stats.Name != NULL &&
            hashtable_get(&node->Children, &(struct __VFSChild) { .Key = stats.Name }) != NULL) {
            osStatus = OS_EOK;
        } else {
            osStatus = VFSNodeChildNew(
                node->FileSystem,
                node,
                &stats,
                &result
            );
        }

        __CleanupVFSStat(&stats);
        if (osStatus != OS_EOK) {
            return osStatus;
        }
        bytesAvailable -= entryLength;
        i += entryLength;
    }
    return OS_EOK;
}

static oserr_t __LoadNode(struct VFSNode* node)
{
    struct VFSOperations* ops;
    struct VFS*           vfs;
    oserr_t               osStatus, osStatus2;
    mstring_t*            nodePath;
    void*                 data;
    SHMTransfer_t         transfer = { .RangeMode = SHMTransferRangeFull };
    size_t                read;

    TRACE("__LoadNode(node=%ms)", node ? node->Name : NULL);
    if (node == NULL) {
        return OS_EINVALPARAMS;
    }

    vfs = node->FileSystem;
    ops = &vfs->Interface->Operations;

    nodePath = VFSNodeMakePath(node, 1);
    if (nodePath == NULL) {
        return OS_EOOM;
    }

    osStatus = ops->Open(vfs->Interface, vfs->Data, nodePath, &data);
    if (osStatus != OS_EOK) {
        TRACE("__LoadNode failed to open nodePath %ms: %u", nodePath);
        goto cleanup;
    }

    osStatus = OSGrantTokenCreate(
        vfs->Buffer.ID,
        &(GrantTokenOptions_t) {
            .ResourceType = GrantTokenResourceTypeSHM,
            .TargetType = GrantTokenTargetTypeAny,
            .TargetGlobalHandleID = UUID_INVALID,
            .Scope = GrantTokenScopeRead | GrantTokenScopeWrite,
            .Data.SHM = { .Offset = 0, .Length = (uint32_t)SHMBufferCapacity(&vfs->Buffer) }
        },
        &transfer.Token
    );
    if (osStatus != OS_EOK) {
        goto close;
    }

    while (1) {
        read = 0;
        
        memset(SHMBuffer(&vfs->Buffer), 0, SHMBufferLength(&vfs->Buffer));

        osStatus = ops->Read(
                vfs->Interface,
                vfs->Data, data,
                &transfer,
                &read
        );
        if (osStatus != OS_EOK || read == 0) {
            TRACE("__LoadNode done reading entries");
            break;
        }

        if (read > SHMBufferLength(&vfs->Buffer)) {
            osStatus = OS_EPROTOCOL;
            break;
        }

        osStatus = __ParseEntries(node, SHMBuffer(&vfs->Buffer), read);
        if (osStatus != OS_EOK) {
            break;
        }
    }

    OSGrantTokenRevoke(transfer.Token.ControlHandleID);
close:
    osStatus2 = ops->Close(vfs->Interface, vfs->Data, data);
    if (osStatus2 != OS_EOK) {
        WARNING("__LoadNode failed to cleanup handle with code %u", osStatus2);
    }

cleanup:
    mstr_delete(nodePath);
    return osStatus;
}

// VFSNodeEnsureLoaded loads all directory entries of the given node. If
// the node given is not a directory, then this function is a no-op.
// Assumes that a reader lock is held upon entry of the function.
oserr_t VFSNodeEnsureLoaded(struct VFSNode* node)
{
    oserr_t osStatus;
    TRACE("VFSNodeEnsureLoaded(node=%ms)", node->Name);

    if (node == NULL) {
        return OS_EINVALPARAMS;
    }

    if (!__NodeIsDirectory(node)) {
        return OS_EOK;
    }

    if (node->IsLoaded) {
        return OS_EOK;
    }

    osStatus = OS_EOK;
    usched_rwlock_w_promote(&node->Lock);
    // do another check while holding the lock
    if (!node->IsLoaded) {
        osStatus = __LoadNode(node);
        if (osStatus != OS_EOK) {
            usched_rwlock_w_demote(&node->Lock);
            return osStatus;
        }
        node->IsLoaded = true;
    }
    usched_rwlock_w_demote(&node->Lock);
    return osStatus;
}

oserr_t VFSNodeFind(struct VFSNode* node, mstring_t* name, struct VFSNode** nodeOut)
{
    struct __VFSChild* result;
    oserr_t            oserr;

    TRACE("VFSNodeFind(node=%ms, name=%ms)", node ? node->Name : NULL, name);
    if (node == NULL || name == NULL || nodeOut == NULL) {
        return OS_EINVALPARAMS;
    }

    // check once while having the reader lock only, this is a performance optimization,
    // so we don't on following checks acquire the writer lock for nothing
    oserr = VFSNodeEnsureLoaded(node);
    if (oserr != OS_EOK) {
        return oserr;
    }

    result = hashtable_get(
            &node->Children,
            &(struct __VFSChild) {
                .Key = name
            }
    );
    if (result == NULL) {
        return OS_ENOENT;
    }

    *nodeOut = result->Node;
    return OS_EOK;
}

oserr_t VFSNodeCreateChild(struct VFSNode* node, mstring_t* name, uint32_t flags, uint32_t permissions, struct VFSNode** nodeOut)
{
    struct VFSOperations* ops;
    struct VFS*           vfs;
    struct __VFSChild*    result;
    oserr_t               osStatus, osStatus2;
    mstring_t*            nodePath;
    void*                 parentData;
    void*                 childData;
    TRACE("VFSNodeCreateChild(node=%ms, name=%ms, flags=0x%x, perms=0x%x)",
          node ? node->Name : NULL, name, flags, permissions);

    if (node == NULL || name == NULL || nodeOut == NULL) {
        return OS_EINVALPARAMS;
    }

    vfs = node->FileSystem;
    ops = &vfs->Interface->Operations;
    nodePath = VFSNodeMakePath(node, 1);
    if (nodePath == NULL) {
        return OS_EOOM;
    }

    TRACE("VFSNodeCreateChild nodePath=%ms", nodePath);
    usched_rwlock_w_promote(&node->Lock);

    // make sure the first we do is verify it still does not exist
    result = hashtable_get(&node->Children, &(struct __VFSChild) { .Key = name });
    if (result != NULL) {
        usched_rwlock_w_demote(&node->Lock);
        mstr_delete(nodePath);
        *nodeOut = result->Node;
        return OS_EEXISTS;
    }

    osStatus = ops->Open(vfs->Interface, vfs->Data, nodePath, &parentData);
    if (osStatus != OS_EOK) {
        goto cleanup;
    }

    osStatus = ops->Create(vfs->Interface, vfs->Data, parentData, name, 0, flags, permissions, &childData);
    if (osStatus != OS_EOK) {
        goto close;
    }

    osStatus = VFSNodeChildNew(node->FileSystem, node, &(struct VFSStat) {
            .Name = mstr_clone(name),
            .Size = 0,
            .Owner = 0,
            .Flags = flags,
            .Permissions = permissions
    }, nodeOut);

    osStatus2 = ops->Close(vfs->Interface, vfs->Data, childData);
    if (osStatus2 != OS_EOK) {
        WARNING("VFSNodeCreateChild failed to cleanup handle with code %u", osStatus2);
    }

close:
    osStatus2 = ops->Close(vfs->Interface, vfs->Data, parentData);
    if (osStatus2 != OS_EOK) {
        WARNING("VFSNodeCreateChild failed to cleanup handle with code %u", osStatus2);
    }

cleanup:
    usched_rwlock_w_demote(&node->Lock);
    mstr_delete(nodePath);
    return osStatus;
}

oserr_t VFSNodeCreateLinkChild(struct VFSNode* node, mstring_t* name, mstring_t* target, int symbolic, struct VFSNode** nodeOut)
{
    struct VFSOperations* ops;
    struct VFS*           vfs;
    struct __VFSChild*    result;
    oserr_t               oserr, oserr2;
    mstring_t*            nodePath;
    void*                 data;

    TRACE("VFSNodeCreateLinkChild(node=%ms, name=%ms, symbolic=%i)",
          node ? node->Name : NULL, name, symbolic);
    if (node == NULL || name == NULL || nodeOut == NULL) {
        return OS_EINVALPARAMS;
    }

    vfs = node->FileSystem;
    ops = &vfs->Interface->Operations;
    nodePath = VFSNodeMakePath(node, 1);
    if (nodePath == NULL) {
        return OS_EOOM;
    }

    TRACE("VFSNodeCreateLinkChild nodePath=%ms", nodePath);
    usched_rwlock_w_promote(&node->Lock);

    // make sure the first we do is verify it still does not exist
    result = hashtable_get(&node->Children, &(struct __VFSChild) { .Key = name });
    if (result != NULL) {
        oserr = OS_EEXISTS;
        goto cleanup;
    }

    oserr = ops->Open(vfs->Interface, vfs->Data, nodePath, &data);
    if (oserr != OS_EOK) {
        goto cleanup;
    }

    oserr = ops->Link(vfs->Interface, vfs->Data, data, name, target, symbolic);
    if (oserr != OS_EOK) {
        goto close;
    }

    oserr = VFSNodeChildNew(node->FileSystem, node, &(struct VFSStat) {
            .Name = mstr_clone(name),
            .Size = mstr_bsize(target),
            .Owner = 0,
            .Flags = FILE_FLAG_LINK,
            .Permissions = FILE_PERMISSION_READ | FILE_PERMISSION_WRITE | FILE_PERMISSION_EXECUTE
    }, nodeOut);

close:
    oserr2 = ops->Close(vfs->Interface, vfs->Data, data);
    if (oserr2 != OS_EOK) {
        WARNING("__CreateInNode failed to cleanup handle with code %u", oserr2);
    }

cleanup:
    usched_rwlock_w_demote(&node->Lock);
    mstr_delete(nodePath);
    return oserr;
}

struct __HandleExcCheckContext {
    uint32_t AccessKind;
    bool     Success;
};

static inline bool __IsAccessKindExclusive(uint32_t accessKind)
{
    // Exclusive read access?
    if ((accessKind & (__FILE_READ_ACCESS | __FILE_READ_SHARE)) == __FILE_READ_ACCESS) {
        return true;
    }

    // Exclusive write access?
    if ((accessKind & (__FILE_WRITE_ACCESS | __FILE_WRITE_SHARE)) == __FILE_WRITE_ACCESS) {
        return true;
    }
    return false;
}

static void __VerifyHandleExclusivityEnum(int index, const void* element, void* userContext)
{
    struct __HandleExcCheckContext* context = userContext;
    const struct __VFSHandle*       handle  = element;
    _CRT_UNUSED(index);

    if (context->Success == false) {
        return;
    }

    if (__IsAccessKindExclusive(context->AccessKind)) {
        context->Success = false;
        return;
    }

    if (__IsAccessKindExclusive(handle->AccessKind)) {
        context->Success = false;
        return;
    }
}

oserr_t
VFSNodeOpenHandle(
    _In_  struct VFSNode* node,
    _In_  uint32_t        accessKind,
    _Out_ uuid_t*         handleOut)
{
    struct __HandleExcCheckContext context;
    struct __VFSHandle             vfsHandle;
    OSHandle_t                     resource;
    void*                          data;
    mstring_t*                     nodePath;
    oserr_t                        oserr;
    oserr_t                        closeStatus;

    nodePath = VFSNodeMakePath(node, 1);
    if (nodePath == NULL) {
        return OS_EOOM;
    }

    usched_mtx_lock(&node->HandlesLock);
    context.Success = true;
    context.AccessKind = accessKind;
    hashtable_enumerate(&node->Handles, __VerifyHandleExclusivityEnum, &context);
    if (!context.Success) {
        oserr = OS_EPERMISSIONS;
        goto cleanup;
    }

    oserr = VFSNodeHandleCreate(accessKind, &resource);
    if (oserr != OS_EOK) {
        goto cleanup;
    }

    oserr = node->FileSystem->Interface->Operations.Open(
        node->FileSystem->Interface,
        node->FileSystem->Data,
        nodePath,
        &data
    );
    if (oserr != OS_EOK) {
        goto resource;
    }

    // Tables contain stable resource IDs, not resource references. The local
    // handle registry keeps the initial reference until close or token handoff.
    vfsHandle.Id = resource.ID;
    vfsHandle.AccessKind = accessKind;
    hashtable_set(&node->Handles, &vfsHandle);
    if (hashtable_get(&node->Handles, &vfsHandle) == NULL) {
        oserr = OS_EOOM;
        goto file;
    }

    oserr = VFSNodeHandleAdd(&resource, node, data, accessKind);
    if (oserr != OS_EOK) {
        hashtable_remove(&node->Handles, &vfsHandle);
        goto file;
    }
    *handleOut = resource.ID;
    goto cleanup;

file:
    closeStatus = node->FileSystem->Interface->Operations.Close(
        node->FileSystem->Interface,
        node->FileSystem->Data,
        data
    );
    if (closeStatus != OS_EOK) {
        WARNING("VFSNodeOpenHandle failed to close the file again: %u", closeStatus);
    }

resource:
    // A cleanup event for an unpublished record is harmless: no table entry
    // exists and all filesystem state has already been closed here.
    OSHandleDestroy(&resource);

cleanup:
    usched_mtx_unlock(&node->HandlesLock);
    mstr_delete(nodePath);
    return oserr;
}

oserr_t VFSNodeNewDirectory(struct VFS* vfs, mstring_t* path, uint32_t permissions, struct VFSNode** nodeOut)
{
    struct VFSNode* baseDirectoryNode;
    mstring_t*      directoryPath;
    mstring_t*      directoryName;
    oserr_t         osStatus;

    TRACE("VFSNodeNewDirectory(path=%ms, perms=0x%x)", path, permissions);
    if (vfs == NULL || path == NULL || nodeOut == NULL) {
        return OS_EINVALPARAMS;
    }

    directoryPath = mstr_path_dirname(path);
    if (directoryPath == NULL) {
        return OS_EOOM;
    }

    directoryName = mstr_path_basename(path);
    if (directoryName == NULL) {
        mstr_delete(directoryPath);
        return OS_EOOM;
    }

    osStatus = VFSNodeGet(vfs, directoryPath, 1, &baseDirectoryNode);
    if (osStatus != OS_EOK) {
        mstr_delete(directoryPath);
        mstr_delete(directoryName);
        return osStatus;
    }

    osStatus = VFSNodeCreateChild(
            baseDirectoryNode,
            directoryName,
            FILE_FLAG_DIRECTORY,
            permissions,
            nodeOut
    );

    VFSNodePut(baseDirectoryNode);
    mstr_delete(directoryPath);
    mstr_delete(directoryName);
    return osStatus;
}

// Enters the VFS of a node and then takes a reader lock on the node. The VFS
// must still be allocated, so the caller must hold a lock or VFS entry that
// keeps the node reachable. Every successful call is undone by __UnlockNode.
// Fails with OS_ENOENT when the VFS is being destroyed. The node is not
// touched then, as VFSDestroy may free it as soon as it stops waiting.
static oserr_t
__LockNode(
        _In_ struct VFSNode* node)
{
    oserr_t oserr;

    oserr = VFSEnter(node->FileSystem);
    if (oserr != OS_EOK) {
        TRACE("__LockNode filesystem of %ms is being destroyed", node->Name);
        return oserr;
    }
    usched_rwlock_r_lock(&node->Lock);
    return OS_EOK;
}

// Releases the reader lock and the VFS entry __LockNode took. The VFS pointer
// is read before the unlock, and VFSLeave is the last use of the node.
static void
__UnlockNode(
        _In_ struct VFSNode* node)
{
    struct VFS* vfs = node->FileSystem;

    usched_rwlock_r_unlock(&node->Lock);
    VFSLeave(vfs);
}

// Resolves a path relative to a node. Each node the walk stands on is held
// with __LockNode, so the VFS it belongs to cannot be destroyed under the
// walk. When the walk moves to a node in another VFS (a bind or a mounted
// filesystem), it enters that VFS before it releases the node it came from.
// If that VFS is being destroyed, the walk fails with OS_ENOENT.
// On success the returned node is held with __LockNode. On failure nothing is
// held, and nodeOut is not set.
oserr_t __GetRelative(struct VFSNode* from, mstring_t* path, int followLinks, struct VFSNode** nodeOut)
{
    oserr_t         oserr;
    mstring_t**     tokens;
    int             tokenCount;
    struct VFSNode* node = from;
    struct VFSNode* next;
    ENTRY("__GetRelative(path=%ms, followLinks=1)", path, followLinks);

    tokenCount = mstr_path_tokens(path, &tokens);
    if (tokenCount < 0) {
        return OS_EOOM;
    }

    oserr = __LockNode(node);
    if (oserr != OS_EOK) {
        mstrv_delete(tokens);
        return oserr;
    }

    for (int i = 0; i < tokenCount; i++) {
        TRACE("__GetRelative token[%i] = %ms", i, tokens[i]);
        // Assumptions on entry of this loop.
        // node => current node, held with __LockNode
        // next => not used on entry

        // Handle '.' and '..'. The parent is always in the same VFS, the root
        // node of a mounted filesystem has no parent.
        if (!mstr_cmp(tokens[i], &g_dotToken)) {
            continue;
        } else if (!mstr_cmp(tokens[i], &g_dotdotToken)) {
            if (node->Parent) {
                next = node->Parent;
                oserr = __LockNode(next);
                if (oserr != OS_EOK) {
                    break;
                }
                __UnlockNode(node);
                node = next;
            }
            continue;
        }

        // Find the next entry in the folder. This will automatically handle
        // folder loading and whatnot if the folder is not currently loaded.
        oserr = VFSNodeFind(node, tokens[i], &next);
        if (oserr != OS_EOK) {
            TRACE("__GetRelative failed to find %ms in %ms", tokens[i], node->Name);
            break;
        }

        // Hold next while we 'process' next
        oserr = __LockNode(next);
        if (oserr != OS_EOK) {
            break;
        }

        // If a node is a not a regular file, but instead a redirect, then we must resolve
        // the redirect before continuing with the provided path. We must do this in a loop
        // as it is possible for these redirects to be chained. Every branch either
        // replaces next with a held node, or releases next and sets oserr.
        for (;;) {
            if (__NodeIsSymlink(next)) {
                struct VFSNode* real;

                // The last element is not followed when the caller asked for
                // the link itself.
                if (i == (tokenCount - 1) && !followLinks) {
                    break;
                }

                // OK in all other cases we *must* follow the symlink
                // NOTE: we now end up with two reader locks on 'node'
                oserr = __GetRelative(node, next->Stats.LinkTarget, followLinks, &real);
                __UnlockNode(next);
                if (oserr != OS_EOK) {
                    break;
                }

                // At this point, we hold 'node' and 'real'
                next = real;
            } else if (__NodeIsBindMount(next)) {
                struct VFSNode* real = next->TypeData;
                if (real == NULL) {
                    // *should* never happen
                    ERROR("__GetRelative discovered node having a NULL type-data but marked as bind mount");
                    oserr = OS_EUNKNOWN;
                    __UnlockNode(next);
                    break;
                }

                // The bind source can be in another VFS. It is entered while
                // the bind is still held, as VFSNodeDestroy removes the bind
                // under its write lock before it frees the source.
                oserr = __LockNode(real);
                __UnlockNode(next);
                if (oserr != OS_EOK) {
                    break;
                }
                next = real;
            } else if (__NodeIsMountPoint(next)) {
                struct VFS*     fs = next->TypeData;
                struct VFSNode* real;
                if (fs == NULL) {
                    // *should* never happen
                    ERROR("__GetRelative discovered node having a NULL type-data but marked as mountpoint");
                    oserr = OS_EUNKNOWN;
                    __UnlockNode(next);
                    break;
                }

                // The mounted VFS is entered while the mount point is still
                // held. VFSFileSystemUnmount clears the mount point under its
                // write lock before VFSDestroy runs, so fs is allocated here.
                real = fs->Root;
                oserr = __LockNode(real);
                __UnlockNode(next);
                if (oserr != OS_EOK) {
                    break;
                }
                next = real;
            } else {
                break;
            }
        }

        // next is no longer held when the redirects failed.
        if (oserr != OS_EOK) {
            break;
        }

        // Move one level down the tree, so we release the current node and
        // move to 'next' which is held by this point
        __UnlockNode(node);
        node = next;
    }

    // Don't keep the node if we failed to get the target node
    mstrv_delete(tokens);
    if (oserr != OS_EOK) {
        __UnlockNode(node);
        return oserr;
    }

    *nodeOut = node;
    EXIT("__GetRelative");
    return OS_EOK;
}

oserr_t VFSNodeGet(struct VFS* vfs, mstring_t* path, int followLinks, struct VFSNode** nodeOut)
{
    if (vfs == NULL || path == NULL || nodeOut == NULL) {
        return OS_EINVALPARAMS;
    }
    return __GetRelative(vfs->Root, path, followLinks, nodeOut);
}

void VFSNodePut(struct VFSNode* node)
{
    if (node == NULL) {
        return;
    }
    __UnlockNode(node);
}
