# Hexenlicht test tools

PowerShell scripts (and a patch) for testing Hexenlicht, and `texpack/`, a Python tool that runs
on ComfyUI's bundled interpreter (its README); how to use them is in
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
| `perf_baseline.ps1` | measures the GPU timers (`vk_profiler`, `vk_benchmark 1`) at demo1's and the cathedral's starts for each window size and prints markdown tables; `-Presets` the Renderer Settings page's quality presets (6.10) |
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
| `texpack/` | AI-upscaled texture packs from a manifest of material descriptions (5.8): `texpack.py` draft, sheet, merge, run, verify, calibrate; Python on ComfyUI's bundled one; [its README](texpack/README.md) |
| `tga_luminance.ps1` | each TGA screenshot's mean luminance in linear light (5.6's albedo measurements), or a rectangle's (`-Rect`; 6.18: the view centre over consecutive frames) |
| `export_check.ps1` | checks a texture export (`r_exporttextures`) against the paks without the engine: its own reading, conversion and names of every texture against each manifest row and each PNG, decoded by texconv (5.4) |
| `jsh2color_colors.patch`, `jsh2color_colors.ps1`, `light_colors_compare.ps1` | `utils/jsh2color` printing each light's color; runs it on the maps with its batch files' options; compares with `vk_lights colors` |
| `monsters_near_start.ps1` | monsters near each map's `info_player_start` |
| `patrols.ps1` | monsters with a patrol route near the start |
| `bsp_models.ps1` | bounds of brush submodels of a map and the player starts |
| `mdl_stats.ps1`, `mdl_flags.ps1`, `mdl_skingroups.ps1` | alias model statistics, effect flags (`-Trails`: the trail each model leaves, 6.1), skin groups |
| `effects_run.ps1` | every class weapon (normal and with the tome of power) and the artifacts fired in `glh2` and Hexenlicht from the same saves, paused shots named by effect, what Hexenlicht left out and the validation line (6.1, E6's checklist) |
| `translucency_run.ps1` | the translucency views from saves in `glh2` and Hexenlicht (6.4): village1's translucent windows (a window behind a window, with `vk_rayprobe`), glass shards, smoke rings, the hand effect, the tomed sunstaff's sheath |
| `water_run.ps1` | the water views from saves in `glh2` and Hexenlicht (6.5): above and below translucent and opaque water, across pools at grazing angles, turning above and below, lava, the crossbow's bubbles ([TESTING.md](../../docs/hexenlicht/TESTING.md) "Water (6.5)") |
| `medium_run.ps1` | a swim through demo2's moat (6.17), or with `-Lift`/`-Pitch` the same path above the water looking down (6.18): `cl.light_level` and the medium light line (6.17's eased light, 6.18's light grid at the eye) every few frames into a CSV, or frames of the path shot in a build ([TESTING.md](../../docs/hexenlicht/TESTING.md) "Water (6.5)") |
| `caustics_run.ps1` | the light through water, glass and translucent things from 6.4's and 6.5's saves (6.14): each view paused and shot with `pt_caustics` 0 and 1, test lights behind village1's bay pane and above three pools, the sun through the bay, the sunstaff's sheath, the smoke rings ([TESTING.md](../../docs/hexenlicht/TESTING.md) "Light through (6.14)") |
| `viewer_run.ps1` | the player's own model (6.11): each view paused and shot with `r_viewer_model` 0 and 1 (shadows from the sun and map lights, four classes, the chase camera, the torch, the Assassin's cloak, pools from above and below), `vk_models check` and `cl.light_level` with both; `-Mirror` makes the cathedral's and demo1's start floors mirrors (material files, removed after) and looks down at the model; `-Explore` looks for places, `-Cost` profiles them ([TESTING.md](../../docs/hexenlicht/TESTING.md) "Own model (6.11)") |
| `windows_run.ps1` | the reflective windows (6.20): village1's and village2's clear panes from inside, the street, at angles and through both bay panes, each view shot with `r_windows` 0, 1 and 2 (live); `-Bin` a build before it with `-Windows 0`, `-Cost` profiles them ([TESTING.md](../../docs/hexenlicht/TESTING.md) "Windows (6.20)") |
| `ice_run.ps1` | frozen monsters as ice (6.15): `vk_freeze` on a monster (created where none stands), each view paused and shot with `r_ice` 0 and 1: demo1's archer from the front and the side, one behind village1's bay window, one in demo1's pool, village2's crystal golem; `-Saves` for `glh2`'s shots, `-Cost` profiles them ([TESTING.md](../../docs/hexenlicht/TESTING.md) "Ice (6.15)") |
| `blend_run.ps1` | GL's view blends in `glh2` and Hexenlicht from demo1's start (6.6): each blend's numbers through `v_cshift` on a paused frame with `gl_polyblend` 0 and 1, checked against GL's blend function; the flashes, two power-ups and an archer's hits in play; the bonus flash under water ([TESTING.md](../../docs/hexenlicht/TESTING.md) "View blends (6.6)") |
| `praevus_run.ps1` | the flames of Portal of Praevus's light entities (6.8): tibet1's burners, tibet6's candles, tibet7's `light_newfire`, tibet8's palace torches, keep2's lantern, from saves (`-Saves`) in `glh2` and Hexenlicht or another build (`-Bin`), the pitch held through the load ([TESTING.md](../../docs/hexenlicht/TESTING.md) "Praevus (6.8)") |
| `menu_run.ps1` | the Renderer Settings page (6.10): steps through every row with keys posted to the game's window, a screenshot per step, and checks which settings `hexenlicht.cfg` saved ([TESTING.md](../../docs/hexenlicht/TESTING.md) "Settings menu (6.10)") |
| `spr_stats.ps1` | sprites: orientation type, frames, sizes |
