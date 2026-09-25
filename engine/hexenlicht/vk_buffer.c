/* vk_buffer.c -- GPU buffers and one-time upload commands
 *
 * VK_CreateBuffer/VK_DestroyBuffer wrap VMA. Uploads (textures, the world
 * geometry, the material table) record into one command buffer between
 * VK_BeginUpload and VK_EndUpload, which submits and waits: they happen at
 * load time, outside frames.
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
#include "vk_local.h"

static VkCommandPool	upload_pool;
static VkCommandBuffer	upload_cmd;
static VkFence		upload_fence;


void VK_CreateBuffer (vk_buffer_t *b, VkDeviceSize size, VkBufferUsageFlags usage, vk_memory_t memory)
{
	VkBufferCreateInfo		buffer_info;
	VmaAllocationCreateInfo		alloc_info;
	VmaAllocationInfo		info;
	VkBufferDeviceAddressInfo	address_info;

	memset (b, 0, sizeof(*b));

	memset (&buffer_info, 0, sizeof(buffer_info));
	buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	buffer_info.size = size;
	buffer_info.usage = usage;
	memset (&alloc_info, 0, sizeof(alloc_info));
	switch (memory)
	{
	case VK_MEMORY_UPLOAD:		/* written by the CPU, e.g. staging */
		alloc_info.usage = VMA_MEMORY_USAGE_AUTO;
		alloc_info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
		break;
	case VK_MEMORY_READBACK:	/* written by the GPU, read by the CPU */
		alloc_info.usage = VMA_MEMORY_USAGE_AUTO;
		alloc_info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
		break;
	default:
		alloc_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
		break;
	}
	VK_CHECK (vmaCreateBuffer (vk.allocator, &buffer_info, &alloc_info, &b->buffer, &b->allocation, &info));

	b->size = size;
	b->mapped = info.pMappedData;
	if (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT)
	{
		memset (&address_info, 0, sizeof(address_info));
		address_info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
		address_info.buffer = b->buffer;
		b->address = vkGetBufferDeviceAddress (vk.device, &address_info);
	}
}

void VK_DestroyBuffer (vk_buffer_t *b)
{
	if (b->buffer)
		vmaDestroyBuffer (vk.allocator, b->buffer, b->allocation);
	memset (b, 0, sizeof(*b));
}


/* ==========================================================================
 * One-time upload commands
 * ========================================================================== */

VkCommandBuffer VK_BeginUpload (void)
{
	VkCommandBufferBeginInfo	begin;

	VK_CHECK (vkResetCommandPool (vk.device, upload_pool, 0));
	memset (&begin, 0, sizeof(begin));
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	VK_CHECK (vkBeginCommandBuffer (upload_cmd, &begin));
	return upload_cmd;
}

void VK_EndUpload (void)
{
	VkCommandBufferSubmitInfo	cmd_info;
	VkSubmitInfo2			submit;

	VK_CHECK (vkEndCommandBuffer (upload_cmd));

	memset (&cmd_info, 0, sizeof(cmd_info));
	cmd_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
	cmd_info.commandBuffer = upload_cmd;
	memset (&submit, 0, sizeof(submit));
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
	submit.commandBufferInfoCount = 1;
	submit.pCommandBufferInfos = &cmd_info;
	VK_CHECK (vkQueueSubmit2 (vk.queue, 1, &submit, upload_fence));
	VK_CHECK (vkWaitForFences (vk.device, 1, &upload_fence, VK_TRUE, UINT64_MAX));
	VK_CHECK (vkResetFences (vk.device, 1, &upload_fence));
}

/* Copies data into a device-local buffer through a staging buffer. The
 * caller makes sure no submitted work still uses that part of dst. */
void VK_UploadBuffer (vk_buffer_t *dst, VkDeviceSize offset, const void *data, VkDeviceSize size)
{
	vk_buffer_t		staging;
	VkCommandBuffer		cmd;
	VkBufferCopy		copy;
	VkMemoryBarrier2	barrier;
	VkDependencyInfo	dep;

	if (!size)
		return;

	VK_CreateBuffer (&staging, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_UPLOAD);
	memcpy (staging.mapped, data, (size_t)size);
	VK_CHECK (vmaFlushAllocation (vk.allocator, staging.allocation, 0, VK_WHOLE_SIZE));

	cmd = VK_BeginUpload ();

	memset (&copy, 0, sizeof(copy));
	copy.dstOffset = offset;
	copy.size = size;
	vkCmdCopyBuffer (cmd, staging.buffer, dst->buffer, 1, &copy);

	/* visible to whatever reads the buffer next: shaders, AS builds */
	memset (&barrier, 0, sizeof(barrier));
	barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
	barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
	barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
	barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
	barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT;
	memset (&dep, 0, sizeof(dep));
	dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
	dep.memoryBarrierCount = 1;
	dep.pMemoryBarriers = &barrier;
	vkCmdPipelineBarrier2 (cmd, &dep);

	VK_EndUpload ();
	VK_DestroyBuffer (&staging);
}


/* ==========================================================================
 * Init / shutdown
 * ========================================================================== */

void VK_InitBuffers (void)
{
	VkCommandPoolCreateInfo		pool_info;
	VkCommandBufferAllocateInfo	cmd_info;
	VkFenceCreateInfo		fence_info;

	memset (&pool_info, 0, sizeof(pool_info));
	pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
	pool_info.queueFamilyIndex = vk.queue_family;
	VK_CHECK (vkCreateCommandPool (vk.device, &pool_info, NULL, &upload_pool));

	memset (&cmd_info, 0, sizeof(cmd_info));
	cmd_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	cmd_info.commandPool = upload_pool;
	cmd_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	cmd_info.commandBufferCount = 1;
	VK_CHECK (vkAllocateCommandBuffers (vk.device, &cmd_info, &upload_cmd));

	memset (&fence_info, 0, sizeof(fence_info));
	fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	VK_CHECK (vkCreateFence (vk.device, &fence_info, NULL, &upload_fence));
}

void VK_ShutdownBuffers (void)
{
	if (upload_fence)
		vkDestroyFence (vk.device, upload_fence, NULL);
	if (upload_pool)
		vkDestroyCommandPool (vk.device, upload_pool, NULL);
	upload_fence = VK_NULL_HANDLE;
	upload_pool = VK_NULL_HANDLE;
	upload_cmd = VK_NULL_HANDLE;
}
