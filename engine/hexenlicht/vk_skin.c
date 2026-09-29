/* vk_skin.c -- alias model skins
 *
 * VK_SkinMaterial picks the skin an alias entity shows the way the GL
 * renderer's R_DrawAliasModel binds it: skins 100 and up are the pictures
 * gfx/skin<n>.lmp (stone, ice), others the model's skin, whose four slots
 * cycle at 10 Hz (skin groups; gl_model.c fills them), and players with
 * translated colors show their own skin. It returns the material of that
 * texture: one per skin texture, created on demand (the precached models'
 * on map load, VK_AddSkinMaterials), with the skin as its cutout mask for
 * EF_HOLEY models, and one more with the skin's emissive texture for the
 * light models' flames (4.5, vk_emissive.c), made when first shown. The
 * skin's material files (5.3, vk_matfiles.c) apply to its materials
 * (VK_ApplyMaterialFiles): a replaced albedo with alpha is also a masked
 * skin's mask, one without keeps the original as the mask; a player whose
 * colors leave the skin as it is (the translation changes no texel, as
 * top and bottom color 0 do) shows the replaced albedo, other players
 * their translated original; both take the other maps.
 *
 * R_TranslatePlayerSkin is gl_rmisc.c's per-class color translation of the
 * player skins (gfx/player.lmp), with two differences: the translated
 * 8-bit skin goes through GL_LoadTexture with the model's own texture mode,
 * so the Demoness keeps her cutouts (GL converts it to RGBA directly and
 * loses them), and it keeps the skin's size and gets mipmaps like the other
 * skins (GL resamples to 512x256).
 *
 * Copyright (C) 1996-1997  Id Software, Inc.
 * Copyright (C) 1997-1998  Raven Software Corp.
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
#include "r_scene.h"
#include "shaders/hl_shared.h"

byte		*playerTranslation;	/* gfx/player.lmp */

/* player class skin color offsets into playerTranslation (gl_rmisc.c) */
const int color_offsets[MAX_PLAYER_CLASS] =
{
	2 * 14 * 256,
	0,
	1 * 14 * 256,
	2 * 14 * 256,
	2 * 14 * 256
};

/* the same cvar as the GL renderer: no translated player skins */
cvar_t		gl_nocolors = {"gl_nocolors", "0", CVAR_NONE};

extern qmodel_t	*player_models[MAX_PLAYER_CLASS];		/* cl_parse.c */
extern byte	player_8bit_texels[MAX_PLAYER_CLASS][620*245];	/* gl_model.c */

/* [emissive][cutout][texture slot]: the skin's material, 0 = none yet */
static short	skin_materials[2][2][VK_MAX_TEXTURES];
static qboolean	batch_materials;	/* VK_AddSkinMaterials: the caller uploads them */
static qboolean	player_skin_as_is[MAX_CLIENTS];	/* R_TranslatePlayerSkin: the player's colors leave the skin as it is */


/* ==========================================================================
 * Materials
 * ========================================================================== */

/* the texture mode gl_model.c's Mod_LoadAllSkins gives a model's skins */
static int SkinTextureMode (int model_flags)
{
	int	tex_mode = TEX_DEFAULT | TEX_MIPMAP;

	if (model_flags & EF_TRANSPARENT)
		tex_mode |= TEX_TRANSPARENT;
	else if (model_flags & EF_HOLEY)
		tex_mode |= TEX_HOLEY;
	else if (model_flags & EF_SPECIAL_TRANS)
		tex_mode |= TEX_SPECIAL_TRANS;
	return tex_mode;
}

/* the skins of these models have holes (TEX_HOLEY) */
qboolean VK_ModelHasCutouts (const qmodel_t *model)
{
	return (SkinTextureMode (model->flags) & TEX_HOLEY) != 0;
}

/* slot: the texture shown; files: the skin whose material files apply
 * (slot, or the model's skin under a player's colors, 5.3); name: for
 * textures loaded without one (pictures); emissive: with the skin's
 * emissive texture (vk_emissive.c: the light models' flames, an _e file),
 * made when first asked for; translated: a player's colors, which keep
 * the original albedo (MATERIALS.md) */
