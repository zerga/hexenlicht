/* vk_calib.c -- calibration against GL: bookmarks, setpos, averaged screenshots
 *
 * Story 4.9. The same camera in Hexenlicht and the unmodified GL client
 * (glh2) comes from a savegame both load (tools/hexenlicht/calib_shots.ps1):
 *  - vk_setpos x y z [pitch yaw] puts the player there (a local
 *    single-player game), the view turned as given. A "save" on the same
 *    console line keeps the pitch: a load sends the player model's angles
 *    (host_cmd.c's Host_Spawn_f), which the next server frame turns into a
 *    third of the view's pitch (sv_user.c), so the save is made before
 *    that. Angles go through the protocol as bytes (1.4 degrees), the same
 *    in both engines;
 *  - vk_bookmark <name> appends "name map x y z pitch yaw [portals]" for
 *    the player's origin and view to bookmarks.txt in the game folder (and
 *    prints it): the lines of tools/hexenlicht/bookmarks.txt;
 *  - vk_screenshot <name> [frames] writes shots/<name>.tga, the next
 *    frames (1) averaged in linear light (vk_swapchain.c): a still frame's
 *    one-sample noise averages out, without the 100 numbered files of
 *    "screenshot";
 *  - vk_freeze (6.15) freezes the monster the player looks at with the
 *    gamecode's own SnowJob, for good (the ice's checks);
 *  - vk_darkplaces [n] [threshold] (4.10) lists where GL shows the world's
 *    floors black: the leaves with the most floor whose lightmaps are
 *    below the threshold at the light styles now, with a point to go to.
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
#include "q_ctype.h"

#define MAX_SHOT_FRAMES		1024

static qboolean LocalGame (const char *cmd)
{
	if (sv.active && svs.maxclients == 1 && cls.state == ca_connected && cls.signon == SIGNONS)
		return true;
	Con_Printf ("%s: only in a local single-player game\n", cmd);
	return false;
}

/* a number, false if it isn't one */
static qboolean ArgNumber (int i, float *f)
{
	const char	*s = Cmd_Argv (i);
	char		*end;
	double		d = strtod (s, &end);

	*f = (float)d;
	return end != s && *end == 0 && d >= -1e6 && d <= 1e6;
}

/* a name for a file: letters, digits, _ - . (no path) */
static qboolean ValidName (const char *s)
{
	if (!*s || *s == '.')
		return false;
	for ( ; *s; s++)
	{
		if (!q_isalnum (*s) && *s != '_' && *s != '-' && *s != '.')
			return false;
	}
	return true;
}

static void VK_SetPos_f (void)
{
	edict_t	*ent;
	vec3_t	v;
	float	pitch = 0.0f, yaw = 0.0f;
	int	i, argc = Cmd_Argc ();

	if (argc != 4 && argc != 6)
	{
		Con_Printf ("vk_setpos x y z [pitch yaw]: the player there, looking that way\n");
		return;
	}
	for (i = 0; i < 3; i++)
	{
		if (!ArgNumber (1 + i, &v[i]))
		{
			Con_Printf ("vk_setpos: \"%s\" isn't a number\n", Cmd_Argv (1 + i));
			return;
		}
	}
	if (argc == 6 && (!ArgNumber (4, &pitch) || !ArgNumber (5, &yaw)))
	{
		Con_Printf ("vk_setpos: pitch and yaw are numbers\n");
		return;
	}
	if (!LocalGame ("vk_setpos"))
		return;
	ent = EDICT_NUM (1);
	VectorCopy (v, ent->v.origin);
	VectorClear (ent->v.velocity);
	if (argc == 6)
	{
		/* the view's angles in both: a save made now restores them */
		ent->v.angles[0] = ent->v.v_angle[0] = pitch;
		ent->v.angles[1] = ent->v.v_angle[1] = yaw;
		ent->v.angles[2] = ent->v.v_angle[2] = 0.0f;
		ent->v.fixangle = 1;
	}
	SV_LinkEdict (ent, false);
}

dfunction_t *ED_FindFunctioni (const char *fn_name);	/* pr_edict.c; host_cmd.c declares it so too */

/* vk_freeze (6.15): the gamecode's own freeze (icemace.hc's SnowJob, the
 * ice mace's kill) on the monster the player looks at within 1024 units
 * (else the one nearest the view's direction within 20 degrees, seen or
 * not), its freeze_time pushed past any map's end, so that it stays ice
 * (below skill 3 IceCubeThink shatters it 5 s after; something pushing it,
 * obj_push, sets 10 s again); a save keeps it */
