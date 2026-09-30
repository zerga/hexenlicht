/* vk_imagefile.c -- the material spec's image files (story 5.2)
 *
 * Finds and reads the image files of docs/hexenlicht/MATERIALS.md into
 * memory, as they are; vk_texture.c's VK_LoadImageTexture uploads them.
 * The material system (5.3) picks the names (the ~<crc> qualifier before
 * the plain name) and puts the images into materials.
 * - Lookup: a path without an extension (textures/rtex343~ad81_n) is
 *   looked for as .png, .tga, .dds and .ktx2 in turn, each through the
 *   whole search path (a later game folder first, quakefs.c); a path with
 *   one of those extensions is only that file. Lowercased: paks compare
 *   names exactly and the spec's names are lowercase.
 * - PNG and TGA through stb_image (libs/stb, built for those two only):
 *   every channel count becomes RGBA, 16 bits per channel become 8; stb
 *   ignores gamma and color profile chunks, as the spec wants.
 * - DDS: BC7, BC5 and 8-bit RGBA or BGRA. The DX10 header's DXGI formats
 *   BC7_UNORM (and _SRGB), BC5_UNORM, R8G8B8A8_UNORM (and _SRGB),
 *   B8G8R8A8_UNORM (and _SRGB), B8G8R8X8_UNORM (and _SRGB); the older
 *   header's FourCC ATI2 or BC5U (BC5) and 32-bit RGB with an R mask of
 *   0xff (RGBA) or 0xff0000 (BGRA), alpha from its mask or 255 without.
 * - KTX2: R8G8B8A8, B8G8R8A8 and BC7 (UNORM or SRGB), BC5_UNORM; a 2D
 *   image, one layer and face, no supercompression (Basis Universal is
 *   Apache-2.0, zstd would need a decoder: MATERIALS.md). Its level index
 *   gives each level; levelCount 0 is one level (the chain made at upload).
 * - An _SRGB format is read as its UNORM twin (the same bytes: the shaders
 *   decode colors, DECISIONS R104), B8G8R8A8 is swapped to R8G8B8A8, so an
 *   image is VK_FORMAT_R8G8B8A8_UNORM, BC7_UNORM_BLOCK or BC5_UNORM_BLOCK.
 *   Everything else is refused with a reason: other formats (BC1, BC3,
 *   BC4, BC5_SNORM, float and 16-bit ones: not in the spec), cube maps,
 *   arrays, volumes, sizes the GPU can't take, a BC format without the
 *   GPU's BC support (vk.have_bc), files whose levels don't fit in them
 *   (every size is checked against the file's before a copy), PNGs and
 *   TGAs of more than MAX_DECODED_TEXELS (a small file can claim a huge
 *   image), empty files and names with * or ? (both end the game in
 *   quakefs.c: FS_LoadFile's Sys_Error, the loose files' wildcard lookup).
 * - Mips: the file's levels (at most its full chain), level 0 first.
 * - VK_ImageFileHasAlpha (5.3): whether level 0 has coverage (an alpha
 *   below 250, 5.6: BC7 compressors round an opaque image's 255 down to
 *   251-254; a BC7 block unless its mode or endpoints make it opaque),
 *   which decides whether an albedo is a masked skin's mask or a
 *   sprite's coverage.
 * - The loader never prints (5.3 may load while a frame is recorded, when
 *   a print re-enters SCR_UpdateScreen); it says why in img->error.
 * vk_imagefile <file> [scale] reads a file, uploads it as the texture
 * "*imagefile" (TEX_MIPMAP: an uncompressed image without all its mips
 * gets the rest), prints what it read and how long each step took, and
 * shows it at the top left of the 2D screen over black, alpha blended,
 * scale screen pixels per texel (0: fitted to it), as the texture is
 * (vk_draw.c's 8-bit colors: the file's values on screen with gamma 1),
 * until vk_imagefile without arguments or the textures' purge on a change
 * to another map (gl_purge_maptex).
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
#include "vid_vk.h"
#include "stb_image.h"

#define PREVIEW_TEXTURE	"*imagefile"
#define MAX_DECODED_TEXELS	(8192 * 8192)	/* a PNG or TGA: 256 MB of RGBA (the GPU would take 32768 x 32768) */
#define PRINTABLE(c)	((c) >= 32 && (c) < 127)
#define OPAQUE_ALPHA	250	/* an alpha at least this (98 %) has no coverage (5.6: BC7's rounding of 255, see BC7BlockOpaque) */

