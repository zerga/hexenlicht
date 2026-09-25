/* r_scene.c -- per-frame scene description for the Hexenlicht renderer
 *
 * R_RenderView does the renderer-independent part of the GL renderer's
 * frame setup (light style animation, view vectors, view leaf, contents
 * color shift and view blend) and then gathers the frame's scene into
 * r_scene (see r_scene.h). The r_dumpscene command prints it.
 *
 * R_AnimateLight is Hammer of Thyrion's, from gl_rlight.c.
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
#include "vk_local.h"

scene_t		r_scene;

/* view state; also read by the client (sound, chase camera, snow) */
refdef_t	r_refdef;
vec3_t		r_origin, vpn, vright, vup;
int		r_framecount;
mleaf_t		*r_viewleaf;
entity_t	r_worldentity;
int		d_lightstylevalue[256];	/* 8.8 fraction of base light value */

/* same cvars as the GL renderer */
cvar_t		r_drawentities = {"r_drawentities", "1", CVAR_NONE};
cvar_t		r_drawviewmodel = {"r_drawviewmodel", "1", CVAR_NONE};

extern particle_t	*active_particles;	/* r_part.c */


/*
==================
R_AnimateLight
==================
*/
void R_AnimateLight (void)
{
	int	i, c, v;
	int	defaultLocus;
	int	locusHz[3];

	defaultLocus = locusHz[0] = (int)(cl.time*10);
	locusHz[1] = (int)(cl.time*20);
	locusHz[2] = (int)(cl.time*30);
	for (i = 0; i < MAX_LIGHTSTYLES; i++)
	{
		if (!cl_lightstyle[i].length)
		{ // No style def
			d_lightstylevalue[i] = 256;
			continue;
		}
		c = cl_lightstyle[i].map[0];
		if (c == '1' || c == '2' || c == '3')
		{ // Explicit anim rate
			if (cl_lightstyle[i].length == 1)
			{ // Bad style def
				d_lightstylevalue[i] = 256;
				continue;
			}
			v = locusHz[c-'1'] % (cl_lightstyle[i].length-1);
			d_lightstylevalue[i] = (cl_lightstyle[i].map[v+1]-'a')*22;
			continue;
		}
		// Default anim rate (10 Hz)
		v = defaultLocus % cl_lightstyle[i].length;
		d_lightstylevalue[i] = (cl_lightstyle[i].map[v]-'a')*22;
	}
}


/* the renderer-independent part of gl_rmain.c's R_SetupFrame */
static void R_SetupFrame (void)
{
	R_AnimateLight ();

	r_framecount++;

	VectorCopy (r_refdef.vieworg, r_origin);
	AngleVectors (r_refdef.viewangles, vpn, vright, vup);

	r_viewleaf = Mod_PointInLeaf (r_origin, cl.worldmodel);

	V_SetContentsColor (r_viewleaf->contents);
	V_CalcBlend ();
}


static void R_AddSceneEntity (entity_t *e, scene_entkind_t kind, int num)
{
	scene_entity_t	*s;

	if (!e->model || r_scene.num_entities >= MAX_SCENE_ENTITIES)
		return;

	s = &r_scene.entities[r_scene.num_entities++];
	s->ent = e;
	s->model = e->model;
	s->kind = kind;
	s->num = num;
	VectorCopy (e->origin, s->origin);
	VectorCopy (e->angles, s->angles);
	s->frame = e->frame;
	s->syncbase = e->syncbase;
	s->skinnum = e->skinnum;
	s->colormap = e->colormap;
	s->scale = e->scale;
	s->drawflags = e->drawflags;
	s->abslight = e->abslight;
	s->colorshade = e->colorshade;
	s->effects = e->effects;

	/* chase-cam pitch adj. by FrikaC, as in the GL renderer */
	if (kind == SCENE_ENT_DYNAMIC && num == cl.viewentity)
		s->angles[0] *= 0.3f;
}

