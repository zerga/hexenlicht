/* vk_light.c -- the lights of the path tracer
 *
 * Quake II RTX's two kinds of lights (shaders/light_lists.h):
 *  - polygon lights: triangles in the light buffer (shaders/vertex_buffer.h's
 *    LightBuffer, one per frame in flight, by device address), sampled from
 *    the light list of the receiving point's vis cluster (Quake II RTX's
 *    vertex_buffer.c light buffer and bsp_mesh.c light lists);
 *  - sphere lights: up to MAX_LIGHT_SOURCES in the global UBO's
 *    dyn_light_data, one picked at random per pixel (main.c's add_dlights).
 * For now (story 3.3) the lights are test lights placed with vk_testlight:
 * spheres, and quads (two polygon lights each) that emit towards where
 * the camera looked; every cluster's list holds every polygon light (3.4
 * builds them from the PVS). VK_LoadWorld clears the test lights (a new
 * map). Units are Quake II RTX's shaders': a sphere's color is pi times its
 * radiance (the sampling gives its solid angle / pi, the diffuse BRDF
 * divides by pi again), falling off with the inverse square of the
 * distance; a polygon's color is its radiance, with Quake II RTX's
 * sqrt(cos) emission lobe. Quake II RTX's add_dlights divides a dlight's
 * intensity by 25 first; test lights give the color directly. The list
 * entries (clusters x polygons) are rewritten every frame, which is fine
 * for test lights (3.4 writes them when they change). VK_PrepareUBO calls
 * VK_PrepareLights for each 3D frame.
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

#define MAX_TEST_QUADS	16		/* two polygon lights each */

typedef struct
{
	vec3_t		origin;
	float		radius;
	vec3_t		color;		/* radiance */
} test_sphere_t;

typedef struct
{
	vec3_t		corners[4];	/* emitting towards cross(corners[1] - corners[0], corners[2] - corners[0]) */
	vec3_t		color;		/* radiance */
} test_quad_t;

static test_sphere_t	test_spheres[MAX_LIGHT_SOURCES];
static int		num_test_spheres;
static test_quad_t	test_quads[MAX_TEST_QUADS];
static int		num_test_quads;

static vk_buffer_t	light_buffers[VK_FRAMES_IN_FLIGHT];	/* LightBuffer */

/* the quads' triangles, LIGHT_POLY_VEC4S vec4s each: the corners with the
 * color in their w, then the light style scale and last frame's */
static int WriteLightPolys (float *out)
{
	static const int	tris[2][3] = { { 0, 1, 2 }, { 2, 1, 3 } };
	int			i, t, k, n = 0;

	for (i = 0; i < num_test_quads; i++)
	{
		for (t = 0; t < 2; t++, n++)
		{
			float	*p = out + n * LIGHT_POLY_VEC4S * 4;

			for (k = 0; k < 3; k++)
			{
				VectorCopy (test_quads[i].corners[tris[t][k]], p + k * 4);
				p[k * 4 + 3] = test_quads[i].color[k];
			}
			p[12] = p[13] = 1.0f;	/* no light style */
			p[14] = p[15] = 0.0f;
		}
	}
	return n;
}

/* a new map (VK_LoadWorld): no test lights */
void VK_ClearLights (void)
{
	num_test_spheres = num_test_quads = 0;
}

/* fills this frame's light buffer and the UBO's light fields */
void VK_PrepareLights (struct QVKUniformBuffer_s *ubo)
{
	vk_buffer_t	*buf = &light_buffers[vk.frame_index];
	LightBuffer	*lb = (LightBuffer *) buf->mapped;
	int		i, c, num_polys, num_lists;
	uint32_t	*nodes;

	/* sphere lights: Quake II RTX's add_dlights */
	ubo->num_dyn_lights = num_test_spheres;
	for (i = 0; i < num_test_spheres; i++)
	{
		DynLightData	*d = &ubo->dyn_light_data[i];

		memset (d, 0, sizeof(*d));
		VectorCopy (test_spheres[i].origin, d->center);
		d->radius = test_spheres[i].radius;
		VectorCopy (test_spheres[i].color, d->color);
		d->type = DYNLIGHT_SPHERE;
	}

	/* polygon lights, and a list per cluster with all of them (no culling
	 * by the PVS until 3.4) */
	num_polys = WriteLightPolys (&lb->light_polys[0][0]);
	num_lists = vk_pvs.num_clusters;
	if (num_lists + 1 > MAX_LIGHT_LISTS || num_lists * num_polys > MAX_LIGHT_LIST_NODES)
		num_polys = 0;	/* vk_testlight refuses lights that don't fit */
	nodes = lb->light_list_lights;
	for (c = 0; c <= num_lists && c < MAX_LIGHT_LISTS; c++)
	{
		lb->light_list_offsets[c] = (uint32_t)(c * num_polys);
		if (c < num_lists)
		{
			for (i = 0; i < num_polys; i++)
				*nodes++ = (uint32_t)i;
		}
	}
	ubo->num_static_lights = num_polys;
	ubo->lights = buf->address;

	VK_CHECK (vmaFlushAllocation (vk.allocator, buf->allocation, 0, VK_WHOLE_SIZE));
}


