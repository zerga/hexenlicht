/* r_light.c -- the light on the first-person weapon, for the game, and the
 * light maps by style for the liquids' light grid
 *
 * GL's R_DrawViewModel samples the world's light at the weapon (the light
 * maps with the current light styles, at least 24, plus the dynamic
 * lights) and leaves it in cl.light_level. The client sends that to the
 * server with every move (cl_input.c), where it becomes the player's
 * light_level: the gamecode's measure of how well monsters see the player
 * (MG_AI.hc's get_visibility) and of when the Assassin cloaks in the
 * shadows (specials.hc). R_ViewModelLight does the same every frame; only
 * the level matters here, not the color GL shades the weapon with.
 * R_LightPointStyles (6.18) finds the same light maps by light style, for
 * vk_medium.c's light grid of the liquids' medium.
 *
 * R_LightPointColor is Hammer of Thyrion's, from gl_rlight.c (the GL_RGBA
 * light map path; gl_model.c loads the light maps as RGB for it, as
 * stubs.c's gl_lightmap_format is always GL_RGBA; a GL configured with
 * gl_lightmapfmt GL_LUMINANCE uses R_LightPoint, a few units off).
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
#include "r_scene.h"

vec3_t		lightspot;
vec3_t		lightcolor;


/*
=============================================================================

LIGHT SAMPLING

=============================================================================
*/

static int RecursiveLightPointColor (vec3_t color, mnode_t *node, vec3_t start, vec3_t end)
{
	float		front, back, frac;
	vec3_t		mid;

loc0:
	if (node->contents < 0)
		return false;		// didn't hit anything

// calculate mid point
	if (node->plane->type < 3)
	{
		front = start[node->plane->type] - node->plane->dist;
		back = end[node->plane->type] - node->plane->dist;
	}
	else
	{
		front = DotProduct(start, node->plane->normal) - node->plane->dist;
		back = DotProduct(end, node->plane->normal) - node->plane->dist;
	}
	// LordHavoc: optimized recursion
	if ((back < 0) == (front < 0))
	{
		node = node->children[front < 0];
		goto loc0;
	}

	frac = front / (front-back);
	mid[0] = start[0] + (end[0] - start[0])*frac;
	mid[1] = start[1] + (end[1] - start[1])*frac;
	mid[2] = start[2] + (end[2] - start[2])*frac;

// go down front side
	if (RecursiveLightPointColor (color, node->children[front < 0], start, mid))
		return true;	// hit something
	else
	{
		int		i, ds, dt;
		msurface_t	*surf;
// check for impact on this node
		VectorCopy (mid, lightspot);
		surf = cl.worldmodel->surfaces + node->firstsurface;
		for (i = 0; i < node->numsurfaces; i++, surf++)
		{
			if (surf->flags & SURF_DRAWTILED)
				continue;	// no lightmaps
			ds = (int) ((float) DotProduct(mid, surf->texinfo->vecs[0]) + surf->texinfo->vecs[0][3]);
			dt = (int) ((float) DotProduct(mid, surf->texinfo->vecs[1]) + surf->texinfo->vecs[1][3]);
			if (ds < surf->texturemins[0] || dt < surf->texturemins[1])
				continue;

			ds -= surf->texturemins[0];
			dt -= surf->texturemins[1];

			if (ds > surf->extents[0] || dt > surf->extents[1])
				continue;
			if (surf->samples)
			{
				// LordHavoc: enhanced to interpolate lighting
				byte	*lightmap;
				float	scale;
				int	maps, line3,
					dsfrac = ds & 15,
					dtfrac = dt & 15,
					r00 = 0, g00 = 0, b00 = 0,
					r01 = 0, g01 = 0, b01 = 0,
					r10 = 0, g10 = 0, b10 = 0,
					r11 = 0, g11 = 0, b11 = 0;

				line3 = ((surf->extents[0]>>4) + 1) * 3;
				lightmap = surf->samples + ((dt>>4) * ((surf->extents[0]>>4) + 1) + (ds>>4)) * 3;
				for (maps = 0; maps < MAXLIGHTMAPS && surf->styles[maps] != 255; maps++)
				{
					scale = (float) d_lightstylevalue[surf->styles[maps]] * 1.0 / 256.0;
					r00 += (float) lightmap[0] * scale;
					g00 += (float) lightmap[1] * scale;
					b00 += (float) lightmap[2] * scale;
					r01 += (float) lightmap[3] * scale;
					g01 += (float) lightmap[4] * scale;
					b01 += (float) lightmap[5] * scale;
					r10 += (float) lightmap[line3+0] * scale;
					g10 += (float) lightmap[line3+1] * scale;
					b10 += (float) lightmap[line3+2] * scale;
					r11 += (float) lightmap[line3+3] * scale;
					g11 += (float) lightmap[line3+4] * scale;
					b11 += (float) lightmap[line3+5] * scale;
					lightmap += ((surf->extents[0]>>4) + 1) * ((surf->extents[1]>>4) + 1) * 3;
				}
				color[0] += (float) ((int) ((((((((r11-r10) * dsfrac) >> 4) + r10)-((((r01-r00) * dsfrac) >> 4) + r00)) * dtfrac) >> 4) + ((((r01-r00) * dsfrac) >> 4) + r00)));
				color[1] += (float) ((int) ((((((((g11-g10) * dsfrac) >> 4) + g10)-((((g01-g00) * dsfrac) >> 4) + g00)) * dtfrac) >> 4) + ((((g01-g00) * dsfrac) >> 4) + g00)));
				color[2] += (float) ((int) ((((((((b11-b10) * dsfrac) >> 4) + b10)-((((b01-b00) * dsfrac) >> 4) + b00)) * dtfrac) >> 4) + ((((b01-b00) * dsfrac) >> 4) + b00)));
			}
			return true; // success
		}
	// go down back side
		return RecursiveLightPointColor (color, node->children[front >= 0], mid, end);
	}
}

