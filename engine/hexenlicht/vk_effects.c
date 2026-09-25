/* vk_effects.c -- particles and sprites as ray-traced geometry
 *
 * Every frame, VK_UpdateEffects turns the scene's particles and sprite
 * entities into triangles, on the CPU, as the GL renderer draws them:
 * each particle is one camera-facing triangle with GL's particle dot
 * texture (r_part.c's R_DrawParticles), each sprite a quad oriented by
 * the sprite's type (gl_rmain.c's R_DrawSpriteModel and R_GetSpriteFrame).
 * They go into a mapped buffer per frame in flight, laid out like Quake II
 * RTX's transparency.c (shaders/hl_shared.h): vertex positions for the
 * acceleration structures, then a color per particle and a texture and
 * alpha per sprite for the shaders. The sprite quads share a static index
 * buffer. vk_accel.c builds a BLAS over each and puts them into the
 * effects TLAS, which the view pass walks for the effects in front of
 * what it hit.
 *
 * vk_effects prints statistics; "vk_effects check" casts a ray at every
 * effect triangle of the last frame (effects_check.comp) and checks that
 * the effects TLAS reports it where it is.
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

COMPILE_TIME_ASSERT(EffectParticle, sizeof(EffectParticle) == 16);	/* the shaders' std430 layouts */
COMPILE_TIME_ASSERT(EffectSprite, sizeof(EffectSprite) == 8);
COMPILE_TIME_ASSERT(EffectsCheckRay, sizeof(EffectsCheckRay) == 32);
COMPILE_TIME_ASSERT(EffectsCheckResult, sizeof(EffectsCheckResult) == 8);
COMPILE_TIME_ASSERT(EffectsCheckPush, sizeof(EffectsCheckPush) == 48);
COMPILE_TIME_ASSERT(max_sprites, MAX_EFFECT_SPRITES >= MAX_SCENE_ENTITIES);
COMPILE_TIME_ASSERT(sprite_indices, MAX_EFFECT_SPRITES * 4 <= 65536);	/* uint16 indices */

#define TRANSLUCENT_ALPHA	0.33f	/* r_wateralpha's default, which GL uses for translucent sprites */

/* a frame's buffer: [positions][EffectParticle x MAX][EffectSprite x MAX] */
#define POSITIONS_SIZE		((VkDeviceSize)(MAX_EFFECT_PARTICLES * 3 + MAX_EFFECT_SPRITES * 4) * 3 * sizeof(float))
#define PARTICLES_OFFSET	((POSITIONS_SIZE + 15) & ~(VkDeviceSize)15)
#define SPRITES_OFFSET		(PARTICLES_OFFSET + (VkDeviceSize)MAX_EFFECT_PARTICLES * sizeof(EffectParticle))
#define EFFECTS_BUFFER_SIZE	(SPRITES_OFFSET + (VkDeviceSize)MAX_EFFECT_SPRITES * sizeof(EffectSprite))

static vk_buffer_t		buffers[VK_FRAMES_IN_FLIGHT];	/* mapped */
static vk_buffer_t		index_buffer;			/* uint16 x 6 per sprite quad */
static vk_effectsframe_t	frames[VK_FRAMES_IN_FLIGHT];
static const vk_effectsframe_t	no_effects;
static int			last_slot = -1;			/* written last, -1 = none since the map loaded */
static int			dropped_frames;			/* frames that left effects out since the map loaded */

GLuint				particletexture;	/* glquake.h; GL defines it in gl_rmain.c */
static float			srgb_to_linear[256];


/* ==========================================================================
 * GL's particle texture (gl_rmisc.c)
 * ========================================================================== */

#define	TEXSIZE		16

