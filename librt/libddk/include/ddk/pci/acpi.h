/**
 * Copyright, Philip Meulengracht
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

#ifndef __DDK_PCI_ACPI_H__
#define __DDK_PCI_ACPI_H__

#include <ddk/ddkdefs.h>
#include <os/types/acpi_pci.h>

/**
 * @brief Looks up PCI root and window information that the kernel copied from
 * ACPI, or evaluates _PRT to find the INTx route of a device.
 *
 * @param query The query to run. Fields that are not used by the query type
 *              must be zero.
 * @param result Set to the result of the query on success.
 * @return OS_EOK on success, or OS_ENOENT when there are no more entries to
 *         enumerate or the device has no route. On failure result is not changed.
 */
DDKDECL(oserr_t,
AcpiQueryPci(
    _In_  const struct AcpiPciQuery* query,
    _Out_ union AcpiPciResult*      result));

#endif //!__DDK_PCI_ACPI_H__
