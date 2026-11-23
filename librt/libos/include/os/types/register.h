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
 * Integer types for hardware registers. reg_t has the width of a general
 * purpose register of the CPU.
 */

#ifndef __OS_TYPES_REGISTER_H__
#define __OS_TYPES_REGISTER_H__

#include <os/osdefs.h>

typedef uint32_t reg32_t;
typedef uint64_t reg64_t;

#if defined(TESTING)
// Host tests do not depend on the register width.
#elif defined(i386) || defined(__i386__)
typedef reg32_t reg_t;
#elif defined(amd64) || defined(__amd64__) || defined(__aarch64__)
typedef reg64_t reg_t;
#else
#error "Define reg_t for this architecture"
#endif

#endif //!__OS_TYPES_REGISTER_H__
