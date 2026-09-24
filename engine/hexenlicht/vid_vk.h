/* vid_vk.h -- Hexenlicht's Win32 window and video modes (vid_vk.c)
 *
 * Copyright (C) 2026  Hexenlicht contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HEXENLICHT_VID_VK_H
#define HEXENLICHT_VID_VK_H

/* Per-frame housekeeping of the video layer (windowed mouse grab changes).
 * Called at the end of SCR_UpdateScreen. */
void VID_EndFrame (void);

/* Size of the window's client area in physical pixels, i.e. the size the
 * Vulkan swapchain must have. */
void VID_GetClientSize (int *width, int *height);

/* The integer factor between the 2D screen (vid.width x vid.height) and
 * physical pixels; the 2D screen is centered in the client area. */
int VID_GetUIScale (void);

#endif	/* HEXENLICHT_VID_VK_H */
