/* vk_mapfile.c -- the per-map override file: sky, sun, exposure, lights
 *
 * Story 4.7. A map's calibrated settings and light fixes are in
 * maps/<map>.hlmap: plain text, one command per line, "//" comments (after
 * a space; C-style comments within a line too), the same commands the
 * console takes (numbers within a million):
 *  - the per-map cvars (map_cvars below): 4.6's sky and sun cvars,
 *    r_map_light_scale (every map light's intensity times it) and
 *    r_map_exposure (EV: the fixed exposure 2^it, vk_tonemap.c; added to
 *    tm_exposure_bias with the auto exposure, vk_ubo.c).
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
 * Story 4.8's light editor (vk_lightedit.c) edits the file: it is kept as
 * its lines, and an edit of a light (VK_MapFileSetLight) turns that
 * light's lines into one (where the first was, its trailing comment kept;
 * a comment within the line goes, and colors are written 0-1 unless above
 * 1) and parses the lines again, so the lights are always what the lines
 * say; a line that wouldn't parse puts the old lines back. VK_SaveMapFile
 * writes them (through <file>.tmp) into the running game's folder
 * (<game folder>\maps\<map>.hlmap, which the next load finds first), with
 * the per-map cvars as they are now: their lines rewritten, the others
 * that differ from the defaults added; comments, blank lines and skipped
 * lines stay as they were. A map load or vk_mapfile reload drops unsaved
 * edits and says so.
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
#define MAX_FILE_LINES		65536		/* the rest of a longer file is left out */
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

/* the file's lines as read (without their line ends), then as the editor
 * rewrites them (4.8) */
typedef struct
{
	char		*text;
	int		id;		/* lasts while lines come and go */
} fileline_t;

static fileline_t	*lines;
static int		num_lines, max_lines, next_line_id;
static int		unsaved;	/* editor changes since the file was read or saved */

static struct
{
	char		map[MAX_QPATH];		/* the map's name, "" = none loaded */
	char		path[MAX_OSPATH];	/* the file used, "" = none */
	const char	*where;			/* "game folder", "shipped" */
	char		skipped[MAX_OSPATH];	/* a file found but not used, and why */
	int		lines;			/* commands */
	int		settings;		/* per-map cvar lines */
	int		issues;			/* lines skipped */
	char		issue[MAX_ISSUES][128];
	qboolean	truncated;		/* more than MAX_FILE_LINES lines */
} info;


/* ==========================================================================
 * The lines
 * ========================================================================== */

/* a new line of len characters before line at; its id */
static int InsertLine (int at, const char *text, size_t len)
{
	char	*s = (char *) malloc (len + 1);

	if (!s)
		Sys_Error ("%s: out of memory", __thisfunc__);
	memcpy (s, text, len);
	s[len] = 0;
	if (num_lines == max_lines)
	{
		max_lines = max_lines ? max_lines * 2 : 256;
		lines = (fileline_t *) realloc (lines, max_lines * sizeof(*lines));
		if (!lines)
			Sys_Error ("%s: out of memory", __thisfunc__);
	}
	memmove (&lines[at + 1], &lines[at], (num_lines - at) * sizeof(*lines));
	lines[at].text = s;
	lines[at].id = ++next_line_id;
	num_lines++;
	return lines[at].id;
}

static void RemoveLine (int i)
{
	free (lines[i].text);
	memmove (&lines[i], &lines[i + 1], (num_lines - i - 1) * sizeof(*lines));
	num_lines--;
}

static void FreeLines (void)
{
	while (num_lines)
		free (lines[--num_lines].text);
	free (lines);
	lines = NULL;
	max_lines = 0;
}

static int LineIndex (int id)
{
	int	i;

	for (i = 0; i < num_lines; i++)
	{
		if (lines[i].id == id)
			return i;
	}
	return -1;
}

/* where a comment after the line's last word starts, NULL = none */
static const char *TrailingComment (const char *text)
{
	const char	*p = text, *after = text;

	while ((p = COM_Parse (p)) != NULL && com_token[0])
		after = p;
	while (*after && *after <= ' ')
		after++;
	return (after[0] == '/' && (after[1] == '/' || after[1] == '*')) ? after : NULL;
}

/* the line's text becomes text, its trailing comment kept; false if that
 * is what it was */
