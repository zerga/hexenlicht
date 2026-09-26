/* vk_profiler.c -- GPU timers for the frame's passes, their overlay, and
 * vk_benchmark for measuring
 *
 * Quake II RTX's profiler.c: every pass is bracketed by VK_ProfilerStart
 * and VK_ProfilerStop (vk_local.h's PROFILER_LIST), which write a
 * timestamp each into this frame in flight's range of one query pool, and
 * with the validation layer (Debug builds, or -validation: it loads the
 * debug-utils extension) also a debug label for RenderDoc and Nsight. VK_ProfilerBeginFrame (VK_BeginFrame) reads
 * the range the frame in flight used last time, which its fence has made
 * ready, so nothing waits on the GPU, then resets it; the timings are two
 * frames old. Always on, as in Quake II RTX: a few dozen timestamps per
 * frame. Changes: the timestamps are taken after all earlier commands
 * (Quake II RTX starts at the top of the pipe), so a pass's time is its
 * own; a pass that didn't run in a frame drops its samples (Quake II
 * RTX's reset_samples) and its row; the samples are a ring of
 * MAX_SAMPLES per entry, profiler_samples of them averaged (Quake II RTX
 * reallocates its rings); a pass may name itself (the upscaler: TAA,
 * TAAU, DLSS SR, DLSS RR).
 *
 * profiler 1 draws the timings over the top left of the screen
 * (VK_DrawProfiler, from GL_EndRendering, with the game's font, so at
 * vid_uiscale); vk_profiler prints them. vk_benchmark 1 (not archived)
 * lifts the 72 fps cap (host.c), the unfocused window's sleep and the
 * frame throttle (sys_win.c), so the GPU runs at full load and its clocks
 * don't follow a light load: for measuring only, Hexen II's physics isn't
 * meant for more than 72 frames a second.
 *
 * Copyright (C) 2018 Christoph Schied
 * Copyright (C) 2019, NVIDIA CORPORATION. All rights reserved.
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

#define MAX_SAMPLES		1000
#define QUERIES_PER_FRAME	(NUM_PROF_ENTRIES * 2)

static const struct
{
	const char	*name;
	int		indent;
} entries[NUM_PROF_ENTRIES] = {
#define PROF_DO(id, name, indent) { name, indent },
	PROFILER_LIST
#undef PROF_DO
};

/* Quake II RTX's cvars (not its profiler_scale: the overlay follows vid_uiscale) */
static cvar_t	profiler = {"profiler", "0", CVAR_NONE};
static cvar_t	profiler_samples = {"profiler_samples", "60", CVAR_ARCHIVE};
/* measuring: no 72 fps cap, no sleep when the window isn't focused */
static cvar_t	vk_benchmark = {"vk_benchmark", "0", CVAR_NONE};

static VkQueryPool	query_pool;
static int		frame_slot = -1;	/* the frame in flight the markers write for, -1 outside frames */
static qboolean		used[VK_FRAMES_IN_FLIGHT][NUM_PROF_ENTRIES];
static const char	*labels[VK_FRAMES_IN_FLIGHT][NUM_PROF_ENTRIES];

/* the timings read back, per entry */
static struct
{
	double		samples[MAX_SAMPLES];	/* ms, a ring */
	int		num, next;
	double		last;		/* ms, the latest frame read */
	qboolean	ran;		/* in the latest frame read */
	const char	*label;		/* its name in that frame */
} results[NUM_PROF_ENTRIES];

qboolean VK_Benchmark (void)
{
	return vk_benchmark.integer != 0;
}

static void DebugLabel (VkCommandBuffer cmd, const char *name)
{
	VkDebugUtilsLabelEXT	label;

	if (!vk.validation || !vkCmdBeginDebugUtilsLabelEXT)
		return;		/* the extension is loaded with the validation layer */
	memset (&label, 0, sizeof(label));
	label.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;
	label.pLabelName = name;
	vkCmdBeginDebugUtilsLabelEXT (cmd, &label);
}

void VK_ProfilerStartNamed (VkCommandBuffer cmd, int entry, const char *name)
{
	if (!query_pool || frame_slot < 0)
		return;
	vkCmdWriteTimestamp2 (cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, query_pool,
			      (uint32_t)(frame_slot * QUERIES_PER_FRAME + entry * 2));
	used[frame_slot][entry] = true;
	labels[frame_slot][entry] = name;
	DebugLabel (cmd, name);
}

/* renames a running entry for this frame (the upscaler when DLSS failed) */
void VK_ProfilerLabel (int entry, const char *name)
{
	if (frame_slot >= 0)
		labels[frame_slot][entry] = name;
}

void VK_ProfilerStart (VkCommandBuffer cmd, int entry)
{
	VK_ProfilerStartNamed (cmd, entry, entries[entry].name);
}

void VK_ProfilerStop (VkCommandBuffer cmd, int entry)
{
	if (!query_pool || frame_slot < 0)
		return;
	vkCmdWriteTimestamp2 (cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, query_pool,
			      (uint32_t)(frame_slot * QUERIES_PER_FRAME + entry * 2 + 1));
	if (vk.validation && vkCmdEndDebugUtilsLabelEXT)
		vkCmdEndDebugUtilsLabelEXT (cmd);
}

static void RecordSample (int entry, double ms)
{
	results[entry].samples[results[entry].next] = ms;
	results[entry].next = (results[entry].next + 1) % MAX_SAMPLES;
	if (results[entry].num < MAX_SAMPLES)
		results[entry].num++;
	results[entry].last = ms;
}

/* this frame in flight's timings of the last time it ran, which its fence
 * has made ready */
