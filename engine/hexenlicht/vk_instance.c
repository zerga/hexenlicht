/* vk_instance.c -- the frame's model instances
 *
 * Every frame, VK_UpdateInstances turns the scene's entities with geometry
 * into ModelInstances (shaders/global_ubo.h): a model-to-world transform
 * and last frame's, the vis cluster the model is in, where its primitives
 * are and the entity's Hexen II draw state. They go to a mapped buffer
 * per frame in flight, for the acceleration structures and the shaders.
 *
 * Brush entities (doors, lifts, trains, rotating brushes; dynamic and
 * static ones) come first; their primitives are in the world buffer
 * (vk_world.c). The alias model entities follow, in three groups (opaque,
 * transparent, masked = cutouts): their primitives are written every frame
 * by vk_model.c's geometry pass into the instanced buffer, from two poses
 * of the model that the instance blends (r_lerpmodels); stepping monsters
 * glide between their moves (r_lerpmove). The instance
 * carries the material of the skin (vk_skin.c), the fixed light level and
 * the colorshade tint GL uses. The first-person weapon (cl.viewent) comes
 * last, in a group of its own as Quake II RTX's viewer weapon, with GL's
 * fov compensation; it looks like the group it would be in otherwise.
 *
 * The transforms, the pose choice and the draw state are the GL
 * renderer's: R_DrawBrushModel and R_RotateForEntity in gl_rsurf.c and
 * gl_rmain.c; R_RotateForEntity2, R_DrawAliasModel and R_SetupAliasFrame
 * in gl_rmain.c. The
 * cluster lookup follows Quake II RTX's process_bsp_entity, the blending
 * QuakeSpasm's R_SetupAliasFrame (src/refresh/vkpt/main.c; r_alias.c).
 *
 * Copyright (C) 1996-1997  Id Software, Inc.
 * Copyright (C) 1997-1998  Raven Software Corp.
 * Copyright (C) 2002-2009  John Fitzgibbons and others
 * Copyright (C) 2010-2014  QuakeSpasm developers
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

#define LERP_DEFAULT_TIME	0.1f	/* QuakeSpasm's blend time: Quake's monsters animate at 10 Hz */
#define LERP_MAX_INTERVAL	0.2	/* a longer pause between frame changes: the default time */

COMPILE_TIME_ASSERT(ModelInstance, sizeof(ModelInstance) == 224);	/* the shaders' std430 stride */

/* blend alias model poses between animation frames; 0 = GL's look */
static cvar_t	r_lerpmodels = {"r_lerpmodels", "1", CVAR_ARCHIVE};
/* glide stepping entities (walking monsters) between their moves; 0 = GL's steps */
static cvar_t	r_lerpmove = {"r_lerpmove", "1", CVAR_ARCHIVE};

/* this frame's instances */
static int			num_instances;
static ModelInstance		instances[MAX_MODEL_INSTANCES];
static const scene_entity_t	*instance_entities[MAX_MODEL_INSTANCES];	/* their sources */
static int			instance_submodels[MAX_MODEL_INSTANCES];	/* *N, for vk_accel.c; 0 = alias */
static vk_modelframe_t		model_frame;
static uint32_t			weapon_reserve;	/* the weapon's triangles, kept free: it comes last */

static vk_buffer_t		instance_buffers[VK_FRAMES_IN_FLIGHT];

/* what an entity showed last frame: its transform (for the motion) and,
 * for alias models, the animation */
typedef struct
{
	const entity_t	*ent;		/* temporary entities: the entity it belongs to */
	qmodel_t	*model;
	int		framecount;	/* r_scene.framecount it was set in, 0 = never */
	mat4		transform;
	vec3_t		origin, angles;	/* where it was shown */

	/* blending from prev_pose to pose, which became the pose at
	 * lerp_start; measured: lerp_start was a frame change */
	int		pose, prev_pose;
	double		lerp_start;
	float		lerp_time;
	qboolean	measured;

	/* the poses and backlerp last frame showed */
	int		shown_pose, shown_prev_pose;
	float		shown_backlerp;

	/* r_lerpmove: the entity's last move, from where it was to where it
	 * is now, which began at move_start; measured: move_start was a move */
	vec3_t		move_from_origin, move_from_angles;
	vec3_t		move_to_origin, move_to_angles;
	double		move_start;
	float		move_time;
	qboolean	move_measured;
	float		move_blend;	/* how far this frame's glide got, for vk_instances; -1 = not gliding */
} entity_history_t;

/* Temporary entities (the client's effects, beams) have no number: their
 * entity's address is the key; big enough to stay mostly empty. The
 * effects' entities (cl_effect.c) keep their address, but beam segments
 * (cl_tent.c's stream entities) are made anew every frame, so a slot can
 * hold another segment of the same model next frame: their motion and
 * blending are approximate (their frames and roll are random per frame
 * anyway). */
#define TEMP_HISTORY_SIZE	1024	/* power of 2 */
#define TEMP_HISTORY_PROBES	32

