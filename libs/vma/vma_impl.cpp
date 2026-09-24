/* vma_impl.cpp -- compiles the Vulkan Memory Allocator implementation
 * for Hexenlicht. (This file is part of Hexenlicht, not of VMA.)
 *
 * Vulkan functions come from volk (VK_NO_PROTOTYPES), so VMA must not
 * reference them statically; the allocator is created with
 * VmaVulkanFunctions::vkGetInstanceProcAddr / vkGetDeviceProcAddr taken
 * from volk and fetches everything else itself.
 *
 * Copyright (C) 2026  Hexenlicht contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "volk.h"

#define VMA_IMPLEMENTATION
#include "vk_mem_alloc.h"
