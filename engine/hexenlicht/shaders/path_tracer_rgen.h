/*
Copyright (C) 2018 Christoph Schied
Copyright (C) 2019, NVIDIA CORPORATION. All rights reserved.
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

/* Hexenlicht: Quake II RTX's common code of the path tracer's passes
 * (compiled as compute shaders with KHR_RAY_QUERY), with these changes:
 *  - the TLASes are read by device address from the global UBO instead of
 *    its descriptor array topLevelAS[TLAS_COUNT]; the render targets are
 *    set 1 (GLOBAL_TEXTURES_DESC_SET_IDX), the vertex buffers are read by
 *    device address (VERTEX_BUFFER_DESC_SET_IDX's value is unused);
 *  - env_map is Hexen II's sky (4.6, hexen2_sky) and the dome of the sky
 *    light mode, not Quake II RTX's physical sky or environment map;
 *  - trace_effects_ray: pt_logic_sprite takes the hit distance; no beams or
 *    explosions (Hexen II's beams are alias models, 6.3, its explosions
 *    sprites), no effects TLAS = no effects;
 *  - get_direct_illumination: the receiving surface's model instance, which
 *    a glowing projectile's light doesn't light (6.2, light_lists.h's
 *    dynlight_weight), and whether it is a light, which beam lights don't
 *    light (6.3); the light statistics per light list entry
 *    and the light lists' sphere lights (light_lists.h, 3.4), no shadow ray
 *    without a light (Quake II RTX's has t_max < t_min); for gradient
 *    samples the sampled list light's style change (nee_style_change, 4.13);
 *    a map light's GL-like shape (4.15) brings its own angle term instead of
 *    the cosine (the specular gets its light without it);
 *  - get_sunlight (4.6): the shadow ray ends at the first sky face it meets
 *    (trace_sky_distance: Hexen II has world geometry above some skies,
 *    which Quake II RTX's 10000-unit ray would hit; a point at the sky's
 *    face is lit without a ray), the sun's color is the UBO's (no physical
 *    sky to integrate it from), clusters past the sky visibility's bits
 *    trace it;
 *  - get_rng: clamped to the largest float below 1 (Quake II RTX's literal
 *    rounds to 1.0);
 *  - get_material: a model's colorshade tint's hue tints the base color;
 *  - the textures are UNORM (4.17): their colors become linear light here
 *    (base, emissive: transfer.glsl's color_to_linear), the normal map and
 *    the masks' alpha stay as they are;
 *  - get_emissive_shell: not scaled by the tone mapper's adapted luminance
 *    (Hexen II has no shells). */

#include "path_tracer.h"
#include "utils.glsl"
#include "transfer.glsl"
#include "path_tracer_transparency.glsl"

// Hexenlicht: the TLASes by device address (vk_accel.c), not a descriptor array
#define TLAS_GEOMETRY accelerationStructureEXT(global_ubo.tlas)
#define TLAS_EFFECTS  accelerationStructureEXT(global_ubo.effects_tlas)


#define GLOBAL_TEXTURES_DESC_SET_IDX 1	// Hexenlicht: 2 in Quake II RTX
#include "global_textures.h"

#define VERTEX_BUFFER_DESC_SET_IDX 3	// Hexenlicht: unused, the buffers are read by device address
#define VERTEX_READONLY 1
#include "vertex_buffer.h"

#include "asvgf.glsl"
#include "brdf.glsl"
#include "water.glsl"

/* RNG seeds contain 'X' and 'Y' values that are computed w/ a modulo BLUE_NOISE_RES,
 * so the shift values can be chosen to fit BLUE_NOISE_RES - 1
 * (see generate_rng_seed()) */
#define RNG_SEED_SHIFT_X        0u
#define RNG_SEED_SHIFT_Y        8u
#define RNG_SEED_SHIFT_ISODD    16u
#define RNG_SEED_SHIFT_FRAME    17u

#define RNG_PRIMARY_OFF_X   0
#define RNG_PRIMARY_OFF_Y   1
#define RNG_PRIMARY_APERTURE_X   2
#define RNG_PRIMARY_APERTURE_Y   3

#define RNG_NEE_LIGHT_SELECTION(bounce)   (4 + 0 + 9 * bounce)
#define RNG_NEE_TRI_X(bounce)             (4 + 1 + 9 * bounce)
#define RNG_NEE_TRI_Y(bounce)             (4 + 2 + 9 * bounce)
#define RNG_NEE_LIGHT_TYPE(bounce)        (4 + 3 + 9 * bounce)
#define RNG_BRDF_X(bounce)                (4 + 4 + 9 * bounce)
#define RNG_BRDF_Y(bounce)                (4 + 5 + 9 * bounce)
#define RNG_BRDF_FRESNEL(bounce)          (4 + 6 + 9 * bounce)
#define RNG_SUNLIGHT_X(bounce)			  (4 + 7 + 9 * bounce)
#define RNG_SUNLIGHT_Y(bounce)			  (4 + 8 + 9 * bounce)
// Hexenlicht (6.4): reflect_refract.rgen's choice at a further translucent layer, per pass
// (after the indirect lighting's bounces 0-2)
#define RNG_TRANSLUCENT_LAYER(pass)       (4 + 9 * 3 + pass)

// Hexenlicht: the models around a light (AS_FLAG_LIGHT_MODELS: at a map light's origin, owning a
// dynamic light) in every mask but the shadow rays': their mesh surrounds the light (vk_instance.c)
#define PRIMARY_RAY_CULL_MASK        (AS_FLAG_OPAQUE | AS_FLAG_LIGHT_MODELS | AS_FLAG_TRANSPARENT | AS_FLAG_VIEWER_WEAPON | AS_FLAG_SKY)
#define REFLECTION_RAY_CULL_MASK     (AS_FLAG_OPAQUE | AS_FLAG_LIGHT_MODELS | AS_FLAG_SKY)
#define BOUNCE_RAY_CULL_MASK         (AS_FLAG_OPAQUE | AS_FLAG_LIGHT_MODELS | AS_FLAG_SKY | AS_FLAG_CUSTOM_SKY)
#define SHADOW_RAY_CULL_MASK         (AS_FLAG_OPAQUE)

/* no BRDF sampling in last bounce */
#define NUM_RNG_PER_FRAME (RNG_NEE_STATIC_DYNAMIC(1) + 1)

#define BOUNCE_SPECULAR 1

#define MAX_OUTPUT_VALUE 1000

#ifdef KHR_RAY_QUERY

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

// Just global variables in RQ mode.
// No shadow payload necessary.
RayPayloadGeometry ray_payload_geometry;
RayPayloadEffects ray_payload_effects;

#include "path_tracer_hit_shaders.h"

#else // !KHR_RAY_QUERY

layout(location = RT_PAYLOAD_GEOMETRY) rayPayloadEXT RayPayloadGeometry ray_payload_geometry;
layout(location = RT_PAYLOAD_EFFECTS) rayPayloadEXT RayPayloadEffects ray_payload_effects;

#endif

uint rng_seed;

struct Ray {
	vec3 origin, direction;
	float t_min, t_max;
};

