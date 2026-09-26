/*
Copyright (C) 2018 Christoph Schied
Copyright (C) 2019-2021, NVIDIA CORPORATION. All rights reserved.
Copyright (C) 2026  Hexenlicht contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

/* Hexenlicht: Quake II RTX's primitive record and the functions its
 * shaders read primitives and materials with, over our buffers:
 *  - the buffers are read by device address from the global UBO
 *    (shaders/global_ubo.h) instead of descriptors: the world buffer
 *    (vk_world.c), this frame's instanced buffer (vk_model.c), the material
 *    table (vk_material.c);
 *  - the functions are only declared when the shader defines
 *    VERTEX_BUFFER_DESC_SET_IDX (the value is unused), after global_ubo.h
 *    and utils.glsl, so other shaders can include the structs;
 *  - a material has Hexen II's alternate animation (MATERIAL_UINTS 8) and
 *    animate_material follows R_TextureAnimation: materials animate with
 *    time (global_ubo.anim_frame), and brush entities whose frame is not 0
 *    show the alternate animation; Quake II RTX animates the world buffer
 *    with animate_materials.comp and steps instances by their frame;
 *  - the light buffer (vk_light.c) has only the lights and the light lists
 *    (3.3); a light is a polygon or, for Hexen II's point lights, a sphere
 *    (3.4); Quake II RTX's also holds the material table (ours is
 *    vk_material.c's), and the light styles of emissive materials, the
 *    cluster debug mask and the sky visibility come with their stories;
 *  - the light statistics are counted per light list entry (3.4), by
 *    device address; Quake II RTX's per cluster and light;
 *  - left out until their passes: the light count history, the IQM
 *    matrices, the tone mapping, readback and sun color buffers,
 *    store_triangle (model_geometry.comp writes the instanced buffer). */

#ifndef _VERTEX_BUFFER_H_
#define _VERTEX_BUFFER_H_

#include "shader_structs.h"

/* Hexenlicht: the material table (vk_material.c), MATERIAL_UINTS uints per material:
 *   [0] base texture | normal map << 16          (texture slots, 0 = none;
 *   [1] emissive texture | mask texture << 16      the base falls back to white)
 *   [2] half2 (bump scale, roughness override)
 *   [3] half2 (metalness factor, emissive factor)
 *   [4] number of animation frames | next frame's material << 16
 *   [5] half2 (specular factor, base factor)
 *   [6] Hexen II: material of the alternate animation (+a..+j), 0 = none
 *   [7] unused
 * [0]-[5] are Quake II RTX's layout. */
#define MATERIAL_UINTS          8

// should match the same constant declared in material.h
#define MAX_PBR_MATERIALS      4096	// Hexenlicht: MATERIAL_INDEX_MASK + 1; index 0 is unused

#define MAX_LIGHT_LISTS         (1 << 14)
#define MAX_LIGHT_LIST_NODES    (1 << 19)

#define MAX_LIGHT_POLYS         4096
#define LIGHT_POLY_VEC4S        4

// Hexenlicht: a light's type (see LightBuffer), and the light statistics:
// shadowed and unshadowed rays for each of the 6 primary directions of the
// receiving surface's normal, per light list entry
#define LIGHT_TYPE_POLYGON      0
#define LIGHT_TYPE_SPHERE       1
#define LIGHT_STATS_UINTS       12

#define VERTEX_BUFFER_WORLD 0		// Hexenlicht: the world buffer (vk_world.c)
#define VERTEX_BUFFER_INSTANCED 1	// this frame's alias model triangles (vk_model.c)
#define VERTEX_BUFFER_FIRST_MODEL 2	// alias model k (source only): VERTEX_BUFFER_FIRST_MODEL + k

