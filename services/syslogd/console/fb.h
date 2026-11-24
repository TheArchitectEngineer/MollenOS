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
 * Framebuffer
 * - Draws the rows of a text grid on the boot framebuffer, using the font in
 *   font.c. Only 32 bit pixels are supported, which is the only format the
 *   bootloader hands over.
 */

#ifndef __SYSLOGD_FB_H__
#define __SYSLOGD_FB_H__

#include <os/osdefs.h>
#include <os/types/video.h>
#include "text.h"

// Empty border around the text, in pixels, on every side of the screen.
#define FRAMEBUFFER_MARGIN 5

/**
 * @brief The boot framebuffer and how text is laid out on it.
 *
 * Pixels is the mapped framebuffer. It may be uncached, so each text row is
 * drawn into Scratch first and then copied over in one go. Scratch holds
 * one text row: the font height times BytesPerScanline bytes.
 *
 * Columns and Rows are the number of text cells that fit inside the margin.
 * Red, Green, Blue and Alpha come from the boot video descriptor: Position is
 * the lowest bit of the component in a pixel and Mask its largest value.
 */
typedef struct Framebuffer {
    uint8_t*              Pixels;
    uint8_t*              Scratch;
    uint32_t              Width;
    uint32_t              Height;
    uint32_t              BytesPerScanline;
    uint32_t              Columns;
    uint32_t              Rows;
    VideoColorComponent_t Red;
    VideoColorComponent_t Green;
    VideoColorComponent_t Blue;
    VideoColorComponent_t Alpha;
} Framebuffer_t;

/**
 * @brief Tells whether text can be drawn in a boot video mode. Call it before
 * mapping the framebuffer, so an unusable framebuffer is never mapped.
 *
 * @param video Boot video mode, from BootVideoQuery.
 * @return OS_EOK when the mode can be used, OS_EINVALPARAMS for NULL, or
 *         OS_ENOTSUPPORTED when the mode is not a 32 bit framebuffer or is too
 *         small for one cell of text.
 */
__EXTERN oserr_t
FramebufferCheckMode(
    _In_ const OSBootVideoDescriptor_t* video);

/**
 * @brief Sets up drawing on a mapped framebuffer.
 *
 * @param framebuffer Framebuffer to set up.
 * @param video       Boot video mode, from BootVideoQuery.
 * @param pixels      The mapped framebuffer, from BootVideoMapFramebuffer. It
 *                    must hold video->Height rows of video->BytesPerScanline
 *                    bytes.
 * @return OS_EOK on success, OS_EINVALPARAMS for a NULL argument, the error
 *         from FramebufferCheckMode, or OS_EOOM when Scratch could not be
 *         allocated.
 */
__EXTERN oserr_t
FramebufferCreate(
    _In_ Framebuffer_t*                 framebuffer,
    _In_ const OSBootVideoDescriptor_t* video,
    _In_ void*                          pixels);

/**
 * @brief Frees what FramebufferCreate allocated. The mapping stays.
 */
__EXTERN void
FramebufferDestroy(
    _In_ Framebuffer_t* framebuffer);

/**
 * @brief Converts a 0xRRGGBB color to a pixel value in the framebuffer
 * format. The alpha component, if any, is set to fully opaque.
 */
__EXTERN uint32_t
FramebufferColor(
    _In_ const Framebuffer_t* framebuffer,
    _In_ uint32_t             rgb);

/**
 * @brief Fills the whole screen with black, margin included.
 */
__EXTERN void
FramebufferClear(
    _In_ Framebuffer_t* framebuffer);

/**
 * @brief Draws the rows of the grid that changed, then marks them as drawn.
 * Cells are drawn in their color on a black background.
 *
 * @param framebuffer Framebuffer to draw on.
 * @param grid        Text to draw. Must have framebuffer->Columns columns and
 *                    framebuffer->Rows rows.
 */
__EXTERN void
FramebufferDrawGrid(
    _In_ Framebuffer_t* framebuffer,
    _In_ TextGrid_t*    grid);

#endif //!__SYSLOGD_FB_H__
