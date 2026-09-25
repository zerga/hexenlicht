/* vk_accel.c -- acceleration structures for ray tracing
 *
 * On map load, VK_BuildWorldAccel builds a static bottom-level structure
 * (BLAS) for each non-empty primitive range of the world and of every
 * brush submodel (vk_world.c: opaque, transparent, sky), from the world
 * buffer's packed positions. Every frame, VK_BuildTLAS rebuilds the top
 * level (TLAS) in the frame's command buffer: the world's BLASes and one
 * instance of a submodel's BLASes per brush entity (vk_instance.c), with
 * Quake II RTX's instance masks. Each TLAS instance has a TlasInstanceInfo
 * (shaders/hl_shared.h) so shaders can find the primitive they hit.
 *
 * vk_rtcheck casts a grid of rays from the camera through the last TLAS
 * (rt_check.comp) and compares the hits with the engine's own collision
 * traces: the world's hull and each brush entity's, rotated the way the
 * server does it (world.c's SV_ClipMoveToEntity).
 *
 * The structure follows Quake II RTX's path_tracer.c.
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

#define AS_OFFSET_ALIGNMENT	256	/* of acceleration structures in their buffer */

enum { RANGE_OPAQUE, RANGE_TRANSPARENT, RANGE_SKY, NUM_RANGES };
static const char *const range_names[NUM_RANGES] = { "opaque", "transparent", "sky" };
static const uint32_t range_masks[NUM_RANGES] = { AS_FLAG_OPAQUE, AS_FLAG_TRANSPARENT, AS_FLAG_SKY };

typedef struct
{
	VkAccelerationStructureKHR	as;		/* VK_NULL_HANDLE: empty range */
	VkDeviceAddress			address;
	uint32_t			first, count;	/* primitives in the world buffer */
} vk_blas_t;

/* static BLASes: [vk_world.num_models][NUM_RANGES], model 0 is the world */
static vk_blas_t	*blases;
static int		num_blases;
static vk_buffer_t	blas_buffer;
static VkDeviceSize	blas_scratch_size;

typedef struct
{
	vk_buffer_t			instances;	/* VkAccelerationStructureInstanceKHR[], mapped */
	vk_buffer_t			info;		/* TlasInstanceInfo[], mapped */
	vk_buffer_t			buffer;
	vk_buffer_t			scratch;
	VkDeviceAddress			scratch_address;	/* aligned */
	VkAccelerationStructureKHR	as;
	VkDeviceAddress			address;
	uint32_t			num_instances;
	qboolean			timed;		/* has timestamps to read */
} vk_tlas_t;

static vk_tlas_t	tlas[VK_FRAMES_IN_FLIGHT];
static int		last_tlas = -1;		/* the slot built last, -1 = none since the map loaded */
static uint32_t		scratch_alignment;
static VkQueryPool	query_pool;		/* 2 timestamps per slot */
static double		tlas_build_ms;

/* the last TLAS's instances: range and model instance, for vk_rtcheck */
static struct
{
	int	range;
	int	model_instance;
} tlas_sources[MAX_TLAS_INSTANCES];


static VkDeviceSize AlignSize (VkDeviceSize size, VkDeviceSize alignment)
{
	return (size + alignment - 1) & ~(alignment - 1);
}

static const vk_primrange_t *ModelRange (int model, int range)
{
	const vk_bspmodel_t	*m = &vk_world.models[model];

	return (range == RANGE_OPAQUE) ? &m->opaque : (range == RANGE_TRANSPARENT) ? &m->transparent : &m->sky;
}

static void AccelBarrier (VkCommandBuffer cmd)
{
	VkMemoryBarrier2	barrier;
	VkDependencyInfo	dep;

	memset (&barrier, 0, sizeof(barrier));
	barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
	barrier.srcStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
	barrier.srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
	barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
	barrier.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
	memset (&dep, 0, sizeof(dep));
	dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
	dep.memoryBarrierCount = 1;
	dep.pMemoryBarriers = &barrier;
	vkCmdPipelineBarrier2 (cmd, &dep);
}

