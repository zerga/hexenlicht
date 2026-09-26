/* vk_pathtracer.c -- the view passes' framework
 *
 * From Quake II RTX's path_tracer.c, ray query mode only (its .rgen
 * shaders are compiled as compute shaders, and so are ours): the passes'
 * pipeline layouts, compute pipelines and dispatch, and image barriers.
 * Every view pass binds three descriptor sets, Quake II RTX's set numbers
 * of its compute passes:
 *   set 0  the global UBO of the frame (vk_ubo.c, shaders/global_ubo.h)
 *   set 1  the render-target images, even or odd (vk_images.c,
 *          shaders/global_textures.h)
 *   set 2  vk_texture.c's bindless textures
 * The frame's buffers are read by device address from the UBO. Each
 * module creates its pipeline layouts with its own push constants
 * (VK_CreatePassLayout); the path tracer's passes share one with Quake II
 * RTX's pt push constants (VK_PathTracerLayout). A shader can be made
 * into several pipelines by its specialization constant 0
 * (VK_CreateComputePipelineSpec), as Quake II RTX's bounces.
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

COMPILE_TIME_ASSERT(pt_textures_set, GLOBAL_TEXTURES_TEX_ARR_DESC_SET_IDX == 2);

static VkPipelineLayout	pt_layout;

VkPipelineLayout VK_CreatePassLayout (VkShaderStageFlags push_stages, uint32_t push_size)
{
	VkDescriptorSetLayout		sets[3];
	VkPushConstantRange		push_range;
	VkPipelineLayoutCreateInfo	info;
	VkPipelineLayout		layout;

	sets[0] = vk_ubo_set_layout;
	sets[1] = vk_images_set_layout;
	sets[2] = vk.texture_set_layout;

	memset (&push_range, 0, sizeof(push_range));
	push_range.stageFlags = push_stages;
	push_range.size = push_size;
	memset (&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	info.setLayoutCount = 3;
	info.pSetLayouts = sets;
	info.pushConstantRangeCount = push_size ? 1 : 0;
	info.pPushConstantRanges = &push_range;
	VK_CHECK (vkCreatePipelineLayout (vk.device, &info, NULL, &layout));
	return layout;
}

VkPipelineLayout VK_PathTracerLayout (void)
{
	return pt_layout;
}

/* with specialization constants 0 to count - 1 (constant_id) set to values */
static VkPipeline CreateComputePipeline (const char *shader, VkPipelineLayout layout, const uint32_t *values, int count)
{
	VkComputePipelineCreateInfo	info;
	VkSpecializationMapEntry	entries[4];
	VkSpecializationInfo		spec;
	VkShaderModule			module = VK_LoadShader (shader);
	VkPipeline			pipeline;
	int				i;

	memset (&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
	info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	info.stage.module = module;
	info.stage.pName = "main";
	info.layout = layout;
	if (count > 0)
	{
		count = q_min (count, (int)Q_COUNTOF(entries));
		for (i = 0; i < count; i++)
		{
			entries[i].constantID = (uint32_t)i;
			entries[i].offset = (uint32_t)(i * sizeof(uint32_t));
			entries[i].size = sizeof(uint32_t);
		}
		spec.mapEntryCount = (uint32_t)count;
		spec.pMapEntries = entries;
		spec.dataSize = count * sizeof(uint32_t);
		spec.pData = values;
		info.stage.pSpecializationInfo = &spec;
	}
	VK_CHECK (vkCreateComputePipelines (vk.device, VK_NULL_HANDLE, 1, &info, NULL, &pipeline));
	vkDestroyShaderModule (vk.device, module, NULL);
	return pipeline;
}

VkPipeline VK_CreateComputePipeline (const char *shader, VkPipelineLayout layout)
{
	return CreateComputePipeline (shader, layout, NULL, 0);
}

/* with the shader's specialization constant 0 (constant_id = 0) set to value,
 * as Quake II RTX's pipelines that share a shader */
VkPipeline VK_CreateComputePipelineSpec (const char *shader, VkPipelineLayout layout, uint32_t value)
{
	return CreateComputePipeline (shader, layout, &value, 1);
}

/* with its specialization constants 0 to count - 1 (at most 4) set to values */
VkPipeline VK_CreateComputePipelineSpecs (const char *shader, VkPipelineLayout layout, const uint32_t *values, int count)
{
	return CreateComputePipeline (shader, layout, values, count);
}

/* the frame's UBO, images and textures */
void VK_BindPassSets (VkCommandBuffer cmd, VkPipelineBindPoint bind_point, VkPipelineLayout layout)
{
	VkDescriptorSet	sets[3];

	sets[0] = VK_UBOSet ();
	sets[1] = VK_ImagesSet ();
	sets[2] = vk.texture_set;
	vkCmdBindDescriptorSets (cmd, bind_point, layout, 0, 3, sets, 0, NULL);
}

/* Quake II RTX's dispatch_rays in ray query mode: an 8x8 workgroup per
 * tile of the width x height x depth launch (rt_LaunchID) */
void VK_DispatchRays (VkCommandBuffer cmd, VkPipeline pipeline, const pt_push_constants_t *push,
		      uint32_t width, uint32_t height, uint32_t depth)
{
	vkCmdBindPipeline (cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
	VK_BindPassSets (cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pt_layout);
	vkCmdPushConstants (cmd, pt_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(*push), push);
	vkCmdDispatch (cmd, (width + 7) / 8, (height + 7) / 8, depth);
}

/* a compute pass of local_size x local_size workgroups over width x height,
 * with the path tracer's layout (compositing.comp and
 * checkerboard_interleave.comp: 16, as Quake II RTX dispatches them) */
void VK_DispatchCompute (VkCommandBuffer cmd, VkPipeline pipeline, uint32_t width, uint32_t height, uint32_t local_size)
{
	vkCmdBindPipeline (cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
	VK_BindPassSets (cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pt_layout);
	vkCmdDispatch (cmd, (width + local_size - 1) / local_size, (height + local_size - 1) / local_size, 1);
}

/* the same with a module's own layout and push constants (push_size bytes
 * from offset 0; none if push is NULL) */
void VK_DispatchComputeLayout (VkCommandBuffer cmd, VkPipeline pipeline, VkPipelineLayout layout, const void *push,
			       uint32_t push_size, uint32_t width, uint32_t height, uint32_t local_size)
{
	vkCmdBindPipeline (cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
	VK_BindPassSets (cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout);
	if (push)
		vkCmdPushConstants (cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, push_size, push);
	vkCmdDispatch (cmd, (width + local_size - 1) / local_size, (height + local_size - 1) / local_size, 1);
}

/* a barrier on a render target (they stay in the GENERAL layout) */
void VK_RenderTargetBarrier (VkCommandBuffer cmd, VkImage image,
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
	barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
	barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = image;
	barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barrier.subresourceRange.levelCount = 1;
	barrier.subresourceRange.layerCount = 1;
	memset (&dep, 0, sizeof(dep));
	dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
	dep.imageMemoryBarrierCount = 1;
	dep.pImageMemoryBarriers = &barrier;
	vkCmdPipelineBarrier2 (cmd, &dep);
}

/* everything the compute shaders wrote before is visible to the compute
 * shaders after, and what they read before is not overwritten early: between
 * the view passes, which share the render targets (Quake II RTX's
 * BARRIER_COMPUTE on each image) */
void VK_ComputeBarrier (VkCommandBuffer cmd)
{
	VkMemoryBarrier2	barrier;
	VkDependencyInfo	dep;

	memset (&barrier, 0, sizeof(barrier));
	barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
	barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
	barrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
	barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
	barrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT |
				VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
	memset (&dep, 0, sizeof(dep));
	dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
	dep.memoryBarrierCount = 1;
	dep.pMemoryBarriers = &barrier;
	vkCmdPipelineBarrier2 (cmd, &dep);
}

void VK_InitPathTracer (void)
{
	pt_layout = VK_CreatePassLayout (VK_SHADER_STAGE_COMPUTE_BIT, sizeof(pt_push_constants_t));
}

void VK_ShutdownPathTracer (void)
{
	if (pt_layout)
		vkDestroyPipelineLayout (vk.device, pt_layout, NULL);
	pt_layout = VK_NULL_HANDLE;
}