/* Hexenlicht: Hexen II's sky as GL draws it (vk_sky.c), computed per pixel from
 * the direction as the software renderer does (d_sky.c; GL interpolates it from
 * its polygons' vertices, which warps the sky up close): height counts three
 * times, the direction scaled to 6 * 63 units, plus each layer's scroll, in
 * texels of the 128x128 layers; the front layer over the back at r_skyalpha,
 * blended in the 8-bit colors as GL blends the framebuffer (its filtering
 * too: the textures are UNORM, 4.17), as radiance (GL's fullbright) */
vec3
hexen2_sky(vec3 direction)
{
	if(global_ubo.sky_front_texture == 0)
		return vec3(0);

	vec3 d = vec3(direction.xy, direction.z * 3.0);
	vec2 st = d.xy * ((6.0 * 63.0 / 128.0) / max(length(d), 1e-6));

	vec3 back = global_textureLod(global_ubo.sky_back_texture, st + global_ubo.sky_back_scroll, 0).rgb;
	vec4 front = global_textureLod(global_ubo.sky_front_texture, st + global_ubo.sky_front_scroll, 0);

	return color_to_linear(mix(back, front.rgb, front.a * global_ubo.sky_alpha), global_ubo.color_srgb);
}

/* Hexenlicht: Hexen II's sky instead of Quake II RTX's physical sky and
 * environment map (4.6): what is seen (primary rays, reflections, refractions,
 * specular bounces: remove_sun false) is GL's sky; what diffuse bounces gather
 * (remove_sun true) the dome's constant radiance, 0 in the faithful mode. The
 * sun is never in it (get_sunlight samples it), and the sky doesn't rotate */
vec3
env_map(vec3 direction, bool remove_sun)
{
	if(remove_sun)
		return global_ubo.sky_dome;
	return hexen2_sky(direction);
}

// depends on env_map
#include "light_lists.h"

ivec2 get_image_position()
{
	ivec2 pos;

	bool is_even_checkerboard = push_constants.gpu_index == 0 || push_constants.gpu_index < 0 && rt_LaunchID.z == 0;
	if(global_ubo.pt_swap_checkerboard != 0)
		is_even_checkerboard = !is_even_checkerboard;

	if (is_even_checkerboard) {
		pos.x = int(rt_LaunchID.x * 2) + int(rt_LaunchID.y & 1);
	} else {
		pos.x = int(rt_LaunchID.x * 2 + 1) - int(rt_LaunchID.y & 1);
	}

	pos.y = int(rt_LaunchID.y);
	return pos;
}

ivec2 get_image_size()
{
	return ivec2(global_ubo.width, global_ubo.height);
}

bool
found_intersection(RayPayloadGeometry rp)
{
	return rp.primitive_id != ~0u;
}

Triangle
get_hit_triangle(RayPayloadGeometry rp)
{
	return load_and_transform_triangle(
		/* instance_idx = */ rp.buffer_and_instance_idx >> 16,
		/* buffer_idx = */ rp.buffer_and_instance_idx & 0xffff,
		rp.primitive_id);
}

vec3
get_hit_barycentric(RayPayloadGeometry rp)
{
	vec3 bary;
	bary.yz = rp.barycentric;
	bary.x  = 1.0 - bary.y - bary.z;
	return bary;
}

float
get_rng(uint idx)
{
	uvec3 p = uvec3(rng_seed >> RNG_SEED_SHIFT_X, rng_seed >> RNG_SEED_SHIFT_Y, rng_seed >> RNG_SEED_SHIFT_ISODD);
	p.z = (p.z >> 1) + (p.z & 1);
	p.z = (p.z + idx);
	p &= uvec3(BLUE_NOISE_RES - 1, BLUE_NOISE_RES - 1, NUM_BLUE_NOISE_TEX - 1);

	// Hexenlicht: the largest float below 1 (0.9999999999999 rounds to 1.0, which a
	// texel of 65535 returns)
	return min(texelFetch(TEX_BLUE_NOISE, ivec3(p), 0).r, 0.99999994);
	//return fract(vec2(get_rng_uint(idx)) / vec2(0xffffffffu));
}

bool
is_water(uint material)
{
	return (material & MATERIAL_KIND_MASK) == MATERIAL_KIND_WATER;
}

bool
is_slime(uint material)
{
	return (material & MATERIAL_KIND_MASK) == MATERIAL_KIND_SLIME;
}

bool
is_lava(uint material)
{
	return (material & MATERIAL_KIND_MASK) == MATERIAL_KIND_LAVA;
}

bool
is_glass(uint material)
{
	return (material & MATERIAL_KIND_MASK) == MATERIAL_KIND_GLASS;
}

bool
is_transparent(uint material)
{
	uint kind = material & MATERIAL_KIND_MASK;
	return kind == MATERIAL_KIND_TRANSPARENT || kind == MATERIAL_KIND_TRANSP_MODEL;
}

bool
is_chrome(uint material)
{
	uint kind = material & MATERIAL_KIND_MASK;
	return kind == MATERIAL_KIND_CHROME || kind == MATERIAL_KIND_CHROME_MODEL;
}

bool
is_sky(uint material)
{
	uint kind = material & MATERIAL_KIND_MASK;
	return kind == MATERIAL_KIND_SKY;
}

bool
is_screen(uint material)
{
	return (material & MATERIAL_KIND_MASK) == MATERIAL_KIND_SCREEN;
}

bool
is_camera(uint material)
{
	return (material & MATERIAL_KIND_MASK) == MATERIAL_KIND_CAMERA;
}

vec3
correct_emissive(uint material_id, vec3 emissive)
{
	return max(vec3(0), emissive.rgb + vec3(EMISSIVE_TRANSFORM_BIAS));
}

