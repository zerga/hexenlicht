/* vk_asvgf.c -- the denoiser: Quake II RTX's A-SVGF
 *
 * Quake II RTX's asvgf.c without its TAA (3.8): adaptive spatiotemporal
 * variance-guided filtering of the lighting passes' output, in the
 * G-buffer's two checkerboard fields (shaders/asvgf.glsl explains the
 * method). vk_view.c runs it when flt_enable is 1 (Quake II RTX's
 * default):
 *
 *  - VK_GradientReproject, after the reflection and refraction passes and
 *    before the lighting (asvgf_gradient_reproject.comp): in every 3x3
 *    square it picks the brightest pixel whose surface was seen last
 *    frame, gives it last frame's random numbers and surface, and moves it
 *    to where that surface is now. The lighting passes shade these
 *    gradient samples (get_is_gradient) as last frame did, with this
 *    frame's lights. Last frame's model instances are found through the
 *    instance buffer's model_prev_to_current (vk_instance.c). Without
 *    history (below) there are no gradient samples.
 *  - VK_DenoiseLighting, after the bounces, instead of compositing.comp:
 *    the gradient image (the lighting's change at the gradient samples,
 *    asvgf_gradient_img.comp) blurred by 7 a-trous passes
 *    (asvgf_gradient_atrous.comp); the temporal filter
 *    (asvgf_temporal.comp), which blends the lighting with last frame's
 *    at the motion vectors and drops history where the gradients show the
 *    lighting changed (anti-lag); 4 iterations of the spatial a-trous
 *    filter, the indirect diffuse (spherical harmonics) at 1/3 resolution
 *    (asvgf_lf.comp, with bounces) and the direct diffuse and specular at
 *    full resolution (asvgf_atrous.comp); the last one composites the
 *    lighting with the surfaces and the effects into ASVGF_COLOR, as
 *    compositing.comp does without the denoiser.
 *
 * The filters' history is the images of the last 3D frame (the _B images
 * and the history images). It is not used (Quake II RTX's
 * temporal_frame_valid: flt_temporal_* are 0 in the UBO, vk_ubo.c) in
 * the first frame, after a frame without the denoiser, a new map, new
 * images (the swapchain's size), a change of flt_enable or
 * flt_temporal_*, and after a frame whose 3D view or instances were
 * skipped (the entities' history moved on without the images).
 *
 * Copyright (C) 2018 Christoph Schied
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

#define GRADIENT_ATROUS_ITERATIONS	7	/* Quake II RTX's num_atrous_iterations_gradient */
#define ATROUS_ITERATIONS		4	/* its num_atrous_iterations: asvgf_atrous.comp's spec_iteration 0-3 */
#define REPROJECT_GROUP_PIXELS		24	/* asvgf_gradient_reproject.comp's GROUP_SIZE_PIXELS */
#define TEMPORAL_GROUP_SIZE		15	/* asvgf_temporal.comp's GROUP_SIZE */

/* all with the path tracer's layout (VK_PathTracerLayout); the gradient
 * a-trous and the LF filter read their iteration from the first push
 * constant */
static VkPipeline	reproject_pipeline;
static VkPipeline	gradient_pipeline;
static VkPipeline	gradient_atrous_pipeline;
static VkPipeline	temporal_pipeline;
static VkPipeline	lf_pipeline;
static VkPipeline	atrous_pipelines[ATROUS_ITERATIONS];

static qboolean		history_valid;	/* the images hold the last 3D frame's denoiser history */

static void CreatePipelines (void)
{
	VkPipelineLayout	layout = VK_PathTracerLayout ();
	int			i;

	if (!reproject_pipeline)
		reproject_pipeline = VK_CreateComputePipeline ("asvgf_gradient_reproject.comp", layout);
	if (!gradient_pipeline)
		gradient_pipeline = VK_CreateComputePipeline ("asvgf_gradient_img.comp", layout);
	if (!gradient_atrous_pipeline)
		gradient_atrous_pipeline = VK_CreateComputePipeline ("asvgf_gradient_atrous.comp", layout);
	if (!temporal_pipeline)
		temporal_pipeline = VK_CreateComputePipeline ("asvgf_temporal.comp", layout);
	if (!lf_pipeline)
		lf_pipeline = VK_CreateComputePipeline ("asvgf_lf.comp", layout);
	for (i = 0; i < ATROUS_ITERATIONS; i++)
	{
		if (!atrous_pipelines[i])
			atrous_pipelines[i] = VK_CreateComputePipelineSpec ("asvgf_atrous.comp", layout, (uint32_t)i);
	}
}

