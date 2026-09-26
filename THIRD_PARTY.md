# Third-party components

Hexenlicht is licensed under the GPL, version 2 or later (see
[docs/COPYING](docs/COPYING)). The components below keep their own licenses.

## Rules

From the [project plan](docs/hexenlicht/PLAN.md#4-licensing-rules):

- Vendored libraries must be GPL-compatible (MIT, BSD, zlib, LGPL, ...).
  Apache-2.0-only code would make the binary GPLv3 and needs an explicit
  decision first.
- Every vendored library keeps its license file next to its source and gets
  a row in the table below.
- **Never** committed to this repository or included in releases:
  NVIDIA DLSS/NGX binaries or static libraries (`nvngx_*.dll`,
  `nvsdk_ngx_*.lib`), Streamline's proprietary plugin binaries, Hexen II game
  data, or textures derived from Raven's textures. Optional DLSS support
  works with files the player downloads from NVIDIA.

## Added by Hexenlicht

| Component | Version | License | Location | Used for |
|---|---|---|---|---|
| [volk](https://github.com/zeux/volk) | 1.4.350 | MIT | `libs/volk` | Loading Vulkan functions at runtime |
| [Vulkan Memory Allocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator) | 3.4.0 | MIT | `libs/vma` | GPU memory management |
| [stb_image](https://github.com/nothings/stb) | 2.30 (commit `2c980bb`) | MIT or public domain | `libs/stb` | Loading PNG/TGA textures |
| [Free blue noise textures](http://momentsingraphics.de/BlueNoise.html) (Christoph Peters) | `FreeBlueNoiseTextures.zip` of 2025-05-12, `64_64/HDR_RGBA_*` only | CC0 1.0 | `libs/bluenoise` | The path tracer's random numbers |

`libs/vma/vma_impl.cpp` and `libs/stb/stb_image_impl.c` are Hexenlicht's own
files (GPL-2.0-or-later) that compile the libraries' implementations.

Source code adapted from other projects keeps its copyright lines in the
file headers:

| Project | License | Adapted in |
|---|---|---|
| [Quake II RTX](https://github.com/NVIDIA/Q2RTX) (archived 2025-12-11; commit `f2526e9a1`) | GPL-2.0-or-later | `engine/hexenlicht/vk_world.c`, `engine/hexenlicht/vk_pvs.c`, `engine/hexenlicht/vk_instance.c`, `engine/hexenlicht/vk_accel.c`, `engine/hexenlicht/vk_effects.c`, `engine/hexenlicht/vk_core.c` (the module table), `engine/hexenlicht/vk_matrix.c`, `engine/hexenlicht/vk_ubo.c`, `engine/hexenlicht/vk_images.c`, `engine/hexenlicht/vk_pathtracer.c`, `engine/hexenlicht/vk_light.c`; shaders in `engine/hexenlicht/shaders/`: `hl_shared.h`, `constants.h`, `shader_structs.h`, `global_ubo.h`, `global_textures.h`, `vertex_buffer.h`, `path_tracer.h`, `path_tracer_hit_shaders.h`, `path_tracer_rgen.h`, `light_lists.h`, `utils.glsl`, `primary_rays.rgen`, `reflect_refract.rgen`, `direct_lighting.rgen`, `indirect_lighting.rgen`, `debug_view.comp`, `model_geometry.comp`; copied unchanged: `projection.glsl`, `path_tracer_transparency.glsl`, `brdf.glsl`, `water.glsl`, `asvgf.glsl`, `compositing.comp`, `checkerboard_interleave.comp`. How modules come in: [docs/hexenlicht/Q2RTX.md](docs/hexenlicht/Q2RTX.md) |
| [QuakeSpasm](https://sourceforge.net/projects/quakespasm/) | GPL-2.0-or-later | `engine/hexenlicht/vk_instance.c` (alias model frame blending, after `R_SetupAliasFrame`; movement blending, after `R_SetupEntityTransform`) |

Used from the LunarG Vulkan SDK at build time, not vendored:

| Component | License | Used for |
|---|---|---|
| Vulkan headers (Khronos) | Apache-2.0 OR MIT — used under MIT | Vulkan API declarations |
| glslangValidator | BSD-3-Clause and others (see the SDK) | Compiling shaders at build time; not part of the binary |

## Bundled by upstream (Hammer of Thyrion)

These come unchanged from upstream and are used by the existing uHexen2
builds.

| Component | License | Location |
|---|---|---|
| libTiMidity (MIDI playback) | LGPL-2.1 or Perl Artistic License | `libs/timidity` |
| xdelta3 (used by `h2patch`) | GPL-2.0-or-later | `libs/xdelta3` |
| SDL 1.2, prebuilt for Windows | LGPL-2.1 | `oslibs/windows/SDL` |
| Audio codec libraries, prebuilt (FLAC, libmad, libmikmod, mpg123, Ogg/Vorbis, Tremor, Opus/opusfile, libxmp) | Each project's own license | `oslibs/windows/codecs` |
| DirectDraw/DirectInput/DirectSound headers from the Wine project | LGPL-2.1-or-later | `oslibs/windows/dxsdk` |
| Build rules and libraries for DOS, OS/2, macOS, AROS, MorphOS | Various | `oslibs/*` (not used by Hexenlicht) |