static void VK_Freeze_f (void)
{
	edict_t		*player, *ent;
	dfunction_t	*f;
	eval_t		*val, *frozen;
	vec3_t		start, end, forward, right, up;
	trace_t		tr;

	if (!LocalGame ("vk_freeze"))
		return;
	f = ED_FindFunctioni ("SnowJob");
	if (!f)
	{
		Con_Printf ("vk_freeze: the gamecode has no SnowJob\n");
		return;
	}
	player = EDICT_NUM (1);
	VectorAdd (player->v.origin, player->v.view_ofs, start);
	AngleVectors (player->v.v_angle, forward, right, up);
	VectorMA (start, 1024.0f, forward, end);
	tr = SV_Move (start, vec3_origin, vec3_origin, end, MOVE_NORMAL, player);
	ent = tr.ent;
	if (!ent || ent == sv.edicts || !((int)ent->v.flags & FL_MONSTER))
	{	/* else the monster nearest the view's direction, within 20 degrees */
		edict_t	*e;
		float	best = cosf (20.0f * (float)M_PI / 180.0f);
		int	i;

		ent = NULL;
		for (i = 1, e = NEXT_EDICT (sv.edicts); i < sv.num_edicts; i++, e = NEXT_EDICT (e))
		{
			vec3_t	d;
			float	len, c;

			if (e->free || !((int)e->v.flags & FL_MONSTER))
				continue;
			VectorAdd (e->v.mins, e->v.maxs, d);
			VectorMA (e->v.origin, 0.5f, d, d);
			VectorSubtract (d, start, d);
			len = VectorLength (d);
			if (len <= 0.0f || len > 1024.0f)
				continue;
			c = DotProduct (d, forward) / len;
			if (c > best)
			{
				best = c;
				ent = e;
			}
		}
		if (!ent)
		{
			Con_Printf ("vk_freeze: no monster in front\n");
			return;
		}
	}
	frozen = GetEdictFieldValue (ent, "frozen");	/* SnowJob's counter: 50 when frozen */
	if (frozen && frozen->_float > 0.0f)
	{
		Con_Printf ("vk_freeze: %s (entity %d) is frozen already\n", PR_GetString (ent->v.classname), NUM_FOR_EDICT (ent));
		return;
	}
	*sv_globals.time = sv.time;
	*sv_globals.self = EDICT_TO_PROG (player);
	G_INT (OFS_PARM0) = EDICT_TO_PROG (ent);
	G_INT (OFS_PARM1) = EDICT_TO_PROG (player);
	PR_ExecuteProgram (f - pr_functions);
	if (!frozen || frozen->_float <= 0.0f)
	{	/* a boss takes damage instead, a brush model nothing */
		Con_Printf ("vk_freeze: %s (entity %d) didn't freeze\n", PR_GetString (ent->v.classname), NUM_FOR_EDICT (ent));
		return;
	}
	val = GetEdictFieldValue (ent, "freeze_time");
	if (val)
		val->_float = 1.0e9f;
	Con_Printf ("vk_freeze: %s (entity %d)%s\n", PR_GetString (ent->v.classname), NUM_FOR_EDICT (ent),
		    val ? "" : ", no freeze_time: it may shatter");
}

static void VK_Bookmark_f (void)
{
	char		map[MAX_QPATH], line[256], path[MAX_OSPATH];
	const char	*name = Cmd_Argv (1);
	vec3_t		origin;
	FILE		*f;
	int		err;

	if (Cmd_Argc () != 2 || !ValidName (name))
	{
		Con_Printf ("vk_bookmark <name>: adds the player's place and view to bookmarks.txt\n");
		return;
	}
	if (!LocalGame ("vk_bookmark"))
		return;
	VectorCopy (EDICT_NUM (1)->v.origin, origin);
	COM_FileBase (cl.worldmodel->name, map, sizeof(map));
	q_snprintf (line, sizeof(line), "%s %s %.1f %.1f %.1f %.1f %.1f%s", name, map, origin[0], origin[1], origin[2],
		    cl.viewangles[PITCH], cl.viewangles[YAW], (gameflags & GAME_PORTALS) ? " portals" : "");
	FS_MakePath_BUF (FS_USERDIR, &err, path, sizeof(path), "bookmarks.txt");
	f = err ? NULL : fopen (path, "a");
	if (!f)
	{
		Con_Printf ("couldn't write %s\n", path);
		return;
	}
	fprintf (f, "%s\n", line);
	fclose (f);
	Con_Printf ("%s: %s\n", path, line);
}

