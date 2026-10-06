/* vk_menu.c -- the Renderer Settings page of the Options menu, and its
 * settings saved when changed
 *
 * Story 6.10. Hexenlicht's Options menu shows "Renderer Settings" where
 * Hammer of Thyrion's GL shows "OpenGL Features" (menu.c's m_opengl, an
 * upstream file: its label, draw and key dispatch are guarded with
 * #if defined(HEXENLICHT); the GL page's glows, lightmap format, static
 * colored light and texture filtering are GL's and do nothing here).
 * menu_renderer opens it. Every row applies at once, over the live view;
 * the lines under the rows say what the selected one does. The rows:
 *  - Quality: a preset of the next two rows, Low (50 %, half-resolution
 *    bounce light), Medium (67 %, full), High (100 %, full: the
 *    defaults), else Custom;
 *  - Render scale (r_scale, in steps that hold DLSS's and FSR 1's
 *    modes'), Bounce light (pt_num_bounce_rays: half, full, two),
 *    Upscaler (r_upscaler; a DLSS one that can't run says why,
 *    vk_dlss.c);
 *  - Lighting (r_maplight_shape 2 "Original", 0 "Physically based",
 *    DECISIONS R103), Exposure (tm_auto_exposure), Sky light (r_sky_mode,
 *    vk_sky.c), colored map and dynamic lights (r_maplight_colors,
 *    Hammer of Thyrion's gl_colored_dynamic_lights), Glow
 *    (r_emissive_scale: lava, flames and effects);
 *  - Reset to defaults: those settings.
 * Left and right step a row's value and stop at the ends (a row of two
 * values toggles; a value set in the console beyond the steps stays),
 * Enter steps it forward and wraps around. Quality steps from Custom to
 * the nearest preset by render scale. r_resetsettings is the Reset row
 * from the console.
 *
 * Saved when changed: the page's settings but gl_colored_dynamic_lights
 * are written to hexenlicht.cfg only while they differ from their default
 * (the value when VK_InitMenu runs, from R_Init, before the config does):
 * their archive flag follows the value, by a callback in front of the
 * owner's. A default that a later version changes then reaches every
 * config that didn't choose otherwise (a cvar archived always keeps the
 * default of the day it was first saved, DECISIONS R94).
 * gl_colored_dynamic_lights stays archived always, as in Hammer of Thyrion.
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
#include "shaders/hl_shared.h"	/* SPHERE_SHAPE_* */

/* the page's settings */
enum
{
	S_SCALE,
	S_BOUNCE,
	S_UPSCALER,
	S_SHAPE,
	S_EXPOSURE,
	S_SKY,
	S_MAP_COLORS,
	S_DYNAMIC_COLORS,
	S_GLOW,
	NUM_SETTINGS
};

static struct
{
	const char	*name;
	qboolean	saved_when_changed;	/* else archived as its owner registered it */
	cvar_t		*var;
	char		def[16];		/* its value when VK_InitMenu ran: its default */
	cvarcallback_t	callback;		/* its owner's */
} settings[NUM_SETTINGS] =
{
	{ "r_scale",			true },
	{ "pt_num_bounce_rays",		true },
	{ "r_upscaler",			true },
	{ "r_maplight_shape",		true },
	{ "tm_auto_exposure",		true },
	{ "r_sky_mode",			true },
	{ "r_maplight_colors",		true },
	{ "gl_colored_dynamic_lights",	false },	/* Hammer of Thyrion's: archived always, as in HoT */
	{ "r_emissive_scale",		true }
};

enum
{
	ROW_QUALITY,
	ROW_SCALE,
	ROW_BOUNCE,
	ROW_UPSCALER,
	ROW_BLANK1,
	ROW_LIGHTING,
	ROW_EXPOSURE,
	ROW_SKY,
	ROW_MAP_COLORS,
	ROW_DYNAMIC_COLORS,
	ROW_GLOW,
	ROW_RESET,
	NUM_ROWS
};

