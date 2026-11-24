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
 *
 * Kernel Log
 * Buffer layout:
 * - Messages are stored as records (OSKernelLogRecord_t followed by the text)
 *   in one byte buffer. Every record starts on an 8 byte boundary.
 * - Head and Tail are byte positions that only ever grow. The offset in the
 *   buffer is the position masked with (Capacity - 1). Records between Tail
 *   and Head are valid. Because positions never go back, any saved position
 *   that is still >= Tail still points at the same record.
 * - A record never wraps around the end of the buffer. If it does not fit
 *   before the end, a padding record fills the rest of the buffer and the
 *   message starts at offset 0. Padding records have no sequence number.
 *
 * Printing:
 * - One CPU at a time prints. It holds OutputState while it prints every record
 *   from the output cursor up to Head. A CPU that adds a message while another
 *   is printing does not wait. The printing CPU prints that message as well.
 * - The ring lock is only held while copying a record in or out, never while
 *   waiting on the serial port.
 * - The kernel only prints to the serial port. It never draws on the screen.
 *   The syslog service reads the log with LogRead and draws it there.
 */

#include <arch/interrupts.h>
#include <arch/output.h>
#include <arch/utils.h>
#include <component/timer.h>
#include <debug.h>
#include <heap.h>
#include <log.h>
#include <spinlock.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <threading.h>

// Records start on this boundary. Positions and record sizes are multiples of it.
#define __LOG_RECORD_ALIGNMENT 8

// Level value that marks a padding record. Only Size and Level are valid in a
// padding record. It is never a valid enum OSSysLogLevel value.
#define __LOG_PADDING_LEVEL 0xFF

// How many times panic mode tries to take the ring lock before it gives up and
// prints the message without storing it.
#define __LOG_PANIC_LOCK_ATTEMPTS 100000

// Room for the timestamp and the "[level-core-thread] " prefix in a printed line.
#define __LOG_LINE_PREFIX_MAX 64

// Room for the line that reports dropped messages, with the largest count.
#define __LOG_DROPPED_LINE_MAX 96

enum __LogMode {
    __LOG_MODE_NORMAL,
    __LOG_MODE_PANIC
};

enum __LogOutputState {
    __LOG_OUTPUT_IDLE,
    __LOG_OUTPUT_PRINTING
};

/**
 * @brief The kernel log ring buffer and its output cursor.
 *
 * Data, Capacity, Tail, Head, TailSequence, HeadSequence and the Output*
 * cursor fields are protected by SyncObject.
 *
 * TailSequence is the sequence number of the oldest message in the buffer.
 * HeadSequence is the number the next message gets. The buffer is empty of
 * messages when they are equal.
 *
 * OutputPosition and OutputSequence point at the next record to print.
 * OutputDropped counts the messages that were dropped before they could be
 * printed, and is reported by the next print.
 */
typedef struct __KernelLog {
    Spinlock_t                     SyncObject;
    uint8_t*                       Data;
    size_t                         Capacity;
    uint64_t                       Tail;
    uint64_t                       Head;
    uint64_t                       TailSequence;
    uint64_t                       HeadSequence;
    uint64_t                       OutputPosition;
    uint64_t                       OutputSequence;
    uint64_t                       OutputDropped;
    _Atomic(enum __LogOutputState) OutputState;
    _Atomic(enum __LogMode)        Mode;
} __KernelLog_t;

static const char* g_levelNames[] = {
    "trace",
    "debug",
    "warn",
    "error"
};

static __KernelLog_t g_kernelLog = { 0 };

// Declared as uint64_t so that the records inside are 8 byte aligned.
static uint64_t g_bootLogBuffer[LOG_BOOT_BUFFER_SIZE / sizeof(uint64_t)];

static inline size_t
__AlignRecordSize(
    _In_ size_t size)
{
    return (size + (__LOG_RECORD_ALIGNMENT - 1)) & ~((size_t)__LOG_RECORD_ALIGNMENT - 1);
}

static inline OSKernelLogRecord_t*
__RecordAt(
    _In_ __KernelLog_t* log,
    _In_ uint64_t       position)
{
    return (OSKernelLogRecord_t*)&log->Data[(size_t)position & (log->Capacity - 1)];
}

static inline const char*
__RecordText(
    _In_ const OSKernelLogRecord_t* record)
{
    return (const char*)record + sizeof(OSKernelLogRecord_t);
}

