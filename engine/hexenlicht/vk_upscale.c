/* vk_upscale.c -- the upscaler interface: resolution scale, TAA/TAAU and
 * AMD FSR 1
 *
 * Quake II RTX's get_render_extent, evaluate_taa_settings and TAA jitter
 * (main.c), vkpt_taa (asvgf.c) and fsr.c. Once per 3D frame,
 * VK_UpscaleEvaluate decides for the view:
 *  - the render size: the view (its width rounded up to even: "unscaled",
 *    the upscalers' output) times r_scale (25-100 %, Quake II RTX's
 *    scr_viewsize), the width rounded up to even for the two checkerboard
 *    fields. The render targets keep the swapchain's size, enough up to
 *    100 %, so a change needs no new images;
 *  - the TAA pass (asvgf_taau.comp, only for the lit image, with the
 *    denoiser as in Quake II RTX): VK_UpscaleHDR runs it after the lighting,
 *    before bloom and tone mapping, from FLAT_COLOR into TAA_OUTPUT
 *    (blending with its history, ASVGF_TAA_B; a copy without history or
 *    without the denoiser). r_upscaler picks what it does:
 *      0 TAA: at the render size, primary rays through the pixel centers;
 *      1 TAAU (Quake II RTX's flt_taa 2): the primary rays jittered (Halton
 *        2/3, Quake II RTX's 128 samples), upsampled to the view's size (at
 *        100 %: jittered TAA);
 *      2 FSR 1: TAAU's jittered TAA at the render size, then after tone
 *        mapping VK_UpscaleDisplay's EASU upscale and RCAS sharpening
 *        (fsr_easu_fp32.comp, fsr_rcas_fp32.comp; flt_fsr_easu, _rcas,
 *        _sharpness); only below 100 % (Quake II RTX's flt_fsr_enable 1)
 *        and with tone mapping (FSR takes the tone-mapped image), else TAAU;
 *  - what the composite (vk_view.c) shows: TAA_OUTPUT or FSR's output, and
 *    how it scales it to the view.
 * Quake II RTX's AA_MODE_TAA (flt_taa 1) also moves each primary ray to a
 * random point in its pixel; the TAA here keeps the pixel centers (the
 * crisp look closest to GL), so every mode gives the UBO AA_MODE_UPSCALE
 * (the TAA shader blends alike in both), with the jitter and output size
 * deciding. DLSS (3.10) replaces the TAA pass. The debug views skip it:
 * no jitter, at the render size, scaled by the composite.
 *
 * Copyright (C) 2018 Christoph Schied
 * Copyright (C) 2019, NVIDIA CORPORATION. All rights reserved.
 * Copyright (C) 2021, Frank Richter. All rights reserved.
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
#include "shaders/global_textures.h"

/* AMD's FSR 1 headers (libs/fsr1) for FsrEasuCon and FsrRcasCon */
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4505)	/* unreferenced static functions */
#endif
#define A_CPU
#include "ffx_a.h"
#include "ffx_fsr1.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#ifdef HEXENLICHT_STREAMLINE
#include "vk_streamline.h"	/* SPIKE (3.9) */
#endif

#define NUM_TAA_SAMPLES	128	/* Quake II RTX's */

enum { UPSCALER_TAA, UPSCALER_TAAU, UPSCALER_FSR, UPSCALER_DLSS_SR, UPSCALER_DLSS_RR };

/* SPIKE (3.9): RR's checkerboard fields swap every frame (Quake II RTX's
 * noisy and reference modes) */
static cvar_t	r_dlss_swap = {"r_dlss_swap", "0", CVAR_NONE};
/* SPIKE (3.9): RR and the checkerboard fields at translucent surfaces: 1 the
 * interleave blurs them (as with the denoiser), 2 the guides too */
static cvar_t	r_dlss_cb = {"r_dlss_cb", "0", CVAR_NONE};

/* the render size in percent of the view's: 25-100 */
static cvar_t	r_scale = {"r_scale", "100", CVAR_ARCHIVE};
/* 0 TAA, 1 TAAU, 2 FSR 1 */
static cvar_t	r_upscaler = {"r_upscaler", "1", CVAR_ARCHIVE};
/* Quake II RTX's FSR cvars (fsr.c): its steps, and RCAS's sharpness, 0-2
 * (0 the sharpest; AMD's recommended 0.2) */
