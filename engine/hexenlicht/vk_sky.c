/* vk_sky.c -- Hexen II's sky, and the sun
 *
 * Story 4.6. A Hexen II sky is a 256x128 texture of two 128x128 layers.
 * Hammer of Thyrion's GL draws them (gl_warp.c's R_InitSky and
 * EmitSkyPolys, its default path, gl_multitexture 0) as follows:
 *  - the right half is the back layer, scrolling at realtime * 8;
 *  - the left half is the front layer, scrolling at realtime * 16. Its
 *    color index 0 is transparent, and GL gives those texels the back
 *    layer's average color so bilinear filtering leaves no dark fringe
 *    (255, the palette's transparent color, is transparent too, with its
 *    own color);
 *  - the front is blended over the back at r_skyalpha (0.67; at 1 the
 *    front is opaque, as in GL's multitexture path);
 *  - the texture coordinates depend only on the direction: height counts
 *    three times (a flattened sphere), the direction is scaled to 6 * 63
 *    units, plus the scroll, in texels of the layers;
 *  - bilinear, no mipmaps.
 * One sky per map: GL's last R_InitSky, the world's last sky texture
 * (castle5, tower and thomas also have a sky000; GL shows their sky001 on
 * every sky face). VK_LoadSky takes it at map load as two textures.
 * path_tracer_rgen.h's env_map computes GL's texture coordinates per pixel
 * from the ray's direction, as the software renderer does (d_sky.c): GL
 * computes them at the vertices of its polygon pieces and interpolates,
 * which warps the sky when seen up close. The layers are blended in their
 * 8-bit colors, as GL blends the framebuffer's bytes, and the result as
 * linear light (transfer.glsl, 4.17) is the radiance (times 1: GL's
 * fullbright relation to the walls, vk_emissive.c). It is what primary
 * rays, reflections, refractions and specular bounces see, in both modes.
 * It scrolls on GL's realtime, which counts the clock and runs while
 * paused; with host_framerate on game time, so that test runs repeat.
 *
 * The sky's two modes (PLAN.md; the default is chosen per map during
 * calibration, until then faithful everywhere):
 *  - faithful (r_sky_light 0): the sky lights nothing (glossy surfaces
 *    still reflect it: specular bounces see it);
 *  - sky light (r_sky_light 1): diffuse bounce rays that hit the sky
 *    gather a dome of constant radiance: the sky's average color as GL
 *    shows it (both layers blended at r_skyalpha, averaged over all pairs
 *    of their texels, linear) times r_sky_light_scale (1: the sky lights
 *    with the light it shows). Constant rather than the scrolling layers:
 *    the light doesn't flicker as the clouds pass. The sky's faces are
 *    no polygon lights (RENDERER.md: they would crowd the light lists).
 *    With r_sun 1, a sun: Quake II RTX's get_sunlight (a disc of
 *    r_sun_angle degrees, in direct lighting and at bounce hits) from
 *    r_sun_elevation and r_sun_azimuth (degrees; the azimuth counts from
 *    +x towards +y, as a yaw), of r_sun_color (an 8-bit color) at
 *    r_sun_intensity (1: a white surface facing it is lit as GL's
 *    fullbright). Its shadow ray ends at the first sky face it meets
 *    (Hexen II has world geometry above some of its skies), and only
 *    points in clusters that can see a sky triangle trace it (Quake II
 *    RTX's sky visibility: the PVS rows of the clusters holding one, in
 *    the light buffer). There is no visible sun disc: the painted skies
 *    have none.
 * vk_sky prints the sky, the mode and the sun.
 *
 * Copyright (C) 1996-1997  Id Software, Inc.
 * Copyright (C) 1997-1998  Raven Software Corp.
 * Copyright (C) 2018 Christoph Schied
 * Copyright (C) 2019, NVIDIA CORPORATION. All rights reserved.
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

#define SKY_SIZE	128	/* a layer's width and height (GL's R_InitSky) */

