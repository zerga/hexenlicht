# Hexenlicht — materials

How a Hexen II texture gets a physically based material: image files looked
up by the texture's name, one image per map (albedo, normal, ...), an
optional settings file. Frozen in story 5.1 ([DECISIONS.md](DECISIONS.md)
M1–M7); the rest of E5 implements it: the loaders (5.2), the material
system and hot reload (5.3), the export of the original textures under
these names (5.4), the special materials (5.5), a test pack and the
authoring guide (5.6). Until 5.3 the engine reads none of these files.

## Files

Every file is optional; a missing albedo is the original texture (a normal
map alone is a valid material).

| File | Map | Channels | Kind |
|---|---|---|---|
| `<name>.png` | albedo | RGB; A = coverage where the original has it: a masked skin's holes below 0.5, a sprite blended by it; without A the original's stays | color |
| `<name>_n.png` | normal | tangent space, XYZ as RGB × 0.5 + 0.5, OpenGL's convention (+Y up: green points to the image's top; glTF's, Blender's) | data |
| `<name>_r.png` | roughness | grey (R read), perceptual roughness as glTF's (the shader squares it) | data |
| `<name>_m.png` | metallic | grey (R read), 0 dielectric, 1 metal | data |
| `<name>_orm.png` | occlusion, roughness, metallic | R occlusion (read, unused: the path tracer makes its own), G roughness, B metallic (glTF's layout); instead of `_r` and `_m` | data |
| `<name>_e.png` | emissive | RGB | color |
| `<name>.mat` | settings | [below](#settings-mat) | — |

A normal map made with DirectX's convention (green down) shows its bumps
inverted: flip its green channel. `_r` and `_m` must have the same size
(they become one texture); otherwise both are reported and skipped.

## Names

- **Where:** `textures/` in the game's filesystem, as loose files or in a
  `.pak`, in `data1` or the mission pack's `portals` (uHexen2's search
  path). Nothing is looked up next to the exe: texture packs are
  distributed separately from the engine (PLAN §4).
- **Lowercase:** the lookup lowercases the name, and paks compare names
  exactly, so the files are named in lowercase (all of Hexen II's texture
  names are).
- **World textures:** the BSP's name, `*` (no Windows file name takes it)
  as `#` (DarkPlaces' and QuakeSpasm's convention): `*lava1` →
  `textures/#lava1.png`. Animated textures keep their prefixes (`+0`–`+9`,
  `+a`–`+j`); each frame has its own files. The sky (`sky000`, `sky001`)
  is named so too; how its two layers are replaced is 5.5's.
- **Model skins:** `textures/<model path>_<skin>`, and
  `_<skin>_<frame>` for a skin group's frames (`gl_model.c`'s names; none
  of the games' models has a group): `textures/models/paladin.mdl_0.png`;
  the stone and ice skins (skins 100 and 101, `gfx/skin100.lmp` and
  `gfx/skin101.lmp`) `textures/gfx/skin100.png` and
  `textures/gfx/skin101.png`.
- **Sprite frames:** `textures/<sprite path>_<frame>`
  (`textures/models/s_light.spr_0.png`), a group's frames
  `_<frame × 100 + i>` (none in the games).
- **Variants:** 100 of the 870 texture names in both games' maps have
  more than one set of pixels, 90 of them two and 10 three (Raven reused
  its `rtex` numbers per hub: `rtex343` is 128x64 in Egypt and a different
  128x80 texture in Blackmarsh and elsewhere; `sky000` has three;
  `tools/hexenlicht/tex_names.ps1` lists them). Two skins differ between
  the games too (skin 0 of `models/ball.mdl` and of
  `models/puzzle/scepter.mdl` in `data1` and `portals`). A file named
  `<name>~<crc>` applies only to the variant with that CRC; `<name>`
  applies to every variant. `<crc>` is four lowercase hex digits,
  `CRC_Block` (CRC-16, polynomial 0x1021, 0xffff first, not reflected) of
  the 8-bit pixels the engine loads the texture from, which the texture
  cache keys on and `vk_textures list` prints: mip 0 of a world texture, a
  skin as `gl_model.c` passes it (a model's first skin after its flood
  fill). The sky's is the same CRC of its 256x128 pixels, which the cache
  doesn't show (it holds the sky's two layers, `upsky` and `lowsky`;
  `tex_names.ps1` prints it). So `textures/rtex343~ad81.png` is Egypt's,
  `textures/rtex343~d686.png` the others'. No two variants of a name share
  a CRC (`tex_names.ps1` checks the maps'). The export (5.4) writes every
  variant of such a name, the two skins' too, with the qualifier and the
  other names plain.
- **Suffixes** follow the name with its qualifier:
  `textures/rtex343~ad81_n.png`, `textures/rtex343~ad81.mat`.

## Colors and data

- **Albedo and emissive are 8-bit colors**, as the original textures are:
  the engine's transfer, the 2.2 power of linear light by default, the
  sRGB curve with `r_srgb 1` (DECISIONS R104). Tools write sRGB, which the
  2.2 power reads darker only in the dark tones (a value of 25: 0.62 of
  sRGB's linear light, 50: 0.87, from 128 up within about 2 %), as it reads
  the originals.
- **Normal, roughness, metallic and occlusion are data**, read as they
  are, in every format; a PNG's gamma and color profile chunks are
  ignored.
- **The albedo is used as authored,** without a correction. Physically
  based albedo is usually lighter than Hexen II's (the cathedral's mean
  is 0.04 in linear light), which makes bounce light brighter; 5.6's test
  pack shows whether that needs anything.

## Settings (`.mat`)

`textures/<name>.mat` (with the name's qualifier, if any): one setting per
line, `key value`; `#` starts a comment; an unknown key, a bad value or a
setting that doesn't apply is reported and skipped (as in the map file,
R89).

| Key | Values | Default | Meaning |
|---|---|---|---|
| `kind` | `regular`, `chrome`, `glass` (world textures); `regular`, `chrome` (skins) | `regular` | Quake II RTX's material kinds: `chrome` a mirror tinted by the albedo where its roughness is below 0.02 (Quake II RTX's `MAX_MIRROR_ROUGHNESS`), shaded as `regular` with its `metallic` above (set `metallic 1` for metal; at the default roughness 1 no mirror); `glass` refracts and goes into the transparent geometry; both are 5.5's. The kinds the name or the entity gives stay and can't be set: by the name (`gl_model.c`) `sky…` the sky, turbulent `*` textures `lava` (`*lava…`), `slime` (`*slime…`), Quake II RTX's transparent kind (`*rtex078`, `*lowlight`), `water` (the others); by the entity Quake II RTX's transparent-model kind (models drawn translucent: `EF_TRANSPARENT`, `EF_SPECIAL_TRANS`, `DRF_TRANSLUCENT`) |
| `roughness` | 0–1 | 1 | without a roughness map the roughness, with one a factor on it (glTF's rule) |
| `metallic` | 0–1 | 0 without a map, 1 with one | without a metallic map the metallic value, with one a factor on it |
| `bump` | ≥ 0 | 1 | the normal map's strength (Quake II RTX's `bump_scale`, times `pt_bump_scale`) |
| `specular` | ≥ 0 | 1 where the roughness is authored (`_r`, `_orm` or a `roughness` key), else `r_specular` (0) | the dielectric specular (Quake II RTX's `specular_factor`: its Fresnel term from 4 % head-on; metals take 1 whatever it is) |
| `emissive` | ≥ 0 | 1 with `_e`, else 0 (lava's and the light models' flames' own, R82) | the emission: `_e`'s color, or the albedo's without one, times this, in lava's units (a color of 1 emits `r_emissive_scale`, 32) |

What emits other than lava and the light models' flames lights its
surroundings only through bounce rays: the light lists hold the map
lights and lava's triangles; the flames are flagged so that bounce rays
skip them, because their map light lights for them (R82); `r_lava_light`
is lava's alone. A lava texture's replaced albedo or `_e` is what emits;
its triangles' light color follows it (5.5).

Left out: `opacity` (holes are the model's, `EF_HOLEY` and sprites, as in
GL; the world's geometry is opaque, so a world texture with holes needs a
masked world group first), `ior` (Quake II RTX has no index of refraction
per material), `blend` (translucency is the entity's, `EF_TRANSLUCENT`,
and the kind's; 5.5), Quake II RTX's surface-light keys (`is_light`,
`light_styles`, `bsp_radiance`, `default_radiance`, `synth_emissive`,
`emissive_threshold`: Hexen II's lights are its light entities), texture
paths (`texture_base` and the like: the names decide), several textures
in one file (Quake II RTX's `materials/*.mat` sections).

## Defaults

- **A texture without files is as now:** its original texture, roughness
  1, metallic 0, a flat normal, the specular `r_specular` (0: matte, as
  GL; not physically based, a dielectric reflects about 4 % head-on;
  R92), emissive only lava and the light models' flames (R82).
- **An albedo alone** replaces the colors and keeps the rest (matte); so
  does a normal map alone (the bumps shade the diffuse light). An
  authored roughness (`_r`, `_orm` or a `roughness` key) makes the
  surface physically based (the specular 1); at the default roughness 1
  on the original, dark albedo it would look like 4.9's polished walls
  (R92), which is why a normal map alone doesn't.
- **Player colors:** a player's skin is recolored by the player's colors
  (`R_TranslatePlayerSkin`, the `player<n>` textures). A player whose
  colors leave the skin as it is (an identity translation; 5.3 finds
  which colors do) shows the replaced albedo
  (`textures/models/<class>.mdl_0.png`); other players show the
  original, translated; the other maps apply to both.
- **Sprites** are unlit effects without a material: only an albedo (with
  its alpha) applies.
- **Holes kept from the original:** an albedo without alpha keeps the
  original's coverage. A masked skin then takes the original as its mask
  (today the mask is the skin itself, and `vk_models check` asserts it,
  `vk_model.c`); a sprite, which has no mask, gets the original's alpha
  merged into the image at load (5.3).

## Formats and lookup

- **Authoring:** PNG and TGA, 8 bits per channel (a 16-bit PNG is read as
  8). Any size: texture coordinates are the original's divided by its
  size, so an image covers the same surface at any resolution; keep the
  original's aspect ratio (another one stretches). The images of a set may
  have different sizes (but `_r` and `_m`, above).
- **Shipping:** DDS and KTX2 (without supercompression: Basis Universal
  is Apache-2.0, PLAN §4, and zstd would need a decoder) with their mips:
  BC7 or RGBA8 for albedo, emissive and `_orm`, BC5 (RG, Z = √(1 − x² −
  y²)) or RGBA8 for normals. The formats are read as UNORM (an `_SRGB`
  format as its UNORM twin, the same bytes: the shaders decode colors,
  R104). `_r` and `_m` only as PNG or TGA: the engine packs them into one
  roughness and metallic texture at load, which a compressed file can't be
  repacked into; so no BC4.
- **Mips** come from the file; an uncompressed image without them gets
  them made at load, as the originals'; a compressed one without them is
  used without (the GPU can't write compressed mips).
- **Lookup:** each image and the `.mat` on its own: the qualified name,
  then the plain one; for each name the extensions `.png`, `.tga`,
  `.dds`, `.ktx2` in turn, each through the whole search path (a later
  game folder first); the first file found. So an edited PNG wins over a
  shipped DDS, and a set may take its albedo from the qualified name and
  its normal from the plain one. The roughness and metallic come from the
  first name that has any of `_orm`, `_r`, `_m`: its `_orm`, else its `_r`
  and `_m` (never mixed across names). A `.mat` is used whole (settings
  aren't merged across files).
- **Reload:** `r_reloadmaterials` (5.3) reads the changed files again.
- **Cost:** a material reads its albedo and, with a normal map, the
  normal; one with a roughness or metallic map reads one texture more per
  shading point (Quake II RTX packs them into the albedo's and the
  normal's alpha instead, which shipped files can't be repacked into,
  M1); 5.3 measures it.

## Shader changes (5.3)

What the spec asks of `get_material` (`path_tracer_rgen.h`), which reads
Quake II RTX's layout today:

- the normal's green flipped: the tangent frame's bitangent runs down the
  image (world triangles `vk_world.c`, models `model_geometry.comp`), so
  the shaders read DirectX's convention, as Quake II RTX's;
- roughness and metallic from their own texture (a slot in the material
  table, `vk_material.c`), also without a normal map: today they are the
  albedo's and the normal's alpha, read only with a normal map;
- glTF's roughness rule (the value or a factor) instead of Quake II RTX's
  `roughness_override`, a floor;
- BC5 normals: Z rebuilt from X and Y (`rgbToNormal` reads Z), so the
  Toksvig adjustment loses the normal's length there;
- a masked skin's mask from the original where its albedo has no alpha
  (above; with `vk_models check`'s mask-is-the-skin test changed).

## Not in E5

Height maps and parallax, detail textures, materials per level, and the 2D
(menus, the HUD, the console: they aren't lit).
