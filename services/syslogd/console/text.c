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

#include <stdlib.h>
#include "text.h"

// Tab stops are placed every this many columns.
#define __TEXT_TAB_WIDTH 8

// Code point shown for bytes that are not valid UTF-8.
#define __TEXT_REPLACEMENT_CHARACTER 0xFFFD

/**
 * @brief Decodes the UTF-8 character at text[*index] and moves *index past it.
 *
 * Any process can write to the kernel log, so the text is not trusted. A
 * malformed sequence (a stray continuation byte, a lead byte without enough
 * continuation bytes, an overlong form, or a value above U+10FFFF) becomes
 * U+FFFD, and only its first byte is skipped. The decoder never reads past
 * the zero byte at the end of text.
 *
 * Lead byte forms:
 * - 0xxxxxxx: one byte, U+0000 to U+007F.
 * - 110xxxxx: two bytes, from U+0080.
 * - 1110xxxx: three bytes, from U+0800.
 * - 11110xxx: four bytes, from U+10000.
 * Each continuation byte has the form 10xxxxxx and adds 6 bits.
 */
static uint32_t
__DecodeNext(
    _In_    const char* text,
    _InOut_ int*        index)
{
    const unsigned char* bytes   = (const unsigned char*)text + *index;
    uint32_t             lead    = bytes[0];
    uint32_t             character;
    uint32_t             minimum;
    int                  length;

    if (lead < 0x80) {
        (*index)++;
        return lead;
    }

    if ((lead & 0xE0) == 0xC0) {
        character = lead & 0x1F;
        minimum   = 0x80;
        length    = 2;
    } else if ((lead & 0xF0) == 0xE0) {
        character = lead & 0x0F;
        minimum   = 0x800;
        length    = 3;
    } else if ((lead & 0xF8) == 0xF0) {
        character = lead & 0x07;
        minimum   = 0x10000;
        length    = 4;
    } else {
        (*index)++;
        return __TEXT_REPLACEMENT_CHARACTER;
    }

    // The zero byte at the end is not a continuation byte, so this stops at
    // the end of the text.
    for (int i = 1; i < length; i++) {
        if ((bytes[i] & 0xC0) != 0x80) {
            (*index)++;
            return __TEXT_REPLACEMENT_CHARACTER;
        }
        character = (character << 6) | (bytes[i] & 0x3F);
    }

    if (character < minimum || character > 0x10FFFF) {
        (*index)++;
        return __TEXT_REPLACEMENT_CHARACTER;
    }
    *index += length;
    return character;
}

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
    int index = 0;

    while (text[index] != '\0') {
        uint32_t character = __DecodeNext(text, &index);
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
