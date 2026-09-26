/* vk_draw.c -- 2D drawing for Hexenlicht: console, menus, status bar
 *
 * A port of the 2D part of Hammer of Thyrion's gl_draw.c. The Draw_*
 * functions keep their behavior (fonts, pic caches, cropping, the player
 * color menu picture, fades); instead of immediate-mode GL they append
 * quads to a per-frame vertex buffer, which GL_EndRendering draws with a
 * single indexed draw call before presenting.
 *
 * Coordinates are in the virtual 2D screen vid.width x vid.height, which
 * vid_vk.c derives from the window with an integer UI scale, so 2D pixels
 * stay crisp.
 *
 * Like the GL renderer, most quads use an alpha test (drawn or discarded,
 * no blending); the console background, fades and Draw_AlphaPic blend.
 * The shader works in sRGB-encoded space so both look like the original.
 *
 * Copyright (C) 1996-1997  Id Software, Inc.
 * Copyright (C) 1997-1998  Raven Software Corp.
 * Copyright (C) 2005-2012  O.Sezer <sezero@users.sourceforge.net>
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
#include "vid_vk.h"

#define MAX_2D_QUADS		131072	/* per frame: a 4K console at scale 1 fits */
#define FLAG_ALPHA_TEST		0x10000u	/* see draw2d.frag */
#define COLOR_WHITE		0xffffffffu

typedef struct
{
	float		x, y;
	float		u, v;
	uint32_t	color;		/* RGBA8, sRGB-encoded */
	uint32_t	tex;		/* texture slot | FLAG_* */
} draw_vertex_t;

typedef struct
{
	VkBuffer	buffer;
	VmaAllocation	allocation;
	draw_vertex_t	*vertices;	/* persistently mapped */
} draw_frame_t;

typedef struct
{
	float	scale[2];
	float	offset[2];
	float	gamma;
} draw_push_t;

static draw_frame_t	draw_frames[VK_FRAMES_IN_FLIGHT];
static VkBuffer		index_buffer;
static VmaAllocation	index_allocation;
static VkPipelineLayout	draw_layout;
static VkPipeline	draw_pipeline;
static VkFormat		draw_format;
static int		num_quads;
static qboolean		quads_dropped;

/* SCR_UpdateScreen nests: Con_Printf redraws the screen while the console
 * is up, also from inside a frame. Only the outermost level records and
 * presents; nested levels draw nothing. */
static int		draw_depth;
static qboolean		draw_frame;	/* the outermost level began a Vulkan frame */

qboolean	draw_reinit = false;

static cvar_t	gl_constretch = {"gl_constretch", "0", CVAR_ARCHIVE};

static GLuint	draw_backtile;
static GLuint	conback;
static GLuint	char_texture;
static GLuint	cs_texture;		/* crosshair texture */
static GLuint	char_smalltexture;
static GLuint	char_menufonttexture;

/* Crosshair texture is a 32x32 alpha map with 8 levels of alpha.
 * The format is similar to an X11 pixmap, but not the same.
 * 7 is 100% solid, 0 and any other characters are transparent. */
static const char	*cs_data = {
/* This is actually the QuakeWorld crosshair
   which Raven didn't bother changing. It is
   possible to make class-based crosshairs. */
	"................................"
	"................................"
	"..............7777.............."
	"..............7777.............."
	"..............7777.............."
	"..............7777.............."
	"................................"
	"................................"
	"................................"
	"................................"
	"..............7777.............."
	"..............7777.............."
	"..............7777.............."
	"..............7777.............."
	"..7777....7777....7777....7777.."
	"..7777....7777....7777....7777.."
	"..7777....7777....7777....7777.."
	"..7777....7777....7777....7777.."
	"..............7777.............."
	"..............7777.............."
	"..............7777.............."
	"..............7777.............."
	"................................"
	"................................"
	"................................"
	"................................"
	"..............7777.............."
	"..............7777.............."
	"..............7777.............."
	"..............7777.............."
	"................................"
	"................................"
};

static cachepic_t	menu_cachepics[MAX_CACHED_PICS];
static int		menu_numcachepics;
static hashindex_t	hash_cachepics;

/* D_ClearOpenGLTextures purged textures that cached pics may use: forget
 * them, as gl_rmisc.c's version does, so they load again */
void Draw_ClearCachedPics (void)
{
	memset (menu_cachepics, 0, menu_numcachepics * sizeof(cachepic_t));
	menu_numcachepics = 0;
	Hash_Clear (&hash_cachepics);
}

/* Geometry for the player/skin selection screen image. */
#define	PLAYER_PIC_WIDTH	68
#define	PLAYER_PIC_HEIGHT	114
#define	PLAYER_DEST_WIDTH	128
#define	PLAYER_DEST_HEIGHT	128

static byte	menuplyr_pixels[MAX_PLAYER_CLASS][PLAYER_PIC_WIDTH*PLAYER_PIC_HEIGHT];


