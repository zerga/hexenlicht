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

#else	/* C */

#include <stdint.h>

#define BEGIN_SHADER_STRUCT(NAME)	typedef struct
#define END_SHADER_STRUCT(NAME)		NAME;

typedef uint32_t	uint;
typedef float		vec2[2];
typedef float		vec3[3];
typedef float		vec4[4];
typedef uint32_t	uvec2[2];
typedef uint32_t	uvec3[3];
typedef uint32_t	uvec4[4];

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

#endif	/* HL_SHARED_H */
