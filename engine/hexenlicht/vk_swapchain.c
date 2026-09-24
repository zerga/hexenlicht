/* vk_swapchain.c -- swapchain and frame pacing for Hexenlicht
 *
 * Two frames in flight. Each frame slot has its own command pool, fence
 * and "image available" semaphore; the "render finished" semaphores used
 * for presenting belong to the swapchain images, since presentation may
 * still use one after its frame's fence signaled.
 *
 * The swapchain is recreated lazily (before the next frame) whenever the
 * window size changes, the present mode setting changes, or presenting
 * reports it out of date. Nothing is drawn while the window has no area
 * (minimized).
 *
 * The swapchain format is B8G8R8A8_UNORM with sRGB color space: the final
 * pass of the renderer writes already sRGB-encoded values.
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
#include "winquake.h"
#include "vk_local.h"
#include "vid_vk.h"

/* 1 = wait for vertical blank (FIFO); 0 = don't (MAILBOX, else IMMEDIATE) */
static cvar_t	vid_vsync = {"vid_vsync", "1", CVAR_ARCHIVE};

static void VK_VsyncChanged (cvar_t *var)
{
	(void)var;
	VK_SwapchainChanged ();
}

void VK_SwapchainChanged (void)
{
	vk.swapchain_dirty = true;
}


/* ==========================================================================
 * Screenshots
 * ========================================================================== */

static void VK_TransitionImage (VkImageLayout new_layout,
				VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
				VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access);

static char		screenshot_name[MAX_OSPATH];	/* pending request, empty if none */
static VkBuffer		screenshot_buffer;
static VmaAllocation	screenshot_allocation;
static VkDeviceSize	screenshot_size;

void VK_RequestScreenshot (const char *filename)
{
	q_strlcpy (screenshot_name, filename, sizeof(screenshot_name));
}

