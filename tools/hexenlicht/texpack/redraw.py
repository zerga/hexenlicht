"""texpack's redraw stage (story 9.1, the pilot of E9; docs/hexenlicht/PLAN.md section 8):
a texture's surface redrawn by an image-editing model, not only upscaled.

One pass through ComfyUI's core nodes, with FLUX.2 klein 4B (`klein`, `klein-base`) or
Qwen-Image-Edit-2511 with its 8-step Lightning LoRA (`qwen`): the 4x upscale of the padded
original is redrawn as a detailed material under a soft frontal light (`work\\<stem>_A.png`);
how far it may depart from the original is the texture's tier. Then, in this process:
  seams    the redrawn padding is blended into the tile's opposite edges (world textures);
  maps     normal and roughness from the redrawn image (PBRify, as 5.8), so the relief is
           where the drawn light shows it;
  light    the part of the image's brightness that the normal map's directions explain is
           divided out: the directional part (light from a side) fully, the frontal part
           (faces towards the viewer brighter, slopes and joints darker: a cavity term) by
           1 - `front`, so `front` of it stays (DECISIONS: a second edit "to an albedo"
           flattened the material away);
  bright   5.8's brightness match (M37, M43);
  colors   the original's broad colors put back (Oklab's a and b blurred at a tenth to a
           32nd of the texture, by tier): the models draw real-world colors, Hexen II's
           palette is the theme.
measure.json gets numbers for what the pilot judges: seams, light left in, color drift, layout
kept, brightness. Liquids and lava keep their light and colors as drawn (nothing to take out:
the engine warps a liquid, lava emits), but get the colors back too.

  python redraw.py run --pilot pilot.csv --gen klein --out <pack> [--stems a,b] [--export <export>]
  python redraw.py sheet --pilot pilot.csv --packs klein=<pack>,qwen=<pack> --base <5.8 pack> --out <dir>

A pack's textures\\ holds the files in MATERIALS.md's names (as texpack.py run's), work\\ the
model's image, measure.json the numbers.
"""
import argparse
import csv
import hashlib
import io
import json
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import numpy as np  # noqa: E402
from PIL import Image  # noqa: E402

import manifest  # noqa: E402
import pipeline  # noqa: E402
from pipeline import GAMMA, LUM  # noqa: E402

GEN_PIXELS = 1024 * 1024        # the models' native size: the padded image is scaled to about this

PROMPT_A = {
    'reimagine': (
        "Turn this low-resolution game texture into a high-resolution photorealistic material texture of {desc}. "
        "Keep the composition: the same number, size and arrangement of the parts, the same joint lines and the same colors. "
        "Redraw every surface with rich natural detail, grain, pores, chips, cracks, wear and grime, as a real {noun}. "
        "Soft even frontal light, no cast shadows. A flat orthographic view filling the whole image edge to edge, "
        "no perspective, no frame, no text."),
    'layout': (
        "Turn this low-resolution game texture into a high-resolution photorealistic texture of {desc}. "
        "Keep every shape, edge, ornament and part exactly where it is, with the same colors. "
        "Replace the blurry painted surface with real material detail: grain, pores, chips, scratches, wear and grime. "
        "Soft even frontal light, no cast shadows. A flat orthographic view filling the whole image edge to edge, "
        "no perspective, no frame, no text."),
    'faithful': (
        "Restore this low-resolution game texture as a sharp high-resolution image of {desc}. "
        "Keep every figure, symbol, letter, line and color exactly as it is; do not add, remove or change any content. "
        "Only make the edges crisp and add fine natural texture of the material. "
        "Soft even frontal light. A flat orthographic view filling the whole image edge to edge."),
    'skin': (
        "This is the texture atlas of a 3D game character's skin, laid out flat: {desc}. "
        "Repaint it at high resolution with realistic material detail: scratches and dents in metal, grain in leather and wood, "
        "weave in cloth, pores and scales in hide. Keep every part, outline and color exactly where it is, "
        "keep the black background black, and keep the same layout of the pieces. Soft even light."),
}
PROMPT_A['glass'] = (
    "Turn this low-resolution game texture into a high-resolution texture of {desc}, seen flat against a uniform white "
    "backlight. Keep the window's shape, the tracery, the frame and every lead line exactly where they are. Replace whatever "
    "is painted behind the lead with translucent glass panes of even tints, slightly mottled, with no scene, landscape or "
    "objects visible through them. A flat orthographic view filling the whole image edge to edge, no text.")
PROMPT_A['liquid'] = PROMPT_A['faithful']
PROMPT_A['lava'] = PROMPT_A['faithful']
AS_DRAWN = ('liquid', 'lava')       # no light taken out (a liquid is warped, lava emits); colors put back as for all
FRONT = 0.5                         # how much of the frontal (cavity) shading stays
# the color guard: how much of the original's broad colors comes back, and at what scale (a
# fraction of the texture's short side): coarse where the shapes may move, fine where they stay
COLOR_GUARD = {'reimagine': (0.8, 1 / 10), 'layout': (1.0, 1 / 24), 'faithful': (1.0, 1 / 24),
               'glass': (1.0, 1 / 24), 'skin': (1.0, 1 / 32), 'liquid': (1.0, 1 / 24), 'lava': (1.0, 1 / 24)}
