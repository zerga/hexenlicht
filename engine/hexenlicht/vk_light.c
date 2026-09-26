/* vk_light.c -- the lights of the path tracer and their per-cluster lists
 *
 * Quake II RTX's two kinds of lights (shaders/light_lists.h):
 *  - the lights of the light buffer (shaders/vertex_buffer.h's LightBuffer,
 *    one per frame in flight, by device address), sampled from the light
 *    list of the receiving point's vis cluster: polygons (Quake II RTX's
 *    light polygons) and spheres (Hexen II's point lights; Quake II RTX's
 *    lists hold only polygons), a sphere with an optional range at which
 *    its light fades to 0;
 *  - dynamic sphere lights: up to MAX_LIGHT_SOURCES in the global UBO's
 *    dyn_light_data, one picked at random per pixel (main.c's add_dlights),
 *    for lights that move (4.4).
 * The light lists (Quake II RTX's bsp_mesh.c collect_cluster_lights) are
 * built when the lights change: a light goes into the list of every
 * cluster in the PVS of the open leafs its emitter touches (Quake II RTX
 * takes the light's one cluster), except the clusters entirely behind a
 * polygon (light_affects_cluster) and those beyond a sphere's range. A
 * cluster's bounds are its leaf's and its world triangles' (Quake II RTX:
 * its opaque triangles'; vk_instance.c gives models and brush entities
 * the cluster of their center). Each frame in flight's light buffer
 * copies the lists when they have changed; the lights are written every
 * frame.
 * The light statistics (Quake II RTX's, after G. Ward's "Adaptive Shadow
 * Testing for Ray Tracing"): direct_lighting.rgen counts unshadowed and
 * shadowed rays per light list entry and primary direction of the
 * receiving normal, and the next frame's light CDF weighs each light by
 * its unshadowed share. Three device-local buffers take turns per 3D
 * frame (counted this frame, last frame's, the one before for the
 * denoiser's gradient samples, 3.6); VK_ClearLightStats clears this
 * frame's before the passes, and all three after the lists were rebuilt.
 * Quake II RTX counts per cluster and light: 50 MB per buffer on Hexen
 * II's largest maps.
 * For now (stories 3.3, 3.4) the lights are test lights placed with
 * vk_testlight: spheres (at the eye, or at the map's light entities),
 * quads (two polygon lights each) that emit towards where the camera
 * looked, and dynamic spheres; VK_LoadWorld clears them (a new map). Units
 * are Quake II RTX's shaders': a polygon's color is its radiance, with
 * Quake II RTX's sqrt(cos) emission lobe; a sphere contributes its
 * radiance times its solid angle, falling off with the inverse square of
 * the distance. A test sphere's intensity is pi times its radiance, as a
 * UBO sphere light's color is (the sampling gives its solid angle / pi,
 * the diffuse BRDF divides by pi again); Quake II RTX's add_dlights divides
 * a dlight's intensity by 25 first. VK_PrepareUBO calls VK_PrepareLights
 * for each 3D frame.
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

#define MAX_TEST_QUADS		16	/* two polygon lights each */
#define TEST_SPHERE_RADIUS	8.0f
#define TEST_SPHERE_INTENSITY	1000.0f
#define DEFAULT_LIGHT_LEVEL	300	/* utils/light's DEFAULTLIGHTLEVEL */

typedef struct
{
	vec3_t		origin;
	float		radius;
	float		range;		/* 0 = unlimited */
	vec3_t		color;		/* pi x radiance */
} test_sphere_t;

typedef struct
{
	vec3_t		corners[4];	/* emitting towards cross(corners[1] - corners[0], corners[2] - corners[0]) */
	vec3_t		color;		/* radiance */
} test_quad_t;

static test_sphere_t	test_spheres[MAX_LIGHT_POLYS];		/* in the light lists */
static int		num_test_spheres;
static test_sphere_t	test_dlights[MAX_LIGHT_SOURCES];	/* the UBO's */
static int		num_test_dlights;
static test_quad_t	test_quads[MAX_TEST_QUADS];
static int		num_test_quads;

/* the light buffer's lights, in its order: the quads' triangles, then the spheres */
typedef struct
{
	int		type;		/* LIGHT_TYPE_* */
	vec3_t		p[3];		/* a polygon's corners, a sphere's center in p[0] */
	float		radius, range;	/* a sphere's; range 0 = unlimited */
	vec3_t		color;		/* radiance */
} light_t;

static light_t		lights[MAX_LIGHT_POLYS];
static int		num_lights;

