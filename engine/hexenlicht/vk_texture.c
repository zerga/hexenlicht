/* vk_texture.c -- texture manager for Hexenlicht
 *
 * GL_LoadTexture keeps the interface of Hammer of Thyrion's gl_draw.c,
 * because the engine and the reused model loader (gl_model.c) call it:
 * the same flags, the same name + CRC cache and the same conversion of
 * 8-bit palette images (transparent index 255 with fringe fixing, the
 * TEX_TRANSPARENT / TEX_HOLEY / TEX_SPECIAL_TRANS modes). The number it
 * returns is a slot in one bindless array of combined image samplers
 * (vk.texture_set); shaders index it directly.
 *
 * Differences to the GL version:
 * - Images are VK_FORMAT_R8G8B8A8_SRGB: shaders read linear colors, which
 *   the lighting needs. No power-of-two resampling, no gl_picmip.
 * - Mipmaps are generated on the GPU by blitting, which filters in linear
 *   space thanks to the sRGB format.
 * - Filtering follows the flags: mipmapped textures (world, models,
 *   sprites) are trilinear + anisotropic with repeat addressing; TEX_NEAREST
 *   is point sampled, everything else bilinear, both clamped to the edge.
 * - Slot 0 is a 1x1 white texture; freed slots point back to it, so the
 *   array never references a destroyed image.
 * - The array is updated after bind, so textures can be loaded while a
 *   frame is being recorded (the 2D code loads pics lazily while drawing).
 *
 * Copyright (C) 1996-1997  Id Software, Inc.
 * Copyright (C) 1997-1998  Raven Software Corp.
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
#include "hashindex.h"
#include "vk_local.h"

#define TEXTURE_FORMAT	VK_FORMAT_R8G8B8A8_SRGB

typedef struct
{
	char		identifier[MAX_QPATH];
	int		width, height;
	int		flags;
	unsigned short	crc;
	uint32_t	mip_levels;
	VkImage		image;
	VmaAllocation	allocation;
	VkImageView	view;
	VkSampler	sampler;
} vk_texture_t;

static vk_texture_t	textures[VK_MAX_TEXTURES];
static hashindex_t	hash_textures;

/* the engine's names for the texture count (host.c, gl_model.c, menu.c) */
int		numgltextures;		/* slots in use, including slot 0 */
int		gl_texlevel;		/* textures below this survive map changes */
qboolean	flush_textures;		/* set by the server when the map changes */
cvar_t		gl_purge_maptex = {"gl_purge_maptex", "1", CVAR_ARCHIVE};

/* the video menu shows the GL filter names (menu.c) */
int		gl_filter_idx = 4;	/* Bilinear */
GLfloat		gl_max_anisotropy = 1.0f;
glmode_t gl_texmodes[NUM_GL_FILTERS] =
{
	{ "GL_NEAREST",			GL_NEAREST,			GL_NEAREST },
	{ "GL_NEAREST_MIPMAP_NEAREST",	GL_NEAREST_MIPMAP_NEAREST,	GL_NEAREST },
	{ "GL_NEAREST_MIPMAP_LINEAR",	GL_NEAREST_MIPMAP_LINEAR,	GL_NEAREST },
	{ "GL_LINEAR",			GL_LINEAR,			GL_LINEAR  },
	{ "GL_LINEAR_MIPMAP_NEAREST",	GL_LINEAR_MIPMAP_NEAREST,	GL_LINEAR  },
	{ "GL_LINEAR_MIPMAP_LINEAR",	GL_LINEAR_MIPMAP_LINEAR,	GL_LINEAR  }
};

/* translucency table for TEX_SPECIAL_TRANS (gl_vidnt.c) */
static const int ColorIndex[16] = {
	0, 31, 47, 63, 79, 95, 111, 127, 143, 159, 175, 191, 199, 207, 223, 231
};
static const unsigned int ColorPercent[16] = {
	25, 51, 76, 102, 114, 127, 140, 153, 165, 178, 191, 204, 216, 229, 237, 247
};
/* alpha of the odd colors of TEX_TRANSPARENT: the GL renderer's default
 * r_wateralpha; real translucency is the renderer's business (epic E6) */
