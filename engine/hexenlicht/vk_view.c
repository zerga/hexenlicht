/* vk_view.c -- the 3D view: ray-traced passes into a view image, then a
 * composite into the swapchain under the 2D
 *
 * R_RenderView calls VK_RenderView3D after the TLAS is built: it fills
 * this frame's ViewUniforms (shaders/hl_shared.h) from r_scene and
 * dispatches the view pass into the view image, sized to the 3D view in
 * pixels. For now the pass is debug_view.comp, selected by r_debugview;
 * the path tracer of epic E3 replaces it. GL_EndRendering then calls
 * VK_DrawView3D, which copies the image into the swapchain's 3D view
 * rectangle (view_composite.frag) before the 2D is drawn on top.
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

#define VIEW_FORMAT	VK_FORMAT_R16G16B16A16_SFLOAT	/* linear, room for HDR later */

COMPILE_TIME_ASSERT(ViewUniforms, sizeof(ViewUniforms) == 144);	/* the shaders' std430 layout */

/* 1 albedo, 2 normals, 3 material kinds, 4 instances, 5 clusters (and the
 * camera's PVS); 0 draws no 3D view */
static cvar_t	r_debugview = {"r_debugview", "1", CVAR_NONE};

static VkDescriptorSetLayout	view_set_layout;	/* binding 0: the view image */
static VkDescriptorPool		view_pool;
static VkDescriptorSet		view_set;

static VkImage			view_image;
static VmaAllocation		view_allocation;
static VkImageView		view_image_view;
static uint32_t			view_width, view_height;

static VkPipelineLayout		pass_layout;		/* textures (set 0), view image (set 1) */
static VkPipeline		debug_pipeline;
static VkPipelineLayout		composite_layout;	/* view image (set 0) */
static VkPipeline		composite_pipeline;
static VkFormat			composite_format;

static vk_buffer_t		uniform_buffers[VK_FRAMES_IN_FLIGHT];

static qboolean			view_drawn;		/* this frame has a 3D view to composite */
static VkRect2D			view_rect;		/* in the swapchain */


/* ==========================================================================
 * The view image
 * ========================================================================== */

static void DestroyViewImage (void)
{
	if (view_image_view)
		vkDestroyImageView (vk.device, view_image_view, NULL);
	if (view_image)
		vmaDestroyImage (vk.allocator, view_image, view_allocation);
	view_image_view = VK_NULL_HANDLE;
	view_image = VK_NULL_HANDLE;
	view_width = view_height = 0;
}

static void CreateViewImage (uint32_t width, uint32_t height)
{
	VkImageCreateInfo	image_info;
	VmaAllocationCreateInfo	alloc_info;
	VkImageViewCreateInfo	view_info;
	VkDescriptorImageInfo	desc_image;
	VkWriteDescriptorSet	write;

	vkDeviceWaitIdle (vk.device);	/* earlier frames may still use the old one */
	DestroyViewImage ();

	memset (&image_info, 0, sizeof(image_info));
	image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	image_info.imageType = VK_IMAGE_TYPE_2D;
	image_info.format = VIEW_FORMAT;
	image_info.extent.width = width;
	image_info.extent.height = height;
	image_info.extent.depth = 1;
	image_info.mipLevels = 1;
	image_info.arrayLayers = 1;
	image_info.samples = VK_SAMPLE_COUNT_1_BIT;
	image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
	image_info.usage = VK_IMAGE_USAGE_STORAGE_BIT;
	image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	memset (&alloc_info, 0, sizeof(alloc_info));
	alloc_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
	VK_CHECK (vmaCreateImage (vk.allocator, &image_info, &alloc_info, &view_image, &view_allocation, NULL));

	memset (&view_info, 0, sizeof(view_info));
	view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	view_info.image = view_image;
	view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
	view_info.format = VIEW_FORMAT;
	view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	view_info.subresourceRange.levelCount = 1;
	view_info.subresourceRange.layerCount = 1;
	VK_CHECK (vkCreateImageView (vk.device, &view_info, NULL, &view_image_view));

	memset (&desc_image, 0, sizeof(desc_image));
	desc_image.imageView = view_image_view;
	desc_image.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
	memset (&write, 0, sizeof(write));
	write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	write.dstSet = view_set;
	write.dstBinding = 0;
	write.descriptorCount = 1;
	write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	write.pImageInfo = &desc_image;
	vkUpdateDescriptorSets (vk.device, 1, &write, 0, NULL);

	view_width = width;
	view_height = height;
}

