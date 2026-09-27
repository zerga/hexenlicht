/* vk_lightcolor.c -- the map lights' colors: utils/jsh2color's
 *
 * Hammer of Thyrion's colored light is its .lit files (gl_coloredlight 1;
 * its published set, hexen2-litfiles-20140628, has the 42 maps of the
 * original game), which utils/jsh2color (jsh2colour 1.2.6) bakes. This is
 * the tool's step before the bake that gives each light its color
 * (tyrlite.c LightWorld, ltface.c TestLightFace, CalcFaceVectors,
 * CalcFaceExtents and CalcPoints, trace.c TestLine, entities.c
 * LoadEntities, jscolor.c), with its own entity keys (the last of each
 * counts, a level is atof's integer part, "wait" the attenuation, "delay"
 * the formula) and in double, as the tool computes:
 *  - light_torch*, light_flame* and light_gem (any case): orange 255 128 64;
 *  - "light" and light_fluor*: the sum of the colors of the faces they
 *    reach (a face counts once when one of its sample points is in sight
 *    and gets at least a third of the level: within 2/3 of the range; sky
 *    faces never; a texture not in the list is 1 1 1), in integers scaled
 *    to a largest channel of 275 unless it is 255; 255 225 200 when grey;
 *  - every other entity with a level: 255 225 200 (or its _color);
 *  - a map where none ends up colored got no .lit (white in HoT).
 * The texture list is the one the tool's batch files (data_win/colour*.bat)
 * run the map with (-extra -nodefault -external: demo and village, castle
 * with rider1a, egypt with rider2c, meso, romeric), the built-in list for
 * the others (the deathmatch maps, as colourDm.bat; the mission pack and
 * other maps).
 * Faster than the tool (up to 17 s a map, one thread), with the same colors:
 * a sample point that gets too little light isn't traced, faces are culled
 * by their plane and their sample points' bounds, the points are computed
 * only for faces a light can reach, and colored faces come first (a light
 * that reaches none is 255 225 200 whatever else it reaches). The tool
 * adds to the sums from several threads without a lock, so its runs lose
 * some additions (HoT's set too): this is its single-threaded result.
 *
 * Copyright (C) 1996-1997  Id Software, Inc.
 * Copyright (C) 2002  Juraj Styk <jurajstyk@host.sk> (JsH2Colour, from
 *		       MHColour v0.5 and Tyrlite v0.8)
 * Copyright (C) 2005-2012  O.Sezer <sezero@users.sourceforge.net>
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

#define DEFAULT_LIGHT_LEVEL	300		/* the tool's DEFAULTLIGHTLEVEL */
#define TRACE_EPSILON		0.1		/* its ON_EPSILON */
#define MAX_SAMPLES		(64 * 64 * 4)	/* its SINGLEMAP: more points on a face stopped the tool */
#define TRACE_STACK		64		/* its TestLine's stack */
#define SCALEDIST		1.0f		/* its -dist */

typedef double dvec3_t[3];

typedef struct
{
	const char	*name;
	int		len;		/* the characters compared; 0: the whole name */
	int		rgb[3];
} texcolor_t;