static entity_history_t	history_dynamic[MAX_EDICTS];
static entity_history_t	history_static[MAX_STATIC_ENTITIES];
static entity_history_t	history_temp[TEMP_HISTORY_SIZE];
static entity_history_t	history_viewmodel;	/* cl.viewent; a new weapon is a new model */


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

/* three rotations applied as glRotatef calls in this order */
static void Rotations (float rot[3][3], int axis1, float deg1, int axis2, float deg2, int axis3, float deg3)
{
	float	r1[3][3], r2[3][3], r3[3][3], t[3][3];

	AxisRotation (r1, axis1, deg1);
	AxisRotation (r2, axis2, deg2);
	AxisRotation (r3, axis3, deg3);
	MulMat3 (t, r1, r2);
	MulMat3 (rot, t, r3);
}

/* translation to origin, then rot, then a scale per axis and an offset
 * (in the rotated frame): m(p) = origin + rot * (scale * p + offset) */
static void BuildTransform (mat4 m, const vec3_t origin, const float rot[3][3], const vec3_t scale, const vec3_t offset)
{
	int	r, c;

	for (c = 0; c < 3; c++)
	{
		for (r = 0; r < 3; r++)
			m[c][r] = rot[r][c] * scale[c];
		m[c][3] = 0.0f;
	}
	for (r = 0; r < 3; r++)
		m[3][r] = origin[r] + rot[r][0] * offset[0] + rot[r][1] * offset[1] + rot[r][2] * offset[2];
	m[3][3] = 1.0f;
}

/* R_DrawBrushModel negates pitch and roll ("stupid quake bug") and calls
 * R_RotateForEntity, which translates to the origin and rotates by yaw
 * about z, by -pitch about y and by -roll about x. So the model is
 * rotated by yaw, pitch and roll as they are. */
static void BrushTransform (mat4 m, const vec3_t origin, const vec3_t angles)
{
	static const vec3_t	one = { 1.0f, 1.0f, 1.0f }, zero = { 0.0f, 0.0f, 0.0f };
	float			rot[3][3];

	Rotations (rot, 2, angles[YAW], 1, angles[PITCH], 0, angles[ROLL]);
	BuildTransform (m, origin, rot, one, zero);
}

/* R_RotateForEntity2: yaw about z (EF_ROTATE items spin with time), -pitch
 * about y, -roll about x; EF_FACE_VIEW models turn to the camera, pitch
 * first */
static void AliasRotation (const scene_entity_t *e, float rot[3][3])
{
	float	yaw, pitch, forward;
	vec3_t	dir;

	if (e->model->flags & EF_FACE_VIEW)
	{
		VectorSubtract (r_scene.vieworg, e->origin, dir);
		VectorNormalize (dir);
		if (dir[1] == 0 && dir[0] == 0)
		{
			yaw = 0;
			pitch = (dir[2] > 0) ? 90 : 270;
		}
		else
		{
			yaw = (float)(int) (atan2(dir[1], dir[0]) * 180 / M_PI);
			if (yaw < 0)
				yaw += 360;
			forward = (float) sqrt (dir[0]*dir[0] + dir[1]*dir[1]);
			pitch = (float)(int) (atan2(dir[2], forward) * 180 / M_PI);
			if (pitch < 0)
				pitch += 360;
		}
		Rotations (rot, 1, -pitch, 2, yaw, 0, -e->angles[ROLL]);
		return;
	}

	if (e->model->flags & EF_ROTATE)
		yaw = anglemod ((float)((e->origin[0] + e->origin[1]) * 0.8 + (108 * r_scene.time)));
	else
		yaw = e->angles[YAW];
	Rotations (rot, 2, yaw, 1, -e->angles[PITCH], 0, -e->angles[ROLL]);
}

/* R_DrawAliasModel folds the entity's scale (e->scale percent, the
 * SCALE_TYPE_* axes and SCALE_ORIGIN_* point), the EF_ROTATE bobbing and
 * the weapon's fov compensation into the pose decode:
 * tmatrix(v) = hdr->scale * scale * v + tm_offset.
 * The decode p = hdr->scale * v + hdr->scale_origin stays in the model
 * table here, so what remains, in model space, is p' = scale * p + offset
 * with offset = tm_offset - scale * hdr->scale_origin. */
