/* vk_ubo.c -- the global uniform buffer (shaders/global_ubo.h)
 *
 * Quake II RTX's uniform_buffer.c and main.c's prepare_ubo: one buffer
 * per frame in flight, descriptor set 0 of the view passes
 * (vk_pathtracer.c). VK_RenderView3D calls VK_PrepareUBO, which fills the
 * current frame's from r_scene: the camera's matrices and last frame's,
 * the sizes, jitter, TAA mode and FSR constants vk_upscale.c decided,
 * the checkerboard fields' swap, time, the medium the camera is in, the
 * cvars of Quake II RTX's UBO_CVAR_LIST (registered here with its
 * defaults; each does something once the pass that reads it is imported)
 * and the Hexenlicht block: the frame's buffers, the debug view's values
 * and the water's settings (6.5: r_water, r_water_waves, r_water_fog; 6.16:
 * r_water_caustics; registered here); vk_sky.c fills the sky's fields and
 * the sun's (4.6).
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
#include "vk_streamline.h"	/* VK_SL_RR */

/* The C struct must match the shaders' std140 block: when the list
 * changes, compare every member's offsetof with glslang's reflection
 * (glslangValidator -q --reflect-all-block-variables of a shader that
 * includes the UBO) and update these. */
COMPILE_TIME_ASSERT(ubo_tlas, offsetof(QVKUniformBuffer_t, tlas) == 3640);
COMPILE_TIME_ASSERT(ubo_view_cluster, offsetof(QVKUniformBuffer_t, view_cluster) == 3780);
COMPILE_TIME_ASSERT(ubo_sky_dome, offsetof(QVKUniformBuffer_t, sky_dome) == 3792);
COMPILE_TIME_ASSERT(ubo_maplight_gamma, offsetof(QVKUniformBuffer_t, maplight_gamma) == 3816);
COMPILE_TIME_ASSERT(ubo_num_dark_lights, offsetof(QVKUniformBuffer_t, num_dark_lights) == 3820);
COMPILE_TIME_ASSERT(ubo_color_srgb, offsetof(QVKUniformBuffer_t, color_srgb) == 3828);
COMPILE_TIME_ASSERT(ubo_water, offsetof(QVKUniformBuffer_t, water) == 3832);
COMPILE_TIME_ASSERT(ubo_cvars, offsetof(QVKUniformBuffer_t, flt_antilag_hf) == 3852);

#define UBO_SIZE	((sizeof(QVKUniformBuffer_t) + 15) & ~(size_t)15)	/* the std140 block's size */

#define Z_NEAR		4.0f	/* GL's (gl_rmain.c's MYgluPerspective) */
#define Z_FAR		4096.0f

/* Quake II RTX's UBO_CVAR_LIST, registered with its defaults */
#define UBO_CVAR_DO(name, default_value) static cvar_t cvar_##name = { #name, #default_value, CVAR_NONE };
UBO_CVAR_LIST
#undef UBO_CVAR_DO

/* 6.5: the liquids (shaders/water.glsl) */
static cvar_t	r_water = {"r_water", "1", CVAR_NONE};		/* physical water; 0 = as before 6.5 */
static cvar_t	r_water_waves = {"r_water_waves", "1", CVAR_NONE};	/* the waves' slope, 0 = flat */
static cvar_t	r_water_fog = {"r_water_fog", "512", CVAR_NONE};	/* the distance (units) at which the medium is as dense as GL's tint, 0 = clear */
/* 6.16: the strength of the caustic in the light through a liquid's surface
 * (water.glsl's water_caustic: the water texture's pattern, 1 = its own
 * contrast; the weapon's at most 1), 0 = none; 3 (the owner's choice,
 * DECISIONS X29): at 1 it doesn't show */
static cvar_t	r_water_caustics = {"r_water_caustics", "3", CVAR_NONE};

uint32_t		vk_render_frame;	/* 3D frames rendered: the UBO's current_frame_idx */

static QVKUniformBuffer_t	ubo;		/* this frame's after VK_PrepareUBO (VK_CurrentUBO); the last frame's before, for the _prev fields */
static qboolean		ubo_valid;		/* ubo has a frame */
static vk_buffer_t	ubo_buffers[VK_FRAMES_IN_FLIGHT];
static VkDescriptorPool	ubo_pool;
static VkDescriptorSet	ubo_sets[VK_FRAMES_IN_FLIGHT];
VkDescriptorSetLayout	vk_ubo_set_layout;

VkDescriptorSet VK_UBOSet (void)
{
	return ubo_sets[vk.frame_index];
}

/* pt_num_bounce_rays as Quake II RTX's evaluate_reference_mode takes it:
 * 0.5 (half-resolution diffuse), else 0, 1 or 2 */
float VK_NumBounceRays (void)
{
	float	n = cvar_pt_num_bounce_rays.value;

	return (n == 0.5f) ? 0.5f : q_max (0.0f, q_min (2.0f, roundf (n)));
}

