# Hexenlicht — the Hammer of Thyrion engine

Notes on the upstream engine (uHexen2) that Hexenlicht is built on: its
architecture, its own Makefile builds and the gamecode. Hexenlicht itself
builds with CMake (see [RENDERER.md](RENDERER.md#build-target)).

## What it is

Hexen II: Hammer of Thyrion (uHexen2) — a cross-platform source port of Raven
Software's GPL-released Hexen II / HexenWorld engine. Heavily descended from
the Quake codebase. C with x86 NASM assembly fast paths. GPLv2.

Current version: `HOT_VERSION_*` in `engine/hexen2/quakedef.h` (1.5.10 at
the time of writing). The HexenC bytecode (gamecode) carries its own version
(`ENGINE_VERSION` in the same file, currently 1.29 — not the same number).

## Architecture

Two parallel game engines share a large amount of code. Both follow the Quake
client-server-progs model.

```
engine/
├── h2shared/   ── code shared between hexen2 and hexenworld
├── hexen2/     ── single-player / classic LAN multiplayer client + server (combined binary)
├── hexenworld/
│   ├── shared/  ── client+server shared HW code
│   ├── client/  ── QuakeWorld-style network client (hwcl/glhwcl)
│   └── server/  ── standalone dedicated server (hwsv)
├── hexenlicht/ ── Hexenlicht's renderer and window layer
└── resource/   ── icons, win32 manifests
common/         ── headers used by BOTH engine and utils (endian, ctype, pakfile, etc.)
```

- `h2shared/` is the giant pile — sound (`snd_*.c`, one file per
  codec/backend), video drivers (`vid_*.c`, `gl_vid*.c`), input (`in_*.c`),
  platform layers (`sys_*.c`), the software rasterizer (`d_*.c`/`r_*.c` plus
  `.asm` fast paths), and the QuakeC VM (`pr_exec.c`, `pr_edict.c`,
  `pr_cmds.c`). Backends are selected at compile time via `USE_*` makefile
  vars feeding `#ifdef`s.
- The hexen2 client and server are **linked into a single binary** that can
  run as a client or a dedicated server (`server/` builds the dedicated-only
  variant). HexenWorld has separate `hwsv` and `hwcl`/`glhwcl` binaries.
- Per-platform sys layers: `sys_unix.c`, `sys_win.c`, `sys_dos.c`,
  `sys_os2.c`, `sys_amiga.c`, `sys_osx.m`; networking likewise
  (`net_bsd.c`/`net_win.c`/`net_dgrm.c`/...).
- Two renderers per upstream client: software (`d_*`/`r_*`, optional NASM
  fast paths) and OpenGL (`gl_*.c`). Only the software renderer uses x86 asm.

### Client/server intrusion points

The hexen2 client and server are bundled and the boundary is leaky in known
places — `docs/SrcNotes.txt` documents the intentional cross-calls (loading
progress bar, GL texture flush on map change, `sv_kingofhill` mission-pack
hack, `cl_playerclass` propagation, `sv_gravity` particle physics). Read it
before "cleaning up" an apparent layering violation between `sv_*.c` and
`cl_*.c` — most are load-bearing.

### Hexen II vs. HexenWorld in shared code

Many `h2shared/` files use `#ifdef H2W` (or similar) for HexenWorld. When
changing shared code, check both engines build — the HW client is the usual
second casualty. (HexenWorld is out of Hexenlicht's scope, but upstream code
must keep building.)

## Important files

- `engine/hexen2/quakedef.h` — engine version constants, gameplay limits, `QUAKE_GAME`.
- `engine/h2shared/h2config.h` — non-Makefile compile-time options, with
  extensive comments; read it before changing engine behavior.
- `scripts/makefile.inc` — host/target OS detection, included by every component Makefile.
- `docs/SrcNotes.txt` — the "known intentional weirdness" file.
- `docs/COMPILE` — full toolchain/library requirements.
- `gamecode/README`, `gamecode/COMPILE` — gamecode history and how to rebuild progs.dat.

## Upstream Makefile builds

Builds **require a Unix-like shell** — bash, GNU make (not BSD make; BSD users
run `gmake`, which the `build_*.sh` scripts pick automatically), plus the
helper scripts under `scripts/`. On Windows that means MSYS/MSYS2 or WSL;
native cmd/PowerShell will not work for the Makefiles. The makefile machinery (`scripts/makefile.inc`) shells out
to `uname`/`sed`/`tr`/`which` to detect host and target. Watcom builds
(`Makefile.wat`/`Makefile.os2`) use `wmake` natively.

`nasm` or `yasm` is required for the ia32 software-renderer builds; the
OpenGL builds use `USE_X86_ASM=no` (see `build_all.sh`) and need no assembler. SDL 1.2 dev libs are
required for the clients on Unix.

Each component has its own Makefile; there is **no top-level Makefile**. Always
`clean`/`localclean` between renderer variants — the same object files are
rebuilt with different defines, and stale `.o`s silently corrupt the next
link (`localclean` keeps `libtimidity.a`).

```sh
make -C engine/hexen2 h2                    # software renderer client (h2 / hexen2)
make -C engine/hexen2 localclean            # drop *.o but keep libtimidity.a
make -C engine/hexen2 glh2 USE_X86_ASM=no   # OpenGL client (glh2 / glhexen2)
make -C engine/hexen2/server                # dedicated server (h2ded)
sh engine/hexen2/build_all.sh               # the three above

make -C engine/hexenworld/server                       # hwsv
make -C engine/hexenworld/client hw                    # hwcl (software)
make -C engine/hexenworld/client glhw USE_X86_ASM=no   # glhwcl
sh engine/hexenworld/build.sh                          # all three
```

Other components, each with its own Makefile:
- `h2patch/` — pak0/pak1 → v1.11 patcher (uses libxdelta3 in `libs/xdelta3/`)
- `utils/hcc/` — HexenC compiler (needed to rebuild `gamecode/`)
- `utils/{qbsp,light,vis,bsp2map,bspinfo,genmodel,qfiles,dcc,pak,jsh2color,texutils/*}` — map/asset tools
- `hw_utils/{hwmaster,hwmquery,hwrcon}` — HexenWorld master server + query/rcon clients
- `libs/timidity/` — bundled libtimidity (built via `_timi.mak`)

Cross-compilation flags (make vars or env): `W32BUILD=1`, `W64BUILD=1`,
`DOSBUILD=1`, `OSXBUILD=1`; the `build_cross_*.sh` scripts set up the
toolchain. Also `DEMO=1` (demo build), `DEBUG=1` (debug symbols + extra checks), `USE_X86_ASM=no` (required on non-ia32
and for GL; `MACH_TYPE != x86` sets it automatically). x86 asm files live next
to their C counterparts. The CI matrix in `.github/workflows/build-linux.yml`
is the canonical reference for what gets built and how.

Pre-1.11 Hexen II retail compatibility is gated behind `ENABLE_OLD_RETAIL` in
`h2config.h`; the engine otherwise assumes paks patched to v1.11 by `h2patch`.
Documentation, release notes and platform READMEs live in `docs/`.

## Gamecode (HexenC)

`gamecode/hc/` has the HexenC source of the four progs.dat variants — `h2/`
(Hexen II), `portals/` (Portal of Praevus), `hw/` (HexenWorld), `siege/` (HW
Siege), compiled by `hcc`:

```sh
hcc -os                          # original Hexen II progs
hcc -os -name progs2.src         # mission pack progs2
hcc -os -oi -on                  # mission pack / HW / Siege progs (optimized)
```

**Do not pass `-oi` or `-on` for the base Hexen II progs** — those
optimizations break old save-game compatibility and produce harmless but
noisy runtime warnings. See `gamecode/COMPILE`.
