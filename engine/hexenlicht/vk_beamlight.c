/* vk_beamlight.c -- beams glow and light the scene (story 6.3)
 *
 * Hexen II's beams are the client's streams (cl_tent.c): every frame
 * CL_UpdateTEnts draws a model segment every 30 units from a stream's
 * source to its dest (the sunstaff two: a core and a translucent sheath)
 * and the sunstaff two balls at the dest, all at GL's fixed light level
 * (abslight 128, about half); GL gives them no light. r_scene.c gets the
 * streams from CL_UpdateTEnts (r_scene.beams, an upstream hot spot).
 *
 * Glow: the beams of light's models (glowing[] below, by name as 6.2's
 * sprites: the sunstaff's, the lightning, the color beam, Famine's beam,
 * the medusa's gaze) emit their skin as 6.2's glowing projectiles do
 * (VK_SKIN_GLOW: its _e, else its albedo, times r_emissive_scale and GL's
 * light level: x16), flagged a light: vk_instance.c puts the opaque ones
 * in the light group (no shadows); translucent ones stay transparent
 * models (6.4: blended, glowing at their opacity), the gaze a cutout. The chain and the ice
 * chunks aren't light: lit by the world.
 *
 * Light: each beam of light is one line light (DYNLIGHT_LINE, a thin
 * cylinder from its source to its dest, light_lists.h's dynlight_line) in
 * vk_effectlight.c's budget, the sunstaff's hit a sphere there. Its power
 * is what its glowing surface shows: per segment, each triangle's area
 * (the mean over the model's poses: the lightning draws its six frames
 * at random, the never-sent sunstaff2 its eight in turn, the other light
 * models have one) times the mean emitted color of the texels it covers (its glow
 * material's emissive texture: a texture pack's beam lights in its
 * colors), averaged on the GPU (texture_average.comp); times the alpha
 * (0.33 translucent, the color beam's texture's), GL's light level and the
 * material's emissive factor (r_emissive_scale), per 30 units. So the beam
 * looks as bright as the light it gives (a Lambertian surface of that
 * radiance emits pi x it x its area). The hit's two balls at their mean
 * scale (GL's 80-95 % and 150-165 %), as a sphere showing that radiance
 * (intensity: its power / 4). A beam's lights don't light what is flagged
 * a light (DYNLIGHT_NOT_ON_LIGHTS: its own glowing parts; also the light
 * models and lava). A stream that runs out of GL's 128 segment entities
 * keeps its whole light (CL_UpdateTEnts hands it over before its
 * segments); the streams after it aren't drawn, and get none. Not the
 * gaze: a cutout casts shadows (the masked group, alpha tested), so its
 * own would block its light; it glows, and lights through bounces only.
 *
 * The powers are made outside frames: at map load for the precached beam
 * models, after the material files change (r_reloadmaterials), and between
 * frames for a model first drawn later (no light that frame).
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

/* cl_tent.c's stream types and layout */
#define TE_STREAM_LIGHTNING_SMALL	24
#define TE_STREAM_CHAIN			25
#define TE_STREAM_SUNSTAFF1		26
#define TE_STREAM_SUNSTAFF2		27
#define TE_STREAM_LIGHTNING		28
#define TE_STREAM_COLORBEAM		29
#define TE_STREAM_ICECHUNKS		30
#define TE_STREAM_GAZE			31
#define TE_STREAM_FAMINE		32
#define SEGMENT_STEP		30.0f	/* a segment every 30 units */
#define BEAM_ABSLIGHT		128	/* GL's light level of every stream entity */
#define TRANSLUCENT_ALPHA	0.33f	/* DRF_TRANSLUCENT's (vk_instance.c) */
#define HIT_SCALE_CORE		0.875f	/* the sunstaff's hit: GL's 80 + (rand() & 15) percent, on average */
#define HIT_SCALE_GLOW		1.575f	/* and 150 + (rand() & 15) */

#define MAX_BEAM_MODELS		64
#define MAX_BEAM_TRIS		1024

