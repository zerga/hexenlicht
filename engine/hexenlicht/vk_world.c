/* vk_world.c -- the BSP world in static GPU buffers
 *
 * On map load, every surface of the world and of its brush submodels (*1,
 * *2, ...) becomes triangles in Quake II RTX's VboPrimitive format (see
 * shaders/hl_shared.h): positions, texture coordinates, flat normals,
 * tangents, a material ID (kind, flags, material table index) and, for
 * the world, the vis leaf the triangle is in. Each model's triangles are
 * grouped into opaque, transparent and sky ranges, for the acceleration
 * structures. The world's textures become materials, with their animation
 * sequences (+0..+9) and alternate sequences (+a..+j).
 *
 * The structure follows Quake II RTX's bsp_mesh.c; encode_normal and
 * get_triangle_off_center are ported from it.
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

#define ANIM_CYCLE	2	/* gl_model.c: tenths of a second per animation frame */
#define TRANSLUCENT_ALPHA 0.33f	/* r_wateralpha's default, for *rtex078 and *lowlight */

enum { PASS_OPAQUE, PASS_TRANSPARENT, PASS_SKY, NUM_PASSES };

COMPILE_TIME_ASSERT(VboPrimitive, sizeof(VboPrimitive) == 128);	/* the shaders' layout */

vk_world_t	vk_world;

/* statistics for vk_world, from the last build */
static struct
{
	uint32_t	into_solid;		/* world triangles left out: facing into solid */
	uint32_t	kinds[16];		/* triangles per MATERIAL_KIND */
	int		num_textures;		/* world textures that became materials */
	double		build_time;
} stats;

/* the world model's textures and their materials; the loader substitutes
 * r_notexture_mip for missing ones, which gets a material of its own */
static int	*texture_materials;		/* [worldmodel->numtextures], 0 = none */
static int	notexture_material;


/* ==========================================================================
 * Helpers ported from Quake II RTX
 * ========================================================================== */

/* octahedral normal encoding, the same as utils.glsl's encode_normal */
static uint32_t encode_normal (const vec3_t normal)
{
	float		inv_l1 = 1.0f / (fabsf(normal[0]) + fabsf(normal[1]) + fabsf(normal[2]));
	float		p[2], pp[2];
	uint32_t	ux, uy;

	p[0] = normal[0] * inv_l1;
	p[1] = normal[1] * inv_l1;
	pp[0] = p[0];
	pp[1] = p[1];
	if (normal[2] < 0.0f)
	{
		pp[0] = (1.0f - fabsf(p[1])) * ((p[0] >= 0.0f) ? 1.0f : -1.0f);
		pp[1] = (1.0f - fabsf(p[0])) * ((p[1] >= 0.0f) ? 1.0f : -1.0f);
	}
	pp[0] = q_min (q_max (pp[0] * 0.5f + 0.5f, 0.0f), 1.0f);
	pp[1] = q_min (q_max (pp[1] * 0.5f + 0.5f, 0.0f), 1.0f);

	ux = (uint32_t)(pp[0] * 0xffffu);
	uy = (uint32_t)(pp[1] * 0xffffu);
	return ux | (uy << 16);
}

/* a point a little above the center of the triangle (on the side its
 * winding faces), so that it is inside a leaf rather than on a plane */
static void get_triangle_off_center (const VboPrimitive *p, vec3_t center, float offset)
{
	vec3_t	e1, e2, normal;

	VectorAdd (p->pos0, p->pos1, center);
	VectorAdd (center, p->pos2, center);
	VectorScale (center, 1.0f / 3.0f, center);

	VectorSubtract (p->pos1, p->pos0, e1);
	VectorSubtract (p->pos2, p->pos0, e2);
	CrossProduct (e1, e2, normal);
	VectorNormalize (normal);
	VectorMA (center, offset, normal, center);
}


/* ==========================================================================
 * Materials
 * ========================================================================== */

static int TextureIndex (const qmodel_t *m, const texture_t *tx)
{
	int	i;

	for (i = 0; i < m->numtextures; i++)
	{
		if (m->textures[i] == tx)
			return i;
	}
	return -1;
}

static int MaterialForTexture (const qmodel_t *m, texture_t *tx)
{
	int	i = TextureIndex (m, tx);

	if (i >= 0)
		return texture_materials[i];
	if (!notexture_material)
		notexture_material = VK_AddMaterial ("notexture", (int)tx->gl_texturenum);
	return notexture_material;
}

