/* vk_lightedit.c -- live light editing: markers, the selected light, vk_editlight
 *
 * Story 4.8. The map's lights are edited in the game and saved into the
 * map file (vk_mapfile.c, 4.7's format and vocabulary):
 *  - r_editlights 1 shows a marker at each light within
 *    r_editlights_distance (1024) that the eye sees (a trace through the
 *    world only: models and doors don't hide one), a square of the light's
 *    hue; hollow grey when the map file takes it out, hollow red when it
 *    was moved or added inside solid (dropped, still selectable to move it
 *    out). The selected light is bracketed white, the one "select" would
 *    take grey; a panel at the view's top right shows the selected light.
 *    Drawn in the 2D batch from R_RenderView, under the HUD and console
 *    (nothing is printed in a frame);
 *  - vk_editlight select: the light nearest the crosshair within 10
 *    degrees of it, in sight, within r_editlights_distance; select x y z
 *    by entity origin, as the map file's light lines and vk_lights colors
 *    name lights (an addlight by its own); select none;
 *  - vk_editlight <changes> changes the selected light, the map file's
 *    changes (off, level n, scale f, color r g b, style n, origin x y z)
 *    and the editor's: on (not off), origin eye / origin cursor (the
 *    crosshair's world hit, 8 units out along the surface's normal), move
 *    dx dy dz, level *f and scale *f (times the current value: for key
 *    bindings). Origins are rounded to the unit; a change back to the
 *    map's value (origin, level, style; scale 1) drops the key, so a light
 *    with none left loses its line;
 *  - vk_editlight add <changes>: a light at the eye (or origin cursor), an
 *    addlight line (level 300, white), selected; reset: the selected
 *    light's lines removed (the map's own light; an addlight deleted);
 *    save: vk_mapfile.c's VK_SaveMapFile (the running game's folder, the
 *    per-map cvars as they are now); vk_editlight alone prints the
 *    selected light.
 * Every change rewrites the light's lines in the map file (one line where
 * its first was) and applies the lines again (VK_ApplyMapEdits, the light
 * lists rebuilt): what is shown is what save writes and the next load
 * reads; a line that wouldn't parse (a number past a million) changes
 * nothing. A light is selected by its identity (a lump light by its
 * entity, an addlight by its line), which lasts across edits and a lump
 * light's also across vk_mapfile reload (which gives the lines new ids); a
 * new map clears it. Unsaved edits are dropped at a map load, a reload and
 * quit. Gameplay doesn't change: cl.light_level (4.12) comes from the baked
 * lightmaps.
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

#define SELECT_COS		0.98480775f	/* cos(10 degrees): how far from the crosshair select reaches */
#define CURSOR_REACH		8192.0f
#define CURSOR_OFF		8.0f		/* origin cursor: this far out of the surface */
#define SOLID_SIGHT		32.0f		/* the sight test stops this short of a light inside solid */
#define MARKER			2.0f		/* half a marker's width, 2D units */
#define BRACKET			6.0f		/* half the selection brackets' width */
#define PANEL_CHARS		46		/* the panel's line width */
#define PANEL_LINES		10

static cvar_t	r_editlights = {"r_editlights", "0", CVAR_NONE};
static cvar_t	r_editlights_distance = {"r_editlights_distance", "1024", CVAR_NONE};

/* the selected light: a lump light by its entity, an addlight by its line */
static int	sel_entity = -1;
static int	sel_line_id;


static qboolean WorldReady (void)
{
	return cls.signon == SIGNONS && cl.worldmodel && vk_world.worldmodel == cl.worldmodel &&
	       r_scene.worldmodel == cl.worldmodel;	/* a frame's camera */
}

static void RoundVec (const vec3_t v, vec3_t out)
{
	int	k;

	for (k = 0; k < 3; k++)
		out[k] = floorf (v[k] + 0.5f);
}

static void RoundInt (const vec3_t v, int *out)
{
	int	k;

	for (k = 0; k < 3; k++)
		out[k] = (int)floorf (v[k] + 0.5f);
}

