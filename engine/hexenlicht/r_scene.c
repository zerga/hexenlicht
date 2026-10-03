/* r_scene.c -- per-frame scene description for the Hexenlicht renderer
 *
 * R_RenderView does the renderer-independent part of the GL renderer's
 * frame setup (light style animation, view vectors, view leaf, contents
 * color shift and view blend) and then gathers the frame's scene into
 * r_scene (see r_scene.h; 4.19: a player's lights in the hand). The
 * r_dumpscene command prints it.
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

/* 6.3: the beams cl_tent.c's CL_UpdateTEnts drew last (each client frame,
 * before the renderer runs; it has MAX_SCENE_BEAMS streams) */
static scene_beam_t	client_beams[MAX_SCENE_BEAMS];
static int		num_client_beams;


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
	s->movestep = (kind == SCENE_ENT_DYNAMIC) && e->movestep;

	/* chase-cam pitch adj. by FrikaC, as in the GL renderer */
	if (kind == SCENE_ENT_DYNAMIC && num == cl.viewentity)
		s->angles[0] *= 0.3f;
}

/* the client's model flags before each of the three in CL_RelinkEntities'
 * chain: an entity gets the first of the chain's flags it has */
#define CHAIN_BEFORE_VORP	(EF_GIB | EF_ZOMGIB | EF_BLOODSHOT | EF_TRACER | EF_TRACER2 | EF_ROCKET | EF_FIREBALL | \
				 EF_ACIDBALL | EF_ICE | EF_SPIT | EF_SPELL | EF_GRENADE | EF_TRACER3)
#define CHAIN_BEFORE_MAGIC	(CHAIN_BEFORE_VORP | EF_VORP_MISSILE | EF_SET_STAFF)
#define CHAIN_BEFORE_SCARAB	(CHAIN_BEFORE_MAGIC | EF_MAGICMISSILE | EF_BONESHARD)

/* 4.4: the client's "extra dynamic lights" (gl_extra_dynamic_lights, off
 * as in HoT: in cl_dlights they count in cl.light_level, which the server
 * uses for how well monsters see the player) as the renderer's own, only
 * in the scene: the lights CL_RelinkEntities makes with the option for
 * vorpal missiles, magic missiles and scarabs, which replace the entity's
 * own light (the same key); its flicker from a random stream of their own,
 * so the game's rand() stays as without them */
static void R_AddExtraDynamicLights (void)
{
	static unsigned	seed = 1;
	int		i, j;

	if (gl_extra_dynamic_lights.integer)
		return;	/* the client makes them */
	for (i = 0; i < r_scene.num_entities; i++)
	{
		const scene_entity_t	*e = &r_scene.entities[i];
		int			flags;
		const float		*color;
		scene_dlight_t		*sdl;
		static const float	vorpal[3] = {0.3f, 0.3f, 0.8f}, magic[3] = {0.1f, 0.1f, 0.8f}, scarab[3] = {0.9f, 0.6f, 0.1f};
		static const float	white[3] = {1.0f, 1.0f, 1.0f};

		if (e->kind != SCENE_ENT_DYNAMIC || !e->model || e->num <= 0)
			continue;
		flags = e->model->flags;
		if ((flags & EF_VORP_MISSILE) && !(flags & CHAIN_BEFORE_VORP))
			color = vorpal;
		else if ((flags & EF_MAGICMISSILE) && !(flags & CHAIN_BEFORE_MAGIC))
			color = magic;
		else if ((flags & EF_SCARAB) && !(flags & CHAIN_BEFORE_SCARAB))
			color = scarab;
		else
			continue;
		if (!gl_colored_dynamic_lights.integer)
			color = white;

		for (j = 0; j < r_scene.num_dlights && r_scene.dlights[j].key != e->num; j++)
			;
		if (j == r_scene.num_dlights)
		{
			if (r_scene.num_dlights == MAX_DLIGHTS)
				continue;
			r_scene.num_dlights++;
		}
		sdl = &r_scene.dlights[j];
		memset (sdl, 0, sizeof(*sdl));
		seed = seed * 1664525u + 1013904223u;
		VectorCopy (cl_entities[e->num].origin, sdl->origin);	/* the client's (G6: attached lights at the server position) */
		sdl->radius = 240.0f - (float)((seed >> 16) % 20);
		VectorCopy (color, sdl->color);
		sdl->key = e->num;
		sdl->die = (float)cl.time + 0.01f;
	}
}

