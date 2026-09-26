/*
Copyright (C) 2020-2021, NVIDIA CORPORATION. All rights reserved.
Copyright (C) 2021 Frank Richter
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

/* Hexenlicht: the logic of Quake II RTX's hit shaders, which its ray
 * query code calls for each candidate by the instance's shader binding
 * table offset (SBTO_*, set in vk_accel.c), with these changes:
 *  - a TLAS instance's model instance and first primitive come from its
 *    TlasInstanceInfo (shaders/global_ubo.h), the effects from their
 *    buffers by device address (vk_effects.c);
 *  - cutouts are alpha tested against the mask texture's alpha (a model
 *    skin is its own mask, vk_skin.c) instead of its red channel;
 *  - particles and sprites look as GL draws them, unlit: a particle is one
 *    triangle with GL's dot texture and texture coordinates
 *    (r_part.c's ptex_coord), a sprite a quad sampled at the mip level of
 *    the pixel's footprint and clamped at its edges; both are scaled by
 *    the exposure (effects_brightness, 3.7) as Quake II RTX's particles
 *    are, with one factor for both;
 *  - beams and explosions come with their stories (6.3). */

#include "hl_shared.h"

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer EffectParticleBufferRef {
	EffectParticle particles[];
};

layout(buffer_reference, std430, buffer_reference_align = 8) readonly buffer EffectSpriteBufferRef {
	EffectSprite sprites[];
};

void get_model_index_and_prim_offset(int instanceID, int geometryIndex, out int model_index, out uint prim_offset)
{
	// Hexenlicht: each TLAS instance's BLAS has one geometry, its model
	// instance and first primitive are in its TlasInstanceInfo
	TlasInstanceInfo info = tlas_instance_info[instanceID];
	model_index = info.model_instance;
	prim_offset = info.prim_offset;
}

void pt_logic_rchit(inout RayPayloadGeometry ray_payload, int primitiveID, int instanceID, int geometryIndex, uint instanceCustomIndex, float hitT, vec2 bary)
{
	int model_index;
	uint prim_offset;
	get_model_index_and_prim_offset(instanceID, geometryIndex, model_index, prim_offset);

	ray_payload.barycentric = bary.xy;
	ray_payload.primitive_id = primitiveID + prim_offset;
	ray_payload.buffer_and_instance_idx = (int(instanceCustomIndex) & 0xffff)
	                                    | (model_index << 16);
	ray_payload.hit_distance = hitT;
}

bool pt_logic_masked(int primitiveID, int instanceID, int geometryIndex, uint instanceCustomIndex, vec2 bary)
{
	int model_index;
	uint prim_offset;
	get_model_index_and_prim_offset(instanceID, geometryIndex, model_index, prim_offset);

	uint prim = primitiveID + prim_offset;
	uint buffer_idx = instanceCustomIndex;

	Triangle triangle = load_and_transform_triangle(model_index, buffer_idx, prim);

	MaterialInfo minfo = get_material_info(triangle.material_id);

	if (minfo.mask_texture == 0)
		return true;
	
	vec2 tex_coord = triangle.tex_coords * vec3(1.0 - bary.x - bary.y, bary.x, bary.y);

	perturb_tex_coord(triangle.material_id, global_ubo.time, tex_coord);	

	vec4 mask_value = global_textureLod(minfo.mask_texture, tex_coord, /* mip_level = */ 0);

	return mask_value.a >= 0.5;	// Hexenlicht: alpha, Quake II RTX tests .x
}

/* Hexenlicht: GL's texture coordinates of a particle's triangle (r_part.c's ptex_coord) */
const vec2 particle_uvs[12] = vec2[](
	vec2(1.000, 0.000), vec2(1.000, 0.500), vec2(0.500, 0.000),	/* any, or snow count < 30 */
	vec2(0.000, 1.000), vec2(0.500, 1.000), vec2(0.000, 0.500),	/* snow count >= 30 */
	vec2(0.000, 0.000), vec2(0.815, 0.000), vec2(0.000, 0.815),	/* snow count >= 40 */
	vec2(1.000, 1.000), vec2(1.000, 0.180), vec2(0.180, 1.000));	/* snow count >= 69: happy snow! */

/* Hexenlicht: GL draws particles and sprites unlit, at their colors. Under
 * the tone mapper's exposure (3.7) they are scaled by the adapted luminance
 * so that they show at about those colors whatever the exposure (Quake II
 * RTX's particles: prev_adapted_luminance times pt_particle_brightness;
 * its sprites, beams and explosions have their own factors): 1 without the
 * tone mapping and in the debug views */
float effects_brightness()
{
	if(global_ubo.tm_enable == 0 || global_ubo.debug_view != DEBUGVIEW_LIT)
		return 1.0;
	return global_ubo.prev_adapted_luminance * global_ubo.pt_particle_brightness;
}

/* Hexenlicht: a particle's premultiplied color where the ray met it: GL's
 * color times its dot texture, times the effects' brightness */
vec4 pt_logic_particle(int primitiveID, vec2 bary)
{
	EffectParticle p = EffectParticleBufferRef(global_ubo.particles).particles[primitiveID];
	uint set = (p.alpha_and_uvs >> 16) * 3u;
	vec2 uv = particle_uvs[set] * (1.0 - bary.x - bary.y) + particle_uvs[set + 1u] * bary.x + particle_uvs[set + 2u] * bary.y;
	float a = unpackHalf2x16(p.alpha_and_uvs).x * global_textureLod(global_ubo.particle_texture, uv, 0).a;
	return vec4(p.color * (a * effects_brightness()), a);
}

/* Hexenlicht: a sprite's premultiplied color where the ray met its quad at
 * distance hitT: the texture, unlit (Quake II RTX's without its tone curve),
 * at the mip level of the pixel's footprint (sprite frames have a texel per
 * unit) and clamped at the edges as GL does, times the effects' brightness */
vec4 pt_logic_sprite(int primitiveID, vec2 bary, float hitT)
{
	const vec3 barycentric = vec3(1.0 - bary.x - bary.y, bary.x, bary.y);
	
	vec2 uv;
	if((primitiveID & 1) == 0)
	   uv = vec2(0.0, 1.0) * barycentric.x + vec2(0.0, 0.0) * barycentric.y + vec2(1.0, 0.0) * barycentric.z;
	else
	   uv = vec2(1.0, 0.0) * barycentric.x + vec2(1.0, 1.0) * barycentric.y + vec2(0.0, 1.0) * barycentric.z;

	const int sprite_index = primitiveID / 2;

	EffectSprite s = EffectSpriteBufferRef(global_ubo.sprites).sprites[sprite_index];

	/* the pixel's footprint at hitT: P[1][1] is 1 / tan(fov_y / 2), negated */
	float lod = max(log2(hitT * 2.0 / (abs(global_ubo.P[1][1]) * float(global_ubo.height))), 0.0);
	/* half a texel of the coarser of the two levels trilinear filtering
	 * reads, so neither wraps around (the sampler repeats) */
	int level = min(int(ceil(lod)), global_textureQueryLevels(s.texture) - 1);
	vec2 half_texel = min(0.5 / vec2(global_textureSize(s.texture, level)), vec2(0.5));
	vec4 color = global_textureLod(s.texture, clamp(uv, half_texel, 1.0 - half_texel), lod);

	color.a *= s.alpha;
	return vec4(color.rgb * (color.a * effects_brightness()), color.a);
}
