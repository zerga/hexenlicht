/* vk_export.c -- the texture export (story 5.4)
 *
 * r_exporttextures [folder] writes every original texture of the game's
 * files as a PNG under its docs/hexenlicht/MATERIALS.md name, exactly as
 * the engine uploads it, and a manifest, textures.csv, into
 * <game folder>/<folder>/ (export: data1/export, portals/export with
 * -portals), never into the textures/ the engine reads. The starting
 * points for authors and their tools: the right names, the ~<crc>
 * variants, the exact pixels.
 * - What: the world textures of every map (maps/*.bsp, mip 0, the sky
 *   whole), the model skins (*.mdl, both formats), the sprite frames
 *   (*.spr), the stone and ice pictures (gfx/skin<n>.lmp), in models/ and
 *   gfx/ (maps/ for the maps); every occurrence in the search path (each
 *   pak and game folder, quakefs.c's FS_ListSearchPath), not only the one
 *   the game loads. The same name and pixels in several files is one
 *   file; a name with more than one set of pixels gets ~<crc> on every
 *   variant (MATERIALS.md), the others are plain. With -portals the
 *   search path holds data1 and the mission pack; without it data1 only.
 * - Exactly as uploaded: the pixels and the CRC as gl_model.c passes them
 *   to GL_LoadTexture (a model's first skin after Mod_FloodFillSkin, of
 *   which this is a copy; the other skins as they are), the colors by
 *   vk_texture.c's conversion (VK_Convert8Pixels) with the texture's
 *   flags: world TEX_MIPMAP, skins the model's mode (VK_SkinTextureMode:
 *   holey, transparent, special-trans), sprites TEX_MIPMAP | TEX_ALPHA,
 *   pictures TEX_ALPHA | TEX_NEAREST. The alpha is written where the
 *   conversion made one (TEX_ALPHA) and a texel's is below 255: holey,
 *   transparent and special-trans skins, sprites and pictures with
 *   transparent texels; the rest is RGB (their alpha isn't read).
 * - The files are read here (fopen of the pak or loose file), not through
 *   quakefs.c: nothing of the running game changes (no GPU work, no
 *   texture slots, no model cache). A console command, outside frames: it
 *   prints a line per kind, the totals and the problems (a file that
 *   can't be read or is cut short, an unknown format or version, a name
 *   Windows can't take, one too long for the material files' index or
 *   with the qualifier's ~, a CRC two images of a name share).
 * - textures.csv, a row per file: file, name (the engine's: *lava1,
 *   models/ball.mdl_0, gfx/skin100.lmp), crc, width, height, kind (world,
 *   liquid, sky, skin, sprite, picture), alpha (none, coverage: 0 or 255,
 *   translucent), variants (of the name), used in (the maps, else the
 *   model, sprite or picture file), from (the paks or game folders,
 *   relative to the base folder). A new export overwrites an earlier
 *   one's files; files it doesn't make stay.
 *
 * Mod_FloodFillSkin is id's (gl_model.c).
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
#include "q_ctype.h"
#include "vk_local.h"
#include "stb_image_write.h"
#include <windows.h>

#define DEFAULT_FOLDER	"export"
#define TEXTURES_FOLDER	"textures"
#define MAX_WALK_DEPTH	16	/* folders in folders under maps/, models/, gfx/ */
#define NAME_HASH	4096	/* buckets, a power of 2 */
#define MAX_PRINTED	40	/* problems printed; the rest counted */
/* the longest name (lowercased, * as #, without .lmp): vk_matfiles.c's index
 * takes relative paths below MAX_QPATH, and textures/<name>~<crc>_orm.ktx2
 * adds 23 characters */
#define MAX_NAME	(MAX_QPATH - 1 - 23)

typedef enum
{
	KIND_WORLD, KIND_LIQUID, KIND_SKY, KIND_SKIN, KIND_SPRITE, KIND_PICTURE, NUM_KINDS
} kind_t;

static const char *kind_names[NUM_KINDS] = { "world", "liquid", "sky", "skin", "sprite", "picture" };

typedef enum
{
	FILE_MAP, FILE_MODEL, FILE_SPRITE, FILE_PICTURE, NUM_FILE_KINDS
} filekind_t;

static const char *filekind_names[NUM_FILE_KINDS] = { "maps", "models", "sprites", "pictures" };

typedef struct
{
	char		*text;		/* space separated, malloc'd; NULL = empty */
	size_t		len, max;
} list_t;

typedef struct
{
	char		name[MAX_QPATH];	/* the spec's, without textures/: rtex343, #lava1, models/ball.mdl_0 */
	char		identifier[MAX_QPATH];	/* the engine's, of the first occurrence: *lava1, gfx/skin100.lmp */
	unsigned short	crc;
	int		width, height;
	int		flags;			/* GL_LoadTexture's */
	kind_t		kind;
	byte		*pixels;		/* 8-bit, width * height; malloc'd */
	list_t		used, from;
	int		variants;		/* of its name */
	int		next;			/* hash chain */
} variant_t;

typedef struct
{
	char		path[MAX_OSPATH];	/* the pak's file or the game folder */
	char		label[MAX_OSPATH];	/* relative to the base folder: data1/pak1.pak, portals */
	qboolean	pak;
} source_t;

typedef struct
{
	char		name[MAX_QPATH];	/* as in the pak, or the loose file's relative path */
	int		source;
	long		filepos, size;		/* in a pak; a loose file's is read */
	filekind_t	kind;
} entry_t;

static struct
{
	variant_t	*variants;
	int		num_variants, max_variants;
	int		hash[NAME_HASH];
	source_t	*sources;
	int		num_sources, max_sources;
	entry_t		*entries;
	int		num_entries, max_entries;
	const char	*last_pak;		/* while listing: the pak of the last entry */
	int		files_read[NUM_FILE_KINDS];
	list_t		problems;		/* lines, each ending in \n */
	int		num_problems;
} ex;


