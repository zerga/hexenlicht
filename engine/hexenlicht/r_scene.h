/* r_scene.h -- the per-frame scene description of the Hexenlicht renderer
 *
 * R_RenderView fills r_scene once per frame from the client state: the
 * camera, every entity to draw, dynamic lights, light style values,
 * particles and the view blend. The 3D renderer reads the scene only, not
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

/* visible entities, static entities and the view model */
#define MAX_SCENE_ENTITIES	(MAX_VISEDICTS + MAX_STATIC_ENTITIES + 1)

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
} scene_dlight_t;

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

	/* light style values, 1.0 = 256: "m" (normal) is 264/256 */
	float		lightstyles[MAX_LIGHTSTYLES];

	/* the particle simulation's active list (linked by ->next); the
	 * particles stay unchanged until the next host frame */
	particle_t	*particles;
	int		num_particles;

	float		blend[4];	/* full-screen color shift (v_blend), rgba 0-1 */
} scene_t;

extern scene_t	r_scene;

void R_InitScene (void);

/* r_light.c: GL's light level on the first-person weapon, into
 * cl.light_level (the server's player light_level: how well monsters see
 * the player, when the Assassin cloaks) */
void R_ViewModelLight (void);

#endif	/* R_SCENE_H */
