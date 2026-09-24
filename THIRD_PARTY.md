# Third-party components

Hexenlicht is licensed under the GPL, version 2 or later (see
[docs/COPYING](docs/COPYING)). The components below keep their own licenses.

## Rules

From the [project plan](docs/hexenlicht/PLAN.md#4-licensing-rules):

- Vendored libraries must be GPL-compatible (MIT, BSD, zlib, LGPL, ...).
  Apache-2.0-only code would make the binary GPLv3 and needs an explicit
  decision first.
- Every vendored library keeps its license file next to its source and gets
  a row in the table below.
- **Never** committed to this repository or included in releases:
  NVIDIA DLSS/NGX binaries or static libraries (`nvngx_*.dll`,
  `nvsdk_ngx_*.lib`), Streamline's proprietary plugin binaries, Hexen II game
  data, or textures derived from Raven's textures. Optional DLSS support
  works with files the player downloads from NVIDIA.

## Added by Hexenlicht

| Component | Version | License | Location | Used for |
|---|---|---|---|---|
| *(none yet — added with story 0.6)* | | | | |

## Bundled by upstream (Hammer of Thyrion)

These come unchanged from upstream and are used by the existing uHexen2
builds.

| Component | License | Location |
|---|---|---|
| libTiMidity (MIDI playback) | LGPL-2.1 or Perl Artistic License | `libs/timidity` |
| xdelta3 (used by `h2patch`) | GPL-2.0-or-later | `libs/xdelta3` |
| SDL 1.2, prebuilt for Windows | LGPL-2.1 | `oslibs/windows/SDL` |
| Audio codec libraries, prebuilt (FLAC, libmad, libmikmod, mpg123, Ogg/Vorbis, Tremor, Opus/opusfile, libxmp) | Each project's own license | `oslibs/windows/codecs` |
| DirectDraw/DirectInput/DirectSound headers from the Wine project | LGPL-2.1-or-later | `oslibs/windows/dxsdk` |
| Build rules and libraries for DOS, OS/2, macOS, AROS, MorphOS | Various | `oslibs/*` (not used by Hexenlicht) |
