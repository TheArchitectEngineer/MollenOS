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
 * Register list writes. The MAC reset and the ring setup are fixed sequences
 * of register writes and short waits, so they are kept as tables and run from
 * here.
 */

#include <ddk/time.h>
#include <ddk/utils.h>

#include "registers.h"

oserr_t
GenetRegisterWriteList(
    _In_ DeviceIo_t*                   registers,
    _In_ uint32_t                      base,
    _In_ const GenetRegisterSetting_t* settings,
    _In_ size_t                        count)
{
    uint32_t offset;
    oserr_t  status;

    for (size_t i = 0; i < count; i++) {
        if (settings[i].Offset == GENET_REGISTER_WAIT_OFFSET) {
            status = DDKStallUs(settings[i].Value);
            if (status != OS_EOK) {
                ERROR("GENET: waiting %u us in a register sequence failed: %u",
                    settings[i].Value, status);
                return status;
            }
            continue;
        }

        offset = base + settings[i].Offset;
        status = GenetRegisterWrite(registers, offset, settings[i].Value);
        if (status != OS_EOK) {
            ERROR("GENET: writing 0x%08x to register 0x%x failed: %u",
                settings[i].Value, offset, status);
            return status;
        }
    }
    return OS_EOK;
}
