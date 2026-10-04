# Hexenlicht — syncing with upstream Hammer of Thyrion

Hexenlicht keeps the full history of [sezero/uhexen2](https://github.com/sezero/uhexen2)
and merges upstream changes into `main` from time to time. This page is the
procedure.

## Setup (once per clone)

```
git remote add upstream https://github.com/sezero/uhexen2.git
git fetch upstream
git branch --track master upstream/master   # if master does not exist yet
```

- `origin` = zerga/hexenlicht, `upstream` = sezero/uhexen2.
- Local `master` is a **read-only mirror** of `upstream/master`. Never commit
  to it and never push it to `origin`.
- `main` is Hexenlicht.

## When to sync

At the start of each epic, and whenever upstream publishes a release.
To see whether there is anything to merge:

```
git fetch upstream
git log --oneline main..upstream/master
```

## Procedure

1. **Update the mirror.**
   ```
   git switch master
   git merge --ff-only upstream/master
   ```
2. **Note the previous sync point** and start a sync branch from `main`:
   ```
   git switch main && git pull --ff-only
   git merge-base main master          # = OLD, the last upstream commit we have
   git switch -c sync/upstream-YYYY-MM-DD
   ```
3. **Merge** (always a real merge; never rebase or squash upstream history):
   ```
   git merge --no-ff master -m "Merge upstream uHexen2 (<short sha of master>)"
   ```
4. **Resolve conflicts**, if any: keep upstream's change and re-apply ours on
   top. Conflicts can only happen in upstream files Hexenlicht has changed
   (see [hot spots](#conflict-hot-spots)).
5. **Check what upstream changed that our build mirrors.** With `OLD` from
   step 2:

   | Command | If it shows changes |
   |---|---|
   | `git diff OLD master -- engine/hexen2/Makefile` | Update the source lists in `cmake/Hexen2Sources.cmake` (win64 object lists) |
   | `git diff OLD master -- h2patch/Makefile h2patch/VisualStudio.zip` | Update the `h2patch` target in `CMakeLists.txt` |
   | `git diff --stat OLD master -- oslibs/windows libs/timidity libs/xdelta3` | Update `H2_CODEC_DLLS` / codec libraries in `CMakeLists.txt` and the upstream table in `THIRD_PARTY.md` |
   | `git diff OLD master -- engine/h2shared/h2config.h` | Read it: compile-time options that may affect the Hexenlicht renderer |
   | `git diff --stat OLD master -- .github/workflows` | Upstream CI changed; our `build-windows.yml` is separate but may need a similar change |

6. **Build and run**: both presets, then the smoke test and a short play
   (see [SETUP.md](SETUP.md)):
   ```
   cmake --build --preset windows-debug
   cmake --build --preset windows-release
   ```
7. **Pull request**: push the branch, open a PR titled
   *Upstream sync YYYY-MM-DD* with the label `upstream`, listing the merged
   upstream commits (`git log --oneline OLD..master`). Both the Windows and
   upstream's Linux CI must pass.
8. **Merge the PR with "Create a merge commit"** — never *Squash* or
   *Rebase*: those would drop upstream's commits from our history and make
   every later sync conflict.

## Safety nets

- The **Windows CI** fails when `cmake/Hexen2Sources.cmake` lists a file
  upstream removed (configure error) or misses a file upstream added (link
  errors for the new functions).
- Upstream's **Linux CI** (`build-linux.yml`, Makefile build) keeps running
  on every push and PR and shows whether Hexenlicht's changes broke the
  upstream build.

## Conflict hot spots

Upstream files that Hexenlicht modifies. List them here as soon as a story
changes one, with a line on what we changed:

- `engine/hexen2/r_part.c` — `R_DrawParticles` (the OpenGL and the
  software version) is left out when `HEXENLICHT` is defined: an
  `#if defined(HEXENLICHT)` branch in front of the existing
  `#if defined(GLQUAKE)` (story 1.1). The particle simulation is reused
  unchanged.
- `engine/h2shared/gl_screen.c` — in `SCR_ScreenShot_f`, after the file
  name is chosen, `#if defined(HEXENLICHT)` calls `VK_RequestScreenshot()`
  instead of the `glReadPixels` part (story 1.6). The rest of the screen
  layout is reused unchanged.
- `engine/hexen2/host.c` — Hexenlicht's settings file is `hexenlicht.cfg`
  (story 1.8): a `CONFIG_NAME` define after the `Host_WriteConfiguration`
  prototype, used by `Host_Shutdown`; in `Host_Init` an
  `#if defined(HEXENLICHT)` branch opens it for the early cvar reads,
  falling back to `config.cfg`. `Host_FilterTime` (story 3.11): before its
  72 fps check, an `#if defined(HEXENLICHT)` `if (VK_Benchmark ()) { } else`
  skips the check (`vk_benchmark 1`, `vk_profiler.c`), with the prototype
  above the function.
- `engine/hexen2/sys_win.c` — in `WinMain`'s loop, before the sleep for a
  paused, minimized or unfocused window, an `#if defined(HEXENLICHT)`
  `if (VK_Benchmark () && !Minimized && !block_drawing) { } else` skips it,
  and before the `sys_throttle` sleep after `Host_Frame` an
  `if (VK_Benchmark ()) { } else` skips that (story 3.11); the prototype
  after the `ActiveApp` globals.
- `engine/h2shared/cmd.c` — `Cmd_Exec_f` takes the file name into a local
  `name`; under `#if defined(HEXENLICHT)`, `exec config.cfg` runs
  `hexenlicht.cfg` when that file exists (story 1.8), so `hexen.rc` loads it.
- `engine/hexen2/render.h` — `entity_t` ends with an `#if defined(HEXENLICHT)`
  field `movestep` (story 2.9), after all upstream fields.
- `engine/hexen2/cl_tent.c` — the streams (beams) reach the renderer
  (story 6.3): under `#if defined(HEXENLICHT)`, the prototypes of
  `R_ClearBeams` and `R_AddBeam` (`engine/hexenlicht/r_scene.c`) after the
  "EXTERNAL FUNCTION PROTOTYPES" banner; in `CL_UpdateTEnts`,
  `R_ClearBeams ()` after `StreamEntityCount = 0`, and `R_AddBeam (...)`
  for each drawn stream after its attached source is updated (type,
  skin, source, dest, end time, its models). `engine/hexen2` only:
  HexenWorld's client has its own `cl_tent.c`.
- `engine/hexen2/cl_main.c` — demo playback's view angles (story 6.7):
  under `#if defined(HEXENLICHT)`, the prototypes of `CL_DemoAnglesBeforeParse`
  and `CL_DemoAngles` (`engine/hexenlicht/cl_demoangles.c`) above
  `CL_LerpPoint`; in `CL_RelinkEntities`, `if (cls.demoplayback)
  CL_DemoAngles ();` replaces the angle lerp (`#else` keeps upstream's);
  in `CL_ReadFromServer`, `CL_DemoAnglesBeforeParse ()` before the message
  loop. `engine/hexen2` only: HexenWorld's client has its own `cl_main.c`.
- `engine/hexen2/cl_parse.c` — in `CL_ParseUpdate`, after the `U_NOLERP`
  check, `#if defined(HEXENLICHT)` sets `ent->movestep` from `U_NOLERP`
  (story 2.9): `CL_RelinkEntities` clears `forcelink` before the renderer
  runs, and Hexenlicht's `r_lerpmove` needs to know which entities step.

- `engine/h2shared/quakefs.c` — after `FS_FileInGamedir`, an
  `#if defined(HEXENLICHT)` function `FS_ListSearchPath` (story 5.3): walks
  `fs_searchpaths` in its order, giving each pak entry with a prefix (in
  any case; its name, position, size and pak) and each game directory to
  callbacks (the material files' index,
  `vk_matfiles.c`; declared in `engine/hexenlicht/vk_local.h`). Nothing
  else changes; the guard keeps it out of HexenWorld and the Makefile
  builds.

To check: `git diff --name-status upstream/master main | grep -v "^A"`
prints every upstream file that differs on `main`.

## Sync log

| Date | Upstream commit | PR | Notes |
|---|---|---|---|
| 2026-09-24 | `475c048b1` | #89 (story 0.7) | Procedure dry run: no new upstream commits since the fork point, merge was a no-op |
