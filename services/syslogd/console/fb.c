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
 * - Draws text grid rows on the boot framebuffer.
 *
 * Font layout (font.c):
 * - g_fontIndices lists the code points that have a glyph, sorted from low to
 *   high. Glyph i is stored at g_font[i * g_fontHeight].
 * - Each glyph has one byte per pixel row. Bit 7 is the leftmost pixel.
 * - Glyph 0 is the box drawn for code points without a glyph.
 */

#include <stdlib.h>
#include <string.h>
#include "fb.h"

// Bytes in one pixel. The bootloader only hands over 32 bit modes.
#define __FB_BYTES_PER_PIXEL 4

// Background color of the screen, as 0xRRGGBB.
#define __FB_BACKGROUND 0x000000

extern const uint32_t g_fontHeight;
extern const uint32_t g_fontWidth;
extern const uint32_t g_fontCharCount;
extern const uint8_t  g_font[];
extern const uint16_t g_fontIndices[];

// Each cell is one pixel wider than the glyphs, which leaves a gap between
// characters.
static inline uint32_t
__CellWidth(void)
{
    return g_fontWidth + 1;
}

/**
 * @brief Finds the glyph of a code point with a binary search in
 * g_fontIndices. Returns glyph 0 when the font has no glyph for it.
 */
