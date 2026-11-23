/**
 * MollenOS
 *
 * Copyright 2019, Philip Meulengracht
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
 * What a system service receives when it starts. The lookup of other services
 * is in os/services/lookup.h, which this header includes.
 */

#ifndef __DDK_SERVICE_H__
#define __DDK_SERVICE_H__

#include <ddk/ddkdefs.h>
#include <os/services/lookup.h>

_CODE_BEGIN

// imported from gracht types.h, to avoid including that header
typedef struct gracht_server gracht_server_t;

struct ServiceStartupOptions {
    // Server is the handle to the gracht server itself. This is exposed to allow
    // services to register the protocols they support.
    gracht_server_t* Server;
    // ServerHandle is the handle of gracht server link. This is exposed to allow
    // services to share their handle with other procceses to allow communication.
    // While this is possible to discover with just the above Server handle, we add
    // it here for convenience.
    uuid_t ServerHandle;
};

_CODE_END

#endif //!__DDK_SERVICE_H__