/* ==========================================================================
 * Lists and problems
 * ========================================================================== */

static void Append (list_t *l, const char *s, size_t n)
{
	if (l->len + n + 1 > l->max)
	{
		size_t	max = q_max (l->max * 2, l->len + n + 256);
		char	*t = (char *) realloc (l->text, max);

		if (!t)
			Sys_Error ("%s: out of memory", __thisfunc__);
		l->text = t;
		l->max = max;
	}
	memcpy (l->text + l->len, s, n);
	l->len += n;
	l->text[l->len] = '\0';
}

/* adds token (no spaces) unless the list has it; an empty one isn't added */
static void AddToken (list_t *l, const char *token)
{
	size_t		n = strlen (token);
	const char	*p = l->text;

	if (!n)
		return;		/* strstr would find it everywhere */
	while (p && (p = strstr (p, token)) != NULL)
	{
		if ((p == l->text || p[-1] == ' ') && (p[n] == ' ' || p[n] == '\0'))
			return;
		p += n;
	}
	if (l->len)
		Append (l, " ", 1);
	Append (l, token, n);
}

static void FreeList (list_t *l)
{
	free (l->text);
	memset (l, 0, sizeof(*l));
}

static void Problem (const char *format, ...) FUNC_PRINTF(1,2);
static void Problem (const char *format, ...)
{
	char	line[MAX_OSPATH + 256];
	va_list	args;
	int	n;

	va_start (args, format);
	n = q_vsnprintf (line, sizeof(line) - 1, format, args);
	va_end (args);
	if (n < 0 || n >= (int)sizeof(line) - 1)
		n = (int)strlen (line);
	line[n++] = '\n';
	Append (&ex.problems, line, n);
	ex.num_problems++;
}

static int ReadInt (const byte *p)
{
	int	i;

	memcpy (&i, p, sizeof(i));
	return LittleLong (i);
}


/* ==========================================================================
 * The files: every pak entry and loose file of the kinds, in the search
 * path's order
 * ========================================================================== */

static int AddSource (const char *path, qboolean pak)
{
	char		base[MAX_OSPATH], *c;
	source_t	*s;
	int		err;
	size_t		n;

	if (ex.num_sources == ex.max_sources)
	{
		int	max = q_max (ex.max_sources * 2, 16);

		s = (source_t *) realloc (ex.sources, max * sizeof(*s));
		if (!s)
			Sys_Error ("%s: out of memory", __thisfunc__);
		ex.sources = s;
		ex.max_sources = max;
	}
	s = &ex.sources[ex.num_sources];
	q_strlcpy (s->path, path, sizeof(s->path));
	s->pak = pak;

	/* the label: relative to the base folder, / separated */
	FS_MakePath_BUF (FS_BASEDIR, &err, base, sizeof(base), "");
	for (c = base; *c; c++)
	{
		if (*c == '\\')
			*c = '/';
	}
	q_strlcpy (s->label, path, sizeof(s->label));
	for (c = s->label; *c; c++)
	{
		if (*c == '\\')
			*c = '/';
	}
	n = strlen (base);
	if (!err && n && !q_strncasecmp (s->label, base, n))
		memmove (s->label, s->label + n, strlen (s->label + n) + 1);
	return ex.num_sources++;
}

/* the kind of file a relative name is, -1 = not one exported */
static int FileKind (const char *name)
{
	size_t		len = strlen (name);
	const char	*ext = (len > 4) ? name + len - 4 : "";
	const char	*digits;

	if (!q_strncasecmp (name, "maps/", 5))
		return q_strcasecmp (ext, ".bsp") ? -1 : FILE_MAP;
	if (q_strncasecmp (name, "models/", 7) && q_strncasecmp (name, "gfx/", 4))
		return -1;
	if (!q_strcasecmp (ext, ".mdl"))
		return FILE_MODEL;
	if (!q_strcasecmp (ext, ".spr"))
		return FILE_SPRITE;
	/* gfx/skin<n>.lmp: skins 100-255 (stone, ice; vk_skin.c's "gfx/skin%d.lmp") */
	if (q_strncasecmp (name, "gfx/skin", 8) || q_strcasecmp (ext, ".lmp") || ext - (name + 8) != 3 || name[8] == '0')
		return -1;
	for (digits = name + 8; digits < ext; digits++)
	{
		if (!q_isdigit (*digits))
			return -1;
	}
	return (atoi (name + 8) >= 100 && atoi (name + 8) <= 255) ? FILE_PICTURE : -1;
}

static void AddEntry (const char *name, int source, long filepos, long size, int kind)
{
	entry_t	*e;

	if (ex.num_entries == ex.max_entries)
	{
		int	max = q_max (ex.max_entries * 2, 1024);

		e = (entry_t *) realloc (ex.entries, max * sizeof(*e));
		if (!e)
			Sys_Error ("%s: out of memory", __thisfunc__);
		ex.entries = e;
		ex.max_entries = max;
	}
	e = &ex.entries[ex.num_entries++];
	q_strlcpy (e->name, name, sizeof(e->name));
	e->source = source;
	e->filepos = filepos;
	e->size = size;
	e->kind = (filekind_t)kind;
}

/* a loose folder's files of the kinds, and those of the folders in it */
static void WalkFolder (const char *ospath, const char *relative, int source, int depth)
{
	char			pattern[MAX_OSPATH], sub[MAX_OSPATH], rel[MAX_QPATH];
	WIN32_FIND_DATAA	fd;
	HANDLE			h;
	int			kind;

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
			Problem ("%s/%s/%s: the name is too long (%d characters at most), left out",
				 ex.sources[source].label, relative, fd.cFileName, MAX_QPATH - 1);
			continue;
		}
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
		{
			if (depth < MAX_WALK_DEPTH &&
			    q_snprintf (sub, sizeof(sub), "%s/%s", ospath, fd.cFileName) < (int)sizeof(sub))
				WalkFolder (sub, rel, source, depth + 1);
		}
		else if ((kind = FileKind (rel)) >= 0)
			AddEntry (rel, source, -1, -1, kind);
	} while (FindNextFileA (h, &fd));
	FindClose (h);
}

