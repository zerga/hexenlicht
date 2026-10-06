/* r_scene.h -- the per-frame scene description of the Hexenlicht renderer
 *
 * R_RenderView fills r_scene once per frame from the client state: the
 * camera, every entity to draw (6.11: the view entity's own model too,
 * for shadows and reflections), dynamic lights (4.19: a player's lights in
 * the hand), light style values, particles, the beams (6.3) and the view
 * blend. The 3D renderer reads the scene only, not
 * the client structures it was built from, so everything the renderer
 * depends on is gathered in one place.
 *
 * Unlike the GL renderer, the scene is not culled to the view: a path
 * tracer needs geometry and lights behind the camera too. Culling (PVS,
 * instance masks) is up to the renderer.
 *
 * Include after quakedef.h.
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

#ifndef R_SCENE_H
#define R_SCENE_H

/* visible entities, static entities, the view model and the view entity's
 * own model (6.11) */
#define MAX_SCENE_ENTITIES	(MAX_VISEDICTS + MAX_STATIC_ENTITIES + 2)

typedef enum
{
	SCENE_ENT_DYNAMIC,	/* cl_entities[num], sent by the server */
	SCENE_ENT_STATIC,	/* cl_static_entities[num]: torches, decorations */
	SCENE_ENT_TEMP,		/* temporary entity or effect: beams, explosions */
	SCENE_ENT_VIEWMODEL	/* cl.viewent: the first-person weapon */
} scene_entkind_t;

typedef struct scene_entity_s
{
	const entity_t	*ent;		/* source entity, for per-entity caches */
	qmodel_t	*model;		/* never NULL */
	scene_entkind_t	kind;
	int		num;		/* cl_entities / cl_static_entities index, else -1 */
	vec3_t		origin;
	vec3_t		angles;		/* incl. the chase camera's pitch damping */
	int		frame;
	float		syncbase;	/* phase of frame groups */
	int		skinnum;	/* >= 100: gfx/skin<n>.lmp */
	byte		*colormap;	/* != vid.colormap: translated player skin */
	int		scale;		/* percent, 0 = 100 */
	int		drawflags;	/* MLS_*, SCALE_TYPE_*, SCALE_ORIGIN_*, DRF_* */
	int		abslight;	/* light level for MLS_ABSLIGHT */
	int		colorshade;	/* tint (the entity's colormap field), 0 = none */
	int		effects;	/* EF_* */
	qboolean	movestep;	/* dynamic entity that moves in steps (MOVETYPE_STEP): r_lerpmove */
	qboolean	viewer;		/* 6.11: the view entity's own model without the chase camera (dynamic), which only secondary rays see */
} scene_entity_t;

typedef struct
{
	vec3_t		origin;
	float		radius;		/* current radius (decays) */
	float		minlight;
	float		color[3];	/* 1 1 1 unless gl_colored_dynamic_lights */
	qboolean	dark;		/* subtracts light */
	int		key;		/* owning entity number, 0 = none */
	float		die;		/* cl.time when it goes out */
	qboolean	held;		/* 4.19: a player's, moved to the hand (r_scene.c) */
	qboolean	in_view_hand;	/* 4.19: and that player is the view entity: it lights the weapon (vk_light.c) */
} scene_dlight_t;

/* 6.3: a beam the client draws this frame (cl_tent.c's streams: a model
 * segment every 30 units from source to dest), for its light */
#define MAX_SCENE_BEAMS		32	/* cl_tent.c's MAX_STREAMS */

typedef struct
{
	int		type;		/* TE_STREAM_* (cl_tent.c) */
	int		skin;		/* the color beam's color */
	vec3_t		source, dest;
	float		end_time;	/* cl.time it ends; the lightning fades for 0.25 s after it */
	qmodel_t	*models[4];	/* the segments' (0, 1) and the end's (2, 3), NULL = none */
} scene_beam_t;

typedef struct
{
	int		framecount;	/* r_framecount when filled */
	double		time;		/* cl.time */

	/* camera */
	vec3_t		vieworg;
	vec3_t		viewangles;
	vec3_t		forward, right, up;
	float		fov_x, fov_y;	/* degrees */
	vrect_t		vrect;		/* 3D view in vid.width x vid.height units */
	mleaf_t		*viewleaf;
	int		viewcontents;	/* CONTENTS_* at the camera */

	qmodel_t	*worldmodel;

	int		num_entities;
	scene_entity_t	entities[MAX_SCENE_ENTITIES];

	int		num_dlights;
	scene_dlight_t	dlights[MAX_DLIGHTS];

	int		num_beams;
	scene_beam_t	beams[MAX_SCENE_BEAMS];

	/* light style values, 1.0 = 256: "m" (normal) is 264/256 */
	float		lightstyles[MAX_LIGHTSTYLES];

	/* the particle simulation's active list (linked by ->next); the
	 * particles stay unchanged until the next host frame */
	particle_t	*particles;
	int		num_particles;

	float		blend[4];	/* full-screen color shift, rgba 0-1: GL's v_blend, without the contents shift in a liquid (6.6) */
} scene_t;

extern scene_t	r_scene;

void R_InitScene (void);

/* 6.3: cl_tent.c's CL_UpdateTEnts hands over its streams (an upstream hot
 * spot, docs/hexenlicht/UPSTREAM.md); R_BuildScene copies them */
void R_ClearBeams (void);
void R_AddBeam (int type, int skin, const vec3_t source, const vec3_t dest, float end_time, qmodel_t *const *models);

/* r_light.c: GL's light level on the first-person weapon, into
 * cl.light_level (the server's player light_level: how well monsters see
 * the player, when the Assassin cloaks) */
void R_ViewModelLight (void);
/* 6.18: GL's light maps' level at p by light style (the surface straight
 * below, as R_LightPointColor's), for vk_medium.c's light grid: up to
 * MAXLIGHTMAPS styles and their levels at style value 1; returns how many */
int R_LightPointStyles (qmodel_t *model, const vec3_t p, int *styles, float *levels);

#endif	/* R_SCENE_H */
