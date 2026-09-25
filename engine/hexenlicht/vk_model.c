/* vk_model.c -- alias models (MDL) on the GPU
 *
 * Each alias model's GPU data is built from what gl_model.c and gl_mesh.c
 * already made of it, for both MDL formats (Quake's IDPO and Hexen II's
 * RAPO): gl_mesh.c's triangle strips and fans, whose texture coordinates
 * include the seam fix, become one triangle list; the poses are copied as
 * they are, one trivertx_t (4 bytes) per vertex, in the strips' vertex
 * order. A small model table (AliasModel in shaders/hl_shared.h) holds each
 * model's decode scale and the addresses of its data. Models are built for
 * every alias model the map precaches, and when one is first drawn (the
 * client's effects load debris and other models later). The instance
 * carries the material of the skin it shows (vk_skin.c).
 *
 * Every frame, VK_UpdateModelGeometry runs model_geometry.comp: one
 * workgroup per alias instance (vk_instance.c) writes the instance's
 * triangles, blended between two poses and moved into the world, into
 * this frame's instanced buffer (VERTEX_BUFFER_INSTANCED), which the
 * dynamic BLASes (vk_accel.c) and the view passes read.
 *
 * vk_models prints statistics; "vk_models check" compares every triangle
 * of the last frame with the same computation on the CPU.
 *
 * Copyright (C) 1996-1997  Id Software, Inc.
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
#include "r_scene.h"
#include "shaders/hl_shared.h"

COMPILE_TIME_ASSERT(AliasModel, sizeof(AliasModel) == 48);		/* the shaders' std430 layouts */
COMPILE_TIME_ASSERT(AliasTriangle, sizeof(AliasTriangle) == 32);
COMPILE_TIME_ASSERT(ModelGeometryPush, sizeof(ModelGeometryPush) == 48);
COMPILE_TIME_ASSERT(trivertx_t, sizeof(trivertx_t) == 4);		/* one uint per pose vertex */

#define MODEL_HASH_SIZE		2048	/* power of 2, over MAX_ALIAS_MODELS */
#define POSITIONS_OFFSET	((VkDeviceSize)MAX_INSTANCED_PRIMITIVES * sizeof(VboPrimitive))

/* Quake's vertex normals, indexed by trivertx_t.lightnormalindex */
static const float vertex_normals[NUM_VERTEX_NORMALS][3] =
{
#include "anorms.h"
};

static vk_aliasmodel_t	alias_models[MAX_ALIAS_MODELS];
static int		num_alias_models;
static int		too_many_models;	/* models that found no room since the map loaded */
static int		built_later;		/* models built after the map loaded, when first drawn */
static short		model_hash[MODEL_HASH_SIZE];	/* index + 1, 0 = empty */
static qboolean		loading_models;		/* VK_LoadModels: upload the materials at the end */

static vk_buffer_t	model_table;		/* AliasModel[MAX_ALIAS_MODELS], mapped */
static vk_buffer_t	normal_buffer;		/* vec4[NUM_VERTEX_NORMALS] */

/* this frame's model triangles: [VboPrimitive x MAX][float x 9 x MAX] */
static vk_buffer_t	instanced[VK_FRAMES_IN_FLIGHT];

static VkPipelineLayout	geometry_layout;
static VkPipeline	geometry_pipeline;
static VkQueryPool	query_pool;		/* 2 timestamps per frame in flight */
static qboolean		timed[VK_FRAMES_IN_FLIGHT];
static double		geometry_ms;

/* the last geometry pass, for vk_models check */
static struct
{
	int		slot;		/* frame in flight, -1 = none */
	int		framecount;	/* r_scene.framecount */
	uint64_t	frame_count;	/* vk.frame_count */
	vk_modelframe_t	frame;
} last_pass = { -1 };


/* ==========================================================================
 * Model data
 * ========================================================================== */

static unsigned ModelHash (const qmodel_t *m)
{
	return (unsigned)((((uintptr_t)m) >> 4) * 2654435761u) & (MODEL_HASH_SIZE - 1);
}

/* Walks gl_mesh.c's command list: strips (count > 0) and fans (count < 0)
 * of command vertices, each with its s and t; the command vertices are
 * the pose vertices, in order. Writes the triangles to out (if not NULL)
 * in Quake II RTX's winding, the opposite of GL's, like vk_world.c's.
 * Returns the number of triangles, -1 if the list doesn't match the
 * poses. */
