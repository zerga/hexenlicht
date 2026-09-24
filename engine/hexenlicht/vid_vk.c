/* vid_vk.c -- Win32 window, video modes and window messages for Hexenlicht.
 *
 * Derived from Hammer of Thyrion's gl_vidnt.c with everything OpenGL
 * removed. The differences in behavior:
 *
 * - Fullscreen is borderless: a popup window covering the monitor at its
 *   desktop resolution. The display mode is never changed, so there is a
 *   single fullscreen "mode", Alt-Tab is instant, and the window stays
 *   visible when it loses focus. modestate is MS_FULLDIB in that case,
 *   because the menu and input code treat everything but MS_WINDOWED as
 *   "fullscreen, mouse always captured".
 * - One window is kept for the lifetime of the program; vid_restart only
 *   changes its style and size (the Vulkan swapchain follows its size).
 * - The windowed mode list adds widescreen sizes to the 4:3 ones.
 * - The process is per-monitor DPI aware (hexenlicht.manifest), so all
 *   sizes here are physical pixels.
 * - No hardware gamma ramps; gamma will be applied by the renderer.
 *
 * Copyright (C) 1996-1997  Id Software, Inc.
 * Copyright (C) 1997-1998  Raven Software Corp.
 * Copyright (C) 2005-2016  O.Sezer <sezero@users.sourceforge.net>
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
#include <mmsystem.h>
#include "cfgfile.h"
#include "bgmusic.h"
#include "cdaudio.h"
#include "resource.h"
#include "vid_vk.h"

#define MIN_WIDTH		320
#define MIN_HEIGHT		240
#define MAX_DESC		40

#define WM_CLASSNAME		"HexenII"
#define WM_WINDOWNAME		"Hexenlicht"

#define WINDOWED_STYLE		(WS_OVERLAPPED | WS_BORDER | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX)
#define FULLSCREEN_STYLE	(WS_POPUP)

typedef struct {
	modestate_t	type;		/* MS_WINDOWED, or MS_FULLDIB for borderless fullscreen */
	int		width;
	int		height;
	char		modedesc[MAX_DESC];
} vmode_t;

typedef struct {
	int	width;
	int	height;
} stdmode_t;

/* windowed sizes offered when they fit on the monitor; keep sorted */
#define RES_640X480	3	/* index of the default windowed mode */
static const stdmode_t	std_modes[] = {
	{  320,  240 },
	{  400,  300 },
	{  512,  384 },
	{  640,  480 },	/* == RES_640X480 */
	{  800,  600 },
	{ 1024,  768 },
	{ 1280,  720 },
	{ 1280,  960 },
	{ 1280, 1024 },
	{ 1600,  900 },
	{ 1600, 1200 },
	{ 1920, 1080 },
	{ 2560, 1440 },
	{ 3840, 2160 }
};
#define MAX_STDMODES	Q_COUNTOF(std_modes)

static vmode_t	wmodelist[MAX_STDMODES + 1];	/* windowed modes (+ one user mode) */
static vmode_t	fmodelist[1];			/* the one borderless fullscreen mode */
static vmode_t	*modelist;			/* the list in use: one of the above */
static int	num_wmodes;
static int	num_fmodes;
static int	*nummodes;
static vmode_t	badmode;

static qboolean	classregistered;
static HICON	hIcon;
HWND		mainwindow;

static int	window_x, window_y, window_width, window_height;
int		window_center_x, window_center_y;
RECT		window_rect;

viddef_t	vid;			/* global video state */
modestate_t	modestate = MS_UNINIT;
static int	vid_default = RES_640X480;
static int	vid_modenum = NO_MODE;	/* current mode, set after mode setting succeeds */
static int	vid_deskwidth, vid_deskheight;
static qboolean	vid_conscale = false;
static qboolean	vid_initialized = false;

/* vid_mode must be set before calling VID_SetMode or VID_Restart_f */
static cvar_t	vid_mode = {"vid_mode", "0", CVAR_NONE};
static cvar_t	vid_config_consize = {"vid_config_consize", "640", CVAR_ARCHIVE};
static cvar_t	vid_config_glx = {"vid_config_glx", "640", CVAR_ARCHIVE};
static cvar_t	vid_config_gly = {"vid_config_gly", "480", CVAR_ARCHIVE};
static cvar_t	vid_config_fscr = {"vid_config_fscr", "1", CVAR_ARCHIVE};

unsigned int	d_8to24table[256];
byte		globalcolormap[VID_GRADES*256];

/* input */
static int	enable_mouse;
cvar_t		_enable_mouse = {"_enable_mouse", "0", CVAR_ARCHIVE};

#if !defined(NO_SPLASHES)
extern HWND	hwnd_dialog;		/* startup splash, created in sys_win.c */
#endif

