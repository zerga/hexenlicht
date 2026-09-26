# Hexenlicht — testing

There are no test suites: verification is building both presets and running
the game with scripted checks. This page is how. The tools are in
[tools/hexenlicht](../../tools/hexenlicht/README.md); the renderer's check
commands are listed in [RENDERER.md](RENDERER.md#console-commands).

## Every change

- Build both presets fully before a PR (a full build, not just `--target
  hexenlicht`, also builds `glh2`; Release catches optimizer-only problems). From Git Bash, run MSVC tools with `-opt`
  instead of `/opt`.
- Run the Debug build: the log must end with
  **`Vulkan validation: 0 errors, 0 warnings`**.
- Run what the change touches: the matching check command
  (`vk_models check`, `vk_rtcheck`, `vk_effects check`, `vk_pvs`,
  `vk_world`) and screenshots of the affected look.

## Scripted runs

- A test is a config script in the data folder's `data1\` (Portal of
  Praevus: `portals\`, run with `-Portals`): commands with `wait` lines
  between them (one frame each). Start it with
  `tools\hexenlicht\hl_run.ps1 -Cfg hl_test.cfg` (`-Exe glh2` for the GL
  client, `-Release`, `-Timeout s`, `-Width`/`-Height`, default 960x540).
  `-condebug` writes `debug_h2.log` into the data folder; each run overwrites
  it (glh2 runs too), so copy it away before the next run.
- A `map` needs ~150 waits before anything else; `notarget` only works once
  the client is connected (~30 waits after `map`).
- **A script over ~8 KB overflows the command buffer** (`Cbuf_AddText:
  overflow` in the log, and nothing runs): chain small files, each ending
  with `exec` of the next.
- **Quit with the console open:** end with exactly one `toggleconsole`, a few
  waits, `quit`. With the console closed, `quit` opens the quit screen and
  the process hangs; never toggle it closed again after opening it.
- Start with `wait` × 3 and `vid_vsync 0` (end with `vid_vsync 1`) for speed.
- Early-read cvars (`vid_*`, `vid_uiscale`) are locked until `hexen.rc` has
  run: set them after a `wait`.

## Repeatable frames

- **`host_framerate 0.02` before `map`** makes game time identical frame by
  frame in both engines (views, positions, light levels).
- `notarget` keeps monsters from reacting; `pause` freezes the scene (take
  screenshots paused: `pause`, a few waits, `screenshot`, waits, `pause`).
- With these, **the 3D view is bit-identical between runs of the same build**:
  a pixel regression test for renderer refactors (see below). Not repeatable
  between runs, in `main` too: where particles and explosions land, patrolling
  monsters' AI, and the HUD's health number.
- The engines' `screenshot` capture different frames (Hexenlicht the next
  presented one, glh2 an already drawn one): anything that changes per frame
  (beams, effects, animation) must be compared paused (story 2.10 was a false
  alarm from this).

## Screenshots

- `screenshot` writes the first free `data1\shots\hexenNN.tga` (00–99; both
  engines use the same names; nothing clears the folder and the 101st
  fails): empty the folder before a run so the numbers follow the script,
  and move the shots away after it. Put waits after `screenshot` before
  changing what's drawn (it captures the next frame).
- `tga2png.ps1 -Files ... -OutDir ...` converts them; `crop_strip.ps1` puts
  the same crop of several shots side by side (`-Scale`). View shots at half
  size unless detail matters.
- A fixed `vid_uiscale` in `config.cfg` (e.g. 3: a 1280x720 window has a
  426x240 2D screen) is not a bug. Set `playerclass` explicitly: `config.cfg`
  may have another class.

## Pixel regression (renderer refactors)

1. Build the old code: `git worktree add --detach ..\hexenlicht-main <sha>`
   and build it there (its own `build\`); or take the baseline before
   changing anything.
2. Run the same script (every `r_debugview` mode on the regression maps,
   paused) on both builds, twice on the old one (`hl_run.ps1 -Bin
   ..\hexenlicht-main\build\windows-debug\bin` runs the old build). Set
   `flt_enable 0` unless the denoiser is what is compared: with it the
   G-buffer modes hold the gradient samples' last-frame values in up to
   one pixel per 3x3, and mode 14 changes every frame.
3. `tga_diff.ps1 -A old -B new -MaxY 470` compares above the HUD rows;
   `-Noise old2` skips pixels that differ between two old runs; `-DiffDir`
   writes images with differing pixels in red. Expect "identical" or ±1
   rounding; explain everything else.
4. Remove the worktree afterwards.

## Regression maps and useful places

- Nine regression maps (one script each, each `exec`s the next): demo1 demo2
  egypt1 romeric2 castle4 cath meso1 village1 tower.
- Moving brush entities: romeric2 (rotating, `*26`–`*28`) and egypt5 (a lift,
  `*89`) when walking forward from the start (`god`, `+forward`); demo3 and
  village2 have `func_rotating` out of reach of the start.
- Walking monsters: village3's patrolling archers move right away
  (`patrols.ps1`).
- Praevus snow: tibet9, walk forward and look up under the ceiling opening.
- `edict 1` shows the server's player fields (e.g. `light_level`) in both
  engines.

## Driving the player

- `playerclass <n>` before `map`: 1 Paladin, 2 Crusader, 3 Necromancer,
  4 Assassin, 5 Demoness (Praevus).
- Weapons: `impulse 9` (all weapons and mana), wait ~150 frames, `impulse
  <n>`, `+attack`. Crusader's meteor staff (3) and sunstaff (4) make many
  particles and temporary entities.
- Aiming: `viewpos`; `cl_yawspeed 100` with `host_framerate 0.02` turns 2° per
  frame of `+right`. `chase_active 1`, `bf` (view blend), `r_dumpscene`.
- Most monsters stand until they notice the player — in GL too.

## Config safety

- Before tests, back up `data1\config.cfg`, `data1\hexenlicht.cfg` and
  `portals\config.cfg`; restore them afterwards, compare, and delete the
  backups only after the restore is confirmed (a separate command).
- Delete `hexenlicht.cfg` before each run: archived cvars a test sets
  (`r_lerpmodels`, `playerclass`, `color`) leak into the next run.
- glh2 runs rewrite `data1\config.cfg` and drop Hexenlicht-only cvars
  (`vid_uiscale`, `vid_vsync`): restore it before each engine run when
  comparing framing.
- Delete the test scripts afterwards.

## Other checks

- **Window:** `resize_test.ps1` (resize, maximize, restore, too small) with a
  script that echoes `==== STEP1..5` and waits ~300 frames after each; also
  `viewsize`, `vid_restart`.
- **Shader reload:** edit a shader while the game runs, build
  `hexenlicht_shaders`, `vk_reload_shaders`, compare shots before/after;
  then restore the source **and rebuild again**, or the edited SPIR-V stays on
  disk.
- **Global UBO layout:** `ubo_layout_check.ps1` after changing
  `GLOBAL_UBO_VAR_LIST`.
- `vk_rtcheck` must report 0 differing rays; `vk_models check` "all agree".
- **Motion vectors:** `r_debugview 7` (motion check) while walking and
  turning (`+forward`, `+right`), on moving brush entities (romeric2) and
  walking monsters (village3): black except texture detail (bilinear
  resampling), disocclusions (bands along the weapon and near objects),
  animated textures and liquids; dark blue where the point was off the
  screen. A wrong vector shows the texture shifted (double edges).
- **Liquids near a start:** romeric1 (`*skulls`, look down), meso9 (lava).
- **Lighting (until E4's map lights):** `r_debugview 0`, `vk_testlight
  sphere 8 2000` at the start, `+forward` ~60 frames, `+right` 90 frames
  (180° with `cl_yawspeed 100`): the light's shadows face the camera
  (demo1: tombstones, tree, statue). A quad lights only what is in front of
  it: after `+back` the camera is behind it. Modes 15 and 16 show the
  direct diffuse and specular lighting. With `flt_enable 0` the image is
  noisy (one sample per pixel), and low-poly models show grainy
  self-shadowing at grazing angles.
- **Many lights and the light lists:** `vk_testlight entities` puts a
  sphere at every light entity; `vk_lights` prints the lists (demo1: 8270
  entries, mean 10.4, longest 62, 43 lights inside solid; keep2 16472;
  romeric6 the longest list, 249), `r_debugview 17` shows the list lengths.
  Checks, paused, averaging 8–16 screenshots per case: `vk_testlight dlight
  8 2000` and `vk_testlight sphere 8 2000` at the same spot give the same
  image (a dynamic and a list sphere); `vk_lights cull 0` (no range
  culling) gives the same image, only noisier (demo1's start: +1.4 %, noise
  ×1.6). `vk_lights stats` shows the shadow rays the last frame counted
  (0 with `pt_light_stats 0`).
- **Comparing noisy shots:** average them in linear light (sRGB → linear
  before averaging): averaged sRGB values make a noisier image look darker
  (a false 27 % in 3.4's first check). The frames of a paused scene still
  get new random numbers, so averaging shots reduces the noise.
  `tga_mean.ps1 -Dir <shots> -A (0..11) -B (12..23) -MaxY 470 [-OutDir d]`
  compares two sets (means, 60-pixel blocks, noise) and writes the
  averages for `tga2png.ps1`.
- **Bounces (3.5a):** `pt_num_bounce_rays 0` must be pixel-identical to the
  build before the bounces (3.5a: 3 maps × 8 modes identical to 3.4).
  Bounce light is weak on Hexen II's dark textures (demo1's start and the
  cathedral: +2–3 %; `r_debugview 18` shows it); `pt_num_bounce_rays 2`
  adds nothing until emissive surfaces and the sky (the second bounce takes
  no light samples); 0.5 averages to the same as 1. Reflections: paused,
  looking down at demo1's floor (`cl_pitchspeed 100`, `+lookdown` 8
  frames), `vk_testlight entities 4000`, then `pt_roughness_override 0.02`
  with `pt_metallic_override 0` (mirror: the statue and tombstones
  reflected in the floor), 0.15 (glossy, blurred); `r_debugview 16` shows
  the specular alone, 19 the specular rays' hit distances. Reset the
  overrides to −1 afterwards.
- **Translucency (3.5b):** no translucent surface (`*lowlight`, `*rtex078`)
  is visible from a map start. The cathedral's holy water font is one:
  `showpause 0`, `cl_yawspeed 100`, `cl_pitchspeed 100`, `noclip`, then
  `+right` 48 frames, `+forward` 143, `+moveup` 9, `+lookdown` 22 (at
  `host_framerate 0.02`). Compare `pt_reflect_refract 0` and 2 (Q2RTX's
  default): with 2 the odd field of `r_debugview 1` shows the stone floor
  under the font; mode 3 at 0 shows the purple translucent kind in a
  checkerboard. The lit image averages to the surface blended with what is
  behind it.
- **Water stays opaque (3.5b):** with `pt_reflect_refract 2`, romeric1's
  pool (look down at the start) and egypt4's vertical water walls (`+right`
  69 frames) must match the build before 3.5b pixel for pixel in the
  G-buffer modes (1, 3, 9, 2), except that mode 3 now shows vertical water
  as water (blue) instead of glass. romeric2's centre isn't repeatable
  between runs (its rotating brushes): mask it with a second run
  (`tga_diff.ps1 -Noise`).
- **Denoiser (3.6):** `flt_enable 0` must match the build before it
  (3.6: demo1, cath, romeric2 × modes 0 1 2 3 9 15 16 18, paused, `vk_testlight
  entities`: the G-buffer modes identical everywhere, demo1 in all modes;
  cath's and romeric2's lighting modes differ between runs of the same
  build in a few dozen to a few hundred pixels, in `main` too, and
  `-Noise` can't mask it: compare several runs of each). Checks, paused
  with test lights, `r_debugview 0`, `tm_enable 0`, `bloom_enable 0`
  (linear light; since 3.7): a denoised shot against the average
  of 16–24 `flt_enable 0` shots (`tga_mean.ps1`), **at a sixteenth of the
  lights' intensity** (`vk_testlight entities 62.5`): at full intensity the
  raw frames clip at 1 before the screenshot and their average reads
  4–10 % dark; and with `pt_fake_roughness_threshold 1` or
  `pt_num_bounce_rays 0`, because only the denoiser gives rough surfaces
  indirect specular (3.6: +2 % demo1, +5 % the cathedral's font). Paused,
  `flt_show_gradients 1` must show no gradients and `r_debugview 20` full
  history (yellow); after adding a light (`vk_testlight sphere 8 2000` at
  the eye) the image follows within a frame or two. Moving: `+right`,
  `+forward` (demo1, romeric2), village3's sheep: `r_debugview 20` keeps
  history on moving surfaces, drops it at disocclusions and on the bobbing
  weapon; `viewsize` changes and `vid_mode`/`vid_restart` (the history
  starts over: dark, then yellow within 30 frames) show no stale image.
  egypt5's start walks into a mural whose close-up texture looks blurred
  in `r_debugview 1` too: not the denoiser.
- **Tone mapping and bloom (3.7):** `tm_enable 0` and `bloom_enable 0`
  must match the build before, and the G-buffer and effects views (1 2 3
  9 13) with them on too (3.7: the same maps and modes as the denoiser's,
  `flt_enable 0`; `con_notifytime 0`, or the old build's "Unknown
  command" lines for new cvars differ in the top rows). Exposure: paused
  at demo1's start with `vk_testlight entities`, then `vk_testlight
  entities 62.5`: `vk_exposure` and the image mean (`tga_mean.ps1 -A n`)
  come back over ~6 s to 1/16 and ~75 % of before; `vk_testlight entities
  1`: the exposure stops at `tm_min_luminance` 0.0002, dark. Effects:
  Crusader (`playerclass 2`), `impulse 9`, ~120 waits, `impulse 3`, face
  the wall (`+right` 45 frames), `+attack` 24 frames, `pause`; compare
  `r_debugview 13` (the effects in GL's colors over black) with
  `r_debugview 0` at `pt_particle_brightness` 0 (the background) and at
  the value tried, over the effect pixels below the console rows: their
  mean luminance should match. `tm_debug 1/2`, `bloom_debug 1-3` show the
  stages.
- **Upscaling (3.8):** at `r_scale 100` with `flt_enable 0` (the TAA pass
  only copies) the lit image must match the build before within ±1 (the
  TAA's PQ encoding rounds in fp16), apart from bloom halos around the
  test lights' hot spots, which the PQ clamp dims (romeric2: up to 7;
  gone with `tm_enable 0 bloom_enable 0`); the G-buffer views identical,
  modes 4, 5, 9 with `flt_enable 1` too (the debug views get no jitter).
  `vk_upscale` prints the sizes, jitter and passes: check every
  `r_upscaler` at 100, 67 and 50 %. Still image: paused, 8 shots per
  setting into `tga_mean.ps1` (temporal noise; wait ~400 frames after
  changing the lights, the exposure adapts meanwhile). Motion: `+right`
  (`cl_yawspeed 100`), shots mid-turn and 2 and 30 frames after `-right`;
  village3's sheep. Robustness: `resize_test.ps1` with `r_upscaler 2`,
  `r_scale 67`; `vid_restart`, `viewsize`, `r_scale`/`r_upscaler` changes
  while turning, an odd view width (`-Width 1283` at UI scale 3: 1281).
- **GPU cost:** until 3.11's timers, temporary timestamps
  (`vkCmdWriteTimestamp2` between the passes, read back when the frame's
  slot comes round, averaged over 100 frames by a console command; not
  committed). Measure the baseline build the same way on the same day:
  the GPU's clocks follow its load and power limit (3.8 found the machine
  at a ~100 W cap, the 100 % view at 0.8 GHz; watch with `nvidia-smi
  --query-gpu=clocks.gr,power.draw,clocks_throttle_reasons.active
  --format=csv -lms 250`). uHexen2 caps frames at 72 fps outside timedemo,
  so a light setting runs partly idle at other clocks: lift the cap
  temporarily (`host.c`'s `1.0/72.0` check) so every setting runs at full
  load.
- **`screenshot` captures the next frame:** a command in the same frame
  after it (e.g. `vk_testlight` changing the lights) is already in the
  shot; put waits after every `screenshot`.
- In a bash script generator, a helper that loops must use a `local`
  counter, or it overwrites the caller's (story 3.2 chained every
  regression script to the same one that way).
- If the desktop is at 1024x768 (monitor off), Hexenlicht refuses 1280x720
  windows (falls back to 640x480) and vsync crawls: use 960x540 and
  `vid_vsync 0`. Check the desktop size with
  `[System.Windows.Forms.Screen]::AllScreens` after
  `SetThreadDpiAwarenessContext(-4)` (per-monitor DPI aware).
- Screen sampling from outside (GDI) needs a per-monitor DPI aware thread
  and the game window in front (a program started from a background process
  opens behind the active window).
- Subagents compiling shaders without `-o` leave `comp.spv` in the shaders
  folder: check `git status` for strays before committing.
