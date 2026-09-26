/*
Copyright (C) 2018 Christoph Schied
Copyright (C) 2019, NVIDIA CORPORATION. All rights reserved.
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

/* Hexenlicht: Quake II RTX's render-target images (vk_images.c creates
 * them at the swapchain's size, the 3D view renders into their top left
 * global_ubo.width x global_ubo.height) and the bindless texture array,
 * with these changes:
 *  - the images get their own descriptor set (GLOBAL_TEXTURES_DESC_SET_IDX,
 *    storage images at BINDING_OFFSET_IMAGES + binding, the same images
 *    sampled at BINDING_OFFSET_TEXTURES + binding); the bindless array is
 *    vk_texture.c's set (GLOBAL_TEXTURES_TEX_ARR_DESC_SET_IDX);
 *  - the lists hold only the images of the passes imported so far: each
 *    pass brings its rows, with Quake II RTX's names and formats, numbered
 *    from 0 (environment and sky textures come with their passes too);
 *  - the blue noise (vk_images.c) is the set's last binding: 64 x 64
 *    texels, 256 layers (constants.h);
 *  - PT_SPECULAR_HIT_DIST is Hexenlicht's: the specular bounce's hit
 *    distance, for DLSS Ray Reconstruction (indirect_lighting.rgen);
 *  - PT_VIEW_DEPTH is declared r16f, its format (Quake II RTX declares
 *    r32f, which the validation layer reports as undefined behaviour);
 *  - one GPU, so the _MGPU sizes are the full ones;
 *  - a shader that defines GLOBAL_TEXTURES_SAMPLED_ONLY gets only the
 *    sampled TEX_* images (the composite: a fragment shader);
 *  - global_textureQueryLevels. */

#ifndef  _TEXTURES_H_
#define  _TEXTURES_H_

#include "constants.h"

/* the images' size (vk_images.c): the swapchain's, the width rounded up to even */
#define IMG_WIDTH  (vk_image_extent.width)
#define IMG_HEIGHT (vk_image_extent.height)
#define IMG_WIDTH_MGPU IMG_WIDTH
#define IMG_WIDTH_UNSCALED  IMG_WIDTH
#define IMG_HEIGHT_UNSCALED IMG_HEIGHT

#define IMG_WIDTH_GRAD  ((IMG_WIDTH + GRAD_DWN - 1) / GRAD_DWN)
#define IMG_HEIGHT_GRAD ((IMG_HEIGHT + GRAD_DWN - 1) / GRAD_DWN)
#define IMG_WIDTH_GRAD_MGPU  IMG_WIDTH_GRAD

#define IMG_WIDTH_TAA  IMG_WIDTH
#define IMG_HEIGHT_TAA  IMG_HEIGHT

