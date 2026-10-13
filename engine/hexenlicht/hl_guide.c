/* hl_guide.c -- the guide's steps: recorded by playing, checked against the game
 *
 * Story 10.1 (E10, a spike). A hub's guide is maps\<hub>.hlguide: plain
 * text, one line each, "//" comments:
 *	hub <name> <map> <map> ...		the hub's maps, in the order reached
 *	step "<text>" <map> at x y z		the next step: our own text, where
 *	  done <condition>			what completes it (one)
 *	  route <map> x y z [w|j] x y z ...	the way there from the step before
 * A route line is one stretch of the way in one map (split at level changes
 * and teleports), its points about ROUTE_SPACING apart, thinned within
 * ROUTE_THIN; "w" after a point: under water, "j": in the air. The
 * conditions, read only from the game's state (no progress of our own):
 *	item <puzzle_id>	the player holds that puzzle piece (puzzle_inv1-8)
 *	flag <1-8>		that crosslevel bit is set (serverflags 256 << n-1)
 *	gone <key>		the entity is removed, or has no model any more
 *	moved <key> from x y z p y r	the entity is away from that origin and
 *				those angles (a door or wall that opened)
 *	enter <map>		the player has reached that map
 *	field <key> <field> <op> <value>	a float field of the entity (a field
 *				the save leaves out is 0); op: == != < <= > >=
 *	none			nothing the game keeps (never done)
 * An entity's key is its brush model ("*14": one per map, kept in saves) or,
 * for a point entity, "classname@x,y,z", its origin when recorded, matched
 * within POINT_MATCH units (edict numbers differ between skills, classes and
 * fresh loads). The entity conditions are about the step's map: the live
 * edicts when it is the server's map, else its saved state, <map>.gip in the
 * game's user folder (written when the player leaves a level, copied with
 * every save, SaveGamestate/ED_Write), read here; a map not reached yet has
 * none (open). Entities a save leaves out (FL_ARCHIVE_OVERRIDE: the
 * crosslevel triggers and targets, respawned with the map) are left out of
 * the live state too, so both read the same. Progress is the order: the next step is the one after the
 * last one done (a piece used at its lock is no longer held).
 *
 * Recording (a developer's tool; client 1 of a local game):
 *  - guide_record start <hub> | continue <hub> | save | resync | stop:
 *    start one (continue: from the hub's file), write the file into the
 *    game's user folder (maps\<hub>.hlguide, through .tmp), take the state
 *    now as the next mark's start (after a load into another map: a load in
 *    the same map does it by itself), stop; without arguments its state.
 *    HL_GuideFrame (host.c, after the server frame) samples the player's
 *    origin every frame while recording;
 *  - guide_mark "<text>": a step. What changed since the last mark (items
 *    gained, crosslevel bits set, entities gone or moved in the maps of the
 *    snapshot, maps entered) becomes numbered candidates, the most specific
 *    first; the step takes the first, its place is that condition's entity
 *    (else the player), and the route since the last mark. A map's first
 *    snapshot is taken ENTRY_DELAY seconds after the player enters it (the
 *    items have dropped to the floor; crosslevel targets fire after 3);
 *  - guide_cond <n>: the last step takes candidate n; guide_undo drops the
 *    last step (right after its mark also putting back its route and the
 *    state before it, so the next mark sees the same changes).
 *  - guide_check [hub]: each step done or open, and which is next: the
 *    recording's steps while recording, else the hub's file (the user
 *    folder's first, then the shipped <exe folder>\guide\, from the
 *    repository's data/hexenlicht/guide), found by the current map without
 *    a hub given.
 * Nothing prints from the frame hook (CLAUDE.md's pitfall); the commands
 * report.
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

#define GUIDE_EXT		".hlguide"
#define MAX_GUIDE_MAPS		32
#define MAX_CANDIDATES		256
#define MAX_PRINTED_CANDIDATES	32
#define MAX_PENDING_POINTS	65536		/* the route since the last mark */
#define NUM_PUZZLE		8
#define MAX_GUIDE_KEY		64
#define ROUTE_SPACING		32.0f		/* units between route points */
#define ROUTE_JUMP		160.0f		/* more in one frame: a teleport */
#define ROUTE_THIN		16.0f		/* thinning tolerance */
#define POINT_MATCH		16.0f		/* a point entity's key */
#define MOVE_EPSILON		1.0f		/* units or degrees */
#define ENTRY_DELAY		1.0		/* seconds of server time */

typedef struct
{
	vec3_t	pos;
	char	flag;		/* 0, 'w' under water, 'j' in the air */
} gpoint_t;

typedef struct
{
	char		map[MAX_QPATH];
	int		num, max;
	gpoint_t	*points;
} groute_t;

typedef enum
{
	COND_NONE, COND_ITEM, COND_FLAG, COND_GONE, COND_MOVED, COND_ENTER, COND_FIELD
} condkind_t;

static const char *cond_names[] = { "none", "item", "flag", "gone", "moved", "enter", "field" };

typedef struct
{
	condkind_t	kind;
	char		arg[MAX_GUIDE_KEY];		/* item id, entity key, map */
	int		flag;			/* 1-8 */
	vec3_t		from_origin, from_angles;	/* moved */
	char		field[32], op[4];
	float		value;
	/* a candidate's place */
	char		map[MAX_QPATH];
	vec3_t		place;
	int		rank, order;	/* order: as found */
} gcond_t;

typedef struct
{
	char		text[256];
	char		map[MAX_QPATH];
	vec3_t		at;
	gcond_t		done;
	int		num_routes;
	groute_t	*routes;
} gstep_t;

typedef struct
{
	char		hub[MAX_QPATH];
	char		path[MAX_OSPATH];	/* where it was read from */
	int		num_maps;
	char		maps[MAX_GUIDE_MAPS][MAX_QPATH];
	int		num_steps, max_steps;
	gstep_t		*steps;
	int		issues;			/* lines skipped when read */
} gguide_t;

/* an entity of a map's state, live or from its .gip */
typedef struct
{
	char		key[MAX_GUIDE_KEY];
	char		classname[48];
	char		puzzle_id[32];
	qboolean	brush, has_model, moving;
	qboolean	actor;		/* a monster, or a point entity that moves itself (a sheep) */
	qboolean	lock;		/* has puzzle_piece_1: a lock */
	vec3_t		origin, angles, center;
	const char	*block;		/* the .gip's text after its "{"; NULL: live */
	int		edict;		/* live: its number */
} gent_t;

typedef struct
{
	char		map[MAX_QPATH];
	qboolean	live;
	int		num, max;
	gent_t		*ents;
	char		*text;		/* the .gip's text */
} gstate_t;

static struct
{
	qboolean	active;
	gguide_t	guide;
	char		map[MAX_QPATH];		/* of the last sample */
	double		last_time;
	qboolean	entry_pending;
	double		entry_time;
	/* the route since the last mark */
	int		num_segs, max_segs;
	groute_t	*segs;
	int		pending_points, dropped;
	vec3_t		last;			/* the last sample */
	qboolean	have_last;
	/* the state at the last mark */
	char		inv[NUM_PUZZLE][32];
	int		serverflags;
	int		num_snaps;
	gstate_t	snaps[MAX_GUIDE_MAPS];
	int		num_entered;
	char		entered[MAX_GUIDE_MAPS][MAX_QPATH];	/* since the last mark */
	/* the last step's candidates */
	int		num_cands, cands_dropped;
	gcond_t		cands[MAX_CANDIDATES];
	/* the state before the last mark, for guide_undo */
	qboolean	undo_ready;
	char		undo_inv[NUM_PUZZLE][32];
	int		undo_serverflags;
	int		undo_num_snaps;
	gstate_t	undo_snaps[MAX_GUIDE_MAPS];
	int		undo_num_entered;
	char		undo_entered[MAX_GUIDE_MAPS][MAX_QPATH];
	int		unsaved;
} rec;

/* ==========================================================================
 * Routes and guides
 * ========================================================================== */

static void AddPoint (groute_t *r, const vec3_t pos, char flag)
{
	if (r->num == r->max)
	{
		r->max = r->max ? r->max * 2 : 64;
		r->points = (gpoint_t *) realloc (r->points, r->max * sizeof(gpoint_t));
		if (!r->points)
			Sys_Error ("%s: out of memory", __thisfunc__);
	}
	VectorCopy (pos, r->points[r->num].pos);
	r->points[r->num].flag = flag;
	r->num++;
}

