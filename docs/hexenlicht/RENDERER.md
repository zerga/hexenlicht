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
[Map lights](#map-lights-vk_maplightsc) · [Map light colors](#map-light-colors-vk_lightcolorc) ·
[Emissive surfaces](#emissive-surfaces-vk_emissivec) ·
[3D view](#3d-view-vk_viewc) · [Denoiser](#denoiser-vk_asvgfc) ·
[Upscaling](#upscaling-vk_upscalec) · [DLSS](#dlss-vk_dlssc-vk_streamlinecpp) ·
[Bloom and tone mapping](#bloom-and-tone-mapping-vk_bloomc-vk_tonemapc) · [Profiler](#profiler-vk_profilerc) ·
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
  `stb_image` (`libs/stb`, PNG/TGA only, `STBI_NO_STDIO`); AMD's FSR 1
  headers (`libs/fsr1`, header-only: on the include path of `hexenlicht`
  and of the shaders, 3.8); NVIDIA Streamline's headers (`libs/streamline`,
  header-only, C++: `vk_streamline.cpp`, the target's one C++ file, 3.10;
  no NVIDIA binary is linked or in the repository). Versions and
  licenses are in `THIRD_PARTY.md`; update it when a vendored library changes.
- Codec DLLs from `oslibs/windows/codecs/x64` are copied next to the exe
  post-build.
- `engine/hexenlicht/stubs.c` provides the renderer symbols not implemented
  yet, sectioned by the story that replaces them; a story moves its section
  into real files. It still holds `R_InitTextures`/`r_notexture_mip` (GL's
  checkerboard), the rest of `R_Init`, `R_InitSky` (→ 4.6) and GL-named cvars
  kept so configs keep their settings (`gl_glows`, `gl_coloredlight`,
  `gl_lightmapfmt`, …); the client reads `gl_colored_dynamic_lights` (4.4:
  default 1 here, 0 in HoT) and `gl_extra_dynamic_lights` (0 as in HoT:
  the renderer makes those lights itself, see [Lights](#lights-vk_lightc))
  for the dynamic lights. To find what a renderer must
  provide, link without it and read the unresolved externals.

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
  `vk_info`. Before the instance, `VK_SLPreInit` (`vk_streamline.cpp`)
  loads NVIDIA Streamline when `sl.interposer.dll` is next to the exe and
  its signature verifies: volk then loads Vulkan through the interposer's
  `vkGetInstanceProcAddr` (else `vulkan-1.dll`), and the device enables
  Vulkan 1.3 `privateData` for it (see [DLSS](#dlss-vk_dlssc-vk_streamlinecpp)).
- **Module table** (Q2RTX's `vkpt_initialize_all`): modules are initialized
  in table order and shut down in reverse. `VK_INIT_DEFAULT` entries run at
  startup, `VK_INIT_SWAPCHAIN` entries again after a swapchain recreation
  (`VK_SwapchainRecreated`, called by `vk_swapchain.c`: the render targets),
  `VK_INIT_RELOAD_SHADER` entries on `vk_reload_shaders` (pipelines; lazily
  created ones are only destroyed). New modules join the table.
- `vk_swapchain.c`: swapchain (UNORM with sRGB color space, so the final
  pass encodes with `linear_to_srgb()` from `srgb.glsl`; recreated lazily
  when `vk.swapchain_dirty`: `WM_SIZE`, `vid_vsync`, out-of-date; the
  render targets also when DLSS's images change, `VK_DLSSBetweenFrames`),
  two frames in flight, per-image present semaphores, `VK_BeginFrame`/`VK_EndFrame`,
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
  `-DVKPT_SHADER`, `-g` in Debug, `libs/fsr1` on the include path) and load at runtime with
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
- The 8-bit pixels of alias model skins (`gl_model.c` names them
  `<model>_<skin>`) with a texel whose channel reaches
  `VK_EMISSIVE_THRESHOLD` (215) stay with their slot (4.5, freed with it):
  `VK_TextureRGBA` converts them again for the light models' emissive
  textures ([Emissive surfaces](#emissive-surfaces-vk_emissivec)).
  `vk_textures` prints how many (meso9: 100 skins, 2.6 MB).

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
  (`+a..+j`). Set: the base texture, the cutout mask, the animation and
  (4.5) the emissive texture and factor (Q2RTX's; the lava's and the light
  models' skins', see [Emissive surfaces](#emissive-surfaces-vk_emissivec));
  the other factors are Q2RTX's defaults.
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
- Lava (4.5): its materials emit, and a world lava triangle whose front
  leaf isn't lava is a light, flagged `MATERIAL_FLAG_LIGHT` (see
  [Emissive surfaces](#emissive-surfaces-vk_emissivec)).
- `vk_world [materials]` prints statistics (with the lava lights) and
  checks the animation table against `R_TextureAnimation`; `materials`
  lists each material with its emissive texture and factor.

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
- **Alias models** follow in four groups, Q2RTX's order and one of ours
  (`MODEL_GROUP_*`, `VK_ModelFrame` has their ranges): opaque; transparent
  (`DRF_TRANSLUCENT`, `EF_TRANSPARENT`, `EF_SPECIAL_TRANS`; kind
  `MATERIAL_KIND_TRANSP_MODEL`, alpha = 0.33 for `DRF_TRANSLUCENT` times
  the texture's; blending is 6.4); masked (`EF_HOLEY` cutouts); light
  (4.1: opaque models at a map light's origin, `VK_MapLightAt`: the
  torches, flames, candles and the like that the light entities' game code
  spawns there, whose mesh surrounds the light; they cast no shadows, see
  [Map lights](#map-lights-vk_maplightsc); since 4.5 they show their
  skin's emissive texture, flagged `MATERIAL_FLAG_LIGHT`, see
  [Emissive surfaces](#emissive-surfaces-vk_emissivec)).
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
  The triangles' emissive factor is the instance's `light` (GL's fixed
  light level, rounded to a half so the packing is exact; 1 when lit by
  the world): an emissive skin (4.5, the light models' flames) shows at
  GL's abslight.
- `vk_models [list|check]` (the geometry pass's GPU time from the profiler,
  the instances with an emissive skin);
  `check` compares every triangle of the last frame
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
  its own `mask_texture`. A third key (4.5): emissive, for models at a map
  light's origin, a material with the skin's emissive texture (made on first
  use, `VK_EmissiveSkin`), or the plain one where the skin has no bright
  texels (see [Emissive surfaces](#emissive-surfaces-vk_emissivec)).
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
- In the lit image both are scaled by the exposure so that they show at
  GL's colors (3.7, see [Bloom and tone mapping](#bloom-and-tone-mapping-vk_bloomc-vk_tonemapc)).
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
    instance flags decide); the weapon's mask is `AS_FLAG_VIEWER_WEAPON`,
    the light group's ours, `AS_FLAG_LIGHT_MODELS` (bit 6: in the primary,
    reflection and bounce rays' masks, not the shadow rays');
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
  (`VK_TLASAddress`); the builds are timed by the profiler (`dynamic
  BLASes`, `TLAS`; see [Profiler](#profiler-vk_profilerc)).
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
  buffer and the three light statistics buffers (3.4), the tone mapping
  and readback buffers (3.7) — the particle
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
  flight; `VK_PrepareUBO` fills V/invV/P/invP and their `_prev`, the sizes,
  jitter, TAA mode and FSR constants `vk_upscale.c` decided (3.8, see
  [Upscaling](#upscaling-vk_upscalec)), time,
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
  (3.6: Q2RTX's 25 `ASVGF_*` images, some at 1/3 resolution) and the
  bloom's (3.7: `BLOOM_HBLUR`, `BLOOM_VBLUR` at a quarter of the size,
  sampled linearly as `TAA_OUTPUT`) and the upscalers' (3.8: the TAA
  history `ASVGF_TAA_A/B`, sampled linearly, `FSR_EASU_OUTPUT`,
  `FSR_RCAS_OUTPUT`; `HQ_COLOR_INTERLEAVED`, which only Q2RTX's reference
  mode writes, 1x1) and DLSS's inputs (3.10: `DLSS_DEPTH` r32f while DLSS
  SR or RR is chosen, `DLSS_ALBEDO`, `DLSS_SPEC_ALBEDO`,
  `DLSS_NORMAL_ROUGHNESS` rgba16f and `DLSS_SPEC_HIT` r16f while RR is,
  else 1x1: `vk_dlss_images`, `IMG_WIDTH_DLSS`/`IMG_WIDTH_RR`; 4 or 30
  bytes per pixel): about 320 bytes per pixel by their
  formats, 89 of them the denoiser's; 1201 MB allocated at 2560x1440 (3.8: +120). New
  images mean no denoiser history
  (`VK_ResetDenoiserHistory`) and no last frame in the UBO
  (`VK_ResetUBOHistory`: the `_prev` sizes would point past smaller
  images). It also loads the blue noise. `vk_images` lists them with their
  allocated sizes.
- `vk_matrix.c`: Q2RTX's `matrix.c` (view space x right, y up, z forward; clip
  y down); `vk_ubo.c` uses GL's near 4 / far 4096.
- `vk_pathtracer.c`: `VK_CreatePassLayout` (the three sets + push constants),
  `VK_PathTracerLayout` (Q2RTX's `pt_push_constants_t`),
  `VK_CreateComputePipeline` (`VK_CreateComputePipelineSpec` with the
  shader's specialization constant 0, as Q2RTX's bounce pipelines;
  `VK_CreateComputePipelineSpecs` with constants 0 to 3, FSR's),
  `VK_BindPassSets`, `VK_DispatchRays` (Q2RTX's
  `dispatch_rays` in ray-query mode), `VK_DispatchCompute` (Q2RTX's 16x16
  compute passes), `VK_DispatchComputeLayout` (the same with a module's own
  layout and push constants, 3.7), `VK_RenderTargetBarrier`, `VK_ComputeBarrier` (between
  the compute passes, which share the images).

## Lights (`vk_light.c`)

Stories 3.3, 3.4, 4.1, 4.4 and 4.5; Q2RTX's two kinds of lights, sampled in
`light_lists.h`; the map's lights come from
[Map lights](#map-lights-vk_maplightsc), the lava's from
[Emissive surfaces](#emissive-surfaces-vk_emissivec):

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
    sphere-light limit. A **spotlight** (4.1) also has a direction and the
    cosine of half its cone's width (the second vec4's z, the third's
    xyz; direction 0 = none): `sphere_light_spot` takes its light to 0
    outside the cone, utils/light's hard edge softened over a degree to
    each side, in its CDF weight as in its light. The lists don't cull by
    the cone.

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
  changed; the lights are written every frame, with their light style (4.2,
  below). The map's lights (range = their level) on the 59 maps: up to 19020 list
  entries (tibet1), mean 4.7–22 per cluster, the longest 249 (romeric6: 315
  lights in 70 clusters), built in at most 2 ms; by the PVS alone (3.4,
  with the test entity lights) mean 27–305, up to 73000 entries (keep2).
  3.3's every light in every list would not fit on keep2, keep5 or tibet1
  (clusters × lights > 524288).
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
  `dyn_light_data` (Q2RTX's dynamic lights), written every 3D frame: the
  game's (4.4, below), then the test ones. A pixel picks one by its weight
  (`dynlight_weight`: luminance × solid angle, faded by its range, 0
  entirely below the horizon; Q2RTX picks uniformly), two passes over them;
  a sphere's range is in the entry's `spot_data` (a float's bits, 0 =
  unlimited; `dynlight_range`), where its light fades as a list sphere's
  (`sphere_light_window`). Spot lights come with the code, unused.
- **The game's dynamic lights** (4.4): the client's `cl_dlights` (32,
  `MAX_DLIGHTS`), which `r_scene.c` copies into the scene: its entity
  effects (`EF_MUZZLEFLASH` a 0.1 s flash 18 units ahead, `EF_BRIGHTLIGHT`
  radius 400, `EF_DIMLIGHT` and `EF_LIGHT` 200, the torch artifact),
  glowing projectiles by model flag (fireball, acid ball ~120; magic
  missile, vorpal missile, scarab 240 with `gl_extra_dynamic_lights`),
  `TE_EXPLOSION` (350, shrinking 300/s). GL added `radius − distance` to its
  lightmaps and models, where utils/light had halved static light when
  baking (`rangescale` 0.5): one of radius R is a sphere of 8 units with
  twice a map light of level R's intensity, 2 × `r_maplight_scale` ×
  (R/300)³, fading to 0 at R − minlight (GL's surfaces stop there; only the
  muzzle flash has one); the radius changes as GL's (explosions shrink,
  flames flicker by up to 31 per frame: ±20 % of the intensity). The
  color is the client's (with `gl_colored_dynamic_lights`), converted to
  linear as the map lights' (`VK_SRGBToLinear`). Left out: dark lights
  (`EF_DARKLIGHT`, the Necromancer's darkness while invincible: 4.10) and
  lights of a negative radius (the spit; GL lights nothing with them, the
  software renderer darkens: 4.10). An alias model whose entity owns one
  of them this frame (its key) within its bounds (a sphere around its
  origin from the model's bounds, scaled) goes into the light group
  (`VK_DynamicLightOwner`, see [Map lights](#map-lights-vk_maplightsc)):
  the light is inside it, and it casts no shadows from any light while it
  owns it (a projectile; also the Eidolon's and the Fallen Angel's muzzle
  flashes, 0.1 s per attack, the chase-cam player with the torch).
  `r_dlights 0` turns them off. **Extra lights:** the client's
  `cl_dlights` count in `cl.light_level` ([Scene](#scene-r_scenec), GL's
  rule, which the server uses for how well monsters see the player and
  the Assassin's cloak), so `gl_extra_dynamic_lights` stays 0 as in HoT
  and `r_scene.c` (`R_AddExtraDynamicLights`) adds the lights the client
  would make with it to the scene only: vorpal missiles, magic missiles
  and scarabs (the first flag of `CL_RelinkEntities`' model flag chain;
  radius 240 − 0–19, the client's colors with
  `gl_colored_dynamic_lights`), at the entity's server position,
  replacing its own light (the same key), flickering from a random stream
  of their own (the game's `rand()` stays as without them). With
  `gl_extra_dynamic_lights 1` the client makes them (and they count, as in
  HoT with the option) and the renderer doesn't.
  Cost (4.4, castle4, two lights from the magic missile, 1920x1080,
  Release, `vk_benchmark 1`): direct lighting 0.52 → 0.72 ms, bounce
  1.21 → 1.30 ms, the frame 7.5 → 7.7 ms. Without dynamic lights the
  pick returns at once. The denoised image follows a flash on its first
  frame (the anti-lag: castle4, the magic missile's frames beside
  `flt_enable 0`).
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
  `add_dlights` divides a dlight's intensity by 25; a test or map sphere's
  intensity is π × its radiance in both kinds (the same command looks the
  same as a list or a dynamic sphere). Calibrating to Hexen II's linear
  falloff is 4.9's. The lighting is stored RGBE-packed ×32
  (`STORAGE_SCALE_HF/SPEC`), which holds values up to 4088 / 32 ≈ 128:
  `packRGBE` clamps there (Q2RTX's wraps darker above it).
- The light buffer holds the **map's lights** first (`VK_MapLights`,
  none with `r_maplights 0`; without the fake lava lights while lava
  emits), then the **lava's** polygons (4.5, unstyled; none with
  `r_lava_light 0`), then the **test lights** (`VK_LoadWorld`
  clears them, `VK_ClearLights`; colors below 0 become 0), 4096 in all
  (`MAX_LIGHT_POLYS`; lava triangles past it are left out and counted):
  - `vk_testlight sphere [radius] [intensity] [r g b] [range]`: a sphere
    light in the lists at the eye (8, 1000, white, 0 = unlimited);
  - `vk_testlight dlight [radius] [intensity] [r g b]`: a dynamic sphere
    light at the eye (no range; in the UBO slots the game's lights leave);
  - `vk_testlight quad [size] [intensity] [r g b]`: a square polygon light
    at the eye facing the view direction (32, 50, white; two triangles);
  - `vk_testlight list`, `vk_testlight clear` (the test lights only).
    3.4–3.12's `vk_testlight entities` (a 1000 sphere at every light
    entity) is gone with 4.1: the map's lights are there.

  `VK_PrepareLights` (from `VK_PrepareUBO`) writes the buffer and the UBO
  fields every 3D frame.
- **Light styles** (4.2): each map light's scale is `d_lightstylevalue[style]
  / 264`, the value `R_AnimateLight` (HoT's, `r_scene.c`) sets for this
  frame from the server's style strings (Hexen II's `1`/`2`/`3` speed
  prefixes: 10/20/30 Hz; on `cl.time`, so frozen while paused), relative to
  GL's normal `'m'`: an unstyled light stays 1 and a style animates as in GL
  frame for frame; no clamp (`'z'` is 2.08; Q2RTX clamps to 2). The second
  scale is last 3D frame's value of the style, for the denoiser's gradient
  samples, which replay last frame's light choice with it; this frame's
  after the denoiser's history was dropped (Q2RTX's `temporal_frame_valid`).
  Test lights stay 1. A light at 0 (a switchable one off) stays in the
  lists: the light CDF weighs it by its scale, so it is never picked and a
  toggle needs no rebuild. `vk_lights` counts the styled lights, their
  styles and those off last frame; `r_dumpscene` prints the styles' values.
  Beside `glh2` (castle5's style 2 pulse, 60 shots 4 frames apart): the
  same timing (correlation 0.95 at no lag); GL's bright phase flattens at
  1.30× where its 8-bit lightmaps clip, ours goes on to 1.62×.
- `vk_lights` prints the lights, the map's (below), the lava's and the
  light models' emission ([Emissive surfaces](#emissive-surfaces-vk_emissivec)), the lists (entries, mean and longest,
  empty ones), lights inside solid or left out, the last frame's dynamic
  lights (those owned by an entity, left out dark / unlit / over 32, the
  most in a frame and the brightest since the map loaded), the build time, the
  statistics buffers' sizes and the camera cluster's list; `vk_lights stats`
  reads back the shadow rays the last frame counted; `vk_lights cull 0|1`
  turns range culling off and on (a check: the image must stay the same,
  only noisier). `r_debugview 17` shows each pixel's list length: black
  none, blue to green up to 8 (every sample weighs them all), yellow to red
  at 64 and more.

## Map lights (`vk_maplights.c`)

Story 4.1: the lights `utils/light` (the compiler of Hexen II's lightmaps)
lit each map from, as sphere lights, read from the entity lump with its
rules (`entities.c`, `ltface.c`) when the map loads (`VK_LoadMapLights`,
from `VK_LoadWorld` before the light lists):

- **Which:** every entity with a level (the compiler's `LightFace` lights
  from each); a classname starting with `light` has 300 without one:
  plain `light`s, torches, flames, candles, burners, gems, lanterns. 12,928
  on the 59 maps (9,512 plain), two of them other classnames (demo1's
  glowing `obj_tree2`, tower's `trigger_multiple`, inside solid).
- **Level:** `atoi` of a key starting with `light` (so `lightvalue1`/`2`
  too, and `"2oo"` is 2); the sphere's range is the level (the compiler's
  light reaches that far, falling off linearly). The compiler kept the
  last such key it read, but wrote each entity's keys into the lump in
  reverse (its epair lists): in the lump the first counts, for every key
  (the styles it added are the first key; eidolon's and thomas's
  `light_thunderstorm` have `"light" "500"` before `"lightvalue1" "12"`).
- **Style:** the `style` key; the compiler gave switchable lights (those
  with a `targetname`) 32 and up and wrote them into the lump. 254 lights
  have one, animated since 4.2 (see [Lights](#lights-vk_lightc)): the
  switchable ones follow what the game sets (off at `'a'`).
- **Spotlights:** a `target` makes one, towards the first entity of that
  `targetname` (its `origin`, 0 0 0 without one), its cone `angle` degrees
  wide (default 40); 42 on 10 maps (cath's nine aim down at a floor). See
  [Lights](#lights-vk_lightc) for the cone. Aimed at its own origin it lit
  nothing if narrower than 180°, else everywhere (an omni light here); none
  on the maps. The cone is decided by the cosine of half its width, as the
  compiler's (so `angle` −360 or 720 too): 179° or wider an omni light,
  narrower than 1° made 1° (the soft edge's width).
- **Dropped** (counted by `vk_lights`): a light whose origin is inside
  solid (the compiler traces from the origin, and a trace starting in a
  solid leaf is blocked, so it lit nothing). Its `TestLine` takes a start
  point within `ON_EPSILON` (0.1) of a plane to the side of the trace's
  other end, so a light on a solid's face lit the open side: dropped only
  when every leaf that close is solid (`OriginInSolid`). 181 on the 59
  maps (demo1 43, demo3 26, village1 26; the plain leaf test would drop 28
  more). Also a level below 0, a narrow spot aimed at its own origin (none
  on the maps) and lights past 4096.
- **Fake lava lights** (4.5): plain `light`s at most 16 units from a lava
  light triangle, over the lava or under its surface (`VK_OverLava`), are
  marked (`over_lava`); `vk_light.c` leaves them out while the lava emits
  (`r_lava_light 1`): 405 on 13 maps, see
  [Emissive surfaces](#emissive-surfaces-vk_emissivec). `vk_lights` counts
  them.
- **Color** (4.3, `r_maplight_colors` 1, archived): Hammer of Thyrion's
  colored light, the colors `utils/jsh2color` baked HoT's `.lit` files
  from (see [Map light colors](#map-light-colors-vk_lightcolorc)); on a
  map whose lights have `_color` (0–1, or 0–255 when a component is above
  1; later compilers' key, no original map has it) those, the other
  lights white, as that compiler's `.lit` has them. The 0–255 color
  multiplied GL's lightmap, which multiplies the texture in sRGB space, so
  the light's color is its sRGB → linear conversion: the same hue on a
  wall (HoT's orange 255 128 64 is (1, 0.22, 0.05)). Not scaled back to
  white's brightness, as in HoT's colored mode: torch light has 0.37× the
  luminance of white, 255 225 200 0.79× (the exposure adapts; 4.9
  calibrates). `r_maplight_colors 0`: white (the original's and HoT's
  default look); a change rebuilds the lights.
- **Brightness:** a sphere of radius 8 (the test spheres') whose intensity
  (π × radiance, as `vk_testlight`'s) is `r_maplight_scale` × (level /
  300)³: the one power under which inverse-square light scales with each
  light's range as the compiler's linear falloff does (twice the level
  and the distances, twice the light). `r_maplight_scale` 1000: a level
  300 light is as bright as 3.4–3.12's test entity lights, and its direct
  light 16 units from a wall stores ~85 of the ~128 the RGBE storage holds
  (level 1000 lights clip next to walls). The curve and scale are 4.9's.
- **Light models** (`VK_MapLightAt`): the light entities' game code spawns
  their torch, flame or candle model at the light's origin (`makestatic`,
  fullbright). Every flame mesh encloses its origin; from the 8-unit
  sphere a light's own model blocks 4–10 % of directions (flames, castle
  torches) up to 23–56 % (gems, candles, burners, meso torches). Opaque
  alias models whose origin rounds to a map light's (a hash of the rounded
  origins: the lump's are integers) go into `MODEL_GROUP_LIGHT`, which
  shadow rays don't see (`AS_FLAG_LIGHT_MODELS`, see
  [Instances](#instances-vk_instancec), [Acceleration
  structures](#acceleration-structures-vk_accelc)); in the original no
  model shadowed a map light. Without the group, meso1's and castle4's
  starts get 3.5 % less direct light (up to 10–13 % in 60-pixel blocks),
  the lit image 1–2 %. `vk_models` counts the group's triangles ("at
  lights"), `vk_lights` its models. With `r_maplights 0` no model is at a
  map light: the models shadow test lights as any model does. Since 4.4 an
  alias model whose entity owns a dynamic light within its bounds this
  frame (a glowing projectile) joins the group too, with `r_maplights 0`
  as well ([Lights](#lights-vk_lightc)). demo1's glowing
  tree stands at its own light, so it casts no shadows either.
- **Against GL** (4.9 calibrates): inverse-square falloff instead of
  linear; the cosine instead of the compiler's 0.5 + 0.5 cos (surfaces
  facing away get no light instead of half); no minimum light (the
  compiler had none either); models (monsters, doors) shadow the map's
  lights, as none did in the lightmaps. At the map starts beside `glh2`
  the light is where the lightmaps are bright; Hexenlicht is brighter.
- `r_maplights 0` turns them off (test lights only); a change of
  `r_maplights`, `r_maplight_scale` or `r_maplight_colors` rebuilds the lights (`VK_RebuildLights`: only while the client is in
  the loaded world, as between `map` and the new world's load the old
  one's memory is freed; else the next load takes them). `vk_lights` prints the
  lights of the light entities, the dropped ones, spotlights (unmatched
  targets), styled ones and those with `_color`, last frame's models at a
  light's origin, and where the colors come from (jsh2color's list and
  time, torch orange / from textures / 255 225 200); `vk_lights colors`
  lists each light's classname, origin, jsh2color color (0–275) and linear
  color.
- **Cost** (4.1, measured the same day as `main` with the test entity
  lights, `perf_baseline.ps1`): the frame is within the power-capped GPU's
  run-to-run scatter (1920x1080, demo1 at 100 %: 8.48 → 8.59 ms; 2560x1440:
  16.50 → 15.98 ms, the cathedral 14.54 → 15.34 ms).

## Map light colors (`vk_lightcolor.c`)

Story 4.3: HoT's colored light is `gl_coloredlight 1` (default 0, white)
with `.lit` files, which the game doesn't ship; HoT publishes a set
(`hexen2-litfiles-20140628`, the 42 maps of the original game) made by
`utils/jsh2color` (jsh2colour 1.2.6). A `.lit` is the tool re-baking the
lightmaps; `VK_LightColors` (from `VK_LoadMapLights`) is its step before
the bake that gives each light its color (`tyrlite.c` LightWorld, `ltface.c`
TestLightFace, CalcFaceVectors, CalcFaceExtents, CalcPoints, `trace.c`
TestLine, `entities.c` LoadEntities, `jscolor.c`), for every entity of the
lump:

- **Torches, flames, the gem** (`light_torch*`, `light_flame*`,
  `light_gem`, any case): orange 255 128 64 (2,893 on the 59 maps).
- **Plain lights** (`light`, and `light_fluor*`): the sum of the colors of
  the faces they reach, a face once when one of its sample points (the
  lightmap grid at 8 units, `-extra`, moved towards the face's middle
  where it can't see it) is in sight and gets at least a third of the
  level (within 2/3 of the range); sky faces never; a texture not in the
  list counts 1 1 1. In integers, scaled to a largest channel of 275
  (brighter than 255: "colored lights can seem darker") unless it is 255;
  a grey sum is 255 225 200 ("a faint orange tinge"). 2,595 of the 9,512
  are colored by textures.
- **Every other entity with a level** (candles, burners, lanterns, the
  palace torches, demo1's tree): 255 225 200.
- **A map where none ends up colored** got no `.lit`: white (tibet4, 5, 6,
  10).
- **The tool's own entity keys**, not utils/light's: the last of each
  counts, a level is `atof`'s integer part (`_light*` too), `wait` is the
  attenuation (1 without), `delay` the formula (0 linear; 1, 2, 3 don't
  fade: tower has one). A `rotate_` entity's brush faces are at its
  origin. Computed in double, as the tool.
- **The texture list** (tables in the file, with their matching rules):
  the tool's batch files (`utils/jsh2color/data_win/colour*.bat`) run
  each hub with its own (`-extra -nodefault -external`: `hexen2.def` for
  demo and village, `hexen2castle.def` for the castle hub and rider1a,
  `hexen2egypt.def` for egypt and rider2c, `hexen2meso.def`,
  `hexen2romeric.def`; a prefix, the first entry that matches), the
  deathmatch maps with the built-in list (`colourDm.bat`; `jscolor.c`'s
  chain, whole names or its prefix lengths); maps the batch files don't
  name (the mission pack, others) get the built-in list.
- **Faster, the same colors:** the tool tests every sample point of every
  face against every plain light (one thread: 3.1 s on demo1, 17 s on
  keep5). Here a point that would get less than a third of the level isn't
  traced, a face is skipped by its plane and by its sample points' bounds
  (the grid, and 8 units past it for points moved towards the middle),
  the points are computed only for faces some light can reach, and the
  colored faces come first: a light that reaches none is 255 225 200
  whatever else it reaches. Release: at most 83 ms on the original
  game's maps (egypt5), 183 ms on the mission pack's (keep5), median 14
  ms.
- **Checked** (4.3): every one of the 12,747 lights has the color a
  build of `utils/jsh2color` with a print of each light's color computes
  (TESTING.md "Light colors"). The tool adds to a light's sum from
  several threads without a lock: its multi-threaded runs differ between
  themselves (demo1: ~1,600 `.lit` bytes, up to 140), HoT's published set
  among them; `.lit` files baked from these colors match HoT's but for
  those and ±1 rounding (3 of 42 identical). This is the tool's
  single-threaded result.

## Emissive surfaces (`vk_emissive.c`)

Story 4.5. Hexen II has no fullbright texels (`colormap.lmp` keeps only
indices 0 and 255 constant) and `utils/light` gave lava no light: the
mappers put plain lights over it, or just under its surface, which the
compiler lit through. GL draws the turbulent textures unlit and the light
entities' torches, flames and candles with `MLS_ABSLIGHT` (abslight 0.75).
Two things emit, at GL's brightness times `r_emissive_scale` (32: the
radiance of a texture color of 1):

- **Lava** (`r_lava_light 1`): the lava materials' emissive texture is
  their base texture (warped, linear), their emissive factor the scale
  (Q2RTX's primary emission into `PT_TRANSPARENT`, not denoised; lava
  keeps Q2RTX's no bounce light on it). Each world lava triangle whose
  front leaf isn't lava (not the undersides; lava walls too) is a polygon
  light (Q2RTX's `collect_sky_and_lava_light_polys`: one per fan
  triangle, degenerate ones left out) of the texture's average linear
  color times the scale (Q2RTX's emissive `light_color`), and is flagged
  `MATERIAL_FLAG_LIGHT` (Q2RTX's `bsp_mesh.c`): diffuse bounce rays that
  hit it add nothing, since the direct light samples it; specular ones
  add it with Q2RTX's weight. Submodel lava emits but isn't a light (none
  on the maps). `r_lava_light 0`: no emission and the fake lights (GL's
  look); the flag stays (it only matters with emission). If the lava's
  triangles don't fit into the 4096 lights with the map's, there are no
  lava lights and the fake lights stay; triangles the full lists leave
  out light nothing (`vk_lights` reports both; neither on the maps).
- **Fake lava lights:** while lava emits, plain `light`s at most 16 units
  from a lava light triangle (`VK_OverLava`: the closest point on it) are
  left out ([Map lights](#map-lights-vk_maplightsc)): the mappers' grids
  8, 14 or 16 units over the lava or under its surface (castle4, castle5,
  meso5, romeric3 and part of meso8 have them inside); the next nearest
  light is 40 away. On the 16 lava maps (15 of the game's, the mission
  pack's `monsters`): 405 on 13 maps (castle4 12, castle5 53, meso1 16,
  meso2 149, meso5 23, meso6 6, meso8 39, meso9 63, romeric1 1, romeric3
  18, romeric4 15, village2 2, monsters 8), none with a style. Lava light
  triangles: castle4 11, castle5 220, meso1 58, meso2 492, meso5 20,
  meso6 27, meso8 117, meso9 44, ravdm1 42, ravdm5 39, romeric1 5,
  romeric3 50, romeric4 59, village2 2, village3 4, monsters 8; the most
  list entries meso2's 80,233 (mean 94, longest 212; 524,288 fit), built
  in at most 7 ms (Debug).
- **The scale** (4.9 calibrates it): in linear light (`tm_enable 0`)
  meso9's walls lit by the map's lights are about as bright as GL's
  lightmapped ones (0.024, 0.026, 0.015 against GL's 0.023, 0.020, 0.021
  in three blocks), so 1 is GL's fullbright. But lava at 1 lights its rooms
  25–33 times less than the fake lights it replaces (the light it adds per
  unit of scale on meso9's side wall and meso2's cave walls): 32 lights
  them about as they did. The lava is then 32 times GL's relation to the
  walls: the exposure adapts to it and it clips towards orange-white
  (meso9's start: sRGB 254 74 1 where GL shows 141 27 1; the shown lava
  hardly changes from 2 to 64, the walls do).
- **The light models' flames** (`r_emissive_models 1`): models at a map
  light's origin that GL draws with `MLS_ABSLIGHT` (the light group's
  `VK_MapLightAt` part, lit torches and flames; not dynamic light owners,
  nor a monster passing that point) show their skin's emissive texture: Q2RTX's
  `apply_fake_emissive_threshold` (the luminance of the texels with a
  channel of at least 215 in sRGB, `VK_EMISSIVE_THRESHOLD`, blurred with
  an 11-tap filter and normalized, times the texel's color and its
  normalized luminance squared; twice the size, bilinear with wrapping,
  then a 3-tap filter), the texture `<skin>*E<the skin's CRC>`, made when
  a model at a light first shows the skin (`VK_EmissiveSkin`, inside the
  frame: two uploads that wait for the GPU, the texture's and the new
  material's). The instance takes the skin's
  emissive material ([Skins](#skins-vk_skinc)), flagged
  `MATERIAL_FLAG_LIGHT` (the map light is its light), at GL's abslight
  (`model_geometry.comp`'s emissive factor) times the scale. On the paks
  the lit flame models' skins have 6–33 % bright texels (`flame.mdl` 12 %,
  `flame2.mdl` 33 %, `cflmtrch.mdl` 7 %; `newfire.mdl` 61 %), the unlit
  ones none (`castrch`, `egtorch`, `mesotrch`, `rometrch`, `burner`, and
  `palight`, which stays dark); 234 of the 476 models have some, so only
  the light models get one. Seen at castle5's start: the flames glow (the
  look hardly changes from scale 2 to 16), and a glossy surface
  (`pt_roughness_override 0.05`) reflects them (the ceiling over a torch
  +26–33 % in 32-pixel blocks; rough surfaces change only at the flames).
  Left out: the burner's flame (an entity 6 units over its light, not at
  its origin), glowing projectiles and other dynamic light owners, sprites
  and particles (effects).
- **Left out** (not emissive in GL): runes, `+0fire` and `+0sun`
  (buttons), water, slime and the other turbulent textures (unlit in GL
  because they are turbulent; E5's `.mat` files can make `*skulls`,
  `*rtex386`, `*rtex153`, `*rtex346` lava), the sky (4.6); no emissive
  surface has a light style (`vertex_buffer.h`'s `light_style_scale`
  stays 1).
- **Against GL:** the lava lights its rooms through light instead of the
  mappers' grids (their hot spots on meso2's lava are gone; castle5's lava,
  lit from inside by them, is dark and banded without emission); the
  exposure adapts to bright lava (its surroundings darker than GL's);
  flames glow instead of GL's flat 0.75.
- **Cost** (meso2, 1920x1080, Release, the GPU at its power cap, `main`
  the same day): at the start on the bridge, its walls lit by the lava
  below, the frame 8.1 → 9.2–9.5 ms (direct light 0.72 → 1.15, bounce 1.64
  → 2.3: polygon lights in lists of 94 entries); over the lava field no
  change (6.5 → 6.3–6.5; lava gets no bounce); `r_lava_light 0` as `main`.
- `vk_lights` prints the lava's triangles per material with their color,
  the fake lights left out, the lava polygons in the light buffer (and any
  past 4096), the emissive skins made; `vk_world` the lava lights, `vk_world
  materials` each material's emissive texture and factor; `vk_models` the
  instances with an emissive skin; `vk_textures` the kept skin pixels. A
  change of `r_lava_light` or `r_emissive_scale` rewrites the materials
  (after the GPU is idle) and rebuilds the lights.

## 3D view (`vk_view.c`)

- `VK_RenderView3D` takes the 3D view in pixels (`r_refdef.vrect` × UI
  scale, centered like the 2D), lets `vk_upscale.c` decide the render size
  (the view × `r_scale`, the width rounded up to even), jitter and
  upscaling (3.8, see [Upscaling](#upscaling-vk_upscalec)), fills the UBO
  and dispatches the view passes at the render size into `TAA_OUTPUT`
  (FSR's outputs with FSR), with `VK_ComputeBarrier` between them;
  `GL_EndRendering` calls `VK_DrawView3D` after beginning swapchain
  rendering: `fullscreen.vert` + `view_composite.frag` (Q2RTX's final
  blit: scales the upscaler's output over the view, see Upscaling; sRGB
  encode + `gamma` like the 2D) into the 3D rectangle, then restores the
  full viewport for the 2D.
- **`primary_rays.rgen`** (3.2): Q2RTX's primary rays, dispatched as its
  are (width / 2 × height × 2 checkerboard fields: the left half of each
  image holds the pixels where x and y have the same parity, the right half
  the others; swapped every other frame where the interleave doesn't blur,
  below), write the G-buffer:

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
  into the screen layout: `FLAT_COLOR`, `FLAT_MOTION`; with the denoiser or
  DLSS RR it blurs checkerboarded surfaces, so translucent surfaces show
  their blend instead of a fine checkerboard of the surface and what is
  behind it; with DLSS it also writes DLSS's inputs, see
  [DLSS](#dlss-vk_dlssc-vk_streamlinecpp)). Where it doesn't blur (the
  lit image without the denoiser and without RR) the fields swap every
  frame (3.12: `vk_ubo.c` sets `pt_swap_checkerboard` to the frame's
  parity, as Q2RTX without the denoiser), so a translucent surface's
  pixels show the surface and what is behind it in turn: averaged frames
  show the blend (DLSS SR's history, averaged screenshots), a single frame
  keeps the checkerboard (the TAA pass only copies without the denoiser).
  The debug views keep one layout, as nothing averages them; the motion
  check reads last frame's in that layout, so it is misaligned for the
  one frame after a swapped one.
  Unchanged from Q2RTX apart from `direct_lighting.rgen`'s launch check,
  weapon shadows, no sunlight and the hit-distance clear. Without the
  denoiser the lit image is noisy at one sample per pixel. The TAA pass
  (3.8), the bloom and tone mapping (3.7) and FSR (3.8) follow; without
  tone mapping (`tm_enable 0`) the composite clamps.
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
  motion `PT_MOTION.xy` as it is (UV units, R53), the specular hit distance
  `PT_SPECULAR_HIT_DIST` where the pixel traced a specular bounce (3.5a;
  RR needs none in every pixel, DECISIONS R55). The interleave writes them
  in the screen layout for RR (3.10, see
  [DLSS](#dlss-vk_dlssc-vk_streamlinecpp)).
- `r_debugview` (default 0 since the maps have lights, 4.1; 1 before) picks what the
  view shows: 0 the lit image (the TAA pass's, see
  [Upscaling](#upscaling-vk_upscalec), then bloom and tone mapping), or
  what **`debug_view.comp`** writes into `TAA_OUTPUT` at the render size
  (no jitter or TAA; the composite scales it, nearest):
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
  lighting's (15, 16, 18, 19, 20: `DEBUGVIEW_READS_LIGHTING`, which also
  holds 0, the TAA pass's place).

## Denoiser (`vk_asvgf.c`)

Story 3.6: Quake II RTX's A-SVGF (adaptive spatiotemporal variance-guided
filtering; `shaders/asvgf.glsl` explains it), `flt_enable 1` (Q2RTX's
default; 0 = the undenoised composite, as before). Its TAA pass
(`asvgf_taau.comp`) is the upscaler's (3.8, see
[Upscaling](#upscaling-vk_upscalec)). The shaders are Q2RTX's, unchanged but
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
  `prev_style_scale` is last frame's light style (4.2, see
  [Lights](#lights-vk_lightc)).
- `flt_show_gradients 1` adds the gradients to the image (red indirect
  diffuse, green direct diffuse, blue specular); `r_debugview 20` shows
  the history length.
- Measured (2560x1440, Release, test entity lights, temporary GPU
  timestamps): gradient reprojection 0.47 ms, the filters 2.8 ms
  (`compositing.comp`: 0.3 ms); the 3D view 6.6 ms instead of 3.9 (demo1's
  start), 7.1 instead of 4.2 (the cathedral's font).

## Upscaling (`vk_upscale.c`)

Story 3.8: Quake II RTX's resolution scale, TAA/TAAU and AMD FSR 1 behind
one small interface, which DLSS SR and RR (3.10, see
[DLSS](#dlss-vk_dlssc-vk_streamlinecpp)) join in the TAA pass's place.

- **Once per 3D frame** `VK_UpscaleEvaluate` (Q2RTX's `get_render_extent`
  and `evaluate_taa_settings`) decides, into `vk_upscale_t`:
  - the *unscaled* size: the 3D view with its width rounded up to even,
    the upscalers' output; the composite shows it 1:1, so an odd view
    width drops its last column, as before;
  - the *render* size: unscaled × `r_scale` (25–100 %, default 100; Q2RTX
    uses `scr_viewsize`, but Hexen II's `viewsize` is the HUD layout), the
    width rounded up to even for the checkerboard fields. The render
    targets keep the swapchain's size (enough up to 100 %), so a change
    needs no new images; the denoiser (`prev_width`) and the TAA
    (`prev_taa_output_*`) reproject across a size change, as for Q2RTX's
    dynamic resolution;
  - the TAA pass's mode, output size and jitter, whether FSR runs, the
    texture LOD bias (Q2RTX's: `pt_texture_lod_bias` + log2 of the scale
    when upscaling), FSR's constants and what the composite shows.

  `VK_PrepareUBO` puts them into the UBO (`width`/`height`, `unscaled_*`,
  `taa_output_*`, `sub_pixel_jitter`, `flt_taa`, `easu_const*`,
  `rcas_const0`). `vk_upscale` prints the last frame's.
- **`r_upscaler`** (archived, default 1):

  | | TAA pass (`asvgf_taau.comp`) | after tone mapping | composite |
  |---|---|---|---|
  | 0 TAA | at the render size, primary rays through the pixel centers | — | scales `TAA_OUTPUT` (1:1 at 100 %) |
  | 1 TAAU | jittered (Halton 2/3, Q2RTX's 128 samples), upsampled to the unscaled size (at 100 %: jittered TAA) | — | 1:1 |
  | 2 FSR 1 | TAAU's jittered TAA at the render size | EASU into `FSR_EASU_OUTPUT`, RCAS into `FSR_RCAS_OUTPUT` | 1:1 |

  FSR runs only below 100 % (Q2RTX's `flt_fsr_enable 1`) and with tone
  mapping (it wants the tone-mapped image); elsewhere 2 is TAAU. 3 DLSS SR
  and 4 DLSS RR (3.10) replace the TAA pass for the lit image (RR the
  denoiser too), jittered as TAAU, into `TAA_OUTPUT` at the unscaled size,
  shown 1:1, the render size clamped to DLSS's range (`VK_DLSSChoose`);
  where DLSS can't run, TAAU (`vk_upscale` says why); the debug views skip
  it. `vk_upscale_t` carries `dlss`, `dlss_mode` and `denoise` (the
  denoiser runs: `flt_enable`, not with RR), which `vk_view.c` and
  `VK_PrepareUBO` follow.
  `flt_fsr_easu`, `flt_fsr_rcas` (Q2RTX's toggles; RCAS alone sharpens
  TAAU's output, so it needs the denoiser) and `flt_fsr_sharpness` (0.2;
  0 the sharpest, clamped to 2). Q2RTX's `flt_taa` is registered but
  follows `r_upscaler`; its `flt_fsr_enable` is not registered.
- **The TAA pass** (`VK_UpscaleHDR`, Q2RTX's `vkpt_taa`) runs for the lit
  image after the interleave, before bloom and tone mapping: `FLAT_COLOR`
  (in Q2RTX's ×128 storage scale) into `TAA_OUTPUT` and the history
  `ASVGF_TAA_A` (PQ-encoded; last frame's is `ASVGF_TAA_B`, sampled
  Catmull-Rom at the motion vector: the longest of the 3x3 around the
  pixel). It blends in PQ space, the history clamped to the mean ± sigma
  of the 3x3 neighbourhood (`flt_taa_variance` 1; `flt_taa_anti_sparkle`
  0.25 clamps the new sample to its neighbours; Q2RTX's
  `flt_taa_history_weight` is read by no shader), taking a tenth of the
  new sample at most (less for a still pixel whose jittered sample fell
  far from its center). Without history (the denoiser's,
  Q2RTX's `temporal_frame_valid`, and the last 3D frame ran the TAA pass:
  not after a debug view) or without the denoiser (Q2RTX: no TAA without
  it) the UBO's `flt_taa` is `AA_MODE_OFF` and the pass copies the nearest
  render pixel. Changes: threads past the TAA output write their zero only
  inside the images (the dispatch is rounded up to 16x16 groups);
  `HQ_COLOR_INTERLEAVED` (the reference mode's accumulator) is 1x1.
  Hexenlicht's TAA differs from Q2RTX's `AA_MODE_TAA`, which also moves
  each primary ray to a random point in its pixel (`primary_rays.rgen`):
  ours keeps the pixel centers (crisp, the closest to GL), so every mode
  gives the UBO `AA_MODE_UPSCALE` (the shader blends alike in both) and the
  jitter and output size decide.
- **PQ clamps the lit image** at 10000 cd/m², 78 in linear units before the
  storage scale (Q2RTX's): hot spots above lose energy before the bloom.
  The test lights' hottest walls (romeric2) have up to 10/255 dimmer bloom
  halos than before 3.8; elsewhere the image matches within ±1 (PQ's fp16
  rounding).
- **FSR** (`VK_UpscaleDisplay`, Q2RTX's `fsr.c`): `fsr_easu_fp32.comp` and
  `fsr_rcas_fp32.comp` with `fsr_easu.glsl`, `fsr_rcas.glsl`,
  `fsr_utils.glsl` (Q2RTX's) and AMD's `ffx_a.h`/`ffx_fsr1.h` (`libs/fsr1`,
  v1.0.2, which fixed RCAS's limits after Q2RTX's copy); the constants from
  `FsrEasuCon`/`FsrRcasCon` (the headers compiled as C in `vk_upscale.c`).
  SDR pipelines only (`spec_hdr` 0; HDR 7.3), FP32 only (FP16 needs
  `shaderFloat16`; 7.2). Changes: EASU fetches its input texels one by one,
  each clamped to `TAA_OUTPUT`'s rendered part (Q2RTX clamps the gather
  point to the image, which moves taps by a texel at its edges), RCAS
  its input clamped to the view (Q2RTX reads one texel past it), neither
  writes past the view: our images have the swapchain's
  size and hold older frames past the rendered part. Its input is the
  tone-mapped linear image, as in Q2RTX (AMD recommends a perceptual one).
- **The composite** (`view_composite.frag`, Q2RTX's final blit) shows the
  top left `display_size` texels of `TAA_OUTPUT` or FSR's output over the
  view: 1:1 at the unscaled size, nearest at exactly half of it, Q2RTX's
  Lanczos 3 otherwise with its taps clamped to those texels; the debug
  views nearest. Q2RTX's "nearest" at half size samples its linear
  sampler, which blends neighbours; ours is a texel fetch. The lit image
  without tone mapping gets its storage scale taken out there (push
  constant `scale`).
- **The look:** TAA and TAAU soften the image and blur it during fast
  turns, sharpening within a few frames after (no lasting ghosting
  seen: demo1 turning and walking, village3's sheep); `r_upscaler 0` at
  100 % keeps GL's crisp, aliased edges; FSR at 50 % is sharper than TAAU
  at 50 %, TAA at 50 % blocky with Lanczos ringing at edges. The TAA does
  not remove the denoiser's low-frequency flicker (the variance clamp
  follows the current frame): a paused frame's temporal noise (demo1,
  test lights) was 0.024 before 3.8, 0.025 with TAAU, 0.020 with TAA,
  0.031 with TAAU at 50 %, 0.049 with FSR at 50 % (RCAS sharpens noise
  too); the mean stays within 1 %.
- **Measured** (2560x1440, Release, test entity lights, temporary GPU
  timestamps, the 72 fps cap lifted so every setting runs at full load;
  the GPU sat at a ~100 W power cap, 0.8–1.4 GHz, so the absolute times are
  about twice those 3.6/3.7 recorded, and 3.7's build measured the same way
  took 13.6 ms at demo1's start, 14.0 at the cathedral's font):

  | 3D view, ms | 100 % | 67 % | 50 % |
  |---|---|---|---|
  | TAAU, demo1 / cathedral | 14.2 / 14.3 | 6.4 / 6.4 | 3.4 / 3.4 |
  | TAA | 14.2 / 14.3 | 5.8 / 5.9 | 2.7 / 2.7 |
  | FSR | (TAAU) | 6.2 / 6.3 | 3.1 / 3.1 |

  The path tracer and denoiser take 0.42 of their 100 % time at 67 %; the
  TAA pass 0.47 ms at 100 % (the copy it replaces took 0.33), 0.35 ms for
  TAAU at 67 %, 0.18 ms for TAA at 67 %; EASU + RCAS 0.24–0.30 ms; bloom and
  tone mapping 0.69 ms at the view's size, 0.28 ms at 67 %'s render size.
- Left out: dynamic resolution (Q2RTX's `drs_*`: it steers by frame time,
  which uHexen2's 72 fps cap hides; needs GPU timers, 3.11 or 7.2), scales
  above 100 % (bigger images), the FP16 FSR variants (7.2), FSR's HDR
  variant (7.3), Q2RTX's reference accumulation mode (`HQ_COLOR_INTERLEAVED`,
  `pt_accumulation_rendering`: an option for 4.9's reference shots).

## DLSS (`vk_dlss.c`, `vk_streamline.cpp`)

Story 3.10: NVIDIA's DLSS Super Resolution and Ray Reconstruction through
Streamline, optional at runtime with the player's DLLs (PLAN §5; the
player's page is [DLSS.md](DLSS.md)). A-SVGF + TAAU stays the default.

- **Files:** `vk_streamline.cpp` holds only the Streamline calls, behind
  `vk_streamline.h`'s C interface (Streamline's headers are C++,
  `libs/streamline`); `vk_dlss.c` decides what the renderer gives DLSS;
  `dlss_inputs.glsl`, included by `checkerboard_interleave.comp`, writes
  the inputs.
- **Loading** (`VK_SLPreInit`, before the instance): `sl.interposer.dll`
  next to the exe, its signature checked first with Streamline's
  `sl::security::verifyEmbeddedSignature` (`WinVerifyTrust` without
  revocation checks, and NVIDIA's second certificate; Windows may still
  fetch missing certificates for the chain); a DLL that doesn't verify
  isn't loaded. The signed interposer checks the other Streamline DLLs
  itself. When the instance or the device fails through the interposer,
  `VK_Init` shuts Streamline down (`VK_SLVulkanFailed`) and starts over
  with `vulkan-1.dll`. `slInit`: engine
  "custom", a version and the project's GUID (no NVIDIA application ID);
  only the preference flags `eDisableCLStateTracking`,
  `eUseFrameBasedResourceTagging` and `eDisableDebugText` (Streamline's
  defaults would turn on over-the-air updates and downloaded plugins; the
  development DLLs draw text over the view without the last); plugins only from the exe's
  folder; no log files, Streamline's errors and warnings kept for
  `vk_dlss`. volk then loads Vulkan through the interposer's
  `vkGetInstanceProcAddr`, so Streamline's instance and device proxies add
  DLSS's extensions and its present proxy keeps its frame bookkeeping; the
  device enables `privateData`. `VK_SLDeviceReady` asks which features the
  GPU supports and fetches their functions with `slGetFeatureFunction`
  (`sl_dlss.h`'s helpers keep them in static variables). One console line
  at startup says what was found (nothing without the DLL). Streamline
  2.14.1's development DLLs are signed by NVIDIA too, whatever its guide
  says, and load.
- **Selection** (`VK_DLSSChoose`, from `VK_UpscaleEvaluate`): `r_upscaler`
  3 or 4 for the lit image when the feature can run, else TAAU
  (`VK_DLSSUnavailable`: no DLL, a bad signature, `slInit` failed, not
  supported, it failed, its images not created yet). DLSS's mode follows
  `r_scale`: 100 % DLAA, from 66 % Quality, from 58 % Balanced, from 50 %
  Performance, below that Ultra Performance. The render size is clamped to
  the mode's range from its optimal settings (at 1278x612: DLAA 1265–1278
  pixels wide, Quality and Performance 639–1278, Ultra Performance only
  426), the width even (the next mode up where the range has no even
  width); between Ultra Performance's size and Performance's the nearer
  (at 1278 pixels wide `r_scale` 25–41 renders at 33 %, 42–49 at 50 %;
  where a third of the width is odd, Ultra Performance can't be used and
  25–49 render at 50 %). RR reinitializes itself when the
  render size changes (`r_scale` or the window).
- **Inputs:** `checkerboard_interleave.comp`'s specialization constant
  `spec_dlss_inputs` (three pipelines): 1 (SR) writes `DLSS_DEPTH`, 2 (RR)
  also RR's guides, and blurs the checkerboard at translucent surfaces as
  the denoiser's path does (R56). The depth is that of a D3D-style
  projection, 0 at `DLSS_Z_NEAR` (4, GL's near plane) and 1 at
  `DLSS_Z_FAR` (16384, past the longest sight line in a Hexen II map;
  `hl_shared.h`): `PT_VIEW_DEPTH`, the distance along the ray, divided by
  the length of the pixel's view-space direction at z = 1 (the jittered
  pixel center), from the field pixel the motion vector comes from; 1
  without a surface. At blurred pixels the motion vector and the depth
  come from one of the two fields: for RR the one with the larger
  throughput (the split's share, noise-free; a tie goes to the field that
  went through or refracted: 3.12), since the brighter color, the
  denoiser's rule (Q2RTX's), flips per pixel on RR's noisy image. The
  guides: `get_reflectivity`'s albedo, Karis's
  environment-BRDF approximation of the reflectivity × the specular factor
  (as debug view 12), the shading normal with the roughness in `.w`,
  `PT_SPECULAR_HIT_DIST`; at blurred pixels half the pixel's and half the
  mean of its other-field neighbours inside the view (the interleave's
  cross). A pixel has no surface where `PT_VIEW_DEPTH` is
  `PRIMARY_RAY_T_MAX`: with two bounces the first stores its hit in
  `PT_SHADING_POSITION` before the interleave, material included. The
  images are full size only while a feature is chosen
  (`VK_DLSSImagesWanted`); `VK_DLSSBetweenFrames` (`VK_BeginFrame`, outside
  frames) recreates the render targets on a change, as a resize does, and
  frees what a feature no longer chosen holds (`slFreeResources` after
  `vkDeviceWaitIdle`).
- **Evaluation** (`VK_DLSSRun`, after the interleave, instead of
  `VK_UpscaleHDR`): the common constants with R53's conventions (the
  jitter negated; the motion vectors in UV units, scale 1; the UBO's
  column-major matrices as Streamline's row-major ones; the depth row added
  to `P` and `P_prev`; `clipToPrevClip` from the view matrices); the
  options every frame (mode, output size, HDR, `preExposure` 1, SR with
  DLSS's auto exposure; RR: normal and roughness packed, the view
  matrices); the images tagged valid until present (`FLAT_COLOR`,
  `TAA_OUTPUT`, `DLSS_*`, `FLAT_MOTION`, GENERAL layout, the render and
  output extents); a memory barrier over all commands before and after.
  RR takes the noisy image: no denoiser, and `pt_fake_roughness_threshold`
  1, as in Q2RTX without the denoiser. DLSS's history resets on the
  denoiser's reset events (`VK_ResetDenoiserHistory` calls
  `VK_ResetDLSSHistory`), after a frame without DLSS, and on a change of
  feature, mode or size. When an evaluation fails, the frame gets the TAA
  pass's copy (DLSS frames give the UBO `AA_MODE_OFF`) and later frames
  TAAU until `r_upscaler` (its callback), `r_scale` or the view's size
  changes. With `pt_num_bounce_rays 0.5` RR gets bounce light in every
  other row, alternating per frame (Q2RTX's half resolution without the
  denoiser): use 1, the default.
- **`r_dlss_preset`**: 0 (DLSS's default for the mode; with over-the-air
  updates off it comes with the player's DLL version: RR's F in 2.14.1) or
  a preset letter. RR's presets D, E and F cost the same (below).
- **The look:** SR and RR are a little softer than TAAU at 67 %, RR stays
  coherent down to 33 % and is steadier paused (temporal noise 0.019 at
  67 %, as in 3.9). DLSS's output is unclamped HDR, where the TAA's PQ
  encoding clamps at 10000 cd/m² (R51): stronger bloom around the test
  lights' hot spots. At the cathedral's font RR's input is blurred
  (without the blur RR shows the fine checkerboard); 3.12 found no view of
  the font where RR keeps it (DECISIONS R73). SR on the raw image
  (`flt_enable 0`) gets the swapped fields and resolves them in its
  history (R72).
- **Measured** (2560x1440, Release, temporary GPU timestamps, the 72 fps
  cap and the unfocused window's sleep lifted; the GPU at its ~100 W power
  cap, 0.9 GHz):

  | 3D view, ms (demo1 / cathedral start) | 100 % | 67 % | 50 % |
  |---|---|---|---|
  | A-SVGF + TAAU | 14.2 / 13.7 | 6.3 / 6.1 | 3.4 / 3.3 |
  | A-SVGF + DLSS SR | 15.8 / 15.7 | 7.8 / 8.1 | 5.7 / 6.0 |
  | DLSS RR | 24.8 / 25.6 | 11.9 / 12.1 | 6.9 / 7.3 |

  At 67 % on demo1: SR's evaluation 2.2 ms against the TAA pass's 0.36;
  RR's 8.2 ms (presets D, E, F: 8.16, 8.12, 8.19), the interleave with
  RR's inputs 0.21 ms (0.05 without, 0.07 with SR's depth), where the
  denoiser, interleave and TAA took 2.7 ms. The 3.9 spike's separate input
  pass is gone.
- Left out: frame generation, Reflex, dynamic resolution, DLSS's
  sharpening, RR's transparency layer (particles showed no smearing in
  motion), HDR output (7.3), the menu (6.10).

## Bloom and tone mapping (`vk_bloom.c`, `vk_tonemap.c`)

Story 3.7: Quake II RTX's bloom, tone mapping and auto exposure, its shaders
unchanged but the curve's launch check, run by `vk_view.c` on the lit image in `TAA_OUTPUT`
(`r_debugview 0` only; the debug views stay raw): bloom, then tone mapping,
Q2RTX's order after its TAA, at the TAA output's size (since 3.8: the render
size with TAA and FSR, the view's with TAAU). The TAA pass leaves the image
in Q2RTX's ×128 storage scale (`STORAGE_SCALE_HDR`), which the tone
mapper takes out; with `tm_enable 0` the composite takes it out and clamps
(before 3.8 `debug_view.comp` did).

- **Bloom** (`vk_bloom.c`, Q2RTX's `bloom.c`): `bloom_downscale.comp`
  averages the image into `BLOOM_VBLUR` at a quarter of its size,
  `bloom_blur.comp` blurs it horizontally into `BLOOM_HBLUR` and back
  vertically (a Gaussian of `bloom_sigma` 0.037 times the view's height,
  1–100 quarter-size pixels), `bloom_composite.comp` blends it in by
  `bloom_intensity` (0.002: a faint glow around very bright light, barely
  visible with the test lights). Both images are rgba16f, sampled
  linearly. `bloom_enable`, `bloom_debug 1-3` (the stages stretched over
  the view). Left out: Q2RTX's stronger, wider bloom under water (Hexen
  II's underwater look is GL's warp and tint, 6.6) and its blur behind
  menus.
- **Tone mapping** (`vk_tonemap.c`, Q2RTX's `tone_mapping.c`, Eilertsen,
  Mantiuk and Unger's noise-aware tone mapping with Q2RTX's changes; its
  shaders explain it): `tone_mapping_histogram.comp` bins the image's log
  luminance into the tone mapping buffer; `tone_mapping_curve.comp` (one
  workgroup of 128 threads, one per bin, subgroup arithmetic; without
  Q2RTX's launch check against the view's size, which returned some
  threads before its barriers in a view under 128 pixels wide) finds the
  exposure, the adapted luminance:
  the histogram's 70th–90th percentile (`tm_low/high_percentile`), clamped
  to `tm_min_luminance` 0.0002 – `tm_max_luminance` 1 and adapting over
  time (`tm_exposure_speed_up` 2, `_down` 1: per second, exponentially),
  and the tone curve, blended with last frame's; `tone_mapping_apply.comp`
  applies the curve and the exposure (`tm_reinhard` 0.5 blends a Reinhard
  curve in; `tm_exposure_bias` −1), a knee towards white
  (`tm_knee_start`, `tm_white_point`; the push constants from
  `KneeConstants`) and blue noise dither, linear [0, 1] out for the
  composite. Q2RTX's `tm_*` cvars (registered since 3.1) with its
  defaults; `tm_enable` (the UBO gets 0 or 1, as the host decides),
  `tm_debug 1/2` (the histogram or the curve over
  the view, drawn by the shader). The adaptation runs on game time between
  3D frames (at most 1 s), on real time while it stands still (paused).
  The exposure starts over (Q2RTX's request_reset: the buffer cleared, the
  curve not blended) on a new map, with new pipelines, and when the last
  3D frame wasn't tone mapped (`vk_render_frame` not the next one: a debug
  view, `tm_enable 0`). Left out: the HDR output variant (7.3); the full
  screen blend and colorize (`fs_blend_color`, `fs_colorize`: Q2RTX's
  blend is strongest at the screen's edges; GL's view blend `v_blend`,
  not drawn yet, comes with 6.6); Q2RTX's on-screen adapted luminance
  line (`vk_exposure` prints it).
- **Readback** (Q2RTX's `ReadbackBuffer`, only `adapted_luminance`
  written): the curve pass writes the adapted luminance into this frame's
  mapped readback buffer (one per frame in flight, by device address); the
  CPU reads it when the slot comes round (`VK_ReadbackAddress` in
  `VK_PrepareUBO`, two frames old; a compute-to-host barrier after the
  curve pass), as Q2RTX's `prev_adapted_luminance`, 0.005 until the first.
  Q2RTX ignores readbacks of exactly 1 as "mysterious spikes"; 1 is
  `tm_max_luminance`'s clamp, which a bright scene reaches, so only values
  that aren't positive and finite (a slot never written) are ignored.
- **Effects brightness** (`effects_brightness`,
  `path_tracer_hit_shaders.h`): GL draws particles and sprites unlit at
  their colors; under the exposure they are scaled by
  `prev_adapted_luminance × pt_particle_brightness` (Q2RTX's for its
  particles; its sprites have their own factor, ours share it), 1 with
  `tm_enable 0` and in the debug views. `pt_particle_brightness` is 15
  (Q2RTX 100), measured: over the pixels of a paused frame's meteor staff
  particles (demo1), the tone-mapped effects' mean luminance matched their
  GL colors (0.095 against 0.096) at 15, and at a sixteenth of the lights'
  intensity 0.104: the exposure scaling holds them within ~10 %. 100 shows
  them about 3× too bright.
- Measured (demo1's start, test entity lights): after the lights dim
  16×, the image comes back over ~6 s (image mean 0.010 → 0.053, 0.070
  before; the adapted luminance 1/16 of before); 1000× dimmer, the
  exposure stops at `tm_min_luminance` and the image stays dark (0.014).
  Cost at 2560x1440, Release (temporary GPU timestamps): bloom 0.34 ms,
  tone mapping 0.26 ms.
- The look: Q2RTX's curve lifts the shadows and flattens the contrast of
  Hexen II's dark scenes compared with the clamped image before 3.7; the
  calibration against GL (4.9) and keeping dark places dark (4.10:
  `tm_min_luminance`, per-map exposure 4.7) decide the settings.

## Profiler (`vk_profiler.c`)

Story 3.11: Quake II RTX's `profiler.c`, GPU timers for every pass, their
overlay, and a measuring mode.

- **Markers:** a pass is bracketed by `VK_ProfilerStart` and
  `VK_ProfilerStop` with its entry of `vk_local.h`'s `PROFILER_LIST` (name
  and indent; entries nest in the list's order), which write a timestamp
  each into the frame in flight's range of one query pool. `VK_BeginFrame`
  calls `VK_ProfilerBeginFrame`, which reads the range the frame in flight
  used last time (its fence has made it ready: nothing waits on the GPU,
  the timings are two frames old), resets it and starts `frame`;
  `VK_EndFrame` stops it. Always on, as in Q2RTX: about 40 timestamps per
  frame. With the validation layer (Debug builds, or `-validation`; it
  loads the debug-utils extension) each marker also sets a debug label for
  RenderDoc and Nsight; Release captures without it have none.
- **Entries:** `frame` (the whole command buffer); `model geometry`,
  `dynamic BLASes`, `TLAS`; `3D view` with `primary rays`,
  `reflect/refract`, `gradient reproject`, `direct lighting`, `debug view`,
  `bounce 1`, `bounce 2`, `denoiser` (`gradients`, `temporal`, `a-trous`)
  or `compositing`, `interleave`, the upscaler (named by what ran: `TAA`,
  `TAAU`, `TAA copy`, `DLSS SR`, `DLSS RR`), `bloom`, `tone mapping`,
  `FSR`; `composite and 2D` (the swapchain pass). An entry that didn't run
  in a frame drops its samples and its row. `vk_accel` and `vk_models` read
  their build times from it (3.10 and earlier kept query pools of their
  own).
- **Changes from Q2RTX:** both timestamps are taken after all earlier
  commands (Q2RTX starts at the top of the pipe), so an entry's time is its
  own passes'; the samples are a ring of 1000 per entry, of which
  `profiler_samples` (60) are averaged (Q2RTX reallocates); the upscaler
  names itself; the overlay follows `vid_uiscale` (no `profiler_scale`).
- **`profiler 1`** draws the table (name, last frame, average) over the top
  left of the screen, below the notify lines, on a translucent box
  (`VK_DrawShade`), with the game's font (`VK_DrawProfiler`, from
  `GL_EndRendering` before the 2D batch is drawn); **`vk_profiler`**
  prints it.
- **`vk_benchmark 1`** (not archived) lifts the 72 fps cap (`host.c`), the
  sleep of an unfocused or paused window and the per-frame throttle
  (`sys_throttle`; `sys_win.c`), guarded upstream edits (UPSTREAM.md), so
  the GPU runs at full load. Without it the
  times depend on the GPU's clocks, which follow the load: with the cap and
  the sleeps the clocks swing between idle and bursts, and the same scene
  measures anywhere from 7.6 to 13.6 ms (the cathedral at 100 %,
  2560x1440); with it they hold steady (on this machine at its 100 W power
  cap, about 1 GHz). For measuring only: Hexen II's physics isn't meant for
  more than 72 frames a second.
- **Baseline:** `tools/hexenlicht/perf_baseline.ps1` runs demo1's and the
  cathedral's starts with the maps' lights (since 4.1; the tables below
  had test lights at the light entities, whose cost 4.1 measured to be the
  same within the scatter, see [Map lights](#map-lights-vk_maplightsc)), paused, `vk_benchmark 1`,
  `viewsize 100`, `fov 90`, at each window size, and prints the averages
  as markdown (TESTING.md's "GPU cost"). Runs agree within 5–10 %.
  Recorded 2026-09-26 (after the throttle fix of the review), Release,
  RTX 4070 Ti (driver 616.92) at a 100 W power limit (about 1 GHz; an
  unlimited card runs roughly 2.5 times as fast), 120-frame averages, ms:

  | 2560x1440 | demo1 TAAU 100 % | demo1 TAAU 67 % | demo1 DLSS RR 67 % | cath TAAU 100 % | cath TAAU 67 % | cath DLSS RR 67 % |
  |---|---|---|---|---|---|---|
  | frame | 15.67 | 7.14 | 12.37 | 13.67 | 6.49 | 12.31 |
  | model geometry | 0.05 | 0.05 | 0.04 | 0.03 | 0.03 | 0.03 |
  | dynamic BLASes | 0.98 | 0.83 | 0.87 | 0.82 | 0.68 | 0.87 |
  | TLAS | 0.13 | 0.11 | 0.11 | 0.09 | 0.11 | 0.09 |
  | 3D view | 14.43 | 6.08 | 11.28 | 12.66 | 5.61 | 11.25 |
  | – primary rays | 2.08 | 0.94 | 0.97 | 1.67 | 0.80 | 0.96 |
  | – reflect/refract | 0.27 | 0.09 | 0.10 | 0.27 | 0.08 | 0.09 |
  | – gradient reproject | 0.52 | 0.25 | | 0.52 | 0.24 | |
  | – direct lighting | 1.33 | 0.49 | 0.58 | 1.26 | 0.55 | 0.58 |
  | – bounce 1 | 3.06 | 1.13 | 1.28 | 2.47 | 0.97 | 1.20 |
  | – compositing | | | 0.08 | | | 0.10 |
  | – denoiser | 5.89 | 2.24 | | 5.29 | 2.09 | |
  | – – gradients | 0.39 | 0.17 | | 0.41 | 0.18 | |
  | – – temporal | 1.57 | 0.59 | | 1.36 | 0.54 | |
  | – – a-trous | 3.94 | 1.48 | | 3.53 | 1.37 | |
  | – interleave | 0.14 | 0.05 | 0.21 | 0.12 | 0.04 | 0.19 |
  | – TAAU / DLSS RR | 0.46 | 0.33 | 7.47 | 0.49 | 0.36 | 7.54 |
  | – bloom | 0.37 | 0.31 | 0.32 | 0.31 | 0.26 | 0.32 |
  | – tone mapping | 0.29 | 0.25 | 0.26 | 0.25 | 0.21 | 0.26 |
  | composite and 2D | 0.09 | 0.07 | 0.08 | 0.08 | 0.06 | 0.08 |

  | 1920x1080 | demo1 TAAU 100 % | demo1 TAAU 67 % | demo1 DLSS RR 67 % | cath TAAU 100 % | cath TAAU 67 % | cath DLSS RR 67 % |
  |---|---|---|---|---|---|---|
  | frame | 8.12 | 3.72 | 6.94 | 7.61 | 3.56 | 6.71 |
  | 3D view | 7.23 | 2.96 | 6.15 | 6.80 | 2.91 | 6.00 |
  | – bounce 1 | 1.45 | 0.73 | 0.81 | 1.17 | 0.56 | 0.64 |
  | – denoiser | 2.83 | 1.03 | | 2.79 | 1.07 | |
  | – TAAU / DLSS RR | 0.22 | 0.15 | 4.06 | 0.25 | 0.15 | 4.03 |

  The dynamic BLASes (the models' and the effects' triangles, rebuilt every
  frame) take about 1 ms at any size; the denoiser's à-trous filter is the
  largest single pass.
- Left out: dynamic resolution (7.2), CPU timings (upstream's `showfps`),
  graphs.

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
| `tm_enable 0/1`, `tm_debug 0-2`, `bloom_enable 0/1`, `bloom_debug 0-3` | tone mapping and auto exposure (Q2RTX's `tm_*` cvars tune them), their histogram or curve over the view; bloom (`bloom_sigma`, `bloom_intensity`) and its stages (see [Bloom and tone mapping](#bloom-and-tone-mapping-vk_bloomc-vk_tonemapc)) |
| `vk_exposure` | the adapted luminance read back (two frames old) |
| `profiler 0/1`, `profiler_samples`, `vk_profiler`, `vk_benchmark 0/1` | the GPU timers over the screen, the frames they average (60); printed; full load for measuring (no 72 fps cap, no sleep when unfocused) (see [Profiler](#profiler-vk_profilerc)) |
| `r_scale 25-100`, `r_upscaler 0-4`, `vk_upscale` | the render size in percent of the view's; 0 TAA, 1 TAAU, 2 FSR 1 (`flt_fsr_easu`, `flt_fsr_rcas`, `flt_fsr_sharpness`), 3 DLSS SR, 4 DLSS RR; the last frame's sizes, jitter and passes (see [Upscaling](#upscaling-vk_upscalec)) |
| `vk_dlss`, `r_dlss_preset` | Streamline's state (DLL, signature, support, versions, driver, evaluations, its log's warnings and errors), DLSS's images, mode and render sizes; DLSS's model: 0 its default, or a preset letter (see [DLSS](#dlss-vk_dlssc-vk_streamlinecpp)) |
| `pt_particle_brightness` | the effects' brightness under the exposure (15) |
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
| `vk_testlight sphere, dlight, quad, list, clear` | test lights, added to the map's (see [Lights](#lights-vk_lightc)) |
| `r_maplights 0/1`, `r_maplight_scale`, `r_maplight_colors 0/1` | the map's lights off/on (1), the intensity of a level 300 one (1000), white or HoT's colors (1, archived; see [Map lights](#map-lights-vk_maplightsc)) |
| `vk_lights`, `vk_lights stats`, `vk_lights cull 0/1`, `vk_lights colors` | light lists, light statistics read back, range culling off/on, each map light's color |
| `r_lava_light 0/1`, `r_emissive_scale`, `r_emissive_models 0/1` | lava emits and lights, without the mappers' fake lava lights (1), or GL's look (0); the emission of a texture color of 1 (32); the light models' flames glow (1) (see [Emissive surfaces](#emissive-surfaces-vk_emissivec)) |
| `r_dlights 0/1`, `gl_colored_dynamic_lights 0/1`, `gl_extra_dynamic_lights 0/1` | the game's dynamic lights off/on (1); their colors (HoT's option, 1 here) and the client's extra projectile lights (0 as in HoT: they count for gameplay; the renderer makes its own; see [Lights](#lights-vk_lightc)) |
| `vk_reload_shaders` | rebuild pipelines from the SPIR-V on disk |