/* These are images that are to be used as render targets and buffers, but not textures. */
#define LIST_IMAGES \
	IMG_DO(TAA_OUTPUT,                 0, R16G16B16A16_SFLOAT, rgba16f, IMG_WIDTH_TAA,       IMG_HEIGHT_TAA ) \
	IMG_DO(PT_MOTION,                  1, R16G16B16A16_SFLOAT, rgba16f, IMG_WIDTH,           IMG_HEIGHT     ) \
	IMG_DO(PT_TRANSPARENT,             2, R16G16B16A16_SFLOAT, rgba16f, IMG_WIDTH_MGPU,      IMG_HEIGHT     ) \
	IMG_DO(PT_SHADING_POSITION,        3, R32G32B32A32_SFLOAT, rgba32f, IMG_WIDTH_MGPU,      IMG_HEIGHT     ) \
	IMG_DO(PT_VIEW_DIRECTION,          4, R16G16B16A16_SFLOAT, rgba16f, IMG_WIDTH_MGPU,      IMG_HEIGHT     ) \
	IMG_DO(PT_THROUGHPUT,              5, R16G16B16A16_SFLOAT, rgba16f, IMG_WIDTH_MGPU,      IMG_HEIGHT     ) \
	IMG_DO(PT_BOUNCE_THROUGHPUT,       6, R16G16B16A16_SFLOAT, rgba16f, IMG_WIDTH_MGPU,      IMG_HEIGHT     ) \
	IMG_DO(PT_COLOR_LF_SH,             7, R16G16B16A16_SFLOAT, rgba16f, IMG_WIDTH_MGPU,      IMG_HEIGHT     ) \
	IMG_DO(PT_COLOR_LF_COCG,           8, R16G16_SFLOAT,       rg16f,   IMG_WIDTH_MGPU,      IMG_HEIGHT     ) \
	IMG_DO(PT_COLOR_HF,                9, R32_UINT,            r32ui,   IMG_WIDTH_MGPU,      IMG_HEIGHT     ) \
	IMG_DO(PT_COLOR_SPEC,             10, R32_UINT,            r32ui,   IMG_WIDTH_MGPU,      IMG_HEIGHT     ) \
	IMG_DO(ASVGF_COLOR,               11, R16G16B16A16_SFLOAT, rgba16f, IMG_WIDTH,           IMG_HEIGHT     ) \
	IMG_DO(FLAT_COLOR,                12, R16G16B16A16_SFLOAT, rgba16f, IMG_WIDTH,           IMG_HEIGHT     ) \
	IMG_DO(FLAT_MOTION,               13, R16G16B16A16_SFLOAT, rgba16f, IMG_WIDTH,           IMG_HEIGHT     ) \
	IMG_DO(PT_VIEW_DIRECTION2,        14, R16G16B16A16_SFLOAT, rgba16f, IMG_WIDTH_MGPU,      IMG_HEIGHT     ) \
	IMG_DO(PT_GEO_NORMAL2,            15, R32_UINT,            r32ui,   IMG_WIDTH_MGPU,      IMG_HEIGHT     ) \
	IMG_DO(PT_SPECULAR_HIT_DIST,      16, R16_SFLOAT,          r16f,    IMG_WIDTH_MGPU,      IMG_HEIGHT     ) \

#define NUM_IMAGES_BASE     17

/* images that exist twice: the _A names are this frame's, the _B names the
 * last frame's (vk_images.c's even and odd descriptor sets swap them) */
