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

/* vk_core.c: the renderer's modules are initialized and shut down from one
 * table (Quake II RTX's vkpt_initialize_all): all at startup; those that
 * depend on the swapchain's size again when it is recreated
 * (VK_SwapchainRecreated); those with pipelines on vk_reload_shaders,
 * which rebuilds them from the SPIR-V on disk. */
void VK_Init (HINSTANCE hinstance, HWND hwnd);
void VK_Shutdown (void);
void VK_SwapchainRecreated (void);

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
void VK_DestroyDrawPipeline (void);	/* rebuilt when next drawn */

/* vk_swapchain.c: capture the next presented frame into a TGA file
 * (gl_screen.c's "screenshot" command) */
void VK_RequestScreenshot (const char *filename);

/* vk_shader.c: loads <exe folder>\shaders\<name>.spv, e.g. "fullscreen.vert";
 * VK_ExePath gives <exe folder>\<file> */
VkShaderModule VK_LoadShader (const char *name);
void VK_ExePath (const char *file, char *path, size_t size);

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

/* vk_material.c: the material table (layout in shaders/vertex_buffer.h) */
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
 * num_primitives VboPrimitives (shaders/vertex_buffer.h), then their
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
void VK_CreateModelPipelines (void);
void VK_DestroyModelPipelines (void);
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
 * shaders/global_ubo.h): the brush entities, then the alias entities group
 * by group, the first-person weapon last; rebuilt from r_scene by
 * R_RenderView and copied to this frame's mapped buffer */
enum
{
	MODEL_GROUP_OPAQUE,
	MODEL_GROUP_TRANSPARENT,	/* DRF_TRANSLUCENT, EF_TRANSPARENT, EF_SPECIAL_TRANS */
	MODEL_GROUP_MASKED,		/* EF_HOLEY: cutouts, alpha tested */
	MODEL_GROUP_WEAPON,		/* the first-person weapon (Quake II RTX's viewer weapon) */
	NUM_MODEL_GROUPS
};

