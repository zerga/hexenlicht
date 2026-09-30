/* vk_effectlight.c -- effects that glow emit light (story 6.2)
 *
 * The fire, explosion, flash and spark sprites light the scene: each one
 * vk_effects.c draws this frame is a sphere light in the UBO's dynamic
 * lights, where vk_light.c gives them the slots the game's lights, the
 * test lights and the dark lights leave, the brightest at the camera
 * first (the rest are counted). GL gives no sprite a light (only
 * TE_EXPLOSION, the projectiles' model flags and the EF_* effects make
 * dynamic lights); these are the renderer's, not cl_dlights, so
 * cl.light_level stays GL's (G9). r_effect_lights 0 turns them off, and
 * the glowing projectiles (vk_instance.c: their skin glows, and they aren't
 * lit by the light inside them), which gives GL's look.
 *
 * Which sprites: by name (emitting[] below: the client effects' explosions,
 * flashes and sparks, Praevus's fire; not smoke, clouds, ghosts, bubbles or
 * the ice mace's snow hit); a server entity showing one emits too, but not
 * one that owns a dynamic light already (it has its light).
 *
 * The light of a frame: a sphere at the center of the quad as drawn, of
 * the frame's covered area (the quad's area times its mean alpha: pi r^2),
 * whose intensity is the frame's premultiplied mean linear color times the
 * quad's area (the light it shows towards the camera), times 0.33 when
 * translucent, times r_emissive_scale: the radiance lava and the flames
 * emit for a texture color of 1 (vk_emissive.c). The sprite itself stays
 * at GL's colors (R41): as a picture of a brighter fire. The frames darken
 * as the effect ends, so its light fades with them. The sphere shrinks
 * (its intensity kept) to where 14 rays from its center, the axes and the
 * diagonals, marched in 4-unit steps, meet no world solid, so a sphere at
 * a wall doesn't reach through it (brush entities, doors, aren't seen); a
 * center in solid (a hit's spark at its wall) moves towards the camera, up
 * to 4 units, or the light is left out. Its range (vk_light.c's
 * range fade) is where it gives a white wall facing it 1/32 of a full GL
 * texel's light: sqrt(32 I / pi) for the intensity I's luminance.
 *
 * The frames' averages: texture_average.comp (VK_SpriteAverages) over the
 * texture the frame shows (a replaced albedo, 5.3, with the original's
 * coverage where it has no alpha), by both color curves (r_srgb). On the
 * GPU, outside frames: at map load for every frame texture of an emitting
 * sprite (the precached ones and the client's own, loaded at startup) and
 * after the material files change; a frame first drawn later (a sprite
 * loaded mid-game) has none that frame and gets it before the next
 * (VK_BeginFrame).
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
#include "shaders/hl_shared.h"

#define RANGE_FRACTION	(1.0f / 32.0f)	/* a light's range: its light on a white wall there, of a full GL texel's */
#define MIN_RADIUS	2.0f		/* the smallest sphere the shrinking leaves */

static cvar_t	r_effect_lights = {"r_effect_lights", "1", CVAR_NONE};

/* the sprites that emit (cl_effect.c's CE_* sprites; Praevus's fire) */
static const char *const emitting[] =
{
	/* explosions */
	"models/sm_expld.spr", "models/bg_expld.spr", "models/fl_expld.spr", "models/gen_expl.spr",
	"models/xbowexpl.spr", "models/xpspblue.spr", "models/mm_expld.spr", "models/bonexpld.spr",
	"models/fcircle.spr", "models/xplod29.spr", "models/biggy.spr", "models/flrexpl2.spr",
	/* flashes */
	"models/sm_white.spr", "models/gryspt.spr", "models/yr_flsh.spr", "models/bluflash.spr",
	"models/sm_blue.spr", "models/redspt.spr",
	/* sparks and magic hits */
	"models/bspark.spr", "models/spark.spr", "models/rspark.spr", "models/gspark.spr",
	"models/medhit.spr", "models/mezzoref.spr",
	/* Praevus's fire and explosions */
	"models/flamestr.spr", "models/firewal1.spr", "models/firewal2.spr", "models/firewal3.spr",
	"models/firewal4.spr", "models/firewal5.spr", "models/fboom.spr", "models/pow.spr",
	"models/xplsn_1.spr", "models/axplsn_1.spr", "models/axplsn_2.spr", "models/axplsn_5.spr",
	"models/Bluexp3.spr", "models/muzzle1.spr"
};

