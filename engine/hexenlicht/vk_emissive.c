/* vk_emissive.c -- emissive surfaces: lava, and the flames of the map lights' models
 *
 * Story 4.5. Hexen II has no fullbright texels (colormap.lmp keeps only
 * indices 0 and 255 constant), and utils/light gave lava no light: the
 * mappers put plain lights over it. GL draws the turbulent textures unlit
 * (the texture as it is) and the light entities' torches, flames and
 * candles with MLS_ABSLIGHT (the texture times abslight, 0.75). Two things
 * emit here, at GL's brightness times r_emissive_scale (the radiance of a
 * texture color of 1). In linear light the map's lights light meso9's
 * walls about as GL's lightmaps do, so 1 would be GL's fullbright; but
 * then the lava lights its rooms 25-33 times less than the fake lights did
 * (meso9, meso2): the default 32 lights them about as they did, and the
 * lava's surface is 32 times GL's relation to the walls (the exposure
 * adapts to it; it clips towards white). 4.9 calibrates it:
 *  - Lava (r_lava_light 1): its warped texture, linear (the material's
 *    emissive texture is its base texture; 5.3: a replaced albedo or its
 *    _e, times the .mat's emissive, vk_material.c's
 *    VK_ApplyMaterialFiles), times the scale. The side of
 *    each world lava triangle whose front leaf isn't lava (so not the
 *    undersides) is also a polygon light in vk_light.c's lists, of the
 *    texture's average linear color times the scale (Quake II RTX's
 *    collect_sky_and_lava_light_polys: one light per fan triangle, the
 *    emissive image's light_color; 5.5: of what it emits, a material
 *    file's average made on the GPU by texture_average.comp, since a BC7
 *    file has no decoder here, times its emissive factor, the .mat's key
 *    too: VK_LavaFileColors), and is flagged MATERIAL_FLAG_LIGHT:
 *    diffuse bounce rays that hit it add nothing, as the direct light
 *    samples it. While lava emits, the mappers' fake lava lights are left
 *    out (vk_maplights.c, vk_light.c): plain "light" entities at most
 *    LAVA_LIGHT_DIST from a lava light triangle, over the lava or just
 *    under its surface, which utils/light lit through (8, 14 or 16 units
 *    away on the original maps; the next nearest is 40 away).
 *    r_lava_light 0: no emission and the fake lights, as in GL.
 *  - The light models' flames (r_emissive_models 1): the models at a map
 *    light's origin that GL draws with MLS_ABSLIGHT (vk_instance.c's light
 *    group: torches, flames, candles, when lit) show Quake II RTX's fake
 *    emissive texture of their skin
 *    (textures.c's apply_fake_emissive_threshold: the texels with a
 *    channel of at least VK_EMISSIVE_THRESHOLD, 215 of 255, blurred and
 *    scaled by their luminance, at twice the size) at GL's abslight (the
 *    instance's light level, model_geometry.comp's emissive factor) times
 *    the scale. They aren't lights (the map light is their light) but are
 *    flagged MATERIAL_FLAG_LIGHT too. Only skins with bright texels have one
 *    (vk_texture.c keeps their 8-bit pixels); it is made when first shown,
 *    as the texture "<skin>*E<the skin's CRC>" (a reloaded skin gets its
 *    own; "*S" with r_srgb 1, 4.17), and stays with the texture cache.
 *    A skin's _e file (5.3) is its flame's emissive texture instead;
 *    r_emissive_models 0 turns both off.
 * A change of r_lava_light, r_emissive_scale or r_emissive_models
 * rewrites the materials (after the GPU is idle) and rebuilds the
 * lights; r_srgb takes the
 * lava's other average color when the lights are rebuilt (vk_texture.c),
 * the flames' other emissive textures with the next map's materials.
 *
 * apply_fake_emissive_threshold with its filter and 2x upsampling is ported
 * from Quake II RTX's textures.c.
 *
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
#include "shaders/hl_shared.h"

#define LAVA_LIGHT_DIST		16.0f	/* a fake lava light is at most this far from a lava light, either side */
#define MAX_LAVA_MATERIALS	64
#define MIN_LIGHT_AREA2		1e-3f	/* |cross(e1, e2)| below this: a degenerate triangle, no light */

