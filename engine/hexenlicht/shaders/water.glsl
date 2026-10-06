/*
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

/* Hexenlicht (6.5): with r_water 1 (global_ubo.water) a liquid's surface
 * (MATERIAL_FLAG_LIQUID: between the liquid and the air, not vertical,
 * vk_world.c) is physical water: Fresnel reflection and refraction
 * (reflect_refract.rgen), its texture a layer of GL's opacity (0.33 for the
 * translucent *rtex078 and *lowlight, else 1) over what is seen through it,
 * waves from Hexen II's turbulence (get_turbulence_normal), and the liquid
 * a medium (water_fog_*): extinction, and in-scattering of GL's contents
 * color lit as a model where it is (6.18: the liquids' light grid,
 * medium.glsl), added to PT_TRANSPARENT segment by segment. A path's
 * medium so far is a vec4: what its final surface's
 * albedo is multiplied by (.a: the medium's and the texture layers'
 * transmittance) and the texture layers' albedo added to it (.rgb);
 * water_fog_apply puts it into the final surface's material, so a layer
 * is lit as what is seen through it. Quake II RTX's get_water_normal and
 * extinction stay for r_water 0 (its water normal map is in its media:
 * water_normal_texture is 0). 6.16: water_caustic, the water texture's
 * pattern in the light through a liquid's surface (path_tracer_rgen.h's
 * caustic ray) */

bool is_physical_liquid(uint material_id)
{
	return global_ubo.water != 0 && (material_id & MATERIAL_FLAG_LIQUID) != 0;
}

// the opacity of a liquid surface's texture layer: GL's
float liquid_layer_alpha(uint material_id)
{
	return ((material_id & MATERIAL_FLAG_LIQUID_TRANSLUCENT) != 0) ? TRANSLUCENT_LIQUID_ALPHA : 1.0;
}

// the medium's color: GL's V_SetContentsColor colors (view.c's
// cshift_water, cshift_slime, cshift_lava), 8-bit
vec3 water_fog_color(int medium)
{
	if(medium == MEDIUM_WATER)
		return vec3(130, 80, 50) / 255.0;
	if(medium == MEDIUM_SLIME)
		return vec3(0, 25, 5) / 255.0;
	if(medium == MEDIUM_LAVA)
		return vec3(255, 80, 0) / 255.0;
	return vec3(0);
}

// 6.18: the liquids' light grid (vk_medium.c; 0 = none)
#include "hl_shared.h"
layout(buffer_reference, std430, buffer_reference_align = 8) readonly buffer MediumGridRef { uvec2 d[]; };
#define medium_table MediumGridRef(global_ubo.medium_grid).d
#define medium_style(s) global_ubo.medium_styles[(s) >> 2][(s) & 3]
#include "medium.glsl"

// the medium's light at p (6.18): GL's light level of a model there / 200,
// the liquids' light grid (the light maps with their styles, at least 24;
// outside it GL's least, 24) plus the dynamic lights by GL's rule (radius -
// distance, R_DrawViewModel's; the dark ones too); box: medium_grid_level's
float water_fog_light(vec3 p, inout int box)
{
	float level = -1;
	if(global_ubo.medium_grid != uvec2(0u))
		level = medium_grid_level(p, box);
	if(level < 0)
		level = MEDIUM_MIN_LEVEL;
	for(int i = 0; i < global_ubo.num_medium_dlights; i++)
	{
		vec4 dl = global_ubo.medium_dlights[i];
		level += max(dl.w - length(p - dl.xyz), 0.0);
	}
	return level / 200.0;
}

// the light the medium scatters towards the eye where it is dense: its color
// lit as GL lit a model (light: water_fog_light's, 8-bit, decoded as the
// textures' colors, 4.17), in the light of a full light map texel (4.10's
// dark_light_unit)
vec3 water_fog_radiance(int medium, float light)
{
	vec3 c = min(water_fog_color(medium) * light, vec3(1));
	return color_to_linear(c, global_ubo.color_srgb) * global_ubo.dark_light_unit;
}

// the medium's extinction per unit: it passes 1 - GL's tint opacity (water
// 128/255, slime and lava 150/255) at r_water_fog units (0 = clear)
float water_fog_density(int medium)
{
	if(global_ubo.water_fog <= 0 || (medium != MEDIUM_WATER && medium != MEDIUM_SLIME && medium != MEDIUM_LAVA))
		return 0;
	float tint = (medium == MEDIUM_WATER) ? 128.0 / 255.0 : 150.0 / 255.0;
	return -log(1.0 - tint) / global_ubo.water_fog;
}

