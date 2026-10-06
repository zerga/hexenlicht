/* vk_matfiles.c -- the material files (story 5.3)
 *
 * Finds docs/hexenlicht/MATERIALS.md's image files and .mat settings for
 * each original texture; vk_material.c's VK_ApplyMaterialFiles puts them
 * into its materials.
 * - The index: every file under textures/ in the search path, listed once
 *   (quakefs.c's FS_ListSearchPath: each pak's entries, each game
 *   directory's folder walked), the first occurrence of each name in the
 *   search path's order, a loose file with its size and last write time.
 *   A lookup is then a hash probe instead of a search through the path
 *   (up to 7 kinds of file per texture, each under two names and four
 *   extensions: about 50 searches, each a file system call per game
 *   folder). Built on the first lookup after a map load or
 *   r_reloadmaterials. Pak entries not in lowercase are left out (the
 *   lookup lowercases, and paks compare names exactly) and reported.
 * - A set per original texture slot (world textures, skins, the stone and
 *   ice pictures, sprite frames): the spec's name of the slot
 *   (VK_TextureName lowercased, * as #, a picture's .lmp cut:
 *   textures/#lava1, textures/models/paladin.mdl_0, textures/gfx/skin100),
 *   qualified with ~<crc> (VK_TextureCRC). Each image and the .mat is
 *   looked up on its own (the qualified name, then the plain one; for each
 *   .png, .tga, .dds, .ktx2), the roughness and metallic from the first
 *   name with any of _orm, _r, _m (never mixed across names). An _r and an
 *   _m (PNG or TGA only) are packed into one RGBA8 texture, G roughness and
 *   B metallic, 255 where one is missing (its factor then is its value,
 *   VK_ApplyMaterialFiles). Formats by the map: BC5 only for normal maps,
 *   BC7 not for them (MATERIALS.md). A set is resolved when first asked
 *   for and again after a map load or a reload; one whose slot now holds
 *   another texture (another name or CRC) is made again. The .mat's kind
 *   (5.5: chrome, glass) is the set's: a skin's instance takes it every
 *   frame, the world's primitives at map load (vk_world.c); a kind on an
 *   animation's later frame is refused (the triangles keep the first's).
 * - The sky (5.5) has no slot: VK_SkyImageFile looks up the world's sky
 *   texture's name and the CRC of its pixels and reads the file (RGBA8,
 *   2:1) into memory for vk_sky.c, which splits and edits its layers; its
 *   other maps and a .mat are reported (the sky is unlit).
 * - The images: one texture slot per file, named by the file
 *   (textures/<name>_rm for a packed pair), kept with the file's identity
 *   (the game folder or pak, position, size, write time): a set resolved
 *   again reuses unchanged files and reads a changed one into its slot
 *   again. A new slot only while TEXTURE_RESERVE stay free (GL_LoadTexture
 *   ends the game when the cache is full). The bytes are read here, not
 *   through quakefs.c, which ends the game on a file it can't reopen or
 *   read to its end (a loose file an editor is still writing: refused).
 *   They are purged with the map's textures (D_ClearOpenGLTextures,
 *   VK_MaterialFilesPurged); the sprites loaded at startup are looked up
 *   again on the next map load (VK_PreloadStartupSprites).
 * - Problems (refused files, _r and _m of different sizes, maps that don't
 *   apply, .mat lines that don't parse or apply) are collected, never
 *   printed: sets resolve while a frame is recorded (a skin when first
 *   shown, a sprite), when a print re-enters SCR_UpdateScreen.
 *   vk_materials problems lists them; a map load prints one summary line
 *   (VK_ReportMaterialFiles from R_NewMap, outside frames).
 * r_reloadmaterials lists the files again, resolves every set (reading
 * what is new or changed), applies them to the materials and uploads the
 * table after the GPU is idle (primitives reference materials by index:
 * no geometry is rebuilt), and prints what it did and how long it took;
 * 5.5: the lava's lights take their files' colors again
 * (VK_LavaFileColors, VK_RebuildLights), the sky its layers
 * (VK_ReloadSkyFile), and a world texture's changed kind is reported (it
 * applies at the next map load). r_materials 0 shows the original
 * textures only (A/B comparisons), the same way.
 * vk_materials [list|problems] prints the counts, each texture's files,
 * the problems; vk_materials here (5.6) the texture at the view's center:
 * its names, files, and its albedo's mean against the original's.
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
#include "q_ctype.h"
#include "vk_local.h"
#include <windows.h>

#define TEXTURES_FOLDER	"textures"
#define INDEX_HASH	8192	/* buckets, a power of 2 */
#define IMAGE_HASH	1024
#define MAX_WALK_DEPTH	16	/* folders in folders under textures/ */
#define MAX_PROBLEM	256	/* characters in one problem */

#define TEXTURE_RESERVE	256	/* texture slots the files leave free: GL_LoadTexture ends the game when there are none */

static const char *all_extensions[] = { ".png", ".tga", ".dds", ".ktx2", NULL };


/* ==========================================================================
 * Problems: collected, printed by vk_materials problems
 * ========================================================================== */

typedef struct
{
	char	*text;		/* lines, each ending in \n; malloc'd */
	size_t	len;
	int	count;
} problems_t;

static void AddProblem (problems_t *p, const char *format, ...) FUNC_PRINTF(2,3);
static void AddProblem (problems_t *p, const char *format, ...)
{
	char	line[MAX_PROBLEM];
	va_list	args;
	size_t	n;
	char	*text;

	va_start (args, format);
	q_vsnprintf (line, sizeof(line) - 1, format, args);
	va_end (args);
	n = strlen (line);
	line[n++] = '\n';
	line[n] = '\0';
	text = (char *) realloc (p->text, p->len + n + 1);
	if (!text)
		return;		/* out of memory: the problem is lost, nothing else */
	memcpy (text + p->len, line, n + 1);
	p->text = text;
	p->len += n;
	p->count++;
}

static void ClearProblems (problems_t *p)
{
	free (p->text);
	memset (p, 0, sizeof(*p));
}

/* a line per Con_Printf: its buffer is smaller than a long list */
static void PrintProblems (const problems_t *p)
{
	const char	*line, *end;

	for (line = p->text; line && *line; line = end + 1)
	{
		if ((end = strchr (line, '\n')) == NULL)
			break;
		Con_Printf ("%.*s\n", (int)(end - line), line);
	}
}


/* ==========================================================================
 * The index of the files under textures/
 * ========================================================================== */

typedef struct
{
	char	name[MAX_QPATH];	/* lowercase, textures/... */
	int	source;			/* in index.sources */
	long	filepos;		/* in a pak, -1 = a loose file */
	long	size;
	int64_t	mtime;			/* a loose file's last write time (FILETIME), 0 in a pak */
	int	next;			/* hash chain, -1 = end */
} indexfile_t;

typedef struct
{
	char		path[MAX_OSPATH];	/* a game folder's, or a pak's file */
	qboolean	pak;
} source_t;

static struct
{
	indexfile_t	*files;
	int		num_files, max_files;
	int		hash[INDEX_HASH];
	source_t	*sources;		/* the game folders and paks, in the search path's order */
	int		num_sources, max_sources;
	const char	*last_pak;		/* while listing: the pak of the last entry */
	qboolean	valid;
	double		ms;
	problems_t	problems;
} idx;