/* the light lists: list c is list_nodes[list_offsets[c]] up to list_offsets[c + 1] */
static uint32_t		list_offsets[MAX_LIGHT_LISTS];
static uint32_t		list_nodes[MAX_LIGHT_LIST_NODES];
static uint32_t		list_pairs[MAX_LIGHT_LIST_NODES];	/* building: cluster << 16 | light */
static int		num_lists;		/* the clusters with a list */
static uint32_t		num_nodes;
static uint32_t		lists_version = 1;	/* counts the builds */
static uint32_t		buffer_version[VK_FRAMES_IN_FLIGHT];	/* the lists each light buffer holds, 0 = none */

static float		(*cluster_bounds)[6];	/* mins, maxs per cluster */
static int		num_cluster_bounds;
static qboolean		range_culling = true;	/* vk_lights cull: off keeps the spheres in every list their PVS reaches */

static struct
{
	int		homeless;	/* lights touching no open leaf (inside solid) */
	int		dropped;	/* left out: the lists were full */
	int		ranged;		/* spheres with a range */
	double		build_time;
} build;

static vk_buffer_t	light_buffers[VK_FRAMES_IN_FLIGHT];	/* LightBuffer */
static vk_buffer_t	stats_buffers[NUM_LIGHT_STATS_BUFFERS];
static VkDeviceSize	stats_size;	/* in use: num_nodes x LIGHT_STATS_UINTS uints */
static int		stats_clear;	/* the buffers VK_ClearLightStats clears, a bit each */


/* ==========================================================================
 * The light lists
 * ========================================================================== */

/* a new map (VK_LoadWorld, after the PVS is final): each cluster's bounds
 * are its leaf's and those of the world triangles in it */
void VK_LoadLightClusters (qmodel_t *worldmodel, const VboPrimitive *prims, uint32_t num_prims)
{
	int		c, k;
	uint32_t	i;

	free (cluster_bounds);
	num_cluster_bounds = vk_pvs.num_clusters;
	cluster_bounds = (float (*)[6]) malloc (q_max (num_cluster_bounds, 1) * sizeof(*cluster_bounds));
	if (!cluster_bounds)
		Sys_Error ("%s: out of memory", __thisfunc__);
	for (c = 0; c < num_cluster_bounds; c++)
		memcpy (cluster_bounds[c], worldmodel->leafs[c + 1].minmaxs, sizeof(cluster_bounds[c]));
	for (i = 0; i < num_prims; i++)
	{
		const float	*v[3] = { prims[i].pos0, prims[i].pos1, prims[i].pos2 };
		float		*b;

		c = prims[i].cluster;
		if (c < 0 || c >= num_cluster_bounds)
			continue;
		b = cluster_bounds[c];
		for (k = 0; k < 3; k++)
		{
			b[0] = q_min (b[0], v[k][0]);	b[3] = q_max (b[3], v[k][0]);
			b[1] = q_min (b[1], v[k][1]);	b[4] = q_max (b[4], v[k][1]);
			b[2] = q_min (b[2], v[k][2]);	b[5] = q_max (b[5], v[k][2]);
		}
	}
	VK_UpdateLights ();	/* the test lights were cleared: empty lists */
}

/* adds the PVS of the open leafs the box touches to row; returns their number */
static int AddLeafPVS (qmodel_t *world, mnode_t *node, vec3_t mins, vec3_t maxs, byte *row)
{
	int		c, i, n = 0;
	const byte	*pvs;

	while (node->contents >= 0)
	{
		int	side = BoxOnPlaneSide (mins, maxs, node->plane);

		if (side == 3)
			n += AddLeafPVS (world, node->children[0], mins, maxs, row);
		node = node->children[(side == 1) ? 0 : 1];
	}
	if (node->contents == CONTENTS_SOLID)
		return n;
	c = (int)((mleaf_t *) node - world->leafs) - 1;
	pvs = VK_ClusterPVS (c);
	if (!pvs)
		return n;
	for (i = 0; i < vk_pvs.row_bytes; i++)
		row[i] |= pvs[i];
	row[c >> 3] |= (byte)(1 << (c & 7));	/* its own leaf, whatever vis says */
	return n + 1;
}

/* Quake II RTX's light_affects_cluster: false when the cluster is entirely
 * behind the polygon; for a sphere with a range, when it is beyond it */
