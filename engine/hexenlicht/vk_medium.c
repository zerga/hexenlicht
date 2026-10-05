/* vk_medium.c -- the liquids' light grid (6.18)
 *
 * The liquids' medium (shaders/water.glsl, DECISIONS X22) scatters GL's
 * contents color lit as GL lit a model: its light level, the light maps
 * of the surface straight below a point with their light styles, at least
 * 24, plus the dynamic lights. Before 6.18 that was one light for the
 * whole medium, taken at the camera (cl.light_level; 6.17 averaged it
 * around the camera under water and eased it), so the glow of every pool
 * in view changed at once as the camera moved over light map patches.
 * Here the light is the liquid's own: at map load GL's light level is
 * baked on a lattice in boxes around the liquid leaves, by light style
 * (r_light.c's R_LightPointStyles), and water.glsl takes it at points
 * along each fog segment (shaders/medium.glsl) with the frame's light
 * style values and dynamic lights, which VK_PrepareMedium puts into the
 * UBO. The layout is in shaders/hl_shared.h.
 *
 * The boxes: the liquid leaves' bounds, grouped while any two come within
 * MEDIUM_BOX_GAP cells of each other, each group's bounds a box (merged
 * again while two do), its lattice one cell past the bounds below and two
 * above (a point and the point after it on each axis): reaching at most 2
 * cells past the bounds, so no two boxes' lattices overlap and a point in
 * a liquid is in its own box's. The lattice points whose cell (centered
 * on them) may reach into a liquid leaf are sampled: each the
 * mean of GL's light level at MEDIUM_SUB_XY x MEDIUM_SUB_XY x MEDIUM_SUB_Z
 * points of its cell that are in a liquid; a point with
 * none takes the mean of its neighbours that have one (MEDIUM_FILL_PASSES
 * passes), so the interpolation next to a wall, the floor or the surface
 * doesn't pull in darkness. A point keeps 4 light styles, as a light map;
 * more are folded into its brightest (counted). Light styles past 63,
 * which Hexen II's light tools don't make, count as style 0. Beyond
 * MEDIUM_MAX_POINTS the cell doubles.
 *
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

#define MEDIUM_CELL		32.0f		/* units */
#define MEDIUM_MAX_POINTS	(1 << 21)	/* 16 MB */
#define MEDIUM_BOX_GAP		4		/* cells: two boxes nearer share one (their lattices would overlap) */
#define MEDIUM_SUB_XY		4		/* samples of a cell along x and y */
#define MEDIUM_SUB_Z		2		/* and z */
#define MEDIUM_FILL_PASSES	2
#define MEDIUM_SLOTS		8		/* light styles a point gathers before it keeps 4 */

vk_medium_t	vk_medium;

typedef struct
{
	float		mins[3], maxs[3];
	int		lo[3], size[3];		/* the lattice */
	int		first;			/* its first point */
} medium_box_t;

/* a lattice point while it is baked */
typedef struct
{
	int		count;			/* its samples in a liquid; -1: not sampled */
	int		num_styles;
	qboolean	overflow;		/* styles folded into the first (past MEDIUM_SLOTS; past 4 when packed) */
	byte		styles[MEDIUM_SLOTS];
	float		levels[MEDIUM_SLOTS];	/* summed, then the mean */
} medium_acc_t;

static qboolean IsLiquid (int contents)
{
	return contents == CONTENTS_WATER || contents == CONTENTS_SLIME || contents == CONTENTS_LAVA;
}

static void AddStyle (medium_acc_t *a, int style, float level)
{
	int	i;

	if (style >= MEDIUM_LIGHT_STYLES)
		style = 0;
	for (i = 0; i < a->num_styles; i++)
	{
		if (a->styles[i] == style)
		{
			a->levels[i] += level;
			return;
		}
	}
	if (a->num_styles < MEDIUM_SLOTS)
	{
		a->styles[a->num_styles] = (byte)style;
		a->levels[a->num_styles++] = level;
		return;
	}
	a->levels[0] += level;	/* no slot left: rarer than folding */
	a->overflow = true;
}

