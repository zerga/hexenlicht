/* vk_maplights.c -- the map's light entities as the path tracer's lights
 *
 * The lights are those utils/light (the compiler of Hexen II's lightmaps)
 * lit the map from, read from the entity lump with its rules
 * (utils/light/entities.c, ltface.c, trace.c):
 *  - every entity with a level: atoi of a key starting with "light" (so
 *    also lightvalue1/2, and "2oo" is 2); one whose classname starts with
 *    "light" (plain lights, torches, flames, candles, ...) has 300 without;
 *    the light reaches as far as its level (vk_light.c's sphere range). The
 *    compiler kept the last such key it read and wrote the lump's keys in
 *    reverse (its epair lists), so in the lump the first one counts, as
 *    for every key here;
 *  - its style, which the compiler gave switchable lights (32 and up) and
 *    wrote into the lump (animated with 4.2);
 *  - a target makes it a spotlight towards the first entity of that
 *    targetname (its origin key, 0 0 0 without one), its cone "angle"
 *    degrees wide (default 40); aimed at its own origin it lit nothing
 *    (narrower than 180) or everywhere;
 *  - dropped, as the compiler lit nothing from them: a light whose origin
 *    is inside solid (its trace starts there; a point within ON_EPSILON of
 *    a plane goes to the side of the trace's other end, so only when every
 *    leaf that close is solid) and one with a level below 0.
 * Brightness: the intensity (pi x radiance, as vk_testlight's) is
 * r_maplight_scale x (level / 300)^3: the power under which inverse-square
 * light scales with each light's range as the compiler's linear falloff
 * does (twice the level and the distances, twice the light); the curve is
 * calibrated in 4.9.
 * Colors (r_maplight_colors 1; 0 white, the original's and Hammer of
 * Thyrion's default): HoT's colored light, the colors utils/jsh2color baked
 * its .lit files from (vk_lightcolor.c: torches orange, plain lights by the
 * textures near them, the others 255 225 200; white on a map where none is
 * colored), or on a map whose lights have _color (later compilers' key,
 * 0-1 or 0-255; no original map has it) those, the others white, as that
 * compiler's .lit. The 0-255 color multiplied GL's lightmap, which
 * multiplies the texture in sRGB space: the light's color is its sRGB to
 * linear conversion, which shows the same hue on a wall (no scaling to
 * white's brightness: as in HoT, orange light is darker).
 * Plain lights just over lava or under its surface (vk_emissive.c's
 * VK_OverLava) are the mappers' stand-ins for the lava's light: vk_light.c
 * leaves them out while the lava emits (4.5).
 * The map file's light lines (vk_mapfile.c, 4.7) apply whenever the lights
 * are built: "light" changes the lights whose entity origin it names (off,
 * level, scale, color, style, origin; a moved spotlight keeps its
 * direction), "addlight" adds one (white unless it has a color); moved or
 * added lights inside solid are dropped. A light's intensity is its
 * level's times its scale and the map's r_map_light_scale; a map file
 * color replaces its color with r_maplight_colors 1.
 * VK_LoadWorld calls VK_LoadMapLights before the light lists are built;
 * vk_light.c's VK_UpdateLights takes these lights with the test lights.
 * VK_MapLightAt tells vk_instance.c which models stand at a light entity's
 * origin: the torches and flames the light entities' game code spawns
 * there, whose mesh surrounds the light; they go into their own group,
 * which shadow rays don't see, and glow (4.5). The entity's origin, as the
 * game spawns the model there: a light the map file moves or takes out
 * leaves its model glowing and without shadows.
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
#include "shaders/hl_shared.h"

#define DEFAULT_LIGHT_LEVEL	VK_DEFAULT_LIGHT_LEVEL	/* utils/light's DEFAULTLIGHTLEVEL */
#define DEFAULT_SPOT_ANGLE	40.0f	/* degrees wide: utils/light's 20 each side */
#define SOLID_EPSILON		0.1f	/* utils/light's ON_EPSILON */
#define SPOT_COS_OMNI		-0.99984770f	/* cos(179 degrees): a cone at least this wide (half) lights everywhere */
#define SPOT_COS_NARROWEST	0.99984770f	/* cos(1 degree): narrower cones are made this wide (sphere_light_spot's edge) */
#define AT_HASH_SIZE		8192	/* VK_MapLightAt's table, a power of 2 above MAX_LIGHT_POLYS */