static VkAccelerationStructureKHR CreateAS (VkAccelerationStructureTypeKHR type, vk_buffer_t *buffer,
					    VkDeviceSize offset, VkDeviceSize size, VkDeviceAddress *address)
{
	VkAccelerationStructureCreateInfoKHR		info;
	VkAccelerationStructureDeviceAddressInfoKHR	address_info;
	VkAccelerationStructureKHR			as;

	memset (&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
	info.buffer = buffer->buffer;
	info.offset = offset;
	info.size = size;
	info.type = type;
	VK_CHECK (vkCreateAccelerationStructureKHR (vk.device, &info, NULL, &as));

	memset (&address_info, 0, sizeof(address_info));
	address_info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
	address_info.accelerationStructure = as;
	*address = vkGetAccelerationStructureDeviceAddressKHR (vk.device, &address_info);
	return as;
}


/* ==========================================================================
 * Static BLASes, on map load
 * ========================================================================== */

void VK_FreeWorldAccel (void)
{
	int	i;

	if (vk.device)
	{
		for (i = 0; i < num_blases; i++)
		{
			if (blases[i].as)
				vkDestroyAccelerationStructureKHR (vk.device, blases[i].as, NULL);
		}
		VK_DestroyBuffer (&blas_buffer);
	}
	free (blases);
	blases = NULL;
	num_blases = 0;
	blas_scratch_size = 0;
	last_tlas = -1;		/* built from the old BLASes */
}

void VK_BuildWorldAccel (void)
{
	VkAccelerationStructureGeometryKHR		*geoms;
	VkAccelerationStructureBuildGeometryInfoKHR	*infos;
	VkAccelerationStructureBuildRangeInfoKHR	*ranges;
	const VkAccelerationStructureBuildRangeInfoKHR	**range_ptrs;
	VkAccelerationStructureBuildSizesInfoKHR	size;
	VkDeviceSize	*as_offsets, *as_sizes, *scratch_offsets, total = 0, scratch_total = 0;
	int		*blas_index, i, k, count = 0;
	vk_buffer_t	scratch;
	VkDeviceAddress	scratch_address;
	VkCommandBuffer	cmd;

	VK_FreeWorldAccel ();
	if (!vk_world.num_primitives)
		return;

	num_blases = vk_world.num_models * NUM_RANGES;
	blases = (vk_blas_t *) calloc (num_blases, sizeof(vk_blas_t));
	geoms = (VkAccelerationStructureGeometryKHR *) calloc (num_blases, sizeof(*geoms));
	infos = (VkAccelerationStructureBuildGeometryInfoKHR *) calloc (num_blases, sizeof(*infos));
	ranges = (VkAccelerationStructureBuildRangeInfoKHR *) calloc (num_blases, sizeof(*ranges));
	range_ptrs = (const VkAccelerationStructureBuildRangeInfoKHR **) calloc (num_blases, sizeof(*range_ptrs));
	as_offsets = (VkDeviceSize *) calloc (num_blases, sizeof(VkDeviceSize));
	as_sizes = (VkDeviceSize *) calloc (num_blases, sizeof(VkDeviceSize));
	scratch_offsets = (VkDeviceSize *) calloc (num_blases, sizeof(VkDeviceSize));
	blas_index = (int *) calloc (num_blases, sizeof(int));
	if (!blases || !geoms || !infos || !ranges || !range_ptrs || !as_offsets || !as_sizes || !scratch_offsets || !blas_index)
		Sys_Error ("%s: out of memory", __thisfunc__);

	/* one triangle geometry per non-empty range, straight from the packed positions */
	for (i = 0; i < num_blases; i++)
	{
		const vk_primrange_t				*r = ModelRange (i / NUM_RANGES, i % NUM_RANGES);
		VkAccelerationStructureGeometryTrianglesDataKHR	*tri;

		if (!r->count)
			continue;
		blases[i].first = r->first;
		blases[i].count = r->count;

		geoms[count].sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
		geoms[count].geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
		geoms[count].flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
		tri = &geoms[count].geometry.triangles;
		tri->sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
		tri->vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
		tri->vertexData.deviceAddress = vk_world.buffer.address + vk_world.positions_offset +
						(VkDeviceSize)r->first * 9 * sizeof(float);
		tri->vertexStride = 3 * sizeof(float);
		tri->maxVertex = r->count * 3 - 1;
		tri->indexType = VK_INDEX_TYPE_NONE_KHR;

		infos[count].sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
		infos[count].type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
		infos[count].flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
		infos[count].mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
		infos[count].geometryCount = 1;
		infos[count].pGeometries = &geoms[count];

		memset (&size, 0, sizeof(size));
		size.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
		vkGetAccelerationStructureBuildSizesKHR (vk.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
							 &infos[count], &r->count, &size);
		as_offsets[count] = total;
		as_sizes[count] = size.accelerationStructureSize;
		total += AlignSize (size.accelerationStructureSize, AS_OFFSET_ALIGNMENT);
		scratch_offsets[count] = scratch_total;
		scratch_total += AlignSize (size.buildScratchSize, scratch_alignment);

		ranges[count].primitiveCount = r->count;
		range_ptrs[count] = &ranges[count];
		blas_index[count] = i;
		count++;
	}

	VK_CreateBuffer (&blas_buffer, total, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
			 VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VK_MEMORY_DEVICE);
	VK_CreateBuffer (&scratch, scratch_total + scratch_alignment, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
			 VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VK_MEMORY_DEVICE);
	scratch_address = AlignSize (scratch.address, scratch_alignment);
	blas_scratch_size = scratch_total;

	for (k = 0; k < count; k++)
	{
		vk_blas_t	*b = &blases[blas_index[k]];

		b->as = CreateAS (VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, &blas_buffer,
				  as_offsets[k], as_sizes[k], &b->address);
		infos[k].dstAccelerationStructure = b->as;
		infos[k].scratchData.deviceAddress = scratch_address + scratch_offsets[k];
	}

	cmd = VK_BeginUpload ();
	vkCmdBuildAccelerationStructuresKHR (cmd, (uint32_t)count, infos, range_ptrs);
	AccelBarrier (cmd);
	VK_EndUpload ();

	VK_DestroyBuffer (&scratch);
	free (geoms);
	free (infos);
	free (ranges);
	free ((void *)range_ptrs);
	free (as_offsets);
	free (as_sizes);
	free (scratch_offsets);
	free (blas_index);
}


/* ==========================================================================
 * The TLAS, every frame
 * ========================================================================== */

static void AddTLASInstance (vk_tlas_t *t, const mat4 transform, const vk_blas_t *b, uint32_t mask,
			     int range, int model_instance)
{
	VkAccelerationStructureInstanceKHR	*ai;
	TlasInstanceInfo			*info;
	int					r, c;

	if (t->num_instances >= MAX_TLAS_INSTANCES)
		return;

	ai = (VkAccelerationStructureInstanceKHR *) t->instances.mapped + t->num_instances;
	memset (ai, 0, sizeof(*ai));
	for (r = 0; r < 3; r++)
	{
		for (c = 0; c < 4; c++)		/* row-major 3x4 from our column-major 4x4 */
			ai->transform.matrix[r][c] = transform ? transform[c][r] : (r == c ? 1.0f : 0.0f);
	}
	ai->instanceCustomIndex = VERTEX_BUFFER_WORLD;
	ai->mask = mask;
	ai->instanceShaderBindingTableRecordOffset = 0;
	ai->flags = VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR;
	ai->accelerationStructureReference = b->address;

	info = (TlasInstanceInfo *) t->info.mapped + t->num_instances;
	info->prim_offset = b->first;
	info->model_instance = model_instance;

	tlas_sources[t->num_instances].range = range;
	tlas_sources[t->num_instances].model_instance = model_instance;
	t->num_instances++;
}

void VK_BuildTLAS (void)
{
	vk_tlas_t					*t;
	VkCommandBuffer					cmd;
	VkAccelerationStructureGeometryKHR		geom;
	VkAccelerationStructureBuildGeometryInfoKHR	info;
	VkAccelerationStructureBuildRangeInfoKHR	range;
	const VkAccelerationStructureBuildRangeInfoKHR	*range_ptr = &range;
	int						slot, i, r;

	if (!vk.frame_active || !blases)
		return;

	slot = (int)vk.frame_index;
	t = &tlas[slot];
	cmd = vk.frames[slot].cmd;

	/* this slot's fence was waited for, so its last build's timestamps are ready */
	if (query_pool && t->timed)
	{
		uint64_t	ts[2];

		if (vkGetQueryPoolResults (vk.device, query_pool, slot * 2, 2, sizeof(ts), ts, sizeof(uint64_t),
					   VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
			tlas_build_ms = (double)(ts[1] - ts[0]) * vk.props.limits.timestampPeriod / 1.0e6;
	}

	t->num_instances = 0;
	for (r = 0; r < NUM_RANGES; r++)
	{
		if (blases[r].as)
			AddTLASInstance (t, NULL, &blases[r], range_masks[r], r, -1);
	}
	for (i = 0; i < VK_NumInstances (); i++)
	{
		const ModelInstance	*mi = VK_GetInstance (i);
		int			sub = VK_InstanceSubmodel (i);
		qboolean		translucent = (mi->alpha_and_frame & 0xffff) != VK_FloatToHalf (1.0f);

		if (sub <= 0 || sub >= vk_world.num_models)
			continue;
		for (r = 0; r < NUM_RANGES; r++)
		{
			const vk_blas_t	*b = &blases[sub * NUM_RANGES + r];

			if (b->as)
				AddTLASInstance (t, mi->transform, b, translucent ? AS_FLAG_TRANSPARENT : range_masks[r], r, i);
		}
	}
	VK_CHECK (vmaFlushAllocation (vk.allocator, t->instances.allocation, 0, VK_WHOLE_SIZE));
	VK_CHECK (vmaFlushAllocation (vk.allocator, t->info.allocation, 0, VK_WHOLE_SIZE));

	memset (&geom, 0, sizeof(geom));
	geom.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
	geom.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
	geom.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
	geom.geometry.instances.data.deviceAddress = t->instances.address;
	memset (&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
	info.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
	info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
	info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
	info.dstAccelerationStructure = t->as;
	info.geometryCount = 1;
	info.pGeometries = &geom;
	info.scratchData.deviceAddress = t->scratch_address;
	memset (&range, 0, sizeof(range));
	range.primitiveCount = t->num_instances;

	if (query_pool)
	{
		vkCmdResetQueryPool (cmd, query_pool, slot * 2, 2);
		vkCmdWriteTimestamp2 (cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, query_pool, slot * 2);
	}
	vkCmdBuildAccelerationStructuresKHR (cmd, 1, &info, &range_ptr);
	AccelBarrier (cmd);
	if (query_pool)
	{
		vkCmdWriteTimestamp2 (cmd, VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, query_pool, slot * 2 + 1);
		t->timed = true;
	}
	last_tlas = slot;
}

VkDeviceAddress VK_TLASAddress (void)
{
	return tlas[vk.frame_index].address;
}


/* ==========================================================================
 * vk_accel and vk_rtcheck
 * ========================================================================== */

static void VK_Accel_f (void)
{
	int	i, built = 0;

	for (i = 0; i < num_blases; i++)
		built += (blases[i].as != VK_NULL_HANDLE);
	Con_Printf ("%d static BLASes (%d models), %.2f MB, built with %.2f MB scratch\n", built,
			vk_world.num_models, blas_buffer.size / (1024.0 * 1024.0), blas_scratch_size / (1024.0 * 1024.0));
	if (last_tlas >= 0)
	{
		Con_Printf ("TLAS: %u instances, %.2f MB per frame in flight, build %.3f ms on the GPU\n",
				tlas[last_tlas].num_instances, tlas[last_tlas].buffer.size / (1024.0 * 1024.0), tlas_build_ms);
	}
	else
	{
		Con_Printf ("TLAS: none built yet\n");
	}
}

#define CHECK_W		64
#define CHECK_H		48
#define CHECK_TMAX	8192.0f

typedef struct
{
	VkDeviceAddress	tlas;
	VkDeviceAddress	results;
	float		origin[4], forward[4], right[4], up[4];
	float		tan_half_fov[2];
	uint32_t	grid[2];
	uint32_t	cull_mask;
	float		tmax;
} rt_check_push_t;	/* rt_check.comp's push constants */

typedef struct
{
	float		t;		/* < 0: no hit */
	uint32_t	instance;	/* TLAS instance */
	uint32_t	primitive;
	uint32_t	custom;
} rt_check_result_t;

/* a line through a hull, as SV_ClipMoveToEntity traces it; returns the
 * hit fraction, 1 = no hit, and the cosine between the line and the hit
 * plane */
static float HullTrace (hull_t *hull, const vec3_t start, const vec3_t end, qboolean *startsolid,
			qboolean *inwater, float *cos_hit)
{
	trace_t	tr;
	vec3_t	s, e, d;

	VectorCopy (start, s);
	VectorCopy (end, e);
	memset (&tr, 0, sizeof(tr));
	tr.fraction = 1;
	tr.allsolid = true;
	VectorCopy (end, tr.endpos);
	SV_RecursiveHullCheck (hull, hull->firstclipnode, 0, 1, s, e, &tr);
	if (startsolid)
		*startsolid = tr.startsolid || tr.allsolid;
	if (inwater)
		*inwater = tr.inwater;
	VectorSubtract (e, s, d);
	VectorNormalize (d);
	*cos_hit = fabsf (DotProduct (d, tr.plane.normal));
	return tr.fraction;
}

/* the brush entity's hull, rotated into its frame like SV_ClipMoveToEntity */
static float EntityTrace (const scene_entity_t *e, const vec3_t start, const vec3_t end, qboolean *startsolid,
			  float *cos_hit)
{
	vec3_t	s, en, f, r, u, tmp, angles;

	VectorSubtract (start, e->origin, s);
	VectorSubtract (end, e->origin, en);
	if (e->angles[0] || e->angles[1] || e->angles[2])
	{
		VectorCopy (e->angles, angles);	/* AngleVectors takes a non-const vector */
		AngleVectors (angles, f, r, u);
		VectorCopy (s, tmp);
		s[0] = DotProduct (tmp, f);	s[1] = -DotProduct (tmp, r);	s[2] = DotProduct (tmp, u);
		VectorCopy (en, tmp);
		en[0] = DotProduct (tmp, f);	en[1] = -DotProduct (tmp, r);	en[2] = DotProduct (tmp, u);
	}
	return HullTrace (&e->model->hulls[0], s, en, startsolid, NULL, cos_hit);
}

/* The nearest hit of a ray on the CPU, through the world's hull and the
 * brush entities': the distance, -1 = none. *skip is set for rays that
 * start in a wall or pass water, lava or sky, which the hulls let rays
 * through. */
static float CpuTrace (qmodel_t *world, const vec3_t start, const vec3_t dir, float *cos_hit,
		       qboolean *entity_hit, qboolean *skip)
{
	vec3_t		end;
	qboolean	startsolid, inwater, in_entity;
	float		frac, f, c;
	int		i;

	VectorMA (start, CHECK_TMAX, dir, end);
	frac = HullTrace (&world->hulls[0], start, end, &startsolid, &inwater, cos_hit);
	*entity_hit = false;
	for (i = 0; i < VK_NumInstances (); i++)
	{
		f = EntityTrace (VK_InstanceEntity (i), start, end, &in_entity, &c);
		if (!in_entity && f < frac)	/* not entities the camera is inside */
		{
			frac = f;
			*cos_hit = c;
			*entity_hit = true;
		}
	}
	*skip = startsolid || (inwater && !*entity_hit);
	return (frac < 1.0f) ? frac * CHECK_TMAX : -1.0f;
}

/* Do two hit distances agree? The hull trace stops DIST_EPSILON (1/32)
 * in front of the plane, which grows along a ray that grazes it. */
static qboolean HitsAgree (float t_cpu, float t_gpu, float cos_hit, float *diff)
{
	if (t_cpu < 0 || t_gpu < 0)
		return t_cpu < 0 && t_gpu < 0;
	*diff = fabsf (t_cpu - t_gpu);
	return *diff <= 1.0f + 0.04f / q_max (cos_hit, 0.01f);
}

static void RayDirection (int x, int y, float tx, float ty, vec3_t dir)
{
	float	u = ((x + 0.5f) / CHECK_W) * 2.0f - 1.0f;
	float	v = ((y + 0.5f) / CHECK_H) * 2.0f - 1.0f;
	int	i;

	for (i = 0; i < 3; i++)
		dir[i] = r_scene.forward[i] + r_scene.right[i] * u * tx - r_scene.up[i] * v * ty;
	VectorNormalize (dir);
}

static void DescribeTLASInstance (uint32_t instance, char *buf, size_t size)
{
	if (instance >= MAX_TLAS_INSTANCES)
		q_snprintf (buf, size, "nothing");
	else if (tlas_sources[instance].model_instance < 0)
		q_snprintf (buf, size, "world %s", range_names[tlas_sources[instance].range]);
	else
		q_snprintf (buf, size, "%s %s", VK_InstanceEntity (tlas_sources[instance].model_instance)->model->name,
				range_names[tlas_sources[instance].range]);
}

static void VK_RTCheck_f (void)
{
	VkPushConstantRange		push_range;
	VkPipelineLayoutCreateInfo	layout_info;
	VkComputePipelineCreateInfo	pipe_info;
	VkPipelineLayout		layout;
	VkPipeline			pipeline;
	VkShaderModule			module;
	VkCommandBuffer			cmd;
	VkMemoryBarrier2		barrier;
	VkDependencyInfo		dep;
	vk_buffer_t			results;
	rt_check_push_t			push;
	const rt_check_result_t		*res;
	qmodel_t			*world = r_scene.worldmodel;
	int				x, y, i, num_rays = CHECK_W * CHECK_H;
	int				agree = 0, at_edges = 0, differ = 0, skipped = 0, both_missed = 0;
	int				gpu_entity_hits = 0, cpu_entity_hits = 0;
	float				tx, ty, max_diff = 0.0f;
	char				gpu_desc[64];

	if (last_tlas < 0 || !world || cls.signon != SIGNONS)
	{
		Con_Printf ("No TLAS built yet: not in a map\n");
		return;
	}
	vkDeviceWaitIdle (vk.device);	/* the last frame's TLAS is built */

	/* the GPU half */
	module = VK_LoadShader ("rt_check.comp");
	memset (&push_range, 0, sizeof(push_range));
	push_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	push_range.size = sizeof(push);
	memset (&layout_info, 0, sizeof(layout_info));
	layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	layout_info.pushConstantRangeCount = 1;
	layout_info.pPushConstantRanges = &push_range;
	VK_CHECK (vkCreatePipelineLayout (vk.device, &layout_info, NULL, &layout));
	memset (&pipe_info, 0, sizeof(pipe_info));
	pipe_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
	pipe_info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	pipe_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	pipe_info.stage.module = module;
	pipe_info.stage.pName = "main";
	pipe_info.layout = layout;
	VK_CHECK (vkCreateComputePipelines (vk.device, VK_NULL_HANDLE, 1, &pipe_info, NULL, &pipeline));
	vkDestroyShaderModule (vk.device, module, NULL);

	VK_CreateBuffer (&results, num_rays * sizeof(rt_check_result_t),
			 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VK_MEMORY_READBACK);

	tx = tanf (r_scene.fov_x * (float)M_PI / 360.0f);
	ty = tanf (r_scene.fov_y * (float)M_PI / 360.0f);
	memset (&push, 0, sizeof(push));
	push.tlas = tlas[last_tlas].address;
	push.results = results.address;
	VectorCopy (r_scene.vieworg, push.origin);
	VectorCopy (r_scene.forward, push.forward);
	VectorCopy (r_scene.right, push.right);
	VectorCopy (r_scene.up, push.up);
	push.tan_half_fov[0] = tx;
	push.tan_half_fov[1] = ty;
	push.grid[0] = CHECK_W;
	push.grid[1] = CHECK_H;
	push.cull_mask = 0xff;
	push.tmax = CHECK_TMAX;

	cmd = VK_BeginUpload ();
	vkCmdBindPipeline (cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
	vkCmdPushConstants (cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
	vkCmdDispatch (cmd, (num_rays + 63) / 64, 1, 1);
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
	VK_EndUpload ();
	VK_CHECK (vmaInvalidateAllocation (vk.allocator, results.allocation, 0, VK_WHOLE_SIZE));
	res = (const rt_check_result_t *) results.mapped;

	/* the CPU half, and the comparison */
	for (y = 0; y < CHECK_H; y++)
	{
		for (x = 0; x < CHECK_W; x++)
		{
			const rt_check_result_t	*g = &res[y * CHECK_W + x];
			vec3_t			dir, shifted;
			qboolean		skip, cpu_entity, e2, s2;
			float			t_cpu, t_gpu = g->t, cos_hit, diff = 0.0f;

			RayDirection (x, y, tx, ty, dir);
			t_cpu = CpuTrace (world, r_scene.vieworg, dir, &cos_hit, &cpu_entity, &skip);
			if (skip)
			{
				skipped++;
				continue;
			}
			if (t_gpu >= 0 && g->instance < MAX_TLAS_INSTANCES && tlas_sources[g->instance].model_instance >= 0)
				gpu_entity_hits++;
			cpu_entity_hits += cpu_entity;

			if (t_cpu < 0 && t_gpu < 0)
			{
				both_missed++;
				continue;
			}
			if (HitsAgree (t_cpu, t_gpu, cos_hit, &diff))
			{
				agree++;
				max_diff = q_max (max_diff, diff);
				continue;
			}

			/* at an edge or corner, the two can see a hit or a miss: agree
			 * when the ray is shifted a little? */
			for (i = 0; i < 4; i++)
			{
				float	t2, c2, d2;

				VectorMA (r_scene.vieworg, (i & 1) ? 0.05f : -0.05f, (i & 2) ? r_scene.up : r_scene.right, shifted);
				t2 = CpuTrace (world, shifted, dir, &c2, &e2, &s2);
				if (!s2 && HitsAgree (t2, t_gpu, c2, &d2))
					break;
			}
			if (i < 4)
			{
				at_edges++;
				continue;
			}
			if (differ++ < 5)
			{
				DescribeTLASInstance ((t_gpu >= 0) ? g->instance : MAX_TLAS_INSTANCES, gpu_desc, sizeof(gpu_desc));
				Con_Printf ("ray %d,%d: GPU %.1f (%s, primitive %u), CPU %.1f (%s)\n", x, y, t_gpu, gpu_desc,
						g->primitive, t_cpu, cpu_entity ? "brush entity" : "world");
			}
		}
	}

	Con_Printf ("rtcheck: %d rays from %.0f %.0f %.0f through %u TLAS instances: %d agree (max difference %.3f), "
		    "%d at edges (agree with the ray shifted 0.05), %d differ, %d skipped (in a wall, or through water, "
		    "lava or sky), %d missed both\n",
			num_rays, r_scene.vieworg[0], r_scene.vieworg[1], r_scene.vieworg[2], tlas[last_tlas].num_instances,
			agree, max_diff, at_edges, differ, skipped, both_missed);
	Con_Printf ("brush entity hits: %d on the GPU, %d on the CPU\n", gpu_entity_hits, cpu_entity_hits);

	VK_DestroyBuffer (&results);
	vkDestroyPipeline (vk.device, pipeline, NULL);
	vkDestroyPipelineLayout (vk.device, layout, NULL);
}


/* ==========================================================================
 * Init / shutdown
 * ========================================================================== */

void VK_InitAccel (void)
{
	VkPhysicalDeviceAccelerationStructurePropertiesKHR	as_props;
	VkPhysicalDeviceProperties2				props;
	VkAccelerationStructureGeometryKHR			geom;
	VkAccelerationStructureBuildGeometryInfoKHR		info;
	VkAccelerationStructureBuildSizesInfoKHR		size;
	VkQueryPoolCreateInfo					query_info;
	uint32_t						max_instances = MAX_TLAS_INSTANCES;
	int							i;

	memset (&as_props, 0, sizeof(as_props));
	as_props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR;
	memset (&props, 0, sizeof(props));
	props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
	props.pNext = &as_props;
	vkGetPhysicalDeviceProperties2 (vk.physical_device, &props);
	scratch_alignment = q_max (as_props.minAccelerationStructureScratchOffsetAlignment, 1u);

	/* sizes for the most instances a TLAS can have */
	memset (&geom, 0, sizeof(geom));
	geom.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
	geom.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
	geom.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
	memset (&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
	info.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
	info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
	info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
	info.geometryCount = 1;
	info.pGeometries = &geom;
	memset (&size, 0, sizeof(size));
	size.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
	vkGetAccelerationStructureBuildSizesKHR (vk.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
						 &info, &max_instances, &size);

	for (i = 0; i < VK_FRAMES_IN_FLIGHT; i++)
	{
		vk_tlas_t	*t = &tlas[i];

		VK_CreateBuffer (&t->instances, MAX_TLAS_INSTANCES * sizeof(VkAccelerationStructureInstanceKHR),
				 VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
				 VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VK_MEMORY_UPLOAD);
		VK_CreateBuffer (&t->info, MAX_TLAS_INSTANCES * sizeof(TlasInstanceInfo),
				 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VK_MEMORY_UPLOAD);
		VK_CreateBuffer (&t->buffer, size.accelerationStructureSize,
				 VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
				 VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VK_MEMORY_DEVICE);
		VK_CreateBuffer (&t->scratch, size.buildScratchSize + scratch_alignment,
				 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VK_MEMORY_DEVICE);
		t->scratch_address = AlignSize (t->scratch.address, scratch_alignment);
		t->as = CreateAS (VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, &t->buffer, 0,
				  size.accelerationStructureSize, &t->address);
	}

	if (vk.props.limits.timestampComputeAndGraphics)
	{
		memset (&query_info, 0, sizeof(query_info));
		query_info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
		query_info.queryType = VK_QUERY_TYPE_TIMESTAMP;
		query_info.queryCount = VK_FRAMES_IN_FLIGHT * 2;
		VK_CHECK (vkCreateQueryPool (vk.device, &query_info, NULL, &query_pool));
	}

	Cmd_AddCommand ("vk_accel", VK_Accel_f);
	Cmd_AddCommand ("vk_rtcheck", VK_RTCheck_f);
}

void VK_ShutdownAccel (void)
{
	int	i;

	VK_FreeWorldAccel ();
	for (i = 0; i < VK_FRAMES_IN_FLIGHT; i++)
	{
		vk_tlas_t	*t = &tlas[i];

		if (t->as)
			vkDestroyAccelerationStructureKHR (vk.device, t->as, NULL);
		VK_DestroyBuffer (&t->instances);
		VK_DestroyBuffer (&t->info);
		VK_DestroyBuffer (&t->buffer);
		VK_DestroyBuffer (&t->scratch);
		memset (t, 0, sizeof(*t));
	}
	if (query_pool)
		vkDestroyQueryPool (vk.device, query_pool, NULL);
	query_pool = VK_NULL_HANDLE;
}