static void ListFolder (const char *ospath, void *ctx)
{
	static const char	*folders[] = { "maps", "models", "gfx" };
	char			path[MAX_OSPATH];
	int			i, source = AddSource (ospath, false);

	(void)ctx;
	for (i = 0; i < (int)Q_COUNTOF(folders); i++)
	{
		if (q_snprintf (path, sizeof(path), "%s/%s", ospath, folders[i]) < (int)sizeof(path))
			WalkFolder (path, folders[i], source, 0);
	}
	ex.last_pak = NULL;
}

static void ListPakFile (const char *name, long filepos, long size, const char *pakname, void *ctx)
{
	int	kind;

	(void)ctx;
	if (pakname != ex.last_pak)
	{
		AddSource (pakname, true);
		ex.last_pak = pakname;
	}
	if ((kind = FileKind (name)) >= 0)
		AddEntry (name, ex.num_sources - 1, filepos, size, kind);
}

/* an entry's bytes (malloc'd), NULL: a problem reported */
static byte *ReadEntry (const entry_t *e, size_t *len)
{
	const source_t	*s = &ex.sources[e->source];
	char		path[MAX_OSPATH * 2];
	FILE		*fp;
	long		size;
	byte		*data;

	*len = 0;
	if (s->pak)
		q_strlcpy (path, s->path, sizeof(path));
	else if (q_snprintf (path, sizeof(path), "%s/%s", s->path, e->name) >= (int)sizeof(path))
	{
		Problem ("%s in %s: the path is too long", e->name, s->label);
		return NULL;
	}
	if ((fp = fopen (path, "rb")) == NULL)
	{
		Problem ("%s in %s: can't be opened", e->name, s->label);
		return NULL;
	}
	if (s->pak)
		size = (fseek (fp, e->filepos, SEEK_SET) == 0) ? e->size : -1;
	else
		size = (fseek (fp, 0, SEEK_END) == 0) ? ftell (fp) : -1;
	if (size < 0 || (!s->pak && fseek (fp, 0, SEEK_SET) != 0) || (data = (byte *) malloc ((size_t)size + 1)) == NULL)
	{
		fclose (fp);
		Problem ("%s in %s: can't be read", e->name, s->label);
		return NULL;
	}
	if (size > 0 && fread (data, (size_t)size, 1, fp) != 1)
	{
		fclose (fp);
		free (data);
		Problem ("%s in %s: can't be read to its end", e->name, s->label);
		return NULL;
	}
	fclose (fp);
	*len = (size_t)size;
	return data;
}


/* ==========================================================================
 * The textures: one variant per name and set of pixels
 * ========================================================================== */

static unsigned int HashName (const char *s)
{
	unsigned int	h = 5381;

	while (*s)
		h = h * 33 + (unsigned char)*s++;
	return h;
}

/* vk_matfiles.c's SpecName without textures/: lowercase, * as #, a
 * picture's .lmp cut */
static void SpecName (const char *identifier, char *name, size_t size)
{
	size_t	len;
	char	*c;

	q_strlcpy (name, identifier, size);
	q_strlwr (name);
	for (c = name; *c; c++)
	{
		if (*c == '*')
			*c = '#';
	}
	len = strlen (name);
	if (len > 4 && !strcmp (name + len - 4, ".lmp"))
		name[len - 4] = '\0';
}

/* a texture as GL_LoadTexture gets it; used: the map, else the file */
static void AddTexture (const entry_t *e, const char *identifier, kind_t kind, const byte *pixels,
			int width, int height, int flags, const char *used)
{
	char		name[MAX_QPATH + 16];
	unsigned short	crc;
	unsigned int	b;
	variant_t	*v;
	byte		*copy;
	int		i;

	SpecName (identifier, name, sizeof(name));
	if (strlen (name) > MAX_NAME)
	{
		Problem ("%s in %s: the texture name %s is longer than %d characters (the material files' index takes "
			 "textures/<name>~<crc>_orm.ktx2 in %d), left out", e->name, ex.sources[e->source].label, identifier,
			 MAX_NAME, MAX_QPATH - 1);
		return;
	}
	if (strchr (name, '~'))
	{
		Problem ("%s in %s: the texture name %s has a ~ (the qualifier's), left out", e->name, ex.sources[e->source].label, identifier);
		return;
	}
	crc = CRC_Block ((byte *)pixels, width * height);
	b = HashName (name) & (NAME_HASH - 1);
	for (i = ex.hash[b]; i >= 0; i = ex.variants[i].next)
	{
		v = &ex.variants[i];
		if (strcmp (v->name, name) || v->crc != crc)
			continue;
		if (v->width != width || v->height != height || memcmp (v->pixels, pixels, (size_t)width * height))
		{
			Problem ("%s in %s: %s has the CRC %04x of another image of that name (%dx%d, from %s): "
				 "the qualifier can't tell them apart, left out",
				 e->name, ex.sources[e->source].label, identifier, crc, v->width, v->height, v->from.text);
			return;
		}
		AddToken (&v->used, used);
		AddToken (&v->from, ex.sources[e->source].label);
		return;
	}

	/* a file can claim 8192x8192 texels at overlapping offsets: a problem, not Sys_Error */
	if ((copy = (byte *) malloc ((size_t)width * height)) == NULL)
	{
		Problem ("%s in %s: no memory for %s (%dx%d), left out", e->name, ex.sources[e->source].label, identifier, width, height);
		return;
	}
	memcpy (copy, pixels, (size_t)width * height);
	if (ex.num_variants == ex.max_variants)
	{
		int	max = q_max (ex.max_variants * 2, 1024);

		v = (variant_t *) realloc (ex.variants, max * sizeof(*v));
		if (!v)
			Sys_Error ("%s: out of memory", __thisfunc__);
		ex.variants = v;
		ex.max_variants = max;
	}
	v = &ex.variants[ex.num_variants];
	memset (v, 0, sizeof(*v));
	q_strlcpy (v->name, name, sizeof(v->name));
	q_strlcpy (v->identifier, identifier, sizeof(v->identifier));
	v->crc = crc;
	v->width = width;
	v->height = height;
	v->flags = flags;
	v->kind = kind;
	v->pixels = copy;
	AddToken (&v->used, used);
	AddToken (&v->from, ex.sources[e->source].label);
	v->next = ex.hash[b];
	ex.hash[b] = ex.num_variants++;
}