static cvar_t	flt_fsr_easu = {"flt_fsr_easu", "1", CVAR_ARCHIVE};
static cvar_t	flt_fsr_rcas = {"flt_fsr_rcas", "1", CVAR_ARCHIVE};
static cvar_t	flt_fsr_sharpness = {"flt_fsr_sharpness", "0.2", CVAR_ARCHIVE};

static float		taa_samples[NUM_TAA_SAMPLES][2];	/* sub-pixel offsets, -0.5..0.5 */
static vk_upscale_t	up;		/* this frame's */
static qboolean		taa_history;	/* the last 3D frame ran the TAA pass: ASVGF_TAA_B holds its output */

static VkPipeline	taa_pipeline;		/* VK_PathTracerLayout () */
static VkPipeline	easu_pipeline;
static VkPipeline	rcas_pipelines[2];	/* after EASU, after TAAU */

const vk_upscale_t *VK_Upscale (void)
{
	return &up;
}

static qboolean ExtentsEqual (VkExtent2D a, VkExtent2D b)
{
	return a.width == b.width && a.height == b.height;
}

/* Quake II RTX's halton */
static float Halton (int base, int index)
{
	float	f = 1.0f, r = 0.0f;
	int	i = index;

	while (i > 0)
	{
		f = f / (float)base;
		r = r + f * (float)(i % base);
		i = i / base;
	}
	return r;
}

const vk_upscale_t *VK_UpscaleEvaluate (uint32_t view_width, uint32_t view_height, int debug_view)
{
	float		scale = (float)q_max (25, q_min (100, r_scale.integer)) / 100.0f;
	int		upscaler = q_max (UPSCALER_TAA, q_min (UPSCALER_DLSS_RR, r_upscaler.integer));
	qboolean	lit = debug_view == DEBUGVIEW_LIT;
	qboolean	denoise = VK_DenoiserEnabled ();
	qboolean	easu = flt_fsr_easu.integer != 0, rcas = flt_fsr_rcas.integer != 0;
	qboolean	fsr, upscaling = false;

	memset (&up, 0, sizeof(up));
	up.view.width = view_width;
	up.view.height = view_height;
	up.unscaled.width = (view_width + 1) & ~1u;
	up.unscaled.height = view_height;
	/* Quake II RTX's get_render_extent */
	up.render.width = ((uint32_t)((float)up.unscaled.width * scale) + 1) & ~1u;
	up.render.height = (uint32_t)((float)up.unscaled.height * scale);
	up.render.width = q_max (up.render.width, 2u);
	up.render.height = q_max (up.render.height, 1u);
	up.taa_output = up.render;

#ifdef HEXENLICHT_STREAMLINE
	/* SPIKE (3.9): DLSS SR (the denoised image) or RR (the noisy one, no
	 * denoiser) instead of the TAA pass, jittered as TAAU, into
	 * TAA_OUTPUT at the view's size */
	if (lit && upscaler >= UPSCALER_DLSS_SR && VK_SLSupported (upscaler - UPSCALER_DLSS_SR + VK_SL_SR))
	{
		int	i = (int)((vk_render_frame + 1) % NUM_TAA_SAMPLES);

		up.dlss = upscaler - UPSCALER_DLSS_SR + VK_SL_SR;
		up.swap_checkerboard = up.dlss == VK_SL_RR && r_dlss_swap.integer;
		up.rr_blur = (up.dlss == VK_SL_RR) ? q_max (0, q_min (2, r_dlss_cb.integer)) : 0;
		up.jitter[0] = taa_samples[i][0];
		up.jitter[1] = taa_samples[i][1];
		up.taa_output = up.unscaled;
		up.lod_bias = log2f (scale);
		up.display_size = up.unscaled;
		return &up;
	}
#endif
	if (upscaler > UPSCALER_FSR)
		upscaler = UPSCALER_TAAU;	/* no DLSS */

	/* Quake II RTX's vkpt_fsr_is_enabled (flt_fsr_enable 1: only when it
	 * upscales) and evaluate_taa_settings; RCAS alone needs TAAU's
	 * upscaled image, so the denoiser; FSR takes the tone-mapped image */
	fsr = lit && upscaler == UPSCALER_FSR && (easu || rcas) && VK_ToneMappingEnabled () &&
	      up.render.width < up.unscaled.width && up.render.height < up.unscaled.height &&
	      (easu || denoise);
	up.taa = lit;
	if (lit && denoise)
	{
		up.taa_mode = AA_MODE_UPSCALE;	/* the blend; see the top */
		if (upscaler != UPSCALER_TAA)	/* TAAU, FSR (TAAU where FSR doesn't run) */
		{
			int	i = (int)((vk_render_frame + 1) % NUM_TAA_SAMPLES);	/* the frame VK_PrepareUBO numbers next */

			upscaling = true;
			up.jitter[0] = taa_samples[i][0];
			up.jitter[1] = taa_samples[i][1];
			if (!fsr || !easu)
				up.taa_output = up.unscaled;
		}
		/* Quake II RTX's temporal_frame_valid: the denoiser's history,
		 * and the TAA's (the last 3D frame ran the pass); else a copy */
		if (!taa_history || !VK_DenoiserHistoryValid ())
			up.taa_mode = AA_MODE_OFF;
	}
	up.fsr_easu = fsr && easu;
	up.fsr_rcas = fsr && rcas;
	/* Quake II RTX's LOD bias for upscaled images, textures as sharp as the
	 * view: with the denoiser (FSR without it gets none, as in Quake II RTX) */
	if (upscaling)
		up.lod_bias = log2f (scale);
	if (fsr)
	{
		FsrEasuCon (up.easu_const[0], up.easu_const[1], up.easu_const[2], up.easu_const[3],
			    (AF1)up.render.width, (AF1)up.render.height,		/* the TAA output */
			    (AF1)vk_image_extent.width, (AF1)vk_image_extent.height,	/* its image */
			    (AF1)up.unscaled.width, (AF1)up.unscaled.height);		/* the output */
		FsrRcasCon (up.rcas_const, q_max (0.0f, q_min (2.0f, flt_fsr_sharpness.value)));
	}

	/* the composite: FSR's output at the view's size, else TAA_OUTPUT;
	 * scaled as Quake II RTX's final blit (nearest at the same size and
	 * at exactly half, Lanczos otherwise), the debug views nearest */
	if (up.fsr_rcas || up.fsr_easu)
	{
		up.display_source = up.fsr_rcas ? 2 : 1;
		up.display_size = up.unscaled;
	}
	else
	{
		up.display_source = 0;
		up.display_size = up.taa_output;
	}
	up.display_lanczos = lit && !ExtentsEqual (up.display_size, up.unscaled) &&
			     !(up.display_size.width * 2 == up.unscaled.width && up.display_size.height * 2 == up.unscaled.height);
	return &up;
}

