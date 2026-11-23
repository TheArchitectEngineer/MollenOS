/**
 * Copyright 2026, Philip Meulengracht
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
 * Description of the video mode that the bootloader set up.
 */

#ifndef __OS_TYPES_VIDEO_H__
#define __OS_TYPES_VIDEO_H__

#include <os/osdefs.h>

enum OSBootVideoType {
    OSBOOTVIDEO_NONE,
    OSBOOTVIDEO_TTY,
    OSBOOTVIDEO_FRAMEBUFFER
};

typedef struct VideoColorComponent {
    uint8_t  Position;
    uint32_t Mask;
} VideoColorComponent_t;

typedef struct OSBootVideoDescriptor {
    enum OSBootVideoType  Type;
    size_t                BytesPerScanline;
    size_t                Height;
    size_t                Width;
    int                   DepthBits;
    VideoColorComponent_t Red;
    VideoColorComponent_t Green;
    VideoColorComponent_t Blue;
    VideoColorComponent_t Alpha;
} OSBootVideoDescriptor_t;

#endif //!__OS_TYPES_VIDEO_H__