static void
__ResetLog(
    _In_ __KernelLog_t* log,
    _In_ void*          buffer,
    _In_ size_t         capacity)
{
    log->Data           = buffer;
    log->Capacity       = capacity;
    log->Tail           = 0;
    log->Head           = 0;
    log->TailSequence   = 0;
    log->HeadSequence   = 0;
    log->OutputPosition = 0;
    log->OutputSequence = 0;
    log->OutputDropped  = 0;
}

/**
 * @brief Removes the oldest record. The caller holds the ring lock, and the
 * buffer must hold at least one record (Tail != Head).
 */
static void
__DropOldestRecord(
    _In_ __KernelLog_t* log)
{
    OSKernelLogRecord_t* record = __RecordAt(log, log->Tail);

    if (record->Level != __LOG_PADDING_LEVEL) {
        log->TailSequence++;
    }
    log->Tail += record->Size;
}

/**
 * @brief Drops the oldest records until at least size bytes are free. The
 * caller holds the ring lock, and size must not exceed Capacity.
 */
static void
__MakeRoom(
    _In_ __KernelLog_t* log,
    _In_ size_t         size)
{
    while (log->Capacity - (size_t)(log->Head - log->Tail) < size) {
        __DropOldestRecord(log);
    }
}

/**
 * @brief Writes a record at Head and gives it the next sequence number. The
 * caller holds the ring lock. If the record does not fit between Head and the
 * end of the buffer, the rest of the buffer is filled with a padding record
 * first, so the record starts at offset 0.
 *
 * @param log    The log to write to.
 * @param source Level, CoreID, ThreadID and Timestamp of the message. The
 *               other header fields are filled in here.
 * @param text   Message text, source->Length bytes. No zero byte is stored.
 */
static void
__AppendRecord(
    _In_ __KernelLog_t*             log,
    _In_ const OSKernelLogRecord_t* source,
    _In_ const char*                text)
{
    OSKernelLogRecord_t* record;
    size_t               recordSize;
    size_t               bytesToEnd;

    recordSize = __AlignRecordSize(sizeof(OSKernelLogRecord_t) + source->Length);
    bytesToEnd = log->Capacity - ((size_t)log->Head & (log->Capacity - 1));
    if (bytesToEnd < recordSize) {
        // Positions are 8 byte aligned and Capacity is a power of two, so
        // bytesToEnd is at least 8, which is room for Size and Level.
        __MakeRoom(log, bytesToEnd);
        record        = __RecordAt(log, log->Head);
        record->Size  = (uint32_t)bytesToEnd;
        record->Level = __LOG_PADDING_LEVEL;
        log->Head    += bytesToEnd;
    }

    __MakeRoom(log, recordSize);
    record            = __RecordAt(log, log->Head);
    *record           = *source;
    record->Size      = (uint32_t)recordSize;
    record->Reserved  = 0;
    record->Sequence  = log->HeadSequence++;
    memcpy((char*)record + sizeof(OSKernelLogRecord_t), text, source->Length);
    log->Head += recordSize;
}

/**
 * @brief Returns the position of the first message record at or after
 * position, skipping padding records. Returns Head if there is none. The
 * caller holds the ring lock, and position must be between Tail and Head.
 */
static uint64_t
__SkipPadding(
    _In_ __KernelLog_t* log,
    _In_ uint64_t       position)
{
    while (position != log->Head) {
        OSKernelLogRecord_t* record = __RecordAt(log, position);
        if (record->Level != __LOG_PADDING_LEVEL) {
            break;
        }
        position += record->Size;
    }
    return position;
}

/**
 * @brief Takes the ring lock. In panic mode it gives up after a number of
 * attempts instead of waiting forever, because the holder may be stopped or
 * may be this CPU itself.
 *
 * @return OS_EOK when the lock is held, or OS_EBUSY in panic mode when it
 *         could not be taken.
 */
static oserr_t
__LockLog(void)
{
    if (atomic_load(&g_kernelLog.Mode) == __LOG_MODE_NORMAL) {
        SpinlockAcquireIrq(&g_kernelLog.SyncObject);
        return OS_EOK;
    }

    for (int i = 0; i < __LOG_PANIC_LOCK_ATTEMPTS; i++) {
        if (SpinlockTryAcquireIrq(&g_kernelLog.SyncObject) == OS_EOK) {
            return OS_EOK;
        }
    }
    return OS_EBUSY;
}

static inline void
__UnlockLog(void)
{
    SpinlockReleaseIrq(&g_kernelLog.SyncObject);
}

/**
 * @brief Tells whether the serial port is set up. Until it is, messages stay
 * in the buffer and the output cursor does not move, so they are printed once
 * the port exists. Systems without a serial port only store messages, for the
 * syslog service to read.
 */
