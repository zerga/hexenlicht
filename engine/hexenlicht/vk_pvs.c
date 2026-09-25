/* vk_pvs.c -- the world's potentially visible sets, for the CPU and shaders
 *
 * On map load the world's vis data is decompressed into one bit matrix
 * with a row per cluster (vis leaf). Like Quake II RTX, the sets are then
 * connected across transparent surfaces (water, slime, translucent), whose
 * two sides the vis compiler may have treated as opaque, so that light
 * and refraction through them are not culled. Last, the sets are made
 * symmetric: Hexen II's vis sometimes lets A see B but not B see A, and
 * visibility is only used for culling, where the union is the safe side.
 * The result is uploaded for shaders/pvs.glsl.
 *
 * VK_ConnectPVSAcross is ported from Quake II RTX's bsp_mesh.c
 * (connect_pvs).
 *
 * Copyright (C) 2018 Christoph Schied
 * Copyright (C) 2019, NVIDIA CORPORATION. All rights reserved.
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

vk_pvs_t	vk_pvs;

#define PVS_ROW(c)		(vk_pvs.matrix + (size_t)(c) * vk_pvs.row_bytes)
#define PVS_TEST(row, c)	(((row)[(c) >> 3] >> ((c) & 7)) & 1)
#define PVS_SET(row, c)		((row)[(c) >> 3] |= (byte)(1 << ((c) & 7)))


void VK_FreePVS (void)
{
	if (vk.device)
		VK_DestroyBuffer (&vk_pvs.buffer);
	free (vk_pvs.matrix);
	memset (&vk_pvs, 0, sizeof(vk_pvs));
}

void VK_BuildPVS (qmodel_t *worldmodel)
{
	int	n = worldmodel->numleafs;	/* the world's vis leaves, not counting leaf 0 */
	int	src_bytes = (n + 7) >> 3;
	int	c;

	VK_FreePVS ();
	if (n <= 0)
		return;

	vk_pvs.num_clusters = n;
	vk_pvs.row_bytes = ((n + 31) >> 5) * 4;
	vk_pvs.matrix = (byte *) calloc (n, vk_pvs.row_bytes);
	if (!vk_pvs.matrix)
		Sys_Error ("%s: out of memory", __thisfunc__);

	for (c = 0; c < n; c++)
	{
		byte	*row = PVS_ROW(c);

		/* leaves without vis data see everything */
		memcpy (row, Mod_LeafPVS (&worldmodel->leafs[c + 1], worldmodel), src_bytes);
		if (n & 7)
			row[src_bytes - 1] &= (byte)((1 << (n & 7)) - 1);	/* no bits past the last cluster */
	}
}

static void MergeRows (const byte *src, byte *dst)
{
	int	i;

	for (i = 0; i < vk_pvs.row_bytes; i++)
		dst[i] |= src[i];
}

/* Quake II RTX's connect_pvs: whatever sees one side of the surface also
 * sees what the other side sees */
void VK_ConnectPVSAcross (int a, int b)
{
	byte	*pa, *pb;
	int	c;

	if (a < 0 || b < 0 || a == b || a >= vk_pvs.num_clusters || b >= vk_pvs.num_clusters)
		return;
	pa = PVS_ROW(a);
	pb = PVS_ROW(b);
	if (PVS_TEST(pa, b) && PVS_TEST(pb, a))
		return;		/* the vis compiler saw through it already */

	for (c = 0; c < vk_pvs.num_clusters; c++)
	{
		if (c != a && c != b && PVS_TEST(pa, c))
			MergeRows (pb, PVS_ROW(c));
	}
	for (c = 0; c < vk_pvs.num_clusters; c++)
	{
		if (c != a && c != b && PVS_TEST(pb, c))
			MergeRows (pa, PVS_ROW(c));
	}
	MergeRows (pa, pb);
	MergeRows (pb, pa);
	vk_pvs.patched++;
}

void VK_FinishPVS (void)
{
	uint32_t	header[PVS_HEADER_UINTS];
	VkDeviceSize	size;
	int		r, c;

	if (!vk_pvs.num_clusters)
		return;

	/* symmetric: if r sees c, c sees r */
	for (r = 0; r < vk_pvs.num_clusters; r++)
	{
		const byte	*row = PVS_ROW(r);

		for (c = 0; c < vk_pvs.num_clusters; c++)
		{
			if (PVS_TEST(row, c) && !PVS_TEST(PVS_ROW(c), r))
			{
				PVS_SET(PVS_ROW(c), r);
				vk_pvs.one_way_pairs++;
			}
		}
	}

	/* the rows' bytes, little endian, are the shaders' uints */
	memset (header, 0, sizeof(header));
	header[0] = (uint32_t)vk_pvs.num_clusters;
	header[1] = (uint32_t)vk_pvs.row_bytes / 4;
	size = sizeof(header) + (VkDeviceSize)vk_pvs.num_clusters * vk_pvs.row_bytes;
	VK_CreateBuffer (&vk_pvs.buffer, size,
			 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
			 VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VK_MEMORY_DEVICE);
	VK_UploadBuffer (&vk_pvs.buffer, 0, header, sizeof(header));
	VK_UploadBuffer (&vk_pvs.buffer, sizeof(header), vk_pvs.matrix, size - sizeof(header));
}

const byte *VK_ClusterPVS (int cluster)
{
	if (cluster < 0 || cluster >= vk_pvs.num_clusters)
		return NULL;
	return PVS_ROW(cluster);
}