/* ==========================================================================
 * The batch
 * ========================================================================== */

static uint32_t Draw_PackColor (int r, int g, int b, int a)
{
	return (uint32_t)(r & 0xff) | ((uint32_t)(g & 0xff) << 8) |
	       ((uint32_t)(b & 0xff) << 16) | ((uint32_t)(a & 0xff) << 24);
}

static uint32_t Draw_PaletteColor (int c, int a)
{
	return Draw_PackColor (host_basepal[c*3], host_basepal[c*3+1], host_basepal[c*3+2], a);
}

/* append one textured quad; (x0,y0)-(x1,y1) in 2D screen coordinates */
static void Draw_Quad (float x0, float y0, float x1, float y1,
		       float s0, float t0, float s1, float t1,
		       GLuint tex, uint32_t color, qboolean alpha_test)
{
	draw_vertex_t	*v;
	uint32_t	t;

	if (!draw_frame || draw_depth != 1)
		return;		/* no frame (minimized), or a nested SCR_UpdateScreen */
	if (num_quads >= MAX_2D_QUADS)
	{
		quads_dropped = true;
		return;
	}
	if (tex >= VK_MAX_TEXTURES)
		tex = 0;	/* GL_UNUSED_TEXTURE and the like */
	t = (uint32_t)tex | (alpha_test ? FLAG_ALPHA_TEST : 0u);

	v = draw_frames[vk.frame_index].vertices + num_quads * 4;
	v[0].x = x0; v[0].y = y0; v[0].u = s0; v[0].v = t0; v[0].color = color; v[0].tex = t;
	v[1].x = x1; v[1].y = y0; v[1].u = s1; v[1].v = t0; v[1].color = color; v[1].tex = t;
	v[2].x = x1; v[2].y = y1; v[2].u = s1; v[2].v = t1; v[2].color = color; v[2].tex = t;
	v[3].x = x0; v[3].y = y1; v[3].u = s0; v[3].v = t1; v[3].color = color; v[3].tex = t;
	num_quads++;
}

static void Draw_CreatePipeline (void)
{
	VkShaderModule				vert, frag;
	VkPipelineShaderStageCreateInfo		stages[2];
	VkVertexInputBindingDescription		vbinding;
	VkVertexInputAttributeDescription	attributes[4];
	VkPipelineVertexInputStateCreateInfo	vertex_input;
	VkPipelineInputAssemblyStateCreateInfo	input_assembly;
	VkPipelineViewportStateCreateInfo	viewport;
	VkPipelineRasterizationStateCreateInfo	raster;
	VkPipelineMultisampleStateCreateInfo	multisample;
	VkPipelineColorBlendAttachmentState	blend_attachment;
	VkPipelineColorBlendStateCreateInfo	blend;
	VkPipelineDynamicStateCreateInfo	dynamic;
	VkPipelineRenderingCreateInfo		rendering;
	VkPipelineLayoutCreateInfo		layout_info;
	VkPushConstantRange			push_range;
	VkGraphicsPipelineCreateInfo		info;
	const VkDynamicState	dynamic_states[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };

	if (!draw_layout)
	{
		memset (&push_range, 0, sizeof(push_range));
		push_range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
		push_range.size = sizeof(draw_push_t);
		memset (&layout_info, 0, sizeof(layout_info));
		layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
		layout_info.setLayoutCount = 1;
		layout_info.pSetLayouts = &vk.texture_set_layout;
		layout_info.pushConstantRangeCount = 1;
		layout_info.pPushConstantRanges = &push_range;
		VK_CHECK (vkCreatePipelineLayout (vk.device, &layout_info, NULL, &draw_layout));
	}

	vert = VK_LoadShader ("draw2d.vert");
	frag = VK_LoadShader ("draw2d.frag");

	memset (stages, 0, sizeof(stages));
	stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = vert;
	stages[0].pName = "main";
	stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = frag;
	stages[1].pName = "main";

	vbinding.binding = 0;
	vbinding.stride = sizeof(draw_vertex_t);
	vbinding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
	attributes[0].location = 0; attributes[0].binding = 0;
	attributes[0].format = VK_FORMAT_R32G32_SFLOAT; attributes[0].offset = offsetof(draw_vertex_t, x);
	attributes[1].location = 1; attributes[1].binding = 0;
	attributes[1].format = VK_FORMAT_R32G32_SFLOAT; attributes[1].offset = offsetof(draw_vertex_t, u);
	attributes[2].location = 2; attributes[2].binding = 0;
	attributes[2].format = VK_FORMAT_R8G8B8A8_UNORM; attributes[2].offset = offsetof(draw_vertex_t, color);
	attributes[3].location = 3; attributes[3].binding = 0;
	attributes[3].format = VK_FORMAT_R32_UINT; attributes[3].offset = offsetof(draw_vertex_t, tex);

	memset (&vertex_input, 0, sizeof(vertex_input));
	vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	vertex_input.vertexBindingDescriptionCount = 1;
	vertex_input.pVertexBindingDescriptions = &vbinding;
	vertex_input.vertexAttributeDescriptionCount = 4;
	vertex_input.pVertexAttributeDescriptions = attributes;

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
	raster.frontFace = VK_FRONT_FACE_CLOCKWISE;
	raster.lineWidth = 1.0f;

	memset (&multisample, 0, sizeof(multisample));
	multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

	memset (&blend_attachment, 0, sizeof(blend_attachment));
	blend_attachment.blendEnable = VK_TRUE;
	blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
	blend_attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	blend_attachment.colorBlendOp = VK_BLEND_OP_ADD;
	blend_attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
	blend_attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	blend_attachment.alphaBlendOp = VK_BLEND_OP_ADD;
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

	draw_format = vk.surface_format.format;
	memset (&rendering, 0, sizeof(rendering));
	rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
	rendering.colorAttachmentCount = 1;
	rendering.pColorAttachmentFormats = &draw_format;

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
	info.layout = draw_layout;
	VK_CHECK (vkCreateGraphicsPipelines (vk.device, VK_NULL_HANDLE, 1, &info, NULL, &draw_pipeline));

	vkDestroyShaderModule (vk.device, vert, NULL);
	vkDestroyShaderModule (vk.device, frag, NULL);
}