void
trace_geometry_ray(Ray ray, bool cull_back_faces, int instance_mask)
{
	uint rayFlags = 0;
	if (cull_back_faces)
		rayFlags |= gl_RayFlagsCullBackFacingTrianglesEXT;
	rayFlags |= gl_RayFlagsSkipProceduralPrimitives;

	ray_payload_geometry.barycentric = vec2(0);
	ray_payload_geometry.primitive_id = ~0u;
	ray_payload_geometry.buffer_and_instance_idx = 0;
	ray_payload_geometry.hit_distance = 0;

#ifdef KHR_RAY_QUERY

	rayQueryEXT rayQuery;
	rayQueryInitializeEXT(rayQuery, TLAS_GEOMETRY, rayFlags, instance_mask,
		ray.origin, ray.t_min, ray.direction, ray.t_max);

	// Start traversal: return false if traversal is complete
	while (rayQueryProceedEXT(rayQuery))
	{
		uint sbtOffset = rayQueryGetIntersectionInstanceShaderBindingTableRecordOffsetEXT(rayQuery, false);
		int primitiveID = rayQueryGetIntersectionPrimitiveIndexEXT(rayQuery, false);
		int instanceID = rayQueryGetIntersectionInstanceIdEXT(rayQuery, false);
		int geometryIndex = rayQueryGetIntersectionGeometryIndexEXT(rayQuery, false);
		uint instanceCustomIndex = rayQueryGetIntersectionInstanceCustomIndexEXT(rayQuery, false);
		float hitT = rayQueryGetIntersectionTEXT(rayQuery, false);
		vec2 bary = rayQueryGetIntersectionBarycentricsEXT(rayQuery, false);
		bool isProcedural = rayQueryGetIntersectionTypeEXT(rayQuery, false) == gl_RayQueryCandidateIntersectionAABBEXT;

		switch(sbtOffset)
		{
		case SBTO_MASKED:
			if (pt_logic_masked(primitiveID, instanceID, geometryIndex, instanceCustomIndex, bary))
				rayQueryConfirmIntersectionEXT(rayQuery);
			break;
		}
	}

	if (rayQueryGetIntersectionTypeEXT(rayQuery, true) == gl_RayQueryCommittedIntersectionTriangleEXT)
	{
		pt_logic_rchit(ray_payload_geometry, 
			rayQueryGetIntersectionPrimitiveIndexEXT(rayQuery, true),
			rayQueryGetIntersectionInstanceIdEXT(rayQuery, true),
			rayQueryGetIntersectionGeometryIndexEXT(rayQuery, true),
			rayQueryGetIntersectionInstanceCustomIndexEXT(rayQuery, true),
			rayQueryGetIntersectionTEXT(rayQuery, true),
			rayQueryGetIntersectionBarycentricsEXT(rayQuery, true));
	}

#else

	traceRayEXT( topLevelAS[TLAS_INDEX_GEOMETRY], rayFlags, instance_mask,
			SBT_RCHIT_GEOMETRY /*sbtRecordOffset*/, 0 /*sbtRecordStride*/, SBT_RMISS_EMPTY /*missIndex*/,
			ray.origin, ray.t_min, ray.direction, ray.t_max, RT_PAYLOAD_GEOMETRY);

#endif
}

float vmin(vec3 v) { return min(v.x, min(v.y, v.z)); }
float vmax(vec3 v) { return max(v.x, max(v.y, v.z)); }

// Loops over the defined fog volumes and finds the two closest ones along the ray.
// They are stored in the order of min distance in rp.fog1 (closer) and rp.fog2 (further away).
// If the ray starts in a fog volume, that volume will be rp.fog1 with t_min = ray.t_min.
void find_fog_volumes(inout RayPayloadEffects rp, Ray ray)
{
	vec3 inv_dir = vec3(1.0) / ray.direction;
	for (int i = 0; i < MAX_FOG_VOLUMES; i++)
	{
		const ShaderFogVolume volume = global_ubo.fog_volumes[i];

		if (volume.is_active == 0)
			return;

		vec3 t1 = (volume.mins - ray.origin) * inv_dir;
		vec3 t2 = (volume.maxs - ray.origin) * inv_dir;
		float t_in = vmax(min(t1, t2));
		float t_out = vmin(max(t1, t2));
		t_in = max(t_in, ray.t_min);
		t_out = min(t_out, ray.t_max);

		if (t_out > t_in)
		{
			vec2 first_t_min_max = unpackHalf2x16(rp.fog1.w);
			vec2 second_t_min_max = unpackHalf2x16(rp.fog2.w);

			bool replaces_first = t_in < first_t_min_max.x || first_t_min_max.y == 0;
			bool replaces_second = t_in < second_t_min_max.x || second_t_min_max.y == 0;

			if (replaces_first || replaces_second)
			{
				uvec4 packed;
				packed.xy = packHalf4x16(vec4(volume.color * global_ubo.pt_fog_brightness, 0));
				packed.z = packHalf2x16(vec2(t_in, t_out));

				// Convert the volumetric density function into a 1D function along the ray
				float density_variable = dot(volume.density.xyz, ray.direction) * 0.5;
				float density_constant = dot(volume.density.xyz, ray.origin) + volume.density.w;
				// Scale the density stored here because typical values are very small, in fp16 denormal range
				packed.w = packHalf2x16(vec2(density_variable, density_constant) * 65536.0);

				if (replaces_first)
				{
					// Push fog1 to fog2, replace fog1 with the new volume
					rp.fog2 = rp.fog1;
					rp.fog1 = packed;
				}
				else // if (replaces_second) -- must be true
				{
					// Replace fog2 with the new volume
					rp.fog2 = packed;
				}
			}
		}
	}
}

vec4
trace_effects_ray(Ray ray, bool skip_procedural)
{
	uint rayFlags = 0;
	if (skip_procedural)
		rayFlags |= gl_RayFlagsSkipProceduralPrimitives;
	
	uint instance_mask = AS_FLAG_EFFECTS;

	ray_payload_effects.transparency = uvec2(0);
	ray_payload_effects.distances = 0;
	ray_payload_effects.fog1 = uvec4(0);
	ray_payload_effects.fog2 = uvec4(0);
#ifndef KHR_RAY_QUERY
	ray_payload_effects.rayTmax = ray.t_max;
#endif

	if (!skip_procedural)
		find_fog_volumes(ray_payload_effects, ray);

	// Hexenlicht: there is no effects TLAS without effects (vk_accel.c)
	if (global_ubo.effects_tlas == uvec2(0u))
		return skip_procedural ? get_payload_transparency(ray_payload_effects)
		                       : get_payload_transparency_with_fog(ray_payload_effects, ray.t_max);

#ifdef KHR_RAY_QUERY

	rayQueryEXT rayQuery;
	rayQueryInitializeEXT(rayQuery, TLAS_EFFECTS, rayFlags, instance_mask,
		ray.origin, ray.t_min, ray.direction, ray.t_max);

	// Start traversal: return false if traversal is complete
	while (rayQueryProceedEXT(rayQuery))
	{
		uint sbtOffset = rayQueryGetIntersectionInstanceShaderBindingTableRecordOffsetEXT(rayQuery, false);
		int primitiveID = rayQueryGetIntersectionPrimitiveIndexEXT(rayQuery, false);
		int instanceID = rayQueryGetIntersectionInstanceIdEXT(rayQuery, false);
		uint instanceCustomIndex = rayQueryGetIntersectionInstanceCustomIndexEXT(rayQuery, false);
		float hitT = rayQueryGetIntersectionTEXT(rayQuery, false);
		vec2 bary = rayQueryGetIntersectionBarycentricsEXT(rayQuery, false);
		bool isProcedural = rayQueryGetIntersectionTypeEXT(rayQuery, false) == gl_RayQueryCandidateIntersectionAABBEXT;

		vec4 transparent = vec4(0);

		if (isProcedural)
		{
			// Hexenlicht: none: Hexen II's beams are alias models (6.3), not Quake II RTX's procedural beams
		}
		else
		{
			switch(sbtOffset)
			{
			case SBTO_PARTICLE: // particles
				transparent = pt_logic_particle(primitiveID, bary);
				break;

			// Hexenlicht: no explosions (SBTO_EXPLOSION): Hexen II's are sprites

			case SBTO_SPRITE: // sprites
				transparent = pt_logic_sprite(primitiveID, bary, hitT);	// Hexenlicht: with the distance
				break;
			}
		}

		if (transparent.a > 0)
		{
			update_payload_transparency(ray_payload_effects, transparent, hitT);
		}
	}

#else

	traceRayEXT( topLevelAS[TLAS_INDEX_EFFECTS], rayFlags, instance_mask,
			SBT_RCHIT_EFFECTS /*sbtRecordOffset*/, 0 /*sbtRecordStride*/, SBT_RMISS_EMPTY /*missIndex*/,
			ray.origin, ray.t_min, ray.direction, ray.t_max, RT_PAYLOAD_EFFECTS);

#endif

	if (skip_procedural)
		return get_payload_transparency(ray_payload_effects);

	return get_payload_transparency_with_fog(ray_payload_effects, ray.t_max);
}

