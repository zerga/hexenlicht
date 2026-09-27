/* vk_mapfile.c -- the per-map override file: sky, sun, exposure, lights
 *
 * Story 4.7. A map's calibrated settings and light fixes are in
 * maps/<map>.hlmap: plain text, one command per line, "//" comments (after
 * a space; C-style comments within a line too), the same commands the
 * console takes (numbers within a million):
 *  - the per-map cvars (map_cvars below): 4.6's sky and sun cvars,
 *    r_map_light_scale (every map light's intensity times it) and
 *    r_map_exposure (EV added to the global tm_exposure_bias, vk_ubo.c).
 *    Every map load resets them to their defaults (their values when the
 *    renderer started) before the map's file sets them, so they don't
 *    carry over from map to map; the global calibration (r_maplight_scale,
 *    the tm_* cvars) isn't per map;
 *  - light x y z <changes>: the map lights whose entity origin is x y z
 *    (to the unit, as vk_maplights.c's VK_MapLightAt); the changes: off,
 *    level n (at least 1: range and intensity, the mapper's key), scale f
 *    (intensity only), color r g b (sRGB, 0-1 or 0-255 as the _color key;
 *    shown with r_maplight_colors 1), style n, origin x y z (moved there;
 *    a spotlight keeps its direction);
 *  - addlight x y z <changes>: a new light there (level 300, white, style
 *    0 unless changed; off and origin make the line skipped).
 * vk_maplights.c applies the light edits whenever it builds the map's
 * lights (VK_MapEdits).
 * Where: first the game's filesystem (maps/<map>.hlmap, loose or in a
 * pak: a player's or a mod's own file), taken only from the map's game
 * folder or one of higher priority, as uHexen2's external .ent files
 * (gl_model.c's Mod_LoadEntities); then the file shipped next to the exe
 * (<exe folder>\maps\, from the repository's data/hexenlicht/maps), only
 * for maps from the game's own folders (data1, portals: path ids 1 and 2),
 * so that a mod's map of the same name doesn't get it. The first one found
 * is used whole. Lines that aren't one of these commands, or are bad, are
 * reported (at map load, and by vk_mapfile) and skipped.
 * vk_mapfile prints the file, its settings and light edits; vk_mapfile
 * reload reads it again and applies it (the per-map cvars reset first),
 * rebuilding the lights.
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

#define MAPFILE_EXT		".hlmap"
#define MAX_MAP_EDITS		1024		/* light and addlight lines */
#define MAX_MAPFILE_LINE	1024
#define MAX_LINE_TOKENS		32
#define MAX_ISSUES		16		/* reported lines kept for vk_mapfile */
#define BASE_PATH_IDS(portals)	((portals) ? 2u : 1u)	/* quakefs.c: data1 1, portals 2, then mods */

static void MapLightScaleChanged (cvar_t *var);

static cvar_t	r_map_light_scale = {"r_map_light_scale", "1", CVAR_NONE};
static cvar_t	r_map_exposure = {"r_map_exposure", "0", CVAR_NONE};

/* the per-map cvars, reset to their defaults at every map load */
static struct
{
	const char	*name;
	cvar_t		*var;
	char		def[64];	/* the value when the renderer started */
} map_cvars[] =
{
	{ "r_sky_light" },
	{ "r_sky_light_scale" },
	{ "r_sun" },
	{ "r_sun_intensity" },
	{ "r_sun_color" },
	{ "r_sun_elevation" },
	{ "r_sun_azimuth" },
	{ "r_sun_angle" },
	{ "r_map_light_scale" },
	{ "r_map_exposure" }
};

static vk_mapedit_t	edits[MAX_MAP_EDITS];
static int		num_edits;
static qboolean		loading;	/* setting the per-map cvars: no light rebuilds from their callbacks */