static void Draw_CreateBuffers (void)
{
	VkBufferCreateInfo	info;
	VmaAllocationCreateInfo	alloc;
	VmaAllocationInfo	alloc_info;
	uint32_t		*indices;
	int			i;

	memset (&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	memset (&alloc, 0, sizeof(alloc));
	alloc.usage = VMA_MEMORY_USAGE_AUTO;
	alloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;

	/* one vertex buffer per frame in flight, written while recording */
	info.size = sizeof(draw_vertex_t) * 4 * MAX_2D_QUADS;
	info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
	for (i = 0; i < VK_FRAMES_IN_FLIGHT; i++)
	{
		VK_CHECK (vmaCreateBuffer (vk.allocator, &info, &alloc, &draw_frames[i].buffer,
					   &draw_frames[i].allocation, &alloc_info));
		draw_frames[i].vertices = (draw_vertex_t *) alloc_info.pMappedData;
	}

	/* static indices: two triangles per quad */
	info.size = sizeof(uint32_t) * 6 * MAX_2D_QUADS;
	info.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
	VK_CHECK (vmaCreateBuffer (vk.allocator, &info, &alloc, &index_buffer, &index_allocation, &alloc_info));
	indices = (uint32_t *) alloc_info.pMappedData;
	for (i = 0; i < MAX_2D_QUADS; i++)
	{
		indices[i*6+0] = i*4+0;
		indices[i*6+1] = i*4+1;
		indices[i*6+2] = i*4+2;
		indices[i*6+3] = i*4+0;
		indices[i*6+4] = i*4+2;
		indices[i*6+5] = i*4+3;
	}
	VK_CHECK (vmaFlushAllocation (vk.allocator, index_allocation, 0, VK_WHOLE_SIZE));
}

void VK_InitDraw (void)
{
	Draw_CreateBuffers ();
}

void VK_DestroyDrawPipeline (void)
{
	if (draw_pipeline)
		vkDestroyPipeline (vk.device, draw_pipeline, NULL);
	draw_pipeline = VK_NULL_HANDLE;
}

void VK_ShutdownDraw (void)
{
	int	i;

	VK_DestroyDrawPipeline ();
	if (draw_layout)
		vkDestroyPipelineLayout (vk.device, draw_layout, NULL);
	for (i = 0; i < VK_FRAMES_IN_FLIGHT; i++)
	{
		if (draw_frames[i].buffer)
			vmaDestroyBuffer (vk.allocator, draw_frames[i].buffer, draw_frames[i].allocation);
		memset (&draw_frames[i], 0, sizeof(draw_frames[i]));
	}
	if (index_buffer)
		vmaDestroyBuffer (vk.allocator, index_buffer, index_allocation);
	draw_pipeline = VK_NULL_HANDLE;
	draw_layout = VK_NULL_HANDLE;
	index_buffer = VK_NULL_HANDLE;
}

/* record the batch into the current frame */
static void Draw_Flush (void)
{
	VkCommandBuffer	cmd = vk.frames[vk.frame_index].cmd;
	VkDeviceSize	offset = 0;
	draw_push_t	push;
	int		s = VID_GetUIScale ();
	float		w = (float)vk.extent.width, h = (float)vk.extent.height;
	float		ox, oy;

	if (draw_pipeline && draw_format != vk.surface_format.format)
	{
		vkDeviceWaitIdle (vk.device);
		vkDestroyPipeline (vk.device, draw_pipeline, NULL);
		draw_pipeline = VK_NULL_HANDLE;
	}
	if (!draw_pipeline)
		Draw_CreatePipeline ();

	if (quads_dropped)
	{
		Con_DPrintf ("2D: more than %d quads in a frame, some were not drawn\n", MAX_2D_QUADS);
		quads_dropped = false;
	}

	/* the 2D screen, scaled by s, centered in the window */
	ox = (float)(((int)vk.extent.width - vid.width * s) / 2);
	oy = (float)(((int)vk.extent.height - vid.height * s) / 2);
	push.scale[0] = 2.0f * s / w;
	push.scale[1] = 2.0f * s / h;
	push.offset[0] = 2.0f * ox / w - 1.0f;
	push.offset[1] = 2.0f * oy / h - 1.0f;
	push.gamma = v_gamma.value;

	VK_CHECK (vmaFlushAllocation (vk.allocator, draw_frames[vk.frame_index].allocation, 0,
				      sizeof(draw_vertex_t) * 4 * (VkDeviceSize)num_quads));

	vkCmdBindPipeline (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, draw_pipeline);
	vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, draw_layout, 0, 1, &vk.texture_set, 0, NULL);
	vkCmdPushConstants (cmd, draw_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
			    0, sizeof(push), &push);
	vkCmdBindVertexBuffers (cmd, 0, 1, &draw_frames[vk.frame_index].buffer, &offset);
	vkCmdBindIndexBuffer (cmd, index_buffer, 0, VK_INDEX_TYPE_UINT32);
	vkCmdDrawIndexed (cmd, (uint32_t)num_quads * 6, 1, 0, 0, 0);
}


