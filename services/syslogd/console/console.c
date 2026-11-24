/**
 * Copyright 2026, Philip Meulengracht
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
 * Console
 * - Formats kernel log messages and shows them on the boot framebuffer.
 */

#include <ddk/video.h>
#include <os/syslog.h>
#include <stdatomic.h>
#include <stdio.h>
#include "console.h"
#include "fb.h"
#include "text.h"

// Room for the timestamp and the "[level-core-thread] " prefix of a line.
#define __CONSOLE_LINE_PREFIX_MAX 64

// Room for the line that reports dropped messages, with the largest count.
#define __CONSOLE_DROPPED_LINE_MAX 96

// Color of the line that reports dropped messages, as 0xRRGGBB.
#define __CONSOLE_DROPPED_COLOR 0x9B59B6

// SHOWING draws changes. INHIBITED draws nothing. REDRAW is set by an
// uninhibit and makes the next draw clear the screen and draw every row,
// because something else may have drawn on the screen in the meantime.
enum __ConsoleState {
    __CONSOLE_STATE_SHOWING,
    __CONSOLE_STATE_INHIBITED,
    __CONSOLE_STATE_REDRAW
};

/**
 * @brief The console of syslogd. Framebuffer and Grid are only used by the
 * log reader job. State may be changed from any thread.
 */
typedef struct __Console {
    Framebuffer_t                Framebuffer;
    TextGrid_t                   Grid;
    _Atomic(enum __ConsoleState) State;
} __Console_t;

// Names match the ones the kernel prints on serial. Both tables are indexed
// by enum OSSysLogLevel. Colors are 0xRRGGBB.
static const char* g_levelNames[] = {
    "trace",
    "debug",
    "warn",
    "error"
};

static const uint32_t g_levelColors[] = {
    0x99E600,
    0x2ECC71,
    0x9B59B6,
    0xFF392B
};

static __Console_t g_console = { 0 };

oserr_t
ConsoleInitialize(void)
{
    OSBootVideoDescriptor_t video;
    void*                   pixels;
    oserr_t                 status;

    status = BootVideoQuery(&video);
    if (status != OS_EOK) {
        ERROR("ConsoleInitialize failed to query the boot video mode: %u", status);
        return status;
    }

    // Check the mode before mapping, so nothing is mapped that cannot be
    // drawn on.
    status = FramebufferCheckMode(&video);
    if (status != OS_EOK) {
        return status;
    }

    pixels = BootVideoMapFramebuffer();
    if (pixels == NULL) {
        ERROR("ConsoleInitialize failed to map the boot framebuffer");
        return OS_EUNKNOWN;
    }

    status = FramebufferCreate(&g_console.Framebuffer, &video, pixels);
    if (status != OS_EOK) {
        ERROR("ConsoleInitialize failed to set up the %ux%u framebuffer: %u",
              (unsigned int)video.Width, (unsigned int)video.Height, status);
        return status;
    }

    status = TextGridCreate(
        &g_console.Grid,
        g_console.Framebuffer.Columns,
        g_console.Framebuffer.Rows
    );
    if (status != OS_EOK) {
        ERROR("ConsoleInitialize failed to create a %ux%u text grid: %u",
              g_console.Framebuffer.Columns, g_console.Framebuffer.Rows, status);
        FramebufferDestroy(&g_console.Framebuffer);
        return status;
    }

    FramebufferClear(&g_console.Framebuffer);
    atomic_store(&g_console.State, __CONSOLE_STATE_SHOWING);
    return OS_EOK;
}

void
ConsoleShowRecord(
    _In_ const OSKernelLogRecord_t* record,
    _In_ const char*                text)
{
    char line[OSKERNELLOG_MESSAGE_MAX + __CONSOLE_LINE_PREFIX_MAX];

    // Unknown levels are skipped, as there is no name or color for them.
    if (record->Level == OSSYSLOGLEVEL_TRACE ||
        record->Level >= sizeof(g_levelNames) / sizeof(g_levelNames[0])) {
        return;
    }

    // snprintf ends the line with a zero byte, and cuts it off if it is
    // longer than the buffer.
    snprintf(
        &line[0],
        sizeof(line),
        "%09llu [%s-%u-%u] %s\n",
        (unsigned long long)(record->Timestamp / NSEC_PER_MSEC),
        g_levelNames[record->Level],
        record->CoreID,
        record->ThreadID,
        text
    );
    TextGridWrite(&g_console.Grid, g_levelColors[record->Level], &line[0]);
}

void
ConsoleShowDropped(
    _In_ uint64_t count)
{
    char line[__CONSOLE_DROPPED_LINE_MAX];

    snprintf(
        &line[0],
        sizeof(line),
        "[syslogd] %llu messages were dropped before they were shown\n",
        (unsigned long long)count
    );
    TextGridWrite(&g_console.Grid, __CONSOLE_DROPPED_COLOR, &line[0]);
}

void
ConsoleDraw(void)
{
    enum __ConsoleState state = atomic_load(&g_console.State);

    if (state == __CONSOLE_STATE_INHIBITED) {
        return;
    }

    // If an inhibit comes in between the load and the exchange, the exchange
    // fails and nothing is drawn.
    if (state == __CONSOLE_STATE_REDRAW) {
        if (!atomic_compare_exchange_strong(&g_console.State, &state, __CONSOLE_STATE_SHOWING)) {
            return;
        }
        FramebufferClear(&g_console.Framebuffer);
        TextGridMarkAllDirty(&g_console.Grid);
    }
    FramebufferDrawGrid(&g_console.Framebuffer, &g_console.Grid);
}

void
ConsoleInhibit(void)
{
    atomic_store(&g_console.State, __CONSOLE_STATE_INHIBITED);
}

void
ConsoleUninhibit(void)
{
    enum __ConsoleState expected = __CONSOLE_STATE_INHIBITED;

    // Only an inhibited console needs a redraw. A console that is showing
    // already keeps doing so.
    atomic_compare_exchange_strong(&g_console.State, &expected, __CONSOLE_STATE_REDRAW);
}