Ray get_shadow_ray(vec3 p1, vec3 p2, float tmin)
{
	vec3 l = p2 - p1;
	float dist = length(l);
	l /= dist;

	Ray ray;
	ray.origin = p1 + l * tmin;
	ray.t_min = 0;
	ray.t_max = dist - tmin - 0.01;
	ray.direction = l;

	return ray;
}

float
trace_shadow_ray(Ray ray, int cull_mask)
{
	const uint rayFlags = gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsSkipProceduralPrimitives;


#ifdef KHR_RAY_QUERY

	rayQueryEXT rayQuery;
	rayQueryInitializeEXT(rayQuery, TLAS_GEOMETRY, rayFlags, cull_mask, 
		ray.origin, ray.t_min, ray.direction, ray.t_max);

	while (rayQueryProceedEXT(rayQuery))
	{
		uint sbtOffset = rayQueryGetIntersectionInstanceShaderBindingTableRecordOffsetEXT(rayQuery, false);
		int primitiveID = rayQueryGetIntersectionPrimitiveIndexEXT(rayQuery, false);
		int instanceID = rayQueryGetIntersectionInstanceIdEXT(rayQuery, false);
		int geometryIndex = rayQueryGetIntersectionGeometryIndexEXT(rayQuery, false);
		uint instanceCustomIndex = rayQueryGetIntersectionInstanceCustomIndexEXT(rayQuery, false);
		vec2 bary = rayQueryGetIntersectionBarycentricsEXT(rayQuery, false);
		bool isProcedural = rayQueryGetIntersectionTypeEXT(rayQuery, false) == gl_RayQueryCandidateIntersectionAABBEXT;

		if (!isProcedural && sbtOffset == SBTO_MASKED)
		{
			if (pt_logic_masked(primitiveID, instanceID, geometryIndex, instanceCustomIndex, bary))
				rayQueryConfirmIntersectionEXT(rayQuery);
		}
	}

	if(rayQueryGetIntersectionTypeEXT(rayQuery, true) != gl_RayQueryCommittedIntersectionNoneEXT)
		return 0.0f;
	else
		return 1.0f;

#else

	ray_payload_geometry.barycentric = vec2(0);
	ray_payload_geometry.primitive_id = ~0u;
	ray_payload_geometry.buffer_and_instance_idx = 0;
	ray_payload_geometry.hit_distance = -1;

	traceRayEXT( topLevelAS[TLAS_INDEX_GEOMETRY], rayFlags, cull_mask,
			SBT_RCHIT_GEOMETRY /*sbtRecordOffset*/, 0 /*sbtRecordStride*/, SBT_RMISS_EMPTY /*missIndex*/,
			ray.origin, ray.t_min, ray.direction, ray.t_max, RT_PAYLOAD_GEOMETRY);

	return found_intersection(ray_payload_geometry) ? 0.0 : 1.0;

#endif
}

/* Hexenlicht: how far the ray goes before it meets a sky face (the sky's
 * instances only; they are opaque), t_max if it meets none (4.6: the sun's
 * shadow ray ends there) */
float
trace_sky_distance(vec3 origin, vec3 direction, float t_max)
{
#ifdef KHR_RAY_QUERY

	rayQueryEXT rayQuery;
	rayQueryInitializeEXT(rayQuery, TLAS_GEOMETRY, gl_RayFlagsOpaqueEXT | gl_RayFlagsSkipProceduralPrimitives,
		AS_FLAG_SKY, origin, 0, direction, t_max);

	while (rayQueryProceedEXT(rayQuery))
		;

	if(rayQueryGetIntersectionTypeEXT(rayQuery, true) == gl_RayQueryCommittedIntersectionTriangleEXT)
		return rayQueryGetIntersectionTEXT(rayQuery, true);
	return t_max;

#else

	ray_payload_geometry.barycentric = vec2(0);
	ray_payload_geometry.primitive_id = ~0u;
	ray_payload_geometry.buffer_and_instance_idx = 0;
	ray_payload_geometry.hit_distance = -1;

	traceRayEXT( topLevelAS[TLAS_INDEX_GEOMETRY], gl_RayFlagsOpaqueEXT, AS_FLAG_SKY,
			SBT_RCHIT_GEOMETRY /*sbtRecordOffset*/, 0 /*sbtRecordStride*/, SBT_RMISS_EMPTY /*missIndex*/,
			origin, 0, direction, t_max, RT_PAYLOAD_GEOMETRY);

	return found_intersection(ray_payload_geometry) ? ray_payload_geometry.hit_distance : t_max;

#endif
}

