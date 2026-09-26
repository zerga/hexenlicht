# Hexenlicht — CMake build for Windows x64 with MSVC.
#
# This builds alongside the upstream Makefiles, which stay untouched and
# remain the reference for the source lists (see cmake/Hexen2Sources.cmake).
#
# Targets:
#   glhexen2   Hexen II with the upstream OpenGL renderer (glh2.exe)
#   hexenlicht Hexen II with the Hexenlicht renderer (work in progress)
#   h2patch    patches retail pak files to v1.11
#   volk, vma, stb_image   vendored libraries for the Vulkan renderer
#
# Requires the LunarG Vulkan SDK 1.3+ (VULKAN_SDK environment variable).
#
# Use the presets in CMakePresets.json (CLion picks them up automatically):
#   cmake --preset windows-debug   &&  cmake --build --preset windows-debug

cmake_minimum_required(VERSION 3.25)

project(Hexenlicht LANGUAGES C CXX RC)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
	message(FATAL_ERROR "Hexenlicht's CMake build supports Windows x64 only. "
		"Use the upstream Makefiles for other platforms.")
endif()
if(NOT MSVC)
	message(FATAL_ERROR "Hexenlicht's CMake build expects the MSVC toolchain, "
		"but the compiler is ${CMAKE_C_COMPILER_ID}. In CLion: use the "
		"windows-debug / windows-release preset profiles, or set this "
		"profile's toolchain to 'Visual Studio' (Settings > Build, Execution, "
		"Deployment > CMake), then reset the CMake cache and reload.")
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
	set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# Put all executables and their runtime DLLs in one folder per build tree.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

include(cmake/Hexen2Sources.cmake)

set(OSLIBS_WIN ${CMAKE_SOURCE_DIR}/oslibs/windows)
set(CODECS_DIR ${OSLIBS_WIN}/codecs)

# ---------------------------------------------------------------------------
# Streaming music codecs: prebuilt x64 libraries from oslibs/windows/codecs.
# Same set the Makefile enables for win64 (MP3LIB=mad, VORBISLIB=vorbis,
# timidity/wildmidi off because Windows has a native MIDI driver).
# ---------------------------------------------------------------------------
add_library(h2_codecs INTERFACE)
target_include_directories(h2_codecs INTERFACE ${CODECS_DIR}/include)
target_compile_definitions(h2_codecs INTERFACE
	USE_CODEC_WAVE
	USE_CODEC_FLAC
	USE_CODEC_OPUS
	USE_CODEC_VORBIS
	USE_CODEC_MP3
	USE_CODEC_MIKMOD
	USE_CODEC_UMX
)
target_link_directories(h2_codecs INTERFACE ${CODECS_DIR}/x64)
target_link_libraries(h2_codecs INTERFACE
	libvorbisfile libvorbis libopusfile libopus libFLAC libogg libmad libmikmod
)
set(H2_CODEC_DLLS
	${CODECS_DIR}/x64/libFLAC-8.dll
	${CODECS_DIR}/x64/libmad-0.dll
	${CODECS_DIR}/x64/libmikmod-3.dll
	${CODECS_DIR}/x64/libogg-0.dll
	${CODECS_DIR}/x64/libopus-0.dll
	${CODECS_DIR}/x64/libopusfile-0.dll
	${CODECS_DIR}/x64/libvorbis-0.dll
	${CODECS_DIR}/x64/libvorbisfile-3.dll
)

# ---------------------------------------------------------------------------
# Vulkan SDK: headers and the GLSL shader compiler. Vulkan functions are
# loaded at runtime through volk, so nothing links against vulkan-1.lib.
# Version 1.3+ headers include the KHR ray tracing extensions.
# ---------------------------------------------------------------------------
find_package(Vulkan COMPONENTS glslangValidator)
# Old SDKs report only VK_HEADER_VERSION (e.g. "131"), which would pass a
# plain version check, so require a full 1.x.y version and the ray query
# extension in the headers.
set(_vk_ok FALSE)
if(Vulkan_FOUND AND Vulkan_glslangValidator_FOUND
		AND Vulkan_VERSION MATCHES "^1\\.[0-9]+\\.[0-9]+$"
		AND NOT Vulkan_VERSION VERSION_LESS 1.3)
	file(STRINGS "${Vulkan_INCLUDE_DIRS}/vulkan/vulkan_core.h" _vk_rq
		REGEX "^#define VK_KHR_ray_query 1")
	if(_vk_rq)
		set(_vk_ok TRUE)
	endif()
