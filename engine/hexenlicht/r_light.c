/* r_light.c -- the light on the first-person weapon, for the game, and the
 * light of the liquid around the camera
 *
 * GL's R_DrawViewModel samples the world's light at the weapon (the light
 * maps with the current light styles, at least 24, plus the dynamic
 * lights) and leaves it in cl.light_level. The client sends that to the
 * server with every move (cl_input.c), where it becomes the player's
 * light_level: the gamecode's measure of how well monsters see the player
 * (MG_AI.hc's get_visibility) and of when the Assassin cloaks in the
 * shadows (specials.hc). R_ViewModelLight does the same every frame; only
 * the level matters here, not the color GL shades the weapon with.
 * R_MediumLight (6.17) gives the liquid around the camera its light from
 * the same light level, averaged around the camera and eased.
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

THE MEDIUM'S LIGHT (6.17)

The light the liquid around the camera scatters towards the eye (6.5's
medium, water.glsl, DECISIONS X22) is GL's light level of a model, as
cl.light_level is, but over the liquid around the camera rather than at
the eye: R_LightPointColor takes the light map of the floor straight
below a point, so one point jumps between GL's least (24) and a bright
patch as the player swims over the floor (demo2's moat: 24 to 128). The
light maps' level (at least 24, as GL's) is averaged over the eye and a
ring of points around it at eye height that the liquid connects to it,
eased toward that over MEDIUM_LIGHT_TAU of game time; the dynamic lights
at the eye are added as they are (a muzzle flash brightens the medium at
once, as in GL's cl.light_level). In the air the medium's light stays
cl.light_level (looking into a liquid from above). DECISIONS X31.

=============================================================================
*/

#define MEDIUM_RING_POINTS	8
#define MEDIUM_RING_RADIUS	192.0f	/* units, at eye height */
#define MEDIUM_LIGHT_TAU	0.5	/* seconds of game time */

static int	medium_contents;	/* the liquid eased in, 0 = none (start over) */
static double	medium_time;		/* cl.time of the last easing */
static float	medium_level;		/* the eased light maps' level */

/* whether the segment from start to end stays in leaves of these contents
 * (the BSP's leaves only: brush entities don't count); a point on a plane
 * is behind it, as in Mod_PointInLeaf, so the segment starts in the eye's
 * leaf */
static qboolean SegmentInContents (mnode_t *node, const vec3_t start, const vec3_t end, int contents)
{
	float		front, back, frac;
	vec3_t		mid;

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
		if ((front > 0) == (back > 0))
		{
			node = node->children[front <= 0];
			continue;
		}

		frac = front / (front - back);
		mid[0] = start[0] + (end[0] - start[0]) * frac;
		mid[1] = start[1] + (end[1] - start[1]) * frac;
		mid[2] = start[2] + (end[2] - start[2]) * frac;
		return SegmentInContents (node->children[front <= 0], start, mid, contents)
			&& SegmentInContents (node->children[front > 0], mid, end, contents);
	}

	return node->contents == contents;
}

/* GL's light maps' level for a model at p (at least 24, without the
 * dynamic lights) */
static float LightMapLevel (vec3_t p)
{
	float	level = R_LightPointColor (p);

	return (level < 24) ? 24 : level;
}

/* r_scene.water_light and its parts; in R_RenderView, after R_BuildScene */
void R_MediumLight (void)
{
	vec3_t		eye, p;
	float		sum, a, dt;
	int		i, contents = r_scene.viewcontents;

	if (contents != CONTENTS_WATER && contents != CONTENTS_SLIME && contents != CONTENTS_LAVA)
	{
		medium_contents = 0;
		r_scene.water_light = (float)cl.light_level / 200.0f;	/* as before 6.17 */
		r_scene.water_light_now = r_scene.water_light;
		r_scene.water_light_points = 0;
		return;
	}

	VectorCopy (r_scene.vieworg, eye);
	sum = LightMapLevel (eye);
	r_scene.water_light_points = 1;
	for (i = 0; i < MEDIUM_RING_POINTS; i++)
	{
		a = i * (2.0f * (float)M_PI / MEDIUM_RING_POINTS);
		p[0] = eye[0] + MEDIUM_RING_RADIUS * cosf (a);
		p[1] = eye[1] + MEDIUM_RING_RADIUS * sinf (a);
		p[2] = eye[2];
		if (!SegmentInContents (cl.worldmodel->nodes, eye, p, contents))
			continue;	/* a wall, the air or another pool on the way */
		sum += LightMapLevel (p);
		r_scene.water_light_points++;
	}
	sum /= r_scene.water_light_points;

	/* the clock steps back a little when a packet is late (CL_LerpPoint,
	 * network games): the value stands then */
	dt = q_max (0.0f, (float)(r_scene.time - medium_time));
	if (contents != medium_contents)
		medium_level = sum;	/* came into the liquid */
	else
		medium_level += (sum - medium_level) * (1.0f - expf (-dt / (float)MEDIUM_LIGHT_TAU));
	medium_contents = contents;
	medium_time = r_scene.time;

	r_scene.water_light = AddDynamicLights (eye, medium_level) / 200.0f;
	r_scene.water_light_now = AddDynamicLights (eye, sum) / 200.0f;
}

/* a new map: start over */
void R_ResetMediumLight (void)
{
	medium_contents = 0;
	medium_time = 0;
	medium_level = 0;
}