static int
__SerialAvailable(void)
{
    return (VideoGetTerminal()->AvailableOutputs & VIDEO_UART) != 0;
}

static const char*
__LevelName(
    _In_ unsigned int level)
{
    if (level >= sizeof(g_levelNames) / sizeof(g_levelNames[0])) {
        return "unknown";
    }
    return g_levelNames[level];
}

static void
__WriteToSerial(
    _In_ const char* line)
{
    if (!__SerialAvailable()) {
        return;
    }

    for (int i = 0; line[i]; i++) {
        SerialPutCharacter(line[i]);
    }
}

/**
 * @brief Formats one message as a line and writes it to the serial port. The
 * format matches the one the boot tests read from serial:
 * "<milliseconds> [<level>-<core>-<thread>] <text>".
 *
 * @param record Header of the message.
 * @param text   Zero terminated message text.
 */
static void
__PrintRecord(
    _In_ const OSKernelLogRecord_t* record,
    _In_ const char*                text)
{
    char line[OSKERNELLOG_MESSAGE_MAX + __LOG_LINE_PREFIX_MAX];

    // snprintf always ends the line with a zero byte, and cuts it off if the
    // prefix is longer than expected.
    snprintf(
        &line[0],
        sizeof(line),
        "%09llu [%s-%u-%u] %s\n",
        (unsigned long long)(record->Timestamp / NSEC_PER_MSEC),
        __LevelName(record->Level),
        record->CoreID,
        record->ThreadID,
        text
    );
    __WriteToSerial(&line[0]);
}

static void
__PrintDropped(
    _In_ uint64_t dropped)
{
    char line[__LOG_DROPPED_LINE_MAX];

    snprintf(
        &line[0],
        sizeof(line),
        "[log] %llu messages were dropped before they were printed\n",
        (unsigned long long)dropped
    );
    __WriteToSerial(&line[0]);
}

/**
 * @brief Moves the output cursor to the oldest record if the record it points
 * at was dropped, and adds the skipped messages to OutputDropped. The caller
 * holds the ring lock.
 */
static void
__UpdateOutputCursor(
    _In_ __KernelLog_t* log)
{
    if (log->OutputPosition >= log->Tail) {
        return;
    }
    log->OutputDropped  += log->TailSequence - log->OutputSequence;
    log->OutputPosition  = log->Tail;
    log->OutputSequence  = log->TailSequence;
}

/**
 * @brief Copies the next unprinted record out of the buffer and moves the
 * output cursor past it.
 *
 * @param record  Receives the record header.
 * @param text    Receives the zero terminated text. Must hold
 *                OSKERNELLOG_MESSAGE_MAX + 1 bytes.
 * @param dropped Receives the number of messages dropped before they were
 *                printed, since the last call that reported any.
 * @return OS_EOK when a record was copied, OS_ENOENT when all records are
 *         printed, or OS_EBUSY in panic mode when the ring lock could not be
 *         taken.
 */
static oserr_t
__TakeNextOutputRecord(
    _Out_ OSKernelLogRecord_t* record,
    _Out_ char*                text,
    _Out_ uint64_t*            dropped)
{
    OSKernelLogRecord_t* stored;
    oserr_t              status;

    status = __LockLog();
    if (status != OS_EOK) {
        return status;
    }

    __UpdateOutputCursor(&g_kernelLog);
    *dropped = g_kernelLog.OutputDropped;
    g_kernelLog.OutputDropped = 0;

    g_kernelLog.OutputPosition = __SkipPadding(&g_kernelLog, g_kernelLog.OutputPosition);
    if (g_kernelLog.OutputPosition == g_kernelLog.Head) {
        __UnlockLog();
        return OS_ENOENT;
    }

    stored  = __RecordAt(&g_kernelLog, g_kernelLog.OutputPosition);
    
    *record = *stored;
    memcpy(text, __RecordText(stored), stored->Length);
    text[stored->Length] = '\0';

    g_kernelLog.OutputPosition += stored->Size;
    g_kernelLog.OutputSequence  = stored->Sequence + 1;
    __UnlockLog();
    return OS_EOK;
}

/**
 * @brief Prints records until all are printed. The caller must be the only
 * CPU printing (it holds OutputState), or the system must be in panic mode.
 */
static void
__PrintPendingRecords(void)
{
    OSKernelLogRecord_t record;
    char                text[OSKERNELLOG_MESSAGE_MAX + 1];
    uint64_t            dropped;

    while (__SerialAvailable()) {
        oserr_t status = __TakeNextOutputRecord(&record, &text[0], &dropped);
        if (dropped != 0) {
            __PrintDropped(dropped);
        }
        if (status != OS_EOK) {
            // OS_ENOENT means everything is printed. OS_EBUSY only happens in
            // panic mode, and then the caller prints its own message directly.
            break;
        }
        __PrintRecord(&record, &text[0]);
    }
}

