/* vk_view.c -- the 3D view: ray-traced passes into the render targets,
 * then a composite into the swapchain under the 2D
 *
 * R_RenderView calls VK_RenderView3D after the TLAS is built: it fills
 * this frame's global UBO (vk_ubo.c) for the 3D view's size in pixels and
 * dispatches the view passes, which end in the TAA_OUTPUT render target
 * (vk_images.c), the image Quake II RTX's post-processing ends in:
 * primary_rays.rgen writes the G-buffer (Quake II RTX's primary rays, in
 * its two checkerboard fields), then, for now, debug_view.comp shows its
 * channels, selected by r_debugview; the lighting passes of epic E3 come
 * between them. GL_EndRendering then calls VK_DrawView3D, which copies the
 * image into the swapchain's 3D view rectangle (view_composite.frag,
 * Quake II RTX's final blit) before the 2D is drawn on top.
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

#include "quakedef.h"
#include "vk_local.h"
#include "vid_vk.h"
#include "r_scene.h"
#include "shaders/hl_shared.h"
#include "shaders/global_textures.h"

/* the G-buffer's channels: 1 base color with the effects over it, 2
 * normals, 3 material kinds (cutouts yellow, the weapon cyan), 4 instances,
 * 5 clusters (and the camera's PVS), 6 motion vectors, 7 motion check, 8
 * geometric normals, 9 depth, 10 roughness/metallic/specular factor, 11
 * diffuse and 12 specular albedo, 13 effects, 14 blue noise
 * (shaders/hl_shared.h's DEBUGVIEW_*); 0 draws no 3D view */
static cvar_t	r_debugview = {"r_debugview", "1", CVAR_NONE};

static VkPipeline		primary_pipeline;	/* VK_PathTracerLayout () */
static VkPipeline		debug_pipeline;		/* the same */
static VkPipelineLayout		composite_layout;	/* the pass sets, gamma */
static VkPipeline		composite_pipeline;
static VkFormat			composite_format;

static qboolean			view_drawn;		/* this frame has a 3D view to composite */
static VkRect2D			view_rect;		/* in the swapchain */


/* ==========================================================================
 * Pipelines
 * ========================================================================== */

