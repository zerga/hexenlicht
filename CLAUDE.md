# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Hexenlicht (this fork)

This checkout is **Hexenlicht**, a Vulkan path-traced fork of Hammer of Thyrion (uHexen2, the Hexen II source port), for Windows x64, built with CMake + MSVC in CLion. The renderer reuses Quake II RTX code. Live status (epics, stories, open PRs) is in the GitHub tracker, not in the docs: `gh issue list --repo zerga/hexenlicht` — **always pass `--repo zerga/hexenlicht`**; the default is the upstream repo.

**The owner drives:** research and propose each story's approach (one recommendation with reasons, what is left out, deviations from GL), and implement only after approval; "go ahead with proposing X" means propose, not implement. Before proposing a story, read its PLAN §8 row and the DECISIONS.md lines for its area.

## Where to find things

Read only what the task needs.

| Topic | Document |
|---|---|
| Plan: settled decisions, scope, licensing (no NVIDIA DLSS binaries or NGX static libs, no Raven-derived assets), DLSS, epics and stories | [docs/hexenlicht/PLAN.md](docs/hexenlicht/PLAN.md) |
| Technical decisions made in stories, with where | [docs/hexenlicht/DECISIONS.md](docs/hexenlicht/DECISIONS.md) |
| Renderer modules (`engine/hexenlicht/`), shaders, GPU layouts, console commands | [docs/hexenlicht/RENDERER.md](docs/hexenlicht/RENDERER.md) |
| Quake II RTX module map and import rules | [docs/hexenlicht/Q2RTX.md](docs/hexenlicht/Q2RTX.md) |
| Testing: scripted runs, screenshots, pixel regression, check commands, maps | [docs/hexenlicht/TESTING.md](docs/hexenlicht/TESTING.md), tools in [tools/hexenlicht](tools/hexenlicht/README.md) |
| Development setup: Vulkan SDK, game data, CLion | [docs/hexenlicht/SETUP.md](docs/hexenlicht/SETUP.md) |
| Upstream merges and conflict hot spots | [docs/hexenlicht/UPSTREAM.md](docs/hexenlicht/UPSTREAM.md) |
| The upstream engine: architecture, Makefile builds, gamecode | [docs/hexenlicht/ENGINE.md](docs/hexenlicht/ENGINE.md) |
| Third-party code and licenses | [THIRD_PARTY.md](THIRD_PARTY.md) |

Each file in `engine/hexenlicht/` starts with a header comment describing it. **Keep the docs current:** a PR that changes a module updates its RENDERER.md section; a settled decision gets a DECISIONS.md line; imported Q2RTX code updates Q2RTX.md and THIRD_PARTY.md.

## How we work

- One story → branch `story/<num>-<slug>` from `main` after `git pull` (never on an unmerged PR's branch unless the owner says so) → PR with `Closes #<issue>` (story PRs carry no labels or milestone; upstream sync PRs get the label `upstream`). Board: "In progress" when starting, "In review" with the PR. Turn on the app's CI Auto-fix for the PR; never auto-merge — the owner merges. When an epic's last story closes, close the epic by hand and set it Done on the board.
- New work found mid-story becomes a new story issue (sub-issue of its epic, board Backlog) and a PLAN §8 row in the story's PR. PLAN.md changes only when a decision or the scope changes.
- Before committing, have an independent code-review subagent review the change; apply its findings and re-test.
- Report honestly: what was verified and how, what wasn't exercised, own mistakes, harness glitches vs. real bugs.
- Commits end with the `Co-Authored-By` line, PR bodies with the Claude Code line.

## Build and verify

- Presets `windows-debug` and `windows-release`; output in `build/<preset>/bin/`. A full build of either (not just `--target hexenlicht`) also builds `glh2.exe`, the unmodified GL client and look reference. Build both presets before a PR.
- Command-line builds need the MSVC environment (`<VS install>\VC\Auxiliary\Build\vcvarsall.bat x64`) with CMake and Ninja on `PATH` (CLion bundles both: `<CLion>\bin\cmake\win\x64\bin`, `<CLion>\bin\ninja\win\x64`) and `VULKAN_SDK` set. From Git Bash, pass MSVC options with `-` instead of `/`.
- New `.c` files go into `add_executable(hexenlicht ...)` in `CMakeLists.txt` (then re-run `cmake --preset <preset>`); new shaders into `hexenlicht_add_shaders(...)`. A shader change needs no relink: build `hexenlicht_shaders`, then `vk_reload_shaders` in the game.
- Every Debug run must end with `Vulkan validation: 0 errors, 0 warnings` in the log. There are no test suites: see TESTING.md.
- With Git for Windows' default `core.autocrlf=true` the working tree is CRLF (the index LF): keep edited and new files CRLF. Git Bash `sed -i` and the Write tool produce LF — normalize with `sed -i 's/\r$//; s/$/\r/' f` and check that `tr -cd '\r' < f | wc -c` equals `wc -l < f` (grep can't see CRs).

## Pitfalls

- **Never print to the console between `VK_BeginFrame` and the end of `VK_EndFrame`**: `SCR_UpdateScreen` re-enters via `Con_Printf`. Count problems and report them from console commands.
- **GL renderer functions can have game-visible side effects** (`R_DrawViewModel` sets `cl.light_level`, which the server uses): check what a replaced GL function writes besides pixels.
- Hunk allocations move the model cache: don't keep `Mod_Extradata` pointers across `GL_LoadTexture`/`Draw_CachePic` and the like.
- `va()` has only 4 rotating buffers.
- `shaders/hl_shared.h` and the headers it includes must compile in C and in every shader; C/GLSL struct layouts must match (UBO: `tools/hexenlicht/ubo_layout_check.ps1`).
- Vulkan: a dynamic BLAS's geometry flags must not change between its size query and its builds; `gl_RayFlagsOpaqueEXT` overrides instance flags; effects geometry needs `NO_DUPLICATE_ANY_HIT`.
- MSVC: don't `(void)`-cast uninitialized locals; `const vec3_t` can't be passed to `AngleVectors`/`Mod_PointInLeaf`; a `static` definition of a function `glquake.h` declares extern compiles silently (undefined behaviour).
- Hexen II data: vis is asymmetric; qbsp leaves faces facing into solid; there are no standalone brush models; frozen monsters are colormap 159→144 + translucent, then skin 101.

## Upstream

Upstream (`sezero/uhexen2`) is merged into `main` following [UPSTREAM.md](docs/hexenlicht/UPSTREAM.md): local `master` is a read-only mirror of `upstream/master`, syncs are real merges (never rebase/squash). New code goes into new files; edits to upstream files are minimal, guarded with `#if defined(HEXENLICHT)`, and added to UPSTREAM.md's "Conflict hot spots". Edits in `engine/h2shared/` must keep HexenWorld building (upstream's Linux CI checks it). Read `docs/SrcNotes.txt` before "cleaning up" client/server layering and `engine/h2shared/h2config.h` before changing engine behavior; see ENGINE.md for the rest.
