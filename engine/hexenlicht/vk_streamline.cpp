/* vk_streamline.cpp -- SPIKE (story 3.9, not for merging): NVIDIA
 * Streamline's DLSS Super Resolution and Ray Reconstruction on our Vulkan
 * device
 *
 * sl.interposer.dll (the player's, next to the exe) is loaded at runtime;
 * slInit runs before the Vulkan instance, and volk loads Vulkan through
 * the interposer's vkGetInstanceProcAddr, so Streamline's vkCreateInstance
 * and vkCreateDevice proxies add what DLSS needs and its present proxy
 * keeps its frame bookkeeping. No NVIDIA application ID (engine "custom",
 * a version and a project GUID); no over-the-air updates. Streamline's log
 * messages are kept for VK_SLStatus: nothing is printed during a frame.
 *
 * Copyright (C) 2026  Hexenlicht contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <windows.h>
#include <volk.h>
#include <stdio.h>
#include <string.h>
#include <mutex>
#include <string>
#include <vector>

#include <sl.h>
#include <sl_consts.h>
#include <sl_helpers.h>
#include <sl_dlss.h>
#include <sl_dlss_d.h>
#include <sl_matrix_helpers.h>

#include "vk_streamline.h"

extern "C" void CON_Printf (unsigned int flags, const char *fmt, ...);	/* printsys.h */
#define Con_Printf(...)	CON_Printf (0, __VA_ARGS__)

static_assert (sizeof(sl::float4x4) == 16 * sizeof(float), "float4x4 layout");

static HMODULE				sl_module;
static PFun_slInit			*p_slInit;
static PFun_slShutdown			*p_slShutdown;
static PFun_slIsFeatureSupported	*p_slIsFeatureSupported;
static PFun_slIsFeatureLoaded		*p_slIsFeatureLoaded;
static PFun_slGetFeatureRequirements	*p_slGetFeatureRequirements;
static PFun_slGetFeatureVersion		*p_slGetFeatureVersion;
static PFun_slGetFeatureFunction	*p_slGetFeatureFunction;
static PFun_slGetNewFrameToken		*p_slGetNewFrameToken;
static PFun_slSetTagForFrame		*p_slSetTagForFrame;
static PFun_slSetConstants		*p_slSetConstants;
static PFun_slEvaluateFeature		*p_slEvaluateFeature;

static bool		sl_initialized;
static sl::Result	sl_init_result = sl::Result::eErrorNotInitialized;
static sl::Result	supported[3] = { sl::Result::eErrorNotInitialized, sl::Result::eErrorNotInitialized, sl::Result::eErrorNotInitialized };
static std::string	present_module;		/* where volk's vkQueuePresentKHR lives */
static std::string	device_info;		/* requirements and versions, for VK_SLStatus */

/* the log, kept for VK_SLStatus (Streamline may call from other threads) */
static std::mutex		log_mutex;
static std::vector<std::string>	log_lines;
static int			log_errors, log_warnings;

/* the last evaluation, for VK_SLStatus */
static sl::Result	last_result[3] = { sl::Result::eOk, sl::Result::eOk, sl::Result::eOk };
static unsigned		evaluations[3];
static std::string	last_settings;

/* sl_dlss.h's and sl_dlss_d.h's inline helpers call this */
extern "C" sl::Result slGetFeatureFunction (sl::Feature feature, const char *functionName, void *&function)
{
	if (!p_slGetFeatureFunction)
		return sl::Result::eErrorNotInitialized;
	return p_slGetFeatureFunction (feature, functionName, function);
}

static void LogCallback (sl::LogType type, const char *msg)
{
	std::lock_guard<std::mutex>	lock (log_mutex);
	std::string			line (msg ? msg : "");

	while (!line.empty () && (line.back () == '\n' || line.back () == '\r'))
		line.pop_back ();
	if (type == sl::LogType::eError)
		log_errors++;
	else if (type == sl::LogType::eWarn)
		log_warnings++;
	if (type != sl::LogType::eInfo)
	{
		log_lines.push_back ((type == sl::LogType::eError ? "E " : "W ") + line);
		if (log_lines.size () > 40)
			log_lines.erase (log_lines.begin ());
	}
}

template <typename T> static bool Load (T *&p, const char *name)
{
	p = reinterpret_cast<T *> (GetProcAddress (sl_module, name));
	return p != nullptr;
}