/* the beam models that glow (cl_tent.c's): the sunstaff's core, sheath and
 * hit (stsunsf5: TE_STREAM_SUNSTAFF2, which the game never sends), the
 * lightning, the color beam, the medusa's gaze, Famine's beam */
static const char *const glowing[] =
{
	"models/stsunsf1.mdl", "models/stsunsf2.mdl", "models/stsunsf3.mdl", "models/stsunsf4.mdl",
	"models/stsunsf5.mdl", "models/stlghtng.mdl", "models/stltng2.mdl", "models/stclrbm.mdl",
	"models/stmedgaz.mdl", "models/fambeam.mdl"
};

/* a beam model's glowing surface, by model and skin */
enum { POWER_NONE, POWER_PENDING, POWER_KNOWN };
typedef struct
{
	qmodel_t	*model;
	char		name[MAX_QPATH];
	int		skin;
	int		state;		/* POWER_* */
	int		material;	/* its glow material */
	int		emissive;	/* the texture its power was made from */
	float		power[2][3];	/* sum of triangle area x the mean emitted color of its texels, by the 2.2 power, the sRGB curve */
	float		axis_radius;	/* the farthest vertex from its x axis (a segment's axis) */
} beam_model_t;

static beam_model_t	beam_models[MAX_BEAM_MODELS];
static int		num_beam_models;

static struct
{
	int	beams;		/* last frame: beams drawn */
	int	lines;		/* line lights offered */
	int	hits;		/* the sunstaff's hits offered */
	int	unknown;	/* parts without a power yet */
	int	full;		/* parts left out: no room for their model */
	int	made;		/* powers made since startup */
	double	ms;		/* and the time they took */
} stats;


/* is the model one of glowing[]: the answers for the models seen last, by
 * pointer and name (a slot of the model cache can take another model) */
static qboolean GlowingModel (const qmodel_t *model)
{
	static struct
	{
		const qmodel_t	*model;
		char		name[MAX_QPATH];
		qboolean	glows;
	} cache[32];
	static int	next;
	int		i, c;

	for (c = 0; c < (int)Q_COUNTOF(cache); c++)
	{
		if (cache[c].model == model && !strcmp (cache[c].name, model->name))
			return cache[c].glows;
	}
	c = next;
	next = (next + 1) % (int)Q_COUNTOF(cache);
	cache[c].model = model;
	q_strlcpy (cache[c].name, model->name, sizeof(cache[c].name));
	cache[c].glows = false;
	for (i = 0; i < (int)Q_COUNTOF(glowing); i++)
	{
		if (!q_strcasecmp (model->name, glowing[i]))
			cache[c].glows = true;
	}
	return cache[c].glows;
}

/* vk_instance.c: does a drawn entity glow, a beam's part (r_effect_lights,
 * r_emissive_scale above 0): the streams' are temporary entities (the
 * client's own, few models: the name cache above holds them) */
qboolean VK_BeamGlows (const scene_entity_t *e)
{
	return e->kind == SCENE_ENT_TEMP && VK_EffectLightsOn () && VK_EmissiveScale () > 0.0f &&
	       e->model->type == mod_alias && GlowingModel (e->model);
}


/* ==========================================================================
 * The glowing surfaces' powers
 * ========================================================================== */

/* the entry of a model's skin, added (pending) if new; NULL: no room */
static beam_model_t *BeamModel (qmodel_t *model, int skin)
{
	beam_model_t	*b;
	int		i;

	for (i = 0; i < num_beam_models; i++)
	{
		b = &beam_models[i];
		if (b->model == model && b->skin == skin && !strcmp (b->name, model->name))
			return b;
	}
	if (num_beam_models >= MAX_BEAM_MODELS)
		return NULL;
	b = &beam_models[num_beam_models++];
	memset (b, 0, sizeof(*b));
	b->model = model;
	q_strlcpy (b->name, model->name, sizeof(b->name));
	b->skin = skin;
	b->state = POWER_PENDING;
	return b;
}

/* the pending ones (outside frames): their glow materials (made if needed)
 * and the GPU's averages over their triangles' texels */