/* ==========================================================================
 * vk_testlight
 * ========================================================================== */

static void ParseColor (int first, float intensity, vec3_t color)
{
	int	k;

	for (k = 0; k < 3; k++)
		color[k] = q_max (((Cmd_Argc () > first + k) ? (float)atof (Cmd_Argv (first + k)) : 1.0f) * intensity, 0.0f);
}

static void VK_TestLight_f (void)
{
	const char	*what = (Cmd_Argc () > 1) ? Cmd_Argv (1) : "";
	int		i;

	if (!q_strcasecmp (what, "sphere"))
	{
		test_sphere_t	*s;

		if (!r_scene.worldmodel)
			return;
		if (num_test_spheres == MAX_LIGHT_SOURCES)
		{
			Con_Printf ("vk_testlight: at most %d sphere lights\n", MAX_LIGHT_SOURCES);
			return;
		}
		s = &test_spheres[num_test_spheres++];
		VectorCopy (r_scene.vieworg, s->origin);
		s->radius = (Cmd_Argc () > 2) ? q_max ((float)atof (Cmd_Argv (2)), 0.1f) : 8.0f;
		ParseColor (4, (Cmd_Argc () > 3) ? (float)atof (Cmd_Argv (3)) : 1000.0f, s->color);
		return;
	}
	if (!q_strcasecmp (what, "quad"))
	{
		test_quad_t	*q;
		float		half;
		vec3_t		r, u;

		if (!r_scene.worldmodel)
			return;
		if (num_test_quads == MAX_TEST_QUADS)
		{
			Con_Printf ("vk_testlight: at most %d quads\n", MAX_TEST_QUADS);
			return;
		}
		if (vk_pvs.num_clusters + 1 > MAX_LIGHT_LISTS ||
		    vk_pvs.num_clusters * (num_test_quads + 1) * 2 > MAX_LIGHT_LIST_NODES)
		{
			Con_Printf ("vk_testlight: the light lists of %d clusters have no room for another quad\n",
				    vk_pvs.num_clusters);
			return;
		}
		q = &test_quads[num_test_quads++];
		half = ((Cmd_Argc () > 2) ? q_max ((float)atof (Cmd_Argv (2)), 1.0f) : 32.0f) * 0.5f;
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
		ParseColor (4, (Cmd_Argc () > 3) ? (float)atof (Cmd_Argv (3)) : 50.0f, q->color);
		return;
	}
	if (!q_strcasecmp (what, "clear"))
	{
		num_test_spheres = num_test_quads = 0;
		return;
	}
	if (!q_strcasecmp (what, "list"))
	{
		for (i = 0; i < num_test_spheres; i++)
			Con_Printf ("sphere %2d at %.1f %.1f %.1f radius %.1f color %.1f %.1f %.1f\n", i,
				    test_spheres[i].origin[0], test_spheres[i].origin[1], test_spheres[i].origin[2],
				    test_spheres[i].radius, test_spheres[i].color[0], test_spheres[i].color[1],
				    test_spheres[i].color[2]);
		for (i = 0; i < num_test_quads; i++)
		{
			vec3_t	c, side;

			VectorAdd (test_quads[i].corners[0], test_quads[i].corners[3], c);
			VectorScale (c, 0.5f, c);
			VectorSubtract (test_quads[i].corners[2], test_quads[i].corners[0], side);
			Con_Printf ("quad   %2d at %.1f %.1f %.1f size %.1f color %.1f %.1f %.1f\n", i, c[0], c[1], c[2],
				    VectorLength (side), test_quads[i].color[0], test_quads[i].color[1], test_quads[i].color[2]);
		}
		Con_Printf ("%d sphere lights, %d polygon lights in %d cluster lists\n", num_test_spheres,
			    num_test_quads * 2, vk_pvs.num_clusters);
		return;
	}
	Con_Printf ("vk_testlight sphere [radius] [intensity] [r g b]: a sphere light at the eye (8, 1000, 1 1 1)\n"
		    "vk_testlight quad [size] [intensity] [r g b]: a square light at the eye, facing the view (32, 50, 1 1 1)\n"
		    "vk_testlight list | clear\n");
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
	}
	Cmd_AddCommand ("vk_testlight", VK_TestLight_f);
}

void VK_ShutdownLights (void)
{
	int	i;

	for (i = 0; i < VK_FRAMES_IN_FLIGHT; i++)
		VK_DestroyBuffer (&light_buffers[i]);
	num_test_spheres = num_test_quads = 0;
}