COMPILE_TIME_ASSERT(maplight_hash, AT_HASH_SIZE > MAX_LIGHT_POLYS && MAX_LIGHT_POLYS < 32767);

static void MapLightsChanged (cvar_t *var);
static void MapLightColorsChanged (cvar_t *var);

static cvar_t	r_maplights = {"r_maplights", "1", CVAR_NONE};
static cvar_t	r_maplight_scale = {"r_maplight_scale", "1000", CVAR_NONE};	/* pi x radiance of a level 300 light */
static cvar_t	r_maplight_colors = {"r_maplight_colors", "1", CVAR_ARCHIVE};	/* 0 white */

/* an entity as utils/light parses it */
enum { KEY_CLASSNAME = 1, KEY_TARGET = 2, KEY_TARGETNAME = 4, KEY_ORIGIN = 8, KEY_LEVEL = 16, KEY_STYLE = 32,
       KEY_ANGLE = 64, KEY_COLOR = 128 };

typedef struct
{
	int		keys;		/* KEY_*: read (the first one counts) */
	char		classname[64];
	char		target[64];
	char		targetname[64];
	vec3_t		origin;
	int		level;
	int		style;
	float		angle;
	vec3_t		color;
	qboolean	has_color;
} lightent_t;

static vk_maplight_t	maplights[MAX_LIGHT_POLYS];
static int		num_maplights;
static int		at_points[MAX_LIGHT_POLYS][3];	/* the light entities' origins, to the unit: where their models are */
static int		num_at_points;
static short		at_hash[AT_HASH_SIZE];	/* at_points index + 1, 0 = empty */
static int		dropped_points[MAX_LIGHT_POLYS][3];	/* light entities the compiler lit nothing from (vk_mapfile's report) */
static int		num_dropped_points;

/* what r_maplight_colors picks from, per map light */
static struct
{
	char		classname[32];	/* vk_lights colors */
	int		entity;		/* in the lump, -1 = the map file's addlight */
	vec3_t		lump_origin;	/* the map file's light edits match it */
	int		jsh[3];		/* jsh2color's 0-275 */
	vec3_t		own;		/* _color, 0-1 */
	qboolean	has_own;
	vec3_t		edited;		/* the map file's color, sRGB 0-1 */
	qboolean	has_edited;
} maplight_colors[MAX_LIGHT_POLYS];
static vk_lightcolors_t	colorinfo;	/* list NULL: not computed (the map's own colors) */

static struct
{
	int		entities;	/* light entities in the lump: a "light" classname or a level */
	int		others;		/* of them, other classnames with a level */
	int		in_solid;	/* dropped: the origin inside solid */
	int		unlit;		/* dropped: a level below 0, a narrow spot aimed at its origin */
	int		over;		/* dropped: more than MAX_LIGHT_POLYS */
	int		spots;
	int		unmatched;	/* a target no entity has: not a spot */
	int		styled;		/* style other than 0 */
	int		colored;	/* with _color */
	int		orange, textured, warm;	/* jsh2color's torch orange, from textures, 255 225 200 */
	int		on_models;	/* models in the light group last frame (vk_instance.c): at a light's origin, owning a dynamic light */
	int		over_lava;	/* plain lights close over lava (4.5) */
	int		over_lava_styled;	/* of them, with a style */
	int		edited;		/* changed by the map file's light lines (4.7) */
	int		edit_off;	/* of them, taken out */
	int		edit_moved;
	int		edit_added;	/* its addlight lines */
	int		edit_in_solid;	/* moved or added inside solid: dropped */
	int		edit_over;	/* addlights past MAX_LIGHT_POLYS */
} stats;


static unsigned AtHash (const int *p)
{
	return ((unsigned)p[0] * 73856093u ^ (unsigned)p[1] * 19349663u ^ (unsigned)p[2] * 83492791u) & (AT_HASH_SIZE - 1);
}

static void RoundOrigin (const vec3_t v, int *p)
{
	int	k;

	for (k = 0; k < 3; k++)
		p[k] = (int)floorf (v[k] + 0.5f);
}

