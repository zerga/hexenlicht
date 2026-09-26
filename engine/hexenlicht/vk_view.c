/* vk_view.c -- the 3D view: ray-traced passes into the render targets,
 * then a composite into the swapchain under the 2D
 *
 * R_RenderView calls VK_RenderView3D after the TLAS is built: vk_upscale.c
 * decides the frame's render size (the 3D view's size in pixels times
 * r_scale), jitter and upscaling, it fills this frame's global UBO
 * (vk_ubo.c) and dispatches the view passes at the render size, which end
 * in the TAA_OUTPUT render target (vk_images.c), the image Quake II RTX's
 * post-processing ends in: primary_rays.rgen writes the G-buffer (Quake II
 * RTX's primary rays, in its two checkerboard fields), reflect_refract.rgen
 * follows the paths through translucent surfaces and off mirrors and glass
 * pt_reflect_refract times (the G-buffer then holds what is seen through
 * or in them), direct_lighting.rgen lights it (after the denoiser's
 * gradient samples are placed, vk_asvgf.c), indirect_lighting.rgen adds
 * pt_num_bounce_rays bounces (0, 0.5 = half resolution, 1, 2), the
 * denoiser (flt_enable 1) or compositing.comp combines the lighting with
 * the surfaces and checkerboard_interleave.comp puts the fields into the
 * screen layout; the TAA pass (vk_upscale.c) takes the lit image into
 * TAA_OUTPUT (TAA, or TAAU up to the view's size), which the bloom
 * (vk_bloom.c) and the tone mapping (vk_tonemap.c) follow, then FSR if
 * chosen; or debug_view.comp shows a G-buffer or lighting channel,
 * selected by r_debugview (the G-buffer's before the bounces: with two,
 * the first stores its hit into the shading position). GL_EndRendering
 * then calls VK_DrawView3D, which scales the image into the swapchain's
 * 3D view rectangle (view_composite.frag, Quake II RTX's final blit)
 * before the 2D is drawn on top.
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
#ifdef HEXENLICHT_STREAMLINE
#include "vk_streamline.h"	/* SPIKE (3.9) */
#endif

/* 0 the path tracer's image (the lighting passes, bloom and tone mapping),
 * or the G-buffer's and lighting channels: 1 base color with the
 * effects over it, 2 normals, 3 material kinds (cutouts yellow, the weapon
 * cyan), 4 instances, 5 clusters (and the camera's PVS), 6 motion vectors,
 * 7 motion check, 8 geometric normals, 9 depth, 10
 * roughness/metallic/specular factor, 11 diffuse and 12 specular albedo, 13
 * effects, 14 blue noise, 15 direct diffuse and 16 specular lighting
 * (direct and bounced), 17 light list lengths, 18 indirect diffuse
 * lighting, 19 specular hit distances, 20 the denoiser's history length
 * (shaders/hl_shared.h's DEBUGVIEW_*); 1 until the maps have lights (4.1) */
static cvar_t	r_debugview = {"r_debugview", "1", CVAR_NONE};

static VkPipeline		primary_pipeline;	/* VK_PathTracerLayout () */
static VkPipeline		reflect_pipelines[2];	/* the first reflection or refraction pass, the others */
static VkPipeline		direct_pipeline;	/* the same */
static VkPipeline		indirect_pipelines[2];	/* the first and second bounce */
static VkPipeline		compositing_pipeline;
static VkPipeline		interleave_pipeline;
static VkPipeline		debug_pipeline;		/* the same */
#ifdef HEXENLICHT_STREAMLINE
static VkPipeline		guides_pipeline;	/* SPIKE (3.9): dlss_guides.comp */
#endif
static VkPipelineLayout		composite_layout;	/* the pass sets, composite_push_t */
static VkPipeline		composite_pipeline;
static VkFormat			composite_format;

/* view_composite.frag's push constants */
typedef struct
{
	float	uv_to_texel[2];	/* 0..1 over the view to the input's texel coordinates */
	int	input_size[2];	/* the input's texels shown over the view */
	float	gamma;		/* the "gamma" cvar */
	float	scale;		/* 1 / STORAGE_SCALE_HDR for the lit image without tone mapping, else 1 */
	int	filter_lanczos;	/* else nearest */
	int	source;		/* 0 TAA_OUTPUT, 1 FSR_EASU_OUTPUT, 2 FSR_RCAS_OUTPUT */
} composite_push_t;

static qboolean			view_drawn;		/* this frame has a 3D view to composite */
static VkRect2D			view_rect;		/* in the swapchain */
static float			view_scale;		/* composite_push_t's scale for this frame */