/* whether two bounds overlap or come within margin of each other */
static qboolean Near (const float *amins, const float *amaxs, const float *bmins, const float *bmaxs, float margin)
{
	int	k;

	for (k = 0; k < 3; k++)
	{
		if (amins[k] > bmaxs[k] + margin || bmins[k] > amaxs[k] + margin)
			return false;
	}
	return true;
}

static int FindRoot (int *parent, int i)
{
	while (parent[i] != i)
	{
		parent[i] = parent[parent[i]];
		i = parent[i];
	}
	return i;
}

/* the liquid leaves grouped into boxes; returns their number */
static int BuildBoxes (qmodel_t *wm, const int *leaves, int num_leaves, float cell, medium_box_t *boxes)
{
	int		*parent, i, j, k, n = 0;
	qboolean	merged;

	parent = (int *) malloc (q_max (num_leaves, 1) * sizeof(int));
	if (!parent)
		Sys_Error ("%s: out of memory", __thisfunc__);
	for (i = 0; i < num_leaves; i++)
		parent[i] = i;
	for (i = 0; i < num_leaves; i++)
	{
		const float	*a = wm->leafs[leaves[i]].minmaxs;

		for (j = i + 1; j < num_leaves; j++)
		{
			const float	*b = wm->leafs[leaves[j]].minmaxs;

			if (Near (a, a + 3, b, b + 3, MEDIUM_BOX_GAP * cell))
				parent[FindRoot (parent, j)] = FindRoot (parent, i);
		}
	}

	/* a box per group (first holds the group's root until the lattices) */
	for (i = 0; i < num_leaves; i++)
	{
		const float	*a = wm->leafs[leaves[i]].minmaxs;
		int		root = FindRoot (parent, i);

		for (j = 0; j < n && boxes[j].first != root; j++)
			;
		if (j == n)
		{
			boxes[n].first = root;
			VectorCopy (a, boxes[n].mins);
			VectorCopy (a + 3, boxes[n].maxs);
			n++;
			continue;
		}
		for (k = 0; k < 3; k++)
		{
			boxes[j].mins[k] = q_min (boxes[j].mins[k], a[k]);
			boxes[j].maxs[k] = q_max (boxes[j].maxs[k], a[k + 3]);
		}
	}
	free (parent);

	/* two groups' boxes may still come near: their lattices (2 cells past
	 * the bounds at most) would overlap, and a point in one's liquid could
	 * take the other's points, which weren't sampled for it */
	do
	{
		merged = false;
		for (i = 0; i < n; i++)
		{
			for (j = i + 1; j < n; j++)
			{
				if (!Near (boxes[i].mins, boxes[i].maxs, boxes[j].mins, boxes[j].maxs, MEDIUM_BOX_GAP * cell))
					continue;
				for (k = 0; k < 3; k++)
				{
					boxes[i].mins[k] = q_min (boxes[i].mins[k], boxes[j].mins[k]);
					boxes[i].maxs[k] = q_max (boxes[i].maxs[k], boxes[j].maxs[k]);
				}
				boxes[j] = boxes[--n];
				merged = true;
				j--;
			}
		}
	} while (merged);

	return n;
}

/* the lattice range of bounds: a point's cell and the cell after it, one
 * cell more each side */
static void LatticeRange (const float *mins, const float *maxs, float cell, int *lo, int *hi)
{
	int	k;

	for (k = 0; k < 3; k++)
	{
		lo[k] = (int)floorf (mins[k] / cell) - 1;
		hi[k] = (int)floorf (maxs[k] / cell) + 2;
	}
}

/* a leaf's region: the sides of its ancestors' planes it is on */
typedef struct
{
	const mplane_t	*plane;
	int		side;	/* 0 in front (Mod_PointInLeaf's d > 0), 1 behind */
} leaf_side_t;

#define MEDIUM_MAX_SIDES	256