static void LavaChanged (cvar_t *var);

static cvar_t	r_lava_light = {"r_lava_light", "1", CVAR_NONE};
static cvar_t	r_emissive_scale = {"r_emissive_scale", "32", CVAR_NONE};	/* the radiance of a texture color of 1 (see the top) */
static cvar_t	r_emissive_models = {"r_emissive_models", "1", CVAR_NONE};

/* the lava's materials and its light triangles, from the last VK_LoadWorld */
static struct
{
	int		material;	/* in the material table */
	vec3_t		color[2];	/* the texture's average linear color (the lights' at scale 1), [0] by the 2.2 power, [1] the sRGB curve (4.17) */
	int		lights;		/* its light triangles */
	char		name[16];
	int		file;		/* 5.5: the slot of the material file it emits (its _e, its replaced albedo), 0 = the original */
	vec3_t		file_color[2];	/* that texture's average, as color (texture_average.comp) */
} lava_materials[MAX_LAVA_MATERIALS];
static int	num_lava_materials;

typedef struct
{
	vec3_t		p[3];		/* emitting along cross(p[1] - p[0], p[2] - p[0]) */
	int		lava;		/* its lava_materials entry */
} lavatri_t;

static lavatri_t	*lava_tris;
static int		num_lava_tris, max_lava_tris;
static int		lava_degenerate;	/* triangles facing out of the lava too small for a light */
static int		lava_other_materials;	/* surfaces of materials past MAX_LAVA_MATERIALS: no emission */
static int		skins_made;		/* emissive skin textures made since the map loaded */
static struct { int files; double ms; } lava_averages;	/* 5.5: the last VK_LavaFileColors */


/* ==========================================================================
 * Lava
 * ========================================================================== */

/* the average linear color of a world texture (Quake II RTX's
 * vkpt_extract_emissive_texture_info over the whole texture, with its bias),
 * by the sRGB curve or the 2.2 power (r_srgb, 4.17) */
static void AverageColor (const texture_t *tx, qboolean srgb, vec3_t color)
{
	const byte	*pixels = (const byte *)tx + tx->offsets[0];
	double		sum[3] = { 0.0, 0.0, 0.0 };
	int		i, k, n = tx->width * tx->height;

	for (i = 0; i < n; i++)
	{
		unsigned int	c = d_8to24table[pixels[i]];	/* R,G,B,A in memory */

		for (k = 0; k < 3; k++)
			sum[k] += q_max (VK_ColorToLinearAs (((c >> (8 * k)) & 0xff) / 255.0f, srgb) + (float)EMISSIVE_TRANSFORM_BIAS, 0.0f);
	}
	for (k = 0; k < 3; k++)
		color[k] = (n > 0) ? (float)(sum[k] / n) : 0.0f;
}

void VK_ClearLava (void)
{
	num_lava_materials = 0;
	num_lava_tris = 0;
	lava_degenerate = 0;
	lava_other_materials = 0;
	skins_made = 0;
}

static int LavaIndex (int material)
{
	int	i;

	for (i = 0; i < num_lava_materials; i++)
	{
		if (lava_materials[i].material == material)
			return i;
	}
	return -1;
}

/* vk_world.c: the material of a lava surface (the world's or a submodel's) */
void VK_AddLavaMaterial (int material, const texture_t *tx)
{
	if (LavaIndex (material) >= 0)
		return;
	if (num_lava_materials == MAX_LAVA_MATERIALS)
	{
		lava_other_materials++;
		return;
	}
	lava_materials[num_lava_materials].material = material;
	AverageColor (tx, false, lava_materials[num_lava_materials].color[0]);
	AverageColor (tx, true, lava_materials[num_lava_materials].color[1]);
	lava_materials[num_lava_materials].lights = 0;
	lava_materials[num_lava_materials].file = 0;
	q_strlcpy (lava_materials[num_lava_materials].name, tx->name, sizeof(lava_materials[0].name));
	num_lava_materials++;
	VK_GetMaterial (material)->flags |= VK_MAT_LAVA;	/* its emission: VK_ApplyMaterialFiles */
}