/* ==========================================================================
 * Maps: the world textures (gl_model.c's Mod_LoadTextures)
 * ========================================================================== */

static void ReadMap (const entry_t *e, const byte *data, size_t len)
{
	char		map[MAX_QPATH], identifier[sizeof(((miptex_t *)0)->name) + 1], *dot;
	const byte	*lump;
	int		version, ofs, size, n, i;

	if (len < sizeof(dheader_t))
	{
		Problem ("%s in %s: cut short", e->name, ex.sources[e->source].label);
		return;
	}
	version = ReadInt (data);
#if defined(ENABLE_BSP2)
	if (version != BSPVERSION && version != BSP2VERSION)	/* BSP2's textures lump is BSP 29's */
#else
	if (version != BSPVERSION)
#endif
	{
		Problem ("%s in %s: BSP version %d, not %d or BSP2", e->name, ex.sources[e->source].label, version, BSPVERSION);
		return;
	}
	ofs = ReadInt (data + 4 + LUMP_TEXTURES * sizeof(lump_t));
	size = ReadInt (data + 4 + LUMP_TEXTURES * sizeof(lump_t) + 4);
	if (size == 0)
		return;		/* no textures */
	if (ofs < 0 || size < 4 || (size_t)ofs + (size_t)size > len)
	{
		Problem ("%s in %s: the textures lump is outside the file", e->name, ex.sources[e->source].label);
		return;
	}
	lump = data + ofs;
	n = ReadInt (lump);
	if (n < 0 || 4 + (size_t)n * 4 > (size_t)size)
	{
		Problem ("%s in %s: the textures lump is cut short", e->name, ex.sources[e->source].label);
		return;
	}

	/* the map's name: maps/demo1.bsp -> demo1 */
	q_strlcpy (map, e->name + 5, sizeof(map));
	if ((dot = strrchr (map, '.')) != NULL)
		*dot = '\0';
	q_strlwr (map);

	for (i = 0; i < n; i++)
	{
		const byte	*mt;
		int		mofs = ReadInt (lump + 4 + i * 4), w, h;
		kind_t		kind;

		if (mofs == -1)
			continue;
		if (mofs < 0 || (size_t)mofs + sizeof(miptex_t) > (size_t)size)
		{
			Problem ("%s in %s: texture %d is outside the textures lump", e->name, ex.sources[e->source].label, i);
			continue;
		}
		mt = lump + mofs;
		memcpy (identifier, mt, sizeof(identifier) - 1);
		identifier[sizeof(identifier) - 1] = '\0';
		w = ReadInt (mt + 16);
		h = ReadInt (mt + 20);
		if (!identifier[0])
		{
			Problem ("%s in %s: texture %d has no name, left out", e->name, ex.sources[e->source].label, i);
			continue;
		}
		if (!memchr (mt, 0, sizeof(identifier) - 1))
		{	/* GL_LoadTexture's name then runs on into the width */
			Problem ("%s in %s: texture %d's name %s has 16 characters and no end, left out",
				 e->name, ex.sources[e->source].label, i, identifier);
			continue;
		}
		/* the engine ends the game on sizes not a multiple of 16 */
		if (w <= 0 || h <= 0 || (w & 15) || (h & 15))
		{
			Problem ("%s in %s: %s is %dx%d (the engine takes multiples of 16), left out",
				 e->name, ex.sources[e->source].label, identifier, w, h);
			continue;
		}
		if (w > 8192 || h > 8192)
		{
			Problem ("%s in %s: %s is %dx%d, larger than 8192x8192, left out",
				 e->name, ex.sources[e->source].label, identifier, w, h);
			continue;
		}
		/* mip 0: the bytes after the header, as gl_model.c copies them */
		if ((size_t)mofs + sizeof(miptex_t) + (size_t)w * h > (size_t)size)
		{
			Problem ("%s in %s: %s is cut short", e->name, ex.sources[e->source].label, identifier);
			continue;
		}
		if (!strncmp (identifier, "sky", 3))
			kind = KIND_SKY;	/* R_InitSky's: the two layers are the engine's (5.5), the export has it whole */
		else if (identifier[0] == '*')
			kind = KIND_LIQUID;
		else
			kind = KIND_WORLD;
		AddTexture (e, identifier, kind, mt + sizeof(miptex_t), w, h, TEX_MIPMAP, map);
	}
}


/* ==========================================================================
 * Models: the skins (gl_model.c's Mod_LoadAllSkins)
 * ========================================================================== */

/* gl_model.c's Mod_FloodFillSkin (static there): fills the background
 * pixels reached from the top left so that mipmapping doesn't show haloes */
typedef struct
{
	short		x, y;
} floodfill_t;

/* must be a power of 2 */
#define FLOODFILL_FIFO_SIZE	0x1000
#define FLOODFILL_FIFO_MASK	(FLOODFILL_FIFO_SIZE - 1)