static void VK_Screenshot_f (void)
{
	char		name[MAX_OSPATH], folder[MAX_OSPATH];
	float		frames = 1.0f;

	if (Cmd_Argc () < 2 || Cmd_Argc () > 3 || !ValidName (Cmd_Argv (1)) ||
	    (Cmd_Argc () == 3 && (!ArgNumber (2, &frames) || frames < 1.0f || frames > MAX_SHOT_FRAMES)))
	{
		Con_Printf ("vk_screenshot <name> [frames]: shots/<name>.tga, the next frames (1-%d) averaged in linear light\n",
			    MAX_SHOT_FRAMES);
		return;
	}
	FS_MakePath_BUF (FS_USERDIR, NULL, folder, sizeof(folder), "shots");
	Sys_mkdir (folder, false);
	q_snprintf (name, sizeof(name), "shots/%s.tga", Cmd_Argv (1));
	VK_RequestScreenshotAverage (name, (int)frames);
}

/* a surface's brightest lightmap texel as GL shows it now, 0-255: each
 * style map's byte times the style's value, >> 7, clipped (gl_rsurf.c's
 * R_BuildLightMap; the samples are RGB, gl_model.c); 0 without samples
 * (GL draws such a surface black) */
static int SurfaceBrightest (const msurface_t *s)
{
	int	size = ((s->extents[0] >> 4) + 1) * ((s->extents[1] >> 4) + 1);
	int	best = 0, i, c, m;

	if (!s->samples)
		return 0;
	for (i = 0; i < size; i++)
	{
		for (c = 0; c < 3; c++)
		{
			int	sum = 0;

			for (m = 0; m < MAXLIGHTMAPS && s->styles[m] != 255; m++)
				sum += s->samples[(m * size + i) * 3 + c] * d_lightstylevalue[s->styles[m]];
			best = q_max (best, q_min (sum >> 7, 255));
		}
	}
	return best;
}

/* a surface's area and middle (the mean of its corners) */
static float SurfaceArea (const qmodel_t *m, const msurface_t *s, vec3_t mid)
{
	vec3_t	p[64], a, b, c;
	float	area = 0;
	int	i, n = q_min (s->numedges, 64);

	VectorClear (mid);
	for (i = 0; i < n; i++)
	{
		int	e = m->surfedges[s->firstedge + i];

		VectorCopy (m->vertexes[(e >= 0) ? m->edges[e].v[0] : m->edges[-e].v[1]].position, p[i]);
		VectorAdd (mid, p[i], mid);
	}
	if (n < 3)
		return 0;
	VectorScale (mid, 1.0f / (float)n, mid);
	for (i = 1; i + 1 < n; i++)
	{
		VectorSubtract (p[i], p[0], a);
		VectorSubtract (p[i + 1], p[0], b);
		CrossProduct (a, b, c);
		area += 0.5f * VectorLength (c);
	}
	return area;
}

/* vk_darkplaces [n] [threshold] (4.10): the world's dark places as GL
 * shows them with the light styles now: floors (facing up, 0.7) whose
 * lightmap texels are all below threshold (8 of 255), or that have none,
 * grouped by the leaf in front of them (only empty and water leaves: not
 * facing into solid, not inside the sky's brushes). Much of that is out of
 * the player's reach (rooftops, ledges under the sky), so in a local game
 * only leaves where the server has an entity with a model (a monster, an
 * item, a puzzle piece; not the lights, markers or brush entities) count: the n (10) with the most dark floor, with their share of the
 * leaf's floor, the entities, and a point 24 units above the first one (a
 * player's origin, for vk_setpos) */
