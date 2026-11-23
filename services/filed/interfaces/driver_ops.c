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

#include <ddk/convert.h>
#include <os/convert/grants.h>
#include <ddk/utils.h>
#include <gracht/link/vali.h>
#include <internal/_gracht.h>
#include <internal/_utils.h>
#include <vfs/interface.h>

#include <ctt_filesystem_service_client.h>

// Every operation below is one request to the filesystem driver, a separate
// process, and runs the same three steps:
// 1. Send the request. When this fails the driver never saw the request.
// 2. Wait for the answer (__AwaitAnswer). This fails when the driver sent an
//    error message instead of an answer, and the request is then dropped.
// 3. Read the answer with the matching _result call.
// The values in an answer are only valid once step 3 succeeds, so nothing
// reads them before. A failed step is logged, and the operation returns
// OS_EUNKNOWN when the request was not sent or not answered, and OS_EPROTOCOL
// when the answer could not be read.

static oserr_t
__RequestNotSent(
        _In_ struct VFSInterface* interface,
        _In_ const char*          operation)
{
    ERROR("%s failed to send the request to driver %u", operation, interface->DriverID);
    return OS_EUNKNOWN;
}

static oserr_t
__AwaitAnswer(
        _In_ struct VFSInterface*      interface,
        _In_ const char*               operation,
        _In_ struct vali_link_message* msg)
{
    oserr_t oserr;

    oserr = __GrachtAwaitCompleted(GetGrachtClient(), &msg->base, GRACHT_AWAIT_ASYNC);
    if (oserr != OS_EOK) {
        ERROR("%s got no answer from driver %u", operation, interface->DriverID);
    }
    return oserr;
}

static oserr_t
__AnswerUnreadable(
        _In_ struct VFSInterface* interface,
        _In_ const char*          operation)
{
    ERROR("%s failed to read the answer of driver %u", operation, interface->DriverID);
    return OS_EPROTOCOL;
}

static oserr_t
__DriverInitialize(
        struct VFSInterface*         interface,
        struct VFSStorageParameters* params,
        void**                       instanceData)
{
    struct vali_link_message   msg = VALI_MSG_INIT_HANDLE(interface->DriverID);
    struct ctt_fs_setup_params cttParams;
    oserr_t                    oserr;
    uintptr_t                  fsctx;

    to_fs_setup_params(params, &cttParams);
    if (ctt_filesystem_setup(GetGrachtClient(), &msg.base, &cttParams) != 0) {
        return __RequestNotSent(interface, "__DriverInitialize");
    }

    oserr = __AwaitAnswer(interface, "__DriverInitialize", &msg);
    if (oserr != OS_EOK) {
        return oserr;
    }

    if (ctt_filesystem_setup_result(GetGrachtClient(), &msg.base, &oserr, &fsctx) != 0) {
        return __AnswerUnreadable(interface, "__DriverInitialize");
    }

    if (oserr == OS_EOK) {
        *instanceData = (void*)fsctx;
    }
    return oserr;
}

static oserr_t
__DriverDestroy(struct VFSInterface* interface, void* instanceData, unsigned int unmountFlags)
{
    struct vali_link_message msg = VALI_MSG_INIT_HANDLE(interface->DriverID);
    oserr_t                  oserr;

    if (ctt_filesystem_destroy(GetGrachtClient(), &msg.base, (uintptr_t)instanceData) != 0) {
        return __RequestNotSent(interface, "__DriverDestroy");
    }

    oserr = __AwaitAnswer(interface, "__DriverDestroy", &msg);
    if (oserr != OS_EOK) {
        return oserr;
    }

    if (ctt_filesystem_destroy_result(GetGrachtClient(), &msg.base, &oserr) != 0) {
        return __AnswerUnreadable(interface, "__DriverDestroy");
    }
    return oserr;
}

static oserr_t
__DriverOpen(struct VFSInterface* interface, void* instanceData, mstring_t* path, void** dataOut)
{
    struct vali_link_message msg = VALI_MSG_INIT_HANDLE(interface->DriverID);
    oserr_t                  oserr;
    uintptr_t                fctx;
    char*                    cpath;
    int                      status;

    cpath = mstr_u8(path);
    if (cpath == NULL) {
        return OS_EOOM;
    }

    status = ctt_filesystem_open(GetGrachtClient(), &msg.base, (uintptr_t)instanceData, cpath);
    free(cpath);
    if (status != 0) {
        return __RequestNotSent(interface, "__DriverOpen");
    }

    oserr = __AwaitAnswer(interface, "__DriverOpen", &msg);
    if (oserr != OS_EOK) {
        return oserr;
    }

    if (ctt_filesystem_open_result(GetGrachtClient(), &msg.base, &oserr, &fctx) != 0) {
        return __AnswerUnreadable(interface, "__DriverOpen");
    }

    if (oserr == OS_EOK) {
        *dataOut = (void*)fctx;
    }
    return oserr;
}

