# texpack: AI-upscaled texture packs from a manifest

Story 5.8. Takes `r_exporttextures`' PNGs and a **manifest of material
descriptions** ("worn grey castle stone", "the skin of an imp") and makes a
pack in [MATERIALS.md](../../../docs/hexenlicht/MATERIALS.md)'s names: a 4x
albedo, a normal map, an `_orm` and a `.mat` per texture. Everything runs
locally. The pack is derived from Raven's textures, so **it never goes into
this repository or a release** (PLAN §4): this folder is the pipeline and the
manifest, not images. Workflow and values: [AUTHORING.md](../../../docs/hexenlicht/AUTHORING.md).
Since story 9.4 `texpack.py run` **redraws** the world and liquid textures that E9's labels
give a redrawn tier ("Redraw" below); skins (until 9.6), effects, sprites and anything
unlabeled still take 5.8's upscale:

```
export PNGs ──► [draft: a vision-language model proposes class + description]
                         │                    ▼
                         │         materials.csv ◄── you correct it (sheet, merge)
                         ▼                    │
   pad (wrap) ─► 4x upscale ─► img2img, ControlNet Tile ─► crop ─► luminance match
        ComfyUI core nodes, one workflow per texture       │
                                                           ▼
                     PBRify normal + roughness (this process) ─► _n.png, _orm.png, .mat
```

## Setup (once, Windows, NVIDIA)

Everything lives in `$TEXPACK_HOME` (default `hexenlicht-tools\texpack` next
to the repository; nothing is installed system-wide, no Python needed):