/* jscolor.c's FindTexlightColor, in its order */
static const texcolor_t list_builtin[] = {
	{"*lava000", 8, {255, 100, 10}},
	{"*lava001", 8, {255, 10, 10}},
	{"*lava", 5, {255, 10, 10}},
	{"*lowlight", 9, {128, 128, 196}},
	{"*skulls", 7, {255, 10, 10}},
	{"*skullwarp", 10, {255, 64, 64}},
	{"*rtex078", 9, {10, 64, 128}},
	{"*rtex346", 9, {255, 255, 128}},
	{"*rtex385", 9, {196, 64, 64}},
	{"*rtex396", 9, {128, 128, 196}},
	{"+0air", 0, {196, 196, 255}},
	{"+0fire", 0, {255, 196, 128}},
	{"+0pyr", 0, {10, 64, 10}},
	{"+0rune1", 0, {255, 10, 10}},
	{"+0rune2", 0, {255, 255, 128}},
	{"+0steam", 0, {255, 255, 255}},
	{"+0sun1", 0, {255, 64, 10}},
	{"+0tria", 0, {128, 255, 128}},
	{"+0tri", 0, {255, 10, 10}},
	{"+0wat", 0, {40, 40, 196}},
	{"+apen", 0, {255, 128, 10}},
	{"+rtex123", 0, {255, 10, 10}},
	{"celtbrown", 0, {250, 150, 100}},
	{"marbleseam", 0, {200, 100, 60}},
	{"marble", 0, {200, 100, 60}},
	{"mtex402", 0, {10, 128, 10}},
	{"mtex436", 0, {220, 120, 60}},
	{"mtex460", 0, {250, 70, 20}},
	{"mtex462", 0, {250, 70, 20}},
	{"mtex463", 0, {250, 70, 20}},
	{"mtex464", 0, {250, 70, 20}},
	{"mtex465", 0, {250, 70, 20}},
	{"mtex482", 0, {255, 10, 10}},
	{"mtex488", 0, {220, 120, 60}},
	{"mtex489", 0, {220, 160, 80}},
	{"rtex010", 0, {120, 200, 220}},
	{"rtex028", 0, {64, 64, 80}},
	{"rtex044", 0, {200, 120, 60}},
	{"rtex045", 0, {200, 120, 60}},
	{"rtex070", 0, {200, 120, 60}},
	{"rtex074", 0, {150, 200, 150}},
	{"rtex088", 0, {150, 90, 16}},
	{"rtex097", 0, {255, 196, 196}},
	{"rtex099", 0, {64, 64, 128}},
	{"rtex122", 0, {255, 64, 10}},
	{"rtex123", 0, {255, 64, 10}},
	{"rtex124", 0, {255, 64, 64}},
	{"rtex125", 0, {200, 64, 64}},
	{"rtex126", 0, {200, 64, 64}},
	{"rtex128", 0, {64, 196, 64}},
	{"rtex129", 0, {64, 64, 196}},
	{"rtex130", 0, {64, 64, 196}},
	{"rtex131", 0, {90, 90, 196}},
	{"rtex160", 0, {10, 128, 128}},
	{"rtex165", 0, {200, 100, 60}},
	{"rtex251", 0, {10, 100, 200}},
	{"rtex295", 0, {10, 128, 10}},
	{"rtex301", 0, {200, 120, 60}},
	{"rtex321", 0, {250, 220, 10}},
	{"rtex322", 0, {250, 220, 10}},
	{"rtex332", 0, {250, 220, 10}},
	{"rtex333", 0, {10, 250, 10}},
	{"rtex334", 0, {10, 250, 10}},
	{"rtex345", 0, {220, 200, 10}},
	{"rtex349", 0, {128, 128, 196}},
	{"rtex350", 0, {128, 128, 196}},
	{"rtex353", 0, {255, 64, 64}},
	{"rtex356", 0, {250, 220, 10}},
	{"rtex359", 0, {250, 220, 10}},
	{"rtex362", 0, {10, 100, 200}},
	{"rtex363", 0, {64, 64, 128}},
	{"rtex381", 0, {220, 200, 10}},
	{"rtex385", 0, {64, 64, 80}},
	{"rtex386", 0, {255, 220, 10}},
	{"rtex388", 0, {250, 220, 10}},
	{"rtex451", 0, {255, 128, 128}},
	{"rtex452", 0, {255, 128, 128}},
	{"rtex454", 0, {64, 128, 64}},
};

/* data_win/<list>.def, matched as -external matches: a prefix, the first
 * one counts (so egypt's second rtex125 never does; meso's "mtex4648" is
 * the file's) */