static qboolean RewriteLine (int i, const char *text)
{
	const char	*comment = TrailingComment (lines[i].text);
	char		*s;
	size_t		len = strlen (text) + (comment ? strlen (comment) + 1 : 0);

	s = (char *) malloc (len + 1);
	if (!s)
		Sys_Error ("%s: out of memory", __thisfunc__);
	q_snprintf (s, len + 1, "%s%s%s", text, comment ? " " : "", comment ? comment : "");
	if (!strcmp (s, lines[i].text))
	{
		free (s);
		return false;
	}
	free (lines[i].text);
	lines[i].text = s;
	return true;
}

/* the line's first word ("" = none) and its number of words; value: the
 * second word */
static int LineCommand (const char *text, char *cmd, size_t size, char *value, size_t value_size)
{
	const char	*p = COM_Parse (text);
	int		n = 0;

	q_strlcpy (cmd, com_token, size);	/* "" without one */
	value[0] = 0;
	if (!cmd[0])
		return 0;
	for (n = 1; p && (p = COM_Parse (p)) != NULL && com_token[0]; n++)
	{
		if (n == 1)
			q_strlcpy (value, com_token, value_size);
	}
	return n;
}


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
static void ParseLight (char **argv, int argc, int line, int id)
{
	vk_mapedit_t	e;
	vec3_t		at;
	float		f;
	int		i, k;

	memset (&e, 0, sizeof(e));
	e.add = !q_strcasecmp (argv[0], "addlight");
	e.line = line;
	e.id = id;
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

/* one line's command; the per-map cvars are set only when set_cvars (at a
 * load: an edit's parse keeps what the console set since) */
static void ParseLine (const char *text, int line, int id, qboolean set_cvars)
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
		ParseLight (argv, argc, line, id);
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
	if (set_cvars)
		Cvar_SetQuick (map_cvars[c].var, argv[1]);
	info.settings++;
}

/* the lines' commands (the counts and issues start over) */
static void ParseLines (qboolean set_cvars)
{
	int	i;

	info.lines = info.settings = info.issues = 0;
	num_edits = 0;
	for (i = 0; i < num_lines; i++)
	{
		if (strlen (lines[i].text) >= MAX_MAPFILE_LINE)
			Issue (i + 1, "longer than %d characters", MAX_MAPFILE_LINE - 1);
		else
			ParseLine (lines[i].text, i + 1, lines[i].id, set_cvars);
	}
	if (info.truncated)
		Issue (MAX_FILE_LINES + 1, "more than %d lines: the rest left out", MAX_FILE_LINES);
}