void VK_DestroyASVGFPipelines (void)
{
	VkPipeline	*all[] = { &reproject_pipeline, &gradient_pipeline, &gradient_atrous_pipeline,
				   &temporal_pipeline, &lf_pipeline, &atrous_pipelines[0], &atrous_pipelines[1],
				   &atrous_pipelines[2], &atrous_pipelines[3] };
	int		i;

	for (i = 0; i < (int)Q_COUNTOF(all); i++)
	{
		if (*all[i])
			vkDestroyPipeline (vk.device, *all[i], NULL);
		*all[i] = VK_NULL_HANDLE;
	}
}

/* a pass at 1/GRAD_DWN resolution (16x16 groups, Quake II RTX's
 * (width / GRAD_DWN + 15) / 16) whose shader reads its iteration from the
 * push constants */
static void DispatchIteration (VkCommandBuffer cmd, VkPipeline pipeline, uint32_t iteration, uint32_t width, uint32_t height)
{
	VkPipelineLayout	layout = VK_PathTracerLayout ();

	vkCmdBindPipeline (cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
	VK_BindPassSets (cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout);
	vkCmdPushConstants (cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(iteration), &iteration);
	vkCmdDispatch (cmd, (width / GRAD_DWN + 15) / 16, (height / GRAD_DWN + 15) / 16, 1);
}

/* no gradient samples this frame: this frame's ASVGF_GRAD_SMPL_POS_A
 * (the even set's _A image, the odd set's _B) cleared, which the lighting
 * passes, the filters and the next frame's reprojection read */
static void ClearGradientSamples (VkCommandBuffer cmd)
{
	VkImage			image = VK_Image ((vk_render_frame & 1) ? VKPT_IMG_ASVGF_GRAD_SMPL_POS_B : VKPT_IMG_ASVGF_GRAD_SMPL_POS_A);
	VkClearColorValue	zero;
	VkImageSubresourceRange	range;

	memset (&zero, 0, sizeof(zero));
	memset (&range, 0, sizeof(range));
	range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	range.levelCount = 1;
	range.layerCount = 1;
	VK_RenderTargetBarrier (cmd, image, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
				VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
	vkCmdClearColorImage (cmd, image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
	VK_RenderTargetBarrier (cmd, image, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
				VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
}

/* Quake II RTX's vkpt_asvgf_gradient_reproject, for a view rendered
 * width x height (both fields), after VK_PrepareUBO. Only with history:
 * without, the last frame's images and instance map may belong to another
 * map, view or instance list, whose primitives the reprojection would
 * read by device address unchecked (Quake II RTX reprojects every frame);
 * the gradients go unused then anyway (flt_temporal_* 0: no history) */
void VK_GradientReproject (VkCommandBuffer cmd, uint32_t width, uint32_t height)
{
	if (!history_valid)
	{
		ClearGradientSamples (cmd);
		return;
	}
	CreatePipelines ();
	VK_DispatchCompute (cmd, reproject_pipeline, width, height, REPROJECT_GROUP_PIXELS);
	VK_ComputeBarrier (cmd);
}

/* Quake II RTX's vkpt_asvgf_filter; enable_lf: the indirect diffuse has
 * bounce light to filter (pt_num_bounce_rays >= 0.5) */
void VK_DenoiseLighting (VkCommandBuffer cmd, uint32_t width, uint32_t height, qboolean enable_lf)
{
	uint32_t	i;

	CreatePipelines ();

	/* the gradients: made at the gradient samples, then blurred */
	VK_DispatchCompute (cmd, gradient_pipeline, width / GRAD_DWN, height / GRAD_DWN, 16);
	VK_ComputeBarrier (cmd);
	for (i = 0; i < GRADIENT_ATROUS_ITERATIONS; i++)
	{
		DispatchIteration (cmd, gradient_atrous_pipeline, i, width, height);
		VK_ComputeBarrier (cmd);
	}

	/* the temporal filter */
	VK_DispatchCompute (cmd, temporal_pipeline, width, height, TEMPORAL_GROUP_SIZE);
	VK_ComputeBarrier (cmd);

	/* the spatial filter; an LF iteration and the HF/specular one of the
	 * same number don't share images, until the last, which composites */
	for (i = 0; i < ATROUS_ITERATIONS; i++)
	{
		if (enable_lf)
		{
			DispatchIteration (cmd, lf_pipeline, i, width, height);
			if (i == ATROUS_ITERATIONS - 1)
				VK_ComputeBarrier (cmd);
		}
		VK_DispatchCompute (cmd, atrous_pipelines[i], width, height, 16);
		VK_ComputeBarrier (cmd);
	}
}

/* no history for the next frame's filters */
void VK_ResetDenoiserHistory (void)
{
	history_valid = false;
}

qboolean VK_DenoiserHistoryValid (void)
{
	return history_valid;
}

/* at the end of a 3D frame: whether the denoiser ran, so its images are
 * the next frame's history */
void VK_EndDenoiserFrame (qboolean denoised)
{
	history_valid = denoised;
}