static int LeafSides (mleaf_t *leaf, leaf_side_t *sides)
{
	mnode_t	*child = (mnode_t *)leaf, *node;
	int	n = 0;

	for (node = leaf->parent; node && n < MEDIUM_MAX_SIDES; child = node, node = node->parent)
	{
		sides[n].plane = node->plane;
		sides[n].side = (node->children[0] == child) ? 0 : 1;
		n++;
	}
	return n;
}

/* whether the cube of half size h around c may reach into the leaf (each
 * plane on its own: a little generous at the leaf's edges) */
static qboolean CubeTouchesLeaf (const leaf_side_t *sides, int n, const vec3_t c, float h)
{
	const mplane_t	*pl;
	float		d, r;
	int		i;

	for (i = 0; i < n; i++)
	{
		pl = sides[i].plane;
		d = DotProduct (c, pl->normal) - pl->dist;
		r = h * (fabsf (pl->normal[0]) + fabsf (pl->normal[1]) + fabsf (pl->normal[2]));
		if (sides[i].side == 0 ? (d + r <= 0) : (d - r > 0))
			return false;
	}
	return true;
}

/* GL's light level at the samples of the cell around lattice point L in a
 * liquid */
static void SamplePoint (qmodel_t *wm, const int *L, float cell, medium_acc_t *a)
{
	int	x, y, z, i, n = 0, styles[MAXLIGHTMAPS];
	float	levels[MAXLIGHTMAPS];
	vec3_t	p;
	mleaf_t	*leaf, *last;

	a->count = 0;
	for (y = 0; y < MEDIUM_SUB_XY; y++)
	{
		for (x = 0; x < MEDIUM_SUB_XY; x++)
		{
			p[0] = (L[0] + (x + 0.5f) / MEDIUM_SUB_XY - 0.5f) * cell;
			p[1] = (L[1] + (y + 0.5f) / MEDIUM_SUB_XY - 0.5f) * cell;
			last = NULL;
			for (z = 0; z < MEDIUM_SUB_Z; z++)
			{
				p[2] = (L[2] + (z + 0.5f) / MEDIUM_SUB_Z - 0.5f) * cell;
				vk_medium.samples++;
				leaf = Mod_PointInLeaf (p, wm);
				if (!IsLiquid (leaf->contents))
					continue;
				a->count++;
				/* a leaf has no surface inside it: a sample above in
				 * the same one finds the same light maps below */
				if (leaf != last)
				{
					n = R_LightPointStyles (wm, p, styles, levels);
					vk_medium.traces++;
					last = leaf;
				}
				for (i = 0; i < n; i++)
					AddStyle (a, styles[i], levels[i]);
			}
		}
	}
	for (i = 0; i < a->num_styles; i++)
		a->levels[i] /= q_max (a->count, 1);
}

/* the empty points next to sampled ones take their mean */
static void FillPoints (const medium_box_t *b, medium_acc_t *acc)
{
	int		pass, x, y, z, dx, dy, dz, i, j, num, total = b->size[0] * b->size[1] * b->size[2];
	medium_acc_t	*prev;

	prev = (medium_acc_t *) malloc (total * sizeof(medium_acc_t));
	if (!prev)
		Sys_Error ("%s: out of memory", __thisfunc__);
	for (pass = 0; pass < MEDIUM_FILL_PASSES; pass++)
	{
		memcpy (prev, acc, total * sizeof(medium_acc_t));
		for (z = 0; z < b->size[2]; z++)
		for (y = 0; y < b->size[1]; y++)
		for (x = 0; x < b->size[0]; x++)
		{
			medium_acc_t	*a = &acc[x + b->size[0] * (y + b->size[1] * z)];

			if (a->count > 0)
				continue;	/* has samples in a liquid */
			num = 0;
			for (dz = -1; dz <= 1; dz++)
			for (dy = -1; dy <= 1; dy++)
			for (dx = -1; dx <= 1; dx++)
			{
				const medium_acc_t	*o;

				if (x + dx < 0 || x + dx >= b->size[0] || y + dy < 0 || y + dy >= b->size[1] ||
				    z + dz < 0 || z + dz >= b->size[2])
					continue;
				o = &prev[(x + dx) + b->size[0] * ((y + dy) + b->size[1] * (z + dz))];
				if (o->count <= 0)
					continue;
				for (i = 0; i < o->num_styles; i++)
				{
					for (j = 0; j < a->num_styles && a->styles[j] != o->styles[i]; j++)
						;
					if (j < a->num_styles)
						a->levels[j] += o->levels[i];
					else if (a->num_styles < MEDIUM_SLOTS)
					{
						a->styles[a->num_styles] = o->styles[i];
						a->levels[a->num_styles++] = o->levels[i];
					}
					else
					{
						a->levels[0] += o->levels[i];
						a->overflow = true;
					}
				}
				num++;
			}
			if (!num)
				continue;
			for (i = 0; i < a->num_styles; i++)
				a->levels[i] /= num;
			a->count = -2;	/* filled: the next pass may take it */
			vk_medium.filled++;
		}
		for (i = 0; i < total; i++)
		{
			if (acc[i].count == -2)
				acc[i].count = 1;
		}
	}
	free (prev);
}