/* the first frame of tx's animation, which surfaces reference so that the
 * frame shown only depends on the time, as in R_TextureAnimation */
static texture_t *FirstFrame (texture_t *tx)
{
	int	count;

	for (count = 0; tx->anim_total && tx->anim_min != 0 && count < 10; count++)
		tx = tx->anim_next;
	return tx;
}

static void AddWorldMaterials (qmodel_t *m)
{
	vk_material_t	*mat;
	texture_t	*tx;
	int		i;

	texture_materials = (int *) calloc (q_max (m->numtextures, 1), sizeof(int));
	if (!texture_materials)
		Sys_Error ("%s: out of memory", __thisfunc__);
	notexture_material = 0;

	for (i = 0; i < m->numtextures; i++)
	{
		if ((tx = m->textures[i]) != NULL)
			texture_materials[i] = VK_AddMaterial (tx->name, (int)tx->gl_texturenum);
	}
	stats.num_textures = vk_num_materials - 1;

	/* the animation sequences, as gl_model.c's Mod_LoadTextures linked them */
	for (i = 0; i < m->numtextures; i++)
	{
		if ((tx = m->textures[i]) == NULL)
			continue;
		mat = VK_GetMaterial (texture_materials[i]);
		if (tx->anim_total)
		{
			mat->num_frames = tx->anim_total / ANIM_CYCLE;
			mat->next_frame = MaterialForTexture (m, tx->anim_next);
		}
		if (tx->alternate_anims)
			mat->alternate = MaterialForTexture (m, tx->alternate_anims);
	}
}


/* ==========================================================================
 * Surfaces to triangles
 * ========================================================================== */

/* the material ID bits for a surface's kind and flags */
static uint32_t SurfaceKind (const msurface_t *surf)
{
	const char	*name = surf->texinfo->texture->name;
	uint32_t	kind;

	if (surf->flags & SURF_DRAWSKY)
		return MATERIAL_KIND_SKY;
	if (!(surf->flags & SURF_DRAWTURB))
		return MATERIAL_KIND_REGULAR;

	if (surf->flags & SURF_TRANSLUCENT)
		kind = MATERIAL_KIND_TRANSPARENT;
	else if (!q_strncasecmp (name, "*lava", 5))
		kind = MATERIAL_KIND_LAVA;
	else if (!q_strncasecmp (name, "*slime", 6))
		kind = MATERIAL_KIND_SLIME;
	else
		kind = MATERIAL_KIND_WATER;
	return kind | MATERIAL_FLAG_WARP;
}

/* the acceleration structure group, as Quake II RTX sorts them */
static int SurfacePass (uint32_t kind)
{
	switch (kind & MATERIAL_KIND_MASK)
	{
	case MATERIAL_KIND_SKY:
		return PASS_SKY;
	case MATERIAL_KIND_WATER:
	case MATERIAL_KIND_SLIME:
	case MATERIAL_KIND_TRANSPARENT:
		return PASS_TRANSPARENT;
	default:
		return PASS_OPAQUE;
	}
}

static const float *SurfaceVertex (const qmodel_t *m, const msurface_t *surf, int k)
{
	int	lindex = m->surfedges[surf->firstedge + k];

	if (lindex > 0)
		return m->vertexes[m->edges[lindex].v[0]].position;
	return m->vertexes[m->edges[-lindex].v[1]].position;
}

/* the vis leaf of a world triangle, Quake II RTX's way: the leaf just in
 * front of its center */
static int TriangleLeaf (qmodel_t *world, const VboPrimitive *p)
{
	vec3_t	center;
	int	leaf;

	get_triangle_off_center (p, center, 0.01f);
	leaf = (int)(Mod_PointInLeaf (center, world) - world->leafs) - 1;
	if (leaf < 0)
	{	/* the offset was too small to leave the plane: try a larger one */
		get_triangle_off_center (p, center, 1.0f);
		leaf = (int)(Mod_PointInLeaf (center, world) - world->leafs) - 1;
	}
	return q_max (leaf, -1);
}

/* Writes the surface's triangles to out and returns how many (out == NULL:
 * the most there can be). World triangles facing into solid are left out:
 * Hexen II's qbsp leaves some faces whose front is inside a wall, back to
 * back with the real wall face. The GL renderer never shows them, since
 * faces are only seen from the front; rays could hit them. */
