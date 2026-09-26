/* vk_ubo.c -- the global uniform buffer (shaders/global_ubo.h)
 *
 * Quake II RTX's uniform_buffer.c and main.c's prepare_ubo: one buffer
 * per frame in flight, descriptor set 0 of the view passes
 * (vk_pathtracer.c). VK_RenderView3D calls VK_PrepareUBO, which fills the
 * current frame's from r_scene: the camera's matrices and last frame's,
 * the render size, time, the medium the camera is in, the cvars of
 * Quake II RTX's UBO_CVAR_LIST (registered here with its defaults; each
 * does something once the pass that reads it is imported) and the
 * Hexenlicht block: the frame's buffers and the debug view's values.
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
#include "r_scene.h"
#include "shaders/hl_shared.h"

/* The C struct must match the shaders' std140 block: when the list
 * changes, compare every member's offsetof with glslang's reflection
 * (glslangValidator -q --reflect-all-block-variables of a shader that
 * includes the UBO) and update these. */
COMPILE_TIME_ASSERT(ubo_tlas, offsetof(QVKUniformBuffer_t, tlas) == 3640);
COMPILE_TIME_ASSERT(ubo_view_cluster, offsetof(QVKUniformBuffer_t, view_cluster) == 3732);
COMPILE_TIME_ASSERT(ubo_cvars, offsetof(QVKUniformBuffer_t, flt_antilag_hf) == 3736);

#define UBO_SIZE	((sizeof(QVKUniformBuffer_t) + 15) & ~(size_t)15)	/* the std140 block's size */

#define Z_NEAR		4.0f	/* GL's (gl_rmain.c's MYgluPerspective) */
#define Z_FAR		4096.0f

/* Quake II RTX's UBO_CVAR_LIST, registered with its defaults */
#define UBO_CVAR_DO(name, default_value) static cvar_t cvar_##name = { #name, #default_value, CVAR_NONE };
UBO_CVAR_LIST
#undef UBO_CVAR_DO

uint32_t		vk_render_frame;	/* 3D frames rendered: the UBO's current_frame_idx */

static QVKUniformBuffer_t	ubo;		/* the last frame's, for the _prev fields */
static qboolean		ubo_valid;		/* ubo has a frame */
static vk_buffer_t	ubo_buffers[VK_FRAMES_IN_FLIGHT];
static VkDescriptorPool	ubo_pool;
static VkDescriptorSet	ubo_sets[VK_FRAMES_IN_FLIGHT];
VkDescriptorSetLayout	vk_ubo_set_layout;

VkDescriptorSet VK_UBOSet (void)
{
	return ubo_sets[vk.frame_index];
}

/* the camera and size become last frame's, as Quake II RTX's prepare_ubo keeps them */
static void KeepAsPrevious (void)
{
	memcpy (ubo.V_prev, ubo.V, sizeof(ubo.V));
	memcpy (ubo.P_prev, ubo.P, sizeof(ubo.P));
	memcpy (ubo.invP_prev, ubo.invP, sizeof(ubo.invP));
	ubo.prev_width = ubo.width;
	ubo.prev_height = ubo.height;
	ubo.prev_taa_output_width = ubo.taa_output_width;
	ubo.prev_taa_output_height = ubo.taa_output_height;
}

void VK_PrepareUBO (uint32_t width, uint32_t height, int debug_view)
{
	const vk_effectsframe_t	*ef = VK_EffectsFrame ();

	if (ubo_valid)
		KeepAsPrevious ();

	VK_CreateViewMatrix (&ubo.V[0][0], r_scene.vieworg, r_scene.forward, r_scene.right, r_scene.up);
	VK_InverseMatrix (&ubo.V[0][0], &ubo.invV[0][0]);
	VK_CreateProjectionMatrix (&ubo.P[0][0], Z_NEAR, Z_FAR, r_scene.fov_x, r_scene.fov_y);
	VK_InverseMatrix (&ubo.P[0][0], &ubo.invP[0][0]);
	VectorCopy (r_scene.vieworg, ubo.cam_pos);
	ubo.cam_pos[3] = 0.0f;

	ubo.current_frame_idx = (int)++vk_render_frame;
	ubo.width = (int)width;
	ubo.height = (int)height;
	ubo.current_gpu_slice_width = (int)width;
	ubo.inv_width = 1.0f / (float)width;
	ubo.inv_height = 1.0f / (float)height;
	ubo.unscaled_width = (int)width;	/* no resolution scale until 3.8 */
	ubo.unscaled_height = (int)height;
	ubo.screen_image_width = (int)vk_image_extent.width;
	ubo.screen_image_height = (int)vk_image_extent.height;
	ubo.taa_image_width = (int)vk_image_extent.width;
	ubo.taa_image_height = (int)vk_image_extent.height;
	ubo.taa_output_width = (int)width;
	ubo.taa_output_height = (int)height;
	if (!ubo_valid)
		KeepAsPrevious ();	/* the first frame: no last frame */
	ubo.pt_projection = PROJECTION_RECTILINEAR;
	ubo.time = (float)r_scene.time;
	ubo.ui_color_scale = 1.0f;

	switch (r_scene.viewcontents)
	{
	case CONTENTS_WATER:	ubo.medium = MEDIUM_WATER; break;
	case CONTENTS_SLIME:	ubo.medium = MEDIUM_SLIME; break;
	case CONTENTS_LAVA:	ubo.medium = MEDIUM_LAVA; break;
	default:		ubo.medium = MEDIUM_NONE; break;
	}

#define UBO_CVAR_DO(name, default_value) ubo.name = cvar_##name.value;
	UBO_CVAR_LIST
#undef UBO_CVAR_DO

	/* Hexenlicht */
	ubo.tlas = VK_TLASAddress ();
	ubo.effects_tlas = VK_EffectsTLASAddress ();
	ubo.tlas_info = VK_TLASInfoAddress ();
	ubo.instances = VK_InstanceBuffer ()->address;
	ubo.world_primitives = vk_world.buffer.address;
	ubo.instanced_primitives = VK_InstancedBuffer ()->address;
	ubo.materials = vk_material_table.address;
	ubo.pvs = vk_pvs.buffer.address;
	ubo.particles = ef->particles;
	ubo.sprites = ef->sprites;
	ubo.particle_texture = (uint32_t)VK_ParticleTexture ();
	ubo.anim_frame = (int)(r_scene.time * 5.0);	/* R_TextureAnimation's frame */
	ubo.debug_view = (uint32_t)debug_view;
	ubo.view_cluster = r_scene.viewleaf ? (int)(r_scene.viewleaf - r_scene.worldmodel->leafs) - 1 : -1;

	ubo_valid = true;
	memcpy (ubo_buffers[vk.frame_index].mapped, &ubo, sizeof(ubo));
	VK_CHECK (vmaFlushAllocation (vk.allocator, ubo_buffers[vk.frame_index].allocation, 0, UBO_SIZE));
}

