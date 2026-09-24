/* draw2d.frag -- 2D batch (vk_draw.c)
 *
 * Works in sRGB-encoded space, like the original OpenGL renderer did, so
 * that blending and modulation look the same: the (linear) texture sample
 * is re-encoded, multiplied by the vertex color and blended as is. Quads
 * flagged ALPHA_TEST behave like GL's alpha test (GL_GREATER 0.632) with
 * blending off; the others are alpha blended.
 *
 * Copyright (C) 2026  Hexenlicht contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#version 460
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_nonuniform_qualifier : require

#include "srgb.glsl"

#define TEX_SLOT_MASK		0xffffu
#define FLAG_ALPHA_TEST		0x10000u

layout (set = 0, binding = 0) uniform sampler2D textures[];

layout (location = 0) in vec2 in_uv;
layout (location = 1) in vec4 in_color;
layout (location = 2) flat in uint in_tex;

layout (location = 0) out vec4 out_color;

layout (push_constant) uniform push_constants
{
	vec2	scale;
	vec2	offset;
	float	gamma;	/* the "gamma" cvar: <= 1 brightens */
} push;

void main ()
{
	vec4 t = texture (textures[nonuniformEXT (in_tex & TEX_SLOT_MASK)], in_uv);
	vec4 c = vec4 (linear_to_srgb (t.rgb), t.a) * in_color;

	if ((in_tex & FLAG_ALPHA_TEST) != 0u)
	{
		if (c.a <= 0.632)
			discard;
		c.a = 1.0;	/* no blending for alpha tested quads */
	}

	/* hardware gamma ramp of the GL renderer; applied before blending,
	 * which is exact for everything but translucent quads */
	out_color = vec4 (pow (c.rgb, vec3 (push.gamma)), c.a);
}
