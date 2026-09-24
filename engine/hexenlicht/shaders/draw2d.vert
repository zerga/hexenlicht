/* draw2d.vert -- 2D batch (vk_draw.c): console, menus, status bar
 *
 * Positions are in the virtual 2D screen (vid.width x vid.height, y down);
 * scale and offset map it onto the window with the integer UI scale.
 *
 * Copyright (C) 2026  Hexenlicht contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#version 460

layout (location = 0) in vec2 in_pos;
layout (location = 1) in vec2 in_uv;
layout (location = 2) in vec4 in_color;	/* sRGB-encoded, unorm */
layout (location = 3) in uint in_tex;	/* texture slot | flags */

layout (location = 0) out vec2 out_uv;
layout (location = 1) out vec4 out_color;
layout (location = 2) flat out uint out_tex;

layout (push_constant) uniform push_constants
{
	vec2	scale;
	vec2	offset;
	float	gamma;
} push;

void main ()
{
	out_uv = in_uv;
	out_color = in_color;
	out_tex = in_tex;
	gl_Position = vec4 (in_pos * push.scale + push.offset, 0.0, 1.0);
}