#define FLOODFILL_STEP( off, dx, dy )				\
do {								\
	if (pos[(off)] == fillcolor)				\
	{							\
		pos[(off)] = 255;				\
		fifo[inpt].x = x + (dx), fifo[inpt].y = y + (dy); \
		inpt = (inpt + 1) & FLOODFILL_FIFO_MASK;	\
	}							\
	else if (pos[(off)] != 255)				\
		fdc = pos[(off)];				\
} while (0)

static void FloodFillSkin (byte *skin, int skinwidth, int skinheight)
{
	byte		fillcolor = *skin;	/* assume this is the pixel to fill */
	floodfill_t	fifo[FLOODFILL_FIFO_SIZE];
	int		inpt = 0, outpt = 0;
	int		filledcolor = 0;
	int		i;

	/* attempt to find opaque black (gl_model.c's test, on the same table) */
	for (i = 0; i < 256; ++i)
	{
		if (d_8to24table[i] == (255 << 0))
		{
			filledcolor = i;
			break;
		}
	}

	/* can't fill to filled color or to transparent color (used as visited marker) */
	if ((fillcolor == filledcolor) || (fillcolor == 255))
		return;

	fifo[inpt].x = 0, fifo[inpt].y = 0;
	inpt = (inpt + 1) & FLOODFILL_FIFO_MASK;

	while (outpt != inpt)
	{
		int		x = fifo[outpt].x, y = fifo[outpt].y;
		int		fdc = filledcolor;
		byte		*pos = &skin[x + skinwidth * y];

		outpt = (outpt + 1) & FLOODFILL_FIFO_MASK;

		if (x > 0)
			FLOODFILL_STEP( -1, -1, 0 );
		if (x < skinwidth - 1)
			FLOODFILL_STEP( 1, 1, 0 );
		if (y > 0)
			FLOODFILL_STEP( -skinwidth, 0, -1 );
		if (y < skinheight - 1)
			FLOODFILL_STEP( skinwidth, 0, 1 );
		skin[x + skinwidth * y] = fdc;
	}
}

/* data is the file's bytes, which the fill changes */
static void ReadModel (const entry_t *e, byte *data, size_t len)
{
	const char	*label = ex.sources[e->source].label;
	char		identifier[MAX_QPATH + 16], used[MAX_QPATH];
	int		ident = ReadInt (data), version, numskins, w, h, flags, mode, i, j;
	size_t		header, s;
	byte		*end = data + len, *skin, *p;

	if (ident == RAPOLYHEADER)
		header = sizeof(newmdl_t);
	else
		header = sizeof(mdl_t);
	if (len < header)
	{
		Problem ("%s in %s: cut short", e->name, label);
		return;
	}
	version = ReadInt (data + 4);
	if (version != ((ident == RAPOLYHEADER) ? ALIAS_NEWVERSION : ALIAS_VERSION))
	{
		Problem ("%s in %s: model version %d, not %d", e->name, label, version,
			 (ident == RAPOLYHEADER) ? ALIAS_NEWVERSION : ALIAS_VERSION);
		return;
	}
	/* mdl_t and newmdl_t are the same up to the flags */
	numskins = ReadInt (data + offsetof(mdl_t, numskins));
	w = ReadInt (data + offsetof(mdl_t, skinwidth));
	h = ReadInt (data + offsetof(mdl_t, skinheight));
	flags = ReadInt (data + offsetof(mdl_t, flags));
	if (numskins < 1 || numskins > MAX_SKINS || w <= 0 || h <= 0 || h > MAX_SKIN_HEIGHT)
	{
		Problem ("%s in %s: %d skins of %dx%d (the engine ends the game on them), left out", e->name, label, numskins, w, h);
		return;
	}
	if (w > 8192)
	{
		Problem ("%s in %s: skins %d wide, wider than 8192, left out", e->name, label, w);
		return;
	}
	s = (size_t)w * h;
	mode = VK_SkinTextureMode (flags);
	q_strlcpy (used, e->name, sizeof(used));
	q_strlwr (used);

	p = data + header;		/* the skin's type */
	if ((size_t)(end - p) < 4)
		goto cut_short;
	skin = p + 4;			/* the one gl_model.c fills before every skin */
	for (i = 0; i < numskins; i++)
	{
		if ((size_t)(end - p) < 4)
			goto cut_short;
		if (ReadInt (p) == ALIAS_SKIN_SINGLE)
		{
			if ((size_t)(end - (p + 4)) < s)
				goto cut_short;
			FloodFillSkin (skin, w, h);
			q_snprintf (identifier, sizeof(identifier), "%s_%i", e->name, i);
			AddTexture (e, identifier, KIND_SKIN, p + 4, w, h, mode, used);
			p += 4 + s;
		}
		else		/* a group: gl_model.c takes any other type as one */
		{
			int	count;

			if ((size_t)(end - p) < 8)
				goto cut_short;
			count = ReadInt (p + 4);
			if (count < 0 || (size_t)count > (size_t)(end - (p + 8)) / 4)
				goto cut_short;
			p += 8 + (size_t)count * 4;	/* the intervals */
			for (j = 0; j < count; j++)
			{
				if ((size_t)(end - p) < s || (size_t)(end - skin) < s)
					goto cut_short;
				FloodFillSkin (skin, w, h);
				q_snprintf (identifier, sizeof(identifier), "%s_%i_%i", e->name, i, j);
				AddTexture (e, identifier, KIND_SKIN, p, w, h, mode, used);
				p += s;
			}
		}
	}
	return;

cut_short:
	Problem ("%s in %s: the skins are cut short", e->name, label);
}


/* ==========================================================================
 * Sprites: the frames (gl_model.c's Mod_LoadSpriteModel)
 * ========================================================================== */

