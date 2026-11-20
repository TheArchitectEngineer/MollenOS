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
#define __need_quantity
#include <ddk/utils.h>
#include <os/handle.h>
#include <os/shm.h>
#include <os/time.h>
#include <vfs/vfs.h>
#include <vfs/interface.h>
#include <vfs/storage.h>
#include "private.h"
#include <string.h>
#include <stdlib.h>
#include <os/usched/job.h>

static uint64_t __HandlesHash(const void* element);
static int      __HandlesCmp(const void* lh, const void* rh);

static uint64_t __ChildrenHash(const void* element);
static int      __ChildrenCmp(const void* lh, const void* rh);

static uint64_t __MountsHash(const void* element);
static int      __MountsCmp(const void* lh, const void* rh);

static uuid_t g_nodeID = 1;

static oserr_t
__CreateNode(
        _In_  struct VFS*      vfs,
        _In_  enum VFSNodeType type,
        _In_  struct VFSStat*  stats,
        _Out_ struct VFSNode** nodeOut)
{
    struct VFSNode* node;

    node = malloc(sizeof(struct VFSNode));
    if (node == NULL) {
        return OS_EOOM;
    }

    node->FileSystem = vfs;
    node->Parent     = NULL;
    node->Source     = NULL;
    node->Name       = mstr_clone(stats->Name);
    node->Type       = type;
    node->IsLoaded   = false;
    usched_rwlock_init(&node->Lock);

    memcpy(&node->Stats, stats, sizeof(struct VFSStat));
    node->Stats.Name = node->Name;
    node->Stats.StorageID = vfs->Storage->ID;
    node->Stats.ID = g_nodeID++;

    usched_mtx_init(&node->HandlesLock, USCHED_MUTEX_PLAIN);
    hashtable_construct(&node->Handles, 0,
                        sizeof(struct __VFSHandle),
                        __HandlesHash, __HandlesCmp);
    hashtable_construct(&node->Children, 0,
                        sizeof(struct __VFSChild),
                        __ChildrenHash, __ChildrenCmp);
    usched_mtx_init(&node->MountsLock, USCHED_MUTEX_PLAIN);
    hashtable_construct(&node->Mounts, 0,
                        sizeof(struct __VFSMount),
                        __MountsHash, __MountsCmp);
    *nodeOut = node;
    return OS_EOK;
}

static void
__StatRootNode(
        _In_ struct VFS*     vfs,
        _In_ struct VFSStat* stat)
{
    stat->ID = 0;
    stat->StorageID = vfs->Storage->Stats.DeviceID;
    stat->Name = mstr_new_u8("/");
    stat->LinkTarget = NULL;
    stat->Owner = 0;
    stat->Permissions = FILE_PERMISSION_READ | FILE_PERMISSION_WRITE;
    stat->Flags = FILE_FLAG_DIRECTORY;
    stat->Size = 0;

    OSGetTime(OSTimeSource_UTC, &stat->Created);
    stat->Modified.Nanoseconds = stat->Created.Nanoseconds; stat->Modified.Seconds = stat->Created.Seconds;
    stat->Accessed.Nanoseconds = stat->Created.Nanoseconds; stat->Accessed.Seconds = stat->Created.Seconds;
}

static oserr_t
__CreateRootNode(
        _In_  struct VFS*      vfs,
        _Out_ struct VFSNode** nodeOut)
{
    struct VFSNode* root;

    root = malloc(sizeof(struct VFSNode));
    if (root == NULL) {
        return OS_EOOM;
    }

    root->FileSystem = vfs;
    root->Parent     = NULL;
    root->Source     = NULL;
    root->Name       = mstr_new_u8("/");
    root->Type       = VFS_NODE_TYPE_REGULAR;
    root->IsLoaded   = false;
    usched_rwlock_init(&root->Lock);
    __StatRootNode(vfs, &root->Stats);

    usched_mtx_init(&root->HandlesLock, USCHED_MUTEX_PLAIN);
    hashtable_construct(&root->Handles, 0,
                        sizeof(struct __VFSHandle),
                        __HandlesHash, __HandlesCmp);
    hashtable_construct(&root->Children, 0,
                        sizeof(struct __VFSChild),
                        __ChildrenHash, __ChildrenCmp);
    usched_mtx_init(&root->MountsLock, USCHED_MUTEX_PLAIN);
    hashtable_construct(&root->Mounts, 0,
                        sizeof(struct __VFSMount),
                        __MountsHash, __MountsCmp);
    *nodeOut = root;
    return OS_EOK;
}

