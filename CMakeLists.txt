# Hexenlicht — CMake build for Windows x64 with MSVC.
#
# This builds alongside the upstream Makefiles, which stay untouched and
# remain the reference for the source lists (see cmake/Hexen2Sources.cmake).
#
# Targets:
#   glhexen2   Hexen II with the upstream OpenGL renderer (glh2.exe)
#   h2patch    patches retail pak files to v1.11
#
# Use the presets in CMakePresets.json (CLion picks them up automatically):
#   cmake --preset windows-debug   &&  cmake --build --preset windows-debug

cmake_minimum_required(VERSION 3.25)

project(Hexenlicht LANGUAGES C RC)

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
