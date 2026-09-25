/* hl_shared.h -- definitions shared by the Hexenlicht renderer (C) and its
 * shaders (GLSL): the per-triangle primitive record, material ID bits and
 * the material table layout.
 *
 * The primitive record, the material ID bits and the material table
 * layout follow Quake II RTX (src/refresh/vkpt/shader/vertex_buffer.h,
 * constants.h, shader_structs.h), so its shaders can read our buffers.
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

/* glslang defines VULKAN when compiling a shader for Vulkan */
#ifdef VULKAN

#define BEGIN_SHADER_STRUCT(NAME)	struct NAME
#define END_SHADER_STRUCT(NAME)		;

/* a buffer device address: uvec2 needs no 64-bit integers in shaders
 * (GL_EXT_buffer_reference_uvec2 turns it into a buffer reference) */
#define DeviceAddress			uvec2

#else	/* C */

#include <stdint.h>

#define BEGIN_SHADER_STRUCT(NAME)	typedef struct NAME	/* tagged, so C headers can declare pointers to it */
#define END_SHADER_STRUCT(NAME)		NAME;

typedef uint32_t	uint;
typedef float		vec2[2];
typedef float		vec3[3];
typedef float		vec4[4];
typedef uint32_t	uvec2[2];
typedef uint32_t	uvec3[3];
typedef uint32_t	uvec4[4];
typedef float		mat4[4][4];	/* [column][row], as GLSL stores it */
typedef uint64_t	DeviceAddress;

#endif	/* VULKAN */


/* ==========================================================================
 * Material IDs: kind | flags | light style | index into the material table
 * ========================================================================== */

#define MATERIAL_KIND_MASK		0xf0000000
#define MATERIAL_KIND_INVALID		0x00000000
#define MATERIAL_KIND_REGULAR		0x10000000
#define MATERIAL_KIND_CHROME		0x20000000
#define MATERIAL_KIND_WATER		0x30000000
#define MATERIAL_KIND_LAVA		0x40000000
#define MATERIAL_KIND_SLIME		0x50000000
#define MATERIAL_KIND_GLASS		0x60000000
#define MATERIAL_KIND_SKY		0x70000000
#define MATERIAL_KIND_INVISIBLE		0x80000000
#define MATERIAL_KIND_EXPLOSION		0x90000000
#define MATERIAL_KIND_TRANSPARENT	0xa0000000	/* see-through surfaces, e.g. *rtex078 */
#define MATERIAL_KIND_SCREEN		0xb0000000
#define MATERIAL_KIND_CAMERA		0xc0000000
#define MATERIAL_KIND_CHROME_MODEL	0xd0000000
#define MATERIAL_KIND_TRANSP_MODEL	0xe0000000

#define MATERIAL_FLAG_LIGHT		0x08000000
#define MATERIAL_FLAG_HANDEDNESS	0x02000000	/* the bitangent is -cross(normal, tangent) */
#define MATERIAL_FLAG_WEAPON		0x01000000
#define MATERIAL_FLAG_WARP		0x00800000	/* turbulent (*) surface: warped texture coordinates */
#define MATERIAL_FLAG_FLOWING		0x00400000
#define MATERIAL_FLAG_DOUBLE_SIDED	0x00200000

#define MATERIAL_LIGHT_STYLE_MASK	0x0003f000
#define MATERIAL_LIGHT_STYLE_SHIFT	12
#define MATERIAL_INDEX_MASK		0x00000fff

#define MAX_MATERIALS			4096	/* MATERIAL_INDEX_MASK + 1; index 0 is unused */

/* One material is MATERIAL_UINTS uints in the material table:
 *   [0] base texture | normal map << 16          (texture slots, 0 = none;
 *   [1] emissive texture | mask texture << 16      the base falls back to white)
 *   [2] half2 (bump scale, roughness override)
 *   [3] half2 (metalness factor, emissive factor)
 *   [4] number of animation frames | next frame's material << 16
 *   [5] half2 (specular factor, base factor)
 *   [6] Hexen II: material of the alternate animation (+a..+j), 0 = none
 *   [7] unused
 * [0]-[5] are Quake II RTX's layout. */
#define MATERIAL_UINTS			8


/* ==========================================================================
 * PVS buffer: a header of PVS_HEADER_UINTS uints ([0] number of clusters,
 * [1] row size in uints, the rest unused), then one row of bits per
 * cluster: bit c of row r is set when cluster r can see cluster c. A
 * cluster is a vis leaf (leaf number - 1). See pvs.glsl.
 * ========================================================================== */

#define PVS_HEADER_UINTS		4


/* ==========================================================================
 * One triangle, 128 bytes (Quake II RTX's VboPrimitive)
 * ========================================================================== */

BEGIN_SHADER_STRUCT( VboPrimitive )
{
	vec3 pos0;
	uint material_id;	/* MATERIAL_KIND_* | MATERIAL_FLAG_* | material index */

	vec3 pos1;
	int cluster;		/* Hexen II: vis leaf (leaf number - 1), -1 = none */

	vec3 pos2;
	uint shell;		/* unused */

	uvec3 normals;		/* octahedral encoding, see encode_normal */
	uint instance;

	uvec3 tangents;
	uint emissive_and_alpha;	/* half2 (emissive factor, alpha) */

	vec2 uv0;
	vec2 uv1;
	vec2 uv2;
	uvec2 custom0;		/* motion or skinning data of instanced meshes */
	uvec2 custom1;
	uvec2 custom2;
}
END_SHADER_STRUCT( VboPrimitive )


