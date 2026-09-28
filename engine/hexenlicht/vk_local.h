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
unsigned short VK_TextureCRC (int slot);	/* of its data (the cache key) */
#define VK_EMISSIVE_THRESHOLD	215	/* Quake II RTX's pt_surface_lights_threshold: a skin texel with a channel this bright (sRGB) emits */
unsigned int *VK_TextureRGBA (int slot, int *width, int *height);	/* a bright skin's pixels (malloc'd), NULL = none kept */

/* vk_draw.c: the 2D batch drawn by GL_EndRendering */
void VK_InitDraw (void);
void Draw_ClearCachedPics (void);	/* after texture slots were purged */
void VK_ShutdownDraw (void);
void VK_DestroyDrawPipeline (void);	/* rebuilt when next drawn */
void VK_DrawShade (int x, int y, int w, int h, float alpha);	/* a translucent black box, 2D coordinates */
void VK_DrawBox (float x0, float y0, float x1, float y1, const float *rgba);	/* a box of a color (sRGB 0-1), 2D coordinates */

/* vk_profiler.c: GPU timers (Quake II RTX's profiler.c). A pass is
 * bracketed by VK_ProfilerStart and VK_ProfilerStop with its PROF_* entry
 * (nested in the list's order; a debug label each with the validation layer);
 * VK_BeginFrame and VK_EndFrame bracket the frame, and read the timings of
 * the frame in flight's last run. profiler 1 draws them (VK_DrawProfiler,
 * from GL_EndRendering), vk_profiler prints them; vk_benchmark 1 lifts the
 * 72 fps cap, the unfocused window's sleep and the throttle for measuring. */
#define PROFILER_LIST \
	PROF_DO(FRAME,			"frame",		0) \
	PROF_DO(MODELS,			"model geometry",	1) \
	PROF_DO(BLAS,			"dynamic BLASes",	1) \
	PROF_DO(TLAS,			"TLAS",			1) \
	PROF_DO(VIEW,			"3D view",		1) \
	PROF_DO(PRIMARY,		"primary rays",		2) \
	PROF_DO(REFLECT,		"reflect/refract",	2) \
	PROF_DO(GRADIENT,		"gradient reproject",	2) \
	PROF_DO(DIRECT,			"direct lighting",	2) \
	PROF_DO(DEBUG,			"debug view",		2) \
	PROF_DO(BOUNCE1,		"bounce 1",		2) \
	PROF_DO(BOUNCE2,		"bounce 2",		2) \
	PROF_DO(DENOISER,		"denoiser",		2) \
	PROF_DO(DENOISE_GRADIENTS,	"gradients",		3) \
	PROF_DO(DENOISE_TEMPORAL,	"temporal",		3) \
	PROF_DO(DENOISE_ATROUS,		"a-trous",		3) \
	PROF_DO(COMPOSITING,		"compositing",		2) \
	PROF_DO(INTERLEAVE,		"interleave",		2) \
	PROF_DO(UPSCALE,		"TAA",			2) \
	PROF_DO(BLOOM,			"bloom",		2) \
	PROF_DO(TONEMAP,		"tone mapping",		2) \
	PROF_DO(FSR,			"FSR",			2) \
	PROF_DO(COMPOSITE,		"composite and 2D",	1)

enum
{
#define PROF_DO(id, name, indent) PROF_##id,
	PROFILER_LIST
#undef PROF_DO
	NUM_PROF_ENTRIES
};

void VK_InitProfiler (void);
void VK_ShutdownProfiler (void);
void VK_ProfilerBeginFrame (VkCommandBuffer cmd);	/* VK_BeginFrame */
void VK_ProfilerEndFrame (VkCommandBuffer cmd);		/* VK_EndFrame */
void VK_ProfilerStart (VkCommandBuffer cmd, int entry);
void VK_ProfilerStartNamed (VkCommandBuffer cmd, int entry, const char *name);	/* name: a string that lasts */
void VK_ProfilerLabel (int entry, const char *name);	/* renames a running entry for this frame */
void VK_ProfilerStop (VkCommandBuffer cmd, int entry);
qboolean VK_ProfilerTime (int entry, double *last, double *average);	/* ms; false: not in the latest frame read */
void VK_DrawProfiler (void);		/* GL_EndRendering, before the 2D batch is drawn */
qboolean VK_Benchmark (void);		/* vk_benchmark: also host.c and sys_win.c */

