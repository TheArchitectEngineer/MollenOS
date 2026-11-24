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
 *
 * System Calls
 */

#define __MODULE "SCIF"
//#define __TRACE

#include <arch/output.h>
#include <arch/utils.h>
#include <capabilities.h>
#include <heap.h>
#include <log.h>
#include <memoryspace.h>
#include <threading.h>
#include <console.h>
#include <machine.h>
#include <debug.h>

// Largest buffer one LogRead call fills. A larger user buffer is only filled
// up to this size, which keeps the kernel copy of the records small.
#define __LOG_READ_MAX (16 * 1024)

oserr_t
ScSystemDebug(
    _In_ enum OSSysLogLevel level,
    _In_ const char*        message,
    _In_ size_t             length)
{
    char    text[OSKERNELLOG_MESSAGE_MAX + 1];
    oserr_t status;

    if (message == NULL || (unsigned int)level > OSSYSLOGLEVEL_ERROR) {
        return OS_EINVALPARAMS;
    }
    if (length == 0) {
        return OS_EOK;
    }

    // Copy the text into the kernel first, so a bad user pointer fails the call
    // instead of faulting inside the log. Longer messages are cut off.
    if (length > OSKERNELLOG_MESSAGE_MAX) {
        length = OSKERNELLOG_MESSAGE_MAX;
    }

    status = MemorySpaceCopyFromUser(&text[0], message, length);
    if (status != OS_EOK) {
        return status;
    }
    text[length] = '\0';

    // The caller has already formatted the message. Pass it as an argument so
    // that a '%' left in the text is printed as is, and is not read as a
    // conversion that would take arguments the caller never passed.
    LogAppendMessage(level, "%s", &text[0]);
    return OS_EOK;
}

oserr_t
ScLogRead(
    _In_  const uint64_t*          sequence,
    _In_  void*                    buffer,
    _In_  size_t                   bufferSize,
    _Out_ OSKernelLogReadResult_t* result)
{
    OSKernelLogReadResult_t readResult;
    uint64_t                startSequence;
    void*                   records;
    oserr_t                 status;
    oserr_t                 copyStatus;

    if (sequence == NULL || buffer == NULL || bufferSize == 0 || result == NULL) {
        return OS_EINVALPARAMS;
    }

    status = CapabilityVerify(CAPABILITY_SYSLOG_READ);
    if (status != OS_EOK) {
        return status;
    }

    // The sequence number is passed by pointer, because a 64 bit value does
    // not fit in one system call argument on 32 bit platforms.
    status = MemorySpaceCopyFromUser(&startSequence, sequence, sizeof(uint64_t));
    if (status != OS_EOK) {
        return status;
    }

    // LogRead holds the log lock with interrupts disabled, so it cannot write
    // to user memory, which may fault. Read into a kernel buffer and copy that.
    if (bufferSize > __LOG_READ_MAX) {
        bufferSize = __LOG_READ_MAX;
    }

    records = kmalloc(bufferSize);
    if (records == NULL) {
        return OS_EOOM;
    }

    // OS_EBUFFER still fills in readResult, so the caller learns how many
    // messages were dropped and that its buffer is too small.
    status = LogRead(startSequence, records, bufferSize, &readResult);
    if (status != OS_EOK && status != OS_EBUFFER) {
        kfree(records);
        return status;
    }

    if (readResult.BytesRead != 0) {
        copyStatus = MemorySpaceCopyToUser(
            buffer,
            records,
            readResult.BytesRead
        );
        if (copyStatus != OS_EOK) {
            kfree(records);
            return copyStatus;
        }
    }
    kfree(records);

    copyStatus = MemorySpaceCopyToUser(
        result,
        &readResult,
        sizeof(OSKernelLogReadResult_t)
    );
    if (copyStatus != OS_EOK) {
        return copyStatus;
    }
    return status;
}