static const vk_editablelight_t *Selected (void)
{
	const vk_editablelight_t	*l;
	int				n, i;

	if (sel_entity < 0 && !sel_line_id)
		return NULL;
	l = VK_EditableLights (&n);
	for (i = 0; i < n; i++)
	{
		if ((sel_entity >= 0) ? (l[i].entity == sel_entity) : (l[i].entity < 0 && l[i].line_id == sel_line_id))
			return &l[i];
	}
	return NULL;
}

static void Select (const vk_editablelight_t *l)
{
	sel_entity = l ? l->entity : -1;
	sel_line_id = (l && l->entity < 0) ? l->line_id : 0;
}

/* does the segment touch a solid leaf of the world? A walk of the BSP (it
 * prints nothing, unlike SV_RecursiveHullCheck with developer 1: this
 * runs inside frames) */
static qboolean HitsSolid (const mnode_t *node, const vec3_t p1, const vec3_t p2)
{
	while (node->contents >= 0)
	{
		float	d1 = DotProduct (p1, node->plane->normal) - node->plane->dist;
		float	d2 = DotProduct (p2, node->plane->normal) - node->plane->dist;
		vec3_t	mid;
		int	side, k;

		if (d1 >= 0.0f && d2 >= 0.0f)
		{
			node = node->children[0];
			continue;
		}
		if (d1 < 0.0f && d2 < 0.0f)
		{
			node = node->children[1];
			continue;
		}
		side = (d1 < 0.0f);
		for (k = 0; k < 3; k++)
			mid[k] = p1[k] + (p2[k] - p1[k]) * (d1 / (d1 - d2));
		if (HitsSolid (node->children[side], p1, mid))
			return true;
		return HitsSolid (node->children[!side], mid, p2);
	}
	return node->contents == CONTENTS_SOLID;
}

/* a world trace from the eye (SV_RecursiveHullCheck on hull 0, as
 * chase.c's; commands only) */
static void TraceWorld (const vec3_t start, const vec3_t end, trace_t *trace)
{
	vec3_t	a, b;

	memset (trace, 0, sizeof(*trace));
	trace->fraction = 1.0f;
	trace->allsolid = true;
	VectorCopy (start, a);
	VectorCopy (end, b);
	VectorCopy (end, trace->endpos);
	SV_RecursiveHullCheck (cl.worldmodel->hulls, cl.worldmodel->hulls[0].firstclipnode, 0, 1, a, b, trace);
}

/* can the eye see the light (the world only)? The segment stops 2 units
 * short of it (a light may sit on a face), SOLID_SIGHT short of one
 * inside solid; with the eye inside solid (noclip) every light is */
static qboolean InSight (const vk_editablelight_t *l)
{
	vec3_t	d, end;
	float	dist, back = l->in_solid ? SOLID_SIGHT : 2.0f;

	if (r_scene.viewcontents == CONTENTS_SOLID)
		return true;
	VectorSubtract (l->origin, r_scene.vieworg, d);
	dist = VectorNormalize (d);
	if (dist <= back)
		return true;
	VectorMA (r_scene.vieworg, dist - back, d, end);
	return !HitsSolid (cl.worldmodel->nodes, r_scene.vieworg, end);
}

/* the light select takes: nearest the crosshair within 10 degrees, in
 * sight, within r_editlights_distance (the nearer on a tie); NULL = none */
static const vk_editablelight_t *PickLight (void)
{
	const vk_editablelight_t	*l, *best = NULL;
	float				best_cos = SELECT_COS, best_dist = 0.0f;
	float				reach = q_max (r_editlights_distance.value, 0.0f);
	int				n, i;

	l = VK_EditableLights (&n);
	for (i = 0; i < n; i++)
	{
		vec3_t	d;
		float	dist, c;

		VectorSubtract (l[i].origin, r_scene.vieworg, d);
		dist = VectorLength (d);
		if (dist > reach)
			continue;
		c = (dist < 1.0f) ? 1.0f : DotProduct (d, r_scene.forward) / dist;
		if (c < best_cos - 1e-5f || (c <= best_cos + 1e-5f && best && dist >= best_dist))
			continue;
		if (!InSight (&l[i]))
			continue;
		best = &l[i];
		best_cos = q_max (c, best_cos);
		best_dist = dist;
	}
	return best;
}

