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
 * Clause 22 transactions through the UniMAC MDIO block. See mdio.h.
 */

#include <ddk/time.h>
#include <ddk/utils.h>

#include "registers.h"
#include "mdio.h"

// A Clause 22 transaction is 64 bits long. At 2.5 MHz that takes about 26 us.
// The wait starts with short steps, so a normal transaction costs tens of
// microseconds. Each step is twice as long as the one before, up to 1 ms, so
// a PHY that never answers does not cost thousands of register reads. The
// wait gives up after 100 ms in total.
#define GENET_MDIO_FIRST_STEP_US   10U
#define GENET_MDIO_LONGEST_STEP_US 1000U
#define GENET_MDIO_TIMEOUT_US      100000U

static oserr_t
__WaitMdioIdle(
    _In_  GenetMdio_t* mdio,
    _Out_ uint32_t*    value)
{
    uint32_t waited = 0;
    uint32_t step = GENET_MDIO_FIRST_STEP_US;
    oserr_t  status;

    while (1) {
        *value = GenetRegisterRead(mdio->Registers, GENET_MDIO_COMMAND);
        if (!(*value & GENET_MDIO_BUSY)) {
            return OS_EOK;
        }

        if (waited >= GENET_MDIO_TIMEOUT_US) {
            WARNING("GENET MDIO: still busy after %u us, command=0x%08x", waited, *value);
            return OS_ETIMEOUT;
        }

        // The last step is cut short so the total wait is exactly the timeout.
        if (step > GENET_MDIO_TIMEOUT_US - waited) {
            step = GENET_MDIO_TIMEOUT_US - waited;
        }

        status = DDKStallUs(step);
        if (status != OS_EOK) {
            ERROR("GENET MDIO: waiting %u us failed: %u", step, status);
            return status;
        }

        waited += step;
        step *= 2;
        if (step > GENET_MDIO_LONGEST_STEP_US) {
            step = GENET_MDIO_LONGEST_STEP_US;
        }
    }
}

static oserr_t
__Transaction(
    _In_  GenetMdio_t* mdio,
    _In_  uint32_t     command,
    _Out_ uint32_t*    result)
{
    oserr_t status;

    status = __WaitMdioIdle(mdio, result);
    if (status != OS_EOK) {
        return status;
    }

    status = GenetRegisterWrite(
        mdio->Registers,
        GENET_MDIO_COMMAND,
        command | GENET_MDIO_BUSY
    );
    if (status != OS_EOK) {
        ERROR("GENET MDIO: writing command 0x%08x failed: %u", command, status);
        return status;
    }

    status = __WaitMdioIdle(mdio, result);
    if (status != OS_EOK) {
        return status;
    }
    return (*result & GENET_MDIO_FAILED) ? OS_EDEVFAULT : OS_EOK;
}

static oserr_t
__Read(
    _In_  void*     context,
    _In_  uint8_t   address,
    _In_  uint8_t   reg,
    _Out_ uint16_t* value)
{
    uint32_t result;
    oserr_t  status;

    status = __Transaction(
        context,
        GENET_MDIO_OPERATION_READ | ((uint32_t)address << GENET_MDIO_PHY_SHIFT) |
        ((uint32_t)reg << GENET_MDIO_REGISTER_SHIFT),
        &result
    );
    if (status == OS_EOK) {
        *value = (uint16_t)(result & GENET_MDIO_DATA_MASK);
    }
    return status;
}

static oserr_t
__Write(
    _In_ void*    context,
    _In_ uint8_t  address,
    _In_ uint8_t  reg,
    _In_ uint16_t value)
{
    uint32_t result;

    return __Transaction(
        context,
        GENET_MDIO_OPERATION_WRITE | ((uint32_t)address << GENET_MDIO_PHY_SHIFT) |
        ((uint32_t)reg << GENET_MDIO_REGISTER_SHIFT) | value,
        &result
    );
}

static oserr_t
__Delay(
    _In_ void*    context,
    _In_ uint32_t microseconds)
{
    // The delay needs nothing from the MDIO context.
    (void)context;
    return DDKStallUs(microseconds);
}

const struct MdioOperations g_genetMdioOperations = {
    .Read = __Read,
    .Write = __Write,
    .DelayUs = __Delay
};
