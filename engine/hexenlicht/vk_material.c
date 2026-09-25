/* vk_material.c -- the material table
 *
 * A material is what a primitive's material ID indexes (hl_shared.h): the
 * textures to shade it with, factors, and the animation sequence it is in.
 * Materials are rebuilt on every map change (vk_world.c adds the world's
 * textures); VK_UploadMaterials writes them to the GPU table in Quake II
 * RTX's layout. For now only the base texture and the animation are set;
 * the PBR maps come with epic E5.
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

#include "quakedef.h"
#include "vk_local.h"
#include "shaders/hl_shared.h"

vk_buffer_t	vk_material_table;		/* MAX_MATERIALS * MATERIAL_UINTS uints */

static vk_material_t	materials[MAX_MATERIALS];
int			vk_num_materials;	/* including the unused index 0 */


/* IEEE 754 half precision, rounding to nearest */
uint16_t VK_FloatToHalf (float f)
{
	union { float f; uint32_t u; } v;
	uint32_t	sign, mant, h;
	int		exp;

	v.f = f;
	sign = (v.u >> 16) & 0x8000;
	mant = v.u & 0x7fffff;
	if (((v.u >> 23) & 0xff) == 0xff)		/* inf, nan */
		return (uint16_t)(sign | 0x7c00 | (mant ? 0x200 : 0));
	exp = (int)((v.u >> 23) & 0xff) - 127 + 15;
	if (exp >= 31)					/* too big: inf */
		return (uint16_t)(sign | 0x7c00);
	if (exp <= 0)					/* subnormal or zero */
	{
		if (exp < -10)
			return (uint16_t)sign;
		mant |= 0x800000;
		h = mant >> (14 - exp);
		if ((mant >> (13 - exp)) & 1)
			h++;
		return (uint16_t)(sign | h);
	}
	h = sign | ((uint32_t)exp << 10) | (mant >> 13);
	if (mant & 0x1000)
		h++;					/* may carry into the exponent, which is right */
	return (uint16_t)h;
}


void VK_ClearMaterials (void)
{
	memset (materials, 0, sizeof(materials[0]) * vk_num_materials);
	vk_num_materials = 1;		/* index 0 means "no material" */
}

/* a new material showing texture slot base_texture; returns its index */
int VK_AddMaterial (const char *name, int base_texture)
{
	vk_material_t	*m;
	int		index;

	if (vk_num_materials >= MAX_MATERIALS)
		Sys_Error ("Too many materials (%d)", MAX_MATERIALS);

	index = vk_num_materials++;
	m = &materials[index];
	memset (m, 0, sizeof(*m));
	q_strlcpy (m->name, name, sizeof(m->name));
	m->base_texture = base_texture;
	m->num_frames = 1;
	m->next_frame = index;
	return index;
}

vk_material_t *VK_GetMaterial (int index)
{
	if (index <= 0 || index >= vk_num_materials)
		Sys_Error ("%s: bad material %d", __thisfunc__, index);
	return &materials[index];
}

/* writes materials first .. first + count - 1 to the GPU table; nothing
 * may be using those entries (materials added since the last upload) */
void VK_UploadMaterialRange (int first, int count)
{
	uint32_t	*table, *d;
	int		i;

	if (first < 0 || count <= 0 || first + count > vk_num_materials)
		Sys_Error ("%s: bad range %d+%d", __thisfunc__, first, count);
	table = (uint32_t *) calloc (count * MATERIAL_UINTS, sizeof(uint32_t));
	if (!table)
		Sys_Error ("%s: out of memory", __thisfunc__);
	for (i = q_max (first, 1); i < first + count; i++)
	{
		const vk_material_t	*m = &materials[i];

		/* the factors are Quake II RTX's defaults (MAT_Reset) */
		d = table + (i - first) * MATERIAL_UINTS;
		d[0] = (uint32_t)m->base_texture & 0xffff;
		d[1] = ((uint32_t)m->mask_texture & 0xffff) << 16;
		d[2] = VK_FloatToHalf (1.0f) | ((uint32_t)VK_FloatToHalf (-1.0f) << 16);	/* bump scale, no roughness override */
		d[3] = VK_FloatToHalf (1.0f) | ((uint32_t)VK_FloatToHalf (1.0f) << 16);	/* metalness, emissive factor */
		d[4] = ((uint32_t)m->num_frames & 0xffff) | (((uint32_t)m->next_frame & 0xffff) << 16);
		d[5] = VK_FloatToHalf (1.0f) | ((uint32_t)VK_FloatToHalf (1.0f) << 16);	/* specular, base factor */
		d[6] = (uint32_t)m->alternate;
		d[7] = 0;
	}
	VK_UploadBuffer (&vk_material_table, (VkDeviceSize)first * MATERIAL_UINTS * sizeof(uint32_t), table,
			 (VkDeviceSize)count * MATERIAL_UINTS * sizeof(uint32_t));
	free (table);
}

/* writes all materials to the GPU table; nothing may be using it */
void VK_UploadMaterials (void)
{
	VK_UploadMaterialRange (0, vk_num_materials);
}


void VK_InitMaterials (void)
{
	VK_CreateBuffer (&vk_material_table, MAX_MATERIALS * MATERIAL_UINTS * sizeof(uint32_t),
			 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
			 VK_BUFFER_USAGE_TRANSFER_SRC_BIT |	/* vk_models check reads it back */
			 VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VK_MEMORY_DEVICE);
	vk_num_materials = 0;
	VK_ClearMaterials ();
}

void VK_ShutdownMaterials (void)
{
	VK_DestroyBuffer (&vk_material_table);
	vk_num_materials = 0;
}