static void AliasScale (const scene_entity_t *e, const aliashdr_t *hdr, vec3_t scale, vec3_t offset)
{
	float	tm_offset[3];	/* GL's tmatrix translation */
	float	ent_scale, xyfact = 1.0f, zfact = 1.0f;
	int	i;

	for (i = 0; i < 3; i++)
	{
		scale[i] = 1.0f;
		tm_offset[i] = hdr->scale_origin[i];
	}

	if (e->scale != 0 && e->scale != 100)
	{
		ent_scale = (float)e->scale / 100.0f;
		switch (e->drawflags & SCALE_TYPE_MASKIN)
		{
		case SCALE_TYPE_XYONLY:
			scale[0] = scale[1] = ent_scale;
			xyfact = (float)((ent_scale - 1.0) * 127.95);
			zfact = 1.0f;	/* sic: GL moves z by one step of the model's grid */
			break;
		case SCALE_TYPE_ZONLY:
			scale[2] = ent_scale;
			xyfact = 1.0f;	/* sic, as above */
			zfact = (float)((ent_scale - 1.0) * 127.95);
			break;
		default:	/* SCALE_TYPE_UNIFORM */
			scale[0] = scale[1] = scale[2] = ent_scale;
			xyfact = zfact = (float)((ent_scale - 1.0) * 127.95);
			break;
		}

		tm_offset[0] = hdr->scale_origin[0] - hdr->scale[0] * xyfact;
		tm_offset[1] = hdr->scale_origin[1] - hdr->scale[1] * xyfact;
		switch (e->drawflags & SCALE_ORIGIN_MASKIN)
		{
		case SCALE_ORIGIN_BOTTOM:
			tm_offset[2] = hdr->scale_origin[2];
			break;
		case SCALE_ORIGIN_TOP:
			tm_offset[2] = (float)(hdr->scale_origin[2] - hdr->scale[2] * zfact * 2.0);
			break;
		default:	/* SCALE_ORIGIN_CENTER */
			tm_offset[2] = hdr->scale_origin[2] - hdr->scale[2] * zfact;
			break;
		}
	}

	if (e->model->flags & EF_ROTATE)	/* floating motion */
		tm_offset[2] = (float)(tm_offset[2] + sin(e->origin[0] + e->origin[1] + (r_scene.time * 3)) * 5.5);

	/* GL stretches the weapon across the view (y) and up (z) by
	 * tan(fov / 2) above fov 90, so it isn't distorted */
	if (e->kind == SCENE_ENT_VIEWMODEL && scr_fov.integer > 90)
	{
		float	fovscale = (float) tan (scr_fov.value * (0.5 * M_PI / 180));

		for (i = 1; i < 3; i++)
		{
			scale[i] *= fovscale;
			tm_offset[i] *= fovscale;
		}
	}

	for (i = 0; i < 3; i++)
		offset[i] = tm_offset[i] - scale[i] * hdr->scale_origin[i];
}

static void TransformPoint (const mat4 m, const vec3_t in, vec3_t out)
{
	int	r;

	for (r = 0; r < 3; r++)
		out[r] = m[0][r] * in[0] + m[1][r] * in[1] + m[2][r] * in[2] + m[3][r];
}

/* Quake II RTX's way: the cluster at the model's center or, when that is
 * in solid (e.g. a button pushed into a wall, an item's origin on the
 * floor), at one of its corners */
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
 * Entity history
 * ========================================================================== */

static unsigned TempHash (const entity_t *ent)
{
	return (unsigned)((((uintptr_t)ent) >> 3) * 2654435761u) & (TEMP_HISTORY_SIZE - 1);
}

/* A temporary entity's history: its own if it was drawn last frame, else a
 * slot nobody used in the last two frames, cleared. Slots are never
 * emptied, so a search ends at one that was never used. */
static entity_history_t *TempHistory (const entity_t *ent)
{
	entity_history_t	*h, *free_slot = NULL;
	unsigned		i = TempHash (ent), n;
	qboolean		live;

	for (n = 0; n < TEMP_HISTORY_PROBES; n++, i = (i + 1) & (TEMP_HISTORY_SIZE - 1))
	{
		h = &history_temp[i];
		live = h->framecount && h->framecount >= r_scene.framecount - 1;
		if (h->ent == ent && live)
			return h;
		if (!live && !free_slot)
			free_slot = h;
		if (!h->ent)
			break;
	}
	if (free_slot)
	{
		memset (free_slot, 0, sizeof(*free_slot));
		free_slot->ent = ent;
	}
	return free_slot;	/* NULL: the table is full around here, no history */
}

static entity_history_t *EntityHistory (const scene_entity_t *e)
{
	switch (e->kind)
	{
	case SCENE_ENT_DYNAMIC:
		return (e->num >= 0 && e->num < MAX_EDICTS) ? &history_dynamic[e->num] : NULL;
	case SCENE_ENT_STATIC:
		return (e->num >= 0 && e->num < MAX_STATIC_ENTITIES) ? &history_static[e->num] : NULL;
	case SCENE_ENT_TEMP:
		return TempHistory (e->ent);
	case SCENE_ENT_VIEWMODEL:
		return &history_viewmodel;
	default:
		return NULL;
	}
}

/* was h this entity last frame? */
static qboolean HistoryContinues (const entity_history_t *h, const scene_entity_t *e)
{
	return h && h->model == e->model && h->framecount && h->framecount == r_scene.framecount - 1;
}

/* Last frame's transform, and this frame's for the next. An entity that
 * jumped over 100 units on an axis since the last frame teleported (or is
 * a new entity in a reused slot), as CL_RelinkEntities assumes: no motion. */
static void UpdateTransformHistory (ModelInstance *mi, entity_history_t *h, qboolean continues, const scene_entity_t *e)
{
	if (continues && fabsf (e->origin[0] - h->origin[0]) <= 100.0f && fabsf (e->origin[1] - h->origin[1]) <= 100.0f &&
	    fabsf (e->origin[2] - h->origin[2]) <= 100.0f)
		memcpy (mi->transform_prev, h->transform, sizeof(mat4));
	else
		memcpy (mi->transform_prev, mi->transform, sizeof(mat4));
	if (h)
		memcpy (h->transform, mi->transform, sizeof(mat4));
}