/* vk_world.c: a world lava triangle whose front leaf isn't lava, after its
 * material (VK_AddLavaMaterial); false: it is too small for a light */
qboolean VK_AddLavaLight (const VboPrimitive *p)
{
	lavatri_t	*t;
	vec3_t		e1, e2, n;
	int		lava = LavaIndex ((int)(p->material_id & MATERIAL_INDEX_MASK));

	VectorSubtract (p->pos1, p->pos0, e1);
	VectorSubtract (p->pos2, p->pos0, e2);
	CrossProduct (e1, e2, n);
	if (lava < 0)
		return false;
	if (VectorNormalize (n) < MIN_LIGHT_AREA2)
	{
		lava_degenerate++;
		return false;
	}
	if (num_lava_tris == max_lava_tris)
	{
		max_lava_tris = q_max (max_lava_tris * 2, 256);
		lava_tris = (lavatri_t *) realloc (lava_tris, max_lava_tris * sizeof(*lava_tris));
		if (!lava_tris)
			Sys_Error ("%s: out of memory", __thisfunc__);
	}
	t = &lava_tris[num_lava_tris++];
	VectorCopy (p->pos0, t->p[0]);
	VectorCopy (p->pos1, t->p[1]);
	VectorCopy (p->pos2, t->p[2]);
	t->lava = lava;
	lava_materials[lava].lights++;
	return true;
}

qboolean VK_LavaEmits (void)
{
	return r_lava_light.integer != 0;
}

int VK_NumLavaLights (void)
{
	return r_lava_light.integer ? num_lava_tris : 0;
}

/* its color: the average of what it emits (the original, or 5.5's file)
 * times the material's emissive factor, as its surface's (the scale, and
 * the .mat's emissive key, 5.5) */
void VK_GetLavaLight (int i, vec3_t p[3], vec3_t color)
{
	const lavatri_t	*t = &lava_tris[i];
	const vk_material_t *m = VK_GetMaterial (lava_materials[t->lava].material);
	int		k, s = VK_ColorsSRGB () ? 1 : 0;

	for (k = 0; k < 3; k++)
		VectorCopy (t->p[k], p[k]);
	VectorScale (lava_materials[t->lava].file ? lava_materials[t->lava].file_color[s] : lava_materials[t->lava].color[s],
		     m->emissive_texture ? m->emissive_factor : VK_EmissiveScale (), color);
}

/* the average linear colors (texture_average.comp) of n texture slots,
 * [0] by the 2.2 power, [1] by the sRGB curve; bias: lava's lights' (Quake
 * II RTX's EMISSIVE_TRANSFORM_BIAS per texel), else the plain mean (5.6,
 * vk_materials here's albedo); the GPU idle, outside frames */
void VK_TextureAverages (const int *slots, int n, qboolean bias, vec3_t (*colors)[2])
{
	VkPipelineLayout	layout;
	VkPipeline		pipeline;
	VkCommandBuffer		cmd;
	VkMemoryBarrier2	barrier;
	VkDependencyInfo	dep;
	vk_buffer_t		out;
	struct { VkDeviceAddress out; uint32_t slot, no_bias; } push;
	const float		*v;
	int			i, k, c;

	if (n <= 0)
		return;
	layout = VK_CreatePassLayout (VK_SHADER_STAGE_COMPUTE_BIT, sizeof(push));
	pipeline = VK_CreateComputePipeline ("texture_average.comp", layout);
	VK_CreateBuffer (&out, (VkDeviceSize)n * 2 * 4 * sizeof(float),
			 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VK_MEMORY_READBACK);

	cmd = VK_BeginUpload ();
	for (i = 0; i < n; i++)
	{
		memset (&push, 0, sizeof(push));
		push.out = out.address + (VkDeviceAddress)i * 2 * 4 * sizeof(float);
		push.slot = (uint32_t)slots[i];
		push.no_bias = bias ? 0u : 1u;
		VK_DispatchComputeLayout (cmd, pipeline, layout, &push, sizeof(push), 1, 1, 1);	/* one workgroup */
	}
	memset (&barrier, 0, sizeof(barrier));
	barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
	barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
	barrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
	barrier.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
	barrier.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
	memset (&dep, 0, sizeof(dep));
	dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
	dep.memoryBarrierCount = 1;
	dep.pMemoryBarriers = &barrier;
	vkCmdPipelineBarrier2 (cmd, &dep);
	VK_EndUpload ();
	VK_CHECK (vmaInvalidateAllocation (vk.allocator, out.allocation, 0, VK_WHOLE_SIZE));

	v = (const float *) out.mapped;
	for (i = 0; i < n; i++)
	{
		for (k = 0; k < 2; k++)
		{
			for (c = 0; c < 3; c++)
				colors[i][k][c] = v[(i * 2 + k) * 4 + c];
		}
	}
	VK_DestroyBuffer (&out);
	vkDestroyPipeline (vk.device, pipeline, NULL);
	vkDestroyPipelineLayout (vk.device, layout, NULL);
}