static const texcolor_t list_hexen2[] = {
	{"*lava", 5, {255, 10, 10}},
	{"*slime", 6, {10, 255, 10}},
	{"+0rune1", 7, {255, 10, 10}},
	{"+0rune2", 7, {255, 255, 128}},
	{"+rtex123", 8, {255, 10, 10}},
	{"*rtex346", 8, {255, 255, 128}},
	{"*rtex078", 8, {10, 64, 128}},
	{"rtex160", 7, {10, 128, 128}},
	{"rtex099", 7, {64, 64, 128}},
	{"rtex088", 7, {150, 90, 16}},
	{"rtex353", 7, {255, 64, 64}},
	{"rtex126", 7, {255, 64, 64}},
	{"rtex097", 7, {255, 196, 196}},
	{"+0pyr", 5, {10, 64, 10}},
};
static const texcolor_t list_hexen2castle[] = {
	{"*lava000", 8, {255, 100, 10}},
	{"*lowlight", 9, {128, 128, 196}},
	{"rtex125", 7, {200, 64, 64}},
	{"rtex126", 7, {200, 64, 64}},
	{"rtex130", 7, {64, 64, 200}},
	{"marbleseam", 10, {200, 100, 60}},
	{"marble", 6, {200, 100, 60}},
	{"rtex165", 7, {200, 100, 60}},
	{"celtbrown", 9, {250, 150, 100}},
	{"rtex251", 7, {10, 100, 200}},
	{"*skulls", 7, {255, 64, 64}},
	{"rtex074", 7, {150, 200, 150}},
	{"rtex362", 7, {10, 100, 200}},
	{"rtex010", 7, {150, 200, 220}},
	{"+0rune1", 7, {255, 10, 10}},
	{"+0rune2", 7, {255, 10, 10}},
	{"rtex386", 7, {255, 220, 10}},
};
static const texcolor_t list_hexen2egypt[] = {
	{"*lava", 5, {255, 10, 10}},
	{"+apen", 5, {255, 128, 10}},
	{"rtex125", 7, {128, 64, 64}},
	{"*skullwarp", 10, {255, 64, 64}},
	{"*rtex385", 8, {196, 64, 64}},
	{"*rtex396", 8, {128, 128, 196}},
	{"rtex349", 7, {128, 128, 196}},
	{"rtex350", 7, {128, 128, 196}},
	{"rtex128", 7, {64, 196, 64}},
	{"rtex129", 7, {64, 64, 196}},
	{"rtex130", 7, {64, 64, 196}},
	{"rtex131", 7, {90, 90, 196}},
	{"rtex123", 7, {128, 10, 10}},
	{"rtex124", 7, {200, 10, 10}},
	{"rtex125", 7, {150, 30, 30}},
	{"rtex363", 7, {64, 64, 128}},
	{"rtex028", 7, {64, 64, 80}},
	{"rtex385", 7, {64, 64, 80}},
	{"rtex322", 7, {250, 220, 10}},
	{"rtex356", 7, {250, 220, 10}},
	{"rtex321", 7, {250, 220, 10}},
	{"rtex332", 7, {250, 220, 10}},
	{"rtex388", 7, {250, 220, 10}},
	{"rtex359", 7, {250, 220, 10}},
	{"rtex381", 7, {220, 200, 10}},
	{"rtex345", 7, {220, 200, 10}},
};
static const texcolor_t list_hexen2meso[] = {
	{"*lava000", 8, {255, 128, 10}},
	{"*lava001", 8, {255, 10, 10}},
	{"rtex122", 7, {255, 64, 10}},
	{"rtex123", 7, {255, 64, 10}},
	{"rtex124", 7, {255, 64, 10}},
	{"rtex125", 7, {255, 10, 10}},
	{"rtex126", 7, {128, 10, 10}},
	{"mtex482", 7, {255, 10, 10}},
	{"rtex333", 7, {10, 255, 10}},
	{"rtex334", 7, {10, 255, 10}},
	{"rtex097", 7, {10, 255, 10}},
	{"rtex295", 7, {10, 128, 10}},
	{"mtex402", 7, {10, 128, 10}},
	{"mtex489", 7, {220, 160, 80}},
	{"mtex465", 7, {250, 70, 20}},
	{"mtex463", 7, {250, 70, 20}},
	{"mtex460", 7, {250, 70, 20}},
	{"mtex462", 7, {250, 70, 20}},
	{"mtex4648", 8, {250, 70, 20}},
	{"rtex044", 7, {200, 120, 60}},
	{"rtex045", 7, {200, 120, 60}},
	{"rtex070", 7, {200, 120, 60}},
	{"rtex301", 7, {200, 120, 60}},
	{"mtex436", 7, {220, 120, 60}},
	{"mtex488", 7, {220, 120, 60}},
	{"+0sun1", 6, {255, 64, 10}},
	{"rtex130", 7, {10, 10, 128}},
	{"*skulls", 7, {255, 10, 10}},
};
static const texcolor_t list_hexen2romeric[] = {
	{"*lava000", 8, {255, 40, 40}},
	{"*skulls", 7, {255, 40, 40}},
	{"*slime", 6, {10, 255, 10}},
	{"*lowlight", 9, {128, 128, 196}},
	{"+0tria", 6, {128, 255, 128}},
	{"+0tri", 5, {255, 10, 10}},
	{"rtex124", 7, {255, 64, 64}},
	{"rtex125", 7, {255, 64, 64}},
	{"rtex126", 7, {255, 64, 64}},
	{"rtex454", 7, {64, 128, 64}},
	{"rtex451", 7, {255, 128, 128}},
	{"rtex452", 7, {255, 128, 128}},
	{"+0fire", 6, {255, 196, 128}},
	{"+0wat", 5, {40, 40, 196}},
	{"+0steam", 7, {255, 255, 255}},
	{"+0air", 5, {196, 196, 255}},
};

