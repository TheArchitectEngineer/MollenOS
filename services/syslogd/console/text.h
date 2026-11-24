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
 * Text Grid
 * - Holds the characters shown on the screen, one cell per character, and
 *   the cursor where the next character goes. Text wraps at the right edge
 *   and the grid scrolls up when the cursor passes the last row.
 * - The grid knows nothing about pixels. The framebuffer code draws the rows
 *   that changed.
 */

#ifndef __SYSLOGD_TEXT_H__
#define __SYSLOGD_TEXT_H__

#include <os/osdefs.h>

/**
 * @brief One character on the screen.
 * Character is a Unicode code point. Color is the text color as 0xRRGGBB.
 */
typedef struct TextCell {
    uint32_t Character;
    uint32_t Color;
} TextCell_t;

/**
 * @brief A screen of text, Columns cells wide and Rows cells high.
 *
 * Cells holds Rows * Columns cells, one stored row after the other. Scrolling
 * moves FirstRow instead of copying cells, so screen row r is stored at row
 * (FirstRow + r) % Rows. Use TextGridRow to get a screen row.
 *
 * CursorRow and CursorColumn are the screen position of the next character.
 * CursorColumn equals Columns when the row is full. The text then wraps when
 * the next character is written.
 *
 * Screen rows DirtyFirst to DirtyLast changed since the last
 * TextGridClearDirty and must be drawn again. DirtyFirst is larger than
 * DirtyLast when no row changed.
 */
typedef struct TextGrid {
    TextCell_t* Cells;
    uint32_t    Columns;
    uint32_t    Rows;
    uint32_t    FirstRow;
    uint32_t    CursorRow;
    uint32_t    CursorColumn;
    uint32_t    DirtyFirst;
    uint32_t    DirtyLast;
} TextGrid_t;

/**
 * @brief Allocates an empty grid. Every cell starts as a space, and every row
 * is marked as changed, so the first draw fills the whole screen.
 *
 * @param grid    Grid to set up.
 * @param columns Number of cells in a row. Must not be zero.
 * @param rows    Number of rows. Must not be zero.
 * @return OS_EOK on success, OS_EINVALPARAMS for a zero size, or OS_EOOM
 *         when the cells could not be allocated.
 */
__EXTERN oserr_t
TextGridCreate(
    _In_ TextGrid_t* grid,
    _In_ uint32_t    columns,
    _In_ uint32_t    rows);

/**
 * @brief Frees the cells of a grid created with TextGridCreate.
 */
__EXTERN void
TextGridDestroy(
    _In_ TextGrid_t* grid);

/**
 * @brief Writes UTF-8 text at the cursor.
 *
 * Newline moves the cursor to the start of the next row, carriage return to
 * the start of the current row, and tab to the next multiple of 8 columns.
 * Other control characters are skipped, and bytes that are not valid UTF-8
 * are shown as U+FFFD. Text that reaches the right edge
 * continues on the next row. Moving below the last row scrolls the grid up by
 * one row, which marks every row as changed.
 *
 * @param grid  Grid to write to.
 * @param color Text color as 0xRRGGBB.
 * @param text  Zero terminated UTF-8 text.
 */
__EXTERN void
TextGridWrite(
    _In_ TextGrid_t* grid,
    _In_ uint32_t    color,
    _In_ const char* text);

/**
 * @brief Returns the cells of one screen row. Row 0 is the top of the screen.
 *
 * @param grid      Grid to read.
 * @param screenRow Row on the screen. Must be less than grid->Rows.
 * @return Pointer to grid->Columns cells.
 */
__EXTERN const TextCell_t*
TextGridRow(
    _In_ const TextGrid_t* grid,
    _In_ uint32_t          screenRow);

/**
 * @brief Marks every row as changed, so the next draw fills the whole screen.
 */
__EXTERN void
TextGridMarkAllDirty(
    _In_ TextGrid_t* grid);

/**
 * @brief Marks every row as drawn. Call after drawing the changed rows.
 */
__EXTERN void
TextGridClearDirty(
    _In_ TextGrid_t* grid);

#endif //!__SYSLOGD_TEXT_H__