#define LIST_IMAGES_A_B \
	IMG_DO(PT_VISBUF_PRIM_A,          NUM_IMAGES_BASE + 0,  R32G32_UINT,         rg32ui,  IMG_WIDTH_MGPU,      IMG_HEIGHT     ) \
	IMG_DO(PT_VISBUF_PRIM_B,          NUM_IMAGES_BASE + 1,  R32G32_UINT,         rg32ui,  IMG_WIDTH_MGPU,      IMG_HEIGHT     ) \
	IMG_DO(PT_VISBUF_BARY_A,          NUM_IMAGES_BASE + 2,  R16G16_SFLOAT,       rg16f,   IMG_WIDTH_MGPU,      IMG_HEIGHT     ) \
	IMG_DO(PT_VISBUF_BARY_B,          NUM_IMAGES_BASE + 3,  R16G16_SFLOAT,       rg16f,   IMG_WIDTH_MGPU,      IMG_HEIGHT     ) \
	IMG_DO(PT_CLUSTER_A,              NUM_IMAGES_BASE + 4,  R16_UINT,            r16ui,   IMG_WIDTH_MGPU,      IMG_HEIGHT     ) \
	IMG_DO(PT_CLUSTER_B,              NUM_IMAGES_BASE + 5,  R16_UINT,            r16ui,   IMG_WIDTH_MGPU,      IMG_HEIGHT     ) \
	IMG_DO(PT_BASE_COLOR_A,           NUM_IMAGES_BASE + 6,  R16G16B16A16_SFLOAT, rgba16f, IMG_WIDTH_MGPU,      IMG_HEIGHT     ) \
	IMG_DO(PT_BASE_COLOR_B,           NUM_IMAGES_BASE + 7,  R16G16B16A16_SFLOAT, rgba16f, IMG_WIDTH_MGPU,      IMG_HEIGHT     ) \
	IMG_DO(PT_METALLIC_A,             NUM_IMAGES_BASE + 8,  R8G8_UNORM,          rg8,     IMG_WIDTH_MGPU,      IMG_HEIGHT     ) \
	IMG_DO(PT_METALLIC_B,             NUM_IMAGES_BASE + 9,  R8G8_UNORM,          rg8,     IMG_WIDTH_MGPU,      IMG_HEIGHT     ) \
	IMG_DO(PT_VIEW_DEPTH_A,           NUM_IMAGES_BASE + 10, R16_SFLOAT,          r16f,    IMG_WIDTH,           IMG_HEIGHT     ) \
	IMG_DO(PT_VIEW_DEPTH_B,           NUM_IMAGES_BASE + 11, R16_SFLOAT,          r16f,    IMG_WIDTH,           IMG_HEIGHT     ) \
	IMG_DO(PT_NORMAL_A,               NUM_IMAGES_BASE + 12, R32_UINT,            r32ui,   IMG_WIDTH_MGPU,      IMG_HEIGHT     ) \
	IMG_DO(PT_NORMAL_B,               NUM_IMAGES_BASE + 13, R32_UINT,            r32ui,   IMG_WIDTH_MGPU,      IMG_HEIGHT     ) \
	IMG_DO(PT_GEO_NORMAL_A,           NUM_IMAGES_BASE + 14, R32_UINT,            r32ui,   IMG_WIDTH_MGPU,      IMG_HEIGHT     ) \
	IMG_DO(PT_GEO_NORMAL_B,           NUM_IMAGES_BASE + 15, R32_UINT,            r32ui,   IMG_WIDTH_MGPU,      IMG_HEIGHT     ) \
	IMG_DO(ASVGF_RNG_SEED_A,          NUM_IMAGES_BASE + 16, R32_UINT,            r32ui,   IMG_WIDTH,           IMG_HEIGHT     ) \
	IMG_DO(ASVGF_RNG_SEED_B,          NUM_IMAGES_BASE + 17, R32_UINT,            r32ui,   IMG_WIDTH,           IMG_HEIGHT     ) \

#define LIST_IMAGES_B_A \
	IMG_DO(PT_VISBUF_PRIM_B,          NUM_IMAGES_BASE + 0,  R32G32_UINT,         rg32ui,  IMG_WIDTH_MGPU,      IMG_HEIGHT     ) \
	IMG_DO(PT_VISBUF_PRIM_A,          NUM_IMAGES_BASE + 1,  R32G32_UINT,         rg32ui,  IMG_WIDTH_MGPU,      IMG_HEIGHT     ) \
	IMG_DO(PT_VISBUF_BARY_B,          NUM_IMAGES_BASE + 2,  R16G16_SFLOAT,       rg16f,   IMG_WIDTH_MGPU,      IMG_HEIGHT     ) \
	IMG_DO(PT_VISBUF_BARY_A,          NUM_IMAGES_BASE + 3,  R16G16_SFLOAT,       rg16f,   IMG_WIDTH_MGPU,      IMG_HEIGHT     ) \
	IMG_DO(PT_CLUSTER_B,              NUM_IMAGES_BASE + 4,  R16_UINT,            r16ui,   IMG_WIDTH_MGPU,      IMG_HEIGHT     ) \
	IMG_DO(PT_CLUSTER_A,              NUM_IMAGES_BASE + 5,  R16_UINT,            r16ui,   IMG_WIDTH_MGPU,      IMG_HEIGHT     ) \
	IMG_DO(PT_BASE_COLOR_B,           NUM_IMAGES_BASE + 6,  R16G16B16A16_SFLOAT, rgba16f, IMG_WIDTH_MGPU,      IMG_HEIGHT     ) \
	IMG_DO(PT_BASE_COLOR_A,           NUM_IMAGES_BASE + 7,  R16G16B16A16_SFLOAT, rgba16f, IMG_WIDTH_MGPU,      IMG_HEIGHT     ) \
	IMG_DO(PT_METALLIC_B,             NUM_IMAGES_BASE + 8,  R8G8_UNORM,          rg8,     IMG_WIDTH_MGPU,      IMG_HEIGHT     ) \
	IMG_DO(PT_METALLIC_A,             NUM_IMAGES_BASE + 9,  R8G8_UNORM,          rg8,     IMG_WIDTH_MGPU,      IMG_HEIGHT     ) \
	IMG_DO(PT_VIEW_DEPTH_B,           NUM_IMAGES_BASE + 10, R16_SFLOAT,          r16f,    IMG_WIDTH,           IMG_HEIGHT     ) \
	IMG_DO(PT_VIEW_DEPTH_A,           NUM_IMAGES_BASE + 11, R16_SFLOAT,          r16f,    IMG_WIDTH,           IMG_HEIGHT     ) \
	IMG_DO(PT_NORMAL_B,               NUM_IMAGES_BASE + 12, R32_UINT,            r32ui,   IMG_WIDTH_MGPU,      IMG_HEIGHT     ) \
	IMG_DO(PT_NORMAL_A,               NUM_IMAGES_BASE + 13, R32_UINT,            r32ui,   IMG_WIDTH_MGPU,      IMG_HEIGHT     ) \
	IMG_DO(PT_GEO_NORMAL_B,           NUM_IMAGES_BASE + 14, R32_UINT,            r32ui,   IMG_WIDTH_MGPU,      IMG_HEIGHT     ) \
	IMG_DO(PT_GEO_NORMAL_A,           NUM_IMAGES_BASE + 15, R32_UINT,            r32ui,   IMG_WIDTH_MGPU,      IMG_HEIGHT     ) \
	IMG_DO(ASVGF_RNG_SEED_B,          NUM_IMAGES_BASE + 16, R32_UINT,            r32ui,   IMG_WIDTH,           IMG_HEIGHT     ) \
	IMG_DO(ASVGF_RNG_SEED_A,          NUM_IMAGES_BASE + 17, R32_UINT,            r32ui,   IMG_WIDTH,           IMG_HEIGHT     ) \

