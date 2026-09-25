/* vk_core.c -- Vulkan instance, device and memory allocator for Hexenlicht
 *
 * Vulkan functions are loaded at runtime with volk, so hexenlicht.exe has
 * no link-time dependency on vulkan-1.dll.
 *
 * The device must support Vulkan 1.3 and hardware ray tracing (acceleration
 * structures and ray queries); see docs/hexenlicht/PLAN.md. Ray tracing
 * pipelines, shader execution reordering and position fetch are enabled
 * when present.
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

vk_state_t	vk;

#define VALIDATION_LAYER	"VK_LAYER_KHRONOS_validation"

static const char *const required_device_extensions[] = {
	VK_KHR_SWAPCHAIN_EXTENSION_NAME,
	VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
	VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,	/* required by acceleration_structure */
	VK_KHR_RAY_QUERY_EXTENSION_NAME
};
#define NUM_REQUIRED_DEVICE_EXTENSIONS	Q_COUNTOF(required_device_extensions)

const char *VK_ResultString (VkResult result)
{
	switch (result)
	{
	case VK_SUCCESS:			return "VK_SUCCESS";
	case VK_NOT_READY:			return "VK_NOT_READY";
	case VK_TIMEOUT:			return "VK_TIMEOUT";
	case VK_INCOMPLETE:			return "VK_INCOMPLETE";
	case VK_SUBOPTIMAL_KHR:			return "VK_SUBOPTIMAL_KHR";
	case VK_ERROR_OUT_OF_HOST_MEMORY:	return "VK_ERROR_OUT_OF_HOST_MEMORY";
	case VK_ERROR_OUT_OF_DEVICE_MEMORY:	return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
	case VK_ERROR_INITIALIZATION_FAILED:	return "VK_ERROR_INITIALIZATION_FAILED";
	case VK_ERROR_DEVICE_LOST:		return "VK_ERROR_DEVICE_LOST";
	case VK_ERROR_LAYER_NOT_PRESENT:	return "VK_ERROR_LAYER_NOT_PRESENT";
	case VK_ERROR_EXTENSION_NOT_PRESENT:	return "VK_ERROR_EXTENSION_NOT_PRESENT";
	case VK_ERROR_FEATURE_NOT_PRESENT:	return "VK_ERROR_FEATURE_NOT_PRESENT";
	case VK_ERROR_INCOMPATIBLE_DRIVER:	return "VK_ERROR_INCOMPATIBLE_DRIVER";
	case VK_ERROR_SURFACE_LOST_KHR:		return "VK_ERROR_SURFACE_LOST_KHR";
	case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR:	return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
	case VK_ERROR_OUT_OF_DATE_KHR:		return "VK_ERROR_OUT_OF_DATE_KHR";
	default:				return va("VkResult %d", (int)result);
	}
}


/* ==========================================================================
 * Validation messages -> console
 * ========================================================================== */

static VKAPI_ATTR VkBool32 VKAPI_CALL VK_DebugCallback (
		VkDebugUtilsMessageSeverityFlagBitsEXT severity,
		VkDebugUtilsMessageTypeFlagsEXT types,
		const VkDebugUtilsMessengerCallbackDataEXT *data,
		void *user_data)
{
	(void)types; (void)user_data;

	if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
	{
		vk.validation_errors++;
		Con_Printf ("Vulkan validation error: %s\n", data->pMessage);
	}
	else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
	{
		vk.validation_warnings++;
		Con_Printf ("Vulkan validation warning: %s\n", data->pMessage);
	}
	return VK_FALSE;
}

static void VK_FillDebugMessengerInfo (VkDebugUtilsMessengerCreateInfoEXT *info)
{
	memset (info, 0, sizeof(*info));
	info->sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
	info->messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
				VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
	info->messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
			    VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
			    VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
	info->pfnUserCallback = VK_DebugCallback;
}


/* ==========================================================================
 * Instance
 * ========================================================================== */

static qboolean VK_HasInstanceLayer (const char *name)
{
	uint32_t		i, count = 0;
	VkLayerProperties	*layers;
	qboolean		found = false;

	vkEnumerateInstanceLayerProperties (&count, NULL);
	if (!count)
		return false;
	layers = (VkLayerProperties *) malloc (count * sizeof(*layers));
	if (!layers)
		return false;
	vkEnumerateInstanceLayerProperties (&count, layers);
	for (i = 0; i < count && !found; i++)
		found = !strcmp (layers[i].layerName, name);
	free (layers);
	return found;
}