/* one frame at *p, advanced past it; false: cut short */
static qboolean ReadSpriteFrame (const entry_t *e, const byte **p, const byte *end, int framenum, const char *used)
{
	char	identifier[MAX_QPATH + 16];
	int	w, h;

	if ((size_t)(end - *p) < sizeof(dspriteframe_t))
		return false;
	w = ReadInt (*p + offsetof(dspriteframe_t, width));
	h = ReadInt (*p + offsetof(dspriteframe_t, height));
	if (w <= 0 || h <= 0 || w > 8192 || h > 8192 || (size_t)(end - (*p + sizeof(dspriteframe_t))) < (size_t)w * h)
		return false;
	q_snprintf (identifier, sizeof(identifier), "%s_%i", e->name, framenum);
	AddTexture (e, identifier, KIND_SPRITE, *p + sizeof(dspriteframe_t), w, h, TEX_MIPMAP | TEX_ALPHA, used);
	*p += sizeof(dspriteframe_t) + (size_t)w * h;
	return true;
}

static void ReadSprite (const entry_t *e, const byte *data, size_t len)
{
	const char	*label = ex.sources[e->source].label;
	const byte	*end = data + len, *p;
	char		used[MAX_QPATH];
	int		version, numframes, i, j;

	if (len < sizeof(dsprite_t))
	{
		Problem ("%s in %s: cut short", e->name, label);
		return;
	}
	version = ReadInt (data + 4);
	if (version != SPRITE_VERSION)
	{
		Problem ("%s in %s: sprite version %d, not %d", e->name, label, version, SPRITE_VERSION);
		return;
	}
	numframes = ReadInt (data + offsetof(dsprite_t, numframes));
	if (numframes < 1)
	{
		Problem ("%s in %s: %d frames", e->name, label, numframes);
		return;
	}
	q_strlcpy (used, e->name, sizeof(used));
	q_strlwr (used);

	p = data + sizeof(dsprite_t);
	for (i = 0; i < numframes; i++)
	{
		if ((size_t)(end - p) < 4)
			goto cut_short;
		if (ReadInt (p) == SPR_SINGLE)
		{
			p += 4;
			if (!ReadSpriteFrame (e, &p, end, i, used))
				goto cut_short;
		}
		else		/* a group: gl_model.c takes any other type as one */
		{
			int	count;

			if ((size_t)(end - p) < 8)
				goto cut_short;
			count = ReadInt (p + 4);
			if (count < 0 || (size_t)count > (size_t)(end - (p + 8)) / 4)
				goto cut_short;
			p += 8 + (size_t)count * 4;	/* the intervals */
			for (j = 0; j < count; j++)
			{
				if (!ReadSpriteFrame (e, &p, end, i * 100 + j, used))
					goto cut_short;
			}
		}
	}
	return;

cut_short:
	Problem ("%s in %s: the frames are cut short", e->name, label);
}


/* ==========================================================================
 * Pictures: gfx/skin<n>.lmp (vk_draw.c's Draw_CachePic, GL_LoadPicTexture)
 * ========================================================================== */

static void ReadPicture (const entry_t *e, const byte *data, size_t len)
{
	char	used[MAX_QPATH];
	int	w, h;

	if (len < 8)
	{
		Problem ("%s in %s: cut short", e->name, ex.sources[e->source].label);
		return;
	}
	w = ReadInt (data);
	h = ReadInt (data + 4);
	if (w <= 0 || h <= 0 || w > 8192 || h > 8192 || (size_t)w * h > len - 8)
	{
		Problem ("%s in %s: a %dx%d picture in %d bytes", e->name, ex.sources[e->source].label, w, h, (int)len);
		return;
	}
	q_strlcpy (used, e->name, sizeof(used));
	q_strlwr (used);
	AddTexture (e, e->name, KIND_PICTURE, data + 8, w, h, TEX_ALPHA | TEX_NEAREST, used);
}

static void ReadEntries (void)
{
	int	i;

	for (i = 0; i < ex.num_entries; i++)
	{
		const entry_t	*e = &ex.entries[i];
		size_t		len;
		byte		*data = ReadEntry (e, &len);
		int		ident;

		if (!data)
			continue;
		ident = (len >= 4) ? ReadInt (data) : 0;
		/* by the header, as gl_model.c's Mod_LoadModel: its default is a map */
		if (e->kind == FILE_PICTURE)
			ReadPicture (e, data, len);
		else if (ident == RAPOLYHEADER || ident == IDPOLYHEADER)
			ReadModel (e, data, len);
		else if (ident == IDSPRITEHEADER)
			ReadSprite (e, data, len);
		else if (e->kind == FILE_MAP)
			ReadMap (e, data, len);
		else
		{
			Problem ("%s in %s: not a model or sprite", e->name, ex.sources[e->source].label);
			free (data);
			continue;
		}
		ex.files_read[e->kind]++;
		free (data);
	}
}


/* ==========================================================================
 * Writing
 * ========================================================================== */

typedef struct
{
	FILE		*f;
	qboolean	ok;
	size_t		bytes;
} pngout_t;

static void WritePNGBytes (void *context, void *data, int size)
{
	pngout_t	*o = (pngout_t *)context;

	if (o->ok && fwrite (data, 1, (size_t)size, o->f) != (size_t)size)
		o->ok = false;
	o->bytes += (size_t)size;
}

