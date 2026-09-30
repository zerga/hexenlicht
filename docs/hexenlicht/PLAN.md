# Hexenlicht — Project Plan

Hexenlicht is a path-traced fork of Hexen II: Hammer of Thyrion (uHexen2).
Goal: play the complete Hexen II campaign and the Portal of Praevus mission
pack with a real-time path-traced renderer on Vulkan, with PBR material
support so reworked textures can shine, while keeping the original mood.

This document holds the **settled decisions and the scope breakdown**. Live
status (what is in progress / done) lives in the issue tracker, not here —
see [Tracking](#tracking).

---

## 1. Settled decisions

| Topic | Decision |
|---|---|
| Base engine | Hammer of Thyrion (uHexen2), tracking upstream `sezero/uhexen2` |
| Renderer | Own Vulkan path tracer, reusing code from NVIDIA's Quake II RTX (`src/refresh/vkpt`, GPLv2-or-later). Q2RTX was archived on 2025-12-11: we take code from it, we do not track it. Ray queries only (Q2RTX's ray-query path, no ray-tracing pipelines); its module map and the import rules are in [Q2RTX.md](Q2RTX.md) |
| RTX Remix | Not used |
| Platform | Windows, x64 only, Win32 window/input layer (no SDL for now) |
| Toolchain | CMake + MSVC, developed in CLion. Existing Makefiles stay untouched |
| Target hardware | RTX 4070 Ti, 1440p, 60+ fps with upscaling |
| Game scope | Hexen II + Portal of Praevus: single-player and LAN co-op. **HexenWorld is out of scope** |
| Lighting | Fully dynamic path-traced lighting from the maps' own light entities; baked lightmaps unused. The original mood is a sanity reference (overall brightness, gameplay darkness), not a target (R107). By default a light's first arrival is GL's (its shape and sum, "Original"), everything after it path traced (shadows, bounce light), the lava emissive; "Physically based" (inverse square) is an option (R108) |
| Sky | Two modes per map: *faithful* (sky visible, not emissive) and *sky light* (dome + optional sun). Default chosen per map during calibration |
| Materials | Own file-based PBR format ([MATERIALS.md](MATERIALS.md), §6), looked up by Hexen II texture name |
| Upscaling / denoising | Built-in A-SVGF + TAAU, FSR (MIT); DLSS SR/RR **optional** via Streamline with user-supplied NVIDIA DLLs (§5) |
| Other renderers | Software (`hexen2`) and OpenGL (`glhexen2`) renderers stay untouched; `glhexen2` is the look's sanity reference |
| Hosting | Public GitHub repo `hexenlicht` |

### Non-goals (for now)
- Linux/macOS, SDL, 32-bit builds.
- HexenWorld client/server.
- Non-ray-traced fallback renderer inside Hexenlicht (use `glhexen2`).
- Mesh replacements / high-poly models (possible later).

---

## 2. Architecture

```
Hexen II game/client code (unchanged apart from minimal, #ifdef-guarded hooks)
   │   calls R_*, Draw_*, SCR_*, GL_LoadTexture, VID_*   (compile-time renderer seam)
   │
   ├── hexen2.exe / glhexen2.exe      — existing renderers, untouched
   └── hexenlicht.exe                 — NEW
        ├─ vid_vk.c         Win32 window, modes, input (from gl_vidnt.c) + Vulkan device/swapchain
        ├─ scene            per-frame frame description: camera, entities, dlights,
        │                   light styles, particles, view blend, 2D draw list
        ├─ geometry         BSP world + brush models + MDL models + sprites/particles
        │                   → GPU buffers → BLAS/TLAS
        ├─ path tracer      primary / direct / indirect passes (GLSL, ray query)
        ├─ lights           map light entities, light styles, dlights, emissive surfaces, sky/sun
        ├─ materials        PBR texture sets + .mat files, defaults for missing maps
        ├─ post             A-SVGF, tone mapping, bloom, upscaler interface (TAAU / FSR / DLSS)
        └─ 2D               rasterized overlay: console, menus, HUD, loading plaque
```