static qboolean LightAffectsCluster (const light_t *l, const float *b)
{
	int	corner, k;

	if (l->type == LIGHT_TYPE_SPHERE)
	{
		float	d2 = 0.0f;

		if (l->range <= 0.0f || !range_culling)
			return true;
		for (k = 0; k < 3; k++)
		{
			float	d = q_max (q_max (b[k] - l->p[0][k], l->p[0][k] - b[k + 3]), 0.0f);

			d2 += d * d;
		}
		return d2 <= l->range * l->range;
	}
	else
	{
		vec3_t	e1, e2, normal, corner_p;
		float	dist;

		VectorSubtract (l->p[1], l->p[0], e1);
		VectorSubtract (l->p[2], l->p[0], e2);
		CrossProduct (e1, e2, normal);
		VectorNormalize (normal);
		dist = DotProduct (normal, l->p[0]);
		for (corner = 0; corner < 8; corner++)
		{
			for (k = 0; k < 3; k++)
				corner_p[k] = (corner & (1 << k)) ? b[k + 3] : b[k];
			if (DotProduct (normal, corner_p) - dist > 0.0f)
				return true;
		}
		return false;
	}
}

/* the box the light's emitter takes: a sphere's, or a polygon's with its
 * front side one unit off the surface (where its cluster is) */
static void EmitterBounds (const light_t *l, vec3_t mins, vec3_t maxs)
{
	int	k, i;

	if (l->type == LIGHT_TYPE_SPHERE)
	{
		for (k = 0; k < 3; k++)
		{
			mins[k] = l->p[0][k] - l->radius;
			maxs[k] = l->p[0][k] + l->radius;
		}
	}
	else
	{
		vec3_t	e1, e2, normal;

		VectorSubtract (l->p[1], l->p[0], e1);
		VectorSubtract (l->p[2], l->p[0], e2);
		CrossProduct (e1, e2, normal);
		VectorNormalize (normal);
		VectorCopy (l->p[0], mins);
		VectorCopy (l->p[0], maxs);
		for (i = 0; i < 3; i++)
		{
			for (k = 0; k < 3; k++)
			{
				float	a = l->p[i][k], b = a + normal[k];

				mins[k] = q_min (mins[k], q_min (a, b));
				maxs[k] = q_max (maxs[k], q_max (a, b));
			}
		}
	}
}

/* Quake II RTX's collect_cluster_lights, over the lights' leafs' PVS; a
 * light that doesn't fit is left out whole */
static void BuildLightLists (void)
{
	qmodel_t	*world = vk_world.worldmodel;
	byte		*row;
	int		l, c;
	uint32_t	i, num_pairs = 0;
	double		start = Sys_DoubleTime ();

	memset (&build, 0, sizeof(build));
	num_lists = (world && cluster_bounds && num_cluster_bounds == vk_pvs.num_clusters) ?
		    q_min (vk_pvs.num_clusters, MAX_LIGHT_LISTS - 1) : 0;
	row = (byte *) malloc (q_max (vk_pvs.row_bytes, 4));
	if (!row)
		Sys_Error ("%s: out of memory", __thisfunc__);

	for (l = 0; l < num_lights && num_lists; l++)
	{
		const light_t	*light = &lights[l];
		uint32_t	first = num_pairs;
		vec3_t		mins, maxs;

		if (light->type == LIGHT_TYPE_SPHERE && light->range > 0.0f)
			build.ranged++;
		memset (row, 0, vk_pvs.row_bytes);
		EmitterBounds (light, mins, maxs);
		if (!AddLeafPVS (world, world->nodes, mins, maxs, row))
		{
			build.homeless++;
			continue;
		}
		for (c = 0; c < num_lists; c++)
		{
			if (!(row[c >> 3] & (1 << (c & 7))) || !LightAffectsCluster (light, cluster_bounds[c]))
				continue;
			if (num_pairs == MAX_LIGHT_LIST_NODES)
				break;
			list_pairs[num_pairs++] = ((uint32_t)c << 16) | (uint32_t)l;
		}
		if (c < num_lists)
		{
			num_pairs = first;
			build.dropped++;
		}
	}
	free (row);

	/* sorted by cluster, each list in the lights' order */
	memset (list_offsets, 0, (num_lists + 1) * sizeof(list_offsets[0]));
	for (i = 0; i < num_pairs; i++)
		list_offsets[(list_pairs[i] >> 16) + 1]++;
	for (c = 0; c < num_lists; c++)
		list_offsets[c + 1] += list_offsets[c];
	for (i = 0; i < num_pairs; i++)
	{
		c = (int)(list_pairs[i] >> 16);
		list_nodes[list_offsets[c]++] = list_pairs[i] & 0xffff;
	}
	for (c = num_lists; c > 0; c--)	/* each offset moved to its list's end: back to the start */
		list_offsets[c] = list_offsets[c - 1];
	list_offsets[0] = 0;
	num_nodes = num_pairs;
	lists_version++;
	build.build_time = Sys_DoubleTime () - start;
}

