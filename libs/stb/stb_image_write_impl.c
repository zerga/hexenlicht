/* stb_image_write_impl.c -- compiles the stb_image_write implementation
 * for Hexenlicht. (This file is part of Hexenlicht, not of stb.)
 *
 * The texture export (engine/hexenlicht/vk_export.c, story 5.4) writes
 * PNGs with stbi_write_png_to_func into files it opens itself (the game
 * folder's OS path): STBI_WRITE_NO_STDIO is set for all users of the
 * header by CMake, so the file writers don't exist.
 *
 * Copyright (C) 2026  Hexenlicht contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
