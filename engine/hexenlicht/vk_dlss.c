/* vk_dlss.c -- DLSS Super Resolution and Ray Reconstruction in the TAA
 * pass's place, through NVIDIA Streamline (vk_streamline.cpp) with the
 * player's DLLs
 *
 * r_upscaler 3 (DLSS SR, on the denoiser's image) and 4 (DLSS RR, on the
 * noisy image instead of the denoiser) run when Streamline is loaded and
 * the GPU supports the feature; otherwise vk_upscale.c runs TAAU and
 * vk_upscale, vk_dlss and the Renderer Settings page (6.10, vk_menu.c)
 * say why. A-SVGF + TAAU stays the default.
 *  - VK_DLSSChoose (from VK_UpscaleEvaluate) picks DLSS's mode from r_scale
 *    (100 % DLAA, from 66 % Quality, from 58 % Balanced, from 50 %
 *    Performance, below that Ultra Performance) and clamps the render size
 *    to what DLSS accepts for it (its optimal settings' minimum and
 *    maximum), the width kept even for the checkerboard fields; between
 *    Ultra Performance's one size (a third) and Performance's (a half and
 *    more), the nearer one. The frame
 *    is jittered as TAAU's and DLSS writes TAA_OUTPUT at the view's size.
 *  - The inputs: checkerboard_interleave.comp writes them in the screen
 *    layout with the lit image (dlss_inputs.glsl): hardware depth for both
 *    features, RR's guides for RR. Their images are full size only while a
 *    feature is chosen (vk_dlss_images, vk_images.c); VK_DLSSBetweenFrames
 *    recreates the render targets on a change, as a resize does, and frees
 *    the resources of a feature no longer chosen (Streamline keeps them
 *    otherwise).
 *  - VK_DLSSRun evaluates with DECISIONS R53's conventions: DLSS's jitter is
 *    the negation of ours, our motion vectors (previous minus current
 *    position, UV units) go in with scale 1, our column-major matrices are
 *    Streamline's row-major ones, DLSS gets a D3D-style depth row
 *    (DLSS_Z_NEAR to DLSS_Z_FAR: Quake II RTX's projection has none) and
 *    preExposure 1 (our x128 storage scale stays). DLSS's history is reset
 *    on the events that drop the denoiser's (VK_ResetDenoiserHistory: a new
 *    map, new images, a skipped view, changed cvars), after a frame without
 *    DLSS or with the other feature, and when the mode or a size changes.
 *    A failed evaluation leaves the frame to the TAA pass's copy and the
 *    next ones to TAAU until r_upscaler, r_scale or the view's size
 *    changes.
 *  - r_dlss_preset: DLSS's model, 0 its default for the mode (fixed by the
 *    version of the player's DLLs: over-the-air updates are off) or a
 *    preset letter (Streamline 2.14.1: RR D-F, SR J-M; DLSS takes others
 *    as its default).
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
#include "r_scene.h"
#include "shaders/hl_shared.h"
#include "shaders/global_textures.h"
#include "vk_streamline.h"

/* 0 DLSS's default model for the mode, or a preset letter */
static cvar_t	r_dlss_preset = {"r_dlss_preset", "0", CVAR_ARCHIVE};

/* an evaluation failed: TAAU until r_upscaler, r_scale or the view's size changes */
static qboolean		failed[3];
static int		failed_percent[3];
static VkExtent2D	failed_size[3];
static int		chosen_percent;		/* r_scale in the frame VK_DLSSChoose ran DLSS in */
static qboolean		resources[3];		/* the feature has evaluated: Streamline holds its resources */
static qboolean		history;		/* the last 3D frame ran DLSS */
static int		last_feature, last_mode;
static VkExtent2D	last_render, last_output;

/* DLSS's render sizes per feature and mode, for the last output size asked for */
static struct
{
	qboolean	valid;
	uint32_t	width, height;
	vk_sl_range_t	range;
} range_cache[3][VK_SL_NUM_MODES];
static int		chosen_mode = -1;	/* the last frame VK_DLSSChoose ran DLSS in, for vk_dlss */
static vk_sl_range_t	chosen_range;

static const char	*mode_names[VK_SL_NUM_MODES] = { "DLAA", "Quality", "Balanced", "Performance", "Ultra Performance" };