/* the images the composite may show (vk_upscale.c's display_source) */
static const int		display_images[3] = { VKPT_IMG_TAA_OUTPUT, VKPT_IMG_FSR_EASU_OUTPUT, VKPT_IMG_FSR_RCAS_OUTPUT };


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
	int	i;

	if (primary_pipeline)
		vkDestroyPipeline (vk.device, primary_pipeline, NULL);
	if (direct_pipeline)
		vkDestroyPipeline (vk.device, direct_pipeline, NULL);
	for (i = 0; i < 2; i++)
	{
		if (reflect_pipelines[i])
			vkDestroyPipeline (vk.device, reflect_pipelines[i], NULL);
		reflect_pipelines[i] = VK_NULL_HANDLE;
		if (indirect_pipelines[i])
			vkDestroyPipeline (vk.device, indirect_pipelines[i], NULL);
		indirect_pipelines[i] = VK_NULL_HANDLE;
	}
	if (compositing_pipeline)
		vkDestroyPipeline (vk.device, compositing_pipeline, NULL);
	if (interleave_pipeline)
		vkDestroyPipeline (vk.device, interleave_pipeline, NULL);
	if (debug_pipeline)
		vkDestroyPipeline (vk.device, debug_pipeline, NULL);
	if (composite_pipeline)
		vkDestroyPipeline (vk.device, composite_pipeline, NULL);
	primary_pipeline = direct_pipeline = compositing_pipeline = interleave_pipeline = VK_NULL_HANDLE;
	debug_pipeline = composite_pipeline = VK_NULL_HANDLE;
#ifdef HEXENLICHT_STREAMLINE
	if (guides_pipeline)
		vkDestroyPipeline (vk.device, guides_pipeline, NULL);
	guides_pipeline = VK_NULL_HANDLE;
#endif
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

/* the time since the last 3D frame for the eye adaptation, as Quake II
 * RTX's: game time, at most a second, the real time while it stands still
 * (paused) */
static float FrameTime (void)
{
	static double	last_time, last_realtime;
	float		t = (float) q_min (1.0, q_max (0.0, r_scene.time - last_time));
	float		wall = (float) q_min (1.0, q_max (0.0, realtime - last_realtime));

	last_time = r_scene.time;
	last_realtime = realtime;
	return (t > 0.0f) ? t : wall;
}

/* whether the 3D view can be drawn this frame, into view_rect */
static qboolean ViewReady (void)
{
	if (!vk.frame_active || !r_scene.worldmodel || !VK_TLASBuiltThisFrame ())
		return false;
	if (!ViewRect (&view_rect) || !VK_ImagesReady ())
		return false;
	/* the render targets have the swapchain's size (the width rounded up
	 * to even, as the view's for rendering), the view fits in them */
	return ((view_rect.extent.width + 1) & ~1u) <= vk_image_extent.width &&
	       view_rect.extent.height <= vk_image_extent.height;
}

#ifdef HEXENLICHT_STREAMLINE
/* ==========================================================================
 * SPIKE (3.9, not for merging): DLSS SR / RR through Streamline
 * ========================================================================== */

static cvar_t	r_dlss_jitter_sign = {"r_dlss_jitter_sign", "-1", CVAR_NONE};	/* DLSS takes the opposite of our sub-pixel offset (measured) */
static cvar_t	r_dlss_mv_sign = {"r_dlss_mv_sign", "1", CVAR_NONE};		/* our motion vectors or their opposite */
static cvar_t	r_dlss_preexposure = {"r_dlss_preexposure", "1", CVAR_NONE};	/* DLSS's preExposure */
static cvar_t	r_dlss_nohitdist = {"r_dlss_nohitdist", "0", CVAR_NONE};	/* RR gets 0 for every specular hit distance (a test) */
static uint32_t		dlss_last_frame;	/* the last 3D frame DLSS ran in */
static VkExtent2D	dlss_last_size;

static void SLImage (vk_sl_image_t *img, int index)
{
	VK_ImageInfo (index, &img->image, &img->view, &img->format, &img->width, &img->height);
}

static void AllMemoryBarrier (VkCommandBuffer cmd)
{
	VkMemoryBarrier2	barrier;
	VkDependencyInfo	dep;

	memset (&barrier, 0, sizeof(barrier));
	barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
	barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
	barrier.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
	barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
	barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
	memset (&dep, 0, sizeof(dep));
	dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
	dep.memoryBarrierCount = 1;
	dep.pMemoryBarriers = &barrier;
	vkCmdPipelineBarrier2 (cmd, &dep);
}