static void FreeRoutes (groute_t *routes, int num)
{
	int	i;

	for (i = 0; i < num; i++)
		free (routes[i].points);
	free (routes);
}

static void FreeGuide (gguide_t *g)
{
	int	i;

	for (i = 0; i < g->num_steps; i++)
		FreeRoutes (g->steps[i].routes, g->steps[i].num_routes);
	free (g->steps);
	memset (g, 0, sizeof(*g));
}

static gstep_t *AddStep (gguide_t *g)
{
	gstep_t	*s;

	if (g->num_steps == g->max_steps)
	{
		g->max_steps = g->max_steps ? g->max_steps * 2 : 32;
		g->steps = (gstep_t *) realloc (g->steps, g->max_steps * sizeof(gstep_t));
		if (!g->steps)
			Sys_Error ("%s: out of memory", __thisfunc__);
	}
	s = &g->steps[g->num_steps++];
	memset (s, 0, sizeof(*s));
	return s;
}

static groute_t *AddRoute (groute_t **routes, int *num, int *max, const char *map)
{
	groute_t	*r;

	if (*num == *max)
	{
		*max = *max ? *max * 2 : 8;
		*routes = (groute_t *) realloc (*routes, *max * sizeof(groute_t));
		if (!*routes)
			Sys_Error ("%s: out of memory", __thisfunc__);
	}
	r = &(*routes)[(*num)++];
	memset (r, 0, sizeof(*r));
	q_strlcpy (r->map, map, sizeof(r->map));
	return r;
}

static void AddHubMap (gguide_t *g, const char *map)
{
	int	i;

	for (i = 0; i < g->num_maps; i++)
		if (!q_strcasecmp (g->maps[i], map))
			return;
	if (g->num_maps < MAX_GUIDE_MAPS)
		q_strlcpy (g->maps[g->num_maps++], map, sizeof(g->maps[0]));
}

/* distance of p from the segment a-b */
static float SegmentDistance (const vec3_t p, const vec3_t a, const vec3_t b)
{
	vec3_t	ab, ap, c;
	float	len2, t;

	VectorSubtract (b, a, ab);
	VectorSubtract (p, a, ap);
	len2 = DotProduct (ab, ab);
	t = (len2 > 0) ? DotProduct (ap, ab) / len2 : 0;
	t = q_max (0.0f, q_min (1.0f, t));
	VectorMA (a, t, ab, c);
	VectorSubtract (p, c, c);
	return VectorLength (c);
}

/* Douglas-Peucker on points[first..last], marking the kept ones */
static void Thin (const gpoint_t *points, int first, int last, qboolean *keep)
{
	int	i, worst;
	float	d, dmax;

	while (last - first > 1)
	{
		dmax = 0;
		worst = -1;
		for (i = first + 1; i < last; i++)
		{
			d = SegmentDistance (points[i].pos, points[first].pos, points[last].pos);
			if (d > dmax)
			{
				dmax = d;
				worst = i;
			}
		}
		if (dmax <= ROUTE_THIN)
			return;
		keep[worst] = true;
		/* the smaller side recursed, the larger looped: depth log n */
		if (worst - first < last - worst)
		{
			Thin (points, first, worst, keep);
			first = worst;
		}
		else
		{
			Thin (points, worst, last, keep);
			last = worst;
		}
	}
}

/* a copy of r thinned: its ends and the points where the flag changes kept */
static void ThinRoute (const groute_t *r, groute_t *out)
{
	qboolean	*keep;
	int		i, run;

	memset (out, 0, sizeof(*out));
	q_strlcpy (out->map, r->map, sizeof(out->map));
	if (!r->num)
		return;
	keep = (qboolean *) calloc (r->num, sizeof(qboolean));
	if (!keep)
		Sys_Error ("%s: out of memory", __thisfunc__);
	keep[0] = keep[r->num - 1] = true;
	for (run = 0, i = 1; i <= r->num; i++)
	{
		if (i < r->num && r->points[i].flag == r->points[run].flag)
			continue;
		keep[run] = keep[i - 1] = true;
		Thin (r->points, run, i - 1, keep);
		run = i;
	}
	for (i = 0; i < r->num; i++)
		if (keep[i])
			AddPoint (out, r->points[i].pos, r->points[i].flag);
	free (keep);
}

/* ==========================================================================
 * The game's state
 * ========================================================================== */

static void FreeState (gstate_t *st)
{
	free (st->ents);
	free (st->text);
	memset (st, 0, sizeof(*st));
}

static gent_t *AddEnt (gstate_t *st)
{
	gent_t	*e;

	if (st->num == st->max)
	{
		st->max = st->max ? st->max * 2 : 256;
		st->ents = (gent_t *) realloc (st->ents, st->max * sizeof(gent_t));
		if (!st->ents)
			Sys_Error ("%s: out of memory", __thisfunc__);
	}
	e = &st->ents[st->num++];
	memset (e, 0, sizeof(*e));
	return e;
}

/* a deep copy (the .gip text with it, the blocks rebased) */
static void CopyState (gstate_t *dst, const gstate_t *src)
{
	size_t	len;
	int	i;

	*dst = *src;
	dst->ents = NULL;
	dst->text = NULL;
	if (src->num)
	{
		dst->ents = (gent_t *) malloc (src->num * sizeof(gent_t));
		if (!dst->ents)
			Sys_Error ("%s: out of memory", __thisfunc__);
		memcpy (dst->ents, src->ents, src->num * sizeof(gent_t));
	}
	dst->max = src->num;
	if (src->text)
	{
		len = strlen (src->text) + 1;
		dst->text = (char *) malloc (len);
		if (!dst->text)
			Sys_Error ("%s: out of memory", __thisfunc__);
		memcpy (dst->text, src->text, len);
		for (i = 0; i < dst->num; i++)
			if (dst->ents[i].block)
				dst->ents[i].block = dst->text + (src->ents[i].block - src->text);
	}
}

/* left out of the candidates: they move by themselves (and a monster's
 * corpse stays): monsters, and point entities that walk, fly or bounce */
static qboolean Actor (int flags, int movetype, const char *model)
{
	if (flags & FL_MONSTER)
		return true;
	return model[0] != '*' && movetype != MOVETYPE_NONE && movetype != MOVETYPE_TOSS && movetype != MOVETYPE_PUSH;
}

/* a brush model's "*N"; else "classname@x,y,z" (its origin) for an entity
 * with a model, "classname:x,y,z" (its box's middle) for one without (a
 * trigger: InitTrigger clears the model, and its origin is 0 0 0) */
static void MakeKey (gent_t *e, const char *model)
{
	const float	*v = model[0] ? e->origin : e->center;

	if (model[0] == '*')
	{
		e->brush = true;
		q_strlcpy (e->key, model, sizeof(e->key));
	}
	else
		q_snprintf (e->key, sizeof(e->key), "%s%c%d,%d,%d", e->classname, model[0] ? '@' : ':',
			    (int) floor (v[0] + 0.5f), (int) floor (v[1] + 0.5f), (int) floor (v[2] + 0.5f));
}

static void LiveState (gstate_t *st)
{
	edict_t		*ed;
	gent_t		*e;
	eval_t		*val;
	const char	*classname, *model;
	int		i;

	memset (st, 0, sizeof(*st));
	q_strlcpy (st->map, sv.name, sizeof(st->map));
	st->live = true;
	for (i = svs.maxclients + 1; i < sv.num_edicts; i++)
	{
		ed = EDICT_NUM(i);
		if (ed->free || ((int) ed->v.flags & FL_ARCHIVE_OVERRIDE))
			continue;	/* not in saves: respawned with the map */
		classname = PR_GetString (ed->v.classname);
		if (!classname[0] || !strcmp (classname, "player"))
			continue;
		model = PR_GetString (ed->v.model);
		e = AddEnt (st);
		e->edict = i;
		q_strlcpy (e->classname, classname, sizeof(e->classname));
		VectorCopy (ed->v.origin, e->origin);
		VectorCopy (ed->v.angles, e->angles);
		if (!VectorCompare (ed->v.absmin, ed->v.absmax))
		{
			VectorAdd (ed->v.absmin, ed->v.absmax, e->center);
			VectorScale (e->center, 0.5f, e->center);
		}
		else
			VectorCopy (e->origin, e->center);
		e->has_model = (model[0] != 0);
		e->moving = !VectorCompare (ed->v.velocity, vec3_origin) || !VectorCompare (ed->v.avelocity, vec3_origin);
		e->actor = Actor ((int) ed->v.flags, (int) ed->v.movetype, model);
		val = GetEdictFieldValue (ed, "puzzle_id");
		if (val && val->string)
			q_strlcpy (e->puzzle_id, PR_GetString (val->string), sizeof(e->puzzle_id));
		val = GetEdictFieldValue (ed, "puzzle_piece_1");
		e->lock = val && val->string && PR_GetString (val->string)[0];
		MakeKey (e, model);
	}
}

