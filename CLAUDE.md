# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Hexenlicht (this fork)

This checkout is being turned into **Hexenlicht**, a Vulkan path-traced fork of Hammer of Thyrion (Windows x64, CMake + MSVC in CLion). Read `docs/hexenlicht/PLAN.md` before starting any Hexenlicht work — it holds the settled decisions, licensing rules (no NVIDIA DLSS binaries or NGX static libs in the repo), and the epic/story breakdown. Live status is in the GitHub issue tracker, not in the plan. The owner drives: propose each story's approach and get approval before implementing.

Upstream (`sezero/uhexen2`) is merged into `main` following `docs/hexenlicht/UPSTREAM.md`: local `master` is a read-only mirror of `upstream/master`, syncs are real merges (never rebase/squash). When a change modifies an upstream file (rather than adding a new one), add it to the "Conflict hot spots" list there.

## What this is

Hexen II: Hammer of Thyrion (uHexen2) — a cross-platform source port of Raven Software's GPL-released Hexen II / HexenWorld engine. Heavily descended from the Quake codebase. C with x86 NASM assembly fast paths. GPLv2.

Current version: see `HOT_VERSION_*` in `engine/hexen2/quakedef.h` (1.5.10 at time of writing). The HexenC bytecode (gamecode) carries its own version (`ENGINE_VERSION` in the same file, currently 1.29 — these are not the same number).

## Hexenlicht CMake build (Windows x64, MSVC)

Root `CMakeLists.txt` + `CMakePresets.json` (presets `windows-debug`, `windows-release`; Ninja; output in `build/<preset>/bin/`). Source lists live in `cmake/Hexen2Sources.cmake` and mirror the win64 object lists in `engine/hexen2/Makefile` — when an upstream merge changes the Makefile's object lists, update that file too. Target `glhexen2` (output `glh2.exe`) is the unmodified OpenGL client. Target `hexenlicht` is the Hexenlicht client: `H2_COMMON_SOURCES` + `H2_HEXENLICHT_REUSED_SOURCES` (GL-renderer files without GL calls: `gl_model.c`, `gl_mesh.c`, `r_part.c`, `gl_refrag.c`) + new code in `engine/hexenlicht/`, compiled with `GLQUAKE` (client code takes its hardware-renderer paths) and `HEXENLICHT`. `engine/hexenlicht/stubs.c` provides every renderer symbol the engine needs as a placeholder, sectioned by the story that replaces it; when implementing a story, move its section out of `stubs.c` into real files. To find what a renderer must provide, link without it and read the unresolved externals. Codec DLLs from `oslibs/windows/codecs/x64` are copied next to the exe post-build.