/* ==========================================================================
 * Frame (called by gl_screen.c's SCR_UpdateScreen)
 * ========================================================================== */

void GL_BeginRendering (int *x, int *y, int *width, int *height)
{
	*x = *y = 0;
	VID_GetClientSize (width, height);

	if (draw_depth++ > 0)
		return;		/* nested */
	num_quads = 0;
	draw_frame = VK_BeginFrame ();	/* false when minimized: Draw_* then do nothing */
}

void GL_Set2D (void)
{
	/* the 2D projection is applied when the batch is drawn */
}

void GL_EndRendering (void)
{
	if (draw_depth <= 0)
		return;
	if (--draw_depth > 0)
		return;		/* nested */

	if (draw_frame)
	{
		/* the 3D view (black where there is none), then the 2D */
		VK_BeginSwapchainRendering (VK_ATTACHMENT_LOAD_OP_CLEAR);
		VK_DrawView3D ();
		if (num_quads)
			Draw_Flush ();
		VK_EndSwapchainRendering ();
		draw_frame = false;
		VK_EndFrame ();
	}
	num_quads = 0;
	VID_EndFrame ();
}


/* ==========================================================================
 * Pics
 * ========================================================================== */

static void Draw_PicCheckError (void *ptr, const char *name)
{
	if (!ptr)
		Sys_Error ("Failed to load %s", name);
}

static void Draw_StoreGLPic (qpic_t *pic, GLuint texnum)
{
	glpic_t	gl;

	gl.texnum = texnum;
	gl.sl = 0;
	gl.sh = 1;
	gl.tl = 0;
	gl.th = 1;
	memcpy (pic->data, &gl, sizeof(glpic_t));
}

qpic_t *Draw_PicFromFile (const char *name)
{
	qpic_t	*p;

	p = (qpic_t *)FS_LoadHunkFile (name, NULL);
	if (!p)
		return NULL;

	SwapPic (p);
	Draw_StoreGLPic (p, GL_LoadPicTexture (p));
	return p;
}

qpic_t *Draw_PicFromWad (const char *name)
{
	qpic_t	*p;

	p = (qpic_t *) W_GetLumpName (name);
	Draw_StoreGLPic (p, GL_LoadPicTexture (p));
	return p;
}

static cachepic_t *Draw_FindCachePic (const char *path, int *key)
{
	int		i;

	*key = Hash_GenerateKeyString (&hash_cachepics, path, true);
	for (i = Hash_First(&hash_cachepics, *key); i != -1; i = Hash_Next(&hash_cachepics, i))
	{
		if (!strcmp (path, menu_cachepics[i].name))
			return &menu_cachepics[i];
	}
	return NULL;
}

static cachepic_t *Draw_NewCachePic (const char *path, int key)
{
	cachepic_t	*pic;

	if (menu_numcachepics == MAX_CACHED_PICS)
		Sys_Error ("menu_numcachepics == MAX_CACHED_PICS");
	Hash_Add (&hash_cachepics, key, menu_numcachepics);
	pic = &menu_cachepics[menu_numcachepics];
	menu_numcachepics++;
	q_strlcpy (pic->name, path, MAX_QPATH);
	return pic;
}