static oserr_t
__CreateTransferBuffer(
        _In_ OSHandle_t* shmHandle)
{
    return SHMCreate(
            &(SHM_t) {
                .Flags = SHM_COMMIT,
                .Size = MB(1),
                .Access = SHM_ACCESS_READ | SHM_ACCESS_WRITE
            },
            shmHandle
    );
}

oserr_t
VFSNew(
        _In_  uuid_t                id,
        _In_  guid_t*               guid,
        _In_  struct VFSStorage*    storage,
        _In_  struct VFSInterface*  interface,
        _In_  void*                 data,
        _Out_ struct VFS**          vfsOut)
{
    struct VFS* vfs;
    oserr_t     osStatus;

    vfs = malloc(sizeof(struct VFS));
    if (vfs == NULL) {
        return OS_EOOM;
    }
    memset(vfs, 0, sizeof(struct VFS));

    vfs->ID = id;
    memcpy(&vfs->Guid, guid, sizeof(guid_t));
    vfs->Storage = storage;
    vfs->Interface = interface;
    vfs->Data = data;
    vfs->State = VFS_STATE_ACTIVE;
    vfs->Operations = 0;
    usched_mtx_init(&vfs->StateLock, USCHED_MUTEX_PLAIN);
    usched_cnd_init(&vfs->OperationsDone);

    osStatus = __CreateRootNode(vfs, &vfs->Root);
    if (osStatus != OS_EOK) {
        VFSDestroy(vfs);
        return osStatus;
    }

    osStatus = __CreateTransferBuffer(&vfs->Buffer);
    if (osStatus != OS_EOK) {
        VFSDestroy(vfs);
        return osStatus;
    }

    *vfsOut = vfs;
    return OS_EOK;
}

oserr_t
VFSEnter(
        _In_ struct VFS* vfs)
{
    oserr_t oserr = OS_EOK;

    usched_mtx_lock(&vfs->StateLock);
    if (vfs->State != VFS_STATE_ACTIVE) {
        oserr = OS_ENOENT;
    } else {
        vfs->Operations++;
    }
    usched_mtx_unlock(&vfs->StateLock);
    return oserr;
}

void
VFSLeave(
        _In_ struct VFS* vfs)
{
    usched_mtx_lock(&vfs->StateLock);
    if (vfs->Operations == 0) {
        // A leave without a matching enter. Keep the count at zero so a
        // waiting VFSDestroy is not held up forever.
        ERROR("VFSLeave filesystem %u has no operations to end", vfs->ID);
    } else {
        vfs->Operations--;
    }

    // VFSDestroy waits with StateLock released, and can only free the VFS
    // after it has taken StateLock again. This notification and the unlock
    // below are therefore the last uses of vfs here.
    if (vfs->Operations == 0 && vfs->State == VFS_STATE_CLOSING) {
        usched_cnd_notify_all(&vfs->OperationsDone);
    }
    usched_mtx_unlock(&vfs->StateLock);
}

