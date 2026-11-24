/* MollenOS
 *
 * Copyright 2011 - 2017, Philip Meulengracht
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
 * Boot Output Interface
 * - The boot video mode and the serial port, which every architecture
 *   provides. The kernel does not draw on the screen.
 */

#ifndef __VALI_OUTPUT_H__
#define __VALI_OUTPUT_H__

#include <os/osdefs.h>
#include <os/types/video.h>

/**
 * @brief The boot video mode and the outputs the kernel log can print to.
 *
 * AvailableOutputs holds VIDEO_* flags. The kernel only prints to the serial
 * port (VIDEO_UART). It never draws on the screen. Info and
 * FrameBufferAddressPhysical describe the boot video mode, so the syslog
 * service can map the framebuffer and draw the log there.
 */
typedef struct BootTerminal {
    unsigned int            AvailableOutputs;
    OSBootVideoDescriptor_t Info;
    uintptr_t               FrameBufferAddressPhysical;
} BootTerminal_t;

// The serial port is set up and the log can print to it.
#define VIDEO_UART 0x00000001

/**
 * @brief Returns the boot video mode and the available outputs.
 */
KERNELAPI BootTerminal_t* KERNELABI
VideoGetTerminal(void);

/**
 * Initializes the serial output for the operating system. This is the output that is
 * always active if a serial port is present.
 */
KERNELAPI oserr_t KERNELABI
SerialPortInitialize(void);

/**
 * Outputs a character to the default serial port if available.
 * @param character [In] The character to write
 */
void
SerialPutCharacter(
        _In_ int character);

#endif //!__VALI_OUTPUT_H__