/* pt_reflect_refract as Quake II RTX's evaluate_reference_mode takes it:
 * the reflection and refraction passes, 0 to 10 */
int VK_ReflectRefractPasses (void)
{
	return q_min (10, q_max (0, cvar_pt_reflect_refract.integer));
}

const QVKUniformBuffer_t *VK_CurrentUBO (void)
{
	return &ubo;
}

qboolean VK_ToneMappingEnabled (void)
{
	return cvar_tm_enable.integer != 0;
}

/* flt_enable as Quake II RTX's evaluate_reference_mode takes it */
qboolean VK_DenoiserEnabled (void)
{
	return cvar_flt_enable.integer != 0;
}

/* Quake II RTX's temporal_cvar_changed: a change of these leaves the
 * denoiser without history */
static qboolean DenoiserCvarsChanged (void)
{
	static float	last[4];
	float		now[4];

	now[0] = cvar_flt_enable.value;
	now[1] = cvar_flt_temporal_lf.value;
	now[2] = cvar_flt_temporal_hf.value;
	now[3] = cvar_flt_temporal_spec.value;
	if (!memcmp (now, last, sizeof(now)))
		return false;
	memcpy (last, now, sizeof(now));
	return true;
}

/* before a 3D frame's VK_UpscaleEvaluate and VK_PrepareUBO: a change of the
 * denoiser's cvars drops its history (and so the TAA's) */
void VK_CheckDenoiserCvars (void)
{
	if (DenoiserCvarsChanged ())
		VK_ResetDenoiserHistory ();
}

/* the next frame has no last frame: new images (vk_images.c), which the
 * passes would otherwise read at last frame's size */
