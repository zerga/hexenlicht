/* vk_lightfit.c -- GL's sum of overlapping lights for the map lights' GL
 * shape (4.16): each light's factor per light list entry, fitted to the
 * map's own lightmaps
 *
 * GL added a texel's lights before its sRGB step and clipped the sum
 * (utils/light summed each style's lights into a byte, GL's R_BuildLightMap
 * the styles, both clipped). The map lights' GL shape (4.15,
 * shaders/light_lists.h) gives each light the texel it makes alone, in
 * linear light (to the power r_maplight_gamma), and the path tracer adds
 * them after that step: a light alone is the compiler's, several
 * overlapping lights add up to less than GL's (n equal lights: n against
 * n^2.2), and GL's clip is missing. 4.15 took one scale for all
 * (r_maplight_gl_scale 2); here each light gets its own, per cluster it
 * lights. When the factors are first used (at the map's load with GL's
 * shape and r_maplight_fit 1, else when either is set; again for a new
 * r_maplight_gamma), utils/light is repeated on a quarter of the
 * world's lightmap texels (every other one in both directions: as good as
 * half of them, within 0.01 stops) for the lump's lights as vk_maplights.c
 * read them (before the map file's changes), each face with the lights
 * GL's R_MarkLights would find for it (with a margin for the sample points
 * past the face): ltface.c's CalcFaceVectors,
 * CalcFaceExtents, CalcPoints (without -extra: the original game's
 * lightmaps are the compiler's own at 96-100 % of the texels within
 * 1/255) and SingleLightFace's value, through trace.c's TestLine in the
 * world's nodes (only solid leafs block, floats as there).
 * At each texel, GL's value (its lightmap: every style at its normal 264,
 * >> 7, clipped, the brightest channel, in linear light; a .lit file that
 * gl_coloredlight loaded instead: its brightest channel) over the sum of
 * the lights' own values is what the lights there should be multiplied by
 * (at most MAX_TEXEL_RATIO: above it the model doesn't explain the light);
 * a light's factor is the mean of that ratio weighted by its own value,
 * in the cluster of the texel's triangle (vk_world.c's TriangleLeaf: the
 * cluster the renderer shades it with) and over all its texels. The target
 * is the lightmaps rather than the compiler's sum: the mission pack's maps
 * were built with other options (a third of their texels repeated), and a
 * custom map's compiler may differ too.
 * vk_light.c writes each list entry's factor (VK_LightFitFactor) into the
 * entry beside the light's index (shaders/vertex_buffer.h), times
 * r_maplight_fit_scale (1; 4.16's 1.1 made up for the sRGB curve's linear
 * toe, which showed GL's dark tones darker than GL's product of 8-bit
 * colors: with 4.17's 2.2 power the fit to the lightmaps is GL's look); an
 * entry with fewer than MIN_ENTRY_TEXELS texels takes the light's own
 * factor, and a light without any (the map file's addlights, a map
 * without lightmaps) or r_maplight_fit 0 takes r_maplight_gl_scale,
 * 4.15's. The map file's edits apply on top: a light keeps its factors
 * when moved, scaled or given another level; nothing is fitted again for
 * an edit. Not physically based: like 4.15's shape it changes only a
 * light's first arrival, after GL's lightmaps; its shadows, bounces and
 * reflections stay path traced, and the physical shapes (r_maplight_shape
 * 0 and 1) don't take the factors.
 *
 * Copyright (C) 1996-1997  Id Software, Inc.
 * Copyright (C) 1997-1998  Raven Software Corp.
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

#define TEXEL_SUM		247.27273	/* light_lists.h's LIGHTMAP_TEXEL_SUM: the compiler's sum that makes a GL texel 1 */
#define BYTE_CLIP		2.0625		/* its LIGHTMAP_BYTE_CLIP: the compiler's byte 255 as a GL texel */
#define NORMAL_STYLE		264		/* d_lightstylevalue of a style's normal 'm' */
#define TRACE_EPSILON		0.1f		/* trace.c's ON_EPSILON */
#define TRACE_STACK		64		/* its tracestack */
#define MIN_ENTRY_TEXELS	4		/* a list entry's factor from at least these texels, else the light's */
#define MAX_FACE_EDGES		64
#define MARK_STACK		1024	/* MarkLights' nodes to visit */
#define MAX_TEXEL_RATIO		16.0	/* a texel's GL value over its lights' sum at most (10 equal lights: 10^1.2 = 15.8) */
#define MAX_FIT_CLUSTERS	(1 << 20)	/* the entries' key: cluster << 12 | light */

COMPILE_TIME_ASSERT(lightfit_key, MAX_LIGHT_POLYS <= (1 << 12));	/* the key's light, and the list entry's 16 bits (vk_light.c) */
#define EMPTY_KEY		0xffffffffu

typedef double dvec3_t[3];

static cvar_t	r_maplight_fit = {"r_maplight_fit", "1", CVAR_NONE};	/* 0: r_maplight_gl_scale for every light (4.15) */
static cvar_t	r_maplight_fit_scale = {"r_maplight_fit_scale", "1", CVAR_NONE};	/* the fitted factors times this: the lit image against GL's */