static uint32_t EmitSurface (qmodel_t *m, msurface_t *surf, uint32_t material_id, qboolean world, VboPrimitive *out)
{
	const mtexinfo_t	*ti = surf->texinfo;
	const texture_t		*tx = ti->texture;
	uint32_t		num_tris, i, n, enc_normal, enc_tangent, emissive_and_alpha;
	vec3_t			normal, tangent, bitangent;
	const float		*v;
	int			k, corner[3];

	if (surf->numedges < 3)
		return 0;
	num_tris = (uint32_t)surf->numedges - 2;
	if (!out)
		return num_tris;

	/* flat normal; the tangent is the texture's s axis in the plane */
	VectorCopy (surf->plane->normal, normal);
	if (surf->flags & SURF_PLANEBACK)
		VectorInverse (normal);
	VectorMA (ti->vecs[0], -DotProduct (normal, ti->vecs[0]), normal, tangent);
	if (VectorNormalize (tangent) == 0.0f)
	{	/* texture axis along the normal: any direction in the plane */
		vec3_t	axis = { 0.0f, 0.0f, 0.0f };

		axis[(fabsf (normal[2]) < 0.9f) ? 2 : 0] = 1.0f;
		CrossProduct (axis, normal, tangent);
		VectorNormalize (tangent);
	}
	CrossProduct (normal, tangent, bitangent);
	if (DotProduct (bitangent, ti->vecs[1]) < 0.0f)
		material_id |= MATERIAL_FLAG_HANDEDNESS;
	enc_normal = encode_normal (normal);
	enc_tangent = encode_normal (tangent);

	emissive_and_alpha = VK_FloatToHalf (1.0f) |
		((uint32_t)VK_FloatToHalf (((material_id & MATERIAL_KIND_MASK) == MATERIAL_KIND_TRANSPARENT) ?
					  TRANSLUCENT_ALPHA : 1.0f) << 16);

	for (i = n = 0; i < num_tris; i++)
	{
		float	*pos[3], *uv[3];

		memset (out, 0, sizeof(*out));
		pos[0] = out->pos0;	uv[0] = out->uv0;
		pos[1] = out->pos1;	uv[1] = out->uv1;
		pos[2] = out->pos2;	uv[2] = out->uv2;

		/* a fan in Quake II RTX's corner order */
		corner[0] = 0;
		corner[1] = (int)i + 2;
		corner[2] = (int)i + 1;
		for (k = 0; k < 3; k++)
		{
			v = SurfaceVertex (m, surf, corner[k]);
			VectorCopy (v, pos[k]);
			uv[k][0] = (DotProduct (v, ti->vecs[0]) + ti->vecs[0][3]) / tx->width;
			uv[k][1] = (DotProduct (v, ti->vecs[1]) + ti->vecs[1][3]) / tx->height;
			out->normals[k] = enc_normal;
			out->tangents[k] = enc_tangent;
		}

		out->material_id = material_id;
		out->emissive_and_alpha = emissive_and_alpha;
		out->cluster = world ? TriangleLeaf (m, out) : -1;
		if (world && out->cluster < 0)
		{
			stats.into_solid++;
			continue;	/* overwritten by the next one */
		}
		stats.kinds[(material_id & MATERIAL_KIND_MASK) >> 28]++;
		out++;
		n++;
	}

	return n;
}

/* all triangles of the world and its submodels, grouped per model and pass;
 * out == NULL only counts them */
static uint32_t EmitModels (qmodel_t *world, VboPrimitive *out)
{
	uint32_t	n = 0;
	int		mi, pass, s;

	for (mi = 0; mi < world->numsubmodels; mi++)
	{
		const dmodel_t	*bm = &world->submodels[mi];

		for (pass = 0; pass < NUM_PASSES; pass++)
		{
			vk_primrange_t	*range = NULL;

			if (out)
			{
				vk_bspmodel_t	*bsp = &vk_world.models[mi];

				range = (pass == PASS_OPAQUE) ? &bsp->opaque :
					(pass == PASS_TRANSPARENT) ? &bsp->transparent : &bsp->sky;
				range->first = n;
			}
			for (s = bm->firstface; s < bm->firstface + bm->numfaces; s++)
			{
				msurface_t	*surf = &world->surfaces[s];
				uint32_t	kind = SurfaceKind (surf);
				uint32_t	material;

				if (SurfacePass (kind) != pass)
					continue;
				material = out ? (uint32_t)MaterialForTexture (world, FirstFrame (surf->texinfo->texture)) : 0;
				n += EmitSurface (world, surf, kind | (material & MATERIAL_INDEX_MASK),
						  mi == 0, out ? out + n : NULL);
			}
			if (range)
				range->count = n - range->first;
		}
	}
	return n;
}