static const char *row_labels[NUM_ROWS] =
{
	"Quality", "Render scale", "Bounce light", "Upscaler", NULL,
	"Lighting", "Exposure", "Sky light", "Colored map lights", "Colored dyn. lights", "Glow",
	"Reset to defaults"
};

/* the page's layout, in the menu's 320-wide coordinates */
#define HEADING_Y	60
#define ROWS_Y		72
#define LABELS_END	168	/* the labels end here */
#define CURSOR_X	176
#define VALUES_X	192	/* 16 characters to the edge */
#define STATUS_Y	(ROWS_Y + 8 * NUM_ROWS + 4)	/* 3 lines: the last at 188 */
#define STATUS_CHARS	38	/* a line of the status */
#define STATUS_LINES	3

static const struct
{
	const char	*name;
	int		scale;		/* r_scale */
	float		bounce;		/* pt_num_bounce_rays */
} presets[] =
{
	{ "Low",	50,	0.5f },
	{ "Medium",	67,	1.0f },
	{ "High",	100,	1.0f }	/* the defaults */
};

static const float	scale_steps[] = { 25, 33, 50, 58, 67, 77, 100 };	/* DLSS's and FSR 1's modes' scales among them */
static const float	bounce_steps[] = { 0.5f, 1, 2 };
static const float	sky_steps[] = { 0, 1, 2 };
static const float	glow_steps[] = { 8, 16, 32, 64 };

static const char	*upscaler_names[] = { "TAA", "TAAU", "FSR 1", "DLSS SR", "DLSS RR" };
static const char	*sky_names[] = { "per map", "off", "on" };

static int	cursor;


/* ==========================================================================
 * Saved when changed
 * ========================================================================== */

static void SetSaved (int s)
{
	cvar_t	*var = settings[s].var;

	if (var->value != (float)atof (settings[s].def))
		var->flags |= CVAR_ARCHIVE;
	else
		var->flags &= ~CVAR_ARCHIVE;
}

/* the callback in front of the owner's */
static void SettingChanged (cvar_t *var)
{
	int	s;

	for (s = 0; s < NUM_SETTINGS; s++)
	{
		if (settings[s].var != var)
			continue;
		SetSaved (s);
		if (settings[s].callback)
			settings[s].callback (var);
		return;
	}
}


/* ==========================================================================
 * The rows' values
 * ========================================================================== */

static float Value (int s)
{
	return settings[s].var->value;
}

static void SetValue (int s, float value)
{
	Cvar_SetValueQuick (settings[s].var, value);
}

/* the next of the steps after value in dir (or before); if none, value
 * (a value beyond the steps, set in the console, stays), with wrap the
 * first (last) */
static float Step (const float *steps, int count, float value, int dir, qboolean wrap)
{
	int	i;

	if (dir > 0)
	{
		for (i = 0; i < count; i++)
		{
			if (steps[i] > value)
				return steps[i];
		}
		return wrap ? steps[0] : value;
	}
	for (i = count - 1; i >= 0; i--)
	{
		if (steps[i] < value)
			return steps[i];
	}
	return wrap ? steps[count - 1] : value;
}

/* the preset the rows match, or -1 */
static int Preset (void)
{
	int	i;

	for (i = 0; i < (int)Q_COUNTOF(presets); i++)
	{
		if (settings[S_SCALE].var->integer == presets[i].scale && VK_NumBounceRays () == presets[i].bounce)
			return i;
	}
	return -1;
}

/* the preset one step from the rows' in dir: from a preset the next (at
 * the ends the same, with wrap around), from Custom the nearest by render
 * scale in dir (at or above it to the right, at or below it to the left) */