/* copy the current swapchain image into the readback buffer */
static void VK_RecordScreenshotCopy (VkCommandBuffer cmd)
{
	VkBufferCreateInfo	info;
	VmaAllocationCreateInfo	alloc;
	VkBufferImageCopy	copy;
	VkDeviceSize		size = (VkDeviceSize)vk.extent.width * vk.extent.height * 4;

	if (screenshot_size < size)
	{
		if (screenshot_buffer)
			vmaDestroyBuffer (vk.allocator, screenshot_buffer, screenshot_allocation);
		memset (&info, 0, sizeof(info));
		info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
		info.size = size;
		info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
		memset (&alloc, 0, sizeof(alloc));
		alloc.usage = VMA_MEMORY_USAGE_AUTO;
		alloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
		VK_CHECK (vmaCreateBuffer (vk.allocator, &info, &alloc, &screenshot_buffer, &screenshot_allocation, NULL));
		screenshot_size = size;
	}

	VK_TransitionImage (VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
			VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
	memset (&copy, 0, sizeof(copy));
	copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	copy.imageSubresource.layerCount = 1;
	copy.imageExtent.width = vk.extent.width;
	copy.imageExtent.height = vk.extent.height;
	copy.imageExtent.depth = 1;
	vkCmdCopyImageToBuffer (cmd, vk.images[vk.image_index], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				screenshot_buffer, 1, &copy);
}

/* after the frame finished: write the readback buffer as a 24-bit TGA */
static void VK_WriteScreenshot (VkFence fence, const char *filename)
{
	int		w = (int)vk.extent.width, h = (int)vk.extent.height;
	int		x, y, size = w * h * 3 + 18;
	qboolean	bgra = (vk.surface_format.format == VK_FORMAT_B8G8R8A8_UNORM ||
				vk.surface_format.format == VK_FORMAT_B8G8R8A8_SRGB);
	byte		*tga, *out;
	const byte	*pixels, *in;

	VK_CHECK (vkWaitForFences (vk.device, 1, &fence, VK_TRUE, UINT64_MAX));
	VK_CHECK (vmaMapMemory (vk.allocator, screenshot_allocation, (void **)&pixels));
	VK_CHECK (vmaInvalidateAllocation (vk.allocator, screenshot_allocation, 0, VK_WHOLE_SIZE));

	tga = (byte *) malloc (size);
	if (!tga)
	{
		vmaUnmapMemory (vk.allocator, screenshot_allocation);
		Con_Printf ("screenshot: not enough memory\n");
		return;
	}
	memset (tga, 0, 18);
	tga[2] = 2;		/* uncompressed type */
	tga[12] = w & 255;
	tga[13] = w >> 8;
	tga[14] = h & 255;
	tga[15] = h >> 8;
	tga[16] = 24;		/* pixel size */

	/* TGA rows go bottom-up, pixels are BGR */
	out = tga + 18;
	for (y = h - 1; y >= 0; y--)
	{
		in = pixels + (size_t)y * w * 4;
		for (x = 0; x < w; x++, in += 4, out += 3)
		{
			out[0] = bgra ? in[0] : in[2];
			out[1] = in[1];
			out[2] = bgra ? in[2] : in[0];
		}
	}
	vmaUnmapMemory (vk.allocator, screenshot_allocation);

	if (FS_WriteFile (filename, tga, size) == 0)
		Con_Printf ("Wrote %s\n", filename);
	free (tga);
}


/* ==========================================================================
 * Swapchain
 * ========================================================================== */

static VkSurfaceFormatKHR VK_ChooseSurfaceFormat (void)
{
	uint32_t		i, count = 0;
	VkSurfaceFormatKHR	*formats, chosen;

	VK_CHECK (vkGetPhysicalDeviceSurfaceFormatsKHR (vk.physical_device, vk.surface, &count, NULL));
	if (!count)
		Sys_Error ("The window surface reports no formats");
	formats = (VkSurfaceFormatKHR *) malloc (count * sizeof(*formats));
	if (!formats)
		Sys_Error ("%s: out of memory", __thisfunc__);
	VK_CHECK (vkGetPhysicalDeviceSurfaceFormatsKHR (vk.physical_device, vk.surface, &count, formats));

	chosen = formats[0];
	for (i = 0; i < count; i++)
	{
		if ((formats[i].format == VK_FORMAT_B8G8R8A8_UNORM || formats[i].format == VK_FORMAT_R8G8B8A8_UNORM) &&
		    formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
		{
			chosen = formats[i];
			break;
		}
	}
	free (formats);
	return chosen;
}

static VkPresentModeKHR VK_ChoosePresentMode (void)
{
	uint32_t		i, count = 0;
	VkPresentModeKHR	modes[16];
	qboolean		mailbox = false, immediate = false;

	if (vid_vsync.integer)
		return VK_PRESENT_MODE_FIFO_KHR;	/* always supported */

	count = Q_COUNTOF(modes);
	vkGetPhysicalDeviceSurfacePresentModesKHR (vk.physical_device, vk.surface, &count, modes);
	for (i = 0; i < count; i++)
	{
		if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR)
			mailbox = true;
		else if (modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR)
			immediate = true;
	}
	if (mailbox)
		return VK_PRESENT_MODE_MAILBOX_KHR;
	if (immediate)
		return VK_PRESENT_MODE_IMMEDIATE_KHR;
	return VK_PRESENT_MODE_FIFO_KHR;
}

static void VK_DestroySwapchainResources (void)
{
	uint32_t	i;

	for (i = 0; i < vk.num_images; i++)
	{
		if (vk.views[i])
			vkDestroyImageView (vk.device, vk.views[i], NULL);
		if (vk.render_finished[i])
			vkDestroySemaphore (vk.device, vk.render_finished[i], NULL);
		vk.views[i] = VK_NULL_HANDLE;
		vk.render_finished[i] = VK_NULL_HANDLE;
		vk.images[i] = VK_NULL_HANDLE;
	}
	vk.num_images = 0;
}

/* (re)create the swapchain for the current window size; leaves
 * vk.swapchain NULL while the window has no area */
static void VK_CreateSwapchain (void)
{
	VkSurfaceCapabilitiesKHR	caps;
	VkSwapchainCreateInfoKHR	info;
	VkImageViewCreateInfo		view_info;
	VkSemaphoreCreateInfo		sem_info;
	VkSwapchainKHR			old = vk.swapchain;
	int				width, height;
	uint32_t			i, num_images;

	vkDeviceWaitIdle (vk.device);
	VK_DestroySwapchainResources ();
	vk.swapchain_dirty = false;

	VK_CHECK (vkGetPhysicalDeviceSurfaceCapabilitiesKHR (vk.physical_device, vk.surface, &caps));
	if (caps.currentExtent.width != UINT32_MAX)
	{
		vk.extent = caps.currentExtent;
	}
	else
	{
		VID_GetClientSize (&width, &height);
		vk.extent.width = (uint32_t) q_max (width, 0);
		vk.extent.height = (uint32_t) q_max (height, 0);
	}
	if (vk.extent.width == 0 || vk.extent.height == 0)
	{
		/* minimized: nothing to present to */
		if (old)
			vkDestroySwapchainKHR (vk.device, old, NULL);
		vk.swapchain = VK_NULL_HANDLE;
		return;
	}

	vk.surface_format = VK_ChooseSurfaceFormat ();
	vk.present_mode = VK_ChoosePresentMode ();

	num_images = caps.minImageCount + 1;
	if (caps.maxImageCount && num_images > caps.maxImageCount)
		num_images = caps.maxImageCount;

	memset (&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
	info.surface = vk.surface;
	info.minImageCount = num_images;
	info.imageFormat = vk.surface_format.format;
	info.imageColorSpace = vk.surface_format.colorSpace;
	info.imageExtent = vk.extent;
	info.imageArrayLayers = 1;
	info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
			  VK_IMAGE_USAGE_TRANSFER_SRC_BIT;	/* screenshots */
	info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
	info.preTransform = caps.currentTransform;
	info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	info.presentMode = vk.present_mode;
	info.clipped = VK_TRUE;
	info.oldSwapchain = old;

	VK_CHECK (vkCreateSwapchainKHR (vk.device, &info, NULL, &vk.swapchain));
	if (old)
		vkDestroySwapchainKHR (vk.device, old, NULL);

	VK_CHECK (vkGetSwapchainImagesKHR (vk.device, vk.swapchain, &num_images, NULL));
	if (num_images > VK_MAX_SWAPCHAIN_IMAGES)
		Sys_Error ("Swapchain has too many images (%u)", num_images);
	VK_CHECK (vkGetSwapchainImagesKHR (vk.device, vk.swapchain, &num_images, vk.images));
	vk.num_images = num_images;

	memset (&view_info, 0, sizeof(view_info));
	view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
	view_info.format = vk.surface_format.format;
	view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	view_info.subresourceRange.levelCount = 1;
	view_info.subresourceRange.layerCount = 1;

	memset (&sem_info, 0, sizeof(sem_info));
	sem_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

	for (i = 0; i < num_images; i++)
	{
		view_info.image = vk.images[i];
		VK_CHECK (vkCreateImageView (vk.device, &view_info, NULL, &vk.views[i]));
		VK_CHECK (vkCreateSemaphore (vk.device, &sem_info, NULL, &vk.render_finished[i]));
	}

	Con_DPrintf ("Vulkan swapchain: %ux%u, %u images, %s\n", vk.extent.width, vk.extent.height, num_images,
			(vk.present_mode == VK_PRESENT_MODE_FIFO_KHR) ? "vsync" :
			(vk.present_mode == VK_PRESENT_MODE_MAILBOX_KHR) ? "mailbox" : "immediate");
}


/* ==========================================================================
 * Frames
 * ========================================================================== */

static void VK_CreateFrames (void)
{
	VkCommandPoolCreateInfo		pool_info;
	VkCommandBufferAllocateInfo	alloc_info;
	VkFenceCreateInfo		fence_info;
	VkSemaphoreCreateInfo		sem_info;
	int				i;

	memset (&pool_info, 0, sizeof(pool_info));
	pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
	pool_info.queueFamilyIndex = vk.queue_family;

	memset (&alloc_info, 0, sizeof(alloc_info));
	alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	alloc_info.commandBufferCount = 1;

	memset (&fence_info, 0, sizeof(fence_info));
	fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;	/* first wait returns at once */

	memset (&sem_info, 0, sizeof(sem_info));
	sem_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

	for (i = 0; i < VK_FRAMES_IN_FLIGHT; i++)
	{
		vk_frame_t *f = &vk.frames[i];

		VK_CHECK (vkCreateCommandPool (vk.device, &pool_info, NULL, &f->cmd_pool));
		alloc_info.commandPool = f->cmd_pool;
		VK_CHECK (vkAllocateCommandBuffers (vk.device, &alloc_info, &f->cmd));
		VK_CHECK (vkCreateFence (vk.device, &fence_info, NULL, &f->fence));
		VK_CHECK (vkCreateSemaphore (vk.device, &sem_info, NULL, &f->image_available));
	}
}

static void VK_DestroyFrames (void)
{
	int	i;

	for (i = 0; i < VK_FRAMES_IN_FLIGHT; i++)
	{
		vk_frame_t *f = &vk.frames[i];

		if (f->image_available)
			vkDestroySemaphore (vk.device, f->image_available, NULL);
		if (f->fence)
			vkDestroyFence (vk.device, f->fence, NULL);
		if (f->cmd_pool)
			vkDestroyCommandPool (vk.device, f->cmd_pool, NULL);	/* frees f->cmd */
		memset (f, 0, sizeof(*f));
	}
}

/* layout transition of the current swapchain image */
static void VK_TransitionImage (VkImageLayout new_layout,
				VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
				VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access)
{
	VkImageMemoryBarrier2	barrier;
	VkDependencyInfo	dep;

	memset (&barrier, 0, sizeof(barrier));
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
	barrier.srcStageMask = src_stage;
	barrier.srcAccessMask = src_access;
	barrier.dstStageMask = dst_stage;
	barrier.dstAccessMask = dst_access;
	barrier.oldLayout = vk.image_layout;
	barrier.newLayout = new_layout;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = vk.images[vk.image_index];
	barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barrier.subresourceRange.levelCount = 1;
	barrier.subresourceRange.layerCount = 1;

	memset (&dep, 0, sizeof(dep));
	dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
	dep.imageMemoryBarrierCount = 1;
	dep.pImageMemoryBarriers = &barrier;

	vkCmdPipelineBarrier2 (vk.frames[vk.frame_index].cmd, &dep);
	vk.image_layout = new_layout;
}

qboolean VK_BeginFrame (void)
{
	vk_frame_t			*f;
	VkCommandBufferBeginInfo	begin;
	VkResult			result;

	if (!vk.device || vk.frame_active)
		return false;

	if (vk.swapchain_dirty || !vk.swapchain)
		VK_CreateSwapchain ();
	if (!vk.swapchain)
		return false;	/* minimized */

	f = &vk.frames[vk.frame_index];
	VK_CHECK (vkWaitForFences (vk.device, 1, &f->fence, VK_TRUE, UINT64_MAX));

	result = vkAcquireNextImageKHR (vk.device, vk.swapchain, UINT64_MAX,
					f->image_available, VK_NULL_HANDLE, &vk.image_index);
	if (result == VK_ERROR_OUT_OF_DATE_KHR)
	{
		vk.swapchain_dirty = true;
		return false;
	}
	if (result == VK_NOT_READY || result == VK_TIMEOUT)
		return false;	/* no image this time; the semaphore was not signaled */
	if (result == VK_SUBOPTIMAL_KHR)
		vk.swapchain_dirty = true;	/* still usable for this frame */
	else if (result != VK_SUCCESS)
		Sys_Error ("vkAcquireNextImageKHR failed: %s", VK_ResultString (result));

	/* only reset once we know this frame will be submitted */
	VK_CHECK (vkResetFences (vk.device, 1, &f->fence));
	VK_CHECK (vkResetCommandPool (vk.device, f->cmd_pool, 0));

	memset (&begin, 0, sizeof(begin));
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	VK_CHECK (vkBeginCommandBuffer (f->cmd, &begin));

	vk.image_layout = VK_IMAGE_LAYOUT_UNDEFINED;	/* previous contents don't matter */
	vk.frame_active = true;
	return true;
}

void VK_ClearScreen (float r, float g, float b)
{
	VkClearColorValue	color;
	VkImageSubresourceRange	range;

	if (!vk.frame_active)
		return;

	VK_TransitionImage (VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0,
			VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);

	color.float32[0] = r;
	color.float32[1] = g;
	color.float32[2] = b;
	color.float32[3] = 1.0f;
	memset (&range, 0, sizeof(range));
	range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	range.levelCount = 1;
	range.layerCount = 1;
	vkCmdClearColorImage (vk.frames[vk.frame_index].cmd, vk.images[vk.image_index],
				VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);
}

/* start rendering into the current swapchain image (dynamic rendering),
 * with viewport and scissor covering it. load_op: CLEAR (to black),
 * LOAD (draw on top of what is there) or DONT_CARE (everything is
 * overwritten anyway) */
void VK_BeginSwapchainRendering (VkAttachmentLoadOp load_op)
{
	VkRenderingAttachmentInfo	color;
	VkRenderingInfo			info;
	VkViewport			viewport;
	VkRect2D			scissor;
	VkCommandBuffer			cmd;

	if (!vk.frame_active)
		return;
	cmd = vk.frames[vk.frame_index].cmd;

	VK_TransitionImage (VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0,
			VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
			VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

	memset (&color, 0, sizeof(color));
	color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
	color.imageView = vk.views[vk.image_index];
	color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	color.loadOp = load_op;
	color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	/* clearValue stays black */

	memset (&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
	info.renderArea.extent = vk.extent;
	info.layerCount = 1;
	info.colorAttachmentCount = 1;
	info.pColorAttachments = &color;
	vkCmdBeginRendering (cmd, &info);

	viewport.x = 0.0f;
	viewport.y = 0.0f;
	viewport.width = (float)vk.extent.width;
	viewport.height = (float)vk.extent.height;
	viewport.minDepth = 0.0f;
	viewport.maxDepth = 1.0f;
	vkCmdSetViewport (cmd, 0, 1, &viewport);

	scissor.offset.x = 0;
	scissor.offset.y = 0;
	scissor.extent = vk.extent;
	vkCmdSetScissor (cmd, 0, 1, &scissor);
}

void VK_EndSwapchainRendering (void)
{
	if (!vk.frame_active)
		return;
	vkCmdEndRendering (vk.frames[vk.frame_index].cmd);
}

void VK_EndFrame (void)
{
	vk_frame_t			*f;
	VkSemaphoreSubmitInfo		wait_info, signal_info;
	VkCommandBufferSubmitInfo	cmd_info;
	VkSubmitInfo2			submit;
	VkPresentInfoKHR		present;
	VkResult			result;

	qboolean			screenshot;

	if (!vk.frame_active)
		return;
	f = &vk.frames[vk.frame_index];

	screenshot = (screenshot_name[0] != 0);
	if (screenshot)
		VK_RecordScreenshotCopy (f->cmd);

	VK_TransitionImage (VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
			VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
			VK_PIPELINE_STAGE_2_NONE, 0);
	VK_CHECK (vkEndCommandBuffer (f->cmd));

	memset (&wait_info, 0, sizeof(wait_info));
	wait_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
	wait_info.semaphore = f->image_available;
	wait_info.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

	memset (&signal_info, 0, sizeof(signal_info));
	signal_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
	signal_info.semaphore = vk.render_finished[vk.image_index];
	signal_info.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

	memset (&cmd_info, 0, sizeof(cmd_info));
	cmd_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
	cmd_info.commandBuffer = f->cmd;

	memset (&submit, 0, sizeof(submit));
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
	submit.waitSemaphoreInfoCount = 1;
	submit.pWaitSemaphoreInfos = &wait_info;
	submit.commandBufferInfoCount = 1;
	submit.pCommandBufferInfos = &cmd_info;
	submit.signalSemaphoreInfoCount = 1;
	submit.pSignalSemaphoreInfos = &signal_info;
	VK_CHECK (vkQueueSubmit2 (vk.queue, 1, &submit, f->fence));

	memset (&present, 0, sizeof(present));
	present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	present.waitSemaphoreCount = 1;
	present.pWaitSemaphores = &vk.render_finished[vk.image_index];
	present.swapchainCount = 1;
	present.pSwapchains = &vk.swapchain;
	present.pImageIndices = &vk.image_index;
	result = vkQueuePresentKHR (vk.queue, &present);
	if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
		vk.swapchain_dirty = true;
	else if (result != VK_SUCCESS)
		Sys_Error ("vkQueuePresentKHR failed: %s", VK_ResultString (result));

	vk.frame_index = (vk.frame_index + 1) % VK_FRAMES_IN_FLIGHT;
	vk.frame_active = false;
	vk.frame_count++;

	/* last: writing it prints to the console, which may draw a new frame,
	 * so the request is cleared first */
	if (screenshot)
	{
		char	name[MAX_OSPATH];

		q_strlcpy (name, screenshot_name, sizeof(name));
		screenshot_name[0] = 0;
		VK_WriteScreenshot (f->fence, name);
	}
}


/* ==========================================================================
 * Init / shutdown
 * ========================================================================== */

void VK_InitSwapchain (void)
{
	Cvar_RegisterVariable (&vid_vsync);
	Cvar_SetCallback (&vid_vsync, VK_VsyncChanged);

	VK_CreateFrames ();
	VK_CreateSwapchain ();
}

void VK_ShutdownSwapchain (void)
{
	vkDeviceWaitIdle (vk.device);
	VK_DestroySwapchainResources ();
	if (vk.swapchain)
		vkDestroySwapchainKHR (vk.device, vk.swapchain, NULL);
	vk.swapchain = VK_NULL_HANDLE;
	VK_DestroyFrames ();
	if (screenshot_buffer)
		vmaDestroyBuffer (vk.allocator, screenshot_buffer, screenshot_allocation);
	screenshot_buffer = VK_NULL_HANDLE;
	screenshot_size = 0;
}