/* a baked point into the grid's uvec2: four styles and their levels */
static void PackPoint (medium_acc_t *a, uint32_t *out)
{
	int	i, j, level;
	byte	style;
	float	f;

	/* the brightest first, the ones past 4 folded into it */
	for (i = 1; i < a->num_styles; i++)
	{
		for (j = i; j > 0 && a->levels[j] > a->levels[j - 1]; j--)
		{
			f = a->levels[j]; a->levels[j] = a->levels[j - 1]; a->levels[j - 1] = f;
			style = a->styles[j]; a->styles[j] = a->styles[j - 1]; a->styles[j - 1] = style;
		}
	}
	if (a->num_styles > 4)
	{
		for (i = 4; i < a->num_styles; i++)
			a->levels[0] += a->levels[i];
		a->num_styles = 4;
		a->overflow = true;
	}
	if (a->overflow && a->count > 0)
		vk_medium.folded++;

	out[0] = out[1] = 0;
	for (i = 0; i < 4; i++)
	{
		if (i < a->num_styles && a->count > 0)
		{
			level = (int)(a->levels[i] + 0.5f);
			out[0] |= (uint32_t)a->styles[i] << (8 * i);
			out[1] |= (uint32_t)q_min (q_max (level, 0), 255) << (8 * i);
		}
		else
			out[0] |= (uint32_t)MEDIUM_NO_STYLE << (8 * i);
	}
}

void VK_FreeMedium (void)
{
	if (vk.device)
		VK_DestroyBuffer (&vk_medium.buffer);
	free (vk_medium.data);
	memset (&vk_medium, 0, sizeof(vk_medium));
}