/* the guides in the screen layout, then Streamline's DLSS into TAA_OUTPUT */
static qboolean RunDLSS (VkCommandBuffer cmd, const vk_upscale_t *up, const pt_push_constants_t *push)
{
	const QVKUniformBuffer_t	*u = VK_CurrentUBO ();
	vk_sl_frame_t			f;
	qboolean			ok;

	if (!guides_pipeline)
		guides_pipeline = VK_CreateComputePipeline ("dlss_guides.comp", VK_PathTracerLayout ());
	{
		pt_push_constants_t	gp = *push;

		gp.bounce = (r_dlss_nohitdist.integer ? 1 : 0) | ((up->rr_blur == 2) ? 2 : 0);	/* dlss_guides.comp: 1 zeroes the hit distance, 2 blends the fields */
		VK_DispatchRays (cmd, guides_pipeline, &gp, up->render.width, up->render.height, 1);
	}
	AllMemoryBarrier (cmd);

	memset (&f, 0, sizeof(f));
	f.mode = up->dlss;
	f.frame = vk_render_frame;
	f.render_width = up->render.width;
	f.render_height = up->render.height;
	f.output_width = up->unscaled.width;
	f.output_height = up->unscaled.height;
	f.jitter[0] = up->jitter[0] * r_dlss_jitter_sign.value;
	f.jitter[1] = up->jitter[1] * r_dlss_jitter_sign.value;
	f.mvec_scale[0] = f.mvec_scale[1] = r_dlss_mv_sign.value;	/* ours are in UV units */
	memcpy (f.V, &u->V[0][0], sizeof(f.V));
	memcpy (f.invV, &u->invV[0][0], sizeof(f.invV));
	memcpy (f.P, &u->P[0][0], sizeof(f.P));
	memcpy (f.V_prev, &u->V_prev[0][0], sizeof(f.V_prev));
	memcpy (f.P_prev, &u->P_prev[0][0], sizeof(f.P_prev));
	/* a D3D-style depth row (0 at the near plane, 1 at the far), which
	 * dlss_guides.comp's hardware depth follows; Quake II RTX's
	 * projection only maps x and y */
	f.P[10] = f.P_prev[10] = 4096.0f / 4092.0f;
	f.P[14] = f.P_prev[14] = -4096.0f * 4.0f / 4092.0f;
	VK_InverseMatrix (f.P, f.invP);
	VectorCopy (r_scene.vieworg, f.cam_pos);
	VectorCopy (r_scene.forward, f.cam_fwd);
	VectorCopy (r_scene.right, f.cam_right);
	VectorCopy (r_scene.up, f.cam_up);
	f.znear = 4.0f;
	f.zfar = 4096.0f;
	f.fov_y = r_scene.fov_y * (float)M_PI / 180.0f;
	f.aspect = (float)up->unscaled.width / (float)up->unscaled.height;
	f.pre_exposure = r_dlss_preexposure.value;
	f.reset = dlss_last_frame + 1 != vk_render_frame || dlss_last_size.width != up->render.width ||
		  dlss_last_size.height != up->render.height;
	SLImage (&f.color_in, VKPT_IMG_FLAT_COLOR);
	SLImage (&f.color_out, VKPT_IMG_TAA_OUTPUT);
	SLImage (&f.depth, VKPT_IMG_DLSS_DEPTH);
	SLImage (&f.mvec, VKPT_IMG_FLAT_MOTION);
	SLImage (&f.albedo, VKPT_IMG_DLSS_ALBEDO);
	SLImage (&f.spec_albedo, VKPT_IMG_DLSS_SPEC_ALBEDO);
	SLImage (&f.normal_roughness, VKPT_IMG_DLSS_NORMAL_ROUGHNESS);
	SLImage (&f.spec_hit, VKPT_IMG_DLSS_SPEC_HIT);
	ok = VK_SLEvaluate (cmd, &f) != 0;
	AllMemoryBarrier (cmd);
	dlss_last_frame = vk_render_frame;
	dlss_last_size = up->render;
	return ok;
}
#endif