/**
 * @brief Tells whether records are waiting to be printed and the serial port
 * can print them.
 */
static int
__HasPendingOutput(void)
{
    int pending;

    if (!__SerialAvailable() || __LockLog() != OS_EOK) {
        return 0;
    }

    pending = g_kernelLog.OutputSequence != g_kernelLog.HeadSequence;
    __UnlockLog();
    return pending;
}

/**
 * @brief Prints all waiting records, unless another CPU is printing already.
 *
 * Flow:
 * 1. Become the printing CPU by moving OutputState from IDLE to PRINTING. If
 *    another CPU is printing, return. That CPU prints our message too, because
 *    our message was added to the buffer before we got here.
 * 2. Print with interrupts disabled, so this thread cannot be moved off the
 *    CPU while every other CPU relies on it to print.
 * 3. Set OutputState back to IDLE, then check again. A message added between
 *    the last record we printed and the store of IDLE was left to us by a CPU
 *    that saw PRINTING, so it must be printed now.
 */
static void
__FlushOutput(void)
{
    irqstate_t irqState;

    for (;;) {
        enum __LogOutputState expected = __LOG_OUTPUT_IDLE;
        if (!atomic_compare_exchange_strong(&g_kernelLog.OutputState, &expected, __LOG_OUTPUT_PRINTING)) {
            return;
        }

        irqState = InterruptDisable();
        __PrintPendingRecords();
        InterruptRestoreState(irqState);

        atomic_store(&g_kernelLog.OutputState, __LOG_OUTPUT_IDLE);
        if (!__HasPendingOutput()) {
            return;
        }
    }
}

void
LogInitialize(void)
{
    SpinlockConstruct(&g_kernelLog.SyncObject);
    __ResetLog(&g_kernelLog, &g_bootLogBuffer[0], LOG_BOOT_BUFFER_SIZE);
    atomic_store(&g_kernelLog.OutputState, __LOG_OUTPUT_IDLE);
    atomic_store(&g_kernelLog.Mode, __LOG_MODE_NORMAL);

#ifdef __OSCONFIG_HAS_UART
    oserr_t status = SerialPortInitialize();
    if (status != OS_EOK) {
        WARNING("LogInitialize failed to initialize serial output: %u", status);
    }
#endif
}

void
LogInitializeFull(void)
{
    __KernelLog_t upgraded = { 0 };
    void*         buffer;
    uint64_t      position;

    buffer = kmalloc(LOG_FULL_BUFFER_SIZE);
    if (buffer == NULL) {
        WARNING("LogInitializeFull failed to allocate %u bytes, the log stays in the %u byte boot buffer",
                LOG_FULL_BUFFER_SIZE, LOG_BOOT_BUFFER_SIZE);
        return;
    }

    // Copy the records over in order. The new buffer is larger than the boot
    // buffer, so nothing is dropped and no padding is needed. Starting the new
    // sequence count at TailSequence gives every record its old number again.
    SpinlockAcquireIrq(&g_kernelLog.SyncObject);
    __UpdateOutputCursor(&g_kernelLog);
    __ResetLog(&upgraded, buffer, LOG_FULL_BUFFER_SIZE);
    upgraded.TailSequence   = g_kernelLog.TailSequence;
    upgraded.HeadSequence   = g_kernelLog.TailSequence;
    upgraded.OutputSequence = g_kernelLog.OutputSequence;
    upgraded.OutputDropped  = g_kernelLog.OutputDropped;

    position = __SkipPadding(&g_kernelLog, g_kernelLog.Tail);
    while (position != g_kernelLog.Head) {
        OSKernelLogRecord_t* record = __RecordAt(&g_kernelLog, position);
        if (record->Sequence == g_kernelLog.OutputSequence) {
            upgraded.OutputPosition = upgraded.Head;
        }
        __AppendRecord(&upgraded, record, __RecordText(record));
        position = __SkipPadding(&g_kernelLog, position + record->Size);
    }

    // When everything was printed, the cursor waits at the new Head.
    if (g_kernelLog.OutputSequence == g_kernelLog.HeadSequence) {
        upgraded.OutputPosition = upgraded.Head;
    }

    g_kernelLog.Data           = upgraded.Data;
    g_kernelLog.Capacity       = upgraded.Capacity;
    g_kernelLog.Tail           = upgraded.Tail;
    g_kernelLog.Head           = upgraded.Head;
    g_kernelLog.TailSequence   = upgraded.TailSequence;
    g_kernelLog.HeadSequence   = upgraded.HeadSequence;
    g_kernelLog.OutputPosition = upgraded.OutputPosition;
    g_kernelLog.OutputSequence = upgraded.OutputSequence;
    g_kernelLog.OutputDropped  = upgraded.OutputDropped;
    SpinlockReleaseIrq(&g_kernelLog.SyncObject);
}