void VK_BuildMedium (qmodel_t *wm)
{
	medium_box_t	*boxes;
	medium_acc_t	*acc;
	int		*leaves, num_leaves = 0, num_boxes = 0, i, b, k, x, y, z, lo[3], hi[3], L[3], num_sides;
	size_t		num_points = 0, words;
	float		cell = MEDIUM_CELL;
	double		start = Sys_DoubleTime ();
	uint32_t	*box_words;
	leaf_side_t	sides[MEDIUM_MAX_SIDES];
	vec3_t		center;

	VK_FreeMedium ();

	leaves = (int *) malloc (q_max (wm->numleafs, 1) * sizeof(int));
	boxes = (medium_box_t *) malloc (q_max (wm->numleafs, 1) * sizeof(medium_box_t));
	if (!leaves || !boxes)
		Sys_Error ("%s: out of memory", __thisfunc__);
	for (i = 1; i <= wm->numleafs; i++)
	{
		if (IsLiquid (wm->leafs[i].contents))
			leaves[num_leaves++] = i;
	}
	vk_medium.num_leaves = num_leaves;

	/* the boxes and their lattices, the cell doubled while too many */
	while (num_leaves)
	{
		num_boxes = BuildBoxes (wm, leaves, num_leaves, cell, boxes);
		num_points = 0;
		for (b = 0; b < num_boxes; b++)
		{
			LatticeRange (boxes[b].mins, boxes[b].maxs, cell, boxes[b].lo, hi);
			for (k = 0; k < 3; k++)
				boxes[b].size[k] = hi[k] - boxes[b].lo[k] + 1;
			boxes[b].first = (int)num_points;
			num_points += (size_t)boxes[b].size[0] * boxes[b].size[1] * boxes[b].size[2];
		}
		if (num_points <= MEDIUM_MAX_POINTS)
			break;
		cell *= 2;
	}

	/* [header][boxes][points], uvec2s */
	words = 2 * (MEDIUM_GRID_HEADER_UVEC2S + (size_t)num_boxes * MEDIUM_BOX_UVEC2S + num_points);
	vk_medium.data = (uint32_t *) calloc (words, sizeof(uint32_t));
	if (!vk_medium.data)
		Sys_Error ("%s: out of memory", __thisfunc__);
	vk_medium.data[0] = (uint32_t)num_boxes;
	memcpy (&vk_medium.data[1], &cell, sizeof(cell));
	vk_medium.data[2] = (uint32_t)num_points;
	for (b = 0; b < num_boxes; b++)
	{
		box_words = vk_medium.data + 2 * (MEDIUM_GRID_HEADER_UVEC2S + b * MEDIUM_BOX_UVEC2S);
		box_words[0] = (uint32_t)boxes[b].lo[0];
		box_words[1] = (uint32_t)boxes[b].lo[1];
		box_words[2] = (uint32_t)boxes[b].lo[2];
		box_words[3] = (uint32_t)(MEDIUM_GRID_HEADER_UVEC2S + num_boxes * MEDIUM_BOX_UVEC2S + boxes[b].first);
		box_words[4] = (uint32_t)boxes[b].size[0];
		box_words[5] = (uint32_t)boxes[b].size[1];
		box_words[6] = (uint32_t)boxes[b].size[2];
	}

	/* each box: sample the points near a liquid leaf, fill, pack */
	for (b = 0; b < num_boxes; b++)
	{
		medium_box_t	*bx = &boxes[b];
		size_t		total = (size_t)bx->size[0] * bx->size[1] * bx->size[2];

		acc = (medium_acc_t *) calloc (total, sizeof(medium_acc_t));
		if (!acc)
			Sys_Error ("%s: out of memory", __thisfunc__);
		for (i = 0; i < (int)total; i++)
			acc[i].count = -1;

		for (i = 0; i < num_leaves; i++)
		{
			const float	*m = wm->leafs[leaves[i]].minmaxs;

			if (!Near (m, m + 3, bx->mins, bx->maxs, 0))
				continue;
			num_sides = LeafSides (&wm->leafs[leaves[i]], sides);
			LatticeRange (m, m + 3, cell, lo, hi);
			for (z = q_max (lo[2], bx->lo[2]); z <= q_min (hi[2], bx->lo[2] + bx->size[2] - 1); z++)
			for (y = q_max (lo[1], bx->lo[1]); y <= q_min (hi[1], bx->lo[1] + bx->size[1] - 1); y++)
			for (x = q_max (lo[0], bx->lo[0]); x <= q_min (hi[0], bx->lo[0] + bx->size[0] - 1); x++)
			{
				medium_acc_t	*a = &acc[(x - bx->lo[0]) + bx->size[0] * ((y - bx->lo[1]) + bx->size[1] * (z - bx->lo[2]))];

				if (a->count >= 0)
					continue;	/* sampled for another leaf */
				center[0] = x * cell;
				center[1] = y * cell;
				center[2] = z * cell;
				if (!CubeTouchesLeaf (sides, num_sides, center, cell * 0.5f))
					continue;	/* the leaf's bounds are loose */
				L[0] = x;
				L[1] = y;
				L[2] = z;
				SamplePoint (wm, L, cell, a);
				vk_medium.sampled++;
				if (a->count > 0)
					vk_medium.liquid++;
			}
		}

		FillPoints (bx, acc);
		for (i = 0; i < (int)total; i++)
			PackPoint (&acc[i], vk_medium.data + 2 * (MEDIUM_GRID_HEADER_UVEC2S + num_boxes * MEDIUM_BOX_UVEC2S + bx->first + i));
		free (acc);
	}

	vk_medium.num_boxes = num_boxes;
	vk_medium.num_points = (int)num_points;
	vk_medium.cell = cell;
	vk_medium.words = words;
	VK_CreateBuffer (&vk_medium.buffer, words * sizeof(uint32_t),
			 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
			 VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VK_MEMORY_DEVICE);
	VK_UploadBuffer (&vk_medium.buffer, 0, vk_medium.data, words * sizeof(uint32_t));
	vk_medium.build_time = Sys_DoubleTime () - start;

	free (leaves);
	free (boxes);
}