static void ViewImageBarrier (VkCommandBuffer cmd, VkImageLayout old_layout,
			      VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
			      VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access)
{
	VkImageMemoryBarrier2	barrier;
	VkDependencyInfo	dep;

	memset (&barrier, 0, sizeof(barrier));
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
	barrier.srcStageMask = src_stage;
	barrier.srcAccessMask = src_access;
	barrier.dstStageMask = dst_stage;
	barrier.dstAccessMask = dst_access;
	barrier.oldLayout = old_layout;
	barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = view_image;
	barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barrier.subresourceRange.levelCount = 1;
	barrier.subresourceRange.layerCount = 1;
	memset (&dep, 0, sizeof(dep));
	dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
	dep.imageMemoryBarrierCount = 1;
	dep.pImageMemoryBarriers = &barrier;
	vkCmdPipelineBarrier2 (cmd, &dep);
}


/* ==========================================================================
 * Pipelines
 * ========================================================================== */

static void CreateDebugPipeline (void)
{
	VkComputePipelineCreateInfo	info;
	VkShaderModule			module = VK_LoadShader ("debug_view.comp");

	memset (&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
	info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	info.stage.module = module;
	info.stage.pName = "main";
	info.layout = pass_layout;
	VK_CHECK (vkCreateComputePipelines (vk.device, VK_NULL_HANDLE, 1, &info, NULL, &debug_pipeline));
	vkDestroyShaderModule (vk.device, module, NULL);
}

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
	ViewUniforms	*u;
	VkCommandBuffer	cmd;
	VkDescriptorSet	sets[2];
	VkDeviceAddress	address;
	int		mode = r_debugview.integer;

	view_drawn = false;
	if (!vk.frame_active || mode <= DEBUGVIEW_OFF || !r_scene.worldmodel || !VK_TLASBuiltThisFrame ())
		return;
	if (!ViewRect (&view_rect))
		return;
	if (view_rect.extent.width != view_width || view_rect.extent.height != view_height)
		CreateViewImage (view_rect.extent.width, view_rect.extent.height);

	u = (ViewUniforms *) uniform_buffers[vk.frame_index].mapped;
	memset (u, 0, sizeof(*u));
	VectorCopy (r_scene.vieworg, u->origin);
	VectorCopy (r_scene.forward, u->forward);
	VectorCopy (r_scene.right, u->right);
	VectorCopy (r_scene.up, u->up);
	u->tan_half_fov[0] = tanf (r_scene.fov_x * (float)M_PI / 360.0f);
	u->tan_half_fov[1] = tanf (r_scene.fov_y * (float)M_PI / 360.0f);
	u->size[0] = view_width;
	u->size[1] = view_height;
	u->time = (float)r_scene.time;
	u->anim_frame = (int)(r_scene.time * 5.0);	/* R_TextureAnimation's frame */
	u->debug_mode = (uint32_t)q_min (mode, DEBUGVIEW_MAX);
	u->view_cluster = r_scene.viewleaf ? (int)(r_scene.viewleaf - r_scene.worldmodel->leafs) - 1 : -1;
	u->tlas = VK_TLASAddress ();
	u->primitives = vk_world.buffer.address;
	u->tlas_info = VK_TLASInfoAddress ();
	u->instances = VK_InstanceBuffer ()->address;
	u->materials = vk_material_table.address;
	u->pvs = vk_pvs.buffer.address;
	VK_CHECK (vmaFlushAllocation (vk.allocator, uniform_buffers[vk.frame_index].allocation, 0, sizeof(*u)));

	if (!debug_pipeline)
		CreateDebugPipeline ();

	cmd = vk.frames[vk.frame_index].cmd;
	/* the last frame's composite has read the image; its contents go */
	ViewImageBarrier (cmd, VK_IMAGE_LAYOUT_UNDEFINED,
			  VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
			  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
	sets[0] = vk.texture_set;
	sets[1] = view_set;
	address = uniform_buffers[vk.frame_index].address;
	vkCmdBindPipeline (cmd, VK_PIPELINE_BIND_POINT_COMPUTE, debug_pipeline);
	vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pass_layout, 0, 2, sets, 0, NULL);
	vkCmdPushConstants (cmd, pass_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(address), &address);
	vkCmdDispatch (cmd, (view_width + 7) / 8, (view_height + 7) / 8, 1);
	ViewImageBarrier (cmd, VK_IMAGE_LAYOUT_GENERAL,
			  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
			  VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
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
	vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, composite_layout, 0, 1, &view_set, 0, NULL);
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
	VkDescriptorSetLayoutBinding	binding;
	VkDescriptorSetLayoutCreateInfo	layout_info;
	VkDescriptorPoolSize		pool_size;
	VkDescriptorPoolCreateInfo	pool_info;
	VkDescriptorSetAllocateInfo	alloc_info;
	VkPipelineLayoutCreateInfo	pl_info;
	VkPushConstantRange		push_range;
	VkDescriptorSetLayout		pass_sets[2];
	int				i;

	Cvar_RegisterVariable (&r_debugview);

	memset (&binding, 0, sizeof(binding));
	binding.binding = 0;
	binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	binding.descriptorCount = 1;
	binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
	memset (&layout_info, 0, sizeof(layout_info));
	layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	layout_info.bindingCount = 1;
	layout_info.pBindings = &binding;
	VK_CHECK (vkCreateDescriptorSetLayout (vk.device, &layout_info, NULL, &view_set_layout));

	pool_size.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	pool_size.descriptorCount = 1;
	memset (&pool_info, 0, sizeof(pool_info));
	pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	pool_info.maxSets = 1;
	pool_info.poolSizeCount = 1;
	pool_info.pPoolSizes = &pool_size;
	VK_CHECK (vkCreateDescriptorPool (vk.device, &pool_info, NULL, &view_pool));
	memset (&alloc_info, 0, sizeof(alloc_info));
	alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	alloc_info.descriptorPool = view_pool;
	alloc_info.descriptorSetCount = 1;
	alloc_info.pSetLayouts = &view_set_layout;
	VK_CHECK (vkAllocateDescriptorSets (vk.device, &alloc_info, &view_set));

	/* the view passes: textures, the view image, the ViewUniforms' address */
	memset (&push_range, 0, sizeof(push_range));
	push_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	push_range.size = sizeof(VkDeviceAddress);
	pass_sets[0] = vk.texture_set_layout;
	pass_sets[1] = view_set_layout;
	memset (&pl_info, 0, sizeof(pl_info));
	pl_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	pl_info.setLayoutCount = 2;
	pl_info.pSetLayouts = pass_sets;
	pl_info.pushConstantRangeCount = 1;
	pl_info.pPushConstantRanges = &push_range;
	VK_CHECK (vkCreatePipelineLayout (vk.device, &pl_info, NULL, &pass_layout));

	/* the composite: the view image, gamma */
	push_range.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	push_range.size = sizeof(float);
	pl_info.setLayoutCount = 1;
	pl_info.pSetLayouts = &view_set_layout;
	VK_CHECK (vkCreatePipelineLayout (vk.device, &pl_info, NULL, &composite_layout));

	for (i = 0; i < VK_FRAMES_IN_FLIGHT; i++)
	{
		VK_CreateBuffer (&uniform_buffers[i], sizeof(ViewUniforms),
				 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
				 VK_MEMORY_UPLOAD);
	}
}

void VK_ShutdownView (void)
{
	int	i;

	DestroyViewImage ();
	for (i = 0; i < VK_FRAMES_IN_FLIGHT; i++)
		VK_DestroyBuffer (&uniform_buffers[i]);
	if (debug_pipeline)
		vkDestroyPipeline (vk.device, debug_pipeline, NULL);
	if (composite_pipeline)
		vkDestroyPipeline (vk.device, composite_pipeline, NULL);
	if (pass_layout)
		vkDestroyPipelineLayout (vk.device, pass_layout, NULL);
	if (composite_layout)
		vkDestroyPipelineLayout (vk.device, composite_layout, NULL);
	if (view_pool)
		vkDestroyDescriptorPool (vk.device, view_pool, NULL);
	if (view_set_layout)
		vkDestroyDescriptorSetLayout (vk.device, view_set_layout, NULL);
	debug_pipeline = composite_pipeline = VK_NULL_HANDLE;
	pass_layout = composite_layout = VK_NULL_HANDLE;
	view_pool = VK_NULL_HANDLE;
	view_set_layout = VK_NULL_HANDLE;
	view_set = VK_NULL_HANDLE;
	view_drawn = false;
}
