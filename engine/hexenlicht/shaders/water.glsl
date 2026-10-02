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
 * color lit as a model at the camera, added to PT_TRANSPARENT segment by
 * segment. A path's medium so far is a vec4: what its final surface's
 * albedo is multiplied by (.a: the medium's and the texture layers'
 * transmittance) and the texture layers' albedo added to it (.rgb);
 * water_fog_apply puts it into the final surface's material, so a layer
 * is lit as what is seen through it. Quake II RTX's get_water_normal and
 * extinction stay for r_water 0 (its water normal map is in its media:
 * water_normal_texture is 0). 6.14: water_caustic, the waves' pattern in
 * the light through a liquid's surface (path_tracer_rgen.h's caustic ray) */

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

// the light the medium scatters towards the eye where it is dense: its color
// lit as GL lit a model at the camera (R_DrawViewModel: cl.light_level / 200,
// the light maps with their styles and the dynamic lights there, 8-bit,
// decoded as the textures' colors, 4.17), in the light of a full light map
// texel (4.10's dark_light_unit); one light for the whole medium
vec3 water_fog_radiance(int medium)
{
	vec3 c = min(water_fog_color(medium) * global_ubo.water_light, vec3(1));
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

// a path segment of length d through the medium: dims what is behind it
// (fog.a) and returns the light it scatters towards the eye (times the
// path's throughput, for PT_TRANSPARENT)
vec3 water_fog_segment(inout vec4 fog, int medium, float d)
{
	float density = water_fog_density(medium);
	if(density <= 0)
		return vec3(0);
	float T = exp(-density * d);
	vec3 scattered = fog.a * (1.0 - T) * water_fog_radiance(medium);
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
#define WATER_WAVE_SLOPE 0.08	// at r_water_waves 1

// the waves' frame and phase at a point of a liquid's surface (see below)
vec2 turbulence_phase(vec3 geo_normal, vec3 position, out mat3 basis, out float side)
{
	side = (geo_normal.z < 0) ? -1.0 : 1.0;
	basis = construct_ONB_frisvad(geo_normal * side);
	vec2 p = vec2(dot(position, basis[0]), dot(position, basis[2]));
	return fract(p / 128.0 + global_ubo.time * 20.0 / 128.0) * 2 * M_PI;
}

// the waves: the slope of Hexen II's turbulence (utils.glsl's lava_uv_warp,
// d_scan.c: a 128-unit cycle at 20 units a second, s shifted by a sine of t
// and t by one of s), so what is seen through and in the water wobbles as the
// water's texture does; in world units (a liquid's texture is 64 units wide).
// A liquid's surface isn't vertical (vk_world.c): its upward normal frames
// the waves, so its two coincident faces (up and down) are one surface
vec3 get_turbulence_normal(vec3 geo_normal, vec3 position)
{
	mat3 basis;
	float side;
	vec2 phase = turbulence_phase(geo_normal, position, basis, side);
	float slope = WATER_WAVE_SLOPE * global_ubo.water_waves;
	vec3 n = normalize(vec3(slope * sin(phase.y), 1, slope * sin(phase.x)));

	return (basis * n) * side;
}

/* 6.14: the waves' caustic: the light's irradiance behind a liquid's
 * surface, depth units past the point where it crossed it, relative to a
 * flat surface's. A ray refracted by the waves lands displaced by depth
 * times (1 - 1/1.33) times their slope, and the irradiance is 1 / the
 * determinant of that mapping. The turbulence's slopes are a shear (the x
 * tilt varies with y only, the y tilt with x only), so nothing focuses to
 * first order: the determinant is 1 - m cos(phase.x) cos(phase.y), with
 * m = (depth / 1026)^2 at r_water_waves 1, about 6 % at a pool's 256 units.
 * Not physical: the focus is brought to CAUSTIC_FOCUS_DEPTH and m held at
 * CAUSTIC_MAX_FOCUS at most (focal spots up to ~7x). The pattern is taken
 * where the straight shadow ray crosses the surface, so it is divided by
 * its mean there, 1 / agm(1, sqrt(1 - m^2)) (2/pi K(m)): it moves the light
 * around, adding none */
#define CAUSTIC_FOCUS_DEPTH 256.0
#define CAUSTIC_MAX_FOCUS 0.9

float water_caustic(vec3 geo_normal, vec3 position, float depth)
{
	float m = min(square(depth * global_ubo.water_waves / CAUSTIC_FOCUS_DEPTH), CAUSTIC_MAX_FOCUS);
	if(m <= 0)
		return 1;

	mat3 basis;
	float side;
	vec2 phase = turbulence_phase(geo_normal, position, basis, side);

	float a = 1, b = sqrt(1 - m * m);
	for(int i = 0; i < 4; i++)
	{
		float a_next = 0.5 * (a + b);
		b = sqrt(a * b);
		a = a_next;
	}

	return a / (1 - m * cos(phase.x) * cos(phase.y));
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