/* 4.19: a player's lights in the hand. CL_RelinkEntities places an entity's
 * lights with Quake's offsets (EF_DIMLIGHT, the torch's, at the origin,
 * EF_BRIGHTLIGHT 16 up, a muzzle flash 16 up and 18 ahead), but Hexen II's
 * player origin is at the feet, not mid-body as Quake's: a physical light
 * there lies on the floor's plane and lights the floor almost not at all.
 * The scene's copy of each light keyed to a player (the torch, the
 * Sunstaff's and invincibility's bright light, invisibility's, Portals'
 * spell book; the view entity's and other players') moves to the off hand:
 * HELD_AHEAD ahead and HELD_LEFT to the left (the weapons are on the
 * right), at HELD_HEIGHT of the eye height (cl.viewheight for the view
 * entity, 50 standing, 24 crouched: the light 36 or 17 up; other players
 * aren't known to crouch: 36 up, or 17 where that point is in solid, a
 * crouching player under a low ceiling), turned by the yaw only. The point
 * is inside the player's box, which the game keeps out of walls (another
 * player's crouching in the open is 8 above it). A muzzle flash (the only
 * lights with a minlight) keeps the client's place ahead of the gun and
 * moves to that height only. Dark lights stay (GL's darkening at GL's
 * place, 4.10). Only the view entity's own lights reach the first-person
 * weapon as held ones (in_view_hand, vk_light.c). The client's lights stay
 * where they are: cl.light_level (the server's) and the medium's light
 * (r_light.c) read cl_dlights */
#define HELD_AHEAD		8.0f
#define HELD_LEFT		8.0f
#define HELD_HEIGHT		0.72f	/* of the eye height */
#define PLAYER_EYE_HEIGHT	50.0f	/* the gamecode's view_ofs, standing */
#define PLAYER_CROUCH_EYE	24.0f	/* and crouched */
#define MUZZLE_FLASH_UP		16.0f	/* CL_RelinkEntities' EF_MUZZLEFLASH */

static void R_HoldPlayerLights (void)
{
	int	i;

	for (i = 0; i < r_scene.num_dlights; i++)
	{
		scene_dlight_t	*sdl = &r_scene.dlights[i];
		const entity_t	*ent;
		float		yaw, height;
		vec3_t		p;

		if (sdl->key < 1 || sdl->key > cl.maxclients || sdl->dark)
			continue;
		ent = &cl_entities[sdl->key];
		sdl->in_view_hand = (sdl->key == cl.viewentity);
		if (sdl->in_view_hand)
		{
			yaw = cl.viewangles[YAW];
			height = HELD_HEIGHT * cl.viewheight;
		}
		else
		{
			yaw = ent->angles[YAW];
			height = HELD_HEIGHT * PLAYER_EYE_HEIGHT;
		}
		if (sdl->minlight > 0.0f)
			VectorSet (p, sdl->origin[0], sdl->origin[1], sdl->origin[2] - MUZZLE_FLASH_UP);
		else
		{	/* forward (cos, sin), left (-sin, cos) */
			float	s = sinf (yaw * (float)M_PI / 180.0f), c = cosf (yaw * (float)M_PI / 180.0f);

			VectorSet (p, ent->origin[0] + HELD_AHEAD * c - HELD_LEFT * s, ent->origin[1] + HELD_AHEAD * s + HELD_LEFT * c,
				   ent->origin[2]);
		}
		sdl->origin[0] = p[0];
		sdl->origin[1] = p[1];
		sdl->origin[2] = p[2] + height;
		if (!sdl->in_view_hand && Mod_PointInLeaf (sdl->origin, cl.worldmodel)->contents == CONTENTS_SOLID)
			sdl->origin[2] = p[2] + HELD_HEIGHT * PLAYER_CROUCH_EYE;
		sdl->held = true;
	}
}

/* 6.3: cl_tent.c's CL_UpdateTEnts, before its streams (an upstream hot
 * spot: docs/hexenlicht/UPSTREAM.md) */
void R_ClearBeams (void)
{
	num_client_beams = 0;
}

/* 6.3: CL_UpdateTEnts, each stream it draws this frame, after its source
 * follows the entity it is attached to */