/* where the crosshair points at the world, CURSOR_OFF out of the surface */
static qboolean CursorPoint (vec3_t p)
{
	trace_t	trace;
	vec3_t	end;

	if (r_scene.viewcontents == CONTENTS_SOLID)
		return false;
	VectorMA (r_scene.vieworg, CURSOR_REACH, r_scene.forward, end);
	TraceWorld (r_scene.vieworg, end, &trace);
	if (trace.fraction == 1.0f || trace.allsolid)
		return false;
	VectorMA (trace.endpos, CURSOR_OFF, trace.plane.normal, p);
	return true;
}

/* the lights sharing the lump light's entity origin (a light line changes
 * them all) */
static int SharedOrigin (const vk_editablelight_t *l)
{
	const vk_editablelight_t	*all;
	int				n, i, count = 0, a[3], b[3];

	if (l->entity < 0)
		return 1;
	RoundInt (l->lump_origin, a);
	all = VK_EditableLights (&n);
	for (i = 0; i < n; i++)
	{
		if (all[i].entity < 0)
			continue;
		RoundInt (all[i].lump_origin, b);
		count += (a[0] == b[0] && a[1] == b[1] && a[2] == b[2]);
	}
	return count;
}

/* the style's scale now (vk_light.c's: relative to GL's normal 'm') */
static float StyleScale (int style)
{
	return (style >= 0 && style < MAX_LIGHTSTYLES) ? r_scene.lightstyles[style] * 256.0f / 264.0f : 1.0f;
}


/* ==========================================================================
 * The selected light's description (the panel, vk_editlight)
 * ========================================================================== */

/* the lines about the light into text[], their number */
static int Describe (const vk_editablelight_t *l, char text[PANEL_LINES][64])
{
	int	n = 0, shared;
	vec3_t	d;

	if (!l)
	{
		q_strlcpy (text[n++], "no light selected (vk_editlight select)", sizeof(text[0]));
	}
	else
	{
		q_snprintf (text[n++], sizeof(text[0]), "%s", l->classname);
		if (l->entity >= 0)
			q_snprintf (text[n++], sizeof(text[0]), "light %.0f %.0f %.0f (entity %d)", l->lump_origin[0], l->lump_origin[1],
				    l->lump_origin[2], l->entity);
		else
			q_snprintf (text[n++], sizeof(text[0]), "addlight %.0f %.0f %.0f", l->origin[0], l->origin[1], l->origin[2]);
		q_snprintf (text[n++], sizeof(text[0]), "level %d  scale %g  style %d (%.2f)", l->level, l->scale, l->style,
			    StyleScale (l->style));
		q_snprintf (text[n++], sizeof(text[0]), "color %.3g %.3g %.3g %s", l->srgb[0], l->srgb[1], l->srgb[2],
			    VK_MapLightColorsOn () ? l->color_from : "(white: r_maplight_colors 0)");
		if (l->spot)
			q_snprintf (text[n++], sizeof(text[0]), "spotlight %.0f degrees wide",
				    2.0 * acos (q_min (q_max (l->spot_cos, -1.0f), 1.0f)) * 180.0 / M_PI);
		if (l->entity >= 0 && !VectorCompare (l->origin, l->lump_origin))
			q_snprintf (text[n++], sizeof(text[0]), "moved to %.0f %.0f %.0f", l->origin[0], l->origin[1], l->origin[2]);
		if (l->off)
			q_strlcpy (text[n++], "off: taken out by the map file", sizeof(text[0]));
		else if (l->in_solid)
			q_strlcpy (text[n++], "inside solid: dropped (move it out)", sizeof(text[0]));
		shared = SharedOrigin (l);
		VectorSubtract (l->origin, r_scene.vieworg, d);
		q_snprintf (text[n++], sizeof(text[0]), "%s, %.0f units away", (l->entity < 0) ? "added by the map file" :
			    (shared > 1) ? va("%d lights here: edits change all", shared) : l->edited ? "edited" : "as the map has it",
			    VectorLength (d));
	}
	if (VK_MapFileUnsaved ())
		q_snprintf (text[n++], sizeof(text[0]), "%s: %d unsaved edits", VK_MapFileName (), VK_MapFileUnsaved ());
	else
		q_snprintf (text[n++], sizeof(text[0]), "%s: %s", VK_MapFileName (), VK_MapFileUsed ()[0] ? "no unsaved edits" : "no file");
	return n;
}