The renderer seam already exists: the renderer is selected at compile time
(software vs `GLQUAKE`). Hexenlicht is built as a third client target that
keeps the `GLQUAKE` semantics for the ~60 hardware-renderer branches in the
client code, replaces the GL renderer files (`gl_rmain.c`, `gl_rsurf.c`,
`gl_draw.c`, `gl_warp.c`, `gl_rlight.c`, `gl_rmisc.c`, GL parts of
`r_part.c`) and reuses `gl_model.c`, `gl_mesh.c`, `gl_refrag.c`,
`gl_screen.c` and the rest of `r_part.c`.

### Code organisation
- New code under `engine/hexenlicht/` (renderer, vid layer) and
  `engine/hexenlicht/shaders/`.
- Third-party libraries vendored under `libs/` (volk, VMA, stb_image,
  DDS/KTX2 loader, FSR, Streamline's headers; its interposer is one of the
  player's DLLs, PLAN §5), each with its license.
- Edits to shared engine files: minimal, guarded with `#ifdef HEXENLICHT`,
  so upstream merges stay easy.

---

## 3. Repository, branching, upstream

- Standalone public GitHub repo `hexenlicht` (not a GitHub "fork", to avoid
  fork quirks: issues disabled by default, PRs defaulting to upstream, forks
  not indexed by code search) containing the **full upstream history**.
- Remotes: `origin` = our repo, `upstream` = `sezero/uhexen2`.
- `main` = Hexenlicht. `upstream/master` is merged into `main` periodically.
- Feature branches per story, merged via PR with merge commits.
- README states clearly: unofficial fork of Hammer of Thyrion, credits
  upstream, Raven, id Software and Q2RTX authors; game data not included.

---

## 4. Licensing rules

- The codebase is GPLv2-or-later (Raven/id headers). Q2RTX code is
  GPLv2-or-later. Combined: GPLv2-or-later. New files carry the same header.
- Code copied from Q2RTX keeps its original copyright lines (Christoph Schied,
  NVIDIA, id) plus ours.
- Vendored libraries must be GPL-compatible: MIT, BSD, zlib are fine.
  Apache-2.0 is only GPLv3-compatible; it is legally usable (all our code is
  "or later") but would make the binary GPLv3 — avoid unless there is no
  alternative, and decide explicitly.
- **Never in the repo or releases:** Hexen II game data (`pak*.pak`),
  textures derived from Raven's textures, NVIDIA DLSS/NGX binaries or static
  libraries.
- Texture packs are distributed separately from the engine.
- No "RTX" or official-looking branding in the name, logo or UI. Naming
  Hexen II / DLSS descriptively is fine.

---

## 5. DLSS strategy

- The engine never links NVIDIA's NGX static libraries and releases never
  contain NVIDIA DLLs. NVIDIA's own position (Q2RTX issue #68) is that DLSS
  cannot be integrated into a GPL project; the RTX SDKs License §4(e)
  forbids use that makes the SDK subject to an open-source license.
- Integration via **Streamline** (MIT). The DLSS parts
  (`sl.dlss*.dll`, `nvngx_dlss.dll`, `nvngx_dlssd.dll`) are downloaded by the
  player from NVIDIA and placed next to the exe (same model as `vkquake-rt`).
- The engine must be fully functional without them (A-SVGF + TAAU/FSR).
- The G-buffer is designed to feed both A-SVGF and DLSS Ray Reconstruction:
  diffuse albedo, specular albedo, shading normals, roughness, depth, motion
  vectors, specular motion vectors / hit distance.
- Verified by the 3.9 spike (Streamline 2.14.1, NGX 310.9.1, RTX 4070 Ti):
  SR and RR run on our Vulkan device, loaded at runtime (Vulkan through
  `sl.interposer.dll`). The player copies `sl.interposer.dll`,
  `sl.common.dll`, `sl.dlss.dll`, `sl.dlss_d.dll`, `nvngx_dlss.dll` and
  `nvngx_dlssd.dll` from Streamline's release zip on NVIDIA's GitHub;
  Streamline's over-the-air updates stay off. Details: DECISIONS R52–R57.
- Not legal advice. Revisit if the project grows (e.g. ask the Software
  Freedom Conservancy).

---

## 6. Material format

Frozen in 5.1: [MATERIALS.md](MATERIALS.md) (DECISIONS M1–M7). In short:
texture sets in the game's filesystem (loose files or inside a `.pak`),
looked up by the Hexen II texture name under `textures/` (`*` as `#`; a
`~<crc>` qualifier for the 100 names whose pixels differ between maps
and the two skins that differ between the games);
one image per map as tools write them: albedo, `_n` normal (OpenGL
convention), `_r` roughness and `_m` metallic or a packed `_orm`, `_e`
emissive, every one optional; albedo and emissive are 8-bit colors as the
originals, the rest data; an optional `.mat` (`kind`, `roughness`,
`metallic`, `bump`, `specular`, `emissive`); a texture without files
keeps its original, matte look, and only an authored roughness makes a
surface physically based. Authoring in PNG or TGA, shipping in DDS or
KTX2 (BC7, BC5).

- Output from AI tools (Remix AI texture tools, PBRify in chaiNNer,
  Substance Sampler, Materialize) is renamed to these conventions — the
  export command in E5 writes the originals with correct names to start from.

---

## 7. Lighting approach

- The designers placed light entities in every map (`light`,
  `light_torch_small_walltorch`, `light_flame_*`, `light_gem`, `light_globe`,
  `light_newfire`, ...). The offline `utils/light` compiler baked lightmaps
  from them; the entities remain in every BSP's entity lump.
- Hexenlicht turns those entities into real lights and path-traces the scene
  every frame (shadows, bounce light, reflections, moving lights).
- Light `style` values drive flicker/pulse/switchable lights from the same
  per-frame light style strings the client already receives
  (`lightstyle`/`lightstylestatic` builtins).
- Original lights are white; HoT's colored light comes from `.lit` files
  generated by `utils/jsh2color`'s texture heuristic. That heuristic is
  reused to color lights.
- `utils/light` has **no sky or sun lighting**: outdoor areas were lit by
  point lights. Hence the two sky modes (§1).
- Quake-family lights have linear falloff with a hard range; physical lights
  have inverse-square falloff. Calibration (E4) compares the same camera
  positions in `glhexen2` and Hexenlicht and tunes one global curve (the plan
  was to fix outliers per map through a per-map override file; since 4.11
  what is off is fixed for every map, below). Since 4.15 the default
  curve is utils/light's own (each light gives a surface its lightmap
  value; [DECISIONS.md](DECISIONS.md) R95): not physically based for the
  light's first arrival, path traced after it; the physical shape stays a
  setting. Since 4.16 each light's sum with the others is GL's too, fitted
  to the map's own lightmaps (R102). Together they are the "Original"
  lighting mode, the default; the physical shape without them is the
  "Physically based" one (R103; the default in 4.21, R107, until the
  owner took it back in 4.22, R108). GL is a sanity reference, not a
  target (R107). Since 4.17 the 8-bit colors are GL's product (the 2.2
  power, R104). The hubs (4.11) are reviewed without shipped per-map files (owner,
  2026-09-28): a difference with a renderer-wide cause is fixed for every
  map, a hub is done when it passes GL's sanity check and the owner's play
  (R105, R107); the map file (4.7, 4.8) stays for players, mods and
  experiments.

