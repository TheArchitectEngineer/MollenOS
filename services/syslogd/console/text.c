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
 * - Stores the characters shown on the screen and moves the cursor.
 */

#include <ds/mstring.h>
#include <stdlib.h>
#include "text.h"

// Tab stops are placed every this many columns.
#define __TEXT_TAB_WIDTH 8

static TextCell_t*
__StoredRow(
    _In_ const TextGrid_t* grid,
    _In_ uint32_t          screenRow)
{
    uint32_t storedRow = (grid->FirstRow + screenRow) % grid->Rows;
    return &grid->Cells[(size_t)storedRow * grid->Columns];
}

static void
__ClearRow(
    _In_ TextGrid_t* grid,
    _In_ uint32_t    screenRow)
{
    TextCell_t* cells = __StoredRow(grid, screenRow);

    for (uint32_t i = 0; i < grid->Columns; i++) {
        cells[i].Character = ' ';
        cells[i].Color     = 0;
    }
}

static void
__MarkDirty(
    _In_ TextGrid_t* grid,
    _In_ uint32_t    screenRow)
{
    if (grid->DirtyFirst > grid->DirtyLast) {
        grid->DirtyFirst = screenRow;
        grid->DirtyLast  = screenRow;
        return;
    }
    if (screenRow < grid->DirtyFirst) {
        grid->DirtyFirst = screenRow;
    }
    if (screenRow > grid->DirtyLast) {
        grid->DirtyLast = screenRow;
    }
}

/**
 * @brief Moves the cursor to the start of the next row. On the last row the
 * grid scrolls instead. The top row is reused as the new, empty bottom row.
 */
static void
__NewLine(
    _In_ TextGrid_t* grid)
{
    grid->CursorColumn = 0;
    if (grid->CursorRow + 1 < grid->Rows) {
        grid->CursorRow++;
        return;
    }

    grid->FirstRow = (grid->FirstRow + 1) % grid->Rows;
    __ClearRow(grid, grid->Rows - 1);
    TextGridMarkAllDirty(grid);
}

static void
__PutCharacter(
    _In_ TextGrid_t* grid,
    _In_ uint32_t    color,
    _In_ uint32_t    character)
{
    TextCell_t* cell;

    if (grid->CursorColumn == grid->Columns) {
        __NewLine(grid);
    }

    cell = &__StoredRow(grid, grid->CursorRow)[grid->CursorColumn];
    cell->Character = character;
    cell->Color     = color;
    __MarkDirty(grid, grid->CursorRow);
    grid->CursorColumn++;
}

oserr_t
TextGridCreate(
    _In_ TextGrid_t* grid,
    _In_ uint32_t    columns,
    _In_ uint32_t    rows)
{
    if (grid == NULL || columns == 0 || rows == 0) {
        return OS_EINVALPARAMS;
    }

    grid->Cells = malloc((size_t)columns * rows * sizeof(TextCell_t));
    if (grid->Cells == NULL) {
        return OS_EOOM;
    }

    grid->Columns      = columns;
    grid->Rows         = rows;
    grid->FirstRow     = 0;
    grid->CursorRow    = 0;
    grid->CursorColumn = 0;
    for (uint32_t i = 0; i < rows; i++) {
        __ClearRow(grid, i);
    }
    TextGridMarkAllDirty(grid);
    return OS_EOK;
}

void
TextGridDestroy(
    _In_ TextGrid_t* grid)
{
    if (grid == NULL) {
        return;
    }
    free(grid->Cells);
    grid->Cells = NULL;
}

void
TextGridWrite(
    _In_ TextGrid_t* grid,
    _In_ uint32_t    color,
    _In_ const char* text)
{
    int     index = 0;
    mchar_t character;

    // Any process can write to the kernel log, so the text is not trusted.
    // mstr_next reads at most 4 bytes per character, never reads past the
    // zero byte, and returns U+FFFD for bytes that are not valid UTF-8.
    for (;;) {
        character = mstr_next(text, &index);
        if (character == MSTRING_EOS) {
            break;
        }

        switch (character) {
            case '\n': {
                __NewLine(grid);
            } break;
            case '\r': {
                grid->CursorColumn = 0;
            } break;
            case '\t': {
                // A tab at the right edge moves to the next row, like any
                // other character would.
                do {
                    __PutCharacter(grid, color, ' ');
                } while (grid->CursorColumn % __TEXT_TAB_WIDTH != 0 &&
                         grid->CursorColumn < grid->Columns);
            } break;
            default: {
                if (character >= ' ') {
                    __PutCharacter(grid, color, character);
                }
            } break;
        }
    }
}

const TextCell_t*
TextGridRow(
    _In_ const TextGrid_t* grid,
    _In_ uint32_t          screenRow)
{
    return __StoredRow(grid, screenRow);
}

void
TextGridMarkAllDirty(
    _In_ TextGrid_t* grid)
{
    grid->DirtyFirst = 0;
    grid->DirtyLast  = grid->Rows - 1;
}

void
TextGridClearDirty(
    _In_ TextGrid_t* grid)
{
    grid->DirtyFirst = 1;
    grid->DirtyLast  = 0;
}