static struct
{
	char		map[MAX_QPATH];		/* the map's name, "" = none loaded */
	char		path[MAX_OSPATH];	/* the file used, "" = none */
	const char	*where;			/* "game folder", "shipped" */
	char		skipped[MAX_OSPATH];	/* a file found but not used, and why */
	int		lines;			/* commands */
	int		settings;		/* per-map cvars set */
	int		issues;			/* lines skipped */
	char		issue[MAX_ISSUES][128];
} info;


/* ==========================================================================
 * Parsing
 * ========================================================================== */

static void Issue (int line, const char *fmt, ...) FUNC_PRINTF(2,3);
static void Issue (int line, const char *fmt, ...)
{
	va_list	argptr;
	char	msg[112];

	va_start (argptr, fmt);
	q_vsnprintf (msg, sizeof(msg), fmt, argptr);
	va_end (argptr);
	if (info.issues < MAX_ISSUES)
		q_snprintf (info.issue[info.issues], sizeof(info.issue[0]), "line %d: %s", line, msg);
	info.issues++;
}

static int MapCvar (const char *name)
{
	int	i;

	for (i = 0; i < (int)Q_COUNTOF(map_cvars); i++)
	{
		if (!q_strcasecmp (name, map_cvars[i].name))
			return i;
	}
	return -1;
}

/* a number token, false if it isn't one or isn't within a million (no inf,
 * nan, or coordinates and levels an int can't hold) */
static qboolean ParseFloat (const char *s, float *f)
{
	char	*end;
	double	d = strtod (s, &end);

	*f = (float)d;
	return end != s && *end == 0 && d >= -1e6 && d <= 1e6;
}

/* a per-map cvar's value: a number, r_sun_color one to three */
static qboolean ValidValue (const char *name, const char *value)
{
	const char	*p = value;
	float		f;
	int		n = 0;

	if (q_strcasecmp (name, "r_sun_color"))
		return ParseFloat (value, &f);
	while ((p = COM_Parse (p)) != NULL && com_token[0])	/* the line's words are copied already */
	{
		if (++n > 3 || !ParseFloat (com_token, &f))
			return false;
	}
	return n > 0;
}

static qboolean ParseVec (char **argv, int argc, int at, vec3_t v)
{
	int	k;

	if (at + 3 > argc)
		return false;
	for (k = 0; k < 3; k++)
	{
		if (!ParseFloat (argv[at + k], &v[k]))
			return false;
	}
	return true;
}

/* light x y z <changes> / addlight x y z <changes> */
static void ParseLight (char **argv, int argc, int line)
{
	vk_mapedit_t	e;
	vec3_t		at;
	float		f;
	int		i, k;

	memset (&e, 0, sizeof(e));
	e.add = !q_strcasecmp (argv[0], "addlight");
	e.line = line;
	e.scale = 1.0f;
	e.level = VK_DEFAULT_LIGHT_LEVEL;
	if (!ParseVec (argv, argc, 1, at))
	{
		Issue (line, "%s needs an origin x y z", argv[0]);
		return;
	}
	for (k = 0; k < 3; k++)
		e.at[k] = (int)floorf (at[k] + 0.5f);
	VectorCopy (at, e.origin);
	for (i = 4; i < argc; )
	{
		const char	*key = argv[i];

		if (!q_strcasecmp (key, "off") && !e.add)
		{
			e.keys |= MAPEDIT_OFF;
			i++;
		}
		else if (!q_strcasecmp (key, "level") && i + 1 < argc && ParseFloat (argv[i + 1], &f) && f >= 1.0f)	/* 0: not a light in the lump; a range of 0 would be unlimited */
		{
			e.keys |= MAPEDIT_LEVEL;
			e.level = (int)(f + 0.5f);
			i += 2;
		}
		else if (!q_strcasecmp (key, "scale") && i + 1 < argc && ParseFloat (argv[i + 1], &f) && f >= 0.0f)
		{
			e.keys |= MAPEDIT_SCALE;
			e.scale = f;
			i += 2;
		}
		else if (!q_strcasecmp (key, "style") && i + 1 < argc && ParseFloat (argv[i + 1], &f) && f >= 0.0f && f < 256.0f)
		{
			e.keys |= MAPEDIT_STYLE;
			e.style = (int)f;
			i += 2;
		}
		else if (!q_strcasecmp (key, "color") && ParseVec (argv, argc, i + 1, e.color))
		{
			float	m = q_max (e.color[0], q_max (e.color[1], e.color[2]));

			/* as the _color key (vk_maplights.c): 0-1, or 0-255 when above 1 */
			for (k = 0; k < 3; k++)
				e.color[k] = q_max ((m > 1.0f) ? e.color[k] / 255.0f : e.color[k], 0.0f);
			e.keys |= MAPEDIT_COLOR;
			i += 4;
		}
		else if (!q_strcasecmp (key, "origin") && !e.add && ParseVec (argv, argc, i + 1, e.origin))
		{
			e.keys |= MAPEDIT_ORIGIN;
			i += 4;
		}
		else
		{
			Issue (line, "%s: bad or unknown change \"%s\"", argv[0], key);
			return;
		}
	}
	if (!e.add && !e.keys)
	{
		Issue (line, "light without a change");
		return;
	}
	if (num_edits == MAX_MAP_EDITS)
	{
		Issue (line, "more than %d light lines", MAX_MAP_EDITS);
		return;
	}
	edits[num_edits++] = e;
}