// A structure that is used in primitive buffers to store complete information about one triangle. 
// Its size is 8x float4 or 128 bytes to align with GPU cache lines.
// Path tracing accesses the primitive information in a very incoherent way, where every thread
// is likely to read a different primitive. Packing the info into one struct should reduce the
// total traffic from video memory by reading entire cache lines instead of sparse values from
// different buffers.
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
	/* packed half2x16, with emissive in x component/low 16 bits and
	 * alpha in y component/high 16 bits */
	uint emissive_and_alpha;

	vec2 uv0;
	vec2 uv1;
	vec2 uv2;
	uvec2 custom0;  // The custom fields store motion for instanced meshes in the animated buffer,
	uvec2 custom1;  // or blend indices and weights for skinned meshes before they're animated.
	uvec2 custom2;
}
END_SHADER_STRUCT( VboPrimitive )

/* Hexenlicht: Quake II RTX's light buffer without its material table, light
 * styles, cluster debug mask and sky visibility (see the top): a light is
 * LIGHT_POLY_VEC4S vec4s,
 *  - a polygon: the three corners with the color (radiance) in their w,
 *    then (style scale, last frame's style scale, LIGHT_TYPE_POLYGON, 0);
 *  - a sphere: (center, red), (radius, range, 0, green), (0, 0, 0, blue),
 *    then (style scale, last frame's, LIGHT_TYPE_SPHERE, 0); range 0 =
 *    unlimited, else its light fades to 0 there (sphere_light_window);
 * light list n, the lights of vis cluster n, is
 * light_list_lights[light_list_offsets[n]] up to light_list_offsets[n + 1] */
BEGIN_SHADER_STRUCT( LightBuffer )
{
	vec4 light_polys[MAX_LIGHT_POLYS * LIGHT_POLY_VEC4S];
	uint light_list_offsets[MAX_LIGHT_LISTS];
	uint light_list_lights[MAX_LIGHT_LIST_NODES];
}
END_SHADER_STRUCT( LightBuffer )


#if defined(VKPT_SHADER) && defined(VERTEX_BUFFER_DESC_SET_IDX)

#ifdef VERTEX_READONLY
#define VERTEX_READONLY_FLAG readonly
#else
#define VERTEX_READONLY_FLAG
#endif

struct MaterialInfo
{
	uint base_texture;
	uint normals_texture;
	uint emissive_texture;
	uint mask_texture;
	float bump_scale;
	float roughness_override;
	float metalness_factor;
	float emissive_factor;
	float specular_factor;
	float base_factor;
	float light_style_scale;
	uint num_frames;
	uint next_frame;
	uint alternate;		/* Hexenlicht: first material of the alternate animation, 0 = none */
};

/* Hexenlicht: the primitive buffers and the material table by device address */
layout(buffer_reference, std430, buffer_reference_align = 16) VERTEX_READONLY_FLAG buffer PrimitiveBufferRef {
	VboPrimitive primitives[];
};

layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer MaterialTableRef {
	uint material_table[];
};

/* Hexenlicht: this frame's light buffer (vk_light.c) by device address */
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer LightBufferRef {
	LightBuffer light_buffer_data;
};
#define light_buffer LightBufferRef(global_ubo.lights).light_buffer_data

/* Hexenlicht: the light statistics (vk_light.c), LIGHT_STATS_UINTS per list
 * entry, by device address: global_ubo.light_stats is counted this frame,
 * light_stats_prev (last frame's) and light_stats_prev2 are read */
layout(buffer_reference, std430, buffer_reference_align = 4) buffer LightStatsRef {
	uint stats[];
};

struct LightPolygon
{
	mat3 positions;
	vec3 color;
	float light_style_scale;
	float prev_style_scale;
	uint type;		/* Hexenlicht: LIGHT_TYPE_*; a sphere: positions[0] center, [1].x radius, [1].y range */
};

VboPrimitive
get_primitive(uint buffer_idx, uint prim_id)
{
	DeviceAddress addr = (buffer_idx == VERTEX_BUFFER_INSTANCED) ? global_ubo.instanced_primitives
	                                                           : global_ubo.world_primitives;
	return PrimitiveBufferRef(addr).primitives[prim_id];
}

uint animate_material(uint material, int frame, bool alternate);

