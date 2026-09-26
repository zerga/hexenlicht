# AMD FidelityFX Super Resolution 1.0

`ffx_a.h` and `ffx_fsr1.h` from AMD's FidelityFX Super Resolution 1.0
(<https://github.com/GPUOpen-Effects/FidelityFX-FSR>, tag `v1.0.2`, commit
`a21ffb8f6`, folder `ffx-fsr/`), unchanged. The rest of the repository
(samples, documentation) is not included. Quake II RTX ships an earlier
`ffx_fsr1.h` (before v1.0.2's fix of RCAS's limits, which now include the
center pixel); `ffx_a.h` is the same.

License: MIT, see `LICENSE.txt` (the repository's `license.txt`).

Hexenlicht's FSR passes (`engine/hexenlicht/vk_upscale.c`, the shaders
`fsr_easu_fp32.comp` and `fsr_rcas_fp32.comp`, Quake II RTX's) include both
headers: the shaders as GLSL (`A_GPU`, `A_GLSL`), `vk_upscale.c` as C
(`A_CPU`) for the constants `FsrEasuCon` and `FsrRcasCon` put into the
global UBO.
