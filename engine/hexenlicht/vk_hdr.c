/* vk_hdr.c -- HDR output (story 7.3)
 *
 * With vid_hdr 1 on a display Windows runs in HDR, the swapchain is scRGB
 * (R16G16B16A16_SFLOAT in the extended sRGB linear color space: 1 is 80
 * nits, Windows' own composition format), else HDR10 (A2B10G10R10 in
 * ST 2084, PQ with BT.2020's primaries); vid_hdr 2 asks for HDR10. Without
 * VK_EXT_swapchain_colorspace, a known HDR state of the window's display
 * or such a format, the swapchain stays SDR and vk_hdr says why.
 *
 * The frame is drawn as in SDR, in the 8-bit color values (the composite,
 * GL's view blend, gamma, the 2D: they blend as GL's), but into this
 * module's R16G16B16A16_SFLOAT frame image, so values above 1 are kept;
 * VK_EndFrame then has hdr_encode.frag turn them into light (the engine's
 * transfer, or Windows' sRGB decode with r_hdr_decode 1) at the paper
 * white and write the swapchain image. Everything up to 1 is the SDR
 * image at the paper white; the tone mapper keeps the image up to 1 and
 * rolls it off above towards the display's peak (vk_tonemap.c: the
 * headroom, the peak over the paper white).
 *
 * The paper white is Windows' SDR content brightness, the peak the
 * display's (DXGI's MaxLuminance), read when the swapchain is created and
 * by vk_hdr; r_hdr_white and r_hdr_peak (nits) replace them.
 *
 * Screenshots in HDR are the frame image clipped at 1, the SDR image
 * (vk_swapchain.c); vk_hdrshot writes the swapchain image as light in nits
 * (BT.709's primaries) into a PFM, for checking the output.
 *
 * Copyright (C) 2026  Hexenlicht contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#define COBJMACROS
#include "quakedef.h"
#include "winquake.h"
#include <dxgi1_6.h>
#include "vk_local.h"

/* 0 SDR; 1 HDR (scRGB, else HDR10) where Windows runs the display in HDR; 2 HDR10 */
static cvar_t	vid_hdr = {"vid_hdr", "0", CVAR_ARCHIVE};
/* nits; 0: Windows' SDR content brightness, the display's peak */
static cvar_t	r_hdr_white = {"r_hdr_white", "0", CVAR_ARCHIVE};
static cvar_t	r_hdr_peak = {"r_hdr_peak", "0", CVAR_ARCHIVE};
/* 0: the 8-bit colors decoded by the engine's transfer (a 2.2 power, the
 * sRGB curve with r_srgb 1); 1: by the sRGB curve, as Windows shows an SDR
 * program on an HDR desktop */
static cvar_t	r_hdr_decode = {"r_hdr_decode", "0", CVAR_ARCHIVE};

#define DEFAULT_WHITE	200.0f	/* nits, when Windows doesn't say */
#define DEFAULT_PEAK	1000.0f

/* the window's display as Windows reports it */
typedef struct
{
	qboolean	valid;		/* found */
	qboolean	hdr_on;		/* Windows runs it in HDR (DXGI's G2084 color space) */
	float		sdr_white;	/* Windows' SDR content brightness, nits; 0 unknown */
	float		peak;		/* nits; 0 unknown */
	float		full_frame;	/* nits */
	unsigned int	bits;		/* per color */
	char		name[64];	/* the monitor's */
	char		device[32];	/* \\.\DISPLAYn */
} hdr_display_t;

static hdr_display_t	display;
static char		status[128] = "off";
static qboolean		hdr10;		/* the HDR swapchain is HDR10, else scRGB */

/* the frame image */
static VkImage		frame_image;
static VmaAllocation	frame_allocation;
static VkImageView	frame_view;

/* the encode pass */
static VkDescriptorSetLayout	set_layout;
static VkDescriptorPool		pool;
static VkDescriptorSet		set;
static VkSampler		sampler;
static VkPipelineLayout		layout;
static VkPipeline		pipeline;
static VkFormat			pipeline_format;