vec3
trace_caustic_ray(Ray ray, int surface_medium)
{
	ray_payload_geometry.barycentric = vec2(0);
	ray_payload_geometry.primitive_id = ~0u;
	ray_payload_geometry.buffer_and_instance_idx = 0;
	ray_payload_geometry.hit_distance = -1;


	uint rayFlags = gl_RayFlagsCullBackFacingTrianglesEXT | gl_RayFlagsOpaqueEXT | gl_RayFlagsSkipProceduralPrimitives;
	uint instance_mask = AS_FLAG_TRANSPARENT;
	
#ifdef KHR_RAY_QUERY

	rayQueryEXT rayQuery;
	rayQueryInitializeEXT(rayQuery, TLAS_GEOMETRY, rayFlags, instance_mask, 
		ray.origin, ray.t_min, ray.direction, ray.t_max);
	
	rayQueryProceedEXT(rayQuery);

	if (rayQueryGetIntersectionTypeEXT(rayQuery, true) == gl_RayQueryCommittedIntersectionTriangleEXT)
	{
		pt_logic_rchit(ray_payload_geometry, 
			rayQueryGetIntersectionPrimitiveIndexEXT(rayQuery, true),
			rayQueryGetIntersectionInstanceIdEXT(rayQuery, true),
			rayQueryGetIntersectionGeometryIndexEXT(rayQuery, true),
			rayQueryGetIntersectionInstanceCustomIndexEXT(rayQuery, true),
			rayQueryGetIntersectionTEXT(rayQuery, true),
			rayQueryGetIntersectionBarycentricsEXT(rayQuery, true));
	}

#else

	traceRayEXT(topLevelAS[TLAS_INDEX_GEOMETRY], rayFlags, instance_mask, SBT_RCHIT_GEOMETRY, 0, SBT_RMISS_EMPTY,
			ray.origin, ray.t_min, ray.direction, ray.t_max, RT_PAYLOAD_GEOMETRY);

#endif

	float extinction_distance = ray.t_max - ray.t_min;
	vec3 throughput = vec3(1);

	if(found_intersection(ray_payload_geometry))
	{
		Triangle triangle = get_hit_triangle(ray_payload_geometry);
		
		vec3 geo_normal = triangle.normals[0];
		bool is_vertical = abs(geo_normal.z) < 0.1;

		if((is_water(triangle.material_id) || is_slime(triangle.material_id)) && !is_vertical)
		{
			vec3 position = ray.origin + ray.direction * ray_payload_geometry.hit_distance;
			vec3 w = get_water_normal(triangle.material_id, geo_normal, triangle.tangents[0], position, true);

			float caustic = clamp((1 - pow(clamp(1 - length(w.xz), 0, 1), 2)) * 100, 0, 8);
			caustic = mix(1, caustic, clamp(ray_payload_geometry.hit_distance * 0.02, 0, 1));
			throughput = vec3(caustic);

			if(surface_medium != MEDIUM_NONE)
			{
				extinction_distance = ray_payload_geometry.hit_distance;
			}
			else
			{
				if(is_water(triangle.material_id))
					surface_medium = MEDIUM_WATER;
				else
					surface_medium = MEDIUM_SLIME;

				extinction_distance = max(0, ray.t_max - ray_payload_geometry.hit_distance);
			}
		}
		else if(is_glass(triangle.material_id) || is_water(triangle.material_id) && is_vertical)
		{
			vec3 bary = get_hit_barycentric(ray_payload_geometry);
			vec2 tex_coord = triangle.tex_coords * bary;

			MaterialInfo minfo = get_material_info(triangle.material_id);

	    	vec3 base_color = vec3(minfo.base_factor);
	    	if (minfo.base_texture > 0)
	    		base_color *= color_to_linear(global_textureLod(minfo.base_texture, tex_coord, 2).rgb, global_ubo.color_srgb);
	    	base_color = clamp(base_color, vec3(0), vec3(1));

			throughput = base_color;
		}
		else
		{
			throughput = vec3(clamp(1.0 - triangle.alpha, 0.0, 1.0));
		}
	}

	//return vec3(caustic);
	return extinction(surface_medium, extinction_distance) * throughput;
}

/* Hexenlicht (5.3): MATERIALS.md's normal maps, XYZ as RGB x 0.5 + 0.5 (Quake
 * II RTX reads Z as it is); BC5 has X and Y only (Z samples as 0), so Z is
 * rebuilt and the length, which the Toksvig adjustment reads, is 1 */
vec3 rgbToNormal(vec3 rgb, bool bc5, out float len)
{
    vec3 n;
    n.xy = rgb.xy * 2 - 1;
    n.z = bc5 ? sqrt(max(1 - dot(n.xy, n.xy), 0)) : rgb.z * 2 - 1;

    len = length(n);
    return len > 0 ? n / len : vec3(0);
}


float
AdjustRoughnessToksvig(float roughness, float normalMapLen, float mip_level)
{
	float effect = global_ubo.pt_toksvig * clamp(mip_level, 0, 1);
    float shininess = RoughnessSquareToSpecPower(roughness) * effect; // not squaring the roughness here - looks better this way
    float ft = normalMapLen / mix(shininess, 1.0f, normalMapLen);
    ft = max(ft, 0.01f);
    return SpecPowerToRoughnessSquare(ft * shininess / effect);
}

float
get_specular_sampled_lighting_weight(float roughness, vec3 N, vec3 V, vec3 L, float pdfw)
{
    float ggxVndfPdf = ImportanceSampleGGX_VNDF_PDF(max(roughness, 0.01), N, V, L);
  
    // Balance heuristic assuming one sample from each strategy: light sampling and BRDF sampling
    return clamp(pdfw / (pdfw + ggxVndfPdf), 0, 1);
}

/* Hexenlicht (4.13): set by get_direct_illumination for gradient samples: the relative
 * change of the sampled list light's style since last frame (0 = none, or not sampled,
 * shadowed); direct_lighting.rgen hands it to asvgf_gradient_img.comp */
float nee_style_change = 0;