static const byte dottexture[TEXSIZE][TEXSIZE] =
{
	{0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},//1
	{0,0,0,1,0,1,0,0,0,0,0,0,1,1,0,0},
	{0,0,0,0,1,0,0,0,0,0,0,1,1,1,1,0},
	{0,1,0,1,1,1,0,1,0,0,0,1,1,1,1,0},//4
	{0,0,1,1,1,1,1,0,0,0,0,0,1,1,0,0},
	{0,1,0,1,1,1,0,1,0,0,0,0,0,0,0,0},
	{0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0},
	{0,0,0,1,0,1,0,0,0,0,0,0,0,0,0,0},//8
	{0,0,0,0,0,0,0,0,0,0,1,1,1,0,0,0},
	{0,0,0,0,0,0,0,0,0,1,1,1,0,1,0,0},
	{0,0,0,0,0,0,0,0,1,1,0,1,1,0,1,0},
	{0,0,0,1,0,0,0,0,1,1,1,1,1,0,1,0},//12
	{0,1,1,1,0,0,0,0,1,1,0,1,1,0,1,0},
	{0,0,1,1,1,0,0,0,0,1,1,1,0,1,0,0},
	{0,0,1,0,0,0,0,0,0,0,1,1,1,0,0,0},
	{0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},//16
};

void R_InitParticleTexture (void)	/* glquake.h */
{
	int	x, y;
	byte	data[TEXSIZE][TEXSIZE][4];

	for (x = 0; x < TEXSIZE; x++)
	{
		for (y = 0; y < TEXSIZE; y++)
		{
			data[y][x][0] = 255;
			data[y][x][1] = 255;
			data[y][x][2] = 255;
			data[y][x][3] = dottexture[x][y]*255;
		}
	}
	/* GL loads it without a name; ours shows in vk_textures list */
	particletexture = GL_LoadTexture ("#particle", (byte *)data, TEXSIZE, TEXSIZE, TEX_ALPHA | TEX_RGBA | TEX_LINEAR);
}

int VK_ParticleTexture (void)
{
	return (int)particletexture;
}


/* ==========================================================================
 * Particles
 * ========================================================================== */

/* one camera-facing triangle per particle, as R_DrawParticles draws it:
 * the particle's origin and 1.5 units (scaled up with the distance) up and
 * to the right; GL's texture coordinates are the shader's */
static void WriteParticles (vk_effectsframe_t *f, float *pos, EffectParticle *out)
{
	const particle_t	*p;
	vec3_t			pup, pright;
	float			scale, alpha;
	int			n = 0, color, uvs, k;
	const byte		*rgba;

	VectorScale (r_scene.up, 1.5f, pup);
	VectorScale (r_scene.right, 1.5f, pright);

	for (p = r_scene.particles; p; p = p->next)
	{
		if (n >= MAX_EFFECT_PARTICLES)
		{
			f->dropped_particles++;
			continue;
		}

		/* hack a scale up to keep particles from disappearing */
		scale = (p->org[0] - r_scene.vieworg[0]) * r_scene.forward[0] +
			(p->org[1] - r_scene.vieworg[1]) * r_scene.forward[1] +
			(p->org[2] - r_scene.vieworg[2]) * r_scene.forward[2];
#define	SCALE_BASE	((p->type == pt_snow) ? p->count/10 : 1)
		if (scale < 20)
			scale = SCALE_BASE;
		else
			scale = SCALE_BASE + scale * 0.004f;
#undef	SCALE_BASE

		/* clamp the color to 0-511: pt_c_explode and pt_c_explode2
		 * count theirs down below 0 (see r_part.c); 256-511 are
		 * translucent colors */
		color = ((int)p->color) & 0x01ff;
		if (color < 256)
		{
			rgba = (const byte *) &d_8to24table[color];
			alpha = 1.0f;		/* glColor3 */
		}
		else
		{
			rgba = (const byte *) &d_8to24TranslucentTable[color - 256];
			alpha = rgba[3] / 255.0f;
		}

		uvs = 0;
		if (p->type == pt_snow)
		{
			if (p->count >= 69)
				uvs = 3;	/* happy snow! */
			else if (p->count >= 40)
				uvs = 2;
			else if (p->count >= 30)
				uvs = 1;
		}

		for (k = 0; k < 3; k++)
			out[n].color[k] = srgb_to_linear[rgba[k]];
		out[n].alpha_and_uvs = VK_FloatToHalf (alpha) | ((uint32_t)uvs << 16);

		VectorCopy (p->org, pos);
		VectorMA (p->org, scale, pup, pos + 3);
		VectorMA (p->org, scale, pright, pos + 6);
		pos += 9;
		n++;
	}
	f->num_particles = n;
}


/* ==========================================================================
 * Sprites
 * ========================================================================== */