/* hdr_encode.frag's push constants */
typedef struct
{
	float		scale;		/* linear light (1: the paper white) to the swapchain's units */
	float		max_value;	/* the peak in the same units */
	uint32_t	srgb;		/* decode up to 1 by the sRGB curve, else the 2.2 power */
	uint32_t	srgb_above;	/* the same above 1: the engine's transfer, as the composite encoded it */
	uint32_t	pq;		/* HDR10: BT.2020 and PQ */
} encode_push_t;

/* vk_hdrshot */
static char		shot_name[MAX_OSPATH];
static VkBuffer		shot_buffer;
static VmaAllocation	shot_allocation;
static VkDeviceSize	shot_size;
static char		shot_recorded[MAX_OSPATH];	/* the name of the copy this frame recorded, empty if none */


/* ==========================================================================
 * The display
 * ========================================================================== */

static void WideToUTF8 (const WCHAR *w, char *out, int size)
{
	if (!WideCharToMultiByte (CP_UTF8, 0, w, -1, out, size, NULL, NULL))
		out[0] = 0;
}

/* Windows' SDR content brightness and the monitor's name, for the display
 * whose GDI name (\\.\DISPLAYn) DXGI gave */
static void QuerySdrWhite (const WCHAR *gdi_name)
{
	UINT32			num_paths = 0, num_modes = 0, i;
	DISPLAYCONFIG_PATH_INFO	*paths;
	DISPLAYCONFIG_MODE_INFO	*modes;

	if (GetDisplayConfigBufferSizes (QDC_ONLY_ACTIVE_PATHS, &num_paths, &num_modes) != ERROR_SUCCESS)
		return;
	paths = (DISPLAYCONFIG_PATH_INFO *) calloc (q_max (num_paths, 1), sizeof(*paths));
	modes = (DISPLAYCONFIG_MODE_INFO *) calloc (q_max (num_modes, 1), sizeof(*modes));
	if (paths && modes && QueryDisplayConfig (QDC_ONLY_ACTIVE_PATHS, &num_paths, paths, &num_modes, modes, NULL) == ERROR_SUCCESS)
	{
		for (i = 0; i < num_paths; i++)
		{
			DISPLAYCONFIG_SOURCE_DEVICE_NAME	source;
			DISPLAYCONFIG_SDR_WHITE_LEVEL		white;
			DISPLAYCONFIG_TARGET_DEVICE_NAME	target;

			memset (&source, 0, sizeof(source));
			source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
			source.header.size = sizeof(source);
			source.header.adapterId = paths[i].sourceInfo.adapterId;
			source.header.id = paths[i].sourceInfo.id;
			if (DisplayConfigGetDeviceInfo (&source.header) != ERROR_SUCCESS || wcscmp (source.viewGdiDeviceName, gdi_name))
				continue;

			memset (&white, 0, sizeof(white));
			white.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
			white.header.size = sizeof(white);
			white.header.adapterId = paths[i].targetInfo.adapterId;
			white.header.id = paths[i].targetInfo.id;
			if (DisplayConfigGetDeviceInfo (&white.header) == ERROR_SUCCESS)
				display.sdr_white = (float)white.SDRWhiteLevel * 80.0f / 1000.0f;	/* in thousandths of 80 nits */

			memset (&target, 0, sizeof(target));
			target.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
			target.header.size = sizeof(target);
			target.header.adapterId = paths[i].targetInfo.adapterId;
			target.header.id = paths[i].targetInfo.id;
			if (DisplayConfigGetDeviceInfo (&target.header) == ERROR_SUCCESS)
				WideToUTF8 (target.monitorFriendlyDeviceName, display.name, sizeof(display.name));
			break;
		}
	}
	free (paths);
	free (modes);
}

/* the display the window is on, as DXGI and the display configuration
 * report it (a new factory each time: an old one doesn't see changes) */
