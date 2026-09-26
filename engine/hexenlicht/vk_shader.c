/* vk_shader.c -- loading SPIR-V shader modules for Hexenlicht
 *
 * Shaders are compiled at build time (cmake/HexenlichtShaders.cmake) into
 * a "shaders" folder next to the executable and loaded from there, so a
 * rebuilt shader only needs the program restarted, not relinked. VK_ExePath
 * gives the path of other files there (the blue noise, vk_images.c).
 *
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
#include "winquake.h"
#include "vk_local.h"

#define SPIRV_MAGIC	0x07230203u

/* <folder of hexenlicht.exe>\<file> */
void VK_ExePath (const char *file, char *path, size_t size)
{
	char	*slash;
	DWORD	len;

	len = GetModuleFileNameA (NULL, path, (DWORD)size);
	if (len == 0 || len >= size)
		Sys_Error ("%s: can't get the executable's path", __thisfunc__);
	slash = strrchr (path, '\\');
	if (slash)
		slash[1] = '\0';
	else
		path[0] = '\0';
	q_strlcat (path, file, size);
}

/* <folder of hexenlicht.exe>\shaders\<name>.spv */
static void VK_ShaderPath (const char *name, char *path, size_t size)
{
	VK_ExePath ("shaders\\", path, size);
	q_strlcat (path, name, size);
	q_strlcat (path, ".spv", size);
}

VkShaderModule VK_LoadShader (const char *name)
{
	char			path[MAX_OSPATH];
	FILE			*f;
	long			size;
	uint32_t		*code;
	VkShaderModuleCreateInfo info;
	VkShaderModule		module;

	VK_ShaderPath (name, path, sizeof(path));

	f = fopen (path, "rb");
	if (!f)
		Sys_Error ("Shader %s not found.\n\nThe shaders folder must be next to "
			   "hexenlicht.exe (it is created when building the hexenlicht target).",
			   path);
	fseek (f, 0, SEEK_END);
	size = ftell (f);
	fseek (f, 0, SEEK_SET);
	if (size <= 0 || (size % 4) != 0)
	{
		fclose (f);
		Sys_Error ("Shader %s is not valid SPIR-V (size %ld)", path, size);
	}
	code = (uint32_t *) malloc (size);
	if (!code)
		Sys_Error ("%s: out of memory", __thisfunc__);
	if (fread (code, 1, size, f) != (size_t)size)
	{
		fclose (f);
		Sys_Error ("Couldn't read shader %s", path);
	}
	fclose (f);
	if (code[0] != SPIRV_MAGIC)
		Sys_Error ("Shader %s is not valid SPIR-V", path);

	memset (&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	info.codeSize = (size_t)size;
	info.pCode = code;
	VK_CHECK (vkCreateShaderModule (vk.device, &info, NULL, &module));
	free (code);

	Con_DPrintf ("Loaded shader %s.spv (%ld bytes)\n", name, size);
	return module;
}