---

## 8. Epics and stories

Sizes: **S** ≈ one focused session, **M** ≈ a few sessions, **L** ≈ needs
splitting further when we get there. Every story ends with the build passing
and a run in the game; there are no automated tests.

### E0 — Foundation and public repo
Goal: a public repo, a CLion/CMake build of the existing OpenGL client, CI.

| # | Story | Size | Done when |
|---|---|---|---|
| 0.1 | Create GitHub repo `hexenlicht` with full upstream history; `origin`/`upstream` remotes; `main` branch | S | Repo public, history intact, upstream fetchable |
| 0.2 | Repo hygiene: README (what it is, credits, game data setup), `.gitignore` (CLion, build dirs), third-party notice file | S | README renders, no IDE/build junk tracked |
| 0.3 | CMake build of the existing `glhexen2` for MSVC x64 (file lists mirror the Makefile, options from `h2config.h`) | M | Builds in CLion, Debug and Release |
| 0.4 | Baseline run: game data setup (v1.11 paks via `h2patch`, Portal of Praevus), CLion run configuration, docs | S | `glhexen2` from CMake plays both games |
| 0.5 | GitHub Actions: Windows MSVC build, build artifacts | S | CI green on `main` and PRs |
| 0.6 | Vendor dependencies: Vulkan SDK detection, volk, VMA, stb_image | S | Libraries build in the CMake tree |
| 0.7 | Upstream sync procedure documented, first test merge | S | `docs/hexenlicht/UPSTREAM.md` exists, merge done |