typedef struct
{
	const char		*name;
	const texcolor_t	*entries;
	int			count;
} texlist_t;

static const texlist_t texlists[] = {
	{"built-in", list_builtin, Q_COUNTOF(list_builtin)},
	{"hexen2.def", list_hexen2, Q_COUNTOF(list_hexen2)},
	{"hexen2castle.def", list_hexen2castle, Q_COUNTOF(list_hexen2castle)},
	{"hexen2egypt.def", list_hexen2egypt, Q_COUNTOF(list_hexen2egypt)},
	{"hexen2meso.def", list_hexen2meso, Q_COUNTOF(list_hexen2meso)},
	{"hexen2romeric.def", list_hexen2romeric, Q_COUNTOF(list_hexen2romeric)},
};

/* the batch files' maps and lists (texlists' indexes) */
static const struct
{
	const char	*map;
	int		list;
} batch_maps[] = {
	{"demo1", 1}, {"demo2", 1}, {"demo3", 1},
	{"village1", 1}, {"village2", 1}, {"village3", 1}, {"village4", 1}, {"village5", 1},
	{"castle4", 2}, {"castle5", 2}, {"cath", 2}, {"eidolon", 2}, {"tower", 2}, {"rider1a", 2},
	{"egypt1", 3}, {"egypt2", 3}, {"egypt3", 3}, {"egypt4", 3}, {"egypt5", 3}, {"egypt6", 3},
	{"egypt7", 3}, {"rider2c", 3},
	{"meso1", 4}, {"meso2", 4}, {"meso3", 4}, {"meso4", 4}, {"meso5", 4}, {"meso6", 4},
	{"meso8", 4}, {"meso9", 4},
	{"romeric1", 5}, {"romeric2", 5}, {"romeric3", 5}, {"romeric4", 5}, {"romeric5", 5},
	{"romeric6", 5}, {"romeric7", 5},
};

/* an entity as the tool's LoadEntities reads it */
typedef struct
{
	char		classname[64];
	char		model[16];	/* the tool finds a brush model's entity by it */
	dvec3_t		origin;
	int		level;		/* its light */
	float		atten;		/* "wait" */
	int		formula;	/* "delay": 0 linear, 1 1/x, 2 1/x^2, 3 constant */
	int		rgb[3];		/* its lightcolor */
} jshent_t;

/* a face (the tool's lightinfo_t: CalcFaceVectors, CalcFaceExtents) */
typedef struct
{
	qboolean	skip;		/* sky, or a texture axis along the face */
	qboolean	colored;	/* its texture's color isn't grey */
	int		rgb[3];
	dvec3_t		offset;		/* a rotate_ entity's origin */
	dvec3_t		normal;
	double		dist;
	dvec3_t		texorg;
	dvec3_t		textoworld[2];
	double		exactmins[2], exactmaxs[2];
	int		texmins[2], texsize[2];
	dvec3_t		mins, maxs;	/* its sample points' bounds */
	int		numpts;		/* -1: not computed yet */
	double		*pts;
} face_t;

static const texlist_t	*texlist;
static const mnode_t	*trace_nodes;


