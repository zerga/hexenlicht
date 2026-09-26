# Blue noise textures

The 64 blue noise textures of 64x64 texels with four 16-bit channels
(`64_64/HDR_RGBA_0.png` … `HDR_RGBA_63.png`) from Christoph Peters' "Free
blue noise textures" (<http://momentsingraphics.de/BlueNoise.html>, the
archive `FreeBlueNoiseTextures.zip` of 2025-05-12, files dated 2016-12),
unchanged. The other resolutions and channel counts of the archive are not
included.

License: CC0 1.0 (public domain dedication), see `LICENSE.txt` (the
archive's) and `COPYING.txt` (the CC0 legal code, also from the archive).

Hexenlicht's path tracer draws its random numbers from them (Quake II RTX's
`get_rng`: each channel of each texture is one layer of a 256-layer texture
array). The build copies the folder next to `hexenlicht.exe` as
`blue_noise\`, where `vk_images.c` loads it.
