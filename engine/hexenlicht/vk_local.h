/* vk_local.h -- Hexenlicht's Vulkan state (vk_core.c, vk_swapchain.c)
 *
 * Copyright (C) 2026  Hexenlicht contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HEXENLICHT_VK_LOCAL_H
#define HEXENLICHT_VK_LOCAL_H

#include "volk.h"
#include "vk_mem_alloc.h"

#define VK_FRAMES_IN_FLIGHT	2
#define VK_MAX_SWAPCHAIN_IMAGES	8
#define VK_MAX_TEXTURES		4096	/* slots in the bindless texture array (power of 2) */

typedef struct
{
	VkCommandPool	cmd_pool;
	VkCommandBuffer	cmd;
	VkFence		fence;			/* signaled when the frame's commands finished */
	VkSemaphore	image_available;	/* signaled by vkAcquireNextImageKHR */
} vk_frame_t;

typedef struct
{
	/* instance and device (vk_core.c) */
	VkInstance		instance;
	VkDebugUtilsMessengerEXT messenger;
	VkSurfaceKHR		surface;
	VkPhysicalDevice	physical_device;
	VkPhysicalDeviceProperties props;
	VkDevice		device;
	uint32_t		queue_family;	/* graphics + compute + present */
	VkQueue			queue;
	VmaAllocator		allocator;

	qboolean		validation;	/* validation layer enabled */
	int			validation_errors, validation_warnings;

	/* optional ray tracing extensions that were found and enabled */
	qboolean		have_rt_pipeline;	/* VK_KHR_ray_tracing_pipeline */
	qboolean		have_ser;		/* VK_NV_ray_tracing_invocation_reorder */
	qboolean		have_position_fetch;	/* VK_KHR_ray_tracing_position_fetch */

	/* swapchain (vk_swapchain.c) */
	VkSwapchainKHR		swapchain;
	VkSurfaceFormatKHR	surface_format;
	VkPresentModeKHR	present_mode;
	VkExtent2D		extent;
	uint32_t		num_images;
	VkImage			images[VK_MAX_SWAPCHAIN_IMAGES];
	VkImageView		views[VK_MAX_SWAPCHAIN_IMAGES];
	VkSemaphore		render_finished[VK_MAX_SWAPCHAIN_IMAGES];	/* per image */
	qboolean		swapchain_dirty;	/* recreate before the next frame */

	/* textures (vk_texture.c): one bindless array of combined image
	 * samplers, indexed by the numbers GL_LoadTexture returns */
	VkDescriptorSetLayout	texture_set_layout;
	VkDescriptorSet		texture_set;

	/* frames */
	vk_frame_t		frames[VK_FRAMES_IN_FLIGHT];
	uint32_t		frame_index;	/* slot in frames[] */
	uint32_t		image_index;	/* swapchain image of the current frame */
	VkImageLayout		image_layout;	/* its current layout */
	qboolean		frame_active;	/* between VK_BeginFrame and VK_EndFrame */
	uint64_t		frame_count;
} vk_state_t;

extern vk_state_t	vk;

const char *VK_ResultString (VkResult result);

/* for calls that must not fail */
#define VK_CHECK(call)								\
	do {									\
		VkResult vk_check_result_ = (call);				\
		if (vk_check_result_ != VK_SUCCESS)				\
			Sys_Error ("%s failed: %s", #call,			\
					VK_ResultString (vk_check_result_));	\
	} while (0)

/* vk_core.c */
void VK_Init (HINSTANCE hinstance, HWND hwnd);
void VK_Shutdown (void);

/* vk_swapchain.c */
void VK_InitSwapchain (void);
void VK_ShutdownSwapchain (void);
void VK_SwapchainChanged (void);	/* window size or present settings changed */

/* A frame: VK_BeginFrame acquires a swapchain image and starts recording
 * (returns false if there is nothing to draw to, e.g. minimized);
 * VK_EndFrame submits and presents. */
qboolean VK_BeginFrame (void);
void VK_ClearScreen (float r, float g, float b);
void VK_BeginSwapchainRendering (VkAttachmentLoadOp load_op);
void VK_EndSwapchainRendering (void);
void VK_EndFrame (void);

/* vk_texture.c: GL_LoadTexture (declared in glquake.h) returns the slot in
 * vk.texture_set; slot 0 is a 1x1 white texture */
void VK_InitTextures (void);
void VK_ShutdownTextures (void);

/* vk_shader.c: loads <exe folder>\shaders\<name>.spv, e.g. "fullscreen.vert" */
VkShaderModule VK_LoadShader (const char *name);

/* vk_testpattern.c: placeholder screen until the 2D renderer (story 1.6) */
void VK_DrawTestPattern (float time);
void VK_ShutdownTestPattern (void);

#endif	/* HEXENLICHT_VK_LOCAL_H */