#define NUM_IMAGES (NUM_IMAGES_BASE + 18) /* this really sucks but I don't know how to fix it
                                             counting with enum does not work in GLSL */

// todo: make naming consistent!
#define GLOBAL_TEXTURES_TEX_ARR_BINDING_IDX  0
#define BINDING_OFFSET_IMAGES     0
#define BINDING_OFFSET_TEXTURES   (BINDING_OFFSET_IMAGES + NUM_IMAGES)
#define BINDING_OFFSET_BLUE_NOISE (BINDING_OFFSET_TEXTURES + NUM_IMAGES)
#define NUM_IMAGE_BINDINGS        (BINDING_OFFSET_BLUE_NOISE + 1)

/* the set of vk_texture.c's bindless array (vk_pathtracer.c's layout) */
#define GLOBAL_TEXTURES_TEX_ARR_DESC_SET_IDX 2


#ifndef VKPT_SHADER
/***************************************************************************/
/* HOST CODE                                                               */
/***************************************************************************/

#if defined(VK_MAX_TEXTURES) && VK_MAX_TEXTURES != NUM_GLOBAL_TEXTURES
#error need to fix the constant here as well
#endif


enum QVK_IMAGES {
#define IMG_DO(_name, ...) \
	VKPT_IMG_##_name,
	LIST_IMAGES
	LIST_IMAGES_A_B
#undef IMG_DO
	NUM_VKPT_IMAGES
};

typedef char compile_time_check_num_images[(NUM_IMAGES == NUM_VKPT_IMAGES)*2-1];

#elif defined(GLOBAL_TEXTURES_DESC_SET_IDX)
/***************************************************************************/
/* SHADER CODE                                                             */
/***************************************************************************/

#extension GL_EXT_nonuniform_qualifier : require

/* general texture array for world, etc */
layout(
	set = GLOBAL_TEXTURES_TEX_ARR_DESC_SET_IDX,
	binding = GLOBAL_TEXTURES_TEX_ARR_BINDING_IDX
) uniform sampler2D global_texture_descriptors[];