static void MakePowers (void)
{
	static float	uvs[MAX_BEAM_TRIS][6], areas[MAX_BEAM_TRIS], colors[MAX_BEAM_TRIS][2][4];
	double		t0 = Sys_DoubleTime ();
	int		i, t, c, k, n, made = 0;

	for (i = 0; i < num_beam_models; i++)
	{
		beam_model_t		*b = &beam_models[i];
		const vk_material_t	*m;
		qboolean		premultiply;
		int			alpha_slot;

		if (b->state != POWER_PENDING)
			continue;
		b->state = POWER_KNOWN;
		memset (b->power, 0, sizeof(b->power));
		n = VK_AliasTriangleAreas (b->model, uvs, areas, MAX_BEAM_TRIS, &b->axis_radius);
		if (n <= 0)
			continue;
		b->material = VK_GlowSkinMaterial (b->model, b->skin);
		m = VK_GetMaterial (b->material);
		b->emissive = m->emissive_texture;
		if (!b->emissive)
			continue;
		/* the texels' alpha where the model shows it: its skin's mask
		 * (a transparent model's opacity: the color beam; a cutout's
		 * holes: the gaze) */
		premultiply = (m->flags & VK_MAT_ALPHA) != 0;
		alpha_slot = m->mask_texture;
		VK_TriangleAverages (b->emissive, premultiply ? alpha_slot : 0, premultiply, (const float (*)[6]) uvs, n, colors);
		for (t = 0; t < n; t++)
		{
			for (c = 0; c < 2; c++)
			{
				for (k = 0; k < 3; k++)
					b->power[c][k] += areas[t] * colors[t][c][k];
			}
		}
		made++;
	}
	if (made)
	{
		stats.made += made;
		stats.ms += (Sys_DoubleTime () - t0) * 1000.0;
	}
}

/* every precached beam model of light's skins: at map load (VK_LoadModels)
 * and after the material files change (the GPU idle, outside frames) */
void VK_BeamLightAverages (void)
{
	int	i, s;

	num_beam_models = 0;
	for (i = 1; i < MAX_MODELS && cl.model_precache[i]; i++)
	{
		qmodel_t		*model = cl.model_precache[i];
		const aliashdr_t	*hdr;

		if (model->type != mod_alias || !GlowingModel (model) || !q_strcasecmp (model->name, "models/stmedgaz.mdl"))
			continue;
		hdr = (const aliashdr_t *) Mod_Extradata (model);
		for (s = 0; s < hdr->numskins && s < MAX_SKINS; s++)
			BeamModel (model, s);
	}
	MakePowers ();
}

/* VK_BeginFrame, outside frames: the models first drawn last frame */
void VK_BeamLightsBetweenFrames (void)
{
	MakePowers ();
}

/* D_ClearOpenGLTextures freed texture slots (a map change): the next
 * map's load makes them again */
void VK_BeamLightsPurged (void)
{
	num_beam_models = 0;
}


/* ==========================================================================
 * The frame's beams
 * ========================================================================== */

/* a part of the beam: its model's power (skin skin) times alpha, GL's
 * light level and its material's emissive factor, scaled, into sum;
 * *radius the largest axis radius. False: no power yet */
static qboolean AddPart (qmodel_t *model, int skin, float alpha, float level, float scale, int curve, vec3_t sum, float *radius)
{
	beam_model_t		*b;
	const aliashdr_t	*hdr;
	float			f;
	int			k;

	if (!model || model->type != mod_alias)
		return true;
	hdr = (const aliashdr_t *) Mod_Extradata (model);
	if (skin < 0 || skin >= hdr->numskins)
		skin = 0;
	b = BeamModel (model, skin);
	if (!b)
	{
		stats.full++;
		return false;
	}
	if (b->state != POWER_KNOWN)
	{
		stats.unknown++;
		return false;
	}
	if (!b->emissive)
		return true;
	f = alpha * level * VK_GetMaterial (b->material)->emissive_factor * scale * scale;
	for (k = 0; k < 3; k++)
		sum[k] += b->power[curve][k] * f;
	*radius = q_max (*radius, b->axis_radius * scale);
	return true;
}