/* a frame texture's average, by its original slot */
enum { AVERAGE_NONE, AVERAGE_PENDING, AVERAGE_KNOWN };
typedef struct
{
	float	color[2][3];	/* premultiplied mean linear color: by the 2.2 power, by the sRGB curve */
	float	alpha;		/* the mean alpha: the covered share of the quad */
	int	shown;		/* the texture it was made from (a replaced albedo, or the slot) */
	int	alpha_slot;	/* its coverage's texture, 0 = its own alpha */
	int	state;		/* AVERAGE_* */
} frame_average_t;

static frame_average_t	averages[VK_MAX_TEXTURES];
static int		pending[VK_MAX_TEXTURES];	/* slots whose average is to be made, each once */
static int		num_pending;

/* the frame's sprites that emit */
typedef struct
{
	vec3_t		origin;
	float		radius;		/* its covered area's */
	vec3_t		intensity;	/* the radiance times the projected area */
	float		priority;	/* its luminance / distance^2 at the camera */
} candidate_t;

static candidate_t	candidates[MAX_EFFECT_SPRITES];
static int		order[MAX_EFFECT_SPRITES];
static int		num_candidates;
static uint64_t		candidates_frame;	/* vk.frame_count they were collected in */

/* statistics for vk_lights (the lights are chosen inside the frame, which
 * can't print) */
static struct
{
	int	sprites;	/* last frame: emitting sprites drawn */
	int	lit;		/* of them, lights in the UBO */
	int	over;		/* left out: no slot free */
	int	solid;		/* left out: the center in solid */
	int	unknown;	/* left out: no average yet */
	int	owned;		/* left out: the entity owns a dynamic light */
	int	most;		/* since the map loaded: the most lit in a frame */
	float	brightest;	/* the largest intensity's luminance */
	int	averages;	/* the frame averages made */
	double	ms;		/* and the time they took */
} stats;


qboolean VK_EffectLightsOn (void)
{
	return r_effect_lights.integer != 0;
}

/* does the sprite model emit; the answers for the models seen last, by
 * pointer and name (a slot of the model cache can take another model) */
static qboolean SpriteEmits (const qmodel_t *model)
{
	static struct
	{
		const qmodel_t	*model;
		char		name[MAX_QPATH];
		qboolean	emits;
	} cache[64];
	static int	next;
	int		i, c;

	for (c = 0; c < (int)Q_COUNTOF(cache); c++)
	{
		if (cache[c].model == model && !strcmp (cache[c].name, model->name))
			return cache[c].emits;
	}
	c = next;
	next = (next + 1) % (int)Q_COUNTOF(cache);
	cache[c].model = model;
	q_strlcpy (cache[c].name, model->name, sizeof(cache[c].name));
	cache[c].emits = false;
	for (i = 0; i < (int)Q_COUNTOF(emitting); i++)
	{
		if (!q_strcasecmp (model->name, emitting[i]))
			cache[c].emits = true;
	}
	return cache[c].emits;
}

/* a texture slot is a frame of an emitting sprite: named <model>_<frame> (gl_model.c's Mod_LoadSpriteFrame) */
static qboolean EmittingFrame (const char *texname)
{
	int	i;
	size_t	len;

	for (i = 0; i < (int)Q_COUNTOF(emitting); i++)
	{
		len = strlen (emitting[i]);
		if (!q_strncasecmp (texname, emitting[i], len) && texname[len] == '_')
			return true;
	}
	return false;
}


/* ==========================================================================
 * The frames' averages
 * ========================================================================== */

static void Queue (int slot, int shown, int alpha_slot)
{
	frame_average_t	*a = &averages[slot];

	a->shown = shown;
	a->alpha_slot = alpha_slot;
	if (a->state != AVERAGE_PENDING)
	{
		a->state = AVERAGE_PENDING;
		pending[num_pending++] = slot;
	}
}

/* the pending ones, outside frames */
static void MakeAverages (void)
{
	int		slots[64], alpha_slots[64], which[64], i, j, n;
	float		out[64][2][4];
	double		t0;

	if (!num_pending)
		return;
	t0 = Sys_DoubleTime ();
	for (i = 0; i < num_pending; i += n)
	{
		n = q_min (num_pending - i, (int)Q_COUNTOF(slots));
		for (j = 0; j < n; j++)
		{
			const frame_average_t	*a = &averages[pending[i + j]];

			which[j] = pending[i + j];
			slots[j] = a->shown;
			alpha_slots[j] = a->alpha_slot;
		}
		VK_SpriteAverages (slots, alpha_slots, n, out);
		for (j = 0; j < n; j++)
		{
			frame_average_t	*a = &averages[which[j]];

			VectorCopy (out[j][0], a->color[0]);
			VectorCopy (out[j][1], a->color[1]);
			a->alpha = out[j][0][3];
			a->state = AVERAGE_KNOWN;
		}
	}
	stats.averages += num_pending;
	stats.ms += (Sys_DoubleTime () - t0) * 1000.0;
	num_pending = 0;
}