SPECIAL = range(240, 255)           # the palette's saturated row (the archer's green eyes are 243-246): kept as Raven drew it

GENS = {
    'klein': {'family': 'flux2', 'unet': 'flux-2-klein-4b-fp8.safetensors', 'steps': 4, 'cfg': 1.0},
    'klein-base': {'family': 'flux2', 'unet': 'flux-2-klein-base-4b-fp8.safetensors', 'steps': 20, 'cfg': 4.0},
    'qwen': {'family': 'qwen', 'unet': 'qwen_image_edit_2511_fp8mixed.safetensors', 'steps': 8, 'cfg': 1.0,
             'lora': 'Qwen-Image-Edit-2511-Lightning-8steps-V1.0-bf16.safetensors'},
}
NEGATIVE = "blurry, low resolution, pixelated, perspective, vignette, frame, border, text, watermark, strong shadows, harsh highlights"


# -- the workflows -------------------------------------------------------------

def _front(wf, image_name, upscaler, w, h):
    """LoadImage, the 4x model (when upscaler), scaled to w x h: node '9' is the image."""
    wf['4'] = {'class_type': 'LoadImage', 'inputs': {'image': image_name}}
    src = ['4', 0]
    if upscaler:
        wf['5'] = {'class_type': 'UpscaleModelLoader', 'inputs': {'model_name': upscaler}}
        wf['6'] = {'class_type': 'ImageUpscaleWithModel', 'inputs': {'upscale_model': ['5', 0], 'image': src}}
        src = ['6', 0]
    wf['9'] = {'class_type': 'ImageScale', 'inputs': {'image': src, 'upscale_method': 'lanczos', 'width': w, 'height': h, 'crop': 'disabled'}}


def workflow(gen, image_name, prompt, seed, w, h, upscaler=None):
    """The edit, as ComfyUI's templates for the two models (image_flux2_klein_image_edit_4b_*,
    image_qwen_image_edit_2511), with the image scaled to w x h by us rather than to a megapixel."""
    g = GENS[gen]
    wf = {}
    _front(wf, image_name, upscaler, w, h)
    if g['family'] == 'flux2':
        wf.update({
            '1': {'class_type': 'UNETLoader', 'inputs': {'unet_name': g['unet'], 'weight_dtype': 'default'}},
            '2': {'class_type': 'CLIPLoader', 'inputs': {'clip_name': 'qwen_3_4b.safetensors', 'type': 'flux2', 'device': 'default'}},
            '3': {'class_type': 'VAELoader', 'inputs': {'vae_name': 'flux2-vae.safetensors'}},
            '10': {'class_type': 'VAEEncode', 'inputs': {'pixels': ['9', 0], 'vae': ['3', 0]}},
            '11': {'class_type': 'CLIPTextEncode', 'inputs': {'text': prompt, 'clip': ['2', 0]}},
            '13': {'class_type': 'ReferenceLatent', 'inputs': {'conditioning': ['11', 0], 'latent': ['10', 0]}},
            '15': {'class_type': 'EmptyFlux2LatentImage', 'inputs': {'width': w, 'height': h, 'batch_size': 1}},
            '16': {'class_type': 'Flux2Scheduler', 'inputs': {'steps': g['steps'], 'width': w, 'height': h}},
            '17': {'class_type': 'KSamplerSelect', 'inputs': {'sampler_name': 'euler'}},
            '18': {'class_type': 'CFGGuider', 'inputs': {'model': ['1', 0], 'positive': ['13', 0], 'negative': ['14', 0], 'cfg': g['cfg']}},
            '19': {'class_type': 'RandomNoise', 'inputs': {'noise_seed': seed}},
            '20': {'class_type': 'SamplerCustomAdvanced', 'inputs': {'noise': ['19', 0], 'guider': ['18', 0], 'sampler': ['17', 0],
                                                                     'sigmas': ['16', 0], 'latent_image': ['15', 0]}},
            '21': {'class_type': 'VAEDecode', 'inputs': {'samples': ['20', 0], 'vae': ['3', 0]}},
        })
        if g['cfg'] > 1.0:     # the base model: a real negative, with the reference too
            wf['12'] = {'class_type': 'CLIPTextEncode', 'inputs': {'text': NEGATIVE, 'clip': ['2', 0]}}
        else:                  # the distilled one ignores it: the template's zeroed positive
            wf['12'] = {'class_type': 'ConditioningZeroOut', 'inputs': {'conditioning': ['11', 0]}}
        wf['14'] = {'class_type': 'ReferenceLatent', 'inputs': {'conditioning': ['12', 0], 'latent': ['10', 0]}}
    else:
        wf.update({
            '1': {'class_type': 'UNETLoader', 'inputs': {'unet_name': g['unet'], 'weight_dtype': 'default'}},
            '30': {'class_type': 'CFGNorm', 'inputs': {'model': ['1', 0], 'strength': 1.0}},
            '31': {'class_type': 'LoraLoaderModelOnly', 'inputs': {'model': ['30', 0], 'lora_name': g['lora'], 'strength_model': 1.0}},
            '32': {'class_type': 'ModelSamplingAuraFlow', 'inputs': {'model': ['31', 0], 'shift': 3.1}},
            '2': {'class_type': 'CLIPLoader', 'inputs': {'clip_name': 'qwen_2.5_vl_7b_fp8_scaled.safetensors', 'type': 'qwen_image', 'device': 'default'}},
            '3': {'class_type': 'VAELoader', 'inputs': {'vae_name': 'qwen_image_vae.safetensors'}},
            '11': {'class_type': 'TextEncodeQwenImageEditPlus', 'inputs': {'clip': ['2', 0], 'prompt': prompt, 'vae': ['3', 0], 'image1': ['9', 0]}},
            '12': {'class_type': 'TextEncodeQwenImageEditPlus', 'inputs': {'clip': ['2', 0], 'prompt': '', 'vae': ['3', 0], 'image1': ['9', 0]}},
            '13': {'class_type': 'FluxKontextMultiReferenceLatentMethod', 'inputs': {'conditioning': ['11', 0], 'reference_latents_method': 'index_timestep_zero'}},
            '14': {'class_type': 'FluxKontextMultiReferenceLatentMethod', 'inputs': {'conditioning': ['12', 0], 'reference_latents_method': 'index_timestep_zero'}},
            '10': {'class_type': 'VAEEncode', 'inputs': {'pixels': ['9', 0], 'vae': ['3', 0]}},
            '20': {'class_type': 'KSampler', 'inputs': {'model': ['32', 0], 'seed': seed, 'steps': g['steps'], 'cfg': g['cfg'],
                                                        'sampler_name': 'euler', 'scheduler': 'simple', 'positive': ['13', 0],
                                                        'negative': ['14', 0], 'latent_image': ['10', 0], 'denoise': 1.0}},
            '21': {'class_type': 'VAEDecode', 'inputs': {'samples': ['20', 0], 'vae': ['3', 0]}},
        })
    wf['22'] = {'class_type': 'PreviewImage', 'inputs': {'images': ['21', 0]}}
    return wf


