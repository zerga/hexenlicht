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
| G10 | Particles are GL's camera-facing triangles with its dot texture and colors; sprites all five orientation types, unlit; `SPR_FACING_UPRIGHT` uses the sprite's own direction (GL's stale `modelorg`; unused by the game); *scaled by the exposure since 3.7 (R41)* | 2.5 (#27, PR #112) |
| G11 | Effects live in a second, effects-only TLAS and never block rays through the main TLAS | 2.5 |
| G12 | Translucent models and surfaces are opaque in the debug view until 6.4 (the main TLAS is force-opaque except cutouts, so water and translucent surfaces hide the effects behind them); a translucent weapon will need effects blended behind it; *since 3.5b translucent surfaces and models are seen through, effects behind them show (R33); water stays opaque (R31)* | 2.4b, 2.8 |
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
| R7 | `r_debugview` defaults to 1 (albedo) until the path tracer shows a lit image; since 3.3 (R19): until the maps have lights (4.1) | 2.7, 3.3 |
| R8 | Open: the weapon's light never goes below 24 in GL — a path-traced weapon in the dark needs a decision (E4; 3.3 lights the weapon like everything else) | 2.8, 3.3 |
| R9 | The G-buffer is Q2RTX's primary rays' (`primary_rays.rgen`) in its two checkerboard fields, the render width rounded up to even (at an odd view width the view shows one column less of it: under a pixel); the debug view only shows its channels (it traces no rays of its own) | 3.2 (#32) |
| R10 | Random numbers: Q2RTX's `get_rng` over Christoph Peters' CC0 blue noise (`libs/bluenoise`: 64 textures of 64x64 16-bit RGBA = 256 layers); Q2RTX's own 128 textures have no license note | 3.2 |
| R11 | Primary rays don't cull back faces (GL draws `EF_SPECIAL_TRANS` models two-sided, one TLAS instance holds all alias models); normals face the ray, as in Q2RTX | 3.2 |
| R12 | The sky is an empty surface in the G-buffer, black until the sky (4.6) | 3.2 |
| R13 | Liquids warp per pixel with Q2RTX's `lava_uv_warp`, which is Hexen II's software renderer's turbulence (AMP 8, SPEED 20, CYCLE 128, game time); GL's per-vertex warp of subdivided polygons is not reproduced | 3.2 |
| R14 | A model's `colorshade` tint: its hue, scaled to at most 1, multiplies the base color (`get_material`); what GL's tints above 1 brighten is left to the lighting (E4) | 3.2 |
| R15 | No images only for DLSS Ray Reconstruction: its inputs come from the G-buffer (RENDERER.md, 3D view); the specular hit distance comes with the reflections (3.5); *the one exception: R28* | 3.2 |
| R16 | Q2RTX's real-time settings for primary rays: no depth of field (`pt_aperture` 0; Q2RTX only uses it when accumulating), no jitter until TAA (3.8) | 3.2 |
| R17 | Direct lighting is Q2RTX's, with its two kinds of lights: polygon lights in the light buffer, sampled from the receiving cluster's light list, and up to 32 sphere lights in the UBO, picked uniformly; one light sample and shadow ray per pixel; Q2RTX's units (a sphere's color is π × its radiance, a polygon's its radiance; inverse square) until 4.9 calibrates them; *partly superseded by R21 (spheres in the lists)* | 3.3 (#33) |
| R18 | Until E4 the lights are test lights (`vk_testlight`: spheres and quads at the eye, cleared on map change); every cluster's list holds every polygon light until 3.4 culls by the PVS; *the lists: superseded by R22* | 3.3 |
| R19 | `r_debugview 0` is the lit image (no denoiser, exposure or tone curve until 3.6–3.8); the default stays 1 (albedo) until the maps have lights (4.1); *denoised since 3.6 (R34), tone mapped since 3.7 (R39)* | 3.3 |
| R20 | The weapon only shadows itself; the first-person player casts no shadow (it has no model; GL draws no weapon shadow) | 3.3 |
| R21 | Hexen II's point lights are spheres in the per-cluster light lists, next to polygons (Q2RTX's lists hold only polygons): weighed by solid angle like a triangle, contributing radiance × solid angle; the UBO's 32 uniformly picked spheres stay for moving lights (4.4); a test sphere's intensity is π × its radiance in both | 3.4 (#34) |
| R22 | The light lists are built on the CPU when the lights change: a light goes into every cluster in the PVS of the open leafs its emitter touches (Q2RTX: its one cluster), minus clusters behind a polygon and beyond a sphere's range; cluster bounds are the leaf's plus its world triangles'; lights inside solid are in no list; a light that doesn't fit is left out whole | 3.4 |
| R23 | A sphere may have a range, where `saturate(1 - (d / range)^4)^2` takes its inverse-square light to 0 so culling by it never shows; 0 = unlimited. The test entity lights use the entity's `light` value (utils/light's hard range); 4.9 picks the curve and the range scale | 3.4 |
| R24 | Q2RTX's light statistics are counted per list entry (12 uints), not per cluster × light (~50 MB per buffer on the largest maps); three buffers take turns per 3D frame, cleared when the lists change; dynamic lights injected into the lists later (4.4) must not move the entries | 3.4 |
| R25 | Screenshot comparisons of the noisy lit image average in linear light: averaging sRGB values makes a noisier image look darker (3.4's first culling check reported a false 27 %) | 3.4 |
| R26 | Bounces are Q2RTX's `indirect_lighting.rgen`, `pt_num_bounce_rays` 1 by default (0, 0.5, 1, 2); its second bounce gathers only emission and the sky, no light samples, and stays so (it adds nothing until 4.5/4.6) | 3.5a (#35) |
| R27 | The weapon is only in its own surfaces' bounce rays (as R20 for shadows); a model hit by a bounce ray has its `colorshade` hue (as R14) | 3.5a |
| R28 | The first bounce stores the specular ray's hit distance for DLSS Ray Reconstruction (`PT_SPECULAR_HIT_DIST`, r16f, 0 without a specular ray: a diffuse bounce, no surface, lava, the rows half resolution skips); 3.9 decides whether RR needs more | 3.5a |
| R29 | `debug_view.comp` runs before the bounces for the G-buffer's modes (with two bounces the first stores its hit into the shading position), after compositing for the lighting's | 3.5a |
| R30 | 3.5 is split: 3.5a bounces, 3.5b the reflection and refraction pass (#121) with the water decision (GL draws water opaque; lean: GL's until 6.5) | 3.5 proposal |
| R31 | Water and slime stay opaque and textured as GL draws them: the reflection and refraction pass skips them, and vertical ones stay water (Q2RTX makes vertical water glass for its force fields; Hexen II's vertical turbulent surfaces are walls); Q2RTX's physical water waits for 6.5 | 3.5b (#121) |
| R32 | The weapon is in no reflection or refraction ray (as R20, R27); a ray through a translucent weapon sees the world | 3.5b |
| R33 | Translucent surfaces and models (alpha < 1) are seen through by Q2RTX's `reflect_refract.rgen` (`pt_reflect_refract` 2), whose rays cull back faces as Q2RTX's (the inside faces of turbulent volumes; two-sided `EF_SPECIAL_TRANS` models lose their back faces behind a translucent surface) and keep the translucent group in the last pass too (it holds water, slime and alpha-1 models); a ray through a translucent weapon starts at the eye; a ray leaves a liquid through a translucent turbulent surface; 6.4 checks each translucency type against GL | 3.5b |
| R34 | The denoiser is Q2RTX's A-SVGF (`asvgf_*.comp`, `vk_asvgf.c`) without its TAA (3.8), on by default with Q2RTX's `flt_*` defaults; `flt_enable 0` is the undenoised composite, as before 3.6 | 3.6 (#36) |
| R35 | A gradient sample blends into its pixel's history only as far as the anti-lag drops that history (`asvgf_temporal.comp`): it replays the brightest of its 3x3 square's last samples, which Q2RTX blends in again as new, brightening noisy lighting by 8–18 % (demo1, test lights) and up to 30 % in places; left: +2–5 % (Q2RTX.md open questions). Q2RTX's brightest pick stays, for its anti-lag with moving lights | 3.6 |
| R36 | Last frame's model instances map to this frame's (`model_prev_to_current`, after the instances in the instance buffer) through the entity history: an entity continues when it was drawn last frame with the same model, a teleport too | 3.6 |
| R37 | The denoiser's history is dropped (Q2RTX's `temporal_frame_valid`: `flt_temporal_*` 0 for a frame) in the first frame, on a new map, with new images, after a frame without the denoiser, when `flt_enable` or `flt_temporal_*` change, and after a skipped 3D view; without history there are no gradient samples (Q2RTX reprojects every frame, which here would read another map's primitives by device address, unchecked); new images also reset the UBO's last frame | 3.6 |
| R38 | No light-count history (Q2RTX's `light_counts_history`): the lists change only with the lights, and a change costs one frame of gradients where it happened; needed if moving lights join the lists (4.4). `prev_style_scale` is 1 until light styles (4.2) | 3.6 |
| R39 | Tone mapping, auto exposure and bloom are Q2RTX's (`tone_mapping_*.comp`, `bloom_*.comp`, unchanged; `vk_tonemap.c`, `vk_bloom.c`), on by default with its defaults, only for `r_debugview 0` (the debug views stay raw); SDR only (HDR 7.3); `tm_enable 0` and `bloom_enable 0` give the image as before 3.7 | 3.7 (#37) |
| R40 | Left out of Q2RTX's: the stronger under-water bloom (Hexen II's underwater look is GL's warp and tint, 6.6), the blur behind menus (GL draws menus over the plain view), the full screen blend in the tone mapper (strongest at the screen's edges; GL's `v_blend` comes with 6.6) | 3.7 |
| R41 | Particles and sprites are scaled by the exposure (`prev_adapted_luminance × pt_particle_brightness`, Q2RTX's for particles) so they show at GL's colors whatever the exposure; one factor for both, `pt_particle_brightness` 15 (Q2RTX 100), measured against their GL colors; 1 in the debug views and with `tm_enable 0` | 3.7 |
| R42 | The adapted luminance reaches the CPU through Q2RTX's readback buffer, one mapped per frame in flight, read when the slot comes round (two frames old; a host barrier after the curve pass); Q2RTX's filter of readbacks of exactly 1 is dropped (1 is `tm_max_luminance`'s clamp), only non-positive and non-finite values are ignored; the exposure starts over on a new map, with new pipelines and when the last rendered 3D frame wasn't tone mapped (a debug view, `tm_enable 0`); it adapts on game time, real time while paused | 3.7 |
| R43 | `tone_mapping_curve.comp` has no launch check against the view's size: its single workgroup of 128 threads (one per histogram bin) must all reach its barriers, and Q2RTX's check returned some of them early in a view under 128 pixels wide | 3.7 |

## Open questions carried forward

See [Q2RTX.md](Q2RTX.md#open-questions-for-later-stories): water normal map
and physical water (6.5), specular hit distance (3.9), checkerboard fields and RR (3.9),
model tint brightness (E4), light styles and the gradients (4.2), denoiser
brightness (4.9), exposure and the mood (4.9, 4.10, 4.7), effects
brightness (6.3, 6.2), lights inside solid (4.1),
dynamic lights in the light lists (4.4), smooth surfaces and sphere lights
(E5, 4.5), dark albedo (E4, E5).
