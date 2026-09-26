# Hexenlicht — renderer reference

How the `hexenlicht` target's renderer (`engine/hexenlicht/`) is built, module
by module. Read the section you need; each file's header comment has more.
Decisions and their reasons are in [DECISIONS.md](DECISIONS.md), the Quake II
RTX import rules in [Q2RTX.md](Q2RTX.md), testing in [TESTING.md](TESTING.md).
Keep this file current: a PR that changes a module updates its section.

Contents: [Build](#build-target) · [Window](#window-and-video-modes-vid_vkc) ·
[Vulkan core](#vulkan-core-and-swapchain) · [Shaders](#shaders) ·
[Textures](#textures-vk_texturec) · [2D](#2d-vk_drawc) · [Scene](#scene-r_scenec) ·
[Buffers and layouts](#buffers-and-gpu-data-layouts) · [Materials](#materials-vk_materialc) ·
[World](#world-vk_worldc) · [PVS](#pvs-vk_pvsc) · [Instances](#instances-vk_instancec) ·
[Alias models](#alias-models-vk_modelc) · [Skins](#skins-vk_skinc) ·
[Effects](#effects-vk_effectsc) · [Acceleration structures](#acceleration-structures-vk_accelc) ·
[Path tracer framework](#path-tracer-framework) · [3D view](#3d-view-vk_viewc) ·
[Other](#other) · [Console commands](#console-commands)

## Build target

- Root `CMakeLists.txt` + `CMakePresets.json` (presets `windows-debug`,
  `windows-release`; Ninja; output in `build/<preset>/bin/`). Source lists
  live in `cmake/Hexen2Sources.cmake` and mirror the win64 object lists in
  `engine/hexen2/Makefile` — when an upstream merge changes those lists,
  update that file too.
- `glhexen2` (output `glh2.exe`) is the unmodified OpenGL client, the look
  reference.
- `hexenlicht` = `H2_COMMON_SOURCES` + `H2_HEXENLICHT_REUSED_SOURCES`
  (GL-renderer files without GL calls: `gl_model.c`, `gl_mesh.c`,
  `r_part.c`, `gl_refrag.c`, `gl_screen.c`) + the `engine/hexenlicht/`
  files listed in `add_executable(hexenlicht ...)` in `CMakeLists.txt` (no
  glob: add new files there and re-run `cmake --preset`), compiled with
  `GLQUAKE` (client code takes its hardware-renderer paths) and `HEXENLICHT`.
- Needs the LunarG Vulkan SDK 1.3+ via `VULKAN_SDK` (CI pins the version in
  `.github/workflows/build-windows.yml`). Vendored static-library targets:
  `volk` (`libs/volk`), `vma` (`libs/vma`, C++ implementation in
  `vma_impl.cpp`, volk's loaders via `VMA_DYNAMIC_VULKAN_FUNCTIONS`),
  `stb_image` (`libs/stb`, PNG/TGA only, `STBI_NO_STDIO`). Versions and
  licenses are in `THIRD_PARTY.md`; update it when a vendored library changes.
- Codec DLLs from `oslibs/windows/codecs/x64` are copied next to the exe
  post-build.
- `engine/hexenlicht/stubs.c` provides the renderer symbols not implemented
  yet, sectioned by the story that replaces them; a story moves its section
  into real files. It still holds `R_InitTextures`/`r_notexture_mip` (GL's
  checkerboard), the rest of `R_Init`, `R_InitSky` (→ 4.6) and GL-named cvars
  kept so configs keep their settings (`gl_glows`, `gl_coloredlight`,
  `gl_lightmapfmt`, …). To find what a renderer must provide, link without it
  and read the unresolved externals.

## Window and video modes (`vid_vk.c`)

Win32 window layer derived from `gl_vidnt.c`, no OpenGL.

- One window for the program's lifetime. Fullscreen is borderless at desktop
  resolution with `modestate == MS_FULLDIB` (menu and input code treat
  non-`MS_WINDOWED` as "fullscreen, mouse captured"). `vid_restart` only
  restyles/resizes the window (`vid_setting_mode` keeps its `WM_SIZE`s from
  counting as user resizes).
- The windowed mode is resizable and maximizable: `WM_SIZE` →
  `VID_WindowResized`; the client size (`window_width/height`) drives
  `vid.width/height` through the UI scale; a normal window's size becomes the
  current windowed mode (a standard one or the single user mode) and
  `vid_config_glx/gly`, a maximized window keeps its normal size's mode.
- Per-monitor DPI aware (`hexenlicht.manifest`): sizes are physical pixels.
- 2D coordinates are `vid.width x vid.height` with an integer UI scale
  (`vid_uiscale`, 0 = auto: the largest scale keeping >= 640x480;
  `VID_GetUIScale()`).
- `VID_InitPalette` builds `d_8to24table`, `d_8to24TranslucentTable` and
  GL's colorshade tints `RTint/GTint/BTint`.

## Vulkan core and swapchain

- `vk_local.h`: the global `vk_state_t vk`, `VK_CHECK`, all module
  prototypes and shared C types.
- `vk_core.c`: instance (validation layer in Debug or with `-validation`;
  messages counted and printed; the log ends with `Vulkan validation: N
  errors, M warnings`), Win32 surface, device (requires Vulkan 1.3,
  acceleration structures, ray query, buffer device address, descriptor
  indexing, dynamic rendering, synchronization2, storage image extended
  formats; enables RT pipeline, NV SER, position fetch when present), VMA,
  `vk_info`.
- **Module table** (Q2RTX's `vkpt_initialize_all`): modules are initialized
  in table order and shut down in reverse. `VK_INIT_DEFAULT` entries run at
  startup, `VK_INIT_SWAPCHAIN` entries again after a swapchain recreation
  (`VK_SwapchainRecreated`, called by `vk_swapchain.c`: the render targets),
  `VK_INIT_RELOAD_SHADER` entries on `vk_reload_shaders` (pipelines; lazily
  created ones are only destroyed). New modules join the table.
- `vk_swapchain.c`: swapchain (UNORM with sRGB color space, so the final
  pass encodes with `linear_to_srgb()` from `srgb.glsl`; recreated lazily
  when `vk.swapchain_dirty`: `WM_SIZE`, `vid_vsync`, out-of-date), two frames
  in flight, per-image present semaphores, `VK_BeginFrame`/`VK_EndFrame`,
  `VK_BeginSwapchainRendering(loadOp)`/`VK_EndSwapchainRendering()` between
  them. `screenshot` captures the next presented frame
  (`VK_RequestScreenshot`).

## Shaders

- GLSL sources in `engine/hexenlicht/shaders/`, stage from the extension,
  except that Quake II RTX's ray generation shaders (`.rgen`) are compiled as
  compute shaders with `-DKHR_RAY_QUERY` (ray queries only);
  `#include` needs `#extension GL_GOOGLE_include_directive : require`. List
  each new shader in `hexenlicht_add_shaders(hexenlicht_shaders ...)` in
  `CMakeLists.txt` (include files are not listed; glslang's depfile tracks
  them).
- They compile to `build/<preset>/bin/shaders/<file>.spv` (Vulkan 1.3 target,
  `-DVKPT_SHADER`, `-g` in Debug) and load at runtime with
  `VK_LoadShader("<file>")`. A shader change needs no relink: build the
  `hexenlicht_shaders` target and run `vk_reload_shaders` in the game.
- Shared C/GLSL layouts: see [Buffers and layouts](#buffers-and-gpu-data-layouts).
  A shader that uses the global UBO or the render targets defines
  `GLOBAL_UBO_DESC_SET_IDX 0` and `GLOBAL_TEXTURES_DESC_SET_IDX 1` before
  including any header, plus `VERTEX_BUFFER_DESC_SET_IDX` if it uses
  `vertex_buffer.h`'s functions (its value is unused); otherwise only the
  structs are declared. A fragment shader also defines
  `GLOBAL_TEXTURES_SAMPLED_ONLY`.

## Textures (`vk_texture.c`)

- `GL_LoadTexture` keeps HoT's interface, flags, name+CRC cache and 8-bit
  conversion (`gl_model.c` calls it). It returns a slot in one bindless array
  of combined image samplers (`vk.texture_set`/`vk.texture_set_layout`,
  binding 0, `VK_MAX_TEXTURES` = 4096 in `vk_local.h`, which must equal
  `constants.h`'s `NUM_GLOBAL_TEXTURES` — `global_textures.h` checks it in C
  files that include `vk_local.h` first; update-after-bind so pics can load
  mid-frame).
- Images are `R8G8B8A8_SRGB` with GPU-blitted mips. Slot 0 is white; freed
  slots point back to it. `D_FlushCaches` purges slots above `gl_texlevel` on
  a map change like upstream; `D_ClearOpenGLTextures` also clears the 2D pic
  cache (`Draw_ClearCachedPics`), like `gl_rmisc.c`.
- `TEX_SPECIAL_TRANS` alpha is stored as opacity (GL blends those inverted),
  so alpha means opacity in every texture.

## 2D (`vk_draw.c`)

- Port of `gl_draw.c`'s `Draw_*`: quads go into a per-frame host-visible
  vertex buffer, drawn with one indexed draw in `GL_EndRendering`. The screen
  layout is upstream's `gl_screen.c` (reused; it calls our
  `GL_BeginRendering`/`GL_Set2D`/`GL_EndRendering`).
- Quads are alpha-tested (GL_GREATER 0.632, no blend) or blended per quad;
  the shader works in sRGB-encoded space and applies the `gamma` cvar.
- **`SCR_UpdateScreen` re-enters via `Con_Printf`** — only the outermost level
  records a frame (`draw_depth`). Never print to the console between
  `VK_BeginFrame` and the end of `VK_EndFrame`'s state update; count problems
  and report them from console commands.

## Scene (`r_scene.c`)

- `R_RenderView` does the renderer-independent frame setup (`R_AnimateLight`,
  `r_origin`/`vpn`/`vright`/`vup` — the client's sound and effects read them —
  view leaf, `V_CalcBlend`) and fills the global `r_scene`: camera, entities
  (`cl_visedicts`, all static entities, the view model; not culled to the
  view), active dlights, light style values, the active particle list and the
  view blend. The 3D renderer reads only `r_scene`; `r_dumpscene` prints it.
- The order in `R_RenderView`: `R_SetupFrame` → `R_ViewModelLight` →
  `R_BuildScene` (fills `r_scene`) → `VK_UpdateInstances` →
  `VK_UpdateModelGeometry` → `VK_UpdateEffects` → `VK_BuildTLAS` →
  `VK_RenderView3D`.
- `R_ViewModelLight` (`r_light.c`): GL's `R_DrawViewModel` lighting with
  `gl_rlight.c`'s `R_LightPointColor` on the RGB light maps `gl_model.c`
  loads (at least 24, plus dynamic lights) for **`cl.light_level`**, which the client sends to the
  server with every move: the gamecode's player `light_level` decides how well
  monsters see the player and when the Assassin cloaks — renderer output the
  game depends on.

## Buffers and GPU data layouts

- `vk_buffer.c`: `VK_CreateBuffer`/`VK_DestroyBuffer` (VMA; `vk_buffer_t`
  with device address; `vk_memory_t`: device, upload, readback) and load-time
  uploads (`VK_BeginUpload`/`VK_EndUpload`: one-time submit + wait;
  `VK_UploadBuffer`).
- Layouts shared by C and GLSL are found through `shaders/hl_shared.h`, which
  includes Quake II RTX's `shader_structs.h`, `constants.h`, `global_ubo.h`
  and `vertex_buffer.h` and adds Hexen II's own (PVS header, alias models,
  effects, check records, `DEBUGVIEW_*`). C gets typedefs; `DeviceAddress` is
  `uint64_t` in C and `uvec2` in GLSL (`GL_EXT_buffer_reference_uvec2`).
- Q2RTX's 128-byte `VboPrimitive` per triangle (`vertex_buffer.h`); material
  IDs = kind | flags | light style | 12-bit index (`constants.h`); primitive
  buffers `VERTEX_BUFFER_WORLD`, `VERTEX_BUFFER_INSTANCED`,
  `VERTEX_BUFFER_FIRST_MODEL + k`.

## Materials (`vk_material.c`)

- Rebuilt per map, uploaded with `VK_UploadMaterials`. Layout
  (`MATERIAL_UINTS` 8): Q2RTX's 6 uints + Hexen II's alternate animation
  (`+a..+j`).
- `vertex_buffer.h`'s `get_material_info`/`animate_material`: frame =
  `int(cl.time*5)` (`global_ubo.anim_frame`); surfaces reference their
  animation's first frame.

## World (`vk_world.c`)

- `R_NewMap` calls `VK_LoadWorld`: the world and its submodels in one
  device-local buffer `[VboPrimitive x n][3 positions x n]`, grouped per model
  into opaque/transparent/sky ranges (`vk_world.models[]`).
- World triangles carry their vis leaf (`cluster` = leaf number - 1);
  triangles facing into solid (qbsp leftovers) are dropped. Winding is
  reversed for the ray tracer.
- `vk_world [materials]` prints statistics and checks the animation table
  against `R_TextureAnimation`.

## PVS (`vk_pvs.c`)

- `VK_LoadWorld` decompresses the world's vis into `vk_pvs.matrix` (a cluster
  is a vis leaf, leaf number - 1; rows padded to 32 bits), connects it across
  water/slime/translucent triangles whose sides don't see each other (Q2RTX's
  `connect_pvs`; not lava), makes it symmetric (Hexen II's vis is asymmetric)
  and uploads it (`PVS_HEADER_UINTS` header + rows).
- Shaders: `pvs_visible(from, to)` in `shaders/pvs.glsl` (-1 counts as
  visible). CPU: `VK_ClusterPVS`/`VK_PointCluster`. `vk_pvs` prints statistics
  and checks the shader query against the CPU (`pvs_check.comp`, a compute
  pipeline using buffer device addresses in push constants — the pattern of
  all check shaders).

## Instances (`vk_instance.c`)

`VK_UpdateInstances` turns the scene's entities with geometry into Q2RTX
`ModelInstance`s (`global_ubo.h`, 224 bytes, Hexen II's drawflags / light /
entity / colorshade / tint at the end), copied to a mapped buffer per frame in
flight (`VK_InstanceBuffer`). `vk_instances [step|box]` prints them.

- **Brush entities** (dynamic and static; models `*N` are world submodels —
  Hexen II has no standalone brush models): transform like GL's
  `R_DrawBrushModel` + `R_RotateForEntity` (translate, then yaw about z, pitch
  about y, roll about x); last frame's transform per entity (motion
  vectors); cluster at the transformed model center or a corner; the model's
  primitive range in the world buffer; alpha 0.33 for
  `DRF_TRANSLUCENT`; entity frame (alternate animations). romeric2 has
  rotating brushes and egypt5 a lift, when walking forward from the start.
- **Alias models** follow in three groups, Q2RTX's order (`MODEL_GROUP_*`,
  `VK_ModelFrame` has their ranges): opaque; transparent (`DRF_TRANSLUCENT`,
  `EF_TRANSPARENT`, `EF_SPECIAL_TRANS`; kind `MATERIAL_KIND_TRANSP_MODEL`,
  alpha = 0.33 for `DRF_TRANSLUCENT` times the texture's; blending is 6.4);
  masked (`EF_HOLEY` cutouts).
- **The first-person weapon** (`cl.viewent`, `SCENE_ENT_VIEWMODEL`) comes
  last, in `MODEL_GROUP_WEAPON` (Q2RTX's viewer weapon, triangles flagged
  `MATERIAL_FLAG_WEAPON`). It looks like the group it would otherwise be in
  (`vk_modelframe_t.weapon_look`: the ice staff and the Demoness's fourth
  weapon are cutouts, the meteor staff and a cloaked Assassin's weapons
  transparent), has its own history slot (a new weapon is a new model) and
  GL's fov compensation (above `fov 90`, model y/z × `tan(fov/2)`). Its
  triangles are reserved in the instanced buffer (`weapon_reserve`), so an
  overflow drops other models, never the weapon.
- An alias instance's `material` is the skin GL binds (`vk_skin.c`); `light`
  is GL's fixed light level (255 = 1: `MLS_ABSLIGHT`'s abslight, the other
  `MLS_*` modes from light styles 25–30, spinning items' pulse; -1 = lit by
  the world; brush entities only `MLS_ABSLIGHT`); `colorshade` and `tint` are
  GL's `RTint/GTint/BTint`.
- Alias transform, model space (the pose decode stays in the model table) to
  world: GL's `R_RotateForEntity2` (yaw, -pitch, -roll; `EF_ROTATE` spin with
  `cl.time`; `EF_FACE_VIEW` turned to the camera, pitch first) times
  `R_DrawAliasModel`'s `tmatrix` without the decode (entity `scale` percent,
  `SCALE_TYPE_*`, `SCALE_ORIGIN_*` incl. GL's one-grid-step quirk for
  XY/Z-only, `EF_ROTATE` bobbing). The pose is GL's `R_SetupAliasFrame`
  (frame groups by `cl.time / interval`, no syncbase).
- **Entity history** (dynamic entities by number, static by index, temporary
  entities by address in a small hash table; reset when the model changes or
  the entity wasn't in the last frame) holds last frame's transform and the
  animation.
- **`r_lerpmodels 1`** (default, archived): the instance blends the previous
  pose into the current one (Q2RTX's `prim_offset_*_pose_*_frame` as pose
  vertex offsets, `pose_lerp_*` = the previous pose's weight) over the
  interval between the entity's last two frame changes (Hexen II animates at
  20 or 10 Hz; 0.1 s after a pause > 0.2 s or when it appears), frame groups
  over their interval; 0 = GL's pose.
- **`r_lerpmove 1`** (default, archived; QuakeSpasm's
  `R_SetupEntityTransform`): stepping entities (`scene_entity_t.movestep`: the
  server marks `MOVETYPE_STEP` entities `U_NOLERP`, which `cl_parse.c` keeps
  in `entity_t.movestep` under `#if defined(HEXENLICHT)`, an upstream hot
  spot) glide from their last position and angles to the new ones over the
  interval between their last two moves (like the frame blending; QuakeSpasm
  assumes 0.1 s); alias models only; no glide (and no motion) over a
  >100-unit jump; a move that comes before the glide is over glides on from
  where the entity is shown (QuakeSpasm jumps to the old target first). The
  shown place trails the server by up to one move, as in QuakeSpasm; what the
  client attaches (dynamic lights, trails, beam sources, sounds) stays at the
  server's position. 0 = GL's steps. `vk_instances step` lists them with
  where they are and where they're shown.
- `vk_instances box` prints each alias instance's pose bounds in model space
  and in the world, and where the world box's center is in the view.

## Alias models (`vk_model.c`)

- `R_NewMap` calls `VK_LoadModels` after `VK_LoadWorld` (every alias model in
  `cl.model_precache`); others are built when first drawn
  (`VK_AliasModelIndex`, mid-frame upload; `vk_models` counts them).
- A model's buffer is `[AliasTriangle x n][trivertx_t x poses x pose verts]`,
  built from `gl_mesh.c`'s strips/fans (both MDL formats, seams included,
  winding reversed like the world's). The host-visible model table
  (`AliasModel`: decode scale/origin, counts, addresses) is indexed by
  `source_buffer_idx - VERTEX_BUFFER_FIRST_MODEL`.
- `VK_UpdateModelGeometry`: `model_geometry.comp` (Q2RTX's
  `instance_geometry.comp`, one workgroup per alias instance; normals from the
  162-entry table by inverse transpose, per-triangle tangents orthogonalized
  per vertex + handedness flag, motion `prev - curr` as half floats in
  `custom0-2`) writes VboPrimitives and packed positions into this frame's
  instanced buffer (`VERTEX_BUFFER_INSTANCED`, `MAX_INSTANCED_PRIMITIVES`, one
  per frame in flight; overflow is counted, never printed inside a frame).
- `vk_models [list|check]`; `check` compares every triangle of the last frame
  with the same computation on the CPU (`CpuTriangle` — keep it in step with
  the shader).

## Skins (`vk_skin.c`)

- `VK_SkinMaterial` is `R_DrawAliasModel`'s choice: skin >= 100 is
  `gfx/skin<n>.lmp` via `Draw_CachePic` (100 stone, 101 ice); else
  `gl_texturenum[skin][(int)(cl.time*10)&3]`, bad numbers fall back to 0 and
  are counted; players with translated colors use `player<n>` (found with
  `VK_FindTexture`) unless `gl_nocolors`.
- One material per skin texture and cutout use (`VK_AddSkinMaterials` for the
  precache on map load, others on demand); for `EF_HOLEY` models the skin is
  its own `mask_texture`.
- `R_TranslatePlayerSkin` is `gl_rmisc.c`'s per-class translation
  (`gfx/player.lmp`, `color_offsets`) but translates the 8-bit skin and loads
  it with the model's texture mode (so the Demoness keeps her cutouts, which
  GL loses) at native size with mips.

## Effects (`vk_effects.c`)

- `VK_UpdateEffects` writes the scene's particles and sprite entities as
  triangles on the CPU into a mapped buffer per frame in flight, laid out like
  Q2RTX's `transparency.c` (`hl_shared.h`): positions (3 per particle, then 4
  per sprite quad sharing a static uint16 index buffer 0 1 2, 2 3 0), then an
  `EffectParticle` (linear color, half alpha | GL's `ptex_coord` set) per
  particle and an `EffectSprite` (texture slot, alpha) per sprite;
  `MAX_EFFECT_PARTICLES`/`MAX_EFFECT_SPRITES`, the excess is counted.
- Particles as GL's `R_DrawParticles`: one camera-facing triangle, 1.5 units
  up/right scaled with distance, snow's `count/10` base and texture sets,
  color `& 0x1ff` from `d_8to24table` or `d_8to24TranslucentTable`, GL's 16x16
  dot `particletexture` (loaded in `VK_InitEffects`).
- Sprites as `R_DrawSpriteModel`/`R_GetSpriteFrame`: all five orientation
  types, frame groups with `syncbase`, alpha 0.33 for
  `DRF_TRANSLUCENT`/`EF_TRANSPARENT`, unlit; `SPR_FACING_UPRIGHT` uses the
  sprite's own direction to the camera (no game sprite has that type).
- `vk_effects [check]`: counts, drops, BLAS/TLAS sizes; `check` casts a ray at
  every effect triangle of the last frame through the effects TLAS
  (`effects_check.comp`) and compares where it is reported.

## Acceleration structures (`vk_accel.c`)

- `VK_LoadWorld` ends with `VK_BuildWorldAccel`: one static BLAS per non-empty
  range of the world and every submodel, from the packed positions, one batch.
- `VK_BuildTLAS` records into the frame's command buffer:
  - dynamic BLASes over the instanced buffer's model triangles, one per model
    group (fast build, rebuilt every frame, created with room to grow like
    Q2RTX's, per frame in flight); the masked group's TLAS instance is
    `FORCE_NO_OPAQUE` so its hits are candidates alpha-tested against the
    material's `mask_texture`, and so is the weapon's when it has cutouts
    (**a dynamic BLAS's geometry flags must not change between size query and
    builds**, hence the weapon's geometry is always non-opaque and its
    instance flags decide); the weapon's mask is `AS_FLAG_VIEWER_WEAPON`;
  - BLASes over the effects (particles non-indexed, sprites indexed;
    `NO_DUPLICATE_ANY_HIT` so each is blended once);
  - in one call, the TLAS (world BLASes + a submodel's BLASes per brush
    instance + the dynamic BLASes with an identity transform and custom index
    `VERTEX_BUFFER_INSTANCED`; Q2RTX's `AS_FLAG_*` masks; force-opaque except
    cutouts) and Q2RTX's second, effects-only TLAS (`VK_EffectsTLASAddress`,
    0 = no effects; mask `AS_FLAG_EFFECTS` in its own namespace,
    `FORCE_NO_OPAQUE` + cull disable, custom index
    `EFFECTS_PARTICLES`/`EFFECTS_SPRITES`; effects never block rays through the
    main TLAS).
- TLAS instances carry Q2RTX's shader binding table offsets (`SBTO_OPAQUE`,
  `SBTO_MASKED`, `SBTO_PARTICLE`, `SBTO_SPRITE`), which its ray-query loops
  pick the hit logic by, and each has a `TlasInstanceInfo` (first primitive,
  model instance; -1 for the world and for model triangles, whose
  `VboPrimitive.instance` names it). One TLAS per frame in flight
  (`VK_TLASAddress`), GPU timestamps.
- AS size queries use the capacity's `maxVertex`; `gl_RayFlagsOpaqueEXT`
  overrides instance flags.
- Checks: `vk_accel` (sizes, build times); `vk_rayprobe x y z` (one ray from
  the camera towards a point through the last TLAS, every hit a candidate so
  hits behind the first are listed too: the first 32 by distance with what
  they are — world range, brush entity, model instance and triangle;
  `ray_probe.comp`); `vk_rtcheck` (a 64x48 ray grid from the camera compared
  with CPU hull traces — world hull 0 + each brush entity's hull, rotated like
  `SV_ClipMoveToEntity` — allowing for the hull's DIST_EPSILON, re-tracing
  shifted rays at edges, skipping rays through water/lava/sky; model
  triangles pass through, they have no hulls; `rt_check.comp`).

## Path tracer framework

Stories 3.1 and 3.2; the import rules and the Q2RTX module map are in
[Q2RTX.md](Q2RTX.md). Ray queries only; Q2RTX's shader names over our
bindings.

- **Shader headers** from Q2RTX: `constants.h` (blue noise
  `BLUE_NOISE_RES 64`, `NUM_BLUE_NOISE_TEX 256`), `shader_structs.h`,
  `utils.glsl`, `projection.glsl`, `path_tracer_transparency.glsl`,
  `brdf.glsl`, `water.glsl`, `asvgf.glsl` (the last six unchanged); adapted:
  `global_ubo.h` (Q2RTX's `GLOBAL_UBO_VAR_LIST`
  whole, plus a Hexenlicht block before `UBO_CVAR_LIST`: the frame's buffers
  by device address — TLAS, effects TLAS, TLAS info, instances, world and
  instanced primitives, materials, PVS, particles, sprites — the particle
  texture slot, `anim_frame`, `debug_view`, `view_cluster`; our
  `ModelInstance`; `TlasInstanceInfo` instead of Q2RTX's `InstanceBuffer`;
  `instance_buffer.model_instances[]` and `tlas_instance_info[]` are
  buffer-reference macros), `global_textures.h` (render-target lists; set 1:
  storage images at `BINDING_OFFSET_IMAGES + n`, sampled `TEX_*` at
  `BINDING_OFFSET_TEXTURES + n`, `TEX_BLUE_NOISE` last; the bindless
  `global_texture*()` array in set 2; `PT_VIEW_DEPTH` declared `r16f`, its
  format, where Q2RTX's `r32f` is a validation warning), `vertex_buffer.h` (`VboPrimitive`, `MATERIAL_UINTS`,
  `get_primitive`/`load_triangle`/`load_and_transform_triangle`/`get_material_info`/`animate_material`
  over the UBO's addresses; world and brush triangles animate with
  `anim_frame`, brush entities with a frame show alternates),
  `path_tracer.h` (UBO in set 0), `path_tracer_hit_shaders.h`
  (`pt_logic_rchit`, `pt_logic_masked` testing alpha, `pt_logic_particle` and
  `pt_logic_sprite` with GL's look; beams and explosions come with 6.3),
  `path_tracer_rgen.h` (3.2: the passes' common code — `trace_geometry_ray`,
  `trace_effects_ray`, `get_material`, `get_rng`, `env_map` — with the TLASes
  by device address; `env_map` is black until 4.6; `get_material` tints a
  model's base color with its `colorshade` hue; the lighting functions come
  with 3.3, the gradient samples with 3.6).
- **Random numbers:** Q2RTX's `get_rng` over blue noise: Christoph Peters'
  CC0 textures (`libs/bluenoise`, 64 of 64x64, 16-bit RGBA; copied next to
  the exe as `blue_noise\`), each channel one layer of a 256-layer
  `R16_UNORM` array that `vk_images.c` loads at startup; per pixel and frame
  the seed image `ASVGF_RNG_SEED_A` (x, y, field, frame) picks the texel and
  the first layer, dimension `n` adds `n` layers.
- **Descriptor sets** of every view pass: 0 = global UBO, 1 = render targets
  (even/odd), 2 = bindless textures.
- `vk_ubo.c` (Q2RTX's `uniform_buffer.c` + `prepare_ubo`): one UBO per frame in
  flight; `VK_PrepareUBO` fills V/invV/P/invP and their `_prev`, sizes, time,
  medium, the Hexenlicht block and `UBO_CVAR_LIST`'s cvars (registered with
  Q2RTX's defaults, inert until their pass). `vk_render_frame` counts 3D
  frames (= `current_frame_idx`, picks the even/odd image set). Three offset
  asserts guard the C struct's layout; after changing the list, compare every
  member (`tools/hexenlicht/ubo_layout_check.ps1`).
- `vk_images.c`: `VK_CreateImages` makes the render targets at the swapchain's size,
  the width rounded up to even (recreated with it), GENERAL layout, cleared
  to 0 when created, even/odd sets swapping `LIST_IMAGES_A_B`; the 3D view
  renders into their top left `width x height`. Images come with their
  passes: `TAA_OUTPUT` (3.1) and the G-buffer (3.2, see
  [3D view](#3d-view-vk_viewc)): 140 bytes per pixel by their formats;
  `vk_images` reports 82 MB allocated at 960x540 (about 160 bytes per
  pixel, 1.3 GB at 3840x2160). It also loads the blue noise. `vk_images`
  lists them.
- `vk_matrix.c`: Q2RTX's `matrix.c` (view space x right, y up, z forward; clip
  y down); `vk_ubo.c` uses GL's near 4 / far 4096.
- `vk_pathtracer.c`: `VK_CreatePassLayout` (the three sets + push constants),
  `VK_PathTracerLayout` (Q2RTX's `pt_push_constants_t`),
  `VK_CreateComputePipeline`, `VK_BindPassSets`, `VK_DispatchRays` (Q2RTX's
  `dispatch_rays` in ray-query mode), `VK_RenderTargetBarrier`,
  `VK_ComputeBarrier` (between the compute passes, which share the images).

## 3D view (`vk_view.c`)

- `VK_RenderView3D` fills the UBO for the 3D view in pixels (`r_refdef.vrect`
  × UI scale, centered like the 2D; rendered at the width rounded up to even,
  the output `taa_output_width x taa_output_height` is the view's) and
  dispatches the view passes into `TAA_OUTPUT`, with `VK_ComputeBarrier`
  between them; `GL_EndRendering` calls `VK_DrawView3D` after beginning
  swapchain rendering: `fullscreen.vert` + `view_composite.frag` (reads
  `TEX_TAA_OUTPUT`, Q2RTX's final blit; sRGB encode + `gamma` like the 2D)
  into the 3D rectangle, then restores the full viewport for the 2D.
- **`primary_rays.rgen`** (3.2): Q2RTX's primary rays, dispatched as its
  are (width / 2 × height × 2 checkerboard fields: the left half of each
  image holds the pixels where x and y have the same parity, the right half
  the others), write the G-buffer:

  | Image | Contents |
  |---|---|
  | `PT_VISBUF_PRIM_A`, `PT_VISBUF_BARY_A` | instance (~0 = world) and primitive, barycentrics |
  | `PT_BASE_COLOR_A` | base color (textures, a model's tint hue), `.a` specular factor |
  | `PT_METALLIC_A` | metallic, roughness (0 and 1 until E5) |
  | `PT_NORMAL_A`, `PT_GEO_NORMAL_A` | shading and geometric normal (octahedral), facing the ray |
  | `PT_VIEW_DEPTH_A` | view depth (fp16; the sky 10000) |
  | `PT_MOTION` | `.xy` where the point was last frame minus where it is, in UV units, jitter-free; `.z` depth change, `.w` depth derivative |
  | `PT_CLUSTER_A` | vis cluster (a brush entity's triangles its instance's; 0xffff = none) |
  | `PT_SHADING_POSITION` | world position, `.w` material ID (light style bits replaced by the medium; 0 = no surface) |
  | `PT_VIEW_DIRECTION`, `PT_THROUGHPUT`, `PT_BOUNCE_THROUGHPUT` | ray direction and checkerboard flags, throughput and depth, ray cone for the lighting passes |
  | `PT_TRANSPARENT` | effects in front of the surface (premultiplied) and its emission |
  | `ASVGF_RNG_SEED_A` | the pixel's random number seed |

  The `_B` images are last frame's. Hexenlicht's changes: the weapon is
  traced first, from GL's near plane 4 units in front of the eye, and where
  it is hit it is the pixel's surface with no effects over it (GL's depth
  range hack); no back-face culling (GL draws `EF_SPECIAL_TRANS` models
  two-sided); threads past the fields' size return (the dispatch is rounded
  up to 8x8 groups); no readback, god rays or light-buffer PVS overlay; water keeps
  its geometric normal while there is no water normal map. The sky (and
  nothing) is an empty surface: black until 4.6 (`pt_show_sky 1` shows the
  sky polygons). Translucent surfaces (alpha < 1) split the fields as in
  Q2RTX: the even field stays on the surface, the odd one goes through it
  (their material kind), which the refraction pass (3.5) uses.
  Textures are sampled with Q2RTX's anisotropic ray-cone gradients; liquids
  warp as Q2RTX's `lava_uv_warp`, which is Hexen II's software renderer's
  turbulence (`d_scan.c`), with game time.
- **DLSS Ray Reconstruction's inputs** (PLAN §5) from the G-buffer: diffuse
  albedo = `get_reflectivity`'s albedo, specular albedo = Karis's
  environment-BRDF approximation of its reflectivity × the specular factor,
  normals `PT_NORMAL`, roughness `PT_METALLIC.g`, depth `PT_VIEW_DEPTH`,
  motion `PT_MOTION.xy` (× the size in pixels); the specular hit distance or
  motion vectors come with the reflections (3.5). RR needs the fields
  interleaved; at translucent surfaces they differ pixel by pixel (3.9).
- For now **`debug_view.comp`** shows the G-buffer (`r_debugview`, default 1
  until the path tracer shows a lit image, 3.3), reading each screen pixel
  from its field (`checkerboard_interleave.comp`'s mapping); it traces no
  rays: 1 base color with the effects over it, 2 shading normals, 3 material
  kinds (cutouts yellow, the weapon cyan; translucent surfaces alternate
  between regular and their kind), 4 instances, 5 clusters with the camera's
  PVS, 6 motion vectors (gray still, hue = direction, brightness = length up
  to 16 pixels), 7 motion check (this frame's base color minus last frame's
  at the motion vector, ×4, bilinear: black where the vectors are right
  except texture detail, disocclusions and changing textures; dark blue
  where the point was off the screen), 8 geometric normals, 9 depth (log
  scale), 10 roughness/metallic/specular factor as R/G/B, 11 diffuse and 12
  specular albedo (as RR would get them), 13 effects and emission, 14 the
  pixel's first random number; 0 off.

## Other

- **Settings:** `hexenlicht.exe` saves to `hexenlicht.cfg` instead of
  `config.cfg` (`CONFIG_NAME` in `host.c`; `exec config.cfg` from `hexen.rc`
  is redirected in `cmd.c`), reading `config.cfg` until that file exists.
  Early-read cvars (`vid_*`, `vid_uiscale`) are locked until `hexen.rc` has
  run.
- **Upstream files Hexenlicht modifies** (all `#if defined(HEXENLICHT)`) are
  listed in [UPSTREAM.md](UPSTREAM.md#conflict-hot-spots); add to that list
  when a change touches another one.
- Code adapted from Q2RTX or QuakeSpasm keeps its copyright lines and is
  listed in `THIRD_PARTY.md`.

## Console commands

| Command | What |
|---|---|
| `r_debugview 0-14` | debug view mode: the G-buffer's channels (see [3D view](#3d-view-vk_viewc)) |
| `r_lerpmodels`, `r_lerpmove` | frame and movement blending (1) or GL's look (0) |
| `r_dumpscene` | the last frame's scene |
| `vk_info` | device, extensions, swapchain, validation counts |
| `vk_textures [list]` | texture slots |
| `vk_world [materials]` | world buffer statistics, animation check |
| `vk_pvs` | PVS statistics, shader check |
| `vk_instances [step\|box]` | model instances, gliding monsters, pose bounds |
| `vk_models [list\|check]` | alias models, GPU-vs-CPU triangle check |
| `vk_effects [check]` | particles/sprites, effects TLAS ray check |
| `vk_accel` | acceleration structure sizes, build times |
| `vk_rtcheck` | ray grid vs. CPU hull traces |
| `vk_rayprobe x y z` | hits of one ray towards a point |
| `vk_images` | render targets and the blue noise |
| `vk_reload_shaders` | rebuild pipelines from the SPIR-V on disk |