/* 5.5: the lava that emits a material file (its _e, else its replaced
 * albedo: VK_ApplyMaterialFiles) lights with that file's average color;
 * the original's is AverageColor's, made at VK_AddLavaMaterial */
void VK_LavaFileColors (void)
{
	int	slots[MAX_LAVA_MATERIALS], which[MAX_LAVA_MATERIALS], i, n = 0;
	vec3_t	colors[MAX_LAVA_MATERIALS][2];

	for (i = 0; i < num_lava_materials; i++)
	{
		const vk_material_t	*m = VK_GetMaterial (lava_materials[i].material);

		lava_materials[i].file = (m->emissive_texture && m->emissive_texture != m->original) ? m->emissive_texture : 0;
		if (lava_materials[i].file)
		{
			slots[n] = lava_materials[i].file;
			which[n++] = i;
		}
	}
	lava_averages.files = n;
	lava_averages.ms = 0.0;
	if (!n)
		return;
	vkDeviceWaitIdle (vk.device);
	{
		double	t0 = Sys_DoubleTime ();

		VK_TextureAverages (slots, n, true, colors);
		lava_averages.ms = (Sys_DoubleTime () - t0) * 1000.0;
	}
	for (i = 0; i < n; i++)
	{
		VectorCopy (colors[i][0], lava_materials[which[i]].file_color[0]);
		VectorCopy (colors[i][1], lava_materials[which[i]].file_color[1]);
	}
}

/* the distance from p to the triangle (Ericson, Real-Time Collision
 * Detection, 5.1.5: the closest point on the triangle) */
static float TriangleDistance (const vec3_t p, const vec3_t a, const vec3_t b, const vec3_t c)
{
	vec3_t	ab, ac, ap, bp, cp, q;
	float	d1, d2, d3, d4, d5, d6, va, vb, vc, v, w, denom;

	VectorSubtract (b, a, ab);
	VectorSubtract (c, a, ac);
	VectorSubtract (p, a, ap);
	d1 = DotProduct (ab, ap);
	d2 = DotProduct (ac, ap);
	if (d1 <= 0.0f && d2 <= 0.0f)
	{
		VectorCopy (a, q);
		goto done;
	}
	VectorSubtract (p, b, bp);
	d3 = DotProduct (ab, bp);
	d4 = DotProduct (ac, bp);
	if (d3 >= 0.0f && d4 <= d3)
	{
		VectorCopy (b, q);
		goto done;
	}
	vc = d1 * d4 - d3 * d2;
	if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f)
	{
		VectorMA (a, d1 / (d1 - d3), ab, q);
		goto done;
	}
	VectorSubtract (p, c, cp);
	d5 = DotProduct (ab, cp);
	d6 = DotProduct (ac, cp);
	if (d6 >= 0.0f && d5 <= d6)
	{
		VectorCopy (c, q);
		goto done;
	}
	vb = d5 * d2 - d1 * d6;
	if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f)
	{
		VectorMA (a, d2 / (d2 - d6), ac, q);
		goto done;
	}
	va = d3 * d6 - d5 * d4;
	if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f)
	{
		vec3_t	bc;

		VectorSubtract (c, b, bc);
		VectorMA (b, (d4 - d3) / ((d4 - d3) + (d5 - d6)), bc, q);
		goto done;
	}
	denom = 1.0f / (va + vb + vc);
	v = vb * denom;
	w = vc * denom;
	VectorMA (a, v, ab, q);
	VectorMA (q, w, ac, q);