// Stops new operations on the VFS and waits for the ones that have already
// entered it. A lookup that crosses into the VFS after this fails with
// OS_ENOENT. An operation in flight keeps running until it is done. It may be
// waiting for the filesystem driver, which waits for the storage driver. A
// storage driver that has lost its disk fails the requests with an error, so
// the wait ends.
static void
__WaitForOperations(
        _In_ struct VFS* vfs)
{
    usched_mtx_lock(&vfs->StateLock);
    vfs->State = VFS_STATE_CLOSING;
    if (vfs->Operations != 0) {
        NOTICE("VFSDestroy filesystem %u waits for %u operations", vfs->ID, vfs->Operations);
        while (vfs->Operations != 0) {
            usched_cnd_wait(&vfs->OperationsDone, &vfs->StateLock);
        }
        NOTICE("VFSDestroy filesystem %u has no operations left", vfs->ID);
    }
    usched_mtx_unlock(&vfs->StateLock);
}

void VFSDestroy(struct VFS* vfs)
{
    if (vfs == NULL) {
        return;
    }

    // Nodes are only freed once no operation uses them any more. Handlers
    // that work through an open handle instead of a path are not counted.
    // They hold the handle table's reader lock, and VFSNodeDestroy closes
    // every handle, which waits for them.
    __WaitForOperations(vfs);

    // VFSNodeDestroy also undoes binds that point into this VFS, and the
    // records of binds this VFS has into other ones.
    VFSNodeDestroy(vfs->Root);
    OSHandleDestroy(&vfs->Buffer);
    free(vfs);
}

oserr_t VFSChildNew(struct VFS* parent, struct VFS** childOut)
{
    // TODO: implement VFS scopes
    return OS_ENOTSUPPORTED;
}

oserr_t VFSNodeChildNew(struct VFS* vfs, struct VFSNode* node, struct VFSStat* stats, struct VFSNode** nodeOut)
{
    struct VFSNode* result;
    oserr_t         oserr;
    TRACE("VFSNodeChildNew(node=%ms, stats=%ms)", node->Name, stats->Name);

    oserr = __CreateNode(vfs, VFS_NODE_TYPE_REGULAR, stats, &result);
    if (oserr != OS_EOK) {
        return oserr;
    }

    // Link parent and children together. Update parent member and then insert us as child
    result->Parent = node;
    hashtable_set(
            &node->Children,
            &(struct __VFSChild) {
                .Key = result->Name,
                .Node = result
            }
    );
    *nodeOut = result;
    return OS_EOK;
}

static void
__FirstHandle(
    _In_ int         index,
    _In_ const void* element,
    _In_ void*       userContext)
{
    const struct __VFSHandle* handle = element;
    uuid_t*                   id = userContext;

    _CRT_UNUSED(index);
    if (*id == UUID_INVALID) {
        *id = handle->Id;
    }
}

void __CleanupChild(int index, const void* element, void* userContext)
{
    const struct __VFSChild* child = element;
    VFSNodeDestroy(child->Node);
}

static void
__FirstMount(
    _In_ int         index,
    _In_ const void* element,
    _In_ void*       userContext)
{
    const struct __VFSMount* mount = element;
    struct VFSNode**         target = userContext;

    _CRT_UNUSED(index);
    if (*target == NULL) {
        *target = mount->Target;
    }
}

// Undoes every bind that has this node as its source (VFSNodeBind). Each
// target becomes a regular directory again, so a lookup can no longer reach
// the node after it is freed. The locks are taken in the same order as in
// VFSNodeBind and VFSNodeUnbind: the target's node lock first, then the
// source's MountsLock. MountsLock is released before the target is locked, so
// one target is taken at a time.
static void
__RemoveBindsToNode(
    _In_ struct VFSNode* node)
{
    struct VFSNode* target;

    for (;;) {
        target = NULL;
        usched_mtx_lock(&node->MountsLock);
        hashtable_enumerate(&node->Mounts, __FirstMount, &target);
        usched_mtx_unlock(&node->MountsLock);
        if (target == NULL) {
            break;
        }

        // A lookup that holds the target's reader lock may be about to cross
        // into this node. It enters this node's VFS first, which fails while
        // the VFS is closing, so it releases the lock and the write lock
        // here is granted.
        usched_rwlock_w_lock(&target->Lock);
        if (__NodeIsBindMount(target) && target->TypeData == node) {
            target->Type     = VFS_NODE_TYPE_REGULAR;
            target->TypeData = NULL;
        }

        usched_mtx_lock(&node->MountsLock);
        hashtable_remove(&node->Mounts, &(struct __VFSMount) { .Target = target });
        usched_mtx_unlock(&node->MountsLock);
        usched_rwlock_w_unlock(&target->Lock);
        WARNING("VFSNodeDestroy removed the bind of %ms at %ms", node->Name, target->Name);
    }
}