void VK_ResetUBOHistory (void)
{
	ubo_valid = false;
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

void VK_PrepareUBO (const vk_upscale_t *up, int debug_view)
{
	const vk_effectsframe_t	*ef = VK_EffectsFrame ();
	const vk_modelframe_t	*mf = VK_ModelFrame ();

	if (ubo_valid)
		KeepAsPrevious ();

	VK_CreateViewMatrix (&ubo.V[0][0], r_scene.vieworg, r_scene.forward, r_scene.right, r_scene.up);
	VK_InverseMatrix (&ubo.V[0][0], &ubo.invV[0][0]);
	VK_CreateProjectionMatrix (&ubo.P[0][0], Z_NEAR, Z_FAR, r_scene.fov_x, r_scene.fov_y);
	VK_InverseMatrix (&ubo.P[0][0], &ubo.invP[0][0]);
	VectorCopy (r_scene.vieworg, ubo.cam_pos);
	ubo.cam_pos[3] = 0.0f;

	ubo.current_frame_idx = (int)++vk_render_frame;
	/* the sizes vk_upscale.c decided (Quake II RTX's extent_render,
	 * extent_unscaled, extent_taa_output): rendered at the view times
	 * r_scale, the width even for the two checkerboard fields */
	ubo.width = (int)up->render.width;
	ubo.height = (int)up->render.height;
	ubo.current_gpu_slice_width = ubo.width;
	ubo.inv_width = 1.0f / (float)ubo.width;
	ubo.inv_height = 1.0f / (float)ubo.height;
	ubo.unscaled_width = (int)up->unscaled.width;
	ubo.unscaled_height = (int)up->unscaled.height;
	ubo.screen_image_width = (int)vk_image_extent.width;
	ubo.screen_image_height = (int)vk_image_extent.height;
	ubo.taa_image_width = (int)vk_image_extent.width;
	ubo.taa_image_height = (int)vk_image_extent.height;
	ubo.taa_output_width = (int)up->taa_output.width;
	ubo.taa_output_height = (int)up->taa_output.height;
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
	ubo.tm_exposure_bias += VK_MapExposure ();	/* the map file's r_map_exposure (4.7) */

	/* as Quake II RTX's prepare_ubo in its real-time mode: no depth of field
	 * (only when accumulating a reference image), whole aperture polygon
	 * sides */
	ubo.pt_aperture = 0.0f;
	ubo.pt_aperture_type = roundf (ubo.pt_aperture_type);
	/* the TAA pass and the jitter (vk_upscale.c): flt_taa follows
	 * r_upscaler (off without history); textures sharper when upscaling
	 * (its LOD bias) and FSR's constants */
	ubo.flt_taa = (float)up->taa_mode;
	ubo.sub_pixel_jitter[0] = up->jitter[0];
	ubo.sub_pixel_jitter[1] = up->jitter[1];
	ubo.pt_texture_lod_bias += up->lod_bias;
	memcpy (ubo.easu_const0, up->easu_const[0], sizeof(ubo.easu_const0));
	memcpy (ubo.easu_const1, up->easu_const[1], sizeof(ubo.easu_const1));
	memcpy (ubo.easu_const2, up->easu_const[2], sizeof(ubo.easu_const2));
	memcpy (ubo.easu_const3, up->easu_const[3], sizeof(ubo.easu_const3));
	memcpy (ubo.rcas_const0, up->rcas_const, sizeof(ubo.rcas_const0));
	/* the denoiser (vk_asvgf.c); its temporal filters use no history when
	 * the last frame's images aren't (Quake II RTX's temporal_frame_valid;
	 * VK_CheckDenoiserCvars has dropped it on a cvar change) */
	ubo.flt_enable = up->denoise ? 1.0f : 0.0f;
	/* DLSS RR takes the noisy image: as in Quake II RTX without the
	 * denoiser, every specular ray counts (no specular faked from the
	 * denoiser's spherical harmonics) */
	if (up->dlss == VK_SL_RR)
		ubo.pt_fake_roughness_threshold = 1.0f;
	/* the checkerboard fields swap every frame where the interleave doesn't
	 * blur them (the lit image without the denoiser and without DLSS RR), as
	 * Quake II RTX's without the denoiser: a pixel on a translucent surface
	 * shows the surface and what is behind it in turn, so averaged frames
	 * show the blend (DLSS SR's history, averaged screenshots; each frame
	 * keeps the checkerboard: without the denoiser the TAA pass copies).
	 * The debug views keep one layout: nothing averages them (the motion
	 * check reads last frame's in this layout: misaligned for one frame
	 * after a swapped one) */
	ubo.pt_swap_checkerboard = (debug_view == DEBUGVIEW_LIT && !up->denoise && up->dlss != VK_SL_RR)
				   ? (int)(vk_render_frame & 1) : 0;
	/* as vk_view.c decides (tm_enable 0.5: off); the shaders scale the
	 * effects by the adapted luminance only with the auto exposure (4.9) */
	ubo.tm_enable = (VK_ToneMappingEnabled () && VK_AutoExposure ()) ? 1.0f : 0.0f;
	if (!VK_DenoiserHistoryValid ())
	{
		ubo.flt_temporal_lf = 0.0f;
		ubo.flt_temporal_hf = 0.0f;
		ubo.flt_temporal_spec = 0.0f;
	}
	/* the bounces vk_view.c dispatches (indirect_lighting.rgen); no MIS
	 * with the specular bounce without specular rays */
	ubo.pt_num_bounce_rays = VK_NumBounceRays ();
	ubo.pt_reflect_refract = (float)VK_ReflectRefractPasses ();	/* the passes vk_view.c dispatches */
	if (ubo.pt_num_bounce_rays < 1.0f)
		ubo.pt_specular_mis = 0.0f;

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
	VK_PrepareLights (&ubo);	/* light_buffer, the sphere lights, num_static_lights */
	VK_PrepareSky (&ubo);		/* the sky's textures, the dome, the sun, pt_env_scale */
	/* the tone mapper's buffers (vk_tonemap.c), the adapted luminance read
	 * back and the bloom's intensity (vk_bloom.c; Quake II RTX's
	 * vkpt_bloom_update without its under-water and menu variants) */
	ubo.tonemap = VK_ToneMapBufferAddress ();
	ubo.readback = VK_ReadbackAddress (&ubo.prev_adapted_luminance);
	ubo.bloom_intensity = VK_BloomIntensity ();
	ubo.particle_texture = (uint32_t)VK_ParticleTexture ();
	ubo.anim_frame = (int)(r_scene.time * 5.0);	/* R_TextureAnimation's frame */
	ubo.debug_view = (uint32_t)debug_view;
	ubo.view_cluster = r_scene.viewleaf ? (int)(r_scene.viewleaf - r_scene.worldmodel->leafs) - 1 : -1;
	ubo.color_srgb = VK_ColorsSRGB () ? 1u : 0u;	/* the 8-bit colors' curve (4.17, transfer.glsl) */
	ubo.water = (r_water.integer != 0);
	ubo.water_waves = q_max (0.0f, r_water_waves.value);
	ubo.water_fog = q_max (0.0f, r_water_fog.value);
	ubo.water_caustics = q_max (0.0f, r_water_caustics.value);
	ubo.water_light = r_scene.water_light;	/* GL's light level around the camera, eased (6.17, r_light.c) */
	/* 6.11: the view entity's own model (vk_instance.c's viewer group), in
	 * the rays as its look (Quake II RTX's field) */
	ubo.first_person_model = (mf->viewer_instance < 0) ? VIEWER_MODEL_NONE :
				 (mf->viewer_look == MODEL_GROUP_TRANSPARENT) ? VIEWER_MODEL_TRANSLUCENT : VIEWER_MODEL_OPAQUE;

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
	Cvar_RegisterVariable (&r_water);
	Cvar_RegisterVariable (&r_water_waves);
	Cvar_RegisterVariable (&r_water_fog);
	Cvar_RegisterVariable (&r_water_caustics);

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