/* ==========================================================================
 * The grid on the CPU: medium.glsl's medium_grid_level
 * ========================================================================== */

/* a point by its uvec2 index (the boxes' first is one) */
static const uint32_t *GridPoint (int index)
{
	return vk_medium.data + 2 * (size_t)index;
}

static float PointLevel (int index, const float *style_values)
{
	const uint32_t	*p = GridPoint (index);
	float		level = 0;
	int		k, s;

	for (k = 0; k < 4; k++)
	{
		s = (p[0] >> (8 * k)) & 0xff;
		if (s != MEDIUM_NO_STYLE)
			level += (float)((p[1] >> (8 * k)) & 0xff) * style_values[s];
	}
	return q_max (level, (float)MEDIUM_MIN_LEVEL);
}

static float Lerp (float a, float b, float f)
{
	return a + (b - a) * f;
}

/* GL's light level at p without the dynamic lights, < 0 outside the grid */
static float GridLevel (const vec3_t p, const float *style_values)
{
	const uint32_t	*bw;
	float		g[3], f[3], x00, x10, x01, x11;
	int		i[3], r[3], size[3], first = 0, b, k, sy, sz, i000;

	if (!vk_medium.data)
		return -1;
	for (k = 0; k < 3; k++)
	{
		g[k] = p[k] / vk_medium.cell;
		i[k] = (int)floorf (g[k]);
		f[k] = g[k] - (float)i[k];
	}
	for (b = 0; b < vk_medium.num_boxes; b++)
	{
		bw = vk_medium.data + 2 * (MEDIUM_GRID_HEADER_UVEC2S + b * MEDIUM_BOX_UVEC2S);
		r[0] = i[0] - (int)bw[0];
		r[1] = i[1] - (int)bw[1];
		r[2] = i[2] - (int)bw[2];
		first = (int)bw[3];
		size[0] = (int)bw[4];
		size[1] = (int)bw[5];
		size[2] = (int)bw[6];
		if (r[0] >= 0 && r[1] >= 0 && r[2] >= 0 && r[0] < size[0] - 1 && r[1] < size[1] - 1 && r[2] < size[2] - 1)
			break;
	}
	if (b == vk_medium.num_boxes)
		return -1;

	sy = size[0];
	sz = size[0] * size[1];
	i000 = first + r[0] + sy * r[1] + sz * r[2];
	x00 = Lerp (PointLevel (i000, style_values), PointLevel (i000 + 1, style_values), f[0]);
	x10 = Lerp (PointLevel (i000 + sy, style_values), PointLevel (i000 + sy + 1, style_values), f[0]);
	x01 = Lerp (PointLevel (i000 + sz, style_values), PointLevel (i000 + sz + 1, style_values), f[0]);
	x11 = Lerp (PointLevel (i000 + sz + sy, style_values), PointLevel (i000 + sz + sy + 1, style_values), f[0]);
	return Lerp (Lerp (x00, x10, f[1]), Lerp (x01, x11, f[1]), f[2]);
}

/* the frame's style values, as VK_PrepareMedium gives the shaders */
static void StyleValues (float *values)
{
	int	s;

	for (s = 0; s < MEDIUM_LIGHT_STYLES; s++)
		values[s] = (s < MAX_LIGHTSTYLES) ? r_scene.lightstyles[s] : 0.0f;
}