/* one line's command */
static void ParseLine (const char *text, int line)
{
	char	tokens[MAX_LINE_TOKENS][64];
	char	*argv[MAX_LINE_TOKENS];
	const char	*p = text;
	int	argc = 0, c;

	while ((p = COM_Parse (p)) != NULL && com_token[0])
	{
		if (argc == MAX_LINE_TOKENS || strlen (com_token) >= sizeof(tokens[0]))
		{
			info.lines++;
			Issue (line, "more than %d words, or a word longer than %d characters",
			       MAX_LINE_TOKENS, (int)sizeof(tokens[0]) - 1);
			return;
		}
		q_strlcpy (tokens[argc], com_token, sizeof(tokens[0]));
		argv[argc] = tokens[argc];
		argc++;
	}
	if (!argc)
		return;		/* empty or a comment */
	info.lines++;
	if (!q_strcasecmp (argv[0], "light") || !q_strcasecmp (argv[0], "addlight"))
	{
		ParseLight (argv, argc, line);
		return;
	}
	c = MapCvar (argv[0]);
	if (c < 0)
	{
		Issue (line, "\"%s\" isn't a per-map setting or light command", argv[0]);
		return;
	}
	if (argc != 2 || !map_cvars[c].var || !ValidValue (argv[0], argv[1]))
	{
		Issue (line, "%s takes one number (a color three, quoted: \"1 0.9 0.8\")", argv[0]);
		return;
	}
	Cvar_SetQuick (map_cvars[c].var, argv[1]);
	info.settings++;
}

static void ParseFile (const char *text)
{
	char	line[MAX_MAPFILE_LINE];
	int	n = 0;

	while (*text)
	{
		const char	*end = strchr (text, '\n');
		size_t		len = end ? (size_t)(end - text) : strlen (text);

		n++;
		if (len >= sizeof(line))
		{
			Issue (n, "longer than %d characters", MAX_MAPFILE_LINE - 1);
		}
		else
		{
			memcpy (line, text, len);
			line[len] = 0;
			ParseLine (line, n);
		}
		text += len + (end ? 1 : 0);
	}
}


/* ==========================================================================
 * Loading
 * ========================================================================== */