static unsigned int HashName (const char *s)
{
	unsigned int	h = 5381;

	while (*s)
		h = h * 33 + (unsigned char)*s++;
	return h;
}

static int IndexFind (const char *name)
{
	int	i;

	for (i = idx.hash[HashName (name) & (INDEX_HASH - 1)]; i >= 0; i = idx.files[i].next)
	{
		if (!strcmp (idx.files[i].name, name))
			return i;
	}
	return -1;
}

static void IndexAdd (const char *name, int source, long filepos, long size, int64_t mtime)
{
	indexfile_t	*f;
	unsigned int	b;

	if (IndexFind (name) >= 0)
		return;		/* one earlier in the search path is the one found */
	if (idx.num_files == idx.max_files)
	{
		int	max = q_max (idx.max_files * 2, 1024);

		f = (indexfile_t *) realloc (idx.files, max * sizeof(*f));
		if (!f)
			Sys_Error ("%s: out of memory", __thisfunc__);
		idx.files = f;
		idx.max_files = max;
	}
	f = &idx.files[idx.num_files];
	q_strlcpy (f->name, name, sizeof(f->name));
	f->source = source;
	f->filepos = filepos;
	f->size = size;
	f->mtime = mtime;
	b = HashName (name) & (INDEX_HASH - 1);
	f->next = idx.hash[b];
	idx.hash[b] = idx.num_files++;
}

static int AddSource (const char *path, qboolean pak)
{
	if (idx.num_sources == idx.max_sources)
	{
		int		max = q_max (idx.max_sources * 2, 16);
		source_t	*s = (source_t *) realloc (idx.sources, max * sizeof(*s));

		if (!s)
			Sys_Error ("%s: out of memory", __thisfunc__);
		idx.sources = s;
		idx.max_sources = max;
	}
	q_strlcpy (idx.sources[idx.num_sources].path, path, MAX_OSPATH);
	idx.sources[idx.num_sources].pak = pak;
	return idx.num_sources++;
}

