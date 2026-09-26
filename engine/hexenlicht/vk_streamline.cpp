/* vk_streamline.cpp -- NVIDIA Streamline's DLSS Super Resolution and Ray
 * Reconstruction on our Vulkan device, from the player's DLLs
 *
 * Only the Streamline calls, behind the C interface of vk_streamline.h
 * (vk_dlss.c decides what DLSS gets). Streamline's headers are C++
 * (libs/streamline, MIT); nothing NVIDIA's is linked:
 *  - VK_SLPreInit, before the Vulkan instance: when sl.interposer.dll is
 *    next to the exe, its signature is checked first (sl_security.h: the
 *    Windows signature through WinVerifyTrust, without revocation checks,
 *    and NVIDIA's own second one); a DLL that fails isn't loaded. Then
 *    slInit, and vk_core.c loads Vulkan
 *    through the interposer's vkGetInstanceProcAddr (volk), so Streamline's
 *    vkCreateInstance and vkCreateDevice proxies add what DLSS needs and
 *    its present proxy keeps its frame bookkeeping. The signed interposer
 *    checks the DLLs it loads itself (sl.common.dll, the plugins). If the
 *    instance or the device fails through the interposer, vk_core.c
 *    shuts Streamline down (VK_SLVulkanFailed) and starts over without it;
 *  - no NVIDIA application ID (engine "custom", a version and the
 *    project's GUID), and only the preference flags
 *    eDisableCLStateTracking (every later pass binds its own pipeline,
 *    sets and push constants), eUseFrameBasedResourceTagging and
 *    eDisableDebugText (the development DLLs' text over the view):
 *    Streamline's defaults would turn on over-the-air updates and plugins
 *    downloaded by them;
 *  - plugins only from the exe's folder; no log files: Streamline's errors
 *    and warnings are kept for VK_SLStatus (vk_dlss), nothing is printed
 *    during a frame;
 *  - the feature functions are fetched after the device exists
 *    (slGetFeatureFunction), not through sl_dlss.h's helpers, which keep
 *    them in static variables.
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
#include <sl_security.h>

#include "vk_streamline.h"

extern "C" void CON_Printf (unsigned int flags, const char *fmt, ...);	/* printsys.h */
#define Con_Printf(...)	CON_Printf (0, __VA_ARGS__)

static_assert (sizeof(sl::float4x4) == 16 * sizeof(float), "float4x4 layout");

#define HEXENLICHT_PROJECT_ID	"5f4b8e2a-7c1d-4e6b-9a3f-2d8c6e1b0a47"
#define HEXENLICHT_ENGINE_VERSION	"0.3.10"

/* where loading stopped, for VK_SLStatus */
enum { SL_NO_DLL, SL_BAD_SIGNATURE, SL_LOAD_FAILED, SL_INIT_FAILED, SL_NO_VULKAN, SL_VULKAN_FAILED, SL_READY };

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
static PFun_slFreeResources		*p_slFreeResources;

/* the features' functions, after the device exists */
static PFun_slDLSSGetOptimalSettings	*p_slDLSSGetOptimalSettings;
static PFun_slDLSSSetOptions		*p_slDLSSSetOptions;
static PFun_slDLSSDGetOptimalSettings	*p_slDLSSDGetOptimalSettings;
static PFun_slDLSSDSetOptions		*p_slDLSSDSetOptions;

static int		sl_state = SL_NO_DLL;
static bool		sl_initialized;
static sl::Result	sl_init_result = sl::Result::eErrorNotInitialized;
static std::string	sl_path;		/* the interposer's */
static sl::Result	supported[3] = { sl::Result::eErrorNotInitialized, sl::Result::eErrorNotInitialized, sl::Result::eErrorNotInitialized };
static std::string	device_info;		/* versions, requirements, for VK_SLStatus */
static std::string	startup_message;

/* the log, kept for VK_SLStatus (Streamline may call from other threads) */
static std::mutex		log_mutex;
static std::vector<std::string>	log_lines;
static int			log_errors, log_warnings;

