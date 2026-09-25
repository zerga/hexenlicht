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
#define TEX_REPEAT	(1 << 20)	/* Hexenlicht only: repeat addressing for a non-mipmapped texture */
void VK_InitTextures (void);
void VK_ShutdownTextures (void);
int VK_FindTexture (const char *identifier);	/* its slot, -1 = none */
const char *VK_TextureName (int slot);

/* vk_draw.c: the 2D batch drawn by GL_EndRendering */
void VK_InitDraw (void);
void Draw_ClearCachedPics (void);	/* after texture slots were purged */
void VK_ShutdownDraw (void);

/* vk_swapchain.c: capture the next presented frame into a TGA file
 * (gl_screen.c's "screenshot" command) */
void VK_RequestScreenshot (const char *filename);

/* vk_shader.c: loads <exe folder>\shaders\<name>.spv, e.g. "fullscreen.vert" */
VkShaderModule VK_LoadShader (const char *name);

/* vk_buffer.c: buffers, and one-time upload commands for load time
 * (VK_EndUpload submits and waits) */
typedef struct
{
	VkBuffer	buffer;
	VmaAllocation	allocation;
	VkDeviceSize	size;
	VkDeviceAddress	address;	/* if created with SHADER_DEVICE_ADDRESS usage */
	void		*mapped;	/* if host visible */
} vk_buffer_t;

typedef enum
{
	VK_MEMORY_DEVICE,	/* device local */
	VK_MEMORY_UPLOAD,	/* mapped, written by the CPU (staging) */
	VK_MEMORY_READBACK	/* mapped, written by the GPU and read by the CPU */
} vk_memory_t;

void VK_InitBuffers (void);
void VK_ShutdownBuffers (void);
void VK_CreateBuffer (vk_buffer_t *b, VkDeviceSize size, VkBufferUsageFlags usage, vk_memory_t memory);
void VK_DestroyBuffer (vk_buffer_t *b);
VkCommandBuffer VK_BeginUpload (void);
void VK_EndUpload (void);
void VK_UploadBuffer (vk_buffer_t *dst, VkDeviceSize offset, const void *data, VkDeviceSize size);

/* vk_material.c: the material table (layout in shaders/hl_shared.h) */
typedef struct
{
	char		name[16];	/* texture name */
	int		base_texture;	/* texture slot */
	int		mask_texture;	/* cutout: texture slot whose alpha < 0.5 are holes, 0 = none */
	int		num_frames;	/* animation: frames in the sequence (1 = none) */
	int		next_frame;	/* material of the next frame */
	int		alternate;	/* first material of the alternate animation, 0 = none */
} vk_material_t;

extern vk_buffer_t	vk_material_table;
extern int		vk_num_materials;	/* including the unused index 0 */

void VK_InitMaterials (void);
void VK_ShutdownMaterials (void);
void VK_ClearMaterials (void);
int VK_AddMaterial (const char *name, int base_texture);
vk_material_t *VK_GetMaterial (int index);
void VK_UploadMaterials (void);
void VK_UploadMaterialRange (int first, int count);	/* new materials, while others are in use */
uint16_t VK_FloatToHalf (float f);

/* vk_world.c: the BSP world and its brush submodels in one GPU buffer:
 * num_primitives VboPrimitives (shaders/hl_shared.h), then their
 * positions (3 vec3 per triangle) for acceleration structure builds.
 * Each model's primitives are grouped into ranges. */
typedef struct
{
	uint32_t	first, count;	/* in primitives */
} vk_primrange_t;

typedef struct
{
	vk_primrange_t	opaque;		/* regular surfaces, lava */
	vk_primrange_t	transparent;	/* water, slime, translucent (*rtex078, *lowlight) */
	vk_primrange_t	sky;
} vk_bspmodel_t;

typedef struct
{
	qmodel_t	*worldmodel;	/* the model the buffer was built for, NULL = none */
	int		num_models;	/* the world (0) and its submodels *1 .. */
	vk_bspmodel_t	*models;
	uint32_t	num_primitives;
	vk_buffer_t	buffer;
	VkDeviceSize	positions_offset;
} vk_world_t;

extern vk_world_t	vk_world;

void VK_InitWorld (void);
void VK_ShutdownWorld (void);
void VK_LoadWorld (qmodel_t *worldmodel);	/* on map change, outside frames */

/* vk_pvs.c: the world's potentially visible sets. A cluster is a vis leaf
 * (leaf number - 1; -1 = none, e.g. the solid leaf). The matrix has one
 * row of bits per cluster, padded to 32 bits; the GPU buffer holds a
 * PVS_HEADER_UINTS header (shaders/hl_shared.h) and the same rows, which
 * shaders/pvs.glsl queries. */
typedef struct
{
	int		num_clusters;
	int		row_bytes;	/* multiple of 4 */
	byte		*matrix;	/* [num_clusters][row_bytes] */
	vk_buffer_t	buffer;
	int		one_way_pairs;	/* made symmetric */
	int		patched;	/* transparent triangles whose sides were connected */
} vk_pvs_t;

extern vk_pvs_t		vk_pvs;

void VK_InitPVS (void);
void VK_BuildPVS (qmodel_t *worldmodel);	/* decompress; before the triangles */
void VK_ConnectPVSAcross (int front, int back);	/* a transparent triangle's two sides */
void VK_FinishPVS (void);			/* make symmetric, upload */
void VK_FreePVS (void);
const byte *VK_ClusterPVS (int cluster);	/* NULL for -1: everything visible */
int VK_PointCluster (qmodel_t *worldmodel, const vec3_t point);