#define TRANSPARENT_ALPHA	((unsigned int)(255 * 0.33f))

#define MASK_RGB	0x00ffffffu	/* d_8to24table is R,G,B,A in memory */
#define SHIFT_A		24

static VkSampler	sampler_nearest;	/* point, clamp */
static VkSampler	sampler_linear;		/* bilinear, no mips, clamp */
static VkSampler	sampler_trilinear;	/* trilinear + anisotropy, repeat */
static VkDescriptorPool	texture_pool;
static VkCommandPool	upload_pool;
static VkCommandBuffer	upload_cmd;
static VkFence		upload_fence;


/* ==========================================================================
 * Descriptors
 * ========================================================================== */

static void VK_WriteTextureDescriptor (int slot, VkImageView view, VkSampler sampler)
{
	VkDescriptorImageInfo	image;
	VkWriteDescriptorSet	write;

	image.sampler = sampler;
	image.imageView = view;
	image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

	memset (&write, 0, sizeof(write));
	write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	write.dstSet = vk.texture_set;
	write.dstBinding = 0;
	write.dstArrayElement = (uint32_t)slot;
	write.descriptorCount = 1;
	write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	write.pImageInfo = &image;
	vkUpdateDescriptorSets (vk.device, 1, &write, 0, NULL);
}