/* is one of the map's light entities at the origin (to the unit: the entity
 * lump's origins are integers and the server sends static entities to 1/8
 * unit), with the map's lights on (r_maplights)? The entity's origin, where
 * the game spawns its torch or flame model, whatever the map file does with
 * the light (4.7: moved or taken out, the model still glows as GL's
 * MLS_ABSLIGHT flame and casts no shadows) */
qboolean VK_MapLightAt (const vec3_t origin)
{
	int		p[3];
	unsigned	h;

	if (!num_at_points || !r_maplights.integer)
		return false;
	RoundOrigin (origin, p);
	for (h = AtHash (p); at_hash[h]; h = (h + 1) & (AT_HASH_SIZE - 1))
	{
		const int	*q = at_points[at_hash[h] - 1];

		if (p[0] == q[0] && p[1] == q[1] && p[2] == q[2])
			return true;
	}
	return false;
}

/* was a light entity at the point (to the unit) that the compiler lit
 * nothing from (inside solid, unlit)? (vk_mapfile's report of a light line
 * that matched nothing) */
qboolean VK_MapLightDroppedAt (const int *p)
{
	int	i;

	for (i = 0; i < num_dropped_points; i++)
	{
		if (p[0] == dropped_points[i][0] && p[1] == dropped_points[i][1] && p[2] == dropped_points[i][2])
			return true;
	}
	return false;
}

static void AddDroppedPoint (const vec3_t origin)
{
	if (num_dropped_points < MAX_LIGHT_POLYS)
		RoundOrigin (origin, dropped_points[num_dropped_points++]);
}

/* utils/light's ParseEntity: its keys, the others skipped; the first of
 * each counts (see the top) */
static const char *ParseEntity (const char *data, lightent_t *e)
{
	memset (e, 0, sizeof(*e));
	while ((data = COM_Parse (data)) != NULL && com_token[0] != '}')
	{
		char	key[64];
		int	k;

		q_strlcpy (key, com_token, sizeof(key));
		if ((data = COM_Parse (data)) == NULL)
			break;
		k = !strcmp (key, "classname") ? KEY_CLASSNAME : !strcmp (key, "target") ? KEY_TARGET :
		    !strcmp (key, "targetname") ? KEY_TARGETNAME : !strcmp (key, "origin") ? KEY_ORIGIN :
		    !strncmp (key, "light", 5) ? KEY_LEVEL : !strcmp (key, "style") ? KEY_STYLE :
		    !strcmp (key, "angle") ? KEY_ANGLE : !strcmp (key, "_color") ? KEY_COLOR : 0;
		if (!k || (e->keys & k))
			continue;
		e->keys |= k;
		switch (k)
		{
		case KEY_CLASSNAME:	q_strlcpy (e->classname, com_token, sizeof(e->classname)); break;
		case KEY_TARGET:	q_strlcpy (e->target, com_token, sizeof(e->target)); break;
		case KEY_TARGETNAME:	q_strlcpy (e->targetname, com_token, sizeof(e->targetname)); break;
		case KEY_ORIGIN:	sscanf (com_token, "%f %f %f", &e->origin[0], &e->origin[1], &e->origin[2]); break;
		case KEY_LEVEL:		e->level = atoi (com_token); break;
		case KEY_STYLE:		e->style = atoi (com_token); break;
		case KEY_ANGLE:		e->angle = (float)atof (com_token); break;
		case KEY_COLOR:		/* not utils/light's: later compilers' */
			e->has_color = sscanf (com_token, "%f %f %f", &e->color[0], &e->color[1], &e->color[2]) == 3;
			break;
		}
	}
	return data;
}

/* the entities of the lump, utils/light's LoadEntities (a new array; the
 * caller frees it) */
static lightent_t *ParseEntities (const char *data, int *count)
{
	lightent_t	*ents = NULL;
	int		n = 0, size = 0;

	while (data && (data = COM_Parse (data)) != NULL && com_token[0] == '{')
	{
		if (n == size)
		{
			size = size ? size * 2 : 1024;
			ents = (lightent_t *) realloc (ents, size * sizeof(*ents));
			if (!ents)
				Sys_Error ("%s: out of memory", __thisfunc__);
		}
		data = ParseEntity (data, &ents[n++]);
	}
	*count = n;
	return ents;
}