static void R_BuildScene (void)
{
	int		i;
	entity_t	*e;
	dlight_t	*dl;
	scene_dlight_t	*sdl;
	particle_t	*p;

	r_scene.framecount = r_framecount;
	r_scene.time = cl.time;

	VectorCopy (r_refdef.vieworg, r_scene.vieworg);
	VectorCopy (r_refdef.viewangles, r_scene.viewangles);
	VectorCopy (vpn, r_scene.forward);
	VectorCopy (vright, r_scene.right);
	VectorCopy (vup, r_scene.up);
	r_scene.fov_x = r_refdef.fov_x;
	r_scene.fov_y = r_refdef.fov_y;
	r_scene.vrect = r_refdef.vrect;
	r_scene.viewleaf = r_viewleaf;
	r_scene.viewcontents = r_viewleaf->contents;

	r_scene.worldmodel = cl.worldmodel;

	/* entities: what the client linked this frame (the server culled
	 * them to the player's PVS), all static entities, the view model */
	r_scene.num_entities = 0;
	if (r_drawentities.integer)
	{
		for (i = 0; i < cl_numvisedicts; i++)
		{
			e = cl_visedicts[i];
			if (e >= cl_entities && e < cl_entities + MAX_EDICTS)
				R_AddSceneEntity (e, SCENE_ENT_DYNAMIC, (int)(e - cl_entities));
			else
				R_AddSceneEntity (e, SCENE_ENT_TEMP, -1);
		}

		for (i = 0; i < cl.num_statics; i++)
			R_AddSceneEntity (&cl_static_entities[i], SCENE_ENT_STATIC, i);

		/* the same conditions as gl_rmain.c's R_DrawViewModel */
		if (cl.v.health > 0 && !chase_active.integer && r_drawviewmodel.integer)
			R_AddSceneEntity (&cl.viewent, SCENE_ENT_VIEWMODEL, -1);
	}

	r_scene.num_dlights = 0;
	for (i = 0, dl = cl_dlights; i < MAX_DLIGHTS; i++, dl++)
	{
		if (dl->die < cl.time || !dl->radius)
			continue;
		sdl = &r_scene.dlights[r_scene.num_dlights++];
		VectorCopy (dl->origin, sdl->origin);
		sdl->radius = dl->radius;
		sdl->minlight = dl->minlight;
		VectorCopy (dl->color, sdl->color);
		sdl->dark = dl->dark;
		sdl->key = dl->key;
		sdl->die = dl->die;
	}

	for (i = 0; i < MAX_LIGHTSTYLES; i++)
		r_scene.lightstyles[i] = d_lightstylevalue[i] / 256.0f;

	r_scene.particles = active_particles;
	r_scene.num_particles = 0;
	for (p = active_particles; p; p = p->next)
		r_scene.num_particles++;

	memcpy (r_scene.blend, v_blend, sizeof(r_scene.blend));
}


/*
================
R_RenderView

r_refdef must be set before the first call
================
*/
void R_RenderView (void)
{
	if (!r_worldentity.model || !cl.worldmodel)
		Sys_Error ("%s: NULL worldmodel", __thisfunc__);

	R_SetupFrame ();
	R_ViewModelLight ();		/* cl.light_level, for the game (r_light.c) */
	R_BuildScene ();
	VK_UpdateInstances ();		/* the brush and alias model entities, for the GPU */
	VK_UpdateModelGeometry ();	/* the alias models' triangles, in this frame's command buffer */
	VK_UpdateEffects ();		/* the particles' and sprites' triangles */
	VK_BuildTLAS ();		/* the dynamic BLASes, the TLAS and the effects TLAS, in the command buffer */
	VK_RenderView3D ();		/* the view pass into the view image */

	/* r_debugview's pass for now; the path tracer comes with epic E3 */
}

/* The GL renderer marks the surfaces each dynamic light touches for
 * lightmap updates. Here the dynamic lights reach the renderer through
 * the scene, so there is nothing to do. */
void R_PushDlights (void)
{
}

/* the renderer-independent parts of gl_rmisc.c's R_NewMap */
void R_NewMap (void)
{
	int		i;

	for (i = 0; i < 256; i++)
		d_lightstylevalue[i] = 264;	/* normal light value */

	memset (&r_worldentity, 0, sizeof(r_worldentity));
	r_worldentity.model = cl.worldmodel;

	/* clear out efrags in case the level hasn't been reloaded */
	for (i = 0; i < cl.worldmodel->numleafs; i++)
		cl.worldmodel->leafs[i].efrags = NULL;

	r_viewleaf = NULL;
	memset (&r_scene, 0, sizeof(r_scene));	/* no scene until the first frame */

	R_ClearParticles ();

	VK_LoadWorld (cl.worldmodel);	/* the world and its submodels on the GPU */
	VK_LoadModels ();		/* the alias models the map precaches */
	VK_ClearInstances ();
	VK_ClearEffects ();
}


/*
=============================================================================

r_dumpscene: print the last frame's scene

=============================================================================
*/

static const char *ContentsName (int contents)
{
	switch (contents)
	{
	case CONTENTS_EMPTY:	return "empty";
	case CONTENTS_SOLID:	return "solid";
	case CONTENTS_WATER:	return "water";
	case CONTENTS_SLIME:	return "slime";
	case CONTENTS_LAVA:	return "lava";
	case CONTENTS_SKY:	return "sky";
	default:		return "other";
	}
}

/* drawflags as words, e.g. " mls=torch translucent" */
static const char *DrawflagsString (int drawflags)
{
	static const char *const mls_names[8] =
	{
		NULL, "fullbright", "powermode", "torch", "totaldark", "5", "6", "abslight"
	};
	static char	buf[80];
	const char	*mls = mls_names[drawflags & MLS_MASKIN];

	buf[0] = '\0';
	if (mls)
		q_strlcat (buf, va(" mls=%s", mls), sizeof(buf));
	if ((drawflags & SCALE_TYPE_MASKIN) == SCALE_TYPE_XYONLY)
		q_strlcat (buf, " scale=xy", sizeof(buf));
	else if ((drawflags & SCALE_TYPE_MASKIN) == SCALE_TYPE_ZONLY)
		q_strlcat (buf, " scale=z", sizeof(buf));
	if ((drawflags & SCALE_ORIGIN_MASKIN) == SCALE_ORIGIN_BOTTOM)
		q_strlcat (buf, " origin=bottom", sizeof(buf));
	else if ((drawflags & SCALE_ORIGIN_MASKIN) == SCALE_ORIGIN_TOP)
		q_strlcat (buf, " origin=top", sizeof(buf));
	if (drawflags & DRF_TRANSLUCENT)
		q_strlcat (buf, " translucent", sizeof(buf));
	if (drawflags & DRF_ANIMATEONCE)
		q_strlcat (buf, " animateonce", sizeof(buf));
	return buf;
}

