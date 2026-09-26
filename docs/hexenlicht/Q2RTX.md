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
  with Q2RTX's defaults (but `pt_particle_brightness`, 15 since 3.7); each
  does something once the pass that reads it is imported. `VK_PrepareUBO`
  overrides those Q2RTX's host code sets per
  mode until their passes exist: `pt_aperture` 0 (no accumulation mode);
  since 3.6 it sets `flt_temporal_*` to 0 when the denoiser has no
  history, as Q2RTX's `temporal_frame_valid` does; since 3.8 `flt_taa`
  (off without history), the jitter and the sizes follow `vk_upscale.c`
  (`r_upscaler`, `r_scale`).
- Copyright lines stay; ours is added to files we change. `THIRD_PARTY.md`
  lists the files.
- New modules join `vk_core.c`'s init table: `VK_INIT_DEFAULT` at
  startup, `VK_INIT_SWAPCHAIN` also when the swapchain is recreated,
  `VK_INIT_RELOAD_SHADER` also on `vk_reload_shaders` (pipelines).

## Host modules (`src/refresh/vkpt/*.c`)

| Q2RTX | What it does | Hexenlicht | Story |
|---|---|---|---|
| `main.c` | instance, device, swapchain, frame loop, entities, UBO, dynamic lights, readback, dynamic resolution | `vk_core.c`, `vk_swapchain.c` (E1); `vk_instance.c` (E2); init table in `vk_core.c`, `prepare_ubo` in `vk_ubo.c` (3.1); frame loop in `r_scene.c`/`vk_view.c`, grows per pass; `add_dlights` in `vk_light.c` (test dynamic sphere lights, 3.3), the game's dynamic lights 4.4; readback in `vk_tonemap.c` (3.7: only the adapted luminance, `prev_adapted_luminance`); `get_render_extent`, `evaluate_taa_settings` and the TAA jitter in `vk_upscale.c` (3.8: `r_scale` instead of `scr_viewsize`, our TAA mode without random sub-pixel offsets); dynamic resolution left out (3.11 or 7.2, see the open questions) | E1, E2, 3.1, 3.8, … |
| `uniform_buffer.c` | global UBO | `vk_ubo.c` | 3.1 |
| `textures.c` | texture upload, bindless set, render targets, blue noise, env map, fake emissive, normal map normalization | `vk_texture.c` (1.5); render targets `vk_images.c` (3.1); blue noise `vk_images.c` (3.2, CC0 textures, see the open questions); env map 4.6; fake emissive 4.5; normalization 5.3 | 1.5, 3.1, 3.2, … |
| `path_tracer.c` | acceleration structures, pipelines, dispatch | `vk_accel.c` (2.6); pass layouts and ray-query dispatch `vk_pathtracer.c` (3.1; specialization constants 3.5a); the passes 3.2–3.5b (`vk_view.c`: the bounces 3.5a) | 2.6, 3.1, … |
| `matrix.c` | view and projection matrices | `vk_matrix.c` | 3.1 |
| `vk_util.c/.h` | buffers, barriers, labels | `vk_buffer.c` (VMA); image barriers in `vk_pathtracer.c` | E1, 3.1 |
| `draw.c` | 2D, final blit | `vk_draw.c` (1.6); final blit = `view_composite.frag` (3.8: its Lanczos filter and scaling rule, nearest a texel fetch); underwater warp 6.6 | 1.6, 3.8, 6.6 |
| `bsp_mesh.c` | BSP primitives, PVS, light polygons, cluster light lists, sky clusters | `vk_world.c`, `vk_pvs.c` (2.1, 2.2); light polygons: test lights in `vk_light.c` (3.3), map lights E4; cluster light lists in `vk_light.c` (3.4: by the PVS of the leafs a light touches, a polygon's plane and a sphere's range; spheres in the lists; cluster bounds with the leaf's); sky 4.6 | 2.1, 2.2, 3.3, 3.4, 4.6 |
| `vertex_buffer.c` | world and model buffers, light buffer, light stats | `vk_world.c`, `vk_model.c` (E2); light buffer `vk_light.c` (3.3, lights and lists only; 3.4: spheres, lists copied when they change); light stats `vk_light.c` (3.4, per list entry) | E2, 3.3, 3.4 |
| `models.c` | MD2/MD3/IQM loading | `vk_model.c` (Hexen II's MDL) | — |
| `material.c/.h` | materials, `.mat` files | `vk_material.c` (2.1); PBR materials 5.3 | 2.1, 5.3 |
| `transparency.c` | particles, sprites, beams | `vk_effects.c` (2.5); beams 6.3 | 2.5, 6.3 |
| `asvgf.c` | A-SVGF denoiser, TAA | `vk_asvgf.c` (3.6: gradient reprojection and the filters, with the history reset of `main.c`'s `temporal_frame_valid`; `model_prev_to_current` from the entity history in `vk_instance.c`); `vkpt_taa` in `vk_upscale.c` (3.8) | 3.6, 3.8 |
| `tone_mapping.c`, `bloom.c` | tone mapping, auto exposure, bloom | `vk_tonemap.c`, `vk_bloom.c` (3.7: SDR only; no under-water bloom or menu blur; the effects scaled by the exposure with one factor for particles and sprites, `pt_particle_brightness` 15) | 3.7 |
| `fsr.c`, `fsr/` | AMD FSR 1 | `vk_upscale.c` (3.8: SDR and FP32 pipelines only; AMD's v1.0.2 headers in `libs/fsr1` instead of Q2RTX's `fsr/`, whose `ffx_fsr1.h` predates v1.0.2's RCAS fix) | 3.8 |
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
| `constants.h`, `shader_structs.h`, `utils.glsl`, `projection.glsl`, `path_tracer_transparency.glsl` | imported (`projection.glsl`, `path_tracer_transparency.glsl` unchanged; `utils.glsl`: `packRGBE` clamps, 3.3) | 3.1, 3.3 |
| `global_ubo.h`, `global_textures.h`, `vertex_buffer.h`, `path_tracer.h`, `path_tracer_hit_shaders.h` | imported, adapted to our bindings (see the rules) | 3.1 |
| `instance_geometry.comp` | `model_geometry.comp` | 2.4a |
| `stretch_pic.*`, `final_blit.*` | `draw2d.*`, `fullscreen.vert` + `view_composite.frag` (3.8: `filter_lanczos` with its taps clamped to the input; no water warp until 6.6, no debug lines) | 1.6, 2.7, 3.8 |
| `primary_rays.rgen`, `path_tracer_rgen.h`; `brdf.glsl`, `water.glsl`, `asvgf.glsl` (unchanged, included by `path_tracer_rgen.h`) | G-buffer in Q2RTX's checkerboard fields (3.5b: vertical water stays water); `path_tracer_rgen.h` (its lighting functions since 3.3, `get_is_gradient` since 3.6) | 3.2, 3.3, 3.5b, 3.6 |
| `direct_lighting.rgen`, `compositing.comp`, `checkerboard_interleave.comp` | first lit image, without the denoiser (the last two unchanged; `direct_lighting.rgen`: launch check, the specular hit distance cleared (3.5a), the weapon only shadows itself, no sunlight, caustics off) | 3.3 |
| `light_lists.h` | imported (3.3); spheres in the lists and the light statistics per list entry, no pick of a light without mass (Q2RTX's at `rng.x` 0: NaN), a sphere's solid angle in a form precise far away (3.4); without the light-count history (3.6: our lists change only with the lights; see the open questions) and sky lights (4.6) | 3.3, 3.4 |
| `indirect_lighting.rgen` | bounces, glossy reflections (3.5a: launch check, half resolution with (h + 1) / 2 rows, the weapon only in its own rays, bounce hits on models tinted, the specular hit distance stored, no sunlight) | 3.5a |
| `reflect_refract.rgen` | through translucent surfaces and models, off mirrors and glass (3.5b: launch check, water and slime skipped and no vertical water as glass, the weapon in no ray, the water normal only with a map, no god rays) | 3.5b |
| `asvgf_*.comp` (not `asvgf_taau.comp`) | denoiser (3.6: unchanged but `asvgf_temporal.comp`, where a gradient sample blends into its pixel's history only as far as the anti-lag drops it) | 3.6 |
| `tone_mapping_*.comp`, `tone_mapping_utils.glsl`, `bloom_*.comp` | exposure, tone curve, bloom (3.7: unchanged but `tone_mapping_curve.comp`'s launch check, removed; the tone mapping and readback buffers by device address, `vertex_buffer.h`; the apply shader's HDR variant and full screen blend unused) | 3.7 |
| `asvgf_taau.comp` | the TAA pass (3.8: threads past the TAA output write their zero only inside the images; `HQ_COLOR_INTERLEAVED` 1x1) | 3.8 |
| `fsr_easu_fp32.comp`, `fsr_rcas_fp32.comp`, `fsr_easu.glsl`, `fsr_rcas.glsl`, `fsr_utils.glsl` | FSR 1 (3.8: the `.comp` files and `fsr_utils.glsl` unchanged; EASU's and RCAS's input clamped to the rendered part and the view, no writes past the view); the FP16 variants not imported (7.2) | 3.8 |
| `physical_sky*.comp`, `precomputed_sky*`, `sky.h`, `sky_buffer_resolve.comp` | skies | 4.6 |
| `normalize_normal_map.comp` | PBR materials | 5.3 |
| `path_tracer_beam.*` | beams | 6.3 |
| `*.rchit`, `*.rahit`, `*.rmiss`, `*.rint` | not used: ray queries only | — |
| `animate_materials.comp` | not used: materials animate while tracing (`vertex_buffer.h`) | — |
| `god_rays*.comp`, `shadow_map.vert`, `debug_line.*` | not planned | — |
| `tiny_encryption_algorithm.h` | not used: nothing in Q2RTX includes it | — |

## Open questions for later stories

- **Water normal map (6.5).** Q2RTX's water waves (`get_water_normal`)
  sample `textures/water_n.tga` from its media package; we need our own
  (generated or CC0). Until then the water keeps its geometric normal.
- **Physical water (6.5).** Since 3.5b water and slime stay opaque and
  textured as GL draws them; Q2RTX's physical water (Fresnel reflection and
  refraction, extinction, its textures replaced) is in `reflect_refract.rgen`
  behind that skip. 6.5 decides, and must keep Hexen II's vertical
  turbulent surfaces opaque whatever it picks: most of its "water" is walls
  (egypt1's 48 water faces are all vertical `*rtex386` and `*skullwarp`),
  real pools are rare (demo1 2 faces, demo3 8). The caustics trace
  (`path_tracer_rgen.h`, off until then) still treats vertical water as
  glass.
- **Specular hit distance (3.9).** DLSS Ray Reconstruction wants the
  specular hit distance (or specular motion vectors). Since 3.5a the first
  bounce stores it where it traced a specular ray (`PT_SPECULAR_HIT_DIST`,
  0 elsewhere: half the pixels of rough surfaces, and the rows half
  resolution skips). On surfaces rougher than `pt_fake_roughness_threshold`
  (all of Hexen II's until E5) and at half resolution (roughness 1, metal
  0) the stored ray is one that contributes nothing, whose distance RR may
  not want. The 3.9 spike finds out whether RR needs a value in every
  pixel, and whether 3.5b's mirror and glass paths need theirs (Q2RTX's
  reflection pass doesn't store it).
- **Smooth surfaces and sphere lights (E5, 4.5).** Sphere lights are not
  geometry, so no ray hits them. Surfaces smoother than
  `pt_direct_roughness_threshold` (0.18) get their specular only from the
  specular bounce and show no highlight of a sphere (Q2RTX's dynamic lights
  alike); with `pt_roughness_override 0.02` demo1's start is 35 % darker
  (the rough surfaces' direct specular is as strong as their diffuse there).
  Options when E5 brings smooth materials: direct specular for spheres at
  every roughness (no double counting, since bounces can't hit them, but
  noisy for mirrors), or visible emitters (4.5's emissive flames).
- **Dark albedo (E4, E5).** Hexen II's textures are dark in linear light:
  the cathedral's mean diffuse albedo is 0.04 (sRGB ~55), so one bounce adds
  2–3 % to the lit image, where lighter PBR textures would get tens of
  percent. The calibration (4.9) or the materials (E5) decide whether that
  stays.
- **Checkerboard fields and RR (3.9).** At translucent surfaces Q2RTX puts one
  field on the surface and the other through it, so an interleaved G-buffer
  alternates between the two surfaces pixel by pixel there.
- **Model tint brightness (E4).** `colorshade` tints reach 10 (GL multiplies
  the vertex light, then clamps); the G-buffer takes only the hue.
- **Light styles and the gradients (4.2).** A gradient sample weighs the
  lights with last frame's light style (`prev_style_scale`, light_lists.h),
  so it picks the light last frame picked. `vk_light.c` writes 1 for both
  until light styles exist; 4.2 must fill `prev_style_scale` from last
  frame's styles (Q2RTX's `prev_lightstyles`), or flickering lights make
  needless gradients (the denoiser drops history and is noisier near them).
- **Denoiser brightness (4.9).** With noisy lighting the denoised image is
  2–5 % brighter than the average of the raw frames (3.6, direct lighting
  at a sixteenth of the test lights' intensity: demo1 +2 %, the cathedral
  +5 %): the gradient sample is the brightest of its 3x3 square, so that
  pixel keeps a bright history one frame longer. Q2RTX's full blend of the
  replayed sample made it 8–18 % (3.6 blends it in only as far as the
  anti-lag drops the history, `asvgf_temporal.comp`). The calibration
  against GL (4.9) measures the
  denoised image; a random pick (the A-SVGF paper's) would remove the rest
  but lose Q2RTX's anti-lag for moving lights.
- **Exposure and the mood (4.9, 4.10, 4.7).** Since 3.7 Q2RTX's tone
  mapper and auto exposure run with its defaults: its curve lifts the
  shadows of Hexen II's dark scenes, and the exposure brightens a dark
  place over a few seconds down to `tm_min_luminance` (0.0002; the test
  lights' demo1 start adapts to ~0.04–0.07). The calibration against GL
  (4.9) picks `tm_exposure_bias`, `tm_reinhard` and the rest; the
  darkness mechanics (4.10) need a `tm_min_luminance` that keeps dark
  puzzle areas dark; per-map exposure is 4.7's.
- **Effects brightness (6.3, 6.2).** Since 3.7 particles and sprites share
  one exposure factor (`pt_particle_brightness` 15, measured on meteor
  staff particles); Q2RTX has separate ones for sprites, beams and
  explosions. Beams (6.3) and the effect groups (6.2) check it against GL.
- **Lights inside solid (4.1).** Some light entities have their origin
  inside a wall: demo1 43 of 332, demo3 26, village1 26, village2 11, a few
  elsewhere; nearly all plain `light` entities (a `light_torch_meso`,
  `light_torch_rome`, two `light_torch_castle` and a
  `light_flame_small_yellow` too). Their 8-unit test spheres touch no open
  leaf, so 3.4's lists leave them out (`vk_lights` counts them). 4.1 decides
  whether the original lit anything from them (utils/light traces from the
  origin) and moves or drops them.
- **Dynamic lights in the light lists (4.4).** The game's moving lights
  stay the UBO's up to 32 dynamic spheres, picked uniformly with no
  culling; Q2RTX injects its moving model lights into the lists every frame
  (`inject_model_lights`), which would move the light statistics' entries:
  a second range of entries per list would keep them in place. Lists that
  change every frame also need Q2RTX's light-count history
  (`light_counts_history`, left out in 3.6), so a gradient sample picks
  from the count its replayed frame had.
- **Dynamic resolution (3.11, 7.2).** Q2RTX's `drs_*` steer the render
  scale by the measured frame time; uHexen2 caps frames at 72 fps outside
  timedemo, which hides it, and there are no GPU timers before 3.11. The
  render size can already change every frame (`r_scale`; the denoiser and
  the TAA reproject across it), so a controller is all it needs.
- **Hot spots and the TAA's PQ clamp (4.9).** The TAA pass stores the lit
  image PQ-encoded, which clamps at 10000 cd/m²: 78 in linear units before
  the ×128 storage scale. Walls next to the test lights exceed it, and the
  bloom around them is then up to 10/255 weaker than before 3.8. The
  calibration (4.9) decides whether real lights stay below it.
- **Reference shots by accumulation (4.9).** Q2RTX's reference mode
  (`pt_accumulation_rendering`: `temporal_blend_factor`, the
  `HQ_COLOR_INTERLEAVED` accumulator in `asvgf_taau.comp`, random primary
  ray offsets) averages frames of a still scene into an unbiased image. The
  shader path is imported; the host side and a full-size image would give
  4.9 its reference shots.