qboolean VK_MediumLevel (const vec3_t p, float *grid, float *lights)
{
	float	values[MEDIUM_LIGHT_STYLES], add;
	vec3_t	d;
	int	i;

	StyleValues (values);
	*grid = GridLevel (p, values);
	*lights = 0;
	for (i = 0; i < r_scene.num_dlights && i < MEDIUM_MAX_DLIGHTS; i++)
	{
		VectorSubtract (p, r_scene.dlights[i].origin, d);
		add = r_scene.dlights[i].radius - VectorLength (d);
		if (add > 0)
			*lights += add;
	}
	if (*grid < 0)
	{
		*grid = MEDIUM_MIN_LEVEL;	/* water.glsl's outside the grid */
		return false;
	}
	return true;
}


/* ==========================================================================
 * The frame's part: the light styles and the dynamic lights
 * ========================================================================== */

void VK_PrepareMedium (QVKUniformBuffer_t *ubo)
{
	float	values[MEDIUM_LIGHT_STYLES];
	int	i;

	ubo->medium_grid = vk_medium.buffer.address;
	StyleValues (values);
	for (i = 0; i < MEDIUM_LIGHT_STYLES; i++)
		ubo->medium_styles[i >> 2][i & 3] = values[i];
	ubo->num_medium_dlights = q_min (r_scene.num_dlights, MEDIUM_MAX_DLIGHTS);
	for (i = 0; i < ubo->num_medium_dlights; i++)
	{
		VectorCopy (r_scene.dlights[i].origin, ubo->medium_dlights[i]);
		ubo->medium_dlights[i][3] = r_scene.dlights[i].radius;
	}
}


/* ==========================================================================
 * vk_medium: statistics and a check of the shader's lookup
 * ========================================================================== */

#define MEDIUM_CHECK_POINTS	4096	/* a multiple of MEDIUM_CHECK_CHAIN (medium_check.comp) */

/* the check's own random numbers in [0, 1): rand () would shift the
 * game's */
static float CheckRandom (void)
{
	static uint32_t	seed = 1;

	seed = seed * 1664525u + 1013904223u;
	return (float)(seed >> 8) / 16777216.0f;
}

/* runs medium_check.comp, which calls medium_grid_level at points of the
 * boxes, MEDIUM_CHECK_CHAIN in a row carrying the box hint as water.glsl
 * does along a segment, and compares with GridLevel; returns the largest
 * difference */