void
get_direct_illumination(
	vec3 position, 
	vec3 normal, 
	vec3 geo_normal, 
	uint cluster_idx, 
	uint material_id,
	int shadow_cull_mask, 
	vec3 view_direction, 
	vec3 albedo,
	vec3 base_reflectivity,
	float specular_factor,
	float roughness, 
	int surface_medium, 
	bool enable_caustics, 
	float direct_specular_weight, 
	bool enable_polygonal,
	bool enable_dynamic,
	bool is_gradient,
	int bounce,
	uint receiver,	// Hexenlicht (6.2): the surface's model instance (~0u = the world), which its own light doesn't light
	out vec3 diffuse,
	out vec3 specular)
{
	diffuse = vec3(0);
	specular = vec3(0);

	vec3 pos_on_light_polygonal;
	vec3 pos_on_light_dynamic;

	vec3 contrib_polygonal = vec3(0);
	vec3 contrib_dynamic = vec3(0);

	float alpha = square(roughness);
	float phong_exp = RoughnessSquareToSpecPower(alpha);
	float phong_scale = min(100, 1 / (M_PI * square(alpha)));
	float phong_weight = clamp(specular_factor * luminance(base_reflectivity) / (luminance(base_reflectivity) + luminance(albedo)), 0, 0.9);

	int polygonal_light_index = -1;
	uint polygonal_light_node = ~0u;	// Hexenlicht: its light list entry
	float polygonal_light_pdfw = 0;
	bool polygonal_light_is_sky = false;
	float polygonal_light_angle = 0;	// Hexenlicht (4.15): its angle term, 0 = the cosine

	vec3 rng = vec3(
		get_rng(RNG_NEE_LIGHT_SELECTION(bounce)),
		get_rng(RNG_NEE_TRI_X(bounce)),
		get_rng(RNG_NEE_TRI_Y(bounce)));

	// Limit the solid angle of sphere lights for indirect lighting
	// in order to kill some fireflies in locations with many sphere lights.
	// Example: green wall-lamp corridor in the "train" map.
	// Hexenlicht: the light lists' spheres too
	float max_solid_angle = (bounce == 0) ? 2 * M_PI : 0.02;

	/* polygonal light illumination */
	if(enable_polygonal)
	{
		sample_polygonal_lights(
			cluster_idx,
			position,
			normal,
			geo_normal,
			view_direction,
			phong_exp,
			phong_scale,
			phong_weight,
			is_gradient,
			max_solid_angle,
			pos_on_light_polygonal,
			contrib_polygonal,
			polygonal_light_index,
			polygonal_light_node,
			polygonal_light_pdfw,
			polygonal_light_is_sky,
			polygonal_light_angle,
			rng);
	}

	bool is_polygonal = true;
	float vis = 1;

	/* dynamic light illumination */
	if(enable_dynamic)
	{
		sample_dynamic_lights(
			position,
			normal,
			geo_normal,
			max_solid_angle,
			receiver,
			(material_id & MATERIAL_FLAG_LIGHT) != 0,	// Hexenlicht (6.3): a beam isn't lit by beam lights
			pos_on_light_dynamic,
			contrib_dynamic,
			rng);
	}

	float spec_polygonal = phong(normal, normalize(pos_on_light_polygonal - position), view_direction, phong_exp) * phong_scale;
	float spec_dynamic = phong(normal, normalize(pos_on_light_dynamic - position), view_direction, phong_exp) * phong_scale;

	float l_polygonal  = luminance(abs(contrib_polygonal)) * mix(1, spec_polygonal, phong_weight);
	float l_dynamic = luminance(abs(contrib_dynamic)) * mix(1, spec_dynamic, phong_weight);
	float l_sum = l_polygonal + l_dynamic;

	bool null_light = (l_sum == 0);

	float w = null_light ? 0.5 : l_polygonal / (l_polygonal + l_dynamic);

	float rng2 = get_rng(RNG_NEE_LIGHT_TYPE(bounce));
	is_polygonal = (rng2 < w);
	vis = is_polygonal ? (1 / w) : (1 / (1 - w));
	vec3 pos_on_light = null_light ? position : (is_polygonal ? pos_on_light_polygonal : pos_on_light_dynamic);
	vec3 contrib = is_polygonal ? contrib_polygonal : contrib_dynamic;

	Ray shadow_ray = get_shadow_ray(position - view_direction * 0.01, pos_on_light, 0);
	
	// Hexenlicht: no shadow ray without a light: Quake II RTX traces one with an
	// empty mask to the surface itself, whose t_max (0.01 * |V| - 0.01, V from the
	// fp16 PT_VIEW_DIRECTION) can be below t_min, which ray queries don't allow
	if(!null_light)
		vis *= trace_shadow_ray(shadow_ray, shadow_cull_mask);

	// Hexenlicht (4.13): a gradient sample's exact relative change from its list light's
	// style, when that light is the one sampled and it is unshadowed
	nee_style_change = 0;
	if(is_gradient && is_polygonal && !null_light && vis > 0 && polygonal_light_index >= 0)
	{
		LightPolygon chosen = get_light_polygon(uint(polygonal_light_index));
		if(chosen.type == LIGHT_TYPE_SPHERE && chosen.shape == SPHERE_SHAPE_GL)
		{
			// 4.15: the change of its light, whose style is inside GL's clip and power
			float v = lightmap_light_value(chosen, position, normal, geo_normal, chosen.light_style_scale);
			float v_prev = lightmap_light_value(chosen, position, normal, geo_normal, chosen.prev_style_scale);
			float v_max = max(v, v_prev);
			if(v_max > 0)
				nee_style_change = abs(v - v_prev) / v_max;
		}
		else
		{
			float style_max = max(chosen.light_style_scale, chosen.prev_style_scale);
			if(style_max > 0)
				nee_style_change = abs(chosen.light_style_scale - chosen.prev_style_scale) / style_max;
		}
	}
#ifdef ENABLE_SHADOW_CAUSTICS
	if(enable_caustics)
	{
		contrib *= trace_caustic_ray(shadow_ray, surface_medium);
	}
#endif

	/*
		Accumulate light shadowing statistics to guide importance sampling on the next frame.
		Inspired by paper called "Adaptive Shadow Testing for Ray Tracing" by G. Ward, EUROGRAPHICS 1994.

		The algorithm counts the shadowed and unshadowed rays towards each light, per cluster,
		per surface orientation in each cluster. Orientation helps improve accuracy in cases
		when a single cluster has different parts which have the same light mostly shadowed and
		mostly unshadowed.

		On the next frame, the light CDF is built using the counts from this frame, or the frame
		before that in case of gradient rays. See light_lists.h for more info.

		Hexenlicht: counted per light list entry (a cluster's light), polygons and spheres
		alike; the lists hold only the map's lights (vk_light.c), whose entries stay put
		until the lists are rebuilt, which clears the statistics. The UBO's sphere lights
		(sample_dynamic_lights) have none, as in Quake II RTX.
	*/
	if(global_ubo.pt_light_stats != 0
		&& is_polygonal
		&& !null_light
		&& polygonal_light_node != ~0u
		&& global_ubo.light_stats != uvec2(0u))
	{
		uint addr = get_light_stats_addr(polygonal_light_node, get_primary_direction(normal));

		// Offset 0 is unshadowed rays,
		// Offset 1 is shadowed rays
		if(vis == 0) addr += 1;

		// Increment the ray counter
		atomicAdd(LightStatsRef(global_ubo.light_stats).stats[addr], 1);
	}

	if(null_light)
		return;

	vec3 radiance = vis * contrib;

	vec3 L = pos_on_light - position;
	L = normalize(L);

	if(is_polygonal && direct_specular_weight > 0 && polygonal_light_is_sky && global_ubo.pt_specular_mis != 0)
	{
		// MIS with direct specular and indirect specular.
		// Only applied to sky lights, for two reasons:
		//  1) Non-sky lights are trimmed to match the light texture, and indirect rays don't see that;
		//  2) Non-sky lights are usually away from walls, so the direct sampling issue is not as pronounced.

		direct_specular_weight *= get_specular_sampled_lighting_weight(roughness,
			normal, -view_direction, L, polygonal_light_pdfw);
	}

	vec3 F = vec3(0);

	// Hexenlicht (4.15): a list light with its own angle term (light_lists.h: the map
	// lights' GL-like shapes) instead of the cosine; the specular takes its light without it
	float light_angle = is_polygonal ? polygonal_light_angle : 0;

	if(vis > 0 && direct_specular_weight > 0)
	{
		vec3 specular_brdf = GGX_times_NdotL(view_direction, normalize(pos_on_light - position),
			normal, roughness, base_reflectivity, 0.0, specular_factor, F);
		specular = ((light_angle > 0) ? radiance / light_angle : radiance) * specular_brdf * direct_specular_weight;
	}

	float NdotL = (light_angle > 0) ? 1 : max(0, dot(normal, L));

	float diffuse_brdf = NdotL / M_PI;
	diffuse = radiance * diffuse_brdf * (vec3(1.0) - F);
}

