/* material.glsl -- reading the material table and animating materials
 *
 * Include after hl_shared.h and after declaring the material table as a
 * readable array of uints named material_table (MATERIAL_UINTS per
 * material, see hl_shared.h), e.g. in a storage buffer block.
 *
 * Adapted from Quake II RTX (src/refresh/vkpt/shader/vertex_buffer.h),
 * extended with Hexen II's alternate animations.
 *
 * Copyright (C) 2018 Christoph Schied
 * Copyright (C) 2019-2021, NVIDIA CORPORATION. All rights reserved.
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

#ifndef MATERIAL_GLSL
#define MATERIAL_GLSL

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
	uint num_frames;
	uint next_frame;
	uint alternate;		/* Hexen II: first material of the alternate animation, 0 = none */
};

MaterialInfo
get_material_info(uint material_id)
{
	uint i = (material_id & MATERIAL_INDEX_MASK) * MATERIAL_UINTS;
	MaterialInfo minfo;

	minfo.base_texture = material_table[i + 0] & 0xffff;
	minfo.normals_texture = material_table[i + 0] >> 16;
	minfo.emissive_texture = material_table[i + 1] & 0xffff;
	minfo.mask_texture = material_table[i + 1] >> 16;
	minfo.bump_scale = unpackHalf2x16(material_table[i + 2]).x;
	minfo.roughness_override = unpackHalf2x16(material_table[i + 2]).y;
	minfo.metalness_factor = unpackHalf2x16(material_table[i + 3]).x;
	minfo.emissive_factor = unpackHalf2x16(material_table[i + 3]).y;
	minfo.num_frames = material_table[i + 4] & 0xffff;
	minfo.next_frame = (material_table[i + 4] >> 16) & MATERIAL_INDEX_MASK;
	minfo.specular_factor = unpackHalf2x16(material_table[i + 5]).x;
	minfo.base_factor = unpackHalf2x16(material_table[i + 5]).y;
	minfo.alternate = material_table[i + 6] & MATERIAL_INDEX_MASK;

	return minfo;
}

/* The material shown at animation frame 'frame' = int(cl.time * 5), as
 * Hexen II's R_TextureAnimation picks it. Surfaces reference the first
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

	if (frame > 0 && minfo.num_frames > 1)
	{
		frame = frame % int(minfo.num_frames);
		while (frame --> 0)
		{
			new_material = minfo.next_frame;
			minfo = get_material_info(new_material);
		}
	}

	return new_material | (material & ~MATERIAL_INDEX_MASK);	/* keep kind and flags */
}

#endif	/* MATERIAL_GLSL */