/* is every leaf within SOLID_EPSILON of the point solid? utils/light's
 * TestLine takes a start point that close to a plane to the side of the
 * trace's other end, so a light on a solid's face lit the open side */
static qboolean OriginInSolid (const mnode_t *node, const vec3_t p)
{
	while (node->contents >= 0)
	{
		float	d = DotProduct (p, node->plane->normal) - node->plane->dist;

		if (d >= SOLID_EPSILON)
			node = node->children[0];
		else if (d <= -SOLID_EPSILON)
			node = node->children[1];
		else
		{
			if (!OriginInSolid (node->children[0], p))
				return false;
			node = node->children[1];
		}
	}
	return node->contents == CONTENTS_SOLID;
}

/* a color GL multiplied its lightmaps with (in sRGB space) as the light's
 * linear color: the same hue on a wall (above 1 by the curve's power) */
float VK_SRGBToLinear (float c)
{
	return (c <= 0.04045f) ? c / 12.92f : powf ((c + 0.055f) / 1.055f, 2.4f);
}

/* the lights' colors by r_maplight_colors (see the top) */
static void ApplyColors (void)
{
	int	i, k;

	for (i = 0; i < num_maplights; i++)
	{
		float	*c = maplights[i].color;

		VectorSet (c, 1.0f, 1.0f, 1.0f);
		if (!r_maplight_colors.integer)
			continue;
		if (maplight_colors[i].has_edited)	/* the map file's (4.7) */
		{
			for (k = 0; k < 3; k++)
				c[k] = VK_SRGBToLinear (maplight_colors[i].edited[k]);
		}
		else if (maplight_colors[i].entity < 0)
		{
			/* the map file's addlight without a color: white */
		}
		else if (stats.colored)
		{
			if (maplight_colors[i].has_own)
				for (k = 0; k < 3; k++)
					c[k] = VK_SRGBToLinear (maplight_colors[i].own[k]);
		}
		else if (colorinfo.colored)
		{
			for (k = 0; k < 3; k++)
				c[k] = VK_SRGBToLinear (maplight_colors[i].jsh[k] / 255.0f);
		}
	}
}

/* the map file's light lines (vk_mapfile.c, 4.7): "light" changes the
 * lights whose entity origin it names, "addlight" adds one; lights taken
 * out, or moved or added inside solid, are dropped (as the compiler lit
 * nothing from inside solid) */
static void ApplyEdits (qmodel_t *worldmodel)
{
	vk_mapedit_t	*edits;
	qboolean	drop[MAX_LIGHT_POLYS];
	int		num_edits, i, j, q[3], out;

	edits = VK_MapEdits (&num_edits);
	memset (drop, 0, sizeof(drop));
	for (j = 0; j < num_edits; j++)
		edits[j].matched = 0;
	for (i = 0; i < num_maplights; i++)
	{
		vk_maplight_t	*l = &maplights[i];
		qboolean	edited = false, moved = false;

		RoundOrigin (maplight_colors[i].lump_origin, q);
		for (j = 0; j < num_edits; j++)
		{
			const vk_mapedit_t	*e = &edits[j];

			if (e->add || e->at[0] != q[0] || e->at[1] != q[1] || e->at[2] != q[2])
				continue;
			edits[j].matched++;
			edited = true;
			if (e->keys & MAPEDIT_OFF)
				drop[i] = true;
			if (e->keys & MAPEDIT_LEVEL)
				l->level = e->level;
			if (e->keys & MAPEDIT_SCALE)
				l->scale = e->scale;
			if (e->keys & MAPEDIT_STYLE)
				l->style = e->style;
			if (e->keys & MAPEDIT_COLOR)
			{
				VectorCopy (e->color, maplight_colors[i].edited);
				maplight_colors[i].has_edited = true;
			}
			if (e->keys & MAPEDIT_ORIGIN)
			{
				VectorCopy (e->origin, l->origin);
				moved = true;
			}
		}
		stats.edited += edited;
		stats.edit_off += drop[i];
		if (moved && !drop[i] && OriginInSolid (worldmodel->nodes, l->origin))
		{
			drop[i] = true;
			stats.edit_in_solid++;
		}
		stats.edit_moved += (moved && !drop[i]);
	}

	for (j = 0; j < num_edits; j++)
	{
		vk_mapedit_t	*e = &edits[j];
		vk_maplight_t	*l;

		if (!e->add)
			continue;
		if (OriginInSolid (worldmodel->nodes, e->origin))
		{
			stats.edit_in_solid++;
			continue;
		}
		if (num_maplights == MAX_LIGHT_POLYS)
		{
			stats.edit_over++;
			continue;
		}
		l = &maplights[num_maplights];
		memset (l, 0, sizeof(*l));
		memset (&maplight_colors[num_maplights], 0, sizeof(maplight_colors[0]));
		drop[num_maplights] = false;
		VectorCopy (e->origin, l->origin);
		VectorCopy (e->origin, maplight_colors[num_maplights].lump_origin);
		l->level = e->level;
		l->scale = e->scale;
		l->style = e->style;
		q_strlcpy (maplight_colors[num_maplights].classname, "addlight", sizeof(maplight_colors[0].classname));
		maplight_colors[num_maplights].entity = -1;
		if (e->keys & MAPEDIT_COLOR)
		{
			VectorCopy (e->color, maplight_colors[num_maplights].edited);
			maplight_colors[num_maplights].has_edited = true;
		}
		e->matched = 1;
		stats.edit_added++;
		num_maplights++;
	}

	/* the dropped ones out, the colors' table in step */
	for (i = out = 0; i < num_maplights; i++)
	{
		if (drop[i])
			continue;
		if (out != i)
		{
			maplights[out] = maplights[i];
			maplight_colors[out] = maplight_colors[i];
		}
		out++;
	}
	num_maplights = out;
}