/* R_GetSpriteFrame: the frame, or in a frame group the image for the time */
static mspriteframe_t *GetSpriteFrame (const scene_entity_t *e, int *bad_frames)
{
	msprite_t	*psprite = (msprite_t *) e->model->cache.data;
	mspritegroup_t	*pspritegroup;
	int		i, numframes, frame = e->frame;
	float		*pintervals, fullinterval, targettime, time;

	if (frame >= psprite->numframes || frame < 0)
	{
		(*bad_frames)++;	/* GL prints it with Con_DPrintf */
		frame = 0;
	}

	if (psprite->frames[frame].type == SPR_SINGLE)
		return psprite->frames[frame].frameptr;

	pspritegroup = (mspritegroup_t *) psprite->frames[frame].frameptr;
	pintervals = pspritegroup->intervals;
	numframes = pspritegroup->numframes;
	fullinterval = pintervals[numframes-1];

	time = (float)(r_scene.time + e->syncbase);

	/* Mod_LoadSpriteGroup made all intervals positive */
	targettime = time - ((int)(time / fullinterval)) * fullinterval;

	for (i = 0; i < (numframes-1); i++)
	{
		if (pintervals[i] > targettime)
			break;
	}
	return pspritegroup->frames[i];
}

/* R_DrawSpriteModel's axes for the sprite's type; false where GL draws
 * nothing: an upright sprite seen from within 1 degree of straight up or
 * down */
static qboolean SpriteAxes (const scene_entity_t *e, const msprite_t *psprite, vec3_t up, vec3_t right)
{
	vec3_t	tvec, forward, angles;
	float	dot, angle, sr, cr;
	int	i;

	switch (psprite->type)
	{
	case SPR_FACING_UPRIGHT:
		/* up straight up, right perpendicular to the direction to the
		 * camera. GL uses the modelorg of the model it drew before;
		 * this is the sprite's own (no Hexen II sprite has this type) */
		VectorSubtract (e->origin, r_scene.vieworg, tvec);
		VectorNormalize (tvec);
		dot = tvec[2];
		if ((dot > 0.999848f) || (dot < -0.999848f))	/* cos(1 degree) */
			return false;
		VectorSet (up, 0, 0, 1);
		VectorSet (right, tvec[1], -tvec[0], 0);
		VectorNormalize (right);
		return true;

	case SPR_VP_PARALLEL:
		/* parallel to the view plane */
		VectorCopy (r_scene.up, up);
		VectorCopy (r_scene.right, right);
		return true;

	case SPR_VP_PARALLEL_UPRIGHT:
		/* up straight up, right parallel to the view plane */
		dot = r_scene.forward[2];
		if ((dot > 0.999848f) || (dot < -0.999848f))
			return false;
		VectorSet (up, 0, 0, 1);
		VectorSet (right, r_scene.forward[1], -r_scene.forward[0], 0);
		VectorNormalize (right);
		return true;

	case SPR_ORIENTED:
		/* the sprite's own orientation */
		VectorCopy (e->angles, angles);	/* AngleVectors takes a non-const vector */
		AngleVectors (angles, forward, right, up);
		return true;

	case SPR_VP_PARALLEL_ORIENTED:
		/* parallel to the view plane, rolled by the entity's roll */
		angle = e->angles[ROLL] * (float)(M_PI*2 / 360);
		sr = sinf (angle);
		cr = cosf (angle);
		for (i = 0; i < 3; i++)
		{
			right[i] = r_scene.right[i] * cr + r_scene.up[i] * sr;
			up[i] = r_scene.right[i] * -sr + r_scene.up[i] * cr;
		}
		return true;

	default:	/* GL: Sys_Error */
		return false;
	}
}

/* one quad per sprite entity, R_DrawSpriteModel's corners in the order of
 * its texture coordinates (0,1) (0,0) (1,0) (1,1), which is Quake II RTX's */