extern "C" PFN_vkGetInstanceProcAddr VK_SLPreInit (const char *exe_dir)
{
	char			path[MAX_PATH];
	static wchar_t		log_dir[MAX_PATH];
	sl::Preferences		pref;
	static const sl::Feature features[] = { sl::kFeatureDLSS, sl::kFeatureDLSS_RR };
	PFN_vkGetInstanceProcAddr gipa;

	snprintf (path, sizeof(path), "%s\\sl.interposer.dll", exe_dir);
	if (GetFileAttributesA (path) == INVALID_FILE_ATTRIBUTES)
		return nullptr;
	sl_module = LoadLibraryA (path);
	if (!sl_module)
		return nullptr;
	if (!Load (p_slInit, "slInit") || !Load (p_slShutdown, "slShutdown") ||
	    !Load (p_slIsFeatureSupported, "slIsFeatureSupported") || !Load (p_slIsFeatureLoaded, "slIsFeatureLoaded") ||
	    !Load (p_slGetFeatureRequirements, "slGetFeatureRequirements") || !Load (p_slGetFeatureVersion, "slGetFeatureVersion") ||
	    !Load (p_slGetFeatureFunction, "slGetFeatureFunction") || !Load (p_slGetNewFrameToken, "slGetNewFrameToken") ||
	    !Load (p_slSetTagForFrame, "slSetTagForFrame") || !Load (p_slSetConstants, "slSetConstants") ||
	    !Load (p_slEvaluateFeature, "slEvaluateFeature"))
	{
		FreeLibrary (sl_module);
		sl_module = nullptr;
		return nullptr;
	}

	/* its log files next to the exe, in sl_log\ */
	swprintf (log_dir, MAX_PATH, L"%hs\\sl_log", exe_dir);
	CreateDirectoryW (log_dir, nullptr);

	pref.showConsole = false;
	pref.logLevel = sl::LogLevel::eVerbose;
	pref.pathToLogsAndData = log_dir;
	pref.logMessageCallback = LogCallback;
	/* no over-the-air updates, no manual hooking; tags per frame (slSetTagForFrame) */
	pref.flags = sl::PreferenceFlags::eDisableCLStateTracking | sl::PreferenceFlags::eUseFrameBasedResourceTagging;
	pref.featuresToLoad = features;
	pref.numFeaturesToLoad = 2;
	pref.engine = sl::EngineType::eCustom;
	pref.engineVersion = "0.3.9";
	pref.projectId = "5f4b8e2a-7c1d-4e6b-9a3f-2d8c6e1b0a47";	/* Hexenlicht's */
	pref.renderAPI = sl::RenderAPI::eVulkan;
	sl_init_result = p_slInit (pref, sl::kSDKVersion);
	if (sl_init_result != sl::Result::eOk)
		return nullptr;	/* the interposer stays loaded; Vulkan comes from vulkan-1.dll */
	sl_initialized = true;

	gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr> (GetProcAddress (sl_module, "vkGetInstanceProcAddr"));
	return gipa;
}

static std::string ModuleOf (const void *address)
{
	HMODULE	mod = nullptr;
	char	name[MAX_PATH] = "?";

	if (address && GetModuleHandleExA (GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
					   (LPCSTR)address, &mod))
		GetModuleFileNameA (mod, name, sizeof(name));
	return name;
}

