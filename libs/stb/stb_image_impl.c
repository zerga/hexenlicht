/* stb_image_impl.c -- compiles the stb_image implementation for
 * Hexenlicht. (This file is part of Hexenlicht, not of stb.)
 *
 * Only the formats of the material format's authoring files are enabled
 * (docs/hexenlicht/PLAN.md, section 6): PNG and TGA. STBI_NO_STDIO is
 * set for all users of the header by CMake: the engine reads files
 * through its own filesystem (pak files), so only the *_from_memory and
 * callback loaders exist.
 *
 * Copyright (C) 2026  Hexenlicht contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#define STBI_ONLY_PNG
#define STBI_ONLY_TGA
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
