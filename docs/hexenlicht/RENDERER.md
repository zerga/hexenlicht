# Hexenlicht — renderer reference

How the `hexenlicht` target's renderer (`engine/hexenlicht/`) is built, module
by module. Read the section you need; each file's header comment has more.
Decisions and their reasons are in [DECISIONS.md](DECISIONS.md), the Quake II
RTX import rules in [Q2RTX.md](Q2RTX.md), testing in [TESTING.md](TESTING.md).
Keep this file current: a PR that changes a module updates its section.

Contents: [Build](#build-target) · [Window](#window-and-video-modes-vid_vkc) ·
[Vulkan core](#vulkan-core-and-swapchain) · [Shaders](#shaders) ·
[Textures](#textures-vk_texturec) · [Image files](#image-files-vk_imagefilec) ·
[Material files](#material-files-vk_matfilesc) · [Texture export](#texture-export-vk_exportc) · [2D](#2d-vk_drawc) · [Scene](#scene-r_scenec) ·
[Buffers and layouts](#buffers-and-gpu-data-layouts) · [Materials](#materials-vk_materialc) ·
[World](#world-vk_worldc) · [PVS](#pvs-vk_pvsc) · [Instances](#instances-vk_instancec) ·
[Alias models](#alias-models-vk_modelc) · [Skins](#skins-vk_skinc) ·
[Effects](#effects-vk_effectsc) · [Effect lights](#effect-lights-vk_effectlightc) · [Beams](#beams-vk_beamlightc) · [Acceleration structures](#acceleration-structures-vk_accelc) ·
[Path tracer framework](#path-tracer-framework) · [Lights](#lights-vk_lightc) ·
[Map lights](#map-lights-vk_maplightsc) · [Map light colors](#map-light-colors-vk_lightcolorc) ·
[Light fit](#light-fit-vk_lightfitc) ·
[Emissive surfaces](#emissive-surfaces-vk_emissivec) · [Sky](#sky-vk_skyc) ·
[Map file](#map-file-vk_mapfilec) · [Light editor](#light-editor-vk_lighteditc) ·
[Calibration](#calibration-vk_calibc) ·
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
  post-build; the shipped per-map files `data/hexenlicht/maps/*.hlmap` into
  `maps\` next to it (4.7, [Map file](#map-file-vk_mapfilec)).
- `engine/hexenlicht/stubs.c` provides the renderer symbols not implemented
  yet, sectioned by the story that replaces them; a story moves its section
  into real files. It still holds `R_InitTextures`/`r_notexture_mip` (GL's
  checkerboard), the rest of `R_Init` and GL-named cvars
  kept so configs keep their settings (`gl_glows`, `gl_coloredlight`,
  `gl_lightmapfmt`, …); the client reads `gl_colored_dynamic_lights` (0,
  white, as in HoT since 4.9; 4.4 had 1) and `gl_extra_dynamic_lights` (0 as in HoT:
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
  formats; enables RT pipeline, NV SER, position fetch and BC textures
  (`textureCompressionBC`, 5.2) when present), VMA,
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
  pass writes 8-bit colors with `linear_to_color()` from `transfer.glsl`,
  see [Textures](#textures-vk_texturec); recreated lazily
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
- **Colors** (4.17): GL multiplied a texture's 8-bit color by the
  lightmap's and showed the product as it was, which is the product in
  linear light only under a pure power; so Hexenlicht's 8-bit colors are
  the 2.2 power of linear light (`shaders/transfer.glsl`'s
  `color_to_linear`/`linear_to_color`, `VK_ColorToLinear` on the CPU),
  and `r_srgb 1` (archived, 0) takes the sRGB curve instead, the modern
  standard, whose linear toe showed GL's dark tones up to 2–3× darker
  (4.11a's survey: dark views 0.44–0.62 of GL's look while the light
  matched). Images are `R8G8B8A8_UNORM` (were `_SRGB`): the shaders read
  the colors as they are, and the base, emissive and sprite colors, the
  sky's blend and the colorshade tint become linear light where they are
  read (`path_tracer_rgen.h`, `indirect_lighting.rgen`,
  `path_tracer_hit_shaders.h`); the mask's alpha and the (unused) normal
  and falloff maps are read as they are (E5's data maps stay so; its
  color maps take the curve). The mips, blitted on the GPU, and the
  filtering average the 8-bit colors, as GL's did (the sRGB format did it
  in linear light). The composite writes 8-bit colors by the curve
  (`view_composite.frag`), the dither steps by one of them
  (`tone_mapping_apply.comp`), the 2D uses the textures as they are
  (`draw2d.frag`); the CPU's colors take the curve too: the map and
  dynamic lights' colors, the sky's dome average and `r_sun_color`, the
  lava's average (both kept, `vk_emissive.c`), particles (`vk_effects.c`),
  the flames' emissive textures, `vk_screenshot`'s averaging, the
  weapon's least light (`darkness.glsl`). The flag is the UBO's
  `color_srgb`, so a change shows at once but for the flames (their
  textures with the next map's materials); it rebuilds the lights
  (`VK_MapLightColorsChanged`). Not a lighting change: light transport,
  the lights' shape, shadows, bounces and reflections are the same; dark
  textures reflect less (a color of 25: 0.0060 of white instead of
  0.0097) and dark light shows brighter; both lighting modes.
- **Measured** (4.17, Release, `calib_shots.ps1` against `glh2`, every
  shot decoded by the 2.2 power, the look on the surfaces with GL's
  clipped blocks): at 4.11a's 43 Blackmarsh views (its 16 entrances and 27
  deathmatch spots) 27 → 36 within 0.85–1.2 of GL's look, the mean error
  per view 0.28 → 0.17 stops, the spread within a view 0.85 → 0.64 stops,
  pooled 0.98 → 0.98 (with `r_maplight_fit_scale` 1.1 → 1); the dark views
  village1's 0.73 → 0.97, village2's start 0.65 → 0.87, village4's 0.41 →
  0.73; at 4.9's bookmarks without the lava rooms 11 → 13 of 13, 0.16 →
  0.09 stops, castle4 0.64 → 0.90, the tower 0.73 → 0.96 (4.16's dark
  stone). An offline simulation from albedo shots (`r_debugview 1`) had
  predicted it within 3–7 % (the bounce off dark textures and the mips
  make it a little darker). Left outside: demo2's closed door (0.50: the
  lightmaps have no door shadows) and, as 4.11a found, the fit's
  granularity (demo2's sliding doors 2.18, village5's lone torch 1.38,
  village4 0.73, demo1 and village1 0.76–0.82: story 4.18, not planned, DECISIONS
  R106).
- Slot 0 is white; freed slots point back to it. `D_FlushCaches` purges
  slots above `gl_texlevel` on a map change like upstream;
  `D_ClearOpenGLTextures` also clears the 2D pic cache
  (`Draw_ClearCachedPics`), like `gl_rmisc.c`, and (5.3) the material
  files' sets and images (`VK_MaterialFilesPurged`).
- `TEX_SPECIAL_TRANS` alpha is stored as opacity (GL blends those inverted),
  so alpha means opacity in every texture.
- Samplers by the flags: mipmapped textures trilinear and anisotropic,
  repeating; `TEX_NEAREST` point sampled, the rest bilinear, both clamped,
  or repeating with Hexenlicht's `TEX_REPEAT` (the 2D backtile, the sky's
  layers, 4.6).
- The 8-bit pixels of alias model skins (`gl_model.c` names them
  `<model>_<skin>`) with a texel whose channel reaches
  `VK_EMISSIVE_THRESHOLD` (215) stay with their slot (4.5, freed with it):
  `VK_TextureRGBA` converts them again for the light models' emissive
  textures ([Emissive surfaces](#emissive-surfaces-vk_emissivec)).
  `vk_textures` prints how many (meso9: 100 skins, 2.6 MB).
- `VK_Convert8Pixels` (5.4) is the upload's 8-bit conversion for
  pixels that aren't in a slot, with the flags as the conversion settles
  them (`TEX_ALPHA`): the texture export's colors ([Texture
  export](#texture-export-vk_exportc)).
- **Image files** (5.2, [below](#image-files-vk_imagefilec)):
  `VK_LoadImageTexture` makes or replaces the slot of an identifier with
  a file's levels in its format (`R8G8B8A8_UNORM`, `BC7_UNORM_BLOCK`,
  `BC5_UNORM_BLOCK`), its CRC 0 (the cache's CRCs are the originals').
  One upload path for both (`VK_UploadLevels`): the levels given are
  copied, and with `TEX_MIPMAP` an uncompressed image gets the rest of its
  chain blitted; a compressed one keeps what it has. `GL_LoadTexture`'s
  images are one level, so its calls are the same as before (checked: the
  3D view of demo1 and meso2 identical to `main`'s within the runs' noise,
  the HUD's health number aside).

## Image files (`vk_imagefile.c`)

Story 5.2: reads the image files of [MATERIALS.md](MATERIALS.md) into
memory as they are; `VK_LoadImageTexture` (above) uploads them. The
material system (5.3, [below](#material-files-vk_matfilesc)) picks the
names and puts the images into materials.

- **Lookup:** `VK_LoadImageFile` takes a path without an extension and
  tries `.png`, `.tga`, `.dds`, `.ktx2` in turn, each through the whole
  search path (`FS_OpenFile` without a file, `FS_FileExists`'s call,
  which gives the size; a later game folder first), or a path with one of
  them (only that file); lowercased (paks compare names exactly).
- **PNG, TGA:** `stb_image` (built for those two, `libs/stb`): every
  channel count becomes RGBA, 16 bits per channel 8; gamma and color
  profile chunks aren't applied.
- **DDS:** the DX10 header's `BC7_UNORM` (and `_SRGB`), `BC5_UNORM`,
  `R8G8B8A8_UNORM` (and `_SRGB`), `B8G8R8A8_UNORM` and `B8G8R8X8_UNORM`
  (and `_SRGB`); the older header's FourCC `ATI2` or `BC5U` and 32-bit RGB
  with an R mask of `0xff` or `0xff0000` (alpha 255 without its mask).
- **KTX2:** `R8G8B8A8`, `B8G8R8A8`, `BC7` (UNORM or SRGB), `BC5_UNORM`; a
  2D image, one layer and face, no supercompression; the level index gives
  each level (levelCount 0: one).
- **As loaded:** an `_SRGB` format is its UNORM twin (the same bytes: the
  shaders decode colors, DECISIONS R104), BGRA is swapped to RGBA and an
  X byte becomes an alpha of 255, so an image is RGBA8, BC7 or BC5, its
  levels those of the file (at most its chain). Refused, with a reason in
  `img->error`: other formats (BC1, BC3, BC4, `BC5_SNORM`, float), cube
  maps, arrays, volumes, a size above `maxImageDimension2D` (32768 on the
  RTX 4070 Ti), BC without the GPU's BC support (`vk.have_bc`,
  `textureCompressionBC`, enabled when the GPU has it; `vk_info` prints
  it), files whose levels don't fit (every size is checked against the
  file's before a copy), a PNG or TGA of more than 8192x8192 texels
  (`MAX_DECODED_TEXELS`: a small file can claim a huge image), an empty
  file and a name with `*` or `?` (both end the game in `quakefs.c`:
  `FS_LoadFile`'s `Sys_Error` on 0 bytes, the loose files' wildcard
  lookup). The loader never prints (5.3 may load while a frame is
  recorded) and writes its reasons without `va()` (5.3 may load on
  threads).
- **`vk_imagefile <file> [scale]`** reads a file into the texture
  `*imagefile`, prints the file, its kind and format, size, levels in the
  file and uploaded, and the read, decode and upload times (whole ms), and
  shows it at the top left of the 2D screen over black, alpha blended
  (`VK_DrawTexture`, the colors as they are: the file's values on screen
  with `gamma 1`), scale screen pixels per texel (0 fits the screen),
  until `vk_imagefile` alone or the textures' purge on a change to
  another map (`gl_purge_maptex`).
- **Measured** (`tools/hexenlicht/imagefile_set.ps1`,
  `imagefile_compare.ps1`: 32 files at one pixel per texel, 7 at a
  quarter; TESTING.md): the uncompressed ones exactly their sources (PNG
  8 and 16 bits, TGA, DDS RGBA8 and BGRA8 with both headers, BGRX8 and
  X8 layouts with X 0, KTX2 RGBA8 and BGRA8, alpha over black through
  PNG, DDS and KTX2), the grey PNG, BC7 and BC5 exactly texconv's decode
  but for one BC7 4x4 block of the 200x120 image, 2–4 off (BC7 is
  specified bit-exactly, so one of the decoders, the GPU's or
  DirectXTex's, deviates there; not looked into further: the loader
  copies the blocks as they are, and the KTX2 of the same data shows the
  same) and BC5 by 1; the mips: files whose levels are flat colors of
  their own (RGBA8, BC7 and BC5 DDS; RGBA8 and BC7 KTX2) show level 2's color at a
  quarter, and one with two levels the second's (the rest blitted from
  it), and a noise PNG at a quarter is within 1 of its source averaged
  over 4x4 (the blitted mips); the lookup order (`.tga` before `.dds`
  before `.ktx2`, a `.dds` before a `.ktx2` of other content); all twelve
  refusals (BC1, `BC5_SNORM`, float, cut short DDS and KTX2,
  supercompressed, not an image, empty, a wildcard, two missing names)
  with their reason; Debug validation clean. **Times** (Release,
  2048x2048): a PNG 71–74 ms to decode (8 MB of noisy pixels), a TGA 4–5
  ms (40 ms to read on a cold cache), a BC7 DDS 1–2 ms read, 1 ms parsed,
  1–2 ms uploaded, an RGBA8 DDS with mips 12–14 ms in all: PNG decoding
  is what a map load of replaced textures pays (5.3).
- **`VK_ImageFileHasAlpha`** (5.3): coverage, an alpha below 255 in level
  0; a BC7 block counts as opaque when its mode has no alpha (0–3) or its
  alpha endpoints (and p-bits, and a rotated channel's in modes 4 and 5)
  are all at their maximum; BC5 has none. *5.6: below `OPAQUE_ALPHA`
  (250, 98 %), every endpoint unquantized as the spec does (`Expand`): texconv
  writes an opaque image's BC7 blocks with a p-bit of 0 where the color
  wants it, alpha 254 in mode 6 and 251 in mode 7 (0.1–2 % of the texels
  of the test pack's albedos, every file flagged before), which made a
  shipped sprite or holey skin without alpha lose the original's coverage
  (DECISIONS M31).*

## Material files (`vk_matfiles.c`)

Story 5.3: finds [MATERIALS.md](MATERIALS.md)'s files for each original
texture; `vk_material.c`'s `VK_ApplyMaterialFiles` puts them into its
materials ([Materials](#materials-vk_materialc)).

- **The index:** every file under `textures/` in the search path, listed
  once: `quakefs.c`'s `FS_ListSearchPath` (under `HEXENLICHT`, the only
  code that sees the paks) gives each pak entry with the prefix and each
  game folder, whose `textures` folder is walked (`FindFirstFileA`,
  recursive, 16 deep). The first occurrence of a name in the search path's
  order is kept (a later game folder first, and in a folder the loose
  files before its paks: Hexen II's order), a loose file with its size and
  last write time. A lookup is then a hash probe instead of a search
  through the path (up to 7 kinds of file per texture, each under two
  names and four extensions, each search a file system call per game
  folder). Built at the first lookup after a map load (`VK_LoadWorld`
  calls `VK_MaterialFilesNewMap`) or `r_reloadmaterials`: 96 loose files
  in under a millisecond. Pak entries with capitals (`Textures/` too:
  the prefix is matched in any case), `*` or `?` are left out and
  reported (the lookup lowercases; paks compare exactly; the loader
  refuses wildcards), as are names longer than `MAX_QPATH`.
- **Sets:** one per original texture slot (world textures, skins, the
  stone and ice pictures, sprite frames): the slot's spec name
  (`VK_TextureName` lowercased, `*` as `#`, a picture's `.lmp` cut),
  qualified with `~<crc>` (`VK_TextureCRC`); each map and the `.mat`
  looked up on its own (the qualified name, then the plain one; `.png`,
  `.tga`, `.dds`, `.ktx2`), the roughness and metallic from the first name
  with any of `_orm`, `_r`, `_m` (`_orm` first; an `_orm` beside `_r` or
  `_m` is reported). `_r` and `_m` (PNG or TGA: others refused) are packed
  on the CPU into one RGBA8 texture, `textures/<name>_rm`: G roughness, B
  metallic, 255 where one is missing (the material's factor is then its
  value), R and A 255; of different sizes both are refused. Formats by
  the map (M6): BC5 only as a normal map, BC7 not as one (reported, left
  out). A set is resolved when first asked for (`VK_MaterialSet`: world
  materials on map load, skins when their material is made, possibly
  mid-frame, sprites on map load for the precached ones and those the
  client loads at startup, `VK_PreloadStartupSprites`, and when drawn)
  and again after a map load or a reload; one whose slot holds another
  texture now (another name or CRC: `GL_LoadTexture` replaces a slot in
  place on a cache mismatch) is made again. `r_materials 0` makes every
  set empty, so the materials are the originals'.
- **The `.mat`:** `key value` per line, `#` comments, keys and kinds in
  any case (MATERIALS.md's table): `roughness` and `metallic` 0–1,
  `bump`, `specular`, `emissive` ≥ 0 (at most a half float's 65504),
  `kind`. Reported and left out: unknown keys, bad values, a line with no
  or two values, a `kind` other than `regular` on a world texture whose
  name gives its kind (`*…`, `sky…`) or `glass` on a skin, a kind on an
  animation's later frame (`+1…`–`+9…`, `+b…`–`+j…`: a surface's
  triangles keep the first frame's, 5.5), `bump` without a normal map,
  anything on a sprite. Since 5.5 `chrome` and `glass` apply (the set's
  `kind`): a skin's instance takes it every frame ([Skins](#skins-vk_skinc)),
  the world's primitives at map load ([World](#world-vk_worldc): a kind
  is in the primitives' material IDs, glass moves them between groups).
  An empty `.mat` has no settings.
- **The sky** (5.5, [Sky](#sky-vk_skyc)): it has no texture slot (sky
  textures skip `GL_LoadTexture`), so its set is apart from the slots':
  `VK_SkyImageFile` (from `VK_LoadSky`) looks up the sky texture's name
  qualified with the CRC of its 256x128 pixels (M2), then plain, and
  reads the file into memory (not uploaded: `vk_sky.c` splits and edits
  its layers); only RGBA8 (PNG, TGA, uncompressed DDS and KTX2; BC7
  reported) of 2:1; `_n`, `_orm`, `_r`, `_m`, `_e` and a `.mat` for it
  are reported (the sky is unlit). It counts in the summary as a texture
  looked up; `vk_materials list` shows its file.
- **Reading:** a file's bytes are read here, not through `quakefs.c`,
  which ends the game on a file it can't open again or read to its end
  (`Sys_Error`): a loose file that an editor is still writing has a
  stale size in its folder or can't be opened. So a loose file is read
  from its game folder at its size now, a pak entry at its position in
  the pak (`FS_ListSearchPath` gives it; paks don't change while the
  game runs), and a file that can't be opened or read to its end is
  refused ("being written? `r_reloadmaterials` again");
  `VK_ParseImageFile` parses the bytes.
- **Images:** a texture slot per file read (`VK_ParseImageFile`,
  `VK_LoadImageTexture` with `TEX_MIPMAP`: repeating, trilinear, mips
  made for an uncompressed image), named by the file; kept with its
  identity (game folder or pak, position, size, write time) and whether
  it has coverage (`VK_ImageFileHasAlpha`). A set resolved again reuses an
  unchanged file and reads a changed one into its slot again (the slot
  number stays: the material table needn't change for it); a refused one
  isn't retried until it changes. A new slot is taken only while 256 of
  the 4096 stay free (`TEXTURE_RESERVE`: `GL_LoadTexture` ends the game
  when the cache is full); the rest are refused as problems. A file
  deleted, or renamed to another extension, keeps its slot until the
  purge. Purged with the map's textures:
  `D_ClearOpenGLTextures` calls `VK_MaterialFilesPurged`, which drops the
  sets and the images in the purged slots (the replacement slots are made
  during a map, above `gl_texlevel`; the pictures and sprites loaded at
  startup keep their slots, their sets are made again on the next map's
  load). With `gl_purge_maptex 0` the maps' images stay.
- **Never printed while resolving:** problems (refused files and why,
  maps that don't apply, `.mat` lines) are collected per set and for the
  index (`vk_materials problems`); `R_NewMap` prints one line after the
  models (`VK_ReportMaterialFiles`) when there are files or problems:
  textures with files, files read and their time (since 5.6 with the
  decoding's share, `loads.decode_ms`: what threads would share), the
  images kept and their video memory, problems.
- **`r_reloadmaterials`**: `vkDeviceWaitIdle`, the index again, every set
  resolved again (reading what is new or changed, a deleted file's next
  candidate; with `r_materials 0` when it is 1 again), `VK_ReapplyMaterials` (the materials and the table; no
  geometry is rebuilt: primitives reference materials by index), a line
  with what it read and the time (the test set's 22 textures: 2 files
  changed 8 ms in Release, 21–27 ms in Debug; nothing changed 7 and 19
  ms); 5.5: the lava's lights' colors (`VK_LavaFileColors`,
  `VK_RebuildLights`), the sky's layers (`VK_ReloadSkyFile`), and a line
  when a world texture's kind changed (`VK_WorldKindsChanged`: "applies
  at the next map load"). **`r_materials 0/1`** (1, not archived): 0 shows the
  original textures only, for A/B comparisons; a change applies the
  materials again, the same way (a world texture's kind at the next map
  load). **`vk_materials [list|problems]`**: `r_materials`, the
  index (files, sources, time), textures looked up and with files, images
  and their memory, problems; `list` each texture with files (its maps:
  the file, size, kind, format, alpha; refused or left out; the `.mat`'s
  settings); `problems` the problems.
- **`vk_materials here`** (5.6, for authors): the texture at the view's
  center, the nearest hit of the primary rays' instances
  (`VK_ProbeView`, [Acceleration structures](#acceleration-structures-vk_accelc)):
  what was hit and how far, the material's name, the file names that
  apply to it (`<name>~<crc>` its pixels only, `<name>` every texture of
  the name), its set's files as `list` prints them (or "none found"), its
  problems' count, and its albedo's mean in linear light (both curves'
  `r_srgb` one) and luminance against the original's, with the ratio
  (`VK_TextureAverages` without the lights' bias: the shown texture as
  the GPU samples it, a BC7 file too); the sky its name and file. A world
  or brush entity triangle's material is the frame its animation shows now
  (`VK_WorldMaterialNow`), a model's its skin's (the instance's). Every
  candidate is confirmed, so the nearest wins: a masked skin's hole
  counts, glass and water too; the effects (sprites) aren't in that TLAS.
  The albedo's mean is over every texel, a skin's holes' colors too.
- **Measured** (5.3, `material_set.ps1`, `material_check.ps1`, TESTING.md
  "Materials"): 26 checks at demo1's start, the Paladin's gauntlet and the
  player, the Crusader's ice mace, its hits (sprites) and a file held open
  half written during a reload (refused, then read), meso9's lava and
  castle4's torch (with `r_emissive_models 0` too) pass in Debug and
  Release (Debug validation clean); the search order (a
  loose file before its folder's pak, `portals` before `data1`, a pak's
  capitals reported); map load in Release with all 96 of demo1's world
  textures replaced at 512x512: noise PNGs 700–960 ms, BC7 DDS 58–63 ms
  (the world's build 53–56 ms without files); decoding PNGs on threads
  is left to 5.6's test pack. *5.6 (`test_pack.ps1 -LoadSet`, TESTING.md
  "Test pack"): demo1's 96 world textures with albedo, `_n` and `_orm`
  (288 files), warm: at 4x the originals 533 ms as PNG (379 decoding, 164
  MB kept), 90 ms as BC7/BC5 DDS (41 MB); at 8x 2.15–2.22 s as PNG
  (1.74–1.77 s decoding, 656 MB), 160–180 ms as DDS (164 MB); the first
  load after the PNGs were written takes 1.5 s longer (not the decoding;
  not looked into, possibly the virus scanner's first look at new files).
  Decoding is 70–80 % of a PNG map load and the upload (a submit and wait
  per image) most of the rest: story 5.7 (DECISIONS M30).*

## Texture export (`vk_export.c`)

Story 5.4 (DECISIONS M19–M21): **`r_exporttextures [folder]`** writes
every original texture as a PNG under its [MATERIALS.md](MATERIALS.md)
name, exactly as the engine uploads it, and a manifest, `textures.csv`:
the starting points for authors and their tools.

- **What:** the world textures of every map (`maps/*.bsp`, BSP 29 and
  BSP2: mip 0, the bytes after the miptex header as `gl_model.c` copies
  them; the sky whole, 256x128, its layers' layout, the file 5.5 reads), the skins of
  every model (`*.mdl` in `models/` and `gfx/`, both formats:
  `IDPO`/`mdl_t` and `RAPO`/`newmdl_t`, dispatched by the header as
  `Mod_LoadModel`), the frames of every sprite (`*.spr`, `_<frame>`, a
  group's `_<frame × 100 + i>`) and `gfx/skin100.lmp` to `skin255.lmp`
  (`vk_skin.c`'s pictures: stone and ice). **Every
  occurrence in the search path**, not only the one the game loads:
  `FS_ListSearchPath` (5.3) gives each pak's entries and each game
  folder, whose `maps/`, `models/` and `gfx/` are walked. The same name
  (MATERIALS.md's: lowercased, `*` as `#`) and pixels in several files is
  one file; a name with more than one set of pixels gets `~<crc>` on
  every variant, the others are plain (a CRC two different images of a
  name share is reported, the second left out). With `-portals` the
  search path has both games; without it data1 only, and the first line
  says so.
- **Exactly as uploaded:** the 8-bit pixels and the CRC as `gl_model.c`
  passes them to `GL_LoadTexture`: a model's first skin after
  `Mod_FloodFillSkin` (copied here: static in `gl_model.c`; it fills the
  first skin's memory before every skin, so skins 1 and up are raw); the
  colors by `vk_texture.c`'s conversion (`VK_Convert8Pixels`, the
  upload's `VK_Convert8`) with the texture's flags: world `TEX_MIPMAP`,
  skins `VK_SkinTextureMode` (`vk_skin.c`: holey, transparent,
  special-trans), sprites `TEX_MIPMAP | TEX_ALPHA`, pictures `TEX_ALPHA |
  TEX_NEAREST` (`GL_LoadPicTexture`). RGBA where the conversion set
  `TEX_ALPHA` and a texel's alpha is below 255 (holey, transparent and
  special-trans skins, sprites with transparent texels), else RGB: the
  world's and plain skins' alpha (index 255's 0) isn't read. The sky
  (5.5) as `vk_sky.c` uploads its front layer: its left half transparent
  at color 0 and 255 (4 of the 5 skies; egypt's has no transparent
  texels), which a material file's alpha replaces. PNGs by
  `stb_image_write` (`libs/stb`, `STBI_WRITE_NO_STDIO`: written through
  `stbi_write_png_to_func` into files opened here), no gamma chunk.
- **Where:** `<game folder>/<folder>/textures/…` and
  `<game folder>/<folder>/textures.csv` (`FS_USERDIR`; `export` by
  default: `data1\export`, `portals\export` with `-portals`); the name
  takes letters, digits, `_` and `-`, and `textures` is refused (never
  the folder the engine reads). A new export overwrites an earlier one's
  files; files it doesn't make stay (delete the folder for a clean one).
- **`textures.csv`**, a row per file, sorted by name: `file`
  (`textures/rtex343~ad81.png`), `name` (the engine's, as `vk_textures
  list` prints it: `*lava1`, `models/ball.mdl_0`, `gfx/skin100.lmp`),
  `crc`, `width`, `height`, `kind` (`world`, `liquid`, `sky`, `skin`,
  `sprite`, `picture`), `alpha` (`none`, `coverage`: 0 or 255,
  `translucent`), `variants` (of the name), `used in` (the maps, else the
  model, sprite or picture file; space separated), `from` (the paks or
  game folders, relative to the base folder: `data1/pak1.pak`, `portals`).
- **Nothing of the game changes:** the files are read here (`fopen` of
  the pak at the entry's position, or of the loose file), not through
  `quakefs.c` or the model cache; no GPU work, no texture slots. A
  console command, outside frames: it prints what it read (per kind of
  file and the time), a line per kind of texture (files, names, names
  with variants, files with alpha), the totals (files, MB, time, folder)
  and the problems (the first 40): a file that can't be read or is cut
  short, a format or version the engine doesn't take, a size it ends the
  game on (or over 8192), a name Windows can't take (a character, a
  device name like `con`, a folder name ending in `.` or a space), a name
  longer than 40 characters (`vk_matfiles.c`'s index takes relative paths
  below 64 characters, and `textures/<name>~<crc>_orm.ktx2` adds 23) or
  with a `~` (the qualifier's), a 16-character miptex name without its
  end (`GL_LoadTexture` reads on into the width), a CRC collision, no
  memory for an image (a file can claim 8192x8192 texels many times).
- **Measured** (5.4, TESTING.md "Texture export"): with `-portals` 2110
  files, 24.2 MB (870 names of world, liquid and sky textures, 100 with
  variants, as M2 and `tex_names.ps1`; 553 skin names in 555 files,
  `ball.mdl`'s and `scepter.mdl`'s first with two variants; 573 sprite
  frames, 2 pictures;
  629 with alpha), 3.2–3.4 s in Release, 8.6 s in Debug; data1 alone 1430
  files. The CRCs equal the texture cache's for all 3183 textures
  `vk_textures list` shows on demo1, egypt1, egypt5, tibet8 and keep1;
  `export_check.ps1` finds no difference in any file or row against its
  own reading of the paks, decoded by texconv; with the export in
  `textures\` the 3D view is identical to the view without files.

## 2D (`vk_draw.c`)

- Port of `gl_draw.c`'s `Draw_*`: quads go into a per-frame host-visible
  vertex buffer, drawn with one indexed draw in `GL_EndRendering`. The screen
  layout is upstream's `gl_screen.c` (reused; it calls our
  `GL_BeginRendering`/`GL_Set2D`/`GL_EndRendering`).
- Quads are alpha-tested (GL_GREATER 0.632, no blend) or blended per quad;
  the shader works in the 8-bit colors (the UNORM textures as they are,
  times the vertex color) and applies the `gamma` cvar.
- **`SCR_UpdateScreen` re-enters via `Con_Printf`** — only the outermost level
  records a frame (`draw_depth`). Never print to the console between
  `VK_BeginFrame` and the end of `VK_EndFrame`'s state update; count problems
  and report them from console commands.

## Scene (`r_scene.c`)

- `R_RenderView` does the renderer-independent frame setup (`R_AnimateLight`,
  `r_origin`/`vpn`/`vright`/`vup` — the client's sound and effects read them —
  view leaf, `V_CalcBlend`) and fills the global `r_scene`: camera, entities
  (`cl_visedicts`, all static entities, the view model; not culled to the
  view), active dlights, light style values, the active particle list, the
  beams (6.3) and the view blend. The 3D renderer reads only `r_scene`;
  `r_dumpscene` prints it.
- **Beams** (6.3): `cl_tent.c`'s `CL_UpdateTEnts` hands over the streams it
  draws each frame (`R_ClearBeams`, then `R_AddBeam` per stream after its
  source follows its entity: type, skin, source, dest, end time, its four
  models), a guarded upstream hot spot ([UPSTREAM.md](UPSTREAM.md));
  `R_BuildScene` copies them into `r_scene.beams` (none with
  `r_drawentities 0`). The segments themselves are temporary entities as
  before. See [Beams](#beams-vk_beamlightc).
- The order in `R_RenderView`: `R_SetupFrame` → `R_ViewModelLight` →
  `R_BuildScene` (fills `r_scene`) → `VK_UpdateInstances` →
  `VK_UpdateModelGeometry` → `VK_UpdateEffects` → `VK_BuildTLAS` →
  `VK_RenderView3D` → `VK_DrawLightEditor` → `VK_DrawImageFile` (these two
  in the 2D, under the HUD).
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
  (`MATERIAL_UINTS` 8, `vertex_buffer.h`): Q2RTX's 6 uints, Hexen II's
  alternate animation (`+a..+j`) in `[6]` and (5.3) the roughness and
  metallic texture with the `MATERIAL_NORMALS_BC5` flag in `[7]`; `[2].y`
  and `[3].x` are glTF's roughness and metallic (the value without that
  texture, a factor on it; Q2RTX's `roughness_override` was a floor over
  the albedo's alpha).
- A material is what it shows (5.3): its original texture slot
  (`texture`, whose [material files](#material-files-vk_matfilesc) apply),
  the slot it shows without files (`original`: the texture, or a player's
  translated skin) and flags (`VK_MAT_SKIN`, `_CUTOUT`, `_TRANSLATED`,
  `_LAVA`, `_FLAME`); since 5.5 also the files' `kind` (`MATKIND_*`, not
  in the GPU table: a skin's instance takes it every frame, the world's
  primitives at map load).
  `VK_ApplyMaterialFiles` makes the rest from those and the texture's
  files, or the defaults without files, as before E5: the base texture
  (the replaced albedo, but not under a player's colors), the mask of a
  cutout skin (the albedo if it has alpha, else the original), the normal
  map and its BC5 flag, the roughness and metallic texture, the factors
  (roughness 1, metallic 0 or 1 with a metallic map, bump 1, the
  `.mat`'s), the specular (the `.mat`'s, 1 where the roughness is
  authored, else `r_specular`: 0 since 4.9, matte as GL; Q2RTX's 1 made
  the dark walls look like polished metal, see [Map
  lights](#map-lights-vk_maplightsc)) and the emission (4.5's lava: its
  albedo with `r_lava_light`, an `_e` instead; the light models' flames:
  an `_e`, else the fake emissive texture; any other lit texture: an `_e`
  or, with the `.mat`'s `emissive`, its albedo; times `r_emissive_scale`
  and the `.mat`'s `emissive`, see [Emissive
  surfaces](#emissive-surfaces-vk_emissivec)). `VK_ReapplyMaterials`
  applies every material again and uploads the table (after
  `vkDeviceWaitIdle`): `r_reloadmaterials`, `r_materials`, `r_lava_light`,
  `r_emissive_scale`; `r_specular` uploads it again.
- `vertex_buffer.h`'s `get_material_info`/`animate_material`: frame =
  `int(cl.time*5)` (`global_ubo.anim_frame`); surfaces reference their
  animation's first frame.
- **`get_material`** (`path_tracer_rgen.h`, 5.3, MATERIALS.md): the
  roughness and metallic are the material's values times the roughness
  and metallic texture's G and B where it has one, also without a normal
  map (Q2RTX: the albedo's and the normal map's alpha, with a normal map
  only); the normal map's green is flipped (OpenGL's convention: the
  tangent frame's bitangent runs down the image, `vk_world.c`'s `+vecs[1]`
  and `model_geometry.comp`'s dP/dv, which is DirectX's, as Q2RTX's maps
  are); its Z is `B × 2 − 1` (Q2RTX read B as it is) or, for BC5,
  rebuilt from X and Y (the Toksvig adjustment then has nothing to
  soften). 5.5: the bitangent from the triangle's own normal, not
  `geo_normal`, which the rays turn towards themselves: a surface seen
  from behind keeps its normal map's frame (Q2RTX mirrored the map there;
  a liquid's surface is two coincident faces, up and down, and primary
  rays, which don't cull, hit either: with a normal map they shaded
  differently, speckled). Bounce rays read only the base color, as Q2RTX's.
  `pt_roughness_override` and `pt_metallic_override` still apply.
  Measured without files: the 3D view identical to `main`'s (4 maps, lit
  and `r_debugview` 1, 2 and 10, but meso9's flying imp); with a
  roughness and metallic texture on about half of demo1's start view, the
  primary rays 0.42–0.50 ms at 1920x1080 against 0.41–0.47 without files
  and on `main` (Release, run-to-run spread of that size).

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
- **Kinds from material files** (5.5): a regular texture's `.mat` `kind`
  (its set's, resolved before the triangles are emitted: `FileKind`) makes
  its triangles Q2RTX's `MATERIAL_KIND_CHROME` (the opaque group) or
  `MATERIAL_KIND_GLASS` (the transparent group, as Q2RTX's
  `geom_transparent`: seen through; alpha 1; the PVS connected across to
  the leaf past the pane's brush, `GlassBackLeaf` stepping up to 32
  units, so the lights beyond a window are in the near side's lists;
  `vk_world` counts the triangles that found one);
  a surface takes its animation's first frame's (the triangles reference
  it, and `animate_material` keeps their kind); the turbulent and sky
  textures keep their names' kinds. At map load only: glass moves
  triangles between groups (the BLAS), as Quake II RTX builds the map's
  geometry again for a kind change; `r_reloadmaterials` reports a
  changed one (`VK_WorldKindsChanged`, the kinds the geometry was built
  with, `texture_kinds`). Brush entities keep their triangles' kind.
  *5.6: Hexen II's windows are brush entities (breakable panes of
  `rtex018`, `rtex083`, `rtex199`, `ttex210`...; only castle5 has glass on
  world faces), whose triangles don't connect the PVS (the pane is no
  wall in the BSP: nothing needs connecting); `vk_world`'s count is the
  world's.*
- Each triangle's material index stays on the CPU (`prim_materials`, 2
  bytes each, 5.6) for `vk_materials here`: `VK_WorldMaterialNow` gives the
  frame the animation shows now (`r_scene.time` x 5, `vertex_buffer.h`'s
  `anim_frame`; a brush entity's frame choosing the alternate).
- `vk_world [materials]` prints statistics (with the lava lights; the
  kinds, chrome and glass too) and checks the animation table against
  `R_TextureAnimation`; `materials` lists each material with its
  emissive texture and factor, and a kind other than regular.

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
  `MATERIAL_KIND_TRANSP_MODEL`, alpha = 0.33 for `DRF_TRANSLUCENT` but on
  `EF_SPECIAL_TRANS`, as GL; the rays multiply it by the skin's, 6.4, see
  [3D view](#3d-view-vk_viewc); a translucent cutout is here, its holes
  tested); masked (`EF_HOLEY` cutouts); light
  (4.1: opaque models at a map light's origin, `VK_MapLightAt`: the
  torches, flames, candles and the like that the light entities' game code
  spawns there, whose mesh surrounds the light; they cast no shadows, see
  [Map lights](#map-lights-vk_maplightsc); since 4.5 they show their
  skin's emissive texture, flagged `MATERIAL_FLAG_LIGHT`, see
  [Emissive surfaces](#emissive-surfaces-vk_emissivec); 4.4 and 6.2:
  the owners of a dynamic light and the glowing projectiles; 6.3: the
  opaque glowing beam parts, [Beams](#beams-vk_beamlightc)).
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
  the shader), and each instance's material against its group (a masked
  one's mask is its albedo or, 5.3, the original skin).

## Skins (`vk_skin.c`)

- `VK_SkinMaterial` is `R_DrawAliasModel`'s choice: skin >= 100 is
  `gfx/skin<n>.lmp` via `Draw_CachePic` (100 stone, 101 ice); else
  `gl_texturenum[skin][(int)(cl.time*10)&3]`, bad numbers fall back to 0 and
  are counted; players with translated colors use `player<n>` (found with
  `VK_FindTexture`) unless `gl_nocolors`.
- `VK_SkinTextureMode` is the texture mode `Mod_LoadAllSkins` gives a
  model's skins by its flags (holey, transparent, special-trans), for the
  translated player skins, `VK_ModelHasCutouts`, `VK_SkinHasAlpha` (6.4:
  any of the three) and (5.4) the texture export.
- **Chrome** (5.5): a skin whose files' `.mat` says `kind chrome` makes
  its instance Q2RTX's `MATERIAL_KIND_CHROME_MODEL` (`vk_instance.c`,
  every frame, so a reload shows it at once; a mirror only below
  roughness 0.02); a translucent entity stays Q2RTX's transparent model
  (6.4: blended); the player under its colors takes its skin's (the files of the
  skin `R_TranslatePlayerSkin` translates). `vk_instances` shows
  "chrome", `vk_models check` expects it. `glass` isn't for skins.
- One material per skin texture and use of its alpha (`VK_AddSkinMaterials` for the
  precache on map load, others on demand); where the skin's alpha matters
  (`VK_MAT_ALPHA`: `EF_HOLEY`'s holes; 6.4: `EF_TRANSPARENT`'s and
  `EF_SPECIAL_TRANS`'s opacity, which the rays blend by, see
  [3D view](#3d-view-vk_viewc)) the skin is its own `mask_texture`. A third key (4.5; `VK_SKIN_*`): emissive, for models at a map
  light's origin, a material flagged `VK_MAT_FLAME` with the skin's
  emissive texture (made on first use, `VK_EmissiveSkin`; 5.3: its own
  material also where the skin has no bright texels, so that an `_e` found
  later applies, and also with `r_emissive_models 0`, when it emits
  nothing, so that an `_e` doesn't glow on a flame whose map light lights
  for it; see [Emissive surfaces](#emissive-surfaces-vk_emissivec)); or
  glowing (6.2), flagged `VK_MAT_GLOW`, whose `_e` or albedo emits, for
  the glowing projectiles ([Effect lights](#effect-lights-vk_effectlightc))
  and (6.3) the glowing beams, also a cutout's or a transparent model's
  (`VK_GlowSkinMaterial` makes a skin's outside frames for the beams'
  powers; [Beams](#beams-vk_beamlightc)).
- **Material files** (5.3, [above](#material-files-vk_matfilesc)): a
  skin material's texture is the skin (under a player's colors the class
  model's last single skin, the one `R_TranslatePlayerSkin` translates,
  whatever the group frame or skin number), `VK_ApplyMaterialFiles` does
  the rest: a replaced albedo with
  alpha is also the mask of a skin whose alpha matters (holes, 6.4:
  opacity), one without keeps the original
  as the mask (`vk_models check` accepts either); the stone and ice
  pictures are named `gfx/skin100` and `101` (unnamed slots: the set takes
  the picture's name).
- `R_TranslatePlayerSkin` is `gl_rmisc.c`'s per-class translation
  (`gfx/player.lmp`, `color_offsets`) but translates the 8-bit skin and loads
  it with the model's texture mode (so the Demoness keeps her cutouts, which
  GL loses) at native size with mips. 5.3: it notes whether the
  translation changed any texel (top and bottom color 0 don't: an identity
  table); such a player shows the skin's replaced albedo, the others their
  translated original (`VK_MAT_TRANSLATED`), both the other maps
  (MATERIALS.md). When a player's colors or class change, the material
  made for them before is found again (`VK_FindMaterial`: the same
  texture, shown slot and flags) or one is made.

## Effects (`vk_effects.c`)

- `VK_UpdateEffects` writes the scene's particles and sprite entities as
  triangles on the CPU into a mapped buffer per frame in flight, laid out like
  Q2RTX's `transparency.c` (`hl_shared.h`): positions (3 per particle, then 4
  per sprite quad sharing a static uint16 index buffer 0 1 2, 2 3 0), then an
  `EffectParticle` (linear color, half alpha | GL's `ptex_coord` set) per
  particle and an `EffectSprite` (texture slot, alpha, original slot,
  coverage) per sprite;
  `MAX_EFFECT_PARTICLES`/`MAX_EFFECT_SPRITES`, the excess is counted.
- **A sprite frame's replaced albedo** (5.3, `VK_SpriteTexture`; the
  precached sprites' frames are looked up on map load): the texture
  shown, with the original's slot, whose size sets the mip level (GL's
  texel per unit: `pt_logic_sprite` adds the log2 of the sizes' ratio),
  and, where the albedo has no alpha, whose alpha is the coverage (read in
  the shader: the original's 8-bit pixels aren't kept and a BC7 image
  couldn't take them). Sprites have no material: their other maps and
  `.mat` are reported and left out.
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
- `vk_effects [check]`: counts, drops, BLAS/TLAS sizes, the effect lights
  ([below](#effect-lights-vk_effectlightc)); `check` casts a ray at
  every effect triangle of the last frame through the effects TLAS
  (`effects_check.comp`) and compares where it is reported.

## Effect lights (`vk_effectlight.c`)

Story 6.2: effects that glow emit light. GL gives no sprite a light (only
`TE_EXPLOSION`, the projectiles' model flags and the `EF_*` effects make
dynamic lights). `r_effect_lights 1` (not archived; 0 = GL's look, the
image of 6.1's `main`) turns on both parts. The lights are the
renderer's, not `cl_dlights`, so `cl.light_level` stays GL's (G9).

- **Emitting sprites:** by name, the client effects' explosions
  (`sm_expld`, `bg_expld`, `fl_expld`, `gen_expl`, `xbowexpl`,
  `xpspblue`, `mm_expld`, `bonexpld`, `fcircle`, `xplod29`, `biggy`,
  `flrexpl2`), flashes (`sm_white`, `gryspt`, `yr_flsh`, `bluflash`,
  `sm_blue`, `redspt`), sparks and magic hits (`bspark`, `spark`,
  `rspark`, `gspark`, `medhit`, `mezzoref`) and Praevus's fire
  (`flamestr`, `firewal1`–`5`, `fboom`, `pow`, `xplsn_1`, `axplsn_1`,
  `_2`, `_5`, `Bluexp3`, `muzzle1`); not smoke, clouds, ghosts,
  bubbles, the teleport's puffs or the ice mace's snow hit (`icehit`).
  A server entity showing one emits too, but not one that owns a
  dynamic light (it has its light; with `r_dlights 0` it owns no lit one
  and emits).
- **The light of a frame** (`VK_SpriteLight`, from `vk_effects.c`'s
  `WriteSprites`, which draws it): a sphere at the center of the quad as
  drawn, of the frame's covered area (the quad's area times its mean
  alpha: π r²), whose intensity is the light the frame shows towards the
  camera (its premultiplied mean linear color times the quad's area,
  times 0.33 when translucent) times `r_emissive_scale` (32), the
  radiance lava and the flames emit for a texture color of 1 ([Emissive
  surfaces](#emissive-surfaces-vk_emissivec)). The sprite itself stays at
  GL's colors (R41): as a picture of a brighter fire (not physically
  based there; the light's transport is: inverse square, shadows,
  bounces). The frames darken as an effect ends, so its light fades with
  them. The UBO sphere's color (π × radiance) is the intensity over r².
  Its range (the dynamic lights' range fade) is where it gives a white
  wall facing it 1/32 of a full GL texel's light, √(32 I / π) for the
  intensity's luminance (GL's own dynamic lights stop at about a third).
  Not scaled by the auto exposure (`tm_auto_exposure 1`), as the lava.
- **The budget** (`VK_ChooseEffectLights`, from `VK_PrepareLights`,
  inside the frame): the UBO's 32 dynamic spheres hold the game's lights,
  the test lights, then as many effect lights as fit before the dark
  ones, the largest luminance / distance² at the camera first; the rest
  are counted. A chosen sphere shrinks (its intensity kept) to where 14
  rays from its center (the axes and the diagonals), marched in 4-unit
  steps, meet none of the world's solid (`Mod_PointInLeaf`), at least 2
  units: a sphere at a wall doesn't reach through it (brush entities,
  doors, aren't seen). A center in solid (a hit's spark at the wall it
  hit) moves towards the camera, up to 4 units, or the light is left out.
- **The frames' averages** (`VK_SpriteAverages`, `texture_average.comp`
  premultiplied: each texel's linear color times its alpha, the mean
  alpha in w): of the texture the frame shows (5.3's replaced albedo,
  with the original's coverage where it has no alpha), by both color
  curves (`r_srgb`). On the GPU outside frames: at map load for every
  frame texture of an emitting sprite (`VK_SpriteLightAverages`, from
  `VK_LoadModels`: the precached sprites and the client's own, loaded at
  startup; 221 on demo1 in 28 ms, Debug) and after the material files
  change (`r_reloadmaterials`); a frame first drawn later (a sprite loaded
  mid-game) has no light that frame and gets its average before the next
  (`VK_EffectLightsBetweenFrames`, in `VK_BeginFrame`: the pipeline is
  made for each batch and the upload waits for the GPU, a short hitch;
  none of the games' sprites needs it). A texture purge (a map change)
  drops them.
- **Glowing projectiles** (`vk_instance.c`'s `GlowKey`, with
  `r_emissive_scale` above 0): an alias entity that owns a dynamic light
  within its bounds ([Lights](#lights-vk_lightc)), or, without one, carries
  one within 8 units of its origin keyed to a translucent part drawn with
  it, not a player (a part the gamecode chains to it: the scarab's body,
  whose translucent wings `scrbpwng` carry its `EF_SCARAB` light 6 units
  off; `vk_light.c`'s `VK_GlowLight`; a light that another glows around
  first isn't claimed again), whose
  light isn't a muzzle flash (the only lights with a `minlight`: they
  light what fires them), not a player (the torch, invincibility), a
  stepping monster or a model over 64 units (its MDL radius, scaled:
  burning monsters in Praevus, the Riders' deaths, Praevus himself;
  projectiles are 3–60); it joins the light group: the scarab, the
  summoning stone, the tomed purifier's ball, fireballs and the like; not
  cutout or translucent ones, which aren't in the (opaque) light group:
  the Eidolon's `glowball`, `lball`, the vorpal missile (the magic
  missiles' `ball.mdl` glows). Its skin's material emits the whole skin
  (`VK_MAT_GLOW`: its `_e`, else its albedo; [Skins](#skins-vk_skinc))
  times `r_emissive_scale` and GL's light level where it has one
  (abslight, power mode: the flames' rule, M17), flagged
  `MATERIAL_FLAG_LIGHT` (its dynamic light is its light: diffuse bounces
  don't add the emission; 6.3: nor do beam lights light it). Its light doesn't light it: the owner's
  instance + 1 is in the UBO sphere's `type` high 16 bits
  (`VK_GlowingInstance`), and `dynlight_weight` gives it no weight for
  that instance's surfaces, in direct light (the visibility buffer's
  instance) and on bounces (the hit's). It casts no shadows while it
  glows (the light group, R81: a shadow ray per light would have to skip
  its owner). `vk_models` counts them.
- **Cost** (Release, 1920x1080): the Demoness's fire storm (up to 19
  effect lights in a frame) 4.41 → 4.63 ms a frame over the 30 frames
  after a burst; the blast radius's seven flashes 4.25
  → 4.33 ms a frame over the effect's frames; the 32 dynamic slots full
  (the cap's upper bound) 4.16 → 4.95 ms (direct light 0.40 → 0.79,
  bounce 0.65 → 1.04): the dynamic lights' pick runs over every one,
  twice per sample.
- `vk_lights` and `vk_effects` print the last frame's emitting sprites,
  the effect lights and those left out (no slot, in solid, no average
  yet, owning a dynamic light), the most in a frame and the brightest
  since the map loaded, and the frame averages made; since 6.3 also the
  beams' ([Beams](#beams-vk_beamlightc)).
- **Beams' lights** (6.3) join the same budget: a **line light** per
  glowing beam (`VK_BeamLineLight`: its power per unit length, ranked by
  its whole intensity across it, power / π × its length, over the
  distance² to its nearest point) and a sphere at the sunstaff's hit
  (`VK_BeamEndLight`, as a sprite's). A line's UBO color is its power over
  2 r (a cylinder of radiance L and radius r gives L 2πr per unit length),
  its range where a white wall beside a long one gets 1/32 of a full texel
  (power / (2π h)), its ends pulled in by a unit (the game's traces stop
  them in open space: no solid test). `r_effect_lights 0` turns them off.

## Beams (`vk_beamlight.c`)

Story 6.3: beams glow and light the scene. Hexen II's beams are the
client's streams (`cl_tent.c`): every frame `CL_UpdateTEnts` draws a
model segment every 30 units from a stream's source to its dest (the
sunstaff two: its core and a translucent sheath) and two balls at the
sunstaff's dest, all at GL's fixed light level (abslight 128, about half);
GL gives them no light. [Scene](#scene-r_scenec) gets the streams
(`r_scene.beams`).

- **Glow:** the beams of light's models, chosen by name as 6.2's sprites
  (`stsunsf1`–`5`, `stlghtng`, `stltng2`, `stclrbm`, `stmedgaz`,
  `fambeam`; `VK_BeamGlows`), emit their skin as the glowing projectiles
  do (`VK_SKIN_GLOW`: the `_e`, else the albedo, × `r_emissive_scale` ×
  GL's light level: ×16), with `r_effect_lights` and `r_emissive_scale`
  above 0. `vk_instance.c` puts the opaque ones in the light group (no
  shadows); translucent ones (the sheath, the hit's glow, the lightning's
  last 0.25 s, the color beam) stay transparent models, blended and
  glowing at their opacity since 6.4;
  the gaze stays a cutout. All but the gaze are flagged
  `MATERIAL_FLAG_LIGHT`: their line light (and the hit's sphere) is their
  light, which doesn't light what is flagged a light
  (`DYNLIGHT_NOT_ON_LIGHTS`, `light_lists.h`'s `dynlight_weight`; found by
  6.3's review: the hit's sphere lit its own glowing balls), and
  diffuse bounces don't add their emission. The gaze has no line light (a
  cutout casts shadows, the masked group's alpha test, so its own geometry
  would block it): it glows and lights through bounces only. The chain
  and the ice chunks aren't light: lit by the world. `vk_models` counts
  the glowing beam parts.
- **Light:** each beam of light (sunstaff, lightning, the color beam,
  Famine's) is one line light from its source to its dest; the sunstaff's
  hit a sphere at the dest. Its power is what its glowing surface shows:
  per segment, the sum over its triangles of the area (the mean over the
  poses) times the mean emitted color of the texels it covers; times the
  alpha (0.33 translucent; the color beam's texture's), GL's light level
  and the material's emissive factor (`r_emissive_scale` × the `.mat`'s
  `emission`), per 30 units. The cylinder's radius is the segments' widest
  vertex from their axis (its near field is capped there; the light
  itself doesn't depend on it). The hit's balls at their mean scale (GL's
  80–95 % and 150–165 %), a sphere showing that radiance (intensity:
  power / 4). The lightning fades with GL's level after its end
  (128 − 192 × the time past it, as an int) at 0.33. The areas are the
  mean over the poses: the lightning draws its six frames at random, the
  never-sent sunstaff2 its eight in turn, the other light models have one.
  A stream that runs out of GL's 128 segment entities keeps its whole
  light (`R_AddBeam` comes before its segments); the streams after it
  aren't drawn and get none.
- **The powers** (`MakePowers`): `vk_model.c`'s `VK_AliasTriangleAreas`
  gives each triangle's texture coordinates and mean area, the glow
  material (`VK_GlowSkinMaterial`, made if needed) its emissive texture,
  `texture_average.comp`'s triangle mode (`VK_TriangleAverages`) the mean
  of the texels whose centers are inside each triangle (the centroid's
  sample for a sliver; premultiplied by the texture's alpha for a
  transparent model, by the mask for a cutout) by both color curves.
  Outside frames: at map load for the precached beam models' skins
  (`VK_BeamLightAverages`, 12 on demo1 in 7 ms Release, 20 ms Debug),
  after the material files change, and between frames for a model first
  drawn later (no light that frame). Measured from the paks (6.3's
  proposal): a white wall 64 units from a long sunbeam gets 0.31 of a full
  texel, from the lightning 0.23, Famine's 0.11 (red), the red color beam
  0.06 (0.28 in red), the white one 0.40; the sunstaff's hit 0.13.
- **Check** (`vk_testlight line`, [Lights](#lights-vk_lightc)): below
  the middle of a 248-unit test line of power 100, 64 units above demo1's
  floor, the direct light (`r_debugview 15`) is 0.2367 against the
  cylinder's analytic 0.2378; 31 test spheres of the same power along it
  0.2212 against their 0.2210.
- **Cost** (Release, 1920x1080, a paused frame of the tomed sunstaff's
  six beams with GL's 128 segments, six lines and six hit spheres):
  4.67–4.69 → 4.98–4.99 ms a frame (direct light 0.39 → 0.53, bounce
  0.81–0.82 → 0.93–0.94), each twice.
- `vk_lights` and `vk_effects` print the last frame's beams, lines and
  hits offered and lit, the parts without a power yet, and the powers
  made.

## Acceleration structures (`vk_accel.c`)

- `VK_LoadWorld` ends with `VK_BuildWorldAccel`: one static BLAS per non-empty
  range of the world and every submodel, from the packed positions, one batch.
- `VK_BuildTLAS` records into the frame's command buffer:
  - dynamic BLASes over the instanced buffer's model triangles, one per model
    group (fast build, rebuilt every frame, created with room to grow like
    Q2RTX's, per frame in flight); the masked group's TLAS instance is
    `FORCE_NO_OPAQUE` so its hits are candidates alpha-tested against the
    material's `mask_texture`, and so is the weapon's when it has cutouts;
    6.4: so is the transparent group's (and the weapon's when it looks
    transparent), where only clear texels are no hit (alpha below
    `TRANSP_MODEL_MIN_ALPHA`, 0.02: `EF_TRANSPARENT`'s color 0, holes; the
    rays blend the rest by it), still culled (`FORCE_NO_OPAQUE` alone: GL
    culls all but `EF_SPECIAL_TRANS`, and a reflection or refraction ray
    through a translucent model meets the world behind it, not its own
    far side, which would take a pass)
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
    cutouts and, 6.4, transparent models) and Q2RTX's second, effects-only TLAS (`VK_EffectsTLASAddress`,
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
  `ray_probe.comp`, its instance mask pushed since 5.6: 0xff here; the
  first 32 are those found first, in traversal order, then sorted;
  `VK_ProbeView` takes the primary rays' mask, `path_tracer_rgen.h`'s
  `PRIMARY_RAY_CULL_MASK`, along the view's center with `nearest`, every
  candidate confirmed so the committed hit is the nearest, and its
  material for `vk_materials here`); `vk_rtcheck` (a 64x48 ray grid from the camera compared
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
  texture slot, `anim_frame`, `debug_view`, `view_cluster`, and the sky's
  fields (4.6: its textures, scroll, `r_skyalpha`, the dome; the sun uses
  Q2RTX's `sun_*` fields), `maplight_gamma` (4.15, `r_maplight_gamma`); our
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
  `pt_logic_sprite` with GL's look; no Q2RTX beams or explosions: Hexen
  II's beams are alias models, 6.3, its explosions sprites),
  `path_tracer_rgen.h` (3.2: the passes' common code — `trace_geometry_ray`,
  `trace_effects_ray`, `get_material`, `get_rng`, `env_map` — with the TLASes
  by device address; `env_map` is Hexen II's sky and the sky light's dome
  since 4.6 ([Sky](#sky-vk_skyc)); `get_material` tints a
  model's base color with its `colorshade` hue; 3.3: shadow and caustic rays
  and `get_direct_illumination`, since 3.4 with the light statistics per
  list entry; 4.6: `get_sunlight`, its shadow ray ending at the first sky
  face (`trace_sky_distance`); 3.6: Q2RTX's `get_is_gradient`, the
  denoiser's gradient samples),
  `light_lists.h` (3.3: Q2RTX's polygon and sphere light sampling; 3.4:
  spheres in the light lists, the statistics per list entry; a list's
  current light count instead of Q2RTX's light-count history, which only
  lists that change every frame need, no sky lights: 4.6's sky light is a
  dome that bounce rays gather),
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
  formats, 89 of them the denoiser's; 1201 MB allocated at 2560x1440 (3.8: +120; 4.13: +9 bytes per pixel, ~32 MB, for 32-bit barycentrics and RGBA16F gradients). New
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
  Q2RTX's without its material table, light styles and cluster debug mask;
  with its sky visibility since 4.6; one host-visible buffer per frame in flight,
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
    the cone. A map light's sphere has a **light shape** (4.15, the fourth
    vec4's w, `SPHERE_SHAPE_*`; `r_maplight_shape`, see
    [Map lights](#map-lights-vk_maplightsc)); test spheres the physical one
    (0) above. Shape 1 has utils/light's angle term in linear light,
    `(0.5 + 0.5 cos)^2.2` (`lightmap_angle_term`; the cosine from −1 with
    shading normals, so one facing away gets none), instead of the cosine;
    shape 2 ("Original") gives the lightmap texel utils/light and GL made
    of the light alone (`lightmap_light_value`): `(level − d)(0.5 + 0.5
    cos)` from the center (nothing with the center behind the surface's
    plane), over 247.27 (the compiler's `rangescale` 0.5 and GL's 264 >>
    7, so 1 is a full texel), clipped at the compiler's byte (2.0625: 255 × 264 >> 7),
    times the style, clipped at 1, to the power `global_ubo.maplight_gamma`
    (`r_maplight_gamma` 2.2: GL multiplied the texture by it in sRGB
    space), times the color (a full texel's light, not a radiance) and
    the light list entry's factor (4.16: GL's sum of overlapping lights,
    `list_entry_factor`, [Light fit](#light-fit-vk_lightfitc)); its range
    is the level, its CDF weight π × that light (as
    a solid angle weighs a radiance), the sphere is only the shadow ray's
    target (a point of it below the surface's horizon is the center: the
    compiler's plane test), and a bounce's solid-angle limit doesn't apply
    (the light is bounded). Shapes 1 and 2 bring their own angle term, so
    `get_direct_illumination`'s diffuse term leaves out the cosine
    (`light_angle`) and the specular takes their light divided by the term.
    A gradient sample's style change (4.13) is the change of shape 2's
    light, the style being inside its clip and power (castle5's pulse:
    denoised / raw 0.90–1.09 throughout; shape 0 falls up to 1.77 behind).

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
  `light_affects_cluster`) and clusters beyond a sphere's range. An entry
  is the light's index in its low 16 bits and, for a sphere of GL's shape,
  its factor in that cluster as a half float in the high 16 (4.16,
  `ListEntry`: [Light fit](#light-fit-vk_lightfitc); the shader reads it with
  the index, nothing more per candidate). A
  cluster's bounds (`VK_LoadLightClusters`, after the PVS is final) are its
  leaf's and those of its world triangles (Q2RTX: its opaque triangles');
  models and brush entities take the cluster of their center, so their
  parts beyond a cluster's bounds can miss a light near the end of its
  range, where the window has taken it almost to 0. Lights touching no
  open leaf (inside solid) are in no list; a light that doesn't fit into
  `MAX_LIGHT_LIST_NODES` is left out whole (both counted by `vk_lights`).
  Each frame in flight's buffer copies the lists when their version
  changed (and `vk_sky.c`'s sky visibility, the sun's clusters, with a new
  map, 4.6); the lights are written every frame, with their light style (4.2,
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
  game's (4.4, below), then the test ones, then the effects' in the slots
  left (6.2, [Effect lights](#effect-lights-vk_effectlightc)). A sphere's
  `type` high 16 bits can name a model instance it doesn't light (its
  index + 1: a glowing projectile's light). A pixel picks one by its weight
  (`dynlight_weight`: luminance × solid angle, faded by its range, 0
  entirely below the horizon; Q2RTX picks uniformly), two passes over them;
  a sphere's range is in the entry's `spot_data` (a float's bits, 0 =
  unlimited; `dynlight_range`), where its light fades as a list sphere's
  (`sphere_light_window`). Spot lights come with the code, unused.
  **Line lights** (6.3, `DYNLIGHT_LINE`, a beam's, `dynlight_line`): a thin
  cylinder of the entry's radius around the segment from `center` to
  `center + spot_direction` (no layout change), of uniform radiance (the
  color is π × it, as a sphere's). Seen from a point at the distance h
  from its axis (at least the radius: its near field is capped there), a
  piece dl at the distance d and the angle φ from the axis has the solid
  angle 2r sin φ dl / d² = (2r / h) d(sin θ), θ the angle from the point's
  foot on the axis; so the whole cylinder's is (2r / h)(sin θ_b − sin θ_a)
  between its ends, its weight in the pick as a sphere's, and a point
  sampled uniformly in sin θ has the pdf of its solid angle: an exact
  importance sample. The shadow ray goes to that point on the axis (in
  open space where the beam is drawn); the range fades with the distance
  to the segment. The entry's `type` holds the type in its low 8 bits
  (`DYNLIGHT_TYPE_MASK`); a beam's lights (a line, the sunstaff's hit
  sphere) carry `DYNLIGHT_NOT_ON_LIGHTS` and don't light a surface
  flagged `MATERIAL_FLAG_LIGHT` (`receiver_glows`: the beams' own glowing
  parts; also the glowing projectiles, the light models whole, torch and
  flame, and lava). Checked with `vk_testlight line` (flagged too)
  against spheres ([Beams](#beams-vk_beamlightc)).
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
  (R/300)³ (the physical shape's: dynamic lights keep it with the map
  lights' GL shape, 4.15), fading to 0 at R − minlight (GL's surfaces stop there; only the
  muzzle flash has one); the radius changes as GL's (explosions shrink,
  flames flicker by up to 31 per frame: ±20 % of the intensity). The
  color is the client's (with `gl_colored_dynamic_lights`, 0 since 4.9:
  white), converted to
  linear as the map lights' (`VK_ColorToLinear`, 4.17). Dark lights
  (`EF_DARKLIGHT`, the Necromancer's darkness while invincible) aren't
  lights: they follow the sampled ones in `dyn_light_data` and darken the
  world after the denoiser (4.10, [Darkness](#darkness-shadersdarknessglsl));
  lights of a negative radius (the spit; GL lights nothing with them, the
  software renderer darkens) are left out. An alias model whose entity owns one
  of them this frame (its key) within its bounds (a sphere around its
  origin from the model's bounds, scaled) goes into the light group
  (`VK_DynamicLightOwner`, see [Map lights](#map-lights-vk_maplightsc)):
  the light is inside it, and it casts no shadows from any light while it
  owns it (a projectile; also the Eidolon's and the Fallen Angel's muzzle
  flashes, 0.1 s per attack, the chase-cam player with the torch). 6.2:
  a glowing projectile among them emits from its skin and isn't lit by
  its light ([Effect lights](#effect-lights-vk_effectlightc)).
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
  same as a list or a dynamic sphere). 4.9 calibrated the map lights'
  scale against GL ([Map lights](#map-lights-vk_maplightsc)). The lighting is stored RGBE-packed ×32
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
  - `vk_testlight line [length] [power] [radius] [r g b]` (6.3): a dynamic
    line light, a beam's, through the eye along the view's right (128,
    100, 2, white; its power per unit length is its radiance times its
    area per unit length);
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
  samples, which weigh lights by the larger of it and this frame's (4.13); this frame's
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
  lights (those owned by an entity, the dark ones and how many darken the
  world (4.10), left out unlit / over 32, the
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
- **Color** (4.3, `r_maplight_colors 1`, archived; **0 by default since
  4.9**: white, the original's and HoT's default look, the owner's choice;
  color then comes only from bounces off the textures, lava and the
  flames): Hammer of Thyrion's
  colored light, the colors `utils/jsh2color` baked HoT's `.lit` files
  from (see [Map light colors](#map-light-colors-vk_lightcolorc)); on a
  map whose lights have `_color` (0–1, or 0–255 when a component is above
  1; later compilers' key, no original map has it) those, the other
  lights white, as that compiler's `.lit` has them. The 0–255 color
  multiplied GL's lightmap, which multiplies the texture's 8-bit color, so
  the light's color is its linear light (`VK_ColorToLinear`, 4.17: the 2.2
  power, the sRGB curve with `r_srgb 1`): the same hue on a wall (HoT's
  orange 255 128 64 is (1, 0.22, 0.05) by either curve). Not scaled back
  to white's brightness, as in HoT's colored mode: torch light has 0.37×
  the luminance of white, 255 225 200 0.80× (0.79× by the sRGB curve; as
  HoT: a colored map is darker).
  `r_maplight_colors 0`: white; a change rebuilds the lights.
- **Brightness:** a sphere of `r_maplight_radius` (8, the test spheres')
  with a light shape (`r_maplight_shape`, 4.15, below;
  [Lights](#lights-vk_lightc) has the shader side). **Shape 2, "Original",
  the default** (since 4.15; 4.21 made shape 0 the default, 4.22 took it
  back: R107, R108): utils/light's own, each light giving a surface the
  lightmap texel the compiler and GL made of it alone, in linear light,
  times its factor for GL's sum with the other lights in the cluster it lights (4.16,
  fitted to the map's lightmaps: [Light fit](#light-fit-vk_lightfitc);
  `r_maplight_gl_scale` 2, 4.15's one factor for all, where there is none);
  its range is the level.
  **Shapes 0 and 1** (physical; 1 with utils/light's angle term): the
  intensity (π × the radiance of an 8-unit sphere, as `vk_testlight`'s; a
  larger sphere has the same light at a distance, though on bounces its
  solid-angle limit takes it down closer in) is `r_maplight_scale` ×
  (level / 300)^`r_maplight_power` (3): the one power under which
  inverse-square light scales with each light's range as the compiler's
  linear falloff does (twice the level and the distances, twice the
  light); its range is the level times `r_maplight_range` (1). The map
  file (4.7) multiplies either by a light's `scale` and the map's
  `r_map_light_scale`.
- **Calibrated against GL** (4.9, [TESTING.md](TESTING.md#calibration-against-gl-49),
  15 bookmarks: two or three per hub's map starts and two Praevus maps,
  960x540, Release). The direct light (white, `r_debugview 15`) against
  GL's lightmaps in linear light (GL multiplied textures by the lightmap in
  sRGB space), in 30-pixel blocks of the lightmapped world, pooled:

  | Candidate | Median (Hexenlicht / GL) | Spread (stops) | Slope |
  |---|---|---|---|
  | 4.1's: power 3, range = level, scale 1000 | 1.47 | 1.13 | 0.86 |
  | power 4 / power 2 | 1.32 / 1.64 | 1.13 / 1.14 | 0.87 / 0.84 |
  | range 1.5× / 2× the level | 2.25 / 2.61 | 1.13 / 1.18 | 0.48 / 0.40 |
  | range 0.75× / 0.6× | 0.90 / 0.70 | 1.83 / 2.53 | 0.93 / 0.86 |

  So the power hardly matters (most lights are levels 200–300) and GL's
  own range is best; the spread that stays (surfaces lit at grazing
  angles and in shadow: utils/light's 0.5 + 0.5 cos and its 16-unit
  lightmap texels; hot spots next to lights, where GL clips) was story 4.15
  (#145), below. **`r_maplight_scale` 740** (was 1000; **630** since
  4.17: with its 2.2 power 740 showed the lit image at 1.17 of GL's look,
  630 at 1.02 on the surfaces and 1.00 with GL's clipped blocks, the
  direct light 0.99; the numbers below are 4.9's), with matte materials
  (`r_specular 0`, [Materials](#materials-vk_materialc)): the lit image on
  the lightmapped world is as bright as GL's (median 0.99; per bookmark
  0.60–1.63: castle4's corridor, keep1 and romeric1 darker (4.15's
  surfaces), meso1, meso2, tibet1 and demo2 brighter (the lava's light at
  `r_emissive_scale` 32 on the meso maps); the whole frame 1.29× with models
  and sky), the direct light 1.15× GL's lightmaps; the bounce adds ~8 %
  (`pt_num_bounce_rays 0`: 0.92), the denoiser ~1 % (16 raw frames
  averaged: 0.98). With Quake II RTX's specular (`r_specular 1`) the walls'
  specular was ~45 % of their brightness (at 490: 1.00 with it, 0.69
  without): a dielectric's 0.04, up to 1 at grazing angles, against Hexen
  II's dark textures (albedo ~0.04–0.1 in linear light) looked like
  polished metal and made spots of light on walls next to lights (the
  owner's observation). The exposure is fixed ([Bloom and tone
  mapping](#bloom-and-tone-mapping-vk_bloomc-vk_tonemapc)). Dynamic lights
  follow the scale and power (twice a map light of their radius, R81).
- **GL's light shape** (4.15, the same 15 bookmarks and measure).
  utils/light gives a sample point `(level − d)(0.5 + 0.5 cos)` of each
  light in front of the face's plane (none from behind it, so the half is
  at grazing angles, not on faces turned away), sums them, halves the sum
  (`rangescale`), and GL multiplies by 264 >> 7 and clips: a texel is
  `min(1, Σ / 247.27)`, which GL multiplied the texture by in sRGB space.
  So one light is flat and clipped near it and falls steeply towards its
  range (inverse square: a hot spot and a long tail), the half-Lambert is
  `(0.5 + 0.5 cos)^2.2` in linear light (0.22 at grazing, within 10 % of
  the cosine from 0.45 up), and lights add before the sRGB step: two equal
  ones give 2^2.2 = 4.6× one, not 2×. Candidates (scales refitted so that
  the lit image on the lightmapped world matches GL's):

  | Candidate | Median | Spread (stops) | Slope | Surfaces (look) | Whole frame |
  |---|---|---|---|---|---|
  | 0: 4.9's physical shape, radius 8 | 1.15 | 1.15 | 0.86 | 1.01 | 1.29 |
  | 0, radius 16 | 1.15 | 1.10 | 0.85 | 0.99 | 1.27 |
  | 1: GL's angle term (`r_maplight_scale` 705) | 1.18 | 1.03 | 0.83 | 1.01 | 1.24 |
  | 1, radius 16 / 24 | 1.19 / 1.21 | 1.03 / 1.08 | 0.81 / 0.76 | 0.99 / 0.97 | 1.23 / 1.21 |
  | **2: GL's curve** (`r_maplight_gl_scale` 2) | 1.11 | **0.89** | 0.87 | 0.97 | 1.07 |
  | 2, radius 16 / 24 | 1.10 / 1.10 | 0.89 / 0.91 | 0.86 / 0.82 | 0.96 / 0.96 | 1.04 / 1.04 |
  | 2, power 1.8 / 1.5 (scale 1) | 0.86 / 1.15 | 0.89 / 0.93 | 0.75 / 0.66 | 0.72 / 0.96 | 0.77 / 0.91 |
  | 2, the exact sRGB decode (scale 1) | 0.61 | 0.97 | 0.80 | 0.52 | 0.62 |

  **Shape 2 is the default** (the owner's pick, 2026-09-28, again in 4.22
  after 4.21's shape 0, R108; 4.15's run with the defaults gave the same
  light, the lit image 0.98, a single frame): per bookmark the look is 0.75–1.36 of GL's (4.9: 0.60–1.63; castle4's
  corridor 0.92, keep1 0.93–1.03, romeric1 1.00), the hot spots next to torches are GL's flat,
  clipped halos, and the contrast is GL's (slope 0.87). At scale 1 it was
  0.56× GL's lightmaps: overlapping lights, which GL added before the sRGB
  step; 2 makes up for them on average, and what stays of the spread is
  mostly their difference between views (the direct light's median:
  meso2's many overlapping dim lights 0.68, meso9 2.05). Larger spheres don't lower it (the metric's
  30-pixel blocks at 960x540 are ~10 units at 200 units away: GL's
  16-unit texels blur shadows over several; a sphere's penumbra grows with
  the distance to the occluder instead), so the radius stays 8; a lower
  power trades single lights for overlaps (slope down); the exact sRGB
  decode's linear toe brightens faint lights, which GL added before the
  decode. Not physically based: a light has no fixed power, and a wall
  beside a torch gets about a fifth of its head-on light instead of almost
  none; its shadows (ray traced from the sphere) and all light after the
  first hit (bounces, reflections, materials) are path traced, and bounce
  rays see the same shape. Shape 0 stays a setting (6.10's menu; the
  default in 4.21, R107, R108). Cost:
  none measurable (1920x1080, Release, egypt4 and meso2: direct lighting
  +0.02 ms, the frame within the runs' scatter). Left out: decoding GL's
  sum (every light source in GL's encoding, and bounce rays and the
  undenoised paths such as DLSS RR would decode one-sample estimates),
  blurring shadows on the receiver as GL's texels did, dynamic lights in
  GL's shape (they stay physical, R81: GL's formula for them has no
  cosine and an approximate distance), test lights (physical).
- **Map file** (4.7, [Map file](#map-file-vk_mapfilec)): its light lines
  apply whenever the lights are built, after the entities are read and
  before the lava test and the colors: `light` changes the lights whose
  entity origin it names (off, level, scale, color, style, origin),
  `addlight` adds lights, white unless given a color (`vk_lights colors`
  lists them as `addlight`, and a moved light with the origin it was moved
  from); moved or added lights inside solid are dropped; a map file color
  replaces the light's with `r_maplight_colors 1`. `VK_MapLightAt` keeps
  the light entities' origins (where their models are) whatever the file
  does.
- **Kept for editing** (4.8): the map load reads the lump's lights once,
  with their jsh2color colors and the lava test at the entity origin, and
  keeps them; `VK_ApplyMapEdits` copies them and applies the file's light
  lines (at the load, after `vk_mapfile reload`, after each
  [Light editor](#light-editor-vk_lighteditc) edit), repeating the lava test
  only for a light moved elsewhere. `VK_EditableLights` lists every lump
  light and addlight with the file's changes, also those taken out or
  moved or added inside solid (dropped from the lights), for the editor.
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
- **Against GL** (measured in 4.9 and 4.15, above): with shape 2 the lights
  add in linear light, not before the sRGB step (overlaps darker, made up
  for since 4.16 per light and cluster by the fit to the lightmaps, before
  it on average by the scale), each light clips alone (GL clipped the
  sum; the fit takes that in too), shadows come from an 8-unit sphere, not from 16-unit texels
  (sharper contact shadows); with the physical shapes inverse-square
  falloff instead of linear, and with shape 0 the cosine instead of the
  compiler's 0.5 + 0.5 cos (surfaces lit at grazing angles get almost no
  light instead of half of it); no minimum light (the compiler had none
  either); models (monsters, doors) shadow the map's lights, as none did
  in the lightmaps.
- `r_maplights 0` turns them off (test lights only); a change of
  `r_maplights`, `r_maplight_scale`, `r_maplight_power`, `r_maplight_range`,
  `r_maplight_shape`, `r_maplight_gl_scale`, `r_maplight_radius`,
  `r_maplight_colors`, `r_maplight_gamma` (also in the UBO each frame; 4.16
  fits again with it), `r_maplight_fit` or `r_maplight_fit_scale` rebuilds the lights (`VK_RebuildLights`: only while the client is in
  the loaded world, as between `map` and the new world's load the old
  one's memory is freed; else the next load takes them). `vk_lights` prints the shape and the
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

## Light fit (`vk_lightfit.c`)

Story 4.16: GL's sum of overlapping lights for the map lights' GL shape.
GL added a texel's lights before its sRGB step and clipped the sum;
4.15's shape gives each light the texel it makes alone in linear light,
and the path tracer adds those, so a light alone is GL's, overlapping
lights add up to less (n equal lights: n against n^2.2) and GL's clip is
missing. 4.15 had one factor for all (`r_maplight_gl_scale` 2): a lone
torch was twice GL's (4.11a's survey: village5's start 2.19×), fill-lit
yards darker. Here each light gets its own factor per cluster it lights,
fitted to the map's own lightmaps when the map loads.

- **The fit** (`VK_FitMapLights`, from `VK_LoadMapLights` with the lump's
  lights before the map file's changes): utils/light repeated on the
  world's lightmapped faces (not sky or turbulent; a face whose extents
  differ from the engine's lightmap, or whose triangles all face into
  solid, is left out): `ltface.c`'s `CalcFaceVectors`, `CalcFaceExtents`,
  `CalcPoints` without `-extra` and `SingleLightFace`'s value, `trace.c`'s
  `TestLine` through the world's nodes (floats as there), for the lights
  GL's `R_MarkLights` would find for the face (its nodes' planes within the
  level plus a margin: sample points lie up to a texel past their face,
  more world units on a scaled texture, and 8 units more), on a quarter of
  the texels (s and t even). Fitted when the factors are used: at the
  map's load with GL's shape and `r_maplight_fit 1`, else when either is
  set (the physical mode doesn't pay for it). At each texel
  the ratio of GL's value (the lightmap: every style at its normal 264,
  >> 7, clipped, the brightest channel, to the power `r_maplight_gamma`;
  a `.lit` file that `gl_coloredlight` loaded instead: its brightest
  channel)
  to the sum of the lights' own values (each clipped, to that power) is
  what the lights there should be multiplied by (at most 16: GL's sum
  over the lights' own for 10 equal lights is 15.8; above it the model
  doesn't explain the texel's light, e.g. the mission pack's other
  options, and `vk_lights` counts those texels); a light's factor is the
  mean of it weighted by the light's own value, per cluster (the texel's
  triangle's, as `vk_world.c`'s `TriangleLeaf` gives the renderer's
  triangles: a fan, the center 0.01 units in front, else 1) and over all
  its texels. The compiler repeated: on the original game 96–100 % of the
  texels within 1/255 of the BSP's (so its maps were lit without
  `-extra`); the mission pack's only a third (other options), which is
  why the target is the lightmaps, not the compiler's sum. A quarter of
  the texels fits as well as half (within 0.01 stops at 8 maps) in about
  half the time.
- **The factors** (`VK_LightFitFactor`): per light list entry, the
  cluster's (from at least 4 texels), else the light's own, times
  `r_maplight_fit_scale` (1, below); `r_maplight_gl_scale` for a light
  without texels (the map file's addlights, a map without light data) and
  with `r_maplight_fit 0` (4.15's image: the direct light bit-identical to
  `main`'s at five maps). `vk_light.c`'s `ListEntry` writes the factor as
  a half float above the light's index in the entry
  ([Lights](#lights-vk_lightc)); the shader multiplies a GL-shaped light's
  CDF weight and light by it (`list_entry_factor`), so bounce hits and
  models, which sample their cluster's list, have it too. The light's
  color is a full texel's (`VK_MapLightIntensity`: the map file's scale
  and `r_map_light_scale`). The lights' own factors on the 59 maps: the
  median light's 1.2–2.1 on most (0.68 on meso9, whose lightmaps are
  darker than its lights' levels give, 2.66 on keep2), a tenth of the
  lights at most 0.6–1.5, a tenth at least 1.4–3.9.
- **Edits** (4.7, 4.8): the map file's changes apply on top; a light keeps
  its factors when moved, scaled or given another level; nothing is
  fitted again for an edit (a cluster's entry a moved light didn't reach
  before takes its own factor). `r_maplight_gamma` fits again (the power
  is inside the sums). The fake lava lights are in the fit (they are in
  the lightmaps), though the lava replaces them while it emits: their
  neighbours' factors count their overlap (meso2's start 1.04 → 1.26 of
  GL's look, with the lava at `r_emissive_scale` 32; meso9's 1.22 →
  0.83).
- **`r_maplight_fit_scale` 1** (4.17; 1.1 in 4.16): fitted to the
  lightmaps, the direct light is GL's. With the sRGB curve the lit image
  was darker than GL's image (4.9's bookmarks, the surfaces with GL's
  clipped blocks: 0.90): GL multiplied the texture's 8-bit color by the
  lightmap's, which the sRGB curve's linear toe shows darker in linear
  light (a color of 0.2 under half light: 1.4×), and Hexen II's textures
  are dark; 1.1 made the look as bright as before on average, and dark
  stone stayed 0.61–0.68 of GL's look (castle4, the tower). With 4.17's
  2.2 power (see [Textures](#textures-vk_texturec)) GL's product is linear
  light's: at 1 the look with GL's clipped blocks is 1.02 at 4.9's
  bookmarks and 0.98 at 4.11a's 43 Blackmarsh views (decoded by the 2.2
  power, `calib_compare.ps1`'s default since 4.17), castle4 0.90, the
  tower 0.96; the direct light's median 1.02.
- **Measured** (Release): `vk_lights fit` scores the factors on the half of
  the texels the fit didn't use (s + t odd), the direct light against GL's
  lightmaps: on the 42 original maps the spread (stops between the
  quartiles) from 0.73–1.20 with 4.15's factor to 0.44–0.96 with one per
  light and 0.36–0.70 per list entry, the texels within half a stop of
  GL's from 27–52 % to 68–81 %; on the 17 mission pack maps 0.84–1.31 →
  0.45–0.64 (69–77 %); no map worse. `calib_shots.ps1` at 4.9's 15
  bookmarks (`r_maplight_fit 0` / 4.16): the direct light's spread 0.89 →
  0.53 stops, slope 0.87 → 0.91; the look on the surfaces with GL's
  clipped blocks 0.98 → 0.99, their spread 0.78 → 0.59 (the whole frame
  1.07 → 1.06; per bookmark on the surfaces 0.61–1.27, was 0.75–1.34, the
  mean error per bookmark 0.20 stops before and after); at 4.11a's 16
  Blackmarsh views 0.99 → 0.71 stops, the look 0.94 → 0.96, its spread
  0.93 → 0.74, the mean error per view 0.31 → 0.21 stops, per view on the
  surfaces 0.62–1.36 (was 0.63–1.74: village5's torch 1.74 →
  1.24, demo2's start 1.34 → 0.92, village3's yard 0.65 → 0.90; darker
  village2's start 0.80 → 0.62, demo3's 1.07 → 0.74).
- **Cost:** the fit when the map loads in the Original mode (or when it is switched on), 3–76 ms in Release (keep1 65–76, keep5 69–74, demo2 46–51),
  about three times that in Debug; `vk_lights` prints its time. Per frame
  nothing measurable: the entry's factor comes with the light's index;
  only `direct_lighting.rgen` and `indirect_lighting.rgen` changed, and
  their passes were within the runs' scatter (1920x1080, demo1 and the
  cathedral, three runs of `main` and the branch alternated, without
  DLSS: demo1 0.38–0.46 ms against 0.45–0.47 direct, 0.65–0.76 against
  0.77–0.85 bounce).
- **Not physically based**, as 4.15's shape: only a light's first arrival
  changes, after GL's lightmaps; shadows (models' too), bounces and
  reflections stay path traced, and the physical shapes
  (`r_maplight_shape` 0 and 1, the "physically based" mode, R103) don't
  take the factors. Left out: summing every list light per pixel in the
  shader (a loop per pixel, and lights behind walls would count), fitting
  again after edits, fits per light style (every style at its normal
  value: a switched-off light's neighbours keep its overlap), GL's 16-unit
  texel blur (R96), dynamic lights (physical, R97).
- `vk_lights` prints the fit (lights, texels, faces left out, time,
  entries and the largest factor, lights without texels, texels clipped at
  16, the per-light factors' quartiles);
  `vk_lights fit` adds the score.

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
- **The scale** (32 stays: the owner's choice in 4.9, "it really feels
  molten hot", against GL's darker pools; a menu option with 6.10): in linear light (`tm_enable 0`)
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
  channel of at least 215 of 255, `VK_EMISSIVE_THRESHOLD`, blurred with
  an 11-tap filter and normalized, times the texel's color and its
  normalized luminance squared; twice the size, bilinear with wrapping,
  then a 3-tap filter; in linear light by the colors' curve, 4.17), the
  texture `<skin>*E<the skin's CRC>` (`*S` with `r_srgb 1`: a change of
  the curve shows in the flames with the next map's materials), made when
  a model at a light first shows the skin (`VK_EmissiveSkin`, inside the
  frame: two uploads that wait for the GPU, the texture's and the new
  material's). The instance takes the skin's
  emissive material ([Skins](#skins-vk_skinc)), flagged
  `MATERIAL_FLAG_LIGHT` (the map light is its light), at GL's abslight
  (`model_geometry.comp`'s emissive factor; as it is: GL multiplied the
  8-bit texture by it, which 4.17's 2.2 power would make abslight^2.2,
  0.53 for 0.75, but the flames' brightness is the scale's, open) times
  the scale. On the paks
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
  and particles (effects). *6.2: glowing projectiles emit their whole
  skin, and the fire and explosion sprites are sphere lights ([Effect
  lights](#effect-lights-vk_effectlightc)).*
- **Left out** (not emissive in GL): runes, `+0fire` and `+0sun`
  (buttons), water, slime and the other turbulent textures (unlit in GL
  because they are turbulent; since 5.3 an `_e` or a `.mat`'s `emissive`
  makes `*skulls`, `*rtex386`, `*rtex153`, `*rtex346` glow, but not
  lava's lights), the sky (its own modes,
  [Sky](#sky-vk_skyc)); no emissive
  surface has a light style (`vertex_buffer.h`'s `light_style_scale`
  stays 1).
- **Material files** (5.3, `VK_ApplyMaterialFiles`): lava emits its
  replaced albedo, or its `_e`, times the `.mat`'s `emissive` (1); since
  5.5 its light polygons follow (`VK_LavaFileColors`, after the
  materials are applied at map load, `r_reloadmaterials`, `r_materials`,
  `r_lava_light`): the color is the average of the file it emits, made on
  the GPU (`texture_average.comp`: every texel of level 0, each channel
  as linear light plus Q2RTX's bias, by both curves; one workgroup per
  texture, read back; a BC7 file, MATERIALS.md's shipping format, has no
  decoder on the CPU), times the material's emissive factor, the scale
  and the `.mat`'s key (which the light missed before); the original's
  stays `AverageColor`'s. `vk_lights` prints each lava material's color
  ("its file's"), its key ("x 2") and the averages' time (meso9's 64x64
  file: 29 ms at a map load in Debug with the pipeline made, 3 ms on a
  reload). `VK_TextureAverages` is shared since 5.6: without the bias
  (`no_bias`) it gives `vk_materials here` an albedo's plain mean; 6.2's
  sprite lights take it premultiplied (`VK_SpriteAverages`), 6.3's beams
  over the texels inside each triangle (`VK_TriangleAverages`,
  [Beams](#beams-vk_beamlightc)). A flame shows
  its skin's `_e` instead of the fake emissive texture (castle4's torch
  green in the test); `r_emissive_models 0` turns both off (the light
  models keep their flame materials, which then emit nothing). Any other
  lit texture with an `_e`, or a `.mat` `emissive` (then its albedo),
  glows at the scale times that; it lights its surroundings through
  bounce rays only (not a light, not flagged: the light lists hold the
  map's lights and lava's triangles, M5). On a model the emission is also
  times the instance's GL light level where it has one
  (`model_geometry.comp`'s emissive factor, `AliasLight`: abslight, the
  rotating items' pulse, the `MLS_*` modes; 1 when the world lights it),
  as the flames' is: an `_e` on an abslight 0 model shows nothing.
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

## Sky (`vk_sky.c`)

Story 4.6. Hexen II's sky as GL draws it, and the two modes of PLAN §1:
faithful (the sky lights nothing) and sky light (a dome and an optional
sun). Q2RTX's physical sky is not imported (its data has no license, see
[Q2RTX.md](Q2RTX.md)).

- **GL's sky** (HoT's `gl_warp.c`: `R_InitSky`, `EmitSkyPolys`; its
  default path, `gl_multitexture 0`): a 256x128 texture, the right half the
  back layer (scrolling at `realtime` × 8), the left half the front layer
  (× 16; color index 0 transparent, those texels given the back layer's
  average color so bilinear filtering leaves no dark fringe; the palette's
  transparent 255 transparent with its own color), blended over the back at `r_skyalpha` (0.67; 1 is GL's
  multitexture look, an opaque front). The texture coordinates depend on
  the direction only: height counts three times, the direction scaled to
  6 × 63 units, plus the scroll, in texels of the 128x128 layers; bilinear,
  no mipmaps. One sky per map: the world's last sky texture (castle5, tower
  and thomas also have a sky000; GL shows their sky001 on every sky face).
  The game has five: a grey storm (sky001 of 28 maps: demo, castle4,
  castle5, cath, tower, village, keep, tibet, ravdm2, thomas), a blue day
  (egypt, rider2c, ravdm4), red (meso), a night (romeric, ravdm1) and a
  grey-blue one (rider1a, eidolon); keep4, ravdm3, ravdm5 and monsters have
  none.
- **Drawn** (`path_tracer_rgen.h`'s `env_map` and `hexen2_sky`): per pixel
  from the ray's direction, as the software renderer does (`d_sky.c`, the
  same formula); GL computes the coordinates at the vertices of its polygon
  pieces and interpolates, which warps the sky seen up close. The layers
  are two textures made at map load (`VK_LoadSky`: `upsky`, `lowsky`,
  `TEX_RGBA | TEX_LINEAR | TEX_REPEAT`, bilinear and repeating), blended in
  their 8-bit colors as GL blends the framebuffer's bytes (and, since the
  textures are UNORM, 4.17, filtered in them as GL's), then linear light by
  the colors' curve. Primary rays, reflections,
  refractions and specular bounces see it, in both modes; as in Q2RTX it
  goes into `PT_TRANSPARENT` (not denoised) with a rotation-only motion
  vector (the moving clouds looked sharp with TAAU and DLSS SR/RR in 4.6's
  shots). `R_InitSky` does nothing: the sky is taken from the world at map
  load. It scrolls on GL's `realtime` (the clock: it runs while paused and
  differs between runs); with `host_framerate` on game time, so that test
  runs repeat (TESTING.md).
- **Brightness:** the blended color is the radiance (times 1: GL's
  fullbright relation to the walls, measured in 4.5), under the auto
  exposure like everything else: at demo1's start looking up, the sky is
  2.6 times GL's in linear light and the walls 5.6 times, at egypt1's 0.72
  times; a view filled with sky is exposed for it (a night sky looks
  brighter than GL's). *Since 4.9 the exposure is fixed by default: the
  sky is GL's (R93).* Scaling the sky by
  the exposure as the effects are (R41) was rejected: a view filled with
  sky would make the exposure chase itself to its limits.
- **Up close:** flying up (`noclip`) under meso9's sky face (z 704), the
  sky matches `glh2`'s from 328 to 98 units below; 17 units below `glh2`
  draws black, Hexenlicht the sky.
- **Modes:** `r_sky_light 0` (faithful, the default until calibration
  picks per map; the sky and sun cvars are per-map settings of the
  [map file](#map-file-vk_mapfilec), reset at every map load): the sky lights nothing, as
  before (glossy surfaces reflect it: specular bounces see it). `r_sky_light
  1`: diffuse bounce rays that hit the sky gather a dome of constant
  radiance (`env_map` with `remove_sun`, Q2RTX's path for a sky without
  portal lights): the sky's average color as GL shows it (both layers
  blended at `r_skyalpha`, averaged over all pairs of their texels, linear;
  `vk_sky` prints it: grey storm 0.065, blue 0.28 0.28 0.45, red 0.12 0 0,
  night 0.017 0.018 0.024, grey-blue 0.056 0.057 0.067) times
  `r_sky_light_scale` (1: the sky lights with the light it shows). Constant,
  not the scrolling layers: the light doesn't flicker as clouds pass and
  the denoiser's gradients see no change. The sky's faces are no polygon
  lights (Q2RTX's portal lights): they would fit (at most 138,000 list
  entries, tibet1, with the map lights' 19,000; 524,288 fit), but an
  outdoor cluster's list would hold up to ~1000 sky triangles, and a pixel
  weighs 8 candidates of one partition of its list, so the map's lights
  would seldom be among them. Measured at egypt1's courtyard: the floor
  takes the sky's blue (sRGB blue 20 → 28, → 41 at scale 4).
- **The sun** (`r_sun 1`, only in the sky light mode): Q2RTX's
  `get_sunlight` (a disc of `r_sun_angle` degrees, 1; sampled in direct
  lighting and at bounce hits, `pt_direct_sun_light`, `pt_sun_bounce_range`,
  `pt_sun_specular` as Q2RTX's) from `r_sun_elevation` and
  `r_sun_azimuth` (degrees; the azimuth from +x towards +y, as a yaw;
  placeholders 45 and 45 until a map file sets them), of `r_sun_color` (an 8-bit color, white) at
  `r_sun_intensity` (1: its irradiance is π, a white surface facing it is
  lit as GL's fullbright; measured with the bounces off, egypt1's floor of
  albedo 0.055 gains 0.051). Its shadow ray ends at the first sky face it
  meets (`trace_sky_distance`, a ray of the sky's instances only): Hexen II
  has world geometry above some skies (rays up and at 45° from the sky
  ceilings reach other world geometry within Q2RTX's 10000 units: village2
  77 %, cath 65 %, demo3 25 %); with Q2RTX's rule village2's sunlit yard
  seen from the start is dark. Only clusters that can see a sky triangle
  trace it (Q2RTX's sky visibility: the PVS rows of the clusters holding
  one, `LightBuffer.sky_visibility`, copied by `vk_light.c` with a new
  map). The models around a light (torches, flames, glowing projectiles)
  cast its shadows; they are only out of the shadow rays of the lights
  inside them. No visible disc (the painted skies have none) and no sun in
  `env_map`. With the denoiser a change of the sun takes 1–2 s to show
  fully (the HF history).
- **Cost** (1920x1080, Release, TAAU 100 %, the GPU at its power cap, each
  setting twice in one run): the visible sky and the dome none measurable
  (egypt1: the frame 7.71 / 7.69 ms, primary rays 1.19); the sun +0.3 ms at
  egypt1 (direct light 0.58 → 0.68, bounce 1.36 → 1.56), +0.4 at village1
  (0.64 → 0.83, 1.46 → 1.72).
- **Against GL:** no warping up close; the sky in reflections, refractions
  and glossy bounces; its brightness under the exposure; the sky light
  mode (opt-in).
- **Its material file** (5.5, MATERIALS.md; found by
  [Material files](#material-files-vk_matfilesc)' `VK_SkyImageFile`):
  the whole sky in the original's layout, 2:1, any size: the left half
  the front layer, the right half the back, each square. `FileLayers`:
  the front's transparency is the file's alpha (its left half's:
  `VK_SkyFrontHasAlpha`), or without one (every texel 255) the
  original's (index 0 and 255) scaled to it, each texel's centre's
  (MATERIALS.md's "keeps the original's coverage"); its transparent
  texels take the back's average color, truncated, as GL's; the back's
  alpha isn't read. They replace `upsky` and `lowsky` at their size
  (`GL_LoadTexture` replaces the slots), bilinear, repeating, no
  mipmaps, as GL's: the shader addresses the layers in fractions, so a
  file of any size shows the same sky at its detail. The dome's average
  (sky light mode) is the same over all pairs of the layers' texels, from
  histograms per channel (the back's values; the front's values and
  alphas) and a table for the curve (4096 steps, linear between them:
  the blend isn't a byte), made when the file loads and again for
  another `r_skyalpha` or `r_srgb`: the export's files (the original
  layers) give the palette path's averages to four decimals.
  `r_reloadmaterials` and `r_materials` make the layers again
  (`VK_ReloadSkyFile`, reading the file again whether it changed or not:
  one image; the sky's visibility stays). A change of `r_skyalpha` or
  `r_srgb` makes the average again inside a frame (no print; with a
  file whose front has soft alpha up to 256 × 256 × 256 table steps per
  channel: a hitch of that frame). Refused (the original
  sky, reported): BC7 or BC5 (the texels are split and edited), not 2:1.
  Measured (`special_set.ps1`): demo1's and keep1's sky001~6566 and
  egypt1's sky000 from files, a file with holes and one without (the
  original's), the domes as computed by hand (0.2072 0 0.5436, 0.2777 0
  0.3884).
- `vk_sky` prints the sky's texture, its average color, its file's
  layers (5.5), the clusters with sky and those that see it, the mode,
  the dome and the sun.

## Map file (`vk_mapfile.c`)

Story 4.7. A map's calibrated settings and light fixes, in
`maps/<map>.hlmap`: plain text, one command per line, `//` comments (after
a space: `64//x` is one word; C-style comments within a line too), the
same commands the console takes, numbers within a million. Only these are
taken; any other line, a bad value, more than 32 words or a word over 63
characters is reported (at map load and by `vk_mapfile`) and the line
skipped:

```
// meso9
r_sky_light 1
r_sun 1
r_sun_elevation 60
r_sun_azimuth 135
r_sun_color "1 0.9 0.8"
r_map_exposure -0.5
r_map_light_scale 1.2
light 1152 2720 64 level 400 color 1 0.5 0.2
light 1100 2700 64 off
light 900 2600 40 origin 900 2600 72 scale 0.5 style 0
addlight 1000 2000 128 level 300 color 1 1 1
```

- **Per-map cvars:** 4.6's `r_sky_light`, `r_sky_light_scale`, `r_sun`,
  `r_sun_intensity`, `r_sun_color`, `r_sun_elevation`, `r_sun_azimuth`,
  `r_sun_angle`, and `r_map_light_scale` (every map light's intensity times
  it, 1) and `r_map_exposure` (EV added to the global `tm_exposure_bias`,
  0; `vk_ubo.c`). Every map load resets them to their defaults (their values
  when the renderer started) before the map's file sets them: they don't
  carry over from map to map (a `r_sky_light 1` typed in the console lasts
  until the next map, and a test script sets them after `map` and its
  waits). The global calibration (`r_maplight_scale`, 4.15's `r_maplight_shape`, `_gl_scale`, `_gamma` and `_radius`, the `tm_*` cvars,
  `r_skyalpha`) isn't per map.
- **Light lines** ([Map lights](#map-lights-vk_maplightsc) applies them
  when it builds the lights; the lights keep them through `r_maplights`
  and color changes): `light x y z <changes>` changes the map lights whose
  entity origin is x y z (to the unit; the entity lump's origins are
  integers, and an external `.ent` file's reordering doesn't matter):
  `off`, `level n` (at least 1: range and intensity, the mapper's key),
  `scale f` (intensity only), `color r g b` (an 8-bit color, 0–1 or 0–255 as the
  `_color` key; shown with `r_maplight_colors 1`, 0 keeps the original's
  white), `style n`, `origin x y z` (moved there; a spotlight keeps its
  direction). `addlight x y z <changes>` adds a light (level 300, white,
  style 0 unless changed). Lights moved or added inside solid are dropped,
  as the compiler lit nothing from there; lights the compiler dropped
  (inside solid, unlit) can't be changed (`vk_mapfile` says so). The light
  entity's torch or flame model stays where the game spawned it, glowing
  and without shadows, whatever the file does with its light. Up to 1024
  light lines.
- **Where:** first the game's filesystem, `maps/<map>.hlmap` (loose in
  `data1\maps\`, `portals\maps\`, a mod's folder, or in a pak): a player's
  or mod's own file, taken only from the map's game folder or one of higher
  priority (as uHexen2's external `.ent` files: data1's file isn't used for
  a Praevus map); then the file shipped next to the exe (`<exe
  folder>\maps\`, which the build copies from the repository's
  `data/hexenlicht/maps/`), only for maps from the game's own folders
  (data1, portals: a mod's map of the same name doesn't get it). The first
  found is used whole.
- `vk_mapfile` prints the file used (or why one wasn't: a lower game
  folder, not the game's own map), its settings that differ from the
  defaults, each light line and how many lights it changed, and the lines
  skipped; `vk_mapfile reload` reads it again and applies it (the per-map
  cvars reset first) and rebuilds the lights, for calibration without
  reloading the map. `vk_lights` has a line on the light edits; a `light`
  line that matched nothing says whether the compiler had dropped a light
  there.
- Tested (4.7, temporary files): egypt1's start with the sky light, a sun,
  exposure and light scale, a light taken out, one recolored at twice the
  level, one moved, one added, one moved outside the map (dropped), an
  unmatched origin and three bad lines (reported); `vk_mapfile reload`
  after `r_sun 0` gave 1 back; demo1 without a file had the defaults; a
  game-folder file won over a shipped one; with `-portals`, data1's
  `keep1.hlmap` was skipped and the shipped one used; a mod's copy of demo1
  got neither; an empty file loads (no lines); `level 0`, `inf`, `1e99`,
  a coordinate past a million, a non-number `r_sun`, a four-number color
  and a 33-word line were reported; an `addlight` without a color is white
  on a jsh2color map; with demo1's 48 torch and flame lights taken out its
  48 torch models stayed in the light group. Not exercised: a `light` line
  naming a light the compiler dropped.
- **Kept as lines** (4.8, for the [Light editor](#light-editor-vk_lighteditc)):
  the file stays in memory as its lines (without their line ends), each
  with an id that lasts while lines come and go. An edit of a light
  (`VK_MapFileSetLight`) turns all the `light` lines of its entity origin
  into one, where the first was (its trailing `//` or `/* */` comment
  kept; a comment within the line goes, colors are written 0–1 unless a
  component is above 1, then 0–255, which reads back the same), or removes
  them when no change is left; an `addlight` line is rewritten in place.
  Then the lines are parsed again, without setting the per-map cvars (what
  the console set since the load stays), and applied (`VK_ApplyMapEdits`):
  the lights are always what the lines say. A line that wouldn't parse (a
  number past a million, too long with its comment, past 1024 light
  lines) changes nothing: the old lines are put back and the reason
  printed. An edit that changes a line's text counts as unsaved.
- **Save** (`vk_editlight save`, `VK_SaveMapFile`): into the running game's
  folder, `<game folder>\maps\<map>.hlmap` (`data1`, `portals` with
  `-portals`, or the mod's; the folder is made when missing), which the
  next load finds first (loose files come before the paks in Hexen II's
  search order). The per-map cvars are saved as they are now: a line that
  names one gets its value (a line that has it already stays as written),
  one that differs from its default without a line gets one after the last
  cvar line (else before the first light line). Comments, blank lines and
  skipped lines stay as they were; a new file starts with a comment line;
  CRLF line ends. It is written to `<file>.tmp`, which then replaces the
  file: a failed write leaves the old file (and says where the lines
  are). A file cut at 65,536 lines isn't saved. A shipped file is saved into the game folder whole, with
  the edits, and that copy is used from then on. To ship a file, move it
  into the repository's `data/hexenlicht/maps/` (a copy left in the game
  folder hides later shipped versions; `vk_mapfile` says which is used). A
  map load or `vk_mapfile reload` with unsaved edits says they were
  dropped; `vk_mapfile` counts them.

## Light editor (`vk_lightedit.c`)

Story 4.8: the map's lights edited in the game, saved into the
[Map file](#map-file-vk_mapfilec) with 4.7's vocabulary.

- **`r_editlights 1`** (0, not archived) shows a marker at each light
  within `r_editlights_distance` (1024 units) that the eye sees: a walk of
  the world's BSP from the eye to 2 units short of the light (it prints
  nothing, unlike `SV_RecursiveHullCheck` with `developer 1`, as it runs
  in the frame), so models and doors don't hide one, and a light inside
  its torch's mesh is shown; with the eye inside solid (`noclip`) every
  light in reach is. One walk per light in reach and in view per frame
  (tens to a few hundred), only with `r_editlights 1`. A marker is a square of
  the light's hue at full brightness (white with `r_maplight_colors 0`) in
  a black frame; hollow grey when the map file takes the light out; hollow
  red when it was moved or added inside solid (dropped, but selectable, to
  move it out; the walk stops 32 units short of it). The
  selected light is bracketed white (also out of sight), the one `select`
  would take grey. A panel at the view's top right, below the notify
  lines, shows the selected light: classname, the entity origin its lines
  name (an addlight's own), level, scale, style and the style's value now,
  its 8-bit color and where it comes from (jsh2color, `_color`, the map
  file, white), a spotlight's width, where it was moved, off or inside
  solid, whether the file changes it (or how many lights share its origin:
  a light line changes them all), the distance, and the file's unsaved
  edits. With `crosshair 0` (the default) a `+` marks the view's center.
  Drawn from `R_RenderView` into the 2D batch, so under the HUD, menus and
  console; nothing is printed in a frame.
- **`vk_editlight select`**: the light nearest the crosshair within 10° of
  it, in sight and within `r_editlights_distance` (the nearer on a tie);
  `select x y z` by the entity origin (to the unit), as light lines and
  `vk_lights colors` name them, else an addlight at that origin; `select
  none`. It works without `r_editlights`. The selection is the light's
  identity (a lump light's entity, an addlight's line), so it lasts
  across edits and `vk_mapfile reload` (an addlight's line gets a new id
  there: the selection goes); a new map clears it. Unsaved edits are
  dropped at a map load, `vk_mapfile reload` (both say so) and quit.
- **`vk_editlight <changes>`** changes the selected light, several changes
  in one command, applied only if all parse: the map file's `off`, `level
  n` (at least 1), `scale f`, `color r g b` (an 8-bit color, 0–1 or 0–255), `style
  n`, `origin x y z`; and the editor's `on` (clears `off`), `origin eye`,
  `origin cursor` (the crosshair's world hit, 8 units out along the
  surface's normal), `move dx dy dz` (world axes), `level *f` and `scale
  *f` (times the current value: for key bindings, e.g. `bind KP_PLUS
  "vk_editlight scale *1.1"`). Origins are rounded to the unit. A change
  back to the map's own value (origin, level, style; scale 1; an addlight's
  level 300, style 0) drops that key, and a light with no key left loses
  its line. An added light has no `off` (`reset` removes it). Each edit
  prints the light's line and how long it took.
- **`vk_editlight add [changes]`**: a new light at the eye (or `origin
  cursor`), level 300, white: an `addlight` line at the file's end,
  selected. **`reset`**: the selected light's lines removed (the map's own
  light again; an addlight deleted). **`save`**: the map file, see [Map
  file](#map-file-vk_mapfilec). `vk_editlight` alone prints the panel's
  lines, `vk_editlight help` the commands.
- **How an edit applies:** it rewrites the light's lines in the map file
  and applies the lines again onto the lump's lights as the map loaded
  them ([Map lights](#map-lights-vk_maplightsc): no entity parse or
  jsh2color, which takes up to 232 ms in Debug), then rebuilds the light
  lists (`VK_RebuildLights`). Measured (Release, tibet1: the most list
  entries, 19,020): 1–2 ms per edit, with the timer's 1 ms resolution;
  egypt1 in Debug 2–4 ms.
- Gameplay doesn't change: `cl.light_level` ([Scene](#scene-r_scenec),
  4.12) comes from the baked lightmaps. The torch model stays where the
  game spawned it, glowing and without shadows (4.7's rule).
- Left out: dragging with the mouse or carrying a light with the camera
  (a list rebuild and a statistics reset every frame), a 3D gizmo, range
  spheres, an undo stack (`reset` per light, `vk_mapfile reload` for all),
  what 4.7's vocabulary has no key for (a spotlight's direction or cone,
  the 8-unit sphere, range without intensity: `level` sets both, `scale`
  compensates; changes by classname), lights the compiler dropped, lava,
  dynamic, test lights and emissive surfaces, a menu (6.10).
- Tested (4.8, Debug and Release, temporary files in the data folder):
  egypt1 with a hand-written file (comments, two cvar lines, two lines for
  one light, a bad line, an `off` line, an addlight): select by origin,
  `level *1.5` merged the two lines into the first (its comment kept),
  `color 255 128 0` with `move`, `style 1` then `style 0` (dropped), `on`
  removed the `off` line, an addlight moved (its comment kept), `add` at
  the eye then `origin cursor`, a move into solid (reported, red marker)
  and `reset`, bad input reported; `r_sun_elevation 30` in the console was
  saved after the last cvar line; after `map egypt1` the saved file gave
  the same lights; unsaved edits were reported dropped by `vk_mapfile
  reload` and by a map load; no-op edits (`style 0` again, `reset` of an
  unedited light) count nothing. After the code review: `color 300 200
  100` stays 300 200 100 through the next edit's rewrite; a `move` past a
  million was refused with the old line kept; a save onto a read-only file
  left it as it was and the lines in `.tmp`. A map without a file got a new one with a
  header (the `maps\` folder made); a shipped file (next to the exe) was
  saved into `data1\maps\` whole with the edit; with `-portals` keep1's
  went to `portals\maps\`. Crosshair `select` took the light 9.6° above
  the crosshair at egypt1's start. Markers and panel at UI scales 1 and 2.
  A pixel regression against `main` (demo1, egypt1 with a file of every
  kind of change, meso2, castle4; Release, paused) was within `main`'s own
  run-to-run noise, `vk_lights` identical. Not exercised: moving one of
  the mappers' fake lava lights (the lava test is repeated where a moved
  light goes), two light entities at one origin, a pak's map file saved
  over (a loose file comes first).

## Calibration (`vk_calib.c`)

Story 4.9: the commands the calibration against GL uses (the procedure and
the scripts are in [TESTING.md](TESTING.md#calibration-against-gl-49)):

- **`vk_setpos x y z [pitch yaw]`** (a local single-player game) puts the
  player there, not moving, the view turned as given (the player edict's
  `angles` and `v_angle`, `fixangle`). A `save` on the same console line
  keeps the view's pitch: a load sends the player model's angles
  (`Host_Spawn_f`), which the next server frame turns into a third of the
  view's pitch (`sv_user.c`). So both engines, unmodified `glh2` too, load
  a save made this way with the same camera; angles go through the
  protocol as bytes (1.4°).
- **`vk_bookmark <name>`** appends `name map x y z pitch yaw [portals]`
  for the player's origin and view to `bookmarks.txt` in the game folder
  and prints it: the lines of `tools/hexenlicht/bookmarks.txt` (4.9's
  views) and `bookmarks_<hub>.txt` (4.11's, TESTING.md's "Calibrating a
  hub").
- **`vk_screenshot <name> [frames]`** writes `shots\<name>.tga`, the next
  presented frames (1–1024, 1) averaged in linear light
  (`VK_RequestScreenshotAverage`, `vk_swapchain.c`: each frame's capture
  decoded by the colors' curve and summed, the average encoded, 4.17): a paused frame's
  one-sample noise averages out; named, so not limited to `screenshot`'s
  100 numbered files. Each captured frame waits for its fence.
- **`r_debugview_scale`** (1) multiplies the debug views
  (`view_composite.frag`'s scale), so that lighting above 1 isn't clipped
  in a shot (the calibration takes the direct light at 0.25).
- **`vk_darkplaces [n] [threshold]`** (4.10) lists where GL shows the
  world's floors black: world surfaces facing up (normal z above 0.7, not
  sky or turbulent) whose brightest lightmap texel is below the threshold
  (8 of 255) at the light styles now, as `R_BuildLightMap` makes it (each
  style map's byte times its value, >> 7, clipped; RGB samples), or that
  have no samples (GL draws them black; a map without light data GL draws
  fullbright: none); summed by the leaf in front of each (its middle plus 2
  units along the normal; only empty and water leaves: not facing into
  solid, which qbsp leaves such faces, nor inside the sky's brushes). Most
  of that is out of reach (rooftops, ledges under the sky: what a first
  version listed), so only leaves where the server has an entity with a
  model count (monsters, items, puzzle pieces; not the players, the
  lights, markers or brush entities): the n (10) with the most dark floor,
  their share of the leaf's floor, the entities, and a point 24 units above
  the first one's origin (a player's origin for `vk_setpos`). A local game
  only. See [Darkness](#darkness-shadersdarknessglsl).

## Darkness (`shaders/darkness.glsl`)

Story 4.10: Hexen II's darkness as GL shows it.

- **Total darkness** is light style `'a'` (0; `world.hc`: "'a' is total
  darkness"): switchable lights (styles 32–62, `START_LOW`, fades between
  `lightvalue1` and `lightvalue2`), shot-out torches (`torch_death` sets
  their style to 0), broken light brushes (`breakable_brush`), style 63.
  The map lights follow GL's style values every frame (4.2, [Lights](#lights-vk_lightc))
  and the GL shape clips a style with the light (4.15): a light at `'a'`
  gives nothing. Nothing new was needed.
- **Dark lights** (`EF_DARKLIGHT`: only the Necromancer's invincibility, the
  Icon of the Defender, 10 s; in deathmatch also 3 s after a respawn): the client makes a dynamic light of radius
  200 + 0–31 (a new random each frame) with the dark flag. GL subtracts it
  from the lightmap texels it reaches (`R_AddDynamicLights`, the `GL_RGBA`
  path, `gl_lightmapfmt`'s default): `2 (R − |h| − ρ) / 255` of a texel (h
  the plane distance, ρ the in-plane distance, GL's octagonal one), where
  `R − |h| − ρ` is above the light's minlight (0), clipped at 0: the world
  within ~100 units goes black, fading out to ~215. The software renderer
  does the same; both add the light to alias models as any other (GL's
  model lighting ignores the flag: models near him get brighter, clamped at
  128). `vk_light.c` puts the dark lights after the dynamic lights the
  shaders sample, in the same UBO array (`num_dark_lights`, their minlight
  in `spot_data`'s bits, `dark_light_unit` = 2^−`r_map_exposure`);
  `direct_lighting.rgen` computes each pixel's amount at its world surface
  (`dark_light_amount`: the G-buffer's position and geometric normal, 0 on
  alias models, flagged `MATERIAL_FLAG_MODEL` by `vk_instance.c`, on the
  weapon and on turbulent surfaces, `MATERIAL_FLAG_WARP`, which have no
  lightmaps in GL) into `PT_THROUGHPUT`'s w, which nothing reads after
  `reflect_refract.rgen` (only while there are dark lights: otherwise it
  keeps its path length); the composites (`asvgf_atrous.comp`'s last pass,
  `compositing.comp` without the denoiser, which is also DLSS RR's input)
  scale the surface's diffuse and specular light by `dark_light_factor`:
  the light in GL's texel units (`(E / unit)^(1/2.2)`, E the demodulated
  diffuse light's luminance, the unit the light that shows a texture at its
  own color: the lit image was calibrated to GL's look, 4.9 and 4.15, so
  that is GL's full texel whatever the map lights' shape; not shape 2's
  `r_maplight_gl_scale`, which makes up for GL's overlapping lights and
  would make the darkness reach farther), less the amount, clipped at 0,
  back to linear light (safe for a texel of 0 or inf). So it acts after the
  denoiser, frame by frame as GL (no lag; the flicker of its radius is
  GL's). `r_darklights 0` turns it off (`r_dlights 0` too); `vk_lights`
  counts them. Beside `glh2` (the same save, three lit starts looking down
  30°; GL's shots without its yellow-green power-up tint, `gl_polyblend 0`:
  the view blends are 6.6's) the dark disc around the player has GL's shape
  and nearly its size: 59, 81 and 56 % of the pixels dark (below 6 of 255)
  against GL's 47, 67 and 51 % (demo1, castle4, egypt1; with
  `r_darklights 0` 5, 23, 1 %), its edge 10–15 % farther out, where
  Hexenlicht's floor is darker than GL's (demo1's there 0.75×: the texel is
  estimated from the pixel's own light; with shape 2's `r_maplight_gl_scale`
  2 as the unit it reached farther still). No measurable cost with one (demo1, 1920x1080,
  Release: direct lighting 0.84–0.86 ms against 0.79–0.88 without, the
  frame within the runs' scatter); none without. Against GL: the factor is
  from the luminance (GL clips each channel: its dim channels go black
  first; the hue stays here), ρ is Euclidean (GL's octagonal one in
  texture space, so on a texture scaled by s its disc is s times as wide
  and up to ~11 % smaller off the axes), translucent brush surfaces (in
  `PT_TRANSPARENT`) and reflections keep their light, and light bounced
  off darkened surfaces isn't darkened (GL has none). With DLSS RR or
  without the denoiser the factor comes from one-sample light: it is
  convex, so the disc is a little brighter and speckled there (not
  measured).
- **The weapon's least light:** GL's `R_DrawViewModel` gives the weapon at
  least 24 per channel ("always give some light on gun"), a vertex color of
  24 / 200 × GL's shading dots (1 in the middle of the table), which GL
  multiplied the texture's 8-bit color by (with 4.17's 2.2 power exactly
  (24 / 200)^2.2 in linear light); `direct_lighting.rgen` gives the
  weapon's direct light at least that (`weapon_min_light`, per channel from
  its albedo: 0.94 % of it by the 2.2 power, 2–7 % by the sRGB curve),
  so in total darkness it stays dimly visible as in GL (rider2c's dark
  bank room: the hammer about as bright as GL's).
  Only the direct light is floored (GL floors the whole): with bounce light
  about as bright as the floor the weapon gets up to twice GL's least
  light, and the floor on a noisy one-sample light raises its mean a
  little.
- **Dark places:** nothing in the game code marks them; they are floors the
  lightmaps leave black, lit by the torch artifact (4.4's dynamic lights:
  `EF_DIMLIGHT` then `EF_TORCHLIGHT`, `EF_MUZZLEFLASH` | `EF_BRIGHTLIGHT`,
  for 23 s, dim for 7 s; the client keys all of an entity's lights to one
  slot, so the torch is the last, the dim light of radius 200 + 0–31, in
  GL too). `vk_darkplaces` ([Calibration](#calibration-vk_calibc)) finds
  them: 34 leaves with an item or monster on the 53 single-player maps.
  At six of them (castle5, meso5, meso6, rider2c, romeric2, tower; two
  views each, `calib_shots.ps1`, TESTING.md) Hexenlicht's lit image
  without the torch is as dark as GL's or darker (mean 0.65–1.21×, the
  share of visible pixels alike), but for two of its own: meso6's black
  corner is lit red by the lava at `r_emissive_scale` 32 (6.7×: DECISIONS
  R94), and rider2c's big dark room by bounce light off its one lit ceiling
  spot (35 % of the pixels visible against GL's 11 %). With the torch
  both light the rooms; Hexenlicht 1.7–2.6× brighter next to walls (the
  torch is a dynamic light, which stays physical: DECISIONS R97). 4.11a
  found the other side: at dark views, looking ahead, the torch adds
  0.03–0.22 of GL's light, as it sits at the player's feet, on the
  floor's plane, where GL's formula lights the floor most (story 4.19).
- **Gameplay** was already GL's: `cl.light_level` (4.12, `r_light.c`) is
  `R_DrawViewModel`'s, dark lights adding to it as in GL.
- Left out: the hydra's blinding (`df`, GL's full-screen dark flash: the
  view blends of 6.6), `EF_DARKFIELD` (the haste boots' particles, 6.2),
  dynamic lights of a negative radius (GL lights nothing with them), GL's
  brightening of models by a dark light.

## 3D view (`vk_view.c`)

- `VK_RenderView3D` takes the 3D view in pixels (`r_refdef.vrect` × UI
  scale, centered like the 2D), lets `vk_upscale.c` decide the render size
  (the view × `r_scale`, the width rounded up to even), jitter and
  upscaling (3.8, see [Upscaling](#upscaling-vk_upscalec)), fills the UBO
  and dispatches the view passes at the render size into `TAA_OUTPUT`
  (FSR's outputs with FSR), with `VK_ComputeBarrier` between them;
  `GL_EndRendering` calls `VK_DrawView3D` after beginning swapchain
  rendering: `fullscreen.vert` + `view_composite.frag` (Q2RTX's final
  blit: scales the upscaler's output over the view, see Upscaling; the
  colors' encode, 4.17, + `gamma` like the 2D) into the 3D rectangle, then restores the
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
  its geometric normal while there is no water normal map (Q2RTX's global
  waves, 6.5; a texture's own `_n` applies since 5.3, warped as the
  albedo, and alike on a liquid's two coincident faces since 5.5, see
  [Materials](#materials-vk_materialc)); vertical water
  and slime stay water (3.5b: Q2RTX makes them glass for its force fields;
  Hexen II's vertical turbulent surfaces are walls). The sky (and
  nothing) is an empty surface whose color, Hexen II's sky since 4.6
  ([Sky](#sky-vk_skyc)), goes into `PT_TRANSPARENT` (`pt_show_sky 1` shows
  the sky polygons instead). Translucent surfaces (alpha < 1) split the fields as in
  Q2RTX: the even field stays on the surface, the odd one goes through it
  (their material kind), which `reflect_refract.rgen` follows (3.5b, below).
  6.4: a translucent model's alpha is its entity's times its skin's where
  it was hit (`get_hit_alpha`: the material's mask, [Skins](#skins-vk_skinc),
  250 and up opaque, as M31's coverage;
  GL's blend: `EF_TRANSPARENT` clear at color 0 and 0.33 at odd colors,
  `EF_SPECIAL_TRANS`'s table, a translucent cutout's holes), and the field
  going through doesn't show the surface's emission (Q2RTX's both did, so
  a translucent surface glowed at full strength, not at its opacity; it
  does when no pass follows: the field then ends on it).
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
    behind it on the same instance (Q2RTX's rule against showing a
    model's inside; all world triangles are one instance, so a world
    layer behind a world layer is passed through); 6.4: a layer of
    another instance (a window behind a window, a translucent model
    behind one) is passed with the probability of its transparency, a
    blue-noise number per pixel and frame (`RNG_TRANSLUCENT_LAYER`), and
    shown otherwise, so that on average it shows at its opacity as GL
    blends it (Q2RTX's path ended on it: shown solid); passed, it doesn't
    show its emission; the last pass shows it (no pass to continue). The
    rays cull back faces as in Q2RTX, which skips the
    inside faces of turbulent volumes and a translucent model's far side;
    the primary rays don't (R11), so a two-sided `EF_SPECIAL_TRANS` model
    seen from behind through a translucent surface loses its back faces
    (kept in 6.4: culling them is what lets a ray through a translucent
    model reach the world in one pass). A ray from inside a
    liquid leaves it through a translucent turbulent surface (they bound
    liquid volumes).
  - Translucent brush entities (`DRF_TRANSLUCENT`: the game's breakable
    windows) are lit, as Raven's software renderer lit them (its lit
    surface cache through its translucency table; GL draws them unlit, at
    the texture's colors); 6.4: a material file's glass on one replaces
    the entity's blend (`vertex_buffer.h`: its triangles keep alpha 1),
    so it is glass alone.
  - Mirrors and glass (Q2RTX's `chrome` and `glass` kinds) come from a
    texture's `.mat` since 5.5 ([World](#world-vk_worldc),
    [Skins](#skins-vk_skinc)): chrome (and a chrome model) reflects about
    the shading normal below roughness 0.02, the throughput times the
    base color, no Fresnel term, no field split (above 0.02 it is shaded
    regular, `primary_rays.rgen`); glass is thin (`pt_thick_glass` 0):
    Fresnel from 5 % head-on, the even field reflects (F × 2), the odd one
    refracts twice at 1.52 tinted by the base color ((1 − F) × 2), and it
    needs `pt_reflect_refract` ≥ 1. Both are tinted by the albedo, so the
    originals' dark textures make dark mirrors and glass: a light albedo
    with them (MATERIALS.md). Glass casts no shadow and doesn't tint the
    light through it (the shadow rays see the opaque group only). Cost
    (5.5, demo1's start at 1920x1080, Release, full power: a chrome floor
    and a glass pedestal over much of the view): the pass 0.06 → 0.30 ms,
    the frame 3.53 → 3.65 ms. Screens and security cameras are Quake II's.
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
  weapon shadows, the sun's shadow rays (4.6: ending at the first sky face,
  the models around a light in them), the hit-distance clear and a gradient
  sample's light style change (4.13, [Denoiser](#denoiser-vk_asvgfc)). Without the
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
    emission, the sky and the sun, no light samples (the sky's dome and the
    sun only in the sky light mode, 4.6).
  - 0.5: the first bounce for every other row, alternating per frame, ×2.

  Hexenlicht's changes: the launch check; the weapon is only in its own
  surfaces' bounce and shadow rays (as R20; Q2RTX's is in every ray without
  a first-person model); a model hit by a bounce ray has its `colorshade`
  hue (as `get_material`); the first bounce stores the specular ray's hit
  distance in `PT_SPECULAR_HIT_DIST` (r16f; 0 without a specular ray: a
  diffuse bounce, no surface, lava, the rows 0.5 skips, no bounces); at 0.5
  the rows are (h + 1) / 2, so an odd height's last row is traced too
  (Q2RTX: h / 2); 4.6: the sun at bounce hits with the models around a
  light in its shadow rays, and a specular bounce that hits the sky sees
  GL's sky, a diffuse one the dome (Q2RTX: `env_map` with `remove_sun` for
  both). Sphere lights are not geometry: bounce rays
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
  kinds (cutouts yellow, since 6.4 also skins with alpha, the weapon cyan, chrome red since 5.5, glass pale
  cyan; with `pt_reflect_refract 0`
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
  as last frame did (`get_is_gradient`: the light statistics of two
  frames ago; a list light weighed by the larger of last frame's and this
  frame's style, 4.13, Q2RTX by last frame's, so a light that comes on
  can be picked), with this frame's lights. The visibility buffer's
  barycentrics are 32-bit (`PT_VISBUF_BARY_A/B` R32G32F, 4.13; Q2RTX's
  16-bit ones moved the rebuilt point on Hexen II's large world triangles,
  and penumbra samples flipped: paused gradients along shadow edges,
  rider2c looking up).
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
- **Light styles** (4.13): Q2RTX squares the relative change
  (`get_gradient`), so a style's small steps (castle5's pulse: ~7 % every
  0.1 s, a gradient of 0.005) never trigger the anti-lag, and the HF
  history (up to 50 frames, `flt_min_alpha_color_hf` 0.02) smooths them:
  the denoised pulse ended 0.70× the raw image, ~16 shots (1.3 s) behind.
  A gradient sample whose sampled list light changed its style and is
  unshadowed records the exact relative change
  (`path_tracer_rgen.h`'s `nee_style_change`; `direct_lighting.rgen`
  writes it into its stratum's third channel of `ASVGF_GRAD_HF_SPEC_PING`,
  RGBA16F); `asvgf_gradient_img.comp`'s HF and specular gradients take
  the larger of it times `flt_antilag_style` (4; 0: only the squared one) and the
  squared one. Measured (castle5's pulse, 60 shots 4 frames apart, the
  lit image with `tm_enable 0`, `bloom_enable 0`, `r_maplight_scale 100`,
  against `flt_enable 0`): the rise at 0.93–0.97× raw (scale 1:
  0.88–0.91, 2: 0.90–0.94; `main` 0.70–0.72), the fall 1.17–1.64× (`main`
  1.48–2.34); paused, denoised and raw agree (no bias); the denoised
  image's spatial noise +1–16 % (`main` 0.032–0.062, scale 4
  0.037–0.063; raw 0.23–0.26). Cost about +0.1 ms (castle5,
  1920x1080, Release: the frame 7.34 → 7.48 ms moving, 7.37 → 7.47 paused; direct light +0.01, the
  denoiser +0.04–0.07). The paused LF (red) gradients on dark bounce-lit
  surfaces are below 1 % (≤ 0.004 in linear light, `tm_enable 0`): the
  overlay is added before the exposure, which brightens them in a dark
  view.
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
  `pt_accumulation_rendering`: 4.9 averages paused frames with
  `vk_screenshot` instead).

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

**Since 4.9 the exposure is fixed by default (`tm_auto_exposure 0`).**
The lights are calibrated in GL's units ([Map
lights](#map-lights-vk_maplightsc)), so the lit image is GL's: the tone
mapping pass runs only `tone_mapping_apply.comp`, with its
`fixed_exposure` push constant 2^`r_map_exposure` (1 without a map file):
the image times it, clipped at 1 as GL's, dithered; no histogram, curve or
knee (`tm_knee_start` 0.6 would bend GL's range). Bloom as before; the
effects aren't scaled by the adapted luminance (`vk_ubo.c` gives the
shaders `tm_enable` 0 then: `effects_brightness` 1, GL's colors); the sky
and emissive surfaces were already at GL's fullbright. The pass stays on
(`tm_enable 1`) so that FSR gets its [0, 1] image. Q2RTX's auto exposure
brightens each view's 70th–90th percentile towards a middle grey (up to
5000×) and spreads its histogram over the output: fine for Quake II RTX's
sunlit, panel-lit views, but Hexen II's torch-lit rooms are dark as a
whole, so it lifted them to 1.2–17× GL's brightness (castle4 17×,
measured against `glh2` with HoT's colors) and flattened the contrast.
`tm_auto_exposure 1` still gives it (with `tm_exposure_bias` and
`r_map_exposure`; it starts over when turned on); `tm_enable 0` is
Q2RTX's untone-mapped image (clamped by the composite, no FSR). Neither is
archived (a menu option with 6.10).

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
  `KneeConstants`) and blue noise dither (a step of the 8-bit color the
  composite writes, 4.17), linear [0, 1] out for the composite. Q2RTX's
  `tm_*` cvars (registered since 3.1) with its defaults; `tm_enable` (the
  UBO gets 0 or 1, as the host decides),
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
  `tm_enable 0`, with the fixed exposure (4.9) and in the debug views. `pt_particle_brightness` is 15
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
  calibration against GL (4.9) turned it off by default (above); the map
  file's `r_map_exposure` (4.7) adds a per-map EV to the fixed exposure, or
  to `tm_exposure_bias` with `tm_auto_exposure 1`.

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
| `r_debugview 0-20`, `r_debugview_scale` | 0 the lit image, 1-20 the G-buffer's, lighting and denoiser channels (see [3D view](#3d-view-vk_viewc)); the debug views times this (1, 4.9) |
| `flt_enable 0/1`, `flt_show_gradients 0/1` | the denoiser (Q2RTX's cvar, 1), its gradients over the image (see [Denoiser](#denoiser-vk_asvgfc)); Q2RTX's other `flt_*` cvars tune it |
| `flt_antilag_style` | how strongly a light style's change drops the denoiser's history (4; 0 = only Quake II RTX's squared gradient, the 4.13 light choice stays; see [Denoiser](#denoiser-vk_asvgfc)) |
| `tm_auto_exposure 0/1`, `tm_enable 0/1`, `tm_debug 0-2`, `bloom_enable 0/1`, `bloom_debug 0-3` | 0 (since 4.9) a fixed exposure in GL's units, 1 Q2RTX's auto exposure and curve (its `tm_*` cvars tune them); the tone mapping pass (1; 0 Q2RTX's untone-mapped image, no FSR); the histogram or curve over the view; bloom (`bloom_sigma`, `bloom_intensity`) and its stages (see [Bloom and tone mapping](#bloom-and-tone-mapping-vk_bloomc-vk_tonemapc)) |
| `vk_exposure` | the adapted luminance read back (two frames old) |
| `profiler 0/1`, `profiler_samples`, `vk_profiler`, `vk_benchmark 0/1` | the GPU timers over the screen, the frames they average (60); printed; full load for measuring (no 72 fps cap, no sleep when unfocused) (see [Profiler](#profiler-vk_profilerc)) |
| `r_scale 25-100`, `r_upscaler 0-4`, `vk_upscale` | the render size in percent of the view's; 0 TAA, 1 TAAU, 2 FSR 1 (`flt_fsr_easu`, `flt_fsr_rcas`, `flt_fsr_sharpness`), 3 DLSS SR, 4 DLSS RR; the last frame's sizes, jitter and passes (see [Upscaling](#upscaling-vk_upscalec)) |
| `vk_dlss`, `r_dlss_preset` | Streamline's state (DLL, signature, support, versions, driver, evaluations, its log's warnings and errors), DLSS's images, mode and render sizes; DLSS's model: 0 its default, or a preset letter (see [DLSS](#dlss-vk_dlssc-vk_streamlinecpp)) |
| `pt_particle_brightness` | the effects' brightness under the exposure (15) |
| `pt_num_bounce_rays 0/0.5/1/2` | bounces (Q2RTX's cvar, 1); Q2RTX's other `pt_*` cvars, e.g. `pt_roughness_override`, `pt_metallic_override` (−1 = off) to test reflections |
| `pt_reflect_refract 0-10` | reflection and refraction passes (Q2RTX's cvar, 2) |
| `r_lerpmodels`, `r_lerpmove` | frame and movement blending (1) or GL's look (0) |
| `r_dumpscene` | the last frame's scene |
| `vk_info` | device, extensions, BC texture support and the largest image, swapchain, validation counts |
| `vk_imagefile <file> [scale]`, `vk_imagefile` | 5.2: reads an image file of [MATERIALS.md](MATERIALS.md) (without an extension: `.png`, `.tga`, `.dds`, `.ktx2` in turn), prints what it read and the times, shows it at the top left, scale screen pixels per texel (0 fits); alone hides it (see [Image files](#image-files-vk_imagefilec)) |
| `vk_textures [list]` | texture slots (`list`: each slot's size, mips, alpha, the CRC of the pixels it was loaded from, which a material file's `~<crc>` names, [MATERIALS.md](MATERIALS.md), and its name) |
| `r_reloadmaterials`, `r_materials 0/1`, `vk_materials [list\|problems\|here]` | 5.3: the material files again (the new and changed ones read, the materials applied; no map reload); the material files apply (1) or the original textures only (0, not archived; A/B comparisons); the index, textures with files, images, problems; each texture's files; the problems; 5.6: the texture at the view's center, its file names, files and albedo against the original's (see [Material files](#material-files-vk_matfilesc), [AUTHORING.md](AUTHORING.md)) |
| `r_exporttextures [folder]` | 5.4: every original texture of the search path's maps, models, sprites and stone and ice pictures as a PNG under its material file name, exactly as uploaded, and `textures.csv`, into `<game folder>/export` (or folder; run with `-portals` for both games; see [Texture export](#texture-export-vk_exportc)) |
| `vk_world [materials]` | world buffer statistics, animation check |
| `vk_pvs` | PVS statistics, shader check |
| `vk_instances [step\|box]` | model instances, gliding monsters, pose bounds |
| `vk_models [list\|check]` | alias models, GPU-vs-CPU triangle check |
| `vk_effects [check]` | particles/sprites, effects TLAS ray check |
| `r_specular` | the materials' specular factor where no roughness is authored (0 since 4.9: matte, as GL; 1 Quake II RTX's; see [Materials](#materials-vk_materialc)) |
| `vk_accel` | acceleration structure sizes, build times |
| `vk_rtcheck` | ray grid vs. CPU hull traces |
| `vk_rayprobe x y z` | hits of one ray towards a point |
| `vk_images` | render targets and the blue noise |
| `vk_testlight sphere, dlight, line, quad, list, clear` | test lights, added to the map's (see [Lights](#lights-vk_lightc)) |
| `r_maplights 0/1`, `r_maplight_scale`, `r_maplight_power`, `r_maplight_range`, `r_maplight_colors 0/1` | the map's lights off/on (1), the intensity of a level 300 one (630 since 4.17; 740 in 4.9), intensity as (level / 300) to this power (3), the range as the level times this (1) (these three: the physical shapes; the scale and power also dynamic lights), white (0 since 4.9) or HoT's colors (1; archived; see [Map lights](#map-lights-vk_maplightsc)) |
| `r_maplight_shape 0/1/2`, `r_maplight_gl_scale`, `r_maplight_gamma`, `r_maplight_radius` | the map lights' light shape (4.15): 0 physical (inverse square, the cosine: the "physically based" mode, R103; the default in 4.21, R107), 1 physical with utils/light's angle term, 2 utils/light's lightmap value of each light (the default, "original"; again since 4.22, R108); shape 2's factor for a light the fit has none for, and every light's with `r_maplight_fit 0` (2; 1 = a lone light, the texture's own color at a full texel); the power that takes GL's lightmap values into linear light (2.2); the spheres' radius (8: the shadows' softness) |
| `r_maplight_fit 0/1`, `r_maplight_fit_scale` | 4.16: shape 2's factors per light list entry fitted to the map's lightmaps (1), or `r_maplight_gl_scale` for all (0); the fitted factors times this (1, 4.17: GL's look; 1.1 before, with the sRGB curve) |
| `vk_lights`, `vk_lights stats`, `vk_lights cull 0/1`, `vk_lights colors`, `vk_lights fit` | light lists, light statistics read back, range culling off/on, each map light's color, the light fit scored on the texels it didn't use (4.16; with `r_maplight_shape 2`, the fit's mode) |
| `r_lava_light 0/1`, `r_emissive_scale`, `r_emissive_models 0/1` | lava emits and lights, without the mappers' fake lava lights (1), or GL's look (0); the emission of a texture color of 1 (32; also the effect lights' and the glowing projectiles'); the light models' flames glow (1) (see [Emissive surfaces](#emissive-surfaces-vk_emissivec)) |
| `r_effect_lights 0/1` | 6.2: the fire, explosion, flash and spark sprites light the scene and glowing projectiles glow instead of being lit by their light (1), or GL's look (0; not archived); `vk_effects` and `vk_lights` print them (see [Effect lights](#effect-lights-vk_effectlightc)) |
| `r_srgb 0/1` | 4.17: the 8-bit colors (textures, the sky, light colors, the image) are the 2.2 power of linear light (0, GL's product with the lightmap) or the sRGB curve's (1; archived; the flames' emissive textures follow with the next map; see [Textures](#textures-vk_texturec)) |
| `r_skyalpha`, `r_sky_light 0/1`, `r_sky_light_scale`, `vk_sky` | the sky's front layer opacity (GL's cvar, 0.67); the sky lights nothing (0, faithful) or diffuse bounces gather a dome of its average color (1) times the scale (1); the sky, the mode, the dome and the sun (see [Sky](#sky-vk_skyc)) |
| `r_sun 0/1`, `r_sun_intensity`, `r_sun_color`, `r_sun_elevation`, `r_sun_azimuth`, `r_sun_angle` | a sun in the sky light mode (0): 1 lights a white surface facing it as GL's fullbright; an 8-bit color (1 1 1); direction in degrees (45, 45: the azimuth from +x towards +y); the disc's width (1°). These and the sky light cvars are per map: reset at every map load, set by the map file |
| `vk_mapfile [reload]`, `r_map_light_scale`, `r_map_exposure` | the map file used, its settings and light lines, unsaved edits; read it again and apply it (unsaved edits dropped); per-map: every map light's intensity times this (1), EV added to `tm_exposure_bias` (0) (see [Map file](#map-file-vk_mapfilec)) |
| `r_editlights 0/1`, `r_editlights_distance` | markers at the lights in sight and the selected light's panel (0); how far they reach (1024) (see [Light editor](#light-editor-vk_lighteditc)) |
| `vk_setpos x y z [pitch yaw]`, `vk_bookmark <name>`, `vk_screenshot <name> [frames]` | the player there (single player; `save` on the same line keeps the pitch); a calibration bookmark into the game folder's `bookmarks.txt`; `shots\<name>.tga`, frames averaged in linear light (see [Calibration](#calibration-vk_calibc)) |
| `vk_editlight select [x y z\|none]`, `vk_editlight <changes>`, `add`, `reset`, `save`, `help` | the light at the crosshair or by entity origin; `off`, `on`, `level n\|*f`, `scale f\|*f`, `color r g b`, `style n`, `origin x y z\|eye\|cursor`, `move dx dy dz`; a new light at the eye; the map's own light again; the map file into the game folder |
| `r_dlights 0/1`, `gl_colored_dynamic_lights 0/1`, `gl_extra_dynamic_lights 0/1` | the game's dynamic lights off/on (1); their colors (HoT's option, 0 as in HoT since 4.9) and the client's extra projectile lights (0 as in HoT: they count for gameplay; the renderer makes its own; see [Lights](#lights-vk_lightc)) |
| `r_darklights 0/1`, `vk_darkplaces [n] [threshold]` | GL's dark lights (the invincible Necromancer) darken the world (1; 4.10, see [Darkness](#darkness-shadersdarknessglsl)); the map's dark places where an item or monster stands, with a point to go to (a local game; see [Calibration](#calibration-vk_calibc)) |
| `vk_reload_shaders` | rebuild pipelines from the SPIR-V on disk |
