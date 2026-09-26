/* view_composite.frag -- shows the 3D view in its part of the swapchain
 * (drawn with fullscreen.vert and a viewport on the 3D view), Quake II
 * RTX's final blit (final_blit.frag): the upscaler's output (vk_upscale.c),
 * TAA_OUTPUT or FSR's EASU or RCAS output, whose top left
 * push.input_size texels cover the view. It is shown as it is when it has
 * the view's size, else scaled: nearest at exactly half the view's size
 * and for the debug views, with Quake II RTX's Lanczos filter otherwise.
 * Then encoded to sRGB with the gamma cvar, like draw2d.frag. The lit
 * image without tone mapping is still in Quake II RTX's storage scale
 * (STORAGE_SCALE_HDR), which push.scale takes out.
 *
 * Copyright (C) 2019, NVIDIA CORPORATION. All rights reserved.
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

/* vk_view.c's composite_push_t */
layout(push_constant) uniform Push
{
	vec2	uv_to_texel;	/* in_uv (0..1 over the view) to the input's texel coordinates */
	ivec2	input_size;	/* the input's texels shown over the view */
	float	gamma;		/* the "gamma" cvar: <= 1 brightens */
	float	scale;		/* 1 / STORAGE_SCALE_HDR for the lit image without tone mapping, else 1 */
	int	filter_lanczos;	/* 0 nearest, 1 Lanczos (TAA_OUTPUT) */
	int	source;		/* 0 TAA_OUTPUT, 1 FSR_EASU_OUTPUT, 2 FSR_RCAS_OUTPUT */
} push;

// semi-vector form of the ternary operator: (f == val) ? eq : neq
vec2 v_sel(vec2 f, float val, float eq, vec2 neq)
{
    vec2 result;
    result.x = (f.x == val) ? eq : neq.x;
    result.y = (f.y == val) ? eq : neq.y;
    return result;
}

/* Quake II RTX's filter_lanczos (final_blit.frag); Hexenlicht: the taps
 * are clamped to the input's texels (the images are bigger than it and
 * hold older frames past it) */