/* every emitting sprite's frame textures: at map load (VK_LoadModels, after
 * their material files are looked up) and after the files change (the GPU
 * idle, outside frames) */
void VK_SpriteLightAverages (void)
{
	int		i, original;
	qboolean	coverage;

	memset (averages, 0, sizeof(averages));
	num_pending = 0;
	for (i = 1; i < VK_MAX_TEXTURES; i++)
	{
		int	shown;

		if (!strstr (VK_TextureName (i), ".spr_") || !EmittingFrame (VK_TextureName (i)))
			continue;
		shown = VK_SpriteTexture (i, &original, &coverage);
		Queue (i, shown, coverage ? original : 0);
	}
	MakeAverages ();
}

/* VK_BeginFrame, outside frames: the frames first drawn last frame */
void VK_EffectLightsBetweenFrames (void)
{
	MakeAverages ();
}

/* D_ClearOpenGLTextures freed slots (a map change): the next map's load
 * makes them again */
void VK_EffectLightsPurged (void)
{
	memset (averages, 0, sizeof(averages));
	num_pending = 0;
}

/* on map change (VK_ClearEffects): the statistics since the map loaded */
void VK_ClearEffectLights (void)
{
	stats.most = 0;
	stats.brightest = 0.0f;
	num_candidates = 0;
}


/* ==========================================================================
 * The frame's sprite lights
 * ========================================================================== */

/* vk_effects.c's WriteSprites, before its sprites */
void VK_ClearSpriteLights (void)
{
	num_candidates = 0;
	candidates_frame = vk.frame_count;
	stats.sprites = stats.owned = stats.unknown = 0;
}

/* a sprite drawn this frame: slot its frame's original texture, shown the
 * one drawn, alpha_slot its coverage's (0 = its own alpha), alpha the
 * entity's (0.33 translucent), center and area its quad's */
void VK_SpriteLight (const scene_entity_t *e, int slot, int shown, int alpha_slot, float alpha, const vec3_t center, float area)
{
	const frame_average_t	*a;
	candidate_t		*c;
	vec3_t			d;
	float			scale, lum, r2;
	int			k, curve;

	if (!r_effect_lights.integer || !SpriteEmits (e->model))
		return;
	stats.sprites++;
	if (e->kind == SCENE_ENT_DYNAMIC && VK_DynamicLightOwner (e->num, e->origin, 1.0e6f))
	{
		stats.owned++;	/* it has its light */
		return;
	}
	if (slot <= 0 || slot >= VK_MAX_TEXTURES || num_candidates >= MAX_EFFECT_SPRITES)
		return;
	a = &averages[slot];
	if (a->state != AVERAGE_KNOWN || a->shown != shown || a->alpha_slot != alpha_slot)
	{
		if (a->state != AVERAGE_PENDING || a->shown != shown || a->alpha_slot != alpha_slot)
			Queue (slot, shown, alpha_slot);
		stats.unknown++;
		return;
	}

	curve = VK_ColorsSRGB () ? 1 : 0;
	scale = area * alpha * VK_EmissiveScale ();
	c = &candidates[num_candidates];
	for (k = 0; k < 3; k++)
		c->intensity[k] = a->color[curve][k] * scale;
	lum = 0.2126f * c->intensity[0] + 0.7152f * c->intensity[1] + 0.0722f * c->intensity[2];
	if (!(lum > 0.0f) || !(a->alpha > 0.0f))
		return;
	VectorCopy (center, c->origin);
	c->radius = q_max (sqrtf (area * a->alpha / (float)M_PI), MIN_RADIUS);
	VectorSubtract (center, r_scene.vieworg, d);
	r2 = q_max (DotProduct (d, d), c->radius * c->radius);
	c->priority = lum / r2;
	stats.brightest = q_max (stats.brightest, lum);
	num_candidates++;
}

static int ComparePriority (const void *a, const void *b)
{
	float	pa = candidates[*(const int *)a].priority, pb = candidates[*(const int *)b].priority;

	return (pa < pb) ? 1 : (pa > pb) ? -1 : 0;
}

static qboolean InSolid (const vec3_t p)
{
	vec3_t	q;

	VectorCopy (p, q);	/* Mod_PointInLeaf takes a non-const vector */
	return Mod_PointInLeaf (q, r_scene.worldmodel)->contents == CONTENTS_SOLID;
}