static void VK_CreateInstance (void)
{
	VkApplicationInfo	app;
	VkInstanceCreateInfo	info;
	VkDebugUtilsMessengerCreateInfoEXT debug_info;
	const char		*extensions[3];
	const char		*layers[1];
	uint32_t		num_extensions = 0, api_version = 0;

	if (vkEnumerateInstanceVersion)
		vkEnumerateInstanceVersion (&api_version);
	if (api_version < VK_API_VERSION_1_3)
		Sys_Error ("Hexenlicht needs Vulkan 1.3, but the installed Vulkan runtime "
			   "only supports %u.%u.\nUpdate the graphics driver.",
			   VK_API_VERSION_MAJOR(api_version), VK_API_VERSION_MINOR(api_version));

	/* validation: on in debug builds, unless -novalidation; -validation forces it on */
#if defined(DEBUG_BUILD)
	vk.validation = !COM_CheckParm ("-novalidation");
#else
	vk.validation = COM_CheckParm ("-validation") != 0;
#endif
	if (vk.validation && !VK_HasInstanceLayer (VALIDATION_LAYER))
	{
		Con_Printf ("Vulkan: %s not found (install the Vulkan SDK), validation disabled\n", VALIDATION_LAYER);
		vk.validation = false;
	}

	extensions[num_extensions++] = VK_KHR_SURFACE_EXTENSION_NAME;
	extensions[num_extensions++] = VK_KHR_WIN32_SURFACE_EXTENSION_NAME;
	if (vk.validation)
		extensions[num_extensions++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
	layers[0] = VALIDATION_LAYER;

	memset (&app, 0, sizeof(app));
	app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	app.pApplicationName = "Hexenlicht";
	app.applicationVersion = VK_MAKE_API_VERSION(0, HOT_VERSION_MAJ, HOT_VERSION_MID, HOT_VERSION_MIN);
	app.pEngineName = "Hammer of Thyrion";
	app.engineVersion = app.applicationVersion;
	app.apiVersion = VK_API_VERSION_1_3;

	memset (&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	info.pApplicationInfo = &app;
	info.enabledExtensionCount = num_extensions;
	info.ppEnabledExtensionNames = extensions;
	if (vk.validation)
	{
		info.enabledLayerCount = 1;
		info.ppEnabledLayerNames = layers;
		/* also report problems in vkCreateInstance/vkDestroyInstance */
		VK_FillDebugMessengerInfo (&debug_info);
		info.pNext = &debug_info;
	}

	VK_CHECK (vkCreateInstance (&info, NULL, &vk.instance));
	volkLoadInstanceOnly (vk.instance);

	if (vk.validation)
	{
		VK_FillDebugMessengerInfo (&debug_info);
		VK_CHECK (vkCreateDebugUtilsMessengerEXT (vk.instance, &debug_info, NULL, &vk.messenger));
		Con_Printf ("Vulkan: validation layer enabled\n");
	}
}


/* ==========================================================================
 * Physical device selection
 * ========================================================================== */

typedef struct
{
	VkPhysicalDeviceFeatures2				core;
	VkPhysicalDeviceVulkan12Features			v12;
	VkPhysicalDeviceVulkan13Features			v13;
	VkPhysicalDeviceAccelerationStructureFeaturesKHR	as;
	VkPhysicalDeviceRayQueryFeaturesKHR			rq;
	VkPhysicalDeviceRayTracingPipelineFeaturesKHR		rtp;
	VkPhysicalDeviceRayTracingInvocationReorderFeaturesNV	ser;
	VkPhysicalDeviceRayTracingPositionFetchFeaturesKHR	pf;
} vk_features_t;

/* link the structures into a pNext chain; the optional ones only when
 * their extension is present (has_* flags) */
static void VK_ChainFeatures (vk_features_t *f, qboolean has_rtp, qboolean has_ser, qboolean has_pf)
{
	void	**next;

	f->core.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
	f->v12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
	f->v13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
	f->as.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
	f->rq.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;
	f->rtp.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR;
	f->ser.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_INVOCATION_REORDER_FEATURES_NV;
	f->pf.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_POSITION_FETCH_FEATURES_KHR;

	f->core.pNext = &f->v12;
	f->v12.pNext = &f->v13;
	f->v13.pNext = &f->as;
	f->as.pNext = &f->rq;
	next = &f->rq.pNext;
	*next = NULL;
	if (has_rtp) { *next = &f->rtp; next = &f->rtp.pNext; *next = NULL; }
	if (has_ser) { *next = &f->ser; next = &f->ser.pNext; *next = NULL; }
	if (has_pf)  { *next = &f->pf;  next = &f->pf.pNext;  *next = NULL; }
}

typedef struct
{
	VkPhysicalDevice	device;
	VkPhysicalDeviceProperties props;
	uint32_t		queue_family;
	qboolean		has_rtp, has_ser, has_pf;
	vk_features_t		supported;
	char			missing[256];	/* why it can't be used, empty if it can */
} vk_candidate_t;

static qboolean VK_HasDeviceExtension (const VkExtensionProperties *exts, uint32_t count, const char *name)
{
	uint32_t	i;

	for (i = 0; i < count; i++)
	{
		if (!strcmp (exts[i].extensionName, name))
			return true;
	}
	return false;
}

static void VK_AddMissing (vk_candidate_t *c, const char *what)
{
	if (c->missing[0])
		q_strlcat (c->missing, ", ", sizeof(c->missing));
	q_strlcat (c->missing, what, sizeof(c->missing));
}

static void VK_CheckDevice (VkPhysicalDevice dev, vk_candidate_t *c)
{
	uint32_t		i, num_exts = 0, num_families = 0;
	VkExtensionProperties	*exts;
	VkQueueFamilyProperties	*families;
	VkBool32		present;
	vk_features_t		*f = &c->supported;

	memset (c, 0, sizeof(*c));
	c->device = dev;
	c->queue_family = UINT32_MAX;
	vkGetPhysicalDeviceProperties (dev, &c->props);

	if (c->props.apiVersion < VK_API_VERSION_1_3)
		VK_AddMissing (c, "Vulkan 1.3");

	/* extensions */
	vkEnumerateDeviceExtensionProperties (dev, NULL, &num_exts, NULL);
	exts = (VkExtensionProperties *) malloc ((num_exts ? num_exts : 1) * sizeof(*exts));
	if (!exts)
		Sys_Error ("%s: out of memory", __thisfunc__);
	vkEnumerateDeviceExtensionProperties (dev, NULL, &num_exts, exts);
	for (i = 0; i < NUM_REQUIRED_DEVICE_EXTENSIONS; i++)
	{
		if (!VK_HasDeviceExtension (exts, num_exts, required_device_extensions[i]))
			VK_AddMissing (c, required_device_extensions[i]);
	}
	c->has_rtp = VK_HasDeviceExtension (exts, num_exts, VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME);
	c->has_ser = c->has_rtp && VK_HasDeviceExtension (exts, num_exts, VK_NV_RAY_TRACING_INVOCATION_REORDER_EXTENSION_NAME);
	c->has_pf = VK_HasDeviceExtension (exts, num_exts, VK_KHR_RAY_TRACING_POSITION_FETCH_EXTENSION_NAME);
	free (exts);
	if (c->missing[0])
		return;		/* feature structs of missing extensions can't be queried */

	/* features */
	VK_ChainFeatures (f, c->has_rtp, c->has_ser, c->has_pf);
	vkGetPhysicalDeviceFeatures2 (dev, &f->core);
	if (!f->v12.bufferDeviceAddress)	VK_AddMissing (c, "bufferDeviceAddress");
	if (!f->v12.descriptorIndexing)		VK_AddMissing (c, "descriptorIndexing");
	if (!f->v12.runtimeDescriptorArray)	VK_AddMissing (c, "runtimeDescriptorArray");
	if (!f->v12.descriptorBindingPartiallyBound)		VK_AddMissing (c, "descriptorBindingPartiallyBound");
	if (!f->v12.descriptorBindingVariableDescriptorCount)	VK_AddMissing (c, "descriptorBindingVariableDescriptorCount");
	if (!f->v12.shaderSampledImageArrayNonUniformIndexing)	VK_AddMissing (c, "shaderSampledImageArrayNonUniformIndexing");
	if (!f->v12.descriptorBindingSampledImageUpdateAfterBind)	VK_AddMissing (c, "descriptorBindingSampledImageUpdateAfterBind");
	if (!f->v12.descriptorBindingUpdateUnusedWhilePending)	VK_AddMissing (c, "descriptorBindingUpdateUnusedWhilePending");
	if (!f->v12.timelineSemaphore)		VK_AddMissing (c, "timelineSemaphore");
	if (!f->v12.scalarBlockLayout)		VK_AddMissing (c, "scalarBlockLayout");
	if (!f->v13.dynamicRendering)		VK_AddMissing (c, "dynamicRendering");
	if (!f->v13.synchronization2)		VK_AddMissing (c, "synchronization2");
	if (!f->v13.maintenance4)		VK_AddMissing (c, "maintenance4");
	if (!f->v13.shaderDemoteToHelperInvocation)	VK_AddMissing (c, "shaderDemoteToHelperInvocation");
	if (!f->as.accelerationStructure)	VK_AddMissing (c, "accelerationStructure");
	if (!f->rq.rayQuery)			VK_AddMissing (c, "rayQuery");
	c->has_rtp = c->has_rtp && f->rtp.rayTracingPipeline;
	c->has_ser = c->has_ser && f->ser.rayTracingInvocationReorder;
	c->has_pf = c->has_pf && f->pf.rayTracingPositionFetch;

	/* one queue family for graphics, compute and presenting */
	vkGetPhysicalDeviceQueueFamilyProperties (dev, &num_families, NULL);
	families = (VkQueueFamilyProperties *) malloc ((num_families ? num_families : 1) * sizeof(*families));
	if (!families)
		Sys_Error ("%s: out of memory", __thisfunc__);
	vkGetPhysicalDeviceQueueFamilyProperties (dev, &num_families, families);
	for (i = 0; i < num_families; i++)
	{
		const VkQueueFlags need = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
		if ((families[i].queueFlags & need) != need)
			continue;
		present = VK_FALSE;
		vkGetPhysicalDeviceSurfaceSupportKHR (dev, i, vk.surface, &present);
		if (present)
		{
			c->queue_family = i;
			break;
		}
	}
	free (families);
	if (c->queue_family == UINT32_MAX)
		VK_AddMissing (c, "a graphics queue that can present to the window");
}

static void VK_SelectPhysicalDevice (vk_candidate_t *chosen)
{
	uint32_t		i, count = 0;
	VkPhysicalDevice	*devices;
	vk_candidate_t		c;
	int			forced, best = -1, best_score = -1;

	vkEnumeratePhysicalDevices (vk.instance, &count, NULL);
	if (!count)
		Sys_Error ("No Vulkan devices found.\nUpdate the graphics driver.");
	devices = (VkPhysicalDevice *) malloc (count * sizeof(*devices));
	if (!devices)
		Sys_Error ("%s: out of memory", __thisfunc__);
	vkEnumeratePhysicalDevices (vk.instance, &count, devices);

	/* -vkdevice <n> picks a device by its index in the list below */
	forced = COM_CheckParm ("-vkdevice");
	forced = (forced && forced < com_argc - 1) ? atoi (com_argv[forced + 1]) : -1;

	for (i = 0; i < count; i++)
	{
		int	score;

		VK_CheckDevice (devices[i], &c);
		Con_Printf ("Vulkan device %u: %s%s%s\n", i, c.props.deviceName,
				c.missing[0] ? " - unusable, missing: " : "", c.missing);
		if (c.missing[0])
			continue;
		score = (c.props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) ? 2 :
			(c.props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) ? 1 : 0;
		if ((int)i == forced)
			score = 100;
		if (score > best_score)
		{
			best = (int)i;
			best_score = score;
			*chosen = c;
		}
	}

	if (best < 0)
	{
		VK_CheckDevice (devices[0], &c);
		free (devices);
		Sys_Error ("No Vulkan device with hardware ray tracing found.\n\n"
			   "Hexenlicht needs a GPU with Vulkan 1.3 and ray tracing support "
			   "(e.g. NVIDIA GeForce RTX, AMD Radeon RX 6000 or newer). "
			   "%s is missing:\n%s", c.props.deviceName, c.missing);
	}
	free (devices);
}


/* ==========================================================================
 * Logical device and allocator
 * ========================================================================== */

static void VK_CreateDevice (const vk_candidate_t *c)
{
	VkDeviceCreateInfo	info;
	VkDeviceQueueCreateInfo	queue_info;
	vk_features_t		enable;
	const char		*extensions[NUM_REQUIRED_DEVICE_EXTENSIONS + 3];
	uint32_t		i, num_extensions = 0;
	const float		priority = 1.0f;

	for (i = 0; i < NUM_REQUIRED_DEVICE_EXTENSIONS; i++)
		extensions[num_extensions++] = required_device_extensions[i];
	if (c->has_rtp)
		extensions[num_extensions++] = VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME;
	if (c->has_ser)
		extensions[num_extensions++] = VK_NV_RAY_TRACING_INVOCATION_REORDER_EXTENSION_NAME;
	if (c->has_pf)
		extensions[num_extensions++] = VK_KHR_RAY_TRACING_POSITION_FETCH_EXTENSION_NAME;

	/* enable exactly the features the renderer relies on */
	memset (&enable, 0, sizeof(enable));
	VK_ChainFeatures (&enable, c->has_rtp, c->has_ser, c->has_pf);
	enable.core.features.samplerAnisotropy = c->supported.core.features.samplerAnisotropy;
	enable.core.features.shaderInt64 = c->supported.core.features.shaderInt64;
	enable.v12.bufferDeviceAddress = VK_TRUE;
	enable.v12.descriptorIndexing = VK_TRUE;
	enable.v12.runtimeDescriptorArray = VK_TRUE;
	enable.v12.descriptorBindingPartiallyBound = VK_TRUE;
	enable.v12.descriptorBindingVariableDescriptorCount = VK_TRUE;
	enable.v12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
	/* textures are added to the bindless array while frames use it */
	enable.v12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
	enable.v12.descriptorBindingUpdateUnusedWhilePending = VK_TRUE;
	enable.v12.timelineSemaphore = VK_TRUE;
	enable.v12.scalarBlockLayout = VK_TRUE;
	enable.v13.dynamicRendering = VK_TRUE;
	enable.v13.synchronization2 = VK_TRUE;
	enable.v13.maintenance4 = VK_TRUE;
	enable.v13.shaderDemoteToHelperInvocation = VK_TRUE;	/* GLSL discard with a 1.3 target */
	enable.as.accelerationStructure = VK_TRUE;
	enable.rq.rayQuery = VK_TRUE;
	enable.rtp.rayTracingPipeline = c->has_rtp;
	enable.ser.rayTracingInvocationReorder = c->has_ser;
	enable.pf.rayTracingPositionFetch = c->has_pf;

	memset (&queue_info, 0, sizeof(queue_info));
	queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	queue_info.queueFamilyIndex = c->queue_family;
	queue_info.queueCount = 1;
	queue_info.pQueuePriorities = &priority;

	memset (&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	info.pNext = &enable.core;
	info.queueCreateInfoCount = 1;
	info.pQueueCreateInfos = &queue_info;
	info.enabledExtensionCount = num_extensions;
	info.ppEnabledExtensionNames = extensions;

	VK_CHECK (vkCreateDevice (c->device, &info, NULL, &vk.device));
	volkLoadDevice (vk.device);	/* device functions without dispatch overhead */
	vkGetDeviceQueue (vk.device, c->queue_family, 0, &vk.queue);

	vk.physical_device = c->device;
	vk.props = c->props;
	vk.queue_family = c->queue_family;
	vk.have_rt_pipeline = c->has_rtp;
	vk.have_ser = c->has_ser;
	vk.have_position_fetch = c->has_pf;
}

static void VK_CreateAllocator (void)
{
	VmaVulkanFunctions	functions;
	VmaAllocatorCreateInfo	info;

	memset (&functions, 0, sizeof(functions));
	functions.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
	functions.vkGetDeviceProcAddr = vkGetDeviceProcAddr;

	memset (&info, 0, sizeof(info));
	info.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
	info.physicalDevice = vk.physical_device;
	info.device = vk.device;
	info.instance = vk.instance;
	info.vulkanApiVersion = VK_API_VERSION_1_3;
	info.pVulkanFunctions = &functions;

	VK_CHECK (vmaCreateAllocator (&info, &vk.allocator));
}


/* ==========================================================================
 * Info command
 * ========================================================================== */

static void VK_Info_f (void)
{
	const VkPhysicalDeviceProperties *p = &vk.props;

	if (!vk.device)
	{
		Con_Printf ("Vulkan is not initialized\n");
		return;
	}
	Con_Printf ("Device    : %s (vendor 0x%04x, device 0x%04x)\n", p->deviceName, p->vendorID, p->deviceID);
	Con_Printf ("Vulkan    : %u.%u.%u\n", VK_API_VERSION_MAJOR(p->apiVersion),
			VK_API_VERSION_MINOR(p->apiVersion), VK_API_VERSION_PATCH(p->apiVersion));
	if (p->vendorID == 0x10de)	/* NVIDIA's driver version encoding */
		Con_Printf ("Driver    : %u.%u\n", (p->driverVersion >> 22) & 0x3ff, (p->driverVersion >> 14) & 0xff);
	else
		Con_Printf ("Driver    : 0x%08x\n", p->driverVersion);
	Con_Printf ("Ray query : yes\n");
	Con_Printf ("RT pipeline: %s, SER: %s, position fetch: %s\n",
			vk.have_rt_pipeline ? "yes" : "no", vk.have_ser ? "yes" : "no",
			vk.have_position_fetch ? "yes" : "no");
	Con_Printf ("Swapchain : %ux%u, %u images, format %d, present mode %d\n",
			vk.extent.width, vk.extent.height, vk.num_images,
			(int)vk.surface_format.format, (int)vk.present_mode);
	Con_Printf ("Validation: %s (%d errors, %d warnings so far)\n",
			vk.validation ? "on" : "off", vk.validation_errors, vk.validation_warnings);
}


/* ==========================================================================
 * Init / shutdown
 * ========================================================================== */

void VK_Init (HINSTANCE hinstance, HWND hwnd)
{
	VkWin32SurfaceCreateInfoKHR	surface_info;
	vk_candidate_t			chosen;

	if (volkInitialize () != VK_SUCCESS)
		Sys_Error ("Vulkan is not available (vulkan-1.dll not found).\n"
			   "Update the graphics driver.");

	VK_CreateInstance ();

	memset (&surface_info, 0, sizeof(surface_info));
	surface_info.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
	surface_info.hinstance = hinstance;
	surface_info.hwnd = hwnd;
	VK_CHECK (vkCreateWin32SurfaceKHR (vk.instance, &surface_info, NULL, &vk.surface));

	VK_SelectPhysicalDevice (&chosen);
	VK_CreateDevice (&chosen);
	VK_CreateAllocator ();

	Con_Printf ("Vulkan: using %s (Vulkan %u.%u, RT pipeline %s, SER %s)\n", vk.props.deviceName,
			VK_API_VERSION_MAJOR(vk.props.apiVersion), VK_API_VERSION_MINOR(vk.props.apiVersion),
			vk.have_rt_pipeline ? "yes" : "no", vk.have_ser ? "yes" : "no");

	Cmd_AddCommand ("vk_info", VK_Info_f);

	VK_InitBuffers ();
	VK_InitTextures ();
	VK_InitMaterials ();
	VK_InitWorld ();
	VK_InitModels ();
	VK_InitInstances ();
	VK_InitAccel ();
	VK_InitView ();
	VK_InitSwapchain ();
	VK_InitDraw ();
}

void VK_Shutdown (void)
{
	if (!vk.instance)
		return;

	if (vk.device)
	{
		vkDeviceWaitIdle (vk.device);
		VK_ShutdownDraw ();
		VK_ShutdownSwapchain ();
		VK_ShutdownView ();
		VK_ShutdownAccel ();
		VK_ShutdownInstances ();
		VK_ShutdownModels ();
		VK_ShutdownWorld ();
		VK_ShutdownMaterials ();
		VK_ShutdownTextures ();
		VK_ShutdownBuffers ();
		if (vk.allocator)
			vmaDestroyAllocator (vk.allocator);
		vkDestroyDevice (vk.device, NULL);
	}
	if (vk.surface)
		vkDestroySurfaceKHR (vk.instance, vk.surface, NULL);

	if (vk.validation)
		Con_Printf ("Vulkan validation: %d errors, %d warnings\n",
				vk.validation_errors, vk.validation_warnings);
	if (vk.messenger)
		vkDestroyDebugUtilsMessengerEXT (vk.instance, vk.messenger, NULL);
	vkDestroyInstance (vk.instance, NULL);

	memset (&vk, 0, sizeof(vk));
}
