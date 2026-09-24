/* srgb.glsl -- the sRGB transfer function
 *
 * The swapchain is B8G8R8A8_UNORM with an sRGB color space (vk_swapchain.c),
 * so the pass that writes it must store sRGB-encoded values.
 *
 * Copyright (C) 2026  Hexenlicht contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef SRGB_GLSL
#define SRGB_GLSL

vec3 linear_to_srgb (vec3 c)
{
	c = clamp (c, 0.0, 1.0);
	return mix (c * 12.92, 1.055 * pow (c, vec3 (1.0 / 2.4)) - 0.055, step (0.0031308, c));
}

#endif	/* SRGB_GLSL */