cvar_t		r_skyalpha = {"r_skyalpha", "0.67", CVAR_ARCHIVE};	/* GL's (glquake.h) */
static cvar_t	r_sky_light = {"r_sky_light", "0", CVAR_NONE};
static cvar_t	r_sky_light_scale = {"r_sky_light_scale", "1", CVAR_NONE};
static cvar_t	r_sun = {"r_sun", "0", CVAR_NONE};
static cvar_t	r_sun_intensity = {"r_sun_intensity", "1", CVAR_NONE};
static cvar_t	r_sun_color = {"r_sun_color", "1 1 1", CVAR_NONE};
static cvar_t	r_sun_elevation = {"r_sun_elevation", "45", CVAR_NONE};
static cvar_t	r_sun_azimuth = {"r_sun_azimuth", "45", CVAR_NONE};
static cvar_t	r_sun_angle = {"r_sun_angle", "1", CVAR_NONE};

static struct
{
	char		name[16];		/* the world's sky texture, "" = none */
	qboolean	bad_size;		/* it isn't 256x128: no sky */
	int		back_texture;		/* texture slots, 0 = no sky */
	int		front_texture;
	int		back_count[256];	/* the layers' color indices, for the dome's average */
	int		front_count[256];
	vec3_t		average;		/* linear, as GL blends the layers at average_alpha */
	float		average_alpha;		/* the r_skyalpha it was made for, < 0 = none */
	qboolean	average_srgb;		/* the r_srgb it was made for (4.17) */
	int		sky_clusters;		/* clusters holding a sky triangle */
	int		visible_clusters;	/* clusters that can see one: the sun's rays */
	uint32_t	visibility[MAX_LIGHT_LISTS / 32];	/* LightBuffer's sky_visibility */
	uint32_t	visibility_version;	/* counts the maps */
} sky;


/* ==========================================================================
 * Map load
 * ========================================================================== */

/* the world's sky, as GL's R_InitSky makes its two textures */
static void LoadSkyTextures (qmodel_t *worldmodel)
{
	static unsigned int	back[SKY_SIZE * SKY_SIZE], front[SKY_SIZE * SKY_SIZE];
	texture_t		*tx = NULL;
	const byte		*src;
	unsigned int		transpix;
	int			i, j, p, r, g, b;

	for (i = 0; i < worldmodel->numtextures; i++)
	{
		if (worldmodel->textures[i] && !strncmp (worldmodel->textures[i]->name, "sky", 3))
			tx = worldmodel->textures[i];	/* the last one, and case-sensitive as GL */
	}
	if (!tx)
		return;
	q_strlcpy (sky.name, tx->name, sizeof(sky.name));
	if (tx->width != 2 * SKY_SIZE || tx->height != SKY_SIZE)
	{
		sky.bad_size = true;	/* GL reads 256 columns of 128 rows whatever its size */
		return;
	}

	/* the back layer, and its average color for the front's transparent texels */
	src = (const byte *)tx + tx->offsets[0];
	r = g = b = 0;
	for (i = 0; i < SKY_SIZE; i++)
	{
		for (j = 0; j < SKY_SIZE; j++)
		{
			p = src[i * 2 * SKY_SIZE + j + SKY_SIZE];
			back[i * SKY_SIZE + j] = d_8to24table[p];
			sky.back_count[p]++;
			r += ((const byte *)&d_8to24table[p])[0];
			g += ((const byte *)&d_8to24table[p])[1];
			b += ((const byte *)&d_8to24table[p])[2];
		}
	}
	((byte *)&transpix)[0] = (byte)(r / (SKY_SIZE * SKY_SIZE));
	((byte *)&transpix)[1] = (byte)(g / (SKY_SIZE * SKY_SIZE));
	((byte *)&transpix)[2] = (byte)(b / (SKY_SIZE * SKY_SIZE));
	((byte *)&transpix)[3] = 0;

	/* the front layer: index 0 transparent with the back's average color */
	for (i = 0; i < SKY_SIZE; i++)
	{
		for (j = 0; j < SKY_SIZE; j++)
		{
			p = src[i * 2 * SKY_SIZE + j];
			front[i * SKY_SIZE + j] = p ? d_8to24table[p] : transpix;
			sky.front_count[p]++;
		}
	}

	/* GL's names; bilinear, repeating, no mipmaps */
	sky.back_texture = (int)GL_LoadTexture ("upsky", (byte *)back, SKY_SIZE, SKY_SIZE, TEX_RGBA | TEX_LINEAR | TEX_REPEAT);
	sky.front_texture = (int)GL_LoadTexture ("lowsky", (byte *)front, SKY_SIZE, SKY_SIZE, TEX_ALPHA | TEX_RGBA | TEX_LINEAR | TEX_REPEAT);
}

