/* vk_streamline.h -- the C interface of vk_streamline.cpp: NVIDIA
 * Streamline's DLSS Super Resolution and Ray Reconstruction, from the
 * player's sl.interposer.dll next to the exe (vk_dlss.c uses it)
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

#ifndef HEXENLICHT_VK_STREAMLINE_H
#define HEXENLICHT_VK_STREAMLINE_H

#ifdef __cplusplus
extern "C" {
#endif

/* the features */
enum { VK_SL_OFF, VK_SL_SR, VK_SL_RR };

/* DLSS's modes, which pick its model and the render sizes it accepts */
enum { VK_SL_MODE_DLAA, VK_SL_MODE_QUALITY, VK_SL_MODE_BALANCED, VK_SL_MODE_PERFORMANCE,
       VK_SL_MODE_ULTRA_PERFORMANCE, VK_SL_NUM_MODES };

typedef struct
{
	VkImage		image;
	VkImageView	view;
	VkFormat	format;
	uint32_t	width, height;		/* the image's */
} vk_sl_image_t;

/* render sizes for a mode and output size (DLSS's optimal settings) */
typedef struct
{
	uint32_t	optimal_width, optimal_height;
	uint32_t	min_width, min_height;
	uint32_t	max_width, max_height;
} vk_sl_range_t;

typedef struct
{
	int		feature;		/* VK_SL_SR, VK_SL_RR */
	int		mode;			/* VK_SL_MODE_* */
	int		preset;			/* 0 DLSS's default, 1-15 its presets A-O */
	uint32_t	frame;			/* the 3D frame's number */
	uint32_t	render_width, render_height;
	uint32_t	output_width, output_height;
	float		jitter[2];		/* pixels, DLSS's sign */
	float		mvec_scale[2];
	/* column-major (Streamline's row-major with row vectors), jitter-free */
	float		V[16], invV[16], P[16], invP[16];
	float		V_prev[16], P_prev[16];
	float		cam_pos[3], cam_fwd[3], cam_right[3], cam_up[3];
	float		znear, zfar, fov_y, aspect;
	float		pre_exposure;
	int		reset;
	vk_sl_image_t	color_in, color_out, depth, mvec;
	vk_sl_image_t	albedo, spec_albedo, normal_roughness, spec_hit;	/* RR's */
} vk_sl_frame_t;

/* before the instance: the interposer's vkGetInstanceProcAddr, NULL
 * without Streamline (no DLL, a bad signature, slInit failed) */
PFN_vkGetInstanceProcAddr VK_SLPreInit (const char *exe_dir);
int VK_SLActive (void);			/* Streamline is initialized (the device enables privateData) */
void VK_SLVulkanFailed (void);		/* the instance or device failed through the interposer: Streamline shut down */
const char *VK_SLInactiveReason (void);	/* why not */
void VK_SLDeviceReady (VkPhysicalDevice physical_device);	/* after the device */
int VK_SLSupported (int feature);
int VK_SLRange (int feature, int mode, uint32_t output_width, uint32_t output_height, vk_sl_range_t *range);	/* 1 if known */
int VK_SLEvaluate (VkCommandBuffer cmd, const vk_sl_frame_t *f);	/* 1 if evaluated */
void VK_SLFreeResources (int feature);	/* with the device idle */
void VK_SLShutdown (void);		/* before the device and the instance go */
void VK_SLStatus (void);		/* vk_dlss's report, outside frames */
const char *VK_SLStartupMessage (void);	/* for the console after VK_Init */

#ifdef __cplusplus
}
#endif

#endif	/* HEXENLICHT_VK_STREAMLINE_H */