static void CreatePipelines (void)
{
	/* the FSR shaders' specialization constants: spec_hdr (0: SDR, the
	 * only output until 7.3), then EASU's spec_output_display (its
	 * transform is HDR's only) or RCAS's spec_input_tex */
	uint32_t	spec[2];

	if (!taa_pipeline)
		taa_pipeline = VK_CreateComputePipeline ("asvgf_taau.comp", VK_PathTracerLayout ());
	if (!easu_pipeline)
	{
		spec[0] = 0;
		spec[1] = 0;
		easu_pipeline = VK_CreateComputePipelineSpecs ("fsr_easu_fp32.comp", VK_PathTracerLayout (), spec, 2);
	}
	if (!rcas_pipelines[0])
	{
		spec[0] = 0;
		spec[1] = 0;
		rcas_pipelines[0] = VK_CreateComputePipelineSpecs ("fsr_rcas_fp32.comp", VK_PathTracerLayout (), spec, 2);
		spec[1] = 1;
		rcas_pipelines[1] = VK_CreateComputePipelineSpecs ("fsr_rcas_fp32.comp", VK_PathTracerLayout (), spec, 2);
	}
}

void VK_DestroyUpscalePipelines (void)
{
	int	i;

	if (taa_pipeline)
		vkDestroyPipeline (vk.device, taa_pipeline, NULL);
	if (easu_pipeline)
		vkDestroyPipeline (vk.device, easu_pipeline, NULL);
	for (i = 0; i < 2; i++)
	{
		if (rcas_pipelines[i])
			vkDestroyPipeline (vk.device, rcas_pipelines[i], NULL);
		rcas_pipelines[i] = VK_NULL_HANDLE;
	}
	taa_pipeline = easu_pipeline = VK_NULL_HANDLE;
}

/* Quake II RTX's vkpt_taa: the lit image (FLAT_COLOR) into TAA_OUTPUT and
 * the history (ASVGF_TAA_A), after the lighting passes and the interleave,
 * before bloom and tone mapping; DLSS SR/RR replace it (3.10) */
