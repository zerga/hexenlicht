# Hexenlicht — Quake II RTX code intake

Hexenlicht's renderer reuses code from NVIDIA's
[Quake II RTX](https://github.com/NVIDIA/Q2RTX) (`src/refresh/vkpt`,
GPL-2.0-or-later, archived on 2025-12-11). This page maps its modules to
ours and says how code comes in. Each story that imports a module updates its
row. The last Q2RTX commit is `f2526e9a1` (2025-12-10). Read its files from a
local clone (`git clone --depth 1 https://github.com/NVIDIA/Q2RTX`, outside
this repository) or one at a time with
`gh api "repos/NVIDIA/Q2RTX/contents/src/refresh/vkpt/<path>?ref=f2526e9a1" --jq .content | base64 -d`.

## Rules

- **Ray queries only.** Q2RTX can trace with ray-tracing pipelines or with
  ray queries (`KHR_RAY_QUERY`, compute shaders); on NVIDIA it picks ray
  queries. Hexenlicht has no ray-tracing pipelines, shader binding tables
  or hit shaders (`.rchit`, `.rahit`, `.rmiss`, `.rint`); Q2RTX's `.rgen`
  shaders are compiled as compute shaders with `-DKHR_RAY_QUERY` (the build
  rule in `cmake/HexenlichtShaders.cmake`, since 3.2; other shaders that
  include `path_tracer_rgen.h`, such as `debug_view.comp`, define
  `KHR_RAY_QUERY` themselves). `VK_DispatchRays` rounds the launch up to
  8x8 workgroups and Q2RTX's `.rgen` shaders don't check their launch
  bounds (its ray-tracing pipelines launch exact sizes): every imported
  `.rgen` returns at once past `global_ubo.width / 2` × `height`, or its
  padding threads write into the other checkerboard field. The
  shaders keep their `#ifdef KHR_RAY_QUERY` branches, so a pipeline path can
  still be added (7.2, if shader execution reordering is worth it). The TLAS
  instances still carry Q2RTX's shader binding table offsets (`SBTO_*`),
  because its ray-query code picks a candidate's hit logic by them.