static void WriteSprites (vk_effectsframe_t *f, float *pos, EffectSprite *out)
{
	const scene_entity_t	*e;
	const msprite_t		*psprite;
	const mspriteframe_t	*frame;
	vec3_t			up, right, down_pt, up_pt;
	int			i, n = 0;

	for (i = 0; i < r_scene.num_entities; i++)
	{
		e = &r_scene.entities[i];
		if (e->model->type != mod_sprite || e->kind == SCENE_ENT_VIEWMODEL)
			continue;
		if (n >= MAX_EFFECT_SPRITES)
		{
			f->dropped_sprites++;
			continue;
		}

		frame = GetSpriteFrame (e, &f->bad_frames);
		psprite = (const msprite_t *) e->model->cache.data;
		if (!SpriteAxes (e, psprite, up, right))
		{
			if (psprite->type == SPR_FACING_UPRIGHT || psprite->type == SPR_VP_PARALLEL_UPRIGHT)
				f->edge_on++;
			continue;
		}

		out[n].texture = (uint32_t)frame->gl_texturenum;
		/* GL: blended with the texture's alpha, times r_wateralpha if
		 * translucent; unlit (GL_REPLACE, or white GL_MODULATE) */
		out[n].alpha = ((e->drawflags & DRF_TRANSLUCENT) || (e->model->flags & EF_TRANSPARENT)) ?
				TRANSLUCENT_ALPHA : 1.0f;

		VectorMA (e->origin, frame->down, up, down_pt);
		VectorMA (e->origin, frame->up, up, up_pt);
		VectorMA (down_pt, frame->left, right, pos);
		VectorMA (up_pt, frame->left, right, pos + 3);
		VectorMA (up_pt, frame->right, right, pos + 6);
		VectorMA (down_pt, frame->right, right, pos + 9);
		pos += 12;
		n++;
	}
	f->num_sprites = n;
}


/* ==========================================================================
 * The frame's effects
 * ========================================================================== */

/* in R_RenderView, before VK_BuildTLAS */
void VK_UpdateEffects (void)
{
	int			slot = (int)vk.frame_index;
	vk_effectsframe_t	*f = &frames[slot];
	vk_buffer_t		*b = &buffers[slot];
	float			*pos = (float *) b->mapped;

	if (!vk.frame_active)
		return;

	/* this slot's fence was waited for: its buffer is free */
	memset (f, 0, sizeof(*f));
	WriteParticles (f, pos, (EffectParticle *) ((byte *) b->mapped + PARTICLES_OFFSET));
	WriteSprites (f, pos + f->num_particles * 9, (EffectSprite *) ((byte *) b->mapped + SPRITES_OFFSET));
	if (f->dropped_particles || f->dropped_sprites)
		dropped_frames++;

	f->frame_count = vk.frame_count;
	f->positions = b->address;
	f->sprite_positions = b->address + (VkDeviceSize)f->num_particles * 9 * sizeof(float);
	f->particles = b->address + PARTICLES_OFFSET;
	f->sprites = b->address + SPRITES_OFFSET;
	f->indices = index_buffer.address;
	VK_CHECK (vmaFlushAllocation (vk.allocator, b->allocation, 0, VK_WHOLE_SIZE));
	last_slot = slot;
}

const vk_effectsframe_t *VK_EffectsFrame (void)
{
	const vk_effectsframe_t	*f = &frames[vk.frame_index];

	return (vk.frame_active && f->frame_count == vk.frame_count && f->positions) ? f : &no_effects;
}

void VK_ClearEffects (void)
{
	dropped_frames = 0;
	last_slot = -1;
}


/* ==========================================================================
 * vk_effects, and the check through the effects TLAS
 * ========================================================================== */

/* a ray from the camera at the centroid of triangle a b c: false if it
 * would graze it or the triangle has no area */
static qboolean CheckRay (const float *a, const float *b, const float *c, uint32_t custom, uint32_t primitive,
			  EffectsCheckRay *ray, float *dist)
{
	vec3_t	e1, e2, n, center;
	float	len;
	int	k;

	VectorSubtract (b, a, e1);
	VectorSubtract (c, a, e2);
	CrossProduct (e1, e2, n);
	if (VectorNormalize (n) < 1e-6f)
		return false;
	for (k = 0; k < 3; k++)
		center[k] = (a[k] + b[k] + c[k]) / 3.0f;
	VectorSubtract (center, r_scene.vieworg, ray->dir);
	len = VectorNormalize (ray->dir);
	if (len < 0.1f || fabsf (DotProduct (n, ray->dir)) < 0.02f)
		return false;
	ray->tmax = len * 1.001f + 0.1f;
	ray->custom = custom;
	ray->primitive = primitive;
	ray->pad0 = ray->pad1 = 0;
	*dist = len;
	return true;
}

