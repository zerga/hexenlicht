/* view_composite.frag -- copies the 3D view (TEX_TAA_OUTPUT's top left
 * global_ubo.taa_output_width x taa_output_height, the image Quake II RTX's final
 * blit shows) into its part of the swapchain (drawn with fullscreen.vert
 * and a viewport on the 3D view), encoding to sRGB and applying the gamma
 * cvar like draw2d.frag.
 *
 * Copyright (C) 2026  Hexenlicht contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * See the GNU General Public License for more details.
 */

#version 460
#extension GL_GOOGLE_include_directive : require

#define GLOBAL_UBO_DESC_SET_IDX 0
#define GLOBAL_TEXTURES_DESC_SET_IDX 1
#define GLOBAL_TEXTURES_SAMPLED_ONLY

#include "global_ubo.h"
#include "global_textures.h"
#include "srgb.glsl"

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

layout(push_constant) uniform Push
{
	float gamma;	/* the "gamma" cvar: <= 1 brightens */
} push;

void main()
{
	ivec2 size = ivec2(global_ubo.taa_output_width, global_ubo.taa_output_height);
	ivec2 p = min(ivec2(in_uv * vec2(size)), size - 1);
	vec3 c = linear_to_srgb(texelFetch(TEX_TAA_OUTPUT, p, 0).rgb);

	out_color = vec4(pow(c, vec3(push.gamma)), 1.0);
}