endif()
if(NOT _vk_ok)
	message(FATAL_ERROR "Vulkan SDK 1.3 or newer with ray tracing headers "
		"not found (found: '${Vulkan_VERSION}' in '${Vulkan_INCLUDE_DIRS}'). "
		"Install the current LunarG Vulkan SDK from "
		"https://vulkan.lunarg.com/sdk/home#windows and restart CLion (or "
		"the shell) so that the VULKAN_SDK environment variable points to it. "
		"See docs/hexenlicht/SETUP.md.")
endif()
message(STATUS "Vulkan SDK ${Vulkan_VERSION}: ${Vulkan_INCLUDE_DIRS}")
message(STATUS "glslangValidator: ${Vulkan_GLSLANG_VALIDATOR_EXECUTABLE}")

# ---------------------------------------------------------------------------
# Vendored libraries for the Vulkan renderer (see THIRD_PARTY.md).
# ---------------------------------------------------------------------------
set(LIBS_DIR ${CMAKE_SOURCE_DIR}/libs)

# volk: Vulkan function loader
add_library(volk STATIC ${LIBS_DIR}/volk/volk.c)
target_include_directories(volk PUBLIC ${LIBS_DIR}/volk)
target_compile_definitions(volk PUBLIC VK_NO_PROTOTYPES VK_USE_PLATFORM_WIN32_KHR)
target_link_libraries(volk PUBLIC Vulkan::Headers)

# VMA: Vulkan Memory Allocator (C interface, C++ implementation)
add_library(vma STATIC ${LIBS_DIR}/vma/vma_impl.cpp)
target_include_directories(vma PUBLIC ${LIBS_DIR}/vma)
target_compile_definitions(vma
	PUBLIC VMA_STATIC_VULKAN_FUNCTIONS=0 VMA_DYNAMIC_VULKAN_FUNCTIONS=1
	PRIVATE NOMINMAX)	# keep windows.h min/max macros away from std::min/max
target_compile_features(vma PRIVATE cxx_std_17)
target_link_libraries(vma PUBLIC volk)

# stb_image: PNG/TGA loading from memory
add_library(stb_image STATIC ${LIBS_DIR}/stb/stb_image_impl.c)
target_include_directories(stb_image PUBLIC ${LIBS_DIR}/stb)
target_compile_definitions(stb_image PUBLIC STBI_NO_STDIO)

# ---------------------------------------------------------------------------
# Settings shared by all Hexen II client targets (Makefile win64 section).
# ---------------------------------------------------------------------------
add_library(h2_client_settings INTERFACE)
target_include_directories(h2_client_settings INTERFACE
	${H2_DIR}
	${H2S_DIR}
	${H2CMN_DIR}
	${OSLIBS_WIN}/misc/include
	${OSLIBS_WIN}/dxsdk/include
)
target_compile_definitions(h2_client_settings INTERFACE
	WIN32_LEAN_AND_MEAN
	_USE_WINSOCK2
	DX_DLSYM			# load DirectX DLLs at runtime
	_CRT_SECURE_NO_WARNINGS
	_WINSOCK_DEPRECATED_NO_WARNINGS
	$<$<CONFIG:Debug>:DEBUG=1>
	$<$<CONFIG:Debug>:DEBUG_BUILD=1>
)
target_link_libraries(h2_client_settings INTERFACE h2_codecs ws2_32 winmm)

