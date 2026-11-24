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
 * BCM2711 wrapper: device matching, the RGMII and MDIO settings for the Pi 4
 * and the MAC reset line. See wrapper.h for what the wrapper is.
 */

#include <ddk/devices/platform.h>
#include <ddk/time.h>
#include <ddk/utils.h>
#include <string.h>

#include "registers.h"
#include "wrapper.h"
#include "../common/diagnostics.h"

bool
GenetWrapperMatchesDevice(
    _In_ const Device_t* device)
{
    const PlatformDevice_t* platform = (const PlatformDevice_t*)device;
    size_t                  offset = 0;

    if (device->Length != sizeof(PlatformDevice_t) || !PlatformDeviceValidate(platform)) {
        return false;
    }

    // Compatibles is a list of strings, each ending in a zero byte.
    while (offset < platform->CompatibleLength) {
        if (!strcmp(platform->Compatibles + offset, GENET_COMPATIBLE_BCM2711)) {
            return true;
        }
        offset += strlen(platform->Compatibles + offset) + 1;
    }
    return false;
}

static oserr_t
__WriteRegisterAndVerify(
    _In_ DeviceIo_t* registers,
    _In_ size_t      offset,
    _In_ uint32_t    value)
{
    uint32_t readBack;
    oserr_t  status;

    status = GenetRegisterWrite(registers, offset, value);
    if (status != OS_EOK) {
        ERROR("GENET wrapper: writing 0x%08x to offset 0x%x failed: %u", value, (unsigned)offset, status);
        return status;
    }

    readBack = GenetRegisterRead(registers, offset);
    if (readBack != value) {
        ERROR("GENET wrapper: offset 0x%x reads 0x%08x after writing 0x%08x",
            (unsigned)offset, readBack, value);
        return OS_EUNKNOWN;
    }
    return OS_EOK;
}

oserr_t
GenetWrapperConfigurePhy(
    _InOut_ GenetWrapper_t*                           wrapper,
    _In_    DeviceIo_t*                               registers,
    _In_    const struct DeviceEthernetConfiguration* configuration)
{
    uint32_t revision;
    uint32_t oob;
    uint32_t mdio;
    oserr_t  status;

    if (GenetWrapperIsVerified(wrapper)) {
        ERROR("GENET wrapper: the PHY link is already configured");
        return OS_EBUSY;
    }

    // Check everything the firmware told us before the first register access.
    if (registers->Type != DeviceIoMemoryBased ||
        registers->Access.Memory.Length < GENET_REGISTER_WINDOW_SIZE) {
        ERROR("GENET wrapper: the register window is not memory mapped or is shorter than 0x%x bytes",
            GENET_REGISTER_WINDOW_SIZE);
        return OS_ENOTSUPPORTED;
    }

    if (configuration->Version != 1 || configuration->Mode != DeviceEthernetRgmiiRxId) {
        ERROR("GENET wrapper: configuration version %u with PHY mode %u is not supported, only rgmii-rxid is",
            configuration->Version, configuration->Mode);
        return OS_ENOTSUPPORTED;
    }

    if (configuration->MdioOffset != GENET_MDIO_COMMAND ||
        configuration->MdioLength != GENET_MDIO_WINDOW_SIZE) {
        ERROR("GENET wrapper: MDIO block at offset 0x%x with length %u is not supported",
            configuration->MdioOffset, configuration->MdioLength);
        return OS_ENOTSUPPORTED;
    }

    revision = GenetRegisterRead(registers, GENET_SYS_REVISION_CTRL);
    ETHERNET_LOG("GENET revision=0x%08x", revision);
    if (((revision >> GENET_SYS_REVISION_MAJOR_SHIFT) & GENET_SYS_REVISION_MAJOR_MASK) !=
        GENET_SYS_REVISION_MAJOR_V5) {
        ERROR("GENET wrapper: revision register 0x%08x does not report GENET v5", revision);
        return OS_ENOTSUPPORTED;
    }

    // Save the original values before the first write. The stage is moved
    // forward before each write, because a failed write may still have
    // changed the register.
    wrapper->Registers = registers;
    wrapper->Port = GenetRegisterRead(registers, GENET_SYS_PORT_CTRL);
    wrapper->Oob = GenetRegisterRead(registers, GENET_RGMII_OOB_CTRL);
    wrapper->Mdio = GenetRegisterRead(registers, GENET_MDIO_CONFIG);
    wrapper->State = GENET_WRAPPER_PORT_CHANGED;

    status = __WriteRegisterAndVerify(registers, GENET_SYS_PORT_CTRL, GENET_SYS_PORT_MODE_EXT_GPHY);
    if (status != OS_EOK) {
        return status;
    }

    // In rgmii-rxid mode the PHY adds the receive clock delay, and GENET adds
    // the transmit clock delay. Clearing ID_MODE_DIS lets GENET do its part, so
    // the PHY does not need to add a second transmit delay. The link bit stays
    // clear until a link is applied.
    oob = wrapper->Oob & ~(GENET_RGMII_ID_MODE_DISABLE | GENET_RGMII_OOB_DISABLE | GENET_RGMII_LINK);
    oob |= GENET_RGMII_MODE_ENABLE;
    wrapper->State = GENET_WRAPPER_OOB_CHANGED;

    status = __WriteRegisterAndVerify(registers, GENET_RGMII_OOB_CTRL, oob);
    if (status != OS_EOK) {
        return status;
    }

    // Set the 2.5 MHz management bus clock, the Clause 22 register access
    // format and a preamble before every transaction.
    mdio = wrapper->Mdio & ~(GENET_MDIO_CLOCK_DIVIDER_MASK | GENET_MDIO_SUPPRESS_PREAMBLE);
    mdio |= (GENET_MDIO_CLOCK_DIVIDER_PI4 << GENET_MDIO_CLOCK_DIVIDER_SHIFT) | GENET_MDIO_CONFIG_CLAUSE22;
    wrapper->State = GENET_WRAPPER_MDIO_CHANGED;

    status = __WriteRegisterAndVerify(registers, GENET_MDIO_CONFIG, mdio);
    if (status != OS_EOK) {
        return status;
    }

    ETHERNET_LOG("GENET integration verified: mode=rgmii-rxid MAC-TX-delay=on OOB=0x%08x MDIO=0x%08x MDC=2500000",
        GenetRegisterRead(registers, GENET_RGMII_OOB_CTRL), mdio);
    return OS_EOK;
}