/* ==========================================================================
 * Drawing (R_RenderView, the 2D batch: under the HUD and the console)
 * ========================================================================== */

/* the light's point in the 2D screen, false if off the 3D view */
static qboolean Project (const vec3_t p, float *x, float *y)
{
	vec3_t	d;
	float	z, sx, sy;

	VectorSubtract (p, r_scene.vieworg, d);
	z = DotProduct (d, r_scene.forward);
	if (z < 1.0f)
		return false;
	sx = DotProduct (d, r_scene.right) / (z * tanf (r_scene.fov_x * (float)M_PI / 360.0f));
	sy = DotProduct (d, r_scene.up) / (z * tanf (r_scene.fov_y * (float)M_PI / 360.0f));
	if (fabsf (sx) > 1.0f || fabsf (sy) > 1.0f)
		return false;
	*x = r_scene.vrect.x + r_scene.vrect.width * 0.5f * (1.0f + sx);
	*y = r_scene.vrect.y + r_scene.vrect.height * 0.5f * (1.0f - sy);
	return true;
}

static void Outline (float x, float y, float h, const float *rgba)
{
	VK_DrawBox (x - h, y - h, x + h, y - h + 1, rgba);
	VK_DrawBox (x - h, y + h - 1, x + h, y + h, rgba);
	VK_DrawBox (x - h, y - h, x - h + 1, y + h, rgba);
	VK_DrawBox (x + h - 1, y - h, x + h, y + h, rgba);
}

static void Brackets (float x, float y, const float *rgba)
{
	const float	h = BRACKET, s = 3.0f;

	VK_DrawBox (x - h, y - h, x - h + s, y - h + 1, rgba);
	VK_DrawBox (x - h, y - h, x - h + 1, y - h + s, rgba);
	VK_DrawBox (x + h - s, y - h, x + h, y - h + 1, rgba);
	VK_DrawBox (x + h - 1, y - h, x + h, y - h + s, rgba);
	VK_DrawBox (x - h, y + h - 1, x - h + s, y + h, rgba);
	VK_DrawBox (x - h, y + h - s, x - h + 1, y + h, rgba);
	VK_DrawBox (x + h - s, y + h - 1, x + h, y + h, rgba);
	VK_DrawBox (x + h - 1, y + h - s, x + h, y + h, rgba);
}

static void DrawMarker (const vk_editablelight_t *l, float x, float y)
{
	static const float	black[4] = { 0, 0, 0, 1 }, grey[4] = { 0.6f, 0.6f, 0.6f, 1 }, red[4] = { 1, 0.15f, 0.1f, 1 };
	float			c[4] = { 1, 1, 1, 1 };
	int			k;

	VK_DrawBox (x - MARKER - 1, y - MARKER - 1, x + MARKER + 1, y + MARKER + 1, black);
	if (l->off || l->in_solid)
	{
		Outline (x, y, MARKER + 1, l->off ? grey : red);
		return;
	}
	if (VK_MapLightColorsOn ())
	{
		float	m = q_max (l->srgb[0], q_max (l->srgb[1], l->srgb[2]));

		for (k = 0; k < 3; k++)	/* the hue at full brightness */
			c[k] = (m > 0.0f) ? l->srgb[k] / m : 0.0f;
	}
	VK_DrawBox (x - MARKER, y - MARKER, x + MARKER, y + MARKER, c);
}

