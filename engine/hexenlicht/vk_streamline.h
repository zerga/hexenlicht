/* vk_streamline.h -- SPIKE (story 3.9, not for merging): the C interface of
 * vk_streamline.cpp, NVIDIA Streamline's DLSS SR and RR loaded at runtime
 * from sl.interposer.dll next to the exe (the player's DLLs).
 *
 * Copyright (C) 2026  Hexenlicht contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HEXENLICHT_VK_STREAMLINE_H
#define HEXENLICHT_VK_STREAMLINE_H

#ifdef __cplusplus
extern "C" {
#endif

enum { VK_SL_OFF, VK_SL_SR, VK_SL_RR };

typedef struct
{
	VkImage		image;
	VkImageView	view;
	VkFormat	format;
	uint32_t	width, height;		/* the image's */
} vk_sl_image_t;

typedef struct
{
	int		mode;			/* VK_SL_SR, VK_SL_RR */
	uint32_t	frame;			/* the 3D frame's number */
	uint32_t	render_width, render_height;
	uint32_t	output_width, output_height;
	float		jitter[2];		/* pixels */
	float		mvec_scale[2];
	float		V[16], invV[16], P[16], invP[16];	/* column-major, jitter-free */
	float		V_prev[16], P_prev[16];
	float		cam_pos[3], cam_fwd[3], cam_right[3], cam_up[3];
	float		znear, zfar, fov_y, aspect;
	float		pre_exposure;
	int		reset;
	vk_sl_image_t	color_in, color_out, depth, mvec;
	vk_sl_image_t	albedo, spec_albedo, normal_roughness, spec_hit;
} vk_sl_frame_t;

PFN_vkGetInstanceProcAddr VK_SLPreInit (const char *exe_dir);	/* before the instance; NULL: no Streamline */
void VK_SLDeviceReady (VkPhysicalDevice physical_device);
int VK_SLSupported (int mode);
int VK_SLEvaluate (VkCommandBuffer cmd, const vk_sl_frame_t *f);	/* 1 if evaluated */
void VK_SLShutdown (void);	/* before the device and instance are destroyed */
void VK_SLStatus (void);	/* for the console command, outside frames */

#ifdef __cplusplus
}
#endif

#endif	/* HEXENLICHT_VK_STREAMLINE_H */
