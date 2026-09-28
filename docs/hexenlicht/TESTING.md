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
  monsters' AI, and the HUD's health number. The sky (4.6) scrolls on GL's
  `realtime`, the clock (it runs while paused), but on game time while
  `host_framerate` is set, so it repeats too (and stands still while
  paused); `glh2`'s sky always scrolls on the clock.
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

## Calibration against GL (4.9)

- **Bookmarks** are views in `tools/hexenlicht/bookmarks.txt` (`name map x y
  z pitch yaw [portals]`: the player's origin and view). In Hexenlicht,
  `vk_bookmark <name>` appends one for where you stand to the game folder's
  `bookmarks.txt` (copy it into the repository's list); `vk_setpos x y z
  [pitch yaw]` goes to one (single player).
- **The same camera in both engines** comes from a savegame:
  `calib_shots.ps1` makes one per bookmark in Hexenlicht (`vk_setpos ...;
  save hlcal_<name>` on one line keeps the pitch: a load sends the player
  model's angles, which a server frame turns into a third of the view's),
  then `glh2` and Hexenlicht both `load` it and take paused shots at the
  same size, without the HUD (`viewsize 130`), `showpause 0`, the weapon,
  the crosshair and the notify lines. Angles are bytes in the protocol
  (1.4°), the same in both. Checked: egypt1's start at pitch 30, yaw 20
  loaded as 29.5°, 19.7° in both, the same frame.
- **`calib_shots.ps1 -Out <folder> [-Label l] [-HlCvars "..."]
  [-Names ...]`**: `gl\<name>_gl.tga` (the image), `gl\<name>_gllm.tga`
  (`r_lightmap 1`, entities hidden: the lightmaps), `<label>\<name>_lit.tga`
  (the lit image) and `<label>\<name>_direct.tga` (the direct diffuse
  light, `r_debugview 15` with `r_debugview_scale` 0.25, white lights, no
  denoiser, entities hidden, lava and sky light off, 16 frames averaged by
  `vk_screenshot`). `-HlCvars` goes in after each load (a candidate:
  `"r_maplight_power 4"`; 4.15's light shapes: `"r_maplight_shape 1;
  r_maplight_scale 705"`); `-SkipSaves -SkipGl -KeepSaves` reuse the saves
  and GL shots for more candidates (a run without `-KeepSaves` deletes
  them: the next `-SkipSaves` run then shoots the console); `-Bin
  <folder>` runs another build for the Hexenlicht shots (4.17: a copy of
  main's `build\windows-release\bin`, for a comparison on the same GL
  shots); `-GlLit <folder>` adds
  `gl\<name>_glc.tga` with HoT's colored light (`<folder>\maps\*.lit`,
  copied in for the run), `-LitFrames` averages the lit image too. It
  backs up and restores `config.cfg` and `hexenlicht.cfg` of the game
  folder and `data1` (no `hexenlicht.cfg` during the runs: Hexenlicht would
  find `data1`'s from `portals` too), moves the folder's own numbered shots
  aside for `glh2`'s and back, checks that each save is of its map, and
  deletes its scripts (and the saves unless `-KeepSaves`). 15 bookmarks:
  ~3 minutes (Release, 960x540).
- **`calib_compare.ps1 -Out <folder> -Labels a,b [-Pictures] [-Transfer
  srgb] [-ByMap] [-Range lo,hi]`**: per bookmark and pooled, Hexenlicht's direct light against
  GL's lightmaps in linear light (GL multiplied the texture's 8-bit color
  by the lightmap's: its linear light is the lightmap decoded; since 4.17
  every shot is decoded as the engine encodes its image, by the 2.2 power;
  `-Transfer srgb` for shots of a build before 4.17 or with `r_srgb 1`,
  which gives the old numbers), in 30-pixel blocks where GL's
  pixels are grey (lightmaps: not the sky or liquids) and neither is
  clipped or black: the median ratio (1: GL's units), the spread (stops
  between the quartiles), the slope of log Hexenlicht over log GL (1: the
  same contrast); and the lit image's mean luminance against GL's, the
  look on the lightmapped world ("surfaces"), and since 4.16 the look
  with the blocks where GL's lightmap is clipped ("+clip": fill-lit
  rooms and yards, where GL's lights added up to a full texel, which the
  light's mask leaves out) and those blocks' spread in stops. Since 4.11
  a line counts the views whose "+clip" look is within `-Range` (0.85,
  1.2) and gives the mean error per view (stops), and `-ByMap` adds a
  pooled row per map (a bookmark's name up to its first `_`).
  `-Pictures` writes `<label>\compare\<name>.png`: GL, Hexenlicht and their
  ratio (blue darker, red brighter, to 2 stops) for the image and the
  light. Saves are named `hlcal_<bookmark>`: two bookmark files with the
  same names (4.9's `demo1_start` and 4.11a's) overwrite each other's
  saves, so make them again (no `-SkipSaves`) when switching files.
- **The light fit (4.16):** `vk_lights fit` scores the map's fitted light
  factors on the lightmap texels the fit didn't use: the direct light
  against GL's lightmaps with 4.15's one factor, a factor per light and
  per list entry (what the renderer uses), and where GL's lightmap is
  clipped. Over many maps: `map <m>`, ~40 waits, `echo ==== MAP <m>`,
  `vk_lights fit`, at most ~10 maps per script, chained with `exec`.
- `vk_screenshot <name> [frames]` writes `shots\<name>.tga`, frames
  averaged in linear light (not numbered, no 100-file limit).

## Calibrating a hub (4.11)

A hub is done when it is "close enough" to GL (DECISIONS R105), without
shipped per-map files: what is off for a renderer-wide reason is fixed for
every map. Blackmarsh (4.11a) was the first; about an hour of runs.

1. **Views** into `tools/hexenlicht/bookmarks_<hub>.txt` (the format of
   `bookmarks.txt`; Blackmarsh's has 43):
   - the maps' entrances: `pak_entities.ps1 -Paks ... -Pattern
     info_player_start -Keys angle` gives the spots; the player's origin
     is about the spot's z − 24 (the floor; village1's spots stand 18
     above it, which only moves the camera: both engines load the same
     save), the view its `angle`;
   - three views per map in its main areas: `pick_views.ps1 -Paks
     <paks> -Maps <maps> -Entrances <the entrance lines> -Out <file>`
     (deathmatch spots, each farthest from the views taken, turned away
     from walls; `-Portals` for the mission pack), then rename them by
     place (`<map>_<place>`) from their GL shots.
2. **Shots:** `calib_shots.ps1 -Bookmarks
   tools\hexenlicht\bookmarks_<hub>.txt -Out <folder> -KeepSaves`
   (Release, 960x540: 43 views ~12 minutes; the saves for step 4);
   **compare:** `calib_compare.ps1 -Out <folder> -ByMap -Pictures`.
3. **Close enough** (R105): a view's look with GL's clipped blocks within
   0.85–1.2 of GL's, or outside for a reason Hexenlicht keeps on purpose
   or a renderer-wide one already on the board (entity shadows and entity
   lighting GL's lightmaps don't have, bounce light, water, 4.18's fit
   granularity, 4.19's dynamic lights), written down; a map's pooled look
   with the clipped blocks within 0.9–1.1 unless only such views move it;
   the owner reviews the grids and plays the hub.
4. **A view outside without a known reason:** the pictures' two rows say
   whether the direct light is off (the fit, a light's shape) or the look
   is (entities, textures, bounce); a candidate: `calib_shots.ps1 ...
   -SkipSaves -SkipGl -KeepSaves -Label <candidate> -Names <views>
   -HlCvars "..."` (e.g. `"r_maplight_fit 0; r_maplight_gl_scale 1"`:
   each light at its own GL texel; 4.11a found 4.18 this way), then
   `calib_compare.ps1 -Labels hl,<candidate>`. A renderer-wide cause
   becomes a story (fixed for every map); a shipped map file only if the
   owner asks for one.
5. **Checks** of the hub's special lights, each shot in both engines from
   the same save (`-SkipSaves -KeepSaves` on saves made by a script, the
   bookmark lines only name them):
   - switchable lights and styles: `pak_entities.ps1 -Pattern light
     -Keys style, spawnflags` and the entities that target their
     `targetname` (`-Pattern 'trigger_.*|func_.*' -Keys target`); a save
     before and after walking into the trigger (noclip; `bsp_models.ps1`
     gives its bounds);
   - thunderstorms (`light_thunderstorm`): ten saves ~25 frames apart
     catch flashes (a flash lasts ~0.3 s);
   - the torch: `-PreSave` (4.10's, above).
6. **Grids for the owner:** `calib_grid.ps1 -Out <folder> -Names ...
   -Columns 'GL|gl\{0}_gl.tga|1', 'Hexenlicht|hl\{0}_lit.tga|1' -Png
   <file>` (a row per view, a column per pattern; a scale other than 1
   multiplies a shot in linear light).
7. **Record** the hub's verdict in DECISIONS.md (the numbers, the accepted
   differences, the stories found) and delete the shots; saves left by
   killed runs are `hlcal_*` with `<map>.gip` in the game folder.

## Pixel regression (renderer refactors)

1. Build the old code: `git worktree add --detach ..\hexenlicht-main <sha>`
   and build it there (its own `build\`); or take the baseline before
   changing anything.
2. Run the same script (every `r_debugview` mode on the regression maps,
   paused) on both builds, twice on the old one (`hl_run.ps1 -Bin
   ..\hexenlicht-main\build\windows-debug\bin` runs the old build). Set
   `flt_enable 0` unless the denoiser is what is compared: with it the
   G-buffer modes hold the gradient samples' last-frame values in up to
   one pixel per 3x3, and mode 14 changes every frame. A change that
   brightens part of the view (e.g. the sky, 4.6) moves the auto exposure
   and so every pixel: compare the lit image with `tm_enable 0` and
   `bloom_enable 0` too.
3. `tga_diff.ps1 -A old -B new -MaxY 470` compares above the HUD rows;
   `-Noise old2` skips pixels that differ between two old runs; `-DiffDir`
   writes images with differing pixels in red. Expect "identical" or ±1
   rounding; explain everything else. The first map after the game starts
   can differ between two builds in the denoised lit image (4.16: every
   pixel by up to ±20 while the direct light was identical; two runs of
   one build identical, and the same map second in the script identical
   within ±1): put a map you don't compare first.
4. Remove the worktree afterwards.
5. A script's last lines must be `toggleconsole`, a few waits and `quit`:
   `quit` in a game opens the menu's "are you sure" (the run then ends at
   `hl_run.ps1`'s timeout, killed, without the validation summary; a
   killed run also leaves `<map>.gip` files in the game folder).

## Regression maps and useful places

- Nine regression maps (one script each, each `exec`s the next): demo1 demo2
  egypt1 romeric2 castle4 cath meso1 village1 tower.
- Moving brush entities: romeric2 (rotating, `*26`–`*28`) and egypt5 (a lift,
  `*89`) when walking forward from the start (`god`, `+forward`); demo3 and
  village2 have `func_rotating` out of reach of the start.
- Walking monsters: village3's patrolling archers move right away
  (`patrols.ps1`).
- Praevus snow: tibet9, walk forward and look up under the ceiling opening.
- Skies (4.6; `+lookup` frames at `host_framerate 0.02`): egypt1's start
  (4: the blue sky over the courtyard, good for the sky light and the sun),
  demo1's (12: the storm), meso9's (12: red; its sky face is at z 704 over
  the start, reached with `noclip` and `+moveup`), romeric6's (12: night).
  village2 has world geometry above its sky: the sun's shadow rays must
  stop at the sky (its yard through the opening right of the start).
  `vk_sky` prints the sky and the mode.
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

- **Map light brightness in older checks (4.15):** the checks below from
  before 4.15 dim or brighten the map's lights with `r_maplight_scale`
  (50, 62.5, 100, 4000), which only the physical shapes use: add
  `r_maplight_shape 0` to repeat them as measured, or scale
  `r_maplight_fit_scale` (1 by default since 4.17, 1.1 in 4.16) instead with GL's shape,
  or `r_maplight_gl_scale` (2) with `r_maplight_fit 0` for 4.15's (e.g.
  R97's pulse at `r_maplight_gl_scale 0.5`): its light is bounded, a full
  lightmap texel at most, so the physical shape's very bright settings
  have no equivalent next to lights.
- **Map files (4.7):** a test `maps/<map>.hlmap` goes into the data
  folder's `data1\maps\` (or `portals\maps\`), not the repository: it is
  used before a shipped one. `vk_mapfile` shows what it did, `vk_mapfile
  reload` applies an edited file without reloading the map; `vk_lights
  colors` lists each light's origin (for `light` lines). Delete the test
  files afterwards (and any under `build\<preset>\bin\maps\` that aren't
  in `data/hexenlicht/maps/`).
- **Per-map cvars in scripts (4.7):** every map load resets the sky and
  sun cvars and `r_map_*` to their defaults (then the map file sets them):
  set them after `map` and its waits (a `r_sun 1` before `map village2`
  is gone when the map has loaded).
- **Light editing (4.8):** scripts select lights by entity origin
  (`vk_editlight select x y z`, origins from `vk_lights colors`) rather
  than by the crosshair; each edit prints its line and time, `vk_editlight`
  the selected light, `vk_mapfile` the lines and unsaved edits.
  `vk_editlight save` writes into the data folder's `data1\maps\` (or
  `portals\maps\`, the running game's) and makes the folder: delete it
  afterwards. Screenshots with `r_editlights 1` show the markers and the
  panel (console output covers the panel's top in scripted shots).
- **Calibrating with the editor (4.9, 4.11):** `r_editlights 1`, aim at a
  light, `vk_editlight select`, then `level`, `scale *f`, `color`, `move`
  or `off` until it matches `glh2`'s look; `vk_editlight save`, and move
  the file from the game folder into the repository's
  `data/hexenlicht/maps/` (don't copy: a game-folder file is used before
  the shipped one).
- **Darkness (4.10):** `vk_darkplaces` in a local game lists the map's
  dark places with a point (an item's or monster's origin + 24): a
  script over the maps is `map <m>`, ~40 waits, `vk_darkplaces 5` per map,
  in chained scripts of at most ~12 maps (a longer one overflows the 8 KB
  command buffer and runs nothing). Bookmark them (a pitch of 15 shows the
  floor) and run `calib_shots.ps1`: the look is GL's image against the lit
  image (`-LitFrames 8`); the light compare has nothing to count (GL's
  lightmaps are black there). With the torch: `-PreSave "impulse 43;
  <20 waits>; invuse"` (`impulse 43` gives everything, the artifacts in
  their index order, so the torch is the first; the client needs the ~20
  frames to get the new inventory, or `invuse` does nothing; the save
  keeps the torch burning for both engines; `impulse 14` is the
  polymorph, a sheep's eye height) into another `-Out` (GL's shots
  differ). The Necromancer's darkness (`EF_DARKLIGHT`): `-PlayerClass 3
  -PreSave "impulse 43; <20 waits>; invleft; <5 waits>; invleft; <5
  waits>; invuse"` (the first `invleft` only opens the inventory bar, the
  second selects the last artifact, the Icon of the Defender) at lit
  starts with a pitch of 30, and a second label with `-HlCvars
  'r_darklights 0' -SkipSaves -SkipGl -KeepSaves`; `vk_lights` counts the
  dark lights. The debug views don't show the darkening (only the
  composites apply it).

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
- **Lighting with one test light:** `r_maplights 0` (the map's lights
  off), `vk_testlight sphere 8 2000` at the start, `+forward` ~60 frames, `+right` 90 frames
  (180° with `cl_yawspeed 100`): the light's shadows face the camera
  (demo1: tombstones, tree, statue). A quad lights only what is in front of
  it: after `+back` the camera is behind it. Modes 15 and 16 show the
  direct diffuse and specular lighting. With `flt_enable 0` the image is
  noisy (one sample per pixel), and low-poly models show grainy
  self-shadowing at grazing angles.