def gen_size(pw, ph, mult=16):
    s = (GEN_PIXELS / (pw * ph)) ** 0.5
    return max(mult, int(round(pw * s / mult)) * mult), max(mult, int(round(ph * s / mult)) * mult)


# -- image helpers -------------------------------------------------------------

def _lin(rgb):
    return (rgb.astype(np.float64) / 255.0) ** GAMMA


def _enc(lin):
    return np.clip(np.round(np.clip(lin, 0.0, 1.0) ** (1.0 / GAMMA) * 255.0), 0, 255).astype(np.uint8)


def _box(a, f):
    """a downsampled by an integer factor f (mean of f x f blocks; the rest cut off)."""
    h, w = a.shape[0] // f * f, a.shape[1] // f * f
    a = a[:h, :w]
    return a.reshape(h // f, f, w // f, f, *a.shape[2:]).mean(axis=(1, 3))


def _blur(y, r, wrap=False):
    """A (2r+1)-wide box blur of a 2-D array, the edges wrapped or mirrored."""
    k = 2 * r + 1
    p = np.pad(y, r, mode='wrap' if wrap else 'symmetric')
    c = np.vstack([np.zeros((1, p.shape[1])), np.cumsum(p, axis=0)])
    p = (c[k:] - c[:-k]) / k
    c = np.hstack([np.zeros((p.shape[0], 1)), np.cumsum(p, axis=1)])
    return (c[:, k:] - c[:, :-k]) / k


def _smooth(y, r, wrap=False):
    """Three box blurs of radius r // 2: close to a Gaussian of sigma about r / 2."""
    for _ in range(3):
        y = _blur(y, max(1, r // 2), wrap)
    return y


def _highpass(y, r, wrap=False):
    return y - _blur(y, r, wrap)


def _corr(a, b, mask=None):
    if mask is not None:
        a, b = a[mask], b[mask]
    a, b = a - a.mean(), b - b.mean()
    d = np.sqrt((a * a).sum() * (b * b).sum())
    return float((a * b).sum() / d) if d > 0 else 0.0


_M1 = np.array([[0.4122214708, 0.5363325363, 0.0514459929], [0.2119034982, 0.6806995451, 0.1073969566],
                [0.0883024619, 0.2817188376, 0.6299787005]])
_M2 = np.array([[0.2104542553, 0.7936177850, -0.0040720468], [1.9779984951, -2.4285922050, 0.4505937099],
                [0.0259040371, 0.7827717662, -0.8086757660]])


def _oklab(lin):
    return np.cbrt(np.maximum(lin @ _M1.T, 0)) @ _M2.T


def _from_oklab(lab):
    return np.maximum((lab @ np.linalg.inv(_M2).T) ** 3, 0) @ np.linalg.inv(_M1).T


# -- the stages ----------------------------------------------------------------

def blend_seams(big, m4, h4, w4):
    """big: the model's image at 4x the padded size; m4 the padding at 4x, (h4, w4) the tile.
    The padding right of the tile is the model's drawing of what follows the right edge, the
    tile's left edge wrapped round: the tile's left band is faded from it into the tile's own
    (and the top band from the bottom padding), so the right edge meets the left without a step."""
    k = max(2, m4 // 2)
    e = big[m4:m4 + h4 + k, m4:m4 + w4 + k].astype(np.float64)     # the tile, a band of the padding right and below
    ax = (0.5 - 0.5 * np.cos(np.linspace(0, np.pi, k)))[None, :, None]     # 0 at the edge, 1 inside
    e[:, :k] = e[:, w4:w4 + k] * (1 - ax) + e[:, :k] * ax                  # the band below too: the corners meet
    ay = ax.transpose(1, 0, 2)
    e[:k] = e[h4:h4 + k] * (1 - ay) + e[:k] * ay
    return np.clip(np.round(e[:h4, :w4]), 0, 255).astype(np.uint8)


def shading_fit(rgb, normal, r, mask=None, wrap=False):
    """The brightness detail the normal map explains: high-passed log luminance regressed on
    the high-passed normal's x, y (a light from a side) and z (from the front). Returns the
    coefficients and the multiple correlations of the directional fit (x, y) and of all three."""
    ly = np.log(np.maximum(_lin(rgb) @ LUM, 1e-4))
    n = normal.astype(np.float64) / 127.5 - 1.0
    t = _highpass(ly, r, wrap)
    X = np.stack([_highpass(n[..., k], r, wrap) for k in range(3)], -1)
    sel = mask if mask is not None else np.ones(t.shape, bool)
    tv, Xv = t[sel], X[sel]
    tot = (tv * tv).sum()
    if tot <= 0:
        return np.zeros(3), 0.0, 0.0
    c, *_ = np.linalg.lstsq(Xv, tv, rcond=None)
    cd, *_ = np.linalg.lstsq(Xv[:, :2], tv, rcond=None)
    r_all = 1 - ((tv - Xv @ c) ** 2).sum() / tot
    r_dir = 1 - ((tv - Xv[:, :2] @ cd) ** 2).sum() / tot
    return c, float(np.sqrt(max(r_dir, 0))), float(np.sqrt(max(r_all, 0)))


def take_light_out(rgb, normal, r, front=FRONT, mask=None, wrap=False):
    """Divides out the shading shading_fit finds: the directional part fully, the frontal by
    1 - front. The mean brightness is kept (the brightness match comes after)."""
    c, _, _ = shading_fit(rgb, normal, r, mask, wrap)
    n = normal.astype(np.float64) / 127.5 - 1.0
    shade = c[0] * n[..., 0] + c[1] * n[..., 1] + (1.0 - front) * c[2] * n[..., 2]
    shade = shade - _blur(shade, r, wrap)       # the detail only, as fitted: the broad brightness stays
    lin = _lin(rgb)
    out = lin * np.exp(-shade)[..., None]
    y0, y1 = (lin @ LUM).mean(), (out @ LUM).mean()
    return _enc(out * (y0 / y1 if y1 > 0 else 1.0)), c


def color_guard(orig, rgb, strength, frac, wrap=False, mask=None):
    """Puts the original's broad colors back: in Oklab, the result's a and b blurred at frac of
    the texture's short side are replaced by the original's (upscaled smoothly), times strength;
    lightness is left to the brightness match. Keeps the stone-by-stone hue of Raven's palette."""
    if strength <= 0:
        return rgb
    h, w = rgb.shape[:2]
    o = np.asarray(Image.fromarray(orig).resize((w, h), Image.BICUBIC))
    r = max(2, int(min(h, w) * frac))
    lab_o, lab = _oklab(_lin(o)), _oklab(_lin(rgb))
    out = lab.copy()
    for k in (1, 2):
        out[..., k] = lab[..., k] + strength * (_smooth(lab_o[..., k], r, wrap) - _smooth(lab[..., k], r, wrap))
    res = _enc(_from_oklab(out))
    if mask is not None:
        res = np.where(mask[..., None], res, rgb)
    return res


def load_palette(data_dir):
    """The game's palette (gfx/palette.lmp in data1's pak0.pak), 256 x 3, or None."""
    import struct
    path = os.path.join(data_dir, 'data1', 'pak0.pak')
    if not os.path.exists(path):
        return None
    with open(path, 'rb') as f:
        head = f.read(12)
        _, dofs, dlen = struct.unpack('<4sii', head)
        f.seek(dofs)
        d = f.read(dlen)
        for e in range(dlen // 64):
            name, pos, ln = struct.unpack_from('<56sii', d, e * 64)
            if name.split(b'\0')[0] == b'gfx/palette.lmp':
                f.seek(pos)
                return np.frombuffer(f.read(768), np.uint8).reshape(256, 3).copy()
    return None


def special_mask(rgb, palette):
    """The texels drawn in the palette's saturated row (SPECIAL): glowing eyes, gems, the archer's arrow."""
    if palette is None:
        return None
    key = rgb[..., 0].astype(np.int32) << 16 | rgb[..., 1].astype(np.int32) << 8 | rgb[..., 2]
    pk = palette[:, 0].astype(np.int32) << 16 | palette[:, 1].astype(np.int32) << 8 | palette[:, 2]
    m = np.isin(key, pk[list(SPECIAL)])
    return m if m.any() else None


def keep_special(alb, orig, special, wrap=False):
    """The special texels' original colors over the result (upscaled smoothly, a soft edge)."""
    h4, w4 = alb.shape[:2]
    o = np.asarray(Image.fromarray(orig).resize((w4, h4), Image.LANCZOS)).astype(np.float64)
    m = np.asarray(Image.fromarray(special.astype(np.uint8) * 255).resize((w4, h4), Image.BILINEAR)).astype(np.float64) / 255
    m = np.clip(_smooth(m, max(1, w4 // max(1, orig.shape[1]) // 2), wrap) * 1.5, 0, 1)[..., None]
    return np.clip(np.round(alb * (1 - m) + o * m), 0, 255).astype(np.uint8)


# -- the numbers ---------------------------------------------------------------

def seam_score(rgb):
    """The step across the wrap (last column to first, last row to first) against the mean
    step between neighbours inside: about 1 when the texture tiles without a seam."""
    a = _lin(rgb) @ LUM
    inner = (np.abs(np.diff(a, axis=1)).mean() + np.abs(np.diff(a, axis=0)).mean()) / 2
    edge = (np.abs(a[:, 0] - a[:, -1]).mean() + np.abs(a[0, :] - a[-1, :]).mean()) / 2
    return float(edge / inner) if inner > 0 else 0.0


def color_drift(orig, out):
    """The broad colors' change: both averaged in blocks of an eighth of the original's short
    side, in Oklab, the mean distance of their a, b (hue and chroma; the brightness is matched apart)."""
    f0 = max(1, min(orig.shape[:2]) // 8)
    k = out.shape[0] // orig.shape[0]
    a = _oklab(_box(_lin(orig), f0))
    b = _oklab(_box(_lin(out), f0 * k))
    h, w = min(a.shape[0], b.shape[0]), min(a.shape[1], b.shape[1])
    return float(np.sqrt(((a[:h, :w, 1:] - b[:h, :w, 1:]) ** 2).sum(axis=2)).mean())


def layout_kept(orig, out):
    """The original's shapes in the result: the correlation of their luminance detail, the
    result scaled down to the original's size."""
    k = out.shape[0] // orig.shape[0]
    a = _lin(orig) @ LUM
    b = _box(_lin(out) @ LUM, k)
    h, w = min(a.shape[0], b.shape[0]), min(a.shape[1], b.shape[1])
    r = max(2, min(h, w) // 8)
    return _corr(_highpass(a[:h, :w], r), _highpass(b[:h, :w], r))


def dark_fraction(rgb, q=0.2, mask=None):
    """The mean linear luminance of the darkest 20 % (M43's measure, on the texture)."""
    y = _lin(rgb) @ LUM
    y = np.sort((y[mask] if mask is not None else y).ravel())
    return float(y[: max(1, int(len(y) * q))].mean())


# -- the driver ----------------------------------------------------------------

REDRAW_VERSION = '9.1.2'    # bump when a stage's output changes: a run then redoes what an older one made
SPECIAL_MAX = 0.05          # a texture mostly in the special row (rtex465, the water) is redrawn, not kept


def anim_base(stem):
    """An animated texture's frames (+0rune1 ... +4rune1, +arune1) share one seed, a variant's
    qualifier (~crc, other pixels per frame) left out too: with different noise their stone
    would flicker from frame to frame."""
    stem = stem.split('~')[0]
    return stem[2:] if len(stem) > 2 and stem[0] == '+' and stem[1].isalnum() else stem


def load_pilot(path):
    with open(path, newline='', encoding='utf-8') as f:
        lines = [ln for ln in f if ln.strip() and not ln.startswith('# ')]
    rows = {}
    for r in csv.DictReader(lines):
        if None in r or None in r.values():     # a field too many (a comma outside quotes) or too few
            raise SystemExit(f"{path}: the row of {r.get('stem')} hasn't 5 fields: quote a description with commas")
        rows[r['stem']] = r
    return rows


def _key(data, fields):
    return hashlib.sha1(data + json.dumps(fields, sort_keys=True).encode()).hexdigest()


def resolve_row(entry, row, classes):
    """A pilot row through the manifest's resolution: an unknown class or override key is refused
    as texpack.py run refuses it."""
    r = {'pattern': entry.stem, 'class': row['class'], 'description': row['description'],
         'overrides': row.get('overrides') or '', 'source': 'human'}
    return manifest.resolve(entry.stem, entry.kind, [r], classes)


class Redraw:
    def __init__(self, export_dir, out_dir, classes, comfy, gen, seed=9100, front=FRONT, palette=None):
        self.export_dir, self.out, self.classes, self.comfy, self.gen, self.seed, self.front, self.palette = \
            export_dir, out_dir, classes, comfy, gen, seed, front, palette
        self.maps = None
        self.started = False
        self.measure_path = os.path.join(out_dir, 'measure.json')
        self.measures = {}
        if os.path.exists(self.measure_path):
            with open(self.measure_path) as f:
                self.measures = json.load(f)

    def _write(self, rel, data):
        path = os.path.join(self.out, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, 'wb' if isinstance(data, bytes) else 'w', **({} if isinstance(data, bytes) else {'newline': '\n'})) as f:
            f.write(data)

    def _edit(self, rgb, prompt, seed, w, h, upscaler):
        if not self.started:            # only when a texture needs the model (--reuse may never)
            self.comfy.start()
            self.started = True
        name = self.comfy.upload('redraw_in.png', pipeline.png_bytes(rgb))
        t0 = time.time()
        out = self.comfy.run(workflow(self.gen, name, prompt, seed, w, h, upscaler))
        return np.asarray(Image.open(io.BytesIO(out)).convert('RGB')).copy(), time.time() - t0

    def process(self, entry, row, force=False, reuse=False):
        """Redraws one texture unless the pack's files were made from the same model image and
        stages: two keys, the model image's (the source, the prompt, the generator, the seed, the
        size) and the stages' (the class's values, the models, --front, the palette, this tool's
        version). reuse: the model's image from an earlier run (work\\<stem>_A.png, its key,
        generator and time kept), the stages after it redone."""
        stem, tier = entry.stem, row['tier']
        res = resolve_row(entry, row, self.classes)
        params = res.params
        src = os.path.join(self.export_dir, entry.file)
        with open(src, 'rb') as f:
            png = f.read()
        rgb, alpha = pipeline.read_png(src)
        h, w = rgb.shape[:2]
        skin = tier == 'skin'
        wrap = not skin
        m = pipeline.margin_for(w, h)
        ph, pw = h + 2 * m + (h + 2 * m) % 2, w + 2 * m + (w + 2 * m) % 2      # pipeline.pad(..., even=True)'s
        gw, gh = gen_size(pw, ph)
        seed = pipeline.seed_for(anim_base(stem), self.seed)
        prompt = PROMPT_A[tier].format(desc=row['description'], noun=params['noun'])
        a_key = _key(png, {'prompt': prompt, 'gen': GENS[self.gen], 'seed': seed, 'size': [gw, gh],
                           'upscaler': self.classes['models']['upscaler'], 'wrap': wrap})
        stage_key = _key(b'', {'v': REDRAW_VERSION, 'tier': tier, 'params': params, 'models': self.classes['models'],
                               'front': self.front, 'palette': self.palette is not None})
        maps = params['maps'] and tier not in AS_DRAWN
        mat = pipeline.mat_text(res)
        want = ['.png'] + (['_n.png', '_orm.png'] if maps else []) + (['.mat'] if mat else [])
        have = all(os.path.exists(os.path.join(self.out, 'textures', stem + s)) for s in want)
        old = self.measures.get(stem, {})
        if not force and not reuse and have and old.get('a_key') == a_key and old.get('stage_key') == stage_key:
            return dict(old, skipped=True)
        bled = pipeline.bleed(rgb, alpha, m)        # what the model saw: no black under the holes
        padded = pipeline.pad(bled, m, wrap, even=True)
        assert padded.shape[:2] == (ph, pw)
        apath = os.path.join(self.out, 'work', stem + '_A.png')
        if os.path.exists(apath) and (reuse or old.get('a_key') == a_key):
            # an earlier run's image (--reuse, or only the stages changed): its key, generator and
            # time are what it was made from, not what the row says now
            a_img = np.asarray(Image.open(apath).convert('RGB'))
            gen, seconds, a_key = old.get('gen', self.gen), old.get('seconds'), old.get('a_key')
        else:
            a_img, ta = self._edit(padded, prompt, seed, gw, gh, self.classes['models']['upscaler'])
            self._write(f'work/{stem}_A.png', pipeline.png_bytes(a_img))
            gen, seconds = self.gen, round(ta, 1)
        big = np.asarray(Image.fromarray(a_img).resize((pw * 4, ph * 4), Image.LANCZOS))
        h4, w4, m4 = 4 * h, 4 * w, 4 * m
        lit = blend_seams(big, m4, h4, w4) if wrap else big[m4:m4 + h4, m4:m4 + w4].copy()
        a4 = pipeline.upscale_alpha(alpha, w4, h4, entry.alpha) if alpha is not None else None
        mask = (a4 > 0) if a4 is not None else None
        info = {'a_key': a_key, 'stage_key': stage_key, 'tier': tier, 'class': row['class'], 'gen': gen, 'size': [w4, h4], 'gen_size': [gw, gh],
                'seconds': seconds}
        r = max(4, min(h4, w4) // 12)
        for suf in ('_n.png', '_orm.png', '.mat'):      # what an earlier tier or class made and this one doesn't
            p = os.path.join(self.out, 'textures', stem + suf)
            if suf not in want and os.path.exists(p):
                os.remove(p)
        alb = lit
        if maps:
            if self.maps is None:
                self.maps = pipeline.Maps(os.path.join(self.comfy.home, 'models', 'pbrify'), self.classes['models'])
            normal = pipeline.finish_normal(self.maps.run('normal', lit, wrap), True)   # PBRify's are DirectX (M38)
            rough = self.maps.run('roughness', lit, wrap)
            self._write(f'textures/{stem}_n.png', pipeline.png_bytes(normal))
            self._write(f'textures/{stem}_orm.png', pipeline.png_bytes(
                pipeline.make_orm(pipeline.finish_roughness(rough, params['rough_min'], params['rough_max']), params['metallic'])))
            _, info['light_dir_before'], info['light_all_before'] = shading_fit(lit, normal, r, mask, wrap)
            alb, c = take_light_out(lit, normal, r, self.front, mask, wrap)
            info['shade_fit'] = [round(float(v), 2) for v in c]
            _, info['light_dir_after'], info['light_all_after'] = shading_fit(alb, normal, r, mask, wrap)
        target = pipeline.mean_linear_luminance(rgb, alpha) * params['albedo_ratio']
        alb, gain = pipeline.match_luminance(alb, a4, target, params['contrast'])
        # the colors after the brightness: darkening scales Oklab's a and b with the lightness, so a
        # guard before it would hand the original's chroma to a lighter color (rtex324's robe lost a
        # third of it); every tier: lava's average color lights its room. The guard moves the
        # luminance a little (a and b at a fixed L): a plain gain puts it back on the target.
        info['drift_before'] = color_drift(bled, alb)
        alb = color_guard(bled, alb, *COLOR_GUARD.get(tier, (1.0, 1 / 24)), wrap, mask)
        alb, _ = pipeline.match_luminance(alb, a4, target, 1.0)
        special = special_mask(rgb, self.palette) if tier not in AS_DRAWN else None
        if special is not None:
            if special.mean() <= SPECIAL_MAX:
                alb = keep_special(alb, bled, special, wrap)
            info['special_texels'] = int(special.sum())
        self._write(f'textures/{stem}.png', pipeline.png_bytes(np.dstack([alb, a4]) if a4 is not None else alb))
        if mat:
            self._write(f'textures/{stem}.mat', mat.replace('(story 5.8)', f'(story 9.1, {gen}, {tier})'))
        info.update({
            'gain': gain,
            'lum_ratio': pipeline.mean_linear_luminance(alb, a4) / max(pipeline.mean_linear_luminance(rgb, alpha), 1e-9),
            'seam_orig': seam_score(rgb) if wrap else None,
            'seam': seam_score(alb) if wrap else None,
            'drift': color_drift(bled, alb),
            'layout': layout_kept(bled, alb),
            'dark20_ratio': dark_fraction(alb, mask=mask) / max(dark_fraction(np.asarray(Image.fromarray(rgb).resize((w4, h4), Image.NEAREST)), mask=mask), 1e-6),
        })
        info = {k: (round(v, 4) if isinstance(v, float) else v) for k, v in info.items()}
        self.measures[stem] = info
        with open(self.measure_path, 'w') as f:
            json.dump(self.measures, f, indent=1, sort_keys=True)
        return info


def cmd_run(a):
    from comfy import Comfy
    import texpack
    ex = a.export or texpack.default_export()
    entries = pipeline.load_export(ex)
    rows = load_pilot(a.pilot)
    stems = a.stems.split(',') if a.stems else list(rows)
    missing = [s for s in stems if s not in entries or s not in rows]
    if missing:
        raise SystemExit(f"not in the export ({ex}; the pilot needs both games': r_exporttextures with -portals) "
                         f"or the pilot file: {', '.join(missing)}")
    classes = manifest.load_classes(os.path.join(HERE, 'classes.toml'))
    data = a.data or os.environ.get('HEXENLICHT_DATA') or os.path.join(os.path.dirname(texpack.REPO), 'Hexenlicht-data')
    palette = load_palette(data)
    if palette is None:
        print(f"redraw: no data1\\pak0.pak in {data}: the palette's special colors are not kept (--data)")
    os.makedirs(a.out, exist_ok=True)
    with Comfy(texpack.texpack_home()) as comfy:      # started by the first texture that needs the model
        r = Redraw(ex, a.out, classes, comfy, a.gen, a.seed, a.front, palette)
        for i, s in enumerate(stems, 1):
            info = r.process(entries[s], rows[s], a.force, a.reuse)
            print(f"[{i}/{len(stems)}] {s}: " + ' '.join(f"{k}={v}" for k, v in info.items() if k not in ('gen', 'class', 'a_key', 'stage_key')), flush=True)
    return 0


# -- the sheet -----------------------------------------------------------------

def cmd_sheet(a):
    """One PNG per texture: the original (4x, nearest), the 5.8 pack (--base), and per pack its
    model image (padding included), the albedo, the normal and the albedo tiled 2x2, the numbers
    under the albedo. Albedos are shown as they are: as dark as Hexen II's (--lift brightens the
    view only)."""
    from PIL import ImageDraw
    import texpack
    ex = a.export or texpack.default_export()
    entries = pipeline.load_export(ex)
    rows = load_pilot(a.pilot)
    packs = [p.split('=', 1) for p in a.packs.split(',')]
    os.makedirs(a.out, exist_ok=True)
    H = a.height

    def show(img, nearest=False, lift=False):
        k = H / img.height
        img = img.resize((max(1, int(img.width * k)), H), Image.NEAREST if nearest else Image.LANCZOS)
        if lift and a.lift != 1.0:
            img = Image.fromarray(_enc(_lin(np.asarray(img)) * a.lift))
        return img
    for stem in (a.stems.split(',') if a.stems else rows):
        e = entries[stem]
        cols = [('original', show(Image.open(os.path.join(ex, e.file)).convert('RGB'), True, True), '')]
        if a.base and os.path.exists(os.path.join(a.base, 'textures', stem + '.png')):
            cols.append(('5.8 pack', show(Image.open(os.path.join(a.base, 'textures', stem + '.png')).convert('RGB'), lift=True), ''))
        for name, path in packs:
            mp = os.path.join(path, 'measure.json')
            meas = {}
            if os.path.exists(mp):
                with open(mp) as f:
                    meas = json.load(f).get(stem, {})
            note = ' '.join(f"{k.replace('_ratio', '')}={meas[k]}" for k in ('light_dir_after', 'seam', 'drift', 'layout', 'dark20_ratio') if meas.get(k) is not None)
            for p, label, lift in ((os.path.join(path, 'work', stem + '_A.png'), 'model', False),
                                   (os.path.join(path, 'textures', stem + '.png'), 'albedo', True),
                                   (os.path.join(path, 'textures', stem + '_n.png'), 'normal', False)):
                if os.path.exists(p):
                    cols.append((f'{name} {label}', show(Image.open(p).convert('RGB'), lift=lift), note if label == 'albedo' else ''))
            p = os.path.join(path, 'textures', stem + '.png')
            if os.path.exists(p) and rows[stem]['tier'] != 'skin':
                im = Image.open(p).convert('RGB')
                t = Image.new('RGB', (im.width * 2, im.height * 2))
                for dx in (0, 1):
                    for dy in (0, 1):
                        t.paste(im, (dx * im.width, dy * im.height))
                cols.append((f'{name} tiled', show(t, lift=True), ''))
        W = sum(c[1].width + 6 for c in cols) + 6
        img = Image.new('RGB', (W, H + 52), (32, 32, 32))
        d = ImageDraw.Draw(img)
        d.text((6, 4), f"{stem}  [{rows[stem]['tier']}, {rows[stem]['class']}]  {rows[stem]['description'][:150]}"
               + (f"   (albedos and originals shown x{a.lift:g} brighter)" if a.lift != 1.0 else ''), fill=(255, 255, 0))
        x = 6
        for label, im, note in cols:
            d.text((x, 18), label, fill=(200, 200, 200))
            img.paste(im, (x, 32))
            if note:
                d.text((x, 34 + H), note, fill=(150, 220, 150))
            x += im.width + 6
        img.save(os.path.join(a.out, stem.replace('/', '_').replace('#', '_') + '.png'))
    print(f"redraw sheet: {a.out}")
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    sub = ap.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('run', help='redraw the pilot\'s textures into a pack folder')
    p.add_argument('--pilot', default=os.path.join(HERE, 'pilot.csv'))
    p.add_argument('--gen', choices=sorted(GENS), default='klein')
    p.add_argument('--out', required=True)
    p.add_argument('--stems')
    p.add_argument('--export')
    p.add_argument('--seed', type=int, default=9100)
    p.add_argument('--front', type=float, default=FRONT, help='how much of the frontal (cavity) shading stays (0..1)')
    p.add_argument('--data', help='the game data folder (for the palette; default: as texpack.py\'s)')
    p.add_argument('--force', action='store_true')
    p.add_argument('--reuse', action='store_true', help='keep the model images of an earlier run, redo the stages after them')
    p.set_defaults(fn=cmd_run)
    p = sub.add_parser('sheet', help='a comparison PNG per texture')
    p.add_argument('--pilot', default=os.path.join(HERE, 'pilot.csv'))
    p.add_argument('--packs', required=True, help='name=folder,name=folder')
    p.add_argument('--base', help='the 5.8 pack to show beside them')
    p.add_argument('--out', required=True)
    p.add_argument('--stems')
    p.add_argument('--export')
    p.add_argument('--height', type=int, default=320)
    p.add_argument('--lift', type=float, default=1.0, help='brighten the albedos and originals on the sheet (linear factor)')
    p.set_defaults(fn=cmd_sheet)
    a = ap.parse_args(argv)
    return a.fn(a)


if __name__ == '__main__':
    sys.exit(main())