/* a .gip's tokens: a value longer than com_token is cut, not the end */
#define GipParse(p)	COM_ParseEx ((p), CPE_ALLOWTRUNC)

static void ParseVec3 (const char *s, vec3_t v)
{
	VectorClear (v);
	sscanf (s, "%f %f %f", &v[0], &v[1], &v[2]);
}

/* <map>.gip of the game's user folder; false: none (not reached yet) */
static qboolean SavedState (const char *map, gstate_t *st)
{
	char		path[MAX_OSPATH], key[64], model[MAX_QPATH];
	const char	*p, *line;
	vec3_t		absmin, absmax, velocity, avelocity;
	gent_t		*e;
	FILE		*f;
	long		len;
	int		err, flags, movetype;

	memset (st, 0, sizeof(*st));
	q_strlcpy (st->map, map, sizeof(st->map));
	FS_MakePath_VABUF (FS_USERDIR, &err, path, sizeof(path), "%s.gip", map);
	if (err || !(f = fopen (path, "rb")))
		return false;
	fseek (f, 0, SEEK_END);
	len = ftell (f);
	fseek (f, 0, SEEK_SET);
	st->text = (char *) malloc (q_max (len, 0) + 1);
	if (!st->text || len < 0 || fread (st->text, 1, (size_t)len, f) != (size_t)len)
	{
		fclose (f);
		FreeState (st);
		return false;
	}
	fclose (f);
	st->text[len] = 0;

	/* after the header, the light styles and the effects: "-1" and the
	 * globals, then "<edict>" "{" "key" "value" ... "}" each */
	for (line = st->text; line && *line; line = strchr (line, '\n'), line = line ? line + 1 : NULL)
		if (line[0] == '-' && line[1] == '1' && (line[2] == '\n' || line[2] == '\r'))
			break;
	if (!line || !*line)
		return true;	/* no edicts */
	p = line;
	while ((p = GipParse (p)) != NULL && com_token[0])
	{
		/* p is after the edict's number */
		p = GipParse (p);
		if (!p || strcmp (com_token, "{"))
			break;
		e = NULL;
		model[0] = 0;
		flags = movetype = 0;
		VectorClear (absmin);
		VectorClear (absmax);
		VectorClear (velocity);
		VectorClear (avelocity);
		{
			gent_t	tmp;
			memset (&tmp, 0, sizeof(tmp));
			tmp.block = p;
			while ((p = GipParse (p)) != NULL && strcmp (com_token, "}"))
			{
				q_strlcpy (key, com_token, sizeof(key));
				if (!(p = GipParse (p)))
					break;
				if (!strcmp (key, "classname"))
					q_strlcpy (tmp.classname, com_token, sizeof(tmp.classname));
				else if (!strcmp (key, "model"))
					q_strlcpy (model, com_token, sizeof(model));
				else if (!strcmp (key, "origin"))
					ParseVec3 (com_token, tmp.origin);
				else if (!strcmp (key, "angles"))
					ParseVec3 (com_token, tmp.angles);
				else if (!strcmp (key, "absmin"))
					ParseVec3 (com_token, absmin);
				else if (!strcmp (key, "absmax"))
					ParseVec3 (com_token, absmax);
				else if (!strcmp (key, "velocity"))
					ParseVec3 (com_token, velocity);
				else if (!strcmp (key, "avelocity"))
					ParseVec3 (com_token, avelocity);
				else if (!strcmp (key, "movetype"))
					movetype = (int) atof (com_token);
				else if (!strcmp (key, "flags"))
					flags = (int) atof (com_token);
				else if (!strcmp (key, "puzzle_id"))
					q_strlcpy (tmp.puzzle_id, com_token, sizeof(tmp.puzzle_id));
				else if (!strcmp (key, "puzzle_piece_1"))
					tmp.lock = (com_token[0] != 0);
			}
			if (!tmp.classname[0] || !strcmp (tmp.classname, "player"))
				continue;	/* a free edict, or a client */
			e = AddEnt (st);
			*e = tmp;
		}
		if (!VectorCompare (absmin, absmax))
		{
			VectorAdd (absmin, absmax, e->center);
			VectorScale (e->center, 0.5f, e->center);
		}
		else
			VectorCopy (e->origin, e->center);
		e->has_model = (model[0] != 0);
		e->moving = !VectorCompare (velocity, vec3_origin) || !VectorCompare (avelocity, vec3_origin);
		e->actor = Actor (flags, movetype, model);
		e->edict = -1;
		MakeKey (e, model);
		if (!p)
			break;
	}
	return true;
}

/* the map's state now: live when it is the server's, else its .gip */
static qboolean StateNow (const char *map, gstate_t *st)
{
	if (sv.active && !q_strcasecmp (map, sv.name))
	{
		LiveState (st);
		return true;
	}
	return SavedState (map, st);
}

/* the entity with that key, NULL: none */
static const gent_t *FindKey (const gstate_t *st, const char *key)
{
	const gent_t	*best = NULL;
	char		classname[48];
	const char	*at;
	vec3_t		pos, d;
	float		dist, best_dist = POINT_MATCH;
	int		i;

	if (key[0] == '*')
	{
		for (i = 0; i < st->num; i++)
			if (st->ents[i].brush && !strcmp (st->ents[i].key, key))
				return &st->ents[i];
		return NULL;
	}
	at = strpbrk (key, "@:");
	if (!at || (size_t)(at - key) >= sizeof(classname))
		return NULL;
	memcpy (classname, key, at - key);
	classname[at - key] = 0;
	VectorClear (pos);
	if (sscanf (at + 1, "%f,%f,%f", &pos[0], &pos[1], &pos[2]) != 3)
		return NULL;
	for (i = 0; i < st->num; i++)
	{
		if (st->ents[i].brush || strcmp (st->ents[i].classname, classname))
			continue;
		/* by its origin ("@": a picked-up piece keeps it, its model
		 * cleared), or its box's middle */
		VectorSubtract ((*at == '@') ? st->ents[i].origin : st->ents[i].center, pos, d);
		dist = VectorLength (d);
		if (dist <= best_dist)
		{
			best_dist = dist;
			best = &st->ents[i];
		}
	}
	return best;
}

static qboolean Gone (const gstate_t *st, const char *key)
{
	const gent_t	*e = FindKey (st, key);

	/* an entity keyed by its model has to have one */
	return !e || (!e->has_model && !strchr (key, ':'));
}

static float AngleDelta (float a, float b)
{
	float	d = fmod (fabs (a - b), 360.0f);

	return (d > 180) ? 360 - d : d;
}

static qboolean Moved (const gent_t *e, const vec3_t origin, const vec3_t angles)
{
	int	i;

	for (i = 0; i < 3; i++)
		if (fabs (e->origin[i] - origin[i]) > MOVE_EPSILON || AngleDelta (e->angles[i], angles[i]) > MOVE_EPSILON)
			return true;
	return false;
}

/* a float field of the entity; a field the save leaves out is 0 */
static float FieldValue (const gent_t *e, const char *field)
{
	char		key[64];
	const char	*p;
	eval_t		*val;

	if (!e->block)
	{
		val = GetEdictFieldValue (EDICT_NUM(e->edict), field);
		return val ? val->_float : 0;
	}
	p = e->block;
	while ((p = GipParse (p)) != NULL && strcmp (com_token, "}"))
	{
		q_strlcpy (key, com_token, sizeof(key));
		if (!(p = GipParse (p)))
			break;
		if (!strcmp (key, field))
			return atof (com_token);
	}
	return 0;
}