typedef struct
{
	int		first_instance;	/* the alias instances in the instance list */
	int		num_instances;
	vk_primrange_t	groups[NUM_MODEL_GROUPS];	/* their triangles in the instanced buffer, in this order */
	int		weapon_look;	/* the weapon's MODEL_GROUP_OPAQUE, _TRANSPARENT or _MASKED */
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

/* vk_effects.c: the frame's particles and sprites as triangles, in a
 * mapped buffer per frame in flight (layout in shaders/hl_shared.h) */
typedef struct
{
	uint64_t	frame_count;		/* vk.frame_count it was written in */
	int		num_particles;		/* one triangle each */
	int		num_sprites;		/* one quad each */
	VkDeviceAddress	positions;		/* vec3 per vertex: the particles' */
	VkDeviceAddress	sprite_positions;	/* the sprites', after them */
	VkDeviceAddress	particles;		/* EffectParticle[] */
	VkDeviceAddress	sprites;		/* EffectSprite[] */
	VkDeviceAddress	indices;		/* the sprite quads' uint16 indices */
	int		dropped_particles;	/* left out: no room */
	int		dropped_sprites;
	int		bad_frames;		/* sprite frame numbers the model doesn't have */
	int		edge_on;		/* upright sprites seen from straight above or below: GL skips them */
} vk_effectsframe_t;

void VK_InitEffects (void);
void VK_ShutdownEffects (void);
void VK_ClearEffects (void);			/* on map change */
void VK_UpdateEffects (void);			/* in R_RenderView, before VK_BuildTLAS */
const vk_effectsframe_t *VK_EffectsFrame (void);	/* the current frame's; nothing unless written this frame */
int VK_ParticleTexture (void);

/* vk_accel.c: acceleration structures. Static BLASes for the world's and
 * the submodels' primitive ranges are built on map load; every frame, the
 * dynamic BLASes over the instanced buffer's model triangles (opaque,
 * transparent, masked) and the effects (particles, sprites), the TLAS
 * (world + model instances; TlasInstanceInfo in shaders/global_ubo.h) and
 * the effects TLAS are rebuilt in the frame's command buffer, one per
 * frame in flight. The instances have Quake II RTX's shader binding table
 * offsets (SBTO_*), which its ray query code dispatches candidates by. */
void VK_InitAccel (void);
void VK_ShutdownAccel (void);
void VK_BuildWorldAccel (void);		/* after the world buffer is uploaded */
void VK_FreeWorldAccel (void);
void VK_BuildTLAS (void);		/* in R_RenderView, after VK_UpdateModelGeometry and VK_UpdateEffects */
VkDeviceAddress VK_TLASAddress (void);	/* the current frame's */
VkDeviceAddress VK_TLASInfoAddress (void);	/* its TlasInstanceInfo[] */
qboolean VK_TLASBuiltThisFrame (void);
VkDeviceAddress VK_EffectsTLASAddress (void);	/* the current frame's, 0 = no effects */
VkDeviceAddress VK_LastEffectsTLAS (int *slot, uint64_t *frame_count);	/* the last one built (0 = none), for checks */
void VK_PrintEffectsAccel (void);	/* the effects' BLASes and TLAS, for vk_effects */

/* vk_matrix.c: 4x4 matrices in columns (m[column * 4 + row]), Quake II
 * RTX's matrix.c: view space x right, y up, z forward; clip space y down */
void VK_CreateViewMatrix (float m[16], const vec3_t origin, const vec3_t forward, const vec3_t right, const vec3_t up);
void VK_CreateProjectionMatrix (float m[16], float znear, float zfar, float fov_x, float fov_y);
void VK_InverseMatrix (const float m[16], float inv[16]);

/* vk_ubo.c: the global uniform buffer (shaders/global_ubo.h), one per frame
 * in flight: descriptor set 0 of the view passes. VK_PrepareUBO fills the
 * current frame's from r_scene for a width x height 3D view (rendered at
 * the width rounded up to even: global_ubo.width; the output's size is
 * taa_output_width x taa_output_height) and counts the 3D frames
 * (vk_render_frame, the UBO's current_frame_idx). */
extern VkDescriptorSetLayout	vk_ubo_set_layout;
extern uint32_t			vk_render_frame;
void VK_InitUBO (void);
void VK_ShutdownUBO (void);
void VK_PrepareUBO (uint32_t width, uint32_t height, int debug_view);
VkDescriptorSet VK_UBOSet (void);	/* the current frame's */

/* vk_light.c: the path tracer's lights (test lights, vk_testlight, for now):
 * VK_PrepareLights fills this frame's light buffer (polygon lights and
 * their per-cluster lists) and the UBO's sphere lights */
void VK_InitLights (void);
void VK_ShutdownLights (void);
void VK_ClearLights (void);	/* a new map (VK_LoadWorld) */
struct QVKUniformBuffer_s;
void VK_PrepareLights (struct QVKUniformBuffer_s *ubo);	/* shaders/global_ubo.h */

/* vk_images.c: the render targets (shaders/global_textures.h's
 * LIST_IMAGES, VKPT_IMG_*) at the swapchain's size (the width rounded up
 * to even), in the GENERAL layout, and the blue noise: descriptor set 1 of
 * the view passes, even or odd by vk_render_frame */
extern VkExtent2D		vk_image_extent;	/* 0 x 0 = none */
extern VkDescriptorSetLayout	vk_images_set_layout;
void VK_InitImages (void);
void VK_ShutdownImages (void);
void VK_CreateImages (void);
void VK_DestroyImages (void);
qboolean VK_ImagesReady (void);
VkImage VK_Image (int index);
VkDescriptorSet VK_ImagesSet (void);	/* this 3D frame's */

/* vk_pathtracer.c: the view passes' pipeline layouts (set 0 the UBO, set 1
 * the images, set 2 the bindless textures), ray query compute pipelines
 * (Quake II RTX's path_tracer.c in ray query mode) and image barriers */
typedef struct
{
	int	gpu_index;	/* -1: one GPU */
	int	bounce;
} pt_push_constants_t;	/* shaders/path_tracer.h's push_constant_block */

void VK_InitPathTracer (void);
void VK_ShutdownPathTracer (void);
VkPipelineLayout VK_CreatePassLayout (VkShaderStageFlags push_stages, uint32_t push_size);
VkPipelineLayout VK_PathTracerLayout (void);	/* pt_push_constants_t */
VkPipeline VK_CreateComputePipeline (const char *shader, VkPipelineLayout layout);
void VK_BindPassSets (VkCommandBuffer cmd, VkPipelineBindPoint bind_point, VkPipelineLayout layout);
void VK_DispatchRays (VkCommandBuffer cmd, VkPipeline pipeline, const pt_push_constants_t *push,
		      uint32_t width, uint32_t height, uint32_t depth);
void VK_RenderTargetBarrier (VkCommandBuffer cmd, VkImage image,
			     VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
			     VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access);
void VK_ComputeBarrier (VkCommandBuffer cmd);	/* between compute passes */
void VK_DispatchCompute (VkCommandBuffer cmd, VkPipeline pipeline, uint32_t width, uint32_t height, uint32_t local_size);

/* vk_view.c: the 3D view. R_RenderView calls VK_RenderView3D after the
 * TLAS: it fills the UBO and runs the view passes (primary_rays.rgen, the
 * G-buffer; for now r_debugview's debug_view.comp shows it) into the
 * TAA_OUTPUT render target;
 * GL_EndRendering calls VK_DrawView3D, which copies it into the
 * swapchain's 3D view rectangle before the 2D. */
void VK_InitView (void);
void VK_ShutdownView (void);
void VK_DestroyViewPipelines (void);	/* rebuilt when next used */
void VK_RenderView3D (void);
void VK_DrawView3D (void);

#endif	/* HEXENLICHT_VK_LOCAL_H */