/* the last evaluation, for VK_SLStatus */
static sl::Result	last_result[3] = { sl::Result::eOk, sl::Result::eOk, sl::Result::eOk };
static unsigned		evaluations[3];
static int		last_mode = -1, last_preset;
static uint32_t		last_render[2], last_output[2];

static const sl::Feature	features[3] = { sl::kFeatureDLSS, sl::kFeatureDLSS, sl::kFeatureDLSS_RR };
static const sl::DLSSMode	modes[VK_SL_NUM_MODES] = {
	sl::DLSSMode::eDLAA, sl::DLSSMode::eMaxQuality, sl::DLSSMode::eBalanced,
	sl::DLSSMode::eMaxPerformance, sl::DLSSMode::eUltraPerformance
};
static const char		*mode_names[VK_SL_NUM_MODES] = { "DLAA", "Quality", "Balanced", "Performance", "Ultra Performance" };

static void LogCallback (sl::LogType type, const char *msg)
{
	if (type == sl::LogType::eInfo)
		return;

	std::lock_guard<std::mutex>	lock (log_mutex);
	std::string			line (msg ? msg : "");

	while (!line.empty () && (line.back () == '\n' || line.back () == '\r'))
		line.pop_back ();
	if (type == sl::LogType::eError)
		log_errors++;
	else
		log_warnings++;
	log_lines.push_back ((type == sl::LogType::eError ? "E " : "W ") + line);
	if (log_lines.size () > 40)
		log_lines.erase (log_lines.begin ());
}

template <typename T> static bool Load (T *&p, const char *name)
{
	p = reinterpret_cast<T *> (GetProcAddress (sl_module, name));
	return p != nullptr;
}

template <typename T> static bool LoadFeatureFunction (T *&p, sl::Feature feature, const char *name)
{
	void	*f = nullptr;

	p = nullptr;
	if (p_slGetFeatureFunction (feature, name, f) != sl::Result::eOk || !f)
		return false;
	p = reinterpret_cast<T *> (f);
	return true;
}

static void Unload (void)
{
	if (sl_module)
		FreeLibrary (sl_module);
	sl_module = nullptr;
}