static void QueryDisplay (void)
{
	IDXGIFactory1	*factory;
	IDXGIAdapter1	*adapter;
	IDXGIOutput	*output;
	HMONITOR	monitor;
	UINT		i, j;

	memset (&display, 0, sizeof(display));
	if (!mainwindow)
		return;
	monitor = MonitorFromWindow (mainwindow, MONITOR_DEFAULTTONEAREST);
	if (FAILED (CreateDXGIFactory1 (&IID_IDXGIFactory1, (void **)&factory)))
		return;
	for (i = 0; !display.valid && IDXGIFactory1_EnumAdapters1 (factory, i, &adapter) == S_OK; i++)
	{
		for (j = 0; !display.valid && IDXGIAdapter1_EnumOutputs (adapter, j, &output) == S_OK; j++)
		{
			IDXGIOutput6		*output6;
			DXGI_OUTPUT_DESC1	desc;

			if (SUCCEEDED (IDXGIOutput_QueryInterface (output, &IID_IDXGIOutput6, (void **)&output6)))
			{
				if (SUCCEEDED (IDXGIOutput6_GetDesc1 (output6, &desc)) && desc.Monitor == monitor)
				{
					display.valid = true;
					display.hdr_on = (desc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);
					display.peak = desc.MaxLuminance;
					display.full_frame = desc.MaxFullFrameLuminance;
					display.bits = desc.BitsPerColor;
					WideToUTF8 (desc.DeviceName, display.device, sizeof(display.device));
					QuerySdrWhite (desc.DeviceName);
				}
				IDXGIOutput6_Release (output6);
			}
			IDXGIOutput_Release (output);
		}
		IDXGIAdapter1_Release (adapter);
	}
	IDXGIFactory1_Release (factory);
}

/* the paper white and the peak in nits, and where each came from */
static void Levels (float *white, float *peak, const char **white_from, const char **peak_from)
{
	float		w = r_hdr_white.value, p = r_hdr_peak.value;
	const char	*wf = "r_hdr_white", *pf = "r_hdr_peak";

	if (w <= 0)
	{
		w = (display.sdr_white > 0) ? display.sdr_white : DEFAULT_WHITE;
		wf = (display.sdr_white > 0) ? "Windows' SDR content brightness" : "a default: Windows didn't say";
	}
	if (p <= 0)
	{
		p = (display.peak > 0) ? display.peak : DEFAULT_PEAK;
		pf = (display.peak > 0) ? "the display's" : "a default: the display didn't say";
	}
	*white = q_min (q_max (w, 1.0f), 10000.0f);
	*peak = q_min (q_max (p, 1.0f), 10000.0f);
	if (white_from)
		*white_from = wf;
	if (peak_from)
		*peak_from = pf;
}

float VK_HDRHeadroom (void)
{
	float	white, peak;

	if (!vk.hdr)
		return 0.0f;
	Levels (&white, &peak, NULL, NULL);
	return q_max (peak / white, 1.0f);
}

const char *VK_HDRStatus (void)
{
	if (!vk.swapchain && vid_hdr.integer)
		return "off while the window has no area (minimized)";
	return status;
}


/* ==========================================================================
 * The swapchain's format (vk_swapchain.c)
 * ========================================================================== */

static qboolean FindFormat (const VkSurfaceFormatKHR *formats, uint32_t count, VkFormat format,
			    VkColorSpaceKHR space, VkSurfaceFormatKHR *chosen)
{
	uint32_t	i;

	for (i = 0; i < count; i++)
	{
		if (formats[i].format == format && formats[i].colorSpace == space)
		{
			*chosen = formats[i];
			return true;
		}
	}
	return false;
}

