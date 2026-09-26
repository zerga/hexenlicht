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
   ..\hexenlicht-main\build\windows-debug\bin` runs the old build).
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