extern "C" PFN_vkGetInstanceProcAddr VK_SLPreInit (const char *exe_dir)
{
	static wchar_t		plugin_dir[MAX_PATH];
	static const wchar_t	*plugin_dirs[1] = { plugin_dir };
	wchar_t			wpath[MAX_PATH];
	char			path[MAX_PATH];
	sl::Preferences		pref;
	static const sl::Feature to_load[] = { sl::kFeatureDLSS, sl::kFeatureDLSS_RR };
	PFN_vkGetInstanceProcAddr gipa;

	snprintf (path, sizeof(path), "%s\\sl.interposer.dll", exe_dir);
	sl_path = path;
	if (GetFileAttributesA (path) == INVALID_FILE_ATTRIBUTES)
	{
		sl_state = SL_NO_DLL;
		return nullptr;
	}
	/* the full path, as sl_security.h requires */
	if (!MultiByteToWideChar (CP_ACP, 0, path, -1, wpath, MAX_PATH) ||
	    !MultiByteToWideChar (CP_ACP, 0, exe_dir, -1, plugin_dir, MAX_PATH) ||
	    !sl::security::verifyEmbeddedSignature (wpath))
	{
		sl_state = SL_BAD_SIGNATURE;
		startup_message = "DLSS: sl.interposer.dll does not carry a valid NVIDIA signature, not loaded (vk_dlss)\n";
		return nullptr;
	}
	sl_module = LoadLibraryW (wpath);
	if (!sl_module ||
	    !Load (p_slInit, "slInit") || !Load (p_slShutdown, "slShutdown") ||
	    !Load (p_slIsFeatureSupported, "slIsFeatureSupported") || !Load (p_slIsFeatureLoaded, "slIsFeatureLoaded") ||
	    !Load (p_slGetFeatureRequirements, "slGetFeatureRequirements") || !Load (p_slGetFeatureVersion, "slGetFeatureVersion") ||
	    !Load (p_slGetFeatureFunction, "slGetFeatureFunction") || !Load (p_slGetNewFrameToken, "slGetNewFrameToken") ||
	    !Load (p_slSetTagForFrame, "slSetTagForFrame") || !Load (p_slSetConstants, "slSetConstants") ||
	    !Load (p_slEvaluateFeature, "slEvaluateFeature") || !Load (p_slFreeResources, "slFreeResources"))
	{
		Unload ();
		sl_state = SL_LOAD_FAILED;
		startup_message = "DLSS: sl.interposer.dll could not be loaded (vk_dlss)\n";
		return nullptr;
	}

	pref.showConsole = false;
	pref.logLevel = sl::LogLevel::eDefault;
	pref.pathsToPlugins = plugin_dirs;
	pref.numPathsToPlugins = 1;
	pref.pathToLogsAndData = nullptr;	/* no log files */
	pref.logMessageCallback = LogCallback;
	pref.flags = sl::PreferenceFlags::eDisableCLStateTracking | sl::PreferenceFlags::eUseFrameBasedResourceTagging |
		     sl::PreferenceFlags::eDisableDebugText;
	pref.featuresToLoad = to_load;
	pref.numFeaturesToLoad = 2;
	pref.engine = sl::EngineType::eCustom;
	pref.engineVersion = HEXENLICHT_ENGINE_VERSION;
	pref.projectId = HEXENLICHT_PROJECT_ID;
	pref.renderAPI = sl::RenderAPI::eVulkan;
	sl_init_result = p_slInit (pref, sl::kSDKVersion);
	if (sl_init_result != sl::Result::eOk)
	{
		/* the interposer stays loaded: slInit may have hooked into it */
		sl_state = SL_INIT_FAILED;
		startup_message = std::string ("DLSS: Streamline's slInit failed (") + sl::getResultAsStr (sl_init_result) + "), not used (vk_dlss)\n";
		return nullptr;
	}
	gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr> (GetProcAddress (sl_module, "vkGetInstanceProcAddr"));
	if (!gipa)
	{
		p_slShutdown ();
		sl_state = SL_NO_VULKAN;
		startup_message = "DLSS: sl.interposer.dll has no Vulkan entry point, not used (vk_dlss)\n";
		return nullptr;
	}
	sl_initialized = true;
	sl_state = SL_READY;
	return gipa;
}

extern "C" int VK_SLActive (void)
{
	return sl_initialized;
}

extern "C" const char *VK_SLInactiveReason (void)
{
	switch (sl_state)
	{
	case SL_NO_DLL:		return "no sl.interposer.dll next to the exe";
	case SL_BAD_SIGNATURE:	return "sl.interposer.dll does not carry a valid NVIDIA signature";
	case SL_LOAD_FAILED:	return "sl.interposer.dll could not be loaded";
	case SL_INIT_FAILED:	return "Streamline's slInit failed";
	case SL_NO_VULKAN:	return "sl.interposer.dll has no Vulkan entry point";
	case SL_VULKAN_FAILED:	return "Vulkan failed through Streamline";
	default:		return sl_initialized ? "" : "Streamline is shut down";
	}
}

extern "C" void VK_SLVulkanFailed (void)
{
	if (sl_initialized)
		p_slShutdown ();
	sl_initialized = false;
	sl_state = SL_VULKAN_FAILED;
	startup_message = "DLSS: the Vulkan instance or device failed through Streamline, started without it (vk_dlss)\n";
}