done:
	VectorSubtract (p, q, q);
	return VectorLength (q);
}

/* vk_maplights.c: is a light at origin a fake lava light's place: at most
 * LAVA_LIGHT_DIST from a lava light triangle, over it or just under the
 * surface (utils/light lit through lava from there)? */
qboolean VK_OverLava (const vec3_t origin)
{
	int	i;

	for (i = 0; i < num_lava_tris; i++)
	{
		const lavatri_t	*t = &lava_tris[i];

		if (TriangleDistance (origin, t->p[0], t->p[1], t->p[2]) <= LAVA_LIGHT_DIST + 0.01f)
			return true;
	}
	return false;
}


/* ==========================================================================
 * The light models' flames: Quake II RTX's fake emissive textures
 * ========================================================================== */

/* Quake II RTX's decode_srgb of a byte: its linear light by r_srgb (4.17) */
static float DecodeColor (unsigned int c)
{
	return VK_ColorTable ()[c & 0xff];
}

/* Quake II RTX's encode_srgb: linear light as a byte by r_srgb */
static byte EncodeColor (float x)
{
	return (byte)(VK_LinearToColor (x) * 255.0f + 0.5f);
}

#define LUMINANCE(r, g, b)	((r) * 0.2126f + (g) * 0.7152f + (b) * 0.0722f)

/* Quake II RTX's filter_one_dimension_float: a filter along the stripes,
 * wrapping around their ends (filterscratch_fill_from_float_image) */
static void FilterOneDimension (float *pixels, int num_comps, const float *kernel, int kernel_size,
				int stripe_size, int num_stripes, int stripe_stride, int element_stride)
{
	int	pad_left = kernel_size / 2, pad_right = kernel_size - pad_left - 1;
	int	s, i, j, c, src, n = pad_left + stripe_size + pad_right;
	float	*scratch = (float *) malloc ((size_t)n * num_comps * sizeof(float));
	float	*stripe = pixels, values[3];

	if (!scratch)
		Sys_Error ("%s: out of memory", __thisfunc__);
	for (s = 0; s < num_stripes; s++, stripe += stripe_stride * num_comps)
	{
		for (i = 0; i < n; i++)
		{
			src = ((i - pad_left) % stripe_size + stripe_size) % stripe_size;
			memcpy (scratch + i * num_comps, stripe + src * element_stride * num_comps, num_comps * sizeof(float));
		}
		for (i = 0; i < stripe_size; i++)
		{
			for (c = 0; c < num_comps; c++)
				values[c] = 0.0f;
			for (j = 0; j < kernel_size; j++)
			{
				for (c = 0; c < num_comps; c++)
					values[c] += kernel[j] * scratch[(i + j) * num_comps + c];
			}
			memcpy (stripe + i * element_stride * num_comps, values, num_comps * sizeof(float));
		}
	}
	free (scratch);
}

/* Quake II RTX's filter_float_image: a separable filter */
static void FilterImage (float *pixels, int num_comps, const float *kernel, int kernel_size, int width, int height)
{
	FilterOneDimension (pixels, num_comps, kernel, kernel_size, width, height, width, 1);
	FilterOneDimension (pixels, num_comps, kernel, kernel_size, height, width, 1, width);
}

/* Quake II RTX's apply_fake_emissive_threshold on w x h pixels (R,G,B,A in
 * memory, 8-bit colors): returns the 2w x 2h emissive image (8-bit colors,
 * malloc'd) */
