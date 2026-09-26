/*
Copyright (C) 2019, NVIDIA CORPORATION. All rights reserved.
Copyright (C) 2021, Frank Richter. All rights reserved.
Copyright (C) 2026  Hexenlicht contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

/* Hexenlicht: the input texels are clamped one by one to the part of
 * TAA_OUTPUT rendered this frame (FsrEasuR; Quake II RTX clamps the
 * gather point to the image, whose size is the largest output; ours have
 * the swapchain's size and hold older frames past it), and nothing is
 * written past the upscaled view (the dispatch is rounded up to 16x16
 * pixels). Only the SDR pipeline (spec_hdr 0) is created: vk_upscale.c. */

layout(constant_id = 0) const uint spec_hdr = 0;
// Whether output goes to display (1) or  will be processed by RCAS FSR pass (0)
layout(constant_id = 1) const uint spec_output_display = 0;

#include "utils.glsl"

#define GLOBAL_UBO_DESC_SET_IDX 0
#include "global_ubo.h"

#define GLOBAL_TEXTURES_DESC_SET_IDX 1
#include "global_textures.h"

/* FSR upsampling (EASU, "Edge Adaptive Spatial Upsampling") pass
 * Also see overview in fsr.c */

#define A_GPU 1
#define A_GLSL 1

// Those headers contain the bulk of the implementation
#include "ffx_a.h"
#include "ffx_fsr1.h"

#include "fsr_utils.glsl"

layout(local_size_x=64) in;

/* Provide input for EASU, from TEX_TAA_OUTPUT
 * Hack: fetch all channels in FsrEasuR() and only return stored
 * value in the other functions. This is done to allow "HDR input"
 * processing on the input. */
fsr_vec4 input_r, input_g, input_b;

fsr_vec4 FsrEasuR(AF2 p)
{
	/* Hexenlicht: the gather's four texels (x: i0,j1; y: i1,j1; z: i1,j0;
	 * w: i0,j0) fetched one by one, each clamped to the part of TAA_OUTPUT
	 * rendered this frame. Quake II RTX clamps the gather point to the
	 * image instead, which moves whole taps by a texel at its edges; our
	 * images also hold older frames past the rendered part. */
	ivec2 i0 = ivec2(floor(p * vec2(global_ubo.taa_image_width, global_ubo.taa_image_height) - 0.5));
	ivec2 hi = ivec2(global_ubo.taa_output_width, global_ubo.taa_output_height) - 1;
	ivec2 a = clamp(i0, ivec2(0), hi);
	ivec2 b = clamp(i0 + 1, ivec2(0), hi);
	vec4 t00 = texelFetch(TEX_TAA_OUTPUT, ivec2(a.x, a.y), 0);
	vec4 t10 = texelFetch(TEX_TAA_OUTPUT, ivec2(b.x, a.y), 0);
	vec4 t01 = texelFetch(TEX_TAA_OUTPUT, ivec2(a.x, b.y), 0);
	vec4 t11 = texelFetch(TEX_TAA_OUTPUT, ivec2(b.x, b.y), 0);

	input_r = fsr_vec4(t01.r, t11.r, t10.r, t00.r);
	input_g = fsr_vec4(t01.g, t11.g, t10.g, t00.g);
	input_b = fsr_vec4(t01.b, t11.b, t10.b, t00.b);

	if(spec_hdr != 0) {
		#pragma unroll
		for (uint i = 0; i < 4; i++) {
			fsr_vec3 color = fsr_vec3(input_r[i], input_g[i], input_b[i]);
			color = hdr_input(color);
			input_r[i] = color.r;
			input_g[i] = color.g;
			input_b[i] = color.b;
		}
	}

	return input_r;
}
fsr_vec4 FsrEasuG(AF2 p)
{
	return input_g;
}
fsr_vec4 FsrEasuB(AF2 p)
{
	return input_b;
}

void output_color(fsr_vec3 color, AU2 gxy)
{
	// Hexenlicht: only the upscaled view
	if(any(greaterThanEqual(gxy, AU2(global_ubo.unscaled_width, global_ubo.unscaled_height))))
		return;
	if(spec_output_display != 0)
		imageStore(IMG_FSR_EASU_OUTPUT, ivec2(gxy), vec4(hdr_output(color), 1));
	else
		imageStore(IMG_FSR_EASU_OUTPUT, ivec2(gxy), vec4(color, 1));
}

void main()
{
	/* Code from EASU application has been lifted from:
	   https://raw.githubusercontent.com/GPUOpen-Effects/FidelityFX-FSR/master/docs/FidelityFX-FSR-Overview-Integration.pdf */

	// Do remapping of local xy in workgroup for a more PS-like swizzle pattern.
	AU2 gxy = ARmp8x8(gl_LocalInvocationID.x) + AU2(gl_WorkGroupID.x << 4u, gl_WorkGroupID.y << 4u);

	// Run the function four times, as recommended by the official docs
	fsr_vec3 color;
	FsrEasu(color, gxy, global_ubo.easu_const0, global_ubo.easu_const1, global_ubo.easu_const2, global_ubo.easu_const3);
	output_color(color, gxy);
	gxy.x += 8;

	FsrEasu(color, gxy, global_ubo.easu_const0, global_ubo.easu_const1, global_ubo.easu_const2, global_ubo.easu_const3);
	output_color(color, gxy);
	gxy.y += 8;

	FsrEasu(color, gxy, global_ubo.easu_const0, global_ubo.easu_const1, global_ubo.easu_const2, global_ubo.easu_const3);
	output_color(color, gxy);
	gxy.x -= 8;

	FsrEasu(color, gxy, global_ubo.easu_const0, global_ubo.easu_const1, global_ubo.easu_const2, global_ubo.easu_const3);
	output_color(color, gxy);
}