extern "C" void VK_SLDeviceReady (VkPhysicalDevice physical_device)
{
	static const sl::Feature	features[3] = { sl::kFeatureDLSS, sl::kFeatureDLSS, sl::kFeatureDLSS_RR };
	sl::AdapterInfo			adapter;
	char				line[512];
	int				i;
	uint32_t			k;

	present_module = ModuleOf ((const void *)vkQueuePresentKHR);
	if (!sl_initialized)
		return;
	adapter.vkPhysicalDevice = physical_device;
	device_info.clear ();
	for (i = VK_SL_SR; i <= VK_SL_RR; i++)
	{
		sl::FeatureRequirements	req;
		sl::FeatureVersion	ver;
		bool			loaded = false;

		supported[i] = p_slIsFeatureSupported (features[i], adapter);
		p_slIsFeatureLoaded (features[i], loaded);
		snprintf (line, sizeof(line), "%s: supported %s, loaded %d\n", sl::getFeatureAsStr (features[i]),
			  sl::getResultAsStr (supported[i]), loaded ? 1 : 0);
		device_info += line;
		if (p_slGetFeatureVersion (features[i], ver) == sl::Result::eOk)
		{
			snprintf (line, sizeof(line), "  version SL %s, NGX %s\n", ver.versionSL.toStr ().c_str (), ver.versionNGX.toStr ().c_str ());
			device_info += line;
		}
		if (p_slGetFeatureRequirements (features[i], req) == sl::Result::eOk)
		{
			snprintf (line, sizeof(line), "  flags 0x%x, driver %s (needs %s), queues +%u compute +%u graphics, tags %u\n",
				  (unsigned)req.flags, req.driverVersionDetected.toStr ().c_str (), req.driverVersionRequired.toStr ().c_str (),
				  req.vkNumComputeQueuesRequired, req.vkNumGraphicsQueuesRequired, req.numRequiredTags);
			device_info += line;
			for (k = 0; k < req.vkNumInstanceExtensions; k++)
				device_info += std::string ("  instance extension ") + req.vkInstanceExtensions[k] + "\n";
			for (k = 0; k < req.vkNumDeviceExtensions; k++)
				device_info += std::string ("  device extension ") + req.vkDeviceExtensions[k] + "\n";
			for (k = 0; k < req.vkNumFeatures12; k++)
				device_info += std::string ("  1.2 feature ") + req.vkFeatures12[k] + "\n";
			for (k = 0; k < req.vkNumFeatures13; k++)
				device_info += std::string ("  1.3 feature ") + req.vkFeatures13[k] + "\n";
		}
	}
}

extern "C" int VK_SLSupported (int mode)
{
	return sl_initialized && (mode == VK_SL_SR || mode == VK_SL_RR) && supported[mode] == sl::Result::eOk;
}

static void Matrix (sl::float4x4 &m, const float *column_major)
{
	/* column-major with column vectors = row-major with row vectors (Streamline's) */
	memcpy (&m, column_major, sizeof(m));
}

static void MakeResource (sl::Resource &r, const vk_sl_image_t &img)
{
	r = sl::Resource (sl::ResourceType::eTex2d, (void *)img.image, nullptr, (void *)img.view, (uint32_t)VK_IMAGE_LAYOUT_GENERAL);
	r.width = img.width;
	r.height = img.height;
	r.nativeFormat = (uint32_t)img.format;
	r.mipLevels = 1;
	r.arrayLayers = 1;
	r.flags = 0;
	r.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
}

static sl::DLSSMode ModeFor (const vk_sl_frame_t *f)
{
	float	ratio = (float)f->render_width / (float)f->output_width;

	if (ratio >= 0.99f)
		return sl::DLSSMode::eDLAA;
	if (ratio >= 0.66f)
		return sl::DLSSMode::eMaxQuality;
	if (ratio >= 0.57f)
		return sl::DLSSMode::eBalanced;
	if (ratio >= 0.49f)
		return sl::DLSSMode::eMaxPerformance;
	return sl::DLSSMode::eUltraPerformance;
}