static oserr_t
__DriverCreate(struct VFSInterface* interface, void* instanceData, void* data, mstring_t* name, uint32_t owner, uint32_t flags, uint32_t permissions, void** dataOut)
{
    struct vali_link_message  msg = VALI_MSG_INIT_HANDLE(interface->DriverID);
    oserr_t                   oserr;
    uintptr_t                 fctx;
    struct ctt_fs_open_params params;
    int                       status;

    ctt_fs_open_params_init(&params);
    params.name = mstr_u8(name);
    if (params.name == NULL) {
        return OS_EOOM;
    }
    params.owner = owner;
    params.flags = flags;
    params.permissions = permissions;

    status = ctt_filesystem_create(GetGrachtClient(), &msg.base, (uintptr_t)instanceData, (uintptr_t)data, &params);
    ctt_fs_open_params_destroy(&params);
    if (status != 0) {
        return __RequestNotSent(interface, "__DriverCreate");
    }

    oserr = __AwaitAnswer(interface, "__DriverCreate", &msg);
    if (oserr != OS_EOK) {
        return oserr;
    }

    if (ctt_filesystem_create_result(GetGrachtClient(), &msg.base, &oserr, &fctx) != 0) {
        return __AnswerUnreadable(interface, "__DriverCreate");
    }

    if (oserr == OS_EOK) {
        *dataOut = (void*)fctx;
    }
    return oserr;
}

static oserr_t
__DriverClose(struct VFSInterface* interface, void* instanceData, void* data)
{
    struct vali_link_message msg = VALI_MSG_INIT_HANDLE(interface->DriverID);
    oserr_t                  oserr;

    if (ctt_filesystem_close(GetGrachtClient(), &msg.base, (uintptr_t)instanceData, (uintptr_t)data) != 0) {
        return __RequestNotSent(interface, "__DriverClose");
    }

    oserr = __AwaitAnswer(interface, "__DriverClose", &msg);
    if (oserr != OS_EOK) {
        return oserr;
    }

    if (ctt_filesystem_close_result(GetGrachtClient(), &msg.base, &oserr) != 0) {
        return __AnswerUnreadable(interface, "__DriverClose");
    }
    return oserr;
}

static oserr_t
__DriverStat(struct VFSInterface* interface, void* instanceData, struct VFSStatFS* stats)
{
    struct vali_link_message msg = VALI_MSG_INIT_HANDLE(interface->DriverID);
    oserr_t                  oserr;
    struct ctt_fsstat        cttStats;

    if (ctt_filesystem_fsstat(GetGrachtClient(), &msg.base, (uintptr_t)instanceData) != 0) {
        return __RequestNotSent(interface, "__DriverStat");
    }

    oserr = __AwaitAnswer(interface, "__DriverStat", &msg);
    if (oserr != OS_EOK) {
        return oserr;
    }

    // A failed read releases what it had decoded of cttStats by itself.
    if (ctt_filesystem_fsstat_result(GetGrachtClient(), &msg.base, &oserr, &cttStats) != 0) {
        return __AnswerUnreadable(interface, "__DriverStat");
    }

    if (oserr == OS_EOK) {
        from_fsstat(&cttStats, stats);
    }
    ctt_fsstat_destroy(&cttStats);
    return oserr;
}

static oserr_t
__DriverLink(struct VFSInterface* interface, void* instanceData, void* data, mstring_t* linkName, mstring_t* linkTarget, int symbolic)
{
    struct vali_link_message msg = VALI_MSG_INIT_HANDLE(interface->DriverID);
    oserr_t                  oserr;
    char*                    name;
    char*                    target;
    int                      status;

    name = mstr_u8(linkName);
    if (name == NULL) {
        return OS_EOOM;
    }

    target = mstr_u8(linkTarget);
    if (target == NULL) {
        free(name);
        return OS_EOOM;
    }

    status = ctt_filesystem_link(GetGrachtClient(), &msg.base, (uintptr_t)instanceData, (uintptr_t)data, name, target, symbolic);
    free(name);
    free(target);
    if (status != 0) {
        return __RequestNotSent(interface, "__DriverLink");
    }

    oserr = __AwaitAnswer(interface, "__DriverLink", &msg);
    if (oserr != OS_EOK) {
        return oserr;
    }

    if (ctt_filesystem_link_result(GetGrachtClient(), &msg.base, &oserr) != 0) {
        return __AnswerUnreadable(interface, "__DriverLink");
    }
    return oserr;
}