static void ReadResults (int slot)
{
	uint64_t	data[QUERIES_PER_FRAME][2];	/* value, availability */
	int		i;
	qboolean	any = false;

	for (i = 0; i < NUM_PROF_ENTRIES; i++)
		any |= used[slot][i];
	if (!any)
		return;
	memset (data, 0, sizeof(data));
	vkGetQueryPoolResults (vk.device, query_pool, (uint32_t)(slot * QUERIES_PER_FRAME), QUERIES_PER_FRAME,
			       sizeof(data), data, sizeof(data[0]), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
	for (i = 0; i < NUM_PROF_ENTRIES; i++)
	{
		const uint64_t	*start = data[i * 2], *stop = data[i * 2 + 1];

		if (used[slot][i] && start[1] && stop[1] && stop[0] >= start[0])
		{
			RecordSample (i, (double)(stop[0] - start[0]) * vk.props.limits.timestampPeriod / 1.0e6);
			results[i].ran = true;
			results[i].label = labels[slot][i];
		}
		else
		{
			results[i].num = results[i].next = 0;
			results[i].last = 0.0;
			results[i].ran = false;
		}
	}
}

/* VK_BeginFrame, with the command buffer begun */
void VK_ProfilerBeginFrame (VkCommandBuffer cmd)
{
	int	slot = (int)vk.frame_index;

	if (!query_pool)
		return;
	ReadResults (slot);
	vkCmdResetQueryPool (cmd, query_pool, (uint32_t)(slot * QUERIES_PER_FRAME), QUERIES_PER_FRAME);
	memset (used[slot], 0, sizeof(used[slot]));
	frame_slot = slot;
	VK_ProfilerStart (cmd, PROF_FRAME);
}

/* VK_EndFrame, before the command buffer ends */
void VK_ProfilerEndFrame (VkCommandBuffer cmd)
{
	VK_ProfilerStop (cmd, PROF_FRAME);
	frame_slot = -1;
}

/* an entry's latest time and its average over profiler_samples frames, in
 * ms; false if it didn't run in the latest frame read */
qboolean VK_ProfilerTime (int entry, double *last, double *average)
{
	int	n = q_min (results[entry].num, q_max (1, q_min (MAX_SAMPLES, profiler_samples.integer)));
	int	i, k = results[entry].next;
	double	sum = 0.0;

	for (i = 0; i < n; i++)
	{
		k = (k + MAX_SAMPLES - 1) % MAX_SAMPLES;
		sum += results[entry].samples[k];
	}
	*last = results[entry].last;
	*average = n ? sum / n : 0.0;
	return results[entry].ran;
}

/* a row of the table: the entry's name indented, its latest time and average */
static void FormatRow (char *buf, size_t size, int entry)
{
	char	name[32];
	double	last, average;

	VK_ProfilerTime (entry, &last, &average);
	q_snprintf (name, sizeof(name), "%*s%s", entries[entry].indent, "",
		    results[entry].label ? results[entry].label : entries[entry].name);
	q_snprintf (buf, size, "%-20s %6.2f %6.2f", name, last, average);
}

/* GL_EndRendering, before the 2D is drawn: the table over the top left,
 * below the console's notify lines */
void VK_DrawProfiler (void)
{
	char	buf[64];
	int	i, rows = 1, x = 8, y = 40, w = 34 * 8;

	if (!profiler.integer || !query_pool)
		return;
	for (i = 0; i < NUM_PROF_ENTRIES; i++)
		rows += results[i].ran;
	VK_DrawShade (x - 4, y - 4, w + 8, rows * 8 + 8, 0.6f);
	Draw_String (x, y, "GPU ms                 last    avg");
	for (i = 0; i < NUM_PROF_ENTRIES; i++)
	{
		if (!results[i].ran)
			continue;
		y += 8;
		FormatRow (buf, sizeof(buf), i);
		Draw_String (x, y, buf);
	}
}

static void VK_Profiler_f (void)
{
	char	buf[64];
	int	i;

	if (!query_pool)
	{
		Con_Printf ("No GPU timestamps on this device\n");
		return;
	}
	Con_Printf ("GPU ms                 last    avg (%d frames; vk_benchmark %d)\n",
		    q_max (1, q_min (MAX_SAMPLES, profiler_samples.integer)), vk_benchmark.integer);
	for (i = 0; i < NUM_PROF_ENTRIES; i++)
	{
		if (!results[i].ran)
			continue;
		FormatRow (buf, sizeof(buf), i);
		Con_Printf ("%s\n", buf);
	}
}

void VK_InitProfiler (void)
{
	VkQueryPoolCreateInfo	info;

	Cvar_RegisterVariable (&profiler);
	Cvar_RegisterVariable (&profiler_samples);
	Cvar_RegisterVariable (&vk_benchmark);
	Cmd_AddCommand ("vk_profiler", VK_Profiler_f);
	if (!vk.props.limits.timestampComputeAndGraphics)
		return;
	memset (&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
	info.queryType = VK_QUERY_TYPE_TIMESTAMP;
	info.queryCount = VK_FRAMES_IN_FLIGHT * QUERIES_PER_FRAME;
	VK_CHECK (vkCreateQueryPool (vk.device, &info, NULL, &query_pool));
}

void VK_ShutdownProfiler (void)
{
	if (query_pool)
		vkDestroyQueryPool (vk.device, query_pool, NULL);
	query_pool = VK_NULL_HANDLE;
	frame_slot = -1;
	memset (used, 0, sizeof(used));
	memset (results, 0, sizeof(results));
}
