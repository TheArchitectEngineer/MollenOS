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
 *
 * Helpers for drivers. Brings in the log macros of os/syslog.h and adds macros
 * that wait for a condition while sleeping between the checks.
 */

#ifndef __DDK_UTILS_H__
#define __DDK_UTILS_H__

#include <ddk/ddkdefs.h>
#include <os/syslog.h>

/* Threading Utility
 * Waits for a condition to set in a busy-loop using thrd_sleep */
#define WaitForCondition(condition, runs, wait, message, ...)\
    for (unsigned int timeout_ = 0; !(condition); timeout_++) {\
        if (timeout_ >= runs) {\
             OSSysLogWrite(OSSYSLOGLEVEL_WARNING, message, __VA_ARGS__);\
             break;\
		}\
        thrd_sleep(&(struct timespec) { .tv_nsec = wait * NSEC_PER_MSEC }, NULL);\
	}

/* Threading Utility
 * Waits for a condition to set in a busy-loop using thrd_sleep */
#define WaitForConditionWithFault(fault, condition, runs, wait)\
	fault = 0; \
    for (unsigned int timeout_ = 0; !(condition); timeout_++) {\
        if (timeout_ >= (runs)) {\
			 (fault) = 1; \
             break;\
		}\
        thrd_sleep(&(struct timespec) { .tv_nsec = wait * NSEC_PER_MSEC }, NULL);\
	}


#endif //!__DDK_UTILS_H__