/* ==========================================================================
 * Model instances: the frame's entities with geometry, 208 bytes each.
 * Quake II RTX's ModelInstance, with Hexen II's fields at the end.
 * ========================================================================== */

#define MAX_MODEL_INSTANCES		1024

/* primitive buffers (source_buffer_idx, render_buffer_idx, the TLAS
 * instances' custom index), numbered like Quake II RTX's */
#define VERTEX_BUFFER_WORLD		0	/* the world buffer (vk_world.c) */
#define VERTEX_BUFFER_INSTANCED		1	/* this frame's alias model triangles (vk_model.c) */
#define VERTEX_BUFFER_FIRST_MODEL	2	/* alias model k (source only): VERTEX_BUFFER_FIRST_MODEL + k */

BEGIN_SHADER_STRUCT( ModelInstance )
{
	mat4 transform;		/* model to world */
	mat4 transform_prev;	/* the same, last frame */

	uint material;		/* alias models: material ID of every triangle; unused for brush models */
	uint shell;		/* unused */
	int cluster;		/* vis cluster the model is in, -1 = none */
	uint source_buffer_idx;	/* VERTEX_BUFFER_* with the primitives, or the alias model */
	uint prim_count;

	/* Alias models: the first vertex of each pose in the model's pose
	 * data. The frame blends the current pose with the previous one by
	 * pose_lerp_curr_frame (the previous pose's weight, Quake II's
	 * backlerp); the _prev_frame fields are what the last frame showed. */
	uint prim_offset_curr_pose_curr_frame;
	uint prim_offset_prev_pose_curr_frame;
	uint prim_offset_curr_pose_prev_frame;
	uint prim_offset_prev_pose_prev_frame;

	float pose_lerp_curr_frame;
	float pose_lerp_prev_frame;
	int iqm_matrix_offset_curr_frame;	/* unused, -1 */
	int iqm_matrix_offset_prev_frame;

	/* half float alpha (low 16 bits) | entity frame << 16; for brush
	 * entities, a frame other than 0 shows the alternate animations
	 * (alias models: frame 0) */
	uint alpha_and_frame;
	uint render_buffer_idx;
	uint render_prim_offset;	/* first primitive in render_buffer_idx */

	/* Hexen II */
	uint drawflags;		/* the entity's MLS_*, SCALE_*, DRF_* bits */
	float abslight;		/* brightness for MLS_ABSLIGHT, 0-1 */
	uint entity;		/* scene_entkind_t << 16 | entity number, for debugging */
	uint pad;
}
END_SHADER_STRUCT( ModelInstance )


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
 * The top-level acceleration structure (vk_accel.c). Instance masks are
 * Quake II RTX's; each TLAS instance has a TlasInstanceInfo at its index
 * (rayQueryGetIntersectionInstanceIdEXT): the first primitive of its BLAS
 * in the buffer named by its custom index (VERTEX_BUFFER_*), and its model
 * instance, -1 for the world and for the alias model triangles of
 * VERTEX_BUFFER_INSTANCED, whose VboPrimitive.instance names it.
 * ========================================================================== */

#define MAX_TLAS_INSTANCES		4096

#define AS_FLAG_OPAQUE			(1 << 0)
#define AS_FLAG_TRANSPARENT		(1 << 1)
#define AS_FLAG_VIEWER_MODELS		(1 << 2)
#define AS_FLAG_VIEWER_WEAPON		(1 << 3)
#define AS_FLAG_SKY			(1 << 4)
#define AS_FLAG_CUSTOM_SKY		(1 << 5)

BEGIN_SHADER_STRUCT( TlasInstanceInfo )
{
	uint prim_offset;
	int model_instance;
}
END_SHADER_STRUCT( TlasInstanceInfo )


/* ==========================================================================
 * The 3D view (vk_view.c): what the view passes read, one per frame, 152
 * bytes, found through the push constant's address
 * ========================================================================== */

/* r_debugview */
#define DEBUGVIEW_OFF			0
#define DEBUGVIEW_ALBEDO		1
#define DEBUGVIEW_NORMALS		2
#define DEBUGVIEW_MATERIAL		3
#define DEBUGVIEW_INSTANCES		4
#define DEBUGVIEW_CLUSTERS		5
#define DEBUGVIEW_MOTION		6
#define DEBUGVIEW_MAX			6

BEGIN_SHADER_STRUCT( ViewUniforms )
{
	vec4 origin;		/* camera position, w unused */
	vec4 forward;
	vec4 right;
	vec4 up;
	vec2 tan_half_fov;	/* x, y */
	uvec2 size;		/* view image, pixels */
	float time;		/* cl.time */
	int anim_frame;		/* int(cl.time * 5), for animate_material */
	uint debug_mode;	/* DEBUGVIEW_* */
	int view_cluster;	/* the camera's cluster, -1 = none */

	DeviceAddress tlas;
	DeviceAddress primitives;	/* the world buffer's VboPrimitives */
	DeviceAddress tlas_info;	/* TlasInstanceInfo[] */
	DeviceAddress instances;	/* ModelInstance[] */
	DeviceAddress materials;	/* the material table */
	DeviceAddress pvs;		/* the PVS buffer */
	DeviceAddress instanced;	/* this frame's VERTEX_BUFFER_INSTANCED VboPrimitives */
}
END_SHADER_STRUCT( ViewUniforms )

#endif	/* HL_SHARED_H */