void R_AddBeam (int type, int skin, const vec3_t source, const vec3_t dest, float end_time, qmodel_t *const *models)
{
	scene_beam_t	*b;

	if (num_client_beams >= MAX_SCENE_BEAMS)
		return;
	b = &client_beams[num_client_beams++];
	b->type = type;
	b->skin = skin;
	VectorCopy (source, b->source);
	VectorCopy (dest, b->dest);
	b->end_time = end_time;
	memcpy (b->models, models, sizeof(b->models));
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
		sdl->held = sdl->in_view_hand = false;
	}
	R_AddExtraDynamicLights ();
	R_HoldPlayerLights ();

	r_scene.num_beams = r_drawentities.integer ? num_client_beams : 0;
	memcpy (r_scene.beams, client_beams, r_scene.num_beams * sizeof(client_beams[0]));

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
	R_MediumLight ();		/* r_scene.water_light: the liquid around the camera (r_light.c) */
	VK_UpdateInstances ();		/* the brush and alias model entities, for the GPU */
	VK_UpdateModelGeometry ();	/* the alias models' triangles, in this frame's command buffer */
	VK_UpdateEffects ();		/* the particles' and sprites' triangles */
	VK_BuildTLAS ();		/* the dynamic BLASes, the TLAS and the effects TLAS, in the command buffer */
	VK_RenderView3D ();		/* the UBO, then the view pass into the TAA_OUTPUT render target */
	VK_DrawLightEditor ();		/* r_editlights' markers and panel, in the 2D under the HUD */
	VK_DrawImageFile ();		/* vk_imagefile's picture (5.2), in the 2D under the HUD */

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
	R_ResetMediumLight ();			/* the old map's medium light */
	num_client_beams = 0;			/* the old map's, until CL_UpdateTEnts runs */

	R_ClearParticles ();

	VK_LoadWorld (cl.worldmodel);	/* the world and its submodels on the GPU */
	VK_LoadModels ();		/* the alias models the map precaches */
	VK_ReportMaterialFiles ();	/* 5.3: what was found (outside frames) */
	VK_ClearInstances ();
	VK_ClearEffects ();
	VK_ResetDenoiserHistory ();	/* the images show the old map */
	VK_ResetToneMapping ();		/* and its exposure */
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
	if (s->movestep)
		q_strlcat (extra, " step", sizeof(extra));

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
		Con_Printf ("key %4d org %.0f %.0f %.0f radius %.0f minlight %.0f color %.2f %.2f %.2f%s%s, %.2fs left\n",
				dl->key, dl->origin[0], dl->origin[1], dl->origin[2],
				dl->radius, dl->minlight, dl->color[0], dl->color[1], dl->color[2],
				dl->dark ? " dark" : "", dl->in_view_hand ? " held (the view's)" : dl->held ? " held" : "",
				dl->die - r_scene.time);
	}

	Con_Printf ("beams: %d\n", r_scene.num_beams);	/* 6.3 */
	for (i = 0; i < r_scene.num_beams; i++)
	{
		const scene_beam_t	*b = &r_scene.beams[i];

		Con_Printf ("type %2d %-20s skin %d from %.0f %.0f %.0f to %.0f %.0f %.0f, %.2fs left\n",
				b->type, b->models[0] ? b->models[0]->name : "-", b->skin,
				b->source[0], b->source[1], b->source[2], b->dest[0], b->dest[1], b->dest[2],
				b->end_time - r_scene.time);
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
	if (r_scene.water_light_points)	/* 6.17 */
		Con_Printf ("medium light (the liquid's, / 200): %.3f eased, %.3f this frame over %d points (the eye and the ring's in the liquid)\n",
				r_scene.water_light, r_scene.water_light_now, r_scene.water_light_points);
	else
		Con_Printf ("medium light (/ 200): %.3f, cl.light_level's (not in a liquid)\n", r_scene.water_light);
	Con_Printf ("view blend: %.2f %.2f %.2f %.2f\n",
			r_scene.blend[0], r_scene.blend[1], r_scene.blend[2], r_scene.blend[3]);
}


void R_InitScene (void)
{
	Cvar_RegisterVariable (&r_drawentities);
	Cvar_RegisterVariable (&r_drawviewmodel);

	Cmd_AddCommand ("r_dumpscene", R_DumpScene_f);
}