/* a new map (VK_LoadWorld, before the light lists): its light entities,
 * with the map file's light lines (4.7) */
void VK_LoadMapLights (qmodel_t *worldmodel)
{
	lightent_t	*ents;
	int		n, i, j, k;

	memset (&stats, 0, sizeof(stats));
	memset (&colorinfo, 0, sizeof(colorinfo));
	memset (at_hash, 0, sizeof(at_hash));
	num_maplights = num_at_points = num_dropped_points = 0;
	ents = ParseEntities (worldmodel->entities, &n);
	for (i = 0; i < n; i++)
	{
		lightent_t	*e = &ents[i];
		vk_maplight_t	*l;
		unsigned	h;

		/* utils/light's LoadEntities and LightFace: a "light" classname
		 * has a level, every entity with one is a light */
		if (!strncmp (e->classname, "light", 5) && !e->level)
			e->level = DEFAULT_LIGHT_LEVEL;
		if (!e->level)
			continue;
		stats.entities++;
		stats.others += (strncmp (e->classname, "light", 5) != 0);
		if (e->level < 0)
		{
			stats.unlit++;
			AddDroppedPoint (e->origin);
			continue;
		}
		if (OriginInSolid (worldmodel->nodes, e->origin))
		{
			stats.in_solid++;
			AddDroppedPoint (e->origin);
			continue;
		}
		if (num_maplights == MAX_LIGHT_POLYS)
		{
			stats.over++;
			continue;
		}
		l = &maplights[num_maplights];
		memset (l, 0, sizeof(*l));
		memset (&maplight_colors[num_maplights], 0, sizeof(maplight_colors[0]));
		VectorCopy (e->origin, l->origin);
		VectorCopy (e->origin, maplight_colors[num_maplights].lump_origin);
		l->level = e->level;
		l->scale = 1.0f;
		l->style = (e->style > 0 && e->style < 256) ? e->style : 0;	/* utils/light allows 0-254 */
		q_strlcpy (maplight_colors[num_maplights].classname, e->classname, sizeof(maplight_colors[0].classname));
		maplight_colors[num_maplights].entity = i;
		if (e->target[0])
		{
			/* utils/light's MatchTargets: the first entity of the targetname */
			for (j = 0; j < n && strcmp (ents[j].targetname, e->target); j++)
				;
			if (j == n)
			{
				stats.unmatched++;
			}
			else
			{
				float	half = ((e->angle != 0.0f) ? e->angle : DEFAULT_SPOT_ANGLE) * 0.5f;
				float	c = cosf (half * (float)M_PI / 180.0f);

				/* utils/light's cone test skips a point when
				 * dot(towards the target, from the point to the light) > -cos(half) */
				VectorSubtract (ents[j].origin, e->origin, l->spot_dir);
				if (VectorNormalize (l->spot_dir) == 0.0f && c > 0.0f)
				{
					stats.unlit++;	/* aimed at its own origin: every point skipped */
					AddDroppedPoint (e->origin);
					continue;
				}
				/* by the cosine, periodic as the compiler's (angle -360 or 720 too) */
				if (VectorLength (l->spot_dir) == 0.0f || c <= SPOT_COS_OMNI)
				{
					VectorClear (l->spot_dir);	/* lights everywhere: not a spot */
				}
				else
				{
					l->spot_cos = q_min (c, SPOT_COS_NARROWEST);
					stats.spots++;
				}
			}
		}
		if (e->has_color)
		{
			float	m = q_max (e->color[0], q_max (e->color[1], e->color[2]));

			for (k = 0; k < 3; k++)
				maplight_colors[num_maplights].own[k] = q_max ((m > 1.0f) ? e->color[k] / 255.0f : e->color[k], 0.0f);
			maplight_colors[num_maplights].has_own = true;
			stats.colored++;
		}
		/* where the game spawns its model (VK_MapLightAt), whatever the
		 * map file does with the light */
		RoundOrigin (e->origin, at_points[num_at_points]);
		for (h = AtHash (at_points[num_at_points]); at_hash[h]; h = (h + 1) & (AT_HASH_SIZE - 1))
			;
		at_hash[h] = (short)(num_at_points + 1);
		num_at_points++;
		num_maplights++;
	}
	free (ents);

	ApplyEdits (worldmodel);	/* the map file's light lines (4.7) */
	for (i = 0; i < num_maplights; i++)
	{
		vk_maplight_t	*l = &maplights[i];

		stats.styled += (l->style != 0);
		/* the mappers lit lava with plain lights just over it or in it:
		 * left out while it emits (vk_emissive.c) */
		l->over_lava = !strcmp (maplight_colors[i].classname, "light") && VK_OverLava (l->origin);
		stats.over_lava += l->over_lava;
		stats.over_lava_styled += (l->over_lava && l->style != 0);
	}

	/* jsh2color's colors, unless the map has its own */
	if (!stats.colored && num_maplights)
	{
		int	*rgb = VK_LightColors (worldmodel, &colorinfo);

		if (colorinfo.entities != n)
			colorinfo.colored = 0;	/* not the same entities: white (never with COM_Parse's lump) */
		for (i = 0; i < num_maplights && colorinfo.colored; i++)
		{
			int	*c;

			if (maplight_colors[i].entity < 0)
				continue;	/* the map file's addlight: white unless it has a color (ApplyColors) */
			c = &rgb[maplight_colors[i].entity * 3];
			/* 0 0 0: a light the tool has no level for (it reads the
			 * last light key, utils/light the first), none on the maps */
			if (!c[0] && !c[1] && !c[2])
			{
				c[0] = 255;
				c[1] = 225;
				c[2] = 200;
			}
			for (k = 0; k < 3; k++)
				maplight_colors[i].jsh[k] = q_max (c[k], 0);	/* a non-light's _color may be negative */
			if (c[0] == 255 && c[1] == 128 && c[2] == 64)
				stats.orange++;
			else if (c[0] == 255 && c[1] == 225 && c[2] == 200)
				stats.warm++;
			else
				stats.textured++;
		}
		free (rgb);
	}
	ApplyColors ();
}

