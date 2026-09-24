/* stubs.c -- placeholder renderer interface for the hexenlicht target.
 *
 * The engine expects its renderer to provide the R_*, Draw_*, SCR_* and
 * VID_* functions and a few globals (see the GL renderer: gl_rmain.c,
 * gl_rmisc.c, gl_draw.c, gl_screen.c, gl_vidnt.c). This file provides
 * all of them, doing nothing, so that hexenlicht.exe links and runs its
 * game logic without a window. It is replaced piece by piece: each section
 * names the story (docs/hexenlicht/PLAN.md, section 8) that implements it
 * for real, and shrinks until the file can be deleted.
 *
 * The few functions with behavior the game logic depends on (particle pool,
 * per-map resets, the "missing texture" placeholder, loading plaque flags)
 * keep the renderer-independent parts of Hammer of Thyrion's GL versions.
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
#include "winquake.h"
#include "r_part.h"
#include "vid_vk.h"
#include "vk_local.h"

/* The video section (window, modes, VID_*) moved to vid_vk.c (story 1.2). */


/* The texture manager (GL_LoadTexture, texture cache, flushing on map
 * change) moved to vk_texture.c (story 1.5). */

/* ==========================================================================
 * Model textures: player skins, missing texture.  -> epic E2 (models)
 * ========================================================================== */

byte		*playerTranslation;
texture_t	*r_notexture_mip;

/* player class skin color offsets into playerTranslation (gl_rmisc.c) */
const int color_offsets[MAX_PLAYER_CLASS] =
{
	2 * 14 * 256,
	0,
	1 * 14 * 256,
	2 * 14 * 256,
	2 * 14 * 256
};

void R_TranslatePlayerSkin (int playernum) { (void)playernum; }

/* The model loader substitutes this for missing textures, so it must be a
 * valid texture: the 16x16 checkerboard from gl_rmisc.c. Also called by
 * dedicated servers (host.c). */
void R_InitTextures (void)
{
	int		x, y, m;
	byte	*dest;

	r_notexture_mip = (texture_t *) Hunk_AllocName (sizeof(texture_t) + 16*16+8*8+4*4+2*2, "notexture");

	r_notexture_mip->width = r_notexture_mip->height = 16;
	r_notexture_mip->offsets[0] = sizeof(texture_t);
	r_notexture_mip->offsets[1] = r_notexture_mip->offsets[0] + 16*16;
	r_notexture_mip->offsets[2] = r_notexture_mip->offsets[1] + 8*8;
	r_notexture_mip->offsets[3] = r_notexture_mip->offsets[2] + 4*4;

	for (m = 0; m < 4; m++)
	{
		dest = (byte *)r_notexture_mip + r_notexture_mip->offsets[m];

		for (y = 0; y < (16 >> m); y++)
		{
			for (x = 0; x < (16 >> m); x++)
			{
				if ( (y < (8 >> m)) ^ (x < (8 >> m)) )
					*dest++ = 0;
				else
					*dest++ = 0xff;
			}
		}
	}
}


/* ==========================================================================
 * 2D drawing and screen layout.             -> story 1.6 (2D renderer)
 * ========================================================================== */

qboolean	draw_reinit = false;

float		scr_con_current;
float		scr_centertime_off;
int		scr_copytop;
int		scr_copyeverything;
int		scr_fullupdate;
int		scr_topupdate;
qboolean	scr_skipupdate;
qboolean	scr_disabled_for_loading;
qboolean	block_drawing;
int		clearnotify;
int		trans_level = 0;
int		total_loading_size, current_loading_size, loading_stage;

cvar_t		scr_viewsize = {"viewsize", "110", CVAR_ARCHIVE};

/* callers keep and dereference returned pics (e.g. for their size) */
static qpic_t	stub_pic = { 1, 1, {0} };

void Draw_Init (void) {}
qpic_t *Draw_PicFromWad (const char *name) { (void)name; return &stub_pic; }
qpic_t *Draw_CachePic (const char *path) { (void)path; return &stub_pic; }
qpic_t *Draw_CachePicNoTrans (const char *path) { (void)path; return &stub_pic; }
void Draw_Character (int x, int y, unsigned int num) { (void)x; (void)y; (void)num; }
void Draw_BigCharacter (int x, int y, int num) { (void)x; (void)y; (void)num; }
void Draw_String (int x, int y, const char *str) { (void)x; (void)y; (void)str; }
void Draw_SmallString (int x, int y, const char *str) { (void)x; (void)y; (void)str; }
void Draw_Pic (int x, int y, qpic_t *pic) { (void)x; (void)y; (void)pic; }
void Draw_PicCropped (int x, int y, qpic_t *pic) { (void)x; (void)y; (void)pic; }
void Draw_TransPic (int x, int y, qpic_t *pic) { (void)x; (void)y; (void)pic; }
void Draw_TransPicCropped (int x, int y, qpic_t *pic) { (void)x; (void)y; (void)pic; }
void Draw_TransPicTranslate (int x, int y, qpic_t *pic, byte *translation, int p_class)
{
	(void)x; (void)y; (void)pic; (void)translation; (void)p_class;
}
void Draw_IntermissionPic (qpic_t *pic) { (void)pic; }
void Draw_ConsoleBackground (int lines) { (void)lines; }
void Draw_Fill (int x, int y, int w, int h, int c) { (void)x; (void)y; (void)w; (void)h; (void)c; }
void Draw_FadeScreen (void) {}