float R_LightPointColor (vec3_t p)
{
	vec3_t		end;

	if (!cl.worldmodel->lightdata)
	{
		lightcolor[0] = lightcolor[1] = lightcolor[2] = 255.0;
		return 255.0;
	}

	end[0] = p[0];
	end[1] = p[1];
	end[2] = p[2] - 2048;

	lightcolor[0] = lightcolor[1] = lightcolor[2] = 0;
	RecursiveLightPointColor (lightcolor, cl.worldmodel->nodes, p, end);
	return (lightcolor[0] + lightcolor[1] + lightcolor[2]) / 3.0;
}


/*
=============================================================================

THE WEAPON'S LIGHT LEVEL

=============================================================================
*/

/* GL's light level at p with the dynamic lights added to it, in
 * R_DrawViewModel's order (so cl.light_level stays GL's to the bit) */
static float AddDynamicLights (const vec3_t p, float level)
{
	dlight_t	*dl;
	vec3_t		dist;
	float		add;
	int		lnum;

	for (lnum = 0; lnum < MAX_DLIGHTS; lnum++)
	{
		dl = &cl_dlights[lnum];
		if (!dl->radius)
			continue;
		if (dl->die < cl.time)
			continue;

		VectorSubtract (p, dl->origin, dist);
		add = dl->radius - VectorLengthFast(dist);
		if (add > 0)
			level += add;
	}

	return level;
}

/* R_DrawViewModel's lighting of cl.viewent, into cl.light_level; in
 * R_RenderView, after the light styles are animated */
void R_ViewModelLight (void)
{
	entity_t	*e = &cl.viewent;
	float		ambientlight;

	if (!e->model)
		return;

	ambientlight = R_LightPointColor (e->origin);
	if (ambientlight < 24)
		ambientlight = 24;	// always give some light on gun

	ambientlight = AddDynamicLights (e->origin, ambientlight);

	cl.light_level = (int)ambientlight;
}


/*
=============================================================================

THE LIGHT MAPS BY STYLE (6.18)

The medium's light grid (vk_medium.c) bakes GL's light level of a model at
points in the liquids once, at map load, so it can't take the light styles'
values of that moment: R_LightPointStyles finds the surface
R_LightPointColor would (straight below the point) and gives the level of
each of its light maps at style value 1, the RGB average interpolated
between the texels as R_LightPointColor does (in floats; GL's integer steps
are left out).

=============================================================================
*/

/* -1: nothing below; else the number of light maps (0 on a lit surface
 * without samples) */