Configuring requires the LunarG Vulkan SDK 1.3+ via `VULKAN_SDK` (CI pins the version in `.github/workflows/build-windows.yml`). Vendored libraries for the Vulkan renderer are static-library targets `volk` (`libs/volk`), `vma` (`libs/vma`, C++ implementation in `vma_impl.cpp`, uses volk's loaders via `VMA_DYNAMIC_VULKAN_FUNCTIONS`) and `stb_image` (`libs/stb`, PNG/TGA only, `STBI_NO_STDIO`). Versions and licenses are recorded in `THIRD_PARTY.md`; update it whenever a vendored library changes.

Command-line builds need the MSVC environment: run `<VS install>\VC\Auxiliary\Build\vcvarsall.bat x64` first, with CMake and Ninja on `PATH` (CLion bundles both under `<CLion>\bin\cmake\win\x64\bin` and `<CLion>\bin\ninja\win\x64`). From Git Bash, pass MSVC tool options with `-` instead of `/` (Git Bash rewrites `/opt` into a path).

## Upstream build environment (Makefiles)

Builds **require a Unix-like shell** — bash, GNU make, plus the helper scripts under `scripts/`. On Windows, that means MSYS/MSYS2 or WSL; native cmd/PowerShell will not work for the Makefiles. The makefile machinery (driven by `scripts/makefile.inc`) shells out to `uname`/`sed`/`tr`/`which` to auto-detect host and target. Watcom builds (`Makefile.wat`/`Makefile.os2`) are the exception — those use `wmake` natively.

`nasm` or `yasm` is required for the ia32 software-renderer builds; the OpenGL builds are built with `USE_X86_ASM=no` (see `build_all.sh`) and don't need an assembler. SDL 1.2 dev libs are required for the clients on Unix.

Compile-time options that aren't makefile vars live in `engine/h2shared/h2config.h` — read it before changing engine behavior.

## Build commands

Each component has its own Makefile in its own directory; there is **no top-level Makefile**. You must `make -C <dir>` (or `cd` in). Always pair targets with a `clean`/`localclean` between renderer variants — the same object files get rebuilt with different defines, and stale `.o`s will silently corrupt the next link.

Hexen II engine (`engine/hexen2/`):
```sh
make -C engine/hexen2 h2          # software renderer client (h2 / hexen2)
make -C engine/hexen2 localclean  # drop *.o but keep libtimidity.a
make -C engine/hexen2 glh2 USE_X86_ASM=no   # OpenGL client (glh2 / glhexen2)
make -C engine/hexen2/server      # dedicated server (h2ded)
# or use the helper, which orchestrates the three above:
sh engine/hexen2/build_all.sh
```

HexenWorld (`engine/hexenworld/`):
```sh
make -C engine/hexenworld/server                       # hwsv
make -C engine/hexenworld/client hw                    # hwcl (software)
make -C engine/hexenworld/client glhw USE_X86_ASM=no   # glhwcl
sh engine/hexenworld/build.sh                          # builds all three
```

Other components, each with its own Makefile:
- `h2patch/` — pak0/pak1 → v1.11 patcher (uses libxdelta3 in `libs/xdelta3/`)
- `utils/hcc/` — HexenC compiler (needed to rebuild `gamecode/`)
- `utils/{qbsp,light,vis,bsp2map,bspinfo,genmodel,qfiles,dcc,pak,jsh2color,texutils/*}` — map/asset tools
- `hw_utils/{hwmaster,hwmquery,hwrcon}` — HexenWorld master server + query/rcon clients
- `libs/timidity/` — bundled libtimidity (built transitively by the engine via `_timi.mak`)

Cross-compilation flags (set as `make` vars or env): `W32BUILD=1`, `W64BUILD=1`, `DOSBUILD=1`, `OSXBUILD=1`. The matching `build_cross_*.sh` scripts in each component dir set up the toolchain. Other useful flags: `DEMO=1` (demo build), `DEBUG=1` (debug symbols + extra checks), `USE_X86_ASM=no` (required on non-ia32 and for GL).

CI matrix lives in `.github/workflows/build-linux.yml` and is the canonical "what gets built and how" reference if a Makefile change goes sideways.

There are no test suites — verification is build-the-binaries-and-run-the-game.

## Gamecode (HexenC) builds

`gamecode/hc/` contains HexenC source for the four progs.dat variants — `h2/` (Hexen II), `portals/` (mission pack / Portal of Praevus), `hw/` (HexenWorld), `siege/` (HW Siege). These are compiled by `hcc` into `progs.dat`-style bytecode:

```sh
hcc -os                          # original Hexen II progs
hcc -os -name progs2.src         # mission pack progs2
hcc -os -oi -on                  # mission pack / HW / Siege progs (optimized)
```

**Do not pass `-oi` or `-on` when rebuilding the base Hexen II progs** — those optimizations break old save-game compatibility and produce harmless-but-noisy runtime warnings. See `gamecode/COMPILE`.

## Architecture overview

Two parallel game engines share a large amount of code. Both follow the Quake client-server-progs model.

```
engine/
├── h2shared/   ── code shared between hexen2 and hexenworld
├── hexen2/     ── single-player / classic LAN multiplayer client + server (combined binary)
├── hexenworld/
│   ├── shared/  ── client+server shared HW code
│   ├── client/  ── QuakeWorld-style network client (hwcl/glhwcl)
│   └── server/  ── standalone dedicated server (hwsv)
└── resource/   ── icons, win32 manifests
common/         ── headers used by BOTH engine and utils (endian, ctype, pakfile, etc.)
```

`h2shared/` is the giant pile — sound system (`snd_*.c`, with one file per codec/backend), video drivers (`vid_*.c`, `gl_vid*.c`), input (`in_*.c`), platform sys layers (`sys_*.c`), the software rasterizer (`d_*.c`/`r_*.c` plus `.asm` fast paths), and the QuakeC VM (`pr_exec.c`, `pr_edict.c`, `pr_cmds.c`). Backend selection is at compile time via `USE_*` makefile vars feeding `#ifdef`s.

The hexen2 client and server are **linked into a single binary** that can be started either as a client or a dedicated server (the `server/` subdirectory builds the dedicated-only variant). HexenWorld follows the QuakeWorld split: separate `hwsv` and `hwcl`/`glhwcl` binaries.

Per-platform sys layer: `sys_unix.c`, `sys_win.c`, `sys_dos.c`, `sys_os2.c`, `sys_amiga.c`, plus the OSX glue in `sys_osx.m`. Networking is similarly split (`net_bsd.c`/`net_win.c`/`net_dgrm.c`/...).

Two renderers are always built per-client: software (the `d_*`/`r_*` family, with optional NASM fast paths from the `*.asm` files) and OpenGL (`gl_*.c`). The software renderer is the only place x86 asm matters; GL builds always disable it.

### Client/server intrusion points

The hexen2 client and server are bundled and the boundary is leaky in specific known places — `docs/SrcNotes.txt` documents the intentional cross-calls (loading progress bar, GL texture flush on map change, `sv_kingofhill` mission-pack hack, `cl_playerclass` propagation, `sv_gravity` particle physics). Read that file before "cleaning up" any apparent layering violation between `sv_*.c` and `cl_*.c` — most of them are load-bearing.

### Hexen II vs. HexenWorld in shared code

Many `h2shared/` files use `#ifdef H2W` (or similar) to compile slightly different behavior for the HexenWorld variant. When changing shared code, check both engines build — the HW client is the more common second casualty.

## Conventions to know

- The makefile system **requires GNU make**, not BSD make. BSD users invoke `gmake` (the `build_*.sh` scripts pick this automatically).
- A `make clean` between renderer variants is mandatory; `make localclean` is the cheaper version that preserves `libtimidity.a`.
- x86 asm files live alongside their C counterparts (`d_draw.asm` next to `d_draw.c` etc.) and are conditionally assembled. `MACH_TYPE != x86` auto-sets `USE_X86_ASM=no`.
- Pre-1.11 Hexen II retail compatibility is gated behind `ENABLE_OLD_RETAIL` in `h2config.h`; the engine otherwise assumes pak files have been patched to v1.11 by `h2patch`.
- Documentation, release notes, and platform-specific READMEs live in `docs/`. Compile instructions for end-users are in `docs/COMPILE`.

## Important files at a glance

- `engine/hexen2/quakedef.h` — engine version constants, gameplay limits, `QUAKE_GAME` define.
- `engine/h2shared/h2config.h` — non-Makefile compile-time options with extensive comments.
- `scripts/makefile.inc` — host/target OS detection, included by every component Makefile.
- `docs/SrcNotes.txt` — the "known intentional weirdness" file. Skim before refactoring.
- `docs/COMPILE` — full toolchain/library requirements.
- `gamecode/README` and `gamecode/COMPILE` — gamecode change history and how to rebuild progs.dat.