void VK_InitUBO (void)
{
	VkDescriptorSetLayoutBinding	binding;
	VkDescriptorSetLayoutCreateInfo	layout_info;
	VkDescriptorPoolSize		pool_size;
	VkDescriptorPoolCreateInfo	pool_info;
	VkDescriptorSetAllocateInfo	alloc_info;
	VkDescriptorBufferInfo		buffer_info;
	VkWriteDescriptorSet		write;
	VkDescriptorSetLayout		layouts[VK_FRAMES_IN_FLIGHT];
	int				i;

#define UBO_CVAR_DO(name, default_value) Cvar_RegisterVariable (&cvar_##name);
	UBO_CVAR_LIST
#undef UBO_CVAR_DO

	memset (&binding, 0, sizeof(binding));
	binding.binding = GLOBAL_UBO_BINDING_IDX;
	binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	binding.descriptorCount = 1;
	binding.stageFlags = VK_SHADER_STAGE_ALL;
	memset (&layout_info, 0, sizeof(layout_info));
	layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	layout_info.bindingCount = 1;
	layout_info.pBindings = &binding;
	VK_CHECK (vkCreateDescriptorSetLayout (vk.device, &layout_info, NULL, &vk_ubo_set_layout));

	pool_size.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	pool_size.descriptorCount = VK_FRAMES_IN_FLIGHT;
	memset (&pool_info, 0, sizeof(pool_info));
	pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	pool_info.maxSets = VK_FRAMES_IN_FLIGHT;
	pool_info.poolSizeCount = 1;
	pool_info.pPoolSizes = &pool_size;
	VK_CHECK (vkCreateDescriptorPool (vk.device, &pool_info, NULL, &ubo_pool));

	for (i = 0; i < VK_FRAMES_IN_FLIGHT; i++)
		layouts[i] = vk_ubo_set_layout;
	memset (&alloc_info, 0, sizeof(alloc_info));
	alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	alloc_info.descriptorPool = ubo_pool;
	alloc_info.descriptorSetCount = VK_FRAMES_IN_FLIGHT;
	alloc_info.pSetLayouts = layouts;
	VK_CHECK (vkAllocateDescriptorSets (vk.device, &alloc_info, ubo_sets));

	for (i = 0; i < VK_FRAMES_IN_FLIGHT; i++)
	{
		VK_CreateBuffer (&ubo_buffers[i], UBO_SIZE, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VK_MEMORY_UPLOAD);
		memset (ubo_buffers[i].mapped, 0, UBO_SIZE);

		buffer_info.buffer = ubo_buffers[i].buffer;
		buffer_info.offset = 0;
		buffer_info.range = UBO_SIZE;
		memset (&write, 0, sizeof(write));
		write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		write.dstSet = ubo_sets[i];
		write.dstBinding = GLOBAL_UBO_BINDING_IDX;
		write.descriptorCount = 1;
		write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		write.pBufferInfo = &buffer_info;
		vkUpdateDescriptorSets (vk.device, 1, &write, 0, NULL);
	}
}

void VK_ShutdownUBO (void)
{
	int	i;

	for (i = 0; i < VK_FRAMES_IN_FLIGHT; i++)
		VK_DestroyBuffer (&ubo_buffers[i]);
	if (ubo_pool)
		vkDestroyDescriptorPool (vk.device, ubo_pool, NULL);
	if (vk_ubo_set_layout)
		vkDestroyDescriptorSetLayout (vk.device, vk_ubo_set_layout, NULL);
	ubo_pool = VK_NULL_HANDLE;
	vk_ubo_set_layout = VK_NULL_HANDLE;
	memset (ubo_sets, 0, sizeof(ubo_sets));
	memset (&ubo, 0, sizeof(ubo));
	ubo_valid = false;
}
