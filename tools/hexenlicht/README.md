# Hexenlicht test tools

PowerShell scripts for testing Hexenlicht; how to use them is in
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
| `tga2png.ps1` | converts the engines' TGA screenshots to PNG |
| `crop_strip.ps1` | crops the same rectangle out of several PNGs, side by side |
| `tga_diff.ps1` | pixel-diffs two folders of screenshots (skip the HUD rows, mask run-to-run noise, write diff images) |
| `tga_mean.ps1` | averages screenshots of a paused frame in linear light and compares two sets (means, 60-pixel blocks, noise) |
| `ubo_layout_check.ps1` | checks every global UBO member's C offset against glslang's reflection |
| `pak_entities.ps1` | lists entities of a classname pattern per map in the paks |
| `monsters_near_start.ps1` | monsters near each map's `info_player_start` |
| `patrols.ps1` | monsters with a patrol route near the start |
| `bsp_models.ps1` | bounds of brush submodels of a map and the player starts |
| `mdl_stats.ps1`, `mdl_flags.ps1`, `mdl_skingroups.ps1` | alias model statistics, effect flags, skin groups |
| `spr_stats.ps1` | sprites: orientation type, frames, sizes |
