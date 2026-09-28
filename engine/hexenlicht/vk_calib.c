/* vk_calib.c -- calibration against GL: bookmarks, setpos, averaged screenshots
 *
 * Story 4.9. The same camera in Hexenlicht and the unmodified GL client
 * (glh2) comes from a savegame both load (tools/hexenlicht/calib_shots.ps1):
 *  - vk_setpos x y z [pitch yaw] puts the player there (a local
 *    single-player game), the view turned as given. A "save" on the same
 *    console line keeps the pitch: a load sends the player model's angles
 *    (host_cmd.c's Host_Spawn_f), which the next server frame turns into a
 *    third of the view's pitch (sv_user.c), so the save is made before
 *    that. Angles go through the protocol as bytes (1.4 degrees), the same
 *    in both engines;
 *  - vk_bookmark <name> appends "name map x y z pitch yaw [portals]" for
 *    the player's origin and view to bookmarks.txt in the game folder (and
 *    prints it): the lines of tools/hexenlicht/bookmarks.txt;
 *  - vk_screenshot <name> [frames] writes shots/<name>.tga, the next
 *    frames (1) averaged in linear light (vk_swapchain.c): a still frame's
 *    one-sample noise averages out, without the 100 numbered files of
 *    "screenshot".
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
#include "q_ctype.h"

#define MAX_SHOT_FRAMES		1024

static qboolean LocalGame (const char *cmd)
{
	if (sv.active && svs.maxclients == 1 && cls.state == ca_connected && cls.signon == SIGNONS)
		return true;
	Con_Printf ("%s: only in a local single-player game\n", cmd);
	return false;
}

/* a number, false if it isn't one */
static qboolean ArgNumber (int i, float *f)
{
	const char	*s = Cmd_Argv (i);
	char		*end;
	double		d = strtod (s, &end);

	*f = (float)d;
	return end != s && *end == 0 && d >= -1e6 && d <= 1e6;
}

/* a name for a file: letters, digits, _ - . (no path) */
static qboolean ValidName (const char *s)
{
	if (!*s || *s == '.')
		return false;
	for ( ; *s; s++)
	{
		if (!q_isalnum (*s) && *s != '_' && *s != '-' && *s != '.')
			return false;
	}
	return true;
}

static void VK_SetPos_f (void)
{
	edict_t	*ent;
	vec3_t	v;
	float	pitch = 0.0f, yaw = 0.0f;
	int	i, argc = Cmd_Argc ();

	if (argc != 4 && argc != 6)
	{
		Con_Printf ("vk_setpos x y z [pitch yaw]: the player there, looking that way\n");
		return;
	}
	for (i = 0; i < 3; i++)
	{
		if (!ArgNumber (1 + i, &v[i]))
		{
			Con_Printf ("vk_setpos: \"%s\" isn't a number\n", Cmd_Argv (1 + i));
			return;
		}
	}
	if (argc == 6 && (!ArgNumber (4, &pitch) || !ArgNumber (5, &yaw)))
	{
		Con_Printf ("vk_setpos: pitch and yaw are numbers\n");
		return;
	}
	if (!LocalGame ("vk_setpos"))
		return;
	ent = EDICT_NUM (1);
	VectorCopy (v, ent->v.origin);
	VectorClear (ent->v.velocity);
	if (argc == 6)
	{
		/* the view's angles in both: a save made now restores them */
		ent->v.angles[0] = ent->v.v_angle[0] = pitch;
		ent->v.angles[1] = ent->v.v_angle[1] = yaw;
		ent->v.angles[2] = ent->v.v_angle[2] = 0.0f;
		ent->v.fixangle = 1;
	}
	SV_LinkEdict (ent, false);
}

static void VK_Bookmark_f (void)
{
	char		map[MAX_QPATH], line[256], path[MAX_OSPATH];
	const char	*name = Cmd_Argv (1);
	vec3_t		origin;
	FILE		*f;
	int		err;

	if (Cmd_Argc () != 2 || !ValidName (name))
	{
		Con_Printf ("vk_bookmark <name>: adds the player's place and view to bookmarks.txt\n");
		return;
	}
	if (!LocalGame ("vk_bookmark"))
		return;
	VectorCopy (EDICT_NUM (1)->v.origin, origin);
	COM_FileBase (cl.worldmodel->name, map, sizeof(map));
	q_snprintf (line, sizeof(line), "%s %s %.1f %.1f %.1f %.1f %.1f%s", name, map, origin[0], origin[1], origin[2],
		    cl.viewangles[PITCH], cl.viewangles[YAW], (gameflags & GAME_PORTALS) ? " portals" : "");
	FS_MakePath_BUF (FS_USERDIR, &err, path, sizeof(path), "bookmarks.txt");
	f = err ? NULL : fopen (path, "a");
	if (!f)
	{
		Con_Printf ("couldn't write %s\n", path);
		return;
	}
	fprintf (f, "%s\n", line);
	fclose (f);
	Con_Printf ("%s: %s\n", path, line);
}

static void VK_Screenshot_f (void)
{
	char		name[MAX_OSPATH], folder[MAX_OSPATH];
	float		frames = 1.0f;

	if (Cmd_Argc () < 2 || Cmd_Argc () > 3 || !ValidName (Cmd_Argv (1)) ||
	    (Cmd_Argc () == 3 && (!ArgNumber (2, &frames) || frames < 1.0f || frames > MAX_SHOT_FRAMES)))
	{
		Con_Printf ("vk_screenshot <name> [frames]: shots/<name>.tga, the next frames (1-%d) averaged in linear light\n",
			    MAX_SHOT_FRAMES);
		return;
	}
	FS_MakePath_BUF (FS_USERDIR, NULL, folder, sizeof(folder), "shots");
	Sys_mkdir (folder, false);
	q_snprintf (name, sizeof(name), "shots/%s.tga", Cmd_Argv (1));
	VK_RequestScreenshotAverage (name, (int)frames);
}

void VK_InitCalib (void)
{
	Cmd_AddCommand ("vk_setpos", VK_SetPos_f);
	Cmd_AddCommand ("vk_bookmark", VK_Bookmark_f);
	Cmd_AddCommand ("vk_screenshot", VK_Screenshot_f);
}