void VK_RenderView3D (void)
{
	VkCommandBuffer		cmd;
	const vk_upscale_t	*up;
	pt_push_constants_t	push;
	uint32_t		width, height;
	float			num_bounces;
	int			num_reflect, i, mode = q_min (q_max (r_debugview.integer, DEBUGVIEW_LIT), DEBUGVIEW_MAX);
	qboolean		denoise = VK_DenoiserEnabled ();

	view_drawn = false;
	if (!ViewReady ())
	{
		/* the images fall behind the entities' history (vk_instance.c),
		 * which the denoiser's history must keep in step with */
		VK_ResetDenoiserHistory ();
		return;
	}

	/* the render size, jitter and upscaling (vk_upscale.c; whether there
	 * is history, after the denoiser's cvars are checked), then the UBO */
	VK_CheckDenoiserCvars ();
	up = VK_UpscaleEvaluate (view_rect.extent.width, view_rect.extent.height, mode);
	width = up->render.width;
	height = up->render.height;
	VK_PrepareUBO (up, mode);
	if (up->dlss == 2)
		denoise = false;	/* SPIKE (3.9): DLSS RR takes the noisy image */

	if (!primary_pipeline)
		primary_pipeline = VK_CreateComputePipeline ("primary_rays.rgen", VK_PathTracerLayout ());
	for (i = 0; i < 2; i++)
	{
		if (!reflect_pipelines[i])
			reflect_pipelines[i] = VK_CreateComputePipelineSpec ("reflect_refract.rgen", VK_PathTracerLayout (), (uint32_t)i);
	}
	if (!direct_pipeline)
		direct_pipeline = VK_CreateComputePipeline ("direct_lighting.rgen", VK_PathTracerLayout ());
	for (i = 0; i < 2; i++)
	{
		if (!indirect_pipelines[i])
			indirect_pipelines[i] = VK_CreateComputePipelineSpec ("indirect_lighting.rgen", VK_PathTracerLayout (), (uint32_t)i);
	}
	if (!compositing_pipeline)
		compositing_pipeline = VK_CreateComputePipeline ("compositing.comp", VK_PathTracerLayout ());
	if (!interleave_pipeline)
		interleave_pipeline = VK_CreateComputePipeline ("checkerboard_interleave.comp", VK_PathTracerLayout ());
	if (!debug_pipeline)
		debug_pipeline = VK_CreateComputePipeline ("debug_view.comp", VK_PathTracerLayout ());

	cmd = vk.frames[vk.frame_index].cmd;
	/* the last frame's passes have read the images, its composite
	 * TAA_OUTPUT or FSR's output */
	VK_ComputeBarrier (cmd);
	for (i = 0; i < (int)Q_COUNTOF(display_images); i++)
		VK_RenderTargetBarrier (cmd, VK_Image (display_images[i]), VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
					VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
					VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
	VK_ClearLightStats (cmd);	/* the buffer the lighting passes count into */
	push.gpu_index = -1;
	push.bounce = 0;
	/* the G-buffer: each checkerboard field is half the width (Quake II
	 * RTX's vkpt_pt_trace_primary_rays) */
	VK_DispatchRays (cmd, primary_pipeline, &push, width / 2, height, 2);
	VK_ComputeBarrier (cmd);
	/* reflections and refractions (Quake II RTX's vkpt_pt_trace_reflections):
	 * pt_reflect_refract passes, each following the path one surface
	 * further; the first pass has its own pipeline */
	num_reflect = VK_ReflectRefractPasses ();
	for (i = 0; i < num_reflect; i++)
	{
		push.bounce = i;
		VK_DispatchRays (cmd, reflect_pipelines[i ? 1 : 0], &push, width / 2, height, 2);
		VK_ComputeBarrier (cmd);
	}
	push.bounce = 0;
	/* the denoiser's gradient samples: surfaces seen last frame, which the
	 * lighting passes shade as last frame did (vk_asvgf.c) */
	if (denoise)
		VK_GradientReproject (cmd, width, height);
	/* direct lighting of the G-buffer's surfaces, in the same fields */
	VK_DispatchRays (cmd, direct_pipeline, &push, width / 2, height, 2);
	VK_ComputeBarrier (cmd);
	/* the G-buffer's debug views before the bounces: with two, the first
	 * stores its hit into the shading position for the second; at the
	 * render size, which the composite scales */
	if (!DEBUGVIEW_READS_LIGHTING (mode))
	{
		VK_DispatchRays (cmd, debug_pipeline, &push, width, height, 1);
		VK_ComputeBarrier (cmd);
	}
	/* the bounces (Quake II RTX's vkpt_pt_trace_lighting): 0.5 traces
	 * every other row, alternating per frame, (h + 1) / 2 rows so an odd
	 * height's last row gets them too (Quake II RTX: h / 2) */
	num_bounces = VK_NumBounceRays ();
	for (i = 0; i < (int)ceilf (num_bounces); i++)
	{
		VK_DispatchRays (cmd, indirect_pipelines[i], &push, width / 2, (num_bounces == 0.5f) ? (height + 1) / 2 : height, 2);
		VK_ComputeBarrier (cmd);
	}
	/* the lighting times the surfaces, with the effects over them, into
	 * ASVGF_COLOR: denoised (the denoiser's last filter composites) or as
	 * it is (compositing.comp); then the fields interleaved into
	 * FLAT_COLOR, the checkerboard of translucent surfaces blurred when
	 * denoised */
	if (denoise)
	{
		VK_DenoiseLighting (cmd, width, height, num_bounces >= 0.5f);
	}
	else
	{
		VK_DispatchCompute (cmd, compositing_pipeline, width, height, 16);
		VK_ComputeBarrier (cmd);
	}
	VK_DispatchCompute (cmd, interleave_pipeline, width, height, 16);
	VK_ComputeBarrier (cmd);
	if (mode == DEBUGVIEW_LIT)
	{
		/* the lit image into TAA_OUTPUT (TAA, TAAU; a copy without the
		 * denoiser or history), in Quake II RTX's storage scale; then bloom,
		 * tone mapping and exposure on the TAA output, and FSR (Quake II
		 * RTX's order) */
		const uint32_t	w = up->taa_output.width, h = up->taa_output.height;

#ifdef HEXENLICHT_STREAMLINE
		if (up->dlss)
			RunDLSS (cmd, up, &push);	/* SPIKE (3.9) */
		else
#endif
		VK_UpscaleHDR (cmd);
		if (VK_BloomEnabled ())
			VK_Bloom (cmd, w, h);
		if (VK_ToneMappingEnabled ())
			VK_ToneMap (cmd, w, h, FrameTime ());
		VK_UpscaleDisplay (cmd);
	}
	else if (DEBUGVIEW_READS_LIGHTING (mode))
	{
		VK_DispatchRays (cmd, debug_pipeline, &push, width, height, 1);	/* the debug views stay as they are */
	}
	/* the composite shows one of them, but its shader has all three */
	for (i = 0; i < (int)Q_COUNTOF(display_images); i++)
		VK_RenderTargetBarrier (cmd, VK_Image (display_images[i]), VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
					VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
					VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
	/* the lit image without tone mapping is still scaled by
	 * STORAGE_SCALE_HDR (asvgf_atrous.comp, compositing.comp) */
	view_scale = (mode == DEBUGVIEW_LIT && !VK_ToneMappingEnabled ()) ? 1.0f / STORAGE_SCALE_HDR : 1.0f;
	VK_EndDenoiserFrame (denoise);
	VK_EndUpscaleFrame ();
	view_drawn = true;
}

/* in GL_EndRendering, with the swapchain rendering begun, before the 2D */
void VK_DrawView3D (void)
{
	VkCommandBuffer		cmd = vk.frames[vk.frame_index].cmd;
	const vk_upscale_t	*up = VK_Upscale ();
	VkViewport		viewport;
	composite_push_t	push;

	if (!view_drawn)
		return;
	view_drawn = false;

	/* the upscaler's output covers the view (its width rounded up to
	 * even: an odd view drops the last column), shown as Quake II RTX's
	 * final blit shows it */
	push.uv_to_texel[0] = (float)up->view.width * (float)up->display_size.width / (float)up->unscaled.width;
	push.uv_to_texel[1] = (float)up->view.height * (float)up->display_size.height / (float)up->unscaled.height;
	push.input_size[0] = (int)up->display_size.width;
	push.input_size[1] = (int)up->display_size.height;
	push.gamma = v_gamma.value;
	push.scale = view_scale;
	push.filter_lanczos = up->display_lanczos ? 1 : 0;
	push.source = up->display_source;

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
	vkCmdPushConstants (cmd, composite_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
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
#ifdef HEXENLICHT_STREAMLINE
	Cvar_RegisterVariable (&r_dlss_jitter_sign);
	Cvar_RegisterVariable (&r_dlss_mv_sign);
	Cvar_RegisterVariable (&r_dlss_preexposure);
	Cvar_RegisterVariable (&r_dlss_nohitdist);
#endif
	composite_layout = VK_CreatePassLayout (VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(composite_push_t));
}

void VK_ShutdownView (void)
{
	VK_DestroyViewPipelines ();
	if (composite_layout)
		vkDestroyPipelineLayout (vk.device, composite_layout, NULL);
	composite_layout = VK_NULL_HANDLE;
	view_drawn = false;
}