static int SkinMaterial (int slot, int files, qboolean cutout, const char *name, qboolean emissive, qboolean translated)
{
	const char	*slash;
	vk_material_t	*mat;
	int		m;

	if (slot < 0 || slot >= VK_MAX_TEXTURES)
		slot = 0;
	if (files < 0 || files >= VK_MAX_TEXTURES)
		files = slot;
	m = skin_materials[emissive][cutout][slot];
	if (m)
	{
		int	flags = VK_MAT_SKIN | (cutout ? VK_MAT_CUTOUT : 0) | (translated ? VK_MAT_TRANSLATED : 0) |
				(emissive ? VK_MAT_FLAME : 0);

		mat = VK_GetMaterial (m);
		if (mat->texture == files && !(mat->flags & VK_MAT_TRANSLATED) == !translated)
			return m;
		/* a player's skin under another model or other colors: one made
		 * before, else another */
		if ((m = VK_FindMaterial (files, slot, flags)) > 0)
		{
			skin_materials[emissive][cutout][slot] = (short)m;
			return m;
		}
	}

	if (VK_TextureName (slot)[0])
		name = VK_TextureName (slot);	/* e.g. models/paladin.mdl_0 -> paladin.mdl_0 */
	slash = strrchr (name, '/');
	m = VK_AddMaterial (slash ? slash + 1 : name, slot);
	mat = VK_GetMaterial (m);
	mat->texture = files;
	mat->flags = VK_MAT_SKIN | (cutout ? VK_MAT_CUTOUT : 0) | (translated ? VK_MAT_TRANSLATED : 0);
	if (emissive)
		mat->flags |= VK_MAT_FLAME;	/* the skin's emissive texture: VK_ApplyMaterialFiles */
	if (!VK_TextureName (files)[0])
		VK_MaterialSet (files, name, MATUSE_SKIN);	/* a picture: its set takes the name */
	VK_ApplyMaterialFiles (m);
	skin_materials[emissive][cutout][slot] = (short)m;
	if (!batch_materials)
		VK_UploadMaterialRange (m, 1);
	return m;
}

/* after VK_LoadWorld cleared the materials */
void VK_ClearSkins (void)
{
	memset (skin_materials, 0, sizeof(skin_materials));
}

/* on map load: the materials of a model's skins; the caller uploads them */
void VK_AddSkinMaterials (qmodel_t *model)
{
	const aliashdr_t	*hdr = (const aliashdr_t *) Mod_Extradata (model);
	qboolean		cutout = VK_ModelHasCutouts (model);
	int			i, j;

	batch_materials = true;
	for (i = 0; i < hdr->numskins && i < MAX_SKINS; i++)
	{
		for (j = 0; j < 4; j++)
			SkinMaterial ((int)hdr->gl_texturenum[i][j], (int)hdr->gl_texturenum[i][j], cutout, model->name, false, false);
	}
	batch_materials = false;
}


/* ==========================================================================
 * The skin an entity shows
 * ========================================================================== */

static qboolean IsPlayerModel (const qmodel_t *model)
{
	int	i;

	for (i = 0; i < MAX_PLAYER_CLASS; i++)
	{
		if (player_models[i] && model == player_models[i])
			return true;
	}
	return false;
}

/* R_DrawAliasModel's choice of texture, as a material index (emissive: with
 * the skin's emissive texture, if it has one); *bad_skin is set for skin
 * numbers the model doesn't have (GL prints them with developer 1; nothing
 * may print inside a frame) */
