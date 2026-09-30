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
 * - Images are VK_FORMAT_R8G8B8A8_UNORM (4.17; VK_FORMAT_R8G8B8A8_SRGB
 *   before; image files also BC7_UNORM_BLOCK and BC5_UNORM_BLOCK, 5.2,
 *   UNORM too): shaders read the 8-bit colors as they are and turn them into
 *   linear light where they are colors (shaders/transfer.glsl: a 2.2
 *   power, GL's product with the lightmap; the sRGB curve with r_srgb 1).
 *   VK_ColorToLinear is the same on the CPU (light colors, the sky's and
 *   the lava's averages, particles, emissive skins, averaged screenshots).
 *   No power-of-two resampling, no gl_picmip.
 * - Mipmaps are generated on the GPU by blitting, which filters the 8-bit
 *   colors, as GL's mipmaps and filtering did. Image files (5.2,
 *   vk_imagefile.c, VK_LoadImageTexture) bring their levels; with
 *   TEX_MIPMAP an uncompressed one gets the rest of its chain blitted, a
 *   BC7 or BC5 one keeps what it has (the GPU can't write them).
 * - Filtering follows the flags: mipmapped textures (world, models,
 *   sprites) are trilinear + anisotropic with repeat addressing; TEX_NEAREST
 *   is point sampled, everything else bilinear, both clamped to the edge
 *   (repeating with Hexenlicht's TEX_REPEAT: the 2D backtile, the sky).
 * - Slot 0 is a 1x1 white texture; freed slots point back to it, so the
 *   array never references a destroyed image.
 * - TEX_SPECIAL_TRANS alpha is stored as opacity (GL blends those skins
 *   with inverted alpha), so alpha means opacity in every texture.
 * - The array is updated after bind, so textures can be loaded while a
 *   frame is being recorded (the 2D code loads pics lazily while drawing).
 * - The 8-bit pixels of alias model skins with bright texels (a channel of
 *   at least VK_EMISSIVE_THRESHOLD) are kept with the slot (4.5): the
 *   light models' flames get an emissive texture made from them when
 *   first shown (vk_emissive.c, VK_TextureRGBA).
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

#define TEXTURE_FORMAT	VK_FORMAT_R8G8B8A8_UNORM

typedef struct
{
	char		identifier[MAX_QPATH];
	int		width, height;
	int		flags;
	unsigned short	crc;
	uint32_t	mip_levels;
	VkFormat	format;		/* TEXTURE_FORMAT, or an image file's BC7 or BC5 (5.2) */
	VkImage		image;
	VmaAllocation	allocation;
	VkImageView	view;
	VkSampler	sampler;
	byte		*pixels8;	/* a bright skin's 8-bit pixels (see the top), NULL = not kept */
} vk_texture_t;

static vk_texture_t	textures[VK_MAX_TEXTURES];
static hashindex_t	hash_textures;

/* the engine's names for the texture count (host.c, gl_model.c, menu.c) */
int		numgltextures;		/* slots in use, including slot 0 */
int		gl_texlevel;		/* textures below this survive map changes */
qboolean	flush_textures;		/* set by the server when the map changes */
cvar_t		gl_purge_maptex = {"gl_purge_maptex", "1", CVAR_ARCHIVE};

static void ColorsChanged (cvar_t *var);

/* 4.17: the 8-bit colors are a COLOR_GAMMA power of linear light (see the
 * top), or with r_srgb 1 the sRGB curve's */
#define COLOR_GAMMA	2.2f	/* shaders/transfer.glsl's */
static cvar_t	r_srgb = {"r_srgb", "0", CVAR_ARCHIVE};

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

/* translucency table for TEX_SPECIAL_TRANS and colorshade tints (gl_vidnt.c) */
const int ColorIndex[16] = {
	0, 31, 47, 63, 79, 95, 111, 127, 143, 159, 175, 191, 199, 207, 223, 231
};
const unsigned int ColorPercent[16] = {
	25, 51, 76, 102, 114, 127, 140, 153, 165, 178, 191, 204, 216, 229, 237, 247
};
/* alpha of the odd colors of TEX_TRANSPARENT: the GL renderer's default
 * r_wateralpha; real translucency is the renderer's business (epic E6) */
#define TRANSPARENT_ALPHA	((unsigned int)(255 * 0.33f))

#define MASK_RGB	0x00ffffffu	/* d_8to24table is R,G,B,A in memory */
#define SHIFT_A		24

static VkSampler	sampler_nearest;	/* point, clamp */
static VkSampler	sampler_nearest_repeat;	/* point, repeat (TEX_REPEAT: the 2D backtile) */
static VkSampler	sampler_linear;		/* bilinear, no mips, clamp */
static VkSampler	sampler_linear_repeat;	/* bilinear, no mips, repeat (TEX_REPEAT: the sky, 4.6) */
static VkSampler	sampler_trilinear;	/* trilinear + anisotropy, repeat */
static VkDescriptorPool	texture_pool;
static VkCommandBuffer	upload_cmd;		/* while uploading: from VK_BeginUpload */


/* ==========================================================================
 * Colors (4.17): 8-bit colors and linear light, as shaders/transfer.glsl
 * ========================================================================== */

/* the sRGB curve (r_srgb 1), else the COLOR_GAMMA power */
qboolean VK_ColorsSRGB (void)
{
	return r_srgb.integer != 0;
}

/* an 8-bit color (0-1; above 1 too: jsh2color's colors reach 1.08) as
 * linear light, by the sRGB curve or the COLOR_GAMMA power */
float VK_ColorToLinearAs (float c, qboolean srgb)
{
	c = q_max (c, 0.0f);
	if (srgb)
		return (c <= 0.04045f) ? c / 12.92f : powf ((c + 0.055f) / 1.055f, 2.4f);
	return powf (c, COLOR_GAMMA);
}

/* the same by r_srgb */
float VK_ColorToLinear (float c)
{
	return VK_ColorToLinearAs (c, VK_ColorsSRGB ());
}

/* linear light as an 8-bit color, clamped to 0-1 */
float VK_LinearToColor (float x)
{
	x = q_max (0.0f, q_min (1.0f, x));
	if (VK_ColorsSRGB ())
		return (x <= 0.0031308f) ? x * 12.92f : 1.055f * powf (x, 1.0f / 2.4f) - 0.055f;
	return powf (x, 1.0f / COLOR_GAMMA);
}

/* VK_ColorToLinear of the bytes 0-255 */
const float *VK_ColorTable (void)
{
	static float	table[2][256];
	static qboolean	made[2];
	int		s = VK_ColorsSRGB () ? 1 : 0, i;

	if (!made[s])
	{
		for (i = 0; i < 256; i++)
			table[s][i] = VK_ColorToLinearAs (i / 255.0f, s != 0);
		made[s] = true;
	}
	return table[s];
}

/* the shaders follow the UBO's color_srgb (vk_ubo.c), the CPU's colors
 * where they are used (dynamic lights, particles, the sky's average,
 * screenshots) or here: the map lights' colors and the lava's lights;
 * the emissive skins with the next map (vk_emissive.c) */
static void ColorsChanged (cvar_t *var)
{
	(void)var;
	VK_MapLightColorsChanged ();
}


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
		return (flags & TEX_REPEAT) ? sampler_nearest_repeat : sampler_nearest;
	if (flags & TEX_MIPMAP)
		return sampler_trilinear;
	return (flags & TEX_REPEAT) ? sampler_linear_repeat : sampler_linear;
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

/* creates t's image and view (width, height and flags set) from num_levels
 * levels of an image in format, level i at data + offset[i], size[i] bytes
 * (level 0 first); with TEX_MIPMAP an uncompressed image gets the rest of
 * its mip chain blitted from the last of them (a compressed one keeps the
 * levels it has: the GPU can't write them, 5.2). GL_LoadTexture's images
 * have one level. Waits for the GPU: load time, or the first use of a
 * picture or of an emissive skin while a frame is recorded */
static void VK_UploadLevels (vk_texture_t *t, VkFormat format, const byte *data, uint32_t num_levels,
			     const size_t *offset, const size_t *size)
{
	VkImageCreateInfo		image_info;
	VkImageViewCreateInfo		view_info;
	VmaAllocationCreateInfo		alloc_info;
	vk_buffer_t			staging;
	VkBufferImageCopy		copy;
	VkDeviceSize			total = 0;
	uint32_t			i, w, h;

	t->mip_levels = 1;
	if ((t->flags & TEX_MIPMAP) && format == TEXTURE_FORMAT)
	{
		for (w = q_max(t->width, t->height); w > 1; w >>= 1)
			t->mip_levels++;
	}
	t->mip_levels = q_max(t->mip_levels, num_levels);
	t->format = format;

	/* staging buffer with the levels, where they are in data */
	for (i = 0; i < num_levels; i++)
		total = q_max(total, (VkDeviceSize)(offset[i] + size[i]));
	VK_CreateBuffer (&staging, total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_UPLOAD);
	memcpy (staging.mapped, data, (size_t)total);
	VK_CHECK (vmaFlushAllocation (vk.allocator, staging.allocation, 0, VK_WHOLE_SIZE));

	/* the image */
	memset (&image_info, 0, sizeof(image_info));
	image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	image_info.imageType = VK_IMAGE_TYPE_2D;
	image_info.format = format;
	image_info.extent.width = (uint32_t)t->width;
	image_info.extent.height = (uint32_t)t->height;
	image_info.extent.depth = 1;
	image_info.mipLevels = t->mip_levels;
	image_info.arrayLayers = 1;
	image_info.samples = VK_SAMPLE_COUNT_1_BIT;
	image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
	image_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
			   ((t->mip_levels > num_levels) ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0);
	image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	memset (&alloc_info, 0, sizeof(alloc_info));
	alloc_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
	VK_CHECK (vmaCreateImage (vk.allocator, &image_info, &alloc_info, &t->image, &t->allocation, NULL));

	/* commands: copy the levels given, blit each further level from the previous */
	upload_cmd = VK_BeginUpload ();

	VK_ImageBarrier (t->image, 0, t->mip_levels, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			 VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT,
			 VK_ACCESS_2_TRANSFER_WRITE_BIT);

	for (i = 0; i < num_levels; i++)
	{
		memset (&copy, 0, sizeof(copy));
		copy.bufferOffset = (VkDeviceSize)offset[i];
		copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		copy.imageSubresource.mipLevel = i;
		copy.imageSubresource.layerCount = 1;
		copy.imageExtent.width = q_max((uint32_t)t->width >> i, 1u);
		copy.imageExtent.height = q_max((uint32_t)t->height >> i, 1u);
		copy.imageExtent.depth = 1;
		vkCmdCopyBufferToImage (upload_cmd, staging.buffer, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
	}

	w = q_max((uint32_t)t->width >> (num_levels - 1), 1u);
	h = q_max((uint32_t)t->height >> (num_levels - 1), 1u);
	for (i = num_levels; i < t->mip_levels; i++)
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

	/* the levels blitted from are TRANSFER_SRC now, the others TRANSFER_DST */
	if (t->mip_levels > num_levels)
	{
		VK_ImageBarrier (t->image, num_levels - 1, t->mip_levels - num_levels, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
				 VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
				 VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
		if (num_levels > 1)
			VK_ImageBarrier (t->image, 0, num_levels - 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
					 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
					 VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
					 VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
		VK_ImageBarrier (t->image, t->mip_levels - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
				 VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
				 VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
	}
	else
	{
		VK_ImageBarrier (t->image, 0, t->mip_levels, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
				 VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
				 VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
	}

	VK_EndUpload ();
	upload_cmd = VK_NULL_HANDLE;
	VK_DestroyBuffer (&staging);

	memset (&view_info, 0, sizeof(view_info));
	view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	view_info.image = t->image;
	view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
	view_info.format = format;
	view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	view_info.subresourceRange.levelCount = t->mip_levels;
	view_info.subresourceRange.layerCount = 1;
	VK_CHECK (vkCreateImageView (vk.device, &view_info, NULL, &t->view));
}

static void VK_UploadRGBA (vk_texture_t *t, const unsigned int *rgba)
{
	size_t	offset = 0, size = (size_t)t->width * t->height * 4;

	VK_UploadLevels (t, TEXTURE_FORMAT, (const byte *)rgba, 1, &offset, &size);
}

static void VK_DestroyTexture (int slot)
{
	vk_texture_t	*t = &textures[slot];

	if (t->view)
		vkDestroyImageView (vk.device, t->view, NULL);
	if (t->image)
		vmaDestroyImage (vk.allocator, t->image, t->allocation);
	free (t->pixels8);
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
				/* GL blends these with (1 - alpha, alpha): alpha is how much
				 * shows through. Stored as opacity, like every other texture. */
				p = data[i];
				trans[i] = d_8to24table[ColorIndex[p>>4]] & MASK_RGB;
				trans[i] |= ((255 - ColorPercent[p&15]) & 0xff) << SHIFT_A;
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


/* 8-bit pixels converted as GL_LoadTexture uploads them with *flags, R,G,B,A
 * in memory (5.4, the texture export: vk_export.c); *flags as the
 * conversion settles them (TEX_ALPHA) */
void VK_Convert8Pixels (const byte *data, int width, int height, int *flags, unsigned int *rgba)
{
	vk_texture_t	t;

	memset (&t, 0, sizeof(t));
	t.width = width;
	t.height = height;
	t.flags = *flags;
	VK_Convert8 (data, rgba, &t);
	*flags = t.flags;
}

/* does a texel have a channel of at least VK_EMISSIVE_THRESHOLD (of 255)? */
static qboolean HasBrightTexels (const unsigned int *rgba, int count)
{
	int	i;

	for (i = 0; i < count; i++)
	{
		unsigned int	c = rgba[i];	/* R,G,B,A in memory */

		if ((c & 0xff) >= VK_EMISSIVE_THRESHOLD || ((c >> 8) & 0xff) >= VK_EMISSIVE_THRESHOLD ||
		    ((c >> 16) & 0xff) >= VK_EMISSIVE_THRESHOLD)
			return true;
	}
	return false;
}

/* the kept pixels of a bright skin (see the top) as GL_LoadTexture converted
 * them, R,G,B,A in memory (free them); NULL = none kept */
unsigned int *VK_TextureRGBA (int slot, int *width, int *height)
{
	vk_texture_t	t;
	unsigned int	*rgba;

	if (slot <= 0 || slot >= numgltextures || !textures[slot].pixels8)
		return NULL;
	t = textures[slot];	/* VK_Convert8 settles the flags: its copy */
	rgba = (unsigned int *) malloc ((size_t)t.width * t.height * sizeof(unsigned int));
	if (!rgba)
		return NULL;
	VK_Convert8 (t.pixels8, rgba, &t);
	*width = t.width;
	*height = t.height;
	return rgba;
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
		/* an alias model skin (gl_model.c names them <model>_<skin>) with bright
		 * texels: its pixels for an emissive texture (see the top) */
		if ((flags & TEX_MIPMAP) && strstr (identifier, ".mdl_") && HasBrightTexels (rgba, width * height) &&
		    (t->pixels8 = (byte *) malloc (width * height)) != NULL)
			memcpy (t->pixels8, data, width * height);
		Hunk_FreeToLowMark (mark);
	}

	t->sampler = VK_SamplerForFlags (t->flags);
	VK_WriteTextureDescriptor (slot, t->view, t->sampler);
	return (GLuint)slot;
}

/* an image file's texture (5.2, vk_imagefile.c): the slot named
 * identifier, made or replaced, holding the file's levels as they are in
 * its format; flags as GL_LoadTexture's (the sampler; TEX_MIPMAP: the rest
 * of an uncompressed image's mip chain made). Its CRC is 0: the cache's
 * CRCs are the original textures'. 0 (white) when the cache is full or
 * img holds no valid levels. */
int VK_LoadImageTexture (const char *identifier, const vk_imagefile_t *img, int flags)
{
	int		i, key, slot = -1;
	vk_texture_t	*t;

	if (!img->data || img->levels < 1 || img->levels > VK_IMAGE_MAX_LEVELS)
		return 0;
	key = Hash_GenerateKeyString (&hash_textures, identifier, true);
	for (i = Hash_First(&hash_textures, key); i != -1; i = Hash_Next(&hash_textures, i))
	{
		if (i == 0 || strcmp (identifier, textures[i].identifier))
			continue;	/* slot 0, the white texture freed slots point to, stays */
		vkDeviceWaitIdle (vk.device);
		VK_WriteTextureDescriptor (i, textures[0].view, textures[0].sampler);
		VK_DestroyTexture (i);
		slot = i;
		break;
	}
	if (slot < 0)
	{
		if (numgltextures >= VK_MAX_TEXTURES)
			return 0;
		slot = numgltextures++;
		Hash_Add (&hash_textures, key, slot);
	}

	t = &textures[slot];
	q_strlcpy (t->identifier, identifier, MAX_QPATH);
	t->width = img->width;
	t->height = img->height;
	t->flags = flags;
	t->crc = 0;
	VK_UploadLevels (t, img->format, img->data, (uint32_t)img->levels, img->offset, img->size);
	t->sampler = VK_SamplerForFlags (t->flags);
	VK_WriteTextureDescriptor (slot, t->view, t->sampler);
	return slot;
}

uint32_t VK_TextureLevels (int slot)
{
	return (slot >= 0 && slot < numgltextures) ? textures[slot].mip_levels : 0;
}

/* the slot holding the texture loaded as identifier, -1 = none (e.g.
 * purged on a map change) */
int VK_FindTexture (const char *identifier)
{
	int	i, key = Hash_GenerateKeyString (&hash_textures, identifier, true);

	for (i = Hash_First(&hash_textures, key); i != -1; i = Hash_Next(&hash_textures, i))
	{
		if (!strcmp (identifier, textures[i].identifier))
			return i;
	}
	return -1;
}

const char *VK_TextureName (int slot)
{
	return (slot >= 0 && slot < numgltextures) ? textures[slot].identifier : "";
}

/* the CRC of the data the slot was loaded from (GL_LoadTexture's cache key) */
unsigned short VK_TextureCRC (int slot)
{
	return (slot >= 0 && slot < numgltextures) ? textures[slot].crc : 0;
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
	Draw_ClearCachedPics ();	/* some were in the purged slots */
	VK_MaterialFilesPurged (last_tex);	/* and the material files' (5.3) */
	VK_EffectLightsPurged ();		/* and the sprite frames' averages (6.2) */

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
	int		i, mipped = 0, alpha = 0, kept = 0;
	double		bytes = 0, kept_bytes = 0;
	qboolean	list = (Cmd_Argc() > 1 && !q_strcasecmp (Cmd_Argv(1), "list"));

	for (i = 0; i < numgltextures; i++)
	{
		const vk_texture_t	*t = &textures[i];
		double			b = (double)t->width * t->height * ((t->format == VK_FORMAT_BC7_UNORM_BLOCK || t->format == VK_FORMAT_BC5_UNORM_BLOCK) ? 1 : 4) *
					    ((t->mip_levels > 1) ? 4.0 / 3.0 : 1.0);	/* BC7, BC5: a byte per texel */

		bytes += b;
		if (t->mip_levels > 1)
			mipped++;
		if (t->flags & TEX_ALPHA)
			alpha++;
		if (t->pixels8)
		{
			kept++;
			kept_bytes += (double)t->width * t->height;
		}
		if (list)
			Con_Printf ("%4d %4dx%-4d %2u mips %s crc %04x %s\n", i, t->width, t->height, t->mip_levels,
					(t->flags & TEX_ALPHA) ? "a" : " ", t->crc, t->identifier[0] ? t->identifier : "(unnamed)");
	}
	Con_Printf ("%d textures (%d mipmapped, %d with alpha), %.1f MB; %d kept across maps\n",
			numgltextures, mipped, alpha, bytes / (1024.0 * 1024.0), gl_texlevel);
	Con_Printf ("8-bit pixels of %d bright skins kept for emissive textures, %.1f KB\n", kept, kept_bytes / 1024.0);
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
	VkPhysicalDeviceFeatures		features;
	VkPhysicalDeviceVulkan12Properties	props12;
	VkPhysicalDeviceProperties2		props;
	unsigned int				white = 0xffffffffu;
	int					i;

	Cvar_RegisterVariable (&gl_purge_maptex);
	Cvar_RegisterVariable (&r_srgb);
	Cvar_SetCallback (&r_srgb, ColorsChanged);
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
	sampler_nearest_repeat = VK_CreateSampler (VK_FILTER_NEAREST, VK_SAMPLER_MIPMAP_MODE_NEAREST, false,
						   VK_SAMPLER_ADDRESS_MODE_REPEAT, 1.0f);
	sampler_linear = VK_CreateSampler (VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_NEAREST, false,
					   VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, 1.0f);
	sampler_linear_repeat = VK_CreateSampler (VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_NEAREST, false,
						  VK_SAMPLER_ADDRESS_MODE_REPEAT, 1.0f);
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

	if (texture_pool)
		vkDestroyDescriptorPool (vk.device, texture_pool, NULL);
	if (vk.texture_set_layout)
		vkDestroyDescriptorSetLayout (vk.device, vk.texture_set_layout, NULL);
	if (sampler_nearest)
		vkDestroySampler (vk.device, sampler_nearest, NULL);
	if (sampler_nearest_repeat)
		vkDestroySampler (vk.device, sampler_nearest_repeat, NULL);
	if (sampler_linear)
		vkDestroySampler (vk.device, sampler_linear, NULL);
	if (sampler_linear_repeat)
		vkDestroySampler (vk.device, sampler_linear_repeat, NULL);
	if (sampler_trilinear)
		vkDestroySampler (vk.device, sampler_trilinear, NULL);
	texture_pool = VK_NULL_HANDLE;
	vk.texture_set_layout = VK_NULL_HANDLE;
	vk.texture_set = VK_NULL_HANDLE;
	sampler_nearest = sampler_nearest_repeat = sampler_linear = sampler_linear_repeat = sampler_trilinear = VK_NULL_HANDLE;
}
