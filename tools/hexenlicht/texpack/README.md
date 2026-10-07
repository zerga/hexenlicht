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
3. **Review** it: `texpack.py sheet --out sheet.html` makes a page of the
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

## What it gets wrong

The proof set (`testset.txt`, 20 textures, `materials.csv`'s rows) shows where to
look: glass has no maps (a pane is flat; the albedo keeps the scene painted behind
the lead, which tints the view: paint a new one); painted pictures (the fresco) get
a relief of their figure in the normal map (`bump=0.3`); the amber liquid comes out
as crumpled foil (the 4x model invents structure on smooth gradients; lower
`denoise` or leave it out); metal is metallic over its rust; the maps are guesses
from the picture. A draft calls most walls stone: review it.

## The manifest

`materials.csv`: `pattern,class,description,overrides,source`. Lines starting with `# ` (hash, space) are comments: the
liquids' names start with `#` themselves. A pattern is a
texture's name as the export files it, without `textures\` and `.png`
(`rtex022`, `rtex343~ad81`, `#lava000`, `models/imp.mdl_0`) or an fnmatch glob.
Every matching row applies, least specific first (a literal beats a glob, a
glob with more literal characters beats a shorter one): a later class or
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
  original's mean times the class's `albedo_ratio` (1.4 for dielectrics, 1 for
  metals, glass, liquids and lava: with the specular at 0.25 the lit wall came out
  at 0.7x of the original's at equal means; DECISIONS M42). Put the realism into
  the normal map and the roughness, not the brightness.
- **Shine.** An authored roughness makes a dielectric reflect 4 % head-on, as
  bright as the light on Hexen II's albedo: the first pack came out "super
  specular" with a grey veil on the darkest joints. The classes now write
  `specular 0.25` into the `.mat` (`specular=` in a row's overrides changes it;
  glass, liquids and lava keep 1).
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