/* at each swapchain creation: whether it is HDR, in which format */
qboolean VK_HDRChooseFormat (const VkSurfaceFormatKHR *formats, uint32_t count, VkSurfaceFormatKHR *chosen)
{
	qboolean	scrgb, pq;

	if (!vid_hdr.integer)
	{
		q_strlcpy (status, "off (vid_hdr 0)", sizeof(status));
		return false;	/* no display query: SDR does nothing new at a resize */
	}
	QueryDisplay ();
	if (!vk.have_colorspace)
	{
		q_strlcpy (status, "off: the Vulkan driver has no VK_EXT_swapchain_colorspace", sizeof(status));
		return false;
	}
	if (!display.valid)
	{
		q_strlcpy (status, "off: the display's HDR state is unknown", sizeof(status));
		return false;
	}
	if (!display.hdr_on)
	{
		q_strlcpy (status, "off: Windows HDR is off for this display", sizeof(status));
		return false;
	}
	scrgb = (vid_hdr.integer != 2) && FindFormat (formats, count, VK_FORMAT_R16G16B16A16_SFLOAT,
						     VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT, chosen);
	pq = !scrgb && FindFormat (formats, count, VK_FORMAT_A2B10G10R10_UNORM_PACK32,
				   VK_COLOR_SPACE_HDR10_ST2084_EXT, chosen);
	if (!scrgb && !pq)
	{
		q_strlcpy (status, (vid_hdr.integer == 2) ? "off: the driver offers no HDR10 swapchain" :
							"off: the driver offers no HDR swapchain", sizeof(status));
		return false;
	}
	hdr10 = pq;
	q_strlcpy (status, pq ? "on: HDR10 (A2B10G10R10, ST 2084)" :
				"on: scRGB (R16G16B16A16_SFLOAT, extended sRGB linear)", sizeof(status));
	return true;
}

static void HDRChanged (cvar_t *var)
{
	(void)var;
	VK_SwapchainChanged ();
}


/* ==========================================================================
 * The frame image
 * ========================================================================== */

void VK_HDRDestroyFrame (void)
{
	if (frame_view)
		vkDestroyImageView (vk.device, frame_view, NULL);
	if (frame_image)
		vmaDestroyImage (vk.allocator, frame_image, frame_allocation);
	frame_view = VK_NULL_HANDLE;
	frame_image = VK_NULL_HANDLE;
	frame_allocation = NULL;
}

/* after an HDR swapchain was created (outside frames, the device idle) */
void VK_HDRCreateFrame (void)
{
	VkImageCreateInfo	image_info;
	VmaAllocationCreateInfo	alloc_info;
	VkImageViewCreateInfo	view_info;
	VkDescriptorImageInfo	descriptor_image;
	VkWriteDescriptorSet	write;

	VK_HDRDestroyFrame ();

	memset (&image_info, 0, sizeof(image_info));
	image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	image_info.imageType = VK_IMAGE_TYPE_2D;
	image_info.format = VK_HDR_FRAME_FORMAT;
	image_info.extent.width = vk.extent.width;
	image_info.extent.height = vk.extent.height;
	image_info.extent.depth = 1;
	image_info.mipLevels = 1;
	image_info.arrayLayers = 1;
	image_info.samples = VK_SAMPLE_COUNT_1_BIT;
	image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
	image_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
			   VK_IMAGE_USAGE_TRANSFER_SRC_BIT |	/* screenshots */
			   VK_IMAGE_USAGE_TRANSFER_DST_BIT;	/* VK_ClearScreen */
	image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	memset (&alloc_info, 0, sizeof(alloc_info));
	alloc_info.usage = VMA_MEMORY_USAGE_AUTO;
	alloc_info.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
	VK_CHECK (vmaCreateImage (vk.allocator, &image_info, &alloc_info, &frame_image, &frame_allocation, NULL));

	memset (&view_info, 0, sizeof(view_info));
	view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	view_info.image = frame_image;
	view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
	view_info.format = VK_HDR_FRAME_FORMAT;
	view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	view_info.subresourceRange.levelCount = 1;
	view_info.subresourceRange.layerCount = 1;
	VK_CHECK (vkCreateImageView (vk.device, &view_info, NULL, &frame_view));

	descriptor_image.sampler = sampler;
	descriptor_image.imageView = frame_view;
	descriptor_image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	memset (&write, 0, sizeof(write));
	write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	write.dstSet = set;
	write.dstBinding = 0;
	write.descriptorCount = 1;
	write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	write.pImageInfo = &descriptor_image;
	vkUpdateDescriptorSets (vk.device, 1, &write, 0, NULL);
}

