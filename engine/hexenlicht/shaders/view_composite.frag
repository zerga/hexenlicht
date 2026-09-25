/* view_composite.frag -- copies the 3D view image into its part of the
 * swapchain (drawn with fullscreen.vert and a viewport on the 3D view),
 * encoding to sRGB and applying the gamma cvar like draw2d.frag.
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

#include "srgb.glsl"

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0, rgba16f) uniform readonly image2D view_image;

layout(push_constant) uniform Push
{
	float gamma;	/* the "gamma" cvar: <= 1 brightens */
} push;

void main()
{
	ivec2 size = imageSize(view_image);
	ivec2 p = min(ivec2(in_uv * vec2(size)), size - 1);
	vec3 c = linear_to_srgb(imageLoad(view_image, p).rgb);

	out_color = vec4(pow(c, vec3(push.gamma)), 1.0);
}
