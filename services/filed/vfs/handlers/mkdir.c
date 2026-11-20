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

#include <vfs/vfs.h>
#include "../private.h"

// Creates a directory and opens a handle on it. The parent directory is held
// (VFSNodeGet) until the handle is open. Releasing it before the open would
// let VFSDestroy free the new node in between, if the filesystem is torn
// down at that moment.
oserr_t VFSNodeMkdir(struct VFS* vfs, mstring_t* path, uint32_t access, uuid_t* handleOut)
{
    struct VFSNode* parent;
    struct VFSNode* node;
    mstring_t*      parentPath;
    mstring_t*      name;
    oserr_t         oserr;

    if (vfs == NULL || path == NULL || handleOut == NULL) {
        return OS_EINVALPARAMS;
    }

    parentPath = mstr_path_dirname(path);
    if (parentPath == NULL) {
        return OS_EOOM;
    }

    name = mstr_path_basename(path);
    if (name == NULL) {
        mstr_delete(parentPath);
        return OS_EOOM;
    }

    oserr = VFSNodeGet(vfs, parentPath, 1, &parent);
    if (oserr != OS_EOK) {
        goto cleanup;
    }

    oserr = VFSNodeCreateChild(
            parent,
            name,
            FILE_FLAG_DIRECTORY,
            access,
            &node
    );
    if (oserr != OS_EOK) {
        goto put;
    }

    oserr = VFSNodeOpenHandle(node, access, handleOut);

put:
    VFSNodePut(parent);

cleanup:
    mstr_delete(parentPath);
    mstr_delete(name);
    return oserr;
}