static qboolean Compare (float a, const char *op, float b)
{
	if (!strcmp (op, "=="))	return a == b;
	if (!strcmp (op, "!="))	return a != b;
	if (!strcmp (op, "<"))	return a < b;
	if (!strcmp (op, "<="))	return a <= b;
	if (!strcmp (op, ">"))	return a > b;
	if (!strcmp (op, ">="))	return a >= b;
	return false;
}

static edict_t *Player (void)
{
	edict_t	*ed;

	if (!sv.active || svs.maxclients < 1 || !svs.clients[0].active)
		return NULL;
	ed = EDICT_NUM(1);
	return ed->free ? NULL : ed;
}

static int ServerFlags (void)
{
	return sv.active ? (int) *sv_globals.serverflags : svs.serverflags;
}

static qboolean Holds (edict_t *player, const char *id)
{
	string_t	*inv = &player->v.puzzle_inv1;
	int		i;

	for (i = 0; i < NUM_PUZZLE; i++)
		if (inv[i] && !q_strcasecmp (PR_GetString (inv[i]), id))
			return true;
	return false;
}

static qboolean Reached (const char *map)
{
	char	path[MAX_OSPATH];
	int	err;

	if (sv.active && !q_strcasecmp (map, sv.name))
		return true;
	FS_MakePath_VABUF (FS_USERDIR, &err, path, sizeof(path), "%s.gip", map);
	return !err && Sys_FileType (path) == FS_ENT_FILE;
}

/* whether the step is done; why not in *why */
static qboolean StepDone (const gstep_t *s, const char **why)
{
	const gcond_t	*c = &s->done;
	const gent_t	*e;
	gstate_t	st;
	edict_t		*player;
	qboolean	done = false;

	*why = "";
	switch (c->kind)
	{
	case COND_NONE:
		*why = "no condition";
		return false;
	case COND_ITEM:
		if (!(player = Player ()))
		{
			*why = "no local game";
			return false;
		}
		return Holds (player, c->arg);
	case COND_FLAG:
		return (ServerFlags () & (128 << c->flag)) != 0;
	case COND_ENTER:
		return Reached (c->arg);
	default:
		break;
	}
	if (!StateNow (s->map, &st))
	{
		*why = "map not reached";
		return false;
	}
	switch (c->kind)
	{
	case COND_GONE:
		done = Gone (&st, c->arg);
		break;
	case COND_MOVED:
		e = FindKey (&st, c->arg);
		done = !e || Moved (e, c->from_origin, c->from_angles);
		break;
	case COND_FIELD:
		if (!(e = FindKey (&st, c->arg)))
			*why = "no such entity";
		else
			done = Compare (FieldValue (e, c->field), c->op, c->value);
		break;
	default:
		break;
	}
	FreeState (&st);
	return done;
}

/* ==========================================================================
 * Reading and writing the file
 * ========================================================================== */

static void CondText (const gcond_t *c, char *buf, size_t size)
{
	switch (c->kind)
	{
	case COND_ITEM:
	case COND_GONE:
	case COND_ENTER:
		q_snprintf (buf, size, "%s %s", cond_names[c->kind], c->arg);
		break;
	case COND_FLAG:
		q_snprintf (buf, size, "flag %d", c->flag);
		break;
	case COND_MOVED:
		q_snprintf (buf, size, "moved %s from %g %g %g %g %g %g", c->arg,
			    c->from_origin[0], c->from_origin[1], c->from_origin[2],
			    c->from_angles[0], c->from_angles[1], c->from_angles[2]);
		break;
	case COND_FIELD:
		q_snprintf (buf, size, "field %s %s %s %g", c->arg, c->field, c->op, c->value);
		break;
	default:
		q_strlcpy (buf, "none", size);
		break;
	}
}

#define MAX_LINE_TOKENS	8

/* a hub's or map's name: it goes into file paths */
static qboolean ValidName (const char *name)
{
	return name[0] && !strpbrk (name, "/\\:") && !strstr (name, "..") && strlen (name) < MAX_QPATH - 16;
}

static qboolean KnownOp (const char *op)
{
	return !strcmp (op, "==") || !strcmp (op, "!=") || !strcmp (op, "<") || !strcmp (op, "<=") ||
	       !strcmp (op, ">") || !strcmp (op, ">=");
}

/* the line's first tokens (at most max) */
static int Tokens (const char *line, char tokens[][MAX_GUIDE_KEY], int max)
{
	const char	*p = line;
	int		n = 0;

	while (n < max && (p = COM_Parse (p)) != NULL && com_token[0])
		q_strlcpy (tokens[n++], com_token, MAX_GUIDE_KEY);
	return n;
}

/* the line after its first n tokens */
static const char *SkipTokens (const char *line, int n)
{
	const char	*p = line;

	while (n-- > 0 && p)
		p = COM_Parse (p);
	return p ? p : "";
}

static qboolean ParseCond (char argv[][MAX_GUIDE_KEY], int argc, gcond_t *c)
{
	int	i;

	memset (c, 0, sizeof(*c));
	for (i = 0; i < (int)Q_COUNTOF(cond_names); i++)
		if (!strcmp (argv[0], cond_names[i]))
			break;
	if (i == (int)Q_COUNTOF(cond_names))
		return false;
	c->kind = (condkind_t) i;
	switch (c->kind)
	{
	case COND_NONE:
		return argc == 1;
	case COND_ITEM:
	case COND_GONE:
	case COND_ENTER:
		if (argc != 2)
			return false;
		q_strlcpy (c->arg, argv[1], sizeof(c->arg));
		return c->kind != COND_ENTER || ValidName (c->arg);
	case COND_FLAG:
		if (argc != 2)
			return false;
		c->flag = atoi (argv[1]);
		return c->flag >= 1 && c->flag <= 8;
	case COND_FIELD:
		if (argc != 5)
			return false;
		q_strlcpy (c->arg, argv[1], sizeof(c->arg));
		q_strlcpy (c->field, argv[2], sizeof(c->field));
		q_strlcpy (c->op, argv[3], sizeof(c->op));
		c->value = atof (argv[4]);
		return KnownOp (c->op);
	case COND_MOVED:
		break;
	}
	return false;	/* moved: parsed by the caller (more tokens) */
}

static qboolean ParseMoved (const char *rest, gcond_t *c)
{
	vec3_t	o, a;

	if (sscanf (rest, " from %f %f %f %f %f %f", &o[0], &o[1], &o[2], &a[0], &a[1], &a[2]) != 6)
		return false;
	VectorCopy (o, c->from_origin);
	VectorCopy (a, c->from_angles);
	return true;
}

static void ParseRoute (const char *rest, gstep_t *s, int *max_routes, const char *map)
{
	groute_t	*r = AddRoute (&s->routes, &s->num_routes, max_routes, map);
	const char	*p = rest;
	vec3_t		v;
	int		n = 0;

	while ((p = COM_Parse (p)) != NULL && com_token[0])
	{
		if ((com_token[0] == 'w' || com_token[0] == 'j') && !com_token[1])
		{
			if (r->num)
				r->points[r->num - 1].flag = com_token[0];
			continue;
		}
		v[n++] = atof (com_token);
		if (n == 3)
		{
			AddPoint (r, v, 0);
			n = 0;
		}
	}
}