qpic_t *Draw_CachePic (const char *path)
{
	cachepic_t	*pic;
	int		key;
	qpic_t		*dat;

	pic = Draw_FindCachePic (path, &key);
	if (pic)
		return &pic->pic;
	pic = Draw_NewCachePic (path, key);

	dat = (qpic_t *)FS_LoadTempFile (path, NULL);
	Draw_PicCheckError (dat, path);
	SwapPic (dat);

	/* keep the bytes of the translatable player pictures for the
	 * multiplayer configuration menu (garymct) */
	if (!strcmp (path, "gfx/menu/netp1.lmp"))
		memcpy (menuplyr_pixels[0], dat->data, dat->width*dat->height);
	else if (!strcmp (path, "gfx/menu/netp2.lmp"))
		memcpy (menuplyr_pixels[1], dat->data, dat->width*dat->height);
	else if (!strcmp (path, "gfx/menu/netp3.lmp"))
		memcpy (menuplyr_pixels[2], dat->data, dat->width*dat->height);
	else if (!strcmp (path, "gfx/menu/netp4.lmp"))
		memcpy (menuplyr_pixels[3], dat->data, dat->width*dat->height);
	else if (!strcmp (path, "gfx/menu/netp5.lmp"))
		memcpy (menuplyr_pixels[4], dat->data, dat->width*dat->height);

	pic->pic.width = dat->width;
	pic->pic.height = dat->height;
	Draw_StoreGLPic (&pic->pic, GL_LoadPicTexture (dat));
	return &pic->pic;
}

/* like Draw_CachePic() but only for loading.lmp, with its progress
 * bars eliminated */
static const char ls_path[] = "gfx/menu/loading.lmp";
qpic_t *Draw_CacheLoadingPic (void)
{
	cachepic_t	*pic;
	int		key;
	qpic_t		*dat;

	pic = Draw_FindCachePic (ls_path, &key);
	if (pic)
		return &pic->pic;
	if (menu_numcachepics == MAX_CACHED_PICS)
		Sys_Error ("menu_numcachepics == MAX_CACHED_PICS");

	dat = (qpic_t *)FS_LoadTempFile (ls_path, NULL);
	Draw_PicCheckError (dat, ls_path);
	SwapPic (dat);
	if (fs_filesize != 17592 || dat->width != 157 || dat->height != 112)
		return Draw_CachePic (ls_path);

	pic = Draw_NewCachePic (ls_path, key);

	/* kill the progress slot pixels between rows [85:103] */
	memmove (dat->data + 157*85, dat->data + 157*104, 157*(112 - 104));
	dat->height -= (104 - 85);

	pic->pic.width = dat->width;
	pic->pic.height = dat->height;
	Draw_StoreGLPic (&pic->pic, GL_LoadPicTexture (dat));
	return &pic->pic;
}

/* cache a pic ignoring its transparent color (intermission screens; Pa3PyX) */
qpic_t *Draw_CachePicNoTrans (const char *path)
{
	cachepic_t	*pic;
	int		i, key;
	qpic_t		*dat;

	pic = Draw_FindCachePic (path, &key);
	if (pic)
		return &pic->pic;
	pic = Draw_NewCachePic (path, key);

	dat = (qpic_t *)FS_LoadTempFile (path, NULL);
	Draw_PicCheckError (dat, path);
	SwapPic (dat);

	pic->pic.width = dat->width;
	pic->pic.height = dat->height;

	for (i = 0; i < dat->width * dat->height; i++)
	{
		if (dat->data[i] == 255)
			dat->data[i] = 31;	/* pal(31) == pal(255) == FCFCFC (white) */
	}
	Draw_StoreGLPic (&pic->pic, GL_LoadPicTexture (dat));
	return &pic->pic;
}


/* ==========================================================================
 * Init
 * ========================================================================== */

/* a 32x32 greyscale alpha texture from a string, '0'-'7' being alpha
 * levels (from LordHavoc's DarkPlaces) */
static GLuint GL_LoadPixmap (const char *name, const char *data)
{
	int		i;
	unsigned char	pixels[32*32][4];

	for (i = 0; i < 32*32; i++)
	{
		pixels[i][0] = 255;
		pixels[i][1] = 255;
		pixels[i][2] = 255;
		pixels[i][3] = (data[i] >= '0' && data[i] < '8') ? (data[i] - '0') * 32 : 0;
	}

	return GL_LoadTexture (name, (unsigned char *) pixels, 32, 32, TEX_ALPHA | TEX_RGBA | TEX_LINEAR);
}