static int AliasTriangles (const aliashdr_t *hdr, AliasTriangle *out)
{
	const int	*order = (const int *)((const byte *)hdr + hdr->commands);
	const float	*st;
	int		count, base = 0, num_tris = 0, k, i, c[3];
	qboolean	fan;

	while ((count = *order++) != 0)
	{
		fan = (count < 0);
		if (fan)
			count = -count;
		st = (const float *) order;
		for (k = 0; k + 2 < count; k++)
		{
			if (fan)
			{
				c[0] = 0;	c[1] = k + 1;	c[2] = k + 2;
			}
			else if (k & 1)		/* GL's strip order keeps the winding */
			{
				c[0] = k + 1;	c[1] = k;	c[2] = k + 2;
			}
			else
			{
				c[0] = k;	c[1] = k + 1;	c[2] = k + 2;
			}
			if (out)
			{
				int	corner[3];
				float	*uv[3];

				corner[0] = c[0];	corner[1] = c[2];	corner[2] = c[1];	/* reversed */
				uv[0] = out->uv0;	uv[1] = out->uv1;	uv[2] = out->uv2;
				for (i = 0; i < 3; i++)
				{
					uv[i][0] = st[corner[i] * 2 + 0];
					uv[i][1] = st[corner[i] * 2 + 1];
				}
				out->verts[0] = (uint32_t)(base + corner[0]) | ((uint32_t)(base + corner[1]) << 16);
				out->verts[1] = (uint32_t)(base + corner[2]);
				out++;
			}
			num_tris++;
		}
		order += count * 2;
		base += count;
	}
	return (base == hdr->poseverts) ? num_tris : -1;
}

static void WriteModelTable (int index)
{
	const vk_aliasmodel_t	*am = &alias_models[index];
	const aliashdr_t	*hdr = (const aliashdr_t *) Mod_Extradata (am->model);
	AliasModel		*e = (AliasModel *) model_table.mapped + index;
	VkDeviceSize		triangles_size = (VkDeviceSize)am->num_tris * sizeof(AliasTriangle);

	memset (e, 0, sizeof(*e));
	VectorCopy (hdr->scale, e->scale);
	VectorCopy (hdr->scale_origin, e->scale_origin);
	e->num_tris = (uint32_t)am->num_tris;
	e->num_pose_verts = (uint32_t)am->num_pose_verts;
	e->triangles = am->buffer.address;
	e->poses = am->buffer.address + triangles_size;
	VK_CHECK (vmaFlushAllocation (vk.allocator, model_table.allocation, index * sizeof(AliasModel), sizeof(AliasModel)));
}

/* builds a model's GPU data; returns its index, or -1 without room */
static int BuildAliasModel (qmodel_t *model)
{
	vk_aliasmodel_t	*am;
	aliashdr_t	*hdr;
	byte		*data;
	size_t		triangles_size, poses_size;
	int		index, num_tris;
	unsigned	h;

	if (num_alias_models >= MAX_ALIAS_MODELS)
	{
		too_many_models++;
		return -1;
	}
	index = num_alias_models++;
	am = &alias_models[index];
	memset (am, 0, sizeof(*am));
	am->model = model;
	for (h = ModelHash (model); model_hash[h]; h = (h + 1) & (MODEL_HASH_SIZE - 1))
		;
	model_hash[h] = (short)(index + 1);

	hdr = (aliashdr_t *) Mod_Extradata (model);
	num_tris = AliasTriangles (hdr, NULL);
	if (num_tris <= 0 || hdr->numposes <= 0 || hdr->poseverts <= 0 || hdr->poseverts > 0xffff)
		return index;	/* nothing to draw: num_tris 0 */

	/* [AliasTriangle x num_tris][trivertx_t x numposes x poseverts] */
	triangles_size = num_tris * sizeof(AliasTriangle);
	poses_size = (size_t)hdr->numposes * hdr->poseverts * sizeof(trivertx_t);
	data = (byte *) malloc (triangles_size + poses_size);
	if (!data)
		Sys_Error ("%s: out of memory", __thisfunc__);
	AliasTriangles (hdr, (AliasTriangle *) data);
	memcpy (data + triangles_size, (byte *)hdr + hdr->posedata, poses_size);
	VK_CreateBuffer (&am->buffer, triangles_size + poses_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
			 VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VK_MEMORY_DEVICE);
	VK_UploadBuffer (&am->buffer, 0, data, triangles_size + poses_size);
	free (data);

	am->num_tris = num_tris;
	am->num_pose_verts = hdr->poseverts;
	am->num_poses = hdr->numposes;
	am->num_skins = hdr->numskins;
	if (!loading_models)
		built_later++;	/* its skins' materials are made when drawn */

	WriteModelTable (index);
	return index;
}

int VK_AliasModelIndex (qmodel_t *model)
{
	unsigned	h;
	int		index;

	if (!model || model->type != mod_alias)
		return -1;
	for (h = ModelHash (model); model_hash[h]; h = (h + 1) & (MODEL_HASH_SIZE - 1))
	{
		index = model_hash[h] - 1;
		if (alias_models[index].model == model)
			return alias_models[index].num_tris ? index : -1;
	}
	index = BuildAliasModel (model);
	return (index >= 0 && alias_models[index].num_tris) ? index : -1;
}

const vk_aliasmodel_t *VK_GetAliasModel (int index)
{
	return &alias_models[index];
}

static void FreeModels (void)
{
	int	i;

	for (i = 0; i < num_alias_models; i++)
		VK_DestroyBuffer (&alias_models[i].buffer);
	memset (alias_models, 0, sizeof(alias_models[0]) * num_alias_models);
	memset (model_hash, 0, sizeof(model_hash));
	num_alias_models = 0;
	too_many_models = 0;
	built_later = 0;
	last_pass.slot = -1;
}