/* the statistics buffers, grown to the lists (outside frames) and cleared
 * before the next frame's passes: the list entries have changed */
static void ResizeLightStats (void)
{
	VkDeviceSize	need = (VkDeviceSize)num_nodes * LIGHT_STATS_UINTS * sizeof(uint32_t);
	int		i;

	stats_size = need;
	stats_clear = (1 << NUM_LIGHT_STATS_BUFFERS) - 1;
	if (need <= stats_buffers[0].size)
		return;
	if (stats_buffers[0].buffer)
		vkDeviceWaitIdle (vk.device);	/* frames in flight may still count */
	for (i = 0; i < NUM_LIGHT_STATS_BUFFERS; i++)
	{
		VkDeviceSize	size = 65536;

		while (size < need)
			size *= 2;
		VK_DestroyBuffer (&stats_buffers[i]);
		VK_CreateBuffer (&stats_buffers[i], size,
				 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
				 VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_DEVICE);	/* cleared, read back */
	}
}

/* the test lights into the light buffer's order, then their lists: after
 * every change of the lights (outside frames) */
void VK_UpdateLights (void)
{
	static const int	tris[2][3] = { { 0, 1, 2 }, { 2, 1, 3 } };
	int			i, t, k;

	num_lights = 0;
	for (i = 0; i < num_test_quads; i++)
	{
		for (t = 0; t < 2; t++)
		{
			light_t	*l = &lights[num_lights++];

			memset (l, 0, sizeof(*l));
			l->type = LIGHT_TYPE_POLYGON;
			for (k = 0; k < 3; k++)
				VectorCopy (test_quads[i].corners[tris[t][k]], l->p[k]);
			VectorCopy (test_quads[i].color, l->color);
		}
	}
	for (i = 0; i < num_test_spheres && num_lights < MAX_LIGHT_POLYS; i++)
	{
		light_t	*l = &lights[num_lights++];

		memset (l, 0, sizeof(*l));
		l->type = LIGHT_TYPE_SPHERE;
		VectorCopy (test_spheres[i].origin, l->p[0]);
		l->radius = test_spheres[i].radius;
		l->range = test_spheres[i].range;
		VectorScale (test_spheres[i].color, 1.0f / (float)M_PI, l->color);
	}
	BuildLightLists ();
	ResizeLightStats ();
}

/* a new map (VK_LoadWorld): no test lights; VK_LoadLightClusters builds
 * the (empty) lists once the new PVS is final */
void VK_ClearLights (void)
{
	num_test_spheres = num_test_quads = num_test_dlights = 0;
	num_lights = 0;
	num_cluster_bounds = 0;
}


/* ==========================================================================
 * Each frame
 * ========================================================================== */

/* a light's LIGHT_POLY_VEC4S vec4s (shaders/vertex_buffer.h's LightBuffer) */
static void WriteLight (const light_t *l, float *p)
{
	memset (p, 0, LIGHT_POLY_VEC4S * 4 * sizeof(float));
	if (l->type == LIGHT_TYPE_SPHERE)
	{
		VectorCopy (l->p[0], p);
		p[4] = l->radius;
		p[5] = l->range;
	}
	else
	{
		VectorCopy (l->p[0], p + 0);
		VectorCopy (l->p[1], p + 4);
		VectorCopy (l->p[2], p + 8);
	}
	p[3] = l->color[0];
	p[7] = l->color[1];
	p[11] = l->color[2];
	p[12] = p[13] = 1.0f;	/* no light style until 4.2 */
	p[14] = (float)l->type;
}

