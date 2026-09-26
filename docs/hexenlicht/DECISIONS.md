# Hexenlicht — decision log

Technical decisions made while building Hexenlicht, one line each, with the
story (issue) or PR where they were made. The project-level decisions (engine,
renderer, scope, licensing, DLSS) are in [PLAN.md](PLAN.md) §1–§5; how the
code implements these is in [RENDERER.md](RENDERER.md). Add a line when a
story settles something a later session must not undo; mark a line
*superseded* rather than deleting it.

## Process

| # | Decision | Where |
|---|---|---|
| P1 | The owner drives: every story is researched and proposed (one recommendation with reasons, what is left out, deviations from GL), implemented only after approval | PLAN §10 |
| P2 | One story → branch `story/<num>-<slug>` from up-to-date `main` → PR with `Closes #<issue>`; the owner merges (merge commits). Story PRs carry no labels or milestone (issues do; upstream sync PRs get the label `upstream`). Board: "In progress" when starting, "In review" with the PR; CI Auto-fix on, never auto-merge | E0 |
| P3 | GitHub doesn't close an epic when its last story closes: close it and set it Done by hand | E0 |
| P4 | New work found mid-story becomes a new story issue (sub-issue of its epic, board Backlog with Epic/Size) and a PLAN §8 row in that story's PR | 1.8 |
| P5 | An independent code-review subagent reviews every story before its commit; its findings are applied and re-tested | 2.4a |
| P6 | PLAN.md changes only when a decision or the scope changes; this log and RENDERER.md change with the code | — |
| P7 | Session knowledge lives in the repo (CLAUDE.md index + these docs), not in hand-off files | docs PR after 3.1 |

## Build and platform

| # | Decision | Where |
|---|---|---|
| B1 | CMake + MSVC + Ninja presets; the upstream Makefiles stay untouched; `glh2.exe` (unmodified GL client) is built alongside as the look reference | 0.3 |
| B2 | `hexenlicht` reuses the GL renderer files without GL calls (`gl_model.c`, `gl_mesh.c`, `r_part.c`, `gl_refrag.c`, `gl_screen.c`) and is compiled with `GLQUAKE` + `HEXENLICHT`; edits to upstream files are minimal, `#if defined(HEXENLICHT)`, and listed in UPSTREAM.md | 1.1, 1.6 |
| B3 | Shaders are loose SPIR-V next to the exe (`shaders/*.spv`), compiled at build time with `-DVKPT_SHADER`; no relink for shader changes; `vk_reload_shaders` reloads them in the running game | 1.4, 3.1 |
| B4 | `hexenlicht.exe` keeps its settings in `hexenlicht.cfg` and reads `config.cfg` until that exists | 1.8 |

## Window and 2D

| # | Decision | Where |
|---|---|---|
| W1 | One window for the program's lifetime; fullscreen is borderless at desktop resolution; `vid_restart` only restyles/resizes | 1.2 |
| W2 | Resizable, maximizable window (1.9); per-monitor DPI aware, sizes in physical pixels (1.2) | 1.2, 1.9 |
| W3 | Integer 2D UI scale (`vid_uiscale`, 0 = auto: the largest keeping >= 640x480); the 2D screen is centered | 1.6 |
| W4 | Textures are sRGB with GPU mips; the swapchain is UNORM with sRGB color space and the last pass encodes; the `gamma` cvar is applied in the shader | 1.3, 1.5, 1.6 |

## Scene and geometry

