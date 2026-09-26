/* vk_tonemap.c -- tone mapping and auto exposure: Quake II RTX's
 *
 * Quake II RTX's tone_mapping.c (Eilertsen, Mantiuk and Unger's
 * noise-aware tone mapping with Quake II RTX's changes; its shaders
 * explain it, tone_mapping_histogram.comp first). vk_view.c runs it on the
 * lit image in TAA_OUTPUT (r_debugview 0, tm_enable 1), in place, after
 * the bloom (vk_bloom.c):
 *
 *  - tone_mapping_histogram.comp: a histogram of the image's log
 *    luminance in the tone mapping buffer;
 *  - tone_mapping_curve.comp: the exposure (the adapted luminance) from the
 *    histogram's tm_low_percentile to tm_high_percentile, clamped to
 *    tm_min_luminance..tm_max_luminance and adapting over time
 *    (tm_exposure_speed_up/down), and the tone curve, blended with last
 *    frame's; it clears the histogram;
 *  - tone_mapping_apply.comp: the curve and the exposure (tm_reinhard
 *    blends a Reinhard curve in, tm_exposure_bias), a knee towards white
 *    (tm_knee_start, tm_white_point) and blue noise dither; linear [0, 1]
 *    out, which the composite encodes (view_composite.frag).
 *
 * The curve pass also writes the adapted luminance into this frame's
 * readback buffer (Quake II RTX's ReadbackBuffer; a mapped buffer per frame
 * in flight), which the CPU reads when the frame's slot comes round again:
 * the UBO's prev_adapted_luminance, which scales the effects
 * (path_tracer_hit_shaders.h), and vk_exposure.
 *
 * The exposure starts over (Quake II RTX's vkpt_tone_mapping_request_reset)
 * on a new map, with new pipelines and when the last 3D frame wasn't tone
 * mapped (a debug view, tm_enable 0).
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
#include "shaders/global_ubo.h"
#include "shaders/vertex_buffer.h"

#define SLOPE_WEIGHTS	14	/* tone_mapping_curve.comp's weights[] */

typedef struct
{
	float	reset_curve;		/* 1: start the curve and exposure over */
	float	frame_time;		/* seconds since the last frame */
	float	weights[SLOPE_WEIGHTS];	/* half of the symmetric kernel that blurs the curve's slopes */
} curve_push_t;				/* tone_mapping_curve.comp's PushConstants */

typedef struct
{
	float	knee_w, knee_a, knee_b;
} apply_push_t;				/* tone_mapping_apply.comp's */

static VkPipelineLayout	tm_layout;
static VkPipeline	histogram_pipeline;
static VkPipeline	curve_pipeline;
static VkPipeline	apply_pipeline;		/* SDR: spec_tone_mapping_hdr 0 */

static vk_buffer_t	tonemap_buffer;		/* ToneMappingBuffer */
static vk_buffer_t	readback_buffers[VK_FRAMES_IN_FLIGHT];	/* ReadbackBuffer */

static qboolean		reset_required = true;
static uint32_t		last_frame;		/* vk_render_frame last tone mapped */
static float		prev_adapted_luminance;	/* read back, VK_FRAMES_IN_FLIGHT frames old */

static void CreatePipelines (void)
{
	if (!histogram_pipeline)
	{
		histogram_pipeline = VK_CreateComputePipeline ("tone_mapping_histogram.comp", tm_layout);
		reset_required = true;	/* as Quake II RTX's vkpt_tone_mapping_create_pipelines */
	}
	if (!curve_pipeline)
		curve_pipeline = VK_CreateComputePipeline ("tone_mapping_curve.comp", tm_layout);
	if (!apply_pipeline)
		apply_pipeline = VK_CreateComputePipelineSpec ("tone_mapping_apply.comp", tm_layout, 0);
}

void VK_DestroyToneMapPipelines (void)
{
	if (histogram_pipeline)
		vkDestroyPipeline (vk.device, histogram_pipeline, NULL);
	if (curve_pipeline)
		vkDestroyPipeline (vk.device, curve_pipeline, NULL);
	if (apply_pipeline)
		vkDestroyPipeline (vk.device, apply_pipeline, NULL);
	histogram_pipeline = curve_pipeline = apply_pipeline = VK_NULL_HANDLE;
}