/* the file's text (malloc'd), where it came from in info; NULL = none */
static char *FindFile (qmodel_t *worldmodel)
{
	char		name[MAX_QPATH];
	unsigned int	path_id;
	char		*text;
	FILE		*f;
	long		len;

	q_snprintf (name, sizeof(name), "maps/%s" MAPFILE_EXT, info.map);
	len = FS_OpenFile (name, NULL, &path_id);	/* its size, not opened */
	if (len >= 0)
	{
		if (path_id >= worldmodel->path_id)
		{
			/* FS_LoadMallocFile stops the game on an empty file */
			text = (len > 0) ? (char *) FS_LoadMallocFile (name, NULL) : (char *) calloc (1, 1);
			if (!text)
				Sys_Error ("%s: couldn't read %s", __thisfunc__, name);
			q_snprintf (info.path, sizeof(info.path), "%s", name);
			info.where = "game folder";
			return text;
		}
		q_snprintf (info.skipped, sizeof(info.skipped), "%s (a game folder below the map's)", name);
	}

	if (worldmodel->path_id > BASE_PATH_IDS (gameflags & GAME_PORTALS))
	{
		q_strlcat (info.skipped, info.skipped[0] ? "; the shipped ones" : "the shipped ones", sizeof(info.skipped));
		q_strlcat (info.skipped, " (the map isn't the game's own)", sizeof(info.skipped));
		return NULL;
	}
	q_snprintf (name, sizeof(name), "maps\\%s" MAPFILE_EXT, info.map);
	VK_ExePath (name, info.path, sizeof(info.path));
	f = fopen (info.path, "rb");
	if (!f)
	{
		info.path[0] = 0;
		return NULL;
	}
	fseek (f, 0, SEEK_END);
	len = ftell (f);
	fseek (f, 0, SEEK_SET);
	text = (char *) malloc (q_max (len, 0) + 1);
	if (!text || len < 0 || fread (text, 1, (size_t)len, f) != (size_t)len)
	{
		fclose (f);
		free (text);
		Con_Printf ("couldn't read %s\n", info.path);
		info.path[0] = 0;
		return NULL;
	}
	fclose (f);
	text[len] = 0;
	info.where = "shipped";
	return text;
}

/* the per-map cvars to their defaults, then the map's file */
static void LoadFile (qmodel_t *worldmodel)
{
	char	*text;
	int	i;

	memset (&info, 0, sizeof(info));
	num_edits = 0;
	COM_FileBase (worldmodel->name, info.map, sizeof(info.map));

	loading = true;
	for (i = 0; i < (int)Q_COUNTOF(map_cvars); i++)
	{
		if (map_cvars[i].var)
			Cvar_SetQuick (map_cvars[i].var, map_cvars[i].def);
	}
	text = FindFile (worldmodel);
	if (text)
	{
		ParseFile (text);
		free (text);
	}
	loading = false;

	for (i = 0; i < q_min (info.issues, MAX_ISSUES); i++)
		Con_Printf ("%s: %s\n", info.path, info.issue[i]);
	if (info.issues > MAX_ISSUES)
		Con_Printf ("%s: %d more lines skipped\n", info.path, info.issues - MAX_ISSUES);
}

/* a new map (VK_LoadWorld, before its lights) */
void VK_LoadMapFile (qmodel_t *worldmodel)
{
	LoadFile (worldmodel);
}

vk_mapedit_t *VK_MapEdits (int *count)
{
	*count = num_edits;
	return edits;
}

float VK_MapLightScale (void)
{
	return q_max (r_map_light_scale.value, 0.0f);
}

float VK_MapExposure (void)
{
	return r_map_exposure.value;
}


/* ==========================================================================
 * vk_mapfile
 * ========================================================================== */