static void EndHistory (entity_history_t *h, const scene_entity_t *e)
{
	if (h)
	{
		h->model = e->model;
		h->framecount = r_scene.framecount;
		VectorCopy (e->origin, h->origin);
		VectorCopy (e->angles, h->angles);
	}
}


/* ==========================================================================
 * Brush entities
 * ========================================================================== */

static void AddBrushInstance (const scene_entity_t *e)
{
	ModelInstance		*mi;
	const vk_bspmodel_t	*bsp;
	entity_history_t	*h;
	int			submodel = atoi (e->model->name + 1);	/* "*N" */
	float			alpha;

	if (submodel <= 0 || submodel >= vk_world.num_models || num_instances >= MAX_MODEL_INSTANCES)
		return;
	bsp = &vk_world.models[submodel];

	mi = &instances[num_instances];
	instance_entities[num_instances] = e;
	instance_submodels[num_instances] = submodel;
	num_instances++;
	memset (mi, 0, sizeof(*mi));

	BrushTransform (mi->transform, e->origin, e->angles);
	h = EntityHistory (e);
	UpdateTransformHistory (mi, h, HistoryContinues (h, e), e);
	EndHistory (h, e);

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
	/* R_DrawBrushModel: only MLS_ABSLIGHT sets a fixed light level */
	mi->light = ((e->drawflags & MLS_MASKIN) == MLS_ABSLIGHT) ? e->abslight / 255.0f : -1.0f;
	mi->entity = ((uint32_t)e->kind << 16) | ((uint32_t)e->num & 0xffff);
	mi->tint[0] = mi->tint[1] = mi->tint[2] = 1.0f;
}


/* ==========================================================================
 * Alias model entities
 * ========================================================================== */

/* The groups of the instanced buffer, in Quake II RTX's order. GL draws
 * all but the opaque ones in its translucent pass (R_DrawEntitiesOnList);
 * EF_HOLEY skins have alpha 0 or 1, so they are cutouts. */
static int AliasGroup (const scene_entity_t *e)
{
	if ((e->drawflags & DRF_TRANSLUCENT) || (e->model->flags & (EF_TRANSPARENT | EF_SPECIAL_TRANS)))
		return MODEL_GROUP_TRANSPARENT;
	if (e->model->flags & EF_HOLEY)
		return MODEL_GROUP_MASKED;
	return MODEL_GROUP_OPAQUE;
}

/* R_DrawAliasModel's fixed light levels (255 = 1), in its order: spinning
 * items pulse, MLS_ABSLIGHT, the other MLS_* modes (fullbright, power
 * mode, torch, total darkness) from light styles 25-30; -1 = lit by the
 * world */
static float AliasLight (const scene_entity_t *e)
{
	int	mls = e->drawflags & MLS_MASKIN;

	if (e->model->flags & EF_ROTATE)
		return (float)((60 + 34 + sin(e->origin[0] + e->origin[1] + (r_scene.time*3.8)) * 34) / 255.0);
	if (mls == MLS_ABSLIGHT)
		return e->abslight / 255.0f;
	if (mls != MLS_NONE)	/* d_lightstylevalue[24+mls]/2 */
		return (float)((int)(r_scene.lightstyles[24 + mls] * 256.0f + 0.5f) / 2) / 255.0f;
	return -1.0f;
}

/* R_SetupAliasFrame: the pose GL shows. Frame groups step through their
 * poses with time; *group_interval is their interval, 0 otherwise. */
static int AliasPose (const scene_entity_t *e, const aliashdr_t *hdr, float *group_interval)
{
	int	frame = e->frame, pose, numposes;

	if (frame >= hdr->numframes || frame < 0)
	{
		model_frame.bad_frames++;	/* GL prints it with developer 1; no printing inside a frame */
		frame = 0;
	}
	pose = hdr->frames[frame].firstpose;
	numposes = hdr->frames[frame].numposes;
	*group_interval = 0.0f;
	if (numposes > 1 && hdr->frames[frame].interval > 0.0f)
	{
		*group_interval = hdr->frames[frame].interval;
		pose += (int)(r_scene.time / hdr->frames[frame].interval) % numposes;
	}
	return pose;
}

/* QuakeSpasm's frame blending: when the pose changes, blend from the old
 * one to the new over the time the animation takes per frame. Quake
 * animates at 10 Hz, QuakeSpasm's fixed 0.1 s, but Hexen II animates many
 * things at 20 Hz (HX_FRAME_TIME), so the time is the interval between
 * the entity's last two frame changes; the default after a pause or
 * when the entity appears. Returns the current pose's weight. */
