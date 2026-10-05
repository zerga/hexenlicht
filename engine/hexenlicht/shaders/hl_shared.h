/* hl_shared.h -- definitions shared by the Hexenlicht renderer (C) and its
 * shaders (GLSL): Quake II RTX's headers (constants.h, the primitive
 * record in vertex_buffer.h, ModelInstance and the global UBO in
 * global_ubo.h), and Hexenlicht's own GPU data: the PVS buffer, the
 * liquids' light grid, alias models, effects and the checks' records.
 *
 * Shaders are compiled with -DVKPT_SHADER. A shader that uses the global
 * UBO, the render-target images or vertex_buffer.h's functions defines
 * GLOBAL_UBO_DESC_SET_IDX, GLOBAL_TEXTURES_DESC_SET_IDX and
 * VERTEX_BUFFER_DESC_SET_IDX before including any of these headers.
 *
 * Copyright (C) 2018 Christoph Schied
 * Copyright (C) 2019-2021, NVIDIA CORPORATION. All rights reserved.
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

#ifndef HL_SHARED_H
#define HL_SHARED_H

#include "shader_structs.h"
#include "constants.h"
#include "global_ubo.h"
#include "vertex_buffer.h"


/* ==========================================================================
 * PVS buffer: a header of PVS_HEADER_UINTS uints ([0] number of clusters,
 * [1] row size in uints, the rest unused), then one row of bits per
 * cluster: bit c of row r is set when cluster r can see cluster c. A
 * cluster is a vis leaf (leaf number - 1). See pvs.glsl.
 * ========================================================================== */

#define PVS_HEADER_UINTS		4


/* ==========================================================================
 * The liquids' light grid (6.18, vk_medium.c; read by medium.glsl): GL's
 * light level of a model at the points of a lattice (point (i, j, k) at
 * (i, j, k) * the cell size, world units) in boxes around the liquid
 * leaves, by light style. In uvec2s: the header, [0] the number of boxes
 * and the cell size (a float's bits), [1] the number of points and 0; then
 * MEDIUM_BOX_UVEC2S per box: (lo.x, lo.y), (lo.z, the index of its first
 * point's uvec2), (size.x, size.y), (size.z, 0), lo the lattice index of its
 * first point (ints), size its points along each axis (x fastest, then y,
 * then z); then the points: .x four light styles, a byte each
 * (MEDIUM_NO_STYLE: none), .y their levels at style value 1, a byte each
 * in the same order (GL's light map units: unpackUnorm4x8 * 255).
 * ========================================================================== */

#define MEDIUM_GRID_HEADER_UVEC2S	2
#define MEDIUM_BOX_UVEC2S		4
#define MEDIUM_NO_STYLE			255
#define MEDIUM_MIN_LEVEL		24.0	/* GL's least light on a model (R_DrawViewModel) */

/* medium_check.comp (vk_medium check): the grid's level at points,
 * MEDIUM_CHECK_CHAIN in a row by one invocation, carrying the box hint */
#define MEDIUM_CHECK_CHAIN		4
BEGIN_SHADER_STRUCT( MediumCheckPush )
{
	DeviceAddress grid;
	DeviceAddress styles;	/* float[MEDIUM_LIGHT_STYLES]: the light styles' values */
	DeviceAddress points;	/* vec4[]: the points (xyz) */
	DeviceAddress results;	/* float[]: medium_grid_level there, < 0 outside the grid */
	uint num_points;
	uint pad0;
	uint pad1;
	uint pad2;
}
END_SHADER_STRUCT( MediumCheckPush )


/* ==========================================================================
 * Alias models (vk_model.c). The model table has one AliasModel per model
 * (index = source_buffer_idx - VERTEX_BUFFER_FIRST_MODEL); each model's
 * buffer holds its triangles and its poses. A pose vertex is Quake's
 * trivertx_t in one uint: x | y << 8 | z << 16 | normal index << 24, the
 * normal index into Quake's 162 vertex normals (anorms.h). Every frame,
 * model_geometry.comp turns the alias instances into VboPrimitives in
 * VERTEX_BUFFER_INSTANCED.
 * ========================================================================== */