# ---------------------------------------------------------------------------
# glhexen2: Hexen II with the upstream OpenGL renderer.
# ---------------------------------------------------------------------------
add_executable(glhexen2 WIN32
	${H2_COMMON_SOURCES}
	${H2_GL_SOURCES}
	${H2_WIN32_RC}
)
set_target_properties(glhexen2 PROPERTIES OUTPUT_NAME glh2)
target_compile_definitions(glhexen2 PRIVATE
	GLQUAKE
	GL_DLSYM			# load opengl32.dll functions at runtime
)
target_link_libraries(glhexen2 PRIVATE h2_client_settings)

# Copy the codec DLLs next to the executable.
add_custom_command(TARGET glhexen2 POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E copy_if_different ${H2_CODEC_DLLS} $<TARGET_FILE_DIR:glhexen2>
	COMMAND_EXPAND_LISTS
	VERBATIM
)

# ---------------------------------------------------------------------------
# hexenlicht: Hexen II with the Hexenlicht renderer (work in progress).
# Compiled with GLQUAKE so the client code takes its hardware-renderer
# paths, plus HEXENLICHT for the few places where Hexenlicht differs.
# ---------------------------------------------------------------------------
set(HEXENLICHT_DIR ${CMAKE_SOURCE_DIR}/engine/hexenlicht)

include(cmake/HexenlichtShaders.cmake)
hexenlicht_add_shaders(hexenlicht_shaders
	${HEXENLICHT_SHADER_DIR}/fullscreen.vert
	${HEXENLICHT_SHADER_DIR}/draw2d.vert
	${HEXENLICHT_SHADER_DIR}/draw2d.frag
	${HEXENLICHT_SHADER_DIR}/pvs_check.comp
	${HEXENLICHT_SHADER_DIR}/rt_check.comp
	${HEXENLICHT_SHADER_DIR}/effects_check.comp
	${HEXENLICHT_SHADER_DIR}/ray_probe.comp
	${HEXENLICHT_SHADER_DIR}/primary_rays.rgen
	${HEXENLICHT_SHADER_DIR}/reflect_refract.rgen
	${HEXENLICHT_SHADER_DIR}/direct_lighting.rgen
	${HEXENLICHT_SHADER_DIR}/indirect_lighting.rgen
	${HEXENLICHT_SHADER_DIR}/compositing.comp
	${HEXENLICHT_SHADER_DIR}/checkerboard_interleave.comp
	${HEXENLICHT_SHADER_DIR}/asvgf_gradient_reproject.comp
	${HEXENLICHT_SHADER_DIR}/asvgf_gradient_img.comp
	${HEXENLICHT_SHADER_DIR}/asvgf_gradient_atrous.comp
	${HEXENLICHT_SHADER_DIR}/asvgf_temporal.comp
	${HEXENLICHT_SHADER_DIR}/asvgf_lf.comp
	${HEXENLICHT_SHADER_DIR}/asvgf_atrous.comp
	${HEXENLICHT_SHADER_DIR}/debug_view.comp
	${HEXENLICHT_SHADER_DIR}/model_geometry.comp
	${HEXENLICHT_SHADER_DIR}/view_composite.frag
)

# The blue noise textures (libs/bluenoise) next to the executable, in
# blue_noise\, where vk_images.c loads them.
file(GLOB HEXENLICHT_BLUE_NOISE CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/libs/bluenoise/64_64/HDR_RGBA_*.png)
if(NOT HEXENLICHT_BLUE_NOISE)
	message(FATAL_ERROR "The blue noise textures are missing from libs/bluenoise/64_64")
endif()
set(HEXENLICHT_BLUE_NOISE_OUT)
foreach(f IN LISTS HEXENLICHT_BLUE_NOISE)
	get_filename_component(name ${f} NAME)
	list(APPEND HEXENLICHT_BLUE_NOISE_OUT ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/blue_noise/${name})
endforeach()
add_custom_command(
	OUTPUT ${HEXENLICHT_BLUE_NOISE_OUT}
	COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/blue_noise
	COMMAND ${CMAKE_COMMAND} -E copy_if_different ${HEXENLICHT_BLUE_NOISE} ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/blue_noise
	DEPENDS ${HEXENLICHT_BLUE_NOISE}
	COMMENT "Copying the blue noise textures"
	VERBATIM
)
add_custom_target(hexenlicht_blue_noise ALL DEPENDS ${HEXENLICHT_BLUE_NOISE_OUT})