/* ==========================================================================
 * Building and freeing
 * ========================================================================== */

static void VK_FreeWorld (void)
{
	if (vk.device)
		VK_DestroyBuffer (&vk_world.buffer);
	free (vk_world.models);
	free (texture_materials);
	texture_materials = NULL;
	memset (&vk_world, 0, sizeof(vk_world));
}

void VK_LoadWorld (qmodel_t *worldmodel)
{
	VboPrimitive	*prims;
	float		*positions;
	byte		*data;
	size_t		prims_size, size;
	uint32_t	i;
	double		start = Sys_DoubleTime ();

	vkDeviceWaitIdle (vk.device);	/* frames in flight may still use the old buffers */
	VK_FreeWorld ();
	VK_ClearMaterials ();
	memset (&stats, 0, sizeof(stats));

	vk_world.worldmodel = worldmodel;
	vk_world.num_models = worldmodel->numsubmodels;
	vk_world.models = (vk_bspmodel_t *) calloc (q_max (vk_world.num_models, 1), sizeof(vk_bspmodel_t));
	if (!vk_world.models)
		Sys_Error ("%s: out of memory", __thisfunc__);

	AddWorldMaterials (worldmodel);

	/* count, then fill: [VboPrimitive x n][positions x n] */
	i = EmitModels (worldmodel, NULL);	/* at most */
	data = (byte *) malloc (q_max (i * (sizeof(VboPrimitive) + 9 * sizeof(float)), 1));
	if (!data)
		Sys_Error ("%s: out of memory", __thisfunc__);
	prims = (VboPrimitive *) data;

	vk_world.num_primitives = EmitModels (worldmodel, prims);
	prims_size = vk_world.num_primitives * sizeof(VboPrimitive);
	size = prims_size + vk_world.num_primitives * 9 * sizeof(float);
	positions = (float *) (data + prims_size);
	for (i = 0; i < vk_world.num_primitives; i++)
	{
		VectorCopy (prims[i].pos0, positions + i * 9 + 0);
		VectorCopy (prims[i].pos1, positions + i * 9 + 3);
		VectorCopy (prims[i].pos2, positions + i * 9 + 6);
	}

	if (size)
	{
		VK_CreateBuffer (&vk_world.buffer, size,
				 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
				 VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
				 VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, false);
		VK_UploadBuffer (&vk_world.buffer, 0, data, size);
	}
	vk_world.positions_offset = prims_size;
	free (data);

	VK_UploadMaterials ();
	stats.build_time = Sys_DoubleTime () - start;
}


/* ==========================================================================
 * vk_world: statistics and a check of the animation table
 * ========================================================================== */

/* Hexen II's texture animation (gl_rsurf.c's R_TextureAnimation) */
static texture_t *GL_TextureAnimation (texture_t *base, qboolean alternate, double time)
{
	int	relative, count;

	if (alternate && base->alternate_anims)
		base = base->alternate_anims;
	if (!base->anim_total)
		return base;

	relative = (int)(time * 10) % base->anim_total;
	for (count = 0; base->anim_min > relative || base->anim_max <= relative; count++)
	{
		base = base->anim_next;
		if (!base || count > 100)
			return NULL;
	}
	return base;
}

/* the table walk of material.glsl's animate_material */
static int AnimateMaterial (int material, int frame, qboolean alternate)
{
	const vk_material_t	*m = VK_GetMaterial (material);

	if (alternate && m->alternate)
	{
		material = m->alternate;
		m = VK_GetMaterial (material);
	}
	if (frame > 0 && m->num_frames > 1)
	{
		for (frame %= m->num_frames; frame > 0; frame--)
		{
			material = m->next_frame;
			m = VK_GetMaterial (material);
		}
	}
	return material;
}

/* compares both for every animated texture over 20 seconds of time steps;
 * returns the number of mismatches */
static int CheckAnimations (qmodel_t *world, int *num_checked)
{
	texture_t	*tx, *expected;
	int		i, step, alt, start, mismatches = 0;
	double		time;

	*num_checked = 0;
	for (i = 0; i < world->numtextures; i++)
	{
		tx = world->textures[i];
		if (!tx || !(tx->anim_total || tx->alternate_anims))
			continue;
		(*num_checked)++;
		start = MaterialForTexture (world, FirstFrame (tx));
		for (alt = 0; alt < 2; alt++)
		{
			for (step = 0; step < 200; step++)
			{
				time = step * 0.1 + 0.05;	/* away from frame boundaries */
				expected = GL_TextureAnimation (tx, alt, time);
				if (!expected || MaterialForTexture (world, expected) !=
						 AnimateMaterial (start, (int)(time * 5), alt))
					mismatches++;
			}
		}
	}
	return mismatches;
}

