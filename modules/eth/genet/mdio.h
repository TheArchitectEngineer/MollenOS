/**
 * Copyright 2026, Philip Meulengracht
 *
 * This program is free software : you can redistribute it and / or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
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
 * Access to the PHY over MDIO, the two-wire management bus. GENET runs one
 * Clause 22 transaction at a time through its UniMAC MDIO block. This file
 * supplies the read, write and delay operations the common MDIO code calls.
 */

#ifndef __GENET_MDIO_H__
#define __GENET_MDIO_H__

#include <ddk/io.h>

#include "../common/mdio.h"

/**
 * @brief Context of the GENET MDIO operations.
 * Registers is the mapped GENET window, which holds the fixed MDIO block. The
 * common MDIO code runs one transaction at a time, so no other state is needed.
 */
typedef struct GenetMdio {
    DeviceIo_t* Registers;
} GenetMdio_t;

// Operations for MdioInitialize. Their context is a GenetMdio_t.
extern const struct MdioOperations g_genetMdioOperations;

#endif //!__GENET_MDIO_H__