/* the file's text into g; false: no hub line */
static qboolean ParseGuide (char *text, gguide_t *g)
{
	char		argv[MAX_LINE_TOKENS][MAX_GUIDE_KEY], *line, *next;
	const char	*p;
	gstep_t		*s = NULL;
	int		argc, number, max_routes = 0;

	for (line = text, number = 1; line; line = next, number++)
	{
		next = strchr (line, '\n');
		if (next)
			*next++ = 0;
		argc = Tokens (line, argv, MAX_LINE_TOKENS);
		if (!argc)
			continue;
		if (!strcmp (argv[0], "hub") && argc >= 2)
		{
			q_strlcpy (g->hub, argv[1], sizeof(g->hub));
			for (p = SkipTokens (line, 2); (p = COM_Parse (p)) != NULL && com_token[0]; )
				if (ValidName (com_token))
					AddHubMap (g, com_token);
		}
		else if (!strcmp (argv[0], "step"))
		{
			s = NULL;	/* a bad step's done and route lines are skipped too */
			if (argc != 7 || strcmp (argv[3], "at") || !ValidName (argv[2]))
				goto bad;
			s = AddStep (g);
			max_routes = 0;
			COM_Parse (SkipTokens (line, 1));	/* the whole text: argv's are shorter */
			q_strlcpy (s->text, com_token, sizeof(s->text));
			q_strlcpy (s->map, argv[2], sizeof(s->map));
			s->at[0] = atof (argv[4]);
			s->at[1] = atof (argv[5]);
			s->at[2] = atof (argv[6]);
		}
		else if (s && !strcmp (argv[0], "done") && argc >= 2)
		{
			if (ParseCond (argv + 1, argc - 1, &s->done))
				continue;
			if (!strcmp (argv[1], "moved") && argc >= 3)
			{
				memset (&s->done, 0, sizeof(s->done));
				s->done.kind = COND_MOVED;
				q_strlcpy (s->done.arg, argv[2], sizeof(s->done.arg));
				if (ParseMoved (SkipTokens (line, 3), &s->done))
					continue;
			}
			memset (&s->done, 0, sizeof(s->done));
			goto bad;
		}
		else if (!strcmp (argv[0], "route") && argc >= 2)
		{
			if (!s)
				continue;	/* a skipped step's */
			if (!ValidName (argv[1]))
				goto bad;
			ParseRoute (SkipTokens (line, 2), s, &max_routes, argv[1]);
		}
		else if (!strcmp (argv[0], "done") && !s)
			continue;	/* a skipped step's */
		else
			goto bad;
		continue;
bad:
		if (g->issues++ < 8)
			Con_Printf ("%s line %d: skipped\n", g->path, number);
	}
	return g->hub[0] != 0;
}

static char *ReadText (const char *path)
{
	FILE	*f;
	long	len;
	char	*text;

	if (!(f = fopen (path, "rb")))
		return NULL;
	fseek (f, 0, SEEK_END);
	len = ftell (f);
	fseek (f, 0, SEEK_SET);
	text = (char *) malloc (q_max (len, 0) + 1);
	if (!text || len < 0 || fread (text, 1, (size_t)len, f) != (size_t)len)
	{
		fclose (f);
		free (text);
		return NULL;
	}
	fclose (f);
	text[len] = 0;
	return text;
}

static qboolean LoadGuideFile (const char *path, gguide_t *g)
{
	char	*text = ReadText (path);
	qboolean ok;

	memset (g, 0, sizeof(*g));
	if (!text)
		return false;
	q_strlcpy (g->path, path, sizeof(g->path));
	ok = ParseGuide (text, g);
	free (text);
	if (!ok)
		FreeGuide (g);
	return ok;
}

static void UserGuidePath (const char *hub, char *path, size_t size)
{
	int	err;

	FS_MakePath_VABUF (FS_USERDIR, &err, path, size, "maps/%s" GUIDE_EXT, hub);
	if (err)
		path[0] = 0;
}

static void ShippedGuidePath (const char *hub, char *path, size_t size)
{
	char	name[MAX_QPATH];

	q_snprintf (name, sizeof(name), "guide\\%s" GUIDE_EXT, hub);
	VK_ExePath (name, path, size);
}

/* the hub's guide: the user folder's, else the shipped one */
static qboolean LoadHub (const char *hub, gguide_t *g)
{
	char	path[MAX_OSPATH];

	UserGuidePath (hub, path, sizeof(path));
	if (path[0] && LoadGuideFile (path, g))
		return true;
	ShippedGuidePath (hub, path, sizeof(path));
	return LoadGuideFile (path, g);
}

static qboolean ListsMap (const gguide_t *g, const char *map)
{
	int	i;

	for (i = 0; i < g->num_maps; i++)
		if (!q_strcasecmp (g->maps[i], map))
			return true;
	return false;
}

/* the guide of the hub that has the map, the user folder's first */
static qboolean LoadHubOfMap (const char *map, gguide_t *g)
{
	char		dir[MAX_OSPATH], path[MAX_OSPATH];
	const char	*name;
	int		pass, err;
	qboolean	found = false;

	for (pass = 0; pass < 2 && !found; pass++)
	{
		if (pass == 0)
		{
			FS_MakePath_BUF (FS_USERDIR, &err, dir, sizeof(dir), "maps");
			if (err)
				continue;
		}
		else
			VK_ExePath ("guide", dir, sizeof(dir));
		for (name = Sys_FindFirstFile (dir, "*" GUIDE_EXT); name && !found; name = Sys_FindNextFile ())
		{
			q_snprintf (path, sizeof(path), "%s/%s", dir, name);
			if (!LoadGuideFile (path, g))
				continue;
			if (ListsMap (g, map))
				found = true;
			else
				FreeGuide (g);
		}
		Sys_FindClose ();
	}
	return found;
}

static void WriteRoute (FILE *f, const groute_t *r)
{
	int	i;

	fprintf (f, "\troute %s", r->map);
	for (i = 0; i < r->num; i++)
	{
		fprintf (f, "  %d %d %d", (int) floor (r->points[i].pos[0] + 0.5f),
			 (int) floor (r->points[i].pos[1] + 0.5f), (int) floor (r->points[i].pos[2] + 0.5f));
		if (r->points[i].flag)
			fprintf (f, " %c", r->points[i].flag);
	}
	fprintf (f, "\r\n");
}

static qboolean SaveGuide (const gguide_t *g)
{
	char	path[MAX_OSPATH], tmp[MAX_OSPATH], cond[256];
	FILE	*f;
	int	i, j;
	qboolean ok;

	UserGuidePath (g->hub, path, sizeof(path));
	if (!path[0] || FS_CreatePath (path))
	{
		Con_Printf ("couldn't make the folder for %s\n", path);
		return false;
	}
	q_snprintf (tmp, sizeof(tmp), "%s.tmp", path);
	if (!(f = fopen (tmp, "wb")))
	{
		Con_Printf ("couldn't write %s\n", tmp);
		return false;
	}
	fprintf (f, "// %s: Hexenlicht's guide steps (guide_record save)\r\n", g->hub);
	fprintf (f, "hub %s", g->hub);
	for (i = 0; i < g->num_maps; i++)
		fprintf (f, " %s", g->maps[i]);
	fprintf (f, "\r\n");
	for (i = 0; i < g->num_steps; i++)
	{
		const gstep_t *s = &g->steps[i];

		CondText (&s->done, cond, sizeof(cond));
		fprintf (f, "\r\nstep \"%s\" %s at %d %d %d\r\n", s->text, s->map,
			 (int) floor (s->at[0] + 0.5f), (int) floor (s->at[1] + 0.5f), (int) floor (s->at[2] + 0.5f));
		fprintf (f, "\tdone %s\r\n", cond);
		for (j = 0; j < s->num_routes; j++)
			if (s->routes[j].num)
				WriteRoute (f, &s->routes[j]);
	}
	ok = !ferror (f);
	ok &= (fclose (f) == 0);
	if (ok)
	{
		ok = MoveFileExA (tmp, path, MOVEFILE_REPLACE_EXISTING) != 0;	/* the old file stays if it fails */
		if (!ok)
			Con_Printf ("couldn't rename %s to %s\n", tmp, path);
	}
	else
	{
		Con_Printf ("couldn't write %s\n", tmp);
		remove (tmp);
	}
	if (ok)
		Con_Printf ("saved %s: %d steps, %d maps\n", path, g->num_steps, g->num_maps);
	return ok;
}

/* ==========================================================================
 * Recording
 * ========================================================================== */

static void ClearPending (void)
{
	FreeRoutes (rec.segs, rec.num_segs);
	rec.segs = NULL;
	rec.num_segs = rec.max_segs = 0;
	rec.pending_points = 0;
}

static void ClearSnapshots (gstate_t *snaps, int *num)
{
	int	i;

	for (i = 0; i < *num; i++)
		FreeState (&snaps[i]);
	*num = 0;
}

static gstate_t *Snapshot (gstate_t *snaps, int num, const char *map)
{
	int	i;

	for (i = 0; i < num; i++)
		if (!q_strcasecmp (snaps[i].map, map))
			return &snaps[i];
	return NULL;
}