static LRESULT WINAPI MainWndProc (HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
static void VID_MenuDraw (void);
static void VID_MenuKey (int key);


//====================================

void VID_LockBuffer (void)
{
// nothing to do
}

void VID_UnlockBuffer (void)
{
// nothing to do
}

void VID_HandlePause (qboolean paused)
{
	if ((modestate == MS_WINDOWED) && _enable_mouse.integer)
	{
		if (paused)
		{
			IN_DeactivateMouse ();
			IN_ShowMouse ();
		}
		else
		{
			IN_ActivateMouse ();
			IN_HideMouse ();
		}
	}
}

void VID_ShiftPalette (const unsigned char *palette)
{
	/* hardware gamma ramps are not used: the renderer applies gamma */
	(void)palette;
}

/* no progress bars while loading (they would need drawing mid-frame) */
void D_ShowLoadingSize (void)
{
}

void VID_GetClientSize (int *width, int *height)
{
	*width = window_width;
	*height = window_height;
}

/* windowed mouse grab changes; GL_EndRendering in gl_vidnt.c */
void VID_EndFrame (void)
{
	if (modestate == MS_WINDOWED)
	{
		if (_enable_mouse.integer != enable_mouse)
		{
			if (_enable_mouse.integer)
			{
				IN_ActivateMouse ();
				IN_HideMouse ();
			}
			else
			{
				IN_DeactivateMouse ();
				IN_ShowMouse ();
			}

			enable_mouse = _enable_mouse.integer;
		}
	}
}


//====================================
// console size

static void VID_ConWidth (int modenum)
{
	int	w, h;

	if (!vid_conscale)
	{
		Cvar_SetValueQuick (&vid_config_consize, modelist[modenum].width);
		return;
	}

	w = vid_config_consize.integer;
	w &= ~7; /* make it a multiple of eight */
	if (w < MIN_WIDTH)
		w = MIN_WIDTH;
	else if (w > modelist[modenum].width)
		w = modelist[modenum].width;

	h = w * modelist[modenum].height / modelist[modenum].width;
	if (h < 200 /* MIN_HEIGHT */ ||
	    h > modelist[modenum].height || w > modelist[modenum].width)
	{
		vid_conscale = false;
		Cvar_SetValueQuick (&vid_config_consize, modelist[modenum].width);
		return;
	}
	vid.width = vid.conwidth = w;
	vid.height = vid.conheight = h;
	if (w != modelist[modenum].width)
		vid_conscale = true;
	else	vid_conscale = false;
}

void VID_ChangeConsize (int dir)
{
	int	w, h;

	switch (dir)
	{
	case -1: /* smaller text */
		w = ((float)vid.conwidth/(float)vid.width + 0.05f) * vid.width; /* use 0.10f increment ?? */
		w &= ~7; /* make it a multiple of eight */
		if (w > modelist[vid_modenum].width)
			w = modelist[vid_modenum].width;
		break;

	case 1: /* bigger text */
		w = ((float)vid.conwidth/(float)vid.width - 0.05f) * vid.width;
		w &= ~7; /* make it a multiple of eight */
		if (w < MIN_WIDTH)
			w = MIN_WIDTH;
		break;

	default:	/* bad key */
		return;
	}

	h = w * modelist[vid_modenum].height / modelist[vid_modenum].width;
	if (h < 200)
		return;
	vid.width = vid.conwidth = w;
	vid.height = vid.conheight = h;
	Cvar_SetValueQuick (&vid_config_consize, vid.conwidth);
	vid.recalc_refdef = 1;
	if (vid.conwidth != modelist[vid_modenum].width)
		vid_conscale = true;
	else	vid_conscale = false;
}

float VID_ReportConsize (void)
{
	return (float)modelist[vid_modenum].width/vid.conwidth;
}


//====================================
// monitors and window placement

/* monitor and work area rectangles of the monitor the window is on
 * (the primary monitor before the window exists) */
static void VID_GetMonitorRects (RECT *monitor, RECT *work)
{
	MONITORINFO	mi;
	HMONITOR	hmon;
	POINT		origin = { 0, 0 };

	if (mainwindow)
		hmon = MonitorFromWindow (mainwindow, MONITOR_DEFAULTTOPRIMARY);
	else	hmon = MonitorFromPoint (origin, MONITOR_DEFAULTTOPRIMARY);

	mi.cbSize = sizeof(mi);
	if (!GetMonitorInfo (hmon, &mi))
	{
		SetRect (&mi.rcMonitor, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
		mi.rcWork = mi.rcMonitor;
	}
	if (monitor)
		*monitor = mi.rcMonitor;
	if (work)
		*work = mi.rcWork;
}

static void VID_UpdateWindowStatus (void)
{
	window_rect.left = window_x;
	window_rect.top = window_y;
	window_rect.right = window_x + window_width;
	window_rect.bottom = window_y + window_height;
	window_center_x = (window_rect.left + window_rect.right) / 2;
	window_center_y = (window_rect.top + window_rect.bottom) / 2;

	IN_UpdateClipCursor ();
}

/* can a windowed client area of this size be placed on the monitor? */
static qboolean VID_WindowFits (int width, int height)
{
	RECT	rect, work;

	SetRect (&rect, 0, 0, width, height);
	AdjustWindowRectEx (&rect, WINDOWED_STYLE, FALSE, 0);
	VID_GetMonitorRects (NULL, &work);

	return (rect.right - rect.left <= work.right - work.left) &&
	       (rect.bottom - rect.top <= work.bottom - work.top);
}

/* the fullscreen mode is the monitor the window is on */
static void VID_UpdateFullscreenMode (void)
{
	RECT	monitor;

	VID_GetMonitorRects (&monitor, NULL);
	vid_deskwidth = monitor.right - monitor.left;
	vid_deskheight = monitor.bottom - monitor.top;

	fmodelist[0].type = MS_FULLDIB;
	fmodelist[0].width = vid_deskwidth;
	fmodelist[0].height = vid_deskheight;
	q_snprintf (fmodelist[0].modedesc, MAX_DESC, "%dx%d (desktop)", vid_deskwidth, vid_deskheight);
	num_fmodes = 1;
}

static void VID_InitWindowedModes (void)
{
	int	i;

	num_wmodes = 0;
	for (i = 0; i < (int)MAX_STDMODES; i++)
	{
		if (!VID_WindowFits(std_modes[i].width, std_modes[i].height))
			continue;
		wmodelist[num_wmodes].type = MS_WINDOWED;
		wmodelist[num_wmodes].width = std_modes[i].width;
		wmodelist[num_wmodes].height = std_modes[i].height;
		q_snprintf (wmodelist[num_wmodes].modedesc, MAX_DESC, "%dx%d",
				std_modes[i].width, std_modes[i].height);
		num_wmodes++;
	}
}


//====================================
// mode setting

static void ClearAllStates (void)
{
	Key_ClearStates ();
	IN_ClearStates ();
}

static void VID_RegisterWndClass (HINSTANCE hInstance)
{
	WNDCLASS	wc;

	wc.style		= CS_HREDRAW | CS_VREDRAW;
	wc.lpfnWndProc		= MainWndProc;
	wc.cbClsExtra		= 0;
	wc.cbWndExtra		= 0;
	wc.hInstance		= hInstance;
	wc.hIcon		= hIcon;
	wc.hCursor		= LoadCursor (NULL, IDC_ARROW);
	wc.hbrBackground	= NULL;		/* WM_PAINT keeps it black */
	wc.lpszMenuName		= NULL;
	wc.lpszClassName	= WM_CLASSNAME;

	if (!RegisterClass(&wc))
		Sys_Error ("Couldn't register main window class");

	classregistered = true;
}

static void VID_CreateWindow (void)
{
	mainwindow = CreateWindowEx (0, WM_CLASSNAME, WM_WINDOWNAME, WINDOWED_STYLE,
				CW_USEDEFAULT, CW_USEDEFAULT, 640, 480,
				NULL, NULL, global_hInstance, NULL);
	if (!mainwindow)
		Sys_Error ("Couldn't create the main window");

	SendMessage (mainwindow, WM_SETICON, (WPARAM)TRUE, (LPARAM)hIcon);
	SendMessage (mainwindow, WM_SETICON, (WPARAM)FALSE, (LPARAM)hIcon);
}

static void VID_SetMode (int modenum)
{
	vmode_t	*m;
	RECT	rect, work;
	POINT	client_origin = { 0, 0 };
	MSG	msg;

	if (modenum < 0 || modenum >= *nummodes)
		Sys_Error ("Bad video mode\n");

	CDAudio_Pause ();

	if (modelist == fmodelist)
		VID_UpdateFullscreenMode ();	/* the window may have moved monitors */
	m = &modelist[modenum];

	if (m->type == MS_FULLDIB)
	{
		/* borderless: cover the whole monitor */
		VID_GetMonitorRects (&rect, NULL);
		SetWindowLongPtr (mainwindow, GWL_STYLE, FULLSCREEN_STYLE);
		SetWindowPos (mainwindow, HWND_TOP, rect.left, rect.top,
				rect.right - rect.left, rect.bottom - rect.top,
				SWP_FRAMECHANGED | SWP_SHOWWINDOW);
		modestate = MS_FULLDIB;
		Cvar_SetQuick (&vid_config_fscr, "1");
	}
	else
	{
		/* framed window, centered in the monitor's work area */
		SetRect (&rect, 0, 0, m->width, m->height);
		AdjustWindowRectEx (&rect, WINDOWED_STYLE, FALSE, 0);
		VID_GetMonitorRects (NULL, &work);
		SetWindowLongPtr (mainwindow, GWL_STYLE, WINDOWED_STYLE);
		SetWindowPos (mainwindow, HWND_TOP,
				work.left + ((work.right - work.left) - (rect.right - rect.left)) / 2,
				work.top + ((work.bottom - work.top) - (rect.bottom - rect.top)) / 2,
				rect.right - rect.left, rect.bottom - rect.top,
				SWP_FRAMECHANGED | SWP_SHOWWINDOW);
		modestate = MS_WINDOWED;
		Cvar_SetQuick (&vid_config_fscr, "0");
	}

	ShowWindow (mainwindow, SW_SHOWNORMAL);
	UpdateWindow (mainwindow);

	/* where the client area ended up, in screen coordinates */
	ClientToScreen (mainwindow, &client_origin);
	window_x = client_origin.x;
	window_y = client_origin.y;
	window_width = m->width;
	window_height = m->height;
	VID_UpdateWindowStatus ();

	if (modestate == MS_FULLDIB || _enable_mouse.integer)
	{
		IN_ActivateMouse ();
		IN_HideMouse ();
	}
	else
	{
		IN_DeactivateMouse ();
		IN_ShowMouse ();
	}
	enable_mouse = _enable_mouse.integer;

	vid.numpages = 2;
	vid.width  = vid.conwidth  = m->width;
	vid.height = vid.conheight = m->height;
	vid.aspect = ((float)vid.height / (float)vid.width) * (320.0 / 240.0);

	/* setup the effective console width */
	VID_ConWidth (modenum);

	vid_modenum = modenum;
	if (modestate == MS_WINDOWED)
	{
		Cvar_SetValueQuick (&vid_config_glx, m->width);
		Cvar_SetValueQuick (&vid_config_gly, m->height);
	}

	SetForegroundWindow (mainwindow);
	while (PeekMessage (&msg, NULL, 0, 0, PM_REMOVE))
	{
		TranslateMessage (&msg);
		DispatchMessage (&msg);
	}

	/* fix the leftover Alt from any Alt-Tab or the like that switched us away */
	ClearAllStates ();

	CDAudio_Resume ();
}

static void VID_Restart_f (void)
{
	int	temp;

	if (vid_mode.integer < 0 || vid_mode.integer >= *nummodes)
	{
		Con_Printf ("Bad video mode %d\n", vid_mode.integer);
		Cvar_SetValueQuick (&vid_mode, vid_modenum);
		return;
	}

	Con_Printf ("Re-initializing video:\n");

	temp = scr_disabled_for_loading;
	scr_disabled_for_loading = true;
	BGM_Pause ();
	S_ClearBuffer ();
	IN_DeactivateMouse ();

	VID_SetMode (vid_mode.integer);	/* same window, new style and size */
	vid.recalc_refdef = 1;

	BGM_Resume ();
	scr_disabled_for_loading = temp;
	Con_Printf ("%s\n", modelist[vid_modenum].modedesc);
}


//====================================
// mode info commands

static const char *VID_GetExtModeDescription (int mode)
{
	static char	pinfo[100];

	if ((mode < 0) || (mode >= *nummodes))
		return badmode.modedesc;

	if (modelist[mode].type == MS_FULLDIB)
		q_snprintf (pinfo, sizeof(pinfo), "%s borderless fullscreen", modelist[mode].modedesc);
	else	q_snprintf (pinfo, sizeof(pinfo), "%s windowed", modelist[mode].modedesc);

	return pinfo;
}

static void VID_DescribeCurrentMode_f (void)
{
	Con_Printf ("%s\n", VID_GetExtModeDescription (vid_modenum));
}

static void VID_NumModes_f (void)
{
	if (*nummodes == 1)
		Con_Printf ("1 video mode is available\n");
	else
		Con_Printf ("%d video modes are available\n", *nummodes);
}

static void VID_DescribeMode_f (void)
{
	Con_Printf ("%s\n", VID_GetExtModeDescription (atoi(Cmd_Argv(1))));
}

static void VID_DescribeModes_f (void)
{
	int	i;

	for (i = 0; i < *nummodes; i++)
		Con_Printf ("%2d: %s\n", i, VID_GetExtModeDescription (i));
}


//====================================
// window messages

static byte scantokey[128] =
{
//	0        1       2       3       4       5       6       7
//	8        9       A       B       C       D       E       F
	0  ,    27,     '1',    '2',    '3',    '4',    '5',    '6',
	'7',    '8',    '9',    '0',    '-',    '=', K_BACKSPACE, 9,	// 0
	'q',    'w',    'e',    'r',    't',    'y',    'u',    'i',
	'o',    'p',    '[',    ']',  K_ENTER, K_CTRL,  'a',    's',	// 1
	'd',    'f',    'g',    'h',    'j',    'k',    'l',    ';',
	'\'',   '`',  K_SHIFT,  '\\',   'z',    'x',    'c',    'v',	// 2
	'b',    'n',    'm',    ',',    '.',    '/',  K_SHIFT, K_KP_STAR,
	K_ALT,  ' ',     0 ,    K_F1,   K_F2,   K_F3,   K_F4,  K_F5,	// 3
	K_F6,  K_F7,   K_F8,    K_F9,  K_F10, K_PAUSE,   0 , K_HOME,
	K_UPARROW,K_PGUP,K_KP_MINUS,K_LEFTARROW,K_KP_5,K_RIGHTARROW,K_KP_PLUS,K_END,	// 4
	K_DOWNARROW,K_PGDN,K_INS,K_DEL,   0 ,    0 ,     0 ,  K_F11,
	K_F12,   0 ,     0 ,     0 ,      0 ,    0 ,     0 ,     0 ,	// 5
	0  ,     0 ,     0 ,     0 ,      0 ,    0 ,     0 ,     0 ,
	0  ,     0 ,     0 ,     0 ,      0 ,    0 ,     0 ,     0 ,	// 6
	0  ,     0 ,     0 ,     0 ,      0 ,    0 ,     0 ,     0 ,
	0  ,     0 ,     0 ,     0 ,      0 ,    0 ,     0 ,     0	// 7
};

/* map from windows to quake keynums */
static int MapKey (int key)
{
	int result = (key >> 16) & 255;

	if (result > 127)
		return 0;
	result = scantokey[result];

	if (key & (1 << 24)) /* extended */
	{
		switch (result)
		{
		case K_PAUSE:
			return	(Key_IsGameKey()) ? K_KP_NUMLOCK : 0;
		case K_ENTER:
			return	(Key_IsGameKey()) ? K_KP_ENTER : K_ENTER;
		case '/':
			return	(Key_IsGameKey()) ? K_KP_SLASH : '/';
		}
	}
	else /* standart */
	{
		switch (result)
		{
		case K_KP_STAR:
			return	(Key_IsGameKey()) ? K_KP_STAR : '*';
		case K_KP_PLUS:
			return	(Key_IsGameKey()) ? K_KP_PLUS : '+';
		case K_KP_MINUS:
			return	(Key_IsGameKey()) ? K_KP_MINUS : '-';
		case K_HOME:
			return	(Key_IsGameKey()) ? K_KP_HOME :
				(GetKeyState(VK_NUMLOCK) & 0x01) ? '7' : K_HOME;
		case K_UPARROW:
			return	(Key_IsGameKey()) ? K_KP_UPARROW :
				(GetKeyState(VK_NUMLOCK) & 0x01) ? '8' : K_UPARROW;
		case K_PGUP:
			return	(Key_IsGameKey()) ? K_KP_PGUP :
				(GetKeyState(VK_NUMLOCK) & 0x01) ? '9' : K_PGUP;
		case K_LEFTARROW:
			return	(Key_IsGameKey()) ? K_KP_LEFTARROW :
				(GetKeyState(VK_NUMLOCK) & 0x01) ? '4' : K_LEFTARROW;
		case K_KP_5:
			return	(Key_IsGameKey()) ? K_KP_5 : '5';
		case K_RIGHTARROW:
			return	(Key_IsGameKey()) ? K_KP_RIGHTARROW :
				(GetKeyState(VK_NUMLOCK) & 0x01) ? '6' : K_RIGHTARROW;
		case K_END:
			return	(Key_IsGameKey()) ? K_KP_END :
				(GetKeyState(VK_NUMLOCK) & 0x01) ? '1' : K_END;
		case K_DOWNARROW:
			return	(Key_IsGameKey()) ? K_KP_DOWNARROW :
				(GetKeyState(VK_NUMLOCK) & 0x01) ? '2' : K_DOWNARROW;
		case K_PGDN:
			return	(Key_IsGameKey()) ? K_KP_PGDN :
				(GetKeyState(VK_NUMLOCK) & 0x01) ? '3' : K_PGDN;
		case K_INS:
			return	(Key_IsGameKey()) ? K_KP_INS :
				(GetKeyState(VK_NUMLOCK) & 0x01) ? '0' : K_INS;
		case K_DEL:
			return	(Key_IsGameKey()) ? K_KP_DEL :
				(GetKeyState(VK_NUMLOCK) & 0x01) ? '.' : K_DEL;
		}
	}

	return result;
}

static void AppActivate (BOOL fActive, BOOL minimize)
{
	static BOOL	sound_active;

	ActiveApp = fActive;
	Minimized = minimize;

// enable/disable sound on focus gain/loss
	if (!ActiveApp && sound_active)
	{
		S_BlockSound ();
		sound_active = false;
	}
	else if (ActiveApp && !sound_active)
	{
		S_UnblockSound ();
		sound_active = true;
	}

	if (fActive)
	{
		if (modestate == MS_FULLDIB)
		{
			IN_ActivateMouse ();
			IN_HideMouse ();
		}
		else if (modestate == MS_WINDOWED && _enable_mouse.integer)
		{
		// with winmouse, we may fail having our
		// window back from the iconified state. yuck...
			if (dinput_init)
			{
				IN_ActivateMouse ();
				IN_HideMouse ();
			}
		}
	}
	else
	{
		/* borderless fullscreen stays on screen; only the mouse is released */
		if (modestate == MS_FULLDIB || (modestate == MS_WINDOWED && _enable_mouse.integer))
		{
			IN_DeactivateMouse ();
			IN_ShowMouse ();
		}
	}
}

static LRESULT WINAPI MainWndProc (HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	LRESULT	ret = 0;
	int	fActive, fMinimized, temp;
	PAINTSTRUCT	ps;
	HDC	hdc;

	switch (uMsg)
	{
	case WM_ERASEBKGND:
		return 1;

	case WM_PAINT:
		/* nothing presents frames yet (story 1.3): keep the window black */
		hdc = BeginPaint (hWnd, &ps);
		FillRect (hdc, &ps.rcPaint, (HBRUSH) GetStockObject (BLACK_BRUSH));
		EndPaint (hWnd, &ps);
		return 0;

	case WM_MOVE:
		/* client area position in screen coordinates */
		window_x = (short) LOWORD(lParam);
		window_y = (short) HIWORD(lParam);
		VID_UpdateWindowStatus ();
		break;

	case WM_SIZE:
		break;

	case WM_SYSCHAR:
		// keep Alt-Space from happening
		break;

	case WM_ACTIVATE:
		fActive = LOWORD(wParam);
		fMinimized = (BOOL) HIWORD(wParam);
		AppActivate(!(fActive == WA_INACTIVE), fMinimized);

		// fix the leftover Alt from any Alt-Tab or the like that switched us away
		ClearAllStates ();

		break;

	case WM_KEYDOWN:
	case WM_SYSKEYDOWN:
		Key_Event (MapKey(lParam), true);
		break;

	case WM_KEYUP:
	case WM_SYSKEYUP:
		Key_Event (MapKey(lParam), false);
		break;

	// this is complicated because Win32 seems to pack multiple mouse
	// events into one update sometimes, so we always check all states
	// and look for events
	case WM_LBUTTONDOWN:
	case WM_LBUTTONUP:
	case WM_RBUTTONDOWN:
	case WM_RBUTTONUP:
	case WM_MBUTTONDOWN:
	case WM_MBUTTONUP:
	case WM_XBUTTONDOWN:
	case WM_XBUTTONUP:
	case WM_MOUSEMOVE:
		temp = 0;

		if (wParam & MK_LBUTTON)
			temp |= 1;

		if (wParam & MK_RBUTTON)
			temp |= 2;

		if (wParam & MK_MBUTTON)
			temp |= 4;

		// intellimouse explorer
		if (wParam & MK_XBUTTON1)
			temp |= 8;

		if (wParam & MK_XBUTTON2)
			temp |= 16;

		IN_MouseEvent (temp);

		break;

	case WM_MOUSEWHEEL:
		if ((short) HIWORD(wParam) > 0)
		{
			Key_Event(K_MWHEELUP, true);
			Key_Event(K_MWHEELUP, false);
		}
		else
		{
			Key_Event(K_MWHEELDOWN, true);
			Key_Event(K_MWHEELDOWN, false);
		}
		return 0;

	case WM_CLOSE:
		if (MessageBox (mainwindow, "Are you sure you want to quit?", "Confirm Exit",
					MB_YESNO | MB_SETFOREGROUND | MB_ICONQUESTION) == IDYES)
		{
			Sys_Quit ();
		}
		break;

	case WM_DESTROY:
		break;

	case MM_MCINOTIFY:
#if !defined(_NO_CDAUDIO)
		ret = CDAudio_MessageHandler (hWnd, uMsg, wParam, lParam);
#endif	/* ! _NO_CDAUDIO */
		break;

#if !defined(_NO_MIDIDRV)
	case WM_MSTREAM_UPDATEVOLUME:
		MIDI_SetChannelVolume((DWORD)wParam, (DWORD)lParam);
		return 1;

	case WM_MSTREAM_UPDATEVOLUMES:
		MIDI_SetAllChannelVolumes((DWORD) wParam);
		return 1;
#endif	/* ! _NO_MIDIDRV */

	default:
		/* pass all unhandled messages to DefWindowProc */
		ret = DefWindowProc (hWnd, uMsg, wParam, lParam);
		break;
	}

	/* return 1 if handled message, 0 if not */
	return ret;
}


//====================================
// init / shutdown

static void VID_InitPalette (const unsigned char *palette)
{
	int	i;
	const unsigned char	*pal = palette;

	/* RGBA, index 255 transparent (little endian, as on all Windows targets) */
	for (i = 0; i < 256; i++, pal += 3)
	{
		d_8to24table[i] = (unsigned int)pal[0] | ((unsigned int)pal[1] << 8) |
				  ((unsigned int)pal[2] << 16) | 0xff000000u;
	}
	d_8to24table[255] &= 0x00ffffffu;
}

void VID_Init (const unsigned char *palette)
{
	int	i, j, existingmode;
	int	width, height;
	const char	*read_vars[] = {
				"vid_config_fscr",
				"vid_config_glx",
				"vid_config_gly",
				"vid_config_consize" };
#define num_readvars	Q_COUNTOF(read_vars)

	Cvar_RegisterVariable (&vid_config_fscr);
	Cvar_RegisterVariable (&vid_config_gly);
	Cvar_RegisterVariable (&vid_config_glx);
	Cvar_RegisterVariable (&vid_config_consize);
	Cvar_RegisterVariable (&vid_mode);
	Cvar_RegisterVariable (&_enable_mouse);

	Cmd_AddCommand ("vid_nummodes", VID_NumModes_f);
	Cmd_AddCommand ("vid_describecurrentmode", VID_DescribeCurrentMode_f);
	Cmd_AddCommand ("vid_describemode", VID_DescribeMode_f);
	Cmd_AddCommand ("vid_describemodes", VID_DescribeModes_f);
	Cmd_AddCommand ("vid_restart", VID_Restart_f);

	VID_InitPalette (palette);

	hIcon = LoadIcon (global_hInstance, MAKEINTRESOURCE (IDI_ICON2));
	VID_RegisterWndClass (global_hInstance);

	VID_UpdateFullscreenMode ();
	VID_InitWindowedModes ();
	Con_SafePrintf ("Desktop settings: %d x %d\n", vid_deskwidth, vid_deskheight);

	// perform an early read of config.cfg
	CFG_ReadCvars (read_vars, num_readvars);

	width = vid_config_glx.integer;
	height = vid_config_gly.integer;

	if (COM_CheckParm("-window") || COM_CheckParm("-w"))
		Cvar_SetQuick (&vid_config_fscr, "0");
	else if (COM_CheckParm("-fullscreen") || COM_CheckParm("-f"))
		Cvar_SetQuick (&vid_config_fscr, "1");

	if (vid_config_consize.integer != width)
		vid_conscale = true;

	if (!vid_config_fscr.integer)
	{
		modelist = wmodelist;
		nummodes = &num_wmodes;
		vid_default = RES_640X480;

		// start parsing any dimension request from user
		i = COM_CheckParm("-width");
		if (i && i < com_argc-1)
		{
			j = atoi(com_argv[i+1]);
			if (j >= MIN_WIDTH)
			{
				width = j;
				height = width * 3 / 4;
				i = COM_CheckParm("-height");
				if (i && i < com_argc-1)
				{
					j = atoi(com_argv[i+1]);
					if (j >= MIN_HEIGHT)
						height = j;
				}
			}
		}
		// don't allow windows larger than the monitor
		if (!VID_WindowFits(width, height))
		{
			Con_SafePrintf ("%dx%d window does not fit on the monitor, using 640x480\n", width, height);
			width = 640;
			height = 480;
		}

		for (i = 0, existingmode = 0; i < num_wmodes; i++)
		{
			if (wmodelist[i].width == width && wmodelist[i].height == height)
			{
				existingmode = 1;
				vid_default = i;
				break;
			}
		}
		if (!existingmode)
		{
			wmodelist[num_wmodes].type = MS_WINDOWED;
			wmodelist[num_wmodes].width = width;
			wmodelist[num_wmodes].height = height;
			q_snprintf (wmodelist[num_wmodes].modedesc, MAX_DESC, "%dx%d (user mode)", width, height);
			vid_default = num_wmodes;
			num_wmodes++;
		}
		if (vid_default >= num_wmodes)	/* tiny monitor paranoia */
			vid_default = num_wmodes - 1;
	}
	else	/* fullscreen, default */
	{
		modelist = fmodelist;
		nummodes = &num_fmodes;
		vid_default = 0;
		width = fmodelist[0].width;
	}

	if (!vid_conscale)
		Cvar_SetValueQuick (&vid_config_consize, width);

	// This will display a bigger hud and readable fonts at high
	// resolutions. The fonts will be somewhat distorted, though
	i = COM_CheckParm("-conwidth");
	if (i != 0 && i < com_argc-1)
		i = atoi(com_argv[i + 1]);
	else	i = vid_config_consize.integer;
	if (i < MIN_WIDTH)	i = MIN_WIDTH;
	else if (i > width)	i = width;
	Cvar_SetValueQuick(&vid_config_consize, i);
	if (vid_config_consize.integer != width)
		vid_conscale = true;

	vid_initialized = true;

	vid.maxwarpwidth = 320;		/* WARP_WIDTH/HEIGHT of gl_vidnt.c */
	vid.maxwarpheight = 200;
	vid.colormap = host_colormap;
	vid.fullbright = 256 - LittleLong (*((int *)vid.colormap + 2048));

#if !defined(NO_SPLASHES)
	if (hwnd_dialog)
	{
		DestroyWindow (hwnd_dialog);
		hwnd_dialog = NULL;
	}
#endif

	j = scr_disabled_for_loading;
	scr_disabled_for_loading = true;

	VID_CreateWindow ();
	Cvar_SetValueQuick (&vid_mode, vid_default);
	VID_SetMode (vid_default);

	// lock the early-read cvars until Host_Init is finished
	for (i = 0; i < (int)num_readvars; i++)
		Cvar_LockVar (read_vars[i]);

	scr_disabled_for_loading = j;
	vid.recalc_refdef = 1;

	vid_menudrawfn = VID_MenuDraw;
	vid_menukeyfn = VID_MenuKey;

	q_strlcpy (badmode.modedesc, "Bad mode", MAX_DESC);

	Con_SafePrintf ("Video mode %s\n", VID_GetExtModeDescription (vid_modenum));
}

void VID_Shutdown (void)
{
	if (vid_initialized)
	{
		AppActivate (false, false);
		if (mainwindow)
		{
			ShowWindow (mainwindow, SW_HIDE);
			DestroyWindow (mainwindow);
		}
		if (classregistered)
			UnregisterClass (WM_CLASSNAME, global_hInstance);
		mainwindow = NULL;
		classregistered = false;
		vid_initialized = false;
	}
}


//========================================================
// Video menu
//========================================================

static int	vid_menunum;
static int	vid_cursor;
static vmode_t	*vid_menulist;
static qboolean	vid_menu_fs;
static qboolean	want_fstoggle, need_apply;
static qboolean	vid_menu_firsttime = true;

enum {
	VID_FULLSCREEN,
	VID_RESOLUTION,
	VID_BLANKLINE,	// spacer line
	VID_RESET,
	VID_APPLY,
	VID_ITEMS
};

static void M_DrawYesNo (int x, int y, int on, int white)
{
	if (on)
	{
		if (white)
			M_PrintWhite (x, y, "yes");
		else
			M_Print (x, y, "yes");
	}
	else
	{
		if (white)
			M_PrintWhite (x, y, "no");
		else
			M_Print (x, y, "no");
	}
}

static void VID_MenuDraw (void)
{
	ScrollTitle("gfx/menu/title7.lmp");

	if (vid_menu_firsttime)
	{	// settings for entering the menu first time
		vid_menunum = vid_modenum;
		vid_menu_fs = (modestate != MS_WINDOWED);
		vid_menulist = (modestate == MS_WINDOWED) ? wmodelist : fmodelist;
		vid_cursor = 0;
		vid_menu_firsttime = false;
	}

	want_fstoggle = ( ((modestate == MS_WINDOWED) && vid_menu_fs) || ((modestate != MS_WINDOWED) && !vid_menu_fs) );
	need_apply = (vid_menunum != vid_modenum) || want_fstoggle;

	M_Print (76, 92 + 8*VID_FULLSCREEN, "Fullscreen: ");
	M_DrawYesNo (76+12*8, 92 + 8*VID_FULLSCREEN, vid_menu_fs, !want_fstoggle);

	M_Print (76, 92 + 8*VID_RESOLUTION, "Resolution: ");
	if (vid_menunum == vid_modenum && !want_fstoggle)
		M_PrintWhite (76+12*8, 92 + 8*VID_RESOLUTION, vid_menulist[vid_menunum].modedesc);
	else
		M_Print (76+12*8, 92 + 8*VID_RESOLUTION, vid_menulist[vid_menunum].modedesc);

	if (need_apply)
	{
		M_Print (76, 92 + 8*VID_RESET, "RESET CHANGES");
		M_Print (76, 92 + 8*VID_APPLY, "APPLY CHANGES");
	}

	M_DrawCharacter (64, 92 + vid_cursor*8, 12+((int)(realtime*4)&1));
}

/* the mode of the other list with the same size; for windowed, otherwise
 * the last used window size, otherwise 640x480 */
static int match_windowed_fullscr_modes (void)
{
	int	l;
	vmode_t	*tmplist;
	int	*tmpcount;

	tmplist = (vid_menu_fs) ? fmodelist : wmodelist;
	tmpcount = (vid_menu_fs) ? &num_fmodes : &num_wmodes;
	for (l = 0; l < *tmpcount; l++)
	{
		if (tmplist[l].width == vid_menulist[vid_menunum].width &&
		    tmplist[l].height == vid_menulist[vid_menunum].height)
		{
			return l;
		}
	}
	if (vid_menu_fs)
		return 0;
	for (l = 0; l < num_wmodes; l++)
	{
		if (wmodelist[l].width == vid_config_glx.integer &&
		    wmodelist[l].height == vid_config_gly.integer)
		{
			return l;
		}
	}
	return (RES_640X480 < num_wmodes) ? RES_640X480 : num_wmodes - 1;
}

static void VID_MenuKey (int key)
{
	int	*tmpnum;

	switch (key)
	{
	case K_ESCAPE:
		vid_cursor = 0;
		M_Menu_Options_f ();
		break;

	case K_UPARROW:
		S_LocalSound ("raven/menu1.wav");
		vid_cursor--;
		if (vid_cursor < 0)
			vid_cursor = (need_apply) ? VID_ITEMS-1 : VID_BLANKLINE-1;
		else if (vid_cursor == VID_BLANKLINE)
			vid_cursor--;
		break;

	case K_DOWNARROW:
		S_LocalSound ("raven/menu1.wav");
		vid_cursor++;
		if (vid_cursor >= VID_ITEMS)
			vid_cursor = 0;
		else if (vid_cursor >= VID_BLANKLINE)
		{
			if (need_apply)
			{
				if (vid_cursor == VID_BLANKLINE)
					vid_cursor++;
			}
			else
			{
				vid_cursor = 0;
			}
		}
		break;

	case K_ENTER:
		switch (vid_cursor)
		{
		case VID_RESET:
			vid_menunum = vid_modenum;
			vid_menu_fs = (modestate != MS_WINDOWED);
			vid_menulist = (modestate == MS_WINDOWED) ? wmodelist : fmodelist;
			vid_cursor = 0;
			break;
		case VID_APPLY:
			if (need_apply)
			{
				Cvar_SetValueQuick (&vid_mode, vid_menunum);
				modelist = (vid_menu_fs) ? fmodelist : wmodelist;
				nummodes = (vid_menu_fs) ? &num_fmodes : &num_wmodes;
				VID_Restart_f ();
			}
			vid_cursor = 0;
			break;
		}
		return;

	case K_LEFTARROW:
	case K_RIGHTARROW:
		switch (vid_cursor)
		{
		case VID_FULLSCREEN:
			vid_menu_fs = !vid_menu_fs;
			vid_menunum = match_windowed_fullscr_modes ();
			vid_menulist = (vid_menu_fs) ? fmodelist : wmodelist;
			break;
		case VID_RESOLUTION:
			S_LocalSound ("raven/menu1.wav");
			tmpnum = (vid_menu_fs) ? &num_fmodes : &num_wmodes;
			if (key == K_LEFTARROW)
			{
				if (vid_menunum > 0)
					vid_menunum--;
			}
			else
			{
				if (vid_menunum < *tmpnum - 1)
					vid_menunum++;
			}
			break;
		}
		return;

	default:
		break;
	}
}