/* vk_model.c: alias models on the GPU. Each model's triangles and poses
 * (AliasModel in shaders/hl_shared.h) are built from gl_model.c's data on
 * map load or when the model is first drawn; every frame,
 * VK_UpdateModelGeometry runs model_geometry.comp over the frame's alias
 * instances into this frame's instanced buffer (VERTEX_BUFFER_INSTANCED:
 * VboPrimitives, then their positions for the dynamic BLASes). */
typedef struct
{
	qmodel_t	*model;
	vk_buffer_t	buffer;		/* [AliasTriangle x num_tris][pose vertices x num_poses x num_pose_verts] */
	int		num_tris;	/* 0: nothing to draw */
	int		num_pose_verts;	/* vertices per pose (gl_mesh.c's command vertices) */
	int		num_poses;
	int		num_skins;
} vk_aliasmodel_t;

void VK_InitModels (void);
void VK_ShutdownModels (void);
void VK_LoadModels (void);		/* on map change, after VK_LoadWorld: every alias model in cl.model_precache */
int VK_AliasModelIndex (qmodel_t *model);	/* builds the model's data on first use; -1 = can't be drawn */
const vk_aliasmodel_t *VK_GetAliasModel (int index);
void VK_UpdateModelGeometry (void);	/* in R_RenderView, after VK_UpdateInstances */
qboolean VK_ModelGeometryBuiltThisFrame (void);
const vk_buffer_t *VK_InstancedBuffer (void);	/* the current frame's */
VkDeviceAddress VK_InstancedPositionsAddress (void);

/* vk_skin.c: alias model skins: GL's choice of skin per entity, as a
 * material (one per skin texture; the skin is the cutout mask of EF_HOLEY
 * models), and R_TranslatePlayerSkin */
struct scene_entity_s;
void R_InitSkins (void);
void VK_ClearSkins (void);			/* on map change, after VK_LoadWorld */
void VK_AddSkinMaterials (qmodel_t *model);	/* on map load; the caller uploads the materials */
qboolean VK_ModelHasCutouts (const qmodel_t *model);
int VK_SkinMaterial (const struct scene_entity_s *e, const aliashdr_t *hdr, qboolean *bad_skin);

/* vk_instance.c: the frame's model instances (ModelInstance in
 * shaders/hl_shared.h): the brush entities, then the alias entities group
 * by group; rebuilt from r_scene by R_RenderView and copied to this
 * frame's mapped buffer */
enum
{
	MODEL_GROUP_OPAQUE,
	MODEL_GROUP_TRANSPARENT,	/* DRF_TRANSLUCENT, EF_TRANSPARENT, EF_SPECIAL_TRANS */
	MODEL_GROUP_MASKED,		/* EF_HOLEY: cutouts, alpha tested */
	NUM_MODEL_GROUPS
};

typedef struct
{
	int		first_instance;	/* the alias instances in the instance list */
	int		num_instances;
	vk_primrange_t	groups[NUM_MODEL_GROUPS];	/* their triangles in the instanced buffer, in this order */
	int		dropped;	/* alias entities left out this frame: no room */
	int		dropped_total;	/* the same since the map loaded */
	int		bad_frames;	/* entities with a frame number the model doesn't have */
	int		bad_skins;	/* the same for skin numbers */
} vk_modelframe_t;

void VK_InitInstances (void);
void VK_ShutdownInstances (void);
void VK_ClearInstances (void);		/* on map change */
void VK_UpdateInstances (void);
int VK_NumInstances (void);
const vk_buffer_t *VK_InstanceBuffer (void);	/* the current frame's */
const vk_modelframe_t *VK_ModelFrame (void);
struct ModelInstance;
struct scene_entity_s;
const struct ModelInstance *VK_GetInstance (int i);
const struct scene_entity_s *VK_InstanceEntity (int i);
int VK_InstanceSubmodel (int i);		/* brush submodel number (*N), 0 = none (alias models) */

/* vk_accel.c: acceleration structures. Static BLASes for the world's and
 * the submodels' primitive ranges are built on map load; every frame, the
 * dynamic BLASes over the instanced buffer's model triangles (opaque,
 * transparent) and the TLAS (world + model instances, shaders/hl_shared.h)
 * are rebuilt in the frame's command buffer, one per frame in flight. */
void VK_InitAccel (void);
void VK_ShutdownAccel (void);
void VK_BuildWorldAccel (void);		/* after the world buffer is uploaded */
void VK_FreeWorldAccel (void);
void VK_BuildTLAS (void);		/* in R_RenderView, after VK_UpdateModelGeometry */
VkDeviceAddress VK_TLASAddress (void);	/* the current frame's */
VkDeviceAddress VK_TLASInfoAddress (void);	/* its TlasInstanceInfo[] */
qboolean VK_TLASBuiltThisFrame (void);

/* vk_view.c: the 3D view. R_RenderView calls VK_RenderView3D after the
 * TLAS: the view pass (r_debugview's debug_view.comp for now) renders
 * into the view image; GL_EndRendering calls VK_DrawView3D, which copies
 * it into the swapchain's 3D view rectangle before the 2D. */
void VK_InitView (void);
void VK_ShutdownView (void);
void VK_RenderView3D (void);
void VK_DrawView3D (void);

#endif	/* HEXENLICHT_VK_LOCAL_H */
