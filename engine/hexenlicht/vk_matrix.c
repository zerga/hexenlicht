/* vk_matrix.c -- the camera's view and projection matrices
 *
 * From Quake II RTX (src/refresh/vkpt/matrix.c): 4x4 matrices stored in
 * columns (m[column * 4 + row]), as GLSL reads them from the global UBO.
 * The view space has x right, y up and z forward; the projection flips y
 * for Vulkan's clip space, so screen y grows downwards.
 *
 * Copyright (C) 2018 Christoph Schied
 * Copyright (C) 2003-2006 Andrey Nazarov
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

/* world to view, from the camera's position and axes (AngleVectors'),
 * Quake II RTX's create_view_matrix */
void VK_CreateViewMatrix (float m[16], const vec3_t origin, const vec3_t forward, const vec3_t right, const vec3_t up)
{
	m[0]  = right[0];
	m[4]  = right[1];
	m[8]  = right[2];
	m[12] = -DotProduct (right, origin);

	m[1]  = up[0];
	m[5]  = up[1];
	m[9]  = up[2];
	m[13] = -DotProduct (up, origin);

	m[2]  = forward[0];
	m[6]  = forward[1];
	m[10] = forward[2];
	m[14] = -DotProduct (forward, origin);

	m[3]  = 0.0f;
	m[7]  = 0.0f;
	m[11] = 0.0f;
	m[15] = 1.0f;
}

/* view to clip space for field of views fov_x x fov_y (degrees) */
void VK_CreateProjectionMatrix (float m[16], float znear, float zfar, float fov_x, float fov_y)
{
	float	xmin, xmax, ymin, ymax;
	float	width, height, depth;

	ymax = znear * (float)tan (fov_y * M_PI / 360.0);
	ymin = -ymax;

	xmax = znear * (float)tan (fov_x * M_PI / 360.0);
	xmin = -xmax;

	width = xmax - xmin;
	height = ymax - ymin;
	depth = zfar - znear;

	m[0] = 2 * znear / width;
	m[4] = 0;
	m[8] = (xmax + xmin) / width;
	m[12] = 0;

	m[1] = 0;
	m[5] = -2 * znear / height;
	m[9] = (ymax + ymin) / height;
	m[13] = 0;

	m[2] = 0;
	m[6] = 0;
	m[10] = (zfar + znear) / depth;
	m[14] = 2 * zfar * znear / depth;

	m[3] = 0;
	m[7] = 0;
	m[11] = 1;
	m[15] = 0;
}

void VK_InverseMatrix (const float m[16], float inv[16])
{
	float	det;
	int	i;

	inv[0] = m[5]  * m[10] * m[15] -
	         m[5]  * m[11] * m[14] -
	         m[9]  * m[6]  * m[15] +
	         m[9]  * m[7]  * m[14] +
	         m[13] * m[6]  * m[11] -
	         m[13] * m[7]  * m[10];

	inv[1] = -m[1]  * m[10] * m[15] +
	          m[1]  * m[11] * m[14] +
	          m[9]  * m[2] * m[15] -
	          m[9]  * m[3] * m[14] -
	          m[13] * m[2] * m[11] +
	          m[13] * m[3] * m[10];

	inv[2] = m[1]  * m[6] * m[15] -
	         m[1]  * m[7] * m[14] -
	         m[5]  * m[2] * m[15] +
	         m[5]  * m[3] * m[14] +
	         m[13] * m[2] * m[7] -
	         m[13] * m[3] * m[6];

	inv[3] = -m[1] * m[6] * m[11] +
	          m[1] * m[7] * m[10] +
	          m[5] * m[2] * m[11] -
	          m[5] * m[3] * m[10] -
	          m[9] * m[2] * m[7] +
	          m[9] * m[3] * m[6];

	inv[4] = -m[4]  * m[10] * m[15] +
	          m[4]  * m[11] * m[14] +
	          m[8]  * m[6]  * m[15] -
	          m[8]  * m[7]  * m[14] -
	          m[12] * m[6]  * m[11] +
	          m[12] * m[7]  * m[10];

	inv[5] = m[0]  * m[10] * m[15] -
	         m[0]  * m[11] * m[14] -
	         m[8]  * m[2] * m[15] +
	         m[8]  * m[3] * m[14] +
	         m[12] * m[2] * m[11] -
	         m[12] * m[3] * m[10];

	inv[6] = -m[0]  * m[6] * m[15] +
	          m[0]  * m[7] * m[14] +
	          m[4]  * m[2] * m[15] -
	          m[4]  * m[3] * m[14] -
	          m[12] * m[2] * m[7] +
	          m[12] * m[3] * m[6];

	inv[7] = m[0] * m[6] * m[11] -
	         m[0] * m[7] * m[10] -
	         m[4] * m[2] * m[11] +
	         m[4] * m[3] * m[10] +
	         m[8] * m[2] * m[7] -
	         m[8] * m[3] * m[6];

	inv[8] = m[4]  * m[9] * m[15] -
	         m[4]  * m[11] * m[13] -
	         m[8]  * m[5] * m[15] +
	         m[8]  * m[7] * m[13] +
	         m[12] * m[5] * m[11] -
	         m[12] * m[7] * m[9];

	inv[9] = -m[0]  * m[9] * m[15] +
	          m[0]  * m[11] * m[13] +
	          m[8]  * m[1] * m[15] -
	          m[8]  * m[3] * m[13] -
	          m[12] * m[1] * m[11] +
	          m[12] * m[3] * m[9];

	inv[10] = m[0]  * m[5] * m[15] -
	          m[0]  * m[7] * m[13] -
	          m[4]  * m[1] * m[15] +
	          m[4]  * m[3] * m[13] +
	          m[12] * m[1] * m[7] -
	          m[12] * m[3] * m[5];

	inv[11] = -m[0] * m[5] * m[11] +
	           m[0] * m[7] * m[9] +
	           m[4] * m[1] * m[11] -
	           m[4] * m[3] * m[9] -
	           m[8] * m[1] * m[7] +
	           m[8] * m[3] * m[5];

	inv[12] = -m[4]  * m[9] * m[14] +
	           m[4]  * m[10] * m[13] +
	           m[8]  * m[5] * m[14] -
	           m[8]  * m[6] * m[13] -
	           m[12] * m[5] * m[10] +
	           m[12] * m[6] * m[9];

	inv[13] = m[0]  * m[9] * m[14] -
	          m[0]  * m[10] * m[13] -
	          m[8]  * m[1] * m[14] +
	          m[8]  * m[2] * m[13] +
	          m[12] * m[1] * m[10] -
	          m[12] * m[2] * m[9];

	inv[14] = -m[0]  * m[5] * m[14] +
	           m[0]  * m[6] * m[13] +
	           m[4]  * m[1] * m[14] -
	           m[4]  * m[2] * m[13] -
	           m[12] * m[1] * m[6] +
	           m[12] * m[2] * m[5];

	inv[15] = m[0] * m[5] * m[10] -
	          m[0] * m[6] * m[9] -
	          m[4] * m[1] * m[10] +
	          m[4] * m[2] * m[9] +
	          m[8] * m[1] * m[6] -
	          m[8] * m[2] * m[5];

	det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];

	det = 1.0f / det;

	for (i = 0; i < 16; i++)
		inv[i] = inv[i] * det;
}