static int ModeForScale (int percent)
{
	if (percent >= 100)
		return VK_SL_MODE_DLAA;
	if (percent >= 66)
		return VK_SL_MODE_QUALITY;
	if (percent >= 58)
		return VK_SL_MODE_BALANCED;
	if (percent >= 50)
		return VK_SL_MODE_PERFORMANCE;
	return VK_SL_MODE_ULTRA_PERFORMANCE;
}

/* r_dlss_preset: 0, or 1-15 for the letters A-O */
static int Preset (void)
{
	int	c = (unsigned char)r_dlss_preset.string[0];

	if (c >= 'a' && c <= 'z')
		c -= 'a' - 'A';
	return (c >= 'A' && c <= 'O' && !r_dlss_preset.string[1]) ? c - 'A' + 1 : 0;
}

static qboolean Range (int feature, int mode, uint32_t width, uint32_t height, vk_sl_range_t *range)
{
	int	f = q_max (0, q_min (2, feature)), m = q_max (0, q_min (VK_SL_NUM_MODES - 1, mode));

	if (range_cache[f][m].width != width || range_cache[f][m].height != height)
	{
		range_cache[f][m].width = width;
		range_cache[f][m].height = height;
		range_cache[f][m].valid = VK_SLRange (feature, mode, width, height, &range_cache[f][m].range) != 0;
		if (!range_cache[f][m].valid)
			memset (&range_cache[f][m].range, 0, sizeof(range_cache[f][m].range));
	}
	*range = range_cache[f][m].range;
	return range_cache[f][m].valid;
}

/* the render width nearest the wanted one that the mode accepts, even for
 * the two checkerboard fields; false if its range holds none */
static qboolean FitWidth (int feature, int mode, const vk_upscale_t *up, uint32_t *w, vk_sl_range_t *range)
{
	uint32_t	x;

	if (!Range (feature, mode, up->unscaled.width, up->unscaled.height, range))
		return false;
	x = q_max (range->min_width, q_min (range->max_width, up->render.width));
	if (x & 1)
	{
		if (x + 1 <= range->max_width)
			x++;
		else if (x - 1 >= range->min_width)
			x--;
		else
			return false;
	}
	*w = x;
	return true;
}

/* the images the chosen feature needs (vk_images.c's vk_dlss_images): 0,
 * VK_SL_SR (the depth) or VK_SL_RR (RR's guides too) */
int VK_DLSSImagesWanted (void)
{
	int	feature = VK_UpscaleDLSSFeature ();

	return VK_SLSupported (feature) ? feature : 0;
}

/* why the feature can't run, or NULL; images: also while its images
 * aren't created yet */
static const char *Unavailable (int feature, qboolean images)
{
	if (!VK_SLActive ())
		return VK_SLInactiveReason ();
	if (!VK_SLSupported (feature))
		return "not supported by this GPU or driver";
	if (failed[feature])
		return "it failed";
	if (images && vk_dlss_images < feature)
		return "its images are not created yet";
	return NULL;
}

const char *VK_DLSSUnavailable (int feature)
{
	return Unavailable (feature, true);
}

/* 6.10's menu: not while the next frames create its images */
const char *VK_DLSSCantRun (int feature)
{
	return Unavailable (feature, false);
}

/* VK_UpscaleEvaluate: whether DLSS runs this frame; if so the mode and the
 * render size (clamped to DLSS's range) go into up */
qboolean VK_DLSSChoose (int feature, qboolean lit, int percent, vk_upscale_t *up)
{
	vk_sl_range_t	range, range2;
	uint32_t	w, w2, h;
	int		mode;

	if (feature && failed[feature] && (failed_percent[feature] != percent ||
	    failed_size[feature].width != up->unscaled.width || failed_size[feature].height != up->unscaled.height))
		failed[feature] = false;	/* another try at another size */
	if (!feature || !lit || VK_DLSSUnavailable (feature))
		return false;
	chosen_percent = percent;
	/* where the mode's range holds no even width, the next mode up */
	mode = ModeForScale (percent);
	while (!FitWidth (feature, mode, up, &w, &range))
	{
		if (mode == VK_SL_MODE_DLAA)
			return false;
		mode--;
	}
	/* between the modes' ranges (Ultra Performance takes a third,
	 * Performance a half and more) the nearer size */
	if (w != up->render.width && mode > VK_SL_MODE_DLAA && FitWidth (feature, mode - 1, up, &w2, &range2) &&
	    abs ((int)w2 - (int)up->render.width) < abs ((int)w - (int)up->render.width))
	{
		mode--;
		w = w2;
		range = range2;
	}
	chosen_mode = mode;
	chosen_range = range;
	h = q_max (range.min_height, q_min (range.max_height, up->render.height));
	up->render.width = q_max (2u, q_min (w, up->unscaled.width));
	up->render.height = q_max (1u, q_min (h, up->unscaled.height));
	up->dlss = feature;
	up->dlss_mode = mode;
	return true;
}