// Removes the record a bind target left in its source node (VFSNodeBind), when
// the target is destroyed. Without this, the source keeps a pointer to the freed
// target, and VFSNodeUnlink refuses to remove the source because it still
// looks bound.
static void
__RemoveBindRecord(
    _In_ struct VFSNode* node)
{
    struct VFSNode* source;

    if (!__NodeIsBindMount(node) || node->TypeData == NULL) {
        return;
    }

    source = node->TypeData;
    usched_mtx_lock(&source->MountsLock);
    hashtable_remove(&source->Mounts, &(struct __VFSMount) { .Target = node });
    usched_mtx_unlock(&source->MountsLock);
    node->Type     = VFS_NODE_TYPE_REGULAR;
    node->TypeData = NULL;
}

void VFSNodeDestroy(struct VFSNode* node)
{
    uuid_t  handleID;
    oserr_t oserr;

    if (node == NULL) {
        return;
    }

    // A cleanup job can have removed the global record while still closing
    // the backend state. Its per-node entry remains until Close finishes.
    // Inspect that entry under HandlesLock, but release the lock before Close
    // promotes the global table lock. Teardown requires no new opens here.
    for (;;) {
        handleID = UUID_INVALID;
        usched_mtx_lock(&node->HandlesLock);
        hashtable_enumerate(&node->Handles, __FirstHandle, &handleID);
        usched_mtx_unlock(&node->HandlesLock);
        if (handleID == UUID_INVALID) {
            break;
        }

        oserr = VFSNodeClose(NULL, handleID);
        if (oserr == OS_ENOENT) {
            // Let the close which removed the global record finish. The next
            // iteration waits for its node lock before inspecting the table.
            usched_job_yield();
        } else if (oserr != OS_EOK) {
            WARNING("VFSNodeDestroy failed to close %u: %u", handleID, oserr);
            return;
        }
    }

    hashtable_enumerate(&node->Children, __CleanupChild, NULL);
    __RemoveBindsToNode(node);
    __RemoveBindRecord(node);

    hashtable_destroy(&node->Handles);
    hashtable_destroy(&node->Children);
    hashtable_destroy(&node->Mounts);

    mstr_delete(node->Name);
    // Stats.Name is a pointer to .Name
    mstr_delete(node->Stats.LinkTarget);
    free(node);
}

static uint64_t __HandlesHash(const void* element)
{
    const struct __VFSHandle* handle = element;
    return handle->Id;
}

static int __HandlesCmp(const void* lh, const void* rh)
{
    const struct __VFSHandle* handle1 = lh;
    const struct __VFSHandle* handle2 = rh;
    return handle1->Id == handle2->Id ? 0 : 1;
}

static uint64_t __ChildrenHash(const void* element)
{
    const struct __VFSChild* child = element;
    return mstr_hash(child->Key);
}

static int __ChildrenCmp(const void* lh, const void* rh)
{
    const struct __VFSChild* child1 = lh;
    const struct __VFSChild* child2 = rh;
    return mstr_cmp(child1->Key, child2->Key);
}

static uint64_t __MountsHash(const void* element)
{
    const struct __VFSMount* child = element;
    return (uint64_t)child->Target;
}

static int __MountsCmp(const void* lh, const void* rh)
{
    const struct __VFSMount* child1 = lh;
    const struct __VFSMount* child2 = rh;

    // Compare the target nodes, so a lookup or removal 
    // with a record built on the stack can find the entry.
    return child1->Target == child2->Target ? 0 : 1;
}