void
get_sunlight(
	uint cluster_idx,
	uint material_id,
	vec3 position,
	vec3 normal,
	vec3 geo_normal,
	vec3 view_direction,
	vec3 base_reflectivity,
	float specular_factor,
	float roughness,
	int surface_medium,
	bool enable_caustics,
	out vec3 diffuse,
	out vec3 specular,
	int shadow_cull_mask)
{
	diffuse = vec3(0);
	specular = vec3(0);

	if(global_ubo.sun_visible == 0)
		return;

	// Hexenlicht: clusters past the visibility's bits (none on Hexen II's maps) trace it
	bool visible = (cluster_idx >= MAX_LIGHT_LISTS) || (light_buffer.sky_visibility[cluster_idx >> 5] & (1 << (cluster_idx & 31))) != 0;

	if(!visible)
		return;

	vec2 rng3 = vec2(get_rng(RNG_SUNLIGHT_X(0)), get_rng(RNG_SUNLIGHT_Y(0)));
	vec2 disk = sample_disk(rng3);
	disk.xy *= global_ubo.sun_tan_half_angle;

	vec3 direction = normalize(global_ubo.sun_direction + global_ubo.sun_tangent * disk.x + global_ubo.sun_bitangent * disk.y);

	float NdotL = dot(direction, normal);
	float GNdotL = dot(direction, geo_normal);

	if(NdotL <= 0 || GNdotL <= 0)
		return;

	// Hexenlicht: the ray ends at the first sky face (see the top); Quake II RTX's goes 10000 units.
	// A point at the sky's face is lit: no ray there (get_shadow_ray's t_max would be below its t_min)
	vec3 origin = position - view_direction * 0.01;
	float sky_distance = trace_sky_distance(origin, direction, 10000);
	bool shadow_traced = sky_distance > 0.02;
	Ray shadow_ray;

	if(shadow_traced)
	{
		shadow_ray = get_shadow_ray(origin, origin + direction * sky_distance, 0);

		if(trace_shadow_ray(shadow_ray, shadow_cull_mask) == 0)
			return;
	}

	// Hexenlicht: the sun's irradiance from the UBO (vk_sky.c), no sun color buffer
	vec3 radiance = global_ubo.sun_color;

#ifdef ENABLE_SHADOW_CAUSTICS
	if(enable_caustics && shadow_traced)
	{
    	radiance *= trace_caustic_ray(shadow_ray, surface_medium);
	}
#endif

	vec3 F = vec3(0);

    if(global_ubo.pt_sun_specular > 0)
    {
		float NoH_offset = 0.5 * square(global_ubo.sun_tan_half_angle);
		vec3 specular_brdf = GGX_times_NdotL(view_direction, global_ubo.sun_direction,
			normal,roughness, base_reflectivity, NoH_offset, specular_factor, F);
    	specular = radiance * specular_brdf;
	}

	float diffuse_brdf = NdotL / M_PI;
	diffuse = radiance * diffuse_brdf * (vec3(1.0) - F);
}

vec3 clamp_output(vec3 c)
{
	if(any(isnan(c)) || any(isinf(c)))
		return vec3(0);
	else 
		return clamp(c, vec3(0), vec3(MAX_OUTPUT_VALUE));
}

vec3
sample_emissive_texture(uint material_id, MaterialInfo minfo, vec2 tex_coord, vec2 tex_coord_x, vec2 tex_coord_y, float mip_level)
{
	if (minfo.emissive_texture != 0)
    {
        vec4 image3;
	    if (mip_level >= 0)
	        image3 = global_textureLod(minfo.emissive_texture, tex_coord, mip_level);
	    else
	        image3 = global_textureGrad(minfo.emissive_texture, tex_coord, tex_coord_x, tex_coord_y);

    	vec3 corrected = correct_emissive(material_id, color_to_linear(image3.rgb, global_ubo.color_srgb));

	    return corrected * minfo.emissive_factor;
	}

	return vec3(0);
}

vec3 get_emissive_shell(uint material_id, uint shell)
{
	vec3 c = vec3(0);

	if((shell & SHELL_MASK) != 0)
	{ 
		if ((shell & SHELL_HALF_DAM) != 0)
		{
			c.r = 0.56f;
			c.g = 0.59f;
			c.b = 0.45f;
		}
		if ((shell & SHELL_DOUBLE) != 0)
		{
			c.r = 0.9f;
			c.g = 0.7f;
		}
		if ((shell & SHELL_LITE_GREEN) != 0)
		{
			c.r = 0.7f;
			c.g = 1.0f;
			c.b = 0.7f;
		}
	    if((shell & SHELL_RED) != 0) c.r += 1;
	    if((shell & SHELL_GREEN) != 0) c.g += 1;
	    if((shell & SHELL_BLUE) != 0) c.b += 1;

	    if((material_id & MATERIAL_FLAG_WEAPON) != 0) c *= 0.2;
	}

	// Hexenlicht: Quake II RTX scales it by the tone mapping buffer's adapted
	// luminance here; Hexen II has no shells

    return c;
}

bool get_is_gradient(ivec2 ipos)
{
	if(global_ubo.flt_enable != 0)
	{
		uint u = texelFetch(TEX_ASVGF_GRAD_SMPL_POS_A, ipos / GRAD_DWN, 0).r;

		ivec2 grad_strata_pos = ivec2(
				u >> (STRATUM_OFFSET_SHIFT * 0),
				u >> (STRATUM_OFFSET_SHIFT * 1)) & STRATUM_OFFSET_MASK;

		return (u > 0 && all(equal(grad_strata_pos, ipos % GRAD_DWN)));
	}

	return false;
}