/* fullscreen.vert + view_composite.frag, for the swapchain's format */
static void CreateCompositePipeline (void)
{
	VkShaderModule				vert, frag;
	VkPipelineShaderStageCreateInfo		stages[2];
	VkPipelineVertexInputStateCreateInfo	vertex_input;
	VkPipelineInputAssemblyStateCreateInfo	input_assembly;
	VkPipelineViewportStateCreateInfo	viewport;
	VkPipelineRasterizationStateCreateInfo	raster;
	VkPipelineMultisampleStateCreateInfo	multisample;
	VkPipelineColorBlendAttachmentState	blend_attachment;
	VkPipelineColorBlendStateCreateInfo	blend;
	VkPipelineDynamicStateCreateInfo	dynamic;
	VkPipelineRenderingCreateInfo		rendering;
	VkGraphicsPipelineCreateInfo		info;
	const VkDynamicState	dynamic_states[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };

	vert = VK_LoadShader ("fullscreen.vert");
	frag = VK_LoadShader ("view_composite.frag");

	memset (stages, 0, sizeof(stages));
	stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = vert;
	stages[0].pName = "main";
	stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = frag;
	stages[1].pName = "main";

	memset (&vertex_input, 0, sizeof(vertex_input));
	vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	memset (&input_assembly, 0, sizeof(input_assembly));
	input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	memset (&viewport, 0, sizeof(viewport));
	viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	viewport.viewportCount = 1;
	viewport.scissorCount = 1;
	memset (&raster, 0, sizeof(raster));
	raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	raster.polygonMode = VK_POLYGON_MODE_FILL;
	raster.cullMode = VK_CULL_MODE_NONE;
	raster.lineWidth = 1.0f;
	memset (&multisample, 0, sizeof(multisample));
	multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
	memset (&blend_attachment, 0, sizeof(blend_attachment));
	blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
					  VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
	memset (&blend, 0, sizeof(blend));
	blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	blend.attachmentCount = 1;
	blend.pAttachments = &blend_attachment;
	memset (&dynamic, 0, sizeof(dynamic));
	dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dynamic.dynamicStateCount = Q_COUNTOF(dynamic_states);
	dynamic.pDynamicStates = dynamic_states;

	composite_format = vk.surface_format.format;
	memset (&rendering, 0, sizeof(rendering));
	rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
	rendering.colorAttachmentCount = 1;
	rendering.pColorAttachmentFormats = &composite_format;

	memset (&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	info.pNext = &rendering;
	info.stageCount = 2;
	info.pStages = stages;
	info.pVertexInputState = &vertex_input;
	info.pInputAssemblyState = &input_assembly;
	info.pViewportState = &viewport;
	info.pRasterizationState = &raster;
	info.pMultisampleState = &multisample;
	info.pColorBlendState = &blend;
	info.pDynamicState = &dynamic;
	info.layout = composite_layout;
	VK_CHECK (vkCreateGraphicsPipelines (vk.device, VK_NULL_HANDLE, 1, &info, NULL, &composite_pipeline));

	vkDestroyShaderModule (vk.device, vert, NULL);
	vkDestroyShaderModule (vk.device, frag, NULL);
}

void VK_DestroyViewPipelines (void)
{
	if (primary_pipeline)
		vkDestroyPipeline (vk.device, primary_pipeline, NULL);
	if (debug_pipeline)
		vkDestroyPipeline (vk.device, debug_pipeline, NULL);
	if (composite_pipeline)
		vkDestroyPipeline (vk.device, composite_pipeline, NULL);
	primary_pipeline = debug_pipeline = composite_pipeline = VK_NULL_HANDLE;
}


/* ==========================================================================
 * The frame's view
 * ========================================================================== */

/* the 3D view in swapchain pixels: r_refdef's vrect in the 2D screen, at
 * the UI scale, centered like vk_draw.c's Draw_Flush */
static qboolean ViewRect (VkRect2D *r)
{
	int	s = VID_GetUIScale ();
	int	ox = ((int)vk.extent.width - vid.width * s) / 2;
	int	oy = ((int)vk.extent.height - vid.height * s) / 2;
	int	x0 = q_max (ox + r_scene.vrect.x * s, 0);
	int	y0 = q_max (oy + r_scene.vrect.y * s, 0);
	int	x1 = q_min (ox + (r_scene.vrect.x + r_scene.vrect.width) * s, (int)vk.extent.width);
	int	y1 = q_min (oy + (r_scene.vrect.y + r_scene.vrect.height) * s, (int)vk.extent.height);

	if (x1 <= x0 || y1 <= y0)
		return false;
	r->offset.x = x0;
	r->offset.y = y0;
	r->extent.width = (uint32_t)(x1 - x0);
	r->extent.height = (uint32_t)(y1 - y0);
	return true;
}

void VK_RenderView3D (void)
{
	VkCommandBuffer		cmd;
	VkImage			output;
	pt_push_constants_t	push;
	uint32_t		width;
	int			mode = r_debugview.integer;

	view_drawn = false;
	if (!vk.frame_active || mode <= DEBUGVIEW_OFF || !r_scene.worldmodel || !VK_TLASBuiltThisFrame ())
		return;
	if (!ViewRect (&view_rect) || !VK_ImagesReady ())
		return;
	/* the render targets have the swapchain's size (the width rounded up
	 * to even, as the view's for rendering), the view fits in them */
	width = (view_rect.extent.width + 1) & ~1u;
	if (width > vk_image_extent.width || view_rect.extent.height > vk_image_extent.height)
		return;

	VK_PrepareUBO (view_rect.extent.width, view_rect.extent.height, q_min (mode, DEBUGVIEW_MAX));

	if (!primary_pipeline)
		primary_pipeline = VK_CreateComputePipeline ("primary_rays.rgen", VK_PathTracerLayout ());
	if (!debug_pipeline)
		debug_pipeline = VK_CreateComputePipeline ("debug_view.comp", VK_PathTracerLayout ());

	cmd = vk.frames[vk.frame_index].cmd;
	output = VK_Image (VKPT_IMG_TAA_OUTPUT);
	/* the last frame's passes have read the images, its composite TAA_OUTPUT */
	VK_ComputeBarrier (cmd);
	VK_RenderTargetBarrier (cmd, output, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
			 VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
	push.gpu_index = -1;
	push.bounce = 0;
	/* the G-buffer: each checkerboard field is half the width (Quake II
	 * RTX's vkpt_pt_trace_primary_rays) */
	VK_DispatchRays (cmd, primary_pipeline, &push, width / 2, view_rect.extent.height, 2);
	VK_ComputeBarrier (cmd);
	VK_DispatchRays (cmd, debug_pipeline, &push, view_rect.extent.width, view_rect.extent.height, 1);
	VK_RenderTargetBarrier (cmd, output, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
			 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
	view_drawn = true;
}

/* in GL_EndRendering, with the swapchain rendering begun, before the 2D */
void VK_DrawView3D (void)
{
	VkCommandBuffer	cmd = vk.frames[vk.frame_index].cmd;
	VkViewport	viewport;
	float		gamma = v_gamma.value;

	if (!view_drawn)
		return;
	view_drawn = false;

	if (composite_pipeline && composite_format != vk.surface_format.format)
	{
		vkDeviceWaitIdle (vk.device);
		vkDestroyPipeline (vk.device, composite_pipeline, NULL);
		composite_pipeline = VK_NULL_HANDLE;
	}
	if (!composite_pipeline)
		CreateCompositePipeline ();

	viewport.x = (float)view_rect.offset.x;
	viewport.y = (float)view_rect.offset.y;
	viewport.width = (float)view_rect.extent.width;
	viewport.height = (float)view_rect.extent.height;
	viewport.minDepth = 0.0f;
	viewport.maxDepth = 1.0f;
	vkCmdSetViewport (cmd, 0, 1, &viewport);
	vkCmdSetScissor (cmd, 0, 1, &view_rect);
	vkCmdBindPipeline (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, composite_pipeline);
	VK_BindPassSets (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, composite_layout);
	vkCmdPushConstants (cmd, composite_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(gamma), &gamma);
	vkCmdDraw (cmd, 3, 1, 0, 0);

	/* back to the whole swapchain for the 2D */
	viewport.x = viewport.y = 0.0f;
	viewport.width = (float)vk.extent.width;
	viewport.height = (float)vk.extent.height;
	vkCmdSetViewport (cmd, 0, 1, &viewport);
	view_rect.offset.x = view_rect.offset.y = 0;
	view_rect.extent = vk.extent;
	vkCmdSetScissor (cmd, 0, 1, &view_rect);
}


/* ==========================================================================
 * Init / shutdown
 * ========================================================================== */

void VK_InitView (void)
{
	Cvar_RegisterVariable (&r_debugview);
	composite_layout = VK_CreatePassLayout (VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(float));
}

void VK_ShutdownView (void)
{
	VK_DestroyViewPipelines ();
	if (composite_layout)
		vkDestroyPipelineLayout (vk.device, composite_layout, NULL);
	composite_layout = VK_NULL_HANDLE;
	view_drawn = false;
}