void SCR_Init (void)
{
	Cvar_RegisterVariable (&scr_viewsize);
}

/* until the 2D renderer (story 1.6): a shader-drawn test pattern proves
 * the Vulkan and shader path works end to end */
void SCR_UpdateScreen (void)
{
	if (VK_BeginFrame ())
	{
		VK_DrawTestPattern ((float)realtime);
		VK_EndFrame ();
	}
	VID_EndFrame ();
}
void SCR_CenterPrint (const char *str) { (void)str; }
void SCR_SetPlaqueMessage (const char *msg) { (void)msg; }

/* no display to answer on: log the question and say "no" */
int SCR_ModalMessage (const char *text)
{
	Con_Printf ("%s\n(no display: answered \"no\")\n", text);
	return false;
}

/* the flag handling of gl_screen.c, without drawing */
void SCR_BeginLoadingPlaque (void)
{
	S_StopAllSounds (true);

	if (cls.state != ca_connected)
		return;
	if (cls.signon != SIGNONS)
		return;

	Con_ClearNotify ();
	scr_centertime_off = 0;
	scr_con_current = 0;
	scr_disabled_for_loading = true;
	scr_fullupdate = 0;
}

void SCR_EndLoadingPlaque (void)
{
	scr_disabled_for_loading = false;
	scr_fullupdate = 0;
	Con_ClearNotify ();
}


/* ==========================================================================
 * 3D scene, lighting, surfaces.             -> epics E2-E4
 * ========================================================================== */

refdef_t	r_refdef;
vec3_t		r_origin, vpn, vright, vup;
int		r_framecount;
entity_t	r_worldentity;
int		d_lightstylevalue[256];	/* 8.8 fraction of base light value */

int		gl_lightmap_format = GL_RGBA;
int		gl_coloredstatic;

/* same cvars as the GL renderer, so config files keep their settings */
cvar_t		gl_glows = {"gl_glows", "0", CVAR_ARCHIVE};
cvar_t		gl_other_glows = {"gl_other_glows", "0", CVAR_ARCHIVE};
cvar_t		gl_missile_glows = {"gl_missile_glows", "1", CVAR_ARCHIVE};
cvar_t		gl_coloredlight = {"gl_coloredlight", "0", CVAR_ARCHIVE};
cvar_t		gl_colored_dynamic_lights = {"gl_colored_dynamic_lights", "0", CVAR_ARCHIVE};
cvar_t		gl_extra_dynamic_lights = {"gl_extra_dynamic_lights", "0", CVAR_ARCHIVE};
cvar_t		gl_lightmapfmt = {"gl_lightmapfmt", "GL_RGBA", CVAR_ARCHIVE};

void R_Init (void)
{
	Cvar_RegisterVariable (&gl_glows);
	Cvar_RegisterVariable (&gl_missile_glows);
	Cvar_RegisterVariable (&gl_other_glows);
	Cvar_RegisterVariable (&gl_coloredlight);
	Cvar_RegisterVariable (&gl_colored_dynamic_lights);
	Cvar_RegisterVariable (&gl_extra_dynamic_lights);
	Cvar_RegisterVariable (&gl_lightmapfmt);

	R_InitParticles ();	/* particle pool used by the client effects */

	playerTranslation = (byte *)FS_LoadHunkFile ("gfx/player.lmp", NULL);
	if (!playerTranslation)
		Sys_Error ("Couldn't load gfx/player.lmp");
}

/* the renderer-independent parts of gl_rmisc.c's R_NewMap */
void R_NewMap (void)
{
	int		i;

	for (i = 0; i < 256; i++)
		d_lightstylevalue[i] = 264;	/* normal light value */

	memset (&r_worldentity, 0, sizeof(r_worldentity));
	r_worldentity.model = cl.worldmodel;

	/* clear out efrags in case the level hasn't been reloaded */
	for (i = 0; i < cl.worldmodel->numleafs; i++)
		cl.worldmodel->leafs[i].efrags = NULL;

	R_ClearParticles ();
}

void R_RenderView (void) {}
void R_PushDlights (void) {}
void R_InitSky (texture_t *mt) { (void)mt; }

/* called by the model loader for warped (water/sky) surfaces */
void GL_SubdivideSurface (qmodel_t *m, msurface_t *fa) { (void)m; (void)fa; }
void GL_SetupLightmapFmt (void) {}