oserr_t
GenetWrapperRestorePhy(
    _InOut_ GenetWrapper_t* wrapper)
{
    enum GenetWrapperState next;
    oserr_t                status;

    // Undo the writes in the reverse order of GenetWrapperConfigurePhy. Each
    // stage moves on only after its register reads back the original value, so
    // a failed restore retries from the register that is still changed.
    while (wrapper->State != GENET_WRAPPER_UNCHANGED) {
        switch (wrapper->State) {
            case GENET_WRAPPER_MDIO_CHANGED:
                status = __WriteRegisterAndVerify(wrapper->Registers, GENET_MDIO_CONFIG, wrapper->Mdio);
                next = GENET_WRAPPER_OOB_CHANGED;
                break;
            case GENET_WRAPPER_OOB_CHANGED:
                status = __WriteRegisterAndVerify(wrapper->Registers, GENET_RGMII_OOB_CTRL, wrapper->Oob);
                next = GENET_WRAPPER_PORT_CHANGED;
                break;
            case GENET_WRAPPER_PORT_CHANGED:
                status = __WriteRegisterAndVerify(wrapper->Registers, GENET_SYS_PORT_CTRL, wrapper->Port);
                next = GENET_WRAPPER_UNCHANGED;
                break;
            default:
                ERROR("GENET wrapper: unknown restore stage %u", wrapper->State);
                return OS_EUNKNOWN;
        }
        if (status != OS_EOK) {
            ERROR("GENET wrapper: restoring stage %u failed: %u", wrapper->State, status);
            return status;
        }
        wrapper->State = next;
    }

    wrapper->Registers = NULL;
    return OS_EOK;
}

oserr_t
GenetWrapperAssertReset(
    _InOut_ GenetWrapper_t* wrapper,
    _In_    DeviceIo_t*     registers)
{
    oserr_t status;

    // The reset state is unknown from the moment the write is attempted, since
    // a failed write may still have reached the card.
    wrapper->Reset = GENET_WRAPPER_RESET_UNKNOWN;
    status = GenetRegisterWrite(registers, GENET_SYS_RBUF_FLUSH_CTRL, GENET_SYS_MAC_RESET);
    if (status != OS_EOK) {
        ERROR("GENET wrapper: asserting the MAC reset failed: %u", status);
        return status;
    }

    status = DDKStallUs(GENET_WRAPPER_RESET_SETTLE_US);
    if (status != OS_EOK) {
        ERROR("GENET wrapper: waiting with the MAC reset asserted failed: %u", status);
    }
    return status;
}

oserr_t
GenetWrapperReleaseReset(
    _InOut_ GenetWrapper_t* wrapper,
    _In_    DeviceIo_t*     registers)
{
    oserr_t status;

    wrapper->Reset = GENET_WRAPPER_RESET_UNKNOWN;

    // Firmware may have left the wrapper in reset. Release it before touching
    // the UniMAC command register, which includes the first DMA shutdown. This
    // does not reset the MAC address registers, which we may still need to read
    // as a fallback when firmware gave no address.
    ETHERNET_LOG("GENET reset release: SYS_RBUF_FLUSH_CTRL=0x%08x clear write begin",
        GenetRegisterRead(registers, GENET_SYS_RBUF_FLUSH_CTRL));
    status = GenetRegisterWrite(registers, GENET_SYS_RBUF_FLUSH_CTRL, 0);
    if (status != OS_EOK) {
        ERROR("GENET wrapper: releasing the MAC reset failed: %u", status);
        return status;
    }

    ETHERNET_LOG("GENET reset release: delay begin");
    status = DDKStallUs(GENET_WRAPPER_RESET_SETTLE_US);
    if (status != OS_EOK) {
        ERROR("GENET wrapper: waiting after the MAC reset release failed: %u", status);
        return status;
    }

    wrapper->Reset = GENET_WRAPPER_RESET_RELEASED;
    ETHERNET_LOG("GENET reset release: complete");
    return OS_EOK;
}