static uint32_t
__FindGlyph(
    _In_ uint32_t character)
{
    uint32_t low  = 0;
    uint32_t high = g_fontCharCount;

    while (low < high) {
        uint32_t middle = low + (high - low) / 2;
        if (g_fontIndices[middle] == character) {
            return middle;
        }
        
        if (g_fontIndices[middle] < character) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    return 0;
}

static uint32_t
__ScaleComponent(
    _In_ const VideoColorComponent_t* component,
    _In_ uint32_t                     value)
{
    // value is 0 to 255. Scale it to the 0 to Mask range of the component.
    return ((value * component->Mask) / 0xFF) << component->Position;
}

static void
__FillScratch(
    _In_ Framebuffer_t* framebuffer,
    _In_ uint32_t       pixel)
{
    for (uint32_t y = 0; y < g_fontHeight; y++) {
        uint32_t* line = (uint32_t*)(framebuffer->Scratch + (size_t)y * framebuffer->BytesPerScanline);
        for (uint32_t x = 0; x < framebuffer->Width; x++) {
            line[x] = pixel;
        }
    }
}

/**
 * @brief Draws one cell into Scratch. Only the set pixels of the glyph are
 * written. The background was filled before.
 */
static void
__DrawCell(
    _In_ Framebuffer_t*    framebuffer,
    _In_ uint32_t          column,
    _In_ const TextCell_t* cell)
{
    const uint8_t* glyph = &g_font[__FindGlyph(cell->Character) * g_fontHeight];
    uint32_t       pixel = FramebufferColor(framebuffer, cell->Color);
    uint32_t       left  = FRAMEBUFFER_MARGIN + column * __CellWidth();

    for (uint32_t y = 0; y < g_fontHeight; y++) {
        uint32_t* line = (uint32_t*)(framebuffer->Scratch + (size_t)y * framebuffer->BytesPerScanline);
        for (uint32_t x = 0; x < __CellWidth() && x < 8; x++) {
            if (glyph[y] & (0x80 >> x)) {
                line[left + x] = pixel;
            }
        }
    }
}

/**
 * @brief Copies Scratch to the framebuffer, with its top at pixel row top.
 */
static void
__CopyScratch(
    _In_ Framebuffer_t* framebuffer,
    _In_ uint32_t       top)
{
    memcpy(
        framebuffer->Pixels + (size_t)top * framebuffer->BytesPerScanline,
        framebuffer->Scratch,
        (size_t)g_fontHeight * framebuffer->BytesPerScanline
    );
}

oserr_t
FramebufferCheckMode(
    _In_ const OSBootVideoDescriptor_t* video)
{
    if (video == NULL) {
        return OS_EINVALPARAMS;
    }

    if (video->Type != OSBOOTVIDEO_FRAMEBUFFER || video->DepthBits != 32 ||
        video->BytesPerScanline < video->Width * __FB_BYTES_PER_PIXEL) {
        return OS_ENOTSUPPORTED;
    }

    // At least one cell must fit inside the margin.
    if (video->Width < 2 * FRAMEBUFFER_MARGIN + __CellWidth() ||
        video->Height < 2 * FRAMEBUFFER_MARGIN + g_fontHeight) {
        return OS_ENOTSUPPORTED;
    }
    return OS_EOK;
}

oserr_t
FramebufferCreate(
    _In_ Framebuffer_t*                 framebuffer,
    _In_ const OSBootVideoDescriptor_t* video,
    _In_ void*                          pixels)
{
    uint32_t textWidth;
    uint32_t textHeight;
    oserr_t  status;

    if (framebuffer == NULL || video == NULL || pixels == NULL) {
        return OS_EINVALPARAMS;
    }

    status = FramebufferCheckMode(video);
    if (status != OS_EOK) {
        return status;
    }
    textWidth  = (uint32_t)video->Width - 2 * FRAMEBUFFER_MARGIN;
    textHeight = (uint32_t)video->Height - 2 * FRAMEBUFFER_MARGIN;

    // Scratch is zeroed because the padding at the end of each scanline is
    // copied to the framebuffer too, and is never drawn.
    memset(framebuffer, 0, sizeof(Framebuffer_t));
    framebuffer->Scratch = calloc(g_fontHeight, video->BytesPerScanline);
    if (framebuffer->Scratch == NULL) {
        return OS_EOOM;
    }

    framebuffer->Pixels           = pixels;
    framebuffer->Width            = (uint32_t)video->Width;
    framebuffer->Height           = (uint32_t)video->Height;
    framebuffer->BytesPerScanline = (uint32_t)video->BytesPerScanline;
    framebuffer->Columns          = textWidth / __CellWidth();
    framebuffer->Rows             = textHeight / g_fontHeight;
    framebuffer->Red              = video->Red;
    framebuffer->Green            = video->Green;
    framebuffer->Blue             = video->Blue;
    framebuffer->Alpha            = video->Alpha;
    return OS_EOK;
}

void
FramebufferDestroy(
    _In_ Framebuffer_t* framebuffer)
{
    if (framebuffer == NULL) {
        return;
    }
    free(framebuffer->Scratch);
    framebuffer->Scratch = NULL;
}

uint32_t
FramebufferColor(
    _In_ const Framebuffer_t* framebuffer,
    _In_ uint32_t             rgb)
{
    return __ScaleComponent(&framebuffer->Red, (rgb >> 16) & 0xFF) |
           __ScaleComponent(&framebuffer->Green, (rgb >> 8) & 0xFF) |
           __ScaleComponent(&framebuffer->Blue, rgb & 0xFF) |
           __ScaleComponent(&framebuffer->Alpha, 0xFF);
}

void
FramebufferClear(
    _In_ Framebuffer_t* framebuffer)
{
    uint32_t top;

    // Scratch is one text row high, so the screen is filled one text row at
    // a time. The last copy is moved up to end at the bottom of the screen,
    // which covers the rows that are left when the height is not a multiple
    // of the font height.
    __FillScratch(framebuffer, FramebufferColor(framebuffer, __FB_BACKGROUND));
    for (top = 0; top + g_fontHeight <= framebuffer->Height; top += g_fontHeight) {
        __CopyScratch(framebuffer, top);
    }
    if (top < framebuffer->Height) {
        __CopyScratch(framebuffer, framebuffer->Height - g_fontHeight);
    }
}

void
FramebufferDrawGrid(
    _In_ Framebuffer_t* framebuffer,
    _In_ TextGrid_t*    grid)
{
    uint32_t background = FramebufferColor(framebuffer, __FB_BACKGROUND);

    // DirtyFirst is larger than DirtyLast when no row changed, and then the
    // loop does not run.
    for (uint32_t row = grid->DirtyFirst; row <= grid->DirtyLast && row < grid->Rows; row++) {
        const TextCell_t* cells = TextGridRow(grid, row);

        __FillScratch(framebuffer, background);
        for (uint32_t column = 0; column < grid->Columns; column++) {
            __DrawCell(framebuffer, column, &cells[column]);
        }
        __CopyScratch(framebuffer, FRAMEBUFFER_MARGIN + row * g_fontHeight);
    }
    TextGridClearDirty(grid);
}