/* a lump light as the compiler lit from it */
typedef struct
{
	dvec3_t		origin;
	double		level;
	dvec3_t		spot_dir;	/* 0 0 0: not a spot */
	double		spot_cos;
} fitlight_t;

/* a light's sums in a cluster: key = cluster << 12 | light */
typedef struct
{
	uint32_t	key;
	int		texels;
	double		num, den;
} fitentry_t;

/* a face as the compiler set it up (ltface.c's lightinfo_t) */
typedef struct
{
	dvec3_t		normal;
	double		dist;
	dvec3_t		texorg;
	dvec3_t		textoworld[2];
	double		exactmins[2], exactmaxs[2];
	int		texmins[2], texsize[2];
	dvec3_t		facemid;
	dvec3_t		mins, maxs;	/* its sample points' bounds */
	int		num_tris;
	dvec3_t		tri_center[MAX_FACE_EDGES];
	float		tri_2d[MAX_FACE_EDGES][3][2];	/* the corners along the plane's two major axes */
	int		tri_cluster[MAX_FACE_EDGES];
	int		axes[2];
} fitface_t;

/* vk_lights fit: the held-out texels' log2 of Hexenlicht / GL */
typedef struct
{
	float		*v[4];		/* r_maplight_gl_scale, per light, per list entry, per entry where GL clips */
	int		n[4], size;
} fitscore_t;

static struct
{
	qmodel_t	*model;		/* the world fitted, NULL = none */
	fitlight_t	*lights;
	int		num_lights;
	double		*num, *den;	/* per light */
	fitentry_t	*entries;
	uint32_t	size, count;	/* the entries' table (a power of 2) and those in use */
	qboolean	fitted;		/* the sums below are the model's (VK_UpdateLightFit) */
	float		gamma;		/* r_maplight_gamma it was fitted with */
	int		clamped;	/* texels whose ratio was clipped at MAX_TEXEL_RATIO */
	int		faces, skipped, texels_fitted;
	double		seconds;
	/* the lights that may reach each surface (MarkLights):
	 * face_list[face_first[s]] up to face_first[s + 1] */
	int		*face_first, *face_list;
	/* the texel's lights: scratch */
	int		*face_lights;
	double		*values;
} fit;


/* ==========================================================================
 * utils/light
 * ========================================================================== */