extern "C" void VK_SLDeviceReady (VkPhysicalDevice physical_device)
{
	sl::AdapterInfo		adapter;
	char			line[512];
	int			i;

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
		snprintf (line, sizeof(line), "%s: %s%s", (i == VK_SL_SR) ? "DLSS SR" : "DLSS RR",
			  (supported[i] == sl::Result::eOk) ? "supported" : sl::getResultAsStr (supported[i]), loaded ? "" : ", not loaded");
		device_info += line;
		if (p_slGetFeatureVersion (features[i], ver) == sl::Result::eOk)
		{
			snprintf (line, sizeof(line), "; Streamline %s, NGX %s", ver.versionSL.toStr ().c_str (), ver.versionNGX.toStr ().c_str ());
			device_info += line;
		}
		if (p_slGetFeatureRequirements (features[i], req) == sl::Result::eOk)
		{
			snprintf (line, sizeof(line), "; driver %s (needs %s)", req.driverVersionDetected.toStr ().c_str (),
				  req.driverVersionRequired.toStr ().c_str ());
			device_info += line;
		}
		device_info += "\n";
	}
	if (supported[VK_SL_SR] == sl::Result::eOk)
	{
		LoadFeatureFunction (p_slDLSSGetOptimalSettings, sl::kFeatureDLSS, "slDLSSGetOptimalSettings");
		LoadFeatureFunction (p_slDLSSSetOptions, sl::kFeatureDLSS, "slDLSSSetOptions");
		if (!p_slDLSSGetOptimalSettings || !p_slDLSSSetOptions)
			supported[VK_SL_SR] = sl::Result::eErrorMissingOrInvalidAPI;
	}
	if (supported[VK_SL_RR] == sl::Result::eOk)
	{
		LoadFeatureFunction (p_slDLSSDGetOptimalSettings, sl::kFeatureDLSS_RR, "slDLSSDGetOptimalSettings");
		LoadFeatureFunction (p_slDLSSDSetOptions, sl::kFeatureDLSS_RR, "slDLSSDSetOptions");
		if (!p_slDLSSDGetOptimalSettings || !p_slDLSSDSetOptions)
			supported[VK_SL_RR] = sl::Result::eErrorMissingOrInvalidAPI;
	}
	startup_message = std::string ("DLSS: Streamline loaded; SR ") +
			  ((supported[VK_SL_SR] == sl::Result::eOk) ? "supported" : "not supported") + ", RR " +
			  ((supported[VK_SL_RR] == sl::Result::eOk) ? "supported" : "not supported") + " (vk_dlss)\n";
}

extern "C" int VK_SLSupported (int feature)
{
	return sl_initialized && (feature == VK_SL_SR || feature == VK_SL_RR) && supported[feature] == sl::Result::eOk;
}

extern "C" int VK_SLRange (int feature, int mode, uint32_t output_width, uint32_t output_height, vk_sl_range_t *range)
{
	sl::Result	r;

	if (!VK_SLSupported (feature) || mode < 0 || mode >= VK_SL_NUM_MODES)
		return 0;
	if (feature == VK_SL_RR)
	{
		sl::DLSSDOptions		o;
		sl::DLSSDOptimalSettings	s;

		o.mode = modes[mode];
		o.outputWidth = output_width;
		o.outputHeight = output_height;
		r = p_slDLSSDGetOptimalSettings (o, s);
		range->optimal_width = s.optimalRenderWidth;
		range->optimal_height = s.optimalRenderHeight;
		range->min_width = s.renderWidthMin;
		range->min_height = s.renderHeightMin;
		range->max_width = s.renderWidthMax;
		range->max_height = s.renderHeightMax;
	}
	else
	{
		sl::DLSSOptions		o;
		sl::DLSSOptimalSettings	s;

		o.mode = modes[mode];
		o.outputWidth = output_width;
		o.outputHeight = output_height;
		r = p_slDLSSGetOptimalSettings (o, s);
		range->optimal_width = s.optimalRenderWidth;
		range->optimal_height = s.optimalRenderHeight;
		range->min_width = s.renderWidthMin;
		range->min_height = s.renderHeightMin;
		range->max_width = s.renderWidthMax;
		range->max_height = s.renderHeightMax;
	}
	return r == sl::Result::eOk && range->min_width && range->max_width >= range->min_width &&
	       range->min_height && range->max_height >= range->min_height;
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
	/* vk_images.c's */
	r.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
}

template <typename P> static void SetPresets (P &o, int preset)
{
	auto	p = static_cast<decltype(o.dlaaPreset)> ((preset >= 1 && preset <= 15) ? preset : 0);

	o.dlaaPreset = o.qualityPreset = o.balancedPreset = o.performancePreset = o.ultraPerformancePreset = o.ultraQualityPreset = p;
}

