/* vk_images.c -- the render-target images (shaders/global_textures.h) and
 * the blue noise
 *
 * Quake II RTX's vkpt_create_images (textures.c): every image of
 * LIST_IMAGES and LIST_IMAGES_A_B, at the swapchain's size with the width
 * rounded up to even, for the two checkerboard fields (recreated with it,
 * VK_INIT_SWAPCHAIN in vk_core.c's table); the 3D view renders into their
 * top left. They stay in the GENERAL layout and are cleared to 0 when
 * created (the _B images are read as last frame's before anything wrote
 * them). Descriptor set 1 of the view passes (vk_pathtracer.c) has each as
 * a storage image and as a sampled texture; there are two sets, even and
 * odd, which swap the A and B images
 * of LIST_IMAGES_A_B, picked by the 3D frame's number, so the _A names are
 * always this frame's and the _B names the last frame's.
 *
 * The set's last binding is Quake II RTX's blue noise (load_blue_noise),
 * the path tracer's random numbers: Christoph Peters' CC0 textures
 * (libs/bluenoise, copied next to the exe as blue_noise\), 64 of 64 x 64
 * texels with four 16-bit channels, each channel one layer of a
 * NUM_BLUE_NOISE_TEX-layer array; loaded once at startup.
 * vk_images lists them.
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
#include "shaders/hl_shared.h"
#include "shaders/global_textures.h"
#include "stb_image.h"

typedef struct
{
	const char	*name;
	VkFormat	format;
	uint32_t	width, height;
	VkImage		image;
	VmaAllocation	allocation;
	VkImageView	view;
	VkDeviceSize	size;		/* memory */
} vk_image_t;

VkExtent2D		vk_image_extent;	/* the images' size; 0 x 0 = none (IMG_WIDTH, IMG_HEIGHT) */
VkDescriptorSetLayout	vk_images_set_layout;

static vk_image_t	images[NUM_VKPT_IMAGES];
static vk_image_t	blue_noise;		/* BLUE_NOISE_RES x BLUE_NOISE_RES x NUM_BLUE_NOISE_TEX layers */
static VkSampler	sampler_nearest, sampler_linear;
static VkDescriptorPool	images_pool;
static VkDescriptorSet	images_sets[2];		/* even, odd */

VkDescriptorSet VK_ImagesSet (void)
{
	return images_sets[vk_render_frame & 1];
}

VkImage VK_Image (int index)
{
	return images[index].image;
}

/* SPIKE (3.9): what Streamline's resource tags need */
void VK_ImageInfo (int index, VkImage *image, VkImageView *view, VkFormat *format, uint32_t *width, uint32_t *height)
{
	*image = images[index].image;
	*view = images[index].view;
	*format = images[index].format;
	*width = images[index].width;
	*height = images[index].height;
}

qboolean VK_ImagesReady (void)
{
	return vk_image_extent.width > 0;
}

/* the image's descriptors in one of the sets */
static void WriteImageDescriptors (VkDescriptorSet set, int binding, int index)
{
	VkDescriptorImageInfo	storage, sampled;
	VkWriteDescriptorSet	writes[2];

	memset (&storage, 0, sizeof(storage));
	storage.imageView = images[index].view;
	storage.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
	sampled = storage;
	sampled.sampler = (index == VKPT_IMG_TAA_OUTPUT || index == VKPT_IMG_ASVGF_TAA_A || index == VKPT_IMG_ASVGF_TAA_B ||
			   index == VKPT_IMG_BLOOM_HBLUR || index == VKPT_IMG_BLOOM_VBLUR) ?
			  sampler_linear : sampler_nearest;

	memset (writes, 0, sizeof(writes));
	writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	writes[0].dstSet = set;
	writes[0].dstBinding = BINDING_OFFSET_IMAGES + binding;
	writes[0].descriptorCount = 1;
	writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	writes[0].pImageInfo = &storage;
	writes[1] = writes[0];
	writes[1].dstBinding = BINDING_OFFSET_TEXTURES + binding;
	writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	writes[1].pImageInfo = &sampled;
	vkUpdateDescriptorSets (vk.device, 2, writes, 0, NULL);
}