/* vk_swapchain.c: capture the next presented frame into a TGA file
 * (gl_screen.c's "screenshot" command) */
void VK_RequestScreenshot (const char *filename);
void VK_RequestScreenshotAverage (const char *filename, int frames);	/* the next frames averaged in linear light (4.9) */

/* vk_calib.c: calibration against GL (4.9): vk_setpos, vk_bookmark,
 * vk_screenshot */
void VK_InitCalib (void);

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
	int		emissive_texture;	/* texture slot of the emitted radiance, 0 = none (vk_emissive.c) */
	float		emissive_factor;	/* times the emissive texture (1) */
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
float VK_HalfToFloat (uint16_t h);

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
 * models; emissive: with the skin's emissive texture, 4.5), and
 * R_TranslatePlayerSkin */
struct scene_entity_s;
void R_InitSkins (void);
void VK_ClearSkins (void);			/* on map change, after VK_LoadWorld */
void VK_AddSkinMaterials (qmodel_t *model);	/* on map load; the caller uploads the materials */
qboolean VK_ModelHasCutouts (const qmodel_t *model);
int VK_SkinMaterial (const struct scene_entity_s *e, const aliashdr_t *hdr, qboolean emissive, qboolean *bad_skin);

/* vk_emissive.c: emissive surfaces (4.5): lava, whose world triangles facing
 * out of it are polygon lights, and the flames of the models at the map's
 * lights (vk_instance.c's light group) */
void VK_InitEmissive (void);			/* its cvars, from VK_InitLights */
void VK_ShutdownEmissive (void);		/* from VK_ShutdownLights */
void VK_ClearLava (void);			/* VK_LoadWorld, before the surfaces */
void VK_AddLavaMaterial (int material, const texture_t *tx);	/* the material of a lava surface */
struct VboPrimitive;
qboolean VK_AddLavaLight (const struct VboPrimitive *p);	/* a world lava triangle facing out of the lava; false: not a light (degenerate, or its material past the lava table) */
void VK_FinishLava (void);			/* after the surfaces: the lava materials' emission */
int VK_NumLavaLights (void);			/* the lava's polygon lights, none with r_lava_light 0 */
void VK_GetLavaLight (int i, vec3_t p[3], vec3_t color);	/* its corners (emitting along cross(p1 - p0, p2 - p0)) and radiance */
qboolean VK_OverLava (const vec3_t origin);	/* within 16 units of a lava light, either side (vk_maplights.c: a fake lava light) */
qboolean VK_LavaLightsOn (void);		/* vk_light.c: the lava's lights are in the light buffer (the fake lava lights out) */
qboolean VK_ModelsEmit (void);			/* r_emissive_models */
int VK_EmissiveSkin (int slot);			/* the skin's emissive texture, made on first use; 0 = none */
float VK_EmissiveScale (void);			/* r_emissive_scale: the emissive materials' factor */
void VK_PrintEmissive (void);			/* vk_lights */

/* vk_sky.c: Hexen II's sky as GL draws it (env_map), faithful or lighting
 * the scene (r_sky_light: a constant dome, an optional sun), 4.6 */
void VK_InitSky (void);
void VK_ShutdownSky (void);
void VK_LoadSky (qmodel_t *worldmodel, const struct VboPrimitive *prims, uint32_t num_prims);	/* VK_LoadWorld, after the PVS */
const uint32_t *VK_SkyVisibility (uint32_t *version);	/* LightBuffer's sky_visibility; the version counts the maps */
struct QVKUniformBuffer_s;
void VK_PrepareSky (struct QVKUniformBuffer_s *ubo);	/* VK_PrepareUBO */

/* vk_instance.c: the frame's model instances (ModelInstance in
 * shaders/global_ubo.h): the brush entities, then the alias entities group
 * by group, the first-person weapon last; rebuilt from r_scene by
 * R_RenderView and copied to this frame's mapped buffer */