extern "C" int VK_SLEvaluate (VkCommandBuffer cmd, const vk_sl_frame_t *f)
{
	sl::FrameToken		*token = nullptr;
	sl::ViewportHandle	viewport (0);
	sl::Constants		c;
	sl::float4x4		view_to_world, view_to_world_prev, view_to_prev_view, clip_to_prev_view, V_prev_m;
	sl::Extent		in_extent, out_extent;
	sl::Resource		res[9];
	sl::ResourceTag		tags[9] = {
		sl::ResourceTag (nullptr, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eValidUntilPresent),
		sl::ResourceTag (nullptr, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eValidUntilPresent),
		sl::ResourceTag (nullptr, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eValidUntilPresent),
		sl::ResourceTag (nullptr, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eValidUntilPresent),
		sl::ResourceTag (nullptr, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eValidUntilPresent),
		sl::ResourceTag (nullptr, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eValidUntilPresent),
		sl::ResourceTag (nullptr, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eValidUntilPresent),
		sl::ResourceTag (nullptr, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eValidUntilPresent),
		sl::ResourceTag (nullptr, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eValidUntilPresent),
	};
	const vk_sl_image_t	*imgs[9];
	sl::BufferType		types[9];
	uint32_t		n = 0, i;
	sl::Feature		feature = (f->mode == VK_SL_RR) ? sl::kFeatureDLSS_RR : sl::kFeatureDLSS;
	sl::Result		r;
	char			buf[256];

	if (!VK_SLSupported (f->mode))
		return 0;
	evaluations[f->mode]++;
	r = p_slGetNewFrameToken (token, &f->frame);
	if (r != sl::Result::eOk)
	{
		last_result[f->mode] = r;
		return 0;
	}

	/* the camera (Streamline's common constants) */
	Matrix (c.cameraViewToClip, f->P);
	Matrix (c.clipToCameraView, f->invP);
	Matrix (view_to_world, f->invV);
	Matrix (V_prev_m, f->V_prev);
	sl::matrixFullInvert (view_to_world_prev, V_prev_m);
	sl::calcCameraToPrevCamera (view_to_prev_view, view_to_world, view_to_world_prev);
	sl::matrixMul (clip_to_prev_view, c.clipToCameraView, view_to_prev_view);
	{
		sl::float4x4	P_prev_m;

		Matrix (P_prev_m, f->P_prev);
		sl::matrixMul (c.clipToPrevClip, clip_to_prev_view, P_prev_m);
	}
	sl::matrixFullInvert (c.prevClipToClip, c.clipToPrevClip);
	c.jitterOffset = sl::float2 (f->jitter[0], f->jitter[1]);
	c.mvecScale = sl::float2 (f->mvec_scale[0], f->mvec_scale[1]);
	c.cameraPinholeOffset = sl::float2 (0.0f, 0.0f);
	c.cameraPos = sl::float3 (f->cam_pos[0], f->cam_pos[1], f->cam_pos[2]);
	c.cameraUp = sl::float3 (f->cam_up[0], f->cam_up[1], f->cam_up[2]);
	c.cameraRight = sl::float3 (f->cam_right[0], f->cam_right[1], f->cam_right[2]);
	c.cameraFwd = sl::float3 (f->cam_fwd[0], f->cam_fwd[1], f->cam_fwd[2]);
	c.cameraNear = f->znear;
	c.cameraFar = f->zfar;
	c.cameraFOV = f->fov_y;
	c.cameraAspectRatio = f->aspect;
	c.depthInverted = sl::Boolean::eFalse;
	c.cameraMotionIncluded = sl::Boolean::eTrue;
	c.motionVectors3D = sl::Boolean::eFalse;
	c.reset = f->reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
	c.orthographicProjection = sl::Boolean::eFalse;
	c.motionVectorsDilated = sl::Boolean::eFalse;
	c.motionVectorsJittered = sl::Boolean::eFalse;
	r = p_slSetConstants (c, *token, viewport);
	if (r != sl::Result::eOk)
	{
		last_result[f->mode] = r;
		return 0;
	}

	/* the options */
	if (f->mode == VK_SL_RR)
	{
		sl::DLSSDOptions	o;

		o.mode = ModeFor (f);
		o.outputWidth = f->output_width;
		o.outputHeight = f->output_height;
		o.colorBuffersHDR = sl::Boolean::eTrue;
		o.preExposure = f->pre_exposure;
		o.normalRoughnessMode = sl::DLSSDNormalRoughnessMode::ePacked;
		Matrix (o.worldToCameraView, f->V);
		Matrix (o.cameraViewToWorld, f->invV);
		r = slDLSSDSetOptions (viewport, o);
		if (r == sl::Result::eOk)
		{
			sl::DLSSDOptimalSettings	s;

			if (slDLSSDGetOptimalSettings (o, s) == sl::Result::eOk)
			{
				snprintf (buf, sizeof(buf), "RR mode %u: optimal %ux%u, min %ux%u, max %ux%u; rendering %ux%u",
					  (unsigned)o.mode, s.optimalRenderWidth, s.optimalRenderHeight, s.renderWidthMin, s.renderHeightMin,
					  s.renderWidthMax, s.renderHeightMax, f->render_width, f->render_height);
				last_settings = buf;
			}
		}
	}
	else
	{
		sl::DLSSOptions	o;

		o.mode = ModeFor (f);
		o.outputWidth = f->output_width;
		o.outputHeight = f->output_height;
		o.colorBuffersHDR = sl::Boolean::eTrue;
		o.preExposure = f->pre_exposure;
		o.useAutoExposure = sl::Boolean::eTrue;
		r = slDLSSSetOptions (viewport, o);
		if (r == sl::Result::eOk)
		{
			sl::DLSSOptimalSettings	s;

			if (slDLSSGetOptimalSettings (o, s) == sl::Result::eOk)
			{
				snprintf (buf, sizeof(buf), "SR mode %u: optimal %ux%u, min %ux%u, max %ux%u; rendering %ux%u",
					  (unsigned)o.mode, s.optimalRenderWidth, s.optimalRenderHeight, s.renderWidthMin, s.renderHeightMin,
					  s.renderWidthMax, s.renderHeightMax, f->render_width, f->render_height);
				last_settings = buf;
			}
		}
	}
	if (r != sl::Result::eOk)
	{
		last_result[f->mode] = r;
		return 0;
	}

	/* the resources */
	in_extent.top = in_extent.left = 0;
	in_extent.width = f->render_width;
	in_extent.height = f->render_height;
	out_extent.top = out_extent.left = 0;
	out_extent.width = f->output_width;
	out_extent.height = f->output_height;
	imgs[n] = &f->color_in;		types[n++] = sl::kBufferTypeScalingInputColor;
	imgs[n] = &f->color_out;	types[n++] = sl::kBufferTypeScalingOutputColor;
	imgs[n] = &f->depth;		types[n++] = sl::kBufferTypeDepth;
	imgs[n] = &f->mvec;		types[n++] = sl::kBufferTypeMotionVectors;
	if (f->mode == VK_SL_RR)
	{
		imgs[n] = &f->albedo;		types[n++] = sl::kBufferTypeAlbedo;
		imgs[n] = &f->spec_albedo;	types[n++] = sl::kBufferTypeSpecularAlbedo;
		imgs[n] = &f->normal_roughness;	types[n++] = sl::kBufferTypeNormalRoughness;
		imgs[n] = &f->spec_hit;		types[n++] = sl::kBufferTypeSpecularHitDistance;
	}
	for (i = 0; i < n; i++)
	{
		MakeResource (res[i], *imgs[i]);
		tags[i] = sl::ResourceTag (&res[i], types[i], sl::ResourceLifecycle::eValidUntilPresent,
					   types[i] == sl::kBufferTypeScalingOutputColor ? &out_extent : &in_extent);
	}
	r = p_slSetTagForFrame (*token, viewport, tags, n, reinterpret_cast<sl::CommandBuffer *> (cmd));
	if (r != sl::Result::eOk)
	{
		last_result[f->mode] = r;
		return 0;
	}

	{
		const sl::BaseStructure	*inputs[] = { &viewport };

		r = p_slEvaluateFeature (feature, *token, inputs, 1, reinterpret_cast<sl::CommandBuffer *> (cmd));
	}
	last_result[f->mode] = r;
	return r == sl::Result::eOk;
}

extern "C" void VK_SLShutdown (void)
{
	if (sl_initialized)
		p_slShutdown ();
	sl_initialized = false;
}

extern "C" void VK_SLStatus (void)
{
	std::lock_guard<std::mutex>	lock (log_mutex);
	size_t				i;

	if (!sl_module)
	{
		Con_Printf ("Streamline: no sl.interposer.dll next to the exe\n");
		return;
	}
	Con_Printf ("Streamline: slInit %s; vkQueuePresentKHR from %s\n", sl::getResultAsStr (sl_init_result), present_module.c_str ());
	Con_Printf ("%s", device_info.c_str ());
	Con_Printf ("evaluations: SR %u (last %s), RR %u (last %s)\n", evaluations[VK_SL_SR], sl::getResultAsStr (last_result[VK_SL_SR]),
		    evaluations[VK_SL_RR], sl::getResultAsStr (last_result[VK_SL_RR]));
	if (!last_settings.empty ())
		Con_Printf ("%s\n", last_settings.c_str ());
	Con_Printf ("log: %d errors, %d warnings\n", log_errors, log_warnings);
	for (i = 0; i < log_lines.size (); i++)
		Con_Printf ("  %s\n", log_lines[i].c_str ());
}