/* on map change, after VK_LoadWorld (which cleared the materials): the
 * precached alias models and their skins' materials */
void VK_LoadModels (void)
{
	int	i, first_material = vk_num_materials;

	vkDeviceWaitIdle (vk.device);	/* frames in flight may still use the old buffers */
	FreeModels ();
	VK_ClearSkins ();

	loading_models = true;
	for (i = 1; i < MAX_MODELS && cl.model_precache[i]; i++)
	{
		if (cl.model_precache[i]->type == mod_alias && VK_AliasModelIndex (cl.model_precache[i]) >= 0)
			VK_AddSkinMaterials (cl.model_precache[i]);
	}
	loading_models = false;
	if (vk_num_materials > first_material)
		VK_UploadMaterialRange (first_material, vk_num_materials - first_material);
}


/* ==========================================================================
 * The frame's model triangles
 * ========================================================================== */

const vk_buffer_t *VK_InstancedBuffer (void)
{
	return &instanced[vk.frame_index];
}

VkDeviceAddress VK_InstancedPositionsAddress (void)
{
	return instanced[vk.frame_index].address + POSITIONS_OFFSET;
}

qboolean VK_ModelGeometryBuiltThisFrame (void)
{
	return vk.frame_active && last_pass.slot == (int)vk.frame_index && last_pass.frame_count == vk.frame_count;
}