/* why Windows can't take a relative path (/ separated), NULL = it can */
static const char *BadFileName (const char *path)
{
	static const char	*devices[] = { "con", "prn", "aux", "nul", "com", "lpt" };
	const char		*c, *part = path;
	int			i;

	for (c = path; ; c++)
	{
		if (*c == '/' || *c == '\0')
		{
			size_t	n = (size_t)(c - part), base = n;
			const char *dot = memchr (part, '.', n);

			if (!n)
				return "an empty folder name";
			if (n == 2 && part[0] == '.' && part[1] == '.')
				return "a .. in the path";
			if (part[n - 1] == '.' || part[n - 1] == ' ')
				return "a name ending in . or a space";
			if (dot)
				base = (size_t)(dot - part);
			for (i = 0; i < (int)Q_COUNTOF(devices); i++)
			{
				if (!q_strncasecmp (part, devices[i], 3) &&
				    ((i < 4 && base == 3) || (i >= 4 && base == 4 && q_isdigit (part[3]))))
					return "a device name (con, nul, com1, ...)";
			}
			if ((base == 6 && !q_strncasecmp (part, "conin$", 6)) || (base == 7 && !q_strncasecmp (part, "conout$", 7)))
				return "a device name (con, nul, com1, ...)";
			if (*c == '\0')
				return NULL;
			part = c + 1;
		}
		else if ((unsigned char)*c < 32 || strchr ("<>:\"\\|?*", *c))
			return "a character file names can't have";
	}
}

/* a CSV field, quoted when it needs to be */
static void CSVField (FILE *f, const char *s, qboolean last)
{
	if (strpbrk (s, ",\"\r\n"))
	{
		fputc ('"', f);
		for ( ; *s; s++)
		{
			if (*s == '"')
				fputc ('"', f);
			fputc (*s, f);
		}
		fputc ('"', f);
	}
	else
		fputs (s, f);
	fputs (last ? "\r\n" : ",", f);
}

static int CompareVariants (const void *a, const void *b)
{
	const variant_t	*va = &ex.variants[*(const int *)a], *vb = &ex.variants[*(const int *)b];
	int		c = strcmp (va->name, vb->name);

	return c ? c : (int)va->crc - (int)vb->crc;
}

typedef struct
{
	int	files, names, varied;	/* varied: names with variants */
	int	alpha;			/* files with an alpha */
} kindcount_t;

