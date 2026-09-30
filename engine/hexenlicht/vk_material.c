/* vk_material.c -- the material table
 *
 * A material is what a primitive's material ID indexes
 * (shaders/vertex_buffer.h): the textures to shade it with, factors, and
 * the animation sequence it is in.
 * Materials are rebuilt on every map change (vk_world.c adds the world's
 * textures, vk_skin.c the skins); VK_UploadMaterials writes them to the
 * GPU table in Quake II RTX's layout, with Hexen II's alternate animation
 * and (5.3) the roughness and metallic texture in its spare words.
 * A material is its original texture slot, the slot it shows without
 * files (the original, or a player's translated skin) and flags (a skin,
 * a cutout, lava, a light model's flame); VK_ApplyMaterialFiles makes the
 * rest from those and the texture's material files (5.3, vk_matfiles.c,
 * MATERIALS.md): the albedo, the normal map, the roughness and metallic
 * map, the emission and the .mat's factors, or the defaults without
 * files (the original, roughness 1, metallic 0, a flat normal, the
 * specular r_specular: as before E5). The emission: lava its albedo
 * (4.5, r_lava_light), the light models' flames their skin's fake
 * emissive texture (vk_emissive.c), an _e file or the .mat's emissive
 * key on any lit texture; times r_emissive_scale and the key. The .mat's
 * kind (5.5) is kept with the material: a skin's instance takes it every
 * frame (vk_instance.c), the world's primitives at map load (vk_world.c).
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

vk_buffer_t	vk_material_table;		/* MAX_PBR_MATERIALS * MATERIAL_UINTS uints */

static vk_material_t	materials[MAX_PBR_MATERIALS];
int			vk_num_materials;	/* including the unused index 0 */

static void SpecularChanged (cvar_t *var);

/* the materials' specular factor: 0 matte, as GL (4.9); Quake II RTX's 1 */
static cvar_t	r_specular = {"r_specular", "0", CVAR_NONE};


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

float VK_HalfToFloat (uint16_t h)
{
	union { float f; uint32_t u; } v;
	uint32_t	sign = (uint32_t)(h & 0x8000) << 16, exp = (h >> 10) & 0x1f, mant = h & 0x3ff;

	if (exp == 0)			/* zero, subnormal */
	{
		v.f = mant * (1.0f / 16777216.0f);	/* 2^-24 */
		v.u |= sign;
		return v.f;
	}
	if (exp == 31)			/* inf, nan */
		v.u = sign | 0x7f800000 | (mant << 13);
	else
		v.u = sign | ((exp + 112) << 23) | (mant << 13);
	return v.f;
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

	if (vk_num_materials >= MAX_PBR_MATERIALS)
		Sys_Error ("Too many materials (%d)", MAX_PBR_MATERIALS);

	index = vk_num_materials++;
	m = &materials[index];
	memset (m, 0, sizeof(*m));
	q_strlcpy (m->name, name, sizeof(m->name));
	m->texture = m->original = m->base_texture = base_texture;
	m->emissive_factor = 1.0f;
	m->roughness = 1.0f;
	m->bump = 1.0f;
	m->specular = -1.0f;
	m->num_frames = 1;
	m->next_frame = index;
	return index;
}

/* the material's textures, factors and emission from what it is (texture,
 * original, flags) and its texture's material files, or the defaults
 * without them (MATERIALS.md's Defaults) */
