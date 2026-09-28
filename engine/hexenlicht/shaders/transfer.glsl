/* transfer.glsl -- 8-bit colors and linear light (story 4.17)
 *
 * GL multiplied a texture's 8-bit color by the lightmap's and showed the
 * product as it was; in linear light that is the product only under a pure
 * power, so Hexenlicht's 8-bit colors are a power COLOR_GAMMA (2.2) of
 * linear light: the textures (UNORM, vk_texture.c: the shaders get the
 * colors as they are and convert them where they are colors), the sky's
 * layers and the image the composite writes (the swapchain is
 * B8G8R8A8_UNORM with an sRGB color space, vk_swapchain.c). With r_srgb 1
 * (the UBO's color_srgb) they are the sRGB curve's, the modern standard,
 * whose linear toe made GL's dark tones up to 2-3 times darker (4.11a's
 * survey). vk_texture.c's VK_ColorToLinear is the same on the CPU.
 *
 * Copyright (C) 2026  Hexenlicht contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef TRANSFER_GLSL
#define TRANSFER_GLSL

#define COLOR_GAMMA	2.2

/* an 8-bit color (0-1) as linear light; srgb: r_srgb */
vec3 color_to_linear (vec3 c, uint srgb)
{
	c = max (c, vec3 (0.0));
	if (srgb != 0u)
		return mix (c / 12.92, pow ((c + 0.055) / 1.055, vec3 (2.4)), step (0.04045, c));
	return pow (c, vec3 (COLOR_GAMMA));
}

/* linear light as an 8-bit color, clamped to [0, 1] */
vec3 linear_to_color (vec3 x, uint srgb)
{
	x = clamp (x, 0.0, 1.0);
	if (srgb != 0u)
		return mix (x * 12.92, 1.055 * pow (x, vec3 (1.0 / 2.4)) - 0.055, step (0.0031308, x));
	return pow (x, vec3 (1.0 / COLOR_GAMMA));
}

#endif	/* TRANSFER_GLSL */