struct Triangle
{
	mat3x3 positions;
	mat3x3 positions_prev;
	mat3x3 normals;
	mat3x2 tex_coords;
	mat3x3 tangents;
	uint   material_id;
	uint   shell;
	int    cluster;
	uint   instance_index;
	uint   instance_prim;
	float  emissive_factor;
	float  alpha;
};

Triangle
load_triangle(uint buffer_idx, uint prim_id)
{
	VboPrimitive prim = get_primitive(buffer_idx, prim_id);

	Triangle t;
	t.positions[0] = prim.pos0;
	t.positions[1] = prim.pos1;
	t.positions[2] = prim.pos2;

	t.positions_prev[0] = t.positions[0] + unpackHalf4x16(prim.custom0).xyz;
	t.positions_prev[1] = t.positions[1] + unpackHalf4x16(prim.custom1).xyz;
	t.positions_prev[2] = t.positions[2] + unpackHalf4x16(prim.custom2).xyz;
	
	t.normals[0] = decode_normal(prim.normals.x);
	t.normals[1] = decode_normal(prim.normals.y);
	t.normals[2] = decode_normal(prim.normals.z);

	t.tangents[0] = decode_normal(prim.tangents.x);
	t.tangents[1] = decode_normal(prim.tangents.y);
	t.tangents[2] = decode_normal(prim.tangents.z);

	t.tex_coords[0] = prim.uv0;
	t.tex_coords[1] = prim.uv1;
	t.tex_coords[2] = prim.uv2;

	t.material_id = prim.material_id;
	t.shell = prim.shell;
	t.cluster = prim.cluster;
	t.instance_index = prim.instance;
	t.instance_prim = 0;
	
	vec2 emissive_and_alpha = unpackHalf2x16(prim.emissive_and_alpha);
	t.emissive_factor = emissive_and_alpha.x;
	t.alpha = emissive_and_alpha.y;

	return t;
}

Triangle
load_and_transform_triangle(int instance_idx, uint buffer_idx, uint prim_id)
{
	Triangle t = load_triangle(buffer_idx, prim_id);

	if (instance_idx >= 0)
	{
		// Instance of a static mesh: transform the vertices.

		ModelInstance mi = instance_buffer.model_instances[instance_idx];
		
		t.positions[0] = vec3(mi.transform * vec4(t.positions[0], 1.0));
		t.positions[1] = vec3(mi.transform * vec4(t.positions[1], 1.0));
		t.positions[2] = vec3(mi.transform * vec4(t.positions[2], 1.0));

		t.positions_prev[0] = vec3(mi.transform_prev * vec4(t.positions_prev[0], 1.0));
		t.positions_prev[1] = vec3(mi.transform_prev * vec4(t.positions_prev[1], 1.0));
		t.positions_prev[2] = vec3(mi.transform_prev * vec4(t.positions_prev[2], 1.0));

		t.normals[0] = normalize(vec3(mi.transform * vec4(t.normals[0], 0.0)));
		t.normals[1] = normalize(vec3(mi.transform * vec4(t.normals[1], 0.0)));
		t.normals[2] = normalize(vec3(mi.transform * vec4(t.normals[2], 0.0)));

		t.tangents[0] = normalize(vec3(mi.transform * vec4(t.tangents[0], 0.0)));
		t.tangents[1] = normalize(vec3(mi.transform * vec4(t.tangents[1], 0.0)));
		t.tangents[2] = normalize(vec3(mi.transform * vec4(t.tangents[2], 0.0)));

		if (mi.material != 0) {
			t.material_id = mi.material;
			t.shell = mi.shell;
		}
		// Hexenlicht: a brush entity whose frame is not 0 shows the alternate animation
		int frame = int(mi.alpha_and_frame >> 16);
		t.material_id = animate_material(t.material_id, global_ubo.anim_frame, frame != 0);
		t.cluster = mi.cluster;
		t.emissive_factor = 1.0;
		t.alpha *= unpackHalf2x16(mi.alpha_and_frame).x;

		// Store the index of that instance and the prim offset relative to the instance.
		t.instance_index = uint(instance_idx);
		t.instance_prim = prim_id - mi.render_prim_offset;
	}
	else if (buffer_idx == VERTEX_BUFFER_INSTANCED)
	{
		// Instance of an animated or skinned mesh, coming from the primbuf.
		// In this case, `instance_idx` is -1 because it's not a static mesh, 
		// so load the original animated instance to find out its prim offset.

		ModelInstance mi = instance_buffer.model_instances[t.instance_index];
		t.instance_prim = prim_id - mi.render_prim_offset;
		// Hexenlicht: model skins have no animation, the instance chose the skin
	}
	else if (buffer_idx == VERTEX_BUFFER_WORLD)
	{
		// Static BSP primitive.
		
		t.instance_index = ~0u;
		t.instance_prim = prim_id;
		// Hexenlicht: the world's textures animate with time
		t.material_id = animate_material(t.material_id, global_ubo.anim_frame, false);
	}

	return t;
}