static void VK_DarkPlaces_f (void)
{
	typedef struct { float dark, floor; int ents; char first[32]; vec3_t at; } darkleaf_t;
	qmodel_t	*m = cl.worldmodel;
	darkleaf_t	*leaves;
	edict_t		*e;
	int		n = 10, threshold = 8, i, k, shown = 0, surfaces = 0, dark_leaves = 0, reached = 0;
	float		f, total = 0;

	if (!LocalGame ("vk_darkplaces") || !m)
		return;
	if (!m->lightdata)
	{
		Con_Printf ("vk_darkplaces: %s has no light data (GL draws it fullbright)\n", m->name);
		return;
	}
	if (Cmd_Argc () > 1 && ArgNumber (1, &f))
		n = q_max ((int)f, 1);
	if (Cmd_Argc () > 2 && ArgNumber (2, &f))
		threshold = q_max ((int)f, 1);
	leaves = (darkleaf_t *) calloc (m->numleafs + 1, sizeof(*leaves));
	if (!leaves)
		return;
	for (i = 0; i < m->nummodelsurfaces; i++)
	{
		msurface_t	*s = &m->surfaces[m->firstmodelsurface + i];
		vec3_t		normal, mid, front;
		mleaf_t		*leaf;
		darkleaf_t	*d;
		float		area;

		if (s->flags & (SURF_DRAWSKY | SURF_DRAWTURB))
			continue;	/* not lightmapped */
		VectorCopy (s->plane->normal, normal);
		if (s->flags & SURF_PLANEBACK)
			VectorNegate (normal, normal);
		if (normal[2] < 0.7f)
			continue;
		area = SurfaceArea (m, s, mid);
		VectorMA (mid, 2.0f, normal, front);
		leaf = Mod_PointInLeaf (front, m);
		if (!leaf || (leaf->contents != CONTENTS_EMPTY && leaf->contents != CONTENTS_WATER) || area <= 0)
			continue;
		d = &leaves[leaf - m->leafs];
		d->floor += area;
		if (SurfaceBrightest (s) < threshold)
		{
			dark_leaves += (d->dark == 0);
			d->dark += area;
			total += area;
			surfaces++;
		}
	}
	/* the server's entities with a model in them (not the players') */
	for (i = 1, e = NEXT_EDICT (sv.edicts); i < sv.num_edicts; i++, e = NEXT_EDICT (e))
	{
		const char	*classname, *model;
		vec3_t		origin;
		darkleaf_t	*d;

		if (e->free || i <= svs.maxclients)
			continue;
		classname = PR_GetString (e->v.classname);
		model = PR_GetString (e->v.model);
		if (!classname[0] || !q_strncasecmp (classname, "light", 5) || !model[0] || model[0] == '*')
			continue;
		VectorCopy (e->v.origin, origin);
		d = &leaves[Mod_PointInLeaf (origin, m) - m->leafs];
		if (d->dark <= 0)
			continue;
		if (!d->ents++)
		{
			q_strlcpy (d->first, classname, sizeof(d->first));
			VectorCopy (origin, d->at);
		}
	}
	for (i = 0; i <= m->numleafs; i++)
		reached += (leaves[i].dark > 0 && leaves[i].ents > 0);
	Con_Printf ("dark places in %s (floors below %d of 255 at the styles now): %d surfaces, %.0f square units in %d leaves, %d with entities\n",
		    m->name, threshold, surfaces, total, dark_leaves, reached);
	for (k = 0; k < n; k++)
	{
		int	best = -1;

		for (i = 0; i <= m->numleafs; i++)
		{
			if (leaves[i].dark > 0 && leaves[i].ents > 0 && (best < 0 || leaves[i].dark > leaves[best].dark))
				best = i;
		}
		if (best < 0)
			break;
		Con_Printf ("  leaf %d: %.0f square units dark, %.0f %% of its floor, %d entities (%s), at %.0f %.0f %.0f\n", best,
			    leaves[best].dark, 100.0f * leaves[best].dark / leaves[best].floor, leaves[best].ents, leaves[best].first,
			    leaves[best].at[0], leaves[best].at[1], leaves[best].at[2] + 24.0f);
		leaves[best].dark = 0;
		shown++;
	}
	if (!shown)
		Con_Printf ("  none\n");
	free (leaves);
}

void VK_InitCalib (void)
{
	Cmd_AddCommand ("vk_setpos", VK_SetPos_f);
	Cmd_AddCommand ("vk_bookmark", VK_Bookmark_f);
	Cmd_AddCommand ("vk_screenshot", VK_Screenshot_f);
	Cmd_AddCommand ("vk_darkplaces", VK_DarkPlaces_f);
	Cmd_AddCommand ("vk_freeze", VK_Freeze_f);
}