enum
{
	MODEL_GROUP_OPAQUE,
	MODEL_GROUP_TRANSPARENT,	/* DRF_TRANSLUCENT, EF_TRANSPARENT, EF_SPECIAL_TRANS */
	MODEL_GROUP_MASKED,		/* EF_HOLEY: cutouts, alpha tested */
	MODEL_GROUP_LIGHT,		/* opaque ones around a light (at a map light's origin: torches, flames; owning a dynamic light): no shadows */
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
	int		emissive;	/* instances with an emissive skin (4.5: the light models' flames) */
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

/* vk_upscale.c: the upscaler interface (Quake II RTX's
 * evaluate_taa_settings, TAA and FSR 1). VK_UpscaleEvaluate decides the
 * 3D frame's sizes, jitter and passes for the view (r_scale, r_upscaler),
 * which VK_PrepareUBO puts into the UBO; VK_UpscaleHDR runs the TAA pass
 * on the lit image before bloom and tone mapping, VK_UpscaleDisplay FSR
 * after them; the composite shows display_source's top left display_size
 * over the view. DLSS SR and RR (vk_dlss.c) replace the TAA pass. */
typedef struct
{
	int		dlss;		/* 0, or DLSS instead of the TAA pass: VK_SL_SR, VK_SL_RR (vk_streamline.h) */
	int		dlss_mode;	/* its VK_SL_MODE_* */
	qboolean	denoise;	/* the denoiser runs: flt_enable, not with DLSS RR */
	VkExtent2D	view;		/* the 3D view in the swapchain */
	VkExtent2D	unscaled;	/* the view with the width rounded up to even: the upscalers' output,
					 * shown 1:1 (an odd view width drops its last column) */
	VkExtent2D	render;		/* what the path tracer renders: unscaled x r_scale, the width even */
	VkExtent2D	taa_output;	/* what the TAA pass writes, bloom and tone mapping take: render
					 * (TAA, FSR, the debug views) or unscaled (TAAU) */
	float		jitter[2];	/* the primary rays' sub-pixel offset (TAAU, FSR; else 0) */
	qboolean	taa;		/* the TAA pass runs: the lit image */
	int		taa_mode;	/* its flt_taa: AA_MODE_UPSCALE (the blend) or AA_MODE_OFF (a copy) */
	qboolean	fsr_easu, fsr_rcas;	/* FSR's steps, after tone mapping */
	float		lod_bias;	/* added to pt_texture_lod_bias: log2 of the scale when upscaling */
	uint32_t	easu_const[4][4];	/* FsrEasuCon's, for the UBO */
	uint32_t	rcas_const[4];		/* FsrRcasCon's */
	int		display_source;	/* what the composite shows: 0 TAA_OUTPUT, 1 FSR_EASU_OUTPUT, 2 FSR_RCAS_OUTPUT */
	VkExtent2D	display_size;	/* its top left part covering the view */
	qboolean	display_lanczos;	/* scaled with Lanczos, else 1:1 or nearest */
} vk_upscale_t;

void VK_InitUpscale (void);
void VK_ShutdownUpscale (void);
void VK_DestroyUpscalePipelines (void);	/* rebuilt when next used */
const vk_upscale_t *VK_UpscaleEvaluate (uint32_t view_width, uint32_t view_height, int debug_view);	/* before VK_PrepareUBO */
const vk_upscale_t *VK_Upscale (void);	/* this frame's */
void VK_UpscaleHDR (VkCommandBuffer cmd);	/* the TAA pass */
void VK_UpscaleDisplay (VkCommandBuffer cmd);	/* FSR */
void VK_EndUpscaleFrame (void);		/* the frame's TAA output is the next one's history */
int VK_UpscaleDLSSFeature (void);	/* the DLSS feature r_upscaler asks for: 0, VK_SL_SR, VK_SL_RR */

/* vk_dlss.c: DLSS SR and RR through Streamline (vk_streamline.cpp, the
 * player's DLLs) in the TAA pass's place. VK_DLSSChoose decides from
 * VK_UpscaleEvaluate whether it runs and its mode and render size;
 * checkerboard_interleave.comp writes its inputs; VK_DLSSRun evaluates it
 * instead of the TAA pass; VK_DLSSBetweenFrames (VK_BeginFrame) recreates
 * the render targets when its images change and frees what a feature no
 * longer chosen holds. */
void VK_InitDLSS (void);
void VK_ShutdownDLSS (void);
int VK_DLSSImagesWanted (void);		/* vk_dlss_images for the chosen feature */
const char *VK_DLSSUnavailable (int feature);	/* why it can't run, or NULL */
qboolean VK_DLSSChoose (int feature, qboolean lit, int percent, vk_upscale_t *up);
qboolean VK_DLSSRun (VkCommandBuffer cmd, const vk_upscale_t *up);	/* false: it failed */
void VK_EndDLSSFrame (qboolean ran);
void VK_ResetDLSSHistory (void);	/* with the denoiser's */
void VK_DLSSUpscalerChanged (void);	/* r_upscaler changed: a failed feature gets another try */
void VK_DLSSBetweenFrames (void);	/* outside frames */

/* vk_ubo.c: the global uniform buffer (shaders/global_ubo.h), one per frame
 * in flight: descriptor set 0 of the view passes. VK_PrepareUBO fills the
 * current frame's from r_scene and the frame's upscaling (the render size
 * global_ubo.width x height, the TAA output's taa_output_width x height,
 * the jitter) and counts the 3D frames (vk_render_frame, the UBO's
 * current_frame_idx). */
extern VkDescriptorSetLayout	vk_ubo_set_layout;
extern uint32_t			vk_render_frame;
void VK_InitUBO (void);
void VK_ShutdownUBO (void);
void VK_CheckDenoiserCvars (void);	/* before VK_UpscaleEvaluate: a change drops the history */
void VK_PrepareUBO (const vk_upscale_t *up, int debug_view);
void VK_ResetUBOHistory (void);	/* the next frame's _prev values are its own */
const struct QVKUniformBuffer_s *VK_CurrentUBO (void);	/* this frame's, after VK_PrepareUBO */
qboolean VK_ToneMappingEnabled (void);	/* tm_enable */
qboolean VK_AutoExposure (void);	/* vk_tonemap.c: tm_auto_exposure (0: the fixed exposure, 4.9) */
VkDescriptorSet VK_UBOSet (void);	/* the current frame's */
float VK_NumBounceRays (void);	/* pt_num_bounce_rays: 0, 0.5, 1 or 2 */
qboolean VK_DenoiserEnabled (void);	/* flt_enable */
int VK_ReflectRefractPasses (void);	/* pt_reflect_refract: 0 to 10 */

/* vk_maplights.c: the map's light entities as lights (utils/light's
 * rules), loaded by VK_LoadWorld before the light lists; VK_MapLightAt:
 * is a map light at the origin (vk_instance.c's MODEL_GROUP_LIGHT; none with
 * r_maplights 0, but models owning a dynamic light join it then too) */
#define VK_DEFAULT_LIGHT_LEVEL	300	/* utils/light's DEFAULTLIGHTLEVEL */

typedef struct
{
	vec3_t		origin;
	int		level;		/* utils/light's: also the range */
	int		style;		/* animated with 4.2 */
	vec3_t		color;		/* linear (r_maplight_colors; jsh2color's up to 1.19) */
	vec3_t		spot_dir;	/* towards its target; 0 0 0: not a spot */
	float		spot_cos;	/* the cosine of half the cone's width */
	qboolean	over_lava;	/* a plain light close over lava: left out while lava emits (4.5) */
	float		scale;		/* the intensity times this (4.7: the map file's scale, 1) */
} vk_maplight_t;

void VK_InitMapLights (void);	/* its cvars, from VK_InitLights */
void VK_LoadMapLights (qmodel_t *worldmodel);
void VK_ClearMapLights (void);
const vk_maplight_t *VK_MapLights (int *count);	/* none with r_maplights 0 */
float VK_MapLightIntensity (const vk_maplight_t *l);	/* pi x radiance, white (GL's shape: a full GL texel's light, 4.15) */
float VK_LightLevelIntensity (float level);	/* the same for a utils/light level */
float VK_MapLightRange (void);	/* r_maplight_range: a map light's range is its level times this (4.9; GL's shape: 1) */
int VK_MapLightShape (void);	/* r_maplight_shape: SPHERE_SHAPE_* (4.15) */
float VK_MapLightGamma (void);	/* r_maplight_gamma (4.15) */
float VK_MapLightRadius (void);	/* r_maplight_radius (4.15) */
qboolean VK_DynamicLightOwner (int entnum, const vec3_t origin, float radius);	/* vk_light.c: the entity owns a lit dynamic light within radius this frame */
float VK_SRGBToLinear (float c);	/* a GL light color's linear value */
qboolean VK_MapLightAt (const vec3_t origin);
void VK_CountMapLightModels (int n);	/* vk_instance.c, each frame */
void VK_PrintMapLights (void);	/* vk_lights */
void VK_PrintMapLightColors (void);	/* vk_lights colors */
void VK_PrintMapLightEdits (void);	/* vk_mapfile: what the map file's light edits did */
qboolean VK_MapLightDroppedAt (const int *p);	/* a light entity the compiler lit nothing from at the point (to the unit) */
void VK_ApplyMapEdits (qmodel_t *worldmodel);	/* the map file's light lines again onto the lights VK_LoadMapLights read (then VK_RebuildLights) */
qboolean VK_MapLightColorsOn (void);	/* r_maplight_colors */

/* a light vk_lightedit.c (4.8) can select: each of the lump's lights and
 * the map file's addlights, with the file's changes (VK_EditableLights,
 * valid until the edits are applied again) */
typedef struct
{
	int		entity;		/* in the lump, -1: an addlight */
	int		line_id;	/* an addlight's line (vk_mapfile.c) */
	const char	*classname;
	vec3_t		lump_origin;	/* the entity's origin, which a light line names (an addlight's own) */
	int		lump_level;	/* the map's own (an addlight: 300) */
	int		lump_style;
	vec3_t		origin;		/* now */
	int		level;
	int		style;
	float		scale;
	vec3_t		srgb;		/* its color as the map file gives colors (sRGB; jsh2color's up to 1.08), with r_maplight_colors 1 */
	const char	*color_from;	/* where srgb comes from */
	qboolean	spot;
	float		spot_cos;	/* the cosine of half its cone's width */
	qboolean	off;		/* taken out by the map file */
	qboolean	in_solid;	/* moved or added inside solid: dropped */
	qboolean	edited;		/* the map file changes it (an addlight: always) */
} vk_editablelight_t;

const vk_editablelight_t *VK_EditableLights (int *count);

/* vk_mapfile.c: the per-map override file maps/<map>.hlmap (4.7): the
 * per-map cvars (sky, sun, r_map_light_scale, r_map_exposure), reset to
 * their defaults at every map load before the file sets them, and light
 * edits, which vk_maplights.c applies when it builds the map's lights */
enum
{
	MAPEDIT_OFF	= 1 << 0,
	MAPEDIT_LEVEL	= 1 << 1,
	MAPEDIT_SCALE	= 1 << 2,
	MAPEDIT_COLOR	= 1 << 3,
	MAPEDIT_STYLE	= 1 << 4,
	MAPEDIT_ORIGIN	= 1 << 5
};

typedef struct
{
	qboolean	add;		/* addlight, else light */
	int		at[3];		/* light: the entity origin it changes, to the unit */
	vec3_t		origin;		/* addlight's, or where "origin" moves the light */
	int		keys;		/* MAPEDIT_* given */
	int		level;
	float		scale;
	vec3_t		color;		/* sRGB 0-1 */
	int		style;
	int		line;		/* in the file */
	int		id;		/* its line's, which lasts while lines come and go (4.8) */
	int		matched;	/* set by vk_maplights.c: lights it changed; addlight: 1 if added */
} vk_mapedit_t;

void VK_InitMapFile (void);	/* after vk_sky.c's cvars */
void VK_ShutdownMapFile (void);
void VK_LoadMapFile (qmodel_t *worldmodel);	/* VK_LoadWorld, before the map's lights */
vk_mapedit_t *VK_MapEdits (int *count);
float VK_MapLightScale (void);	/* r_map_light_scale */
float VK_MapExposure (void);	/* r_map_exposure: EV of the fixed exposure, or added to tm_exposure_bias (4.9) */
/* the light editor's (4.8): the file is kept as its lines, which the edits
 * rewrite and VK_SaveMapFile writes */
qboolean VK_MapFileLightEdit (const int *at, vk_mapedit_t *merged);	/* the light lines of the entity origin, merged in order; false: none */
const vk_mapedit_t *VK_MapFileAddLight (int id);	/* the addlight line with this id, NULL = none */
int VK_MapFileSetLight (const vk_mapedit_t *e, qboolean remove);	/* the light's lines become e (see vk_mapfile.c); its line's id, 0 = removed */
qboolean VK_SaveMapFile (void);
int VK_MapFileUnsaved (void);	/* editor changes since the file was read or saved */
const char *VK_MapFileLine (int id, int *number);	/* the line's text and number, NULL = none */
const char *VK_MapFileName (void);	/* maps/<map>.hlmap */
const char *VK_MapFileUsed (void);	/* the file used and where from, "" = none */

/* vk_lightedit.c: live light editing (4.8): r_editlights' markers and
 * panel, the vk_editlight command */
void VK_InitLightEditor (void);	/* after vk_mapfile.c */
void VK_ShutdownLightEditor (void);
void VK_ClearLightEditor (void);	/* a new map: nothing selected */
void VK_DrawLightEditor (void);	/* R_RenderView, after the 3D view: its 2D under the HUD */

/* vk_lightcolor.c: utils/jsh2color's colors of the map's lights (what
 * Hammer of Thyrion's .lit files are baked from) */
typedef struct
{
	const char	*list;		/* the texture list the tool's batch files use for the map */
	int		entities;	/* in the lump */
	int		colored;	/* entities not grey: with none the tool wrote no .lit */
	double		seconds;
} vk_lightcolors_t;
int *VK_LightColors (qmodel_t *worldmodel, vk_lightcolors_t *info);	/* 0-275 per entity (3 ints), freed by the caller */

/* vk_light.c: the path tracer's lights (the map's, test lights from
 * vk_testlight) and their per-cluster lists, built when the lights change (VK_UpdateLights);
 * VK_PrepareLights fills this frame's light buffer (the lights, the lists
 * when they changed), the UBO's dynamic sphere lights and the light
 * statistics buffers, which VK_ClearLightStats clears before the passes */
void VK_InitLights (void);
void VK_ShutdownLights (void);
void VK_ClearLights (void);	/* a new map (VK_LoadWorld) */
struct VboPrimitive;
void VK_LoadLightClusters (qmodel_t *worldmodel, const struct VboPrimitive *prims, uint32_t num_prims);	/* after the PVS */
void VK_UpdateLights (void);	/* after the lights change, outside frames */
void VK_RebuildLights (void);	/* the same from cvars and commands: only in a loaded world */
struct QVKUniformBuffer_s;
void VK_PrepareLights (struct QVKUniformBuffer_s *ubo);	/* shaders/global_ubo.h */
void VK_ClearLightStats (VkCommandBuffer cmd);	/* after VK_PrepareUBO, before the passes */

/* vk_images.c: the render targets (shaders/global_textures.h's
 * LIST_IMAGES, VKPT_IMG_*) at the swapchain's size (the width rounded up
 * to even), in the GENERAL layout, and the blue noise: descriptor set 1 of
 * the view passes, even or odd by vk_render_frame */
extern VkExtent2D		vk_image_extent;	/* 0 x 0 = none */
extern int			vk_dlss_images;		/* DLSS's inputs at full size: 0, 1 the depth, 2 RR's too */
extern VkDescriptorSetLayout	vk_images_set_layout;
void VK_InitImages (void);
void VK_ShutdownImages (void);
void VK_CreateImages (void);
void VK_DestroyImages (void);
qboolean VK_ImagesReady (void);
VkImage VK_Image (int index);
void VK_ImageInfo (int index, VkImage *image, VkImageView *view, VkFormat *format, uint32_t *width, uint32_t *height);
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
VkPipeline VK_CreateComputePipelineSpec (const char *shader, VkPipelineLayout layout, uint32_t value);	/* constant_id 0 */
VkPipeline VK_CreateComputePipelineSpecs (const char *shader, VkPipelineLayout layout, const uint32_t *values, int count);	/* constant_id 0..count-1 */
void VK_BindPassSets (VkCommandBuffer cmd, VkPipelineBindPoint bind_point, VkPipelineLayout layout);
void VK_DispatchRays (VkCommandBuffer cmd, VkPipeline pipeline, const pt_push_constants_t *push,
		      uint32_t width, uint32_t height, uint32_t depth);
void VK_RenderTargetBarrier (VkCommandBuffer cmd, VkImage image,
			     VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
			     VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access);
void VK_ComputeBarrier (VkCommandBuffer cmd);	/* between compute passes */
void VK_DispatchCompute (VkCommandBuffer cmd, VkPipeline pipeline, uint32_t width, uint32_t height, uint32_t local_size);
void VK_DispatchComputeLayout (VkCommandBuffer cmd, VkPipeline pipeline, VkPipelineLayout layout, const void *push,
			       uint32_t push_size, uint32_t width, uint32_t height, uint32_t local_size);

/* vk_asvgf.c: the denoiser (Quake II RTX's A-SVGF), for a view rendered
 * width x height: the gradient samples before the lighting passes, the
 * filters after them (instead of compositing.comp); its history is the
 * last 3D frame's images, unless reset */
void VK_DestroyASVGFPipelines (void);	/* rebuilt when next used */
void VK_GradientReproject (VkCommandBuffer cmd, uint32_t width, uint32_t height);
void VK_DenoiseLighting (VkCommandBuffer cmd, uint32_t width, uint32_t height, qboolean enable_lf);
void VK_ResetDenoiserHistory (void);	/* a new map, new images, a skipped view, changed cvars */
qboolean VK_DenoiserHistoryValid (void);	/* for VK_PrepareUBO */
void VK_EndDenoiserFrame (qboolean denoised);	/* the frame's images are the next one's history */

/* vk_bloom.c and vk_tonemap.c: Quake II RTX's bloom, tone mapping and auto
 * exposure, in place on TAA_OUTPUT's width x height (the TAA output's
 * size) after the TAA pass (r_debugview 0);
 * the adapted luminance comes back through a readback buffer per frame in
 * flight */
void VK_InitBloom (void);
void VK_ShutdownBloom (void);
void VK_DestroyBloomPipelines (void);	/* rebuilt when next used */
qboolean VK_BloomEnabled (void);	/* bloom_enable */
float VK_BloomIntensity (void);		/* bloom_intensity, for the UBO */
void VK_Bloom (VkCommandBuffer cmd, uint32_t width, uint32_t height);
void VK_InitToneMap (void);
void VK_ShutdownToneMap (void);
void VK_DestroyToneMapPipelines (void);	/* rebuilt when next used */
void VK_ToneMap (VkCommandBuffer cmd, uint32_t width, uint32_t height, float frame_time);
void VK_ResetToneMapping (void);	/* a new map: the exposure starts over */
VkDeviceAddress VK_ToneMapBufferAddress (void);
VkDeviceAddress VK_ReadbackAddress (float *adapted_luminance);	/* this frame's; the luminance read back from it */

/* vk_view.c: the 3D view. R_RenderView calls VK_RenderView3D after the
 * TLAS: it fills the UBO and runs the view passes (the G-buffer, the
 * lighting, the denoiser, the upscaler, bloom, tone mapping; or
 * r_debugview's debug_view.comp) into the TAA_OUTPUT render target (FSR's
 * outputs with FSR); GL_EndRendering calls VK_DrawView3D, which scales it
 * into the swapchain's 3D view rectangle before the 2D. */
void VK_InitView (void);
void VK_ShutdownView (void);
void VK_DestroyViewPipelines (void);	/* rebuilt when next used */
void VK_RenderView3D (void);
void VK_DrawView3D (void);

#endif	/* HEXENLICHT_VK_LOCAL_H */