void VK_ApplyMaterialFiles (int index)
{
	vk_material_t		*m = VK_GetMaterial (index);
	const vk_matset_t	*s = VK_MaterialSet (m->texture, NULL, (m->flags & VK_MAT_SKIN) ? MATUSE_SKIN : MATUSE_WORLD);
	qboolean		albedo = s && s->albedo && !(m->flags & VK_MAT_TRANSLATED);
	int			own = s ? s->emissive : 0;
	float			key = s ? s->emission : -1.0f;

	m->base_texture = albedo ? s->albedo : m->original;
	m->mask_texture = 0;
	if (m->flags & VK_MAT_CUTOUT)	/* an albedo without alpha keeps the original's holes */
		m->mask_texture = (albedo && s->albedo_alpha) ? s->albedo : m->original;
	m->normal_texture = s ? s->normal : 0;
	m->normal_bc5 = s && s->normal && s->normal_bc5;
	m->rm_texture = s ? s->rm : 0;
	/* glTF's rule: the value without a map, a factor on one; a missing _r
	 * or _m is 255 in the packed texture, so its factor is its value */
	m->roughness = (s && s->roughness >= 0.0f) ? s->roughness : 1.0f;
	m->metallic = (s && s->metallic >= 0.0f) ? s->metallic : ((s && s->metallic_map) ? 1.0f : 0.0f);
	m->bump = (s && s->bump >= 0.0f) ? s->bump : 1.0f;
	/* an authored roughness makes it physically based (M4), else r_specular */
	m->specular = (s && s->specular >= 0.0f) ? s->specular :
		      ((s && (s->roughness_map || s->roughness >= 0.0f)) ? 1.0f : -1.0f);
	m->kind = s ? s->kind : MATKIND_REGULAR;	/* a skin's instance takes it (5.5); the world's primitives at map load */

	if (m->flags & VK_MAT_LAVA)
		m->emissive_texture = VK_LavaEmits () ? (own ? own : m->base_texture) : 0;
	else if (m->flags & VK_MAT_FLAME)
		m->emissive_texture = !VK_ModelsEmit () ? 0 : own ? own : VK_EmissiveSkin (m->original);	/* made when first asked for */
	else if (m->flags & VK_MAT_GLOW)
		m->emissive_texture = own ? own : m->base_texture;	/* 6.2: a glowing projectile's whole skin */
	else
		m->emissive_texture = own ? own : ((key > 0.0f) ? m->base_texture : 0);
	m->emissive_factor = m->emissive_texture ? VK_EmissiveScale () * ((key >= 0.0f) ? key : 1.0f) : 1.0f;
}

/* every material's files again, then the whole table; nothing may be
 * using it (r_reloadmaterials, r_materials, r_lava_light and the like) */
void VK_ReapplyMaterials (void)
{
	int	i;

	for (i = 1; i < vk_num_materials; i++)
		VK_ApplyMaterialFiles (i);
	VK_UploadMaterials ();
}

/* a material made for this texture, shown slot and flags, 0 = none */
int VK_FindMaterial (int texture, int original, int flags)
{
	int	i;

	for (i = 1; i < vk_num_materials; i++)
	{
		if (materials[i].texture == texture && materials[i].original == original && materials[i].flags == flags)
			return i;
	}
	return 0;
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

		d = table + (i - first) * MATERIAL_UINTS;
		d[0] = ((uint32_t)m->base_texture & 0xffff) | (((uint32_t)m->normal_texture & 0xffff) << 16);
		d[1] = ((uint32_t)m->emissive_texture & 0xffff) | (((uint32_t)m->mask_texture & 0xffff) << 16);
		d[2] = VK_FloatToHalf (m->bump) | ((uint32_t)VK_FloatToHalf (m->roughness) << 16);
		d[3] = VK_FloatToHalf (m->metallic) | ((uint32_t)VK_FloatToHalf (m->emissive_factor) << 16);
		d[4] = ((uint32_t)m->num_frames & 0xffff) | (((uint32_t)m->next_frame & 0xffff) << 16);
		d[5] = VK_FloatToHalf ((m->specular >= 0.0f) ? m->specular : q_max (r_specular.value, 0.0f)) |
		       ((uint32_t)VK_FloatToHalf (1.0f) << 16);	/* specular, base factor */
		d[6] = (uint32_t)m->alternate;
		d[7] = ((uint32_t)m->rm_texture & 0xffff) | ((m->normal_bc5 ? MATERIAL_NORMALS_BC5 : 0u) << 16);
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


/* a new r_specular: the whole table again, once no frame uses it */
static void SpecularChanged (cvar_t *var)
{
	(void)var;
	if (vk_num_materials <= 1 || !vk_material_table.buffer)
		return;
	vkDeviceWaitIdle (vk.device);
	VK_UploadMaterials ();
}

void VK_InitMaterials (void)
{
	Cvar_RegisterVariable (&r_specular);
	Cvar_SetCallback (&r_specular, SpecularChanged);
	VK_CreateBuffer (&vk_material_table, MAX_PBR_MATERIALS * MATERIAL_UINTS * sizeof(uint32_t),
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
