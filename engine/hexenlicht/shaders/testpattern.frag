/* testpattern.frag -- placeholder screen until the 2D renderer (story 1.6)
 *
 * Dark purple at the top to a warm glow at the bottom, with a slow shimmer
 * so that it is visible when frames keep coming.
 *
 * Copyright (C) 2026  Hexenlicht contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#version 460
#extension GL_GOOGLE_include_directive : require

#include "srgb.glsl"

layout (location = 0) in vec2 in_uv;
layout (location = 0) out vec4 out_color;

layout (push_constant) uniform push_constants
{
	float	time;	/* seconds */
} push;

void main ()
{
	const vec3 top    = vec3 (0.010, 0.004, 0.022);	/* linear */
	const vec3 bottom = vec3 (0.550, 0.200, 0.050);

	float t = in_uv.y + 0.05 * sin (in_uv.x * 6.2831853 + push.time);
	vec3 color = mix (top, bottom, smoothstep (0.0, 1.0, t));

	out_color = vec4 (linear_to_srgb (color), 1.0);
}