MaterialInfo
get_material_info(uint material_id)
{
	uint material_index = material_id & MATERIAL_INDEX_MASK;
	
	uint data[MATERIAL_UINTS];
	for (int i = 0; i < MATERIAL_UINTS; i++)
		data[i] = MaterialTableRef(global_ubo.materials).material_table[material_index * MATERIAL_UINTS + i];

	MaterialInfo minfo;
	minfo.base_texture = data[0] & 0xffff;
	minfo.normals_texture = data[0] >> 16;
	minfo.emissive_texture = data[1] & 0xffff;
	minfo.mask_texture = data[1] >> 16;
	minfo.bump_scale = unpackHalf2x16(data[2]).x;
	minfo.roughness_override = unpackHalf2x16(data[2]).y;
	minfo.metalness_factor = unpackHalf2x16(data[3]).x;
	minfo.emissive_factor = unpackHalf2x16(data[3]).y;
	minfo.specular_factor = unpackHalf2x16(data[5]).x;
	minfo.base_factor = unpackHalf2x16(data[5]).y;
	minfo.light_style_scale = 1.0;	// Hexenlicht: light styles of emissive materials come with 4.5
	minfo.num_frames = data[4] & 0xffff;
	minfo.next_frame = (data[4] >> 16) & (MAX_PBR_MATERIALS - 1);
	minfo.alternate = data[6] & MATERIAL_INDEX_MASK;

	return minfo;
}

/* Hexenlicht: the material shown at animation frame 'frame' (int(cl.time * 5)),
 * as Hexen II's R_TextureAnimation picks it. Surfaces reference the first
 * frame of their animation; 'alternate' is set for brush entities whose
 * frame is not 0, which show the +a..+j textures instead of +0..+9. */
uint
animate_material(uint material, int frame, bool alternate)
{
	uint new_material = material & MATERIAL_INDEX_MASK;
	MaterialInfo minfo = get_material_info(new_material);

	if (alternate && minfo.alternate != 0)
	{
		new_material = minfo.alternate;
		minfo = get_material_info(new_material);
	}

	// Apply frame-based material animation: go through the linked list of materials.
	if (frame > 0 && minfo.num_frames > 1)
	{
		frame = frame % int(minfo.num_frames);

		while (frame --> 0) {
			new_material = minfo.next_frame;
			minfo = get_material_info(new_material);
		}
	}

	return new_material | (material & ~MATERIAL_INDEX_MASK); // preserve flags
}

LightPolygon
get_light_polygon(uint index)
{
	vec4 p0 = light_buffer.light_polys[index * LIGHT_POLY_VEC4S + 0];
	vec4 p1 = light_buffer.light_polys[index * LIGHT_POLY_VEC4S + 1];
	vec4 p2 = light_buffer.light_polys[index * LIGHT_POLY_VEC4S + 2];
	vec4 p3 = light_buffer.light_polys[index * LIGHT_POLY_VEC4S + 3];

	LightPolygon light;
	light.positions = mat3x3(p0.xyz, p1.xyz, p2.xyz);
	light.color = vec3(p0.w, p1.w, p2.w);
	light.light_style_scale = p3.x;
	light.prev_style_scale = p3.y;
	light.type = uint(p3.z);	// Hexenlicht
	return light;
}

#endif
#endif
