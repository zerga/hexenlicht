/*
Copyright (C) 2021, NVIDIA CORPORATION. All rights reserved.
Copyright (C) 2026  Hexenlicht contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

/* Hexenlicht: from Quake II RTX, with an include guard that defines its
 * macro, tagged C structs (so C headers can declare pointers to them) and
 * DeviceAddress. Shaders are compiled with -DVKPT_SHADER. */

#ifndef SHADER_STRUCTS_H
#define SHADER_STRUCTS_H

#ifdef VKPT_SHADER

#define BEGIN_SHADER_STRUCT(NAME) struct NAME
#define END_SHADER_STRUCT(NAME) ;

/* Hexenlicht: a buffer device address; uvec2 needs no 64-bit integers in
 * shaders (GL_EXT_buffer_reference_uvec2 turns it into a buffer reference) */
#define DeviceAddress uvec2

#else // VKPT_SHADER

#include <stdint.h>

#define BEGIN_SHADER_STRUCT(NAME) typedef struct NAME
#define END_SHADER_STRUCT(NAME) NAME;

typedef uint32_t uint;
typedef float vec2[2];
typedef float vec3[3];
typedef float vec4[4];
typedef uint32_t uvec2[2];
typedef uint32_t uvec3[3];
typedef uint32_t uvec4[4];
typedef int ivec2[2];
typedef int ivec3[3];
typedef int ivec4[4];
typedef float mat3[3][3];
typedef float mat4[4][4];	/* Hexenlicht: [column][row], as GLSL stores it */
typedef uint64_t DeviceAddress;	/* Hexenlicht */

#endif // VKPT_SHADER

#endif // SHADER_STRUCTS_H
