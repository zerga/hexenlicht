/* vk_testpattern.c -- placeholder screen until the 2D renderer (story 1.6)
 *
 * A fullscreen triangle with a slowly moving gradient. It exercises the
 * whole shader path: build-time SPIR-V, runtime loading, a graphics
 * pipeline with dynamic rendering and push constants. Remove it when the
 * 2D renderer draws the real screen.
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

static VkPipelineLayout	testpattern_layout;
static VkPipeline	testpattern_pipeline;
static VkFormat		testpattern_format;	/* color format the pipeline was made for */

static void VK_CreateTestPattern (void)
{
	VkShaderModule				vert, frag;
	VkPipelineShaderStageCreateInfo		stages[2];
	VkPipelineVertexInputStateCreateInfo	vertex_input;
	VkPipelineInputAssemblyStateCreateInfo	input_assembly;
	VkPipelineViewportStateCreateInfo	viewport;
	VkPipelineRasterizationStateCreateInfo	raster;
	VkPipelineMultisampleStateCreateInfo	multisample;
	VkPipelineColorBlendAttachmentState	blend_attachment;
	VkPipelineColorBlendStateCreateInfo	blend;
	VkPipelineDynamicStateCreateInfo	dynamic;
	VkPipelineRenderingCreateInfo		rendering;
	VkPipelineLayoutCreateInfo		layout_info;
	VkPushConstantRange			push_range;
	VkGraphicsPipelineCreateInfo		info;
	const VkDynamicState	dynamic_states[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };

	vert = VK_LoadShader ("fullscreen.vert");
	frag = VK_LoadShader ("testpattern.frag");

	memset (&push_range, 0, sizeof(push_range));
	push_range.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	push_range.size = sizeof(float);

	memset (&layout_info, 0, sizeof(layout_info));
	layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	layout_info.pushConstantRangeCount = 1;
	layout_info.pPushConstantRanges = &push_range;
	VK_CHECK (vkCreatePipelineLayout (vk.device, &layout_info, NULL, &testpattern_layout));

	memset (stages, 0, sizeof(stages));
	stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = vert;
	stages[0].pName = "main";
	stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = frag;
	stages[1].pName = "main";

	memset (&vertex_input, 0, sizeof(vertex_input));
	vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

	memset (&input_assembly, 0, sizeof(input_assembly));
	input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

	memset (&viewport, 0, sizeof(viewport));
	viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	viewport.viewportCount = 1;
	viewport.scissorCount = 1;

	memset (&raster, 0, sizeof(raster));
	raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	raster.polygonMode = VK_POLYGON_MODE_FILL;
	raster.cullMode = VK_CULL_MODE_NONE;
	raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	raster.lineWidth = 1.0f;

	memset (&multisample, 0, sizeof(multisample));
	multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

	memset (&blend_attachment, 0, sizeof(blend_attachment));
	blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
					  VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
	memset (&blend, 0, sizeof(blend));
	blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	blend.attachmentCount = 1;
	blend.pAttachments = &blend_attachment;

	memset (&dynamic, 0, sizeof(dynamic));
	dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dynamic.dynamicStateCount = Q_COUNTOF(dynamic_states);
	dynamic.pDynamicStates = dynamic_states;

	testpattern_format = vk.surface_format.format;
	memset (&rendering, 0, sizeof(rendering));
	rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
	rendering.colorAttachmentCount = 1;
	rendering.pColorAttachmentFormats = &testpattern_format;

	memset (&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	info.pNext = &rendering;
	info.stageCount = 2;
	info.pStages = stages;
	info.pVertexInputState = &vertex_input;
	info.pInputAssemblyState = &input_assembly;
	info.pViewportState = &viewport;
	info.pRasterizationState = &raster;
	info.pMultisampleState = &multisample;
	info.pColorBlendState = &blend;
	info.pDynamicState = &dynamic;
	info.layout = testpattern_layout;
	VK_CHECK (vkCreateGraphicsPipelines (vk.device, VK_NULL_HANDLE, 1, &info, NULL, &testpattern_pipeline));

	vkDestroyShaderModule (vk.device, vert, NULL);
	vkDestroyShaderModule (vk.device, frag, NULL);
}

void VK_ShutdownTestPattern (void)
{
	if (testpattern_pipeline)
		vkDestroyPipeline (vk.device, testpattern_pipeline, NULL);
	if (testpattern_layout)
		vkDestroyPipelineLayout (vk.device, testpattern_layout, NULL);
	testpattern_pipeline = VK_NULL_HANDLE;
	testpattern_layout = VK_NULL_HANDLE;
}

void VK_DrawTestPattern (float time)
{
	VkCommandBuffer	cmd;

	if (!vk.frame_active)
		return;

	if (testpattern_pipeline && testpattern_format != vk.surface_format.format)
	{
		vkDeviceWaitIdle (vk.device);
		VK_ShutdownTestPattern ();
	}
	if (!testpattern_pipeline)
		VK_CreateTestPattern ();

	cmd = vk.frames[vk.frame_index].cmd;
	VK_BeginSwapchainRendering (VK_ATTACHMENT_LOAD_OP_DONT_CARE);	/* covers the screen */
	vkCmdBindPipeline (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, testpattern_pipeline);
	vkCmdPushConstants (cmd, testpattern_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(time), &time);
	vkCmdDraw (cmd, 3, 1, 0, 0);
	VK_EndSwapchainRendering ();
}
