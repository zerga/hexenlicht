/* cl_demoangles.c -- the view's angles in demo playback, interpolated
 *
 * Story 6.7. In demo playback the client turns the view between the
 * angles stored with the last two messages it read (cl_main.c's
 * CL_RelinkEntities), which is choppy in two ways:
 *  - a frame that reads two messages of one server time (the second
 *    without svc_time, e.g. Hexen II's svc_setangle_interpolate) has the
 *    same angles at both ends, so the view snaps;
 *  - Portal of Praevus's intro (t9.dem, intro_playing) doesn't use the
 *    stored angles: its cameras turn by svc_setangle_interpolate, an
 *    eighth of the way to the camera's angles ten times a second
 *    (cl_parse.c), a step every 0.1 s.
 * Here each change of the demo's angles (the stored ones, or the intro's
 * after its messages) is a move to them over the time since the previous
 * change, at most 0.1 s, from where the last move is at the previous
 * message's time (the start of the client's interpolation between two
 * messages): a camera turned ten times a second turns smoothly, a demo
 * whose angles change with every message turns as before. A playback's
 * start, a change of more than 45 degrees (so a turn faster than 900
 * degrees a second steps at 20 messages a second), another view entity or
 * the time running over a second back (a new map) is a cut, shown at once;
 * timedemo shows the latest angles, as before. The intro's messages turn
 * from the intro's own angles, not the shown ones
 * (CL_DemoAnglesBeforeParse), so its cameras keep their path.
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

#define	MAX_SPAN	0.1	/* seconds: the longest a change is spread over */
#define	CUT_ANGLE	45.0f	/* degrees: a larger change is shown at once */

static qboolean	have;		/* the state below follows this playback */
static int	view_entity;	/* cl.viewentity it follows */
static vec3_t	latest;		/* the demo's latest angles */
static vec3_t	from;		/* where the move to them starts */
static vec3_t	intro_angles;	/* the intro's own angles, after its messages */
static double	msg_time;	/* cl.mtime[0] when "latest" was last checked */
static double	change_time;	/* the server time of the latest change */
static double	start, span;	/* the move from "from" to "latest": when (cl.time), how long */

/* a difference of angles, in -180..180 */
static float AngleDiff (float a)
{
	return a - 360.0f * floorf ((a + 180.0f) / 360.0f);
}

/* the move's angles at time t */
static void AnglesAt (double t, vec3_t out)
{
	float	f = (span > 0.0) ? (float)((t - start) / span) : 1.0f;
	int	i;

	f = q_min (q_max (f, 0.0f), 1.0f);
	for (i = 0; i < 3; i++)
		out[i] = from[i] + f * AngleDiff (latest[i] - from[i]);
}

/* before the frame's messages are read (CL_ReadFromServer): each playback
 * starts over (CL_Disconnect zeroes cls.signon before it), and the intro's
 * svc_setangle_interpolate turns from the intro's own angles */
void CL_DemoAnglesBeforeParse (void)
{
	if (!cls.demoplayback || cls.signon < SIGNONS)
		have = false;
	else if (intro_playing && have)
		VectorCopy (intro_angles, cl.viewangles);
}

/* in demo playback, after CL_LerpPoint (CL_RelinkEntities): the angles to
 * show, into cl.viewangles */
void CL_DemoAngles (void)
{
	vec3_t	now;
	float	largest = 0.0f;
	int	i;

	if (intro_playing)
	{
		VectorCopy (cl.viewangles, now);	/* after the frame's messages */
		VectorCopy (now, intro_angles);
	}
	else
		VectorCopy (cl.mviewangles[0], now);

	if (cl.mtime[0] != msg_time)
	{
		for (i = 0; i < 3; i++)
			largest = q_max (largest, fabsf (AngleDiff (now[i] - latest[i])));
	}
	if (!have || cls.timedemo || cl.viewentity != view_entity || cl.time < start - 1.0 || largest > CUT_ANGLE)
	{	/* a cut */
		have = true;
		view_entity = cl.viewentity;
		msg_time = change_time = cl.mtime[0];
		start = cl.time;
		span = 0.0;
		VectorCopy (now, latest);
		VectorCopy (now, from);
	}
	else if (cl.mtime[0] != msg_time)
	{
		msg_time = cl.mtime[0];
		if (largest > 0.0f)
		{	/* a change: a move from the last message's time (cl.mtime[1],
			 * which the frames shown so far haven't passed), where the
			 * last move is then, over the time since the last change */
			AnglesAt (cl.mtime[1], from);
			start = cl.mtime[1];
			span = q_min (msg_time - change_time, MAX_SPAN);
			change_time = msg_time;
			VectorCopy (now, latest);
		}
	}

	AnglesAt (cl.time, cl.viewangles);
}
