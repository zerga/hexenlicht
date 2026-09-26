# DLSS in Hexenlicht (optional)

Hexenlicht can use NVIDIA DLSS instead of its built-in denoiser and
upscaler. NVIDIA's DLSS files can't be shipped with Hexenlicht (see
[why](#why-arent-the-files-included)), so you add them yourself. Without
them Hexenlicht works as usual with its own denoiser and TAAU upscaler.

## What you need

- An NVIDIA GeForce RTX graphics card and a current driver.
- NVIDIA Streamline's release zip, version 2.14.1 or newer, from NVIDIA's
  GitHub page: <https://github.com/NVIDIA-RTX/Streamline/releases>
  (`streamline-sdk-v<version>.zip`).

## Install

1. Download the zip and open it.
2. From its `bin\x64` folder (not `bin\x64\development`), copy these files
   into the folder that holds `hexenlicht.exe`:
   - `sl.interposer.dll`
   - `sl.common.dll`
   - `sl.dlss.dll`
   - `sl.dlss_d.dll`
   - `nvngx_dlss.dll`
   - `nvngx_dlssd.dll`
   - and, recommended, `NvLowLatencyVk.dll` (without it Streamline logs a
     harmless error).
3. Start Hexenlicht. The console shows
   `DLSS: Streamline loaded; SR supported, RR supported`.

Hexenlicht loads `sl.interposer.dll` only if it carries NVIDIA's digital
signature, and Streamline's online updates are turned off: the DLSS files
stay the ones you copied.

## Turn it on

Open the console and type:

| Command | What it does |
|---|---|
| `r_upscaler 3` | DLSS Super Resolution: upscales Hexenlicht's denoised image |
| `r_upscaler 4` | DLSS Ray Reconstruction: denoises and upscales in one step. The best image, but it costs the most GPU time |
| `r_upscaler 1` | back to Hexenlicht's own upscaler (TAAU), the default |

`r_scale` sets the resolution the game renders at, in percent of the
window, and so the DLSS mode:

| `r_scale` | DLSS mode |
|---|---|
| 100 | DLAA (no upscaling, only anti-aliasing) |
| 67 | Quality |
| 58 | Balanced |
| 50 | Performance |
| 33 | Ultra Performance |

Other values use the nearest size DLSS accepts: for example 45 renders at
50 %, and 25 at 33 % (at some window sizes at 50 %). Both settings are
saved with your configuration. The options menu will offer them later.

For testing, `r_dlss_preset` picks DLSS's model: `0` (the default) lets
DLSS choose, or a preset letter such as `F` pins one.

## If it doesn't work

Type `vk_dlss` in the console. It shows whether Hexenlicht found
`sl.interposer.dll`, whether its signature is valid, whether your graphics
card and driver support DLSS, which versions were loaded, and Streamline's
warnings and errors. When DLSS can't run, Hexenlicht uses TAAU instead, and
`vk_upscale` says why.

- *"no sl.interposer.dll next to the exe"*: the files are not in the
  folder with `hexenlicht.exe`.
- *"does not carry a valid NVIDIA signature"*: the DLL is damaged or not
  NVIDIA's. Copy it again from the zip.
- *"not supported by this GPU or driver"*: DLSS needs an NVIDIA RTX card;
  update the driver (`vk_dlss` shows the version DLSS needs).
- *"the Vulkan instance or device failed through Streamline"*: Hexenlicht
  started without DLSS. The files most likely come from different
  Streamline versions: copy all of them from one zip.

## Why aren't the files included?

Hexenlicht is free software under the GNU GPL. NVIDIA's DLSS files come
under NVIDIA's own license (included in the zip), which does not allow
distributing them as part of a GPL program, so Hexenlicht contains no
NVIDIA files and loads them only when you add them. Streamline itself, the
layer between Hexenlicht and DLSS, is open source (MIT); Hexenlicht
includes only its header files. The project's reasoning is in the
[plan](PLAN.md#5-dlss-strategy).