static void TakePlayerState (void)
{
	edict_t		*player = Player ();
	string_t	*inv;
	int		i;

	memset (rec.inv, 0, sizeof(rec.inv));
	if (player)
		for (inv = &player->v.puzzle_inv1, i = 0; i < NUM_PUZZLE; i++)
			if (inv[i])
				q_strlcpy (rec.inv[i], PR_GetString (inv[i]), sizeof(rec.inv[i]));
	rec.serverflags = ServerFlags ();
}

/* the current map's first snapshot */
static void TakeEntrySnapshot (void)
{
	if (rec.num_snaps < MAX_GUIDE_MAPS && !Snapshot (rec.snaps, rec.num_snaps, sv.name))
		LiveState (&rec.snaps[rec.num_snaps++]);
	rec.entry_pending = false;
}

/* every snapshot map's state now, and the current map's */
static void ResnapshotAll (void)
{
	gstate_t	st;
	int		i;

	for (i = 0; i < rec.num_snaps; i++)
	{
		if (!StateNow (rec.snaps[i].map, &st))
			continue;
		FreeState (&rec.snaps[i]);
		rec.snaps[i] = st;
	}
	if (sv.active)
		TakeEntrySnapshot ();
	TakePlayerState ();
	rec.num_entered = 0;
}

static void StartSegment (const char *map)
{
	AddRoute (&rec.segs, &rec.num_segs, &rec.max_segs, map);
}

/* host.c, every frame after the server's */
void HL_GuideFrame (void)
{
	edict_t		*player;
	groute_t	*seg;
	vec3_t		d;
	char		flag;
	qboolean	new_seg = false;

	if (!rec.active || !(player = Player ()))
		return;
	if (!svs.clients[0].spawned || sv.paused)
		return;		/* not in the level yet (its origin 0 0 0), or paused */

	if (q_strcasecmp (rec.map, sv.name))
	{
		/* left within ENTRY_DELAY: the map's snapshot from its .gip, as
		 * just written */
		if (rec.entry_pending && rec.num_snaps < MAX_GUIDE_MAPS && SavedState (rec.map, &rec.snaps[rec.num_snaps]))
			rec.num_snaps++;
		rec.entry_pending = false;
		q_strlcpy (rec.map, sv.name, sizeof(rec.map));
		AddHubMap (&rec.guide, sv.name);
		if (rec.num_entered < MAX_GUIDE_MAPS)
			q_strlcpy (rec.entered[rec.num_entered++], sv.name, sizeof(rec.entered[0]));
		if (!Snapshot (rec.snaps, rec.num_snaps, sv.name))
		{
			rec.entry_pending = true;
			rec.entry_time = sv.time;
		}
		new_seg = true;
		rec.have_last = false;
	}
	else if (sv.time < rec.last_time)
	{
		/* a load or a restart in this map: the next mark compares with
		 * the state loaded (a load into another map can't be told from
		 * a level change: guide_record resync) */
		new_seg = true;
		rec.have_last = false;
		ResnapshotAll ();
		rec.undo_ready = false;
	}
	rec.last_time = sv.time;
	if (rec.entry_pending && sv.time >= rec.entry_time + ENTRY_DELAY)
		TakeEntrySnapshot ();

	if (player->v.waterlevel >= 2)
		flag = 'w';
	else if (!((int) player->v.flags & FL_ONGROUND))
		flag = 'j';
	else
		flag = 0;

	if (rec.have_last)
	{
		VectorSubtract (player->v.origin, rec.last, d);
		if (VectorLength (d) > ROUTE_JUMP)
			new_seg = true;		/* a teleport */
	}
	VectorCopy (player->v.origin, rec.last);
	rec.have_last = true;
	if (new_seg || !rec.num_segs)
		StartSegment (sv.name);

	seg = &rec.segs[rec.num_segs - 1];
	if (seg->num)
	{
		VectorSubtract (player->v.origin, seg->points[seg->num - 1].pos, d);
		if (VectorLength (d) < ROUTE_SPACING && flag == seg->points[seg->num - 1].flag)
			return;
	}
	if (rec.pending_points >= MAX_PENDING_POINTS)
	{
		rec.dropped++;
		return;
	}
	AddPoint (seg, player->v.origin, flag);
	rec.pending_points++;
}

static int AddCandidate (const gcond_t *c)
{
	/* the last place is kept for none, which comes last */
	if (rec.num_cands >= MAX_CANDIDATES - (c->kind != COND_NONE))
	{
		rec.cands_dropped++;
		return 0;
	}
	rec.cands[rec.num_cands] = *c;
	rec.cands[rec.num_cands].order = rec.num_cands;
	rec.num_cands++;
	return 1;
}

/* by rank, then as found (qsort isn't stable) */
static int CompareCandidates (const void *a, const void *b)
{
	const gcond_t	*x = (const gcond_t *) a, *y = (const gcond_t *) b;

	return (x->rank != y->rank) ? x->rank - y->rank : x->order - y->order;
}

/* where an item was: the piece now gone with that id, in a snapshot */
static qboolean ItemPlace (const char *id, gcond_t *c)
{
	gstate_t	st;
	int		i, j;
	qboolean	found = false;

	for (i = 0; i < rec.num_snaps && !found; i++)
	{
		const gstate_t *before = &rec.snaps[i];

		if (!StateNow (before->map, &st))
			continue;
		for (j = 0; j < before->num; j++)
		{
			const gent_t *e = &before->ents[j];

			if (e->puzzle_id[0] && !q_strcasecmp (e->puzzle_id, id) && e->has_model && Gone (&st, e->key))
			{
				q_strlcpy (c->map, before->map, sizeof(c->map));
				VectorCopy (e->center, c->place);
				found = true;
				break;
			}
		}
		FreeState (&st);
	}
	return found;
}

static void BuildCandidates (void)
{
	edict_t		*player = Player ();
	string_t	*inv;
	gstate_t	st;
	gcond_t		c;
	int		i, j, flags, bit;
	const char	*id;

	rec.num_cands = rec.cands_dropped = 0;

	/* puzzle pieces gained */
	for (inv = &player->v.puzzle_inv1, i = 0; i < NUM_PUZZLE; i++)
	{
		if (!inv[i] || !*(id = PR_GetString (inv[i])))
			continue;
		for (j = 0; j < NUM_PUZZLE; j++)
			if (!q_strcasecmp (rec.inv[j], id))
				break;
		if (j < NUM_PUZZLE)
			continue;
		memset (&c, 0, sizeof(c));
		c.kind = COND_ITEM;
		c.rank = 0;
		q_strlcpy (c.arg, id, sizeof(c.arg));
		if (!ItemPlace (id, &c))
		{
			q_strlcpy (c.map, sv.name, sizeof(c.map));
			VectorCopy (player->v.origin, c.place);
		}
		AddCandidate (&c);
	}

	/* crosslevel bits set */
	flags = ServerFlags ();
	for (bit = 1; bit <= 8; bit++)
	{
		if (!(flags & (128 << bit)) || (rec.serverflags & (128 << bit)))
			continue;
		memset (&c, 0, sizeof(c));
		c.kind = COND_FLAG;
		c.rank = 1;
		c.flag = bit;
		q_strlcpy (c.map, sv.name, sizeof(c.map));
		VectorCopy (player->v.origin, c.place);
		AddCandidate (&c);
	}

	/* entities gone or moved in the snapshot maps */
	for (i = 0; i < rec.num_snaps; i++)
	{
		const gstate_t *before = &rec.snaps[i];

		if (!StateNow (before->map, &st))
			continue;
		for (j = 0; j < before->num; j++)
		{
			const gent_t	*e = &before->ents[j], *now;

			if (e->actor)
				continue;
			memset (&c, 0, sizeof(c));
			q_strlcpy (c.map, before->map, sizeof(c.map));
			q_strlcpy (c.arg, e->key, sizeof(c.arg));
			now = FindKey (&st, e->key);
			if (Gone (&st, e->key))
			{
				/* a used lock or a brush first: a trigger_once the
				 * player walked through is removed too */
				c.kind = COND_GONE;
				c.rank = (e->lock || e->brush) ? 2 : 5;
				VectorCopy (e->center, c.place);
			}
			else if (e->brush && Moved (now, e->origin, e->angles))
			{
				c.kind = COND_MOVED;
				c.rank = now->moving ? 7 : 3;
				VectorCopy (e->origin, c.from_origin);
				VectorCopy (e->angles, c.from_angles);
				VectorCopy (now->center, c.place);
			}
			else
				continue;
			AddCandidate (&c);
		}
		FreeState (&st);
	}

	/* maps entered, the last first */
	for (i = rec.num_entered - 1; i >= 0; i--)
	{
		memset (&c, 0, sizeof(c));
		c.kind = COND_ENTER;
		c.rank = 6;
		q_strlcpy (c.arg, rec.entered[i], sizeof(c.arg));
		q_strlcpy (c.map, rec.entered[i], sizeof(c.map));
		VectorCopy (player->v.origin, c.place);
		for (j = 0; j < rec.num_segs; j++)
			if (!q_strcasecmp (rec.segs[j].map, rec.entered[i]) && rec.segs[j].num)
			{
				VectorCopy (rec.segs[j].points[0].pos, c.place);
				break;
			}
		AddCandidate (&c);
	}

	/* nothing the game keeps: the player's place */
	memset (&c, 0, sizeof(c));
	c.kind = COND_NONE;
	c.rank = 8;
	q_strlcpy (c.map, sv.name, sizeof(c.map));
	VectorCopy (player->v.origin, c.place);
	AddCandidate (&c);

	qsort (rec.cands, rec.num_cands, sizeof(gcond_t), CompareCandidates);
}