/* a center in solid (a hit's spark or flash at the wall it hit) moved out
 * towards the camera, which sees the sprite, up to 4 units (not through a
 * wall: Hexen II's are thicker); false: still in solid */
static qboolean OutOfSolid (vec3_t p)
{
	vec3_t	dir, q;
	int	step;

	if (!InSolid (p))
		return true;
	VectorSubtract (r_scene.vieworg, p, dir);
	if (VectorNormalize (dir) < 1.0f)
		return false;
	for (step = 1; step <= 4; step++)
	{
		VectorMA (p, (float)step, dir, q);
		if (!InSolid (q))
		{
			VectorCopy (q, p);
			return true;
		}
	}
	return false;
}

/* the largest radius up to radius at which 14 rays from the center (the
 * axes and the diagonals) meet no solid, marched in steps of at most 4
 * units, so a wall between two points of the sphere isn't missed; at least
 * MIN_RADIUS (0: the center in solid). The world's solid only: brush
 * entities (doors) aren't seen */
static float FreeRadius (const vec3_t center, float radius)
{
	vec3_t	dir, p;
	float	reach = radius, step;
	int	k, j, i, n;

	if (InSolid (center))
		return 0.0f;
	n = (int)ceilf (radius / 4.0f);
	step = radius / (float)q_max (n, 1);
	for (k = 0; k < 14; k++)
	{
		VectorClear (dir);
		if (k < 6)
			dir[k >> 1] = (k & 1) ? -1.0f : 1.0f;
		else
		{
			for (j = 0; j < 3; j++)
				dir[j] = ((k - 6) & (1 << j)) ? -0.57735f : 0.57735f;	/* 1 / sqrt(3) */
		}
		for (i = 1; i <= n && (float)i * step <= reach; i++)
		{
			VectorMA (center, (float)i * step, dir, p);
			if (InSolid (p))
			{
				reach = (float)(i - 1) * step;
				break;
			}
		}
	}
	return q_max (reach, MIN_RADIUS);
}

/* vk_light.c's VK_PrepareLights, inside the frame: up to room of this
 * frame's sprite lights, the brightest at the camera first */
int VK_ChooseEffectLights (int room, vk_effectlight_t *out)
{
	int	i, n = 0, count;
	vec3_t	origin;

	stats.lit = stats.over = stats.solid = 0;
	if (!r_effect_lights.integer || candidates_frame != vk.frame_count || !r_scene.worldmodel)
		return 0;
	count = num_candidates;
	for (i = 0; i < count; i++)
		order[i] = i;
	qsort (order, count, sizeof(order[0]), ComparePriority);

	for (i = 0; i < count; i++)
	{
		const candidate_t	*c = &candidates[order[i]];
		vk_effectlight_t	*l;
		float			radius, lum;
		int			k;

		if (n >= room)
		{
			stats.over += count - i;
			break;
		}
		VectorCopy (c->origin, origin);
		radius = OutOfSolid (origin) ? FreeRadius (origin, c->radius) : 0.0f;
		if (radius <= 0.0f)
		{
			stats.solid++;
			continue;
		}
		l = &out[n++];
		VectorCopy (origin, l->origin);
		l->radius = radius;
		/* a UBO sphere's color is pi x its radiance: the intensity over r^2 */
		for (k = 0; k < 3; k++)
			l->color[k] = c->intensity[k] / (radius * radius);
		lum = 0.2126f * c->intensity[0] + 0.7152f * c->intensity[1] + 0.0722f * c->intensity[2];
		l->range = sqrtf (lum / (RANGE_FRACTION * (float)M_PI));
	}
	stats.lit = n;
	stats.most = q_max (stats.most, n);
	return n;
}

/* vk_lights */
void VK_PrintEffectLights (void)
{
	int	known = 0, i;

	for (i = 0; i < VK_MAX_TEXTURES; i++)
		known += (averages[i].state == AVERAGE_KNOWN);
	Con_Printf ("effect lights (6.2)%s: last frame %d emitting sprites, %d lights; left out: %d over the 32 slots, %d in solid, "
		    "%d without an average yet, %d owning a dynamic light; the most in a frame %d, the brightest %.0f\n",
		    r_effect_lights.integer ? "" : " off (r_effect_lights 0)", stats.sprites, stats.lit, stats.over, stats.solid,
		    stats.unknown, stats.owned, stats.most, stats.brightest);
	Con_Printf ("  frame averages: %d known, %d made since startup in %.1f ms\n", known, stats.averages, stats.ms);
}

void VK_InitEffectLights (void)
{
	Cvar_RegisterVariable (&r_effect_lights);
}