#define MAX_ALIAS_MODELS		1024
#define MAX_INSTANCED_PRIMITIVES	262144	/* model triangles per frame */
#define NUM_VERTEX_NORMALS		162

BEGIN_SHADER_STRUCT( AliasModel )
{
	vec3 scale;		/* pose vertex to model space: v * scale + scale_origin */
	uint num_tris;
	vec3 scale_origin;
	uint num_pose_verts;	/* vertices per pose */
	DeviceAddress triangles;	/* AliasTriangle[num_tris] */
	DeviceAddress poses;		/* uint[poses * num_pose_verts] */
}
END_SHADER_STRUCT( AliasModel )

BEGIN_SHADER_STRUCT( AliasTriangle )
{
	uvec2 verts;		/* pose vertices: x = v0 | v1 << 16, y = v2 */
	vec2 uv0;
	vec2 uv1;
	vec2 uv2;
}
END_SHADER_STRUCT( AliasTriangle )

/* model_geometry.comp's push constants */
BEGIN_SHADER_STRUCT( ModelGeometryPush )
{
	DeviceAddress instances;	/* ModelInstance[] */
	DeviceAddress models;		/* AliasModel[] */
	DeviceAddress normals;		/* vec4[NUM_VERTEX_NORMALS] */
	DeviceAddress primitives;	/* VboPrimitive[], the instanced buffer */
	DeviceAddress positions;	/* float[9 per primitive], for the BLASes */
	uint first_instance;		/* the first alias instance; one workgroup each */
	uint pad;
}
END_SHADER_STRUCT( ModelGeometryPush )


/* ==========================================================================
 * Effects (vk_effects.c): the frame's particles and sprites, written by the
 * CPU into one buffer per frame in flight, laid out like Quake II RTX's
 * transparency.c: the vertex positions (vec3: 3 per particle, one
 * triangle as GL draws it; then 4 per sprite, a quad whose two triangles
 * are vertices 0 1 2 and 2 3 0 of a shared uint16 index buffer), an
 * EffectParticle per particle and an EffectSprite per sprite. They are
 * ray traced through a second, effects-only TLAS (Quake II RTX's
 * TLAS_INDEX_EFFECTS), whose instance masks are their own namespace
 * (AS_FLAG_EFFECTS); the instances' shader binding table offsets
 * (SBTO_PARTICLE, SBTO_SPRITE) and custom indices tell particles from
 * sprites. Particle i is primitive i of its BLAS, sprite i primitives 2i
 * and 2i + 1 of its own.
 * ========================================================================== */

#define MAX_EFFECT_PARTICLES		32768	/* r_part.c has 7000 unless -particles N */
#define MAX_EFFECT_SPRITES		1024	/* over r_scene.h's MAX_SCENE_ENTITIES */

#define EFFECTS_PARTICLES		0	/* the effects TLAS instances' custom index */
#define EFFECTS_SPRITES			1

BEGIN_SHADER_STRUCT( EffectParticle )
{
	vec3 color;		/* linear; GL's glColor */
	uint alpha_and_uvs;	/* half float alpha | GL's texture coordinate set (ptex_coord) << 16 */
}
END_SHADER_STRUCT( EffectParticle )

BEGIN_SHADER_STRUCT( EffectSprite )
{
	uint texture;		/* texture slot of the frame: its replaced albedo (5.3), or the original */
	float alpha;		/* multiplies the texture's */
	uint original;		/* the original frame's slot when replaced (its size, a texel per unit, sets the mip level), 0 = not */
	uint coverage;		/* 1: the original's alpha is the coverage (the albedo has none) */
}
END_SHADER_STRUCT( EffectSprite )


/* effects_check.comp (vk_effects check): one ray from the camera towards a
 * point inside an effect triangle, which the effects TLAS should report */
BEGIN_SHADER_STRUCT( EffectsCheckRay )
{
	vec3 dir;
	float tmax;		/* a little beyond the point */
	uint custom;		/* the triangle: EFFECTS_* */
	uint primitive;		/* in its BLAS */
	uint pad0;
	uint pad1;
}
END_SHADER_STRUCT( EffectsCheckRay )