static float AliasBlend (entity_history_t *h, qboolean continues, int pose, float group_interval)
{
	double	time = r_scene.time, since;

	if (!continues || time < h->lerp_start)
	{
		h->pose = h->prev_pose = pose;
		h->lerp_start = time;
		h->lerp_time = 0.0f;
		h->measured = false;
	}
	else if (pose != h->pose)
	{
		since = time - h->lerp_start;
		if (group_interval > 0.0f)
			h->lerp_time = group_interval;
		else if (h->measured && since > 0.0 && since <= LERP_MAX_INTERVAL)
			h->lerp_time = (float)since;
		else
			h->lerp_time = LERP_DEFAULT_TIME;
		h->prev_pose = h->pose;
		h->pose = pose;
		h->lerp_start = time;
		h->measured = true;
	}

	if (h->lerp_time <= 0.0f)
		return 1.0f;
	return (float) q_min (q_max ((time - h->lerp_start) / h->lerp_time, 0.0), 1.0);
}

/* QuakeSpasm's movement blending (R_SetupEntityTransform): a stepping
 * entity (MOVETYPE_STEP, the walking monsters; the server marks them
 * U_NOLERP) moves only when it thinks, and GL shows it jumping from place
 * to place. When its origin or angles change, it glides from the old ones
 * to the new. QuakeSpasm glides for 0.1 s, but Hexen II's monsters step
 * at 20 Hz or 10 Hz, so the time is the interval between the entity's
 * last two moves, as for the frame blending. A move that arrives before
 * the glide is over (moves come on whole server frames) glides on from
 * where the entity is shown, where QuakeSpasm jumps to the old target
 * first. A move of over 100 units on an axis is a teleport, as
 * CL_RelinkEntities assumes: no glide, and true is returned. Leaves the
 * origin and angles to show in shown. */
static float GlideBlend (const entity_history_t *h, double time)
{
	return (h->move_time > 0.0f) ? (float) q_min (q_max ((time - h->move_start) / h->move_time, 0.0), 1.0) : 1.0f;
}

static void GlidePlace (const entity_history_t *h, float blend, vec3_t origin, vec3_t angles)
{
	float	d;
	int	i;

	for (i = 0; i < 3; i++)
	{
		origin[i] = h->move_from_origin[i] + (h->move_to_origin[i] - h->move_from_origin[i]) * blend;
		d = h->move_to_angles[i] - h->move_from_angles[i];
		if (d > 180)
			d -= 360;
		else if (d < -180)
			d += 360;
		angles[i] = h->move_from_angles[i] + d * blend;
	}
}

static qboolean MoveBlend (entity_history_t *h, qboolean continues, const scene_entity_t *e, scene_entity_t *shown)
{
	double		time = r_scene.time, since;
	qboolean	jumped;

	if (!h)
		return false;
	h->move_blend = -1.0f;

	jumped = continues && (fabsf (e->origin[0] - h->move_to_origin[0]) > 100.0f ||
			       fabsf (e->origin[1] - h->move_to_origin[1]) > 100.0f ||
			       fabsf (e->origin[2] - h->move_to_origin[2]) > 100.0f);
	if (!continues || time < h->move_start || jumped)
	{
		VectorCopy (e->origin, h->move_from_origin);
		VectorCopy (e->origin, h->move_to_origin);
		VectorCopy (e->angles, h->move_from_angles);
		VectorCopy (e->angles, h->move_to_angles);
		h->move_start = time;
		h->move_time = 0.0f;
		h->move_measured = false;
	}
	else if (!VectorCompare (e->origin, h->move_to_origin) || !VectorCompare (e->angles, h->move_to_angles))
	{
		vec3_t	origin, angles;

		/* on from where the last glide has got to */
		GlidePlace (h, GlideBlend (h, time), origin, angles);
		since = time - h->move_start;
		if (h->move_measured && since > 0.0 && since <= LERP_MAX_INTERVAL)
			h->move_time = (float)since;
		else
			h->move_time = LERP_DEFAULT_TIME;
		VectorCopy (origin, h->move_from_origin);
		VectorCopy (angles, h->move_from_angles);
		VectorCopy (e->origin, h->move_to_origin);
		VectorCopy (e->angles, h->move_to_angles);
		h->move_start = time;
		h->move_measured = true;
	}

	if (!r_lerpmove.integer || !e->movestep || h->move_time <= 0.0f)
		return jumped;
	h->move_blend = GlideBlend (h, time);
	GlidePlace (h, h->move_blend, shown->origin, shown->angles);
	return jumped;
}