static void R_ExportTextures_f (void)
{
	const char	*folder = DEFAULT_FOLDER, *c;
	char		path[MAX_OSPATH], dir[MAX_OSPATH], last_dir[MAX_OSPATH] = "", file[MAX_QPATH + 16], num[32];
	kindcount_t	counts[NUM_KINDS];
	unsigned int	*rgba = NULL;
	size_t		rgba_size = 0;
	double		t0 = Sys_DoubleTime (), t_read, bytes = 0;
	int		*order, i, j, k, err, written = 0, games = 0;
	FILE		*csv;

	if (Cmd_Argc () > 2 || (Cmd_Argc () == 2 && (!*(folder = Cmd_Argv (1)) || !q_strcasecmp (folder, TEXTURES_FOLDER))))
	{
		Con_Printf ("r_exporttextures [folder]: every original texture as a PNG under its material file name, "
			    "and textures.csv, into <game folder>/%s (or folder; not textures)\n", DEFAULT_FOLDER);
		return;
	}
	for (c = folder; *c; c++)
	{
		if (!q_isalnum (*c) && *c != '_' && *c != '-')
		{
			Con_Printf ("r_exporttextures: the folder's name takes letters, digits, _ and - only\n");
			return;
		}
	}
	if ((c = BadFileName (folder)) != NULL)
	{
		Con_Printf ("r_exporttextures: %s is %s\n", folder, c);
		return;
	}

	/* the files, in the search path's order */
	memset (ex.hash, 0xff, sizeof(ex.hash));	/* -1 */
	FS_ListSearchPath ("", ListFolder, ListPakFile, NULL);
	ReadEntries ();
	t_read = Sys_DoubleTime () - t0;

	/* the variants of each name */
	for (i = 0; i < ex.num_variants; i++)
	{
		variant_t	*v = &ex.variants[i];

		for (j = ex.hash[HashName (v->name) & (NAME_HASH - 1)]; j >= 0; j = ex.variants[j].next)
		{
			if (!strcmp (ex.variants[j].name, v->name))
				v->variants++;
		}
	}
	order = (int *) malloc ((size_t)q_max (ex.num_variants, 1) * sizeof(int));
	if (!order)
		Sys_Error ("%s: out of memory", __thisfunc__);
	for (i = 0; i < ex.num_variants; i++)
		order[i] = i;
	qsort (order, ex.num_variants, sizeof(int), CompareVariants);

	FS_MakePath_VABUF (FS_USERDIR, &err, path, sizeof(path), "%s/%s.csv", folder, TEXTURES_FOLDER);
	csv = (err || FS_CreatePath (path)) ? NULL : fopen (path, "wb");
	if (!csv)
	{
		Con_Printf ("r_exporttextures: couldn't write %s\n", path);
		goto done;
	}
	fputs ("file,name,crc,width,height,kind,alpha,variants,used in,from\r\n", csv);

	memset (counts, 0, sizeof(counts));
	for (k = 0; k < ex.num_variants; k++)
	{
		const variant_t	*v = &ex.variants[order[k]];

		/* the first of its name in the order (by name, then CRC) */
		if (k == 0 || strcmp (ex.variants[order[k - 1]].name, v->name))
		{
			counts[v->kind].names++;
			if (v->variants > 1)
				counts[v->kind].varied++;
		}
	}
	for (k = 0; k < ex.num_variants; k++)
	{
		variant_t	*v = &ex.variants[order[k]];
		size_t		texels = (size_t)v->width * v->height;
		const char	*why, *alpha = "none";
		int		flags = v->flags, comp = 3;
		pngout_t	out;

		if (v->variants > 1)
			q_snprintf (file, sizeof(file), "%s/%s~%04x.png", TEXTURES_FOLDER, v->name, v->crc);
		else
			q_snprintf (file, sizeof(file), "%s/%s.png", TEXTURES_FOLDER, v->name);
		if ((why = BadFileName (file)) != NULL)
		{
			Problem ("%s (%s in %s): %s, not written", file, v->identifier, v->from.text, why);
			continue;
		}

		/* the colors as GL_LoadTexture uploads them */
		if (texels * 4 > rgba_size)
		{
			free (rgba);
			rgba_size = texels * 4;
			if ((rgba = (unsigned int *) malloc (rgba_size)) == NULL)
			{
				rgba_size = 0;
				Problem ("%s (%s in %s): no memory for %dx%d, not written", file, v->identifier, v->from.text,
					 v->width, v->height);
				continue;
			}
		}
		VK_Convert8Pixels (v->pixels, v->width, v->height, &flags, rgba);
		if (flags & TEX_ALPHA)
		{
			size_t	t;

			for (t = 0; t < texels; t++)
			{
				unsigned int	a = rgba[t] >> 24;	/* R,G,B,A in memory */

				if (a != 255)
				{
					comp = 4;
					alpha = "coverage";
					if (a != 0)
					{
						alpha = "translucent";
						break;
					}
				}
			}
		}
		if (comp == 3)
		{
			byte	*b = (byte *)rgba;	/* RGBA to RGB, in place */
			size_t	t;

			for (t = 0; t < texels; t++)
			{
				b[t * 3 + 0] = b[t * 4 + 0];
				b[t * 3 + 1] = b[t * 4 + 1];
				b[t * 3 + 2] = b[t * 4 + 2];
			}
		}

		FS_MakePath_VABUF (FS_USERDIR, &err, path, sizeof(path), "%s/%s", folder, file);
		q_strlcpy (dir, path, sizeof(dir));
		if ((c = strrchr (dir, '/')) != NULL)
			dir[c - dir + 1] = '\0';
		if (!err && strcmp (dir, last_dir))
		{
			err = FS_CreatePath (dir);	/* the folders (FS_CreatePath refuses any "..", a file name's too) */
			q_strlcpy (last_dir, err ? "" : dir, sizeof(last_dir));
		}
		out.f = err ? NULL : fopen (path, "wb");
		if (!out.f)
		{
			Problem ("%s: can't be written", path);
			continue;
		}
		out.ok = true;
		out.bytes = 0;
		if (!stbi_write_png_to_func (WritePNGBytes, &out, v->width, v->height, comp, rgba, v->width * comp))
			out.ok = false;
		if (fclose (out.f) != 0)
			out.ok = false;
		if (!out.ok)
		{
			remove (path);	/* not a PNG cut short */
			Problem ("%s: can't be written", path);
			continue;
		}
		written++;
		bytes += (double)out.bytes;

		CSVField (csv, file, false);
		CSVField (csv, v->identifier, false);
		q_snprintf (num, sizeof(num), "%04x", v->crc);
		CSVField (csv, num, false);
		q_snprintf (num, sizeof(num), "%d", v->width);
		CSVField (csv, num, false);
		q_snprintf (num, sizeof(num), "%d", v->height);
		CSVField (csv, num, false);
		CSVField (csv, kind_names[v->kind], false);
		CSVField (csv, alpha, false);
		q_snprintf (num, sizeof(num), "%d", v->variants);
		CSVField (csv, num, false);
		CSVField (csv, v->used.text ? v->used.text : "", false);
		CSVField (csv, v->from.text ? v->from.text : "", true);

		counts[v->kind].files++;
		if (comp == 4)
			counts[v->kind].alpha++;
	}
	if (fclose (csv) != 0)
		Problem ("%s/%s.csv: can't be written to its end", folder, TEXTURES_FOLDER);

	/* what was read, from where */
	Con_Printf ("r_exporttextures: ");
	for (i = 0; i < ex.num_sources; i++)
	{
		if (!ex.sources[i].pak)
			Con_Printf ("%s%s", games++ ? ", " : "", ex.sources[i].label);
	}
	Con_Printf ("%s: ", (gameflags & GAME_PORTALS) ? "" : " only (-portals for the mission pack's too)");
	for (i = 0; i < NUM_FILE_KINDS; i++)
		Con_Printf ("%d %s%s", ex.files_read[i], filekind_names[i], (i < NUM_FILE_KINDS - 1) ? ", " : "");
	Con_Printf (" read in %.1f s\n", t_read);
	for (i = 0; i < NUM_KINDS; i++)
	{
		Con_Printf ("%-8s %5d files, %4d names, %3d with variants, %4d with alpha\n", kind_names[i],
			    counts[i].files, counts[i].names, counts[i].varied, counts[i].alpha);
	}
	FS_MakePath_BUF (FS_USERDIR, &err, path, sizeof(path), folder);
	Con_Printf ("%d files, %.1f MB, in %.1f s: %s/%s, %s.csv\n", written, bytes / (1024.0 * 1024.0),
		    Sys_DoubleTime () - t0, path, TEXTURES_FOLDER, TEXTURES_FOLDER);

done:
	if (ex.num_problems)
	{
		const char	*line = ex.problems.text;

		Con_Printf ("%d problems:\n", ex.num_problems);
		for (i = 0; i < MAX_PRINTED && line && *line; i++)
		{
			const char	*nl = strchr (line, '\n');

			Con_Printf ("  %.*s\n", (int)(nl - line), line);
			line = nl + 1;
		}
		if (ex.num_problems > MAX_PRINTED)
			Con_Printf ("  ... and %d more\n", ex.num_problems - MAX_PRINTED);
	}
	else
		Con_Printf ("no problems\n");

	free (order);
	free (rgba);
	for (i = 0; i < ex.num_variants; i++)
	{
		free (ex.variants[i].pixels);
		FreeList (&ex.variants[i].used);
		FreeList (&ex.variants[i].from);
	}
	free (ex.variants);
	free (ex.sources);
	free (ex.entries);
	FreeList (&ex.problems);
	memset (&ex, 0, sizeof(ex));
}

void VK_InitExport (void)
{
	Cmd_AddCommand ("r_exporttextures", R_ExportTextures_f);
}
