/* hl_shared.h -- definitions shared by the Hexenlicht renderer (C) and its
 * shaders (GLSL): Quake II RTX's headers (constants.h, the primitive
 * record in vertex_buffer.h, ModelInstance and the global UBO in
 * global_ubo.h), and Hexenlicht's own GPU data: the PVS buffer, alias
 * models, effects and the checks' records.
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
	uint texture;		/* texture slot of the frame */
	float alpha;		/* multiplies the texture's */
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
 * global UBO's debug_view
 * ========================================================================== */

#define DEBUGVIEW_OFF			0
#define DEBUGVIEW_ALBEDO		1
#define DEBUGVIEW_NORMALS		2
#define DEBUGVIEW_MATERIAL		3
#define DEBUGVIEW_INSTANCES		4
#define DEBUGVIEW_CLUSTERS		5
#define DEBUGVIEW_MOTION		6
#define DEBUGVIEW_MAX			6

#endif	/* HL_SHARED_H */
