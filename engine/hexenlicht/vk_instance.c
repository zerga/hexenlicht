/* vk_instance.c -- the frame's model instances
 *
 * Every frame, VK_UpdateInstances turns the scene's brush entities (doors,
 * lifts, trains, rotating brushes; dynamic and static ones) into
 * ModelInstances (shaders/hl_shared.h): a model-to-world transform and
 * last frame's, the vis cluster the model is in, its primitives in the
 * world buffer (vk_world.c) and the entity's Hexen II draw state. They go
 * to a mapped buffer per frame in flight, for the acceleration structures
 * and the shaders. The alias models of epic E2's 2.4 will be appended.
 *
 * The transform is the GL renderer's: R_DrawBrushModel and
 * R_RotateForEntity in gl_rsurf.c and gl_rmain.c. The cluster lookup
 * follows Quake II RTX's process_bsp_entity (src/refresh/vkpt/main.c).
 *
 * Copyright (C) 1996-1997  Id Software, Inc.
 * Copyright (C) 1997-1998  Raven Software Corp.
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

#define TRANSLUCENT_ALPHA	0.33f	/* r_wateralpha's default, which GL uses for DRF_TRANSLUCENT */

COMPILE_TIME_ASSERT(ModelInstance, sizeof(ModelInstance) == 208);	/* the shaders' std430 stride */

/* this frame's instances */
static int			num_instances;
static ModelInstance		instances[MAX_MODEL_INSTANCES];
static const scene_entity_t	*instance_entities[MAX_MODEL_INSTANCES];	/* for vk_instances */

static vk_buffer_t		instance_buffers[VK_FRAMES_IN_FLIGHT];

/* last frame's transforms, per dynamic and static entity */
typedef struct
{
	mat4		transform;
	qmodel_t	*model;
	int		framecount;	/* r_scene.framecount it was set in, 0 = never */
} prev_transform_t;

static prev_transform_t	prev_dynamic[MAX_EDICTS];
static prev_transform_t	prev_static[MAX_STATIC_ENTITIES];


/* ==========================================================================
 * Transforms
 * ========================================================================== */

/* a rotation of 'degrees' about axis 0, 1 or 2, as glRotatef makes it */
static void AxisRotation (float r[3][3], int axis, float degrees)
{
	float	a = degrees * (float)M_PI / 180.0f;
	float	c = cosf (a), s = sinf (a);
	int	i = (axis + 1) % 3, j = (axis + 2) % 3;

	memset (r, 0, sizeof(float) * 9);
	r[axis][axis] = 1.0f;
	r[i][i] = c;	r[i][j] = -s;	/* [row][column] */
	r[j][i] = s;	r[j][j] = c;
}

static void MulMat3 (float out[3][3], const float a[3][3], const float b[3][3])
{
	int	r, c;

	for (r = 0; r < 3; r++)
	{
		for (c = 0; c < 3; c++)
			out[r][c] = a[r][0] * b[0][c] + a[r][1] * b[1][c] + a[r][2] * b[2][c];
	}
}

/* R_DrawBrushModel negates pitch and roll ("stupid quake bug") and calls
 * R_RotateForEntity, which translates to the origin and rotates by yaw
 * about z, by -pitch about y and by -roll about x. So the model is
 * rotated by yaw, pitch and roll as they are. */
static void BrushTransform (mat4 m, const vec3_t origin, const vec3_t angles)
{
	float	rz[3][3], ry[3][3], rx[3][3], t[3][3], rot[3][3];
	int	r, c;

	AxisRotation (rz, 2, angles[YAW]);
	AxisRotation (ry, 1, angles[PITCH]);
	AxisRotation (rx, 0, angles[ROLL]);
	MulMat3 (t, rz, ry);
	MulMat3 (rot, t, rx);

	for (c = 0; c < 3; c++)
	{
		for (r = 0; r < 3; r++)
			m[c][r] = rot[r][c];
		m[c][3] = 0.0f;
	}
	m[3][0] = origin[0];
	m[3][1] = origin[1];
	m[3][2] = origin[2];
	m[3][3] = 1.0f;
}

static void TransformPoint (const mat4 m, const vec3_t in, vec3_t out)
{
	int	r;

	for (r = 0; r < 3; r++)
		out[r] = m[0][r] * in[0] + m[1][r] * in[1] + m[2][r] * in[2] + m[3][r];
}

/* Quake II RTX's way: the cluster at the model's center or, when that is
 * in solid (e.g. a button pushed into a wall), at one of its corners */
static int InstanceCluster (qmodel_t *model, const mat4 m)
{
	vec3_t	p, world_p;
	int	i, corner, cluster;

	for (i = 0; i < 3; i++)
		p[i] = (model->mins[i] + model->maxs[i]) * 0.5f;
	TransformPoint (m, p, world_p);
	cluster = VK_PointCluster (vk_world.worldmodel, world_p);

	for (corner = 0; corner < 8 && cluster < 0; corner++)
	{
		for (i = 0; i < 3; i++)
			p[i] = (corner & (1 << i)) ? model->maxs[i] : model->mins[i];
		TransformPoint (m, p, world_p);
		cluster = VK_PointCluster (vk_world.worldmodel, world_p);
	}
	return cluster;
}


/* ==========================================================================
 * The frame's instances
 * ========================================================================== */

static prev_transform_t *PrevTransform (const scene_entity_t *e)
{
	if (e->kind == SCENE_ENT_DYNAMIC && e->num >= 0 && e->num < MAX_EDICTS)
		return &prev_dynamic[e->num];
	if (e->kind == SCENE_ENT_STATIC && e->num >= 0 && e->num < MAX_STATIC_ENTITIES)
		return &prev_static[e->num];
	return NULL;
}