void Draw_Init (void)
{
	qpic_t		*p;
	byte		*chars;
	int		i;

	if (!draw_reinit)
	{
		Cvar_RegisterVariable (&gl_constretch);
		Hash_Allocate (&hash_cachepics, MAX_CACHED_PICS);
	}

	/* the charset: 8*8 graphic characters */
	chars = FS_LoadTempFile ("gfx/menu/conchars.lmp", NULL);
	Draw_PicCheckError (chars, "gfx/menu/conchars.lmp");
	if (fs_filesize != 256*128)
		Sys_Error ("gfx/menu/conchars.lmp: bad size.");
	for (i = 0; i < 256*128; i++)
	{
		if (chars[i] == 0)
			chars[i] = 255;	/* proper transparent color */
	}
	char_texture = GL_LoadTexture ("charset", chars, 256, 128, TEX_ALPHA|TEX_NEAREST);

	/* the small characters for the status bar */
	chars = (byte *) W_GetLumpName("tinyfont");
	for (i = 0; i < 128*32; i++)
	{
		if (chars[i] == 0)
			chars[i] = 255;
	}
	char_smalltexture = GL_LoadTexture ("smallcharset", chars, 128, 32, TEX_ALPHA|TEX_NEAREST);

	/* the big menu font (the old demo has bigfont.lmp, not bigfont2.lmp) */
	p = (qpic_t *)FS_LoadTempFile("gfx/menu/bigfont2.lmp", NULL);
	if (!p) p = (qpic_t *)FS_LoadTempFile("gfx/menu/bigfont.lmp", NULL);
	Draw_PicCheckError (p, "gfx/menu/bigfont2.lmp");
	SwapPic (p);
	for (i = 0; i < p->width * p->height; i++)	/* MUST be 160 * 80 */
	{
		if (p->data[i] == 0)
			p->data[i] = 255;
	}
	char_menufonttexture = GL_LoadTexture ("menufont", p->data, p->width, p->height, TEX_ALPHA|TEX_LINEAR);

	/* the console background */
	p = (qpic_t *)FS_LoadTempFile ("gfx/menu/conback.lmp", NULL);
	Draw_PicCheckError (p, "gfx/menu/conback.lmp");
	SwapPic (p);
	conback = GL_LoadTexture ("conback", p->data, p->width, p->height, TEX_LINEAR);

	/* the backtile, repeated around a sized down 3D view */
	p = (qpic_t *)FS_LoadTempFile ("gfx/menu/backtile.lmp", NULL);
	Draw_PicCheckError (p, "gfx/menu/backtile.lmp");
	SwapPic (p);
	draw_backtile = GL_LoadTexture ("", p->data, p->width, p->height, TEX_ALPHA|TEX_NEAREST|TEX_REPEAT);

	/* the crosshair */
	cs_texture = GL_LoadPixmap ("crosshair", cs_data);
}

void Draw_ReInit (void)
{
	/* textures survive vid_restart with Vulkan: nothing to reload */
}


/* ==========================================================================
 * Characters and strings
 * ========================================================================== */

/* one 8*8 graphics character with 0 being transparent; clipped at the top
 * of the screen to allow the console to be smoothly scrolled off */
void Draw_Character (int x, int y, unsigned int num)
{
	int	row, col;
	float	frow, fcol;
	const float	xsize = 0.03125f, ysize = 0.0625f;

	num &= 511;
	if (num == 32)
		return;		/* space */
	if (y <= -8)
		return;		/* totally off screen */

	row = num >> 5;
	col = num & 31;
	fcol = col*xsize;
	frow = row*ysize;
	Draw_Quad (x, y, x+8, y+8, fcol, frow, fcol + xsize, frow + ysize,
		   char_texture, COLOR_WHITE, true);
}

void Draw_String (int x, int y, const char *str)
{
	while (*str)
	{
		Draw_Character (x, y, *str);
		str++;
		x += 8;
	}
}

void Draw_RedString (int x, int y, const char *str)
{
	while (*str)
	{
		Draw_Character (x, y, ((unsigned char)(*str))+256);
		str++;
		x += 8;
	}
}

/* a small character, clipped at the bottom edge of the screen */
void Draw_SmallCharacter (int x, int y, int num)
{
	int	row, col;
	float	frow, fcol;
	const float	xsize = 0.0625f, ysize = 0.25f;

	if (num < 32)
		num = 0;
	else if (num >= 'a' && num <= 'z')
		num -= 64;
	else if (num > '_')
		num = 0;
	else
		num -= 32;

	if (num == 0)
		return;
	if (y <= -8 || y >= vid.height)
		return;		/* totally off screen */

	row = num >> 4;
	col = num & 15;
	fcol = col*xsize;
	frow = row*ysize;
	Draw_Quad (x, y, x+8, y+8, fcol, frow, fcol + xsize, frow + ysize,
		   char_smalltexture, COLOR_WHITE, true);
}

void Draw_SmallString (int x, int y, const char *str)
{
	while (*str)
	{
		Draw_SmallCharacter (x, y, *str);
		str++;
		x += 6;
	}
}

/* callback for M_DrawBigCharacter() of menu.c */
void Draw_BigCharacter (int x, int y, int num)
{
	int	row, col;
	float	frow, fcol;
	const float	xsize = 0.125f, ysize = 0.25f;

	row = num / 8;
	col = num % 8;
	fcol = col*xsize;
	frow = row*ysize;
	Draw_Quad (x, y, x+20, y+20, fcol, frow, fcol + xsize, frow + ysize,
		   char_menufonttexture, COLOR_WHITE, true);
}

