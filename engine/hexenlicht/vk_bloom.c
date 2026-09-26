/* vk_bloom.c -- bloom: Quake II RTX's
 *
 * Quake II RTX's bloom.c: vk_view.c runs it on the lit image in TAA_OUTPUT
 * (r_debugview 0, bloom_enable 1) before the tone mapping (vk_tonemap.c):
 * bloom_downscale.comp averages it into BLOOM_VBLUR at a quarter of the
 * size, bloom_blur.comp blurs that horizontally into BLOOM_HBLUR and back
 * vertically (a Gaussian of bloom_sigma times the view's height, at most
 * 100 quarter-size pixels), bloom_composite.comp blends it into TAA_OUTPUT
 * by bloom_intensity (the UBO's). bloom_debug 1-3 shows the stages
 * (downscaled, blurred horizontally, both) instead.
 *
 * Left out: Quake II RTX's stronger, wider bloom under water (Hexen II's
 * underwater look is GL's warp and tint, 6.6) and its blur behind menus
 * (GL draws menus over the plain view).
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

#include "quakedef.h"
#include "vk_local.h"
#include "shaders/hl_shared.h"
#include "shaders/global_textures.h"

typedef struct
{
	float	pixstep_x, pixstep_y;
	float	argument_scale;		/* -1 / (2 sigma^2) */
	float	normalization_scale;	/* 1 / (sqrt(2 pi) sigma) */
	int	num_samples;
	int	pass;			/* 0 horizontal, 1 vertical */
} blur_push_t;				/* bloom_blur.comp's push_constant_block */

/* Quake II RTX's defaults */
static cvar_t	bloom_enable = {"bloom_enable", "1", CVAR_NONE};
static cvar_t	bloom_debug = {"bloom_debug", "0", CVAR_NONE};
static cvar_t	bloom_sigma = {"bloom_sigma", "0.037", CVAR_NONE};	/* of the view's height */
static cvar_t	bloom_intensity = {"bloom_intensity", "0.002", CVAR_NONE};

static VkPipelineLayout	bloom_layout;
static VkPipeline	downscale_pipeline;
static VkPipeline	blur_pipeline;
static VkPipeline	composite_pipeline;

static void CreatePipelines (void)
{
	if (!downscale_pipeline)
		downscale_pipeline = VK_CreateComputePipeline ("bloom_downscale.comp", bloom_layout);
	if (!blur_pipeline)
		blur_pipeline = VK_CreateComputePipeline ("bloom_blur.comp", bloom_layout);
	if (!composite_pipeline)
		composite_pipeline = VK_CreateComputePipeline ("bloom_composite.comp", bloom_layout);
}

void VK_DestroyBloomPipelines (void)
{
	if (downscale_pipeline)
		vkDestroyPipeline (vk.device, downscale_pipeline, NULL);
	if (blur_pipeline)
		vkDestroyPipeline (vk.device, blur_pipeline, NULL);
	if (composite_pipeline)
		vkDestroyPipeline (vk.device, composite_pipeline, NULL);
	downscale_pipeline = blur_pipeline = composite_pipeline = VK_NULL_HANDLE;
}

qboolean VK_BloomEnabled (void)
{
	return bloom_enable.integer != 0;
}

float VK_BloomIntensity (void)
{
	return bloom_intensity.value;
}

/* bloom_debug: a stage's quarter-size image stretched over the view (Quake
 * II RTX's bloom_debug_show_image) */
static void ShowStage (VkCommandBuffer cmd, int image, uint32_t width, uint32_t height)
{
	VkImage		out = VK_Image (VKPT_IMG_TAA_OUTPUT);
	VkImageBlit	blit;

	memset (&blit, 0, sizeof(blit));
	blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	blit.srcSubresource.layerCount = 1;
	blit.srcOffsets[1].x = (int32_t)(width / 4);
	blit.srcOffsets[1].y = (int32_t)(height / 4);
	blit.srcOffsets[1].z = 1;
	blit.dstSubresource = blit.srcSubresource;
	blit.dstOffsets[1].x = (int32_t)width;
	blit.dstOffsets[1].y = (int32_t)height;
	blit.dstOffsets[1].z = 1;
	VK_RenderTargetBarrier (cmd, out, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
				VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
	VK_RenderTargetBarrier (cmd, VK_Image (image), VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
				VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
	vkCmdBlitImage (cmd, VK_Image (image), VK_IMAGE_LAYOUT_GENERAL, out, VK_IMAGE_LAYOUT_GENERAL, 1, &blit, VK_FILTER_LINEAR);
	VK_RenderTargetBarrier (cmd, out, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
				VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
}

/* Quake II RTX's vkpt_bloom_record_cmd_buffer, on TAA_OUTPUT's width x
 * height view */
void VK_Bloom (VkCommandBuffer cmd, uint32_t width, uint32_t height)
{
	blur_push_t	hblur, vblur;
	float		sigma;

	CreatePipelines ();

	/* its compute_push_constants: sigma in quarter-size pixels */
	sigma = bloom_sigma.value * (float)height * 0.25f;
	sigma = q_min (sigma, 100.0f);
	sigma = q_max (sigma, 1.0f);
	memset (&hblur, 0, sizeof(hblur));
	hblur.pixstep_x = 1.0f;
	hblur.argument_scale = -1.0f / (2.0f * sigma * sigma);
	hblur.normalization_scale = 1.0f / (sqrtf (2.0f * (float)M_PI) * sigma);
	hblur.num_samples = (int)roundf (sigma * 4.0f);
	vblur = hblur;
	vblur.pixstep_x = 0.0f;
	vblur.pixstep_y = 1.0f;
	vblur.pass = 1;

	VK_DispatchComputeLayout (cmd, downscale_pipeline, bloom_layout, NULL, 0, width / 4, height / 4, 16);
	VK_ComputeBarrier (cmd);
	if (bloom_debug.integer == 1)
	{
		ShowStage (cmd, VKPT_IMG_BLOOM_VBLUR, width, height);
		return;
	}
	VK_DispatchComputeLayout (cmd, blur_pipeline, bloom_layout, &hblur, sizeof(hblur), width / 4, height / 4, 16);
	VK_ComputeBarrier (cmd);
	if (bloom_debug.integer == 2)
	{
		ShowStage (cmd, VKPT_IMG_BLOOM_HBLUR, width, height);
		return;
	}
	VK_DispatchComputeLayout (cmd, blur_pipeline, bloom_layout, &vblur, sizeof(vblur), width / 4, height / 4, 16);
	VK_ComputeBarrier (cmd);
	if (bloom_debug.integer == 3)
	{
		ShowStage (cmd, VKPT_IMG_BLOOM_VBLUR, width, height);
		return;
	}
	VK_DispatchComputeLayout (cmd, composite_pipeline, bloom_layout, NULL, 0, width, height, 16);
	VK_ComputeBarrier (cmd);
}

void VK_InitBloom (void)
{
	Cvar_RegisterVariable (&bloom_enable);
	Cvar_RegisterVariable (&bloom_debug);
	Cvar_RegisterVariable (&bloom_sigma);
	Cvar_RegisterVariable (&bloom_intensity);
	bloom_layout = VK_CreatePassLayout (VK_SHADER_STAGE_COMPUTE_BIT, sizeof(blur_push_t));
}

void VK_ShutdownBloom (void)
{
	VK_DestroyBloomPipelines ();
	if (bloom_layout)
		vkDestroyPipelineLayout (vk.device, bloom_layout, NULL);
	bloom_layout = VK_NULL_HANDLE;
}
