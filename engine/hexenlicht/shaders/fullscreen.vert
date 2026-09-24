/* fullscreen.vert -- one triangle covering the whole viewport
 *
 * Draw with 3 vertices and no vertex buffer. out_uv is 0..1 across the
 * screen, (0,0) at the top left.
 *
 * Copyright (C) 2026  Hexenlicht contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#version 460

layout (location = 0) out vec2 out_uv;

void main ()
{
	vec2 uv = vec2 ((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
	out_uv = uv;
	gl_Position = vec4 (uv * 2.0 - 1.0, 0.0, 1.0);
}