| What | Where it goes | License |
|---|---|---|
| [ComfyUI portable, NVIDIA](https://github.com/comfyanonymous/ComfyUI/releases) (`ComfyUI_windows_portable_nvidia.7z`, 1.9 GB; its Python 3.13, PyTorch and spandrel run texpack too) | unpack (Windows' own `tar.exe -xf` does 7z) into `ComfyUI_windows_portable\` | GPL-3.0 |
| `v1-5-pruned-emaonly.safetensors` ([Stable Diffusion 1.5](https://huggingface.co/stable-diffusion-v1-5/stable-diffusion-v1-5), 4.3 GB) | `ComfyUI\models\checkpoints\` | CreativeML OpenRAIL-M |
| `control_v11f1e_sd15_tile_fp16.safetensors` ([ControlNet 1.1 Tile](https://huggingface.co/comfyanonymous/ControlNet-v1-1_fp16_safetensors), 0.7 GB) | `ComfyUI\models\controlnet\` | CreativeML OpenRAIL-M |
| `4x-PBRify-UpscalerV4.safetensors` ([PBRify_Remix](https://github.com/Kim2091/PBRify_Remix) 1.7.2 release, `Models\`) | `ComfyUI\models\upscale_models\` | CC0 |
| `1x-PBRify_NormalV3.pth`, `1x-PBRify_RoughnessV2.pth`, `1x-PBRify_Height.pth` (same release) | `models\pbrify\` | CC0 |
| [Qwen3-VL-4B-Instruct](https://huggingface.co/Qwen/Qwen3-VL-4B-Instruct) (8.3 GB, only for `draft`) | `models\qwen3-vl-4b\` | Apache-2.0 |

`texpack.py check-env` says what is missing. Run every command with ComfyUI's
Python:

```
$py = "$env:TEXPACK_HOME\ComfyUI_windows_portable\python_embeded\python.exe"
& $py tools\hexenlicht\texpack\texpack.py check-env
```

## Workflow

1. **Export** the textures: `r_exporttextures texpack` in the game (with
   `-portals` for both games; AUTHORING.md section 1). Use your own folder: the
   tools only read an export. `--export` names it (default: the data folder's
   `portals\export`, else `data1\export`).
2. **Draft** the manifest: `texpack.py draft --kind world,liquid,skin`. A local
   vision-language model proposes a class and a ten-word description per
   texture (about a second each). Drafts are marked `source=draft`.
3. **Review** it: `texpack.py montage --out pages --kind world` writes PNG pages of 40
textures each with their class and description under them: the fastest way to
spot a bottle labelled stone. `texpack.py sheet --out sheet.html` makes a page of the
   textures grouped by map (`--group kind|class`), each with its class and
   description editable. "Download edits" saves the changed rows; `texpack.py
   merge texpack_edits.csv` puts them into `materials.csv` as `source=human`
   (a later `draft` never touches those). Or edit the CSV by hand: a row can
   name a glob (`rtex02*`, `models/imp.mdl_*`) to label many textures at once.
4. **Run**: `texpack.py run --out <pack> [--select list.txt | --stems a,b | --map demo1 | --kind skin | --glob 'mtex4*']
   [--tier layout | --family bm-ashlar | --hub blackmarsh] [--data <game data>]`.
   `<pack>\textures\` is what goes into `data1\textures\`. Unchanged textures
   (same pixels, class, description, seed, tool version) are skipped, so
   correcting one label redoes one texture (`--force` redoes all). The redraw needs the
   game's palette (`--data`: a folder with `data1\pak0.pak`).
5. **Check**: `texpack.py verify --pack <pack> [--data <game data>]` (sizes, luminance, alpha, map
   encodings, `.mat` keys; the redraw's materials, cavities, seams, sets and copies), `texpack.py sheet --pack <pack>` (the result beside
   each original), then in the game `r_materials 0/1`, `vk_materials here` and
   `vk_materials problems`: `proof_run.ps1 -Pack <pack> -Data <a copy of the
   original's data1 without textures> -Out <folder>` does it at demo1's and
   meso9's views (TESTING.md "Texture pack tooling (5.8)"; never `-Data` your own
   game folder). `selftest.py` checks the functions without models or data.

## The whole export

World and liquid textures first (about 70 minutes for the 975 of both games on an
RTX 4070 Ti; the 555 skins are slower, up to 55 s each):
`texpack.py run --export <export> --manifest <manifest> --kind world,liquid --out <pack>`.
A run restarts ComfyUI every 80 textures (`--restart-every`) and frees the GPU
cache after every map inference: without these a long run slowed from 4 s to
16 s a texture within 250. Anything the review changes is redone alone on the next run.

## Skins

`texpack.py run --kind skin` makes the skins (a model's atlas, not a tile: mirrored padding,
denoise 0.16). Light, flame, missile and effect models are class `fx`: the 4x upscale only
(their glow is the engine's). `skin_metal` sets metallic 1 for the whole atlas, so give it
only to atlases that are mostly metal. Check them in the game with `proof_run.ps1
-Creatures` (monsters created around demo1's courtyard; only the types the map
precaches appear). PNG skins at 4x are about 1 GB of images in a map: ship DDS.

## What it gets wrong

The proof set (`testset.txt`, 20 textures, `materials.csv`'s rows) shows where to
look: glass has no maps (a pane is flat; the albedo keeps the scene painted behind
the lead, which tints the view: paint a new one); painted pictures (the fresco) get
a relief of their figure in the normal map (`bump=0.3`); the amber liquid comes out
as crumpled foil (the 4x model invents structure on smooth gradients; lower
`denoise` or leave it out); metal is metallic over its rust; the maps are guesses
from the picture. A draft calls most walls stone: review it.

## The manifest

`materials.csv`: `pattern,purpose,tier,class,family,regions,description,overrides,source`
(story 9.3 added purpose, tier, family and regions: "Labels" below). Lines starting with `# ` (hash, space) are comments: the
liquids' names start with `#` themselves. A pattern is a
texture's name as the export files it, without `textures\` and `.png`
(`rtex022`, `rtex343~ad81`, `#lava000`, `models/imp.mdl_0`) or an fnmatch glob.
A labeled row (a literal pattern with a tier, source `claude` or `human`) stands alone: the
texture takes it and no other row, the owner's over Claude's. Otherwise
every matching row applies, least specific first (a human row beats a draft
whatever its pattern; then a literal beats a glob, a glob with more literal
characters beats a shorter one): a later class or
description replaces an earlier one, overrides add up. `overrides` are
`key=value;key=value` over any key of `classes.toml` (`denoise=0.2;rough_max=0.8`).
A texture no row names takes the class of its export kind (world, liquid,
skin, sprite). The sky and the 2D pictures are skipped.

`classes.toml` holds what a class means: the prompt, the denoise (how far the
diffusion pass may depart from the upscale: 0.16 for skins, 0.28 for walls),
the roughness range the PBRify roughness map is stretched over, metallic,
the `.mat` values, the albedo's luminance against the original's.

## What the stages do, and why

- **Wrap padding.** A world texture repeats, so the image is padded by a
  quarter of its size with its own opposite edges (skins and sprites, which don't
  repeat: mirrored), upscaled and cropped: the result tiles without a seam.
- **Luminance match.** Hexen II's albedo is very dark (mean 0.04 in linear
  light) and a model draws it lighter; at real albedo a scene is 3.5 to 5 times
  as bright (AUTHORING.md section 5). The result is scaled in linear light to the
  original's mean times the class's `albedo_ratio` (1.2 for dielectrics, 1 for
  metals, glass, liquids and lava; DECISIONS M42, M43), after a `contrast` of 1.5 on
  the linear luminance (hue and saturation kept) that blackens the darkest joints and
  lifts the lit faces. Put the realism into
  the normal map and the roughness, not the brightness.
- **Shine.** An authored roughness makes a dielectric reflect 4 % head-on, as
  bright as the light on Hexen II's albedo: the first pack came out "super
  specular" with a grey veil on the darkest joints. The classes now write
  `specular 0.04` into the `.mat`: where the albedo is near black, the specular is
  all that is seen (`specular=`, `contrast=` and `albedo_ratio=` in a row's
  overrides change them; glass, liquids, lava and metals keep 1).
- **Alpha.** Holes and coverage come from the original (color bled under the
  holes before upscaling, the alpha upscaled and cut again for 0/255 kinds).
- **Maps.** PBRify's NormalV3 and RoughnessV2 from the final albedo.
  PBRify's normals are DirectX (green down) by measurement
  (`texpack.py calibrate`: its normal against its height model, -0.79), so
  green is flipped to OpenGL's, which MATERIALS.md wants. Roughness is the
  model's output stretched over the class's range; metallic is the class's
  (0 or 1). The maps are guesses from the picture: for the materials that matter
  (metals, glass, water) set values in the manifest or a `.mat` by hand.
- **Sprites** get the 4x upscale only (they take an albedo only).
- **Reproducible.** The seed is a CRC of the texture's name xor `--seed`;
  ComfyUI runs `--deterministic`.

Model licenses: the PBRify models are CC0, ControlNet and Stable Diffusion
1.5 are OpenRAIL(-M) (use restrictions, none that touch this), ComfyUI is GPL.
None is vendored here; see THIRD_PARTY.md.

## Redraw (stories 9.1, 9.4)

`texpack.py run` redraws a world or liquid texture instead of upscaling it when its label
(9.3: "Labels" below) gives it a redrawn **tier** (DECISIONS M46–M53, M66–M71; PLAN E9):
reimagine (a new surface at the same arrangement), layout (every shape in place), faithful
(restored, no new content), glass, liquid, lava. `redraw.py` has the stages. FLUX.2 klein 4B
(distilled, 4 steps, M50) gets the 4x upscale of the padded original and the tier's prompt with
the texture's description, and draws it as a real material under a soft frontal light. What
follows is this process, not a model:

- **The broad light out** (M68): klein's soft light often falls off across the whole drawing,
  which the normal map can't explain and a tiled wall shows as bands (9.4's oak planks a third
  darker at one end). On the whole padded drawing, before the seams, its log luminance blurred
  at a quarter of the short side is replaced by the original's: Raven's broad light and dark
  pattern, coarser than the parts (the oak hero's step across the tile 0.32, its original's
  0.27, without it 1.08).
- **Seams:** the redrawn padding is faded into the tile's opposite edges.
- **Maps:** PBRify's normal and roughness of the redraw, as 5.8's.
- **The light out:** the part of the redraw's brightness that the normal map's directions
  explain is divided out: the light from a side fully, the frontal part (faces brighter,
  joints darker) by half (`--front`, how much of it stays). A second edit "to an albedo"
  flattens the material away (M46).
- **Materials per region** (`materials.py`, M67): 9.3's regions name the material of each of
  the palette's ramps (`*=stone;yellow=gold`), so every texel of the original has one; at 4x
  the materials are weights, the texels' upscaled and snapped to the redraw's edges (a guided
  filter). Each material's values are `classes.toml`'s `[materials]`: the roughness range the
  roughness model's output is stretched over in its texels, metallic (per texel in `_orm`'s
  blue), the contrast and the brightness ratio. A ramp the regions don't name takes the
  class's values, and so do a row's overrides of `rough_min`, `rough_max`, `metallic`,
  `contrast` and `albedo_ratio`: with regions that name every ramp (`*=stone`) they change
  nothing; change the regions or a material's values instead. A hole takes the material of the
  nearest opaque texel. The materials are only as good as the ramps: where Raven painted two
  materials in one ramp (rtex367's dark wood in the grey of its iron straps) the regions decide.
- **Brightness, then colors:** 5.8's match (M37, M43) per material (each material's mean to the
  original's there times its ratio: gold stays as bright against the stone as Raven painted
  it), then the original's broad colors put back (Oklab; the models draw real-world colors,
  Hexen II's palette is the theme), then the brightness again.
- **Cavities** (M68): where the albedo is near black against its material's median (a gap
  between planks, a joint the redraw drew black; reimagine and layout, not a picture's black
  paint) the roughness is 1. `_orm`'s red is an occlusion: the redraw's own frontal shading term
  against a face turned to the viewer (`exp(c_z (n_z − 1))`), times that near-black factor, so a
  gap is fully occluded. The renderer reads it from 9.7 (not physically based: geometry the
  normal map lacks); until then it changes nothing in the game.
- **Special colors:** texels in the palette's saturated row (240–254: glowing eyes, the
  archer's arrow) keep the original's color, unless more than 5 % of the texture is in the
  row (rtex465, the water); the palette comes from the game's `pak0.pak` (`--data`).
- **Copies, sets, animations** (`groups.py`, M69): a texture with the same pixels as another
  (36 groups in both games, mostly the mission pack's renames) is drawn once, as the one with
  the most area, and its files copied. Textures of one size whose originals share a fifth or
  more of their texels at the same places (120 sets of 359 textures: the Four Horsemen's frame,
  a wall with and without its chains, the switches' discs) and an animation's frames are a
  **set**: each member is drawn alone with one seed, its first member's (the most area; or the
  first `seed=` override among the members), and
  then each texel takes the drawing of the first member whose original has the same color there,
  in every map (a ramp of a texel where that member changes), and each member's own texels are
  scaled to its brightness target again (a rune's brightest frame had come out at 0.66 of it). A
  canvas of the set side by side broke beyond four framed panels, and a member as another's
  reference copied its rune (9.4's spike).
- **Heroes** (M70): a family's lead (`families.csv`) is drawn first; the family's layout
  members get its drawing as a second image, a reference for the material ("the same stone,
  its detail and wear, image 1's layout": an ashlar trim's layout kept 0.63 with it, 0.22
  without). Not reimagine members (the hero's arrangement leaked into them: a rock face 0.86 ->
  0.18, planks 0.65 -> 0.34), faithful ones (their content must not change) nor members of the
  hero's set. A new hero (9.5 picks one by `seed=`) redraws its layout members.

A run draws what a selection needs: its sets whole, its copies' sources, its heroes. It is
incremental as 5.8's (M41): a texture is skipped when its model image (the source, the prompt,
the seed, the size, the hero's drawing) and its stages (the class's and materials' values, the
models, `--front`, the tool's version) are unchanged, and only the stages are redone from
`work\<stem>_A.png` when only they changed (`--reuse` keeps every earlier model image). The
stages write `work\<stem>.png`, `_n.png`, `_orm.png`; a last step writes `textures\` (a set's
members unified, the copies, the `.mat`) where what they come from changed or a file is
missing. `<pack>\.texpack\redraw.json` has per texture what it was made from and 9.1's
numbers: the light left in (`light_dir_before`/`after`), the seam against the original's, the
color drift, the layout kept, the brightness ratio, each material's gain, the share of
cavities, the occlusion's mean. A row's `seed=1234` override replaces the seed from the name.

```
& $py tools\hexenlicht\texpack\texpack.py run --select redrawset.txt --data <game data> --out <pack>
& $py tools\hexenlicht\texpack\texpack.py verify --select redrawset.txt --data <game data> --pack <pack>
& $py tools\hexenlicht\texpack\redraw.py sheet --packs v2=D:/...pack --stems rtex022,rtex013 --out <dir> --lift 4
& $py tools\hexenlicht\texpack\bspviews.py --export <export> --maps demo1,meso9 --stems rtex022,mtex466 > views.txt
```

`redrawset.txt` is the test selection (9.1's pilot and 9.4's sets, families and copies). About
6.5 s an edit on an RTX 4070 Ti (the hero as a reference costs nothing measurable at a quarter
of a megapixel); a run restarts ComfyUI every 80 edits (`--restart-every`). `redraw.py sheet`
writes a PNG per texture: the original, the 5.8 pack (`--base`), per pack the model's image,
the albedo, the normal, the `_orm` and a 2x2 tiling.

The models (Apache-2.0; `classes.toml`'s `[redraw]`), in ComfyUI's folders or any folder named
in `$TEXPACK_HOME\extra_model_paths.yaml` (`comfy.py` passes it to ComfyUI; the owner's are on
C:): `flux-2-klein-4b-fp8.safetensors` from black-forest-labs/FLUX.2-klein-4b-fp8 in
`diffusion_models`, `qwen_3_4b.safetensors` in `text_encoders` and `flux2-vae.safetensors` in
`vae` (Comfy-Org/vae-text-encorder-for-flux-klein-4b), about 16 GB; `check-env` finds them.
9.1 compared klein's base model and Qwen-Image-Edit-2511 (M46, M50); 9.4 left them out of the
tool.

`bspviews.py` finds a view of each texture in the maps (`vk_setpos` lines; since 9.2 the
census's picker, below) for `proof_run.ps1 -ViewFile` (`-Portals` for the mission pack's maps).
TESTING.md, "Art-directed pack pilot (9.1)" and "Texpack pipeline v2 (9.4)".

## Census and views (story 9.2)

What each world texture is for, from the game itself (9.3 labels from it; DECISIONS M54–M56):
`census.csv` here has a row per world, liquid and sky texture of both games' export, read from
the maps' BSPs in the paks in about a minute (`census.py`, `bspviews.py`'s reader). It holds
facts about the maps, no pixels, so it is in the repository; `texpack.py census` makes it again.

| Column | What |
|---|---|
| `stem`, `name`, `kind`, `size` | the texture as the export names it, its BSP name, world, liquid or sky, its size in texels |
| `game`, `hubs`, `maps`, `area`, `faces` | where it is: data1 or portals; the hubs and maps (`map:area`) by area, most first; the area in square units of its drawn faces, their count |
| `floor`, `wall`, `ceiling` | percent of the area by the face's normal (\|z\| > 0.7: floor or ceiling; a liquid's surface is both, seen from either side) |
| `entities` | percent of the area on brush entities: `door`, `button` (and `func_angletrigger`, `func_pressure`), `plaque`, `mover` (platforms, trains, crushers, rotators, pushables), `breakable`, `static` (`func_wall`, `func_illusionary`), `other`; the rest is the world. What the game doesn't draw doesn't count: triggers, weather volumes, invisible plaques, breakables and (the mission pack's) walls, glowing trains (DECISIONS M54) |
| `scale`, `tiles` | world units per texel (texinfo; `2` is a texture drawn at twice its size, `1x2` differs along s and t), and how many tiles the typical face spans (`4x0.25`: a strip, a quarter of the texture's height shown) |
| `light`, `light_lo`, `light_hi` | how dark it usually is: its faces' lightmaps (0–255, the styles summed with switched lights on), the area's mean and its 10th and 90th percentile; empty for liquids and the sky (no lightmap) |
| `beside`, `corner` | the textures it meets at shared edges, by the edges' length in units: on the same plane (a trim and its wall) and at an angle (the floor under a wall) |
| `anim` | an animation's frames (`+0…`, `+a…`), the same in every member's row |
| `view`, `view_rank`, `view_map`, `view_close`, `view_wide`, `view_hit` | the views: `picked` (not shot yet), `ok` (`vk_materials here` named it), `none` (no face the camera sees), `unused` (on no drawn face), `frame` (a frame no face shows: its animation's view), `sky`; which face it is (1 the best), the map, `vk_setpos` numbers of both views, what was hit instead |

```
& $py tools\hexenlicht\texpack\texpack.py census --export <both games' export>
& $py tools\hexenlicht\texpack\texpack.py views --todo --game data1 --data <data copy> --out views_data1.txt
& pwsh tools\hexenlicht\texpack\views_run.ps1 -Views views_data1.txt -Data <data copy> -Out <runs> -Tag r1_data1 -Release
& $py tools\hexenlicht\texpack\texpack.py checkviews <runs>\r1_data1 --data <data copy> --export <export>
```

The **views** (`bspviews.py`): a texture's faces in the map where it covers the most area
first; in a map by their area, a dark face counting less and a brush entity's less (it may
move); the first face the camera can see gets a close view along its normal at a distance that
fits it (48–220 units; floors and ceilings from 55°) and a wide one about 2.5 times as far.
Each is checked as the game will show it: exact traces through the world and the brush
entities, the player's box off triggers (a teleporter would move it, a `trigger_once` would
change the map for the views after it), the view's angles in the protocol's steps, and first
clear of monsters and props (a torch's flame in front of a wall). **`views_run.ps1`** shoots the
originals at both and the wide view's albedo (`r_debugview 1`, unlit: readable where the room
is dark), paused, with `vk_materials here` after each; under a second a texture.
**`checkviews`** reads a run: where `vk_materials here` named the texture (any frame of an
animation) the view is `ok` and its shots become `$TEXPACK_HOME\views\<stem>_close.png`,
`_wide.png` and `_albedo.png`; a miss gets the texture's next face (up to 8), so `views
--todo` and another run take only those. `census --reset` starts every view again at the first
face (after a change to the picker). The shots are derived from Raven's textures: they stay
outside the repository.

## Labels (story 9.3)

E9's art direction per texture (DECISIONS M57–; the rules and the hubs' styles in
[STYLE.md](../../../docs/hexenlicht/STYLE.md)), written by Claude from 9.2's census and shots,
in the manifest's columns:

| Column | What |
|---|---|
| `purpose` | what the texture is for: fill, trim, pillar, panel, door, window, grate, picture, sign, symbol, prop, liquid, sky, tool; skins: creature, player, weapon, item, object, debris, fx (`classes.toml`'s `[labels]`) |
| `tier` | how far a redraw may depart (redraw.py's reimagine, layout, faithful, glass, liquid, lava, skin; fx: upscaled only; skip) |
| `family` | the textures that should read as one material (`families.csv`: name, hub, lead, what; each family's lead is redrawn as its hero in 9.5) |
| `regions` | which of the palette's ramps is which material (`grey,taupe=iron;orange,amber=wood`; `*` the ramps not named; `a/b` a ramp of two materials, the larger first), from `classes.toml`'s `[materials]` |
| `source` | `claude` for 9.3's labels; the owner's answers make a row `human` |

The palette's 256 colors are 17 **ramps** (`labels.py`): one grey of 32 (black to white), then
mostly 16, two of 8 (maroon, brown), one of 15 (amber) and a single purple, and the saturated
row (240–254, `special`, kept as drawn: M49). Raven shaded one material over several ramps, so
a region is a set of them.

```
& $py tools\hexenlicht\texpack\texpack.py cards --kind world,liquid,sky --hub thysis --data <data copy> --out <dir>\thysis
& $py tools\hexenlicht\texpack\texpack.py cards --kind world --hub tulku --overview --out <dir>\tulku
& $py tools\hexenlicht\texpack\texpack.py cards --kind skin --select <list> --data <data copy> --out <dir>\creatures
& $py tools\hexenlicht\texpack\texpack.py labels check --labels <labels.csv> --kind world,liquid,sky --data <data copy>
& $py tools\hexenlicht\texpack\texpack.py labels apply --labels <labels.csv> --data <data copy>
& $py tools\hexenlicht\texpack\texpack.py labels leads --data <data copy>
& $py tools\hexenlicht\texpack\texpack.py sheet --questions <labels.csv> --out questions.html
& $py tools\hexenlicht\texpack\texpack.py merge texpack_answers.csv
```

- **`cards`** writes the pages a labeler reads, 6 textures a page: the original, its ramp map
  (each texel colored by its ramp, the shares under it), 9.2's close, wide and albedo shots, and
  a .txt beside each page with the census facts (home hub, maps, floor, wall or ceiling, brush
  entities, size in metres, tiles, light, neighbours, animation, ramps, the earlier label, the
  related textures). One card per animation (its frames are labeled alike). `--overview`: the
  originals only, 60 to a page; `--family`: a family's members side by side (the review).
  **Skins** have pages of their own (`--kind skin`, 4 a page): the atlas, its ramp map and the
  model drawn with it from the front, its left side and the back (`mdlview.py`), and a .txt with
  the model's facts: which game's, the skin's number of the model's skins, triangles, frames,
  flags (rotates, holey, translucent, a trail), its size in metres, what the gamecode does with
  it (the spawn classes whose `/*QUAKED*/` comment heads the function that sets it, or the up to
  four whose spawn functions call such a helper, `init_imp`; the other functions that name it;
  comments left out), the maps whose entities place those, a puzzle item's name; `related` is
  only `texels` for skins, of the texels not black in both (any two atlases share their unused
  black, and their ramp shares are mostly it).
- **`families`** lists each texture's related ones: `texels` (the same color at the same place
  in a texture of its size: Raven's variants), `beside` (long shared edges on one plane, from the
  census), `colors` (the same ramp shares in its home hub). Candidates for a labeler, not
  decisions.
- **`labels check`** checks every selected texture's labeled row: the words, tier and class
  agreeing (glass, liquid, lava, skip, fx and skin tiers have their own classes), regions that
  cover every ramp of 5 % or more and name only ramps the texture uses, families in
  `families.csv`, an animation's frames alike, copies (the same pixels under two names)
  labeled alike, a family's lead one of its members. With `--labels` it checks a labels CSV as
  if applied.
- **`labels apply`** puts a labels CSV (the manifest's columns, a stem per row) into the manifest
  as literal rows: the class and overrides the texture had are folded in, an animation's other
  frames get a copy, and the old rows that matched a labeled texture and match no unlabeled one
  are dropped. A labeled row of the owner's stays unless the CSV's row is `human` too.
- **`labels leads`** gives each family without a lead its member with the most area in the maps
  (a skin family: its biggest atlas).
- **`sheet --questions`** is the owner's page: the textures a labels CSV's `question` column asks
  about, with 9.2's shots (a skin: its atlas and three views of its model; `--data` for the
  paks), the labels editable and an answer each; "Download answers" saves the
  answered or edited rows, `merge` takes their fields as the owner's (`human`) and copies them to
  an animation's other frames (the `answer` column is read by hand). It embeds Raven's textures:
  keep it local.

**`mdlview.py`** reads an alias model from the paks (Hexen II's IDPO and the mission pack's
RAPO, whose texture coordinates are their own; where both games have a model under one name,
the export's version: its game, then its skins' size) and draws it with a skin in its first frame,
unlit, from given sides (yaw 0 its front), each side at one scale: the skin cards' views, and
9.6's turntables to compare skins exactly (9.1's prototype; a row per skin):

```
& $py tools\hexenlicht\texpack\mdlview.py models/archer.mdl --png orig=<export png> klein=<pack png> --views 0,45,90,180 --out archer.png --data <data copy>
```
