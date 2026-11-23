/**
 * Copyright 2020, Philip Meulengracht
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
 */

#ifndef __IPCONTEXT_H__
#define	__IPCONTEXT_H__

#include <os/types/ipc.h>
#include <time.h>

_CODE_BEGIN
CRTDECL(int, ipcontext(unsigned int len, IPCAddress_t* addr));
/**
 * @brief Sends one IPC message from an IPC stream to the given address.
 * @return 0 if the message was sent. Otherwise -1 with errno set: EBADF when
 *         iod is not open, ENOTSOCK when iod is open but is not an IPC context,
 *         or the errno of the failed send.
 */
CRTDECL(int, ipsend(int iod, IPCAddress_t* addr, const void* data, unsigned int len, const struct timespec* deadline));

/**
 * @brief Recieve an IPC message from an IPC stream. Unless IPC_DONTWAIT is passed in flags this
 * is a blocking operation, and will wait for a message to be available.
 * @param[In] iod The io descriptor the ipc stream is bound to.
 * @param[In] buffer The buffer to store the message in.
 * @param[In] len The maximum number of bytes to read from the IPC message. It's important to note
 *                that if the message is longer than this value, then the message is truncated to this
 *                length.
 * @param[In] flags Flags supported are IPC_* flags, like IPC_DONTWAIT.
 * @param[Out] msgContext If a message was received, then this contains the sender's address & capabilities.
 *                        This handle can then be used in conjunction with IPC_ADDRESS_HANDLE_INIT.
 * @return If successful, returns the number of bytes received in buffer
 *         If an error occurs, this function returns -1, and sets a status code
 *         in errno: EINVAL for a NULL buffer or a zero len, EBADF when iod is
 *         not open, ENOTSOCK when iod is open but is not an IPC context, and
 *         ENODATA when no message was read.
 */
CRTDECL(int, iprecv(int iod, void* buffer, unsigned int len, int flags, IPCMessageContext_t* msgContext));

_CODE_END
#endif //!__IPCONTEXT_H__
