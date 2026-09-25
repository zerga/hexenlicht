/* normal.glsl -- octahedral normal encoding of VboPrimitive normals and
 * tangents (a uint: RG16_UNORM), and half float packing
 *
 * From Quake II RTX (src/refresh/vkpt/shader/utils.glsl); vk_world.c has
 * the same encoding in C.
 *
 * Copyright (C) 2018 Christoph Schied
 * Copyright (C) 2019, NVIDIA CORPORATION. All rights reserved.
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

#ifndef NORMAL_GLSL
#define NORMAL_GLSL

vec3
decode_normal(uint enc)
{
	vec2 p = vec2(uvec2(enc & 0xffffu, enc >> 16)) / float(0xffff) * 2.0 - 1.0;
	vec3 n = vec3(p.x, p.y, 1.0 - abs(p.x) - abs(p.y));
	float t = max(0.0, -n.z);
	n.xy += mix(vec2(t), vec2(-t), greaterThanEqual(n.xy, vec2(0.0)));
	return normalize(n);
}

uint
encode_normal(vec3 normal)
{
	float inv_l1 = 1.0 / (abs(normal.x) + abs(normal.y) + abs(normal.z));
	vec2 p = normal.xy * inv_l1;
	p = (normal.z < 0.0) ? (1.0 - abs(p.yx)) * mix(vec2(-1.0), vec2(1.0), greaterThanEqual(p.xy, vec2(0.0))) : p;
	p = clamp(p.xy * 0.5 + 0.5, vec2(0.0), vec2(1.0));
	uvec2 u = uvec2(p * 0xffffu);
	return u.x | (u.y << 16);
}

uvec2
packHalf4x16(vec4 v)
{
	return uvec2(packHalf2x16(v.xy), packHalf2x16(v.zw));
}

vec4
unpackHalf4x16(uvec2 v)
{
	return vec4(unpackHalf2x16(v.x), unpackHalf2x16(v.y));
}

#endif	/* NORMAL_GLSL */