static unsigned int *FakeEmissive (const unsigned int *pixels, int w, int h)
{
	static const float	filter[] = { 0.0093f, 0.028002f, 0.065984f, 0.121703f, 0.175713f, 0.198596f,
					     0.175713f, 0.121703f, 0.065984f, 0.028002f, 0.0093f };
	static const float	filter_final[] = { 0.157731f, 0.684538f, 0.157731f };
	float		*mask, *final, *final_2x, max_src_lum = 0.0f, max_lum = 0.0f, src_lum_scale, lum_scale;
	unsigned int	*out;
	int		i, x, y, k, w2 = w * 2, h2 = h * 2;

	mask = (float *) malloc ((size_t)w * h * sizeof(float));
	final = (float *) malloc ((size_t)w * h * 3 * sizeof(float));
	final_2x = (float *) malloc ((size_t)w2 * h2 * 3 * sizeof(float));
	out = (unsigned int *) malloc ((size_t)w2 * h2 * sizeof(unsigned int));
	if (!mask || !final || !final_2x || !out)
		Sys_Error ("%s: out of memory", __thisfunc__);

	/* the "bright" pixels: one component at the threshold or above */
	for (i = 0; i < w * h; i++)
	{
		unsigned int	c = pixels[i];
		unsigned int	r = c & 0xff, g = (c >> 8) & 0xff, b = (c >> 16) & 0xff;
		float		src_lum = LUMINANCE (DecodeColor (r), DecodeColor (g), DecodeColor (b));

		mask[i] = (q_max (r, q_max (g, b)) < VK_EMISSIVE_THRESHOLD) ? 0.0f : src_lum;
		max_src_lum = q_max (max_src_lum, src_lum);
	}
	src_lum_scale = (max_src_lum > 0.0f) ? 1.0f / max_src_lum : 1.0f;

	/* blurred, normalized to a maximum of 1 */
	FilterImage (mask, 1, filter, Q_COUNTOF(filter), w, h);
	for (i = 0; i < w * h; i++)
		max_lum = q_max (max_lum, mask[i]);
	lum_scale = (max_lum > 0.0f) ? 1.0f / max_lum : 1.0f;

	/* the blurred mask with the image's colors: "keeps the light pixels but
	 * avoids bleeding from neighbouring other pixels"; the luminance is
	 * normalized for textures that are relatively dark */
	for (i = 0; i < w * h; i++)
	{
		unsigned int	c = pixels[i];
		float		color[3], src_lum, scale;

		for (k = 0; k < 3; k++)
			color[k] = DecodeColor ((c >> (8 * k)) & 0xff);
		src_lum = LUMINANCE (color[0], color[1], color[2]) * src_lum_scale;
		scale = mask[i] * src_lum * src_lum * lum_scale;
		for (k = 0; k < 3; k++)
			final[i * 3 + k] = color[k] * scale;
	}

	/* twice the size (Quake II RTX's bilerp: the odd rows and columns between
	 * their neighbours, the last ones between the last and the first), with
	 * a mild filter, less blocky */
	for (y = 0; y < h2; y++)
	{
		int	y0 = y >> 1, y1 = (y & 1) ? (y0 + 1) % h : y0;

		for (x = 0; x < w2; x++)
		{
			int	x0 = x >> 1, x1 = (x & 1) ? (x0 + 1) % w : x0;

			for (k = 0; k < 3; k++)
				final_2x[(y * w2 + x) * 3 + k] = (final[(y0 * w + x0) * 3 + k] + final[(y0 * w + x1) * 3 + k] +
								  final[(y1 * w + x0) * 3 + k] + final[(y1 * w + x1) * 3 + k]) * 0.25f;
		}
	}
	FilterImage (final_2x, 3, filter_final, Q_COUNTOF(filter_final), w2, h2);

	for (i = 0; i < w2 * h2; i++)
		out[i] = (unsigned int)EncodeColor (final_2x[i * 3]) | ((unsigned int)EncodeColor (final_2x[i * 3 + 1]) << 8) |
			 ((unsigned int)EncodeColor (final_2x[i * 3 + 2]) << 16) | 0xff000000u;

	free (mask);
	free (final);
	free (final_2x);
	return out;
}

/* vk_skin.c: the emissive texture of a skin (see the top), 0 = none; may
 * be called while a frame is recorded (GL_LoadTexture's upload waits for
 * the GPU once) */
