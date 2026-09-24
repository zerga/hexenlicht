# Hexenlicht

[![Windows build](https://github.com/zerga/hexenlicht/actions/workflows/build-windows.yml/badge.svg)](https://github.com/zerga/hexenlicht/actions/workflows/build-windows.yml)

**A path-traced fork of Hexen II: Hammer of Thyrion.**

Hexenlicht ("witch-light") brings real-time path tracing to Raven Software's
Hexen II. It is built on [Hammer of Thyrion](https://uhexen2.sourceforge.net/)
(uHexen2), the long-running Hexen II source port, and adds a new Vulkan
renderer in which the maps are lit live by their original light sources,
with shadows, bounce light, reflections and PBR materials.

> **Status: early development — not playable yet.**
> The repository currently contains the unmodified Hammer of Thyrion engine
> plus the project plan. Follow progress on the
> [project board](https://github.com/users/zerga/projects/1).

## Goals

- Play the full Hexen II campaign and the Portal of Praevus mission pack
  with a real-time path-traced renderer (Vulkan, Windows x64).
- Keep the original mood: lighting comes from the light entities the level
  designers placed, not from new hand-made lighting.
- Support PBR texture sets (albedo, normal, roughness, metallic, emissive)
  so reworked textures can shine.
- Optional NVIDIA DLSS support (players download NVIDIA's files themselves;
  see the plan for why).

The existing software and OpenGL renderers of Hammer of Thyrion stay intact.
HexenWorld is not part of Hexenlicht's scope.

## Project documents

- [Project plan](docs/hexenlicht/PLAN.md) — decisions, architecture,
  licensing rules and the epic/story breakdown.
- [Issues](https://github.com/zerga/hexenlicht/issues) and the
  [project board](https://github.com/users/zerga/projects/1) — live status.
- [Third-party components](THIRD_PARTY.md).

## Building

Hexenlicht builds on Windows x64 with MSVC and CMake.

Requirements:
- Visual Studio 2022 or newer, or the Visual Studio Build Tools, with the
  *Desktop development with C++* workload and a Windows SDK.
- CMake 3.25+ and Ninja (both are bundled with CLion).
- The [LunarG Vulkan SDK](https://vulkan.lunarg.com/sdk/home#windows),
  1.3 or newer (1.4.357.0 is what CI uses).

In **CLion**, open the repository folder, set the *Visual Studio* toolchain
as the default, and pick the `windows-debug` or `windows-release` preset.
From a *Developer Command Prompt* (x64):

```
cmake --preset windows-debug
cmake --build --preset windows-debug
```

The executable (`glh2.exe`, Hexen II with the upstream OpenGL renderer for
now) and the music codec DLLs end up in `build/<preset>/bin/`.

GitHub Actions builds both presets for every pull request and every push to
`main`; the binaries can be downloaded from the run's *Artifacts* section for
30 days.

The upstream Makefiles still work for all other platforms; see
[docs/COMPILE](docs/COMPILE).

## Game data

Hexenlicht does not include any game data. You need a copy of the original
Hexen II (and optionally the Portal of Praevus mission pack), with its pak
files patched to version 1.11; the `h2patch` tool in this repository does
that. The [development setup guide](docs/hexenlicht/SETUP.md) walks through
the data folder, patching and running from CLion.

## License

Hexenlicht is free software, licensed under the
[GNU General Public License, version 2 or (at your option) any later
version](docs/COPYING), like the Hammer of Thyrion code it is based on.
Third-party components keep their own licenses; see
[THIRD_PARTY.md](THIRD_PARTY.md).

The Hexen II game data is **not** covered by this license and is not
distributed here.

## Credits

- **Raven Software** and **id Software** — the original Hexen II and Quake
  engines and game code, released under the GPL.
- **O. Sezer (sezero)** and all **Hammer of Thyrion contributors** — the
  source port this fork is built on (see [docs/AUTHORS](docs/AUTHORS)).
  Upstream: <https://github.com/sezero/uhexen2>.
- The **Quake II RTX** authors (Christoph Schied, NVIDIA) — Hexenlicht's
  path tracer will reuse GPL-licensed code from Quake II RTX.

Hexenlicht is an unofficial project. It is not affiliated with or endorsed
by Raven Software, id Software, Activision, NVIDIA or the Hammer of Thyrion
project. Hexen and Hexen II are trademarks of their respective owners.