/* fills this frame's light buffer and the UBO's light fields */
void VK_PrepareLights (struct QVKUniformBuffer_s *ubo)
{
	vk_buffer_t	*buf = &light_buffers[vk.frame_index];
	LightBuffer	*lb = (LightBuffer *) buf->mapped;
	int		i, cur;

	/* dynamic sphere lights: Quake II RTX's add_dlights */
	ubo->num_dyn_lights = num_test_dlights;
	for (i = 0; i < num_test_dlights; i++)
	{
		DynLightData	*d = &ubo->dyn_light_data[i];

		memset (d, 0, sizeof(*d));
		VectorCopy (test_dlights[i].origin, d->center);
		d->radius = test_dlights[i].radius;
		VectorCopy (test_dlights[i].color, d->color);
		d->type = DYNLIGHT_SPHERE;
	}

	/* the lights every frame (light styles, 4.2), the lists when they have changed */
	for (i = 0; i < num_lights; i++)
		WriteLight (&lights[i], &lb->light_polys[i * LIGHT_POLY_VEC4S][0]);
	if (buffer_version[vk.frame_index] != lists_version)
	{
		memcpy (lb->light_list_offsets, list_offsets, (num_lists + 1) * sizeof(list_offsets[0]));
		memcpy (lb->light_list_lights, list_nodes, num_nodes * sizeof(list_nodes[0]));
		buffer_version[vk.frame_index] = lists_version;
	}
	ubo->num_static_lights = num_lights;
	ubo->lights = buf->address;

	/* the statistics: counted into this frame's buffer (vk_render_frame
	 * counts the 3D frames), read from the last two frames' */
	if (stats_size)
	{
		cur = (int)(vk_render_frame % NUM_LIGHT_STATS_BUFFERS);
		ubo->light_stats = stats_buffers[cur].address;
		ubo->light_stats_prev = stats_buffers[(cur + 2) % NUM_LIGHT_STATS_BUFFERS].address;
		ubo->light_stats_prev2 = stats_buffers[(cur + 1) % NUM_LIGHT_STATS_BUFFERS].address;
		stats_clear |= 1 << cur;
	}
	else
	{
		ubo->light_stats = ubo->light_stats_prev = ubo->light_stats_prev2 = 0;
	}

	VK_CHECK (vmaFlushAllocation (vk.allocator, buf->allocation, 0, VK_WHOLE_SIZE));
}

static void StatsBarrier (VkCommandBuffer cmd, VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
			  VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access)
{
	VkMemoryBarrier2	barrier;
	VkDependencyInfo	dep;

	memset (&barrier, 0, sizeof(barrier));
	barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
	barrier.srcStageMask = src_stage;
	barrier.srcAccessMask = src_access;
	barrier.dstStageMask = dst_stage;
	barrier.dstAccessMask = dst_access;
	memset (&dep, 0, sizeof(dep));
	dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
	dep.memoryBarrierCount = 1;
	dep.pMemoryBarriers = &barrier;
	vkCmdPipelineBarrier2 (cmd, &dep);
}

/* before the view passes (vk_view.c, after VK_PrepareUBO): clears this
 * frame's statistics buffer, and all three after the lists changed
 * (Quake II RTX's vkpt_light_buffer_upload_staging) */
