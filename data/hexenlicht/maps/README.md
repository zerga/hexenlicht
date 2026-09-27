# Per-map override files

`<map>.hlmap` files here are Hexenlicht's per-map settings and light fixes
for the game's own maps (story 4.7): the build copies them next to
`hexenlicht.exe`, into `maps\`. A file with the same name in the game's
folders (`data1\maps\`, `portals\maps\`, loose or in a pak) is used instead.
The format, the settings and the light lines are described in
[RENDERER.md](../../../docs/hexenlicht/RENDERER.md#map-file-vk_mapfilec).

To make or change one in the game (story 4.8, see
[RENDERER.md](../../../docs/hexenlicht/RENDERER.md#light-editor-vk_lighteditc)):
edit the lights with `r_editlights 1` and `vk_editlight`, set the sky, sun
and exposure cvars in the console, then `vk_editlight save`, which writes
the game folder's `maps\<map>.hlmap`. Move that file here (don't copy it:
the game folder's file would still be used before the shipped one).
