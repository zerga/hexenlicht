# Hexenlicht — authoring materials

How to give Hexen II's textures physically based materials for
Hexenlicht, from the original textures to a pack players can install.
[MATERIALS.md](MATERIALS.md) is the reference (every file, key and rule);
this is the workflow and what the test pack (story 5.6,
`tools/hexenlicht/test_pack.ps1`) showed about making materials look
right. Numbers from the test pack are in DECISIONS M28–M33.

## 1. Start from the export

Run the game with both games in the search path and export every texture:

```
hexenlicht.exe -portals
] r_exporttextures
```

`portals\export\textures\` then holds about 2,100 PNGs, each named as the
engine looks it up, exactly as the engine shows it, and `textures.csv` a
row per file: the name, the size, the kind (world, liquid, sky, skin,
sprite, picture), whether it has alpha, how many variants its name has,
and the maps (or model or sprite file) it is used in. Unchanged, a copy in
`data1\textures\` changes nothing in the game: it is the safe starting
point for an edit.

- Work on copies in `data1\textures\` (loose files; the game reads them
  before its paks), a few at a time.
- **Names:** `<name>.png` applies to every texture of that name,
  `<name>~<crc>.png` only to the texture with those pixels. Raven reused
  names per hub (100 of the 870 names have two or three sets of pixels);
  the export writes those qualified (`rtex343~ad81.png`) and the rest plain.
  Keep the export's names; a plain name on a texture that has variants
  changes every map's.
- The manifest's `used in` tells where a texture appears: a texture used
  in 47 maps changes all of them.

## 2. Find a surface's texture

Look at the surface and type `vk_materials here`:

```
here: world opaque, 96 units away: rtex022 (material 49)
  files: textures/rtex022~679e (this texture's pixels only) or textures/rtex022 (every texture of the name)
    71 rtex022~679e: albedo rtex022.png (256x704 PNG RGBA8); normal rtex022_n.png (...); roughness/metallic rtex022_orm.png (...);
  albedo: mean 0.179 0.153 0.120 (luminance 0.156), the original's 0.023 0.014 0.008 (luminance 0.015): 10.14 times (linear light, r_srgb 0)
```

It names what the center of the view hits (the world, a brush entity such
as a door or a breakable window, `*20`, or a model and its skin), the file
names that apply to it, the files found for it, and its albedo's brightness
against the original's (section 5). A glass pane, water or a model's
transparent part counts as what is hit. `noclip` and `notarget` help to
get close.

## 3. Make the maps

Every file is optional (MATERIALS.md "Files"). What each holds:

| File | What | Values |
|---|---|---|
| `<name>.png` | albedo: the surface's color without light, shadow or shine | dielectrics' linear albedo about 0.02 (charcoal) to 0.8 (snow); Hexen II's own 0.01–0.05; for metals their reflectance (below). Alpha only where a skin or sprite has holes; without alpha the original's holes stay |
| `<name>_n.png` | normal map, tangent space | OpenGL's convention: green is up (towards the image's top), as glTF and Blender; a DirectX-convention map (green down) shows its bumps inverted: flip its green |
| `<name>_orm.png` | glTF's packed map: R occlusion (unused), G roughness, B metallic | roughness 0 mirror-smooth to 1 matte: polished metal 0.2–0.4, worn stone 0.7–0.95, water 0.02–0.1; metallic 0 or 1 (in between only at edges, such as rust on metal) |
| `<name>_r.png`, `<name>_m.png` | roughness and metallic apart, instead of `_orm` | the same size; PNG or TGA only |
| `<name>_e.png` | what glows, its color | black where nothing glows; the `emissive` key scales it |
| `<name>.mat` | settings: `kind`, `roughness`, `metallic`, `bump`, `specular`, `emissive` | MATERIALS.md "Settings" |

- **Metals:** the albedo is the metal's reflectance, in linear light:
  iron 0.56 0.57 0.58, silver 0.97 0.96 0.91, aluminium 0.91 0.92 0.92,
  copper 0.95 0.64 0.54, gold 1.0 0.71 0.29 (as 8-bit colors their reds
  are 196, 251, 244, 249 and 255); with metallic 1. Old metal is darker through what covers it: rust and dirt
  as dielectric patches (metallic 0, rough), not a darker reflectance.
- **Sizes:** any size, the original's aspect ratio (texture coordinates
  follow the original: another aspect ratio stretches). Four times the
  original's size is a good start (a 64x64 original: 256x256); for BC
  compression (section 7) sizes in multiples of 4; PNG and TGA at most
  8192x8192.
- **Colors are 8-bit colors** as the originals: the engine reads albedo and
  emissive as the 2.2 power of linear light (`r_srgb 1`: the sRGB curve);
  a tool writing sRGB matches it from a value of about 128 up and is a
  little darker below. Normal, roughness and metallic maps are data, read
  as they are.
- **What tools write:** rename to the names above. A glTF
  (metallic-roughness) export has them all: base color the albedo, the
  normal map in OpenGL's convention, the occlusion-roughness-metallic map
  `_orm`, emissive `_e`. Material libraries name them per map, for
  example ambientCG's `_Color`, `_NormalGL` (`_NormalDX` needs its green
  flipped), `_Roughness`, `_Metalness`, `_Emission`, and Poly Haven's
  `_diff`, `_nor_gl`, `_rough`, `_metal` and `_arm` (occlusion, roughness,
  metallic: our `_orm`). AI tools that make maps from the originals
  (PLAN §6: PBRify in chaiNNer, NVIDIA's RTX Remix texture tools,
  Substance Sampler, Materialize): check their normal convention, and
  turn a glossiness map into roughness (1 − gloss); height, displacement,
  specular-workflow and cavity maps aren't used.
- **Glowing parts** light what is around them only through bounce rays
  (the lights are the map's light entities and lava), so a strong glow
  over a large area is noisy; `emissive` 0.1–0.2 makes details glow
  (a color of 1 emits `r_emissive_scale`, 32 times GL's fullbright).

## 4. See it in the game

- **Reload:** `bind F5 r_reloadmaterials`: new and changed files are
  read again and applied, without a map reload. A world texture's `kind`
  (chrome, glass) applies at the next map load (`restart`).
- **Compare:** `r_materials 0` shows the originals, `1` the files.
- **Check:** `vk_materials here` (above), `vk_materials problems` (files
  refused and why, `.mat` lines left out; a map load prints how many),
  `vk_materials list` (every texture with files), `vk_imagefile <file>`
  (one file as the engine reads it, on the screen).
- **Debug views** (`r_debugview`, 0 is the lit image): 1 base color, 2
  shading normals, 8 geometric normals, 10 roughness, metallic and
  specular as red, green and blue, 13 emission, 3 the material kinds
  (chrome red, glass pale cyan). A normal map in the other convention
  shows its bumps lit from the wrong side, top and bottom swapped.
- **Both light shapes:** the default "Original" (`r_maplight_shape 2`:
  a light's first arrival as GL's lightmaps) and "Physically based"
  (`r_maplight_shape 0`). They agree within 10 % on rough stone; metal
  comes out 1.3x as bright in the physically based one (DECISIONS M33).

## 5. How bright

The originals are very dark in linear light: Raven painted shading and
dirt into them, and GL's lightmaps made up for it. Demo1's cobbles and
walls are 0.012 and 0.015, where real stone is 0.1–0.3. Hexenlicht's
lights are fitted to GL's lightmaps, which are light, not color, so an
albedo's brightness shows in the image as it is (physically right):

| demo1 and the cathedral, every world texture replaced by stone | lit image, against the originals | bounce light |
|---|---|---|
| the originals | 1x | 2–3 % of the image |
| stone at real albedo (0.18) | 5.4x and 3.5x | 7–12 % |
| stone scaled to each original's mean | 1.65x and 1.27x | 1–2 % |

- **For Hexen II's look,** and for a pack that replaces only some
  textures, **keep an albedo's mean near the original's**: `vk_materials
  here` prints both and their ratio; aim for about 1–1.5 times. Put the
  realism into the detail (the normal map, the roughness), not the
  brightness. That is darker than real stone (charcoal-dark), a choice
  for the mood.
- Even matched, a surface with an authored roughness shines a little:
  every dielectric reflects 4 % head-on, which on albedo that dark is as
  bright as its diffuse light (the second row's rise; GL had no specular
  at all). Physically right; `specular` below 1 lowers it.
- **A physically plausible pack** (albedo at real values) is a choice for a
  complete pack, with the exposure lowered (`tm_exposure_bias`, about −2
  EV at the views measured): the models, effects and textures it doesn't
  replace then look darker than before.
- **Metals** at their real reflectance read glaring against Hexen II's
  dark stone; weather them (rust, dirt), as above.

## 6. Special materials

MATERIALS.md "Special materials" has the rules; what matters for authors:

- **Chrome** (`kind chrome`): a mirror below roughness 0.02, tinted by the
  albedo: give it a metal's light albedo.
- **Glass** (`kind glass`, world textures): the albedo is the tint of what
  is seen through it, and glass has no opaque parts: lead cames and frames
  are a dark tint (0.02). Since 6.14 the albedo also tints the light
  through the pane (blurred: the texture's mip 2), so the cames cast a
  soft pattern of shade on the floor. Hexen II's windows are breakable brush entities
  with the texture on both sides of a thin pane (the stained glass
  `rtex018`, the panes `rtex083`, the clear `rtex199`, the mission pack's
  `ttex210`); the originals paint a scene behind the lead, which would
  tint the view: draw a new albedo. Windows the game draws translucent
  (village1's clear glass) are glass alone with `kind glass` (6.4): the
  glass replaces the game's blend. Since 6.20 the clear `rtex199` is
  reflective glass without any file (0.33 head-on, clear, `r_windows`):
  an albedo alone for it doesn't show; give it a `kind` to use one. A
  world texture's kind applies to every face with that name: check the
  manifest's `used in` (castle5 has `rtex199` on world faces too).
- **Liquids:** every map applies, warped as the original. Since 6.5 a
  pool's surface reflects and is seen through as water, whatever its
  `roughness`; for `*rtex078` and `*lowlight` your albedo is a layer at
  a third over what is seen through it, lit as that; the other liquids
  are opaque under the reflection (their own specular left out). An
  `_n` replaces the default waves. Vertical liquid faces (egypt's rune walls) stay opaque
  surfaces. Since 6.16 your albedo's brightness pattern is also the
  caustic in the light through the surface (`r_water_caustics`): bright
  lines send more light onto the floor, dark areas less, so a liquid with
  little contrast casts little. Ship a DDS albedo with its full mip chain
  (`pack_dds.ps1` does): without it there is no pattern.
- **Lava:** its albedo or `_e` emits and lights the room with its average
  color, times `emissive`.
- **The sky:** one 2:1 image, the front layer left, the back right, the
  front's holes as alpha (the export writes it so); only the albedo.
- **Animated textures:** each frame (`+0…`, `+1…`) its own files; a kind
  only on the first frame.
- **Skins and players:** `textures/models/<model>.mdl_<skin>`; a player
  whose colors leave the skin as it is (colors 0 0) shows its albedo,
  other players the original recolored; `kind chrome` works on skins, not
  glass.
- **Sprites:** only the albedo (with its alpha, or the original's shape).

## 7. Ship it

- **Convert** the PNGs to the shipping formats with their mips:

  ```
  pwsh tools\hexenlicht\pack_dds.ps1 -Texconv <texconv.exe> -Source <folder>\textures -Out <pack>\textures
  ```

  (texconv is Microsoft's DirectXTex converter, MIT, not included):
  albedo, `_e` and `_orm` BC7, normal maps BC5, `_r`, `_m` and the sky as
  they are (the engine repacks and splits those), `.mat` files copied; the
  8-bit values kept (a PNG's gamma chunk ignored). Don't ship a PNG and a
  DDS of one name: the PNG wins.
- **Why DDS:** demo1 with all 96 world textures in three maps each loads
  in 0.09 s as DDS and 0.53 s as PNG at 4x the originals, 0.17 s and 2.2 s
  at 8x (video memory 41 and 164 MB as DDS, 164 and 656 MB as PNG);
  story 5.7 will decode PNGs on threads, for authoring.
- **Where:** in `data1` (it applies to both games; `portals` only for the
  mission pack's own changes), as loose files under `data1\textures\` or in
  a pak (`data1\pak2.pak`; the game reads `pak0.pak` to `pak9.pak` of each
  game folder), the names under `textures/` in lowercase (paks compare
  names exactly).
- **Distribution:** packs are distributed separately from Hexenlicht
  (PLAN §4): its repository and releases carry no textures derived from
  Raven's, and a pack made from the export is derived from the game's
  textures.

## The test pack

`tools/hexenlicht/test_pack.ps1` makes one of each kind with no game data
(TESTING.md "Test pack"): ashlar, cobbles and bricks on demo1's walls and
floors, riveted iron plates, rippled water on the pool, glowing runes on
the statue's pedestal, stained and clear leaded glass on the breakable
windows of demo1, the cathedral and village1. It is a worked example of
this guide: each material's maps come from one height field so that they
agree, the values are measured real ones, `-Albedo matched` makes the
section 5 variant, and `-Dds` ships it through `pack_dds.ps1`.