static void DrawPanel (const vk_editablelight_t *l)
{
	char	text[PANEL_LINES][64];
	int	n = Describe (l, text), i, w = 0, x, y;

	for (i = 0; i < n; i++)
	{
		text[i][PANEL_CHARS] = 0;
		w = q_max (w, (int)strlen (text[i]));
	}
	x = r_scene.vrect.x + r_scene.vrect.width - w * 8 - 8;
	y = r_scene.vrect.y + 40;	/* below the console's notify lines, as vk_profiler.c's table */
	VK_DrawShade (x - 4, y - 4, w * 8 + 8, n * 8 + 8, 0.6f);
	for (i = 0; i < n; i++)
		Draw_String (x, y + i * 8, text[i]);
}

void VK_DrawLightEditor (void)
{
	static const float		white[4] = { 1, 1, 1, 1 }, grey[4] = { 0.6f, 0.6f, 0.6f, 1 };
	const vk_editablelight_t	*l, *sel, *pick;
	float				reach = q_max (r_editlights_distance.value, 0.0f), x, y;
	qboolean			sel_shown = false;
	int				n, i;

	if (!r_editlights.integer || !WorldReady ())
		return;
	sel = Selected ();
	pick = PickLight ();
	l = VK_EditableLights (&n);
	for (i = 0; i < n; i++)
	{
		vec3_t	d;

		VectorSubtract (l[i].origin, r_scene.vieworg, d);
		if (DotProduct (d, d) > reach * reach || !Project (l[i].origin, &x, &y) || !InSight (&l[i]))
			continue;
		DrawMarker (&l[i], x, y);
		if (&l[i] == sel)
		{
			Brackets (x, y, white);
			sel_shown = true;
		}
		else if (&l[i] == pick)
		{
			Brackets (x, y, grey);
		}
	}
	if (sel && !sel_shown && Project (sel->origin, &x, &y))
		Brackets (x, y, white);	/* out of sight or reach: still where it is */
	if (!crosshair.integer)
		Draw_Character (r_scene.vrect.x + r_scene.vrect.width / 2 - 4, r_scene.vrect.y + r_scene.vrect.height / 2 - 4, '+');
	DrawPanel (sel);
}


/* ==========================================================================
 * vk_editlight
 * ========================================================================== */

/* a number, false if it isn't one or isn't within a million (as the map
 * file's) */
static qboolean ArgFloat (const char *s, float *f)
{
	char	*end;
	double	d = strtod (s, &end);

	*f = (float)d;
	return end != s && *end == 0 && d >= -1e6 && d <= 1e6;
}

/* the file's edit of the light: its lines merged, an addlight's line */
static qboolean CurrentEdit (const vk_editablelight_t *l, vk_mapedit_t *e)
{
	const vk_mapedit_t	*a;
	int			at[3];

	if (l->entity >= 0)
	{
		RoundInt (l->lump_origin, at);
		VK_MapFileLightEdit (at, e);
		return true;
	}
	a = VK_MapFileAddLight (l->line_id);
	if (!a)
		return false;
	*e = *a;
	return true;
}

/* where an origin change puts the light: x y z, eye, cursor (the words
 * from argv[*i], which moves past them) */
static qboolean ArgOrigin (int *i, vec3_t p)
{
	int	k;

	if (*i < Cmd_Argc () && !q_strcasecmp (Cmd_Argv (*i), "eye"))
	{
		VectorCopy (r_scene.vieworg, p);
		(*i)++;
		return true;
	}
	if (*i < Cmd_Argc () && !q_strcasecmp (Cmd_Argv (*i), "cursor"))
	{
		(*i)++;
		if (CursorPoint (p))
			return true;
		Con_Printf ("vk_editlight: the crosshair points at no world surface\n");
		return false;
	}
	for (k = 0; k < 3; k++)
	{
		if (*i >= Cmd_Argc () || !ArgFloat (Cmd_Argv (*i), &p[k]))
		{
			Con_Printf ("vk_editlight: origin takes x y z, eye or cursor\n");
			return false;
		}
		(*i)++;
	}
	return true;
}