/* a loose folder's files, and those of the folders in it */
static void WalkFolder (const char *ospath, const char *relative, int source, int depth)
{
	char			pattern[MAX_OSPATH], sub[MAX_OSPATH], rel[MAX_QPATH];
	WIN32_FIND_DATAA	fd;
	HANDLE			h;

	if (q_snprintf (pattern, sizeof(pattern), "%s/*", ospath) >= (int)sizeof(pattern))
		return;
	h = FindFirstFileA (pattern, &fd);
	if (h == INVALID_HANDLE_VALUE)
		return;
	do
	{
		if (!strcmp (fd.cFileName, ".") || !strcmp (fd.cFileName, ".."))
			continue;
		if (q_snprintf (rel, sizeof(rel), "%s/%s", relative, fd.cFileName) >= (int)sizeof(rel))
		{
			AddProblem (&idx.problems, "%s/%s/%s: the name is too long (%d characters at most), left out",
				    idx.sources[source].path, relative, fd.cFileName, MAX_QPATH - 1);
			continue;
		}
		q_strlwr (rel);
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
		{
			if (depth < MAX_WALK_DEPTH &&
			    q_snprintf (sub, sizeof(sub), "%s/%s", ospath, fd.cFileName) < (int)sizeof(sub))
				WalkFolder (sub, rel, source, depth + 1);
		}
		else
		{
			IndexAdd (rel, source, -1, (long)q_min (((int64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow, (int64_t)LONG_MAX),
				  ((int64_t)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime);
		}
	} while (FindNextFileA (h, &fd));
	FindClose (h);
}

static void ListFolder (const char *ospath, void *ctx)
{
	char	path[MAX_OSPATH];
	int	source = AddSource (ospath, false);

	(void)ctx;
	if (q_snprintf (path, sizeof(path), "%s/%s", ospath, TEXTURES_FOLDER) < (int)sizeof(path))
		WalkFolder (path, TEXTURES_FOLDER, source, 0);
	idx.last_pak = NULL;
}

static void ListPakFile (const char *name, long filepos, long size, const char *pakname, void *ctx)
{
	const char	*c;

	(void)ctx;
	if (pakname != idx.last_pak)
	{
		AddSource (pakname, true);
		idx.last_pak = pakname;
	}
	for (c = name; *c; c++)
	{
		if (q_isupper (*c) || *c == '*' || *c == '?')
		{
			AddProblem (&idx.problems, "%s in %s: %s, left out", name, pakname,
				    q_isupper (*c) ? "not in lowercase (paks compare names exactly)" : "a * or ? in the name");
			return;
		}
	}
	IndexAdd (name, idx.num_sources - 1, filepos, size, 0);
}

static void BuildIndex (void)
{
	double	t0 = Sys_DoubleTime ();

	idx.num_files = idx.num_sources = 0;
	idx.last_pak = NULL;
	memset (idx.hash, 0xff, sizeof(idx.hash));	/* -1 */
	ClearProblems (&idx.problems);
	FS_ListSearchPath (TEXTURES_FOLDER "/", ListFolder, ListPakFile, NULL);
	idx.valid = true;
	idx.ms = (Sys_DoubleTime () - t0) * 1000.0;
}

/* the first of name + suffix + each extension in the index, -1 = none */
static int FindFile (const char *name, const char *suffix, const char **extensions)
{
	char	path[MAX_QPATH];
	int	i, f;

	for (i = 0; extensions[i]; i++)
	{
		if (q_snprintf (path, sizeof(path), "%s%s%s", name, suffix, extensions[i]) >= (int)sizeof(path))
			continue;
		if ((f = IndexFind (path)) >= 0)
			return f;
	}
	return -1;
}


/* ==========================================================================
 * The images: a texture slot per file read
 * ========================================================================== */

typedef struct
{
	char	name[MAX_QPATH];	/* "" = none */
	char	source[MAX_OSPATH];
	long	filepos, size;
	int64_t	mtime;
} fileid_t;

typedef struct
{
	char		identifier[MAX_QPATH];	/* the texture's: the file's name, <name>_rm for a packed pair */
	fileid_t	id[2];			/* the file read; a packed pair's _r and _m */
	int		slot;			/* 0: refused, error says why */
	char		error[200];
	const char	*kind;			/* "PNG", ... ("packed" for a pair) */
	VkFormat	format;
	int		width, height, levels;	/* levels: in the file */
	qboolean	alpha;			/* a texel's alpha below 255 */
	double		mb;			/* video memory */
	int		next;			/* hash chain */
} image_t;

static image_t	*images;
static int	num_images, max_images;
static int	image_hash[IMAGE_HASH];

static struct
{
	int	read, refused;		/* files read (again) since the last map load or reload */
	double	ms;			/* their reading, decoding and uploading */
	double	decode_ms;		/* of it the decoding (5.6: what threads would share; PNG and TGA) */
} loads;

static void SetId (fileid_t *id, int f)
{
	memset (id, 0, sizeof(*id));
	if (f < 0)
		return;
	q_strlcpy (id->name, idx.files[f].name, sizeof(id->name));
	q_strlcpy (id->source, idx.sources[idx.files[f].source].path, sizeof(id->source));
	id->filepos = idx.files[f].filepos;
	id->size = idx.files[f].size;
	id->mtime = idx.files[f].mtime;
}

static qboolean SameFile (const fileid_t *id, int f)
{
	if (f < 0)
		return !id->name[0];
	return !strcmp (id->name, idx.files[f].name) && !strcmp (id->source, idx.sources[idx.files[f].source].path) &&
	       id->filepos == idx.files[f].filepos && id->size == idx.files[f].size && id->mtime == idx.files[f].mtime;
}

/* index file f's bytes (malloc'd, a 0 after them), read here rather than
 * through quakefs.c, which ends the game on a file it can't read to its
 * end (Sys_Error): a loose file an editor is still writing has a stale
 * size in its folder, or can't be opened. false: why says why. */
static qboolean ReadIndexFile (int f, byte **data, size_t *len, char *why, size_t whysize)
{
	const indexfile_t	*e = &idx.files[f];
	const source_t		*s = &idx.sources[e->source];
	char			path[MAX_OSPATH * 2];
	FILE			*fp;
	long			size;

	*data = NULL;
	*len = 0;
	if (s->pak)
		q_strlcpy (path, s->path, sizeof(path));
	else if (q_snprintf (path, sizeof(path), "%s/%s", s->path, e->name) >= (int)sizeof(path))
	{
		q_strlcpy (why, "the path is too long", whysize);
		return false;
	}
	if ((fp = fopen (path, "rb")) == NULL)
	{
		q_strlcpy (why, "can't be opened (being written? r_reloadmaterials again)", whysize);
		return false;
	}
	if (s->pak)
		size = (fseek (fp, e->filepos, SEEK_SET) == 0) ? e->size : -1;	/* paks don't change while the game runs */
	else
		size = (fseek (fp, 0, SEEK_END) == 0) ? ftell (fp) : -1;	/* its size now, not the listing's */
	if (size < 0 || (!s->pak && fseek (fp, 0, SEEK_SET) != 0) || (*data = (byte *) malloc ((size_t)size + 1)) == NULL)
	{
		fclose (fp);
		q_strlcpy (why, "can't be read", whysize);
		return false;
	}
	if (size > 0 && fread (*data, (size_t)size, 1, fp) != 1)
	{
		fclose (fp);
		free (*data);
		*data = NULL;
		q_strlcpy (why, "can't be read to its end (being written? r_reloadmaterials again)", whysize);
		return false;
	}
	fclose (fp);
	(*data)[size] = 0;
	*len = (size_t)size;
	return true;
}

/* index file f read and parsed into img; false: refused, img->error or
 * why says why */
static qboolean ReadIndexImage (int f, vk_imagefile_t *img, char *why, size_t whysize)
{
	byte		*data;
	size_t		len;
	qboolean	ok;
	double		t0 = Sys_DoubleTime ();

	memset (img, 0, sizeof(*img));
	if (!ReadIndexFile (f, &data, &len, why, whysize))
		return false;
	ok = VK_ParseImageFile (idx.files[f].name, data, len, img);
	img->read_ms = (Sys_DoubleTime () - t0) * 1000.0 - img->decode_ms;
	loads.decode_ms += img->decode_ms;
	free (data);
	if (!ok)
		q_strlcpy (why, img->error, whysize);
	return ok;
}

static void RebuildImageHash (void)
{
	int		i;
	unsigned int	b;

	memset (image_hash, 0xff, sizeof(image_hash));
	for (i = 0; i < num_images; i++)
	{
		b = HashName (images[i].identifier) & (IMAGE_HASH - 1);
		images[i].next = image_hash[b];
		image_hash[b] = i;
	}
}

/* the image of identifier, made (empty) if there is none */
static int GetImage (const char *identifier)
{
	unsigned int	b = HashName (identifier) & (IMAGE_HASH - 1);
	int		i;

	for (i = image_hash[b]; i >= 0; i = images[i].next)
	{
		if (!strcmp (images[i].identifier, identifier))
			return i;
	}
	if (num_images == max_images)
	{
		int	max = q_max (max_images * 2, 256);
		image_t	*im = (image_t *) realloc (images, max * sizeof(*im));

		if (!im)
			Sys_Error ("%s: out of memory", __thisfunc__);
		images = im;
		max_images = max;
	}
	i = num_images++;
	memset (&images[i], 0, sizeof(images[i]));
	q_strlcpy (images[i].identifier, identifier, sizeof(images[i].identifier));
	images[i].next = image_hash[b];
	image_hash[b] = i;
	return i;
}

static void Refused (image_t *im, const char *file, const char *why)
{
	im->slot = 0;
	q_snprintf (im->error, sizeof(im->error), "%s: %s", file, why);
	loads.refused++;
}

/* uploads img as im's texture (mipmapped, repeating, as the originals) */
static void Upload (image_t *im, const vk_imagefile_t *img, const char *kind)
{
	double	texels = (double)img->width * img->height;

	im->kind = kind;
	im->format = img->format;
	im->width = img->width;
	im->height = img->height;
	im->levels = img->levels;
	im->alpha = VK_ImageFileHasAlpha (img);
	im->error[0] = '\0';
	/* a new slot only while TEXTURE_RESERVE stay free for the game's own
	 * textures (a changed file takes its old one) */
	if (VK_FindTexture (im->identifier) < 0 && numgltextures >= VK_MAX_TEXTURES - TEXTURE_RESERVE)
	{
		char	why[96];

		q_snprintf (why, sizeof(why), "the texture cache is nearly full (%d of %d slots)", numgltextures, VK_MAX_TEXTURES);
		Refused (im, im->identifier, why);
		return;
	}
	im->slot = VK_LoadImageTexture (im->identifier, img, TEX_MIPMAP);
	if (!im->slot)
	{
		Refused (im, im->identifier, "the texture cache is full");
		return;
	}
	/* BC7 and BC5 a byte per texel; the mips a third more */
	im->mb = texels * ((img->format == VK_FORMAT_R8G8B8A8_UNORM) ? 4.0 : 1.0) *
		 ((VK_TextureLevels (im->slot) > 1) ? 4.0 / 3.0 : 1.0) / (1024.0 * 1024.0);
}

/* the image of index file f: the one loaded if it hasn't changed, else
 * read (again) */
static int ReadImage (int f)
{
	int		i = GetImage (idx.files[f].name);
	image_t		*im = &images[i];
	vk_imagefile_t	img;
	char		why[160];
	double		t0;

	if (im->id[0].name[0] && SameFile (&im->id[0], f))
		return i;
	SetId (&im->id[0], f);
	SetId (&im->id[1], -1);
	t0 = Sys_DoubleTime ();
	loads.read++;
	if (!ReadIndexImage (f, &img, why, sizeof(why)))
		Refused (im, im->id[0].name, why);
	else
		Upload (im, &img, img.kind);
	VK_FreeImageFile (&img);
	loads.ms += (Sys_DoubleTime () - t0) * 1000.0;
	return i;
}

/* one of a pair: an 8-bit PNG or TGA read into img; false: refused */
static qboolean ReadPairFile (image_t *im, int f, vk_imagefile_t *img)
{
	char	why[160];

	memset (img, 0, sizeof(*img));
	if (f < 0)
		return true;	/* missing: 255 */
	if (!ReadIndexImage (f, img, why, sizeof(why)))
	{
		Refused (im, idx.files[f].name, why);
		return false;
	}
	if (img->format != VK_FORMAT_R8G8B8A8_UNORM || (strcmp (img->kind, "PNG") && strcmp (img->kind, "TGA")))
	{
		Refused (im, idx.files[f].name, "_r and _m only as PNG or TGA (they are packed into one texture at load)");
		return false;
	}
	return true;
}

/* _r (file r) and _m (file m) packed into one RGBA8 texture: G roughness,
 * B metallic, 255 where one is missing (R, occlusion, and A 255) */
static int ReadPacked (const char *identifier, int r, int m)
{
	int		i = GetImage (identifier);
	image_t		*im = &images[i];
	vk_imagefile_t	ri, mi, packed;
	const vk_imagefile_t	*size;
	byte		*d;
	size_t		k, n;
	double		t0;

	if (im->id[0].name[0] || im->id[1].name[0])
	{
		if (SameFile (&im->id[0], r) && SameFile (&im->id[1], m))
			return i;
	}
	SetId (&im->id[0], r);
	SetId (&im->id[1], m);
	t0 = Sys_DoubleTime ();
	loads.read += (r >= 0) + (m >= 0);
	memset (&mi, 0, sizeof(mi));
	if (ReadPairFile (im, r, &ri) && ReadPairFile (im, m, &mi))
	{
		size = ri.data ? &ri : &mi;
		if (ri.data && mi.data && (ri.width != mi.width || ri.height != mi.height))
		{
			char	why[128];

			q_snprintf (why, sizeof(why), "%dx%d, its _m %dx%d: _r and _m must have the same size, both left out",
				    ri.width, ri.height, mi.width, mi.height);
			Refused (im, idx.files[r].name, why);
		}
		else if ((d = (byte *) malloc ((size_t)size->width * size->height * 4)) == NULL)
			Refused (im, identifier, "out of memory");
		else
		{
			n = (size_t)size->width * size->height;
			for (k = 0; k < n; k++)
			{
				d[k * 4 + 0] = 255;
				d[k * 4 + 1] = ri.data ? ri.data[k * 4] : 255;
				d[k * 4 + 2] = mi.data ? mi.data[k * 4] : 255;
				d[k * 4 + 3] = 255;
			}
			memset (&packed, 0, sizeof(packed));
			packed.format = VK_FORMAT_R8G8B8A8_UNORM;
			packed.width = size->width;
			packed.height = size->height;
			packed.levels = 1;
			packed.size[0] = n * 4;
			packed.data = d;
			Upload (im, &packed, "packed");
			free (d);
		}
	}
	VK_FreeImageFile (&ri);
	VK_FreeImageFile (&mi);
	loads.ms += (Sys_DoubleTime () - t0) * 1000.0;
	return i;
}


/* ==========================================================================
 * The sets: an original texture's maps and settings
 * ========================================================================== */

enum { MAP_ALBEDO, MAP_NORMAL, MAP_RM, MAP_EMISSIVE, NUM_MAPS };
static const char *map_names[NUM_MAPS] = { "albedo", "normal", "roughness/metallic", "emissive" };

static const char *kind_names[] = { "regular", "chrome", "glass" };	/* MATKIND_* */

static const char *FormatName (VkFormat format);

typedef struct
{
	int		slot;			/* the original texture's */
	unsigned short	crc;
	int		use;			/* MATUSE_* */
	char		source[MAX_QPATH];	/* the slot's name it was made for */
	char		name[MAX_QPATH];	/* the spec's, textures/<name>, "" = none (too long) */
	qboolean	resolved;
	qboolean	any;			/* a file was found */
	vk_matset_t	set;
	int		image[NUM_MAPS];	/* in images[], -1 = none */
	char		mat[MAX_QPATH];		/* the .mat used, "" = none */
	problems_t	problems;
} texset_t;

static texset_t	*sets[VK_MAX_TEXTURES];		/* by the original's slot */
static texset_t	sky_set;			/* the world's sky (5.5): it has no slot */
static struct
{
	char		file[MAX_QPATH];	/* read, "" = none */
	int		width, height;
	const char	*kind;
	qboolean	alpha;
} sky_file;
static cvar_t	r_materials = {"r_materials", "1", CVAR_NONE};

/* the spec's name of a texture: textures/, lowercase, * as #, a picture's
 * .lmp cut; false if it doesn't fit */
static qboolean SpecName (const char *source, char *name, size_t size)
{
	size_t	len;
	char	*c;

	if (q_snprintf (name, size, "%s/%s", TEXTURES_FOLDER, source) >= (int)size)
		return false;
	q_strlwr (name);
	for (c = name; *c; c++)
	{
		if (*c == '*')
			*c = '#';
	}
	len = strlen (name);
	if (len > 4 && !strcmp (name + len - 4, ".lmp"))
		name[len - 4] = '\0';
	return true;
}

/* a number in [lo, hi] (hi < 0: no upper bound) */
static qboolean ParseNumber (const char *s, float lo, float hi, float *value)
{
	char	*end;
	double	v = strtod (s, &end);

	if (end == s || *end || !(v == v) || v < lo || (hi >= 0.0f && v > hi) || v > 65504.0)	/* the table holds halfs */
		return false;
	*value = (float)v;
	return true;
}

/* the .mat's settings (MATERIALS.md): key value per line, # comments */
static void ParseMat (texset_t *s, int f)
{
	const char	*file = idx.files[f].name;
	vk_matset_t	*m = &s->set;
	char		*data, *line, *next;
	int		lineno;

	q_strlcpy (s->mat, file, sizeof(s->mat));
	{
		char	why[160];
		size_t	len;

		if (!ReadIndexFile (f, (byte **)&data, &len, why, sizeof(why)))	/* ended by a 0 */
		{
			AddProblem (&s->problems, "%s: %s", file, why);
			return;
		}
	}
	if (s->use == MATUSE_SPRITE)
	{
		AddProblem (&s->problems, "%s: sprites are unlit effects, only their albedo applies; left out", file);
		free (data);
		return;
	}
	for (line = data, lineno = 1; line; line = next, lineno++)
	{
		char	*key, *value, *extra, *c;
		float	v, *dst = NULL, lo = 0.0f, hi = -1.0f;

		if ((next = strchr (line, '\n')) != NULL)
			*next++ = '\0';
		if ((c = strchr (line, '#')) != NULL)
			*c = '\0';
		key = strtok (line, " \t\r");
		if (!key)
			continue;
		value = strtok (NULL, " \t\r");
		extra = value ? strtok (NULL, " \t\r") : NULL;
		if (!value || extra)
		{
			AddProblem (&s->problems, "%s line %d: \"%s\" needs one value, left out", file, lineno, key);
			continue;
		}
		if (!q_strcasecmp (key, "kind"))
		{
			int	k;

			for (k = 0; k < (int)(sizeof(kind_names) / sizeof(kind_names[0])); k++)
			{
				if (!q_strcasecmp (value, kind_names[k]))
					break;
			}
			if (k == (int)(sizeof(kind_names) / sizeof(kind_names[0])))
				AddProblem (&s->problems, "%s line %d: kind %s: regular, chrome or glass; left out", file, lineno, value);
			else if (s->use == MATUSE_WORLD && (s->source[0] == '*' || !q_strncasecmp (s->source, "sky", 3)) &&
				 k != MATKIND_REGULAR)
				AddProblem (&s->problems, "%s line %d: kind %s: this texture's name gives its kind; left out", file, lineno, value);
			else if (s->use == MATUSE_SKIN && k == MATKIND_GLASS)
				AddProblem (&s->problems, "%s line %d: kind glass is for world textures; left out", file, lineno);
			else if (s->use == MATUSE_WORLD && s->source[0] == '+' && s->source[1] && s->source[1] != '0' &&
				 q_tolower (s->source[1]) != 'a' && k != MATKIND_REGULAR)
				AddProblem (&s->problems, "%s line %d: kind %s: an animated texture's kind is its first frame's "
					    "(+0..., +a...: a surface's triangles keep it); left out", file, lineno, value);
			else
			{
				m->kind = k;	/* the world's at map load (vk_world.c), a skin's live */
				m->kind_set = true;	/* 6.20: any kind replaces rtex199's window pane */
			}
			continue;
		}
		if (!q_strcasecmp (key, "roughness"))
			dst = &m->roughness, hi = 1.0f;
		else if (!q_strcasecmp (key, "metallic"))
			dst = &m->metallic, hi = 1.0f;
		else if (!q_strcasecmp (key, "bump"))
			dst = &m->bump;
		else if (!q_strcasecmp (key, "specular"))
			dst = &m->specular;
		else if (!q_strcasecmp (key, "emissive"))
			dst = &m->emission;
		if (!dst)
		{
			AddProblem (&s->problems, "%s line %d: unknown setting \"%s\", left out", file, lineno, key);
			continue;
		}
		if (!ParseNumber (value, lo, hi, &v))
		{
			AddProblem (&s->problems, "%s line %d: %s %s: a number %s, left out", file, lineno, key, value,
				    (hi >= 0.0f) ? "from 0 to 1" : "of 0 or more");
			continue;
		}
		*dst = v;
	}
	free (data);
}

/* the image of a map into the set, or why not */
static void UseImage (texset_t *s, int map, int image)
{
	const image_t	*im = &images[image];
	vk_matset_t	*m = &s->set;

	s->image[map] = image;
	if (!im->slot)
	{
		AddProblem (&s->problems, "%s", im->error);
		return;
	}
	if (s->use == MATUSE_SPRITE && map != MAP_ALBEDO)
	{
		AddProblem (&s->problems, "%s: sprites are unlit effects, only their albedo applies; left out", im->identifier);
		return;
	}
	if ((im->format == VK_FORMAT_BC5_UNORM_BLOCK) != (map == MAP_NORMAL) && im->format != VK_FORMAT_R8G8B8A8_UNORM)
	{
		AddProblem (&s->problems, "%s: %s is for %s (MATERIALS.md: BC7 or RGBA8 for colors and _orm, BC5 or RGBA8 for normal maps); left out",
			    im->identifier, (im->format == VK_FORMAT_BC5_UNORM_BLOCK) ? "BC5" : "BC7",
			    (im->format == VK_FORMAT_BC5_UNORM_BLOCK) ? "normal maps" : "colors and _orm");
		return;
	}
	switch (map)
	{
	case MAP_ALBEDO:
		m->albedo = im->slot;
		m->albedo_alpha = im->alpha;
		break;
	case MAP_NORMAL:
		m->normal = im->slot;
		m->normal_bc5 = (im->format == VK_FORMAT_BC5_UNORM_BLOCK);
		break;
	case MAP_RM:
		m->rm = im->slot;
		break;
	default:
		m->emissive = im->slot;
		break;
	}
}

static void ResolveSet (texset_t *s)
{
	char	names[2][MAX_QPATH], identifier[MAX_QPATH];
	int	n, f, orm, r, m, num_names = 2;

	ClearProblems (&s->problems);
	memset (&s->set, 0, sizeof(s->set));
	s->set.roughness = s->set.metallic = s->set.bump = s->set.specular = s->set.emission = -1.0f;
	for (n = 0; n < NUM_MAPS; n++)
		s->image[n] = -1;
	s->mat[0] = '\0';
	s->any = false;
	s->resolved = true;
	if (!idx.valid)
		BuildIndex ();

	/* the qualified name first */
	if (q_snprintf (names[0], sizeof(names[0]), "%s~%04x", s->name, s->crc) >= (int)sizeof(names[0]))
	{
		q_strlcpy (names[0], s->name, sizeof(names[0]));
		num_names = 1;
	}
	q_strlcpy (names[1], s->name, sizeof(names[1]));

	for (n = 0, f = -1; n < num_names && f < 0; n++)
		f = FindFile (names[n], "", all_extensions);
	if (f >= 0)
		UseImage (s, MAP_ALBEDO, ReadImage (f));
	for (n = 0, f = -1; n < num_names && f < 0; n++)
		f = FindFile (names[n], "_n", all_extensions);
	if (f >= 0)
		UseImage (s, MAP_NORMAL, ReadImage (f));
	for (n = 0, f = -1; n < num_names && f < 0; n++)
		f = FindFile (names[n], "_e", all_extensions);
	if (f >= 0)
		UseImage (s, MAP_EMISSIVE, ReadImage (f));

	/* roughness and metallic: the first name with any of _orm, _r, _m */
	for (n = 0; n < num_names; n++)
	{
		orm = FindFile (names[n], "_orm", all_extensions);
		r = FindFile (names[n], "_r", all_extensions);
		m = FindFile (names[n], "_m", all_extensions);
		if (orm < 0 && r < 0 && m < 0)
			continue;
		if (orm >= 0)
		{
			if (r >= 0 || m >= 0)
				AddProblem (&s->problems, "%s_r, _m: %s is used instead", names[n], idx.files[orm].name);
			UseImage (s, MAP_RM, ReadImage (orm));
			s->set.roughness_map = s->set.metallic_map = (s->set.rm != 0);
		}
		else if (q_snprintf (identifier, sizeof(identifier), "%s_rm", names[n]) >= (int)sizeof(identifier))
			AddProblem (&s->problems, "%s_r, _m: the name is too long to pack them (%d characters at most)",
				    names[n], (int)sizeof(identifier) - 4);
		else
		{
			UseImage (s, MAP_RM, ReadPacked (identifier, r, m));
			s->set.roughness_map = s->set.rm && r >= 0;
			s->set.metallic_map = s->set.rm && m >= 0;
		}
		break;
	}

	for (n = 0, f = -1; n < num_names && f < 0; n++)
	{
		char	path[MAX_QPATH];

		if (q_snprintf (path, sizeof(path), "%s.mat", names[n]) < (int)sizeof(path))
			f = IndexFind (path);
	}
	if (f >= 0)
		ParseMat (s, f);
	if (s->set.bump >= 0.0f && !s->set.normal)
		AddProblem (&s->problems, "%s: bump without a normal map, nothing to scale", s->mat);

	for (n = 0; n < NUM_MAPS; n++)
	{
		if (s->image[n] >= 0)
			s->any = true;
	}
	if (s->mat[0])
		s->any = true;
}

static void FreeSet (int slot)
{
	if (!sets[slot])
		return;
	ClearProblems (&sets[slot]->problems);
	free (sets[slot]);
	sets[slot] = NULL;
}

/* the set of slot, made for its texture (again, if the slot now holds
 * another one); NULL: none possible */
static texset_t *GetSet (int slot, const char *name, int use)
{
	texset_t	*s;
	const char	*source;

	if (slot <= 0 || slot >= VK_MAX_TEXTURES)
		return NULL;
	s = sets[slot];
	source = VK_TextureName (slot)[0] ? VK_TextureName (slot) : name ? name : s ? s->source : NULL;
	if (!source || !source[0])
		return NULL;
	if (s && (s->crc != VK_TextureCRC (slot) || strcmp (s->source, source)))
	{
		FreeSet (slot);
		s = NULL;
	}
	if (!s)
	{
		if ((s = (texset_t *) calloc (1, sizeof(*s))) == NULL)
			return NULL;
		s->slot = slot;
		s->crc = VK_TextureCRC (slot);
		s->use = use;
		q_strlcpy (s->source, source, sizeof(s->source));
		if (!SpecName (source, s->name, sizeof(s->name)))
			s->name[0] = '\0';
		sets[slot] = s;
	}
	return s;
}

const vk_matset_t *VK_MaterialSet (int slot, const char *name, int use)
{
	texset_t	*s = GetSet (slot, name, use);

	if (!s || !r_materials.integer || !s->name[0])
		return NULL;
	if (!s->resolved)
		ResolveSet (s);
	return s->any ? &s->set : NULL;
}

/* the sky's file (5.5): its albedo is the whole sky, the two layers side by
 * side, which vk_sky.c splits and edits on the CPU (the front's
 * transparent texels take the back's average color), so it is read here
 * rather than uploaded; the other maps and a .mat don't apply (the sky is
 * unlit: its color is its radiance) */
qboolean VK_SkyImageFile (const char *name, unsigned short crc, vk_imagefile_t *img)
{
	static const char	*others[] = { "_n", "_orm", "_r", "_m", "_e" };
	texset_t		*s = &sky_set;
	char			names[2][MAX_QPATH], path[MAX_QPATH], why[160];
	int			n, k, f = -1, g, num_names = 2;
	double			t0 = Sys_DoubleTime ();

	memset (img, 0, sizeof(*img));
	ClearProblems (&s->problems);
	memset (s, 0, sizeof(*s));
	memset (&sky_file, 0, sizeof(sky_file));
	for (n = 0; n < NUM_MAPS; n++)
		s->image[n] = -1;
	if (!r_materials.integer || !name[0])
		return false;
	s->crc = crc;
	s->use = MATUSE_SKY;
	q_strlcpy (s->source, name, sizeof(s->source));
	if (!SpecName (name, s->name, sizeof(s->name)))
		return false;
	s->resolved = true;
	if (!idx.valid)
		BuildIndex ();
	if (q_snprintf (names[0], sizeof(names[0]), "%s~%04x", s->name, crc) >= (int)sizeof(names[0]))
	{
		q_strlcpy (names[0], s->name, sizeof(names[0]));
		num_names = 1;
	}
	q_strlcpy (names[1], s->name, sizeof(names[1]));

	for (n = 0; n < num_names; n++)
	{
		if (f < 0)
			f = FindFile (names[n], "", all_extensions);
		for (k = 0; k < (int)Q_COUNTOF(others); k++)
		{
			if ((g = FindFile (names[n], others[k], all_extensions)) >= 0)
				AddProblem (&s->problems, "%s: the sky is unlit, only its albedo applies (its two layers); left out",
					    idx.files[g].name);
		}
		if (q_snprintf (path, sizeof(path), "%s.mat", names[n]) < (int)sizeof(path) && (g = IndexFind (path)) >= 0)
			AddProblem (&s->problems, "%s: the sky is unlit, only its albedo applies (its two layers); left out",
				    idx.files[g].name);
	}
	s->any = (f >= 0 || s->problems.count);
	if (f < 0)
		return false;

	loads.read++;
	q_strlcpy (sky_file.file, idx.files[f].name, sizeof(sky_file.file));
	if (!ReadIndexImage (f, img, why, sizeof(why)))
		AddProblem (&s->problems, "%s: %s", idx.files[f].name, why);
	else if (img->format != VK_FORMAT_R8G8B8A8_UNORM)
		AddProblem (&s->problems, "%s: %s: the sky's layers are split and edited at load: PNG, TGA or an RGBA8 DDS or KTX2; left out",
			    idx.files[f].name, FormatName (img->format));
	else if (img->width != 2 * img->height)
		AddProblem (&s->problems, "%s: %dx%d: the sky is its two square layers side by side (2:1, as the original's 256x128); left out",
			    idx.files[f].name, img->width, img->height);
	else
	{
		sky_file.width = img->width;
		sky_file.height = img->height;
		sky_file.kind = img->kind;
		sky_file.alpha = VK_SkyFrontHasAlpha (img);
		loads.ms += (Sys_DoubleTime () - t0) * 1000.0;
		return true;
	}
	loads.refused++;
	VK_FreeImageFile (img);
	memset (img, 0, sizeof(*img));
	loads.ms += (Sys_DoubleTime () - t0) * 1000.0;
	return false;
}

int VK_SpriteTexture (int slot, int *original, qboolean *coverage)
{
	const vk_matset_t	*s = VK_MaterialSet (slot, NULL, MATUSE_SPRITE);

	*original = 0;
	*coverage = false;
	if (!s || !s->albedo)
		return slot;
	*original = slot;
	*coverage = !s->albedo_alpha;
	return s->albedo;
}

/* a precached sprite's frames, so that they don't load while drawn */
void VK_PreloadSpriteFiles (qmodel_t *model)
{
	const msprite_t	*psprite = (const msprite_t *) model->cache.data;
	int		i, j, original;
	qboolean	coverage;

	if (!psprite)
		return;
	for (i = 0; i < psprite->numframes; i++)
	{
		if (psprite->frames[i].type == SPR_SINGLE)
			VK_SpriteTexture ((int)psprite->frames[i].frameptr->gl_texturenum, &original, &coverage);
		else
		{
			const mspritegroup_t	*group = (const mspritegroup_t *) psprite->frames[i].frameptr;

			for (j = 0; j < group->numframes; j++)
				VK_SpriteTexture ((int)group->frames[j]->gl_texturenum, &original, &coverage);
		}
	}
}

/* the sprites the client loads at startup (below gl_texlevel: puffs,
 * sparks, explosions), whose replaced frames the purge took with the
 * map's textures: looked up again on map load, not when first drawn */
void VK_PreloadStartupSprites (void)
{
	int		i, original;
	qboolean	coverage;

	for (i = 1; i < gl_texlevel && i < VK_MAX_TEXTURES; i++)
	{
		if (strstr (VK_TextureName (i), ".spr_"))
			VK_SpriteTexture (i, &original, &coverage);
	}
}

/* a map load: the files may have changed since the last one */
void VK_MaterialFilesNewMap (void)
{
	int	i;

	idx.valid = false;
	for (i = 0; i < VK_MAX_TEXTURES; i++)
	{
		if (sets[i])
			sets[i]->resolved = false;
	}
	sky_set.resolved = false;	/* VK_LoadSky looks again */
	memset (&loads, 0, sizeof(loads));
}

/* D_ClearOpenGLTextures freed the slots from first on: the sets (made
 * again when asked for) and the images there */
void VK_MaterialFilesPurged (int first)
{
	int	i, n;

	for (i = 0; i < VK_MAX_TEXTURES; i++)
		FreeSet (i);
	for (i = n = 0; i < num_images; i++)
	{
		if (images[i].slot > 0 && images[i].slot < first)
			images[n++] = images[i];
	}
	num_images = n;
	RebuildImageHash ();
}


/* ==========================================================================
 * Reports and commands
 * ========================================================================== */

typedef struct
{
	int	sets, with_files, images, problems;
	double	mb;
} counts_t;

static void Count (counts_t *c)
{
	int	i, k;

	memset (c, 0, sizeof(*c));
	c->problems = idx.problems.count;
	for (i = 0; i < VK_MAX_TEXTURES; i++)
	{
		const texset_t	*s = sets[i];

		if (!s || !s->resolved)
			continue;
		c->sets++;
		c->with_files += s->any;
		c->problems += s->problems.count;
	}
	if (sky_set.resolved)
	{
		c->sets++;
		c->with_files += sky_set.any;
		c->problems += sky_set.problems.count;
	}
	for (k = 0; k < num_images; k++)
	{
		if (images[k].slot)
		{
			c->images++;
			c->mb += images[k].mb;
		}
	}
}

/* R_NewMap: one line, when there are files or problems (outside frames) */
void VK_ReportMaterialFiles (void)
{
	counts_t	c;

	if (!r_materials.integer)
		return;
	Count (&c);
	if (!c.with_files && !c.problems)
		return;
	Con_Printf ("materials: %d of %d textures with files, %d files read in %.0f ms (%.0f decoding; %d images kept, %.1f MB)%s",
		    c.with_files, c.sets, loads.read, loads.ms, loads.decode_ms, c.images, c.mb, c.problems ? ", " : "\n");
	if (c.problems)
		Con_Printf ("%d problems (vk_materials problems)\n", c.problems);
}

/* after the sets were resolved again (r_reloadmaterials) or r_materials
 * changed, the GPU idle: the materials, the lava's lights (5.5: their
 * colors follow the files), the sky's layers */
static void FilesChanged (void)
{
	VK_ReapplyMaterials ();
	VK_LavaFileColors ();
	VK_RebuildLights ();
	VK_ReloadSkyFile ();
	VK_SpriteLightAverages ();	/* 6.2: the emitting sprites' frames show their files */
	VK_BeamLightAverages ();	/* 6.3: and the beam models */
}

/* a world texture's kind is in the geometry: a changed one applies at the
 * next map load (5.5; Quake II RTX, too, builds the map's geometry again) */
static void ReportKinds (void)
{
	char	first[MAX_QPATH];
	int	n = VK_WorldKindsChanged (first, sizeof(first));

	if (n)
		Con_Printf ("the kind of %d world texture%s (%s%s) changed: it applies at the next map load\n",
			    n, (n == 1) ? "" : "s", first, (n > 1) ? ", ..." : "");
}

static void R_ReloadMaterials_f (void)
{
	double		t0 = Sys_DoubleTime ();
	counts_t	c;
	int		i;

	if (vk_num_materials <= 1)
	{
		Con_Printf ("r_reloadmaterials: no map\n");
		return;
	}
	vkDeviceWaitIdle (vk.device);
	idx.valid = false;
	memset (&loads, 0, sizeof(loads));
	for (i = 0; i < VK_MAX_TEXTURES; i++)
	{
		if (sets[i])
			sets[i]->resolved = false;	/* with r_materials 0 too: resolved when it is 1 again */
	}
	for (i = 0; i < VK_MAX_TEXTURES && r_materials.integer; i++)
	{
		if (sets[i])
			VK_MaterialSet (i, NULL, sets[i]->use);	/* a sprite's too */
	}
	FilesChanged ();
	Count (&c);
	Con_Printf ("r_reloadmaterials: %d of %d textures with files, %d files read (%.0f ms, %.0f decoding), %d problems; %.0f ms in all\n",
		    c.with_files, c.sets, loads.read, loads.ms, loads.decode_ms, c.problems, (Sys_DoubleTime () - t0) * 1000.0);
	ReportKinds ();
}

static void MaterialsChanged (cvar_t *var)
{
	(void)var;
	if (vk_num_materials <= 1 || !vk.device)
		return;
	vkDeviceWaitIdle (vk.device);
	FilesChanged ();
	ReportKinds ();
}

static const char *FormatName (VkFormat format)
{
	switch (format)
	{
	case VK_FORMAT_BC7_UNORM_BLOCK:	return "BC7";
	case VK_FORMAT_BC5_UNORM_BLOCK:	return "BC5";
	default:			return "RGBA8";
	}
}

static void PrintSet (const texset_t *s)
{
	const vk_matset_t	*m = &s->set;
	const int		used[NUM_MAPS] = { m->albedo, m->normal, m->rm, m->emissive };
	int			k;

	Con_Printf ("%4d %s~%04x:", s->slot, s->name + sizeof(TEXTURES_FOLDER), s->crc);
	for (k = 0; k < NUM_MAPS; k++)
	{
		const image_t	*im;

		if (s->image[k] < 0)
			continue;
		im = &images[s->image[k]];
		if (!im->slot)
			Con_Printf (" %s refused;", map_names[k]);
		else if (!used[k])
			Con_Printf (" %s %s left out;", map_names[k], im->identifier + sizeof(TEXTURES_FOLDER));
		else
			Con_Printf (" %s %s (%dx%d %s %s%s%s);", map_names[k], im->identifier + sizeof(TEXTURES_FOLDER),
				    im->width, im->height, im->kind, FormatName (im->format),
				    (k == MAP_ALBEDO && im->alpha) ? ", alpha" : "",
				    (k == MAP_RM) ? (m->roughness_map ? (m->metallic_map ? ", both" : ", roughness") : ", metallic") : "");
	}
	if (s->mat[0])
	{
		Con_Printf (" %s:", s->mat + sizeof(TEXTURES_FOLDER));
		if (m->roughness >= 0.0f)	Con_Printf (" roughness %g", m->roughness);
		if (m->metallic >= 0.0f)	Con_Printf (" metallic %g", m->metallic);
		if (m->bump >= 0.0f)		Con_Printf (" bump %g", m->bump);
		if (m->specular >= 0.0f)	Con_Printf (" specular %g", m->specular);
		if (m->emission >= 0.0f)	Con_Printf (" emissive %g", m->emission);
		if (m->kind != MATKIND_REGULAR)	Con_Printf (" kind %s", kind_names[m->kind]);
	}
	Con_Printf ("\n");
}

/* the mean of a texture's colors in linear light, and its luminance */
static void PrintMean (const char *label, const vec3_t mean)
{
	Con_Printf ("%s %.3f %.3f %.3f (luminance %.3f)", label, mean[0], mean[1], mean[2],
		    0.2126f * mean[0] + 0.7152f * mean[1] + 0.0722f * mean[2]);
}

/* vk_materials here (5.6): the texture at the view's center (the nearest
 * primary ray hit, vk_accel.c's VK_ProbeView): its names as MATERIALS.md
 * spells them, its files, and its albedo's mean in linear light against
 * the original's (texture_average.comp, without the lights' bias: the
 * shown texture as the GPU samples it, a BC7 file too) */
static void MaterialHere (void)
{
	const vk_material_t	*m;
	texset_t		*s;
	char			what[96];
	float			t;
	int			material, slots[2], n = 1, srgb = VK_ColorsSRGB () ? 1 : 0;
	vec3_t			means[2][2];

	if (vk_num_materials <= 1)
	{
		Con_Printf ("vk_materials here: no map\n");
		return;
	}
	if (!VK_ProbeView (&material, &t, what, sizeof(what)))
	{
		Con_Printf ("vk_materials here: nothing at the view's center\n");
		return;
	}
	if (material <= 0 || material >= vk_num_materials)
	{
		Con_Printf ("here: %s, %.0f units away: no material\n", what, t);
		return;
	}
	m = VK_GetMaterial (material);
	Con_Printf ("here: %s, %.0f units away: %s (material %d)\n", what, t, m->name, material);

	if (!(m->flags & VK_MAT_SKIN) && !q_strncasecmp (m->name, "sky", 3))	/* the sky's set has no slot (VK_SkyImageFile) */
	{
		if (sky_set.name[0])
			Con_Printf ("  the sky: %s~%04x (this sky) or %s (every sky of the name), 2:1, the layers side by side%s%s\n",
				    sky_set.name, sky_set.crc, sky_set.name, sky_file.file[0] ? "; read " : "",
				    sky_file.file[0] ? sky_file.file : "");
		return;
	}
	s = GetSet (m->texture, NULL, (m->flags & VK_MAT_SKIN) ? MATUSE_SKIN : MATUSE_WORLD);
	if (!s || !s->name[0])
	{
		Con_Printf ("  no material file name (too long)\n");
		return;
	}
	Con_Printf ("  files: %s~%04x (this texture's pixels only) or %s (every texture of the name)\n", s->name, s->crc, s->name);
	if (m->window_pane)	/* 6.20 */
		Con_Printf ("  a window pane: glass with r_windows %d where its brush entity is drawn translucent "
			    "(0 the game's blend); a kind in its .mat replaces it\n", VK_WindowPanes ());
	if (!r_materials.integer)
		Con_Printf ("  r_materials 0: the original texture only\n");
	else if (!s->resolved)	/* read-only: resolving here would read files the material doesn't show */
		Con_Printf ("  not looked up since the last map load or reload (r_reloadmaterials)\n");
	else
	{
		if (s->any)
		{
			Con_Printf ("  ");
			PrintSet (s);
		}
		else
			Con_Printf ("  none found\n");
		if (s->problems.count)
			Con_Printf ("  %d problems (vk_materials problems)\n", s->problems.count);
	}

	slots[0] = m->base_texture;
	if (m->original && m->original != m->base_texture)
		slots[n++] = m->original;
	vkDeviceWaitIdle (vk.device);
	VK_TextureAverages (slots, n, false, means);
	if (n == 2)
	{
		float	y0 = 0.2126f * means[0][srgb][0] + 0.7152f * means[0][srgb][1] + 0.0722f * means[0][srgb][2];
		float	y1 = 0.2126f * means[1][srgb][0] + 0.7152f * means[1][srgb][1] + 0.0722f * means[1][srgb][2];

		PrintMean ("  albedo: mean", means[0][srgb]);
		PrintMean (", the original's", means[1][srgb]);
		if (y1 > 0.0f)
			Con_Printf (": %.2f times", y0 / y1);
		Con_Printf (" (linear light, r_srgb %d)\n", srgb);
	}
	else
	{
		PrintMean ("  albedo: the original's, mean", means[0][srgb]);
		Con_Printf (" (linear light, r_srgb %d)\n", srgb);
	}
}

static void VK_Materials_f (void)
{
	const char	*arg = (Cmd_Argc () > 1) ? Cmd_Argv (1) : "";
	counts_t	c;
	int		i;

	if (!q_strcasecmp (arg, "here"))
	{
		MaterialHere ();
		return;
	}

	if (!q_strcasecmp (arg, "list"))
	{
		for (i = 0; i < VK_MAX_TEXTURES; i++)
		{
			if (sets[i] && sets[i]->resolved && sets[i]->any)
				PrintSet (sets[i]);
		}
		if (sky_set.resolved && sky_file.width)
			Con_Printf ("sky  %s~%04x: the layers %s (%dx%d %s RGBA8%s)\n", sky_set.name + sizeof(TEXTURES_FOLDER),
				    sky_set.crc, sky_file.file + sizeof(TEXTURES_FOLDER), sky_file.width, sky_file.height,
				    sky_file.kind, sky_file.alpha ? ", alpha" : ", the original's transparency");
		return;
	}
	if (!q_strcasecmp (arg, "problems"))
	{
		PrintProblems (&idx.problems);
		for (i = 0; i < VK_MAX_TEXTURES; i++)
		{
			if (sets[i] && sets[i]->resolved)
				PrintProblems (&sets[i]->problems);
		}
		if (sky_set.resolved)
			PrintProblems (&sky_set.problems);
		return;
	}
	if (arg[0])
	{
		Con_Printf ("vk_materials [list|problems|here]\n");
		return;
	}
	Count (&c);
	Con_Printf ("r_materials %d; %s\n", r_materials.integer, r_materials.integer ? "the material files apply" : "the original textures only");
	if (idx.valid)
		Con_Printf ("index: %d files under %s/ (%d game folders, and paks with files there), listed in %.1f ms\n",
			    idx.num_files, TEXTURES_FOLDER, idx.num_sources, idx.ms);
	Con_Printf ("%d textures looked up, %d with files; %d images in textures (%.1f MB); %d problems\n",
		    c.sets, c.with_files, c.images, c.mb, c.problems);
}

void VK_InitMaterialFiles (void)
{
	Cvar_RegisterVariable (&r_materials);
	Cvar_SetCallback (&r_materials, MaterialsChanged);
	Cmd_AddCommand ("r_reloadmaterials", R_ReloadMaterials_f);
	Cmd_AddCommand ("vk_materials", VK_Materials_f);
	memset (idx.hash, 0xff, sizeof(idx.hash));
	memset (image_hash, 0xff, sizeof(image_hash));
}

void VK_ShutdownMaterialFiles (void)
{
	int	i;

	for (i = 0; i < VK_MAX_TEXTURES; i++)
		FreeSet (i);
	ClearProblems (&sky_set.problems);
	memset (&sky_set, 0, sizeof(sky_set));
	free (images);
	images = NULL;
	num_images = max_images = 0;
	free (idx.files);
	free (idx.sources);
	ClearProblems (&idx.problems);
	memset (&idx, 0, sizeof(idx));
	memset (idx.hash, 0xff, sizeof(idx.hash));
	memset (image_hash, 0xff, sizeof(image_hash));
}