static int StepPreset (int dir, qboolean wrap)
{
	int	i = Preset (), count = (int)Q_COUNTOF(presets), scale = settings[S_SCALE].var->integer;

	if (i >= 0)
	{
		i += dir;
		if (i < 0)
			return wrap ? count - 1 : 0;
		if (i >= count)
			return wrap ? 0 : count - 1;
		return i;
	}
	if (dir > 0)
	{
		for (i = 0; i < count - 1 && presets[i].scale < scale; i++)
			;
		return i;
	}
	for (i = count - 1; i > 0 && presets[i].scale > scale; i--)
		;
	return i;
}

static int Upscaler (void)
{
	return q_max (0, q_min ((int)Q_COUNTOF(upscaler_names) - 1, settings[S_UPSCALER].var->integer));
}

static int SkyMode (void)
{
	int	mode = settings[S_SKY].var->integer;

	return (mode >= 1 && mode <= 2) ? mode : 0;
}

static const char *RowValue (int row)
{
	static char	text[32];
	float		n;
	int		i;

	switch (row)
	{
	case ROW_QUALITY:
		i = Preset ();
		return (i >= 0) ? presets[i].name : "Custom";
	case ROW_SCALE:
		q_snprintf (text, sizeof(text), "%d %%", q_max (25, q_min (100, settings[S_SCALE].var->integer)));
		return text;
	case ROW_BOUNCE:
		n = VK_NumBounceRays ();
		return (n == 0.5f) ? "half" : (n == 0.0f) ? "off" : (n == 1.0f) ? "full" : "two";
	case ROW_UPSCALER:
		return upscaler_names[Upscaler ()];
	case ROW_LIGHTING:
		switch (VK_MapLightShape ())
		{
		case SPHERE_SHAPE_GL:		return "Original";
		case SPHERE_SHAPE_PHYSICAL:	return "Physically based";
		default:			return "shape 1";
		}
	case ROW_EXPOSURE:
		return VK_AutoExposure () ? "auto" : "fixed";
	case ROW_SKY:
		if (SkyMode () != 0)
			return sky_names[SkyMode ()];
		return Cvar_VariableValue ("r_sky_light") ? "per map (on)" : "per map (off)";
	case ROW_MAP_COLORS:
		return Value (S_MAP_COLORS) ? "on" : "off";
	case ROW_DYNAMIC_COLORS:
		return Value (S_DYNAMIC_COLORS) ? "on" : "off";
	case ROW_GLOW:
		q_snprintf (text, sizeof(text), "x%g", Value (S_GLOW));
		return text;
	default:
		return NULL;
	}
}

/* what the selected row does */
static const char *RowStatus (int row)
{
	static char	text[160];
	const char	*why;
	float		n;

	switch (row)
	{
	case ROW_QUALITY:
		return "Low: 50 % and half the bounce light, Medium: 67 %, High: 100 %";
	case ROW_SCALE:
		return "the 3D view's resolution, upscaled to the window";
	case ROW_BOUNCE:
		n = VK_NumBounceRays ();
		if (n == 0.5f)
			return "bounce light on every other row: faster, noisier";
		if (n == 0.0f)
			return "no bounce light (pt_num_bounce_rays 0)";
		if (n == 1.0f)
			return "bounce light for every pixel";
		return "a second bounce: more light from lava and the sky";
	case ROW_UPSCALER:
		why = VK_UpscaleDLSSFeature () ? VK_DLSSCantRun (VK_UpscaleDLSSFeature ()) : NULL;
		if (why)
		{
			q_snprintf (text, sizeof(text), "DLSS can't run: %s. TAAU instead", why);
			return text;
		}
		switch (Upscaler ())
		{
		case 0:		return "temporal anti-aliasing at the render size";
		case 1:		return "temporal upscaling";
		case 2:		return "AMD FSR 1 below 100 %, TAAU at 100 %";
		case 3:		return "NVIDIA DLSS super resolution";
		default:	return "NVIDIA DLSS ray reconstruction instead of the denoiser";
		}
	case ROW_LIGHTING:
		switch (VK_MapLightShape ())
		{
		case SPHERE_SHAPE_GL:		return "each light as GL's lightmaps show it; shadows and bounces path traced";
		case SPHERE_SHAPE_PHYSICAL:	return "inverse-square light from each map light";
		default:			return "physical with GL's angle term (r_maplight_shape 1)";
		}
	case ROW_EXPOSURE:
		return VK_AutoExposure () ? "the exposure adapts to the view, as in Quake II RTX" : "a fixed exposure: GL's brightness";
	case ROW_SKY:
		switch (SkyMode ())
		{
		case 1:		return "the sky lights nothing";
		case 2:		return "the sky lights the world through bounce light";
		default:	return "the map's file chooses; without one the sky lights nothing";
		}
	case ROW_MAP_COLORS:
		return "the map lights in Hammer of Thyrion's colors; off: white";
	case ROW_DYNAMIC_COLORS:
		return "the dynamic lights of effects and projectiles in color; off: white";
	case ROW_GLOW:
		return "the light of lava, flames and effects; at x32 lava lights its rooms as the maps' lava lights did";
	case ROW_RESET:
		return "the settings above to their defaults";
	default:
		return "";
	}
}