static void AddAliasInstance (const scene_entity_t *e, int group, uint32_t *next_prim)
{
	ModelInstance		*mi;
	const vk_aliasmodel_t	*am;
	const aliashdr_t	*hdr;
	entity_history_t	*h;
	scene_entity_t		shown;		/* e where r_lerpmove shows it */
	qboolean		continues, jumped, bad_skin;
	float			rot[3][3], group_interval, blend, backlerp, alpha;
	vec3_t			scale, offset;
	int			index = VK_AliasModelIndex (e->model), pose, curr, prev, material;
	uint32_t		reserve = (e->kind == SCENE_ENT_VIEWMODEL) ? 0 : weapon_reserve;

	if (index < 0)
		return;		/* nothing to draw */
	am = VK_GetAliasModel (index);
	if (num_instances >= MAX_MODEL_INSTANCES ||
	    *next_prim + (uint32_t)am->num_tris + reserve > (uint32_t)MAX_INSTANCED_PRIMITIVES)
	{
		model_frame.dropped++;
		return;
	}
	hdr = (const aliashdr_t *) Mod_Extradata (e->model);

	mi = &instances[num_instances];
	instance_entities[num_instances] = e;
	instance_submodels[num_instances] = 0;
	num_instances++;
	memset (mi, 0, sizeof(*mi));

	h = EntityHistory (e);
	continues = HistoryContinues (h, e);
	shown = *e;
	jumped = MoveBlend (h, continues, e, &shown);
	AliasRotation (&shown, rot);
	AliasScale (&shown, hdr, scale, offset);
	BuildTransform (mi->transform, shown.origin, rot, scale, offset);
	UpdateTransformHistory (mi, h, continues && !jumped, &shown);	/* a teleport: no motion */

	/* the poses: blending from prev to curr, or GL's pose */
	pose = AliasPose (e, hdr, &group_interval);
	curr = prev = pose;
	backlerp = 0.0f;
	if (h)
	{
		blend = AliasBlend (h, continues, pose, group_interval);
		if (r_lerpmodels.integer)
		{
			curr = h->pose;
			prev = h->prev_pose;
			backlerp = (curr != prev) ? 1.0f - blend : 0.0f;
		}
	}
	mi->prim_offset_curr_pose_curr_frame = (uint32_t)(curr * am->num_pose_verts);
	mi->prim_offset_prev_pose_curr_frame = (uint32_t)(prev * am->num_pose_verts);
	mi->pose_lerp_curr_frame = backlerp;
	if (continues)
	{
		mi->prim_offset_curr_pose_prev_frame = (uint32_t)(h->shown_pose * am->num_pose_verts);
		mi->prim_offset_prev_pose_prev_frame = (uint32_t)(h->shown_prev_pose * am->num_pose_verts);
		mi->pose_lerp_prev_frame = h->shown_backlerp;
	}
	else
	{
		mi->prim_offset_curr_pose_prev_frame = mi->prim_offset_curr_pose_curr_frame;
		mi->prim_offset_prev_pose_prev_frame = mi->prim_offset_prev_pose_curr_frame;
		mi->pose_lerp_prev_frame = backlerp;
	}
	if (h)
	{
		h->shown_pose = curr;
		h->shown_prev_pose = prev;
		h->shown_backlerp = backlerp;
	}
	EndHistory (h, &shown);

	/* the skin GL would bind; translucent ones are Quake II RTX's
	 * transparent models; the weapon's triangles are flagged, as Quake II
	 * RTX's viewer weapon */
	material = VK_SkinMaterial (e, hdr, &bad_skin);
	model_frame.bad_skins += bad_skin;
	mi->material = ((group == MODEL_GROUP_TRANSPARENT) ? MATERIAL_KIND_TRANSP_MODEL : MATERIAL_KIND_REGULAR) |
		       (uint32_t)material;
	if (e->kind == SCENE_ENT_VIEWMODEL)
		mi->material |= MATERIAL_FLAG_WEAPON;
	mi->cluster = InstanceCluster (e->model, mi->transform);
	mi->source_buffer_idx = VERTEX_BUFFER_FIRST_MODEL + (uint32_t)index;
	mi->prim_count = (uint32_t)am->num_tris;
	mi->iqm_matrix_offset_curr_frame = -1;
	mi->iqm_matrix_offset_prev_frame = -1;
	mi->render_buffer_idx = VERTEX_BUFFER_INSTANCED;
	mi->render_prim_offset = *next_prim;
	*next_prim += (uint32_t)am->num_tris;

	/* the entity's alpha; the skin's is in its texture (opacity) */
	alpha = (e->drawflags & DRF_TRANSLUCENT) ? TRANSLUCENT_ALPHA : 1.0f;
	mi->alpha_and_frame = VK_FloatToHalf (alpha);
	mi->drawflags = (uint32_t)e->drawflags;
	mi->light = AliasLight (&shown);
	mi->entity = ((uint32_t)e->kind << 16) | ((uint32_t)e->num & 0xffff);
	mi->colorshade = (uint32_t)(e->colorshade & 0xff);
	if (mi->colorshade)
	{
		mi->tint[0] = RTint[mi->colorshade];
		mi->tint[1] = GTint[mi->colorshade];
		mi->tint[2] = BTint[mi->colorshade];
	}
	else
	{
		mi->tint[0] = mi->tint[1] = mi->tint[2] = 1.0f;
	}
}


/* ==========================================================================
 * The frame's instances
 * ========================================================================== */