void
get_material(
	Triangle triangle,
	vec3 bary,
	vec2 tex_coord,
	vec2 tex_coord_x,
	vec2 tex_coord_y,
	float mip_level,
	vec3 geo_normal,
    out vec3 base_color,
    out vec3 normal,
    out float metallic,
    out float roughness,
    out vec3 emissive,
    out float specular_factor)
{
	MaterialInfo minfo = get_material_info(triangle.material_id);

	perturb_tex_coord(triangle.material_id, global_ubo.time, tex_coord);	

    vec4 image1 = vec4(1);
    if (minfo.base_texture != 0)
    {
		if (mip_level >= 0)
		    image1 = global_textureLod(minfo.base_texture, tex_coord, mip_level);
		else
		    image1 = global_textureGrad(minfo.base_texture, tex_coord, tex_coord_x, tex_coord_y);
	}

	base_color = color_to_linear(image1.rgb, global_ubo.color_srgb) * minfo.base_factor;
	base_color = clamp(base_color, vec3(0), vec3(1));

	// Hexenlicht: a model's colorshade tint multiplies GL's vertex light (then
	// clamped to 1); its values reach 10, so its hue, scaled to at most 1,
	// tints the base color (its brightness is a matter of the lighting, E4),
	// as GL multiplied it: an 8-bit color (4.17)
	if (triangle.instance_index != ~0u)
	{
		vec3 tint = instance_buffer.model_instances[triangle.instance_index].tint;
		base_color *= color_to_linear(tint / max(max(tint.r, max(tint.g, tint.b)), 1.0), global_ubo.color_srgb);
	}

	normal = geo_normal;

	// Hexenlicht (5.3, MATERIALS.md): roughness and metallic from their own
	// texture (G, B), also without a normal map, by glTF's rule: the
	// material's value, or a factor on the map (Quake II RTX: the albedo's
	// and the normal map's alpha, read with a normal map only, the roughness
	// override a floor)
	roughness = minfo.roughness;
	metallic = minfo.metalness_factor;
	if (minfo.rm_texture != 0)
	{
		vec4 rm;
		if (mip_level >= 0)
			rm = global_textureLod(minfo.rm_texture, tex_coord, mip_level);
		else
			rm = global_textureGrad(minfo.rm_texture, tex_coord, tex_coord_x, tex_coord_y);
		roughness *= rm.g;
		metallic *= rm.b;
	}
	roughness = clamp(roughness, 0, 1);
	metallic = clamp(metallic, 0, 1);

    if (minfo.normals_texture != 0)
    {
        vec4 image2;
	    if (mip_level >= 0)
	        image2 = global_textureLod(minfo.normals_texture, tex_coord, mip_level);
	    else
	        image2 = global_textureGrad(minfo.normals_texture, tex_coord, tex_coord_x, tex_coord_y);

		float normalMapLen;
		vec3 local_normal = rgbToNormal(image2.rgb, (minfo.normals_flags & MATERIAL_NORMALS_BC5) != 0, normalMapLen);
		// Hexenlicht (5.3): OpenGL's convention, green up the image; the
		// bitangent runs down it (+v: vk_world.c, model_geometry.comp),
		// which is DirectX's, as Quake II RTX's maps are
		local_normal.y = -local_normal.y;

		if(dot(triangle.tangents[0], triangle.tangents[0]) > 0)
		{
			vec3 tangent = normalize(triangle.tangents * bary);
			// Hexenlicht (5.5): the bitangent from the triangle's own normal,
			// not geo_normal, which faces the ray: a surface seen from behind
			// keeps its map's frame (a liquid's two coincident faces, up and
			// down, shade alike: Quake II RTX mirrored the map on the back one)
			vec3 bitangent = cross(normalize(triangle.normals * bary), tangent);

			if((triangle.material_id & MATERIAL_FLAG_HANDEDNESS) != 0)
        		bitangent = -bitangent;
			
			normal = tangent * local_normal.x + bitangent * local_normal.y + geo_normal * local_normal.z;
        
			float bump_scale = global_ubo.pt_bump_scale * minfo.bump_scale;
			if(is_glass(triangle.material_id))
        		bump_scale *= 0.2;

			normal = normalize(mix(geo_normal, normal, bump_scale));
		}

        float effective_mip = mip_level;

    	if (effective_mip < 0)
    	{
        	ivec2 texSize = global_textureSize(minfo.normals_texture, 0);
        	vec2 tx = tex_coord_x * texSize;
        	vec2 ty = tex_coord_y * texSize;
        	float d = max(dot(tx, tx), dot(ty, ty));
        	effective_mip = 0.5 * log2(d);
        }

        bool is_mirror = (roughness < MAX_MIRROR_ROUGHNESS) && (is_chrome(triangle.material_id) || is_screen(triangle.material_id));

        if (normalMapLen > 0 && global_ubo.pt_toksvig > 0 && effective_mip > 0 && !is_mirror)
        {
            roughness = AdjustRoughnessToksvig(roughness, normalMapLen, effective_mip);
        }
    } 

    if(global_ubo.pt_roughness_override >= 0) roughness = global_ubo.pt_roughness_override;
    if(global_ubo.pt_metallic_override >= 0) metallic = global_ubo.pt_metallic_override;
    
    // The specular factor parameter should only affect dielectrics, so make it 1.0 for metals
    specular_factor = mix(minfo.specular_factor, 1.0, metallic);

	if (triangle.emissive_factor > 0)
	{
	    emissive = sample_emissive_texture(triangle.material_id, minfo, tex_coord, tex_coord_x, tex_coord_y, mip_level);
	    emissive *= triangle.emissive_factor;
	}
	else
		emissive = vec3(0);

    emissive += get_emissive_shell(triangle.material_id, triangle.shell) * base_color * (1 - metallic * 0.9);
}

/* Hexenlicht (6.4): a translucent model's opacity where it was hit, its
 * entity's (triangle.alpha: 0.33 for DRF_TRANSLUCENT) times its skin's
 * (the material's mask, vk_skin.c: EF_TRANSPARENT's 0.33 at odd colors,
 * EF_SPECIAL_TRANS's table, EF_HOLEY's holes), as GL blends them; the
 * entity's alone for other surfaces. A texel of 250 and up is opaque
 * (DECISIONS M31: BC7 stores an opaque albedo's texels at 251-254), so
 * it doesn't split the path */
float get_hit_alpha(Triangle triangle, vec2 tex_coord, vec2 tex_coord_x, vec2 tex_coord_y)
{
	if ((triangle.material_id & MATERIAL_KIND_MASK) != MATERIAL_KIND_TRANSP_MODEL)
		return triangle.alpha;

	MaterialInfo minfo = get_material_info(triangle.material_id);
	if (minfo.mask_texture == 0)
		return triangle.alpha;

	float a = global_textureGrad(minfo.mask_texture, tex_coord, tex_coord_x, tex_coord_y).a;
	return (a >= 250.0 / 255.0) ? triangle.alpha : triangle.alpha * a;
}

bool get_camera_uv(vec2 tex_coord, out vec2 cameraUV)
{
	const vec2 minUV = vec2(11.0 / 256.0, 14.0 / 256.0);
	const vec2 maxUV = vec2(245.0 / 256.0, 148.0 / 256.0);
	
	tex_coord = fract(tex_coord);
	cameraUV = (tex_coord - minUV) / (maxUV - minUV);

	//vec2 resolution = vec2(7, 4) * 50;
	//cameraUV = (floor(cameraUV * resolution) + vec2(0.5)) / resolution;

	return all(greaterThan(cameraUV, vec2(0))) && all(lessThan(cameraUV, vec2(1)));
}

// Anisotropic texture sampling algorithm from 
// "Improved Shader and Texture Level of Detail Using Ray Cones"
// by T. Akenine-Moller et al., JCGT Vol. 10, No. 1, 2021.
// See section 5. Anisotropic Lookups.
void compute_anisotropic_texture_gradients(
	vec3 intersection,
	vec3 normal,
	vec3 ray_direction,
	float cone_radius,
	mat3 positions,
	mat3x2 tex_coords,
	vec2 tex_coords_at_intersection,
	out vec2 texGradient1,
	out vec2 texGradient2,
	out float fwidth_depth)
{
	// Compute ellipse axes.
	vec3 a1 = ray_direction - dot(normal, ray_direction) * normal;
	vec3 p1 = a1 - dot(ray_direction, a1) * ray_direction;
	a1 *= cone_radius / max(0.0001, length(p1));

	vec3 a2 = cross(normal, a1);
	vec3 p2 = a2 - dot(ray_direction, a2) * ray_direction;
	a2 *= cone_radius / max(0.0001, length(p2));

	// Compute texture coordinate gradients.
	vec3 eP, delta = intersection - positions[0];
	vec3 e1 = positions[1] - positions[0];
	vec3 e2 = positions[2] - positions[0];
	float inv_tri_area = 1.0 / dot(normal, cross(e1, e2));

	eP = delta + a1;
	float u1 = dot(normal, cross(eP, e2)) * inv_tri_area;
	float v1 = dot(normal, cross(e1, eP)) * inv_tri_area;
	texGradient1 = (1.0-u1-v1) * tex_coords[0] + u1 * tex_coords[1] +
		v1 * tex_coords[2] - tex_coords_at_intersection;

	eP = delta + a2;
	float u2 = dot(normal, cross(eP, e2)) * inv_tri_area;
	float v2 = dot(normal, cross(e1, eP)) * inv_tri_area;
	texGradient2 = (1.0-u2-v2) * tex_coords[0] + u2 * tex_coords[1] +
		v2 * tex_coords[2] - tex_coords_at_intersection;

	fwidth_depth = 1.0 / max(0.1, abs(dot(a1, ray_direction)) + abs(dot(a2, ray_direction)));
}