/* in R_RenderView, after VK_UpdateInstances */
void VK_UpdateModelGeometry (void)
{
	const vk_modelframe_t	*mf = VK_ModelFrame ();
	int			slot = (int)vk.frame_index;
	VkCommandBuffer		cmd;
	ModelGeometryPush	push;
	VkMemoryBarrier2	barrier;
	VkDependencyInfo	dep;

	if (!vk.frame_active)
		return;

	/* this slot's fence was waited for, so its last pass's timestamps are ready */
	if (query_pool && timed[slot])
	{
		uint64_t	ts[2];

		if (vkGetQueryPoolResults (vk.device, query_pool, slot * 2, 2, sizeof(ts), ts, sizeof(uint64_t),
					   VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
			geometry_ms = (double)(ts[1] - ts[0]) * vk.props.limits.timestampPeriod / 1.0e6;
		timed[slot] = false;
	}

	if (!mf->num_instances)
		return;

	cmd = vk.frames[slot].cmd;
	memset (&push, 0, sizeof(push));
	push.instances = VK_InstanceBuffer ()->address;
	push.models = model_table.address;
	push.normals = normal_buffer.address;
	push.primitives = instanced[slot].address;
	push.positions = instanced[slot].address + POSITIONS_OFFSET;
	push.first_instance = (uint32_t)mf->first_instance;

	if (query_pool)
	{
		vkCmdResetQueryPool (cmd, query_pool, slot * 2, 2);
		vkCmdWriteTimestamp2 (cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, query_pool, slot * 2);
	}
	vkCmdBindPipeline (cmd, VK_PIPELINE_BIND_POINT_COMPUTE, geometry_pipeline);
	vkCmdPushConstants (cmd, geometry_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
	vkCmdDispatch (cmd, (uint32_t)mf->num_instances, 1, 1);

	/* for the BLAS builds and the view passes */
	memset (&barrier, 0, sizeof(barrier));
	barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
	barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
	barrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
	barrier.dstStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
	barrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
	memset (&dep, 0, sizeof(dep));
	dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
	dep.memoryBarrierCount = 1;
	dep.pMemoryBarriers = &barrier;
	vkCmdPipelineBarrier2 (cmd, &dep);

	if (query_pool)
	{
		vkCmdWriteTimestamp2 (cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, query_pool, slot * 2 + 1);
		timed[slot] = true;
	}

	last_pass.slot = slot;
	last_pass.framecount = r_scene.framecount;
	last_pass.frame_count = vk.frame_count;
	last_pass.frame = *mf;
}


/* ==========================================================================
 * vk_models, and the check against the CPU
 * ========================================================================== */

static float HalfToFloat (uint16_t h)
{
	union { float f; uint32_t u; } v;
	uint32_t	sign = (uint32_t)(h & 0x8000) << 16, exp = (h >> 10) & 0x1f, mant = h & 0x3ff;

	if (exp == 0)			/* zero, subnormal */
	{
		v.f = mant * (1.0f / 16777216.0f);	/* 2^-24 */
		v.u |= sign;
		return v.f;
	}
	if (exp == 31)			/* inf, nan */
		v.u = sign | 0x7f800000 | (mant << 13);
	else
		v.u = sign | ((exp + 112) << 23) | (mant << 13);
	return v.f;
}

/* utils.glsl's decode_normal */
static void DecodeNormal (uint32_t enc, vec3_t n)
{
	float	px = (enc & 0xffff) / 65535.0f * 2.0f - 1.0f;
	float	py = (enc >> 16) / 65535.0f * 2.0f - 1.0f;
	float	t;

	n[0] = px;
	n[1] = py;
	n[2] = 1.0f - fabsf (px) - fabsf (py);
	t = q_max (0.0f, -n[2]);
	n[0] += (n[0] >= 0.0f) ? -t : t;
	n[1] += (n[1] >= 0.0f) ? -t : t;
	VectorNormalize (n);
}

static void MixVec (const vec3_t a, const vec3_t b, float t, vec3_t out)
{
	int	i;

	for (i = 0; i < 3; i++)
		out[i] = a[i] * (1.0f - t) + b[i] * t;	/* GLSL's mix */
}

static void PosePosition (const aliashdr_t *hdr, const trivertx_t *v, vec3_t out)
{
	int	i;

	for (i = 0; i < 3; i++)
		out[i] = v->v[i] * hdr->scale[i] + hdr->scale_origin[i];
}

static const float *PoseNormal (const trivertx_t *v)
{
	return vertex_normals[q_min (v->lightnormalindex, NUM_VERTEX_NORMALS - 1)];
}

static void TransformPoint4 (const mat4 m, const vec3_t in, vec3_t out)
{
	int	r;

	for (r = 0; r < 3; r++)
		out[r] = m[0][r] * in[0] + m[1][r] * in[1] + m[2][r] * in[2] + m[3][r];
}

/* transpose(inverse(mat3(m))), as the shader computes it: cofactors / det */
static void NormalMatrix (const mat4 m, float nm[3][3])
{
	float	a[3][3], det;
	int	r, c;

	for (r = 0; r < 3; r++)
	{
		for (c = 0; c < 3; c++)
			a[r][c] = m[c][r];
	}
	nm[0][0] = a[1][1] * a[2][2] - a[1][2] * a[2][1];
	nm[0][1] = a[1][2] * a[2][0] - a[1][0] * a[2][2];
	nm[0][2] = a[1][0] * a[2][1] - a[1][1] * a[2][0];
	nm[1][0] = a[0][2] * a[2][1] - a[0][1] * a[2][2];
	nm[1][1] = a[0][0] * a[2][2] - a[0][2] * a[2][0];
	nm[1][2] = a[0][1] * a[2][0] - a[0][0] * a[2][1];
	nm[2][0] = a[0][1] * a[1][2] - a[0][2] * a[1][1];
	nm[2][1] = a[0][2] * a[1][0] - a[0][0] * a[1][2];
	nm[2][2] = a[0][0] * a[1][1] - a[0][1] * a[1][0];
	det = a[0][0] * nm[0][0] + a[0][1] * nm[0][1] + a[0][2] * nm[0][2];
	for (r = 0; r < 3; r++)
	{
		for (c = 0; c < 3; c++)
			nm[r][c] /= det;	/* [row][column] */
	}
}

typedef struct
{
	vec3_t		pos[3], pos_prev[3], nrm[3], tangent[3];
	qboolean	flip;
	qboolean	unsure_tangent[3];	/* near the shader's fallback threshold */
	qboolean	unsure_flip;		/* texture axes almost parallel to the face */
	float		facing;			/* face normal . average vertex normal */
} cpu_triangle_t;

/* model_geometry.comp's computation of one triangle */
static void CpuTriangle (const ModelInstance *mi, const aliashdr_t *hdr, const AliasTriangle *tri,
			 const float nm[3][3], cpu_triangle_t *out)
{
	const trivertx_t	*poses = (const trivertx_t *)((const byte *)hdr + hdr->posedata);
	const uint32_t		verts[3] = { tri->verts[0] & 0xffff, tri->verts[0] >> 16, tri->verts[1] };
	vec3_t			e1, e2, face, tangent, bitangent, c, pa, pb, n, avg;
	float			d1[2], d2[2], det_sign, flip_dot, len;
	int			k, i;

	VectorClear (avg);
	for (k = 0; k < 3; k++)
	{
		const trivertx_t	*a = poses + mi->prim_offset_curr_pose_curr_frame + verts[k];
		const trivertx_t	*b = poses + mi->prim_offset_prev_pose_curr_frame + verts[k];
		const trivertx_t	*ppa = poses + mi->prim_offset_curr_pose_prev_frame + verts[k];
		const trivertx_t	*ppb = poses + mi->prim_offset_prev_pose_prev_frame + verts[k];

		PosePosition (hdr, a, pa);
		PosePosition (hdr, b, pb);
		MixVec (pa, pb, mi->pose_lerp_curr_frame, c);
		TransformPoint4 (mi->transform, c, out->pos[k]);
		PosePosition (hdr, ppa, pa);
		PosePosition (hdr, ppb, pb);
		MixVec (pa, pb, mi->pose_lerp_prev_frame, c);
		TransformPoint4 (mi->transform_prev, c, out->pos_prev[k]);

		MixVec (PoseNormal (a), PoseNormal (b), mi->pose_lerp_curr_frame, n);
		if (DotProduct (n, n) < 1e-12f)
			VectorCopy (PoseNormal (a), n);
		for (i = 0; i < 3; i++)
			out->nrm[k][i] = nm[i][0] * n[0] + nm[i][1] * n[1] + nm[i][2] * n[2];
		VectorNormalize (out->nrm[k]);
		VectorAdd (avg, out->nrm[k], avg);
	}

	VectorSubtract (out->pos[1], out->pos[0], e1);
	VectorSubtract (out->pos[2], out->pos[0], e2);
	d1[0] = tri->uv1[0] - tri->uv0[0];	d1[1] = tri->uv1[1] - tri->uv0[1];
	d2[0] = tri->uv2[0] - tri->uv0[0];	d2[1] = tri->uv2[1] - tri->uv0[1];
	det_sign = (d1[0] * d2[1] - d2[0] * d1[1] < 0.0f) ? -1.0f : 1.0f;
	for (i = 0; i < 3; i++)
	{
		tangent[i] = (e1[i] * d2[1] - e2[i] * d1[1]) * det_sign;
		bitangent[i] = (e2[i] * d1[0] - e1[i] * d2[0]) * det_sign;
	}
	CrossProduct (e1, e2, face);
	CrossProduct (face, tangent, c);
	flip_dot = DotProduct (c, bitangent);
	out->flip = flip_dot < 0.0f;
	out->unsure_flip = fabsf (flip_dot) <= 1e-3f * VectorLength (face) * VectorLength (tangent) * VectorLength (bitangent);
	VectorNormalize (face);
	VectorNormalize (avg);
	out->facing = DotProduct (face, avg);

	for (k = 0; k < 3; k++)
	{
		VectorMA (tangent, -DotProduct (out->nrm[k], tangent), out->nrm[k], out->tangent[k]);
		len = DotProduct (out->tangent[k], out->tangent[k]);
		/* near the shader's 1e-12 fallback threshold, or almost along
		 * the normal (the direction is then rounding noise) */
		out->unsure_tangent[k] = (len > 1e-13f && len < 1e-11f) ||
					 (len >= 1e-11f && len < 1e-6f * DotProduct (tangent, tangent));
		if (len > 1e-12f)
			VectorNormalize (out->tangent[k]);
		else
		{	/* any direction in the plane */
			vec3_t	axis = { 0.0f, 0.0f, 0.0f };

			axis[(fabsf (out->nrm[k][2]) < 0.9f) ? 2 : 0] = 1.0f;
			CrossProduct (axis, out->nrm[k], out->tangent[k]);
			VectorNormalize (out->tangent[k]);
		}
	}
}

static void VK_ModelsCheck (void)
{
	const vk_modelframe_t	*mf = &last_pass.frame;
	uint32_t		total = mf->groups[NUM_MODEL_GROUPS - 1].first + mf->groups[NUM_MODEL_GROUPS - 1].count;
	vk_buffer_t		readback;
	VkCommandBuffer		cmd;
	VkBufferCopy		copy[3];
	VkDeviceSize		materials_size;
	const uint32_t		*gpu_materials;
	VkMemoryBarrier2	barrier;
	VkDependencyInfo	dep;
	const VboPrimitive	*gpu;
	const float		*gpu_pos;
	AliasTriangle		*tris;
	int			*inward_per_model;	/* triangles facing against their vertex normals */
	int			i, k, differences, max_tris = 1, bad_instances = 0, checked = 0, unsure = 0;
	int			inward = 0, shown = 0, pos_bad = 0, nrm_bad = 0, tan_bad = 0, flip_bad = 0;
	int			uv_bad = 0, motion_bad = 0, field_bad = 0, positions_bad = 0, group_bad = 0, table_bad = 0;
	float			max_pos = 0.0f, min_nrm = 1.0f, min_tan = 1.0f, max_motion = 0.0f;

	if (last_pass.slot < 0 || last_pass.framecount != r_scene.framecount || !total)
	{
		Con_Printf ("No model triangles in the last frame\n");
		return;
	}

	vkDeviceWaitIdle (vk.device);	/* the last frame's pass has run */
	/* the triangles and their BLAS positions, and the material table */
	materials_size = (VkDeviceSize)vk_num_materials * MATERIAL_UINTS * sizeof(uint32_t);
	VK_CreateBuffer (&readback, (VkDeviceSize)total * (sizeof(VboPrimitive) + 9 * sizeof(float)) + materials_size,
			 VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_READBACK);
	memset (copy, 0, sizeof(copy));
	copy[0].size = (VkDeviceSize)total * sizeof(VboPrimitive);
	copy[1].srcOffset = POSITIONS_OFFSET;
	copy[1].dstOffset = copy[0].size;
	copy[1].size = (VkDeviceSize)total * 9 * sizeof(float);
	cmd = VK_BeginUpload ();
	vkCmdCopyBuffer (cmd, instanced[last_pass.slot].buffer, readback.buffer, 2, copy);
	copy[2].srcOffset = 0;
	copy[2].dstOffset = copy[1].dstOffset + copy[1].size;
	copy[2].size = materials_size;
	vkCmdCopyBuffer (cmd, vk_material_table.buffer, readback.buffer, 1, &copy[2]);
	memset (&barrier, 0, sizeof(barrier));
	barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
	barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
	barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
	barrier.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
	barrier.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
	memset (&dep, 0, sizeof(dep));
	dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
	dep.memoryBarrierCount = 1;
	dep.pMemoryBarriers = &barrier;
	vkCmdPipelineBarrier2 (cmd, &dep);
	VK_EndUpload ();
	VK_CHECK (vmaInvalidateAllocation (vk.allocator, readback.allocation, 0, VK_WHOLE_SIZE));
	gpu = (const VboPrimitive *) readback.mapped;
	gpu_pos = (const float *)((const byte *) readback.mapped + copy[0].size);
	gpu_materials = (const uint32_t *)((const byte *) readback.mapped + copy[2].dstOffset);

	for (i = 0; i < num_alias_models; i++)
		max_tris = q_max (max_tris, alias_models[i].num_tris);
	tris = (AliasTriangle *) malloc (max_tris * sizeof(AliasTriangle));
	inward_per_model = (int *) calloc (q_max (num_alias_models, 1) * 2, sizeof(int));	/* inward, triangles checked */
	if (!tris || !inward_per_model)
		Sys_Error ("%s: out of memory", __thisfunc__);

	for (i = mf->first_instance; i < mf->first_instance + mf->num_instances; i++)
	{
		const ModelInstance	*mi = VK_GetInstance (i);
		int			index = (int)mi->source_buffer_idx - VERTEX_BUFFER_FIRST_MODEL;
		const vk_aliasmodel_t	*am;
		const aliashdr_t	*hdr;
		float			nm[3][3], alpha = HalfToFloat ((uint16_t)(mi->alpha_and_frame & 0xffff));
		int			num_tris, max_pose;

		if (index < 0 || index >= num_alias_models)
		{
			bad_instances++;
			continue;
		}
		am = &alias_models[index];
		hdr = (const aliashdr_t *) Mod_Extradata (am->model);
		num_tris = AliasTriangles (hdr, NULL);
		max_pose = (hdr->numposes - 1) * hdr->poseverts;
		if (num_tris != am->num_tris || (int)mi->prim_count != num_tris ||
		    mi->render_prim_offset + mi->prim_count > total ||
		    (int)mi->prim_offset_curr_pose_curr_frame > max_pose || (int)mi->prim_offset_prev_pose_curr_frame > max_pose ||
		    (int)mi->prim_offset_curr_pose_prev_frame > max_pose || (int)mi->prim_offset_prev_pose_prev_frame > max_pose)
		{
			bad_instances++;
			continue;
		}
		AliasTriangles (hdr, tris);
		NormalMatrix (mi->transform, nm);
		inward_per_model[index * 2 + 1] += num_tris;

		/* the group its triangles are in against its material: transparent
		 * ones are Q2RTX's transparent models, masked ones have the skin
		 * as their cutout mask, the others neither; and the material as
		 * the GPU's table has it (uploaded on map load or mid-frame) */
		{
			int			m = (int)(mi->material & MATERIAL_INDEX_MASK);
			const vk_material_t	*mat = VK_GetMaterial (m);
			const uint32_t		*gm = gpu_materials + m * MATERIAL_UINTS;
			uint32_t		kind = mi->material & MATERIAL_KIND_MASK;
			int			g;

			for (g = 0; g < NUM_MODEL_GROUPS; g++)
			{
				if (mi->render_prim_offset >= mf->groups[g].first &&
				    mi->render_prim_offset + mi->prim_count <= mf->groups[g].first + mf->groups[g].count)
					break;
			}
			if (g == NUM_MODEL_GROUPS ||
			    kind != ((g == MODEL_GROUP_TRANSPARENT) ? MATERIAL_KIND_TRANSP_MODEL : MATERIAL_KIND_REGULAR) ||
			    (g == MODEL_GROUP_MASKED && mat->mask_texture != mat->base_texture) ||
			    (g == MODEL_GROUP_OPAQUE && mat->mask_texture))
				group_bad++;
			if ((int)(gm[0] & 0xffff) != mat->base_texture || (int)(gm[1] >> 16) != mat->mask_texture)
				table_bad++;
		}

		for (k = 0; k < num_tris; k++)
		{
			const VboPrimitive	*g = &gpu[mi->render_prim_offset + k];
			const float		*gp = &gpu_pos[(mi->render_prim_offset + k) * 9];
			const float		*gpu_p[3] = { g->pos0, g->pos1, g->pos2 };
			const uint32_t		gpu_motion[3][2] = { { g->custom0[0], g->custom0[1] },
							     { g->custom1[0], g->custom1[1] },
							     { g->custom2[0], g->custom2[1] } };
			const float		*gpu_uv[3] = { g->uv0, g->uv1, g->uv2 };
			const float		*cpu_uv[3] = { tris[k].uv0, tris[k].uv1, tris[k].uv2 };
			cpu_triangle_t		c;
			uint32_t		expect_emissive = VK_FloatToHalf (1.0f) | ((uint32_t)VK_FloatToHalf (alpha) << 16);
			qboolean		bad = false;
			int			v, j;

			CpuTriangle (mi, hdr, &tris[k], nm, &c);
			checked++;
			if (c.facing < 0.0f)
			{
				inward++;
				inward_per_model[index * 2]++;
			}

			for (v = 0; v < 3; v++)
			{
				vec3_t	n, t, motion;
				float	d;

				for (j = 0; j < 3; j++)
				{
					d = fabsf (gpu_p[v][j] - c.pos[v][j]);
					max_pos = q_max (max_pos, d);
					if (d > 0.01f)
					{
						pos_bad++;
						bad = true;
					}
					if (gp[v * 3 + j] != gpu_p[v][j])
					{
						positions_bad++;
						bad = true;
					}
				}

				DecodeNormal (g->normals[v], n);
				d = DotProduct (n, c.nrm[v]);
				min_nrm = q_min (min_nrm, d);
				if (d < 0.9995f)
				{
					nrm_bad++;
					bad = true;
				}

				if (c.unsure_tangent[v])
					unsure++;
				else
				{
					DecodeNormal (g->tangents[v], t);
					d = DotProduct (t, c.tangent[v]);
					min_tan = q_min (min_tan, d);
					if (d < 0.999f)
					{
						tan_bad++;
						bad = true;
					}
				}

				if (gpu_uv[v][0] != cpu_uv[v][0] || gpu_uv[v][1] != cpu_uv[v][1])
				{
					uv_bad++;
					bad = true;
				}

				motion[0] = HalfToFloat ((uint16_t)(gpu_motion[v][0] & 0xffff));
				motion[1] = HalfToFloat ((uint16_t)(gpu_motion[v][0] >> 16));
				motion[2] = HalfToFloat ((uint16_t)(gpu_motion[v][1] & 0xffff));
				for (j = 0; j < 3; j++)
				{
					float	expect = c.pos_prev[v][j] - c.pos[v][j];

					d = fabsf (motion[j] - expect);
					max_motion = q_max (max_motion, d);
					if (d > 0.01f + 2e-3f * fabsf (expect))	/* half precision */
					{
						motion_bad++;
						bad = true;
					}
				}
			}
			if (!c.unsure_flip && ((g->material_id & MATERIAL_FLAG_HANDEDNESS) != 0) != c.flip)
			{
				flip_bad++;
				bad = true;
			}
			if ((g->material_id & ~MATERIAL_FLAG_HANDEDNESS) != mi->material || g->cluster != mi->cluster ||
			    g->instance != (uint32_t)i || g->shell != mi->shell || g->emissive_and_alpha != expect_emissive)
			{
				field_bad++;
				bad = true;
			}

			if (bad && shown++ < 3)
			{
				Con_Printf ("instance %d (%s) triangle %d differs: GPU pos0 %.3f %.3f %.3f, CPU %.3f %.3f %.3f\n",
						i, am->model->name, k, g->pos0[0], g->pos0[1], g->pos0[2],
						c.pos[0][0], c.pos[0][1], c.pos[0][2]);
			}
		}
	}
	free (tris);
	VK_DestroyBuffer (&readback);

	differences = pos_bad + nrm_bad + tan_bad + flip_bad + uv_bad + motion_bad + field_bad + positions_bad + bad_instances +
		      group_bad + table_bad;
	Con_Printf ("models check: %d triangles of %d instances (frame %d): %s\n", checked, mf->num_instances,
			last_pass.framecount, differences ? "DIFFERENCES" : "all agree");
	Con_Printf ("  positions: max difference %.5f, %d over 0.01; BLAS positions %d differ\n", max_pos, pos_bad, positions_bad);
	Con_Printf ("  normals: min dot %.5f, %d under 0.9995; tangents: min dot %.5f, %d under 0.999 (%d near the fallback not compared)\n",
			min_nrm, nrm_bad, min_tan, tan_bad, unsure);
	Con_Printf ("  motion: max difference %.5f, %d over tolerance; uvs %d, handedness %d, material/cluster/instance/alpha %d differ\n",
			max_motion, motion_bad, uv_bad, flip_bad, field_bad);
	Con_Printf ("  %d instances with bad offsets, %d whose material doesn't fit their group (kind, cutout mask), "
		    "%d whose material differs in the GPU's table (texture, mask)\n", bad_instances, group_bad, table_bad);
	Con_Printf ("  %d triangles face against their vertex normals%s\n", inward, inward ? ", in:" : "");
	for (i = 0; i < num_alias_models; i++)
	{
		if (inward_per_model[i * 2])
		{
			Con_Printf ("    %s: %d of %d\n", alias_models[i].model->name, inward_per_model[i * 2],
					inward_per_model[i * 2 + 1]);
		}
	}
	free (inward_per_model);
}

static void VK_Models_f (void)
{
	const vk_modelframe_t	*mf = VK_ModelFrame ();
	VkDeviceSize		bytes = 0;
	int			i, tris = 0, poses = 0;

	if (Cmd_Argc () > 1 && !q_strcasecmp (Cmd_Argv (1), "check"))
	{
		if (cls.signon != SIGNONS || !r_scene.worldmodel)
			Con_Printf ("Not in a map\n");
		else
			VK_ModelsCheck ();
		return;
	}

	for (i = 0; i < num_alias_models; i++)
	{
		const vk_aliasmodel_t	*am = &alias_models[i];

		tris += am->num_tris;
		poses += am->num_poses;
		bytes += am->buffer.size;
		if (Cmd_Argc () > 1 && !q_strcasecmp (Cmd_Argv (1), "list"))
		{
			Con_Printf ("%3d %-28s %5d tris %5d verts %4d poses %7.1f KB %2d skins%s\n", i, am->model->name,
					am->num_tris, am->num_pose_verts, am->num_poses, am->buffer.size / 1024.0,
					am->num_skins, VK_ModelHasCutouts (am->model) ? " cutout" : "");
		}
	}
	Con_Printf ("%d alias models on the GPU (%d built after the map loaded): %d triangles, %d poses, %.2f MB%s\n",
			num_alias_models, built_later, tris, poses, bytes / (1024.0 * 1024.0),
			too_many_models ? va(" (%d found no room)", too_many_models) : "");
	Con_Printf ("instanced buffer: %d triangles per frame in flight, %.1f MB each\n", MAX_INSTANCED_PRIMITIVES,
			instanced[0].size / (1024.0 * 1024.0));
	Con_Printf ("last frame: %d alias instances, %u opaque + %u transparent + %u masked triangles, geometry pass %.3f ms on the GPU\n",
			mf->num_instances, mf->groups[MODEL_GROUP_OPAQUE].count, mf->groups[MODEL_GROUP_TRANSPARENT].count,
			mf->groups[MODEL_GROUP_MASKED].count, geometry_ms);
	Con_Printf ("left out: %d instances this frame, %d since the map loaded (no room); bad frame numbers: %d, bad skin numbers: %d this frame\n",
			mf->dropped, mf->dropped_total, mf->bad_frames, mf->bad_skins);
}


/* ==========================================================================
 * Init / shutdown
 * ========================================================================== */

void VK_InitModels (void)
{
	VkPushConstantRange		push_range;
	VkPipelineLayoutCreateInfo	layout_info;
	VkComputePipelineCreateInfo	pipe_info;
	VkQueryPoolCreateInfo		query_info;
	VkShaderModule			module;
	float				normals[NUM_VERTEX_NORMALS][4];
	int				i;

	VK_CreateBuffer (&model_table, MAX_ALIAS_MODELS * sizeof(AliasModel),
			 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VK_MEMORY_UPLOAD);

	for (i = 0; i < NUM_VERTEX_NORMALS; i++)
	{
		VectorCopy (vertex_normals[i], normals[i]);
		normals[i][3] = 0.0f;
	}
	VK_CreateBuffer (&normal_buffer, sizeof(normals), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
			 VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VK_MEMORY_DEVICE);
	VK_UploadBuffer (&normal_buffer, 0, normals, sizeof(normals));

	for (i = 0; i < VK_FRAMES_IN_FLIGHT; i++)
	{
		VK_CreateBuffer (&instanced[i], POSITIONS_OFFSET + (VkDeviceSize)MAX_INSTANCED_PRIMITIVES * 9 * sizeof(float),
				 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
				 VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
				 VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_DEVICE);
	}

	memset (&push_range, 0, sizeof(push_range));
	push_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	push_range.size = sizeof(ModelGeometryPush);
	memset (&layout_info, 0, sizeof(layout_info));
	layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	layout_info.pushConstantRangeCount = 1;
	layout_info.pPushConstantRanges = &push_range;
	VK_CHECK (vkCreatePipelineLayout (vk.device, &layout_info, NULL, &geometry_layout));

	module = VK_LoadShader ("model_geometry.comp");
	memset (&pipe_info, 0, sizeof(pipe_info));
	pipe_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
	pipe_info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	pipe_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	pipe_info.stage.module = module;
	pipe_info.stage.pName = "main";
	pipe_info.layout = geometry_layout;
	VK_CHECK (vkCreateComputePipelines (vk.device, VK_NULL_HANDLE, 1, &pipe_info, NULL, &geometry_pipeline));
	vkDestroyShaderModule (vk.device, module, NULL);

	if (vk.props.limits.timestampComputeAndGraphics)
	{
		memset (&query_info, 0, sizeof(query_info));
		query_info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
		query_info.queryType = VK_QUERY_TYPE_TIMESTAMP;
		query_info.queryCount = VK_FRAMES_IN_FLIGHT * 2;
		VK_CHECK (vkCreateQueryPool (vk.device, &query_info, NULL, &query_pool));
	}

	Cmd_AddCommand ("vk_models", VK_Models_f);
}

void VK_ShutdownModels (void)
{
	int	i;

	FreeModels ();
	for (i = 0; i < VK_FRAMES_IN_FLIGHT; i++)
		VK_DestroyBuffer (&instanced[i]);
	VK_DestroyBuffer (&normal_buffer);
	VK_DestroyBuffer (&model_table);
	if (geometry_pipeline)
		vkDestroyPipeline (vk.device, geometry_pipeline, NULL);
	if (geometry_layout)
		vkDestroyPipelineLayout (vk.device, geometry_layout, NULL);
	if (query_pool)
		vkDestroyQueryPool (vk.device, query_pool, NULL);
	geometry_pipeline = VK_NULL_HANDLE;
	geometry_layout = VK_NULL_HANDLE;
	query_pool = VK_NULL_HANDLE;
}
