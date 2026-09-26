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
 *  - no environment until the sky (4.6): env_map returns black;
 *  - trace_effects_ray: pt_logic_sprite takes the hit distance, beams and
 *    explosions come with their story (6.3), no effects TLAS = no effects;
 *  - left out until their passes: the light lists (light_lists.h), shadow
 *    and caustic rays, direct illumination and sunlight (3.3, 3.4), the
 *    gradient samples of the denoiser (get_is_gradient, 3.6);
 *  - get_rng: clamped to the largest float below 1 (Quake II RTX's literal
 *    rounds to 1.0);
 *  - get_material: a model's colorshade tint's hue tints the base color;
 *  - get_emissive_shell: no tone mapping buffer (3.7); Hexen II has no
 *    shells. */

#include "path_tracer.h"
#include "utils.glsl"
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

#define PRIMARY_RAY_CULL_MASK        (AS_FLAG_OPAQUE | AS_FLAG_TRANSPARENT | AS_FLAG_VIEWER_WEAPON | AS_FLAG_SKY)
#define REFLECTION_RAY_CULL_MASK     (AS_FLAG_OPAQUE | AS_FLAG_SKY)
#define BOUNCE_RAY_CULL_MASK         (AS_FLAG_OPAQUE | AS_FLAG_SKY | AS_FLAG_CUSTOM_SKY)
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

vec3
env_map(vec3 direction, bool remove_sun)
{
	direction = (global_ubo.environment_rotation_matrix * vec4(direction, 0)).xyz;

    vec3 envmap = vec3(0);
	// Hexenlicht: no environment until the sky (4.6), which brings Quake II
	// RTX's physical sky (TEX_PHYSICAL_SKY) and environment map (TEX_ENVMAP)
	return envmap;
}

// depends on env_map
// Hexenlicht: light_lists.h comes with the light lists (3.3, 3.4)

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
			// Hexenlicht: beams, the procedural primitives, come with 6.3
		}
		else
		{
			switch(sbtOffset)
			{
			case SBTO_PARTICLE: // particles
				transparent = pt_logic_particle(primitiveID, bary);
				break;

			// Hexenlicht: explosions (SBTO_EXPLOSION) come with 6.3

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

// Hexenlicht: get_shadow_ray, trace_shadow_ray and trace_caustic_ray come with
// the direct lighting (3.3)

vec3 rgbToNormal(vec3 rgb, out float len)
{
    vec3 n = vec3(rgb.xy * 2 - 1, rgb.z);

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

// Hexenlicht: get_specular_sampled_lighting_weight, get_direct_illumination and
// get_sunlight come with the direct lighting (3.3)

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

    	vec3 corrected = correct_emissive(material_id, image3.rgb);

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

	// Hexenlicht: scaled by the tone mapping buffer's adapted luminance (3.7)
	// in Quake II RTX; Hexen II has no shells

    return c;
}

// Hexenlicht: get_is_gradient, the denoiser's gradient samples, comes with the denoiser (3.6)


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

	base_color = image1.rgb * minfo.base_factor;
	base_color = clamp(base_color, vec3(0), vec3(1));

	// Hexenlicht: a model's colorshade tint multiplies GL's vertex light (then
	// clamped to 1); its values reach 10, so its hue, scaled to at most 1,
	// tints the base color (its brightness is a matter of the lighting, E4)
	if (triangle.instance_index != ~0u)
	{
		vec3 tint = instance_buffer.model_instances[triangle.instance_index].tint;
		base_color *= tint / max(max(tint.r, max(tint.g, tint.b)), 1.0);
	}

	normal = geo_normal;
	metallic = 0;
    roughness = 1;

    if (minfo.normals_texture != 0)
    {
        vec4 image2;
	    if (mip_level >= 0)
	        image2 = global_textureLod(minfo.normals_texture, tex_coord, mip_level);
	    else
	        image2 = global_textureGrad(minfo.normals_texture, tex_coord, tex_coord_x, tex_coord_y);

		float normalMapLen;
		vec3 local_normal = rgbToNormal(image2.rgb, normalMapLen);

		if(dot(triangle.tangents[0], triangle.tangents[0]) > 0)
		{
			vec3 tangent = normalize(triangle.tangents * bary);
			vec3 bitangent = cross(geo_normal, tangent);

			if((triangle.material_id & MATERIAL_FLAG_HANDEDNESS) != 0)
        		bitangent = -bitangent;
			
			normal = tangent * local_normal.x + bitangent * local_normal.y + geo_normal * local_normal.z;
        
			float bump_scale = global_ubo.pt_bump_scale * minfo.bump_scale;
			if(is_glass(triangle.material_id))
        		bump_scale *= 0.2;

			normal = normalize(mix(geo_normal, normal, bump_scale));
		}

        metallic = clamp(image2.a * minfo.metalness_factor, 0, 1);
        
        if(minfo.roughness_override >= 0)
        	roughness = max(image1.a, minfo.roughness_override);
        else
        	roughness = image1.a;

        roughness = clamp(roughness, 0, 1);

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