static void SetCondition (gstep_t *s, const gcond_t *c)
{
	s->done = *c;
	q_strlcpy (s->map, c->map, sizeof(s->map));
	VectorCopy (c->place, s->at);
}

static void PrintStep (int n, const gstep_t *s)
{
	char	cond[256];
	int	i, points = 0;

	for (i = 0; i < s->num_routes; i++)
		points += s->routes[i].num;
	CondText (&s->done, cond, sizeof(cond));
	Con_Printf ("step %d \"%s\" %s at %.0f %.0f %.0f: done %s; route %d points in %d parts\n",
		    n, s->text, s->map, s->at[0], s->at[1], s->at[2], cond, points, s->num_routes);
}

static void PrintCandidates (void)
{
	char	cond[256];
	int	i;

	for (i = 0; i < rec.num_cands && i < MAX_PRINTED_CANDIDATES; i++)
	{
		CondText (&rec.cands[i], cond, sizeof(cond));
		Con_Printf ("  %d: %s (%s at %.0f %.0f %.0f)\n", i + 1, cond,
			    rec.cands[i].map, rec.cands[i].place[0], rec.cands[i].place[1], rec.cands[i].place[2]);
	}
	if (rec.num_cands > MAX_PRINTED_CANDIDATES)
		Con_Printf ("  (%d more, to %d: none)\n", rec.num_cands - MAX_PRINTED_CANDIDATES, rec.num_cands);
	if (rec.cands_dropped)
		Con_Printf ("  (%d more changes left out: over %d)\n", rec.cands_dropped, MAX_CANDIDATES);
}

static void SaveUndo (void)
{
	int	i;

	ClearSnapshots (rec.undo_snaps, &rec.undo_num_snaps);
	for (i = 0; i < rec.num_snaps; i++)
		CopyState (&rec.undo_snaps[i], &rec.snaps[i]);
	rec.undo_num_snaps = rec.num_snaps;
	memcpy (rec.undo_inv, rec.inv, sizeof(rec.inv));
	rec.undo_serverflags = rec.serverflags;
	memcpy (rec.undo_entered, rec.entered, sizeof(rec.entered));
	rec.undo_num_entered = rec.num_entered;
	rec.undo_ready = true;
}

static void Guide_Mark_f (void)
{
	gstep_t		*s;
	groute_t	*last;
	vec3_t		here;
	char		flag;
	int		i;

	if (Cmd_Argc () != 2)
	{
		Con_Printf ("guide_mark \"<text>\": a step of the recording\n");
		return;
	}
	if (!rec.active || !Player ())
	{
		Con_Printf ("not recording (guide_record start <hub>, in a local game)\n");
		return;
	}
	if (strchr (Cmd_Argv (1), '"') || !Cmd_Argv (1)[0])
	{
		Con_Printf ("a text without quotes\n");
		return;
	}
	if (rec.entry_pending)
		TakeEntrySnapshot ();

	BuildCandidates ();
	SaveUndo ();

	s = AddStep (&rec.guide);
	q_strlcpy (s->text, Cmd_Argv (1), sizeof(s->text));
	SetCondition (s, &rec.cands[0]);
	{
		int max_routes = 0;

		for (i = 0; i < rec.num_segs; i++)
		{
			groute_t *r;

			if (rec.segs[i].num < 1)
				continue;
			r = AddRoute (&s->routes, &s->num_routes, &max_routes, rec.segs[i].map);
			ThinRoute (&rec.segs[i], r);
		}
	}
	/* the pending route, kept for guide_undo, continues from here */
	last = rec.num_segs ? &rec.segs[rec.num_segs - 1] : NULL;
	if (last && last->num)
	{
		VectorCopy (last->points[last->num - 1].pos, here);
		flag = last->points[last->num - 1].flag;
	}
	else
	{
		VectorCopy (Player ()->v.origin, here);
		flag = 0;
	}
	ClearPending ();
	StartSegment (sv.name);
	AddPoint (&rec.segs[0], here, flag);
	rec.pending_points = 1;

	ResnapshotAll ();
	rec.unsaved++;

	PrintStep (rec.guide.num_steps, s);
	Con_Printf ("candidates (guide_cond <n>):\n");
	PrintCandidates ();
	if (rec.dropped)
		Con_Printf ("%d route points were dropped (more than %d since the mark before)\n", rec.dropped, MAX_PENDING_POINTS);
	rec.dropped = 0;
}

static void Guide_Cond_f (void)
{
	gstep_t	*s;
	int	n;

	if (!rec.active || !rec.guide.num_steps || !rec.num_cands)
	{
		Con_Printf ("no step to change (guide_mark first)\n");
		return;
	}
	if (Cmd_Argc () != 2)
	{
		Con_Printf ("guide_cond <n>: the last step's condition, of these:\n");
		PrintCandidates ();
		return;
	}
	n = atoi (Cmd_Argv (1));
	if (n < 1 || n > rec.num_cands)
	{
		Con_Printf ("1 to %d\n", rec.num_cands);
		return;
	}
	s = &rec.guide.steps[rec.guide.num_steps - 1];
	SetCondition (s, &rec.cands[n - 1]);
	rec.unsaved++;
	PrintStep (rec.guide.num_steps, s);
}

