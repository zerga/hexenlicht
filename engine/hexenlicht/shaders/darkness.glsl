/* darkness.glsl -- GL's dark lights and the weapon's least light (4.10)
 *
 * Dark lights are the game's dynamic lights with the dark flag
 * (EF_DARKLIGHT: the Necromancer's invincibility). GL subtracts them from
 * the lightmap texels they reach (gl_rsurf.c's R_AddDynamicLights, the
 * GL_RGBA path), 2 (R - |h| - rho) / 255 of a texel (R its radius, h the
 * plane distance, rho the in-plane distance; where R - |h| - rho is above
 * its minlight), and clips the texel at 0 (per channel); here alias models
 * and the turbulent surfaces, which have no lightmaps in GL, get no
 * darkness (GL and the software renderer add the same light to models as
 * any other). vk_light.c puts them into the UBO's dyn_light_data after the
 * lights the shaders sample (num_dyn_lights), num_dark_lights of them, the
 * minlight in spot_data's bits. direct_lighting.rgen leaves the amount at
 * the primary world surface (dark_light_amount; 0 on models, the weapon and
 * turbulent surfaces) in PT_THROUGHPUT's w, which nothing reads after
 * reflect_refract.rgen, and the composites (asvgf_atrous.comp's last pass,
 * compositing.comp) scale the surface's light by dark_light_factor: the
 * light in GL's texel units, less the amount, clipped at 0. A texel is the
 * light to the power 1 / maplight_gamma in units of dark_light_unit, the
 * light that shows a texture at its own color (1 at the fixed exposure
 * 2^r_map_exposure; the lit image was calibrated to GL's look, 4.9, 4.15);
 * from its luminance, so the hue stays (GL's dim channels go black first).
 * Only while there are dark lights: otherwise the w keeps its path length.
 *
 * The weapon's least light is GL's R_DrawViewModel's "always give some
 * light on gun": at least 24 per channel, a vertex color of 24 / 200 of
 * the texture (times GL's shading dots, 1 in the middle of their table),
 * which GL multiplied the texture's 8-bit color by (weapon_min_light; with
 * 4.17's 2.2 power exactly (24 / 200)^2.2 in linear light).
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

#ifndef DARKNESS_GLSL
#define DARKNESS_GLSL

#include "transfer.glsl"

/* how much of a GL lightmap texel the dark lights take from a world
 * surface at p with the geometric normal gn */
float
dark_light_amount(vec3 p, vec3 gn)
{
	float amount = 0;

	for(int i = 0; i < global_ubo.num_dark_lights; i++)
	{
		DynLightData dl = global_ubo.dyn_light_data[global_ubo.num_dyn_lights + i];
		vec3 v = p - dl.center;
		float h = dot(v, gn);
		float rad = dl.radius - abs(h);
		float minlight = uintBitsToFloat(dl.spot_data);
		float rho = length(v - h * gn);

		if(rad >= minlight && rho < rad - minlight)
			amount += 2 * (rad - rho) / 255;
	}
	return amount;
}

/* the factor on a surface's light (diffuse, demodulated) that takes the
 * amount from it in GL's texel units */
float
dark_light_factor(vec3 light, float amount)
{
	float e = luminance(light);
	if(!(amount > 0) || !(e > 0) || isinf(e) || !(global_ubo.dark_light_unit > 0))
		return 1;

	float gamma = global_ubo.maplight_gamma;
	float texel = pow(e / global_ubo.dark_light_unit, 1 / gamma);
	return pow(max(1 - amount / texel, 0), gamma);	// a texel of 0: 0, of inf: 1
}

/* the least diffuse light (demodulated) on the weapon with this albedo */
vec3
weapon_min_light(vec3 albedo)
{
	const float least = 24.0 / 200.0;
	vec3 a = max(albedo, vec3(1e-4));
	return color_to_linear(linear_to_color(a, global_ubo.color_srgb) * least, global_ubo.color_srgb) / a;
}

#endif	/* DARKNESS_GLSL */
