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
| [stb_image_write](https://github.com/nothings/stb) | 1.16 (commit `2c980bb`) | MIT or public domain | `libs/stb` | Writing PNGs (the texture export, `r_exporttextures`) |
| [Free blue noise textures](http://momentsingraphics.de/BlueNoise.html) (Christoph Peters) | `FreeBlueNoiseTextures.zip` of 2025-05-12, `64_64/HDR_RGBA_*` only | CC0 1.0 | `libs/bluenoise` | The path tracer's random numbers |
| [AMD FidelityFX Super Resolution 1.0](https://github.com/GPUOpen-Effects/FidelityFX-FSR) (`ffx_a.h`, `ffx_fsr1.h`) | v1.0.2 (commit `a21ffb8f6`) | MIT | `libs/fsr1` | FSR 1 upscaling and sharpening (EASU, RCAS) |
| [NVIDIA Streamline](https://github.com/NVIDIA-RTX/Streamline) headers (18 of `include/`, listed in `libs/streamline/README.md`; not `sl_nvperf.h`) | v2.14.1 (`streamline-sdk-v2.14.1.zip`) | MIT | `libs/streamline` | Optional DLSS SR and RR with the player's DLLs (`vk_streamline.cpp`); no Streamline or NVIDIA binary is in the repository or releases |

`libs/vma/vma_impl.cpp`, `libs/stb/stb_image_impl.c` and
`libs/stb/stb_image_write_impl.c` are Hexenlicht's own files
(GPL-2.0-or-later) that compile the libraries' implementations.

Source code adapted from other projects keeps its copyright lines in the
file headers:

| Project | License | Adapted in |
|---|---|---|
| [Quake II RTX](https://github.com/NVIDIA/Q2RTX) (archived 2025-12-11; commit `f2526e9a1`) | GPL-2.0-or-later | `engine/hexenlicht/vk_world.c`, `engine/hexenlicht/vk_pvs.c`, `engine/hexenlicht/vk_instance.c`, `engine/hexenlicht/vk_accel.c`, `engine/hexenlicht/vk_effects.c`, `engine/hexenlicht/vk_core.c` (the module table), `engine/hexenlicht/vk_matrix.c`, `engine/hexenlicht/vk_ubo.c`, `engine/hexenlicht/vk_images.c`, `engine/hexenlicht/vk_pathtracer.c`, `engine/hexenlicht/vk_asvgf.c`, `engine/hexenlicht/vk_bloom.c`, `engine/hexenlicht/vk_tonemap.c`, `engine/hexenlicht/vk_upscale.c` (with Q2RTX's `fsr.c`, Frank Richter's), `engine/hexenlicht/vk_light.c`, `engine/hexenlicht/vk_emissive.c` (`apply_fake_emissive_threshold` from `textures.c`), `engine/hexenlicht/vk_sky.c` (the sun's UBO fields from `physical_sky.c`, the sky visibility from `bsp_mesh.c`), `engine/hexenlicht/vk_profiler.c`; shaders in `engine/hexenlicht/shaders/`: `hl_shared.h`, `constants.h`, `shader_structs.h`, `global_ubo.h`, `global_textures.h`, `vertex_buffer.h`, `path_tracer.h`, `path_tracer_hit_shaders.h`, `path_tracer_rgen.h`, `light_lists.h`, `utils.glsl`, `primary_rays.rgen`, `reflect_refract.rgen`, `direct_lighting.rgen`, `indirect_lighting.rgen`, `asvgf_temporal.comp`, `asvgf_taau.comp`, `fsr_easu.glsl`, `fsr_rcas.glsl`, `tone_mapping_curve.comp`, `checkerboard_interleave.comp`, `asvgf_gradient_img.comp`, `debug_view.comp`, `model_geometry.comp`, `view_composite.frag` (`filter_lanczos` from `final_blit.frag`), `compositing.comp`, `asvgf_atrous.comp`; copied unchanged: `projection.glsl`, `path_tracer_transparency.glsl`, `brdf.glsl`, `water.glsl`, `asvgf.glsl`, `asvgf_gradient_reproject.comp`, `asvgf_gradient_atrous.comp`, `asvgf_lf.comp`, `tone_mapping_histogram.comp`, `tone_mapping_apply.comp`, `tone_mapping_utils.glsl`, `bloom_downscale.comp`, `bloom_blur.comp`, `bloom_composite.comp`, `fsr_easu_fp32.comp`, `fsr_rcas_fp32.comp`, `fsr_utils.glsl`. How modules come in: [docs/hexenlicht/Q2RTX.md](docs/hexenlicht/Q2RTX.md) |
| [QuakeSpasm](https://sourceforge.net/projects/quakespasm/) | GPL-2.0-or-later | `engine/hexenlicht/vk_instance.c` (alias model frame blending, after `R_SetupAliasFrame`; movement blending, after `R_SetupEntityTransform`) |
| JsH2Colour (upstream's `utils/jsh2color`: Juraj Styk, O. Sezer; from MHColour and Tyrlite) | GPL-2.0-or-later | `engine/hexenlicht/vk_lightcolor.c` (the per-light color step, its texture lists, sample points and line trace) |
| utils/light (upstream's lightmap compiler: Id Software, Raven Software) | GPL-2.0-or-later | `engine/hexenlicht/vk_lightfit.c` (`ltface.c`'s face setup, sample points and a light's value, `trace.c`'s line trace) |

Used from the LunarG Vulkan SDK at build time, not vendored:

| Component | License | Used for |
|---|---|---|
| Vulkan headers (Khronos) | Apache-2.0 OR MIT — used under MIT | Vulkan API declarations |
| glslangValidator | BSD-3-Clause and others (see the SDK) | Compiling shaders at build time; not part of the binary |

Used outside the repository by `tools/hexenlicht/texpack` (story 5.8), downloaded
by whoever makes a texture pack, never vendored or shipped; the pack it makes is
derived from Raven's textures and stays out of the repository and releases:

| Component | License | Used for |
|---|---|---|
| [ComfyUI](https://github.com/comfyanonymous/ComfyUI) portable build (its Python, PyTorch, spandrel) | GPL-3.0 (PyTorch BSD-3-Clause) | Runs the diffusion stage headless and the tool's own code |
| [PBRify_Remix](https://github.com/Kim2091/PBRify_Remix) 1.7.2 models (4x upscaler, normal, roughness, height) | CC0 1.0 | The 4x upscale and the normal and roughness maps |
| [Stable Diffusion 1.5](https://huggingface.co/stable-diffusion-v1-5/stable-diffusion-v1-5) | CreativeML OpenRAIL-M | The img2img pass |
| [ControlNet 1.1 Tile](https://huggingface.co/lllyasviel/control_v11f1e_sd15_tile) (fp16 safetensors) | CreativeML OpenRAIL-M | Holds the upscale's shapes and colors in the img2img pass |
| [Qwen3-VL-4B-Instruct](https://huggingface.co/Qwen/Qwen3-VL-4B-Instruct) | Apache-2.0 | Drafting the manifest's classes and descriptions (a tool, not shipped code) |
| [FLUX.2 klein 4B](https://huggingface.co/black-forest-labs/FLUX.2-klein-4b-fp8) (distilled and base, fp8) with its Qwen3 4B text encoder and the FLUX.2 VAE ([Comfy-Org's repackage](https://huggingface.co/Comfy-Org/vae-text-encorder-for-flux-klein-4b)) | Apache-2.0 | `redraw.py`'s edit (story 9.1; the generator since M50) |
| [Qwen-Image-Edit-2511](https://huggingface.co/Comfy-Org/Qwen-Image-Edit_ComfyUI) (fp8mixed) with Qwen2.5-VL 7B and the Qwen-Image VAE ([Comfy-Org](https://huggingface.co/Comfy-Org/Qwen-Image_ComfyUI)), [lightx2v's 8-step Lightning LoRA](https://huggingface.co/lightx2v/Qwen-Image-Edit-2511-Lightning) | Apache-2.0 | `redraw.py --gen qwen`, 9.1's comparison |

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