/* a level or scale: n, or *f times the current value */
static qboolean ArgValue (const char *s, float current, float *f)
{
	if (s[0] == '*')
	{
		if (!ArgFloat (s + 1, f))
			return false;
		*f *= current;
		return true;
	}
	return ArgFloat (s, f);
}

/* the changes from argv[first] onto e (l: the light, NULL for a new one) */
static qboolean ParseChanges (int first, vk_mapedit_t *e, const vk_editablelight_t *l)
{
	int	i = first, k;
	float	f;
	vec3_t	p;

	while (i < Cmd_Argc ())
	{
		const char	*key = Cmd_Argv (i++);
		const char	*arg = (i < Cmd_Argc ()) ? Cmd_Argv (i) : "";

		if (!q_strcasecmp (key, "off"))
		{
			if (e->add)
			{
				Con_Printf ("vk_editlight: an added light has no off (reset removes it)\n");
				return false;
			}
			e->keys |= MAPEDIT_OFF;
		}
		else if (!q_strcasecmp (key, "on"))
		{
			e->keys &= ~MAPEDIT_OFF;
		}
		else if (!q_strcasecmp (key, "level"))
		{
			float	current = (e->keys & MAPEDIT_LEVEL) ? (float)e->level : l ? (float)l->lump_level : (float)e->level;

			if (!ArgValue (arg, current, &f) || f < 0.5f || f > 1e6f)
			{
				Con_Printf ("vk_editlight: level takes a number of at least 1, or *f\n");
				return false;
			}
			e->level = (int)(f + 0.5f);
			e->keys |= MAPEDIT_LEVEL;
			i++;
		}
		else if (!q_strcasecmp (key, "scale"))
		{
			float	current = (e->keys & MAPEDIT_SCALE) ? e->scale : 1.0f;

			if (!ArgValue (arg, current, &f) || f < 0.0f || f > 1e6f)
			{
				Con_Printf ("vk_editlight: scale takes a number of at least 0, or *f\n");
				return false;
			}
			e->scale = f;
			e->keys |= MAPEDIT_SCALE;
			i++;
		}
		else if (!q_strcasecmp (key, "style"))
		{
			if (!ArgFloat (arg, &f) || f < 0.0f || f >= 256.0f || f != floorf (f))
			{
				Con_Printf ("vk_editlight: style takes 0-255\n");
				return false;
			}
			e->style = (int)f;
			e->keys |= MAPEDIT_STYLE;
			i++;
		}
		else if (!q_strcasecmp (key, "color"))
		{
			float	m;

			for (k = 0; k < 3; k++)
			{
				if (i >= Cmd_Argc () || !ArgFloat (Cmd_Argv (i), &p[k]) || p[k] < 0.0f)
				{
					Con_Printf ("vk_editlight: color takes r g b (an 8-bit color, 0-1 or 0-255)\n");
					return false;
				}
				i++;
			}
			/* as the map file's (vk_mapfile.c) */
			m = q_max (p[0], q_max (p[1], p[2]));
			for (k = 0; k < 3; k++)
				e->color[k] = (m > 1.0f) ? p[k] / 255.0f : p[k];
			e->keys |= MAPEDIT_COLOR;
		}
		else if (!q_strcasecmp (key, "origin") || !q_strcasecmp (key, "move"))
		{
			if (!q_strcasecmp (key, "origin"))
			{
				if (!ArgOrigin (&i, p))
					return false;
			}
			else
			{
				vec3_t	d;

				for (k = 0; k < 3; k++)
				{
					if (i >= Cmd_Argc () || !ArgFloat (Cmd_Argv (i), &d[k]))
					{
						Con_Printf ("vk_editlight: move takes dx dy dz\n");
						return false;
					}
					i++;
				}
				VectorAdd (e->origin, d, p);	/* e->origin: where the light is (CurrentEdit) */
			}
			RoundVec (p, e->origin);
			if (e->add)
				RoundInt (e->origin, e->at);
			else
				e->keys |= MAPEDIT_ORIGIN;
		}
		else
		{
			Con_Printf ("vk_editlight: unknown change \"%s\" (off, on, level, scale, color, style, origin, move)\n", key);
			return false;
		}
	}
	return true;
}