### E1 — Vulkan foundation and renderer seam
Goal: `hexenlicht.exe` boots with Vulkan; menus, console and HUD work; maps load with a black 3D view.

| # | Story | Size | Done when |
|---|---|---|---|
| 1.1 | New `hexenlicht` target with stub renderer entry points | M | Links, runs, reaches the console |
| 1.2 | `vid_vk.c`: Win32 window, modes, fullscreen, Alt-Tab, input (from `gl_vidnt.c`) | M | Window/mode switching behaves like `glhexen2` |
| 1.3 | Vulkan core: instance, validation layers (debug), device selection with ray tracing features, queues, VMA, swapchain, resize, `vid_restart` | M | Clear-color frames, no validation errors |
| 1.4 | Shader pipeline: GLSL → SPIR-V at build time | S | Shaders rebuilt by CMake on change |
| 1.5 | Texture manager: `GL_LoadTexture` equivalent (identifiers, palette → RGBA, flags), bindless texture array | M | All game textures upload |
| 1.6 | 2D renderer: `Draw_*`, console, menus, status bar, loading plaque, palette/gamma | M | All 2D screens match `glhexen2` |
| 1.7 | Frame description struct filled by the client each frame; debug dump command | S | Dump shows camera, entities, dlights, light styles |

### E2 — Scene on the GPU
Goal: all geometry and animation correct in a ray-traced debug view.
Order: 2.1–2.3, then 2.6 and 2.7 (so the debug view can check everything after them), then 2.4a, 2.4b, 2.5, 2.8, 2.9, 2.10.