/* the file's text into lines (without the line ends) */
static void SplitLines (const char *text)
{
	int	n = 0;

	while (*text)
	{
		const char	*end = strchr (text, '\n');
		size_t		len = end ? (size_t)(end - text) : strlen (text);

		if (++n > MAX_FILE_LINES)
		{
			info.truncated = true;
			break;
		}
		InsertLine (num_lines, text, (len && text[len - 1] == '\r') ? len - 1 : len);
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

	if (unsaved && info.map[0])
		Con_Printf ("%s: %d unsaved light edit%s dropped (vk_editlight save writes them)\n", info.map, unsaved,
			    (unsaved == 1) ? "" : "s");
	unsaved = 0;
	memset (&info, 0, sizeof(info));
	FreeLines ();
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
		SplitLines (text);
		free (text);
	}
	ParseLines (true);
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
	VK_ClearLightEditor ();	/* the old map's light isn't this one's */
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
 * The light editor's (vk_lightedit.c, 4.8)
 * ========================================================================== */

static void MergeEdit (vk_mapedit_t *d, const vk_mapedit_t *s)
{
	d->keys |= s->keys;
	if (s->keys & MAPEDIT_LEVEL)
		d->level = s->level;
	if (s->keys & MAPEDIT_SCALE)
		d->scale = s->scale;
	if (s->keys & MAPEDIT_COLOR)
		VectorCopy (s->color, d->color);
	if (s->keys & MAPEDIT_STYLE)
		d->style = s->style;
	if (s->keys & MAPEDIT_ORIGIN)
		VectorCopy (s->origin, d->origin);
}

static qboolean SameAt (const int *a, const int *b)
{
	return a[0] == b[0] && a[1] == b[1] && a[2] == b[2];
}

/* the light lines of the entity origin at, merged in the order
 * vk_maplights.c applies them; false: there are none */
qboolean VK_MapFileLightEdit (const int *at, vk_mapedit_t *merged)
{
	qboolean	found = false;
	int		j, k;

	memset (merged, 0, sizeof(*merged));
	merged->scale = 1.0f;
	merged->level = VK_DEFAULT_LIGHT_LEVEL;
	for (k = 0; k < 3; k++)
	{
		merged->at[k] = at[k];
		merged->origin[k] = (float)at[k];
	}
	for (j = 0; j < num_edits; j++)
	{
		if (!edits[j].add && SameAt (edits[j].at, at))
		{
			MergeEdit (merged, &edits[j]);
			found = true;
		}
	}
	return found;
}

const vk_mapedit_t *VK_MapFileAddLight (int id)
{
	int	j;

	for (j = 0; j < num_edits; j++)
	{
		if (edits[j].add && edits[j].id == id)
			return &edits[j];
	}
	return NULL;
}

/* a number with digits significant digits */
static void AppendNum (char *buf, size_t size, float f, int digits)
{
	char	n[32];

	q_snprintf (n, sizeof(n), " %.*g", digits, (f == 0.0f) ? 0.0f : f);	/* no -0 */
	q_strlcat (buf, n, size);
}

/* an edit as its line, the changes in PrintEdit's order */
static void FormatEdit (const vk_mapedit_t *e, char *buf, size_t size)
{
	int	k;

	if (e->add)
	{
		q_strlcpy (buf, "addlight", size);
		for (k = 0; k < 3; k++)
			AppendNum (buf, size, e->origin[k], 7);
	}
	else
	{
		q_snprintf (buf, size, "light %d %d %d%s", e->at[0], e->at[1], e->at[2], (e->keys & MAPEDIT_OFF) ? " off" : "");
	}
	if (e->keys & MAPEDIT_LEVEL)
		q_strlcat (buf, va(" level %d", e->level), size);
	if (e->keys & MAPEDIT_SCALE)
	{
		q_strlcat (buf, " scale", size);
		AppendNum (buf, size, e->scale, 6);
	}
	if (e->keys & MAPEDIT_COLOR)
	{
		/* above 1 in 0-255, which ParseLight reads back the same */
		float	m = q_max (e->color[0], q_max (e->color[1], e->color[2]));

		q_strlcat (buf, " color", size);
		for (k = 0; k < 3; k++)
			AppendNum (buf, size, (m > 1.0f) ? e->color[k] * 255.0f : e->color[k], 4);
	}
	if (e->keys & MAPEDIT_STYLE)
		q_strlcat (buf, va(" style %d", e->style), size);
	if ((e->keys & MAPEDIT_ORIGIN) && !e->add)
	{
		q_strlcat (buf, " origin", size);
		for (k = 0; k < 3; k++)
			AppendNum (buf, size, e->origin[k], 7);
	}
}

/* a copy of the lines, to put back when an edit's line doesn't parse */
static fileline_t *CopyLines (void)
{
	fileline_t	*copy = (fileline_t *) malloc (q_max (num_lines, 1) * sizeof(*copy));
	int		i;

	if (!copy)
		Sys_Error ("%s: out of memory", __thisfunc__);
	for (i = 0; i < num_lines; i++)
	{
		size_t	len = strlen (lines[i].text);

		copy[i].text = (char *) malloc (len + 1);
		if (!copy[i].text)
			Sys_Error ("%s: out of memory", __thisfunc__);
		memcpy (copy[i].text, lines[i].text, len + 1);
		copy[i].id = lines[i].id;
	}
	return copy;
}

static void FreeCopy (fileline_t *copy, int count)
{
	while (count)
		free (copy[--count].text);
	free (copy);
}

/* the lines become the copy (which they take over) */
static void RestoreLines (fileline_t *copy, int count)
{
	FreeLines ();
	lines = copy;
	num_lines = max_lines = count;
	if (!max_lines)
	{
		free (lines);
		lines = NULL;
	}
}

/* a light's lines become e: for a map light (e->add false) every light
 * line of e->at becomes one, where the first was (removed with no keys,
 * or remove); an addlight's line e->id is rewritten (a new one at the end
 * when there is none, removed with remove). The lines are parsed again
 * (not their cvars: the console's values stay) and a change of their text
 * counted as unsaved; the caller applies them (VK_ApplyMapEdits). A line
 * that doesn't parse (a number past a million, too long with its comment,
 * past MAX_MAP_EDITS light lines) changes nothing: the lines are put back
 * and -1 returned (the reason printed). Returns the line's id, 0 if it was
 * removed */
int VK_MapFileSetLight (const vk_mapedit_t *e, qboolean remove)
{
	char		text[MAX_MAPFILE_LINE];
	int		ids[MAX_MAP_EDITS];
	int		num_ids = 0, i, j, id = 0, copy_count = num_lines;
	qboolean	changed = false;
	fileline_t	*copy;

	remove |= !e->add && !e->keys;
	if (!remove)
		FormatEdit (e, text, sizeof(text));
	copy = CopyLines ();
	if (e->add)
	{
		i = e->id ? LineIndex (e->id) : -1;
		if (remove)
		{
			if (i >= 0)
			{
				RemoveLine (i);
				changed = true;
			}
		}
		else if (i >= 0)
		{
			changed = RewriteLine (i, text);
			id = e->id;
		}
		else
		{
			id = InsertLine (num_lines, text, strlen (text));
			changed = true;
		}
	}
	else
	{
		for (j = 0; j < num_edits; j++)
		{
			if (!edits[j].add && SameAt (edits[j].at, e->at))
				ids[num_ids++] = edits[j].id;
		}
		for (i = 0; i < num_lines; )
		{
			for (j = 0; j < num_ids && ids[j] != lines[i].id; j++)
				;
			if (j == num_ids)
			{
				i++;
			}
			else if (!remove && !id)
			{
				changed |= RewriteLine (i, text);
				id = lines[i++].id;
			}
			else
			{
				RemoveLine (i);
				changed = true;
			}
		}
		if (!remove && !id)
		{
			id = InsertLine (num_lines, text, strlen (text));
			changed = true;
		}
	}
	ParseLines (false);

	if (!remove)
	{
		for (j = 0; j < num_edits && edits[j].id != id; j++)
			;
		if (j == num_edits)
		{
			char	prefix[32];
			size_t	len;

			q_snprintf (prefix, sizeof(prefix), "line %d: ", LineIndex (id) + 1);
			len = strlen (prefix);
			Con_Printf ("the light's line wouldn't be read, nothing changed: %s\n", text);
			for (i = 0; i < q_min (info.issues, MAX_ISSUES); i++)
			{
				if (!strncmp (info.issue[i], prefix, len))
					Con_Printf ("  %s\n", info.issue[i] + len);
			}
			RestoreLines (copy, copy_count);
			ParseLines (false);
			return -1;
		}
	}
	FreeCopy (copy, copy_count);
	unsaved += changed;
	return id;
}

/* the lines parsed again after a change that left their light lines as
 * they were (a save): the counts vk_maplights.c set stay */
static void ReparseKeepMatched (void)
{
	static int	ids[MAX_MAP_EDITS], matched[MAX_MAP_EDITS];
	int		n = num_edits, i, j;

	for (i = 0; i < n; i++)
	{
		ids[i] = edits[i].id;
		matched[i] = edits[i].matched;
	}
	ParseLines (false);
	for (j = 0; j < num_edits; j++)
	{
		for (i = 0; i < n && ids[i] != edits[j].id; i++)
			;
		if (i < n)
			edits[j].matched = matched[i];
	}
}

/* the per-map cvars' lines get their values now (a line that has it stays
 * as written); one that differs from its default without a line gets one,
 * after the last cvar line (else before the first light line) */
static void SyncCvarLines (void)
{
	char	cmd[64], value[64], text[MAX_MAPFILE_LINE];
	int	c, i, n;

	for (c = 0; c < (int)Q_COUNTOF(map_cvars); c++)
	{
		const cvar_t	*v = map_cvars[c].var;
		qboolean	found = false;
		int		at = -1, first_light = -1;

		if (!v)
			continue;
		q_snprintf (text, sizeof(text), strchr (v->string, ' ') ? "%s \"%s\"" : "%s %s", map_cvars[c].name, v->string);
		for (i = 0; i < num_lines; i++)
		{
			n = LineCommand (lines[i].text, cmd, sizeof(cmd), value, sizeof(value));
			if (!q_strcasecmp (cmd, map_cvars[c].name))
			{
				if (n != 2 || strcmp (value, v->string))
					RewriteLine (i, text);
				found = true;
			}
			if (MapCvar (cmd) >= 0)
				at = i + 1;
			else if (first_light < 0 && (!q_strcasecmp (cmd, "light") || !q_strcasecmp (cmd, "addlight")))
				first_light = i;
		}
		if (found || !strcmp (v->string, map_cvars[c].def))
			continue;
		if (at < 0)
			at = (first_light >= 0) ? first_light : num_lines;
		InsertLine (at, text, strlen (text));
	}
}

/* the lines into <game folder>\maps\<map>.hlmap (the running game's folder:
 * the next load finds it first), the per-map cvars as they are now; a new
 * file starts with a comment. Written to <file>.tmp first, which then
 * replaces the file, so a failed write leaves the old one */
qboolean VK_SaveMapFile (void)
{
	char		name[MAX_QPATH], path[MAX_OSPATH], tmp[MAX_OSPATH], header[160];
	FILE		*f;
	int		err, i;
	qboolean	ok;

	if (!info.map[0])
		return false;
	if (info.truncated)
	{
		Con_Printf ("%s has more than %d lines: not saved (its end would be lost)\n", info.path, MAX_FILE_LINES);
		return false;
	}
	SyncCvarLines ();
	q_snprintf (header, sizeof(header), "// %s: Hexenlicht's per-map settings and light fixes (vk_editlight save)", info.map);
	q_snprintf (name, sizeof(name), "maps/%s" MAPFILE_EXT, info.map);
	FS_MakePath_BUF (FS_USERDIR, &err, path, sizeof(path), name);
	if (err || FS_CreatePath (path))
	{
		Con_Printf ("couldn't make the folder for %s\n", path);
		ReparseKeepMatched ();
		return false;
	}
	q_snprintf (tmp, sizeof(tmp), "%s.tmp", path);
	f = fopen (tmp, "wb");
	if (!f)
	{
		Con_Printf ("couldn't write %s\n", tmp);
		ReparseKeepMatched ();
		return false;
	}
	if (!info.path[0])
		fprintf (f, "%s\r\n", header);
	for (i = 0; i < num_lines; i++)
	{
		fputs (lines[i].text, f);
		fputs ("\r\n", f);
	}
	ok = !ferror (f);
	ok &= (fclose (f) == 0);
	if (ok)
	{
		remove (path);	/* rename doesn't replace a file on Windows */
		ok = (rename (tmp, path) == 0);
		if (!ok)
			Con_Printf ("couldn't rename %s to %s: the lines are in %s\n", tmp, path, tmp);
	}
	else
	{
		Con_Printf ("couldn't write %s\n", tmp);
		remove (tmp);
	}
	if (ok && !info.path[0])
		InsertLine (0, header, strlen (header));
	ReparseKeepMatched ();
	if (!ok)
		return false;
	q_strlcpy (info.path, name, sizeof(info.path));
	info.where = "game folder";
	info.skipped[0] = 0;
	unsaved = 0;
	Con_Printf ("saved %s: %d lines, %d settings, %d light lines\n", path, num_lines, info.settings, num_edits);
	return true;
}

int VK_MapFileUnsaved (void)
{
	return unsaved;
}

/* the line with this id and its number, NULL = none */
const char *VK_MapFileLine (int id, int *number)
{
	int	i = LineIndex (id);

	*number = i + 1;
	return (i >= 0) ? lines[i].text : NULL;
}

const char *VK_MapFileName (void)
{
	static char	name[MAX_QPATH];

	q_snprintf (name, sizeof(name), "maps/%s" MAPFILE_EXT, info.map);
	return name;
}

const char *VK_MapFileUsed (void)
{
	static char	used[MAX_OSPATH + 32];

	if (!info.path[0])
		return "";
	q_snprintf (used, sizeof(used), "%s (%s)", info.path, info.where);
	return used;
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
		VK_ApplyMapEdits (cl.worldmodel);
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
	if (unsaved)
		Con_Printf ("  %d unsaved light edit%s (vk_editlight save)\n", unsaved, (unsaved == 1) ? "" : "s");
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
	unsaved = 0;
	FreeLines ();
	memset (&info, 0, sizeof(info));
}