void
LogFlush(void)
{
    __FlushOutput();
}

void
LogEnterPanicMode(void)
{
    atomic_store(&g_kernelLog.Mode, __LOG_MODE_PANIC);

    // The printing CPU may have stopped, or may be this CPU, so do not wait
    // for OutputState. Print whatever has not been printed yet.
    __PrintPendingRecords();
}

void
LogAppendMessage(
    _In_ enum OSSysLogLevel level,
    _In_ const char*        format, ...)
{
    OSKernelLogRecord_t record = { 0 };
    char                text[OSKERNELLOG_MESSAGE_MAX + 1];
    va_list             arguments;
    tick_t              timestamp;
    int                 written;

    if (format == NULL) {
        return;
    }

    // Format before taking the ring lock, so the lock is held only for the copy.
    va_start(arguments, format);
    written = vsnprintf(&text[0], sizeof(text), format, arguments);
    va_end(arguments);
    if (written < 0) {
        written = 0;
        text[0] = '\0';
    } else if (written > OSKERNELLOG_MESSAGE_MAX) {
        written = OSKERNELLOG_MESSAGE_MAX;
    }

    SystemTimerGetTimestamp(&timestamp);
    record.Length    = (uint16_t)written;
    record.Level     = (uint8_t)level;
    record.CoreID    = (uint32_t)ArchGetProcessorCoreId();
    record.ThreadID  = (uint32_t)ThreadCurrentHandle();
    record.Timestamp = (uint64_t)timestamp;

    if (__LockLog() != OS_EOK) {
        // Panic mode, and the lock is stuck. Print the message so it is seen,
        // even though it cannot be stored.
        __PrintRecord(&record, &text[0]);
        return;
    }
    __AppendRecord(&g_kernelLog, &record, &text[0]);
    __UnlockLog();

    if (atomic_load(&g_kernelLog.Mode) == __LOG_MODE_PANIC) {
        __PrintPendingRecords();
        return;
    }
    __FlushOutput();
}

oserr_t
LogRead(
    _In_  uint64_t                 sequence,
    _In_  void*                    buffer,
    _In_  size_t                   bufferSize,
    _Out_ OSKernelLogReadResult_t* result)
{
    uint8_t* output = buffer;
    size_t   bytesRead = 0;
    uint64_t dropped = 0;
    uint64_t position;
    oserr_t  status;

    if (buffer == NULL || result == NULL) {
        return OS_EINVALPARAMS;
    }

    status = __LockLog();
    if (status != OS_EOK) {
        return status;
    }

    if (sequence > g_kernelLog.HeadSequence) {
        __UnlockLog();
        return OS_EINVALPARAMS;
    }
    if (sequence < g_kernelLog.TailSequence) {
        dropped  = g_kernelLog.TailSequence - sequence;
        sequence = g_kernelLog.TailSequence;
    }

    // Records are found by walking from the oldest one. Readers pass only a
    // sequence number, so the kernel never has to trust a position from them.
    position = __SkipPadding(&g_kernelLog, g_kernelLog.Tail);
    while (position != g_kernelLog.Head) {
        OSKernelLogRecord_t* record = __RecordAt(&g_kernelLog, position);
        size_t               outputSize;

        if (record->Sequence >= sequence) {
            outputSize = __AlignRecordSize(sizeof(OSKernelLogRecord_t) + record->Length);
            if (outputSize > bufferSize - bytesRead) {
                break;
            }
            memcpy(&output[bytesRead], record, sizeof(OSKernelLogRecord_t) + record->Length);
            ((OSKernelLogRecord_t*)&output[bytesRead])->Size = (uint32_t)outputSize;
            bytesRead += outputSize;
            sequence   = record->Sequence + 1;
        }
        position = __SkipPadding(&g_kernelLog, position + record->Size);
    }

    if (bytesRead == 0 && sequence != g_kernelLog.HeadSequence) {
        status = OS_EBUFFER;
    }
    __UnlockLog();

    result->NextSequence = sequence;
    result->Dropped      = dropped;
    result->BytesRead    = (uint32_t)bytesRead;
    result->Reserved     = 0;
    return status;
}