static const char *extensions[] = { ".png", ".tga", ".dds", ".ktx2" };
#define NUM_EXTENSIONS	(int)(sizeof(extensions) / sizeof(extensions[0]))

enum { SWIZZLE_NONE, SWIZZLE_BGRA, SWIZZLE_BGRX, SWIZZLE_RGBX };

static struct
{
	int	slot;		/* 0 = nothing shown */
	int	width, height;
	float	scale;		/* screen pixels per texel, 0 = fit */
} preview;

static uint32_t U32 (const byte *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t U64 (const byte *p)
{
	return (uint64_t)U32 (p) | ((uint64_t)U32 (p + 4) << 32);
}

/* bytes of a level: 4 per texel, or 16 per 4x4 block (BC7 and BC5) */
static size_t LevelSize (VkFormat format, uint32_t w, uint32_t h)
{
	if (format == VK_FORMAT_R8G8B8A8_UNORM)
		return (size_t)w * h * 4;
	return (size_t)((w + 3) / 4) * ((h + 3) / 4) * 16;
}

static int FullChain (uint32_t w, uint32_t h)
{
	int	n = 1;

	for (w = q_max(w, h); w > 1; w >>= 1)
		n++;
	return n;
}

/* the reason into img->error (not va(): its rotating buffers are shared, and
 * the loader may run on another thread in 5.3) */
static qboolean Refuse (vk_imagefile_t *img, const char *format, ...) FUNC_PRINTF(2,3);
static qboolean Refuse (vk_imagefile_t *img, const char *format, ...)
{
	va_list	args;

	va_start (args, format);
	q_vsnprintf (img->error, sizeof(img->error), format, args);
	va_end (args);
	return false;
}

/* checks the size and the levels, and lays the levels out one after the
 * other from offset 0 (their sizes by the format) */
static qboolean LayOut (vk_imagefile_t *img, uint32_t w, uint32_t h, uint32_t levels)
{
	size_t		at = 0;
	uint32_t	i;

	if (w < 1 || h < 1)
		return Refuse (img, "no pixels (a width or height of 0)");
	if (w > vk.props.limits.maxImageDimension2D || h > vk.props.limits.maxImageDimension2D)
		return Refuse (img, "%ux%u is larger than the GPU takes (%u)", w, h, vk.props.limits.maxImageDimension2D);
	if (levels < 1)
		levels = 1;
	if (levels > (uint32_t)FullChain (w, h) || levels > VK_IMAGE_MAX_LEVELS)
		return Refuse (img, "%u mip levels, more than a %ux%u image has", levels, w, h);
	if (img->format != VK_FORMAT_R8G8B8A8_UNORM && !vk.have_bc)
		return Refuse (img, "a BC format, and the GPU has no BC texture support (vk_info)");
	img->width = (int)w;
	img->height = (int)h;
	img->levels = (int)levels;
	for (i = 0; i < levels; i++)
	{
		img->offset[i] = at;
		img->size[i] = LevelSize (img->format, q_max(w >> i, 1u), q_max(h >> i, 1u));
		at += img->size[i];
	}
	return true;
}

static size_t TotalSize (const vk_imagefile_t *img)
{
	return img->offset[img->levels - 1] + img->size[img->levels - 1];
}

/* copies the levels from the file (level i at src[i]) into img->data,
 * swizzled to RGBA */
static qboolean CopyLevels (vk_imagefile_t *img, const byte **src, int swizzle)
{
	size_t	i, n, total = TotalSize (img);
	int	level;
	byte	*d;

	if ((img->data = (byte *) malloc (total)) == NULL)
		return Refuse (img, "out of memory");
	for (level = 0; level < img->levels; level++)
	{
		d = img->data + img->offset[level];
		memcpy (d, src[level], img->size[level]);
		if (swizzle == SWIZZLE_NONE)
			continue;
		for (i = 0, n = img->size[level]; i < n; i += 4)
		{
			if (swizzle != SWIZZLE_RGBX)
			{
				byte	b = d[i];

				d[i] = d[i + 2];
				d[i + 2] = b;
			}
			if (swizzle != SWIZZLE_BGRA)
				d[i + 3] = 255;
		}
	}
	return true;
}

/* ==========================================================================
 * PNG and TGA (stb_image)
 * ========================================================================== */

static qboolean LoadStb (const byte *file, size_t len, vk_imagefile_t *img)
{
	int		w, h, lw, lh, channels;
	stbi_uc		*pixels;
	double		t0 = Sys_DoubleTime ();

	if (len > INT_MAX || !stbi_info_from_memory (file, (int)len, &w, &h, &channels))
		return Refuse (img, "not a %s file stb_image reads (%s)", img->kind, stbi_failure_reason ());
	img->file_format = stbi_is_16_bit_from_memory (file, (int)len) ? "16 bits per channel, read as 8" : "8 bits per channel";
	img->format = VK_FORMAT_R8G8B8A8_UNORM;
	/* a small file can claim a huge image (a decompression bomb) */
	if ((int64_t)w * h > MAX_DECODED_TEXELS)
		return Refuse (img, "%dx%d: more than the %d texels a PNG or TGA may decode to", w, h, MAX_DECODED_TEXELS);
	if (!LayOut (img, (uint32_t)w, (uint32_t)h, 1))
		return false;
	pixels = stbi_load_from_memory (file, (int)len, &lw, &lh, &channels, 4);
	if (!pixels)
		return Refuse (img, "stb_image: %s", stbi_failure_reason ());
	if (lw != w || lh != h)
	{
		stbi_image_free (pixels);
		return Refuse (img, "stb_image read %dx%d, its header said %dx%d", lw, lh, w, h);
	}
	img->data = pixels;	/* stb_image allocates with malloc (STBI_MALLOC's default): VK_FreeImageFile frees it */
	img->decode_ms = (Sys_DoubleTime () - t0) * 1000.0;
	return true;
}

/* ==========================================================================
 * DDS
 * ========================================================================== */

#define DDS_HEADER_SIZE		128	/* the magic and DDS_HEADER */
#define DDS_DX10_SIZE		20	/* DDS_HEADER_DXT10 */
#define DDPF_ALPHAPIXELS	0x1
#define DDPF_FOURCC		0x4
#define DDPF_RGB		0x40
#define DDSCAPS2_CUBEMAP	0x200
#define DDSCAPS2_VOLUME		0x200000
#define FOURCC(a, b, c, d)	((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))

static qboolean FormatFromDXGI (vk_imagefile_t *img, uint32_t dxgi, int *swizzle)
{
	*swizzle = SWIZZLE_NONE;
	switch (dxgi)
	{
	case 98: img->file_format = "BC7_UNORM"; img->format = VK_FORMAT_BC7_UNORM_BLOCK; return true;
	case 99: img->file_format = "BC7_UNORM_SRGB"; img->format = VK_FORMAT_BC7_UNORM_BLOCK; return true;
	case 83: img->file_format = "BC5_UNORM"; img->format = VK_FORMAT_BC5_UNORM_BLOCK; return true;
	case 28: img->file_format = "R8G8B8A8_UNORM"; break;
	case 29: img->file_format = "R8G8B8A8_UNORM_SRGB"; break;
	case 87: img->file_format = "B8G8R8A8_UNORM"; *swizzle = SWIZZLE_BGRA; break;
	case 91: img->file_format = "B8G8R8A8_UNORM_SRGB"; *swizzle = SWIZZLE_BGRA; break;
	case 88: img->file_format = "B8G8R8X8_UNORM"; *swizzle = SWIZZLE_BGRX; break;
	case 93: img->file_format = "B8G8R8X8_UNORM_SRGB"; *swizzle = SWIZZLE_BGRX; break;
	default:
		return Refuse (img, "DXGI format %u: not in the spec (BC7, BC5 or 8-bit RGBA)", dxgi);
	}
	img->format = VK_FORMAT_R8G8B8A8_UNORM;
	return true;
}

static qboolean LoadDDS (const byte *file, size_t len, vk_imagefile_t *img)
{
	const byte	*src[VK_IMAGE_MAX_LEVELS];
	uint32_t	w, h, levels, pf_flags, fourcc, bits, rmask, gmask, bmask, amask;
	size_t		header = DDS_HEADER_SIZE;
	int		swizzle = SWIZZLE_NONE, i;

	if (len < DDS_HEADER_SIZE || U32 (file) != FOURCC('D','D','S',' ') || U32 (file + 4) != 124)
		return Refuse (img, "not a DDS file (its header)");
	h = U32 (file + 12);
	w = U32 (file + 16);
	levels = U32 (file + 28);
	pf_flags = U32 (file + 80);
	fourcc = U32 (file + 84);
	bits = U32 (file + 88);
	rmask = U32 (file + 92);
	gmask = U32 (file + 96);
	bmask = U32 (file + 100);
	amask = U32 (file + 104);
	if (U32 (file + 112) & (DDSCAPS2_CUBEMAP | DDSCAPS2_VOLUME))
		return Refuse (img, "a cube map or volume, not a 2D image");

	if ((pf_flags & DDPF_FOURCC) && fourcc == FOURCC('D','X','1','0'))
	{
		if (len < DDS_HEADER_SIZE + DDS_DX10_SIZE)
			return Refuse (img, "cut short in its DX10 header");
		header += DDS_DX10_SIZE;
		if (U32 (file + 132) != 3 || (U32 (file + 136) & 0x4) || U32 (file + 140) > 1)
			return Refuse (img, "not a single 2D image (a DX10 cube map, array or other dimension)");
		if (!FormatFromDXGI (img, U32 (file + 128), &swizzle))
			return false;
	}
	else if (pf_flags & DDPF_FOURCC)
	{
		if (fourcc != FOURCC('A','T','I','2') && fourcc != FOURCC('B','C','5','U'))
		{
			const byte	*c = file + 84;

			if (PRINTABLE(c[0]) && PRINTABLE(c[1]) && PRINTABLE(c[2]) && PRINTABLE(c[3]))
				return Refuse (img, "FourCC %c%c%c%c: not in the spec (BC7, BC5 or 8-bit RGBA)", c[0], c[1], c[2], c[3]);
			return Refuse (img, "FourCC 0x%08x: not in the spec (BC7, BC5 or 8-bit RGBA)", fourcc);
		}
		img->file_format = (fourcc == FOURCC('A','T','I','2')) ? "BC5 (ATI2)" : "BC5 (BC5U)";
		img->format = VK_FORMAT_BC5_UNORM_BLOCK;
	}
	else if ((pf_flags & DDPF_RGB) && bits == 32 && gmask == 0xff00 &&
		 ((rmask == 0xff && bmask == 0xff0000) || (rmask == 0xff0000 && bmask == 0xff)))
	{
		qboolean	alpha = (pf_flags & DDPF_ALPHAPIXELS) && amask == 0xff000000u;

		img->format = VK_FORMAT_R8G8B8A8_UNORM;
		if (rmask == 0xff)
		{
			img->file_format = alpha ? "R8G8B8A8" : "R8G8B8X8";
			swizzle = alpha ? SWIZZLE_NONE : SWIZZLE_RGBX;
		}
		else
		{
			img->file_format = alpha ? "B8G8R8A8" : "B8G8R8X8";
			swizzle = alpha ? SWIZZLE_BGRA : SWIZZLE_BGRX;
		}
	}
	else
		return Refuse (img, "pixel format %u bits, masks %08x %08x %08x %08x: not in the spec (BC7, BC5 or 8-bit RGBA)",
				      bits, rmask, gmask, bmask, amask);

	if (!LayOut (img, w, h, levels))
		return false;
	if (TotalSize (img) > len - header)
		return Refuse (img, "cut short: %u levels need %u bytes, the file has %u after its header",
				      (unsigned)img->levels, (unsigned)TotalSize (img), (unsigned)(len - header));
	for (i = 0; i < img->levels; i++)
		src[i] = file + header + img->offset[i];
	return CopyLevels (img, src, swizzle);
}

/* ==========================================================================
 * KTX2
 * ========================================================================== */

#define KTX2_HEADER_SIZE	80	/* the identifier, the header and the index */
#define KTX2_LEVEL_SIZE		24	/* a level index entry */

static const byte ktx2_identifier[12] = { 0xab, 'K', 'T', 'X', ' ', '2', '0', 0xbb, '\r', '\n', 0x1a, '\n' };

static qboolean LoadKTX2 (const byte *file, size_t len, vk_imagefile_t *img)
{
	const byte	*src[VK_IMAGE_MAX_LEVELS];
	uint32_t	vkformat, w, h, depth, layers, faces, levels, super, i;
	int		swizzle = SWIZZLE_NONE;

	if (len < KTX2_HEADER_SIZE || memcmp (file, ktx2_identifier, sizeof(ktx2_identifier)))
		return Refuse (img, "not a KTX2 file (its identifier)");
	vkformat = U32 (file + 12);
	w = U32 (file + 20);
	h = U32 (file + 24);
	depth = U32 (file + 28);
	layers = U32 (file + 32);
	faces = U32 (file + 36);
	levels = U32 (file + 40);
	super = U32 (file + 44);
	if (super != 0)
		return Refuse (img, "supercompressed (scheme %u): the spec takes none", super);
	if (h == 0 || depth != 0 || layers > 1 || faces != 1)
		return Refuse (img, "not a single 2D image (1D, 3D, an array or a cube map)");

	switch (vkformat)
	{
	case VK_FORMAT_BC7_UNORM_BLOCK: img->file_format = "BC7_UNORM"; img->format = vkformat; break;
	case VK_FORMAT_BC7_SRGB_BLOCK: img->file_format = "BC7_SRGB"; img->format = VK_FORMAT_BC7_UNORM_BLOCK; break;
	case VK_FORMAT_BC5_UNORM_BLOCK: img->file_format = "BC5_UNORM"; img->format = vkformat; break;
	case VK_FORMAT_R8G8B8A8_UNORM: img->file_format = "R8G8B8A8_UNORM"; img->format = vkformat; break;
	case VK_FORMAT_R8G8B8A8_SRGB: img->file_format = "R8G8B8A8_SRGB"; img->format = VK_FORMAT_R8G8B8A8_UNORM; break;
	case VK_FORMAT_B8G8R8A8_UNORM: img->file_format = "B8G8R8A8_UNORM"; img->format = VK_FORMAT_R8G8B8A8_UNORM; swizzle = SWIZZLE_BGRA; break;
	case VK_FORMAT_B8G8R8A8_SRGB: img->file_format = "B8G8R8A8_SRGB"; img->format = VK_FORMAT_R8G8B8A8_UNORM; swizzle = SWIZZLE_BGRA; break;
	default:
		return Refuse (img, "VkFormat %u: not in the spec (BC7, BC5 or 8-bit RGBA)", vkformat);
	}

	if (!LayOut (img, w, h, levels))
		return false;
	if (len < KTX2_HEADER_SIZE + (size_t)img->levels * KTX2_LEVEL_SIZE)
		return Refuse (img, "cut short in its level index");
	for (i = 0; i < (uint32_t)img->levels; i++)
	{
		const byte	*entry = file + KTX2_HEADER_SIZE + i * KTX2_LEVEL_SIZE;
		uint64_t	at = U64 (entry), size = U64 (entry + 8);

		if (size != img->size[i])
			return Refuse (img, "level %u has %u bytes; a %ux%u level of its format has %u", i, (unsigned)size,
					      q_max(w >> i, 1u), q_max(h >> i, 1u), (unsigned)img->size[i]);
		if (at > len || size > len - at)
			return Refuse (img, "cut short: level %u lies outside the file", i);
		src[i] = file + at;
	}
	return CopyLevels (img, src, swizzle);
}

/* ==========================================================================
 * Lookup
 * ========================================================================== */

/* the file's size through the search path, -1 = none (FS_OpenFile without a
 * file: FS_FileExists's call) */
static long FileSize (const char *name)
{
	return FS_OpenFile (name, NULL, NULL);
}

/* a file's bytes as its extension's kind; img->file and read_ms are set */
static qboolean ParseFile (const char *name, const byte *file, size_t len, vk_imagefile_t *img)
{
	qboolean	ok;
	double		t0 = Sys_DoubleTime ();

	if (!len)
		return Refuse (img, "an empty file");
	if (!q_strcasecmp (COM_FileGetExtension (name), "dds"))
	{
		img->kind = "DDS";
		ok = LoadDDS (file, len, img);
	}
	else if (!q_strcasecmp (COM_FileGetExtension (name), "ktx2"))
	{
		img->kind = "KTX2";
		ok = LoadKTX2 (file, len, img);
	}
	else
	{
		img->kind = q_strcasecmp (COM_FileGetExtension (name), "tga") ? "PNG" : "TGA";
		ok = LoadStb (file, len, img);
	}
	if (!ok)
	{
		free (img->data);
		img->data = NULL;
	}
	else if (!img->decode_ms)
		img->decode_ms = (Sys_DoubleTime () - t0) * 1000.0;
	return ok;
}

static qboolean LoadFile (const char *name, long size, vk_imagefile_t *img)
{
	byte		*file;
	qboolean	ok;
	double		t0 = Sys_DoubleTime ();

	q_strlcpy (img->file, name, sizeof(img->file));
	/* FS_LoadFile ends the game with a Sys_Error on a file of 0 bytes (its
	 * fread of nothing "fails") */
	if (size <= 0)
		return Refuse (img, "an empty file");
	if ((file = FS_LoadMallocFile (name, NULL)) == NULL)
		return Refuse (img, "can't be read");
	img->read_ms = (Sys_DoubleTime () - t0) * 1000.0;
	ok = ParseFile (name, file, (size_t)fs_filesize, img);
	free (file);
	return ok;
}

/* 5.3 (vk_matfiles.c, which reads the bytes itself: FS_LoadFile ends the
 * game on a file that is being written): a file's bytes, the kind by the
 * name's extension; never prints */
qboolean VK_ParseImageFile (const char *name, const byte *file, size_t len, vk_imagefile_t *img)
{
	memset (img, 0, sizeof(*img));
	q_strlcpy (img->file, name, sizeof(img->file));
	return ParseFile (name, file, len, img);
}

qboolean VK_LoadImageFile (const char *path, vk_imagefile_t *img)
{
	char	name[MAX_QPATH];
	long	size;
	int	i;

	memset (img, 0, sizeof(*img));
	/* the loose files' lookup (Sys_filesize: FindFirstFile) takes wildcards,
	 * and FS_OpenFile then ends the game when it can't open the name */
	if (strpbrk (path, "*?"))
	{
		q_strlcpy (img->file, path, sizeof(img->file));
		return Refuse (img, "a * or ? in the name (the spec writes * as #)");
	}
	for (i = 0; i < NUM_EXTENSIONS; i++)
	{
		if (!q_strcasecmp (COM_FileGetExtension (path), extensions[i] + 1))
			break;
	}
	if (i < NUM_EXTENSIONS)
	{
		/* only this file */
		if (q_strlcpy (name, path, sizeof(name)) >= sizeof(name))
		{
			q_strlcpy (img->file, path, sizeof(img->file));
			return Refuse (img, "the name is too long (%d characters at most)", (int)sizeof(name) - 1);
		}
		q_strlwr (name);
		if ((size = FileSize (name)) < 0)
			return false;
		return LoadFile (name, size, img);
	}
	for (i = 0; i < NUM_EXTENSIONS; i++)
	{
		if (q_snprintf (name, sizeof(name), "%s%s", path, extensions[i]) >= (int)sizeof(name))
		{
			q_strlcpy (img->file, path, sizeof(img->file));
			return Refuse (img, "the name is too long (%d characters at most)", (int)sizeof(name) - 1);
		}
		q_strlwr (name);
		if ((size = FileSize (name)) >= 0)
			return LoadFile (name, size, img);
	}
	return false;
}

void VK_FreeImageFile (vk_imagefile_t *img)
{
	free (img->data);
	img->data = NULL;
}

/* count bits from bit first of a 128-bit BC7 block (little endian) */
static uint32_t BlockBits (const byte *b, int first, int count)
{
	uint32_t	v = 0;
	int		i;

	for (i = 0; i < count; i++)
		v |= (uint32_t)((b[(first + i) >> 3] >> ((first + i) & 7)) & 1) << i;
	return v;
}

/* a BC7 endpoint of bits bits as 8 bits (the spec's unquantization: the
 * top bits repeated below) */
static uint32_t Expand (uint32_t v, int bits)
{
	return (v << (8 - bits)) | (v >> (2 * bits - 8));
}

/* the n endpoints of bits bits from bit first (with a p-bit each below them
 * from bit pbits, if pbits >= 0) all at least OPAQUE_ALPHA */
static qboolean EndpointsOpaque (const byte *b, int first, int bits, int n, int pbits)
{
	int	i;

	for (i = 0; i < n; i++)
	{
		uint32_t	v = BlockBits (b, first + i * bits, bits);

		if (pbits >= 0 ? Expand ((v << 1) | BlockBits (b, pbits + i, 1), bits + 1) < OPAQUE_ALPHA :
				 Expand (v, bits) < OPAQUE_ALPHA)
			return false;
	}
	return true;
}

/* a BC7 block whose every texel's alpha is at least OPAQUE_ALPHA: modes 0-3
 * have none, the others' alpha lies between endpoints (with their p-bits),
 * so all of them at least that make it so; modes 4 and 5 may rotate a color
 * channel into alpha (the Khronos Data Format spec's BC7). 5.6: compressors
 * (texconv) write an opaque image's blocks with a p-bit of 0 where the color
 * wants it, 254 in mode 6, 251 in mode 7, which isn't coverage */
static qboolean BC7BlockOpaque (const byte *b)
{
	int	mode, rot;

	for (mode = 0; mode < 8 && !(b[0] & (1 << mode)); mode++)
		;
	switch (mode)
	{
	case 0: case 1: case 2: case 3:
		return true;
	case 4:		/* rotation 2, index mode 1, RGB 5 bits x 2 each, A 6 bits x 2 */
		rot = (int)BlockBits (b, 5, 2);
		return rot ? EndpointsOpaque (b, 8 + (rot - 1) * 10, 5, 2, -1) : EndpointsOpaque (b, 38, 6, 2, -1);
	case 5:		/* rotation 2, RGB 7 bits x 2 each, A 8 bits x 2 */
		rot = (int)BlockBits (b, 6, 2);
		return rot ? EndpointsOpaque (b, 8 + (rot - 1) * 14, 7, 2, -1) : EndpointsOpaque (b, 50, 8, 2, -1);
	case 6:		/* RGBA 7 bits x 2 each, then a p-bit per endpoint */
		return EndpointsOpaque (b, 49, 7, 2, 63);
	case 7:		/* partition 6, RGBA 5 bits x 4 each, then 4 p-bits */
		return EndpointsOpaque (b, 74, 5, 4, 94);
	default:	/* reserved: decodes to 0 */
		return false;
	}
}

/* coverage: a texel of level 0 whose alpha is below OPAQUE_ALPHA (a BC7
 * block that isn't certainly opaque; BC5 has no alpha); 250 and more count
 * as opaque, as BC7 compressors round 255 (above), in every format alike */
qboolean VK_ImageFileHasAlpha (const vk_imagefile_t *img)
{
	const byte	*p;
	size_t		i;

	if (!img->data || img->levels < 1)
		return false;
	p = img->data + img->offset[0];
	if (img->format == VK_FORMAT_R8G8B8A8_UNORM)
	{
		for (i = 3; i < img->size[0]; i += 4)
		{
			if (p[i] < OPAQUE_ALPHA)
				return true;
		}
		return false;
	}
	if (img->format == VK_FORMAT_BC7_UNORM_BLOCK)
	{
		for (i = 0; i + 16 <= img->size[0]; i += 16)
		{
			if (!BC7BlockOpaque (p + i))
				return true;
		}
	}
	return false;
}

/* ==========================================================================
 * vk_imagefile
 * ========================================================================== */

static const char *FormatName (VkFormat format)
{
	switch (format)
	{
	case VK_FORMAT_BC7_UNORM_BLOCK: return "BC7";
	case VK_FORMAT_BC5_UNORM_BLOCK: return "BC5";
	default: return "RGBA8";
	}
}

static void VK_ImageFile_f (void)
{
	vk_imagefile_t	img;
	double		t0;
	int		slot;

	if (Cmd_Argc () < 2)
	{
		if (preview.slot)
			preview.slot = 0;
		else
			Con_Printf ("vk_imagefile <file> [scale]: reads an image file (MATERIALS.md: a path without its extension\n"
				    "tries .png .tga .dds .ktx2), prints it and shows it, scale screen pixels per texel (0 fits);\n"
				    "vk_imagefile alone hides it\n");
		return;
	}
	if (!VK_LoadImageFile (Cmd_Argv (1), &img))
	{
		if (img.error[0])
			Con_Printf ("%s: %s\n", img.file, img.error);
		else
			Con_Printf ("%s: no such image file\n", Cmd_Argv (1));
		return;
	}

	t0 = Sys_DoubleTime ();
	slot = VK_LoadImageTexture (PREVIEW_TEXTURE, &img, TEX_MIPMAP);
	if (!slot)
	{
		Con_Printf ("%s: read, but the texture cache is full (%d slots)\n", img.file, VK_MAX_TEXTURES);
		VK_FreeImageFile (&img);
		return;
	}
	Con_Printf ("%s: %s %s, %dx%d, %s, %d level%s in the file, %u uploaded; read %.0f ms, decode %.0f ms, upload %.0f ms (whole ms: Sys_DoubleTime)\n",
		    img.file, img.kind, img.file_format, img.width, img.height, FormatName (img.format),
		    img.levels, (img.levels == 1) ? "" : "s", VK_TextureLevels (slot),
		    img.read_ms, img.decode_ms, (Sys_DoubleTime () - t0) * 1000.0);
	preview.slot = slot;
	preview.width = img.width;
	preview.height = img.height;
	preview.scale = (Cmd_Argc () > 2) ? q_max((float)atof (Cmd_Argv (2)), 0.0f) : 0.0f;
	VK_FreeImageFile (&img);
}

void VK_DrawImageFile (void)
{
	float	s = (float)VID_GetUIScale ();	/* screen pixels per 2D unit */
	float	w, h, fit;

	if (!preview.slot)
		return;
	if (VK_FindTexture (PREVIEW_TEXTURE) != preview.slot)
	{
		preview.slot = 0;	/* purged with the map's textures */
		return;
	}
	if (preview.scale > 0)
	{
		w = preview.width * preview.scale / s;
		h = preview.height * preview.scale / s;
	}
	else
	{
		fit = q_min((float)vid.width / preview.width, (float)vid.height / preview.height);
		w = preview.width * fit;
		h = preview.height * fit;
	}
	VK_DrawTexture (0, 0, w, h, preview.slot, true);
}

void VK_InitImageFiles (void)
{
	Cmd_AddCommand ("vk_imagefile", VK_ImageFile_f);
}