/* called by R_RenderView after the scene is built */
void VK_UpdateInstances (void)
{
	int		i, group, dropped_total = model_frame.dropped_total;
	uint32_t	next_prim = 0;

	num_instances = 0;
	memset (&model_frame, 0, sizeof(model_frame));
	model_frame.dropped_total = dropped_total;
	if (!vk_world.worldmodel || vk_world.worldmodel != r_scene.worldmodel)
		return;

	for (i = 0; i < r_scene.num_entities; i++)
	{
		const scene_entity_t	*e = &r_scene.entities[i];

		if (e->model->type == mod_brush && e->model->name[0] == '*')
			AddBrushInstance (e);
	}

	/* the alias models, group by group; the weapon in its own group, which
	 * looks like the group it would be in otherwise, with room kept for it
	 * when the others fill the instanced buffer */
	weapon_reserve = 0;
	for (i = 0; i < r_scene.num_entities; i++)
	{
		const scene_entity_t	*e = &r_scene.entities[i];
		int			index;

		if (e->kind == SCENE_ENT_VIEWMODEL && e->model->type == mod_alias &&
		    (index = VK_AliasModelIndex (e->model)) >= 0)
			weapon_reserve = (uint32_t)VK_GetAliasModel (index)->num_tris;
	}
	model_frame.first_instance = num_instances;
	for (group = 0; group < NUM_MODEL_GROUPS; group++)
	{
		vk_primrange_t	*range = &model_frame.groups[group];

		range->first = next_prim;
		for (i = 0; i < r_scene.num_entities; i++)
		{
			const scene_entity_t	*e = &r_scene.entities[i];

			if (e->model->type != mod_alias)
				continue;
			if (group == MODEL_GROUP_WEAPON && e->kind == SCENE_ENT_VIEWMODEL)
			{
				model_frame.weapon_look = AliasGroup (e);
				AddAliasInstance (e, model_frame.weapon_look, &next_prim);
			}
			else if (group != MODEL_GROUP_WEAPON && e->kind != SCENE_ENT_VIEWMODEL && AliasGroup (e) == group)
			{
				AddAliasInstance (e, group, &next_prim);
			}
		}
		range->count = next_prim - range->first;
	}
	model_frame.num_instances = num_instances - model_frame.first_instance;
	model_frame.dropped_total += model_frame.dropped;

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

const ModelInstance *VK_GetInstance (int i)
{
	return &instances[i];
}

const scene_entity_t *VK_InstanceEntity (int i)
{
	return instance_entities[i];
}

int VK_InstanceSubmodel (int i)
{
	return instance_submodels[i];
}

const vk_buffer_t *VK_InstanceBuffer (void)
{
	return &instance_buffers[vk.frame_index];
}

const vk_modelframe_t *VK_ModelFrame (void)
{
	return &model_frame;
}

void VK_ClearInstances (void)
{
	num_instances = 0;
	memset (&model_frame, 0, sizeof(model_frame));
	memset (history_dynamic, 0, sizeof(history_dynamic));
	memset (history_static, 0, sizeof(history_static));
	memset (&history_viewmodel, 0, sizeof(history_viewmodel));
	memset (history_temp, 0, sizeof(history_temp));
}


/* ==========================================================================
 * vk_instances
 * ========================================================================== */

static void VK_Instances_f (void)
{
	static const char *const kinds[] = { "dyn", "static", "temp", "view" };
	qboolean	step_only = Cmd_Argc () > 1 && !q_strcasecmp (Cmd_Argv (1), "step");
	int		i, moved = 0, steps = 0;

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
		char			state[160], extra[96];

		if (Cmd_Argc () > 1 && !q_strcasecmp (Cmd_Argv (1), "box"))
		{
			/* "vk_instances box": the alias instances' current pose, its
			 * bounds in model space and in the world, and where the
			 * world box's center is in the view (depth, degrees right
			 * and up of the view center) */
			const vk_aliasmodel_t	*am;
			const aliashdr_t	*hdr;
			const trivertx_t	*v;
			vec3_t			mmin, mmax, wmin, wmax, p, w;
			int			k, c, pose;

			if (mi->render_buffer_idx != VERTEX_BUFFER_INSTANCED)
				continue;
			am = VK_GetAliasModel ((int)mi->source_buffer_idx - VERTEX_BUFFER_FIRST_MODEL);
			hdr = (const aliashdr_t *) Mod_Extradata (am->model);
			pose = (int)(mi->prim_offset_curr_pose_curr_frame / am->num_pose_verts);
			v = (const trivertx_t *)((const byte *)hdr + hdr->posedata) + pose * hdr->poseverts;
			for (c = 0; c < 3; c++)
			{
				mmin[c] = wmin[c] = 1e9f;
				mmax[c] = wmax[c] = -1e9f;
			}
			for (k = 0; k < hdr->poseverts; k++)
			{
				for (c = 0; c < 3; c++)
					p[c] = v[k].v[c] * hdr->scale[c] + hdr->scale_origin[c];
				TransformPoint (mi->transform, p, w);
				for (c = 0; c < 3; c++)
				{
					mmin[c] = q_min (mmin[c], p[c]);	mmax[c] = q_max (mmax[c], p[c]);
					wmin[c] = q_min (wmin[c], w[c]);	wmax[c] = q_max (wmax[c], w[c]);
				}
			}
			{
				vec3_t	d;
				float	depth, sx, sy;

				for (c = 0; c < 3; c++)
					d[c] = (wmin[c] + wmax[c]) * 0.5f - r_scene.vieworg[c];
				depth = DotProduct (d, r_scene.forward);
				sx = (float)(atan2 (DotProduct (d, r_scene.right), depth) * 180 / M_PI);
				sy = (float)(atan2 (DotProduct (d, r_scene.up), depth) * 180 / M_PI);
				Con_Printf ("%3d %-20s org %.0f %.0f %.0f ang %.0f %.0f %.0f pose %d: model %.0f %.0f %.0f .. %.0f %.0f %.0f, "
					    "world %.0f %.0f %.0f .. %.0f %.0f %.0f; center depth %.1f at %.1f right %.1f up\n",
						i, e->model->name, e->origin[0], e->origin[1], e->origin[2], e->angles[0], e->angles[1],
						e->angles[2], pose, mmin[0], mmin[1], mmin[2], mmax[0], mmax[1], mmax[2],
						wmin[0], wmin[1], wmin[2], wmax[0], wmax[1], wmax[2], depth, sx, sy);
			}
			continue;
		}
		if (step_only)
		{
			/* "vk_instances step": the stepping entities, where they are
			 * and where r_lerpmove shows them */
			const entity_history_t	*h = (e->num >= 0 && e->num < MAX_EDICTS) ? &history_dynamic[e->num] : NULL;

			if (!e->movestep || e->kind != SCENE_ENT_DYNAMIC || !h || mi->render_buffer_idx != VERTEX_BUFFER_INSTANCED)
				continue;
			steps++;
			Con_Printf ("%4d %-20s at %.1f %.1f %.1f yaw %.1f, shown %.1f %.1f %.1f yaw %.1f, glide %.2f of %.3f s\n",
					e->num, e->model->name, e->origin[0], e->origin[1], e->origin[2], e->angles[1],
					h->origin[0], h->origin[1], h->origin[2], h->angles[1], h->move_blend, h->move_time);
			continue;
		}
		moved += has_moved;
		extra[0] = '\0';
		if (mi->render_buffer_idx == VERTEX_BUFFER_INSTANCED)
		{
			const vk_aliasmodel_t	*am = VK_GetAliasModel ((int)mi->source_buffer_idx - VERTEX_BUFFER_FIRST_MODEL);
			const vk_material_t	*mat = VK_GetMaterial ((int)(mi->material & MATERIAL_INDEX_MASK));

			q_snprintf (state, sizeof(state), "frame %d pose %u<%u %.2f skin %d %s%s%s", e->frame,
				    mi->prim_offset_curr_pose_curr_frame / am->num_pose_verts,
				    mi->prim_offset_prev_pose_curr_frame / am->num_pose_verts, mi->pose_lerp_curr_frame,
				    e->skinnum, mat->name,
				    ((mi->material & MATERIAL_KIND_MASK) == MATERIAL_KIND_TRANSP_MODEL) ? " transp" : "",
				    mat->mask_texture ? " cutout" : "");
			if (e->scale && e->scale != 100)
				q_strlcat (extra, va(" scale %d%%", e->scale), sizeof(extra));
			if (mi->colorshade)
				q_strlcat (extra, va(" tint %u (%.2f %.2f %.2f)", mi->colorshade, mi->tint[0], mi->tint[1], mi->tint[2]), sizeof(extra));
		}
		else
		{
			q_snprintf (state, sizeof(state), "frame %u", mi->alpha_and_frame >> 16);
		}
		if (mi->light >= 0.0f)
			q_strlcat (extra, va(" light %.2f", mi->light), sizeof(extra));
		Con_Printf ("%3d %-6s %4d %-16s org %.1f %.1f %.1f ang %.1f %.1f %.1f cluster %4d prims %u+%u alpha %.2f %s%s%s\n",
				i, kinds[e->kind], e->num, e->model->name,
				mi->transform[3][0], mi->transform[3][1], mi->transform[3][2],
				e->angles[0], e->angles[1], e->angles[2],
				mi->cluster, mi->render_prim_offset, mi->prim_count,
				(mi->alpha_and_frame & 0xffff) == VK_FloatToHalf (1.0f) ? 1.0f : TRANSLUCENT_ALPHA,
				state, extra, has_moved ? " moved" : "");
	}
	if (step_only)
		Con_Printf ("%d stepping entities in frame %d, time %.3f (r_lerpmove %d)\n", steps, r_scene.framecount,
				r_scene.time, r_lerpmove.integer);
	else
		Con_Printf ("%d instances in frame %d (%d alias models), %d moved since the last frame\n", num_instances,
				r_scene.framecount, model_frame.num_instances, moved);
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
	Cvar_RegisterVariable (&r_lerpmodels);
	Cvar_RegisterVariable (&r_lerpmove);
	Cmd_AddCommand ("vk_instances", VK_Instances_f);
}

void VK_ShutdownInstances (void)
{
	int	i;

	for (i = 0; i < VK_FRAMES_IN_FLIGHT; i++)
		VK_DestroyBuffer (&instance_buffers[i]);
	num_instances = 0;
}