static void AddBrushInstance (const scene_entity_t *e)
{
	ModelInstance		*mi;
	const vk_bspmodel_t	*bsp;
	prev_transform_t	*prev;
	int			submodel = atoi (e->model->name + 1);	/* "*N" */
	float			alpha;

	if (submodel <= 0 || submodel >= vk_world.num_models || num_instances >= MAX_MODEL_INSTANCES)
		return;
	bsp = &vk_world.models[submodel];

	mi = &instances[num_instances];
	instance_entities[num_instances] = e;
	num_instances++;
	memset (mi, 0, sizeof(*mi));

	BrushTransform (mi->transform, e->origin, e->angles);
	prev = PrevTransform (e);
	if (prev && prev->model == e->model && prev->framecount == r_scene.framecount - 1)
		memcpy (mi->transform_prev, prev->transform, sizeof(mat4));
	else
		memcpy (mi->transform_prev, mi->transform, sizeof(mat4));
	if (prev)
	{
		memcpy (prev->transform, mi->transform, sizeof(mat4));
		prev->model = e->model;
		prev->framecount = r_scene.framecount;
	}

	/* the model's opaque, transparent and sky ranges follow each other */
	mi->cluster = InstanceCluster (e->model, mi->transform);
	mi->source_buffer_idx = VERTEX_BUFFER_WORLD;
	mi->render_buffer_idx = VERTEX_BUFFER_WORLD;
	mi->render_prim_offset = bsp->opaque.first;
	mi->prim_count = bsp->opaque.count + bsp->transparent.count + bsp->sky.count;
	mi->iqm_matrix_offset_curr_frame = -1;
	mi->iqm_matrix_offset_prev_frame = -1;

	alpha = (e->drawflags & DRF_TRANSLUCENT) ? TRANSLUCENT_ALPHA : 1.0f;
	mi->alpha_and_frame = VK_FloatToHalf (alpha) | ((uint32_t)(e->frame & 0xffff) << 16);
	mi->drawflags = (uint32_t)e->drawflags;
	mi->abslight = e->abslight / 255.0f;
	mi->entity = ((uint32_t)e->kind << 16) | ((uint32_t)e->num & 0xffff);
}

/* called by R_RenderView after the scene is built */
void VK_UpdateInstances (void)
{
	int	i;

	num_instances = 0;
	if (!vk_world.worldmodel || vk_world.worldmodel != r_scene.worldmodel)
		return;

	for (i = 0; i < r_scene.num_entities; i++)
	{
		const scene_entity_t	*e = &r_scene.entities[i];

		if (e->model->type == mod_brush && e->model->name[0] == '*')
			AddBrushInstance (e);
	}

	if (vk.frame_active && num_instances)
	{
		vk_buffer_t	*b = &instance_buffers[vk.frame_index];

		memcpy (b->mapped, instances, num_instances * sizeof(ModelInstance));
		VK_CHECK (vmaFlushAllocation (vk.allocator, b->allocation, 0, num_instances * sizeof(ModelInstance)));
	}
}

int VK_NumInstances (void)
{
	return num_instances;
}

const vk_buffer_t *VK_InstanceBuffer (void)
{
	return &instance_buffers[vk.frame_index];
}

void VK_ClearInstances (void)
{
	num_instances = 0;
	memset (prev_dynamic, 0, sizeof(prev_dynamic));
	memset (prev_static, 0, sizeof(prev_static));
}


/* ==========================================================================
 * vk_instances
 * ========================================================================== */

static void VK_Instances_f (void)
{
	static const char *const kinds[] = { "dyn", "static", "temp", "view" };
	int	i, moved = 0;

	if (!r_scene.worldmodel || cls.signon != SIGNONS)
	{
		Con_Printf ("No scene: not in a map\n");
		return;
	}

	for (i = 0; i < num_instances; i++)
	{
		const ModelInstance	*mi = &instances[i];
		const scene_entity_t	*e = instance_entities[i];
		qboolean		has_moved = memcmp (mi->transform, mi->transform_prev, sizeof(mat4)) != 0;

		moved += has_moved;
		Con_Printf ("%3d %-6s %4d %-5s org %.1f %.1f %.1f ang %.1f %.1f %.1f cluster %4d prims %u+%u alpha %.2f frame %u%s%s\n",
				i, kinds[e->kind], e->num, e->model->name,
				mi->transform[3][0], mi->transform[3][1], mi->transform[3][2],
				e->angles[0], e->angles[1], e->angles[2],
				mi->cluster, mi->render_prim_offset, mi->prim_count,
				(mi->alpha_and_frame & 0xffff) == VK_FloatToHalf (1.0f) ? 1.0f : TRANSLUCENT_ALPHA,
				mi->alpha_and_frame >> 16,
				((mi->drawflags & MLS_MASKIN) == MLS_ABSLIGHT) ? va(" abslight %.2f", mi->abslight) : "",
				has_moved ? " moved" : "");
	}
	Con_Printf ("%d instances in frame %d, %d moved since the last frame\n", num_instances, r_scene.framecount, moved);
}


/* ==========================================================================
 * Init / shutdown
 * ========================================================================== */

void VK_InitInstances (void)
{
	int	i;

	for (i = 0; i < VK_FRAMES_IN_FLIGHT; i++)
	{
		VK_CreateBuffer (&instance_buffers[i], MAX_MODEL_INSTANCES * sizeof(ModelInstance),
				 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
				 VK_MEMORY_UPLOAD);
	}
	Cmd_AddCommand ("vk_instances", VK_Instances_f);
}

void VK_ShutdownInstances (void)
{
	int	i;

	for (i = 0; i < VK_FRAMES_IN_FLIGHT; i++)
		VK_DestroyBuffer (&instance_buffers[i]);
	num_instances = 0;
}