/* a change back to the map's value drops the key (an addlight's: level
 * 300, scale 1, style 0) */
static void DropUnchanged (vk_mapedit_t *e, const vk_editablelight_t *l)
{
	int	level = (l && !e->add) ? l->lump_level : VK_DEFAULT_LIGHT_LEVEL;
	int	style = (l && !e->add) ? l->lump_style : 0;

	if ((e->keys & MAPEDIT_LEVEL) && e->level == level)
		e->keys &= ~MAPEDIT_LEVEL;
	if ((e->keys & MAPEDIT_SCALE) && e->scale == 1.0f)
		e->keys &= ~MAPEDIT_SCALE;
	if ((e->keys & MAPEDIT_STYLE) && e->style == style)
		e->keys &= ~MAPEDIT_STYLE;
	if ((e->keys & MAPEDIT_ORIGIN) && l)
	{
		int	a[3], b[3];

		RoundInt (e->origin, a);
		RoundInt (l->lump_origin, b);
		if (a[0] == b[0] && a[1] == b[1] && a[2] == b[2])
			e->keys &= ~MAPEDIT_ORIGIN;
	}
	if (!(e->keys & MAPEDIT_LEVEL))
		e->level = level;
	if (!(e->keys & MAPEDIT_SCALE))
		e->scale = 1.0f;
	if (!(e->keys & MAPEDIT_STYLE))
		e->style = style;
}

/* the light's lines become e (or go), then the lights are built from the
 * lines again */
static void Commit (vk_mapedit_t *e, qboolean remove)
{
	double				start = Sys_DoubleTime ();
	const vk_editablelight_t	*l;
	const char			*text;
	int				id, number;

	id = VK_MapFileSetLight (e, remove);
	VK_ApplyMapEdits (cl.worldmodel);	/* also after a refused line: its lines were parsed again */
	VK_RebuildLights ();
	if (id < 0)
		return;		/* refused, said why */
	if (e->add)
	{
		sel_entity = -1;
		sel_line_id = id;
	}
	text = id ? VK_MapFileLine (id, &number) : NULL;
	if (text)
		Con_Printf ("line %d: %s (%.1f ms)\n", number, text, (Sys_DoubleTime () - start) * 1000.0);
	else if (e->add)
		Con_Printf ("the added light is gone (%.1f ms)\n", (Sys_DoubleTime () - start) * 1000.0);
	else
		Con_Printf ("light %d %d %d: as the map has it, no line (%.1f ms)\n", e->at[0], e->at[1], e->at[2],
			    (Sys_DoubleTime () - start) * 1000.0);
	l = Selected ();
	if (l && l->in_solid)
		Con_Printf ("inside solid: dropped (move it out)\n");
}

static void PrintSelected (void)
{
	char	text[PANEL_LINES][64];
	int	n, i;

	n = Describe (Selected (), text);
	for (i = 0; i < n; i++)
		Con_Printf ("%s\n", text[i]);
}

static void Usage (void)
{
	Con_Printf ("vk_editlight select [x y z | none]   the light at the crosshair, or by its entity origin\n"
		    "vk_editlight <changes>   off, on, level n|*f, scale f|*f, color r g b, style n,\n"
		    "                         origin x y z|eye|cursor, move dx dy dz\n"
		    "vk_editlight add [changes]   a light at the eye (or origin cursor)\n"
		    "vk_editlight reset   the selected light as the map has it (an added one removed)\n"
		    "vk_editlight save    into the game folder's maps/<map>.hlmap\n"
		    "r_editlights 1   markers and the selected light's panel\n");
}