/* str in lines of at most STATUS_CHARS, broken at spaces, centered */
static void DrawStatus (const char *str)
{
	char	line[STATUS_CHARS + 1];
	int	lines, len, cut;

	for (lines = 0; *str && lines < STATUS_LINES; lines++)
	{
		len = (int)strlen (str);
		cut = len;
		if (len > STATUS_CHARS)
		{
			for (cut = STATUS_CHARS; cut > 0 && str[cut] != ' '; cut--)
				;
			if (cut == 0)
				cut = STATUS_CHARS;
		}
		memcpy (line, str, cut);
		line[cut] = 0;
		M_Print ((320 - cut * 8) / 2, STATUS_Y + lines * 8, line);
		str += cut;
		while (*str == ' ')
			str++;
	}
}

/* drawn inside the frame: nothing here may print or set a cvar */
void VK_RendererMenuDraw (void)
{
	const char	*text;
	int		row, y;

	ScrollTitle ("gfx/menu/title3.lmp");
	M_PrintWhite ((320 - 18 * 8) / 2, HEADING_Y, "Renderer Settings:");
	for (row = 0; row < NUM_ROWS; row++)
	{
		if (!row_labels[row])
			continue;
		y = ROWS_Y + 8 * row;
		M_Print (LABELS_END - 8 * (int)strlen (row_labels[row]), y, row_labels[row]);
		text = RowValue (row);
		if (text)
			M_Print (VALUES_X, y, text);
	}
	M_DrawCharacter (CURSOR_X, ROWS_Y + 8 * cursor, 12 + ((int)(realtime * 4) & 1));
	DrawStatus (RowStatus (cursor));
}