static oserr_t
__DriverUnlink(struct VFSInterface* interface, void* instanceData, mstring_t* path)
{
    struct vali_link_message msg = VALI_MSG_INIT_HANDLE(interface->DriverID);
    oserr_t                  oserr;
    char*                    cpath;
    int                      status;

    cpath = mstr_u8(path);
    if (cpath == NULL) {
        return OS_EOOM;
    }

    status = ctt_filesystem_unlink(GetGrachtClient(), &msg.base, (uintptr_t)instanceData, cpath);
    free(cpath);
    if (status != 0) {
        return __RequestNotSent(interface, "__DriverUnlink");
    }

    oserr = __AwaitAnswer(interface, "__DriverUnlink", &msg);
    if (oserr != OS_EOK) {
        return oserr;
    }

    if (ctt_filesystem_unlink_result(GetGrachtClient(), &msg.base, &oserr) != 0) {
        return __AnswerUnreadable(interface, "__DriverUnlink");
    }
    return oserr;
}

static oserr_t
__DriverReadlink(struct VFSInterface* interface, void* instanceData, mstring_t* path, mstring_t** pathOut)
{
    struct vali_link_message msg = VALI_MSG_INIT_HANDLE(interface->DriverID);
    oserr_t                  oserr;
    char*                    cpath;
    char*                    buffer;
    int                      status;

    cpath = mstr_u8(path);
    if (cpath == NULL) {
        return OS_EOOM;
    }

    buffer = malloc(_MAXPATH);
    if (buffer == NULL) {
        free(cpath);
        return OS_EOOM;
    }

    status = ctt_filesystem_readlink(GetGrachtClient(), &msg.base, (uintptr_t)instanceData, cpath);
    free(cpath);
    if (status != 0) {
        free(buffer);
        return __RequestNotSent(interface, "__DriverReadlink");
    }

    oserr = __AwaitAnswer(interface, "__DriverReadlink", &msg);
    if (oserr != OS_EOK) {
        free(buffer);
        return oserr;
    }

    if (ctt_filesystem_readlink_result(GetGrachtClient(), &msg.base, &oserr, buffer, _MAXPATH) != 0) {
        free(buffer);
        return __AnswerUnreadable(interface, "__DriverReadlink");
    }

    if (oserr == OS_EOK) {
        *pathOut = mstr_new_u8(buffer);
        if (*pathOut == NULL) {
            oserr = OS_EOOM;
        }
    }
    free(buffer);
    return oserr;
}

static oserr_t
__DriverMove(struct VFSInterface* interface, void* instanceData, mstring_t* from, mstring_t* to, int copy)
{
    struct vali_link_message msg = VALI_MSG_INIT_HANDLE(interface->DriverID);
    oserr_t                  oserr;
    char*                    cfrom;
    char*                    cto;
    int                      status;

    cfrom = mstr_u8(from);
    if (cfrom == NULL) {
        return OS_EOOM;
    }

    cto = mstr_u8(to);
    if (cto == NULL) {
        free(cfrom);
        return OS_EOOM;
    }

    status = ctt_filesystem_move(GetGrachtClient(), &msg.base, (uintptr_t)instanceData, cfrom, cto, copy);
    free(cfrom);
    free(cto);
    if (status != 0) {
        return __RequestNotSent(interface, "__DriverMove");
    }

    oserr = __AwaitAnswer(interface, "__DriverMove", &msg);
    if (oserr != OS_EOK) {
        return oserr;
    }

    if (ctt_filesystem_move_result(GetGrachtClient(), &msg.base, &oserr) != 0) {
        return __AnswerUnreadable(interface, "__DriverMove");
    }
    return oserr;
}

static oserr_t
__DriverRead(
    _In_  struct VFSInterface* interface,
    _In_  void*                instanceData,
    _In_  void*                data,
    _In_  const SHMTransfer_t* transfer,
    _Out_ size_t*              unitsOut)
{
    struct vali_link_message      msg = VALI_MSG_INIT_HANDLE(interface->DriverID);
    struct ctt_fs_transfer_params params;
    uint64_t                      completed = 0;
    size_t                        length;
    oserr_t                       status;

    // reset even on error
    *unitsOut = 0;
    
    status = __ToSysSHMTransfer(transfer, &params.buffer);
    if (status != OS_EOK) {
        return status;
    }

    length = __SHMTransferLength(transfer);
    if (ctt_filesystem_read(GetGrachtClient(), &msg.base,
                          (uintptr_t)instanceData, (uintptr_t)data, &params) != 0) {
        return __RequestNotSent(interface, "__DriverRead");
    }

    status = __AwaitAnswer(interface, "__DriverRead", &msg);
    if (status != OS_EOK) {
        return status;
    }

    if (ctt_filesystem_read_result(GetGrachtClient(), &msg.base, &status, &completed) != 0) {
        return __AnswerUnreadable(interface, "__DriverRead");
    }

    if (completed > length) {
        ERROR("__DriverRead driver %u reported %llu bytes for a transfer of %llu bytes",
              interface->DriverID, completed, (uint64_t)length);
        return OS_EPROTOCOL;
    }

    *unitsOut = (size_t)completed;
    return status;
}