static const char *KindName (uint32_t kind)
{
	switch (kind & MATERIAL_KIND_MASK)
	{
	case MATERIAL_KIND_REGULAR:	return "regular";
	case MATERIAL_KIND_WATER:	return "water";
	case MATERIAL_KIND_LAVA:	return "lava";
	case MATERIAL_KIND_SLIME:	return "slime";
	case MATERIAL_KIND_SKY:		return "sky";
	case MATERIAL_KIND_TRANSPARENT:	return "transparent";
	default:			return "other";
	}
}

static void VK_World_f (void)
{
	qmodel_t	*world = vk_world.worldmodel;
	uint32_t	sub[NUM_PASSES] = { 0, 0, 0 };
	int		i, checked, mismatches;
	qboolean	list = (Cmd_Argc() > 1 && !q_strcasecmp (Cmd_Argv(1), "materials"));

	if (!world || cls.state != ca_connected)
	{
		Con_Printf ("No world loaded\n");
		return;
	}

	Con_Printf ("World %s: %u triangles, %d models, built in %.0f ms\n", world->name,
			vk_world.num_primitives, vk_world.num_models, stats.build_time * 1000.0);
	Con_Printf ("buffer %.2f MB (primitives %.2f MB, positions %.2f MB), materials %d\n",
			vk_world.buffer.size / (1024.0 * 1024.0), vk_world.positions_offset / (1024.0 * 1024.0),
			(vk_world.buffer.size - vk_world.positions_offset) / (1024.0 * 1024.0), vk_num_materials - 1);
	Con_Printf ("world:     opaque %u, transparent %u, sky %u; %u facing into solid left out\n",
			vk_world.models[0].opaque.count, vk_world.models[0].transparent.count,
			vk_world.models[0].sky.count, stats.into_solid);
	for (i = 1; i < vk_world.num_models; i++)
	{
		sub[PASS_OPAQUE] += vk_world.models[i].opaque.count;
		sub[PASS_TRANSPARENT] += vk_world.models[i].transparent.count;
		sub[PASS_SKY] += vk_world.models[i].sky.count;
	}
	Con_Printf ("submodels: opaque %u, transparent %u, sky %u\n",
			sub[PASS_OPAQUE], sub[PASS_TRANSPARENT], sub[PASS_SKY]);
	Con_Printf ("kinds:");
	for (i = 1; i < 16; i++)
	{
		if (stats.kinds[i])
			Con_Printf (" %s %u", KindName ((uint32_t)i << 28), stats.kinds[i]);
	}
	Con_Printf ("\n");

	/* the animation sequences, each from its first frame */
	for (i = 0; i < world->numtextures; i++)
	{
		texture_t		*tx = world->textures[i];
		const vk_material_t	*m, *f;
		char			chain[128];
		int			k;

		if (!tx || !tx->anim_total || tx->anim_min != 0)
			continue;
		m = VK_GetMaterial (texture_materials[i]);
		chain[0] = '\0';
		for (k = 0, f = m; k < m->num_frames; k++, f = VK_GetMaterial (f->next_frame))
			q_strlcat (chain, va("%s%s", k ? " > " : "", f->name), sizeof(chain));
		Con_Printf ("anim: %s%s%s\n", chain, m->alternate ? ", alternate " : "",
				m->alternate ? VK_GetMaterial (m->alternate)->name : "");
	}

	mismatches = CheckAnimations (world, &checked);
	Con_Printf ("animation check: %d textures, %d mismatches with R_TextureAnimation\n", checked, mismatches);

	if (list)
	{
		for (i = 1; i < vk_num_materials; i++)
		{
			const vk_material_t	*m = VK_GetMaterial (i);

			Con_Printf ("%4d %-16s texture %4d frames %d next %4d alternate %4d\n", i, m->name,
					m->base_texture, m->num_frames, m->next_frame, m->alternate);
		}
	}
}


void VK_InitWorld (void)
{
	Cmd_AddCommand ("vk_world", VK_World_f);
}

void VK_ShutdownWorld (void)
{
	VK_FreeWorld ();
}