void VK_ClearLightStats (VkCommandBuffer cmd)
{
	int	i;

	if (stats_size && stats_clear)
	{
		StatsBarrier (cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
			      VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
			      VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
		for (i = 0; i < NUM_LIGHT_STATS_BUFFERS; i++)
		{
			if (stats_clear & (1 << i))
				vkCmdFillBuffer (cmd, stats_buffers[i].buffer, 0, stats_size, 0);
		}
		StatsBarrier (cmd, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
			      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
			      VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
	}
	stats_clear = 0;
}


/* ==========================================================================
 * vk_testlight, vk_lights
 * ========================================================================== */

/* the client is in the map vk_world loaded: between "map" and R_NewMap the
 * old map's memory is already freed (Host_ClearMemory) */
static qboolean WorldReady (void)
{
	return cls.signon == SIGNONS && cl.worldmodel && vk_world.worldmodel == cl.worldmodel;
}

static void ParseColor (int first, float intensity, vec3_t color)
{
	int	k;

	for (k = 0; k < 3; k++)
		color[k] = q_max (((Cmd_Argc () > first + k) ? (float)atof (Cmd_Argv (first + k)) : 1.0f) * intensity, 0.0f);
}

static float ArgFloat (int n, float def)
{
	return (Cmd_Argc () > n) ? (float)atof (Cmd_Argv (n)) : def;
}

/* a white sphere at each light entity, its range the entity's light value
 * (utils/light's hard range) times range_scale (0 = unlimited): the real
 * lights come with 4.1 */
static int AddEntitySpheres (float intensity, float range_scale)
{
	const char	*data = vk_world.worldmodel->entities;
	int		n = 0;

	while (data && (data = COM_Parse (data)) != NULL && com_token[0] == '{')
	{
		qboolean	is_light = false, has_origin = false;
		vec3_t		origin;
		int		level = 0;

		while ((data = COM_Parse (data)) != NULL && com_token[0] != '}')
		{
			int	key = !strcmp (com_token, "classname") ? 1 : !strcmp (com_token, "origin") ? 2 :
				      !strncmp (com_token, "light", 5) ? 3 : 0;	/* utils/light's keys */

			if ((data = COM_Parse (data)) == NULL)
				break;
			if (key == 1)
				is_light = !strncmp (com_token, "light", 5);
			else if (key == 2)
				has_origin = sscanf (com_token, "%f %f %f", &origin[0], &origin[1], &origin[2]) == 3;
			else if (key == 3)
				level = atoi (com_token);
		}
		if (!is_light || !has_origin || num_test_spheres == MAX_LIGHT_POLYS)
			continue;
		test_spheres[num_test_spheres].radius = TEST_SPHERE_RADIUS;
		test_spheres[num_test_spheres].range = (float)(level ? level : DEFAULT_LIGHT_LEVEL) * range_scale;
		VectorCopy (origin, test_spheres[num_test_spheres].origin);
		VectorSet (test_spheres[num_test_spheres].color, intensity, intensity, intensity);
		num_test_spheres++;
		n++;
	}
	return n;
}

static void PrintBuild (void)
{
	if (build.homeless)
		Con_Printf ("%d lights touch no open leaf (inside solid): in no list\n", build.homeless);
	if (build.dropped)
		Con_Printf ("%d lights left out: the light lists are full (%d entries)\n", build.dropped,
			    MAX_LIGHT_LIST_NODES);
}

static void VK_TestLight_f (void)
{
	const char	*what = (Cmd_Argc () > 1) ? Cmd_Argv (1) : "";
	int		i;

	if (!q_strcasecmp (what, "sphere") || !q_strcasecmp (what, "dlight"))
	{
		qboolean	dlight = !q_strcasecmp (what, "dlight");
		test_sphere_t	*s;

		if (!WorldReady ())
			return;
		if (dlight ? (num_test_dlights == MAX_LIGHT_SOURCES) :
			     (num_test_spheres + num_test_quads * 2 >= MAX_LIGHT_POLYS))
		{
			Con_Printf ("vk_testlight: at most %d %s\n", dlight ? MAX_LIGHT_SOURCES : MAX_LIGHT_POLYS,
				    dlight ? "dynamic sphere lights" : "lights in the light lists");
			return;
		}
		s = dlight ? &test_dlights[num_test_dlights++] : &test_spheres[num_test_spheres++];
		VectorCopy (r_scene.vieworg, s->origin);
		s->radius = q_max (ArgFloat (2, TEST_SPHERE_RADIUS), 0.1f);
		ParseColor (4, ArgFloat (3, TEST_SPHERE_INTENSITY), s->color);
		s->range = dlight ? 0.0f : q_max (ArgFloat (7, 0.0f), 0.0f);
		if (!dlight)
		{
			VK_UpdateLights ();
			PrintBuild ();
		}
		return;
	}
	if (!q_strcasecmp (what, "quad"))
	{
		test_quad_t	*q;
		float		half;
		vec3_t		r, u;

		if (!WorldReady ())
			return;
		if (num_test_quads == MAX_TEST_QUADS || num_test_spheres + (num_test_quads + 1) * 2 > MAX_LIGHT_POLYS)
		{
			Con_Printf ("vk_testlight: at most %d quads\n", MAX_TEST_QUADS);
			return;
		}
		q = &test_quads[num_test_quads++];
		half = q_max (ArgFloat (2, 32.0f), 1.0f) * 0.5f;
		VectorScale (r_scene.right, half, r);
		VectorScale (r_scene.up, half, u);
		/* cross(up, right) is forward: corners 0 1 2 3 = -r-u, -r+u, +r-u, +r+u */
		VectorSubtract (r_scene.vieworg, r, q->corners[0]);
		VectorSubtract (q->corners[0], u, q->corners[0]);
		VectorSubtract (r_scene.vieworg, r, q->corners[1]);
		VectorAdd (q->corners[1], u, q->corners[1]);
		VectorAdd (r_scene.vieworg, r, q->corners[2]);
		VectorSubtract (q->corners[2], u, q->corners[2]);
		VectorAdd (r_scene.vieworg, r, q->corners[3]);
		VectorAdd (q->corners[3], u, q->corners[3]);
		ParseColor (4, ArgFloat (3, 50.0f), q->color);
		VK_UpdateLights ();
		PrintBuild ();
		return;
	}
	if (!q_strcasecmp (what, "entities"))
	{
		if (!WorldReady ())
			return;
		num_test_spheres = num_test_quads = num_test_dlights = 0;
		i = AddEntitySpheres (ArgFloat (2, TEST_SPHERE_INTENSITY), q_max (ArgFloat (3, 1.0f), 0.0f));
		VK_UpdateLights ();
		Con_Printf ("%d sphere lights at the light entities, %u light list entries\n", i, num_nodes);
		PrintBuild ();
		return;
	}
	if (!q_strcasecmp (what, "clear"))
	{
		num_test_spheres = num_test_quads = num_test_dlights = 0;
		VK_UpdateLights ();
		return;
	}
	if (!q_strcasecmp (what, "list"))
	{
		for (i = 0; i < num_test_spheres; i++)
			Con_Printf ("sphere %3d at %.1f %.1f %.1f radius %.1f color %.1f %.1f %.1f range %.0f\n", i,
				    test_spheres[i].origin[0], test_spheres[i].origin[1], test_spheres[i].origin[2],
				    test_spheres[i].radius, test_spheres[i].color[0], test_spheres[i].color[1],
				    test_spheres[i].color[2], test_spheres[i].range);
		for (i = 0; i < num_test_dlights; i++)
			Con_Printf ("dlight %3d at %.1f %.1f %.1f radius %.1f color %.1f %.1f %.1f\n", i,
				    test_dlights[i].origin[0], test_dlights[i].origin[1], test_dlights[i].origin[2],
				    test_dlights[i].radius, test_dlights[i].color[0], test_dlights[i].color[1],
				    test_dlights[i].color[2]);
		for (i = 0; i < num_test_quads; i++)
		{
			vec3_t	c, side;

			VectorAdd (test_quads[i].corners[0], test_quads[i].corners[3], c);
			VectorScale (c, 0.5f, c);
			VectorSubtract (test_quads[i].corners[2], test_quads[i].corners[0], side);
			Con_Printf ("quad   %3d at %.1f %.1f %.1f size %.1f color %.1f %.1f %.1f\n", i, c[0], c[1], c[2],
				    VectorLength (side), test_quads[i].color[0], test_quads[i].color[1], test_quads[i].color[2]);
		}
		Con_Printf ("%d sphere lights, %d polygon lights, %d dynamic sphere lights\n", num_test_spheres,
			    num_test_quads * 2, num_test_dlights);
		return;
	}
	Con_Printf ("vk_testlight sphere [radius] [intensity] [r g b] [range]: a sphere light at the eye\n"
		    "  (8, 1000, 1 1 1, 0 = unlimited); the intensity is pi x its radiance\n"
		    "vk_testlight dlight [radius] [intensity] [r g b]: the same as a dynamic sphere light\n"
		    "vk_testlight quad [size] [intensity] [r g b]: a square light at the eye, facing the view (32, 50, 1 1 1)\n"
		    "vk_testlight entities [intensity] [range scale]: replaces the test lights with a sphere at each\n"
		    "  light entity, its range the entity's light value x range scale (1000, 1; 0 = unlimited)\n"
		    "vk_testlight list | clear\n");
}

/* vk_lights stats: the light statistics the last 3D frame counted, read back */
static void PrintLightStats (void)
{
	vk_buffer_t		readback;
	VkCommandBuffer		cmd;
	VkBufferCopy		copy;
	const uint32_t		*s;
	unsigned long long	lit = 0, shadowed = 0;
	uint32_t		i, sampled = 0, mostly_shadowed = 0;
	int			k;

	if (!stats_size || !vk_render_frame)
	{
		Con_Printf ("No light statistics\n");
		return;
	}
	if (stats_clear)
	{
		Con_Printf ("No light statistics since the light lists changed\n");
		return;
	}
	vkDeviceWaitIdle (vk.device);	/* the last frame's passes have run */
	VK_CreateBuffer (&readback, stats_size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_READBACK);
	cmd = VK_BeginUpload ();
	StatsBarrier (cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
		      VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
	memset (&copy, 0, sizeof(copy));
	copy.size = stats_size;
	vkCmdCopyBuffer (cmd, stats_buffers[vk_render_frame % NUM_LIGHT_STATS_BUFFERS].buffer, readback.buffer, 1, &copy);
	StatsBarrier (cmd, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
		      VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
	VK_EndUpload ();
	VK_CHECK (vmaInvalidateAllocation (vk.allocator, readback.allocation, 0, VK_WHOLE_SIZE));
	s = (const uint32_t *) readback.mapped;
	for (i = 0; i < num_nodes; i++)
	{
		uint32_t	h = 0, m = 0;

		for (k = 0; k < LIGHT_STATS_UINTS / 2; k++)
		{
			h += s[i * LIGHT_STATS_UINTS + k * 2];
			m += s[i * LIGHT_STATS_UINTS + k * 2 + 1];
		}
		lit += h;
		shadowed += m;
		sampled += (h + m != 0);
		mostly_shadowed += (h + m != 0 && m > 9 * h);	/* weighed down to the lower limit, 0.1 */
	}
	Con_Printf ("the last frame's shadow rays to list lights: %llu unshadowed, %llu shadowed; %u of %u list entries sampled, %u of them over 90%% shadowed\n",
		    lit, shadowed, sampled, num_nodes, mostly_shadowed);
	VK_DestroyBuffer (&readback);
}

static void VK_Lights_f (void)
{
	int		c, max_c = -1, empty = 0, spheres = 0, view, k;
	uint32_t	n, max_n = 0;

	if (Cmd_Argc () > 2 && !q_strcasecmp (Cmd_Argv (1), "cull"))
	{
		/* a check: without range culling the image must stay the same,
		 * only noisier (the window already takes the light to 0) */
		range_culling = atoi (Cmd_Argv (2)) != 0;
		if (WorldReady ())
			VK_UpdateLights ();	/* else with the next map */
	}
	if (Cmd_Argc () > 1 && !q_strcasecmp (Cmd_Argv (1), "stats"))
	{
		PrintLightStats ();
		return;
	}
	for (c = 0; c < num_lights; c++)
		spheres += (lights[c].type == LIGHT_TYPE_SPHERE);
	for (c = 0; c < num_lists; c++)
	{
		n = list_offsets[c + 1] - list_offsets[c];
		empty += (n == 0);
		if (n > max_n)
		{
			max_n = n;
			max_c = c;
		}
	}
	Con_Printf ("%d lights in the light lists: %d polygons, %d spheres (%d with a range%s); %d dynamic sphere lights\n",
		    num_lights, num_lights - spheres, spheres, build.ranged, range_culling ? "" : ", not culled by it",
		    num_test_dlights);
	Con_Printf ("lists of %d clusters (of %d): %u entries (at most %d), mean %.1f, max %u (cluster %d), %d empty\n",
		    num_lists, vk_pvs.num_clusters, num_nodes, MAX_LIGHT_LIST_NODES,
		    num_lists ? (double)num_nodes / num_lists : 0.0, max_n, max_c, empty);
	PrintBuild ();
	Con_Printf ("built in %.2f ms; statistics %d x %.1f KB (allocated %.1f KB each)\n", build.build_time * 1000.0,
		    NUM_LIGHT_STATS_BUFFERS, stats_size / 1024.0, stats_buffers[0].size / 1024.0);
	if (!WorldReady ())
		return;
	view = VK_PointCluster (vk_world.worldmodel, r_scene.vieworg);
	if (view < 0 || view >= num_lists)
	{
		Con_Printf ("the camera is in no cluster with a list\n");
		return;
	}
	n = list_offsets[view + 1] - list_offsets[view];
	Con_Printf ("the camera's cluster %d: %u lights", view, n);
	for (k = 0; k < (int)n && k < 32; k++)
		Con_Printf (" %u", list_nodes[list_offsets[view] + k]);
	Con_Printf ((n > 32) ? " ...\n" : "\n");
}


/* ==========================================================================
 * Init / shutdown
 * ========================================================================== */

void VK_InitLights (void)
{
	int	i;

	for (i = 0; i < VK_FRAMES_IN_FLIGHT; i++)
	{
		VK_CreateBuffer (&light_buffers[i], sizeof(LightBuffer),
				 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
				 VK_MEMORY_UPLOAD);
		memset (light_buffers[i].mapped, 0, sizeof(LightBuffer));	/* no stale lists */
		buffer_version[i] = 0;
	}
	Cmd_AddCommand ("vk_testlight", VK_TestLight_f);
	Cmd_AddCommand ("vk_lights", VK_Lights_f);
}

void VK_ShutdownLights (void)
{
	int	i;

	for (i = 0; i < VK_FRAMES_IN_FLIGHT; i++)
		VK_DestroyBuffer (&light_buffers[i]);
	for (i = 0; i < NUM_LIGHT_STATS_BUFFERS; i++)
		VK_DestroyBuffer (&stats_buffers[i]);
	stats_size = 0;
	stats_clear = 0;
	free (cluster_bounds);
	cluster_bounds = NULL;
	VK_ClearLights ();
	num_lists = 0;
	num_nodes = 0;
}