BEGIN_SHADER_STRUCT( EffectsCheckResult )
{
	float t;		/* where the ray met the triangle, < 0: not reported */
	uint candidates;	/* all candidates the ray met */
}
END_SHADER_STRUCT( EffectsCheckResult )

BEGIN_SHADER_STRUCT( EffectsCheckPush )
{
	DeviceAddress tlas;
	DeviceAddress rays;	/* EffectsCheckRay[] */
	DeviceAddress results;	/* EffectsCheckResult[] */
	uint num_rays;
	uint pad;
	vec4 origin;		/* the camera */
}
END_SHADER_STRUCT( EffectsCheckPush )


/* ==========================================================================
 * The debug view (vk_view.c, debug_view.comp): r_debugview's modes, in the
 * global UBO's debug_view: the lit image (0) or G-buffer and lighting channels
 * ========================================================================== */

#define DEBUGVIEW_LIT			0	/* the path tracer's image (the TAA pass's, vk_upscale.c; not debug_view.comp's) */
#define DEBUGVIEW_ALBEDO		1	/* base color, the effects over it */
#define DEBUGVIEW_NORMALS		2	/* shading normal */
#define DEBUGVIEW_MATERIAL		3	/* material kinds */
#define DEBUGVIEW_INSTANCES		4
#define DEBUGVIEW_CLUSTERS		5	/* and the camera's PVS */
#define DEBUGVIEW_MOTION		6	/* screen-space motion vectors */
#define DEBUGVIEW_MOTION_CHECK		7	/* base color minus last frame's at the motion vector */
#define DEBUGVIEW_GEO_NORMALS		8
#define DEBUGVIEW_DEPTH			9	/* view depth */
#define DEBUGVIEW_ROUGHNESS		10	/* roughness, metallic, specular factor as R, G, B */
#define DEBUGVIEW_DIFFUSE_ALBEDO	11	/* DLSS Ray Reconstruction's, from the G-buffer */
#define DEBUGVIEW_SPECULAR_ALBEDO	12	/* the same */
#define DEBUGVIEW_EFFECTS		13	/* effects and emissive (PT_TRANSPARENT) */
#define DEBUGVIEW_BLUE_NOISE		14	/* the first random number of the pixel */
#define DEBUGVIEW_DIRECT_DIFFUSE	15	/* direct lighting, diffuse (PT_COLOR_HF, demodulated) */
#define DEBUGVIEW_DIRECT_SPECULAR	16	/* specular lighting, direct and bounced (PT_COLOR_SPEC) */
#define DEBUGVIEW_LIGHT_LISTS		17	/* the length of the pixel's cluster's light list */
#define DEBUGVIEW_INDIRECT_DIFFUSE	18	/* indirect lighting, diffuse (PT_COLOR_LF_SH, demodulated) */
#define DEBUGVIEW_SPECULAR_HIT_DIST	19	/* the specular bounce's hit distance (PT_SPECULAR_HIT_DIST) */
#define DEBUGVIEW_HISTORY		20	/* the denoiser's history length (ASVGF_HIST_MOMENTS_HF) */
#define DEBUGVIEW_MAX			20

/* the modes that read the lighting: debug_view.comp (the TAA pass for the
 * lit image) runs after the indirect
 * passes (and the denoiser) for them, before for the others (vk_view.c:
 * with two bounces, the first stores its hit into the G-buffer's shading
 * position for the second) */
#define DEBUGVIEW_READS_LIGHTING(m)	((m) == DEBUGVIEW_LIT || (m) == DEBUGVIEW_DIRECT_DIFFUSE || \
					 (m) == DEBUGVIEW_DIRECT_SPECULAR || (m) == DEBUGVIEW_INDIRECT_DIFFUSE || \
					 (m) == DEBUGVIEW_SPECULAR_HIT_DIST || (m) == DEBUGVIEW_HISTORY)


/* ==========================================================================
 * DLSS (vk_dlss.c, dlss_inputs.glsl): the depth range of the D3D-style
 * projection DLSS gets, from GL's near plane to past the longest sight line
 * in a Hexen II map (its coordinates stay within +-4096: under 14190)
 * ========================================================================== */

#define DLSS_Z_NEAR			4.0
#define DLSS_Z_FAR			16384.0

#endif	/* HL_SHARED_H */