#define SAMPLER_r16ui   usampler2D
#define SAMPLER_r32ui   usampler2D
#define SAMPLER_rg32ui  usampler2D
#define SAMPLER_r32i    isampler2D
#define SAMPLER_r32f    sampler2D
#define SAMPLER_r16f    sampler2D	// Hexenlicht: PT_VIEW_DEPTH
#define SAMPLER_rg32f   sampler2D
#define SAMPLER_rg16f   sampler2D
#define SAMPLER_rgba32f sampler2D
#define SAMPLER_rgba16f sampler2D
#define SAMPLER_rgba8   sampler2D
#define SAMPLER_r8      sampler2D
#define SAMPLER_rg8     sampler2D

#define IMAGE_r16ui   uimage2D
#define IMAGE_r32ui   uimage2D
#define IMAGE_rg32ui  uimage2D
#define IMAGE_r32i    iimage2D
#define IMAGE_r32f    image2D
#define IMAGE_r16f    image2D	// Hexenlicht: PT_VIEW_DEPTH
#define IMAGE_rg32f   image2D
#define IMAGE_rg16f   image2D
#define IMAGE_rgba32f image2D
#define IMAGE_rgba16f image2D
#define IMAGE_rgba8   image2D
#define IMAGE_r8      image2D
#define IMAGE_rg8     image2D

/* framebuffer images; Hexenlicht: not in shaders that define
 * GLOBAL_TEXTURES_SAMPLED_ONLY (fragment shaders: writable storage images
 * there would need fragmentStoresAndAtomics) */
#ifndef GLOBAL_TEXTURES_SAMPLED_ONLY
#define IMG_DO(_name, _binding, _vkformat, _glslformat, _w, _h) \
	layout(set = GLOBAL_TEXTURES_DESC_SET_IDX, binding = BINDING_OFFSET_IMAGES + _binding, _glslformat) \
	uniform IMAGE_##_glslformat IMG_##_name;
LIST_IMAGES
LIST_IMAGES_A_B
#undef IMG_DO
#endif

/* framebuffer textures */
#define IMG_DO(_name, _binding, _vkformat, _glslformat, _w, _h) \
	layout(set = GLOBAL_TEXTURES_DESC_SET_IDX, binding = BINDING_OFFSET_TEXTURES + _binding) \
	uniform SAMPLER_##_glslformat TEX_##_name;
LIST_IMAGES
LIST_IMAGES_A_B
#undef IMG_DO

layout(
	set = GLOBAL_TEXTURES_DESC_SET_IDX,
	binding = BINDING_OFFSET_BLUE_NOISE
) uniform sampler2DArray TEX_BLUE_NOISE;

vec4
global_texture(uint idx, vec2 tex_coord)
{
	if(idx >= NUM_GLOBAL_TEXTURES)
		return vec4(1, 0, 1, 0);
	return texture(global_texture_descriptors[nonuniformEXT(idx)], tex_coord);
}

vec4
global_textureLod(uint idx, vec2 tex_coord, float lod)
{
	if(idx >= NUM_GLOBAL_TEXTURES)
		return vec4(1, 1, 0, 0);
	return textureLod(global_texture_descriptors[nonuniformEXT(idx)], tex_coord, lod);
}

vec4
global_textureGrad(uint idx, vec2 tex_coord, vec2 d_x, vec2 d_y)
{
	if(idx >= NUM_GLOBAL_TEXTURES)
		return vec4(1, 1, 0, 0);
	return textureGrad(global_texture_descriptors[nonuniformEXT(idx)], tex_coord, d_x, d_y);
}

ivec2
global_textureSize(uint idx, int level)
{
	if(idx >= NUM_GLOBAL_TEXTURES)
		return ivec2(0);
	return textureSize(global_texture_descriptors[nonuniformEXT(idx)], level);
}

/* Hexenlicht: the number of mip levels of a texture */
int
global_textureQueryLevels(uint idx)
{
	if(idx >= NUM_GLOBAL_TEXTURES)
		return 0;
	return textureQueryLevels(global_texture_descriptors[nonuniformEXT(idx)]);
}


#endif


#endif /*_TEXTURES_H_*/
// vim: shiftwidth=4 noexpandtab tabstop=4 cindent