int VK_SkinMaterial (const scene_entity_t *e, const aliashdr_t *hdr, qboolean emissive, qboolean *bad_skin)
{
	char		name[MAX_QPATH] = "skin";
	int		skinnum = e->skinnum, slot, files, player;
	qboolean	translated = false;

	*bad_skin = false;
	if (skinnum >= 100 && skinnum <= 255)
	{	/* stone, ice: a picture over the whole skin */
		qpic_t	*pic;

		q_snprintf (name, sizeof(name), "gfx/skin%d.lmp", skinnum);
		pic = Draw_CachePic (name);
		slot = files = (int)((glpic_t *)pic->data)->texnum;
	}
	else
	{
		if (skinnum >= hdr->numskins || skinnum < 0)	/* GL: Sys_Error above 255 */
		{
			*bad_skin = true;
			skinnum = 0;
		}
		slot = files = (int)hdr->gl_texturenum[skinnum][(int)(r_scene.time * 10) & 3];

		/* translated player colors; the model's skin's files apply, its
		 * albedo where the colors leave the skin as it is (5.3) */
		player = e->num - 1;
		if (e->colormap != vid.colormap && !gl_nocolors.integer && IsPlayerModel (e->model) &&
		    e->kind == SCENE_ENT_DYNAMIC && player >= 0 && player < cl.maxclients && player < MAX_CLIENTS)
		{
			int	p;

			q_snprintf (name, sizeof(name), "player%d", player);
			p = VK_FindTexture (name);
			if (p >= 0)
			{
				/* the skin R_TranslatePlayerSkin translates, whatever the
				 * group frame or skin number (GL's too): gl_model.c keeps
				 * the class model's last single skin */
				files = (int)hdr->gl_texturenum[q_min (hdr->numskins, MAX_SKINS) - 1][0];
				slot = p;
				translated = !player_skin_as_is[player];
			}
		}
	}
	return SkinMaterial (slot, files, VK_ModelHasCutouts (e->model), name, emissive, translated);
}


/* ==========================================================================
 * Player skins
 * ========================================================================== */

/*
===============
R_TranslatePlayerSkin

Translates a skin texture by the per-player color lookup
===============
*/
void R_TranslatePlayerSkin (int playernum)
{
	static byte	translated[sizeof(player_8bit_texels[0])];
	int		top, bottom, i, s, size, width, height;
	byte		translate[256];
	qmodel_t	*model;
	aliashdr_t	*paliashdr;
	byte		*original;
	byte		*sourceA, *sourceB, *colorA, *colorB;
	int		playerclass = (int)cl.scores[playernum].playerclass;
	char		name[MAX_QPATH];

	for (i = 0; i < 256; i++)
		translate[i] = i;

	top = (cl.scores[playernum].colors & 0xf0) >> 4;
	bottom = (cl.scores[playernum].colors & 15);

	if (top > 10)
		top = 0;
	if (bottom > 10)
		bottom = 0;

	top -= 1;
	bottom -= 1;

	colorA = playerTranslation + 256 + color_offsets[q_min (q_max (playerclass, 1), MAX_PLAYER_CLASS) - 1];
	colorB = colorA + 256;
	sourceA = colorB + 256 + (top * 256);
	sourceB = colorB + 256 + (bottom * 256);
	for (i = 0; i < 256; i++, colorA++, colorB++, sourceA++, sourceB++)
	{
		if (top >= 0 && (*colorA != 255))
			translate[i] = *sourceA;
		if (bottom >= 0 && (*colorB != 255))
			translate[i] = *sourceB;
	}

	//
	// locate the original skin pixels
	//
	model = cl_entities[1+playernum].model;
	if (!model || model->type != mod_alias)
		return;		// player doesn't have a model yet

	// class limit is mission pack dependant
	s = (gameflags & GAME_PORTALS) ? MAX_PLAYER_CLASS : MAX_PLAYER_CLASS - PORTALS_EXTRA_CLASSES;
	if (playerclass >= 1 && playerclass <= s)
		original = player_8bit_texels[playerclass-1];
	else	original = player_8bit_texels[0];

	/* the header is in the cache, which hunk allocations (GL_LoadTexture's)
	 * can move: take what is needed first */
	paliashdr = (aliashdr_t *)Mod_Extradata (model);
	width = paliashdr->skinwidth;
	height = paliashdr->skinheight;
	size = width * height;
	if (size <= 0 || size > (int)sizeof(translated))
		return;

	/* the 8-bit skin, translated, converted like the model's own skins */
	for (i = 0; i < size; i++)
		translated[i] = translate[original[i]];
	if (playernum >= 0 && playernum < MAX_CLIENTS)	/* 5.3: such a player shows a replaced albedo */
		player_skin_as_is[playernum] = !memcmp (translated, original, size);
	q_snprintf (name, sizeof(name), "player%d", playernum);
	GL_LoadTexture (name, translated, width, height, SkinTextureMode (model->flags));
}


void R_InitSkins (void)
{
	Cvar_RegisterVariable (&gl_nocolors);

	playerTranslation = (byte *)FS_LoadHunkFile ("gfx/player.lmp", NULL);
	if (!playerTranslation)
		Sys_Error ("Couldn't load gfx/player.lmp");
}