const vec4 WATER_FOG_NONE = vec4(0, 0, 0, 1);

#define WATER_FOG_SAMPLES 4

// a path segment from a (its end nearer the eye) to b through the medium:
// dims what is behind it (fog.a) and returns the light it scatters towards
// the eye (times the path's throughput, for PT_TRANSPARENT). 6.18: lit where
// it is: the light scattered along it, the integral of density *
// transmittance * the light, is 1 - T times the mean of the light over
// WATER_FOG_SAMPLES parts that each scatter an equal share, taken at their
// middles (nearer a, where more of it reaches the eye)
vec3 water_fog_segment(inout vec4 fog, int medium, vec3 a, vec3 b)
{
	float density = water_fog_density(medium);
	if(density <= 0)
		return vec3(0);
	float d = length(b - a);
	float T = exp(-density * d);
	float drop = 1.0 - T;
	vec3 radiance = vec3(0);
	if(drop > 0)
	{
		vec3 dir = (b - a) / d;
		int box = -1;
		for(int k = 0; k < WATER_FOG_SAMPLES; k++)
		{
			float u = (float(k) + 0.5) / float(WATER_FOG_SAMPLES);
			float t = -log(1.0 - u * drop) / density;
			radiance += water_fog_radiance(medium, water_fog_light(a + dir * t, box));
		}
		radiance /= float(WATER_FOG_SAMPLES);
	}
	vec3 scattered = fog.a * drop * radiance;
	fog.a *= T;
	return scattered;
}

// a liquid surface's texture layer over what is seen through it
vec4 water_fog_layer(vec4 fog, vec3 color, float alpha)
{
	fog.rgb += fog.a * alpha * color;
	fog.a *= 1.0 - alpha;
	return fog;
}

// the path ends on this surface: what it shows through the medium
void water_fog_apply(vec4 fog, inout vec3 base_color, inout float metallic, inout float specular_factor, inout vec3 emissive)
{
	base_color = base_color * fog.a + fog.rgb;
	metallic *= fog.a;
	specular_factor *= fog.a;
	emissive *= fog.a;
}

// unpolarized Fresnel reflectance of a dielectric boundary, eta = n2 / n1;
// 1 at total internal reflection
float fresnel_dielectric(float cos_i, float eta)
{
	float sin_t2 = (1.0 - cos_i * cos_i) / (eta * eta);
	if(sin_t2 >= 1.0)
		return 1.0;
	float cos_t = sqrt(1.0 - sin_t2);
	float rs = (cos_i - eta * cos_t) / (cos_i + eta * cos_t);
	float rp = (eta * cos_i - cos_t) / (eta * cos_i + cos_t);
	return 0.5 * (rs * rs + rp * rp);
}

#define WATER_INDEX_OF_REFRACTION 1.33
#define WATER_WAVE_SLOPE 0.08	// at r_water_waves 1 (6.5's; 6.21's default 0.25: 0.02)

// the waves: the slope of Hexen II's turbulence (utils.glsl's lava_uv_warp,
// d_scan.c: a 128-unit cycle at 20 units a second, s shifted by a sine of t
// and t by one of s), so what is seen through and in the water wobbles as the
// water's texture does; in world units (a liquid's texture is 64 units wide).
// A liquid's surface isn't vertical (vk_world.c): its upward normal frames
// the waves, so its two coincident faces (up and down) are one surface
vec3 get_turbulence_normal(vec3 geo_normal, vec3 position)
{
	float side = (geo_normal.z < 0) ? -1.0 : 1.0;
	mat3 basis = construct_ONB_frisvad(geo_normal * side);
	vec2 p = vec2(dot(position, basis[0]), dot(position, basis[2]));
	vec2 phase = fract(p / 128.0 + global_ubo.time * 20.0 / 128.0) * 2 * M_PI;
	float slope = WATER_WAVE_SLOPE * global_ubo.water_waves;
	vec3 n = normalize(vec3(slope * sin(phase.y), 1, slope * sin(phase.x)));

	return (basis * n) * side;
}