static void BufferBarrier (VkCommandBuffer cmd, VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
			   VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access)
{
	VkBufferMemoryBarrier2	barrier;
	VkDependencyInfo	dep;

	memset (&barrier, 0, sizeof(barrier));
	barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
	barrier.srcStageMask = src_stage;
	barrier.srcAccessMask = src_access;
	barrier.dstStageMask = dst_stage;
	barrier.dstAccessMask = dst_access;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.buffer = tonemap_buffer.buffer;
	barrier.size = VK_WHOLE_SIZE;
	memset (&dep, 0, sizeof(dep));
	dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
	dep.bufferMemoryBarrierCount = 1;
	dep.pBufferMemoryBarriers = &barrier;
	vkCmdPipelineBarrier2 (cmd, &dep);
}

/* the curve pass's write into the readback buffer made visible to the CPU,
 * which reads it after the frame's fence (a fence alone doesn't) */
static void ReadbackBarrier (VkCommandBuffer cmd)
{
	VkMemoryBarrier2	barrier;
	VkDependencyInfo	dep;

	memset (&barrier, 0, sizeof(barrier));
	barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
	barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
	barrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
	barrier.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
	barrier.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
	memset (&dep, 0, sizeof(dep));
	dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
	dep.memoryBarrierCount = 1;
	dep.pMemoryBarriers = &barrier;
	vkCmdPipelineBarrier2 (cmd, &dep);
}

/* the knee of Quake II RTX's vkpt_tone_mapping_record_cmd_buffer: Reinhard
 * y(x) = (w x + a) / (x + b) meeting the identity at tm_knee_start with
 * slope 1 and reaching 1 at tm_white_point */
static void KneeConstants (const QVKUniformBuffer_t *ubo, apply_push_t *push)
{
	float	knee_start = ubo->tm_knee_start;
	float	white_point = ubo->tm_white_point;

	push->knee_w = (knee_start * (knee_start - 2.0f) + white_point) / (white_point - 1.0f);
	push->knee_a = -knee_start * knee_start;
	push->knee_b = push->knee_w - 2.0f * knee_start;
}

/* Quake II RTX's vkpt_tone_mapping_record_cmd_buffer, on TAA_OUTPUT's
 * width x height view; frame_time: seconds since the last frame */