static void SLImage (vk_sl_image_t *img, int index)
{
	VK_ImageInfo (index, &img->image, &img->view, &img->format, &img->width, &img->height);
}

/* Streamline records its own passes: everything before is done, and
 * everything after waits for them */
static void AllCommandsBarrier (VkCommandBuffer cmd)
{
	VkMemoryBarrier2	barrier;
	VkDependencyInfo	dep;

	memset (&barrier, 0, sizeof(barrier));
	barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
	barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
	barrier.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
	barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
	barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
	memset (&dep, 0, sizeof(dep));
	dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
	dep.memoryBarrierCount = 1;
	dep.pMemoryBarriers = &barrier;
	vkCmdPipelineBarrier2 (cmd, &dep);
}

/* after the interleave, instead of the TAA pass: the lit image (FLAT_COLOR)
 * and the inputs into TAA_OUTPUT at the view's size; false if it failed */
qboolean VK_DLSSRun (VkCommandBuffer cmd, const vk_upscale_t *up)
{
	const QVKUniformBuffer_t	*u = VK_CurrentUBO ();
	vk_sl_frame_t			f;
	qboolean			ok;

	memset (&f, 0, sizeof(f));
	f.feature = up->dlss;
	f.mode = up->dlss_mode;
	f.preset = Preset ();
	f.frame = vk_render_frame;
	f.render_width = up->render.width;
	f.render_height = up->render.height;
	f.output_width = up->unscaled.width;
	f.output_height = up->unscaled.height;
	f.jitter[0] = -up->jitter[0];	/* DLSS's sign (measured, R53) */
	f.jitter[1] = -up->jitter[1];
	f.mvec_scale[0] = f.mvec_scale[1] = 1.0f;	/* ours are in UV units */
	memcpy (f.V, &u->V[0][0], sizeof(f.V));
	memcpy (f.invV, &u->invV[0][0], sizeof(f.invV));
	memcpy (f.P, &u->P[0][0], sizeof(f.P));
	memcpy (f.V_prev, &u->V_prev[0][0], sizeof(f.V_prev));
	memcpy (f.P_prev, &u->P_prev[0][0], sizeof(f.P_prev));
	/* the depth row of a D3D-style projection (0 at the near plane, 1 at
	 * the far), which dlss_inputs.glsl's hardware depth follows */
	f.P[10] = f.P_prev[10] = (float)(DLSS_Z_FAR / (DLSS_Z_FAR - DLSS_Z_NEAR));
	f.P[14] = f.P_prev[14] = (float)(-DLSS_Z_FAR * DLSS_Z_NEAR / (DLSS_Z_FAR - DLSS_Z_NEAR));
	VK_InverseMatrix (f.P, f.invP);
	VectorCopy (r_scene.vieworg, f.cam_pos);
	VectorCopy (r_scene.forward, f.cam_fwd);
	VectorCopy (r_scene.right, f.cam_right);
	VectorCopy (r_scene.up, f.cam_up);
	f.znear = (float)DLSS_Z_NEAR;
	f.zfar = (float)DLSS_Z_FAR;
	f.fov_y = r_scene.fov_y * (float)M_PI / 180.0f;
	f.aspect = (float)up->unscaled.width / (float)up->unscaled.height;
	f.pre_exposure = 1.0f;
	f.reset = !history || last_feature != up->dlss || last_mode != up->dlss_mode ||
		  last_render.width != up->render.width || last_render.height != up->render.height ||
		  last_output.width != up->unscaled.width || last_output.height != up->unscaled.height;
	SLImage (&f.color_in, VKPT_IMG_FLAT_COLOR);
	SLImage (&f.color_out, VKPT_IMG_TAA_OUTPUT);
	SLImage (&f.depth, VKPT_IMG_DLSS_DEPTH);
	SLImage (&f.mvec, VKPT_IMG_FLAT_MOTION);
	if (up->dlss == VK_SL_RR)
	{
		SLImage (&f.albedo, VKPT_IMG_DLSS_ALBEDO);
		SLImage (&f.spec_albedo, VKPT_IMG_DLSS_SPEC_ALBEDO);
		SLImage (&f.normal_roughness, VKPT_IMG_DLSS_NORMAL_ROUGHNESS);
		SLImage (&f.spec_hit, VKPT_IMG_DLSS_SPEC_HIT);
	}

	AllCommandsBarrier (cmd);
	ok = VK_SLEvaluate (cmd, &f) != 0;
	AllCommandsBarrier (cmd);
	resources[up->dlss] = true;	/* also when it failed after Streamline made them */
	if (!ok)
	{
		failed[up->dlss] = true;
		failed_percent[up->dlss] = chosen_percent;
		failed_size[up->dlss] = up->unscaled;
	}
	last_feature = up->dlss;
	last_mode = up->dlss_mode;
	last_render = up->render;
	last_output = up->unscaled;
	return ok;
}