/* Quake II RTX's compute_sky_visibility: the PVS rows of the clusters
 * holding a world sky triangle */
static void LoadSkyVisibility (const VboPrimitive *prims, uint32_t num_prims)
{
	byte		*has_sky;
	uint32_t	i;
	int		c, k, n = q_min (vk_pvs.num_clusters, MAX_LIGHT_LISTS);

	has_sky = (byte *) calloc (q_max (n, 1), 1);
	if (!has_sky)
		Sys_Error ("%s: out of memory", __thisfunc__);
	for (i = 0; i < num_prims; i++)
	{
		c = prims[i].cluster;
		if ((prims[i].material_id & MATERIAL_KIND_MASK) == MATERIAL_KIND_SKY && c >= 0 && c < n)
			has_sky[c] = 1;
	}
	for (c = 0; c < n; c++)
	{
		const byte	*row;

		if (!has_sky[c])
			continue;
		sky.sky_clusters++;
		row = VK_ClusterPVS (c);
		for (k = 0; k < n; k++)
		{
			if ((row[k >> 3] >> (k & 7)) & 1)
				sky.visibility[k >> 5] |= 1u << (k & 31);
		}
	}
	free (has_sky);
	for (c = 0; c < n; c++)
		sky.visible_clusters += (sky.visibility[c >> 5] >> (c & 31)) & 1;
}

/* a new map (VK_LoadWorld, after the PVS is final) */
void VK_LoadSky (qmodel_t *worldmodel, const VboPrimitive *prims, uint32_t num_prims)
{
	uint32_t	version = sky.visibility_version;

	memset (&sky, 0, sizeof(sky));
	sky.visibility_version = version + 1;
	sky.average_alpha = -1.0f;
	LoadSkyTextures (worldmodel);
	LoadSkyVisibility (prims, num_prims);
}

const uint32_t *VK_SkyVisibility (uint32_t *version)
{
	*version = sky.visibility_version;
	return sky.visibility;
}


/* ==========================================================================
 * Each frame
 * ========================================================================== */

/* the sky's average color as GL shows it: the front layer over the back at
 * alpha, over all pairs of their texels (the layers scroll apart), blended
 * in their 8-bit colors, linear */
static void MakeAverage (float alpha)
{
	double	sum[3] = { 0.0, 0.0, 0.0 }, total = 0.0;
	int	bi, fi, k;

	for (bi = 0; bi < 256; bi++)
	{
		const byte	*bc = (const byte *)&d_8to24table[bi];

		if (!sky.back_count[bi])
			continue;
		for (fi = 0; fi < 256; fi++)
		{
			const byte	*fc = (const byte *)&d_8to24table[fi];
			double		w = (double)sky.back_count[bi] * sky.front_count[fi];
			float		a = (fi != 0 && fc[3]) ? alpha : 0.0f;	/* index 0 and the palette's transparent 255 */

			if (!sky.front_count[fi])
				continue;
			for (k = 0; k < 3; k++)
				sum[k] += w * VK_ColorToLinear ((bc[k] * (1.0f - a) + fc[k] * a) / 255.0f);
			total += w;
		}
	}
	for (k = 0; k < 3; k++)
		sky.average[k] = (total > 0.0) ? (float)(sum[k] / total) : 0.0f;
	sky.average_alpha = alpha;
	sky.average_srgb = VK_ColorsSRGB ();
}