static void PrintEdit (const vk_mapedit_t *e)
{
	char	changes[160] = "";

	if (e->keys & MAPEDIT_OFF)
		q_strlcat (changes, " off", sizeof(changes));
	if (e->keys & MAPEDIT_LEVEL)
		q_strlcat (changes, va(" level %d", e->level), sizeof(changes));
	if (e->keys & MAPEDIT_SCALE)
		q_strlcat (changes, va(" scale %g", e->scale), sizeof(changes));
	if (e->keys & MAPEDIT_COLOR)
		q_strlcat (changes, va(" color %g %g %g", e->color[0], e->color[1], e->color[2]), sizeof(changes));
	if (e->keys & MAPEDIT_STYLE)
		q_strlcat (changes, va(" style %d", e->style), sizeof(changes));
	if (e->keys & MAPEDIT_ORIGIN)
		q_strlcat (changes, va(" origin %g %g %g", e->origin[0], e->origin[1], e->origin[2]), sizeof(changes));
	if (e->add)
		Con_Printf ("  line %d: addlight %g %g %g%s: %s\n", e->line, e->origin[0], e->origin[1], e->origin[2], changes,
			    e->matched ? "added" : "dropped (inside solid, or no room)");
	else if (e->matched)
		Con_Printf ("  line %d: light %d %d %d%s: %d light%s\n", e->line, e->at[0], e->at[1], e->at[2], changes,
			    e->matched, (e->matched == 1) ? "" : "s");
	else
		Con_Printf ("  line %d: light %d %d %d%s: none (%s)\n", e->line, e->at[0], e->at[1], e->at[2], changes,
			    VK_MapLightDroppedAt (e->at) ? "the compiler lit nothing from the light there: inside solid or unlit"
							 : "no light entity there");
}

static void VK_MapFile_f (void)
{
	int	i;

	if (Cmd_Argc () > 1 && !q_strcasecmp (Cmd_Argv (1), "reload"))
	{
		if (!cl.worldmodel || vk_world.worldmodel != cl.worldmodel)
		{
			Con_Printf ("no map\n");
			return;
		}
		LoadFile (cl.worldmodel);
		VK_LoadMapLights (cl.worldmodel);
		VK_RebuildLights ();
	}
	if (!info.map[0])
	{
		Con_Printf ("no map\n");
		return;
	}
	if (!info.path[0])
		Con_Printf ("%s: no %s file (per-map settings at their defaults)%s%s\n", info.map, MAPFILE_EXT,
			    info.skipped[0] ? "; not used: " : "", info.skipped);
	else
		Con_Printf ("%s: %s (%s): %d commands, %d settings, %d light lines, %d skipped%s%s\n", info.map, info.path,
			    info.where, info.lines, info.settings, num_edits, info.issues,
			    info.skipped[0] ? "; not used: " : "", info.skipped);
	for (i = 0; i < q_min (info.issues, MAX_ISSUES); i++)
		Con_Printf ("  %s\n", info.issue[i]);
	for (i = 0; i < (int)Q_COUNTOF(map_cvars); i++)
	{
		const cvar_t	*v = map_cvars[i].var;

		if (v && strcmp (v->string, map_cvars[i].def))
			Con_Printf ("  %s \"%s\" (default \"%s\")\n", v->name, v->string, map_cvars[i].def);
	}
	for (i = 0; i < num_edits; i++)
		PrintEdit (&edits[i]);
	VK_PrintMapLightEdits ();
}


/* ==========================================================================
 * Init
 * ========================================================================== */

static void MapLightScaleChanged (cvar_t *var)
{
	(void)var;
	if (!loading)
		VK_RebuildLights ();
}

/* after vk_sky.c registered its cvars */
void VK_InitMapFile (void)
{
	int	i;

	Cvar_RegisterVariable (&r_map_light_scale);
	Cvar_RegisterVariable (&r_map_exposure);
	Cvar_SetCallback (&r_map_light_scale, MapLightScaleChanged);
	for (i = 0; i < (int)Q_COUNTOF(map_cvars); i++)
	{
		map_cvars[i].var = Cvar_FindVar (map_cvars[i].name);
		if (!map_cvars[i].var)
			Sys_Error ("%s: %s isn't registered", __thisfunc__, map_cvars[i].name);
		q_strlcpy (map_cvars[i].def, map_cvars[i].var->string, sizeof(map_cvars[i].def));
	}
	Cmd_AddCommand ("vk_mapfile", VK_MapFile_f);
}

void VK_ShutdownMapFile (void)
{
	num_edits = 0;
	memset (&info, 0, sizeof(info));
}
