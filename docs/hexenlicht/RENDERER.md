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
[Path tracer framework](#path-tracer-framework) · [Lights](#lights-vk_lightc) ·
[3D view](#3d-view-vk_viewc) · [Denoiser](#denoiser-vk_asvgfc) · [Other](#other) · [Console commands](#console-commands)

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
  the entity wasn't in the last frame) holds last frame's transform, the
  animation and the entity's instance index.
- **Instance map** (3.6, for the denoiser's gradient samples): after the
  `MAX_MODEL_INSTANCES` instances the instance buffer holds, for each of
  last frame's instances, its index this frame (~0 = gone; Q2RTX's
  `model_prev_to_current`, read as `instance_buffer.model_prev_to_current`):
  an entity continues when its history does (drawn last frame, same model;
  a teleport too). Temporary entities map as approximately as their
  history (beam segments are made anew every frame).
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
  `projection.glsl`, `path_tracer_transparency.glsl`, `brdf.glsl`,
  `water.glsl`, `asvgf.glsl` (the last five unchanged), `utils.glsl` (3.3:
  `packRGBE` clamps to what it can store); adapted:
  `global_ubo.h` (Q2RTX's `GLOBAL_UBO_VAR_LIST`
  whole, plus a Hexenlicht block before `UBO_CVAR_LIST`: the frame's buffers
  by device address — TLAS, effects TLAS, TLAS info, instances, world and
  instanced primitives, materials, PVS, particles, sprites, the light
  buffer and the three light statistics buffers (3.4) — the particle
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
  model's base color with its `colorshade` hue; 3.3: shadow and caustic rays
  and `get_direct_illumination`, since 3.4 with the light statistics per
  list entry; no sunlight until 4.6; 3.6: Q2RTX's `get_is_gradient`, the
  denoiser's gradient samples),
  `light_lists.h` (3.3: Q2RTX's polygon and sphere light sampling; 3.4:
  spheres in the light lists, the statistics per list entry; a list's
  current light count instead of Q2RTX's light-count history, which only
  lists that change every frame need, no sky lights),
  `brdf.glsl` (GGX, `get_reflectivity`, `composite_color`).
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
  passes: `TAA_OUTPUT` (3.1), the G-buffer (3.2, see
  [3D view](#3d-view-vk_viewc)) and the lighting's (3.3: `PT_COLOR_LF_SH`,
  `PT_COLOR_LF_COCG`, `PT_COLOR_HF`, `PT_COLOR_SPEC`, `ASVGF_COLOR`,
  `FLAT_COLOR`, `FLAT_MOTION`; 3.5a: `PT_VIEW_DIRECTION2`,
  `PT_GEO_NORMAL2` and our `PT_SPECULAR_HIT_DIST`) and the denoiser's
  (3.6: Q2RTX's 25 `ASVGF_*` images, some at 1/3 resolution): about 287
  bytes per pixel by their formats, 89 of them the denoiser's; 1078 MB
  allocated at 2560x1440. New images mean no denoiser history
  (`VK_ResetDenoiserHistory`) and no last frame in the UBO
  (`VK_ResetUBOHistory`: the `_prev` sizes would point past smaller
  images). It also loads the blue noise. `vk_images` lists them with their
  allocated sizes.
- `vk_matrix.c`: Q2RTX's `matrix.c` (view space x right, y up, z forward; clip
  y down); `vk_ubo.c` uses GL's near 4 / far 4096.
- `vk_pathtracer.c`: `VK_CreatePassLayout` (the three sets + push constants),
  `VK_PathTracerLayout` (Q2RTX's `pt_push_constants_t`),
  `VK_CreateComputePipeline` (`VK_CreateComputePipelineSpec` with the
  shader's specialization constant 0, as Q2RTX's bounce pipelines),
  `VK_BindPassSets`, `VK_DispatchRays` (Q2RTX's
  `dispatch_rays` in ray-query mode), `VK_DispatchCompute` (Q2RTX's 16x16
  compute passes), `VK_RenderTargetBarrier`, `VK_ComputeBarrier` (between
  the compute passes, which share the images).

## Lights (`vk_light.c`)

Stories 3.3 and 3.4; Q2RTX's two kinds of lights, sampled in
`light_lists.h`:

- **The light buffer's lights** (`LightBuffer` in `shaders/vertex_buffer.h`:
  Q2RTX's without its material table, light styles, cluster debug mask and
  sky visibility; one host-visible buffer per frame in flight,
  `global_ubo.lights`; the shaders' `light_buffer`), `LIGHT_POLY_VEC4S`
  vec4s each, of two types (`LIGHT_TYPE_*` in the fourth vec4's z):
  - **polygons** (Q2RTX's light polygons): the corners with the color in
    w, then the style scales; emission is one-sided, along
    `cross(p1 - p0, p2 - p0)`;
  - **spheres** (3.4, for Hexen II's point lights; Q2RTX's lists hold only
    polygons): center, radius, an optional **range** (0 = unlimited) and
    the color. With a range the light fades to 0 there:
    `saturate(1 - (d / range)^4)^2` times the inverse square
    (`sphere_light_window`), so culling by the range never shows as a seam.
    A sphere's weight in the light CDF is its solid angle, as a triangle's
    (`sphere_light_mass`); it contributes its radiance times its solid
    angle; the shadow ray goes to a point on it (Q2RTX's
    `compute_dynlight_sphere`); on bounces its solid angle has Q2RTX's
    sphere-light limit.

  A pixel samples the light list of its cluster (`PT_CLUSTER`): Q2RTX's up
  to `MAX_BRUTEFORCE_SAMPLING` (8) candidates, in `ceil(n / 8)` interleaved
  partitions of which each sample weighs one, weighted by solid angle,
  luminance and the light statistics; clusters past `MAX_LIGHT_LISTS - 1`
  get none.
- **The light lists** (3.4, Q2RTX's `collect_cluster_lights`), built on the
  CPU when the lights change (`VK_UpdateLights`: test light commands, map
  load; 1–5 ms): a light goes into the list of every cluster in the PVS of
  the open leafs its emitter touches (a BSP walk with the sphere's box, or
  the polygon's box reaching one unit in front of it; Q2RTX takes the
  light's one cluster), except clusters entirely behind a polygon (Q2RTX's
  `light_affects_cluster`) and clusters beyond a sphere's range. A
  cluster's bounds (`VK_LoadLightClusters`, after the PVS is final) are its
  leaf's and those of its world triangles (Q2RTX: its opaque triangles');
  models and brush entities take the cluster of their center, so their
  parts beyond a cluster's bounds can miss a light near the end of its
  range, where the window has taken it almost to 0. Lights touching no
  open leaf (inside solid) are in no list; a light that doesn't fit into
  `MAX_LIGHT_LIST_NODES` is left out whole (both counted by `vk_lights`).
  Each frame in flight's buffer copies the lists when their version
  changed; the lights are written every frame (light styles, 4.2). Hexen
  II's light entities as test spheres (below), range = their `light` value:
  1500–18000 list entries, mean 8–22 per cluster, the longest 249
  (romeric6: 317 lights in 70 clusters); by the PVS alone mean 27–305, up to
  73000 entries (keep2). 3.3's every light in every list would not fit on
  keep2, keep5 or tibet1 (clusters × lights > 524288).
- **Light statistics** (3.4, Q2RTX's, after G. Ward's "Adaptive Shadow
  Testing for Ray Tracing"): `get_direct_illumination` counts unshadowed
  and shadowed rays (of the primary surfaces and, as in Q2RTX, of the first
  bounce's hits, 3.5a) per light list entry and primary direction of the
  normal (`LIGHT_STATS_UINTS` = 12 per entry; Q2RTX counts per cluster and
  light, ~50 MB per buffer on the largest Hexen II maps); the next frame's
  CDF weighs each light by its unshadowed share (at least 0.1;
  `pt_light_stats`). Three device-local buffers take turns per 3D frame
  (`global_ubo.light_stats` counted this frame, `light_stats_prev` read,
  `light_stats_prev2` for the denoiser's gradient samples, which replay
  last frame's light choice), sized to
  the lists (grown after `vkDeviceWaitIdle`); `VK_ClearLightStats` fills
  this frame's before the passes, and all three after the lists changed
  (their entries moved). The UBO's sphere lights have none, as in Q2RTX.
- **Dynamic sphere lights:** up to `MAX_LIGHT_SOURCES` (32) in the UBO's
  `dyn_light_data`, one picked at random per pixel (Q2RTX's dynamic lights,
  no culling), for lights that move (4.4); spot lights come with the code,
  unused.
- Per pixel, `get_direct_illumination` picks a list light or a dynamic
  sphere sample by their estimated contributions and traces one shadow ray (opaque
  geometry; cutouts alpha-tested; translucent surfaces and effects don't
  shadow; no shadow ray without a light). The weapon only shadows itself
  (`direct_lighting.rgen`). Direct specular only where the roughness is
  above `pt_direct_roughness_threshold` (0.18): smoother surfaces get it
  from the reflections (3.5), so mode 16 is black on them.
- Units are Q2RTX's shaders': inverse-square falloff; a list sphere's
  color is its radiance, a dynamic sphere's π × its radiance (the sampling
  gives solid angle / π, the diffuse BRDF divides by π again); a polygon's
  color is its radiance with Q2RTX's sqrt(cos) emission lobe. Q2RTX's
  `add_dlights` divides a dlight's intensity by 25; a test sphere's
  intensity is π × its radiance in both kinds (the same command looks the
  same as a list or a dynamic sphere). Calibrating to Hexen II's linear
  falloff is 4.9's. The lighting is stored RGBE-packed ×32
  (`STORAGE_SCALE_HF/SPEC`), which holds values up to 4088 / 32 ≈ 128:
  `packRGBE` clamps there (Q2RTX's wraps darker above it).
- For now the lights are **test lights** (`VK_LoadWorld` clears them,
  `VK_ClearLights`; colors below 0 become 0):
  - `vk_testlight sphere [radius] [intensity] [r g b] [range]`: a sphere
    light in the lists at the eye (8, 1000, white, 0 = unlimited);
  - `vk_testlight dlight [radius] [intensity] [r g b]`: a dynamic sphere
    light at the eye;
  - `vk_testlight quad [size] [intensity] [r g b]`: a square polygon light
    at the eye facing the view direction (32, 50, white; two triangles);
  - `vk_testlight entities [intensity] [range scale]` replaces the test
    lights with a white sphere (radius 8) at each light entity (classname
    `light*`), its range the entity's `light` value (utils/light's hard
    range; default 300) × the scale (1000, 1; 0 = unlimited); the real
    lights come with 4.1;
  - `vk_testlight list`, `vk_testlight clear`.

  `VK_PrepareLights` (from `VK_PrepareUBO`) writes the buffer and the UBO
  fields every 3D frame.
- `vk_lights` prints the lights, the lists (entries, mean and longest,
  empty ones), lights inside solid or left out, the build time, the
  statistics buffers' sizes and the camera cluster's list; `vk_lights stats`
  reads back the shadow rays the last frame counted; `vk_lights cull 0|1`
  turns range culling off and on (a check: the image must stay the same,
  only noisier). `r_debugview 17` shows each pixel's list length: black
  none, blue to green up to 8 (every sample weighs them all), yellow to red
  at 64 and more.

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
  | `PT_VISBUF_PRIM_A`, `PT_VISBUF_BARY_A` | instance (~0 = world) and primitive, barycentrics (after 3.5b's passes: of the last surface hit) |
  | `PT_BASE_COLOR_A` | base color (textures, a model's tint hue), `.a` specular factor |
  | `PT_METALLIC_A` | metallic, roughness (0 and 1 until E5) |
  | `PT_NORMAL_A`, `PT_GEO_NORMAL_A` | shading and geometric normal (octahedral), facing the ray |
  | `PT_VIEW_DEPTH_A` | view depth (fp16; the sky 10000; negative behind a reflection or refraction, 3.5b: −10000 for the sky there) |
  | `PT_MOTION` | `.xy` where the point was last frame minus where it is, in UV units, jitter-free; `.z` depth change (negated behind a reflection or refraction, 3.5b), `.w` depth derivative |
  | `PT_CLUSTER_A` | vis cluster (a brush entity's triangles its instance's; 0xffff = none) |
  | `PT_SHADING_POSITION` | world position, `.w` material ID (light style bits replaced by the medium; 0 = no surface) |
  | `PT_VIEW_DIRECTION`, `PT_THROUGHPUT`, `PT_BOUNCE_THROUGHPUT` | ray direction and checkerboard flags, throughput (`.a` the optical path length) and depth, ray cone for the lighting passes |
  | `PT_TRANSPARENT` | effects in front of the surface (premultiplied) and its emission |
  | `ASVGF_RNG_SEED_A` | the pixel's random number seed |

  The `_B` images are last frame's. Hexenlicht's changes: the weapon is
  traced first, from GL's near plane 4 units in front of the eye, and where
  it is hit it is the pixel's surface with no effects over it (GL's depth
  range hack); no back-face culling (GL draws `EF_SPECIAL_TRANS` models
  two-sided); threads past the fields' size return (the dispatch is rounded
  up to 8x8 groups); no readback, god rays or light-buffer PVS overlay; water keeps
  its geometric normal while there is no water normal map; vertical water
  and slime stay water (3.5b: Q2RTX makes them glass for its force fields;
  Hexen II's vertical turbulent surfaces are walls). The sky (and
  nothing) is an empty surface: black until 4.6 (`pt_show_sky 1` shows the
  sky polygons). Translucent surfaces (alpha < 1) split the fields as in
  Q2RTX: the even field stays on the surface, the odd one goes through it
  (their material kind), which `reflect_refract.rgen` follows (3.5b, below).
  Textures are sampled with Q2RTX's anisotropic ray-cone gradients; liquids
  warp as Q2RTX's `lava_uv_warp`, which is Hexen II's software renderer's
  turbulence (`d_scan.c`), with game time.
- **Reflections and refractions** (3.5b): `reflect_refract.rgen`, Q2RTX's,
  dispatched after the primary rays `pt_reflect_refract` times (Q2RTX's
  default 2; `VK_ReflectRefractPasses`: at least 0), the first pass and the
  others two pipelines of one shader (specialization constant 0; the push
  constant's bounce is the pass). Only pixels on a mirror, glass or
  translucent surface do work: each pass follows the path one surface
  further and puts that surface into the G-buffer (with its apparent
  position's motion vector and a negative depth), so the lighting passes
  light what is seen through or in it; effects along the way are blended
  over it (particles and sprites behind a translucent surface show).
  - Translucent surfaces and models (alpha < 1: Hexen II's `*rtex078` and
    `*lowlight` at 0.33, 2.1; translucent entities, 2.4b): the odd field
    continues through them (Q2RTX's slight distortion needs a normal map:
    none until E5). The next pass continues through a translucent layer
    behind it only on the same instance (Q2RTX's rule against showing a
    model's inside; all world triangles are one instance, so a world
    layer behind a world layer is passed through), else the path ends on
    that layer. The rays cull back faces as in Q2RTX, which skips the
    inside faces of turbulent volumes; the primary rays don't (R11), so a
    two-sided `EF_SPECIAL_TRANS` model seen from behind through a
    translucent surface loses its back faces (6.4). A ray from inside a
    liquid leaves it through a translucent turbulent surface (they bound
    liquid volumes).
  - Mirrors and glass (Q2RTX's `chrome` and `glass` kinds) come with the
    code for E5's materials; screens and security cameras are Quake II's.
  - Water and slime stay opaque and textured as GL draws them (only
    `SURF_TRANSLUCENT` surfaces and translucent entities blend there):
    they are skipped, and vertical ones stay water; Q2RTX's physical water
    (Fresnel reflection and refraction, extinction, a waves normal map) is
    in the shader for 6.5.

  Hexenlicht's changes: the launch check; water and slime skipped, no
  vertical water as glass; the weapon is in no reflection or refraction
  ray (as R20, R27), and the ray through a translucent weapon starts at the
  eye from GL's near plane (the weapon can reach into a wall); the
  translucent group is in every pass's rays, the last too (it also holds
  water, slime and alpha-1 models; Q2RTX leaves it out of the last); the
  liquid is left through translucent turbulent surfaces; the water normal
  only with a water normal map; no god rays; at most 10 passes, as Q2RTX.
  Measured
  (3.5b, the cathedral's holy water font filling much of the view,
  2560x1440, Release): 0.30 ms for one pass, 0.35 ms for two.
- **The lighting passes** (3.3), after the primary rays:
  `direct_lighting.rgen` (the same fields; one light sample and shadow ray
  per pixel, see [Lights](#lights-vk_lightc); demodulated diffuse into
  `PT_COLOR_HF`, specular into `PT_COLOR_SPEC`, RGBE-packed; it clears `LF`
  and `PT_SPECULAR_HIT_DIST`), `indirect_lighting.rgen` (3.5a, see below),
  then the denoiser (3.6, `flt_enable 1`, see
  [Denoiser](#denoiser-vk_asvgfc)), whose last filter composites, or
  `compositing.comp` (Q2RTX's path without the denoiser, `flt_enable 0`:
  lighting × albedo + specular, × throughput, the effects and emission over
  it, into `ASVGF_COLOR`), then `checkerboard_interleave.comp` (the fields
  into the screen layout: `FLAT_COLOR`, `FLAT_MOTION`; with the denoiser it
  blurs checkerboarded surfaces, so translucent surfaces show their blend
  instead of a fine checkerboard of the surface and what is behind it).
  Unchanged from Q2RTX apart from `direct_lighting.rgen`'s launch check,
  weapon shadows, no sunlight and the hit-distance clear. Without the
  denoiser the lit image is noisy at one sample per pixel; there is no
  exposure or tone curve until 3.7 (the composite clamps).
- **Bounces** (3.5a): `indirect_lighting.rgen`, Q2RTX's, as two pipelines
  of one shader (specialization constant 0: the first and the second
  bounce), dispatched after direct lighting by `pt_num_bounce_rays`
  (Q2RTX's default 1; `VK_NumBounceRays` takes it as Q2RTX does: 0.5, else
  0–2 rounded):
  - 1: one bounce ray per pixel, specular (GGX VNDF sampling) with
    probability 0.5 (1 for metals), else diffuse (cosine); at its hit the
    base color and one light sample through the hit's light list
    (`get_direct_illumination`, bounce 1: spheres with Q2RTX's solid-angle
    limit), plus the hit's emission. Diffuse into `PT_COLOR_LF_SH` and
    `PT_COLOR_LF_COCG` (spherical harmonics with the denoiser, whose diffuse
    rays sample the hemisphere of the geometric normal more evenly; plain
    color without), specular into `PT_COLOR_SPEC` (demodulated). Specular
    rays of surfaces rougher than `pt_fake_roughness_threshold` (0.2, fading
    out by 0.3) count for nothing: the denoiser makes their indirect
    specular from the spherical harmonics (Q2RTX's; without the denoiser
    they have none).
  - 2: the second bounce continues from the first one's hit (which
    overwrites `PT_SHADING_POSITION` and `PT_BOUNCE_THROUGHPUT`, with
    `PT_VIEW_DIRECTION2` and `PT_GEO_NORMAL2`); as in Q2RTX it gathers only
    emission and the sky, no light samples, so it adds nothing until
    emissive surfaces (4.5) and the sky (4.6).
  - 0.5: the first bounce for every other row, alternating per frame, ×2.

  Hexenlicht's changes: the launch check; the weapon is only in its own
  surfaces' bounce and shadow rays (as R20; Q2RTX's is in every ray without
  a first-person model); a model hit by a bounce ray has its `colorshade`
  hue (as `get_material`); the first bounce stores the specular ray's hit
  distance in `PT_SPECULAR_HIT_DIST` (r16f; 0 without a specular ray: a
  diffuse bounce, no surface, lava, the rows 0.5 skips, no bounces); at 0.5
  the rows are (h + 1) / 2, so an odd height's last row is traced too
  (Q2RTX: h / 2); no sunlight (4.6). Sphere lights are not geometry: bounce rays
  never hit them, so surfaces smoother than `pt_direct_roughness_threshold`
  (0.18), whose specular comes only from the specular bounce, reflect lit
  surfaces but show no highlight of a sphere (Q2RTX's dynamic lights
  alike; Q2RTX.md open questions). Measured (3.5a, test entity lights,
  2560x1440, Release): the pass takes 0.7 ms at 0.5, 1.1–1.3 ms at 1,
  1.7–1.9 ms at 2. Hexen II's textures are dark in linear light (the
  cathedral's mean diffuse albedo 0.04), so one bounce adds 2–3 % to the
  lit image there.
- **DLSS Ray Reconstruction's inputs** (PLAN §5) from the G-buffer: diffuse
  albedo = `get_reflectivity`'s albedo, specular albedo = Karis's
  environment-BRDF approximation of its reflectivity × the specular factor,
  normals `PT_NORMAL`, roughness `PT_METALLIC.g`, depth `PT_VIEW_DEPTH`,
  motion `PT_MOTION.xy` (× the size in pixels), the specular hit distance
  `PT_SPECULAR_HIT_DIST` where the pixel traced a specular bounce (3.5a;
  whether RR needs one in every pixel is the 3.9 spike's). RR needs the
  fields interleaved; at translucent surfaces they differ pixel by pixel
  (3.9).
- For now **`debug_view.comp`** writes `TAA_OUTPUT` (`r_debugview`, default
  1 until the maps have lights, 4.1): 0 the lit image (`FLAT_COLOR` /
  `STORAGE_SCALE_HDR`, denoised with `flt_enable 1`, until TAA and tone
  mapping take over, 3.7–3.8), or
  the G-buffer and lighting channels, reading each screen pixel from its
  field (`checkerboard_interleave.comp`'s mapping); it traces no rays: 1
  base color with the effects over it, 2 shading normals, 3 material
  kinds (cutouts yellow, the weapon cyan; with `pt_reflect_refract 0`
  translucent surfaces alternate between regular and their kind, else the
  odd field shows what is behind them: the debug view reads the G-buffer
  after the reflection and refraction passes), 4 instances, 5 clusters with the camera's
  PVS, 6 motion vectors (gray still, hue = direction, brightness = length up
  to 16 pixels), 7 motion check (this frame's base color minus last frame's
  at the motion vector, ×4, bilinear: black where the vectors are right
  except texture detail, disocclusions, changing textures and translucent
  surfaces, whose two fields differ since 3.5b; dark blue
  where the point was off the screen), 8 geometric normals, 9 depth (log
  scale, its absolute value: negative behind reflections and refractions), 10 roughness/metallic/specular factor as R/G/B, 11 diffuse and 12
  specular albedo (as RR would get them), 13 effects and emission, 14 the
  pixel's first random number, 15 direct diffuse (without the albedo),
  16 specular lighting (direct and bounced), 17 the length of the pixel's
  light list (see [Lights](#lights-vk_lightc)), 18 indirect diffuse
  (without the albedo; with the denoiser its spherical harmonics projected
  on the normal), 19 the specular hit distance (log scale as the
  depth, black without a specular ray) and 20 the denoiser's history
  length (red the direct diffuse's, green the indirect's, full at 32
  frames: yellow both, black none or no denoiser). 15, 16 and 18 show the
  lighting passes' output, before the denoiser (16 multiplied back by the
  base reflectivity, which the passes divide the specular by for the
  denoiser); with the denoiser up to one pixel in nine is a gradient
  sample, a replay of last frame's brightest sample of its 3x3 square, so
  their average reads brighter than with `flt_enable 0` (16: +15 % on
  demo1 with test lights; most likely this, not isolated). With the
  denoiser the G-buffer modes show the gradient
  samples' last-frame normal, base color, metallic and random seed in up
  to one pixel per 3x3 (the same values while paused, but mode 14's seed):
  compare G-buffers with `flt_enable 0`. `vk_view.c` runs it before
  the bounces for the G-buffer's modes (with two bounces the first stores
  its hit into the shading position) and after compositing for the
  lighting's (0, 15, 16, 18, 19, 20: `DEBUGVIEW_READS_LIGHTING`).

## Denoiser (`vk_asvgf.c`)

Story 3.6: Quake II RTX's A-SVGF (adaptive spatiotemporal variance-guided
filtering; `shaders/asvgf.glsl` explains it), `flt_enable 1` (Q2RTX's
default; 0 = the undenoised composite, as before). Its TAA pass
(`asvgf_taau.comp`) comes with 3.8. The shaders are Q2RTX's, unchanged but
`asvgf_temporal.comp` (below); they work in the G-buffer's two fields.

- **Gradient samples** (`VK_GradientReproject`, after the reflection and
  refraction passes, before direct lighting;
  `asvgf_gradient_reproject.comp`): in every 3x3 square the brightest pixel
  whose surface was seen last frame (motion vector, same cluster, depth
  within 10 %, geometric normals within ~25°; never last frame's gradient
  pixel) gets last frame's random number seed, normal, base color and
  metallic, and its position becomes that surface's position now, found
  through the visibility buffer and `model_prev_to_current` (the instance
  map, [Instances](#instances-vk_instancec)). The lighting passes shade it
  as last frame did (`get_is_gradient`: last frame's light style scale,
  the light statistics of two frames ago), with this frame's lights.
  Only with history: without, the last frame's visibility buffer and
  instance map may belong to another map or instance list, whose
  primitives the shader would read by device address unchecked (Q2RTX
  reprojects every frame); this frame's `ASVGF_GRAD_SMPL_POS_A` is cleared
  instead, and the unused gradients cost nothing (no history to drop).
- **The filters** (`VK_DenoiseLighting`, after the bounces, instead of
  `compositing.comp`): `asvgf_gradient_img.comp` makes the gradients (how
  much the lighting changed at each gradient sample, at 1/3 resolution),
  seven passes of `asvgf_gradient_atrous.comp` blur them;
  `asvgf_temporal.comp` blends each pixel's lighting (direct diffuse,
  indirect diffuse as spherical harmonics, specular) with its history at
  the motion vector where the surface matches, and drops history as far
  as the gradients show the lighting changed (anti-lag;
  `flt_antilag_*`, `flt_temporal_*`); four iterations of the spatial
  a-trous filter follow, the indirect diffuse at 1/3 resolution
  (`asvgf_lf.comp`, only with bounces) and the direct diffuse and specular
  at full resolution (`asvgf_atrous.comp`, specialization constant 0 =
  iteration), whose last iteration composites into `ASVGF_COLOR` (and adds
  rough surfaces' indirect specular from the spherical harmonics). All
  passes use the path tracer's layout; the gradient a-trous and LF passes
  read their iteration from the first push constant.
- **Hexenlicht's change** (`asvgf_temporal.comp`): a gradient sample
  blends into its pixel's history only as far as the anti-lag drops that
  history. It replays the brightest of its square's samples of last frame,
  which the history already holds; Q2RTX blends it in like a new sample,
  which brightened the image by as much as the lighting is noisy (demo1
  with test lights: +18 % at full intensity, +8 % at a sixteenth where the
  raw frames don't clip; up to 30 % in places). Left: +2 % (demo1) to +5 %
  (the cathedral), measured with direct lighting only against the average
  of 20 raw frames at a sixteenth of the intensity (Q2RTX.md open
  questions).
- **History** is the last 3D frame's images. `VK_EndDenoiserFrame` marks
  them valid after a frame with the denoiser; `VK_ResetDenoiserHistory`
  drops them on a new map (`R_NewMap`), with new images (`VK_CreateImages`),
  when `flt_enable` or `flt_temporal_*` change (`vk_ubo.c`), and when
  `VK_RenderView3D` skips the view or `VK_UpdateInstances` has no world
  (the entities' history moved on without the images). Without history `VK_PrepareUBO` sets `flt_temporal_*` to 0
  for the frame (Q2RTX's `temporal_frame_valid`).
- Not in Q2RTX's filters: the effects (particles, sprites) and emission,
  composited over the denoised lighting as before. The light-count history
  (Q2RTX's `light_counts_history`) is left out: our light lists change
  only with the lights, and a change costs one frame of gradients where it
  happened (4.4 needs it if moving lights join the lists).
  `prev_style_scale` is 1 until light styles (4.2).
- `flt_show_gradients 1` adds the gradients to the image (red indirect
  diffuse, green direct diffuse, blue specular); `r_debugview 20` shows
  the history length.
- Measured (2560x1440, Release, test entity lights, temporary GPU
  timestamps): gradient reprojection 0.47 ms, the filters 2.8 ms
  (`compositing.comp`: 0.3 ms); the 3D view 6.6 ms instead of 3.9 (demo1's
  start), 7.1 instead of 4.2 (the cathedral's font).

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
| `r_debugview 0-20` | 0 the lit image, 1-20 the G-buffer's, lighting and denoiser channels (see [3D view](#3d-view-vk_viewc)) |
| `flt_enable 0/1`, `flt_show_gradients 0/1` | the denoiser (Q2RTX's cvar, 1), its gradients over the image (see [Denoiser](#denoiser-vk_asvgfc)); Q2RTX's other `flt_*` cvars tune it |
| `pt_num_bounce_rays 0/0.5/1/2` | bounces (Q2RTX's cvar, 1); Q2RTX's other `pt_*` cvars, e.g. `pt_roughness_override`, `pt_metallic_override` (−1 = off) to test reflections |
| `pt_reflect_refract 0-10` | reflection and refraction passes (Q2RTX's cvar, 2) |
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
| `vk_testlight sphere, dlight, quad, entities, list, clear` | test lights (see [Lights](#lights-vk_lightc)) |
| `vk_lights`, `vk_lights stats`, `vk_lights cull 0/1` | light lists, light statistics read back, range culling off/on |
| `vk_reload_shaders` | rebuild pipelines from the SPIR-V on disk |