static double DDot (const double *a, const double *b)
{
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static void DNormalize (double *v)
{
	double	length = sqrt (DDot (v, v));

	if (length != 0)
	{
		v[0] /= length;
		v[1] /= length;
		v[2] /= length;
	}
}

static void TexColor (const char *name, int *rgb)
{
	int	i;

	for (i = 0; i < texlist->count; i++)
	{
		const texcolor_t	*e = &texlist->entries[i];

		if (e->len ? !strncmp (name, e->name, e->len) : !strcmp (name, e->name))
		{
			rgb[0] = e->rgb[0];
			rgb[1] = e->rgb[1];
			rgb[2] = e->rgb[2];
			return;
		}
	}
	rgb[0] = rgb[1] = rgb[2] = 1;
}

/* trace.c's TestLine through the world's nodes: only solid leafs block;
 * within TRACE_EPSILON of a plane a point takes the other end's side */
static qboolean TestLine (const double *start, const double *stop)
{
	struct
	{
		dvec3_t		back;
		int		side;
		const mnode_t	*node;
	}		stack[TRACE_STACK];
	int		sp = 0, k;
	const mnode_t	*node = trace_nodes;
	dvec3_t		front, back;

	for (k = 0; k < 3; k++)
	{
		front[k] = start[k];
		back[k] = stop[k];
	}
	for (;;)
	{
		const mplane_t	*plane;
		double		f, b;

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
			f = front[plane->type] - (double)plane->dist;
			b = back[plane->type] - (double)plane->dist;
		}
		else
		{
			f = (front[0] * plane->normal[0] + front[1] * plane->normal[1] + front[2] * plane->normal[2]) - plane->dist;
			b = (back[0] * plane->normal[0] + back[1] * plane->normal[1] + back[2] * plane->normal[2]) - plane->dist;
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
			return false;	/* deeper than the tool's stack: never on the maps */
		stack[sp].node = node;
		stack[sp].side = (f < 0.0) ? 1 : 0;
		for (k = 0; k < 3; k++)
			stack[sp].back[k] = back[k];
		f /= (f - b);
		for (k = 0; k < 3; k++)
			back[k] = front[k] + f * (back[k] - front[k]);
		node = node->children[stack[sp].side];
		sp++;
	}
}

/* the tool's LoadEntities: the last of each key counts; lights ("light*"
 * classnames) get the default level, attenuation 1 and formula 0, and
 * torches, flames and the gem orange */
static jshent_t *LoadEntities (const char *data, int *count)
{
	jshent_t	*ents = NULL;
	int		n = 0, size = 0;

	while (data && (data = COM_Parse (data)) != NULL && com_token[0] == '{')
	{
		jshent_t	*e;

		if (n == size)
		{
			size = size ? size * 2 : 1024;
			ents = (jshent_t *) realloc (ents, size * sizeof(*ents));
			if (!ents)
				Sys_Error ("%s: out of memory", __thisfunc__);
		}
		e = &ents[n++];
		memset (e, 0, sizeof(*e));
		while ((data = COM_Parse (data)) != NULL && com_token[0] != '}')
		{
			char	key[64];
			double	v[3];
			int	k;

			q_strlcpy (key, com_token, sizeof(key));
			if ((data = COM_Parse (data)) == NULL)
				break;
			if (!strcmp (key, "classname"))
				q_strlcpy (e->classname, com_token, sizeof(e->classname));
			else if (!strcmp (key, "model"))
				q_strlcpy (e->model, com_token, sizeof(e->model));
			else if (!strcmp (key, "origin"))
			{
				if (sscanf (com_token, "%lf %lf %lf", &v[0], &v[1], &v[2]) == 3)
					for (k = 0; k < 3; k++)
						e->origin[k] = v[k];
			}
			else if (!strncmp (key, "light", 5) || !strncmp (key, "_light", 6))
				e->level = (int)atof (com_token);
			else if (!strcmp (key, "wait"))
				e->atten = (float)atof (com_token);
			else if (!strcmp (key, "delay"))
				e->formula = atoi (com_token);
			else if (!strcmp (key, "_color"))
			{
				if (sscanf (com_token, "%lf %lf %lf", &v[0], &v[1], &v[2]) == 3)
					for (k = 0; k < 3; k++)
						e->rgb[k] = (int)v[k];
			}
		}
		if (!strncmp (e->classname, "light", 5))
		{
			if (!e->level)
				e->level = DEFAULT_LIGHT_LEVEL;
			if (e->atten <= 0.0f)
				e->atten = 1.0f;
			if (e->formula < 0 || e->formula > 3)
				e->formula = 0;
			if (!q_strncasecmp (e->classname, "light_flame", 11) ||
			    !q_strncasecmp (e->classname, "light_torch", 11) ||
			    !q_strcasecmp (e->classname, "light_gem"))
			{
				e->rgb[0] = 255;
				e->rgb[1] = 128;
				e->rgb[2] = 64;
			}
			else
			{
				e->rgb[0] = e->rgb[1] = e->rgb[2] = 0;
			}
		}
	}
	*count = n;
	return ents;
}

/* the tool's FindFaceOffsets: the faces of a rotate_ entity's brush model
 * are at its origin */
static void FaceOffsets (const qmodel_t *m, const jshent_t *ents, int n, face_t *faces)
{
	int	i, j, s, k;

	for (i = 1; i < m->numsubmodels; i++)
	{
		const dmodel_t	*sub = &m->submodels[i];
		char		name[16];

		q_snprintf (name, sizeof(name), "*%d", i);
		for (j = 0; j < n && strcmp (ents[j].model, name); j++)
			;
		if (j == n || strncmp (ents[j].classname, "rotate_", 7))
			continue;
		for (s = sub->firstface; s < sub->firstface + sub->numfaces && s < m->numsurfaces; s++)
			for (k = 0; k < 3; k++)
				faces[s].offset[k] = ents[j].origin[k];
	}
}

/* the tool's TestLightFace up to the points: its plane, CalcFaceVectors and
 * CalcFaceExtents, and the bounds of CalcPoints' points */
static void SetupFace (const qmodel_t *m, const msurface_t *surf, face_t *f)
{
	const mtexinfo_t	*tex = surf->texinfo;
	const float		(*vecs)[4] = tex->vecs;
	char			texname[sizeof(tex->texture->name) + 1];
	dvec3_t			worldtotex[2], texnormal, point;
	float			distscale;	/* float in the tool */
	double			dist, len, mins[2], maxs[2], val, us, ut;
	int			i, j, c, w, h;

	q_strlcpy (texname, tex->texture->name, sizeof(texname));
	f->numpts = -1;
	f->skip = !strncmp (texname, "sky", 3);
	if (f->skip)
		return;
	TexColor (texname, f->rgb);
	f->colored = !(f->rgb[0] == f->rgb[1] && f->rgb[1] == f->rgb[2]);

	for (j = 0; j < 3; j++)
		f->normal[j] = surf->plane->normal[j];
	for (j = 0; j < 3; j++)
		point[j] = f->normal[j] * (double)surf->plane->dist + f->offset[j];
	f->dist = DDot (point, f->normal);
	if (surf->flags & SURF_PLANEBACK)
	{
		for (j = 0; j < 3; j++)
			f->normal[j] = -f->normal[j];
		f->dist = -f->dist;
	}

	/* CalcFaceVectors (its texture normal and distscale are floats, as there) */
	for (i = 0; i < 2; i++)
		for (j = 0; j < 3; j++)
			worldtotex[i][j] = vecs[i][j];
	texnormal[0] = vecs[1][1] * vecs[0][2] - vecs[1][2] * vecs[0][1];
	texnormal[1] = vecs[1][2] * vecs[0][0] - vecs[1][0] * vecs[0][2];
	texnormal[2] = vecs[1][0] * vecs[0][1] - vecs[1][1] * vecs[0][0];
	DNormalize (texnormal);
	distscale = DDot (texnormal, f->normal);
	if (!distscale)
	{
		f->skip = true;	/* the tool stopped: "Texture axis perpendicular to face" */
		return;
	}
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
			f->textoworld[i][j] = (worldtotex[i][j] + -dist * texnormal[j]) * ((1 / len) * (1 / len));
	}
	for (j = 0; j < 3; j++)
		f->texorg[j] = -vecs[0][3] * f->textoworld[0][j] - vecs[1][3] * f->textoworld[1][j];
	dist = (DDot (f->texorg, f->normal) - f->dist - 1) * distscale;
	for (j = 0; j < 3; j++)
		f->texorg[j] = f->texorg[j] + -dist * texnormal[j];

	/* CalcFaceExtents */
	mins[0] = mins[1] = 999999;
	maxs[0] = maxs[1] = -99999;
	for (i = 0; i < surf->numedges; i++)
	{
		int		e = m->surfedges[surf->firstedge + i];
		const float	*v = m->vertexes[(e >= 0) ? m->edges[e].v[0] : m->edges[-e].v[1]].position;

		for (j = 0; j < 2; j++)
		{
			val = ((double)v[0] + f->offset[0]) * (double)vecs[j][0] +
			      ((double)v[1] + f->offset[1]) * (double)vecs[j][1] +
			      ((double)v[2] + f->offset[2]) * (double)vecs[j][2] + (double)vecs[j][3];
			if (val < mins[j])
				mins[j] = val;
			if (val > maxs[j])
				maxs[j] = val;
		}
	}
	for (i = 0; i < 2; i++)
	{
		f->exactmins[i] = mins[i];
		f->exactmaxs[i] = maxs[i];
		mins[i] = floor (mins[i] / 16);
		maxs[i] = ceil (maxs[i] / 16);
		f->texmins[i] = (int)mins[i];
		f->texsize[i] = (int)(maxs[i] - mins[i]);
	}

	/* CalcPoints' grid (-extra) bounds its points: their texture
	 * coordinates only move towards the face's middle, which is inside
	 * it; a point that can't see the middle after 6 tries ends 8 units
	 * from its last spot towards the middle, less than 8 past the grid */
	w = (f->texsize[0] + 1) * 2;
	h = (f->texsize[1] + 1) * 2;
	for (j = 0; j < 3; j++)
	{
		f->mins[j] = 1e30;
		f->maxs[j] = -1e30;
	}
	for (c = 0; c < 4; c++)
	{
		us = (f->texmins[0] - 0.5) * 16 + ((c & 1) ? (w - 1) * 8 : 0);
		ut = (f->texmins[1] - 0.5) * 16 + ((c & 2) ? (h - 1) * 8 : 0);
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
}

/* the tool's CalcPoints (-extra): a point that can't see the face's middle
 * moves towards it, up to 6 times */
static void CalcPoints (face_t *f)
{
	int	w = (f->texsize[0] + 1) * 2, h = (f->texsize[1] + 1) * 2;
	double	mids = (f->exactmaxs[0] + f->exactmins[0]) / 2, midt = (f->exactmaxs[1] + f->exactmins[1]) / 2;
	double	starts = (f->texmins[0] - 0.5) * 16, startt = (f->texmins[1] - 0.5) * 16;
	dvec3_t	facemid, move;
	double	*surf;
	int	s, t, i, j;

	if (w * h > MAX_SAMPLES)
	{
		f->numpts = 0;	/* the tool stopped: "surf out of bounds" */
		return;
	}
	for (j = 0; j < 3; j++)
		facemid[j] = f->texorg[j] + f->textoworld[0][j] * mids + f->textoworld[1][j] * midt;
	f->numpts = w * h;
	f->pts = surf = (double *) malloc (sizeof(dvec3_t) * f->numpts);
	if (!surf)
		Sys_Error ("%s: out of memory", __thisfunc__);
	for (t = 0; t < h; t++)
	{
		for (s = 0; s < w; s++, surf += 3)
		{
			double	us = starts + s * 8, ut = startt + t * 8;

			for (i = 0; i < 6; i++)
			{
				for (j = 0; j < 3; j++)
					surf[j] = f->texorg[j] + f->textoworld[0][j] * us + f->textoworld[1][j] * ut;
				if (TestLine (facemid, surf))
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
					move[j] = facemid[j] - surf[j];
				DNormalize (move);
				for (j = 0; j < 3; j++)
					surf[j] = surf[j] + 8 * move[j];
			}
		}
	}
}

/* the tool's scaledDistance and scaledLight */
static double ScaledDistance (double distance, const jshent_t *l)
{
	if (l->formula)
		return (distance <= 0) ? -0.25 : 0.25;	/* these formulas don't fade to nothing */
	return (double)(SCALEDIST * l->atten) * distance;
}

static double ScaledLight (double distance, const jshent_t *l)
{
	double	tmp = (double)(SCALEDIST * l->atten) * distance;

	switch (l->formula)
	{
	case 3:
		return l->level;
	case 1:
		return l->level / (tmp / 128);
	case 2:
		return l->level / ((tmp * tmp) / 16384);
	default:
		if (l->level > 0)
			return (l->level - tmp > 0) ? l->level - tmp : 0;
		return (l->level + tmp < 0) ? l->level + tmp : 0;
	}
}

/* the tool's TestSingleLightFace: does the light reach the face (its color
 * added once)? */
static qboolean TestLightFace (jshent_t *l, face_t *f)
{
	double	dist = ScaledDistance (DDot (l->origin, f->normal) - f->dist, l);
	int	c, j;

	if (dist <= 0 || dist > abs (l->level))
		return false;
	if (l->formula == 0 && l->level / 3 > 0)
	{
		/* a point farther than (level - level/3) / atten gets less than
		 * a third: none of the face's may be closer (levels 1 and 2 have
		 * a third of 0, which every point in sight reaches) */
		double	reach = (double)(l->level - l->level / 3) / (double)(SCALEDIST * l->atten) + 1;
		double	d2 = 0;

		for (j = 0; j < 3; j++)
		{
			double	e = (l->origin[j] < f->mins[j]) ? f->mins[j] - l->origin[j] :
				    (l->origin[j] > f->maxs[j]) ? l->origin[j] - f->maxs[j] : 0;

			d2 += e * e;
		}
		if (d2 > reach * reach)
			return false;
	}
	if (f->numpts < 0)
		CalcPoints (f);
	for (c = 0; c < f->numpts; c++)
	{
		const double	*p = f->pts + c * 3;
		double		t = 0;

		for (j = 0; j < 3; j++)
			t += (p[j] - l->origin[j]) * (p[j] - l->origin[j]);
		if (t <= 0)
			t = 1;
		if (ScaledLight (sqrt (t), l) < (l->level / 3))
			continue;
		if (!TestLine (l->origin, p))
			continue;
		for (j = 0; j < 3; j++)
			l->rgb[j] += f->rgb[j];
		return true;
	}
	return false;
}

/* the colors (0-275, the tool's integers; 0 0 0 without a level) of every
 * entity in the lump, in its order, in a new array the caller frees */
int *VK_LightColors (qmodel_t *worldmodel, vk_lightcolors_t *info)
{
	jshent_t	*ents;
	face_t		*faces;
	int		*out;
	char		mapname[MAX_QPATH];
	int		n, i, s, j;
	double		start = Sys_DoubleTime ();

	memset (info, 0, sizeof(*info));
	COM_FileBase (worldmodel->name, mapname, sizeof(mapname));
	texlist = &texlists[0];
	for (i = 0; i < (int)Q_COUNTOF(batch_maps); i++)
		if (!q_strcasecmp (mapname, batch_maps[i].map))
			texlist = &texlists[batch_maps[i].list];
	info->list = texlist->name;
	trace_nodes = worldmodel->nodes;

	ents = LoadEntities (worldmodel->entities, &n);
	faces = (face_t *) calloc (q_max (worldmodel->numsurfaces, 1), sizeof(*faces));
	if (!faces)
		Sys_Error ("%s: out of memory", __thisfunc__);
	FaceOffsets (worldmodel, ents, n, faces);
	for (s = 0; s < worldmodel->numsurfaces; s++)
		SetupFace (worldmodel, &worldmodel->surfaces[s], &faces[s]);

	/* LightWorld: the plain lights' sums (TestLightFace's lights) */
	for (i = 0; i < n; i++)
	{
		jshent_t	*l = &ents[i];
		qboolean	hit = false;

		if (strcmp (l->classname, "light") && strncmp (l->classname, "light_fluor", 11))
			continue;
		l->rgb[0] = l->rgb[1] = l->rgb[2] = 0;
		for (s = 0; s < worldmodel->numsurfaces; s++)
			if (!faces[s].skip && faces[s].colored)
				hit |= TestLightFace (l, &faces[s]);
		if (!hit)
			continue;	/* grey whatever else it reaches: 255 225 200 below */
		for (s = 0; s < worldmodel->numsurfaces; s++)
			if (!faces[s].skip && !faces[s].colored)
				TestLightFace (l, &faces[s]);
	}

	/* LightWorld's scale: grey is 255 225 200, else the largest channel 275 unless it is 255 */
	out = (int *) calloc (q_max (n, 1) * 3, sizeof(int));
	if (!out)
		Sys_Error ("%s: out of memory", __thisfunc__);
	for (i = 0; i < n; i++)
	{
		int	*rgb = ents[i].rgb, colormax = 0;

		if (!ents[i].level)
			continue;
		if (rgb[0] == rgb[1] && rgb[1] == rgb[2])
		{
			rgb[0] = 255;
			rgb[1] = 225;
			rgb[2] = 200;
		}
		else
		{
			info->colored++;
			for (j = 0; j < 3; j++)
				colormax = q_max (colormax, rgb[j]);
			if (colormax != 255 && colormax != 0)
				for (j = 0; j < 3; j++)
					rgb[j] = (275 * rgb[j]) / colormax;
		}
		for (j = 0; j < 3; j++)
			out[i * 3 + j] = rgb[j];
	}

	for (s = 0; s < worldmodel->numsurfaces; s++)
		free (faces[s].pts);
	free (faces);
	free (ents);
	info->entities = n;
	info->seconds = Sys_DoubleTime () - start;
	return out;
}