vec3 filter_lanczos(sampler2D img, vec2 uv)
{
	ivec2 size = textureSize(img, 0);

    // Lanczos 3
    vec2 UV = uv.xy * size;
    vec2 tc = floor(UV - 0.5) + 0.5;
    vec2 f = UV - tc + 2;

    // compute at f, f-1, f-2, f-3, f-4, and f-5 using trig angle addition
    vec2 fpi = f * M_PI, fpi3 = f * (M_PI / 3.0);
    vec2 sinfpi = sin(fpi), sinfpi3 = sin(fpi3), cosfpi3 = cos(fpi3);
    const float r3 = sqrt(3.0);
    vec2 w0 = v_sel(f, 0, M_PI * M_PI * 1.0 / 3.0, (sinfpi *       sinfpi3) / (f       * f));
    vec2 w1 = v_sel(f, 1, M_PI * M_PI * 2.0 / 3.0, (-sinfpi * (sinfpi3 - r3 * cosfpi3)) / ((f - 1.0)*(f - 1.0)));
    vec2 w2 = v_sel(f, 2, M_PI * M_PI * 2.0 / 3.0, (sinfpi * (-sinfpi3 - r3 * cosfpi3)) / ((f - 2.0)*(f - 2.0)));
    vec2 w3 = v_sel(f, 3, M_PI * M_PI * 2.0 / 3.0, (-sinfpi * (-2.0*sinfpi3)) / ((f - 3.0)*(f - 3.0)));
    vec2 w4 = v_sel(f, 4, M_PI * M_PI * 2.0 / 3.0, (sinfpi * (-sinfpi3 + r3 * cosfpi3)) / ((f - 4.0)*(f - 4.0)));
    vec2 w5 = v_sel(f, 5, M_PI * M_PI * 2.0 / 3.0, (-sinfpi * (sinfpi3 + r3 * cosfpi3)) / ((f - 5.0)*(f - 5.0)));

    // use bilinear texture weights to merge center two samples in each dimension
    vec2 Weight[5];
    Weight[0] = w0;
    Weight[1] = w1;
    Weight[2] = w2 + w3;
    Weight[3] = w4;
    Weight[4] = w5;

    vec2 invTextureSize = 1.0 / vec2(size);
    vec2 lo = vec2(0.5), hi = vec2(push.input_size) - 0.5;	// Hexenlicht

    vec2 Sample[5];
    Sample[0] = invTextureSize * clamp(tc - 2, lo, hi);
    Sample[1] = invTextureSize * clamp(tc - 1, lo, hi);
    Sample[2] = invTextureSize * clamp(tc + w3 / Weight[2], lo, hi);
    Sample[3] = invTextureSize * clamp(tc + 2, lo, hi);
    Sample[4] = invTextureSize * clamp(tc + 3, lo, hi);

    vec4 o_rgba = vec4(0);

    // 5x5 footprint with corners dropped to give 13 texture taps
    o_rgba += vec4(textureLod(img, vec2(Sample[0].x, Sample[2].y), 0).rgb, 1.0) * Weight[0].x * Weight[2].y;
    o_rgba += vec4(textureLod(img, vec2(Sample[1].x, Sample[1].y), 0).rgb, 1.0) * Weight[1].x * Weight[1].y;
    o_rgba += vec4(textureLod(img, vec2(Sample[1].x, Sample[2].y), 0).rgb, 1.0) * Weight[1].x * Weight[2].y;
    o_rgba += vec4(textureLod(img, vec2(Sample[1].x, Sample[3].y), 0).rgb, 1.0) * Weight[1].x * Weight[3].y;
    o_rgba += vec4(textureLod(img, vec2(Sample[2].x, Sample[0].y), 0).rgb, 1.0) * Weight[2].x * Weight[0].y;
    o_rgba += vec4(textureLod(img, vec2(Sample[2].x, Sample[1].y), 0).rgb, 1.0) * Weight[2].x * Weight[1].y;
    o_rgba += vec4(textureLod(img, vec2(Sample[2].x, Sample[2].y), 0).rgb, 1.0) * Weight[2].x * Weight[2].y;
    o_rgba += vec4(textureLod(img, vec2(Sample[2].x, Sample[3].y), 0).rgb, 1.0) * Weight[2].x * Weight[3].y;
    o_rgba += vec4(textureLod(img, vec2(Sample[2].x, Sample[4].y), 0).rgb, 1.0) * Weight[2].x * Weight[4].y;
    o_rgba += vec4(textureLod(img, vec2(Sample[3].x, Sample[1].y), 0).rgb, 1.0) * Weight[3].x * Weight[1].y;
    o_rgba += vec4(textureLod(img, vec2(Sample[3].x, Sample[2].y), 0).rgb, 1.0) * Weight[3].x * Weight[2].y;
    o_rgba += vec4(textureLod(img, vec2(Sample[3].x, Sample[3].y), 0).rgb, 1.0) * Weight[3].x * Weight[3].y;
    o_rgba += vec4(textureLod(img, vec2(Sample[4].x, Sample[2].y), 0).rgb, 1.0) * Weight[4].x * Weight[2].y;

    return o_rgba.rgb / o_rgba.w;
}

void main()
{
	vec2 t = in_uv * push.uv_to_texel;	/* in the input's texels */
	vec3 c;

	if (push.filter_lanczos != 0)
	{
		c = filter_lanczos(TEX_TAA_OUTPUT, t / vec2(textureSize(TEX_TAA_OUTPUT, 0)));
	}
	else
	{
		ivec2 p = min(ivec2(t), push.input_size - 1);
		if (push.source == 1)
			c = texelFetch(TEX_FSR_EASU_OUTPUT, p, 0).rgb;
		else if (push.source == 2)
			c = texelFetch(TEX_FSR_RCAS_OUTPUT, p, 0).rgb;
		else
			c = texelFetch(TEX_TAA_OUTPUT, p, 0).rgb;
	}
	c = linear_to_srgb(c * push.scale);	/* clamped to [0, 1] */

	out_color = vec4(pow(c, vec3(push.gamma)), 1.0);
}