static void VK_EffectsCheck (void)
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
	vk_buffer_t			rays, results;
	EffectsCheckPush		push;
	EffectsCheckRay			*ray;
	const EffectsCheckResult	*res;
	const vk_effectsframe_t		*f;
	const float			*pos;
	float				*dists;
	int				slot, i, num_rays = 0, skipped = 0, agree = 0, missing = 0, wrong = 0, printed = 0;
	uint32_t			max_candidates = 0;
	double				sum_candidates = 0.0;
	float				max_diff = 0.0f;
	uint64_t			built;
	VkDeviceAddress			tlas;

	tlas = VK_LastEffectsTLAS (&slot, &built);
	f = &frames[slot];
	if (!tlas || f->frame_count != built)
	{
		Con_Printf ("No effects TLAS: no particles or sprites last frame\n");
		return;
	}
	vkDeviceWaitIdle (vk.device);	/* the last frame's TLAS is built */

	VK_CreateBuffer (&rays, (VkDeviceSize)(f->num_particles + f->num_sprites * 2) * sizeof(EffectsCheckRay),
			 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VK_MEMORY_UPLOAD);
	VK_CreateBuffer (&results, (VkDeviceSize)(f->num_particles + f->num_sprites * 2) * sizeof(EffectsCheckResult),
			 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VK_MEMORY_READBACK);
	dists = (float *) malloc ((f->num_particles + f->num_sprites * 2) * sizeof(float));
	if (!dists)
		Sys_Error ("%s: out of memory", __thisfunc__);

	/* a ray at every particle triangle and at both triangles of every
	 * sprite quad, from the buffer the TLAS was built from */
	pos = (const float *) buffers[slot].mapped;
	ray = (EffectsCheckRay *) rays.mapped;
	for (i = 0; i < f->num_particles; i++)
	{
		const float	*v = pos + i * 9;

		if (CheckRay (v, v + 3, v + 6, EFFECTS_PARTICLES, (uint32_t)i, &ray[num_rays], &dists[num_rays]))
			num_rays++;
		else
			skipped++;
	}
	pos += f->num_particles * 9;
	for (i = 0; i < f->num_sprites; i++)
	{
		const float	*v = pos + i * 12;

		/* the index buffer's 0 1 2 and 2 3 0 */
		if (CheckRay (v, v + 3, v + 6, EFFECTS_SPRITES, (uint32_t)(i * 2), &ray[num_rays], &dists[num_rays]))
			num_rays++;
		else
			skipped++;
		if (CheckRay (v + 6, v + 9, v, EFFECTS_SPRITES, (uint32_t)(i * 2 + 1), &ray[num_rays], &dists[num_rays]))
			num_rays++;
		else
			skipped++;
	}
	VK_CHECK (vmaFlushAllocation (vk.allocator, rays.allocation, 0, VK_WHOLE_SIZE));

	if (num_rays)
	{
		module = VK_LoadShader ("effects_check.comp");
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

		memset (&push, 0, sizeof(push));
		push.tlas = tlas;
		push.rays = rays.address;
		push.results = results.address;
		push.num_rays = (uint32_t)num_rays;
		VectorCopy (r_scene.vieworg, push.origin);

		cmd = VK_BeginUpload ();
		vkCmdBindPipeline (cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
		vkCmdPushConstants (cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
		vkCmdDispatch (cmd, ((uint32_t)num_rays + 63) / 64, 1, 1);
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

		vkDestroyPipeline (vk.device, pipeline, NULL);
		vkDestroyPipelineLayout (vk.device, layout, NULL);

		/* reported at the distance of the point it was cast at? */
		res = (const EffectsCheckResult *) results.mapped;
		for (i = 0; i < num_rays; i++)
		{
			float	diff = fabsf (res[i].t - dists[i]);

			sum_candidates += res[i].candidates;
			max_candidates = q_max (max_candidates, res[i].candidates);
			if (res[i].t < 0.0f)
				missing++;
			else if (diff > dists[i] * 1e-3f + 0.01f)
				wrong++;
			else
			{
				agree++;
				max_diff = q_max (max_diff, diff);
				continue;
			}
			if (printed++ < 5)
			{
				Con_Printf ("%s %u: expected at %.2f, %s %.2f (%u candidates)\n",
						(ray[i].custom == EFFECTS_PARTICLES) ? "particle triangle" : "sprite triangle",
						ray[i].primitive, dists[i], (res[i].t < 0.0f) ? "not reported" : "reported at",
						res[i].t, res[i].candidates);
			}
		}
	}

	Con_Printf ("effects check: %d particles, %d sprites; %d rays from %.0f %.0f %.0f: %d agree (max difference %.4f), "
		    "%d not reported, %d at the wrong distance; %d triangles skipped (seen edge-on or no area); "
		    "candidates per ray %.1f on average, %u at most\n",
			f->num_particles, f->num_sprites, num_rays, r_scene.vieworg[0], r_scene.vieworg[1],
			r_scene.vieworg[2], agree, max_diff, missing, wrong, skipped,
			num_rays ? sum_candidates / num_rays : 0.0, max_candidates);

	free (dists);
	VK_DestroyBuffer (&results);
	VK_DestroyBuffer (&rays);
}

static void VK_Effects_f (void)
{
	const vk_effectsframe_t	*f;

	if (cls.signon != SIGNONS || !r_scene.worldmodel || last_slot < 0)
	{
		Con_Printf ("Not in a map\n");
		return;
	}
	if (Cmd_Argc () > 1 && !q_strcasecmp (Cmd_Argv (1), "check"))
	{
		VK_EffectsCheck ();
		return;
	}

	f = &frames[last_slot];
	Con_Printf ("last frame: %d particles (of %d in the scene), %d sprites\n", f->num_particles,
			r_scene.num_particles, f->num_sprites);
	Con_Printf ("left out: %d particles and %d sprites (no room; in %d frames since the map loaded), %d upright "
		    "sprites seen from straight above or below (as GL); bad sprite frame numbers: %d\n",
			f->dropped_particles, f->dropped_sprites, dropped_frames, f->edge_on, f->bad_frames);
	Con_Printf ("effects buffer: %.2f MB per frame in flight, room for %d particles and %d sprites\n",
			EFFECTS_BUFFER_SIZE / (1024.0 * 1024.0), MAX_EFFECT_PARTICLES, MAX_EFFECT_SPRITES);
	VK_PrintEffectsAccel ();
}


/* ==========================================================================
 * Init / shutdown
 * ========================================================================== */

void VK_InitEffects (void)
{
	uint16_t	*indices;
	int		i;

	for (i = 0; i < 256; i++)
	{
		float	c = i / 255.0f;

		srgb_to_linear[i] = (c <= 0.04045f) ? c / 12.92f : powf ((c + 0.055f) / 1.055f, 2.4f);
	}

	for (i = 0; i < VK_FRAMES_IN_FLIGHT; i++)
	{
		VK_CreateBuffer (&buffers[i], EFFECTS_BUFFER_SIZE, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
				 VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
				 VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, VK_MEMORY_UPLOAD);
	}

	/* Quake II RTX's fill_index_buffer */
	indices = (uint16_t *) malloc (MAX_EFFECT_SPRITES * 6 * sizeof(uint16_t));
	if (!indices)
		Sys_Error ("%s: out of memory", __thisfunc__);
	for (i = 0; i < MAX_EFFECT_SPRITES; i++)
	{
		uint16_t	*quad = indices + i * 6;
		uint16_t	base_vertex = (uint16_t)(i * 4);

		quad[0] = base_vertex + 0;
		quad[1] = base_vertex + 1;
		quad[2] = base_vertex + 2;
		quad[3] = base_vertex + 2;
		quad[4] = base_vertex + 3;
		quad[5] = base_vertex + 0;
	}
	VK_CreateBuffer (&index_buffer, MAX_EFFECT_SPRITES * 6 * sizeof(uint16_t), VK_BUFFER_USAGE_TRANSFER_DST_BIT |
			 VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
			 VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, VK_MEMORY_DEVICE);
	VK_UploadBuffer (&index_buffer, 0, indices, MAX_EFFECT_SPRITES * 6 * sizeof(uint16_t));
	free (indices);

	R_InitParticleTexture ();

	Cmd_AddCommand ("vk_effects", VK_Effects_f);
}

void VK_ShutdownEffects (void)
{
	int	i;

	for (i = 0; i < VK_FRAMES_IN_FLIGHT; i++)
		VK_DestroyBuffer (&buffers[i]);
	VK_DestroyBuffer (&index_buffer);
	memset (frames, 0, sizeof(frames));
}