static double DDot (const double *a, const double *b)
{
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static double DNormalize (double *v)
{
	double	length = sqrt (DDot (v, v));

	if (length != 0)
	{
		v[0] /= length;
		v[1] /= length;
		v[2] /= length;
	}
	return length;
}

/* trace.c's TestLine through the world's nodes, in floats as there: only
 * solid leafs block; within TRACE_EPSILON of a plane a point takes the
 * other end's side */
static qboolean TestLine (const mnode_t *root, const double *start, const double *stop)
{
	struct
	{
		float		back[3];
		int		side;
		const mnode_t	*node;
	}		stack[TRACE_STACK];
	int		sp = 0, k;
	const mnode_t	*node = root;
	float		front[3], back[3];

	for (k = 0; k < 3; k++)
	{
		front[k] = (float)start[k];
		back[k] = (float)stop[k];
	}
	for (;;)
	{
		const mplane_t	*plane;
		float		f, b;
		int		side;

		while (node->contents < 0 && node->contents != CONTENTS_SOLID)
		{
			/* a leaf that doesn't block: up the stack for a back side */
			if (--sp < 0)
				return true;
			for (k = 0; k < 3; k++)
			{
				front[k] = back[k];
				back[k] = stack[sp].back[k];
			}
			node = stack[sp].node->children[!stack[sp].side];
		}
		if (node->contents == CONTENTS_SOLID)
			return false;

		plane = node->plane;
		if (plane->type < 3)
		{
			f = front[plane->type] - plane->dist;
			b = back[plane->type] - plane->dist;
		}
		else
		{
			/* in double, as the compiler's (DOUBLEVEC_T: its normals are doubles) */
			f = (float)(((double)front[0] * plane->normal[0] + (double)front[1] * plane->normal[1] +
				     (double)front[2] * plane->normal[2]) - plane->dist);
			b = (float)(((double)back[0] * plane->normal[0] + (double)back[1] * plane->normal[1] +
				     (double)back[2] * plane->normal[2]) - plane->dist);
		}
		if (f > -TRACE_EPSILON && b > -TRACE_EPSILON)
		{
			node = node->children[0];
			continue;
		}
		if (f < TRACE_EPSILON && b < TRACE_EPSILON)
		{
			node = node->children[1];
			continue;
		}
		if (sp == TRACE_STACK)
			return false;	/* deeper than the compiler's stack: never on the maps */
		side = (f < 0.0f) ? 1 : 0;
		f /= (f - b);
		stack[sp].node = node;
		stack[sp].side = side;
		for (k = 0; k < 3; k++)
		{
			stack[sp].back[k] = back[k];
			back[k] = front[k] + f * (back[k] - front[k]);
		}
		sp++;
		node = node->children[side];
	}
}

static const float *FaceVertex (const qmodel_t *m, const msurface_t *s, int i)
{
	int	e = m->surfedges[s->firstedge + i];

	return m->vertexes[(e >= 0) ? m->edges[e].v[0] : m->edges[-e].v[1]].position;
}

/* the face as the compiler set it up (CalcFaceVectors, CalcFaceExtents),
 * the bounds of its sample points and the clusters of its triangles (as
 * vk_world.c's EmitSurface and TriangleLeaf make them); false: a face the
 * fit leaves out (the engine's lightmap extents differ, a texture axis
 * along the face, no triangle outside solid) */
static qboolean SetupFace (qmodel_t *m, const msurface_t *s, fitface_t *f)
{
	const float	(*vecs)[4] = s->texinfo->vecs;
	dvec3_t		worldtotex[2], texnormal, point;
	float		distscale;	/* float in the compiler */
	double		dist, len, mins[2], maxs[2], val, us, ut;
	int		i, j, c;

	if (s->numedges < 3 || s->numedges > MAX_FACE_EDGES)
		return false;
	for (j = 0; j < 3; j++)
		f->normal[j] = s->plane->normal[j];
	f->dist = s->plane->dist;
	if (s->flags & SURF_PLANEBACK)
	{
		for (j = 0; j < 3; j++)
			f->normal[j] = -f->normal[j];
		f->dist = -f->dist;
	}

	/* CalcFaceVectors */
	for (i = 0; i < 2; i++)
		for (j = 0; j < 3; j++)
			worldtotex[i][j] = vecs[i][j];
	texnormal[0] = vecs[1][1] * vecs[0][2] - vecs[1][2] * vecs[0][1];
	texnormal[1] = vecs[1][2] * vecs[0][0] - vecs[1][0] * vecs[0][2];
	texnormal[2] = vecs[1][0] * vecs[0][1] - vecs[1][1] * vecs[0][0];
	DNormalize (texnormal);
	distscale = (float)DDot (texnormal, f->normal);
	if (!distscale)
		return false;	/* the compiler stopped: "Texture axis perpendicular to face" */
	if (distscale < 0)
	{
		distscale = -distscale;
		for (j = 0; j < 3; j++)
			texnormal[j] = -texnormal[j];
	}
	distscale = 1 / distscale;
	for (i = 0; i < 2; i++)
	{
		len = sqrt (DDot (worldtotex[i], worldtotex[i]));
		dist = DDot (worldtotex[i], f->normal) * distscale;
		for (j = 0; j < 3; j++)
			f->textoworld[i][j] = (worldtotex[i][j] - dist * texnormal[j]) * ((1 / len) * (1 / len));
	}
	for (j = 0; j < 3; j++)
		f->texorg[j] = -vecs[0][3] * f->textoworld[0][j] - vecs[1][3] * f->textoworld[1][j];
	dist = (DDot (f->texorg, f->normal) - f->dist - 1) * distscale;
	for (j = 0; j < 3; j++)
		f->texorg[j] -= dist * texnormal[j];

	/* CalcFaceExtents: the engine's lightmap must be the same size */
	mins[0] = mins[1] = 999999;
	maxs[0] = maxs[1] = -99999;
	for (i = 0; i < s->numedges; i++)
	{
		const float	*v = FaceVertex (m, s, i);

		for (j = 0; j < 2; j++)
		{
			val = (double)v[0] * vecs[j][0] + (double)v[1] * vecs[j][1] + (double)v[2] * vecs[j][2] + (double)vecs[j][3];
			mins[j] = q_min (mins[j], val);
			maxs[j] = q_max (maxs[j], val);
		}
	}
	for (i = 0; i < 2; i++)
	{
		f->exactmins[i] = mins[i];
		f->exactmaxs[i] = maxs[i];
		f->texmins[i] = (int)floor (mins[i] / 16);
		f->texsize[i] = (int)(ceil (maxs[i] / 16) - floor (mins[i] / 16));
		if (s->texturemins[i] != f->texmins[i] * 16 || s->extents[i] != f->texsize[i] * 16)
			return false;
	}
	for (j = 0; j < 3; j++)
		f->facemid[j] = f->texorg[j] + f->textoworld[0][j] * (f->exactmaxs[0] + f->exactmins[0]) / 2 +
				f->textoworld[1][j] * (f->exactmaxs[1] + f->exactmins[1]) / 2;

	/* CalcPoints' grid bounds its points: they move towards the face's
	 * middle, less than 8 units past their last spot */
	for (j = 0; j < 3; j++)
	{
		f->mins[j] = 1e30;
		f->maxs[j] = -1e30;
	}
	for (c = 0; c < 4; c++)
	{
		us = f->texmins[0] * 16 + ((c & 1) ? f->texsize[0] * 16 : 0);
		ut = f->texmins[1] * 16 + ((c & 2) ? f->texsize[1] * 16 : 0);
		for (j = 0; j < 3; j++)
		{
			val = f->texorg[j] + f->textoworld[0][j] * us + f->textoworld[1][j] * ut;
			f->mins[j] = q_min (f->mins[j], val);
			f->maxs[j] = q_max (f->maxs[j], val);
		}
	}
	for (j = 0; j < 3; j++)
	{
		f->mins[j] -= 9;
		f->maxs[j] += 9;
	}

	/* the renderer's triangles (a fan) and their clusters (TriangleLeaf:
	 * the center 0.01 units in front, else 1 unit); those facing into
	 * solid aren't drawn */
	j = (fabs (f->normal[0]) > fabs (f->normal[1])) ? ((fabs (f->normal[0]) > fabs (f->normal[2])) ? 0 : 2) :
	    ((fabs (f->normal[1]) > fabs (f->normal[2])) ? 1 : 2);
	f->axes[0] = (j == 0) ? 1 : 0;
	f->axes[1] = (j == 2) ? 1 : 2;
	f->num_tris = 0;
	for (i = 0; i < s->numedges - 2; i++)
	{
		const float	*v[3];
		vec3_t		p;
		int		k, cluster;

		v[0] = FaceVertex (m, s, 0);
		v[1] = FaceVertex (m, s, i + 2);
		v[2] = FaceVertex (m, s, i + 1);
		for (j = 0; j < 3; j++)
			point[j] = (v[0][j] + v[1][j] + v[2][j]) / 3.0;
		for (j = 0; j < 3; j++)
			p[j] = (float)(point[j] + 0.01 * f->normal[j]);
		cluster = VK_PointCluster (m, p);
		if (cluster < 0)
		{
			for (j = 0; j < 3; j++)
				p[j] = (float)(point[j] + f->normal[j]);
			cluster = VK_PointCluster (m, p);
		}
		if (cluster < 0)
			continue;
		for (j = 0; j < 3; j++)
			f->tri_center[f->num_tris][j] = point[j];
		for (k = 0; k < 3; k++)
		{
			f->tri_2d[f->num_tris][k][0] = v[k][f->axes[0]];
			f->tri_2d[f->num_tris][k][1] = v[k][f->axes[1]];
		}
		f->tri_cluster[f->num_tris++] = cluster;
	}
	return f->num_tris > 0;
}

/* CalcPoints for texel (s, t), without -extra: a point that can't see the
 * face's middle moves towards it, up to 6 times */
static void CalcPoint (const qmodel_t *m, const fitface_t *f, int s, int t, double *surf)
{
	double	mids = (f->exactmaxs[0] + f->exactmins[0]) / 2, midt = (f->exactmaxs[1] + f->exactmins[1]) / 2;
	double	us = f->texmins[0] * 16 + s * 16, ut = f->texmins[1] * 16 + t * 16;
	dvec3_t	move;
	int	i, j;

	for (i = 0; i < 6; i++)
	{
		for (j = 0; j < 3; j++)
			surf[j] = f->texorg[j] + f->textoworld[0][j] * us + f->textoworld[1][j] * ut;
		if (TestLine (m->nodes, f->facemid, surf))
			break;
		if (i & 1)
		{
			if (us > mids)
			{
				us -= 8;
				if (us < mids)
					us = mids;
			}
			else
			{
				us += 8;
				if (us > mids)
					us = mids;
			}
		}
		else
		{
			if (ut > midt)
			{
				ut -= 8;
				if (ut < midt)
					ut = midt;
			}
			else
			{
				ut += 8;
				if (ut > midt)
					ut = midt;
			}
		}
		for (j = 0; j < 3; j++)
			move[j] = f->facemid[j] - surf[j];
		DNormalize (move);
		for (j = 0; j < 3; j++)
			surf[j] += 8 * move[j];
	}
}

/* the cluster of the triangle a point is on (the nearest one's when it is
 * on none: the lightmap reaches past the face) */
static int PointTriangleCluster (const fitface_t *f, const double *p)
{
	float	x = (float)p[f->axes[0]], y = (float)p[f->axes[1]];
	double	best = 1e30;
	int	i, j, nearest = 0;

	if (f->num_tris == 1)
		return f->tri_cluster[0];
	for (i = 0; i < f->num_tris; i++)
	{
		const float	(*t)[2] = f->tri_2d[i];
		float		d0 = (t[1][0] - t[0][0]) * (y - t[0][1]) - (t[1][1] - t[0][1]) * (x - t[0][0]);
		float		d1 = (t[2][0] - t[1][0]) * (y - t[1][1]) - (t[2][1] - t[1][1]) * (x - t[1][0]);
		float		d2 = (t[0][0] - t[2][0]) * (y - t[2][1]) - (t[0][1] - t[2][1]) * (x - t[2][0]);
		double		d = 0;

		if ((d0 >= 0 && d1 >= 0 && d2 >= 0) || (d0 <= 0 && d1 <= 0 && d2 <= 0))
			return f->tri_cluster[i];
		for (j = 0; j < 3; j++)
			d += (p[j] - f->tri_center[i][j]) * (p[j] - f->tri_center[i][j]);
		if (d < best)
		{
			best = d;
			nearest = i;
		}
	}
	return f->tri_cluster[nearest];
}

/* SingleLightFace's value of a light at a sample point, as a GL texel of
 * this light alone in linear light (the GL shape's, style 1); the caller
 * has checked the face's plane (in front, within the level) */
static double LightValue (const qmodel_t *m, const fitlight_t *l, const fitface_t *f, const double *p, float gamma)
{
	dvec3_t	incoming;
	double	t = 0, dist, angle, add;
	int	j;

	for (j = 0; j < 3; j++)
		t += (p[j] - l->origin[j]) * (p[j] - l->origin[j]);
	if (t >= l->level * l->level)
		return 0;	/* (level - dist) x angle <= 0: nothing, without the trace */
	dist = (t > 0) ? sqrt (t) : 1;
	for (j = 0; j < 3; j++)
		incoming[j] = l->origin[j] - p[j];
	DNormalize (incoming);
	if (l->spot_dir[0] || l->spot_dir[1] || l->spot_dir[2])
	{
		/* the compiler's cone: skipped when dot(towards the target, to the light) > -cos(half) */
		if (DDot (l->spot_dir, incoming) > -l->spot_cos)
			return 0;
	}
	if (!TestLine (m->nodes, l->origin, p))
		return 0;
	angle = 0.5 + 0.5 * DDot (incoming, f->normal);
	add = (l->level - dist) * angle;
	if (add <= 0)
		return 0;
	return pow (q_min (q_min (add / TEXEL_SUM, BYTE_CLIP), 1.0), gamma);
}


/* ==========================================================================
 * The fit
 * ========================================================================== */

static uint32_t EntryHash (uint32_t key)
{
	key ^= key >> 16;
	key *= 0x7feb352du;
	key ^= key >> 15;
	return key;
}

static fitentry_t *FindEntry (uint32_t key, qboolean add)
{
	uint32_t	h;

	if (!fit.size)
		return NULL;
	for (h = EntryHash (key) & (fit.size - 1); fit.entries[h].key != EMPTY_KEY; h = (h + 1) & (fit.size - 1))
	{
		if (fit.entries[h].key == key)
			return &fit.entries[h];
	}
	if (!add)
		return NULL;
	memset (&fit.entries[h], 0, sizeof(fit.entries[h]));
	fit.entries[h].key = key;
	fit.count++;
	return &fit.entries[h];
}

/* the entries' table at twice the entries in use at least */
static void GrowEntries (void)
{
	fitentry_t	*old = fit.entries;
	uint32_t	old_size = fit.size, i;

	if (fit.size && fit.count * 2 < fit.size)
		return;
	fit.size = fit.size ? fit.size * 2 : 4096;
	fit.entries = (fitentry_t *) malloc (fit.size * sizeof(fitentry_t));
	if (!fit.entries)
		Sys_Error ("%s: out of memory", __thisfunc__);
	for (i = 0; i < fit.size; i++)
		fit.entries[i].key = EMPTY_KEY;
	fit.count = 0;
	for (i = 0; i < old_size; i++)
	{
		if (old[i].key != EMPTY_KEY)
			*FindEntry (old[i].key, true) = old[i];
	}
	free (old);
}

/* a lump light's factor in a cluster, without r_maplight_fit_scale: its
 * entry's, else its own, else the fallback */
static float FittedFactor (int base, int cluster, float fallback)
{
	const fitentry_t	*e = FindEntry (((uint32_t)cluster << 12) | (uint32_t)base, false);

	if (e && e->texels >= MIN_ENTRY_TEXELS && e->den > 0)
		return (float)(e->num / e->den);
	if (fit.den[base] > 0)
		return (float)(fit.num[base] / fit.den[base]);
	return fallback;
}

static void AddScore (fitscore_t *score, int which, double log2_ratio)
{
	if (score->n[which] == score->size)
		return;
	score->v[which][score->n[which]++] = (float)log2_ratio;
}

/* the world's lightmapped faces: the sums over the fitted quarter of the
 * texels (s and t even), or with score the ratios to GL's of the held-out
 * half (s + t odd), which the fit didn't see */
static void FitFaces (qmodel_t *m, fitscore_t *score)
{
	float	gamma = VK_MapLightGamma (), gl_scale = VK_MapLightGLScale ();
	int	i, j, k, s, t;

	for (i = 0; i < m->nummodelsurfaces; i++)
	{
		const msurface_t	*surf = &m->surfaces[m->firstmodelsurface + i];
		fitface_t		f;
		int			w, h, size, num_face_lights = 0, styles;

		if ((surf->flags & (SURF_DRAWSKY | SURF_DRAWTURB)) || !surf->samples)
			continue;
		if (!SetupFace (m, surf, &f))
		{
			fit.skipped += !score;
			continue;
		}
		fit.faces += !score;
		for (styles = 0; styles < MAXLIGHTMAPS && surf->styles[styles] != 255; styles++)
			;
		w = f.texsize[0] + 1;
		h = f.texsize[1] + 1;
		size = w * h;

		/* the lights that reach the face: in front of it within their
		 * level (the compiler's test) and near its sample points */
		for (j = fit.face_first[m->firstmodelsurface + i]; j < fit.face_first[m->firstmodelsurface + i + 1]; j++)
		{
			const fitlight_t	*l = &fit.lights[fit.face_list[j]];
			double			d = DDot (l->origin, f.normal) - f.dist, d2 = 0;
			int			a;

			if (d <= 0 || d > l->level)
				continue;
			for (a = 0; a < 3; a++)
			{
				double	e = (l->origin[a] < f.mins[a]) ? f.mins[a] - l->origin[a] :
					    (l->origin[a] > f.maxs[a]) ? l->origin[a] - f.maxs[a] : 0;

				d2 += e * e;
			}
			if (d2 < l->level * l->level)
				fit.face_lights[num_face_lights++] = fit.face_list[j];
		}
		if (!num_face_lights)
			continue;

		for (t = 0; t < h; t++)
		{
			for (s = 0; s < w; s++)
			{
				dvec3_t		p;
				double		sum = 0, target, ratio;
				int		c, best = 0, cluster;

				if (score ? !((s + t) & 1) : ((s | t) & 1))
					continue;
				CalcPoint (m, &f, s, t, p);
				for (k = 0; k < num_face_lights; k++)
				{
					fit.values[k] = LightValue (m, &fit.lights[fit.face_lights[k]], &f, p, gamma);
					sum += fit.values[k];
				}
				if (sum <= 0)
					continue;	/* nothing reaches the point: nothing to fit */
				/* GL's texel: the style maps at their normal value, >> 7, clipped, the brightest channel */
				for (c = 0; c < 3; c++)
				{
					int	v = 0;

					for (j = 0; j < styles; j++)
						v += surf->samples[(j * size + t * w + s) * 3 + c] * NORMAL_STYLE;
					best = q_max (best, q_min (v >> 7, 255));
				}
				target = pow (best / 255.0, gamma);
				cluster = PointTriangleCluster (&f, p);
				if (cluster >= MAX_FIT_CLUSTERS)
					continue;

				if (!score)
				{
					ratio = target / sum;
					if (ratio > MAX_TEXEL_RATIO)
					{
						/* light the model doesn't explain (the compiler's options,
						 * a light it lit from and this one didn't): not a factor */
						ratio = MAX_TEXEL_RATIO;
						fit.clamped++;
					}
					fit.texels_fitted++;
					for (k = 0; k < num_face_lights; k++)
					{
						fitentry_t	*e;
						int		l = fit.face_lights[k];
						double		v = fit.values[k];

						if (v <= 0)
							continue;
						fit.num[l] += v * ratio;
						fit.den[l] += v;
						GrowEntries ();
						e = FindEntry (((uint32_t)cluster << 12) | (uint32_t)l, true);
						e->num += v * ratio;
						e->den += v;
						e->texels++;
					}
					continue;
				}

				/* scored: as calib_compare.ps1, not where GL's texel is black */
				if (best < 12)
					continue;
				{
					double	by_light = 0, by_entry = 0;

					for (k = 0; k < num_face_lights; k++)
					{
						int	l = fit.face_lights[k];

						if (fit.values[k] <= 0)
							continue;
						by_light += fit.values[k] * ((fit.den[l] > 0) ? fit.num[l] / fit.den[l] : gl_scale);
						by_entry += fit.values[k] * FittedFactor (l, cluster, gl_scale);
					}
					AddScore (score, 0, log (q_max (gl_scale * sum, 1e-6) / target) / log (2.0));
					AddScore (score, 1, log (q_max (by_light, 1e-6) / target) / log (2.0));
					AddScore (score, 2, log (q_max (by_entry, 1e-6) / target) / log (2.0));
					if (best == 255)
						AddScore (score, 3, log (q_max (by_entry, 1e-6) / target) / log (2.0));
				}
			}
		}
	}
}

/* the surfaces each light may reach, as GL's R_MarkLights finds them: the
 * world's nodes whose plane is within its level (faces lie on their node's
 * plane); lists per surface, in the lights' order */
static void MarkLights (qmodel_t *m)
{
	const mnode_t	*stack[MARK_STACK];
	int		*pos, pass, l, i, total = 0;
	double		margin = 0;

	/* a sample point lies up to a texel past its face (16 texture units,
	 * more world units on a scaled texture) and CalcPoints' 8 units more */
	for (i = 0; i < m->nummodelsurfaces; i++)
	{
		const float	(*vecs)[4] = m->surfaces[m->firstmodelsurface + i].texinfo->vecs;
		double		s = sqrt (vecs[0][0] * vecs[0][0] + vecs[0][1] * vecs[0][1] + vecs[0][2] * vecs[0][2]);
		double		u = sqrt (vecs[1][0] * vecs[1][0] + vecs[1][1] * vecs[1][1] + vecs[1][2] * vecs[1][2]);

		if (s > 0 && u > 0)
			margin = q_max (margin, 16 / s + 16 / u + 8);
	}

	fit.face_first = (int *) calloc (m->numsurfaces + 1, sizeof(int));
	pos = (int *) calloc (m->numsurfaces + 1, sizeof(int));
	if (!fit.face_first || !pos)
		Sys_Error ("%s: out of memory", __thisfunc__);
	for (pass = 0; pass < 2; pass++)
	{
		for (l = 0; l < fit.num_lights; l++)
		{
			const fitlight_t	*light = &fit.lights[l];
			int			sp = 0;

			stack[sp++] = m->nodes;
			while (sp)
			{
				const mnode_t	*node = stack[--sp];
				const mplane_t	*plane;
				double		d;

				if (node->contents < 0)
					continue;
				plane = node->plane;
				d = light->origin[0] * plane->normal[0] + light->origin[1] * plane->normal[1] +
				    light->origin[2] * plane->normal[2] - plane->dist;
				if (sp + 2 > MARK_STACK)
					continue;	/* deeper than any map's tree */
				if (d > light->level + margin)
				{
					stack[sp++] = node->children[0];
					continue;
				}
				if (d < -light->level - margin)
				{
					stack[sp++] = node->children[1];
					continue;
				}
				for (i = 0; i < (int)node->numsurfaces; i++)
				{
					int	s = (int)node->firstsurface + i;

					if (pass == 0)
						fit.face_first[s + 1]++;
					else
						fit.face_list[pos[s]++] = l;
				}
				stack[sp++] = node->children[0];
				stack[sp++] = node->children[1];
			}
		}
		if (pass == 0)
		{
			for (i = 0; i < m->numsurfaces; i++)
				fit.face_first[i + 1] += fit.face_first[i];
			total = fit.face_first[m->numsurfaces];
			fit.face_list = (int *) malloc (q_max (total, 1) * sizeof(int));
			if (!fit.face_list)
				Sys_Error ("%s: out of memory", __thisfunc__);
			memcpy (pos, fit.face_first, m->numsurfaces * sizeof(int));
		}
	}
	free (pos);
}

/* fits the lights (VK_FitMapLights, or again for a new r_maplight_gamma) */
static void Fit (void)
{
	double	start = Sys_DoubleTime ();
	int	n = fit.num_lights;

	free (fit.face_first);
	free (fit.face_list);
	MarkLights (fit.model);
	free (fit.num);
	free (fit.den);
	free (fit.entries);
	free (fit.face_lights);
	free (fit.values);
	fit.num = (double *) calloc (q_max (n, 1), sizeof(double));
	fit.den = (double *) calloc (q_max (n, 1), sizeof(double));
	fit.face_lights = (int *) malloc (q_max (n, 1) * sizeof(int));
	fit.values = (double *) malloc (q_max (n, 1) * sizeof(double));
	if (!fit.num || !fit.den || !fit.face_lights || !fit.values)
		Sys_Error ("%s: out of memory", __thisfunc__);
	fit.entries = NULL;
	fit.size = fit.count = 0;
	GrowEntries ();
	fit.faces = fit.skipped = fit.texels_fitted = fit.clamped = 0;
	fit.gamma = VK_MapLightGamma ();
	fit.fitted = true;
	FitFaces (fit.model, NULL);
	fit.seconds = Sys_DoubleTime () - start;
}

/* a new map (VK_LoadMapLights): the lump's lights, before the map file's
 * changes */
void VK_FitMapLights (qmodel_t *worldmodel, const vk_maplight_t *lights, int count)
{
	int	i, j;

	VK_ClearLightFit ();
	if (!worldmodel->lightdata || count <= 0)
		return;	/* GL draws a map without light data fullbright: r_maplight_gl_scale */
	fit.lights = (fitlight_t *) calloc (count, sizeof(fitlight_t));
	if (!fit.lights)
		Sys_Error ("%s: out of memory", __thisfunc__);
	for (i = 0; i < count; i++)
	{
		for (j = 0; j < 3; j++)
		{
			fit.lights[i].origin[j] = lights[i].origin[j];
			fit.lights[i].spot_dir[j] = lights[i].spot_dir[j];
		}
		fit.lights[i].level = lights[i].level;
		fit.lights[i].spot_cos = lights[i].spot_cos;
	}
	fit.num_lights = count;
	fit.model = worldmodel;	/* fitted by VK_UpdateLightFit when the factors are used */
}

/* before the light lists are built (vk_light.c, also at the map's load):
 * fitted when GL's shape takes the factors (not for the physical shapes or
 * with r_maplight_fit 0) and not yet, or again when r_maplight_gamma
 * changed, the power inside the sums */
void VK_UpdateLightFit (void)
{
	if (!fit.model || VK_MapLightShape () != SPHERE_SHAPE_GL || !r_maplight_fit.integer)
		return;
	if (!fit.fitted || fit.gamma != VK_MapLightGamma ())
		Fit ();
}

void VK_ClearLightFit (void)
{
	free (fit.face_first);
	free (fit.face_list);
	free (fit.lights);
	free (fit.num);
	free (fit.den);
	free (fit.entries);
	free (fit.face_lights);
	free (fit.values);
	memset (&fit, 0, sizeof(fit));
}

/* the factor of a light list entry of lump light base (-1: none, an
 * addlight) in a cluster (see the top); r_maplight_gl_scale without one */
float VK_LightFitFactor (int base, int cluster)
{
	float	f;

	if (!r_maplight_fit.integer || !fit.fitted || base < 0 || base >= fit.num_lights || cluster < 0 || cluster >= MAX_FIT_CLUSTERS)
		return VK_MapLightGLScale ();
	f = FittedFactor (base, cluster, -1.0f);
	return (f < 0.0f) ? VK_MapLightGLScale () : f * q_max (r_maplight_fit_scale.value, 0.0f);
}

static int CompareFloats (const void *a, const void *b)
{
	float	x = *(const float *)a, y = *(const float *)b;

	return (x > y) - (x < y);
}

static float Quantile (const float *s, int n, float q)
{
	float	p = q * (n - 1);
	int	i = (int)p;

	return (i + 1 < n) ? s[i] + (p - i) * (s[i + 1] - s[i]) : s[i];
}

/* vk_lights: the fit, and with score its factors against GL's lightmaps
 * on the texels it didn't use */
void VK_PrintLightFit (qboolean score)
{
	static const char	*names[] = {"r_maplight_gl_scale", "a factor per light", "a factor per list entry", "  where GL clips"};
	fitscore_t		sc;
	float			*g;
	int			i, n = 0, fallback = 0, used = 0;
	uint32_t		k;
	double			largest = 0;

	/* a fit made for GL's shape stays when the shape changes: say it isn't used */
	if (!fit.model || !fit.fitted || !r_maplight_fit.integer || VK_MapLightShape () != SPHERE_SHAPE_GL)
	{
		Con_Printf ("  fit (4.16): %s\n", !fit.model ? "none (no map, or no light data)" :
			    !r_maplight_fit.integer ? "off (r_maplight_fit 0)" :
			    (VK_MapLightShape () != SPHERE_SHAPE_GL) ? "not used (a physical shape; r_maplight_shape 2 uses it)" :
			    "not made yet");
		return;
	}
	for (k = 0; k < fit.size; k++)
	{
		const fitentry_t	*e = &fit.entries[k];

		if (e->key == EMPTY_KEY || e->texels < MIN_ENTRY_TEXELS || e->den <= 0)
			continue;
		used++;
		largest = q_max (largest, e->num / e->den);
	}
	g = (float *) malloc (q_max (fit.num_lights, 1) * sizeof(float));
	if (!g)
		return;
	for (i = 0; i < fit.num_lights; i++)
	{
		if (fit.den[i] > 0)
			g[n++] = (float)(fit.num[i] / fit.den[i]);
		else
			fallback++;
	}
	qsort (g, n, sizeof(float), CompareFloats);
	Con_Printf ("  fit (4.16)%s: %d lump lights on %d texels of %d faces (%d left out) in %.0f ms, r_maplight_gamma %g; %u cluster entries (%d of %d texels or more, the largest factor %.2f), %d lights without texels (r_maplight_gl_scale), %d texels' ratio clipped at %g\n",
		    r_maplight_fit.integer ? "" : ", not used (r_maplight_fit 0)", fit.num_lights, fit.texels_fitted, fit.faces, fit.skipped, fit.seconds * 1000.0, fit.gamma, fit.count, used,
		    MIN_ENTRY_TEXELS, largest, fallback, fit.clamped, MAX_TEXEL_RATIO);
	if (n)
		Con_Printf ("  per-light factors: 10%% %.2f, 25%% %.2f, median %.2f, 75%% %.2f, 90%% %.2f; times r_maplight_fit_scale %g\n",
			    Quantile (g, n, 0.1f), Quantile (g, n, 0.25f), Quantile (g, n, 0.5f), Quantile (g, n, 0.75f),
			    Quantile (g, n, 0.9f), r_maplight_fit_scale.value);
	free (g);
	if (!score)
		return;

	/* the held-out half of the texels: about twice the fitted quarter */
	memset (&sc, 0, sizeof(sc));
	sc.size = (fit.texels_fitted + 1024) * 4;
	for (i = 0; i < 4; i++)
	{
		sc.v[i] = (float *) malloc (sc.size * sizeof(float));
		if (!sc.v[i])
			goto done;
	}
	FitFaces (fit.model, &sc);
	Con_Printf ("  the held-out texels, the direct light against GL's lightmaps (log2 of the ratio; without r_maplight_fit_scale):\n");
	for (i = 0; i < 4; i++)
	{
		int	within = 0, j;

		if (!sc.n[i])
			continue;
		qsort (sc.v[i], sc.n[i], sizeof(float), CompareFloats);
		for (j = 0; j < sc.n[i]; j++)
			within += (fabsf (sc.v[i][j]) < 0.5f);
		Con_Printf ("  %-26s %7d texels: median %.2fx, spread %.2f stops, %.0f%% within half a stop\n", names[i], sc.n[i],
			    powf (2.0f, Quantile (sc.v[i], sc.n[i], 0.5f)),
			    Quantile (sc.v[i], sc.n[i], 0.75f) - Quantile (sc.v[i], sc.n[i], 0.25f), 100.0f * within / sc.n[i]);
	}
done:
	for (i = 0; i < 4; i++)
		free (sc.v[i]);
}

static void LightFitChanged (cvar_t *var)
{
	(void)var;
	VK_RebuildLights ();
}

void VK_InitLightFit (void)
{
	Cvar_RegisterVariable (&r_maplight_fit);
	Cvar_RegisterVariable (&r_maplight_fit_scale);
	Cvar_SetCallback (&r_maplight_fit, LightFitChanged);
	Cvar_SetCallback (&r_maplight_fit_scale, LightFitChanged);
}