void VK_CreateImages (void)
{
	VkImageCreateInfo		image_info;
	VmaAllocationCreateInfo		alloc_info;
	VmaAllocationInfo		allocated;
	VkImageViewCreateInfo		view_info;
	VkImageMemoryBarrier2		barriers[NUM_VKPT_IMAGES];
	VkMemoryBarrier2		cleared;
	VkDependencyInfo		dep;
	VkClearColorValue		clear_color;
	VkImageSubresourceRange		range;
	VkCommandBuffer			cmd;
	VkDeviceSize			total = 0;
	int				i;

	VK_ResetDenoiserHistory ();	/* new images hold no history, */
	VK_ResetUBOHistory ();		/* nor a last frame of another size */
	vk_image_extent = vk.extent;
	vk_image_extent.width = (vk_image_extent.width + 1) & ~1u;	/* two checkerboard fields */
	if (!vk.swapchain || !vk_image_extent.width || !vk_image_extent.height)
	{
		vk_image_extent.width = vk_image_extent.height = 0;	/* minimized: none until it has a size */
		return;
	}

#define IMG_DO(_name, _binding, _vkformat, _glslformat, _w, _h) \
	images[VKPT_IMG_##_name].name = #_name; \
	images[VKPT_IMG_##_name].format = VK_FORMAT_##_vkformat; \
	images[VKPT_IMG_##_name].width = _w; \
	images[VKPT_IMG_##_name].height = _h;
	LIST_IMAGES
	LIST_IMAGES_A_B
#undef IMG_DO

	for (i = 0; i < NUM_VKPT_IMAGES; i++)
	{
		vk_image_t	*img = &images[i];

		memset (&image_info, 0, sizeof(image_info));
		image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
		image_info.imageType = VK_IMAGE_TYPE_2D;
		image_info.format = img->format;
		image_info.extent.width = img->width;
		image_info.extent.height = img->height;
		image_info.extent.depth = 1;
		image_info.mipLevels = 1;
		image_info.arrayLayers = 1;
		image_info.samples = VK_SAMPLE_COUNT_1_BIT;
		image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
		image_info.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
				   VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
		image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		memset (&alloc_info, 0, sizeof(alloc_info));
		alloc_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
		VK_CHECK (vmaCreateImage (vk.allocator, &image_info, &alloc_info, &img->image, &img->allocation, &allocated));
		img->size = allocated.size;
		total += allocated.size;

		memset (&view_info, 0, sizeof(view_info));
		view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		view_info.image = img->image;
		view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
		view_info.format = img->format;
		view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		view_info.subresourceRange.levelCount = 1;
		view_info.subresourceRange.layerCount = 1;
		VK_CHECK (vkCreateImageView (vk.device, &view_info, NULL, &img->view));

		memset (&barriers[i], 0, sizeof(barriers[i]));
		barriers[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
		barriers[i].dstStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
		barriers[i].dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
		barriers[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		barriers[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
		barriers[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barriers[i].image = img->image;
		barriers[i].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		barriers[i].subresourceRange.levelCount = 1;
		barriers[i].subresourceRange.layerCount = 1;
	}

	/* the even set has the A images under the _A names, the odd set the B images */
#define IMG_DO(_name, _binding, ...) WriteImageDescriptors (images_sets[even_odd], _binding, VKPT_IMG_##_name);
	{
		int	even_odd;

		for (even_odd = 0; even_odd < 2; even_odd++)
		{
			LIST_IMAGES
			if (even_odd)
			{
				LIST_IMAGES_B_A
			}
			else
			{
				LIST_IMAGES_A_B
			}
		}
	}
#undef IMG_DO

	/* into the GENERAL layout, for good, and cleared: the _B images are
	 * read as last frame's before anything wrote them */
	memset (&dep, 0, sizeof(dep));
	dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
	dep.imageMemoryBarrierCount = NUM_VKPT_IMAGES;
	dep.pImageMemoryBarriers = barriers;
	cmd = VK_BeginUpload ();
	vkCmdPipelineBarrier2 (cmd, &dep);
	memset (&clear_color, 0, sizeof(clear_color));
	memset (&range, 0, sizeof(range));
	range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	range.levelCount = 1;
	range.layerCount = 1;
	for (i = 0; i < NUM_VKPT_IMAGES; i++)
		vkCmdClearColorImage (cmd, images[i].image, VK_IMAGE_LAYOUT_GENERAL, &clear_color, 1, &range);
	memset (&cleared, 0, sizeof(cleared));
	cleared.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
	cleared.srcStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
	cleared.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
	cleared.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
	cleared.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_READ_BIT;
	memset (&dep, 0, sizeof(dep));
	dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
	dep.memoryBarrierCount = 1;
	dep.pMemoryBarriers = &cleared;
	vkCmdPipelineBarrier2 (cmd, &dep);
	VK_EndUpload ();

	Con_DPrintf ("Render targets: %u x %u, %.1f MB\n", vk_image_extent.width, vk_image_extent.height,
		     (double)total / (1024.0 * 1024.0));
}

void VK_DestroyImages (void)
{
	int	i;

	for (i = 0; i < NUM_VKPT_IMAGES; i++)
	{
		if (images[i].view)
			vkDestroyImageView (vk.device, images[i].view, NULL);
		if (images[i].image)
			vmaDestroyImage (vk.allocator, images[i].image, images[i].allocation);
		memset (&images[i], 0, sizeof(images[i]));
	}
	vk_image_extent.width = vk_image_extent.height = 0;
}

static const char *FormatName (VkFormat format)
{
	switch (format)
	{
	case VK_FORMAT_R16G16B16A16_SFLOAT:	return "rgba16f";
	case VK_FORMAT_R32G32B32A32_SFLOAT:	return "rgba32f";
	case VK_FORMAT_R16G16_SFLOAT:		return "rg16f";
	case VK_FORMAT_R16_SFLOAT:		return "r16f";
	case VK_FORMAT_R32_SFLOAT:		return "r32f";
	case VK_FORMAT_R32_UINT:		return "r32ui";
	case VK_FORMAT_R32G32_UINT:		return "rg32ui";
	case VK_FORMAT_R16_UINT:		return "r16ui";
	case VK_FORMAT_R8G8_UNORM:		return "rg8";
	case VK_FORMAT_R8G8B8A8_UNORM:		return "rgba8";
	case VK_FORMAT_R16_UNORM:		return "r16";
	default:				return "?";
	}
}

static void VK_Images_f (void)
{
	VkDeviceSize	total = 0;
	int		i;

	if (!VK_ImagesReady ())
	{
		Con_Printf ("No render targets (the window has no size)\n");
		return;
	}
	for (i = 0; i < NUM_VKPT_IMAGES; i++)
	{
		Con_Printf ("%2d %-26s %-8s %5u x %-5u %6.1f MB\n", i, images[i].name, FormatName (images[i].format),
			    images[i].width, images[i].height, (double)images[i].size / (1024.0 * 1024.0));
		total += images[i].size;
	}
	Con_Printf ("%d render targets at %u x %u, %.1f MB; 3D frame %u uses the %s set\n", NUM_VKPT_IMAGES,
		    vk_image_extent.width, vk_image_extent.height, (double)total / (1024.0 * 1024.0),
		    vk_render_frame, (vk_render_frame & 1) ? "odd" : "even");
	Con_Printf ("Blue noise: %s %u x %u x %d layers, %.1f MB\n", FormatName (blue_noise.format),
		    blue_noise.width, blue_noise.height, NUM_BLUE_NOISE_TEX, (double)blue_noise.size / (1024.0 * 1024.0));
}

/* <exe folder>\blue_noise\HDR_RGBA_<index>.png, decoded to 16-bit RGBA */
static stbi_us *LoadBlueNoiseFile (int index)
{
	char		name[64], path[MAX_OSPATH];
	FILE		*f;
	long		size;
	stbi_uc		*file;
	stbi_us		*data = NULL;
	int		w, h, n;

	q_snprintf (name, sizeof(name), "blue_noise\\HDR_RGBA_%d.png", index);
	VK_ExePath (name, path, sizeof(path));
	f = fopen (path, "rb");
	if (f)
	{
		fseek (f, 0, SEEK_END);
		size = ftell (f);
		fseek (f, 0, SEEK_SET);
		file = (stbi_uc *) malloc (size > 0 ? size : 1);
		if (file && size > 0 && fread (file, 1, size, f) == (size_t)size)
			data = stbi_load_16_from_memory (file, (int)size, &w, &h, &n, 4);
		free (file);
		fclose (f);
		if (data && (w != BLUE_NOISE_RES || h != BLUE_NOISE_RES))
		{
			stbi_image_free (data);
			data = NULL;
		}
	}
	if (!data)
		Sys_Error ("Blue noise %s is missing or not %d x %d.\n\nThe blue_noise folder must be next to "
			   "hexenlicht.exe (it is created when building the hexenlicht target).",
			   path, BLUE_NOISE_RES, BLUE_NOISE_RES);
	return data;
}

/* Quake II RTX's load_blue_noise: every channel of every texture becomes
 * one layer; sampled (texelFetch) from the images' set */
static void LoadBlueNoise (void)
{
	const size_t		layer_texels = (size_t)BLUE_NOISE_RES * BLUE_NOISE_RES;
	vk_buffer_t		staging;
	uint16_t		*texels;
	VkImageCreateInfo	image_info;
	VmaAllocationCreateInfo	alloc_info;
	VmaAllocationInfo	allocated;
	VkImageViewCreateInfo	view_info;
	VkImageMemoryBarrier2	barrier;
	VkDependencyInfo	dep;
	VkBufferImageCopy	copy;
	VkCommandBuffer		cmd;
	int			i, k;
	size_t			j;

	VK_CreateBuffer (&staging, layer_texels * NUM_BLUE_NOISE_TEX * sizeof(uint16_t),
			 VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_UPLOAD);
	texels = (uint16_t *) staging.mapped;
	for (i = 0; i < NUM_BLUE_NOISE_TEX / 4; i++)
	{
		stbi_us	*data = LoadBlueNoiseFile (i);

		for (k = 0; k < 4; k++)
		{
			for (j = 0; j < layer_texels; j++)
				texels[(i * 4 + k) * layer_texels + j] = data[j * 4 + k];
		}
		stbi_image_free (data);
	}
	VK_CHECK (vmaFlushAllocation (vk.allocator, staging.allocation, 0, VK_WHOLE_SIZE));

	blue_noise.name = "BLUE_NOISE";
	blue_noise.format = VK_FORMAT_R16_UNORM;
	blue_noise.width = blue_noise.height = BLUE_NOISE_RES;
	memset (&image_info, 0, sizeof(image_info));
	image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	image_info.imageType = VK_IMAGE_TYPE_2D;
	image_info.format = blue_noise.format;
	image_info.extent.width = blue_noise.width;
	image_info.extent.height = blue_noise.height;
	image_info.extent.depth = 1;
	image_info.mipLevels = 1;
	image_info.arrayLayers = NUM_BLUE_NOISE_TEX;
	image_info.samples = VK_SAMPLE_COUNT_1_BIT;
	image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
	image_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	memset (&alloc_info, 0, sizeof(alloc_info));
	alloc_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
	VK_CHECK (vmaCreateImage (vk.allocator, &image_info, &alloc_info, &blue_noise.image, &blue_noise.allocation, &allocated));
	blue_noise.size = allocated.size;

	memset (&view_info, 0, sizeof(view_info));
	view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	view_info.image = blue_noise.image;
	view_info.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
	view_info.format = blue_noise.format;
	view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	view_info.subresourceRange.levelCount = 1;
	view_info.subresourceRange.layerCount = NUM_BLUE_NOISE_TEX;
	VK_CHECK (vkCreateImageView (vk.device, &view_info, NULL, &blue_noise.view));

	cmd = VK_BeginUpload ();
	memset (&barrier, 0, sizeof(barrier));
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
	barrier.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
	barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
	barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = blue_noise.image;
	barrier.subresourceRange = view_info.subresourceRange;
	memset (&dep, 0, sizeof(dep));
	dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
	dep.imageMemoryBarrierCount = 1;
	dep.pImageMemoryBarriers = &barrier;
	vkCmdPipelineBarrier2 (cmd, &dep);

	memset (&copy, 0, sizeof(copy));
	copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	copy.imageSubresource.layerCount = NUM_BLUE_NOISE_TEX;
	copy.imageExtent = image_info.extent;
	vkCmdCopyBufferToImage (cmd, staging.buffer, blue_noise.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

	barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
	barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
	barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
	barrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
	barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	vkCmdPipelineBarrier2 (cmd, &dep);
	VK_EndUpload ();
	VK_DestroyBuffer (&staging);
}

/* the blue noise's descriptor in both sets */
static void WriteBlueNoiseDescriptors (void)
{
	VkDescriptorImageInfo	info;
	VkWriteDescriptorSet	writes[2];

	memset (&info, 0, sizeof(info));
	info.sampler = sampler_nearest;
	info.imageView = blue_noise.view;
	info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	memset (writes, 0, sizeof(writes));
	writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	writes[0].dstSet = images_sets[0];
	writes[0].dstBinding = BINDING_OFFSET_BLUE_NOISE;
	writes[0].descriptorCount = 1;
	writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	writes[0].pImageInfo = &info;
	writes[1] = writes[0];
	writes[1].dstSet = images_sets[1];
	vkUpdateDescriptorSets (vk.device, 2, writes, 0, NULL);
}

void VK_InitImages (void)
{
	VkSamplerCreateInfo		sampler_info;
	VkDescriptorSetLayoutBinding	bindings[NUM_IMAGE_BINDINGS];
	VkDescriptorSetLayoutCreateInfo	layout_info;
	VkDescriptorPoolSize		pool_sizes[2];
	VkDescriptorPoolCreateInfo	pool_info;
	VkDescriptorSetAllocateInfo	alloc_info;
	VkDescriptorSetLayout		layouts[2];
	int				i;

	memset (&sampler_info, 0, sizeof(sampler_info));
	sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	sampler_info.magFilter = VK_FILTER_NEAREST;
	sampler_info.minFilter = VK_FILTER_NEAREST;
	sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	VK_CHECK (vkCreateSampler (vk.device, &sampler_info, NULL, &sampler_nearest));
	sampler_info.magFilter = VK_FILTER_LINEAR;
	sampler_info.minFilter = VK_FILTER_LINEAR;
	sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
	VK_CHECK (vkCreateSampler (vk.device, &sampler_info, NULL, &sampler_linear));

	memset (bindings, 0, sizeof(bindings));
	for (i = 0; i < NUM_IMAGES; i++)
	{
		bindings[i].binding = BINDING_OFFSET_IMAGES + i;
		bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
		bindings[i].descriptorCount = 1;
		bindings[i].stageFlags = VK_SHADER_STAGE_ALL;
		bindings[NUM_IMAGES + i].binding = BINDING_OFFSET_TEXTURES + i;
		bindings[NUM_IMAGES + i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		bindings[NUM_IMAGES + i].descriptorCount = 1;
		bindings[NUM_IMAGES + i].stageFlags = VK_SHADER_STAGE_ALL;
	}
	bindings[2 * NUM_IMAGES].binding = BINDING_OFFSET_BLUE_NOISE;
	bindings[2 * NUM_IMAGES].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	bindings[2 * NUM_IMAGES].descriptorCount = 1;
	bindings[2 * NUM_IMAGES].stageFlags = VK_SHADER_STAGE_ALL;
	memset (&layout_info, 0, sizeof(layout_info));
	layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	layout_info.bindingCount = NUM_IMAGE_BINDINGS;
	layout_info.pBindings = bindings;
	VK_CHECK (vkCreateDescriptorSetLayout (vk.device, &layout_info, NULL, &vk_images_set_layout));

	pool_sizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	pool_sizes[0].descriptorCount = 2 * NUM_IMAGES;
	pool_sizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	pool_sizes[1].descriptorCount = 2 * (NUM_IMAGES + 1);	/* + the blue noise */
	memset (&pool_info, 0, sizeof(pool_info));
	pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	pool_info.maxSets = 2;
	pool_info.poolSizeCount = 2;
	pool_info.pPoolSizes = pool_sizes;
	VK_CHECK (vkCreateDescriptorPool (vk.device, &pool_info, NULL, &images_pool));

	layouts[0] = layouts[1] = vk_images_set_layout;
	memset (&alloc_info, 0, sizeof(alloc_info));
	alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	alloc_info.descriptorPool = images_pool;
	alloc_info.descriptorSetCount = 2;
	alloc_info.pSetLayouts = layouts;
	VK_CHECK (vkAllocateDescriptorSets (vk.device, &alloc_info, images_sets));

	LoadBlueNoise ();
	WriteBlueNoiseDescriptors ();

	Cmd_AddCommand ("vk_images", VK_Images_f);
}

void VK_ShutdownImages (void)
{
	VK_DestroyImages ();
	if (blue_noise.view)
		vkDestroyImageView (vk.device, blue_noise.view, NULL);
	if (blue_noise.image)
		vmaDestroyImage (vk.allocator, blue_noise.image, blue_noise.allocation);
	memset (&blue_noise, 0, sizeof(blue_noise));
	if (images_pool)
		vkDestroyDescriptorPool (vk.device, images_pool, NULL);
	if (vk_images_set_layout)
		vkDestroyDescriptorSetLayout (vk.device, vk_images_set_layout, NULL);
	if (sampler_nearest)
		vkDestroySampler (vk.device, sampler_nearest, NULL);
	if (sampler_linear)
		vkDestroySampler (vk.device, sampler_linear, NULL);
	images_pool = VK_NULL_HANDLE;
	vk_images_set_layout = VK_NULL_HANDLE;
	sampler_nearest = sampler_linear = VK_NULL_HANDLE;
	memset (images_sets, 0, sizeof(images_sets));
}