static float SkyAlpha (void)
{
	return q_max (0.0f, q_min (1.0f, r_skyalpha.value));
}

static qboolean SkyLights (void)
{
	return sky.front_texture && r_sky_light.integer;
}

static qboolean SunShines (void)
{
	return SkyLights () && r_sun.integer && r_sun_intensity.value > 0.0f;
}

/* r_sun_color: an 8-bit color, missing components 1 */
static void SunColor (vec3_t color)
{
	float	c[3] = { 1.0f, 1.0f, 1.0f };
	int	k;

	sscanf (r_sun_color.string, "%f %f %f", &c[0], &c[1], &c[2]);
	for (k = 0; k < 3; k++)
		color[k] = VK_ColorToLinear (c[k]);
}

/* VK_PrepareUBO: the sky's textures, scroll and blend, the dome and the
 * sun (Quake II RTX's vkpt_physical_sky_update_ubo for the sun's fields) */
void VK_PrepareSky (struct QVKUniformBuffer_s *ubo)
{
	float	alpha = SkyAlpha (), elevation, azimuth, angle, intensity;
	vec3_t	color, up = { 0.0f, 0.0f, 1.0f };
	double	time;
	int	k;

	/* GL's realtime; game time with host_framerate, so that test runs repeat
	 * (TESTING.md: realtime counts the clock, and runs while paused) */
	time = (Cvar_VariableValue ("host_framerate") > 0.0f) ? r_scene.time : realtime;
	ubo->sky_back_texture = (uint32_t)sky.back_texture;
	ubo->sky_front_texture = (uint32_t)sky.front_texture;
	ubo->sky_alpha = alpha;
	ubo->sky_back_scroll = (float)(fmod (time * 8.0, 128.0) / 128.0);	/* GL's, in texels of 128 */
	ubo->sky_front_scroll = (float)(fmod (time * 16.0, 128.0) / 128.0);
	ubo->pt_env_scale = 1.0f;

	if (sky.front_texture && (sky.average_alpha != alpha || sky.average_srgb != VK_ColorsSRGB ()))
		MakeAverage (alpha);
	for (k = 0; k < 3; k++)
		ubo->sky_dome[k] = SkyLights () ? sky.average[k] * q_max (r_sky_light_scale.value, 0.0f) : 0.0f;

	ubo->sun_visible = SunShines () ? 1 : 0;
	elevation = q_max (-90.0f, q_min (90.0f, r_sun_elevation.value)) * (float)M_PI / 180.0f;
	azimuth = r_sun_azimuth.value * (float)M_PI / 180.0f;
	ubo->sun_direction[0] = cosf (azimuth) * cosf (elevation);
	ubo->sun_direction[1] = sinf (azimuth) * cosf (elevation);
	ubo->sun_direction[2] = sinf (elevation);
	VectorCopy (ubo->sun_direction, ubo->sun_direction_envmap);
	if (fabsf (ubo->sun_direction[2]) >= 0.99f)	/* Quake II RTX tests +z only: below, the frame would be 0 */
	{
		VectorSet (ubo->sun_tangent, 1.0f, 0.0f, 0.0f);
		VectorSet (ubo->sun_bitangent, 0.0f, 1.0f, 0.0f);
	}
	else
	{
		CrossProduct (ubo->sun_direction, up, ubo->sun_tangent);
		VectorNormalize (ubo->sun_tangent);
		CrossProduct (ubo->sun_direction, ubo->sun_tangent, ubo->sun_bitangent);
		VectorNormalize (ubo->sun_bitangent);
	}
	angle = q_max (0.1f, q_min (10.0f, r_sun_angle.value)) * (float)M_PI / 180.0f;	/* the disc's diameter */
	ubo->sun_tan_half_angle = tanf (angle * 0.5f);
	ubo->sun_cos_half_angle = cosf (angle * 0.5f);
	ubo->sun_solid_angle = (float)(2.0 * M_PI * (1.0 - cos (angle * 0.5)));
	ubo->sun_bounce_scale = 1.0f;	/* Quake II RTX's sun_bounce */
	/* its irradiance: a white surface facing it at intensity times GL's fullbright */
	intensity = (float)M_PI * q_max (r_sun_intensity.value, 0.0f);
	SunColor (color);
	VectorScale (color, intensity, ubo->sun_color);
}