add_executable(hexenlicht WIN32
	${H2_COMMON_SOURCES}
	${H2_HEXENLICHT_REUSED_SOURCES}
	${HEXENLICHT_DIR}/stubs.c
	${HEXENLICHT_DIR}/vid_vk.c
	${HEXENLICHT_DIR}/vk_core.c
	${HEXENLICHT_DIR}/vk_swapchain.c
	${HEXENLICHT_DIR}/vk_shader.c
	${HEXENLICHT_DIR}/vk_texture.c
	${HEXENLICHT_DIR}/vk_draw.c
	${HEXENLICHT_DIR}/vk_buffer.c
	${HEXENLICHT_DIR}/vk_material.c
	${HEXENLICHT_DIR}/vk_world.c
	${HEXENLICHT_DIR}/vk_pvs.c
	${HEXENLICHT_DIR}/vk_model.c
	${HEXENLICHT_DIR}/vk_skin.c
	${HEXENLICHT_DIR}/vk_instance.c
	${HEXENLICHT_DIR}/vk_effects.c
	${HEXENLICHT_DIR}/vk_accel.c
	${HEXENLICHT_DIR}/vk_matrix.c
	${HEXENLICHT_DIR}/vk_ubo.c
	${HEXENLICHT_DIR}/vk_light.c
	${HEXENLICHT_DIR}/vk_images.c
	${HEXENLICHT_DIR}/vk_pathtracer.c
	${HEXENLICHT_DIR}/vk_asvgf.c
	${HEXENLICHT_DIR}/vk_view.c
	${HEXENLICHT_DIR}/r_scene.c
	${HEXENLICHT_DIR}/r_light.c
	${HEXENLICHT_DIR}/hexenlicht.manifest	# per-monitor DPI awareness
	${H2_WIN32_RC}
)
target_include_directories(hexenlicht PRIVATE ${HEXENLICHT_DIR})
target_compile_definitions(hexenlicht PRIVATE GLQUAKE HEXENLICHT)
target_link_libraries(hexenlicht PRIVATE h2_client_settings volk vma stb_image)
add_dependencies(hexenlicht hexenlicht_shaders hexenlicht_blue_noise)	# building the exe also builds its shaders and data
add_custom_command(TARGET hexenlicht POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E copy_if_different ${H2_CODEC_DLLS} $<TARGET_FILE_DIR:hexenlicht>
	COMMAND_EXPAND_LISTS
	VERBATIM
)

# ---------------------------------------------------------------------------
# h2patch: patches retail Hexen II pak files to v1.11 (see h2patch/Makefile).
# Run it from the game data folder; it reads the deltas from ./patchdat
# (copy gamecode/patch111/patchdat there). See docs/hexenlicht/SETUP.md.
# ---------------------------------------------------------------------------
set(XDELTA_DIR ${CMAKE_SOURCE_DIR}/libs/xdelta3)
add_executable(h2patch
	${CMAKE_SOURCE_DIR}/h2patch/h2patch3.c
	${H2CMN_DIR}/qsnprint.c
	${XDELTA_DIR}/xdelta3.c
	${CMAKE_SOURCE_DIR}/h2patch/h2patch3.rc
)
target_include_directories(h2patch PRIVATE
	${CMAKE_SOURCE_DIR}/h2patch
	${H2CMN_DIR}
	${XDELTA_DIR}
	${OSLIBS_WIN}/misc/include
)
target_compile_definitions(h2patch PRIVATE WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
# h2patch3.rc embeds upstream's manifest; don't let the linker add a second one.
target_link_options(h2patch PRIVATE /MANIFEST:NO)
# xdelta3 settings from upstream's Visual Studio project (Win32 file I/O).
set_source_files_properties(${XDELTA_DIR}/xdelta3.c PROPERTIES COMPILE_DEFINITIONS
	"XD3_DEBUG=0;XD3_STDIO=0;XD3_POSIX=0;XD3_WIN32=1;XD3_USE_LARGEFILE64=1")