static VkSampler VK_CreateSampler (VkFilter filter, VkSamplerMipmapMode mip_mode, qboolean mips,
				   VkSamplerAddressMode address, float anisotropy)
{
	VkSamplerCreateInfo	info;
	VkSampler		sampler;

	memset (&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	info.magFilter = filter;
	info.minFilter = filter;
	info.mipmapMode = mip_mode;
	info.addressModeU = address;
	info.addressModeV = address;
	info.addressModeW = address;
	info.anisotropyEnable = (anisotropy > 1.0f) ? VK_TRUE : VK_FALSE;
	info.maxAnisotropy = anisotropy;
	info.maxLod = mips ? VK_LOD_CLAMP_NONE : 0.0f;
	VK_CHECK (vkCreateSampler (vk.device, &info, NULL, &sampler));
	return sampler;
}

static VkSampler VK_SamplerForFlags (int flags)
{
	if (flags & TEX_NEAREST)
		return sampler_nearest;
	if (flags & TEX_MIPMAP)
		return sampler_trilinear;
	return sampler_linear;
}


/* ==========================================================================
 * Upload
 * ========================================================================== */

static void VK_ImageBarrier (VkImage image, uint32_t base_mip, uint32_t num_mips,
			     VkImageLayout old_layout, VkImageLayout new_layout,
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
	barrier.oldLayout = old_layout;
	barrier.newLayout = new_layout;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = image;
	barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barrier.subresourceRange.baseMipLevel = base_mip;
	barrier.subresourceRange.levelCount = num_mips;
	barrier.subresourceRange.layerCount = 1;

	memset (&dep, 0, sizeof(dep));
	dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
	dep.imageMemoryBarrierCount = 1;
	dep.pImageMemoryBarriers = &barrier;
	vkCmdPipelineBarrier2 (upload_cmd, &dep);
}

/* create t's image and view, upload RGBA data and generate the mipmaps;
 * waits for the GPU, so it is only for load time */
static void VK_UploadRGBA (vk_texture_t *t, const unsigned int *rgba)
{
	VkBufferCreateInfo		buffer_info;
	VkImageCreateInfo		image_info;
	VkImageViewCreateInfo		view_info;
	VmaAllocationCreateInfo		alloc_info;
	VmaAllocationInfo		staging_info;
	VkBuffer			staging;
	VmaAllocation			staging_alloc;
	VkCommandBufferBeginInfo	begin;
	VkBufferImageCopy		copy;
	VkCommandBufferSubmitInfo	cmd_info;
	VkSubmitInfo2			submit;
	VkDeviceSize			size = (VkDeviceSize)t->width * t->height * 4;
	uint32_t			i, w, h;

	t->mip_levels = 1;
	if (t->flags & TEX_MIPMAP)
	{
		for (w = q_max(t->width, t->height); w > 1; w >>= 1)
			t->mip_levels++;
	}

	/* staging buffer with the pixels */
	memset (&buffer_info, 0, sizeof(buffer_info));
	buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	buffer_info.size = size;
	buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	memset (&alloc_info, 0, sizeof(alloc_info));
	alloc_info.usage = VMA_MEMORY_USAGE_AUTO;
	alloc_info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
	VK_CHECK (vmaCreateBuffer (vk.allocator, &buffer_info, &alloc_info, &staging, &staging_alloc, &staging_info));
	memcpy (staging_info.pMappedData, rgba, (size_t)size);
	VK_CHECK (vmaFlushAllocation (vk.allocator, staging_alloc, 0, VK_WHOLE_SIZE));

	/* the image */
	memset (&image_info, 0, sizeof(image_info));
	image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	image_info.imageType = VK_IMAGE_TYPE_2D;
	image_info.format = TEXTURE_FORMAT;
	image_info.extent.width = (uint32_t)t->width;
	image_info.extent.height = (uint32_t)t->height;
	image_info.extent.depth = 1;
	image_info.mipLevels = t->mip_levels;
	image_info.arrayLayers = 1;
	image_info.samples = VK_SAMPLE_COUNT_1_BIT;
	image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
	image_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
			   ((t->mip_levels > 1) ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0);
	image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	memset (&alloc_info, 0, sizeof(alloc_info));
	alloc_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
	VK_CHECK (vmaCreateImage (vk.allocator, &image_info, &alloc_info, &t->image, &t->allocation, NULL));

	/* commands: copy level 0, blit each further level from the previous */
	VK_CHECK (vkResetCommandPool (vk.device, upload_pool, 0));
	memset (&begin, 0, sizeof(begin));
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	VK_CHECK (vkBeginCommandBuffer (upload_cmd, &begin));

	VK_ImageBarrier (t->image, 0, t->mip_levels, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			 VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT,
			 VK_ACCESS_2_TRANSFER_WRITE_BIT);

	memset (&copy, 0, sizeof(copy));
	copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	copy.imageSubresource.layerCount = 1;
	copy.imageExtent = image_info.extent;
	vkCmdCopyBufferToImage (upload_cmd, staging, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

	w = (uint32_t)t->width;
	h = (uint32_t)t->height;
	for (i = 1; i < t->mip_levels; i++)
	{
		VkImageBlit	blit;

		VK_ImageBarrier (t->image, i - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				 VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
				 VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);

		memset (&blit, 0, sizeof(blit));
		blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		blit.srcSubresource.mipLevel = i - 1;
		blit.srcSubresource.layerCount = 1;
		blit.srcOffsets[1].x = (int32_t)w;
		blit.srcOffsets[1].y = (int32_t)h;
		blit.srcOffsets[1].z = 1;
		w = q_max(w >> 1, 1u);
		h = q_max(h >> 1, 1u);
		blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		blit.dstSubresource.mipLevel = i;
		blit.dstSubresource.layerCount = 1;
		blit.dstOffsets[1].x = (int32_t)w;
		blit.dstOffsets[1].y = (int32_t)h;
		blit.dstOffsets[1].z = 1;
		vkCmdBlitImage (upload_cmd, t->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
	}

	/* all levels but the last are TRANSFER_SRC now, the last TRANSFER_DST */
	if (t->mip_levels > 1)
	{
		VK_ImageBarrier (t->image, 0, t->mip_levels - 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
				 VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
				 VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
	}
	VK_ImageBarrier (t->image, t->mip_levels - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			 VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
			 VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

	VK_CHECK (vkEndCommandBuffer (upload_cmd));

	memset (&cmd_info, 0, sizeof(cmd_info));
	cmd_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
	cmd_info.commandBuffer = upload_cmd;
	memset (&submit, 0, sizeof(submit));
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
	submit.commandBufferInfoCount = 1;
	submit.pCommandBufferInfos = &cmd_info;
	VK_CHECK (vkQueueSubmit2 (vk.queue, 1, &submit, upload_fence));
	VK_CHECK (vkWaitForFences (vk.device, 1, &upload_fence, VK_TRUE, UINT64_MAX));
	VK_CHECK (vkResetFences (vk.device, 1, &upload_fence));

	vmaDestroyBuffer (vk.allocator, staging, staging_alloc);

	memset (&view_info, 0, sizeof(view_info));
	view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	view_info.image = t->image;
	view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
	view_info.format = TEXTURE_FORMAT;
	view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	view_info.subresourceRange.levelCount = t->mip_levels;
	view_info.subresourceRange.layerCount = 1;
	VK_CHECK (vkCreateImageView (vk.device, &view_info, NULL, &t->view));
}

static void VK_DestroyTexture (int slot)
{
	vk_texture_t	*t = &textures[slot];

	if (t->view)
		vkDestroyImageView (vk.device, t->view, NULL);
	if (t->image)
		vmaDestroyImage (vk.allocator, t->image, t->allocation);
	memset (t, 0, sizeof(*t));
}


/* ==========================================================================
 * 8-bit conversion (GL_Upload8 of gl_draw.c)
 * ========================================================================== */

static void VK_Convert8 (const byte *data, unsigned int *trans, vk_texture_t *t)
{
	int	i, p, s = t->width * t->height;

	if (t->flags & (TEX_ALPHA|TEX_TRANSPARENT|TEX_HOLEY|TEX_SPECIAL_TRANS))
	{
		/* if there are no transparent pixels, it is not an alpha
		 * texture even if it was flagged as TEX_ALPHA */
		qboolean noalpha = !(t->flags & (TEX_TRANSPARENT|TEX_HOLEY|TEX_SPECIAL_TRANS));

		for (i = 0; i < s; i++)
		{
			p = data[i];
			trans[i] = d_8to24table[p];

			if (p == 255)
			{
				noalpha = false;
				/* transparent: take the color of a neighbor to avoid
				 * dark fringes when filtering (from Quake II) */
				if (i > t->width && data[i-t->width] != 255)
					p = data[i-t->width];
				else if (i < s-t->width && data[i+t->width] != 255)
					p = data[i+t->width];
				else if (i > 0 && data[i-1] != 255)
					p = data[i-1];
				else if (i < s-1 && data[i+1] != 255)
					p = data[i+1];
				else
					p = 0;
				trans[i] = (d_8to24table[p] & MASK_RGB) | (trans[i] & ~MASK_RGB);
			}

			if (t->flags & TEX_TRANSPARENT)
			{
				p = data[i];
				if (p == 0)
					trans[i] &= MASK_RGB;
				else if (p & 1)
					trans[i] = (trans[i] & MASK_RGB) | (TRANSPARENT_ALPHA << SHIFT_A);
				else
					trans[i] |= ~MASK_RGB;
			}
			else if (t->flags & TEX_HOLEY)
			{
				if (data[i] == 0)
					trans[i] &= MASK_RGB;
			}
			else if (t->flags & TEX_SPECIAL_TRANS)
			{
				p = data[i];
				trans[i] = d_8to24table[ColorIndex[p>>4]] & MASK_RGB;
				trans[i] |= (ColorPercent[p&15] & 0xff) << SHIFT_A;
			}
		}

		if (noalpha)
			t->flags &= ~TEX_ALPHA;
		if (t->flags & (TEX_TRANSPARENT|TEX_HOLEY|TEX_SPECIAL_TRANS))
			t->flags |= TEX_ALPHA;
	}
	else
	{
		for (i = 0; i < s; i++)
			trans[i] = d_8to24table[data[i]];
	}
}


/* ==========================================================================
 * GL_LoadTexture
 * ========================================================================== */

GLuint GL_LoadTexture (const char *identifier, byte *data, int width, int height, int flags)
{
	int		i, size, key, slot, mark;
	unsigned short	crc;
	vk_texture_t	*t;
	unsigned int	*rgba;

#if !defined (H2W)
	if (cls.state == ca_dedicated)
		return GL_UNUSED_TEXTURE;
#endif
	if (width <= 0 || height <= 0)
		Sys_Error ("%s: bad size %dx%d for %s", __thisfunc__, width, height, identifier);

	size = width * height;
	if (flags & TEX_RGBA)
		size *= 4;
	crc = CRC_Block (data, size);

	key = Hash_GenerateKeyString (&hash_textures, identifier, true);
	slot = -1;
	if (identifier[0])
	{
		/* texture already present? */
		for (i = Hash_First(&hash_textures, key); i != -1; i = Hash_Next(&hash_textures, i))
		{
			t = &textures[i];
			if (strcmp (identifier, t->identifier))
				continue;
			if (crc == t->crc && width == t->width && height == t->height &&
			    (t->flags & TEX_MIPMAP) == (flags & TEX_MIPMAP))
				return (GLuint)i;	/* the same is present */
			/* not the same: replace the image in this slot */
			Con_DPrintf ("Texture cache mismatch: %d, %s, reloading\n", i, identifier);
			vkDeviceWaitIdle (vk.device);
			VK_WriteTextureDescriptor (i, textures[0].view, textures[0].sampler);
			VK_DestroyTexture (i);
			slot = i;
			break;
		}
	}

	if (slot < 0)
	{
		if (numgltextures >= VK_MAX_TEXTURES)
			Sys_Error ("%s: cache full, max is %i textures.", __thisfunc__, VK_MAX_TEXTURES);
		slot = numgltextures++;
		Hash_Add (&hash_textures, key, slot);
	}

	t = &textures[slot];
	q_strlcpy (t->identifier, identifier, MAX_QPATH);
	t->width = width;
	t->height = height;
	t->flags = flags;
	t->crc = crc;

	if (flags & TEX_RGBA)
	{
		VK_UploadRGBA (t, (unsigned int *)data);
	}
	else
	{
		mark = Hunk_LowMark ();
		rgba = (unsigned int *) Hunk_AllocName (width * height * sizeof(unsigned int), "texbuf_upload8");
		VK_Convert8 (data, rgba, t);
		VK_UploadRGBA (t, rgba);
		Hunk_FreeToLowMark (mark);
	}

	t->sampler = VK_SamplerForFlags (t->flags);
	VK_WriteTextureDescriptor (slot, t->view, t->sampler);
	return (GLuint)slot;
}

GLuint GL_LoadPicTexture (qpic_t *pic)
{
	return GL_LoadTexture ("", pic->data, pic->width, pic->height, TEX_ALPHA|TEX_NEAREST);
}

/* free all textures from slot last_tex on (gl_rmisc.c) */
void D_ClearOpenGLTextures (int last_tex)
{
	int	i, key;

	if (last_tex < 1)
		last_tex = 1;	/* slot 0 is the white default */
	if (last_tex >= numgltextures)
		return;

	vkDeviceWaitIdle (vk.device);	/* frames in flight may still sample them */
	for (i = last_tex; i < numgltextures; i++)
	{
		key = Hash_GenerateKeyString (&hash_textures, textures[i].identifier, true);
		Hash_Remove (&hash_textures, key, i);
		VK_WriteTextureDescriptor (i, textures[0].view, textures[0].sampler);
		VK_DestroyTexture (i);
	}
	numgltextures = last_tex;

	Con_DPrintf ("Purged textures\n");
}

/* on a new map: drop the previous map's textures (gl_rmisc.c) */
void D_FlushCaches (void)
{
	if (numgltextures - gl_texlevel > 0 && flush_textures && gl_purge_maptex.integer)
		D_ClearOpenGLTextures (gl_texlevel);
}


/* ==========================================================================
 * Info command
 * ========================================================================== */

static void VK_Textures_f (void)
{
	int		i, mipped = 0, alpha = 0;
	double		bytes = 0;
	qboolean	list = (Cmd_Argc() > 1 && !q_strcasecmp (Cmd_Argv(1), "list"));

	for (i = 0; i < numgltextures; i++)
	{
		const vk_texture_t	*t = &textures[i];
		double			b = (double)t->width * t->height * 4 * ((t->mip_levels > 1) ? 4.0 / 3.0 : 1.0);

		bytes += b;
		if (t->mip_levels > 1)
			mipped++;
		if (t->flags & TEX_ALPHA)
			alpha++;
		if (list)
			Con_Printf ("%4d %4dx%-4d %2u mips %s %s\n", i, t->width, t->height, t->mip_levels,
					(t->flags & TEX_ALPHA) ? "a" : " ", t->identifier[0] ? t->identifier : "(unnamed)");
	}
	Con_Printf ("%d textures (%d mipmapped, %d with alpha), %.1f MB; %d kept across maps\n",
			numgltextures, mipped, alpha, bytes / (1024.0 * 1024.0), gl_texlevel);
}


/* ==========================================================================
 * Init / shutdown
 * ========================================================================== */

void VK_InitTextures (void)
{
	VkDescriptorSetLayoutBinding		binding;
	VkDescriptorBindingFlags		binding_flags;
	VkDescriptorSetLayoutBindingFlagsCreateInfo flags_info;
	VkDescriptorSetLayoutCreateInfo		layout_info;
	VkDescriptorPoolSize			pool_size;
	VkDescriptorPoolCreateInfo		pool_info;
	VkDescriptorSetAllocateInfo		alloc_info;
	VkCommandPoolCreateInfo			cmd_pool_info;
	VkCommandBufferAllocateInfo		cmd_info;
	VkFenceCreateInfo			fence_info;
	VkPhysicalDeviceFeatures		features;
	VkPhysicalDeviceVulkan12Properties	props12;
	VkPhysicalDeviceProperties2		props;
	unsigned int				white = 0xffffffffu;
	int					i;

	Cvar_RegisterVariable (&gl_purge_maptex);
	Cmd_AddCommand ("vk_textures", VK_Textures_f);
	Hash_Allocate (&hash_textures, VK_MAX_TEXTURES);

	memset (&props12, 0, sizeof(props12));
	props12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES;
	memset (&props, 0, sizeof(props));
	props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
	props.pNext = &props12;
	vkGetPhysicalDeviceProperties2 (vk.physical_device, &props);
	if (props12.maxDescriptorSetUpdateAfterBindSampledImages < VK_MAX_TEXTURES ||
	    props12.maxPerStageDescriptorUpdateAfterBindSampledImages < VK_MAX_TEXTURES)
		Sys_Error ("The Vulkan device supports only %u bindless textures, %d needed",
			   q_min (props12.maxDescriptorSetUpdateAfterBindSampledImages,
				  props12.maxPerStageDescriptorUpdateAfterBindSampledImages), VK_MAX_TEXTURES);

	/* samplers */
	vkGetPhysicalDeviceFeatures (vk.physical_device, &features);
	gl_max_anisotropy = features.samplerAnisotropy ? vk.props.limits.maxSamplerAnisotropy : 1.0f;
	sampler_nearest = VK_CreateSampler (VK_FILTER_NEAREST, VK_SAMPLER_MIPMAP_MODE_NEAREST, false,
					    VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, 1.0f);
	sampler_linear = VK_CreateSampler (VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_NEAREST, false,
					   VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, 1.0f);
	sampler_trilinear = VK_CreateSampler (VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_LINEAR, true,
					      VK_SAMPLER_ADDRESS_MODE_REPEAT, q_min (gl_max_anisotropy, 16.0f));

	/* the bindless array: binding 0, all stages, updated while in use */
	memset (&binding, 0, sizeof(binding));
	binding.binding = 0;
	binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	binding.descriptorCount = VK_MAX_TEXTURES;
	binding.stageFlags = VK_SHADER_STAGE_ALL;
	binding_flags = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
			VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
			VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;
	memset (&flags_info, 0, sizeof(flags_info));
	flags_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
	flags_info.bindingCount = 1;
	flags_info.pBindingFlags = &binding_flags;
	memset (&layout_info, 0, sizeof(layout_info));
	layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	layout_info.pNext = &flags_info;
	layout_info.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
	layout_info.bindingCount = 1;
	layout_info.pBindings = &binding;
	VK_CHECK (vkCreateDescriptorSetLayout (vk.device, &layout_info, NULL, &vk.texture_set_layout));

	pool_size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	pool_size.descriptorCount = VK_MAX_TEXTURES;
	memset (&pool_info, 0, sizeof(pool_info));
	pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
	pool_info.maxSets = 1;
	pool_info.poolSizeCount = 1;
	pool_info.pPoolSizes = &pool_size;
	VK_CHECK (vkCreateDescriptorPool (vk.device, &pool_info, NULL, &texture_pool));

	memset (&alloc_info, 0, sizeof(alloc_info));
	alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	alloc_info.descriptorPool = texture_pool;
	alloc_info.descriptorSetCount = 1;
	alloc_info.pSetLayouts = &vk.texture_set_layout;
	VK_CHECK (vkAllocateDescriptorSets (vk.device, &alloc_info, &vk.texture_set));

	/* uploads */
	memset (&cmd_pool_info, 0, sizeof(cmd_pool_info));
	cmd_pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	cmd_pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
	cmd_pool_info.queueFamilyIndex = vk.queue_family;
	VK_CHECK (vkCreateCommandPool (vk.device, &cmd_pool_info, NULL, &upload_pool));
	memset (&cmd_info, 0, sizeof(cmd_info));
	cmd_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	cmd_info.commandPool = upload_pool;
	cmd_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	cmd_info.commandBufferCount = 1;
	VK_CHECK (vkAllocateCommandBuffers (vk.device, &cmd_info, &upload_cmd));
	memset (&fence_info, 0, sizeof(fence_info));
	fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	VK_CHECK (vkCreateFence (vk.device, &fence_info, NULL, &upload_fence));

	/* slot 0: white, and every slot starts out pointing at it */
	textures[0].width = textures[0].height = 1;
	textures[0].flags = TEX_RGBA | TEX_LINEAR;
	q_strlcpy (textures[0].identifier, "*white", MAX_QPATH);
	VK_UploadRGBA (&textures[0], &white);
	textures[0].sampler = sampler_linear;
	for (i = 0; i < VK_MAX_TEXTURES; i++)
		VK_WriteTextureDescriptor (i, textures[0].view, textures[0].sampler);
	Hash_Add (&hash_textures, Hash_GenerateKeyString (&hash_textures, textures[0].identifier, true), 0);
	numgltextures = 1;
}

void VK_ShutdownTextures (void)
{
	int	i;

	if (!vk.device)
		return;
	vkDeviceWaitIdle (vk.device);
	for (i = 0; i < numgltextures; i++)
		VK_DestroyTexture (i);
	numgltextures = 0;
	Hash_Free (&hash_textures);

	if (upload_fence)
		vkDestroyFence (vk.device, upload_fence, NULL);
	if (upload_pool)
		vkDestroyCommandPool (vk.device, upload_pool, NULL);
	if (texture_pool)
		vkDestroyDescriptorPool (vk.device, texture_pool, NULL);
	if (vk.texture_set_layout)
		vkDestroyDescriptorSetLayout (vk.device, vk.texture_set_layout, NULL);
	if (sampler_nearest)
		vkDestroySampler (vk.device, sampler_nearest, NULL);
	if (sampler_linear)
		vkDestroySampler (vk.device, sampler_linear, NULL);
	if (sampler_trilinear)
		vkDestroySampler (vk.device, sampler_trilinear, NULL);
	upload_fence = VK_NULL_HANDLE;
	upload_pool = VK_NULL_HANDLE;
	texture_pool = VK_NULL_HANDLE;
	vk.texture_set_layout = VK_NULL_HANDLE;
	vk.texture_set = VK_NULL_HANDLE;
	sampler_nearest = sampler_linear = sampler_trilinear = VK_NULL_HANDLE;
}