static oserr_t
__DriverWrite(
    _In_  struct VFSInterface* interface,
    _In_  void*                instanceData,
    _In_  void*                data,
    _In_  const SHMTransfer_t* transfer,
    _Out_ size_t*              unitsOut)
{
    struct vali_link_message      msg = VALI_MSG_INIT_HANDLE(interface->DriverID);
    struct ctt_fs_transfer_params params;
    uint64_t                      completed = 0;
    size_t                        length;
    oserr_t                       status;

    // reset even on error
    *unitsOut = 0;

    status = __ToSysSHMTransfer(transfer, &params.buffer);
    if (status != OS_EOK) {
        return status;
    }

    length = __SHMTransferLength(transfer);
    if (ctt_filesystem_write(GetGrachtClient(), &msg.base,
                          (uintptr_t)instanceData, (uintptr_t)data, &params) != 0) {
        return __RequestNotSent(interface, "__DriverWrite");
    }

    status = __AwaitAnswer(interface, "__DriverWrite", &msg);
    if (status != OS_EOK) {
        return status;
    }

    if (ctt_filesystem_write_result(GetGrachtClient(), &msg.base, &status, &completed) != 0) {
        return __AnswerUnreadable(interface, "__DriverWrite");
    }

    if (completed > length) {
        ERROR("__DriverWrite driver %u reported %llu bytes for a transfer of %llu bytes",
              interface->DriverID, completed, (uint64_t)length);
        return OS_EPROTOCOL;
    }

    *unitsOut = (size_t)completed;
    return status;
}

static oserr_t
__DriverTruncate(struct VFSInterface* interface, void* instanceData, void* data, uint64_t size)
{
    struct vali_link_message msg = VALI_MSG_INIT_HANDLE(interface->DriverID);
    oserr_t                  oserr;

    if (ctt_filesystem_truncate(GetGrachtClient(), &msg.base, (uintptr_t)instanceData, (uintptr_t)data, size) != 0) {
        return __RequestNotSent(interface, "__DriverTruncate");
    }

    oserr = __AwaitAnswer(interface, "__DriverTruncate", &msg);
    if (oserr != OS_EOK) {
        return oserr;
    }

    if (ctt_filesystem_truncate_result(GetGrachtClient(), &msg.base, &oserr) != 0) {
        return __AnswerUnreadable(interface, "__DriverTruncate");
    }
    return oserr;
}

static oserr_t
__DriverSeek(struct VFSInterface* interface, void* instanceData, void* data, uint64_t absolutePosition, uint64_t* absolutePositionOut)
{
    struct vali_link_message msg = VALI_MSG_INIT_HANDLE(interface->DriverID);
    oserr_t                  oserr;
    uint64_t                 position;

    if (ctt_filesystem_seek(GetGrachtClient(), &msg.base, (uintptr_t)instanceData, (uintptr_t)data, absolutePosition) != 0) {
        return __RequestNotSent(interface, "__DriverSeek");
    }

    oserr = __AwaitAnswer(interface, "__DriverSeek", &msg);
    if (oserr != OS_EOK) {
        return oserr;
    }

    if (ctt_filesystem_seek_result(GetGrachtClient(), &msg.base, &oserr, &position) != 0) {
        return __AnswerUnreadable(interface, "__DriverSeek");
    }

    if (oserr == OS_EOK) {
        *absolutePositionOut = position;
    }
    return oserr;
}

struct VFSOperations g_driverOps = {
        .Initialize = __DriverInitialize,
        .Destroy = __DriverDestroy,
        .Stat = __DriverStat,
        .Open = __DriverOpen,
        .Close = __DriverClose,
        .Link = __DriverLink,
        .Unlink = __DriverUnlink,
        .ReadLink = __DriverReadlink,
        .Create = __DriverCreate,
        .Move = __DriverMove,
        .Truncate = __DriverTruncate,
        .Read = __DriverRead,
        .Write = __DriverWrite,
        .Seek = __DriverSeek
};