static void VK_EditLight_f (void)
{
	const char			*what = (Cmd_Argc () > 1) ? Cmd_Argv (1) : "";
	const vk_editablelight_t	*l;
	vk_mapedit_t			e;

	if (!q_strcasecmp (what, "help"))
	{
		Usage ();
		return;
	}
	if (!WorldReady ())
	{
		Con_Printf ("vk_editlight: no map\n");
		return;
	}
	if (!what[0])
	{
		PrintSelected ();
		return;
	}
	if (!q_strcasecmp (what, "save"))
	{
		VK_SaveMapFile ();
		return;
	}
	if (!q_strcasecmp (what, "select"))
	{
		if (Cmd_Argc () == 3 && !q_strcasecmp (Cmd_Argv (2), "none"))
		{
			Select (NULL);
		}
		else if (Cmd_Argc () == 5)
		{
			const vk_editablelight_t	*all, *found = NULL;
			vec3_t				v;
			int				n, i, k, p[3], q[3];

			for (k = 0; k < 3; k++)
			{
				if (!ArgFloat (Cmd_Argv (2 + k), &v[k]))
				{
					Con_Printf ("vk_editlight: select takes x y z\n");
					return;
				}
			}
			RoundInt (v, p);
			all = VK_EditableLights (&n);
			for (i = 0; i < n && !found; i++)	/* a lump light first, as a light line names it */
			{
				RoundInt (all[i].lump_origin, q);
				if (all[i].entity >= 0 && p[0] == q[0] && p[1] == q[1] && p[2] == q[2])
					found = &all[i];
			}
			for (i = 0; i < n && !found; i++)
			{
				RoundInt (all[i].origin, q);
				if (all[i].entity < 0 && p[0] == q[0] && p[1] == q[1] && p[2] == q[2])
					found = &all[i];
			}
			if (!found)
			{
				Con_Printf ("vk_editlight: no light entity or addlight at %d %d %d%s\n", p[0], p[1], p[2],
					    VK_MapLightDroppedAt (p) ? " (the compiler lit nothing from the one there)" : "");
				return;
			}
			Select (found);
		}
		else if (Cmd_Argc () == 2)
		{
			l = PickLight ();
			if (!l)
			{
				Select (NULL);
				Con_Printf ("vk_editlight: no light in sight within 10 degrees of the crosshair and %g units\n",
					    r_editlights_distance.value);
				return;
			}
			Select (l);
		}
		else
		{
			Con_Printf ("vk_editlight: select [x y z | none]\n");
			return;
		}
		PrintSelected ();
		return;
	}
	if (!q_strcasecmp (what, "add"))
	{
		memset (&e, 0, sizeof(e));
		e.add = true;
		e.level = VK_DEFAULT_LIGHT_LEVEL;
		e.scale = 1.0f;
		RoundVec (r_scene.vieworg, e.origin);
		if (!ParseChanges (2, &e, NULL))
			return;
		RoundInt (e.origin, e.at);
		DropUnchanged (&e, NULL);
		Commit (&e, false);
		PrintSelected ();
		return;
	}

	l = Selected ();
	if (!l)
	{
		Con_Printf ("vk_editlight: no light selected (vk_editlight select)\n");
		return;
	}
	if (!CurrentEdit (l, &e))
	{
		Con_Printf ("vk_editlight: the added light's line is gone\n");
		return;
	}
	if (!q_strcasecmp (what, "reset"))
	{
		Commit (&e, true);
		if (e.add)
			Select (NULL);
		return;
	}
	if (!e.add && !(e.keys & MAPEDIT_ORIGIN))
		VectorCopy (l->lump_origin, e.origin);
	if (!ParseChanges (1, &e, l))
		return;
	DropUnchanged (&e, l);
	Commit (&e, false);
}


/* ==========================================================================
 * Init
 * ========================================================================== */

void VK_ClearLightEditor (void)
{
	Select (NULL);
}

void VK_InitLightEditor (void)
{
	Cvar_RegisterVariable (&r_editlights);
	Cvar_RegisterVariable (&r_editlights_distance);
	Cmd_AddCommand ("vk_editlight", VK_EditLight_f);
}

void VK_ShutdownLightEditor (void)
{
	Select (NULL);
}
