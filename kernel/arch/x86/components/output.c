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
 * Boot Output
 * - Records the boot video mode for the syslog service, which draws the
 *   kernel log on the screen. The kernel itself never draws.
 * - Writes kernel log output to the first serial port (COM1).
 */

#include <machine.h>
#include <arch/output.h>
#include <arch/io.h>
#include <arch/x86/vbe.h>

static BootTerminal_t g_bootTerminal = { 0 };

static void
__SetTerminalMode(
        _In_ struct VBootVideo* video)
{
    g_bootTerminal.Info.Type                  = OSBOOTVIDEO_TTY;
    g_bootTerminal.Info.Width                 = video->Width;
    g_bootTerminal.Info.Height                = video->Height;
    g_bootTerminal.Info.DepthBits             = 16;
    g_bootTerminal.Info.BytesPerScanline      = 2 * video->Width;
    g_bootTerminal.FrameBufferAddressPhysical = STD_VIDEO_MEMORY;
}

static void
__SetFramebufferMode(
        _In_ struct VBootVideo* video)
{
    g_bootTerminal.Info.Type                  = OSBOOTVIDEO_FRAMEBUFFER;
    g_bootTerminal.FrameBufferAddressPhysical = video->FrameBuffer;
    g_bootTerminal.Info.Width                 = video->Width;
    g_bootTerminal.Info.Height                = video->Height;
    g_bootTerminal.Info.DepthBits             = (int)video->BitsPerPixel;
    g_bootTerminal.Info.BytesPerScanline      = video->Pitch;
    g_bootTerminal.Info.Red.Position          = (int)video->RedPosition;
    g_bootTerminal.Info.Red.Mask              = (int)video->RedMask;
    g_bootTerminal.Info.Blue.Position         = (int)video->BluePosition;
    g_bootTerminal.Info.Blue.Mask             = (int)video->BlueMask;
    g_bootTerminal.Info.Green.Position        = (int)video->GreenPosition;
    g_bootTerminal.Info.Green.Mask            = (int)video->GreenMask;
    g_bootTerminal.Info.Alpha.Position        = (int)video->ReservedPosition;
    g_bootTerminal.Info.Alpha.Mask            = (int)video->ReservedMask;
}

void
OutputInitialize(void)
{
    // Which kind of mode has been enabled for us
    if (GetMachine()->BootInformation.Video.FrameBuffer) {
        __SetFramebufferMode(&GetMachine()->BootInformation.Video);
    }
    else {
        // Either headless or terminal mode
        if (!GetMachine()->BootInformation.Video.Pitch) {
            return;
        }
        __SetTerminalMode(&GetMachine()->BootInformation.Video);
    }
}

BootTerminal_t*
VideoGetTerminal(void)
{
    return &g_bootTerminal;
}

void
SerialPutCharacter(
        _In_ int character)
{
    size_t lineStatus      = 0x0;
    size_t characterBuffer = (size_t)(character & 0xFF);

    if (!(g_bootTerminal.AvailableOutputs & VIDEO_UART)) {
        return;
    }

    while (!(lineStatus & 0x20)) {
        ReadDirectIo(DeviceIoPortBased, 0x3F8 + 5, 1, &lineStatus);
    }
    WriteDirectIo(DeviceIoPortBased, 0x3F8, 1, characterBuffer);
}

oserr_t
SerialPortInitialize(void)
{
    // Initalize the UART port (1)
#ifdef __OSCONFIG_DEBUGMODE
    g_bootTerminal.AvailableOutputs |= VIDEO_UART;
#endif
    return OS_EOK;
}