| # | Story | Size | Done when |
|---|---|---|---|
| 2.1 | BSP world → static GPU buffers (triangles, UVs, normals, tangents, material IDs, triangle→leaf), animated textures, surface flags (sky, water, lava) | M | World buffers built on map load |
| 2.2 | Leaf clusters and decompressed PVS on the GPU | S | Visibility queryable from shaders |
| 2.3 | Brush entities (doors, lifts, rotating) as instances with transforms | S | Moving brushes move |
| 2.4a | MDL geometry and animation on the GPU: both MDL formats, compact poses, compute pass writing the frame's model triangles, frame interpolation (`r_lerpmodels`, on by default; 0 = GL's look; blending over the entity's own frame interval, since Hexen II animates at 20 or 10 Hz), scale types/origins, `EF_ROTATE`/`EF_FACE_VIEW` | M | Monsters, items, players animate correctly |
| 2.4b | Model skins and draw state: skin groups, `gfx/skinN.lmp` skins, player class skins (translated 8-bit, so cutouts survive), model flags and draw flags as material kinds/alpha/cutouts, lighting modes and `colorshade` tint recorded | M | Right skins, incl. Praevus models and class skins |
| 2.5 | Sprites and particles as geometry: rebuilt every frame on the CPU as GL draws them (a camera-facing triangle per particle with GL's dot texture, a quad per sprite for all five orientation types), in their own effects TLAS as in Q2RTX (never blocking other rays); the debug view blends them in front of its hit | M | Visible in the debug view |
| 2.6 | Acceleration structures: static world BLAS, per-frame dynamic BLAS, TLAS, instance masks | M | Rebuilt/refit per frame, stable |
| 2.7 | Debug view: primary rays showing albedo / normals / material / instance IDs | S | `r_debugview` cvar works |
| 2.8 | First-person weapon model as GL draws it: GL's fov compensation above 90 (no separate gun FOV), its own model group and BLAS with Q2RTX's `AS_FLAG_VIEWER_WEAPON` and `MATERIAL_FLAG_WEAPON`, traced first so it stays in front of walls like GL's depth hack; plus GL's light level on the weapon (`cl.light_level`), which the game sends to the server for monster awareness and the Assassin's cloak | S | Weapon renders correctly |
| 2.9 | Smooth movement of walking monsters (`r_lerpmove`, QuakeSpasm's, but over the interval between the monster's last two moves, since Hexen II's step at 20 or 10 Hz; needs the server's step bit kept in the client, an upstream hot spot) | S | Walking monsters move smoothly with 1, step like GL with 0 |
| 2.10 | Beam segments (the client's stream entities, e.g. the sunstaff's `stsunsf1/2.mdl`) drawn as GL draws them. Closed without a fix: not a bug. The two engines' `screenshot` capture different frames and the sunstaff's reflected beam changes shape while firing; paused, both match. The thicker horizontal beam is its translucent sheath, drawn opaque until 6.4. Left the diagnostics `vk_rayprobe` and `vk_instances box` | S | Sunstaff, lightning and chain beams match `glhexen2` |

### E3 — Path tracer core
Goal: a test map path-traced, denoised, 60+ fps at 1440p with upscaling.

| # | Story | Size | Done when |
|---|---|---|---|
| 3.1 | Q2RTX code intake: map `vkpt` modules to ours, import framework with headers intact | M | Plan agreed, first modules imported |
| 3.2 | Primary visibility / G-buffer incl. motion vectors (RR-ready layout, §5) | M | G-buffer channels visible in debug view |
| 3.3 | Direct lighting: point/sphere and polygon lights, shadow rays | M | Test lights cast shadows |
| 3.4 | Per-cluster light lists using PVS (Q2RTX approach) | M | Many lights without cost explosion |
| 3.5a | Indirect lighting: diffuse and GGX specular bounces (Q2RTX's `indirect_lighting.rgen`), split from 3.5 | M | Bounce light; glossy reflections with the roughness override (materials are rough until E5) |
| 3.5b | Reflection and refraction pass (Q2RTX's `reflect_refract.rgen`): translucent surfaces and models seen through, mirrors and glass for E5's kinds; decide the water look (GL draws it opaque; lean: GL's until 6.5) | M | Translucent surfaces and models show what is behind them |
| 3.6 | A-SVGF denoiser | M | Stable image at 1 spp |
| 3.7 | Tone mapping, auto exposure, bloom | S | Exposure adapts between areas |
| 3.8 | Upscaler interface + TAAU; FSR backend | M | Render at lower res, upscale |
| 3.9 | **Spike:** Streamline on Vulkan with DLSS SR + RR, user-supplied DLLs | S | Go/no-go documented |
| 3.10 | DLSS backend via Streamline (optional at runtime) + player docs | M | DLSS selectable when DLLs present |
| 3.11 | GPU timers overlay, performance baseline | S | Per-pass timings on screen |
| 3.12 | Translucent surfaces keep a fine checkerboard at some views (found in 3.9; the cause: without the denoiser the fields didn't swap every frame as in Q2RTX) | S | Blend without a checkerboard from every view, also as DLSS RR's input |

### E4 — Hexen II lighting
Goal: every map lit with no manual work; mood matches the original reasonably.

| # | Story | Size | Done when |
|---|---|---|---|
| 4.1 | Light entities → lights (classnames, `light`, `style`, `_color`, torch/flame entities) | M | Map lights appear where designers placed them |
| 4.2 | Light styles: flicker, pulse, switchable lights per frame | S | Matches `glhexen2` animation |
| 4.3 | Light colors from `jsh2color` heuristic / `.lit` data | S | Colored lights plausibly match HoT's |
| 4.4 | Dynamic lights: dlights, effect flags, muzzle flashes, torch-lit models, projectiles; light pool | M | Spells and projectiles light the scene |
| 4.5 | Emissive surfaces: lava (lights instead of the mappers' fake lava lights), the flames of the map lights' models (Hexen II has no fullbright texels; runes and fire textures aren't emissive in GL) | M | Lava lights rooms |
| 4.6 | Sky rendering (GL's two-layer scrolling skies, per pixel) + faithful / sky-light modes (a dome of the sky's color, optional sun; not Q2RTX's physical sky: its data has no license) | M | Both modes switchable (per map with 4.7's file) |
| 4.7 | Per-map override file: add/remove/tune lights, sky mode, sun, exposure | S | Overrides load with the map |
| 4.8 | Live light editing commands (select, move, color, intensity, radius, save) | M | Edit in game, saved to override file |
| 4.9 | Calibration tooling: camera bookmarks, matched screenshots from `glhexen2` and Hexenlicht; global falloff/intensity curve | M | Side-by-side comparison per bookmark |
| 4.10 | Darkness mechanics: total-darkness light style, darkness effects, dark puzzle areas | S | Gameplay darkness preserved |
| 4.12 | Player light level for gameplay (`cl.light_level` from the baked lightmaps and dynamic lights, as GL's `R_DrawViewModel`; found in 1.7, done with 2.8, G9) | S | `cl.light_level` matches `glhexen2` at the same spots; the Assassin only cloaks in the dark |
| 4.13 | Denoiser lags brightening light styles (a light whose last style was 0 is never picked by the gradient samples); paused gradients in bounce-lit areas (found in 4.2) | S | A pulsing or switched-on light's denoised brightness follows the undenoised within a few frames; no paused HF or specular gradients (LF below 1 %) |
| 4.14 | `glh2` shows no dynamic lights on surfaces (its `gl_flashblend` bubbles show they exist; found in 4.4) | S | Cause known; fixed (upstream if it is theirs) or TESTING.md has a GL reference for dynamic lights |
| 4.15 | GL's light shape: utils/light's half-Lambert (0.5 + 0.5 cos) and its 16-unit lightmap texels' soft shadows light surfaces Hexenlicht leaves dark; candidates in the shader (half-Lambert, a GL-shaped falloff, larger spheres), measured with 4.9's tools (found in 4.9) | M | A candidate picked by the owner; the direct light's spread against GL's lightmaps below 4.9's 1.16 stops, or why not documented |
| 4.16 | GL's sum of overlapping lights: GL added a texel's lights before its sRGB step and clipped the sum, 4.15's shape adds them after it with one factor (2), so a lone torch is twice GL's and fill-lit yards are dark; a factor per light list entry fitted to the map's lightmaps at load (found in 4.11a's survey; before 4.11a) | M | The direct light's spread against GL's lightmaps on Blackmarsh well below 1.1–1.2 stops, the lit image as bright as GL's at 4.9's bookmarks, no cost per frame, the physical shape unchanged |
| 4.17 | GL's dark tones: textures and the image in a 2.2 power instead of the sRGB curve (GL multiplied 8-bit colors, which is linear light only under a power; sRGB's linear toe showed dark views at 0.44–0.62 of GL's look while the light matched; found in 4.11a's second survey; before 4.11a) | M | The look per view within about 0.85–1.2 of GL's at 4.11a's 43 Blackmarsh views and 4.9's bookmarks (lava rooms excepted), `r_maplight_fit_scale` 1, the sRGB curve a setting, no cost per frame |
| 4.19 | The torch in the hand: the torch (`EF_DIMLIGHT`) sits at the player's feet, where a physical light on the floor's plane lights the floor almost not at all (it adds 3–22 % of GL's torch's light at 4.11a's dark views); the view entity's own lights held at hand height, physically (found in 4.11a; re-scoped by the owner, 2026-09-29) | M | The torch lights the floor and walls around the player in dark places, a playable amount (GL's as a sanity check), its own shadow on the player's body left out (6.11), no cost per frame |
| 4.21 | Physically based by default: `r_maplight_shape` 0, "Original" a setting, the lava emitting at ×32 in both; GL a sanity reference for the hubs, not a target (owner, 2026-09-29; found in 4.11b's survey) | S | The default switched, DECISIONS, PLAN and TESTING say so, Blackmarsh measured in it |
| 4.22 | Original by default again: `r_maplight_shape` 2 with 4.16's fit, "Physically based" a setting; the rest of 4.21 stays (GL a sanity reference, the lava at ×32 in both modes, 4.18 and 4.20 not planned) (owner, 2026-09-29, before 4.11b) | S | The default switched back, DECISIONS, PLAN, RENDERER and TESTING say so, a run with the defaults shows GL's shape and the fit in use |
| 4.11 | Calibrate hubs: Blackmarsh, Mazaera, Thysis, Septimus, the Eidolon finale, Tulku (Praevus) — one story per hub; each hub's views (`tools/hexenlicht/bookmarks_<hub>.txt`) against GL by TESTING.md's "Calibrating a hub", renderer-wide causes fixed for every map, no shipped per-map files (R105) | M each | Hub within GL's sanity check (R107), played and approved by the owner |

### E5 — Materials and texture pipeline
Goal: edit a PNG, reload in game, see the change.

| # | Story | Size | Done when |
|---|---|---|---|
| 5.1 | Finalise the material spec (§6) | S | Spec frozen in `docs/hexenlicht/MATERIALS.md` |
| 5.2 | Loaders: PNG/TGA (stb_image), DDS/KTX2 (BC5/7; no BC4 since 5.1), colors vs data (UNORM formats, R104), mips | M | All formats load |
| 5.3 | Material system: texture sets, `.mat` parser, defaults, `r_reloadmaterials` hot reload; `get_material`'s reads as MATERIALS.md's "Shader changes" (since 5.1); an index of the files (`quakefs.c`), skins, sprites, lava and flames (L since 5.3: they share the index and the sets) | L | Live reload works |
| 5.4 | Export command: all original textures with canonical names (`~<crc>` for every variant of a name whose pixels differ between maps, 5.1) + manifest CSV; every occurrence in the search path, exactly as uploaded (M since 5.4: four formats, the variants, PNG writing and the round trip) | M | Full export of both games |
| 5.5 | Special materials: water/slime/lava, glass, chrome (since 5.1), sky, animated textures (L since 5.5: the sky's layers from a file, lava's lights averaged on the GPU, the kinds in the world's geometry and on skins, a test set in both games) | L | Correct in both games |
| 5.6 | Test pack (stone, metal, water, emissive, glass) + authoring guide (M since 5.6: `vk_materials here`, the measurements of the dark albedo, PNG decoding and glass on the real panes; [AUTHORING.md](AUTHORING.md)) | M | Pack looks right in game |
| 5.7 | PNG and TGA decoding on threads at map load and `r_reloadmaterials`, the uploads batched (found in 5.6: at 8x the originals a map's PNGs take 2.2 s, 1.75 s of it decoding, against 0.17 s as DDS; the owner will author above 4x; DECISIONS M30) | S | A map with a full PNG set loads as fast as the decoding on the CPU's cores allows, the materials the same as before |

### E6 — Full Hexen II coverage
Goal: an effects checklist of both games, its stories' lines ticked (since 6.2's scope, DECISIONS X4: the lines no story takes are looked at in 7.1's playthroughs).

| # | Story | Size | Done when |
|---|---|---|---|
| 6.1 | Effects inventory from `cl_effect.c`, `cl_tent.c`, `r_part.c` → checklist | S | Checklist in tracker |
| 6.2 | Effects that glow emit light (since 6.2's split proposal, owner 2026-09-30, DECISIONS X4; the rest of "particle effects by group" not planned). The fire, explosion, flash and spark sprites light the scene: a sphere light each from its shown frame's average color and coverage, renderer-only, sharing R81's 32 dynamic spheres; Praevus's fire sprites too. Glowing projectiles (entities owning their dynamic light, R81's light group) emit from their skin instead of being lit by the light inside them. `r_effect_lights` (0 = GL's look). HoT's missile glows left out | M | Explosions and fire light walls and monsters, with shadows, in `effects_run.ps1`'s shots against `r_effect_lights 0`; the scarab, the summoning stone and the tomed purifier's ball glow without being lit by their own light; the cost measured on a busy scene; validation 0/0; the checklist's emitting lines ticked |
| 6.3 | Beams / lightning effects | S | Beams render and emit light |
| 6.4 | Translucency: translucent entities, transparent models, cutout textures, glass (since 5.6: a material file's glass on a translucent brush entity, the game's breakable windows, is half the entity's blend and half glass, DECISIONS M33) | M | Matches original intent |
| 6.5 | Water: refraction, underwater fog/tint | M | Above/below water correct |
| 6.6 | View effects: damage/power-up flashes, underwater warp as post-process (GL's view blends, none drawn yet: also the power-up tints, e.g. the Icon of the Defender's yellow-green, and the hydra's blinding dark flash `df`; found in 4.10) | S | Matches `glhexen2` feel |
| 6.7 | Cutscenes, intermissions, finale screens, demo playback | S | All play correctly |
| 6.8 | Portal of Praevus specifics (Demoness, new effects) | M | Praevus checklist ticked |
| 6.9 | Robustness: save/load, map change, `vid_restart`, Alt-Tab, resize | S | No leaks, no crashes |
| 6.10 | Renderer settings menu (quality presets, upscaler, sky mode; since 4.9 also the exposure mode (fixed, `tm_auto_exposure 1` auto), colored light (`r_maplight_colors`, `gl_colored_dynamic_lights`) and the lava's glow (`r_emissive_scale`); since 4.16 the lighting mode, "Original" or "Physically based" (`r_maplight_shape` 2, the default, or 0: GL's light shape and sum of lights, 4.15–4.16, or inverse square)) | S | Options in the video menu |
| 6.11 | The player's own model in shadows and reflections: in single player without the chase camera the local player's model where the player stands, seen by shadow, bounce, reflection and refraction rays, not by primary rays (Quake II RTX's first-person player model, `AS_FLAG_VIEWER_MODELS`); the player's own lights don't shadow on it (the owner's idea, 2026-09-29) | M | The body's shadow and reflection animated as the server sends it, nothing in the view, a setting, no cost per frame, `cl.light_level` unchanged |
| 6.12 | GL's near plane for the view: GL clips everything within 4 units of the eye (`NEARCLIP`); Hexenlicht's primary rays start at the eye, only the weapon's at the near plane. So the Necromancer's proximity mine, which the gamecode spawns 6 units below the eye with the eye inside its model, blackens the view (found in 6.1, DECISIONS X3). The effect rays too: particles within 4 units of the eye are large blobs, e.g. haste's dark field around the player (found in 6.2's proposal) | S | A model or particle at the eye is left out of the view as in GL, the weapon unchanged, no cost per frame |

### E7 — Playthrough, performance, release
Goal: v1.0 on GitHub Releases.

| # | Story | Size | Done when |
|---|---|---|---|
| 7.1 | Full playthrough per hub, issues logged | M each | Every hub completed |
| 7.2 | Performance pass (shader execution reordering, BLAS update cost, light lists) | M | 60+ fps target met everywhere |
| 7.3 | HDR output (optional) | S | HDR on capable displays |
| 7.4 | Release packaging via CI: zip layout, player README (game data, `h2patch`, optional DLSS download) | S | Release zip works on a clean machine |
| 7.5 | v1.0 release | S | Published |

### Recurring
- Upstream merge from `sezero/uhexen2` when upstream changes — procedure in
  [UPSTREAM.md](UPSTREAM.md).

---

## 9. Risks

| Risk | Mitigation |
|---|---|
| Path tracer effort (E3) | Reuse Q2RTX code; keep the debug view; small stories |
| Mood mismatch (physical vs linear falloff) | Calibration tooling (4.9), GL's light shape and sum (4.15, 4.16) as the default "Original" mode (since 4.16, again since 4.22, R108), GL's colors (4.17), GL's brightness as a sanity check for each hub rather than a target (4.21, R107), renderer-wide fixes for what the hubs show (4.11; no shipped per-map files); inverse square as the "Physically based" option |
| Denoiser artefacts with translucency, particles, flickering lights | RR-ready G-buffer, handle translucency in a separate path as Q2RTX does |
| Streamline lacking RR on Vulkan | Retired by spike 3.9: RR runs on our device; A-SVGF stays the default (RR costs more) |
| Q2RTX archived — no upstream fixes | We own the imported code from day one |
| Upstream merge conflicts | New code in new files, minimal `#ifdef HEXENLICHT` hooks |
| Legal (DLSS, texture packs, branding) | Rules in §4 and §5 |

---

## 10. Working agreement

- The project owner drives: Claude proposes each story's approach, the owner
  approves, Claude implements.
- One story → one branch → one PR; commits reference the issue number.
- Verification = build (CLion/CMake and CI) + run the game; screenshots for
  visual changes.
- Open questions go into the tracker as issues labelled `question`.

---

## 11. Tracking

GitHub Issues + a GitHub Project board:
[issues](https://github.com/zerga/hexenlicht/issues),
[project board](https://github.com/users/zerga/projects/1).

- **Epics** = one issue per epic (E0–E7, issues #1–#8), label `epic`.
- **Stories** = sub-issues of their epic, label `story` (or `spike`), titled
  with their plan number (e.g. `3.9 Spike: ...`). The epic shows a progress
  bar from its sub-issues.
- **Milestones**: `0.1 Boots on Vulkan` (E0, E1), `0.2 First path-traced
  frame` (E2, E3), `0.3 Lit and textured` (E4, E5), `0.4 Complete game` (E6),
  `1.0` (E7).
- New work found along the way becomes a new story under the right epic;
  this plan is updated only when a decision or the scope changes.
- **Labels**: `area:build`, `area:vid`, `area:2d`, `area:geometry`,
  `area:pathtracer`, `area:lighting`, `area:materials`, `area:effects`,
  `area:release`, `question`, `bug`, `upstream`.
- **Project board** columns: Backlog → Ready → In progress → In review → Done;
  custom fields: Epic, Size (S/M/L).
