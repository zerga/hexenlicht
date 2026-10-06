/* medium.glsl -- the liquids' light grid (6.18)
 *
 * GL's light level of a model at a point in a liquid (without the dynamic
 * lights), from the grid vk_medium.c bakes at map load (its layout is in
 * hl_shared.h): the box whose lattice holds the point, the eight lattice
 * points around it, each the sum of its light styles' levels times the
 * styles' values this frame, at least GL's 24, interpolated trilinearly.
 * vk_medium.c's GridLevel is the same on the CPU (the vk_medium command
 * compares the two through medium_check.comp). No two boxes' lattices
 * overlap (vk_medium.c), so the box hint only saves the search.
 *
 * Include after hl_shared.h, after defining medium_table as a readable
 * array of uvec2s (e.g. through a buffer reference) and medium_style(s)
 * as light style s's value (1 = 256).
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

#ifndef MEDIUM_GLSL
#define MEDIUM_GLSL

/* a lattice point's level: its styles' levels times their values, at
 * least GL's 24 */
float
medium_point_level(uint index)
{
	uvec2 p = medium_table[index];
	float level = 0;

	for (uint k = 0u; k < 4u; k++)
	{
		uint s = (p.x >> (8u * k)) & 0xffu;
		if (s != MEDIUM_NO_STYLE)
			level += float((p.y >> (8u * k)) & 0xffu) * medium_style(s);
	}
	return max(level, MEDIUM_MIN_LEVEL);
}

/* whether box b's lattice holds the cell at lattice index i (its eight
 * points); the point's index in the box (r), the box's size and its first
 * point */
bool
medium_box_holds(int b, ivec3 i, out ivec3 r, out ivec3 size, out uint first)
{
	uint base = MEDIUM_GRID_HEADER_UVEC2S + uint(b) * MEDIUM_BOX_UVEC2S;
	uvec2 lo_xy = medium_table[base];
	uvec2 lo_z = medium_table[base + 1];
	uvec2 size_xy = medium_table[base + 2];
	uvec2 size_z = medium_table[base + 3];

	r = i - ivec3(int(lo_xy.x), int(lo_xy.y), int(lo_z.x));
	size = ivec3(int(size_xy.x), int(size_xy.y), int(size_z.x));
	first = lo_z.y;
	return all(greaterThanEqual(r, ivec3(0))) && all(lessThan(r, size - 1));
}

/* GL's light level at p without the dynamic lights, < 0 outside the
 * grid; box: the box to try first (-1: none), then the one found (the
 * points along a path are mostly in one) */
float
medium_grid_level(vec3 p, inout int box)
{
	int num_boxes = int(medium_table[0].x);
	float cell = uintBitsToFloat(medium_table[0].y);
	vec3 g = p / cell;
	ivec3 i = ivec3(floor(g));
	ivec3 r, size;
	uint first;

	if (box < 0 || box >= num_boxes || !medium_box_holds(box, i, r, size, first))
	{
		box = -1;
		for (int b = 0; b < num_boxes; b++)
		{
			if (medium_box_holds(b, i, r, size, first))
			{
				box = b;
				break;
			}
		}
		if (box < 0)
			return -1;
	}

	vec3 f = g - vec3(i);
	uint sy = uint(size.x);
	uint sz = uint(size.x * size.y);
	uint i000 = first + uint(r.x) + sy * uint(r.y) + sz * uint(r.z);

	float x00 = mix(medium_point_level(i000), medium_point_level(i000 + 1u), f.x);
	float x10 = mix(medium_point_level(i000 + sy), medium_point_level(i000 + sy + 1u), f.x);
	float x01 = mix(medium_point_level(i000 + sz), medium_point_level(i000 + sz + 1u), f.x);
	float x11 = mix(medium_point_level(i000 + sz + sy), medium_point_level(i000 + sz + sy + 1u), f.x);
	return mix(mix(x00, x10, f.y), mix(x01, x11, f.y), f.z);
}

#endif	/* MEDIUM_GLSL */