/* the selected row's value one step in dir; wrap: around at the end */
static void Adjust (int dir, qboolean wrap)
{
	int	i, shape;

	if (cursor != ROW_RESET)
		S_LocalSound ("raven/menu3.wav");
	switch (cursor)
	{
	case ROW_QUALITY:
		i = StepPreset (dir, wrap);
		SetValue (S_SCALE, (float)presets[i].scale);
		SetValue (S_BOUNCE, presets[i].bounce);
		break;
	case ROW_SCALE:
		SetValue (S_SCALE, Step (scale_steps, (int)Q_COUNTOF(scale_steps), Value (S_SCALE), dir, wrap));
		break;
	case ROW_BOUNCE:
		SetValue (S_BOUNCE, Step (bounce_steps, (int)Q_COUNTOF(bounce_steps), VK_NumBounceRays (), dir, wrap));
		break;
	case ROW_UPSCALER:
		i = Upscaler () + dir;
		if (i < 0)
			i = wrap ? (int)Q_COUNTOF(upscaler_names) - 1 : 0;
		else if (i >= (int)Q_COUNTOF(upscaler_names))
			i = wrap ? 0 : (int)Q_COUNTOF(upscaler_names) - 1;
		SetValue (S_UPSCALER, (float)i);
		break;
	case ROW_LIGHTING:
		shape = VK_MapLightShape ();
		SetValue (S_SHAPE, (float)((shape == SPHERE_SHAPE_GL) ? SPHERE_SHAPE_PHYSICAL : SPHERE_SHAPE_GL));
		break;
	case ROW_EXPOSURE:
		SetValue (S_EXPOSURE, VK_AutoExposure () ? 0.0f : 1.0f);
		break;
	case ROW_SKY:
		SetValue (S_SKY, Step (sky_steps, (int)Q_COUNTOF(sky_steps), (float)SkyMode (), dir, wrap));
		break;
	case ROW_MAP_COLORS:
		SetValue (S_MAP_COLORS, Value (S_MAP_COLORS) ? 0.0f : 1.0f);
		break;
	case ROW_DYNAMIC_COLORS:
		SetValue (S_DYNAMIC_COLORS, Value (S_DYNAMIC_COLORS) ? 0.0f : 1.0f);
		break;
	case ROW_GLOW:
		SetValue (S_GLOW, Step (glow_steps, (int)Q_COUNTOF(glow_steps), Value (S_GLOW), dir, wrap));
		break;
	default:
		break;
	}
}

static void ResetSettings (void)
{
	int	s;

	for (s = 0; s < NUM_SETTINGS; s++)
		Cvar_SetQuick (settings[s].var, settings[s].def);
}

static void Reset (void)
{
	S_LocalSound ("raven/menu2.wav");
	ResetSettings ();
}

/* r_resetsettings: the page's Reset to defaults from the console (hl_run.ps1
 * runs it before a test script, so the player's saved choices don't change
 * what a test sees) */
static void R_ResetSettings_f (void)
{
	ResetSettings ();
	Con_Printf ("the Renderer Settings page's settings are at their defaults\n");
}

void VK_RendererMenuKey (int key)
{
	switch (key)
	{
	case K_ESCAPE:
		M_Menu_Options_f ();
		break;
	case K_UPARROW:
		S_LocalSound ("raven/menu1.wav");
		do
		{
			if (--cursor < 0)
				cursor = NUM_ROWS - 1;
		} while (!row_labels[cursor]);
		break;
	case K_DOWNARROW:
		S_LocalSound ("raven/menu1.wav");
		do
		{
			if (++cursor >= NUM_ROWS)
				cursor = 0;
		} while (!row_labels[cursor]);
		break;
	case K_LEFTARROW:
		Adjust (-1, false);
		break;
	case K_RIGHTARROW:
		Adjust (1, false);
		break;
	case K_ENTER:
		if (cursor == ROW_RESET)
			Reset ();
		else
			Adjust (1, true);
		break;
	default:
		break;
	}
}

/* menu_renderer: the page, as menu_video opens Video Modes */
static void Menu_Renderer_f (void)
{
	Key_SetDest (key_menu);
	m_state = m_opengl;
	S_LocalSound ("raven/menu2.wav");
}

/* from R_Init, after every setting is registered and before the config runs */
void VK_InitMenu (void)
{
	int	s;

	for (s = 0; s < NUM_SETTINGS; s++)
	{
		settings[s].var = Cvar_FindVar (settings[s].name);
		if (!settings[s].var)
			Sys_Error ("%s: %s isn't registered", __thisfunc__, settings[s].name);
		q_strlcpy (settings[s].def, settings[s].var->string, sizeof(settings[s].def));
		if (!settings[s].saved_when_changed)
			continue;
		settings[s].callback = settings[s].var->callback;
		Cvar_SetCallback (settings[s].var, SettingChanged);
		SetSaved (s);
	}
	Cmd_AddCommand ("menu_renderer", Menu_Renderer_f);
	Cmd_AddCommand ("r_resetsettings", R_ResetSettings_f);
}