void VK_ClearMapLights (void)
{
	num_maplights = num_at_points = num_dropped_points = 0;
	memset (at_hash, 0, sizeof(at_hash));
	memset (&stats, 0, sizeof(stats));
	memset (&colorinfo, 0, sizeof(colorinfo));
}

/* the lights for the light lists (r_maplights 0: none) and the intensity
 * (pi x radiance) of a white one */
const vk_maplight_t *VK_MapLights (int *count)
{
	*count = r_maplights.integer ? num_maplights : 0;
	return maplights;
}

/* the intensity (pi x radiance) of a white light of a utils/light level
 * (see the top), also dynamic lights' (vk_light.c) */
float VK_LightLevelIntensity (float level)
{
	float	x = level / (float)DEFAULT_LIGHT_LEVEL;

	return q_max (r_maplight_scale.value, 0.0f) * x * x * x;
}

/* a map light's: its level's, times its map file scale and the map's
 * r_map_light_scale (4.7) */
float VK_MapLightIntensity (const vk_maplight_t *l)
{
	return VK_LightLevelIntensity ((float)l->level) * l->scale * VK_MapLightScale ();
}

void VK_CountMapLightModels (int n)
{
	stats.on_models = n;
}

/* vk_lights's lines about them */
void VK_PrintMapLights (void)
{
	Con_Printf ("map lights: %d (of %d light entities, %d other classnames; %d added by the map file)%s, r_maplight_scale %g; dropped %d inside solid, %d unlit, %d over %d\n",
		    num_maplights, stats.entities, stats.others, stats.edit_added, r_maplights.integer ? "" : ", off (r_maplights 0)",
		    r_maplight_scale.value, stats.in_solid, stats.unlit, stats.over, MAX_LIGHT_POLYS);
	Con_Printf ("  %d spotlights (%d targets unmatched), %d with a style (4.2), %d with _color; %d models in the light group last frame (at a light's origin, owning a dynamic light)\n",
		    stats.spots, stats.unmatched, stats.styled, stats.colored, stats.on_models);
	Con_Printf ("  %d plain lights over lava (%d with a style): %s\n", stats.over_lava, stats.over_lava_styled,
		    VK_LavaLightsOn () ? "left out, the lava lights" : "lit (no lava lights)");
	VK_PrintMapLightEdits ();
	if (!r_maplight_colors.integer)
		Con_Printf ("  colors: white (r_maplight_colors 0)\n");
	else if (stats.colored)
		Con_Printf ("  colors: the map's _color, the other lights white\n");
	else if (!colorinfo.colored)
		Con_Printf ("  colors: white, jsh2color colors none (%s list; it writes no .lit)\n", colorinfo.list ? colorinfo.list : "no");
	else
		Con_Printf ("  colors: jsh2color's (%s list, %.0f ms): %d torch orange, %d from textures, %d 255 225 200\n",
			    colorinfo.list, colorinfo.seconds * 1000.0, stats.orange, stats.textured, stats.warm);
}