| # | Decision | Where |
|---|---|---|
| G1 | GPU data follows Quake II RTX's formats (`VboPrimitive`, material IDs and table, `ModelInstance`, AS masks, the instanced buffer, model groups incl. the viewer weapon, the effects TLAS), so its shaders can be imported | 2.1–2.8 |
| G2 | The 3D renderer reads only `r_scene` (filled by `R_RenderView`); entities are not culled to the view | 1.7 |
| G3 | A cluster is a vis leaf (leaf number - 1); the PVS is made symmetric and connected across water/slime/translucent (not lava) | 2.1, 2.2 |
| G4 | World triangles facing into solid (qbsp leftovers) are dropped | 2.1 |
| G5 | Frame blending (`r_lerpmodels 1`, archived, default) over each entity's own measured interval (Hexen II animates at 20 or 10 Hz); 0 = GL's poses | 2.4a (#26) |
| G6 | Movement blending for stepping monsters (`r_lerpmove 1`, archived, default), QuakeSpasm's but over the interval between the monster's last two moves; a move before the glide ends glides on from the shown position (QuakeSpasm jumps first); attached lights/trails/sounds stay at the server position; 0 = GL's steps | 2.9 (#108, PR #114) |
| G7 | Translated player skins are translated in 8 bits and keep their cutouts (GL loses the Demoness's); `TEX_SPECIAL_TRANS` alpha is stored as opacity | 2.4b (#105) |
| G8 | The first-person weapon: GL's fov compensation above 90 (no separate gun FOV), its own model group and BLAS, traced first from GL's 4-unit near plane so it stays in front of walls | 2.8 (#30, PR #113) |
| G9 | `cl.light_level` (sent to the server: monster awareness, Assassin cloak) is computed as GL's `R_DrawViewModel` does | 2.8 |
| G10 | Particles are GL's camera-facing triangles with its dot texture and colors; sprites all five orientation types, unlit; `SPR_FACING_UPRIGHT` uses the sprite's own direction (GL's stale `modelorg`; unused by the game) | 2.5 (#27, PR #112) |
| G11 | Effects live in a second, effects-only TLAS and never block rays through the main TLAS | 2.5 |
| G12 | Translucent models and surfaces are opaque in the debug view until 6.4 (the main TLAS is force-opaque except cutouts, so water and translucent surfaces hide the effects behind them); a translucent weapon will need effects blended behind it | 2.4b, 2.8 |
| G13 | The sunstaff beam report (2.10) was not a bug: the engines' `screenshot` capture different frames; compare paused | 2.10 (#111, PR #115) |

## Path tracer

| # | Decision | Where |
|---|---|---|
| R1 | Ray queries only (Q2RTX's `KHR_RAY_QUERY` path); no ray-tracing pipelines, SBTs or hit shaders; the `#ifdef KHR_RAY_QUERY` branches stay so 7.2 could add pipelines for SER | 3.1 (#31, PR #116) |
| R2 | Q2RTX's shader names over our bindings: set 0 global UBO, set 1 render targets (even/odd), set 2 bindless textures; all other buffers by device address from the UBO | 3.1 |
| R3 | Q2RTX shaders come in near-verbatim with its file names, changes marked `Hexenlicht:`; host code is adapted into our modules and style | 3.1 |
| R4 | Render targets at the swapchain's size, recreated with it; the 3D view renders into their top left; images come with the passes that use them | 3.1 |
| R5 | Q2RTX's global UBO is imported whole; its `UBO_CVAR_LIST` cvars are registered with Q2RTX's defaults and are inert until their pass | 3.1 |
| R6 | Triangles of brush entities take their instance's cluster (Q2RTX's `load_and_transform_triangle`) | 3.1 |
| R7 | `r_debugview` defaults to 1 (albedo) until the path tracer shows a lit image | 2.7 |
| R8 | Open: the weapon's light never goes below 24 in GL — a path-traced weapon in the dark needs a decision (3.3/E4) | 2.8 |
| R9 | The G-buffer is Q2RTX's primary rays' (`primary_rays.rgen`) in its two checkerboard fields, the render width rounded up to even (at an odd view width the view shows one column less of it: under a pixel); the debug view only shows its channels (it traces no rays of its own) | 3.2 (#32) |
| R10 | Random numbers: Q2RTX's `get_rng` over Christoph Peters' CC0 blue noise (`libs/bluenoise`: 64 textures of 64x64 16-bit RGBA = 256 layers); Q2RTX's own 128 textures have no license note | 3.2 |
| R11 | Primary rays don't cull back faces (GL draws `EF_SPECIAL_TRANS` models two-sided, one TLAS instance holds all alias models); normals face the ray, as in Q2RTX | 3.2 |
| R12 | The sky is an empty surface in the G-buffer, black until the sky (4.6) | 3.2 |
| R13 | Liquids warp per pixel with Q2RTX's `lava_uv_warp`, which is Hexen II's software renderer's turbulence (AMP 8, SPEED 20, CYCLE 128, game time); GL's per-vertex warp of subdivided polygons is not reproduced | 3.2 |
| R14 | A model's `colorshade` tint: its hue, scaled to at most 1, multiplies the base color (`get_material`); what GL's tints above 1 brighten is left to the lighting (E4) | 3.2 |
| R15 | No images only for DLSS Ray Reconstruction: its inputs come from the G-buffer (RENDERER.md, 3D view); the specular hit distance comes with the reflections (3.5) | 3.2 |
| R16 | Q2RTX's real-time settings for primary rays: no depth of field (`pt_aperture` 0; Q2RTX only uses it when accumulating), no jitter until TAA (3.8) | 3.2 |

## Open questions carried forward

See [Q2RTX.md](Q2RTX.md#open-questions-for-later-stories): water normal map
(3.5, 6.5), specular hit distance (3.5), checkerboard fields and RR (3.9),
model tint brightness (E4), instance history for A-SVGF (3.6), effects
brightness (3.3).