void Draw_Crosshair (void)
{
	int		x, y;
	const byte	*c;

	x = scr_vrect.x + scr_vrect.width/2 + cl_crossx.value;
	y = scr_vrect.y + scr_vrect.height/2 + cl_crossy.value;

	if (crosshair.integer == 2)
	{
		/* the 32x32 pixmap drawn at 16x16, modulated by crosshaircolor */
		c = (const byte *) &d_8to24table[(byte) crosshaircolor.integer];
		Draw_Quad (x - 7, y - 7, x + 9, y + 9, 0, 0, 1, 1, cs_texture,
			   Draw_PackColor (c[0], c[1], c[2], 255), true);
	}
	else if (crosshair.integer)
	{
		Draw_Character (x - 4, y - 4, '+');
	}
}


/* ==========================================================================
 * Pictures
 * ========================================================================== */

void Draw_Pic (int x, int y, qpic_t *pic)
{
	glpic_t	*gl = (glpic_t *)pic->data;

	Draw_Quad (x, y, x+pic->width, y+pic->height, gl->sl, gl->tl, gl->sh, gl->th,
		   gl->texnum, COLOR_WHITE, true);
}

void Draw_AlphaPic (int x, int y, qpic_t *pic, float alpha)
{
	glpic_t	*gl = (glpic_t *)pic->data;

	Draw_Quad (x, y, x+pic->width, y+pic->height, gl->sl, gl->tl, gl->sh, gl->th,
		   gl->texnum, Draw_PackColor (255, 255, 255, (int)(alpha * 255.0f)), false);
}

/* the intermission screen, stretched over the whole 2D screen (Pa3PyX) */
void Draw_IntermissionPic (qpic_t *pic)
{
	glpic_t	*gl = (glpic_t *)pic->data;

	Draw_Quad (0, 0, vid.width, vid.height, 0, 0, 1, 1, gl->texnum, COLOR_WHITE, true);
}

void Draw_SubPic (int x, int y, qpic_t *pic, int srcx, int srcy, int width, int height)
{
	glpic_t	*gl = (glpic_t *)pic->data;
	float	newsl, newtl, newsh, newth;
	float	oldglwidth = gl->sh - gl->sl, oldglheight = gl->th - gl->tl;

	newsl = gl->sl + (srcx*oldglwidth)/pic->width;
	newsh = newsl + (width*oldglwidth)/pic->width;
	newtl = gl->tl + (srcy*oldglheight)/pic->height;
	newth = newtl + (height*oldglheight)/pic->height;

	Draw_Quad (x, y, x+width, y+height, newsl, newtl, newsh, newth, gl->texnum, COLOR_WHITE, true);
}

/* vertical cropping to the screen, as in gl_draw.c */
static void Draw_CroppedRange (int *y, int *height, float *tl, float *th, const qpic_t *pic, const glpic_t *gl)
{
	if (*y + pic->height > vid.height)
	{
		*height = vid.height - *y;
		*tl = 0;
		*th = (*height - 0.01f) / pic->height;
	}
	else if (*y < 0)
	{
		*height = pic->height + *y;
		*tl = (-*y - 0.01f) / pic->height;
		*th = (pic->height - 0.01f) / pic->height;
		*y = 0;
	}
	else
	{
		*height = pic->height;
		*tl = gl->tl;
		*th = gl->th;
	}
}

void Draw_PicCropped (int x, int y, qpic_t *pic)
{
	int	height;
	glpic_t	*gl = (glpic_t *)pic->data;
	float	th, tl;

	if ((x < 0) || (x+pic->width > vid.width))
		Sys_Error("%s: bad coordinates", __thisfunc__);
	if (y >= vid.height || y+pic->height < 0)
		return;		/* totally off screen */

	Draw_CroppedRange (&y, &height, &tl, &th, pic, gl);
	Draw_Quad (x, y, x+pic->width, y+height, gl->sl, tl, gl->sh, th, gl->texnum, COLOR_WHITE, true);
}

void Draw_SubPicCropped (int x, int y, int h, qpic_t *pic)
{
	int	height;
	glpic_t	*gl = (glpic_t *)pic->data;
	float	th, tl;

	if ((x < 0) || (x+pic->width > vid.width))
		Sys_Error("%s: bad coordinates", __thisfunc__);
	if (y >= vid.height || y+h < 0)
		return;		/* totally off screen */

	Draw_CroppedRange (&y, &height, &tl, &th, pic, gl);
	if (height > h)
		height = h;
	Draw_Quad (x, y, x+pic->width, y+height, gl->sl, tl, gl->sh, th, gl->texnum, COLOR_WHITE, true);
}