/* vk_lights colors: each light's jsh2color color (0-275, as the tool
 * computes it) and its linear color now */
void VK_PrintMapLightColors (void)
{
	int	i;

	for (i = 0; i < num_maplights; i++)
	{
		const int	*c = maplight_colors[i].jsh;
		const float	*o = maplights[i].origin, *l = maplights[i].color, *f = maplight_colors[i].lump_origin;

		/* the map file's light lines name the entity's origin: a moved light's too */
		Con_Printf ("%s %.0f %.0f %.0f %d %d %d -> %.3f %.3f %.3f%s\n", maplight_colors[i].classname, o[0], o[1], o[2],
			    c[0], c[1], c[2], l[0], l[1], l[2],
			    VectorCompare (o, f) ? "" : va(" (moved from %.0f %.0f %.0f)", f[0], f[1], f[2]));
	}
	VK_PrintMapLights ();
}

/* vk_mapfile's and vk_lights's line about the map file's light lines */
void VK_PrintMapLightEdits (void)
{
	Con_Printf ("  map file (4.7): %d lights changed (%d taken out, %d moved), %d added, %d moved or added inside solid dropped, %d past %d; r_map_light_scale %g\n",
		    stats.edited, stats.edit_off, stats.edit_moved, stats.edit_added, stats.edit_in_solid, stats.edit_over,
		    MAX_LIGHT_POLYS, VK_MapLightScale ());
}

static void MapLightsChanged (cvar_t *var)
{
	(void)var;
	VK_RebuildLights ();
}

static void MapLightColorsChanged (cvar_t *var)
{
	(void)var;
	ApplyColors ();
	VK_RebuildLights ();
}

void VK_InitMapLights (void)
{
	Cvar_RegisterVariable (&r_maplights);
	Cvar_RegisterVariable (&r_maplight_scale);
	Cvar_RegisterVariable (&r_maplight_colors);
	Cvar_SetCallback (&r_maplights, MapLightsChanged);
	Cvar_SetCallback (&r_maplight_scale, MapLightsChanged);
	Cvar_SetCallback (&r_maplight_colors, MapLightColorsChanged);
}
