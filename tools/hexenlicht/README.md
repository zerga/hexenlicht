# Hexenlicht test tools

PowerShell scripts (and a patch) for testing Hexenlicht; how to use them is in
[docs/hexenlicht/TESTING.md](../../docs/hexenlicht/TESTING.md). Run them
from a PowerShell 7 (`pwsh`) prompt: through `pwsh -File`/`powershell -File`
(e.g. from Git Bash) array parameters (`-Files`, `-Paks`) arrive as one
string, and Windows PowerShell's execution policy may block `-File`. The
scripts that start the game find the build in this repository (`hl_run.ps1
-Bin` for another one) and the game data in `-Data`, `$env:HEXENLICHT_DATA`,
or `Hexenlicht-data` next to the repository.

| Script | What it does |
|---|---|
| `hl_run.ps1` | runs `hexenlicht.exe` or `glh2.exe` with a test script from the data folder, with a timeout |
| `resize_test.ps1` | resizes, maximizes and restores the window from outside while a test script runs |
| `perf_baseline.ps1` | measures the GPU timers (`vk_profiler`, `vk_benchmark 1`) at demo1's and the cathedral's starts for each window size and prints markdown tables |
| `calib_shots.ps1`, `bookmarks.txt` | matched screenshots of `glh2` and Hexenlicht at the calibration bookmarks: both load the same savegame (4.9) |
| `calib_compare.ps1` | compares them: Hexenlicht's direct light against GL's lightmaps, the lit image against GL's, and pictures with ratio maps; per map (`-ByMap`) and the views within a range |
| `bookmarks_blackmarsh.txt` | 4.11a's views of the Blackmarsh hub (entrances and main areas) for `calib_shots.ps1 -Bookmarks` |
| `pick_views.ps1` | picks a hub's views in its maps' main areas (deathmatch spots, away from the other views and from walls) |
| `calib_grid.ps1` | a labelled grid of `calib_shots.ps1`'s shots, a row per view and a column per shot kind, for review |
| `tga2png.ps1` | converts the engines' TGA screenshots to PNG |
| `crop_strip.ps1` | crops the same rectangle out of several PNGs, side by side |
| `tga_diff.ps1` | pixel-diffs two folders of screenshots (skip the HUD rows, mask run-to-run noise, write diff images) |
| `tga_mean.ps1` | averages screenshots of a paused frame in linear light and compares two sets (means, 60-pixel blocks, noise) |
| `tga_checkerboard.ps1` | averages screenshots of a paused frame and scores a fine checkerboard (unresolved checkerboard fields) per block, with a heat map |
| `ubo_layout_check.ps1` | checks every global UBO member's C offset against glslang's reflection |
| `pak_entities.ps1` | lists entities of a classname pattern per map in the paks |
| `pak_bsp.ps1` | extracts the paks' maps (`.bsp`) into a folder |
| `tex_names.ps1` | the maps' world textures under the material spec's names: the names whose pixels differ between maps, with their `~<crc>` qualifiers (5.1, [MATERIALS.md](../../docs/hexenlicht/MATERIALS.md)) |
| `imagefile_set.ps1`, `imagefile_compare.ps1` | the image file loader's test set (generated images in every PNG, TGA, DDS and KTX2 kind it reads, files it must refuse, test scripts; made with texconv) and the comparison of its screenshots with the files (5.2) |
| `material_set.ps1`, `material_check.ps1` | the material system's test set (generated maps and `.mat` files at demo1's start, skins, a sprite, meso9's lava, castle4's torch; test scripts; the run with a hot reload's file swap; `-Remove`) and the checks of its screenshots (5.3) |
| `special_set.ps1`, `special_check.ps1` | the special materials' test set (chrome and glass, a water normal map, a translucent albedo, animated frames, lava's light, skies with and without alpha, refusals; the run with two file swaps; `-Remove`) and the checks of its screenshots and log (5.5) |
| `test_pack.ps1` | the test material pack (5.6): generated stone, iron, water, glowing runes and leaded glass on demo1's, the cathedral's and village1's textures (`-Albedo matched`, `-Dds`), its scripted views; `-LoadSet` a stone material on every world texture of maps, for load times and the albedo question; `-Run`, `-Remove` ([AUTHORING.md](../../docs/hexenlicht/AUTHORING.md)) |
| `pack_dds.ps1` | converts material files as authored (PNG, TGA) into a pack's shipping formats with texconv: BC7, BC5 for normal maps, `_r`/`_m`/the sky and `.mat` copied (5.6) |
| `tga_luminance.ps1` | each TGA screenshot's mean luminance in linear light (5.6's albedo measurements) |
| `export_check.ps1` | checks a texture export (`r_exporttextures`) against the paks without the engine: its own reading, conversion and names of every texture against each manifest row and each PNG, decoded by texconv (5.4) |
| `jsh2color_colors.patch`, `jsh2color_colors.ps1`, `light_colors_compare.ps1` | `utils/jsh2color` printing each light's color; runs it on the maps with its batch files' options; compares with `vk_lights colors` |
| `monsters_near_start.ps1` | monsters near each map's `info_player_start` |
| `patrols.ps1` | monsters with a patrol route near the start |
| `bsp_models.ps1` | bounds of brush submodels of a map and the player starts |
| `mdl_stats.ps1`, `mdl_flags.ps1`, `mdl_skingroups.ps1` | alias model statistics, effect flags, skin groups |
| `spr_stats.ps1` | sprites: orientation type, frames, sizes |