void VK_ToneMap (VkCommandBuffer cmd, uint32_t width, uint32_t height, float frame_time)
{
	const QVKUniformBuffer_t	*ubo = VK_CurrentUBO ();
	VkPipelineLayout		layout = tm_layout;
	curve_push_t			curve;
	apply_push_t			apply;
	float				sigma = ubo->tm_slope_blur_sigma, sum = 0.0f;
	int				i;

	CreatePipelines ();
	if (vk_render_frame != last_frame + 1)
		reset_required = true;	/* the exposure of an older frame */
	last_frame = vk_render_frame;

	if (reset_required)
	{
		BufferBarrier (cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
			       VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
		vkCmdFillBuffer (cmd, tonemap_buffer.buffer, 0, VK_WHOLE_SIZE, 0);
		BufferBarrier (cmd, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
			       VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
	}

	/* the histogram */
	VK_DispatchComputeLayout (cmd, histogram_pipeline, layout, NULL, 0, width, height, 16);
	VK_ComputeBarrier (cmd);

	/* the exposure and the curve: a normalized Gaussian over the slopes,
	 * symmetric, so half of it (Quake II RTX's) */
	memset (&curve, 0, sizeof(curve));
	curve.reset_curve = reset_required ? 1.0f : 0.0f;
	curve.frame_time = frame_time;
	for (i = 0; i < SLOPE_WEIGHTS; i++)
	{
		curve.weights[i] = expf (-(float)(i * i) / (2.0f * sigma * sigma));
		sum += curve.weights[i] * ((i == 0) ? 1.0f : 2.0f);
	}
	for (i = 0; i < SLOPE_WEIGHTS; i++)
		curve.weights[i] /= sum;
	VK_DispatchComputeLayout (cmd, curve_pipeline, layout, &curve, sizeof(curve), 1, 1, 1);
	VK_ComputeBarrier (cmd);
	ReadbackBarrier (cmd);

	/* applied to the image */
	KneeConstants (ubo, &apply);
	VK_DispatchComputeLayout (cmd, apply_pipeline, layout, &apply, sizeof(apply), width, height, 16);
	VK_ComputeBarrier (cmd);

	reset_required = false;
}

/* the exposure starts over (a new map) */
void VK_ResetToneMapping (void)
{
	reset_required = true;
}

VkDeviceAddress VK_ToneMapBufferAddress (void)
{
	return tonemap_buffer.address;
}

/* this frame's readback buffer, after reading what the curve pass wrote into
 * it VK_FRAMES_IN_FLIGHT frames ago (the frame's fence has been waited for):
 * Quake II RTX's process_render_feedback and prev_adapted_luminance, 0.005
 * until the first; Quake II RTX also ignores readbacks of exactly 1 as
 * "mysterious spikes", but 1 is tm_max_luminance's clamp, which a bright
 * scene reaches: kept, and only values that aren't a positive number (a
 * slot never written: 0) are ignored */
VkDeviceAddress VK_ReadbackAddress (float *adapted_luminance)
{
	vk_buffer_t		*b = &readback_buffers[vk.frame_index];
	const ReadbackBuffer	*r = (const ReadbackBuffer *) b->mapped;
	float			l;

	VK_CHECK (vmaInvalidateAllocation (vk.allocator, b->allocation, 0, VK_WHOLE_SIZE));
	l = r->adapted_luminance;
	if (l > 0.0f && l < 1e30f)	/* not 0, NaN or infinite */
		prev_adapted_luminance = l;
	if (!(prev_adapted_luminance > 0.0f))
		prev_adapted_luminance = 0.005f;
	*adapted_luminance = prev_adapted_luminance;
	return b->address;
}

static void VK_Exposure_f (void)
{
	if (!(prev_adapted_luminance > 0.0f))
	{
		Con_Printf ("no adapted luminance yet\n");
		return;
	}
	Con_Printf ("adapted luminance %.6f (EV %.2f), %u frames old; tone mapping %s\n", prev_adapted_luminance,
		    log2f (prev_adapted_luminance), (unsigned) VK_FRAMES_IN_FLIGHT,
		    VK_ToneMappingEnabled () ? "on" : "off (tm_enable 0)");
}

void VK_InitToneMap (void)
{
	int	i;

	tm_layout = VK_CreatePassLayout (VK_SHADER_STAGE_COMPUTE_BIT, sizeof(curve_push_t));
	VK_CreateBuffer (&tonemap_buffer, sizeof(ToneMappingBuffer),
			 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
			 VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_DEVICE);
	for (i = 0; i < VK_FRAMES_IN_FLIGHT; i++)
	{
		VK_CreateBuffer (&readback_buffers[i], sizeof(ReadbackBuffer),
				 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
				 VK_MEMORY_READBACK);
		memset (readback_buffers[i].mapped, 0, sizeof(ReadbackBuffer));
		VK_CHECK (vmaFlushAllocation (vk.allocator, readback_buffers[i].allocation, 0, VK_WHOLE_SIZE));
	}
	reset_required = true;
	prev_adapted_luminance = 0.0f;
	Cmd_AddCommand ("vk_exposure", VK_Exposure_f);
}

void VK_ShutdownToneMap (void)
{
	int	i;

	VK_DestroyToneMapPipelines ();
	for (i = 0; i < VK_FRAMES_IN_FLIGHT; i++)
		VK_DestroyBuffer (&readback_buffers[i]);
	VK_DestroyBuffer (&tonemap_buffer);
	if (tm_layout)
		vkDestroyPipelineLayout (vk.device, tm_layout, NULL);
	tm_layout = VK_NULL_HANDLE;
}