static float CheckShaderLookup (int *outside, int *mismatched)
{
	VkPushConstantRange		range;
	VkPipelineLayoutCreateInfo	layout_info;
	VkComputePipelineCreateInfo	pipe_info;
	VkPipelineLayout		layout;
	VkPipeline			pipeline;
	VkShaderModule			module;
	VkCommandBuffer			cmd;
	VkMemoryBarrier2		barrier;
	VkDependencyInfo		dep;
	vk_buffer_t			input, result;
	MediumCheckPush			push;
	float				*values, *points, *results, cpu, worst = 0;
	const uint32_t			*bw;
	int				n, b, k;

	module = VK_LoadShader ("medium_check.comp");
	memset (&range, 0, sizeof(range));
	range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	range.size = sizeof(push);
	memset (&layout_info, 0, sizeof(layout_info));
	layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	layout_info.pushConstantRangeCount = 1;
	layout_info.pPushConstantRanges = &range;
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

	/* the style values, then the points: chains of MEDIUM_CHECK_CHAIN, a
	 * random point in a box's lattice (a few past its ends) and steps of up
	 * to 3 cells from it, which sometimes leave the box */
	VK_CreateBuffer (&input, (MEDIUM_LIGHT_STYLES + 4 * MEDIUM_CHECK_POINTS) * sizeof(float),
			 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VK_MEMORY_UPLOAD);
	VK_CreateBuffer (&result, MEDIUM_CHECK_POINTS * sizeof(float),
			 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VK_MEMORY_READBACK);
	values = (float *)input.mapped;
	points = values + MEDIUM_LIGHT_STYLES;
	StyleValues (values);
	for (n = 0; n < MEDIUM_CHECK_POINTS; n++)
	{
		if (n % MEDIUM_CHECK_CHAIN == 0)
		{
			b = (int)(CheckRandom () * vk_medium.num_boxes);
			bw = vk_medium.data + 2 * (MEDIUM_GRID_HEADER_UVEC2S + b * MEDIUM_BOX_UVEC2S);
			for (k = 0; k < 3; k++)
				points[4 * n + k] = ((int)bw[k] - 1.0f + CheckRandom () * ((int)bw[4 + k] + 1)) * vk_medium.cell;
		}
		else
		{
			for (k = 0; k < 3; k++)
				points[4 * n + k] = points[4 * (n - 1) + k] + (CheckRandom () * 6.0f - 3.0f) * vk_medium.cell;
		}
		points[4 * n + 3] = 0;
	}
	VK_CHECK (vmaFlushAllocation (vk.allocator, input.allocation, 0, VK_WHOLE_SIZE));

	memset (&push, 0, sizeof(push));
	push.grid = vk_medium.buffer.address;
	push.styles = input.address;
	push.points = input.address + MEDIUM_LIGHT_STYLES * sizeof(float);
	push.results = result.address;
	push.num_points = MEDIUM_CHECK_POINTS;

	cmd = VK_BeginUpload ();
	vkCmdBindPipeline (cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
	vkCmdPushConstants (cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
	vkCmdDispatch (cmd, (MEDIUM_CHECK_POINTS / MEDIUM_CHECK_CHAIN + 63) / 64, 1, 1);
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

	VK_CHECK (vmaInvalidateAllocation (vk.allocator, result.allocation, 0, VK_WHOLE_SIZE));
	results = (float *)result.mapped;
	*outside = *mismatched = 0;
	for (n = 0; n < MEDIUM_CHECK_POINTS; n++)
	{
		cpu = GridLevel (points + 4 * n, values);
		if (cpu < 0 || results[n] < 0)
		{
			if ((cpu < 0) != (results[n] < 0))
				(*mismatched)++;
			else
				(*outside)++;
			continue;
		}
		worst = q_max (worst, fabsf (cpu - results[n]));
	}

	VK_DestroyBuffer (&input);
	VK_DestroyBuffer (&result);
	vkDestroyPipeline (vk.device, pipeline, NULL);
	vkDestroyPipelineLayout (vk.device, layout, NULL);
	return worst;
}

static void VK_Medium_f (void)
{
	float	grid, lights, worst;
	int	outside, mismatched;

	if (!r_scene.worldmodel || cls.state != ca_connected)
	{
		Con_Printf ("No world loaded\n");
		return;
	}

	Con_Printf ("liquids' light grid: %d liquid leaves, %d boxes, %d points of %.0f units (%.2f MB), baked in %.0f ms\n",
			vk_medium.num_leaves, vk_medium.num_boxes, vk_medium.num_points, vk_medium.cell,
			vk_medium.words * 4.0 / (1024.0 * 1024.0), vk_medium.build_time * 1000.0);
	Con_Printf ("%d points sampled (%d samples, %d light traces), %d with a liquid sample, %d filled from neighbours, %d with styles folded\n",
			vk_medium.sampled, vk_medium.samples, vk_medium.traces, vk_medium.liquid, vk_medium.filled, vk_medium.folded);
	if (VK_MediumLevel (r_scene.vieworg, &grid, &lights))
		Con_Printf ("at the eye: light level %.1f, %.1f with the dynamic lights\n", grid, grid + lights);
	else
		Con_Printf ("the eye is outside the grid\n");
	if (!vk_medium.num_boxes)
		return;

	worst = CheckShaderLookup (&outside, &mismatched);
	Con_Printf ("shader check: %d points, %d outside both, %d inside only one, largest difference %.4f\n",
			MEDIUM_CHECK_POINTS, outside, mismatched, worst);
}

void VK_InitMedium (void)
{
	Cmd_AddCommand ("vk_medium", VK_Medium_f);
}