/* vk_effects.c, after the sprites (inside the frame): each beam of light's
 * line light, the sunstaff's hits' spheres */
void VK_BeamLights (void)
{
	int	i, curve = VK_ColorsSRGB () ? 1 : 0;

	stats.beams = stats.lines = stats.hits = stats.unknown = stats.full = 0;
	if (!VK_EffectLightsOn () || !(VK_EmissiveScale () > 0.0f))
		return;
	for (i = 0; i < r_scene.num_beams; i++)
	{
		const scene_beam_t	*b = &r_scene.beams[i];
		vec3_t			line = {0, 0, 0}, hit = {0, 0, 0};
		float			line_radius = 0.0f, hit_radius = 0.0f, level = BEAM_ABSLIGHT / 255.0f, alpha = 1.0f;
		qboolean		known = true, ends = false;

		stats.beams++;
		switch (b->type)
		{
		case TE_STREAM_SUNSTAFF1:	/* the core, the translucent sheath */
			if (!AddPart (b->models[0], 0, 1.0f, level, 1.0f, curve, line, &line_radius))
				known = false;
			if (!AddPart (b->models[1], 0, TRANSLUCENT_ALPHA, level, 1.0f, curve, line, &line_radius))
				known = false;
			ends = true;
			break;
		case TE_STREAM_SUNSTAFF2:
			if (!AddPart (b->models[0], 0, 1.0f, level, 1.0f, curve, line, &line_radius))
				known = false;
			ends = true;
			break;
		case TE_STREAM_LIGHTNING:
		case TE_STREAM_LIGHTNING_SMALL:
			if (b->end_time < r_scene.time)
			{	/* its last moments: translucent, GL's level falling (an int, as entity_t's) */
				alpha = TRANSLUCENT_ALPHA;
				level = (float)(int)(BEAM_ABSLIGHT + (b->end_time - r_scene.time) * 192.0f) / 255.0f;
			}
			if (!AddPart (b->models[0], 0, alpha, q_max (level, 0.0f), 1.0f, curve, line, &line_radius))
				known = false;
			break;
		case TE_STREAM_COLORBEAM:	/* its skin the color; alpha its texture's */
			if (!AddPart (b->models[0], b->skin, 1.0f, level, 1.0f, curve, line, &line_radius))
				known = false;
			break;
		case TE_STREAM_FAMINE:
			if (!AddPart (b->models[0], 0, 1.0f, level, 1.0f, curve, line, &line_radius))
				known = false;
			break;
		default:	/* the chain, the ice chunks: not light; the gaze: a cutout (see the top) */
			continue;
		}
		if (known)
		{
			VectorScale (line, 1.0f / SEGMENT_STEP, line);
			stats.lines += VK_BeamLineLight (b->source, b->dest, line_radius, line);
		}
		if (ends)
		{
			known = AddPart (b->models[2], 0, 1.0f, level, HIT_SCALE_CORE, curve, hit, &hit_radius);
			if (!AddPart (b->models[3], 0, TRANSLUCENT_ALPHA, level, HIT_SCALE_GLOW, curve, hit, &hit_radius))
				known = false;
			if (known)
			{
				/* a ball showing that radiance: its power / 4 (pi r^2 of 4 pi r^2) */
				VectorScale (hit, 0.25f, hit);
				stats.hits += VK_BeamEndLight (b->dest, hit_radius, hit);
			}
		}
	}
}

/* vk_lights, vk_effects */
void VK_PrintBeamLights (void)
{
	int	i, known = 0;

	for (i = 0; i < num_beam_models; i++)
		known += (beam_models[i].state == POWER_KNOWN);
	Con_Printf ("beams (6.3): last frame %d drawn, %d line lights and %d sunstaff hits offered, %d parts without a power yet, "
		    "%d without room; %d beam model skins known, %d powers made since startup in %.1f ms\n",
		    stats.beams, stats.lines, stats.hits, stats.unknown, stats.full, known, stats.made, stats.ms);
}