oserr_t
ScSystemQuery(
    _In_  enum OSSystemQueryRequest request,
    _In_  void*                     buffer,
    _In_  size_t                    bufferSize,
    _Out_ size_t*                   bytesQueriedOut)
{
    switch (request) {
        case OSSYSTEMQUERY_BOOTVIDEOINFO: {
            OSBootVideoDescriptor_t* info = buffer;
            oserr_t                  oserr;
            if (bufferSize < sizeof(OSBootVideoDescriptor_t)) {
                return OS_EINVALPARAMS;
            }

            oserr = VideoQuery(info);
            if (oserr != OS_EOK) {
                return oserr;
            }
            
            *bytesQueriedOut = sizeof(OSBootVideoDescriptor_t);
            return OS_EOK;
        } break;
        case OSSYSTEMQUERY_CPUINFO: {
            OSSystemCPUInfo_t* info = buffer;
            size_t             line = CpuDataCacheLineSize();
            
            if (bufferSize < sizeof(OSSystemCPUInfo_t)) {
                return OS_EINVALPARAMS;
            }
            
            info->NumberOfProcessors = atomic_load(&GetMachine()->NumberOfProcessors);
            info->NumberOfActiveCores = atomic_load(&GetMachine()->NumberOfActiveCores);
            info->DataCacheLineSizeBytes = line != 0 ? line : 1;

#if defined(__aarch64__)
            info->Features = Arm64GetCpuFeatures();
#else
            info->Features = 0;
#endif

            *bytesQueriedOut = sizeof(OSSystemCPUInfo_t);
            return OS_EOK;
        } break;
        case OSSYSTEMQUERY_MEMINFO: {
            OSSystemMemoryInfo_t* info = buffer;
            if (bufferSize < sizeof(OSSystemMemoryInfo_t)) {
                return OS_EINVALPARAMS;
            }
            size_t maxBlocks  = GetMachine()->NumberOfMemoryBlocks;
            size_t freeBlocks = GetMachine()->NumberOfFreeMemoryBlocks;
            info->AllocationGranularityBytes = GetMachine()->MemoryGranularity;
            info->PageSizeBytes = GetMemorySpacePageSize();
            info->PagesTotal = maxBlocks;
            info->PagesUsed  = maxBlocks - freeBlocks;
            *bytesQueriedOut = sizeof(OSSystemMemoryInfo_t);
            return OS_EOK;
        } break;
        default: {
            return OS_ENOTSUPPORTED;
        }
    }
}

oserr_t
ScFlushHardwareCache(
    _In_     int    Cache,
    _In_Opt_ void*  Start, 
    _In_Opt_ size_t Length)
{
    if (Cache == CACHE_INSTRUCTION) {
        CpuFlushInstructionCache(Start, Length);
        return OS_EOK;
    }
    else if (Cache == CACHE_MEMORY) {
        CpuInvalidateMemoryCache(Start, Length);
        return OS_EOK;
    }
    return OS_EUNKNOWN;
}

oserr_t
ScInstallSignalHandler(
    _In_ uintptr_t handler)
{
    return MemorySpaceSetSignalHandler(GetCurrentMemorySpace(), handler);
}

/**
 * @brief Maps the boot framebuffer into the calling process, so the syslog
 * service can draw the log on it. The caller needs the boot-video capability.
 */
oserr_t
ScMapBootFramebuffer(
    _Out_ void** bufferOut)
{
    MemorySpace_t* memorySpace     = GetCurrentMemorySpace();
    uintptr_t      addressPhysical = VideoGetTerminal()->FrameBufferAddressPhysical;
    uintptr_t      fbVirtual       = 0;
    size_t         fbSize          = VideoGetTerminal()->Info.BytesPerScanline * VideoGetTerminal()->Info.Height;
    oserr_t        oserr;

    if (bufferOut == NULL) {
        return OS_EINVALPARAMS;
    }

    oserr = CapabilityVerify(CAPABILITY_BOOT_VIDEO);
    if (oserr != OS_EOK) {
        return oserr;
    }

    if (!VideoGetTerminal()->FrameBufferAddressPhysical) {
        return OS_ENOTSUPPORTED;
    }

    oserr = MemorySpaceMap(
        memorySpace,
        &(struct MemorySpaceMapOptions) {
            .PhysicalStart = addressPhysical,
            .Length = fbSize,
            .Flags = MAPPING_COMMIT | MAPPING_USERSPACE | MAPPING_NOCACHE | MAPPING_PERSISTENT,
            .PlacementFlags = MAPPING_PHYSICAL_CONTIGUOUS | MAPPING_VIRTUAL_PROCESS
        },
        &fbVirtual
    );
    if (oserr != OS_EOK) {
        ERROR("ScMapBootFramebuffer failed to map the framebuffer: %u", oserr);
        return oserr;
    }

    *bufferOut = (void*)fbVirtual;
    return OS_EOK;
}