static void DumpEntity (const scene_entity_t *s)
{
	static const char *const kind_names[] = { "dyn", "static", "temp", "view" };
	char	extra[160];

	extra[0] = '\0';
	if (s->scale && s->scale != 100)
		q_strlcat (extra, va(" scale %d%%", s->scale), sizeof(extra));
	q_strlcat (extra, DrawflagsString (s->drawflags), sizeof(extra));
	if ((s->drawflags & MLS_MASKIN) == MLS_ABSLIGHT)
		q_strlcat (extra, va(" abslight %d", s->abslight), sizeof(extra));
	if (s->effects)
		q_strlcat (extra, va(" effects 0x%x", s->effects), sizeof(extra));
	if (s->colormap && s->colormap != vid.colormap)
		q_strlcat (extra, " colormap", sizeof(extra));
	if (s->colorshade)
		q_strlcat (extra, va(" colorshade %d", s->colorshade), sizeof(extra));

	Con_Printf ("%-6s %4d %-24s frame %3d skin %3d org %.0f %.0f %.0f ang %.0f %.0f %.0f%s\n",
			kind_names[s->kind], s->num, s->model->name, s->frame, s->skinnum,
			s->origin[0], s->origin[1], s->origin[2],
			s->angles[0], s->angles[1], s->angles[2], extra);
}

static void R_DumpScene_f (void)
{
	int		i, counts[4] = { 0, 0, 0, 0 };
	const scene_dlight_t	*dl;

	if (cls.signon != SIGNONS || !r_scene.worldmodel)
	{
		Con_Printf ("No scene: not in a map\n");
		return;
	}

	Con_Printf ("Scene of frame %d, time %.2f, world %s\n",
			r_scene.framecount, r_scene.time, r_scene.worldmodel->name);
	Con_Printf ("camera: org %.1f %.1f %.1f ang %.1f %.1f %.1f fov %.1f x %.1f\n",
			r_scene.vieworg[0], r_scene.vieworg[1], r_scene.vieworg[2],
			r_scene.viewangles[0], r_scene.viewangles[1], r_scene.viewangles[2],
			r_scene.fov_x, r_scene.fov_y);
	Con_Printf ("        view %d,%d %dx%d, leaf %d (%s)\n",
			r_scene.vrect.x, r_scene.vrect.y, r_scene.vrect.width, r_scene.vrect.height,
			(int)(r_scene.viewleaf - r_scene.worldmodel->leafs),
			ContentsName (r_scene.viewcontents));

	for (i = 0; i < r_scene.num_entities; i++)
		counts[r_scene.entities[i].kind]++;
	Con_Printf ("entities: %d (%d dynamic, %d static, %d temp, %d view model)\n",
			r_scene.num_entities, counts[SCENE_ENT_DYNAMIC], counts[SCENE_ENT_STATIC],
			counts[SCENE_ENT_TEMP], counts[SCENE_ENT_VIEWMODEL]);
	for (i = 0; i < r_scene.num_entities; i++)
		DumpEntity (&r_scene.entities[i]);

	Con_Printf ("dlights: %d\n", r_scene.num_dlights);
	for (i = 0, dl = r_scene.dlights; i < r_scene.num_dlights; i++, dl++)
	{
		Con_Printf ("key %4d org %.0f %.0f %.0f radius %.0f minlight %.0f color %.2f %.2f %.2f%s, %.2fs left\n",
				dl->key, dl->origin[0], dl->origin[1], dl->origin[2],
				dl->radius, dl->minlight, dl->color[0], dl->color[1], dl->color[2],
				dl->dark ? " dark" : "", dl->die - r_scene.time);
	}

	/* the style strings are the client's, printed for reference */
	Con_Printf ("light styles (defined):\n");
	for (i = 0; i < MAX_LIGHTSTYLES; i++)
	{
		if (cl_lightstyle[i].length)
			Con_Printf ("%2d %4.2f \"%s\"\n", i, r_scene.lightstyles[i], cl_lightstyle[i].map);
	}

	Con_Printf ("particles: %d\n", r_scene.num_particles);
	Con_Printf ("light level on the weapon (cl.light_level, sent to the server): %d\n", cl.light_level);
	Con_Printf ("view blend: %.2f %.2f %.2f %.2f\n",
			r_scene.blend[0], r_scene.blend[1], r_scene.blend[2], r_scene.blend[3]);
}


void R_InitScene (void)
{
	Cvar_RegisterVariable (&r_drawentities);
	Cvar_RegisterVariable (&r_drawviewmodel);

	Cmd_AddCommand ("r_dumpscene", R_DumpScene_f);
}