static int RecursiveLightPointStyles (qmodel_t *model, mnode_t *node, const vec3_t p, const vec3_t end,
				      int *styles, float *levels)
{
	float		front, back, frac;
	vec3_t		start, mid;
	int		i, n, ds, dt, maps, line3, size;
	msurface_t	*surf;
	const byte	*lightmap;
	float		fs, ft, top, bottom;

	VectorCopy (p, start);
	while (node->contents >= 0)
	{
		if (node->plane->type < 3)
		{
			front = start[node->plane->type] - node->plane->dist;
			back = end[node->plane->type] - node->plane->dist;
		}
		else
		{
			front = DotProduct(start, node->plane->normal) - node->plane->dist;
			back = DotProduct(end, node->plane->normal) - node->plane->dist;
		}
		if ((back < 0) == (front < 0))
		{
			node = node->children[front < 0];
			continue;
		}

		frac = front / (front - back);
		mid[0] = start[0] + (end[0] - start[0]) * frac;
		mid[1] = start[1] + (end[1] - start[1]) * frac;
		mid[2] = start[2] + (end[2] - start[2]) * frac;

		/* the front side first, then this node's surfaces, then the back */
		n = RecursiveLightPointStyles (model, node->children[front < 0], start, mid, styles, levels);
		if (n >= 0)
			return n;

		surf = model->surfaces + node->firstsurface;
		for (i = 0; i < node->numsurfaces; i++, surf++)
		{
			if (surf->flags & SURF_DRAWTILED)
				continue;	/* no lightmaps */
			ds = (int) ((float) DotProduct(mid, surf->texinfo->vecs[0]) + surf->texinfo->vecs[0][3]);
			dt = (int) ((float) DotProduct(mid, surf->texinfo->vecs[1]) + surf->texinfo->vecs[1][3]);
			if (ds < surf->texturemins[0] || dt < surf->texturemins[1])
				continue;
			ds -= surf->texturemins[0];
			dt -= surf->texturemins[1];
			if (ds > surf->extents[0] || dt > surf->extents[1])
				continue;

			n = 0;
			if (surf->samples)
			{
				line3 = ((surf->extents[0] >> 4) + 1) * 3;
				size = line3 * ((surf->extents[1] >> 4) + 1);
				lightmap = surf->samples + (dt >> 4) * line3 + (ds >> 4) * 3;
				fs = (ds & 15) / 16.0f;
				ft = (dt & 15) / 16.0f;
				for (maps = 0; maps < MAXLIGHTMAPS && surf->styles[maps] != 255; maps++, lightmap += size)
				{
					top = (lightmap[0] + lightmap[1] + lightmap[2]) +
					      ((lightmap[3] + lightmap[4] + lightmap[5]) - (lightmap[0] + lightmap[1] + lightmap[2])) * fs;
					bottom = (lightmap[line3 + 0] + lightmap[line3 + 1] + lightmap[line3 + 2]) +
						 ((lightmap[line3 + 3] + lightmap[line3 + 4] + lightmap[line3 + 5]) -
						  (lightmap[line3 + 0] + lightmap[line3 + 1] + lightmap[line3 + 2])) * fs;
					styles[n] = surf->styles[maps];
					levels[n] = (top + (bottom - top) * ft) / 3.0f;
					n++;
				}
			}
			return n;
		}

		VectorCopy (mid, start);	/* the back side */
		node = node->children[front >= 0];
	}

	return -1;	/* didn't hit anything */
}

/* GL's light maps' level at p by style (R_LightPointColor's surface): up
 * to MAXLIGHTMAPS styles and their levels at style value 1; returns how
 * many (0: nothing lit below). Without light data, as R_LightPointColor,
 * full light (255 in style 0). */
int R_LightPointStyles (qmodel_t *model, const vec3_t p, int *styles, float *levels)
{
	vec3_t		end;
	int		n;

	if (!model->lightdata)
	{
		styles[0] = 0;
		levels[0] = 255.0f;
		return 1;
	}

	end[0] = p[0];
	end[1] = p[1];
	end[2] = p[2] - 2048;
	n = RecursiveLightPointStyles (model, model->nodes, p, end, styles, levels);
	return (n < 0) ? 0 : n;
}