/* ==========================================================================
 * vk_sky
 * ========================================================================== */

static void VK_Sky_f (void)
{
	vec3_t	color;

	if (!sky.name[0])
		Con_Printf ("no sky in this map\n");
	else if (sky.bad_size)
		Con_Printf ("sky %s: not 256x128, not drawn\n", sky.name);
	else
	{
		if (sky.average_alpha != SkyAlpha () || sky.average_srgb != VK_ColorsSRGB ())
			MakeAverage (SkyAlpha ());
		Con_Printf ("sky %s: r_skyalpha %g, average color (linear) %.4f %.4f %.4f\n", sky.name,
			    SkyAlpha (), sky.average[0], sky.average[1], sky.average[2]);
	}
	Con_Printf ("sky clusters %d, clusters that see the sky %d of %d\n", sky.sky_clusters, sky.visible_clusters,
		    vk_pvs.num_clusters);
	if (!SkyLights ())
	{
		Con_Printf ("faithful: the sky lights nothing%s\n", r_sky_light.integer ? " (no sky)" : " (r_sky_light 0)");
		return;
	}
	Con_Printf ("sky light: dome %.4f %.4f %.4f (r_sky_light_scale %g)\n", sky.average[0] * r_sky_light_scale.value,
		    sky.average[1] * r_sky_light_scale.value, sky.average[2] * r_sky_light_scale.value, r_sky_light_scale.value);
	if (!SunShines ())
	{
		Con_Printf ("no sun (r_sun %d, r_sun_intensity %g)\n", r_sun.integer, r_sun_intensity.value);
		return;
	}
	SunColor (color);
	Con_Printf ("sun: elevation %g, azimuth %g, %g degrees wide, intensity %g, color (linear) %.3f %.3f %.3f\n",
		    r_sun_elevation.value, r_sun_azimuth.value, q_max (0.1f, q_min (10.0f, r_sun_angle.value)),
		    r_sun_intensity.value, color[0], color[1], color[2]);
}

/* GL's R_InitSky makes the sky's textures when a model's textures load;
 * Hexenlicht takes the world's sky at map load (VK_LoadSky) */
void R_InitSky (texture_t *mt)
{
	(void)mt;
}

void VK_InitSky (void)
{
	Cvar_RegisterVariable (&r_skyalpha);
	Cvar_RegisterVariable (&r_sky_light);
	Cvar_RegisterVariable (&r_sky_light_scale);
	Cvar_RegisterVariable (&r_sun);
	Cvar_RegisterVariable (&r_sun_intensity);
	Cvar_RegisterVariable (&r_sun_color);
	Cvar_RegisterVariable (&r_sun_elevation);
	Cvar_RegisterVariable (&r_sun_azimuth);
	Cvar_RegisterVariable (&r_sun_angle);
	Cmd_AddCommand ("vk_sky", VK_Sky_f);
}

void VK_ShutdownSky (void)
{
	memset (&sky, 0, sizeof(sky));
}