- **Many lights and the light lists:** the map's lights load with it
  (4.1); `vk_lights` prints them and the lists (demo1: 290 of 333 light
  entities, 43 dropped inside solid, 8329 entries, mean 10.5, longest 63;
  keep2 16472, tibet1 the most, 19020; romeric6 the longest list, 249),
  `r_debugview 17` shows the list lengths.
  Checks, paused, averaging 8–16 screenshots per case: `vk_testlight dlight
  8 2000` and `vk_testlight sphere 8 2000` at the same spot give the same
  image (a dynamic and a list sphere); `vk_lights cull 0` (no range
  culling) gives the same image, only noisier (demo1's start: +1.4 %, noise
  ×1.6). `vk_lights stats` shows the shadow rays the last frame counted
  (0 with `pt_light_stats 0`).
- **Map lights (4.1):** `vk_lights` on every map (a script with `map`,
  ~180 waits and `vk_lights` per map: the 42 of pak0/pak1, then the 17 of
  pak3 with `-Portals`, scripts in `portals\`): 12,747 of 12,928 light
  entities, 181 dropped inside solid, 42 spotlights (every target
  matched), at most 19,020 list entries, built in at most 2 ms.
  Placement: the same script's start views in `glh2` (`-Exe glh2`) show
  light where GL's lightmaps are bright (Hexenlicht is brighter until
  4.9). Spotlights: cath's nine hang at z −468 and aim down at −696; from
  cath's start as the Necromancer at `host_framerate 0.02`: `noclip`,
  `cl_yawspeed 100`, `cl_pitchspeed 100`, `+right` 27 frames, `+lookdown`
  8, `+forward` 550, `+lookdown` 27 ends at (1091 1671 −525), where
  `r_maplight_scale 50` and `r_debugview 15` (not saturated) show their
  pools. Models at a light's origin: `vk_models` counts their triangles
  ("at lights"), `vk_lights` the models; a temporary build whose light
  group has the mask `AS_FLAG_OPAQUE` shows what they would shadow (4.1:
  mode 15 averaged at meso1's and castle4's starts, 3.5 % darker).
- **Light styles (4.2):** castle5's style 2 pulse (`'a'` to `'z'`, 5.1 s)
  at (992 −1520 −216) is straight ahead of its start: `noclip`, `+forward`
  150 frames at `host_framerate 0.02` ends at (992 −1338 −272); then 60
  shots 4 frames apart in both engines (Hexenlicht `flt_enable 0`,
  `r_debugview 15`, `r_maplight_scale 100`: raw direct light, unsaturated,
  without the denoiser's or the exposure's lag; `glh2` its lit view) and
  the per-shot mean luminance of the rows above the HUD: the two series
  correlate (4.2: 0.95 at no lag; GL flattens at 1.30× on the bright phase,
  its lightmaps clip). rider2c's switchable bank (styles 32–35) is off at
  the start in both (`vk_lights`: 25 lights off; `r_dumpscene` shows the
  styles at `"a"`). A camera inside a brush (noclip) shows GL the room
  beyond (back faces culled) and Hexenlicht the brush's inside: check
  `viewpos` and the view before comparing.
- **Light colors (4.3):** every map light must have the color
  `utils/jsh2color` computes for it.
  1. `pak_bsp.ps1 -Paks <pak0>,<pak1>,<pak3> -Out <dir>` extracts the 59
     maps (outside the repository; delete them afterwards).
  2. Build the tool with `jsh2color_colors.patch` (prints each light's
     color, writes no `.lit`): `git archive HEAD utils/jsh2color | tar -x
     -C <scratch>`, `patch -p3 < <repo>/tools/hexenlicht/jsh2color_colors.patch`
     in its `utils/jsh2color`, then in the MSVC environment `cl -nologo
     -O2 -DNDEBUG -DDOUBLEVEC_T -DWIN32_LEAN_AND_MEAN
     -D_CRT_SECURE_NO_WARNINGS -I. -I<repo>\utils\common -I<repo>\common
     -I<repo>\oslibs\windows\misc\include -Fejsh2colour.exe *.c` plus
     `utils\common\` `cmdlib.c byteordr.c util_io.c pathutil.c threads.c
     mathlib.c bspfile.c` and `common\` `q_endian.c qsnprint.c strlcat.c
     strlcpy.c`, with `-link -STACK:67108864`: with one thread the tool
     runs on the main thread, whose default stack its 2.6 MB per-face
     struct overflows (it exits silently).
  3. `jsh2color_colors.ps1 -Exe <it> -Bsp <dir> -Out <ref>` runs each map
     with the options the tool's batch files give it, one thread (its
     threads add to the sums without a lock: multi-threaded runs differ
     between themselves). One thread takes 0.2–17 s a map.
  4. A script per map (`map`, ~180 waits, `vk_lights colors`; the 42 of
     pak0/pak1, then the 17 of pak3 with `-Portals`), then
     `light_colors_compare.ps1 -Log <debug_h2.log> -Ref <ref> -Maps <the
     same order>`. 4.3: all 12,747 lights as the tool's (Debug and
     Release); tibet4, 5, 6, 10 white (the tool colors none there);
     Release at most 83 ms per map on the original game's maps (egypt5),
     183 ms on the mission pack's (keep5; `vk_lights` prints the time).
  Look: HoT's published `.lit` files (`hexen2-litfiles-20140628.zip`
  from the uhexen2 SourceForge project, "HoT - Other content/extra
  data") in the data copy's `data1\maps\` (delete them afterwards; never
  in the repository: derived from Raven's maps), `glh2` with
  `gl_coloredlight 1` before the `map`, beside Hexenlicht with
  `r_maplight_colors` 1 and 0 at `host_framerate 0.02` starts (4.3:
  castle4, egypt1, meso2, romeric1, village1, demo1 — the hues match;
  Hexenlicht is brighter until 4.9). `_color`: a `data1\maps\<map>.ent`
  (the engine's `external_ents` loads it instead of the lump) with
  `_color` on a few lights: `vk_lights colors` shows them converted, the
  other lights white (4.3: castle5).
- **Dynamic lights (4.4):** castle4's start at `host_framerate 0.02`,
  `gl_colored_dynamic_lights 1` in the script (without `hexenlicht.cfg`
  the engine reads `config.cfg`, whose is 0; `gl_extra_dynamic_lights`
  stays 0: the renderer makes the missile's light, `r_dumpscene`'s
  dlights include it, `cl.light_level` doesn't), `notarget`, `impulse 43` (weapons, mana, artifacts, below skill 3; 42 only
  prints coordinates), ~90 waits, `impulse 2`, ~150 waits (the weapon
  change), `+attack`: the Necromancer's magic missile (muzzle flash, the
  missile's blue extra light: `vk_lights` counts 1–3); `pause` 8 frames
  after, ~120 waits for the denoiser, shots with `r_dlights` 1 and 0 and
  `r_maplights 0` (the dynamic lights alone). As the Crusader
  (`playerclass 2` before the map) the sun staff (`impulse 4`) lights the
  room with `EF_BRIGHTLIGHT` (radius 400–431). The denoiser: a shot every
  frame from `+attack` with `flt_enable` 1 and 0, the mean linear
  luminance above the HUD: the denoised series follows the flash on its
  first frame (4.4: 0.052 → 0.084, raw 0.091). Cost: `vk_benchmark 1`,
  `vk_profiler` with `r_dlights` 1 and 0 on the paused frame. For `glh2`
  see the next entry (4.4 saw no dynamic light there: castle4's start
  corridor is at GL's lightmap ceiling); with `gl_flashblend 1` it draws
  their bubbles instead of lighting surfaces, a reference for where they
  are and their color.
- **A GL reference for dynamic lights (4.14):** `glh2` adds them to the
  lightmaps, but uHexen2's GL has no overbright: a lightmap texel is
  `min(255, byte × 264 >> 7)` (the normal style value 264 over 128;
  styles are summed before the clip; with `gl_lightmapfmt GL_RGBA` and
  `gl_coloredlight 0`, the data folder's, it stores 0.33 × the sum of the
  clipped channels, so its ceiling is 252), the texture at most at its own
  color: a texel whose compiled byte is about 124 or more is already at
  that ceiling and a dynamic light adds nothing there. Models (GL_RGBA)
  add `radius − distance` times the light's color to their light, which
  the 128/192 clamp doesn't limit, but GL clamps the vertex color at 1, so
  they stop at the texture's color too; pickups (`EF_ROTATE`) and models
  with an `MLS_*` light mode (the torches) get no dynamic light, the
  first-person weapon gets it too. How to see GL's dynamic light:
  - `r_lightmap 1` shows the lightmaps alone, with `gl_multitexture 0`
    and `gl_lightmapfmt GL_RGBA` (the defaults and the data folder's:
    with multitexture it does nothing, with GL_LUMINANCE it shows them
    inverted): white is the ceiling, grey has room. `gl_coloredlight 1`
    colors them only with HoT's `.lit` files, set before `map` (see "Light
    colors (4.3)"); the data folder's configs have `gl_coloredlight`,
    `gl_colored_dynamic_lights` and `gl_extra_dynamic_lights` at 0;
  - measure where both shots are below it, paused, with the light minus
    without, averaged in linear light (`tga_mean.ps1`'s blocks; not over
    the first-person weapon, whose animation changes between the shots);
  - bright lights show best: the Crusader's sun staff (`EF_BRIGHTLIGHT`,
    radius 400–431), explosions. The magic missile's light exists only
    with `gl_extra_dynamic_lights 1` in `glh2` and is blue only with
    `gl_colored_dynamic_lights 1`; in grey lightmaps (`gl_coloredlight 0`)
    a blue light counts a third of a white one (the channels' mean;
    GL_LUMINANCE ignores light colors);
  - GL's surface light adds `2 × (radius − |plane distance| − in-plane
    distance) × color` on the 0–255 scale, the in-plane distance to the
    lightmap sample as max(|ds|, |dt|) + min / 2 in texture units (at
    most ~12 % over the Euclidean), only where it is above `minlight`
    (a hard edge: only the muzzle flash has one, 32), cut off at the
    ceiling: for the brightness compare with that formula (R81) rather
    than with clipped pixels.

  Example (castle4's start, `host_framerate 0.02`, `gl_flashblend 0`,
  `gl_colored_dynamic_lights 1`, `gl_extra_dynamic_lights 1`, the
  Necromancer's magic missile; shots paused about 15–20 frames after the
  attack, when the muzzle flash, 0.1 s = 5 frames, is gone. 4.14 counted
  in an instrumented build that the missile's light marks ~120 surfaces
  and `R_BuildLightMap` adds it to ~85 of them, the drawn ones, each
  frame): the start corridor is white in `r_lightmap 1` before the
  missile, so nothing shows there; a grey block (x 288–384, y 160–256 at
  960x540) goes from 0.21 to 0.46 in the lightmap view, 0.0058 to 0.0099
  textured (sRGB 17 to 25 on the dark stone, easy to miss). The
  Crusader's sun staff makes the side walls about 4–6 times brighter
  (0.0043 → 0.019 and 0.0030 → 0.019, 12 frames after `+attack`).
  Measure walls, not the whole frame: its mean includes the weapon and the
  effects' sprites (a whole-frame +44 % 8 frames after the Necromancer's
  attack was the missile and the hand).
- **Emissive surfaces (4.5):** `vk_lights` on the 16 lava maps (castle4,
  castle5, meso1, meso2, meso5, meso6, meso8, meso9, ravdm1, ravdm5,
  romeric1, romeric3, romeric4, village2, village3; `monsters` with
  `-Portals`): lava triangles, fake lights left out (RENDERER.md's counts),
  list entries. Views at `host_framerate 0.02`, `cl_yawspeed 100`,
  `god`, `notarget`, paused, shots with `r_lava_light` 1 and 0 beside
  `glh2` (the same script: its `noclip` moves the same): meso9's start;
  meso2's lava field (`noclip`, `+forward` 232 frames, `+right` 15,
  `+lookdown` 14: `viewpos` (0 961 0) 40 60 0); castle5's lava by the
  walkway (`noclip`, `+moveright` 207, `+back` 25, `+moveup` 23,
  `+lookdown` 14: (99 −705 −99) 40 270 0; higher up the camera is inside a
  block, where Hexenlicht shows the block's back faces and GL culls them).
  Noclip speeds per frame: forward 4 units, sideways ~4.3, up ~7.5; there
  is no `setpos`. Flames: castle5's start with `r_emissive_models` 1 and
  0, and with `pt_roughness_override 0.05` for their reflections. The
  scale in linear light: `tm_enable 0` shows the raw radiance, directly
  comparable with `glh2`'s shots (texture × lightmap); the light the lava
  adds to a block is linear in `r_emissive_scale` (shots at 0, 4, 8 and
  `r_lava_light 0` for the fake lights).
- **Comparing noisy shots:** average them in linear light (decoded before
  averaging; `tga_mean.ps1` decodes by the sRGB curve, close enough to
  4.17's 2.2 power for comparing two sets): averaged 8-bit values make a
  noisier image look darker (a false 27 % in 3.4's first check). The
  frames of a paused scene still get new random numbers, so averaging
  shots reduces the noise.
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
  frames), `r_maplight_scale 4000`, then `pt_roughness_override 0.02`
  with `pt_metallic_override 0` (mirror: the statue and tombstones
  reflected in the floor), 0.15 (glossy, blurred); `r_debugview 16` shows
  the specular alone, 19 the specular rays' hit distances. Reset the
  overrides to −1 afterwards.
- **Translucency (3.5b):** no translucent surface (`*lowlight`, `*rtex078`)
  is visible from a map start. The cathedral's holy water font is one:
  `showpause 0`, `cl_yawspeed 100`, `cl_pitchspeed 100`, `noclip`, then
  `+right` 48 frames, `+forward` 143, `+moveup` 9, `+lookdown` 22 (at
  `host_framerate 0.02`, as the Necromancer, `playerclass 3`: the classes
  move at different speeds; the font is then right of the crosshair). Compare `pt_reflect_refract 0` and 2 (Q2RTX's
  default): with 2 the odd field of `r_debugview 1` shows the stone floor
  under the font; mode 3 at 0 shows the purple translucent kind in a
  checkerboard. The lit image averages to the surface blended with what is
  behind it.
- **Checkerboard fields (3.12):** `tga_checkerboard.ps1 -Dir <shots>
  -Shots (1..16) -MaxY <HUD row> [-Region x,y,w,h] [-Heat h.tga]` averages
  paused shots in linear light and scores a fine checkerboard per 8x8
  block (up to 2; the font's two fields unresolved score 1.4–1.6, the same
  view without the split up to ~0.7 with the noise of 16 raw frames,
  denoised images ~0.1–0.25 at the weapon's edges). Without the denoiser
  the fields swap every frame, so each frame keeps a checkerboard: take an
  even number of shots at alternating intervals (`screenshot` then 3 and 4
  waits in turn) so both parities count. Single noisy frames score high
  from noise alone; score them only with the denoiser or DLSS. At the
  font (the route above, then `+back` 20 and 40 frames and `+moveright`
  25): the denoised image with TAA and TAAU, DLSS RR at 100 % and 67 %,
  and 16 raw frames or DLSS SR with `flt_enable 0` all stay below 0.4 on
  the font (TAA without jitter reaches ~0.4 on the weapon's edge).
- **Water stays opaque (3.5b):** with `pt_reflect_refract 2`, romeric1's
  pool (look down at the start) and egypt4's vertical water walls (`+right`
  69 frames) must match the build before 3.5b pixel for pixel in the
  G-buffer modes (1, 3, 9, 2), except that mode 3 now shows vertical water
  as water (blue) instead of glass. romeric2's centre isn't repeatable
  between runs (its rotating brushes): mask it with a second run
  (`tga_diff.ps1 -Noise`).
- **Denoiser and light styles (4.13):** castle5's pulse as in "Light
  styles (4.2)" (`noclip`, `+forward` 150 frames, then 60 shots 4 frames
  apart) with `tm_enable 0`, `bloom_enable 0`, `r_maplight_scale 100`,
  once with `flt_enable 1` and once with 0; the per-shot mean linear
  luminance above the HUD, the ratio denoised / raw (4.13: rise
  0.93–0.97, fall 1.17–1.64; `main` 0.70–0.72 and 1.48–2.34;
  `flt_antilag_style 0` gives `main`'s gradient formula: 0.73–0.76 against `main`'s 0.70–0.73, the light choice stays). A bias check: `pause` and
  ~250 waits, then shots with `flt_enable` 1 and 0 must agree. Paused
  gradients: rider2c, `+lookup` 20 frames, paused, `flt_show_gradients 1`:
  no green or blue along the lit wall's upper edge (16-bit barycentrics
  put a band there); the red (LF) there is below 1 % — the overlay is added
  before the exposure: read its size with `tm_enable 0` (the red channel's
  linear difference to a shot without the overlay). Noise: the spatial
  noise of the same shots against `main`'s.
- **Denoiser (3.6):** `flt_enable 0` must match the build before it
  (3.6: demo1, cath, romeric2 × modes 0 1 2 3 9 15 16 18, paused, `vk_testlight
  entities`: the G-buffer modes identical everywhere, demo1 in all modes;
  cath's and romeric2's lighting modes differ between runs of the same
  build in a few dozen to a few hundred pixels, in `main` too, and
  `-Noise` can't mask it: compare several runs of each). Checks, paused
  with the map's lights, `r_debugview 0`, `tm_enable 0`, `bloom_enable 0`
  (linear light; since 3.7): a denoised shot against the average
  of 16–24 `flt_enable 0` shots (`tga_mean.ps1`), **at a sixteenth of the
  lights' intensity** (`r_maplight_scale 62.5`; before 4.1 `vk_testlight
  entities 62.5`): at full intensity the
  raw frames clip at 1 before the screenshot and their average reads
  4–10 % dark; and with `pt_fake_roughness_threshold 1` or
  `pt_num_bounce_rays 0`, because only the denoiser gives rough surfaces
  indirect specular (3.6: +2 % demo1, +5 % the cathedral's font). Paused,
  `flt_show_gradients 1` must show no gradients (in directly lit views:
  weakly lit, bounce-lit areas such as rider2c's arena ceiling show some,
  4.1 too at a lower `r_maplight_scale`; story 4.13) and `r_debugview 20` full
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
  at demo1's start, then `r_maplight_scale 62.5`: `vk_exposure` and the
  image mean (`tga_mean.ps1 -A n`) come back over ~6 s to 1/16 and ~75 %
  of before (3.7, with test lights at the light entities); `r_maplight_scale
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
  test lights' hot spots, which the PQ clamp dims (romeric2: up to 10;
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
- **DLSS (3.10):** without the DLLs, `r_upscaler 0–2` must match `main`
  pixel for pixel and 3 and 4 give TAAU (3.10: demo1 identical in 12
  cases, the cathedral within ±1 as between two `main` runs; with
  `tm_enable 0 bloom_enable 0`, since paused the exposure adapts in real
  time and runs differ; print `vk_upscale` a few frames before
  `screenshot`, or its lines are in the shot). With the DLLs (SETUP.md §5):
  `r_upscaler 3` and 4 at `r_scale` 100, 67, 50, 33, 25 (`vk_upscale`,
  `vk_dlss`: mode, render size, evaluations `eOk`, Streamline's three hook
  warnings and no errors), switching while turning, `vid_restart`, a map
  change, `resize_test.ps1`, `pt_num_bounce_rays 2` against 1 (RR: same
  mean), particles in motion (Crusader's meteor staff while turning). RR's
  image differs between runs of the same build (up to ±17 per channel:
  NGX isn't bit-exact), so compare with a threshold (more than 8) and a
  second run. A tampered `sl.interposer.dll` (a byte flipped in its DOS
  stub keeps it loadable) must be refused; Streamline's development DLLs
  are signed and load. Synchronization validation
  (`VK_KHRONOS_VALIDATION_VALIDATE_SYNC=1`, and
  `VK_KHRONOS_VALIDATION_DUPLICATE_MESSAGE_LIMIT` raised past 10): 3.10
  found 19 hazards, all between NGX's own commands on its RR resources
  (`nv.ngx.dlssd.resource`), none on ours; `main` has none. Remove the
  DLLs afterwards.
- **GPU cost** (since 3.11): the profiler (RENDERER.md's Profiler section)
  with **`vk_benchmark 1`**, which lifts the 72 fps cap, the frame
  throttle and the sleep of an unfocused or paused window (a game
  `hl_run.ps1` starts often isn't in front), so the GPU runs at full load
  and steady clocks. Without it the
  clocks swing between idle and bursts and the times scatter both ways
  (3.11: the cathedral at 100 % measured 7.6 ms unfocused at the cap, 13.6
  with `vk_benchmark 1`; 3.10's first run measured an idling GPU at
  0.3–0.6 GHz). Set `profiler_samples` (e.g. 120), wait longer than that
  after a change, then `vk_profiler` prints the averages.
  `tools/hexenlicht/perf_baseline.ps1` does this for demo1's and the
  cathedral's starts at 1920x1080 and 2560x1440 (`-Sizes`), TAAU at 100 %
  and 67 % and DLSS RR at 67 % when the DLLs are in the build folder, and
  prints markdown tables (`-Out` writes them); it backs up and restores the
  configs. Compare with a baseline measured the same day: this machine's GPU
  runs at a ~100 W power limit (about 1 GHz at full load; watch with
  `nvidia-smi --query-gpu=clocks.gr,power.draw,clocks_throttle_reasons.active
  --format=csv -lms 250`: 0x4 the power cap, 0x1 idle).
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