static void Guide_Undo_f (void)
{
	gstep_t		*s;
	groute_t	*segs;
	int		i, num, max = 0;

	if (!rec.active || !rec.guide.num_steps)
	{
		Con_Printf ("no step to undo\n");
		return;
	}
	s = &rec.guide.steps[--rec.guide.num_steps];
	Con_Printf ("dropped step %d \"%s\"\n", rec.guide.num_steps + 1, s->text);
	rec.num_cands = 0;
	rec.unsaved++;
	if (!rec.undo_ready)
	{
		FreeRoutes (s->routes, s->num_routes);
		return;
	}
	/* its route back in front of the pending one (thinned), and the state
	 * before its mark */
	segs = NULL;
	num = 0;
	for (i = 0; i < s->num_routes; i++)
	{
		groute_t *r = AddRoute (&segs, &num, &max, s->routes[i].map);
		int j;

		for (j = 0; j < s->routes[i].num; j++)
			AddPoint (r, s->routes[i].points[j].pos, s->routes[i].points[j].flag);
	}
	for (i = 0; i < rec.num_segs; i++)
	{
		groute_t *r = AddRoute (&segs, &num, &max, rec.segs[i].map);
		int j;

		/* the mark's point repeated the step's last: skip it */
		for (j = (i == 0 && s->num_routes) ? 1 : 0; j < rec.segs[i].num; j++)
			AddPoint (r, rec.segs[i].points[j].pos, rec.segs[i].points[j].flag);
	}
	FreeRoutes (s->routes, s->num_routes);
	ClearPending ();
	rec.segs = segs;
	rec.num_segs = num;
	rec.max_segs = max;
	for (i = 0; i < num; i++)
		rec.pending_points += segs[i].num;

	/* the snapshots before the mark, and those of maps first entered since
	 * (their entry snapshots) kept */
	{
		gstate_t	keep[MAX_GUIDE_MAPS];
		char		since[MAX_GUIDE_MAPS][MAX_QPATH];
		int		num_keep = 0, num_since = rec.num_entered;

		for (i = 0; i < rec.num_snaps; i++)
		{
			if (Snapshot (rec.undo_snaps, rec.undo_num_snaps, rec.snaps[i].map))
				FreeState (&rec.snaps[i]);
			else
				keep[num_keep++] = rec.snaps[i];
		}
		memcpy (since, rec.entered, sizeof(since));
		for (i = 0; i < rec.undo_num_snaps; i++)
			rec.snaps[i] = rec.undo_snaps[i];
		rec.num_snaps = rec.undo_num_snaps;
		for (i = 0; i < num_keep && rec.num_snaps < MAX_GUIDE_MAPS; i++)
			rec.snaps[rec.num_snaps++] = keep[i];
		for (; i < num_keep; i++)
			FreeState (&keep[i]);
		rec.undo_num_snaps = 0;
		memcpy (rec.entered, rec.undo_entered, sizeof(rec.entered));
		rec.num_entered = rec.undo_num_entered;
		for (i = 0; i < num_since && rec.num_entered < MAX_GUIDE_MAPS; i++)
			q_strlcpy (rec.entered[rec.num_entered++], since[i], sizeof(rec.entered[0]));
	}
	memcpy (rec.inv, rec.undo_inv, sizeof(rec.inv));
	rec.serverflags = rec.undo_serverflags;
	rec.undo_ready = false;
	Con_Printf ("its route and the state before it are back: the next mark sees the same changes\n");
}

static void StopRecording (void)
{
	FreeGuide (&rec.guide);
	ClearPending ();
	ClearSnapshots (rec.snaps, &rec.num_snaps);
	ClearSnapshots (rec.undo_snaps, &rec.undo_num_snaps);
	memset (&rec, 0, sizeof(rec));
}

static void Guide_Record_f (void)
{
	const char	*cmd = (Cmd_Argc () >= 2) ? Cmd_Argv (1) : "";
	qboolean	cont;

	if (!strcmp (cmd, "start") || !strcmp (cmd, "continue"))
	{
		char	path[MAX_OSPATH];

		cont = !strcmp (cmd, "continue");
		if (Cmd_Argc () != 3 || !ValidName (Cmd_Argv (2)))
		{
			Con_Printf ("guide_record %s <hub>\n", cmd);
			return;
		}
		if (!Player ())
		{
			Con_Printf ("guide_record needs a local game\n");
			return;
		}
		if (rec.active && rec.unsaved)
		{
			Con_Printf ("the recording of %s has %d unsaved changes: guide_record save or stop first\n", rec.guide.hub, rec.unsaved);
			return;
		}
		StopRecording ();
		UserGuidePath (Cmd_Argv (2), path, sizeof(path));
		if (cont)
		{
			if (!LoadHub (Cmd_Argv (2), &rec.guide))
			{
				Con_Printf ("no guide for %s\n", Cmd_Argv (2));
				return;
			}
		}
		else if (path[0] && Sys_FileType (path) == FS_ENT_FILE)
		{
			Con_Printf ("%s exists: guide_record continue %s, or delete it\n", path, Cmd_Argv (2));
			return;
		}
		q_strlcpy (rec.guide.hub, Cmd_Argv (2), sizeof(rec.guide.hub));
		rec.active = true;
		/* the current map: snapshot now, entered */
		q_strlcpy (rec.map, sv.name, sizeof(rec.map));
		rec.last_time = sv.time;
		AddHubMap (&rec.guide, sv.name);
		TakeEntrySnapshot ();
		TakePlayerState ();
		Con_Printf ("recording %s from %s (%d steps): guide_mark \"<text>\" after each step\n",
			    rec.guide.hub, sv.name, rec.guide.num_steps);
		return;
	}
	if (!rec.active)
	{
		Con_Printf ("guide_record start <hub> | continue <hub>: not recording\n");
		return;
	}
	if (!strcmp (cmd, "save"))
	{
		if (SaveGuide (&rec.guide))
			rec.unsaved = 0;
	}
	else if (!strcmp (cmd, "resync"))
	{
		if (!Player ())
		{
			Con_Printf ("guide_record resync needs a local game\n");
			return;
		}
		ResnapshotAll ();
		rec.undo_ready = false;
		ClearPending ();
		rec.have_last = false;
		Con_Printf ("the state now is the next mark's start (after a load); the route since the last mark dropped\n");
	}
	else if (!strcmp (cmd, "stop"))
	{
		if (rec.unsaved)
			Con_Printf ("dropped %d unsaved changes\n", rec.unsaved);
		StopRecording ();
		Con_Printf ("stopped\n");
	}
	else
	{
		int i, points = 0;

		for (i = 0; i < rec.num_segs; i++)
			points += rec.segs[i].num;
		Con_Printf ("recording %s: %d steps, %d maps, %d unsaved changes; %d route points since the last mark; %d maps in the snapshot\n",
			    rec.guide.hub, rec.guide.num_steps, rec.guide.num_maps, rec.unsaved, points, rec.num_snaps);
		Con_Printf ("guide_record save | stop | resync | start <hub> | continue <hub>\n");
	}
}

/* ==========================================================================
 * Checking
 * ========================================================================== */

static void CheckGuide (const gguide_t *g)
{
	const char	**why;
	qboolean	*done;
	int		i, next = 0, count = 0;
	char		cond[256];

	if (!g->num_steps)
	{
		Con_Printf ("%s: no steps\n", g->hub);
		return;
	}
	done = (qboolean *) calloc (g->num_steps, sizeof(qboolean));
	why = (const char **) calloc (g->num_steps, sizeof(const char *));
	if (!done || !why)
		Sys_Error ("%s: out of memory", __thisfunc__);
	for (i = 0; i < g->num_steps; i++)
	{
		done[i] = StepDone (&g->steps[i], &why[i]);
		if (done[i])
		{
			next = i + 1;
			count++;
		}
	}
	Con_Printf ("%s%s%s: %d steps, %d done; ", g->hub, g->path[0] ? " " : "", g->path, g->num_steps, count);
	if (next < g->num_steps)
		Con_Printf ("next: %d \"%s\"\n", next + 1, g->steps[next].text);
	else
		Con_Printf ("all done\n");
	for (i = 0; i < g->num_steps; i++)
	{
		CondText (&g->steps[i].done, cond, sizeof(cond));
		Con_Printf ("%c%2d %-4s %s: %s%s%s%s\n", (i == next) ? '>' : ' ', i + 1, done[i] ? "done" : "open",
			    g->steps[i].map, g->steps[i].text, (i < next && !done[i]) ? " (passed)" : "",
			    why[i][0] ? "; " : "", why[i]);
		Con_Printf ("        done %s\n", cond);
	}
	free (done);
	free (why);
}

static void Guide_Check_f (void)
{
	gguide_t	g;

	if (Cmd_Argc () < 2 && rec.active)
	{
		CheckGuide (&rec.guide);
		return;
	}
	if (Cmd_Argc () >= 2)
	{
		if (!ValidName (Cmd_Argv (1)) || !LoadHub (Cmd_Argv (1), &g))
		{
			Con_Printf ("no guide for %s\n", Cmd_Argv (1));
			return;
		}
	}
	else if (!sv.active || !LoadHubOfMap (sv.name, &g))
	{
		Con_Printf ("no guide lists %s (guide_check <hub>)\n", sv.active ? sv.name : "the map: no local game");
		return;
	}
	CheckGuide (&g);
	FreeGuide (&g);
}

void HL_InitGuide (void)
{
	Cmd_AddCommand ("guide_record", Guide_Record_f);
	Cmd_AddCommand ("guide_mark", Guide_Mark_f);
	Cmd_AddCommand ("guide_cond", Guide_Cond_f);
	Cmd_AddCommand ("guide_undo", Guide_Undo_f);
	Cmd_AddCommand ("guide_check", Guide_Check_f);
}