- **Q2RTX's names, our bindings.** Shaders read what Q2RTX's read under the
  same names (`global_ubo`, `IMG_*`/`TEX_*`, `instance_buffer`,
  `load_and_transform_triangle`, `get_material_info`, ...), but only three
  descriptor sets exist (sets 0 and 1 are numbered as in Q2RTX's compute
  passes; Q2RTX keeps the bindless array in set 1 with the images and its
  vertex buffers in set 2):
  - set 0: the global UBO (`shaders/global_ubo.h`, `vk_ubo.c`), one per
    frame in flight;
  - set 1: the render-target images, storage and sampled
    (`shaders/global_textures.h`, `vk_images.c`), an even and an odd set that
    swap the A/B images;
  - set 2: the bindless textures (`vk_texture.c`).

  Everything else — primitives, instances, TLAS instance info, materials,
  PVS, effects, the TLASes — is read by device address from the UBO's
  Hexenlicht block. Q2RTX's vertex-buffer and ray-tracing descriptor sets do
  not exist; the headers that declare descriptors or set numbers are the
  ones that are adapted: `global_ubo.h`, `global_textures.h`,
  `vertex_buffer.h`, `path_tracer.h` (the UBO's set), `path_tracer_hit_shaders.h`
  (the effects' texel buffers) and `path_tracer_rgen.h` (3.2: its TLAS array
  becomes `TLAS_GEOMETRY`/`TLAS_EFFECTS` by device address,
  `GLOBAL_TEXTURES_DESC_SET_IDX 2` and `VERTEX_BUFFER_DESC_SET_IDX 3` become
  1 and unused). A shader defines `GLOBAL_UBO_DESC_SET_IDX`,
  `GLOBAL_TEXTURES_DESC_SET_IDX` and `VERTEX_BUFFER_DESC_SET_IDX` before
  including them; a fragment shader also defines
  `GLOBAL_TEXTURES_SAMPLED_ONLY` (no writable storage images there).
- **Shaders come in almost verbatim**, with Q2RTX's file names, into
  `engine/hexenlicht/shaders/`; every change is marked `Hexenlicht:`. Files
  copied unchanged stay unchanged. All shaders are compiled with
  `-DVKPT_SHADER`, as in Q2RTX.
- **Host code is adapted** into our `vk_*` modules and style (VMA, one
  command buffer per frame, uHexen2's cvars and console).
- **Images come with their passes.** `global_textures.h`'s image lists hold
  only the images of the passes imported so far, with Q2RTX's names and
  formats. They are created at the swapchain's size; the 3D view renders
  into their top left `global_ubo.width x global_ubo.height`.
- **Q2RTX's cvars** (`UBO_CVAR_LIST`: `pt_*`, `flt_*`, `tm_*`) are registered
  with Q2RTX's defaults; each does something once the pass that reads it
  is imported.
- Copyright lines stay; ours is added to files we change. `THIRD_PARTY.md`
  lists the files.
- New modules join `vk_core.c`'s init table: `VK_INIT_DEFAULT` at
  startup, `VK_INIT_SWAPCHAIN` also when the swapchain is recreated,
  `VK_INIT_RELOAD_SHADER` also on `vk_reload_shaders` (pipelines).

## Host modules (`src/refresh/vkpt/*.c`)

| Q2RTX | What it does | Hexenlicht | Story |
|---|---|---|---|
| `main.c` | instance, device, swapchain, frame loop, entities, UBO, dynamic lights, readback, dynamic resolution | `vk_core.c`, `vk_swapchain.c` (E1); `vk_instance.c` (E2); init table in `vk_core.c`, `prepare_ubo` in `vk_ubo.c` (3.1); frame loop in `r_scene.c`/`vk_view.c`, grows per pass; dynamic lights 4.4; readback 3.7; dynamic resolution 3.8 | E1, E2, 3.1, … |
| `uniform_buffer.c` | global UBO | `vk_ubo.c` | 3.1 |
| `textures.c` | texture upload, bindless set, render targets, blue noise, env map, fake emissive, normal map normalization | `vk_texture.c` (1.5); render targets `vk_images.c` (3.1); blue noise `vk_images.c` (3.2, CC0 textures, see the open questions); env map 4.6; fake emissive 4.5; normalization 5.3 | 1.5, 3.1, 3.2, … |
| `path_tracer.c` | acceleration structures, pipelines, dispatch | `vk_accel.c` (2.6); pass layouts and ray-query dispatch `vk_pathtracer.c` (3.1); the passes 3.2–3.5 | 2.6, 3.1, … |
| `matrix.c` | view and projection matrices | `vk_matrix.c` | 3.1 |
| `vk_util.c/.h` | buffers, barriers, labels | `vk_buffer.c` (VMA); image barriers in `vk_pathtracer.c` | E1, 3.1 |
| `draw.c` | 2D, final blit | `vk_draw.c` (1.6); final blit = `view_composite.frag`; underwater warp 6.6 | 1.6, 6.6 |
| `bsp_mesh.c` | BSP primitives, PVS, light polygons, cluster light lists, sky clusters | `vk_world.c`, `vk_pvs.c` (2.1, 2.2); light polygons 3.3; cluster light lists 3.4; sky 4.6 | 2.1, 2.2, 3.3, 3.4, 4.6 |
| `vertex_buffer.c` | world and model buffers, light buffer, light stats | `vk_world.c`, `vk_model.c` (E2); light buffer 3.3; light stats 3.4 | E2, 3.3, 3.4 |
| `models.c` | MD2/MD3/IQM loading | `vk_model.c` (Hexen II's MDL) | — |
| `material.c/.h` | materials, `.mat` files | `vk_material.c` (2.1); PBR materials 5.3 | 2.1, 5.3 |
| `transparency.c` | particles, sprites, beams | `vk_effects.c` (2.5); beams 6.3 | 2.5, 6.3 |
| `asvgf.c` | A-SVGF denoiser, TAA | 3.6; TAAU 3.8 | 3.6, 3.8 |
| `tone_mapping.c`, `bloom.c` | tone mapping, auto exposure, bloom | 3.7 | 3.7 |
| `fsr.c`, `fsr/` | AMD FSR 1 | 3.8 | 3.8 |
| `profiler.c` | GPU timers | 3.11 | 3.11 |
| `physical_sky.c`, `precomputed_sky.c` | physical sky, sun | 4.6 | 4.6 |
| `conversion.c/.h`, `dds.h` | half floats, DDS | as needed; DDS 5.2 | 5.2 |
| `shadow_map.c`, `god_rays.c` | sun shadow map, volumetric light | not planned; reconsider at 4.6 | — |
| `fog.c` | fog volumes | not planned; reconsider at 6.5 | — |
| `debug.c` | debug lines and text | not planned; maybe 4.8 | — |
| `cameras.c`, `freecam.c`, `mgpu.c`, `device_memory_allocator.c`, `buddy_allocator.c` | security cameras, photo mode camera, multi-GPU, memory allocators | not used (VMA) | — |

## Shaders (`src/refresh/vkpt/shader/`)

| Q2RTX | Hexenlicht | Story |
|---|---|---|
| `constants.h`, `shader_structs.h`, `utils.glsl`, `projection.glsl`, `path_tracer_transparency.glsl` | imported (`utils.glsl`, `projection.glsl`, `path_tracer_transparency.glsl` unchanged) | 3.1 |
| `global_ubo.h`, `global_textures.h`, `vertex_buffer.h`, `path_tracer.h`, `path_tracer_hit_shaders.h` | imported, adapted to our bindings (see the rules) | 3.1 |
| `instance_geometry.comp` | `model_geometry.comp` | 2.4a |
| `stretch_pic.*`, `final_blit.*` | `draw2d.*`, `fullscreen.vert` + `view_composite.frag` | 1.6, 2.7 |
| `primary_rays.rgen`, `path_tracer_rgen.h`; `brdf.glsl`, `water.glsl`, `asvgf.glsl` (unchanged, included by `path_tracer_rgen.h`) | G-buffer in Q2RTX's checkerboard fields; `path_tracer_rgen.h` without the lighting functions (3.3) and the gradient samples (3.6) | 3.2 |
| `direct_lighting.rgen`, `compositing.comp`, `checkerboard_interleave.comp` | first lit image, without the denoiser | 3.3 |
| `light_lists.h` | per-cluster light lists | 3.4 |
| `indirect_lighting.rgen`, `reflect_refract.rgen` | bounces, reflections, refraction | 3.5 |
| `asvgf_*.comp` (not `asvgf_taau.comp`) | denoiser | 3.6 |
| `tone_mapping_*.comp`, `tone_mapping_utils.glsl`, `bloom_*.comp` | exposure, tone curve, bloom | 3.7 |
| `asvgf_taau.comp`, `fsr_*` | upscaling | 3.8 |
| `physical_sky*.comp`, `precomputed_sky*`, `sky.h`, `sky_buffer_resolve.comp` | skies | 4.6 |
| `normalize_normal_map.comp` | PBR materials | 5.3 |
| `path_tracer_beam.*` | beams | 6.3 |
| `*.rchit`, `*.rahit`, `*.rmiss`, `*.rint` | not used: ray queries only | — |
| `animate_materials.comp` | not used: materials animate while tracing (`vertex_buffer.h`) | — |
| `god_rays*.comp`, `shadow_map.vert`, `debug_line.*` | not planned | — |
| `tiny_encryption_algorithm.h` | not used: nothing in Q2RTX includes it | — |

## Open questions for later stories

- **Water normal map (3.5, 6.5).** Q2RTX's water waves (`get_water_normal`)
  sample `textures/water_n.tga` from its media package; we need our own
  (generated or CC0). Until then the water keeps its geometric normal.
- **Specular hit distance (3.5).** DLSS Ray Reconstruction wants the
  specular hit distance (or specular motion vectors) of the reflection
  bounce; Q2RTX's reflection pass doesn't store it. The other RR inputs come
  from the G-buffer (RENDERER.md, 3D view).
- **Checkerboard fields and RR (3.9).** At translucent surfaces Q2RTX puts one
  field on the surface and the other through it, so an interleaved G-buffer
  alternates between the two surfaces pixel by pixel there.
- **Model tint brightness (E4).** `colorshade` tints reach 10 (GL multiplies
  the vertex light, then clamps); the G-buffer takes only the hue.
- **Instance history (3.6).** A-SVGF's gradient reprojection maps last
  frame's instances to this frame's (`model_prev_to_current`); our entity
  history in `vk_instance.c` has to provide it.
- **Effects brightness (3.3).** Q2RTX scales particles and sprites by the
  exposure; the debug view shows GL's colors unlit.