void VK_UpscaleHDR (VkCommandBuffer cmd)
{
	uint32_t	w = up.taa_output.width, h = up.taa_output.height;

	if (!up.taa)
		return;
	CreatePipelines ();
	/* Quake II RTX's dispatch: 8 more pixels, zeroed, unless that's past the images */
	if (w < vk_image_extent.width)
		w += 8;
	if (h < vk_image_extent.height)
		h += 8;
	VK_DispatchCompute (cmd, taa_pipeline, w, h, 16);
	VK_ComputeBarrier (cmd);
}

/* Quake II RTX's vkpt_fsr_do: EASU and RCAS over the view, after tone
 * mapping */
void VK_UpscaleDisplay (VkCommandBuffer cmd)
{
	if (!up.fsr_easu && !up.fsr_rcas)
		return;
	CreatePipelines ();
	/* 16x16 pixels per group of 64 threads (4 each), as AMD's integration
	 * guide dispatches them */
	if (up.fsr_easu)
	{
		VK_DispatchCompute (cmd, easu_pipeline, up.unscaled.width, up.unscaled.height, 16);
		VK_ComputeBarrier (cmd);
	}
	if (up.fsr_rcas)
	{
		VK_DispatchCompute (cmd, rcas_pipelines[up.fsr_easu ? 0 : 1], up.unscaled.width, up.unscaled.height, 16);
		VK_ComputeBarrier (cmd);
	}
}

/* after VK_RenderView3D's passes: whether the next frame has TAA history */
void VK_EndUpscaleFrame (void)
{
	taa_history = up.taa;
}

static void VK_Upscale_f (void)
{
	static const char	*names[] = { "TAA", "TAAU", "FSR 1", "DLSS SR", "DLSS RR" };
	const char		*pass;

	if (!up.view.width)
	{
		Con_Printf ("No 3D view rendered yet\n");
		return;
	}
	pass = up.dlss ? ((up.dlss == 2) ? "DLSS RR (spike)" : "DLSS SR (spike)") : !up.taa ? "none (a debug view)" : (up.taa_mode == AA_MODE_OFF) ? "copy (no history, or no denoiser)" : "TAA";
	Con_Printf ("r_upscaler %d (%s), r_scale %d: view %u x %u, render %u x %u\n",
		    r_upscaler.integer, names[q_max (0, q_min (4, r_upscaler.integer))], r_scale.integer,
		    up.view.width, up.view.height, up.render.width, up.render.height);
	Con_Printf ("TAA pass: %s, output %u x %u, jitter %.3f %.3f, LOD bias %.2f\n",
		    pass, up.taa_output.width, up.taa_output.height, up.jitter[0], up.jitter[1], up.lod_bias);
	Con_Printf ("FSR: %s; shown: %s %u x %u, %s\n",
		    (up.fsr_easu && up.fsr_rcas) ? "EASU + RCAS" : up.fsr_easu ? "EASU" : up.fsr_rcas ? "RCAS" : "off",
		    (up.display_source == 2) ? "FSR_RCAS_OUTPUT" : (up.display_source == 1) ? "FSR_EASU_OUTPUT" : "TAA_OUTPUT",
		    up.display_size.width, up.display_size.height,
		    up.display_lanczos ? "Lanczos" : ExtentsEqual (up.display_size, up.unscaled) ? "1:1" : "nearest");
}

void VK_InitUpscale (void)
{
	int	i;

	Cvar_RegisterVariable (&r_scale);
	Cvar_RegisterVariable (&r_upscaler);
	Cvar_RegisterVariable (&flt_fsr_easu);
	Cvar_RegisterVariable (&flt_fsr_rcas);
	Cvar_RegisterVariable (&flt_fsr_sharpness);
	Cvar_RegisterVariable (&r_dlss_swap);
	Cvar_RegisterVariable (&r_dlss_cb);
	Cmd_AddCommand ("vk_upscale", VK_Upscale_f);
	for (i = 0; i < NUM_TAA_SAMPLES; i++)
	{
		taa_samples[i][0] = Halton (2, i + 1) - 0.5f;
		taa_samples[i][1] = Halton (3, i + 1) - 0.5f;
	}
}

void VK_ShutdownUpscale (void)
{
	VK_DestroyUpscalePipelines ();
	memset (&up, 0, sizeof(up));
	taa_history = false;
}