/* 6.16: the caustic, the light's irradiance behind a liquid's surface,
 * depth units past the point where it crossed it, relative to a plain
 * surface's: the water texture's brightness there (tex_coord: warped as the
 * surface shows it) over its mean, so its painted veins (Hexen II's liquid
 * textures are caustic networks) send more light and its dark patches less.
 * Brightness (luminance's weights) in the textures' 8-bit values: their
 * last mip is the mean of those (blitted; a texture without its full mip
 * chain, a pack's DDS without mips, has no pattern), so the pattern's mean
 * is 1: it moves the light around, adding none. strength (r_water_caustics,
 * path_tracer_rgen.h's caustic_strength) stretches it around 1 (1: the
 * texture's contrast, 0: none); past where the dark parts reach 0 it adds
 * light (*rtex078, half dark: +14 % at 2, +40 % at 3); at most
 * CAUSTIC_MAX (no original reaches it at 3: a pack's dark albedo with
 * bright glints would make fireflies). Faded in over CAUSTIC_FADE_DEPTH
 * below the surface, blurred a mip level per doubling of the depth past
 * CAUSTIC_BLUR_DEPTH. Not physical: a look tied to the painting (6.14's
 * focusing of 6.5's sine waves, physically ~6 % at a pool's 256 units,
 * was invisible even at 16x) */
#define CAUSTIC_FADE_DEPTH 32.0
#define CAUSTIC_BLUR_DEPTH 64.0
#define CAUSTIC_MAX 8.0

float water_caustic(uint base_texture, vec2 tex_coord, float depth, float strength)
{
	if(base_texture == 0 || strength <= 0)
		return 1;

	float lod = log2(max(depth / CAUSTIC_BLUR_DEPTH, 1.0));
	float y = luminance(global_textureLod(base_texture, tex_coord, lod).rgb);
	float y_mean = luminance(global_textureLod(base_texture, tex_coord, 16.0).rgb);
	if(y_mean <= 0)
		return 1;

	float caustic = clamp(1.0 + strength * (y / y_mean - 1.0), 0.0, CAUSTIC_MAX);
	return mix(1.0, caustic, clamp(depth / CAUSTIC_FADE_DEPTH, 0.0, 1.0));
}

vec3 get_water_normal(uint material_id, vec3 geo_normal, vec3 tangent, vec3 position, bool local_space)
{
	// Add flow
	if((material_id & MATERIAL_FLAG_FLOWING) != 0)
	{
		position -= tangent * global_ubo.time * 32;
	}	

	// Remove the sign from the normal to make water simulation uniform,
	// regardless of which side we're looking at the surface from.
	// This is necessary to have caustics motion match the waves.
	vec3 unsigned_geo_normal = abs(geo_normal);

	// Construct a basis around the normal, get object-local 2D position.
	mat3 basis = construct_ONB_frisvad(unsigned_geo_normal);
	vec2 p = vec2(dot(position, basis[0]), dot(position, basis[2]));
	
	// Sample the texture and add a few instances of noise.

	const float speed = 2.5;

	vec2 uv1 = p.xy * 0.006 + global_ubo.time * vec2(0.01, 0.02) * speed;
	vec3 a = global_textureLod(global_ubo.water_normal_texture, uv1, 0).xyz;
	a.xy = a.xy * 2 - vec2(1);
	a.xy *= 0.3;

	vec2 uv2 = p.xy * 0.003 + global_ubo.time * vec2(0.013, 0.014) * speed;
	vec3 b = global_textureLod(global_ubo.water_normal_texture, uv2, 0).xyz;
	b.xy = b.xy * 2 - vec2(1);
	b.xy *= 0.5;

	vec2 uv3 = p.xy * 0.0061 + global_ubo.time * vec2(-0.01, -0.02) * speed;
	vec3 c = global_textureLod(global_ubo.water_normal_texture, uv3, 0).xyz;
	c.xy = c.xy * 2 - vec2(1);
	c.xy *= 0.3;

	vec3 n = normalize(a + b + c).xzy;

	if(local_space)
		return n;

	// Back into world space
	n = basis * n;

	// Restore the sign
	if(geo_normal.x < 0) n.x = -n.x;
	if(geo_normal.y < 0) n.y = -n.y;
	if(geo_normal.z < 0) n.z = -n.z;

	return n;
}

vec3 get_extinction_factors(int medium)
{
	vec3 factors = vec3(0);
	if(medium == MEDIUM_WATER)
		factors = vec3(0.035, 0.013, 0.012);
	else if(medium == MEDIUM_SLIME)
		factors = vec3(0.200, 0.010, 0.050);
	else if(medium == MEDIUM_LAVA)
		factors = vec3(0.001, 0.100, 0.300);

	return factors * global_ubo.pt_water_density;
}

vec3 extinction(int medium, float distance)
{
	vec3 factors = get_extinction_factors(medium);

	return exp(-factors * distance);
}
