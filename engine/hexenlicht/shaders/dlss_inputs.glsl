/* dlss_inputs.glsl -- DLSS's inputs in the screen layout, which
 * checkerboard_interleave.comp writes with the lit image (vk_dlss.c):
 * hardware depth for SR and RR, and RR's guides from the G-buffer's
 * checkerboard fields: diffuse albedo, specular albedo, the shading normal
 * (world space) with the roughness in .w, the specular bounce's hit
 * distance (DECISIONS R53-R55).
 *
 * The interleave runs after the bounces, and with two of them the first
 * stores its hit in PT_SHADING_POSITION: a pixel without a surface (the
 * sky, nothing) is known by its view depth (PRIMARY_RAY_T_MAX, which
 * primary_rays.rgen and reflect_refract.rgen store for a miss).
 *
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

#include "brdf.glsl"

/* Brian Karis' analytic approximation of the split-sum environment BRDF
 * ("Physically Based Shading on Mobile", 2014), as debug_view.comp's */
vec3
dlss_env_brdf_approx(vec3 F0, float roughness, float NoV)
{
	const vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022);
	const vec4 c1 = vec4(1.0, 0.0425, 1.04, -0.04);
	vec4 r = roughness * c0 + c1;
	float a004 = min(r.x * r.x, exp2(-9.28 * NoV)) * r.x + r.y;
	vec2 AB = vec2(-1.04, 1.04) * a004 + r.zw;
	return F0 * AB.x + AB.y;
}

/* whether a field pixel holds a surface */
bool
dlss_has_surface(ivec2 field_pos)
{
	return abs(texelFetch(TEX_PT_VIEW_DEPTH_A, field_pos, 0).r) < float(PRIMARY_RAY_T_MAX);
}

/* the hardware depth of a D3D-style projection (0 at DLSS_Z_NEAR, 1 at
 * DLSS_Z_FAR), which vk_dlss.c gives DLSS as the projection's depth row:
 * Quake II RTX's projection has none. PT_VIEW_DEPTH holds the distance
 * along the ray (projection.glsl's length(view_pos)), the depth along the
 * view axis is that divided by the length of the pixel's primary ray
 * direction in view space at z = 1 (primary_rays.rgen: the pixel's center
 * plus the jitter; rectilinear projection) */
float
dlss_hardware_depth(ivec2 screen_pos, ivec2 field_pos)
{
	if (!dlss_has_surface(field_pos))
		return 1.0;

	float distance = abs(texelFetch(TEX_PT_VIEW_DEPTH_A, field_pos, 0).r);
	vec2 uv = (vec2(screen_pos) + 0.5 + global_ubo.sub_pixel_jitter) / vec2(global_ubo.width, global_ubo.height);
	vec2 ndc = uv * 2.0 - 1.0;
	vec2 xy = (ndc - vec2(global_ubo.P[2][0], global_ubo.P[2][1])) / vec2(global_ubo.P[0][0], global_ubo.P[1][1]);
	float z = max(distance / length(vec3(xy, 1.0)), DLSS_Z_NEAR);
	return clamp((DLSS_Z_FAR / (DLSS_Z_FAR - DLSS_Z_NEAR)) * (1.0 - DLSS_Z_NEAR / z), 0.0, 1.0);
}

/* RR's guides of the surface a field pixel holds */
struct DlssGuides
{
	vec3 albedo;
	vec3 specular;
	vec3 normal;
	float roughness;
};

DlssGuides
dlss_field_guides(ivec2 field_pos)
{
	DlssGuides g;
	vec3 dir = texelFetch(TEX_PT_VIEW_DIRECTION, field_pos, 0).xyz;

	g.albedo = vec3(0.0);
	g.specular = vec3(0.0);
	g.roughness = 1.0;
	if (!dlss_has_surface(field_pos))
	{
		g.normal = -dir;
		return g;
	}

	vec4 base = texelFetch(TEX_PT_BASE_COLOR_A, field_pos, 0);	/* .a: the specular factor */
	vec2 metallic_roughness = texelFetch(TEX_PT_METALLIC_A, field_pos, 0).xy;
	vec3 base_reflectivity;

	g.normal = decode_normal(texelFetch(TEX_PT_NORMAL_A, field_pos, 0).r);
	g.roughness = metallic_roughness.y;
	get_reflectivity(base.rgb, metallic_roughness.x, g.albedo, base_reflectivity);
	g.specular = dlss_env_brdf_approx(base_reflectivity, g.roughness, clamp(-dot(g.normal, dir), 0.0, 1.0)) * base.a;
	return g;
}

/* the guides of the other field's pixel at field_pos + offset, added to
 * sum with weight 1 when it is inside the rendered part */
void
dlss_add_neighbor(inout DlssGuides sum, inout float weight, ivec2 field_pos)
{
	if (any(lessThan(field_pos, ivec2(0))) || field_pos.x >= global_ubo.width || field_pos.y >= global_ubo.height)
		return;

	DlssGuides g = dlss_field_guides(field_pos);
	sum.albedo += g.albedo;
	sum.specular += g.specular;
	sum.normal += g.normal;
	sum.roughness += g.roughness;
	weight += 1.0;
}

/* the inputs of screen pixel screen_pos: its field pixel field_pos, the
 * field pixel its motion vector came from (depth goes with the motion), and
 * where the interleave blurs the checkerboard of a translucent surface
 * (blend), RR's guides blended the same way: half the pixel's, half its
 * other-field neighbors' (the cross the interleave samples) */
void
dlss_write_inputs(ivec2 screen_pos, ivec2 field_pos, ivec2 motion_pos, int other_side_offset, bool blend, bool guides)
{
	imageStore(IMG_DLSS_DEPTH, screen_pos, vec4(dlss_hardware_depth(screen_pos, motion_pos)));
	if (!guides)
		return;

	DlssGuides g = dlss_field_guides(field_pos);
	if (blend)
	{
		DlssGuides sum;
		float weight = 0.0;

		sum.albedo = vec3(0.0);
		sum.specular = vec3(0.0);
		sum.normal = vec3(0.0);
		sum.roughness = 0.0;
		dlss_add_neighbor(sum, weight, field_pos + ivec2(other_side_offset, 1));
		dlss_add_neighbor(sum, weight, field_pos + ivec2(other_side_offset, -1));
		if (screen_pos.x != 0 && screen_pos.x != global_ubo.width - 1)
		{
			dlss_add_neighbor(sum, weight, field_pos + ivec2(other_side_offset, 0));
			dlss_add_neighbor(sum, weight, field_pos + ivec2(other_side_offset + (((screen_pos.x & 1) != 0) ? 1 : -1), 0));
		}
		if (weight > 0.0)
		{
			g.albedo = mix(g.albedo, sum.albedo / weight, 0.5);
			g.specular = mix(g.specular, sum.specular / weight, 0.5);
			g.normal = normalize(g.normal + sum.normal / weight + vec3(1e-6));
			g.roughness = mix(g.roughness, sum.roughness / weight, 0.5);
		}
	}

	imageStore(IMG_DLSS_ALBEDO, screen_pos, vec4(g.albedo, 1.0));
	imageStore(IMG_DLSS_SPEC_ALBEDO, screen_pos, vec4(g.specular, 1.0));
	imageStore(IMG_DLSS_NORMAL_ROUGHNESS, screen_pos, vec4(g.normal, g.roughness));
	imageStore(IMG_DLSS_SPEC_HIT, screen_pos, vec4(texelFetch(TEX_PT_SPECULAR_HIT_DIST, field_pos, 0).r));
}