/* at the end of a 3D frame: whether DLSS ran, so the next frame continues
 * its history */
void VK_EndDLSSFrame (qboolean ran)
{
	history = ran;
}

/* VK_ResetDenoiserHistory's events */
void VK_ResetDLSSHistory (void)
{
	history = false;
}

/* VK_BeginFrame, outside frames: the render targets follow the chosen
 * feature's images; a feature no longer chosen frees its resources */
void VK_DLSSBetweenFrames (void)
{
	int	chosen = VK_UpscaleDLSSFeature ();
	int	i;

	if (VK_ImagesReady () && VK_DLSSImagesWanted () != vk_dlss_images)
		VK_SwapchainRecreated ();	/* with the device idle */
	for (i = VK_SL_SR; i <= VK_SL_RR; i++)
	{
		if (!resources[i] || chosen == i)
			continue;
		vkDeviceWaitIdle (vk.device);
		VK_SLFreeResources (i);
		resources[i] = false;
	}
}

static void VK_DLSS_f (void)
{
	int		feature = VK_UpscaleDLSSFeature ();
	const char	*why;

	VK_SLStatus ();
	Con_Printf ("images: %s; r_dlss_preset %s (%s)\n",
		    (vk_dlss_images == VK_SL_RR) ? "depth and RR's guides" : (vk_dlss_images == VK_SL_SR) ? "depth" : "none",
		    r_dlss_preset.string, Preset () ? "pinned" : "DLSS's default");
	if (!feature)
	{
		Con_Printf ("r_upscaler %s: no DLSS (3 DLSS SR, 4 DLSS RR)\n", Cvar_VariableString ("r_upscaler"));
		return;
	}
	why = VK_DLSSUnavailable (feature);
	if (why)
		Con_Printf ("DLSS %s: TAAU instead, %s\n", (feature == VK_SL_RR) ? "RR" : "SR", why);
	else if (history)
		Con_Printf ("DLSS %s: running, %s, render %u x %u, output %u x %u\n", (feature == VK_SL_RR) ? "RR" : "SR",
			    mode_names[last_mode], last_render.width, last_render.height, last_output.width, last_output.height);
	else
		Con_Printf ("DLSS %s: chosen, not running (no 3D view, or a debug view)\n", (feature == VK_SL_RR) ? "RR" : "SR");
	if (chosen_mode >= 0)
		Con_Printf ("%s's render sizes at %u x %u: optimal %u x %u, %u x %u to %u x %u\n", mode_names[chosen_mode],
			    last_output.width, last_output.height, chosen_range.optimal_width, chosen_range.optimal_height,
			    chosen_range.min_width, chosen_range.min_height, chosen_range.max_width, chosen_range.max_height);
}

void VK_InitDLSS (void)
{
	Cvar_RegisterVariable (&r_dlss_preset);
	Cmd_AddCommand ("vk_dlss", VK_DLSS_f);
}

void VK_ShutdownDLSS (void)
{
	memset (resources, 0, sizeof(resources));
	memset (failed, 0, sizeof(failed));
	memset (range_cache, 0, sizeof(range_cache));
	chosen_mode = -1;
	history = false;
}

/* r_upscaler's callback (vk_upscale.c): a failed feature gets another try */
void VK_DLSSUpscalerChanged (void)
{
	memset (failed, 0, sizeof(failed));
}