int VK_PointCluster (qmodel_t *worldmodel, const vec3_t point)
{
	vec3_t	p;

	VectorCopy (point, p);	/* Mod_PointInLeaf takes a non-const vector */
	return (int)(Mod_PointInLeaf (p, worldmodel) - worldmodel->leafs) - 1;
}


/* ==========================================================================
 * vk_pvs: statistics and a check of the shader query
 * ========================================================================== */

/* runs pvs_check.comp, which calls pvs_visible for every pair of clusters,
 * and returns the number of rows that differ from the CPU's matrix */
static int CheckShaderQuery (void)
{
	VkPushConstantRange		range;
	VkPipelineLayoutCreateInfo	layout_info;
	VkComputePipelineCreateInfo	pipe_info;
	VkPipelineLayout		layout;
	VkPipeline			pipeline;
	VkShaderModule			module;
	VkCommandBuffer			cmd;
	VkMemoryBarrier2		barrier;
	VkDependencyInfo		dep;
	vk_buffer_t			result;
	VkDeviceAddress			push[2];
	uint32_t			words = (uint32_t)(vk_pvs.num_clusters * vk_pvs.row_bytes / 4);
	int				r, bad_rows = 0;

	module = VK_LoadShader ("pvs_check.comp");
	memset (&range, 0, sizeof(range));
	range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	range.size = sizeof(push);
	memset (&layout_info, 0, sizeof(layout_info));
	layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	layout_info.pushConstantRangeCount = 1;
	layout_info.pPushConstantRanges = &range;
	VK_CHECK (vkCreatePipelineLayout (vk.device, &layout_info, NULL, &layout));

	memset (&pipe_info, 0, sizeof(pipe_info));
	pipe_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
	pipe_info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	pipe_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	pipe_info.stage.module = module;
	pipe_info.stage.pName = "main";
	pipe_info.layout = layout;
	VK_CHECK (vkCreateComputePipelines (vk.device, VK_NULL_HANDLE, 1, &pipe_info, NULL, &pipeline));
	vkDestroyShaderModule (vk.device, module, NULL);

	VK_CreateBuffer (&result, (VkDeviceSize)words * 4,
			 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VK_MEMORY_READBACK);

	cmd = VK_BeginUpload ();
	vkCmdBindPipeline (cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
	push[0] = vk_pvs.buffer.address;
	push[1] = result.address;
	vkCmdPushConstants (cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), push);
	vkCmdDispatch (cmd, (words + 63) / 64, 1, 1);
	memset (&barrier, 0, sizeof(barrier));
	barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
	barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
	barrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
	barrier.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
	barrier.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
	memset (&dep, 0, sizeof(dep));
	dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
	dep.memoryBarrierCount = 1;
	dep.pMemoryBarriers = &barrier;
	vkCmdPipelineBarrier2 (cmd, &dep);
	VK_EndUpload ();

	VK_CHECK (vmaInvalidateAllocation (vk.allocator, result.allocation, 0, VK_WHOLE_SIZE));
	for (r = 0; r < vk_pvs.num_clusters; r++)
	{
		if (memcmp ((const byte *)result.mapped + (size_t)r * vk_pvs.row_bytes, PVS_ROW(r), vk_pvs.row_bytes))
			bad_rows++;
	}

	VK_DestroyBuffer (&result);
	vkDestroyPipeline (vk.device, pipeline, NULL);
	vkDestroyPipelineLayout (vk.device, layout, NULL);
	return bad_rows;
}

static int CountBits (const byte *row)
{
	int	c, n = 0;

	for (c = 0; c < vk_pvs.num_clusters; c++)
		n += PVS_TEST(row, c);
	return n;
}

static void VK_PVS_f (void)
{
	long long	total = 0;
	int		c, view, bad_rows;

	if (!vk_pvs.num_clusters || cls.state != ca_connected)
	{
		Con_Printf ("No world loaded\n");
		return;
	}

	for (c = 0; c < vk_pvs.num_clusters; c++)
		total += CountBits (PVS_ROW(c));
	Con_Printf ("PVS: %d clusters, rows of %d bytes (%.2f MB), each sees %.0f clusters on average\n",
			vk_pvs.num_clusters, vk_pvs.row_bytes,
			(double)vk_pvs.num_clusters * vk_pvs.row_bytes / (1024.0 * 1024.0),
			(double)total / vk_pvs.num_clusters);
	Con_Printf ("%d transparent triangles connected their sides, %d one-way pairs made symmetric\n",
			vk_pvs.patched, vk_pvs.one_way_pairs);

	if (r_scene.viewleaf && r_scene.worldmodel)
	{
		view = (int)(r_scene.viewleaf - r_scene.worldmodel->leafs) - 1;
		if (VK_ClusterPVS (view))
			Con_Printf ("the camera is in cluster %d, which sees %d clusters\n", view, CountBits (VK_ClusterPVS (view)));
		else
			Con_Printf ("the camera is outside the clusters (%d)\n", view);
	}

	bad_rows = CheckShaderQuery ();
	Con_Printf ("shader check: pvs_visible for %d x %d pairs, %d rows differ\n",
			vk_pvs.num_clusters, vk_pvs.num_clusters, bad_rows);
}

void VK_InitPVS (void)
{
	Cmd_AddCommand ("vk_pvs", VK_PVS_f);
}