static sl::Result Evaluate (VkCommandBuffer cmd, const vk_sl_frame_t *f)
{
	sl::FrameToken		*token = nullptr;
	sl::ViewportHandle	viewport (0);
	sl::Constants		c;
	sl::float4x4		view_to_world, view_to_world_prev, view_to_prev_view, clip_to_prev_view, V_prev_m, P_prev_m;
	sl::Extent		in_extent, out_extent;
	sl::Resource		res[8];
	std::vector<sl::ResourceTag>	tags;
	const vk_sl_image_t	*imgs[8];
	sl::BufferType		types[8];
	uint32_t		n = 0, i;
	sl::Result		r;

	r = p_slGetNewFrameToken (token, &f->frame);
	if (r != sl::Result::eOk)
		return r;

	/* the camera (Streamline's common constants) */
	Matrix (c.cameraViewToClip, f->P);
	Matrix (c.clipToCameraView, f->invP);
	Matrix (view_to_world, f->invV);
	Matrix (V_prev_m, f->V_prev);
	sl::matrixFullInvert (view_to_world_prev, V_prev_m);
	sl::calcCameraToPrevCamera (view_to_prev_view, view_to_world, view_to_world_prev);
	sl::matrixMul (clip_to_prev_view, c.clipToCameraView, view_to_prev_view);
	Matrix (P_prev_m, f->P_prev);
	sl::matrixMul (c.clipToPrevClip, clip_to_prev_view, P_prev_m);
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
		return r;

	/* the options (Streamline only acts on a change) */
	if (f->feature == VK_SL_RR)
	{
		sl::DLSSDOptions	o;

		o.mode = modes[f->mode];
		o.outputWidth = f->output_width;
		o.outputHeight = f->output_height;
		o.colorBuffersHDR = sl::Boolean::eTrue;
		o.preExposure = f->pre_exposure;
		o.normalRoughnessMode = sl::DLSSDNormalRoughnessMode::ePacked;
		Matrix (o.worldToCameraView, f->V);
		Matrix (o.cameraViewToWorld, f->invV);
		SetPresets (o, f->preset);
		r = p_slDLSSDSetOptions (viewport, o);
	}
	else
	{
		sl::DLSSOptions	o;

		o.mode = modes[f->mode];
		o.outputWidth = f->output_width;
		o.outputHeight = f->output_height;
		o.colorBuffersHDR = sl::Boolean::eTrue;
		o.preExposure = f->pre_exposure;
		o.useAutoExposure = sl::Boolean::eTrue;
		SetPresets (o, f->preset);
		r = p_slDLSSSetOptions (viewport, o);
	}
	if (r != sl::Result::eOk)
		return r;

	/* the images: the inputs don't change until the frame is presented;
	 * TAA_OUTPUT does (bloom, tone mapping), which SR and RR don't mind, as
	 * they write it in the evaluation and read no tag later (no frame
	 * generation). eValidUntilEvaluate can make Streamline copy
	 * (sl_core_types.h), so everything is eValidUntilPresent */
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
	if (f->feature == VK_SL_RR)
	{
		imgs[n] = &f->albedo;		types[n++] = sl::kBufferTypeAlbedo;
		imgs[n] = &f->spec_albedo;	types[n++] = sl::kBufferTypeSpecularAlbedo;
		imgs[n] = &f->normal_roughness;	types[n++] = sl::kBufferTypeNormalRoughness;
		imgs[n] = &f->spec_hit;		types[n++] = sl::kBufferTypeSpecularHitDistance;
	}
	for (i = 0; i < n; i++)
	{
		MakeResource (res[i], *imgs[i]);
		tags.push_back (sl::ResourceTag (&res[i], types[i], sl::ResourceLifecycle::eValidUntilPresent,
						 (types[i] == sl::kBufferTypeScalingOutputColor) ? &out_extent : &in_extent));
	}
	r = p_slSetTagForFrame (*token, viewport, tags.data (), n, reinterpret_cast<sl::CommandBuffer *> (cmd));
	if (r != sl::Result::eOk)
		return r;

	{
		const sl::BaseStructure	*inputs[] = { &viewport };

		return p_slEvaluateFeature (features[f->feature], *token, inputs, 1, reinterpret_cast<sl::CommandBuffer *> (cmd));
	}
}

