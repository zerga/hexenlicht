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
 * 6.8: the fire missiles that own no light (fire_missiles[] below:
 * Praevus's sucwp1p, the blood rain, the pentacles' spit and the tomed fire
 * storm's flame balls; Hexen II's flaming arrows of the tomed crossbow and
 * the fallen angel's spell) glow as 6.2's glowing projectiles (the whole
 * skin, x16 at their fire flicker or abslight 0.5) in the light group, and
 * each is a light in the same budget of the power its glowing surface shows
 * in the pose drawn (the blood missile's five poses grow): a line light
 * along the model's x axis, as a beam's, where the pose is long (the blood
 * missile is a streak 90 units long and 5 across, its origin at the head;
 * the flaming arrow 31 by 11), else a sphere showing that radiance at the
 * axis' middle, as the sunstaff's hit (intensity: power / 4; the fallen
 * angel's spell, 8 by 8), of the radius of a sphere of its surface's
 * area. GL gives them none (Hammer of Thyrion's 2D missile glows aside).
 * A missile owning a dynamic light is 6.2's (vk_instance.c's GlowKey); its
 * light is its own.
 *
 * The powers are made outside frames: at map load for the precached beam
 * and fire missile models, after the material files change
 * (r_reloadmaterials), and between frames for a model first drawn later
 * (no light that frame).
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
#define MAX_MISSILE_POSES	32	/* a fire missile's poses with a power of their own; later ones take the last's */

/* the beam models that glow (cl_tent.c's): the sunstaff's core, sheath and
 * hit (stsunsf5: TE_STREAM_SUNSTAFF2, which the game never sends), the
 * lightning, the color beam, the medusa's gaze, Famine's beam */
static const char *const glowing[] =
{
	"models/stsunsf1.mdl", "models/stsunsf2.mdl", "models/stsunsf3.mdl", "models/stsunsf4.mdl",
	"models/stsunsf5.mdl", "models/stlghtng.mdl", "models/stltng2.mdl", "models/stclrbm.mdl",
	"models/stmedgaz.mdl", "models/fambeam.mdl"
};

/* 6.8: the fire missiles that own no light (fire flicker or abslight, no
 * light flag): Praevus's blood rain, pentacles' spit and flame balls, the
 * tomed crossbow's flaming arrows, the fallen angel's spell */
static const char *const fire_missiles[] =
{
	"models/sucwp1p.mdl", "models/flaming.mdl", "models/faspell.mdl"
};

/* a glowing model's surface, by model and skin */
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
	/* 6.8: a fire missile's, per pose (later ones take the last's) */
	int		num_poses;
	float		pose_power[MAX_MISSILE_POSES][2][3];
	float		pose_area[MAX_MISSILE_POSES];	/* its surface's area */
	vec3_t		pose_axis[MAX_MISSILE_POSES][2];	/* its x extent through its y z center, in model space */
	float		pose_radius[MAX_MISSILE_POSES];	/* the farthest vertex from that axis */
} beam_model_t;

static beam_model_t	beam_models[MAX_BEAM_MODELS];
static int		num_beam_models;

/* 6.8: this frame's glowing fire missiles (vk_instance.c), whose lights
 * VK_BeamLights offers */
typedef struct
{
	qmodel_t	*model;
	int		skin, pose;
	float		transform[4][4];	/* model to world, as drawn ([column][row], a mat4) */
	float		scale;		/* the entity's */
	float		level;		/* GL's light level, 1 lit by the world */
} fire_missile_t;

static fire_missile_t	missiles[MAX_FIRE_MISSILES];
static int		num_missiles;

static struct
{
	int	beams;		/* last frame: beams drawn */
	int	lines;		/* line lights offered */
	int	hits;		/* the sunstaff's hits offered */
	int	unknown;	/* parts without a power yet */
	int	full;		/* parts left out: no room for their model */
	int	missiles;	/* 6.8: fire missiles drawn glowing */
	int	missiles_over;	/* glowing past MAX_FIRE_MISSILES: no light */
	int	missiles_unknown;	/* drawn without a power yet */
	int	missiles_full;	/* left out: no room for their model */
	int	made;		/* powers made since startup */
	double	ms;		/* and the time they took */
} stats;


/* which glowing models (none, glowing[], fire_missiles[]) is the model: the
 * answers for the models seen last, by pointer and name (a slot of the
 * model cache can take another model) */
enum { GLOW_NONE, GLOW_BEAM, GLOW_MISSILE };

static int GlowKind (const qmodel_t *model)
{
	static struct
	{
		const qmodel_t	*model;
		char		name[MAX_QPATH];
		int		kind;
	} cache[32];
	static int	next;
	int		i, c;

	for (c = 0; c < (int)Q_COUNTOF(cache); c++)
	{
		if (cache[c].model == model && !strcmp (cache[c].name, model->name))
			return cache[c].kind;
	}
	c = next;
	next = (next + 1) % (int)Q_COUNTOF(cache);
	cache[c].model = model;
	q_strlcpy (cache[c].name, model->name, sizeof(cache[c].name));
	cache[c].kind = GLOW_NONE;
	for (i = 0; i < (int)Q_COUNTOF(glowing); i++)
	{
		if (!q_strcasecmp (model->name, glowing[i]))
			cache[c].kind = GLOW_BEAM;
	}
	for (i = 0; i < (int)Q_COUNTOF(fire_missiles); i++)
	{
		if (!q_strcasecmp (model->name, fire_missiles[i]))
			cache[c].kind = GLOW_MISSILE;
	}
	return cache[c].kind;
}

/* vk_instance.c: does a drawn entity glow, a beam's part (r_effect_lights,
 * r_emissive_scale above 0): the streams' are temporary entities (the
 * client's own, few models: the name cache above holds them) */
qboolean VK_BeamGlows (const scene_entity_t *e)
{
	return e->kind == SCENE_ENT_TEMP && VK_EffectLightsOn () && VK_EmissiveScale () > 0.0f &&
	       e->model->type == mod_alias && GlowKind (e->model) == GLOW_BEAM;
}

/* 6.8, vk_instance.c: is the model one of the fire missiles (with
 * r_effect_lights and r_emissive_scale above 0) */
qboolean VK_FireMissileModel (const qmodel_t *model)
{
	return VK_EffectLightsOn () && VK_EmissiveScale () > 0.0f && model->type == mod_alias && GlowKind (model) == GLOW_MISSILE;
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

/* 6.8: a fire missile's poses' axes: each pose's x extent through the
 * middle of its y and z extents (gl_mesh.c's decode, model space), and the
 * farthest vertex from it */
static void MissileAxes (beam_model_t *b)
{
	const aliashdr_t	*hdr = (const aliashdr_t *) Mod_Extradata (b->model);
	const trivertx_t	*verts = (const trivertx_t *) ((const byte *)hdr + hdr->posedata);
	vec3_t			mins, maxs, p;
	float			r2;
	int			pose, v, k;

	b->num_poses = q_min (hdr->numposes, MAX_MISSILE_POSES);
	for (pose = 0; pose < b->num_poses; pose++)
	{
		const trivertx_t	*pv = verts + pose * hdr->poseverts;

		VectorSet (mins, 1e9f, 1e9f, 1e9f);
		VectorSet (maxs, -1e9f, -1e9f, -1e9f);
		for (v = 0; v < hdr->poseverts; v++)
		{
			for (k = 0; k < 3; k++)
			{
				p[k] = pv[v].v[k] * hdr->scale[k] + hdr->scale_origin[k];
				mins[k] = q_min (mins[k], p[k]);
				maxs[k] = q_max (maxs[k], p[k]);
			}
		}
		VectorSet (b->pose_axis[pose][0], mins[0], 0.5f * (mins[1] + maxs[1]), 0.5f * (mins[2] + maxs[2]));
		VectorSet (b->pose_axis[pose][1], maxs[0], b->pose_axis[pose][0][1], b->pose_axis[pose][0][2]);
		b->pose_radius[pose] = 0.0f;
		for (v = 0; v < hdr->poseverts; v++)
		{
			for (k = 1; k < 3; k++)
				p[k] = pv[v].v[k] * hdr->scale[k] + hdr->scale_origin[k] - b->pose_axis[pose][0][k];
			r2 = p[1] * p[1] + p[2] * p[2];
			b->pose_radius[pose] = q_max (b->pose_radius[pose], r2);
		}
		b->pose_radius[pose] = sqrtf (b->pose_radius[pose]);
	}
}

/* the pending ones (outside frames): their glow materials (made if needed)
 * and the GPU's averages over their triangles' texels; 6.8: a fire
 * missile's per pose too */
static void MakePowers (void)
{
	static float	uvs[MAX_BEAM_TRIS][6], areas[MAX_BEAM_TRIS], colors[MAX_BEAM_TRIS][2][4];
	static float	pose_areas[MAX_MISSILE_POSES * MAX_BEAM_TRIS];
	double		t0 = Sys_DoubleTime ();
	int		i, t, c, k, n, p, made = 0;

	for (i = 0; i < num_beam_models; i++)
	{
		beam_model_t		*b = &beam_models[i];
		const vk_material_t	*m;
		qboolean		premultiply, missile;
		int			alpha_slot;

		if (b->state != POWER_PENDING)
			continue;
		b->state = POWER_KNOWN;
		memset (b->power, 0, sizeof(b->power));
		memset (b->pose_power, 0, sizeof(b->pose_power));
		memset (b->pose_area, 0, sizeof(b->pose_area));
		b->num_poses = 0;
		missile = (GlowKind (b->model) == GLOW_MISSILE);
		n = VK_AliasTriangleAreas (b->model, uvs, areas, MAX_BEAM_TRIS, &b->axis_radius,
					   missile ? pose_areas : NULL, MAX_MISSILE_POSES);
		if (n <= 0)
			continue;
		if (missile)
		{
			MissileAxes (b);	/* before the material: Mod_Extradata */
			for (t = 0; t < n; t++)
			{
				for (p = 0; p < b->num_poses; p++)
					b->pose_area[p] += pose_areas[p * MAX_BEAM_TRIS + t];
			}
		}
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
				{
					b->power[c][k] += areas[t] * colors[t][c][k];
					for (p = 0; p < b->num_poses; p++)
						b->pose_power[p][c][k] += pose_areas[p * MAX_BEAM_TRIS + t] * colors[t][c][k];
				}
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

/* every precached beam model of light's and fire missile's skins: at map load (VK_LoadModels)
 * and after the material files change (the GPU idle, outside frames) */
void VK_BeamLightAverages (void)
{
	int	i, s;

	num_beam_models = 0;
	for (i = 1; i < MAX_MODELS && cl.model_precache[i]; i++)
	{
		qmodel_t		*model = cl.model_precache[i];
		const aliashdr_t	*hdr;

		if (model->type != mod_alias || GlowKind (model) == GLOW_NONE || !q_strcasecmp (model->name, "models/stmedgaz.mdl"))
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

/* ==========================================================================
 * 6.8: the frame's fire missiles
 * ========================================================================== */

/* VK_UpdateInstances, before its instances */
void VK_ClearMissileLights (void)
{
	num_missiles = 0;
	stats.missiles = stats.missiles_over = stats.missiles_unknown = stats.missiles_full = 0;
}

/* vk_instance.c: a glowing fire missile drawn: its model (skin, the pose
 * drawn), its model-to-world transform, the entity's scale and GL's light
 * level (1 lit by the world); VK_BeamLights offers its light */
void VK_AddMissileLight (qmodel_t *model, int skin, int pose, const float transform[4][4], float scale, float level)
{
	fire_missile_t	*m;

	stats.missiles++;
	if (num_missiles >= MAX_FIRE_MISSILES)
	{
		stats.missiles_over++;
		return;
	}
	m = &missiles[num_missiles++];
	m->model = model;
	m->skin = skin;
	m->pose = pose;
	memcpy (m->transform, transform, sizeof(m->transform));
	m->scale = scale;
	m->level = level;
}

static void TransformPoint (const float m[4][4], const vec3_t in, vec3_t out)
{
	int	r;

	for (r = 0; r < 3; r++)
		out[r] = m[0][r] * in[0] + m[1][r] * in[1] + m[2][r] * in[2] + m[3][r];
}

/* each one's light, of its glowing surface's power in the pose drawn: a
 * line light along its axis where the pose is long (at least 4 times its
 * radius across: power per unit length), else a ball at the axis' middle
 * showing that radiance (intensity: power / 4, as the sunstaff's hit) of
 * the radius of a sphere of the surface's area */
static void MissileLights (int curve)
{
	int	i, k;

	for (i = 0; i < num_missiles; i++)
	{
		const fire_missile_t	*m = &missiles[i];
		const aliashdr_t	*hdr = (const aliashdr_t *) Mod_Extradata (m->model);
		beam_model_t		*b;
		vec3_t			a, z, power;
		float			f, len, radius;
		int			skin = m->skin, pose;

		if (skin < 0 || skin >= hdr->numskins)
			skin = 0;
		b = BeamModel (m->model, skin);
		if (!b)
		{
			stats.missiles_full++;
			continue;
		}
		if (b->state != POWER_KNOWN)
		{
			stats.missiles_unknown++;
			continue;
		}
		if (!b->emissive || b->num_poses < 1)
			continue;
		pose = q_min (q_max (m->pose, 0), b->num_poses - 1);
		f = m->level * VK_GetMaterial (b->material)->emissive_factor * m->scale * m->scale;
		for (k = 0; k < 3; k++)
			power[k] = b->pose_power[pose][curve][k] * f;
		TransformPoint (m->transform, b->pose_axis[pose][0], a);
		TransformPoint (m->transform, b->pose_axis[pose][1], z);
		len = sqrtf ((z[0] - a[0]) * (z[0] - a[0]) + (z[1] - a[1]) * (z[1] - a[1]) + (z[2] - a[2]) * (z[2] - a[2]));
		radius = b->pose_radius[pose] * m->scale;
		if (len >= 4.0f * radius && len >= 1.0f)
		{
			VectorScale (power, 1.0f / len, power);
			VK_MissileLight (a, z, radius, power, true);
		}
		else
		{
			VectorScale (power, 0.25f, power);
			VectorAdd (a, z, a);
			VectorScale (a, 0.5f, a);
			VK_MissileLight (a, a, sqrtf (b->pose_area[pose] * m->scale * m->scale / (4.0f * (float)M_PI)), power, false);
		}
	}
}

/* vk_effects.c, after the sprites (inside the frame): each beam of light's
 * line light, the sunstaff's hits' spheres; 6.8: the fire missiles' */
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
	MissileLights (curve);
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
	Con_Printf ("fire missiles (6.8): last frame %d glowing, %d past the %d with a light, %d without a power yet, %d without room\n",
		    stats.missiles, stats.missiles_over, MAX_FIRE_MISSILES, stats.missiles_unknown, stats.missiles_full);
}
