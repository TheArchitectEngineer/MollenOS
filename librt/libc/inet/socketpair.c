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
 *
 * Standard C Support
 * - Standard Socket IO Implementation
 */

#include <os/syslog.h>
#include <errno.h>
#include <inet/socket.h>
#include <internal/_io.h>
#include <io.h>
#include <os/mollenos.h>
#include <os/services/net.h>

// Closes a socket that socketpair opened, without changing errno. The caller
// has already set errno for the failure it reports.
static void
__close_pair_socket(
        _In_ int iod)
{
    int error = errno;

    if (close(iod)) {
        // The descriptor stays open. Nothing more can be done from here.
        ERROR("socketpair: failed to close socket %i: %i", iod, errno);
    }
    _set_errno(error);
}

int socketpair(int domain, int type, int protocol, int* iods)
{
    stdio_handle_t* io_object1;
    stdio_handle_t* io_object2;
    int             status;

    if (!iods) {
        _set_errno(EINVAL);
        return -1;
    }

    // On failure both entries are -1, so the caller never sees a stale number.
    iods[1] = -1;
    iods[0] = socket(domain, type, protocol);
    if (iods[0] == -1) {
        return -1;
    }

    // From here every failure must close the sockets that are already open,
    // so the caller is not left with descriptors it never got back.
    iods[1] = socket(domain, type, protocol);
    if (iods[1] == -1) {
        __close_pair_socket(iods[0]);
        iods[0] = -1;
        return -1;
    }

    io_object1 = stdio_handle_get(iods[0]);
    io_object2 = stdio_handle_get(iods[1]);
    if (io_object1 == NULL || io_object2 == NULL) {
        _set_errno(EBADF);
        status = -1;
    } else {
        status = OsErrToErrNo(
                OSSocketPair(
                        &io_object1->OSHandle,
                        &io_object2->OSHandle
                )
        );
    }

    if (status) {
        __close_pair_socket(iods[1]);
        __close_pair_socket(iods[0]);
        iods[0] = -1;
        iods[1] = -1;
        return -1;
    }
    return 0;
}