extern "C" int VK_SLEvaluate (VkCommandBuffer cmd, const vk_sl_frame_t *f)
{
	if (!VK_SLSupported (f->feature) || f->mode < 0 || f->mode >= VK_SL_NUM_MODES)
		return 0;
	evaluations[f->feature]++;
	last_result[f->feature] = Evaluate (cmd, f);
	last_mode = f->mode;
	last_preset = f->preset;
	last_render[0] = f->render_width;
	last_render[1] = f->render_height;
	last_output[0] = f->output_width;
	last_output[1] = f->output_height;
	return last_result[f->feature] == sl::Result::eOk;
}

extern "C" void VK_SLFreeResources (int feature)
{
	if (VK_SLSupported (feature))
		p_slFreeResources (features[feature], sl::ViewportHandle (0));
}

extern "C" void VK_SLShutdown (void)
{
	if (sl_initialized)
		p_slShutdown ();
	sl_initialized = false;
	p_slDLSSGetOptimalSettings = nullptr;
	p_slDLSSSetOptions = nullptr;
	p_slDLSSDGetOptimalSettings = nullptr;
	p_slDLSSDSetOptions = nullptr;
}

extern "C" const char *VK_SLStartupMessage (void)
{
	return startup_message.c_str ();
}

extern "C" void VK_SLStatus (void)
{
	std::vector<std::string>	lines;
	int				errors, warnings;
	size_t				i;

	/* a copy: when disconnected every Con_Printf draws a frame, in which
	 * Streamline may log */
	{
		std::lock_guard<std::mutex>	lock (log_mutex);

		lines = log_lines;
		errors = log_errors;
		warnings = log_warnings;
	}
	switch (sl_state)
	{
	case SL_NO_DLL:
		Con_Printf ("Streamline: no %s\n", sl_path.c_str ());
		return;
	case SL_BAD_SIGNATURE:
		Con_Printf ("Streamline: %s does not carry a valid NVIDIA signature (WinVerifyTrust and NVIDIA's certificate), not loaded\n", sl_path.c_str ());
		return;
	case SL_LOAD_FAILED:
		Con_Printf ("Streamline: %s could not be loaded, or lacks Streamline's functions\n", sl_path.c_str ());
		return;
	case SL_INIT_FAILED:
		Con_Printf ("Streamline: slInit failed: %s\n", sl::getResultAsStr (sl_init_result));
		break;
	case SL_NO_VULKAN:
		Con_Printf ("Streamline: sl.interposer.dll has no vkGetInstanceProcAddr\n");
		break;
	case SL_VULKAN_FAILED:
		Con_Printf ("Streamline: the Vulkan instance or device failed through %s; running without it\n", sl_path.c_str ());
		break;
	default:
		Con_Printf ("Streamline: %s, signature verified\n", sl_path.c_str ());
		Con_Printf ("%s", device_info.c_str ());
		break;
	}
	if (evaluations[VK_SL_SR] || evaluations[VK_SL_RR])
	{
		Con_Printf ("evaluations: SR %u (last %s), RR %u (last %s)\n", evaluations[VK_SL_SR], sl::getResultAsStr (last_result[VK_SL_SR]),
			    evaluations[VK_SL_RR], sl::getResultAsStr (last_result[VK_SL_RR]));
		if (last_mode >= 0)
			Con_Printf ("last: %s, preset %c, render %u x %u, output %u x %u\n", mode_names[last_mode],
				    last_preset ? 'A' + last_preset - 1 : '-', last_render[0], last_render[1], last_output[0], last_output[1]);
	}
	Con_Printf ("log: %d errors, %d warnings\n", errors, warnings);
	for (i = 0; i < lines.size (); i++)
		Con_Printf ("  %s\n", lines[i].c_str ());
}