VkImage VK_HDRFrameImage (void)
{
	return frame_image;
}

VkImageView VK_HDRFrameView (void)
{
	return frame_view;
}


/* ==========================================================================
 * The encode pass
 * ========================================================================== */

/* fullscreen.vert + hdr_encode.frag, for the swapchain's format */
static void CreatePipeline (void)
{
	VkShaderModule				vert, frag;
	VkPipelineShaderStageCreateInfo		stages[2];
	VkPipelineVertexInputStateCreateInfo	vertex_input;
	VkPipelineInputAssemblyStateCreateInfo	input_assembly;
	VkPipelineViewportStateCreateInfo	viewport;
	VkPipelineRasterizationStateCreateInfo	raster;
	VkPipelineMultisampleStateCreateInfo	multisample;
	VkPipelineColorBlendAttachmentState	blend_attachment;
	VkPipelineColorBlendStateCreateInfo	blend;
	VkPipelineDynamicStateCreateInfo	dynamic;
	VkPipelineRenderingCreateInfo		rendering;
	VkGraphicsPipelineCreateInfo		info;
	const VkDynamicState	dynamic_states[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };

	vert = VK_LoadShader ("fullscreen.vert");
	frag = VK_LoadShader ("hdr_encode.frag");

	memset (stages, 0, sizeof(stages));
	stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = vert;
	stages[0].pName = "main";
	stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = frag;
	stages[1].pName = "main";

	memset (&vertex_input, 0, sizeof(vertex_input));
	vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	memset (&input_assembly, 0, sizeof(input_assembly));
	input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	memset (&viewport, 0, sizeof(viewport));
	viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	viewport.viewportCount = 1;
	viewport.scissorCount = 1;
	memset (&raster, 0, sizeof(raster));
	raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	raster.polygonMode = VK_POLYGON_MODE_FILL;
	raster.cullMode = VK_CULL_MODE_NONE;
	raster.lineWidth = 1.0f;
	memset (&multisample, 0, sizeof(multisample));
	multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
	memset (&blend_attachment, 0, sizeof(blend_attachment));
	blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
					  VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
	memset (&blend, 0, sizeof(blend));
	blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	blend.attachmentCount = 1;
	blend.pAttachments = &blend_attachment;
	memset (&dynamic, 0, sizeof(dynamic));
	dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dynamic.dynamicStateCount = Q_COUNTOF(dynamic_states);
	dynamic.pDynamicStates = dynamic_states;

	pipeline_format = vk.surface_format.format;
	memset (&rendering, 0, sizeof(rendering));
	rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
	rendering.colorAttachmentCount = 1;
	rendering.pColorAttachmentFormats = &pipeline_format;

	memset (&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	info.pNext = &rendering;
	info.stageCount = 2;
	info.pStages = stages;
	info.pVertexInputState = &vertex_input;
	info.pInputAssemblyState = &input_assembly;
	info.pViewportState = &viewport;
	info.pRasterizationState = &raster;
	info.pMultisampleState = &multisample;
	info.pColorBlendState = &blend;
	info.pDynamicState = &dynamic;
	info.layout = layout;
	VK_CHECK (vkCreateGraphicsPipelines (vk.device, VK_NULL_HANDLE, 1, &info, NULL, &pipeline));

	vkDestroyShaderModule (vk.device, vert, NULL);
	vkDestroyShaderModule (vk.device, frag, NULL);
}

void VK_DestroyHDRPipeline (void)
{
	if (pipeline)
		vkDestroyPipeline (vk.device, pipeline, NULL);
	pipeline = VK_NULL_HANDLE;
}

/* the frame image into the swapchain image: VK_EndFrame, inside a
 * rendering into the swapchain image covering it */
void VK_HDRDrawEncode (VkCommandBuffer cmd)
{
	encode_push_t	push;
	float		white, peak, unit;

	if (pipeline && pipeline_format != vk.surface_format.format)
	{
		vkDeviceWaitIdle (vk.device);
		VK_DestroyHDRPipeline ();
	}
	if (!pipeline)
		CreatePipeline ();

	Levels (&white, &peak, NULL, NULL);
	unit = hdr10 ? 10000.0f : 80.0f;	/* PQ's 1, scRGB's 1 */
	push.scale = white / unit;
	push.max_value = peak / unit;
	push.srgb = (r_hdr_decode.integer || VK_ColorsSRGB ()) ? 1u : 0u;
	push.srgb_above = VK_ColorsSRGB () ? 1u : 0u;	/* the shoulder's light as the tone mapper made it */
	push.pq = hdr10 ? 1u : 0u;

	vkCmdBindPipeline (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
	vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, NULL);
	vkCmdPushConstants (cmd, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
	vkCmdDraw (cmd, 3, 1, 0, 0);
}


/* ==========================================================================
 * vk_hdrshot: the swapchain image as light, in a PFM
 * ========================================================================== */

qboolean VK_HDRShotPending (void)
{
	return shot_name[0] != 0 && vk.hdr;
}

void VK_HDRRecordShot (VkCommandBuffer cmd, VkImage image)
{
	VkBufferCreateInfo	info;
	VmaAllocationCreateInfo	alloc;
	VkBufferImageCopy	copy;
	VkDeviceSize		size = (VkDeviceSize)vk.extent.width * vk.extent.height * (hdr10 ? 4 : 8);

	if (shot_size < size)
	{
		if (shot_buffer)
			vmaDestroyBuffer (vk.allocator, shot_buffer, shot_allocation);
		memset (&info, 0, sizeof(info));
		info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
		info.size = size;
		info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
		memset (&alloc, 0, sizeof(alloc));
		alloc.usage = VMA_MEMORY_USAGE_AUTO;
		alloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
		VK_CHECK (vmaCreateBuffer (vk.allocator, &info, &alloc, &shot_buffer, &shot_allocation, NULL));
		shot_size = size;
	}
	memset (&copy, 0, sizeof(copy));
	copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	copy.imageSubresource.layerCount = 1;
	copy.imageExtent.width = vk.extent.width;
	copy.imageExtent.height = vk.extent.height;
	copy.imageExtent.depth = 1;
	vkCmdCopyImageToBuffer (cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, shot_buffer, 1, &copy);
	q_strlcpy (shot_recorded, shot_name, sizeof(shot_recorded));
	shot_name[0] = 0;	/* a frame the write draws (Con_Printf) doesn't record it again */
}

/* ST 2084's EOTF: a PQ value (0-1) as light, 1 = 10000 nits */
static float PQDecode (float v)
{
	const float	m1 = 2610.0f / 16384.0f, m2 = 2523.0f / 4096.0f * 128.0f;
	const float	c1 = 3424.0f / 4096.0f, c2 = 2413.0f / 4096.0f * 32.0f, c3 = 2392.0f / 4096.0f * 32.0f;
	float		p = powf (q_max (v, 0.0f), 1.0f / m2);

	return powf (q_max (p - c1, 0.0f) / (c2 - c3 * p), 1.0f / m1);
}

/* after the frame's submit: the copy as nits in BT.709's primaries */
void VK_HDRWriteShot (VkFence fence)
{
	int		w = (int)vk.extent.width, h = (int)vk.extent.height, x, y;
	char		header[64], name[MAX_OSPATH];
	int		header_len;
	size_t		size;
	byte		*file;
	float		*out;
	const byte	*pixels;

	if (!shot_recorded[0])
		return;
	q_strlcpy (name, shot_recorded, sizeof(name));
	shot_recorded[0] = 0;

	VK_CHECK (vkWaitForFences (vk.device, 1, &fence, VK_TRUE, UINT64_MAX));
	header_len = q_snprintf (header, sizeof(header), "PF\n%d %d\n-1.0\n", w, h);	/* little endian */
	size = (size_t)header_len + (size_t)w * h * 3 * sizeof(float);
	file = (byte *) malloc (size);
	if (!file)
	{
		Con_Printf ("vk_hdrshot: not enough memory\n");
		return;
	}
	memcpy (file, header, header_len);
	out = (float *)(file + header_len);

	VK_CHECK (vmaMapMemory (vk.allocator, shot_allocation, (void **)&pixels));
	VK_CHECK (vmaInvalidateAllocation (vk.allocator, shot_allocation, 0, VK_WHOLE_SIZE));
	for (y = h - 1; y >= 0; y--)	/* PFM's rows go bottom-up */
	{
		for (x = 0; x < w; x++, out += 3)
		{
			size_t	i = (size_t)y * w + x;

			if (hdr10)
			{
				uint32_t	v;
				float		r, g, b;

				memcpy (&v, pixels + i * 4, 4);
				r = PQDecode ((float)(v & 0x3ff) / 1023.0f) * 10000.0f;
				g = PQDecode ((float)((v >> 10) & 0x3ff) / 1023.0f) * 10000.0f;
				b = PQDecode ((float)((v >> 20) & 0x3ff) / 1023.0f) * 10000.0f;
				/* BT.2020 to BT.709 */
				out[0] =  1.660491f * r - 0.587641f * g - 0.072850f * b;
				out[1] = -0.124550f * r + 1.132900f * g - 0.008349f * b;
				out[2] = -0.018151f * r - 0.100579f * g + 1.118730f * b;
			}
			else
			{
				uint16_t	v[4];

				memcpy (v, pixels + i * 8, 8);
				out[0] = VK_HalfToFloat (v[0]) * 80.0f;
				out[1] = VK_HalfToFloat (v[1]) * 80.0f;
				out[2] = VK_HalfToFloat (v[2]) * 80.0f;
			}
		}
	}
	vmaUnmapMemory (vk.allocator, shot_allocation);

	if (FS_WriteFile (name, file, size) == 0)
		Con_Printf ("Wrote %s (%s, nits)\n", name, hdr10 ? "HDR10" : "scRGB");
	free (file);
}


/* ==========================================================================
 * Console commands
 * ========================================================================== */

static void VK_HDR_f (void)
{
	float		white, peak;
	const char	*white_from, *peak_from;

	if (vk.frame_active)
		return;
	QueryDisplay ();	/* Windows' levels now; its HDR switch needs a new swapchain (vid_hdr, vid_restart, a resize) */
	Con_Printf ("HDR: %s\n", status);
	if (display.valid)
		Con_Printf ("display: %s (%s), Windows HDR %s, %u bits; SDR content brightness %.0f nits, peak %.0f nits (full frame %.0f)\n",
				display.name[0] ? display.name : "?", display.device, display.hdr_on ? "on" : "off",
				display.bits, display.sdr_white, display.peak, display.full_frame);
	else
		Con_Printf ("display: unknown (DXGI found no output for the window)\n");
	Levels (&white, &peak, &white_from, &peak_from);
	Con_Printf ("paper white %.0f nits (%s), peak %.0f nits (%s): headroom %.2f\n",
			white, white_from, peak, peak_from, q_max (peak / white, 1.0f));
	Con_Printf ("decode: %s\n", r_hdr_decode.integer ? "the sRGB curve (r_hdr_decode 1: Windows' SDR look)" :
				VK_ColorsSRGB () ? "the sRGB curve (the engine's, r_srgb 1)" : "a 2.2 power (the engine's)");
}

static void VK_HDRShot_f (void)
{
	if (Cmd_Argc () != 2)
	{
		Con_Printf ("vk_hdrshot <name>: shots\\<name>.pfm, the HDR output in nits\n");
		return;
	}
	if (!vk.hdr)
	{
		Con_Printf ("vk_hdrshot: HDR is %s\n", VK_HDRStatus ());
		return;
	}
	q_snprintf (shot_name, sizeof(shot_name), "shots/%s.pfm", Cmd_Argv (1));
}


/* ==========================================================================
 * Init / shutdown
 * ========================================================================== */

void VK_InitHDR (void)
{
	VkDescriptorSetLayoutBinding	binding;
	VkDescriptorSetLayoutCreateInfo	set_layout_info;
	VkDescriptorPoolSize		pool_size;
	VkDescriptorPoolCreateInfo	pool_info;
	VkDescriptorSetAllocateInfo	alloc_info;
	VkSamplerCreateInfo		sampler_info;
	VkPushConstantRange		push_range;
	VkPipelineLayoutCreateInfo	layout_info;

	Cvar_RegisterVariable (&vid_hdr);
	Cvar_SetCallback (&vid_hdr, HDRChanged);
	Cvar_RegisterVariable (&r_hdr_white);
	Cvar_RegisterVariable (&r_hdr_peak);
	Cvar_RegisterVariable (&r_hdr_decode);
	Cmd_AddCommand ("vk_hdr", VK_HDR_f);
	Cmd_AddCommand ("vk_hdrshot", VK_HDRShot_f);

	memset (&binding, 0, sizeof(binding));
	binding.binding = 0;
	binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	binding.descriptorCount = 1;
	binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	memset (&set_layout_info, 0, sizeof(set_layout_info));
	set_layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	set_layout_info.bindingCount = 1;
	set_layout_info.pBindings = &binding;
	VK_CHECK (vkCreateDescriptorSetLayout (vk.device, &set_layout_info, NULL, &set_layout));

	pool_size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	pool_size.descriptorCount = 1;
	memset (&pool_info, 0, sizeof(pool_info));
	pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	pool_info.maxSets = 1;
	pool_info.poolSizeCount = 1;
	pool_info.pPoolSizes = &pool_size;
	VK_CHECK (vkCreateDescriptorPool (vk.device, &pool_info, NULL, &pool));

	memset (&alloc_info, 0, sizeof(alloc_info));
	alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	alloc_info.descriptorPool = pool;
	alloc_info.descriptorSetCount = 1;
	alloc_info.pSetLayouts = &set_layout;
	VK_CHECK (vkAllocateDescriptorSets (vk.device, &alloc_info, &set));

	memset (&sampler_info, 0, sizeof(sampler_info));
	sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	sampler_info.magFilter = VK_FILTER_NEAREST;
	sampler_info.minFilter = VK_FILTER_NEAREST;
	sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	VK_CHECK (vkCreateSampler (vk.device, &sampler_info, NULL, &sampler));

	push_range.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	push_range.offset = 0;
	push_range.size = sizeof(encode_push_t);
	memset (&layout_info, 0, sizeof(layout_info));
	layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	layout_info.setLayoutCount = 1;
	layout_info.pSetLayouts = &set_layout;
	layout_info.pushConstantRangeCount = 1;
	layout_info.pPushConstantRanges = &push_range;
	VK_CHECK (vkCreatePipelineLayout (vk.device, &layout_info, NULL, &layout));
}

void VK_ShutdownHDR (void)
{
	VK_HDRDestroyFrame ();
	VK_DestroyHDRPipeline ();
	if (shot_buffer)
		vmaDestroyBuffer (vk.allocator, shot_buffer, shot_allocation);
	shot_buffer = VK_NULL_HANDLE;
	shot_size = 0;
	if (layout)
		vkDestroyPipelineLayout (vk.device, layout, NULL);
	if (sampler)
		vkDestroySampler (vk.device, sampler, NULL);
	if (pool)
		vkDestroyDescriptorPool (vk.device, pool, NULL);	/* frees the set */
	if (set_layout)
		vkDestroyDescriptorSetLayout (vk.device, set_layout, NULL);
	layout = VK_NULL_HANDLE;
	sampler = VK_NULL_HANDLE;
	pool = VK_NULL_HANDLE;
	set = VK_NULL_HANDLE;
	set_layout = VK_NULL_HANDLE;
}