void Draw_TransPic (int x, int y, qpic_t *pic)
{
	if (x < 0 || (x + pic->width) > vid.width ||
	    y < 0 || (y + pic->height) > vid.height)
	{
		Sys_Error ("%s: bad coordinates", __thisfunc__);
	}
	Draw_Pic (x, y, pic);
}

void Draw_TransPicCropped (int x, int y, qpic_t *pic)
{
	Draw_PicCropped (x, y, pic);
}

/* only used for the player color selection menu: the texture is rebuilt
 * from the translation (the texture cache reuses it while it is unchanged) */
void Draw_TransPicTranslate (int x, int y, qpic_t *pic, byte *translation, int p_class)
{
	int		i, j, v, u, p;
	unsigned int	trans[PLAYER_DEST_WIDTH * PLAYER_DEST_HEIGHT], *dest;
	byte		*src;
	GLuint		tex;

	dest = trans;
	for (v = 0; v < 64; v++, dest += 64)
	{
		src = &menuplyr_pixels[p_class-1][((v*pic->height)>>6) * pic->width];
		for (u = 0; u < 64; u++)
		{
			p = src[(u*pic->width)>>6];
			dest[u] = (p == 255) ? 255 : d_8to24table[translation[p]];
		}
	}
	for (i = 0; i < PLAYER_PIC_WIDTH; i++)
	{
		for (j = 0; j < PLAYER_PIC_HEIGHT; j++)
		{
			trans[j * PLAYER_DEST_WIDTH + i] =
			 d_8to24table[translation[menuplyr_pixels[p_class-1][j * PLAYER_PIC_WIDTH + i]]];
		}
	}

	tex = GL_LoadTexture (va("menuplyr%d", p_class), (byte *)trans, PLAYER_DEST_WIDTH, PLAYER_DEST_HEIGHT,
			      TEX_RGBA | TEX_ALPHA | TEX_LINEAR);
	Draw_Quad (x, y, x+pic->width, y+pic->height,
		   0, 0, (float)PLAYER_PIC_WIDTH / PLAYER_DEST_WIDTH, (float)PLAYER_PIC_HEIGHT / PLAYER_DEST_HEIGHT,
		   tex, COLOR_WHITE, true);
}


/* ==========================================================================
 * Screen areas
 * ========================================================================== */

static void Draw_ConsoleVersionInfo (int lines)
{
	static const char ver[] = ENGINE_WATERMARK;
	const char *ptr = ver;
	int x = vid.conwidth - (strlen(ver) * 8 + 11);
	int y = lines - 14;

	for (; *ptr; ++ptr)
		Draw_Character (x + (int)(ptr - ver) * 8, y, *ptr | 0x100);
}

void Draw_ConsoleBackground (int lines)
{
	int	y;
	float	ofs, alpha;

	y = (vid.height * 3) >> 2;
	ofs = (gl_constretch.integer) ? 0.0f : (vid.conheight - lines) / (float) vid.conheight;
	alpha = (lines > y) ? 1.0f : 1.1f * lines / y;
	if (alpha > 1.0f)
		alpha = 1.0f;

	Draw_Quad (0, 0, vid.conwidth, lines, 0, ofs, 1, 1, conback,
		   Draw_PackColor (255, 255, 255, (int)(alpha * 255.0f)), false);

	Draw_ConsoleVersionInfo (lines);
}

/* repeats a 64*64 tile graphic to fill the screen around a sized down
 * refresh window */
void Draw_TileClear (int x, int y, int w, int h)
{
	Draw_Quad (x, y, x+w, y+h, x/64.0f, y/64.0f, (x+w)/64.0f, (y+h)/64.0f,
		   draw_backtile, COLOR_WHITE, false);
}

/* fills a box of pixels with a single color */
void Draw_Fill (int x, int y, int w, int h, int c)
{
	Draw_Quad (x, y, x+w, y+h, 0, 0, 1, 1, 0, Draw_PaletteColor (c, 255), false);
}

void Draw_FadeScreen (void)
{
	int	bx, by, ex, ey;
	int	c;

	Draw_Quad (0, 0, vid.width, vid.height, 0, 0, 1, 1, 0,
		   Draw_PackColor (248, 220, 120, (int)(0.1f * 255.0f)), false);

	for (c = 0 ; c < 40 ; c++)
	{
		bx = (rand() % vid.width) - 20;
		by = (rand() % vid.height) - 20;
		ex = bx + (rand() % 40) + 20;
		ey = by + (rand() % 40) + 20;
		if (bx < 0)
			bx = 0;
		if (by < 0)
			by = 0;
		if (ex > vid.width)
			ex = vid.width;
		if (ey > vid.height)
			ey = vid.height;

		Draw_Quad (bx, by, ex, ey, 0, 0, 1, 1, 0,
			   Draw_PackColor (248, 220, 120, (int)(0.018f * 255.0f)), false);
	}

	Sbar_Changed();
}
