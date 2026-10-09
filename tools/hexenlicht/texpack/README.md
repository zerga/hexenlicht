# texpack: AI-upscaled texture packs from a manifest

Story 5.8. Takes `r_exporttextures`' PNGs and a **manifest of material
descriptions** ("worn grey castle stone", "the skin of an imp") and makes a
pack in [MATERIALS.md](../../../docs/hexenlicht/MATERIALS.md)'s names: a 4x
albedo, a normal map, an `_orm` and a `.mat` per texture. Everything runs
locally. The pack is derived from Raven's textures, so **it never goes into
this repository or a release** (PLAN §4): this folder is the pipeline and the
manifest, not images. Workflow and values: [AUTHORING.md](../../../docs/hexenlicht/AUTHORING.md).

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
4. **Run**: `texpack.py run --out <pack> [--select list.txt | --stems a,b | --map demo1 | --kind skin | --glob 'mtex4*']`.
   `<pack>\textures\` is what goes into `data1\textures\`. Unchanged textures
   (same pixels, class, description, seed, tool version) are skipped, so
   correcting one label redoes one texture (`--force` redoes all).
5. **Check**: `texpack.py verify --pack <pack>` (sizes, luminance, alpha, map
   encodings, `.mat` keys), `texpack.py sheet --pack <pack>` (the result beside
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

## Redraw (story 9.1, E9's pilot)

`redraw.py` redraws a texture instead of upscaling it (DECISIONS M46–M52; PLAN E9):
an image-editing model gets the 4x upscale of the padded original and a prompt for the
texture's **tier** (reimagine: a new surface at the same arrangement; layout: every shape
in place; faithful: restored, no new content; glass; liquid; lava; skin) and draws it as a
real material under a soft frontal light. What follows is this process, not a model:

- **Seams:** the redrawn padding is faded into the tile's opposite edges.
- **Maps:** PBRify's normal and roughness of the redraw, as `texpack.py run`'s.
- **The light out:** the part of the redraw's brightness that the normal map's directions
  explain is divided out: the light from a side fully, the frontal part (faces brighter,
  joints darker) by half (`--front`, how much of it stays). A second edit "to an albedo"
  flattens the material away (M46).
- **Brightness, then colors:** 5.8's match (M37, M43), then the original's broad colors
  put back (Oklab; the models draw real-world colors, Hexen II's palette is the theme),
  then the brightness again.
- **Special colors:** texels in the palette's saturated row (240–254: glowing eyes, the
  archer's arrow) keep the original's color, unless more than 5 % of the texture is in the
  row (rtex465, the water); the palette comes from the game's `pak0.pak` (`--data`).
- **Animations:** the frames of `+0…` to `+9…`, `+a…` share one seed; a pack needs them all.

```
& $py tools\hexenlicht\texpack\redraw.py run --gen klein --export <both games' export> --out <pack>
& $py tools\hexenlicht\texpack\redraw.py sheet --packs klein=D:/...pack --base <5.8 pack> --out <dir> --lift 4
& $py tools\hexenlicht\texpack\bspviews.py --export <export> --maps demo1,meso9 --stems rtex022,mtex466 > views.txt
```

`--gen`: `klein` (FLUX.2 klein 4B, distilled: the generator since M50, about 6.5 s a
texture), `klein-base`, `qwen` (Qwen-Image-Edit-2511 with the 8-step Lightning LoRA, 37 s).
The rows are `pilot.csv`'s (stem, tier, class, description, overrides: the manifest's
classes and keys); 9.4 brings the stage into `texpack.py run`. A run skips a texture whose
model image (the source, the prompt, the generator, the seed) and stages (the class's values,
the models, `--front`, the tool's version) are unchanged and redoes only the stages from
`work\<stem>_A.png` when only they changed; `--reuse` does that for every texture, keeping
what each image was made from; `measure.json` has the numbers
(the light left in, the seam, the color drift, the layout kept, the brightness ratio).

The models (Apache-2.0), in ComfyUI's folders or any folder named in
`$TEXPACK_HOME\extra_model_paths.yaml` (`comfy.py` passes it to ComfyUI; the owner's are on
C:): `flux-2-klein-4b-fp8.safetensors` (and `-base-`) from black-forest-labs/FLUX.2-klein-4b-fp8
in `diffusion_models`, `qwen_3_4b.safetensors` in `text_encoders` and `flux2-vae.safetensors`
in `vae` (Comfy-Org/vae-text-encorder-for-flux-klein-4b); for `qwen`
`qwen_image_edit_2511_fp8mixed.safetensors` (Comfy-Org/Qwen-Image-Edit_ComfyUI),
`qwen_2.5_vl_7b_fp8_scaled.safetensors` and `qwen_image_vae.safetensors`
(Comfy-Org/Qwen-Image_ComfyUI), `Qwen-Image-Edit-2511-Lightning-8steps-V1.0-bf16.safetensors`
in `loras` (lightx2v/Qwen-Image-Edit-2511-Lightning). About 16 GB and 31 GB.

`bspviews.py` finds a view of each texture in the maps (`vk_setpos` lines; since 9.2 the
census's picker, below) for `proof_run.ps1 -ViewFile` (`-Portals` for the mission pack's maps).
TESTING.md, "Art-directed pack pilot (9.1)".

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
- **`labels leads`** gives each family without a lead its member with the most area in the maps.
- **`sheet --questions`** is the owner's page: the textures a labels CSV's `question` column asks
  about, with 9.2's shots, the labels editable and an answer each; "Download answers" saves the
  answered or edited rows, `merge` takes their fields as the owner's (`human`) and copies them to
  an animation's other frames (the `answer` column is read by hand). It embeds Raven's textures:
  keep it local.