int VK_EmissiveSkin (int slot)
{
	char		name[MAX_QPATH];
	unsigned int	*rgba, *emissive;
	int		w, h, e;

	if (!VK_TextureName (slot)[0] || strlen (VK_TextureName (slot)) + 7 >= sizeof(name))
		return 0;	/* unnamed, or the name would be cut */
	q_snprintf (name, sizeof(name), "%s*%c%04x", VK_TextureName (slot), VK_ColorsSRGB () ? 'S' : 'E',
		    VK_TextureCRC (slot));	/* made by the curve (4.17) */
	if ((e = VK_FindTexture (name)) >= 0)
		return e;
	rgba = VK_TextureRGBA (slot, &w, &h);
	if (!rgba)
		return 0;	/* no bright texels */
	emissive = FakeEmissive (rgba, w, h);
	e = (int)GL_LoadTexture (name, (byte *)emissive, w * 2, h * 2, TEX_RGBA | TEX_MIPMAP);
	free (emissive);
	free (rgba);
	skins_made++;
	return e;
}


/* ==========================================================================
 * Cvars, vk_lights
 * ========================================================================== */

float VK_EmissiveScale (void)
{
	return q_max (r_emissive_scale.value, 0.0f);
}

qboolean VK_ModelsEmit (void)
{
	return r_emissive_models.integer != 0;
}

/* vk_lights's lines */
void VK_PrintEmissive (void)
{
	int	i, s = VK_ColorsSRGB () ? 1 : 0;

	Con_Printf ("emissive: r_emissive_scale %g; lava %s, %d light triangles (%d too small left out)",
		    VK_EmissiveScale (), r_lava_light.integer ? "emits" : "off (r_lava_light 0)", num_lava_tris,
		    lava_degenerate);
	for (i = 0; i < num_lava_materials; i++)
	{
		const float		*c = lava_materials[i].file ? lava_materials[i].file_color[s] : lava_materials[i].color[s];
		const vk_material_t	*m = VK_GetMaterial (lava_materials[i].material);
		float			key = (m->emissive_texture && VK_EmissiveScale () > 0.0f) ?	/* the .mat's emissive (5.5) */
					      m->emissive_factor / VK_EmissiveScale () : 1.0f;

		Con_Printf ("%s %s %d (color %.3f %.3f %.3f%s, x %g)", i ? "," : ":", lava_materials[i].name, lava_materials[i].lights,
			    c[0], c[1], c[2], lava_materials[i].file ? " its file's" : "", key);
	}
	Con_Printf ("\n");
	if (lava_averages.files)
		Con_Printf ("  the lava's files' average colors: %d, on the GPU in %.1f ms (texture_average.comp)\n",
			    lava_averages.files, lava_averages.ms);
	if (lava_other_materials)
		Con_Printf ("  %d lava surfaces of materials past %d don't emit\n", lava_other_materials, MAX_LAVA_MATERIALS);
	Con_Printf ("  the light models' flames %s; %d emissive skin textures made since the map loaded\n",
		    r_emissive_models.integer ? "emit" : "off (r_emissive_models 0)", skins_made);
}

/* the materials' emission and the lights by the cvars; the materials are in
 * use by the frames in flight */
static void LavaChanged (cvar_t *var)
{
	(void)var;
	if (vk_num_materials <= 1)
		return;		/* no map */
	vkDeviceWaitIdle (vk.device);
	VK_ReapplyMaterials ();		/* the emission of each (vk_material.c) */
	VK_LavaFileColors ();		/* 5.5: a file emitted now */
	VK_RebuildLights ();
}

void VK_ShutdownEmissive (void)
{
	free (lava_tris);
	lava_tris = NULL;
	num_lava_tris = max_lava_tris = 0;
	num_lava_materials = 0;
}

void VK_InitEmissive (void)
{
	Cvar_RegisterVariable (&r_lava_light);
	Cvar_RegisterVariable (&r_emissive_scale);
	Cvar_RegisterVariable (&r_emissive_models);
	Cvar_SetCallback (&r_lava_light, LavaChanged);
	Cvar_SetCallback (&r_emissive_scale, LavaChanged);
	Cvar_SetCallback (&r_emissive_models, LavaChanged);	/* 5.3: the flames' materials (VK_MAT_FLAME) */
}
