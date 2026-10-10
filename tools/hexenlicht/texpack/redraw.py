"""texpack's redraw (story 9.1, E9's pilot; in `texpack.py run` since 9.4; docs/hexenlicht/PLAN.md
section 8): a texture's surface redrawn by an image-editing model, not only upscaled.

One pass through ComfyUI's core nodes with FLUX.2 klein 4B (DECISIONS M50): the 4x upscale of the
padded original is redrawn as a detailed material under a soft frontal light (`work\\<stem>_A.png`);
how far it may depart from the original is the texture's tier (9.3's labels); a family's layout
member gets its hero's drawing as a second image, a reference for the material (groups.py). Then,
in this process:
  broad    the drawing's soft falloff of light (bands on a tiled wall) out: its broad lightness
           replaced by the original's, on the whole padded drawing (light_guard);
  seams    the redrawn padding is blended into the tile's opposite edges;
  maps     normal and roughness from the redrawn image (PBRify, as 5.8), so the relief is
           where the drawn light shows it;
  light    the part of the image's brightness that the normal map's directions explain is
           divided out: the directional part (light from a side) fully, the frontal part
           (faces towards the viewer brighter, slopes and joints darker: a cavity term) by
           1 - `front`, so `front` of it stays (DECISIONS M47: a second edit "to an albedo"
           flattened the material away);
  bright   5.8's brightness match (M37, M43) per material (materials.py: 9.3's regions);
  colors   the original's broad colors put back (Oklab's a and b blurred at a tenth to a
           24th of the texture, by tier): the models draw real-world colors, Hexen II's
           palette is the theme;
  orm      roughness and metallic per material, roughness 1 in near-black cavities, the
           occlusion from the cavity term (materials.py; 9.7 uses it).
Liquids and lava keep their light as drawn (nothing to take out: the engine warps a liquid, lava
emits), but get the colors back too. Copies are drawn once, a set's members with one seed and
their shared texels made identical, their own rebalanced (groups.py). The stages write `work\\`, a last step
`textures\\` (MATERIALS.md's names); `.texpack\\redraw.json` keeps what each was made from and
9.1's numbers: seams, light left in, color drift, layout kept, brightness.

  python redraw.py sheet --packs klein=<pack>,other=<pack> --base <5.8 pack> --out <dir>
"""
import argparse
import hashlib
import io
import json
import os
import shutil
import sys
import time
import types

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import numpy as np  # noqa: E402
from PIL import Image  # noqa: E402

import groups  # noqa: E402
import manifest  # noqa: E402
import materials  # noqa: E402
import pipeline  # noqa: E402
from imaging import _blur, _box, _corr, _enc, _from_oklab, _highpass, _lin, _oklab, _smooth  # noqa: E402,F401
from pipeline import LUM  # noqa: E402

GEN_PIXELS = 1024 * 1024        # the model's native size: the padded image is scaled to about this
REF_PIXELS = 512 * 512          # the hero's drawing as a reference: no slower than without (9.4's spike)
GEN = {'steps': 4, 'cfg': 1.0}  # klein, distilled

TIERS = ('reimagine', 'layout', 'faithful', 'glass', 'liquid', 'lava')     # what texpack.py run redraws
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
}
PROMPT_A['glass'] = (
    "Turn this low-resolution game texture into a high-resolution texture of {desc}, seen flat against a uniform white "
    "backlight. Keep the window's shape, the tracery, the frame and every lead line exactly where they are. Replace whatever "
    "is painted behind the lead with translucent glass panes of even tints, slightly mottled, with no scene, landscape or "
    "objects visible through them. A flat orthographic view filling the whole image edge to edge, no text.")
PROMPT_A['liquid'] = PROMPT_A['faithful']
PROMPT_A['lava'] = PROMPT_A['faithful']
# the hero as image 2 (9.4's spike: the material followed it, the layout stayed image 1's)
PROMPT_REF = (" Image 2 is only a reference for the material: draw the surfaces of image 1 as the same {noun}, "
              "with the same kind of detail, grain, wear and finish as image 2, but keep image 1's own layout, shapes and colors.")
AS_DRAWN = ('liquid', 'lava')       # no light taken out (a liquid is warped, lava emits); colors put back as for all
FRONT = 0.5                         # how much of the frontal (cavity) shading stays
# the color guard: how much of the original's broad colors comes back, and at what scale (a
# fraction of the texture's short side): coarse where the shapes may move, fine where they stay
COLOR_GUARD = {'reimagine': (0.8, 1 / 10), 'layout': (1.0, 1 / 24), 'faithful': (1.0, 1 / 24),
               'glass': (1.0, 1 / 24), 'liquid': (1.0, 1 / 24), 'lava': (1.0, 1 / 24)}
SPECIAL = range(240, 255)           # the palette's saturated row (the archer's green eyes are 243-246): kept as Raven drew it
LIGHT_GUARD = 1 / 4                 # light_guard's scale: the original's broad lightness, coarser than the parts


# -- the workflow ----------------------------------------------------------------

def workflow(models, image_name, prompt, seed, w, h, upscaler=None, ref_name=None, ref_size=None):
    """The edit as ComfyUI's template for klein (image_flux2_klein_image_edit_4b_distilled), the
    image scaled to w x h by us rather than to a megapixel; a reference image (ref_name, scaled to
    ref_size) is a second ReferenceLatent on both conditionings. models: classes.toml's [redraw]."""
    wf = {'4': {'class_type': 'LoadImage', 'inputs': {'image': image_name}}}
    src = ['4', 0]
    if upscaler:
        wf['5'] = {'class_type': 'UpscaleModelLoader', 'inputs': {'model_name': upscaler}}
        wf['6'] = {'class_type': 'ImageUpscaleWithModel', 'inputs': {'upscale_model': ['5', 0], 'image': src}}
        src = ['6', 0]
    wf.update({
        '1': {'class_type': 'UNETLoader', 'inputs': {'unet_name': models['generator'], 'weight_dtype': 'default'}},
        '2': {'class_type': 'CLIPLoader', 'inputs': {'clip_name': models['text_encoder'], 'type': 'flux2', 'device': 'default'}},
        '3': {'class_type': 'VAELoader', 'inputs': {'vae_name': models['vae']}},
        '9': {'class_type': 'ImageScale', 'inputs': {'image': src, 'upscale_method': 'lanczos', 'width': w, 'height': h, 'crop': 'disabled'}},
        '10': {'class_type': 'VAEEncode', 'inputs': {'pixels': ['9', 0], 'vae': ['3', 0]}},
        '11': {'class_type': 'CLIPTextEncode', 'inputs': {'text': prompt, 'clip': ['2', 0]}},
        '12': {'class_type': 'ConditioningZeroOut', 'inputs': {'conditioning': ['11', 0]}},    # the distilled model ignores a negative
        '13': {'class_type': 'ReferenceLatent', 'inputs': {'conditioning': ['11', 0], 'latent': ['10', 0]}},
        '14': {'class_type': 'ReferenceLatent', 'inputs': {'conditioning': ['12', 0], 'latent': ['10', 0]}},
        '15': {'class_type': 'EmptyFlux2LatentImage', 'inputs': {'width': w, 'height': h, 'batch_size': 1}},
        '16': {'class_type': 'Flux2Scheduler', 'inputs': {'steps': GEN['steps'], 'width': w, 'height': h}},
        '17': {'class_type': 'KSamplerSelect', 'inputs': {'sampler_name': 'euler'}},
        '18': {'class_type': 'CFGGuider', 'inputs': {'model': ['1', 0], 'positive': ['13', 0], 'negative': ['14', 0], 'cfg': GEN['cfg']}},
        '19': {'class_type': 'RandomNoise', 'inputs': {'noise_seed': seed}},
        '20': {'class_type': 'SamplerCustomAdvanced', 'inputs': {'noise': ['19', 0], 'guider': ['18', 0], 'sampler': ['17', 0],
                                                                 'sigmas': ['16', 0], 'latent_image': ['15', 0]}},
        '21': {'class_type': 'VAEDecode', 'inputs': {'samples': ['20', 0], 'vae': ['3', 0]}},
        '22': {'class_type': 'PreviewImage', 'inputs': {'images': ['21', 0]}},
    })
    if ref_name:
        rw, rh = ref_size
        wf['40'] = {'class_type': 'LoadImage', 'inputs': {'image': ref_name}}
        wf['41'] = {'class_type': 'ImageScale', 'inputs': {'image': ['40', 0], 'upscale_method': 'lanczos', 'width': rw, 'height': rh, 'crop': 'disabled'}}
        wf['42'] = {'class_type': 'VAEEncode', 'inputs': {'pixels': ['41', 0], 'vae': ['3', 0]}}
        wf['43'] = {'class_type': 'ReferenceLatent', 'inputs': {'conditioning': ['13', 0], 'latent': ['42', 0]}}
        wf['44'] = {'class_type': 'ReferenceLatent', 'inputs': {'conditioning': ['14', 0], 'latent': ['42', 0]}}
        wf['18']['inputs']['positive'] = ['43', 0]
        wf['18']['inputs']['negative'] = ['44', 0]
    return wf


def gen_size(pw, ph, mult=16, pixels=GEN_PIXELS):
    s = (pixels / (pw * ph)) ** 0.5
    return max(mult, int(round(pw * s / mult)) * mult), max(mult, int(round(ph * s / mult)) * mult)


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


def light_guard(orig, rgb, frac=None, mask=None):
    """Takes out the light take_light_out can't see: a soft gradient across the whole drawing (the
    model's frontal light falling off; 9.4's oak planks came out a third darker at one end), which
    the normal map doesn't explain, so a tiled wall bands. The result's log luminance blurred at
    frac of the texture's short side is replaced by the original's (upscaled smoothly), its mean
    kept: Raven's broad light and dark pattern, at a scale coarser than the parts. Hue kept; the
    brightness match follows. The blur mirrors the edges even where the texture tiles: the
    falloff doesn't wrap round, and a wrapped blur averages the dark end with the light one right
    where the band is (a synthetic falloff kept 0.75 of its step that way)."""
    frac = LIGHT_GUARD if frac is None else frac
    h, w = rgb.shape[:2]
    o = np.asarray(Image.fromarray(orig).resize((w, h), Image.BICUBIC))
    r = max(2, int(min(h, w) * frac))
    lin = _lin(rgb)
    lo = np.log(np.maximum(_lin(o) @ LUM, 1e-4))
    ly = np.log(np.maximum(lin @ LUM, 1e-4))
    d = _smooth(lo, r, False) - _smooth(ly, r, False)
    d -= d[mask].mean() if mask is not None and mask.any() else d.mean()
    res = _enc(lin * np.exp(d)[..., None])
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


def broad_step(rgb):
    """A band when tiled that seam_score's texel steps don't see: the mean luminance of the first
    eighth of the rows (columns) against the last eighth's, the larger, against the mean (9.4)."""
    y = _lin(rgb) @ LUM
    kh, kw = max(1, y.shape[0] // 8), max(1, y.shape[1] // 8)
    mu = float(y.mean())
    return max(abs(y[:kh].mean() - y[-kh:].mean()), abs(y[:, :kw].mean() - y[:, -kw:].mean())) / mu if mu > 0 else 0.0


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

REDRAW_VERSION = '9.4.4'    # bump when a stage's output changes: a run then redoes what an older one made
FINISH_VERSION = '9.4.3'    # bump when the last step's changes (unify): a run then rewrites textures only
SPECIAL_MAX = 0.05          # a texture mostly in the special row (rtex465, the water) is redrawn, not kept
MAPS = ('.png', '_n.png', '_orm.png')
STALE = ('_n.png', '_orm.png', '_r.png', '_m.png', '_e.png', '.mat')    # what an earlier run or class may have left


def anim_base(stem):
    """An animated texture's frames (+0rune1 ... +4rune1, +arune1) share one seed, a variant's
    qualifier (~crc, other pixels per frame) left out too: with different noise their stone
    would flicker from frame to frame."""
    stem = stem.split('~')[0]
    return stem[2:] if len(stem) > 2 and stem[0] == '+' and stem[1].isalnum() else stem


def _key(data, fields):
    return hashlib.sha1(data + json.dumps(fields, sort_keys=True).encode()).hexdigest()


def _rgba(path):
    rgb, a = pipeline.read_png(path)
    return np.dstack([rgb, a]) if a is not None else rgb


def export_plan(ex, entries, rows, classes, census, leads):
    """Every redrawn texture of the export (world and liquid textures of TIERS): their resolved
    rows, their originals' pixels and the plan of their copies, sets and heroes (groups.py)."""
    res = {}
    for s, e in entries.items():
        if e.kind in ('world', 'liquid'):
            r = manifest.resolve(s, e.kind, rows, classes)
            if r.tier in TIERS:
                res[s] = r
    pix = {s: _rgba(os.path.join(ex, entries[s].file)) for s in res}
    return res, pix, groups.Plan(sorted(res), res, pix, census, leads)


class Redraw:
    """Redraws a selection into a pack: plans the copies, sets and heroes over every redrawn
    texture of the export (groups.py), draws in that order into work\\, then writes textures\\."""

    def __init__(self, export_dir, out_dir, classes, comfy, entries, rows, census, leads, palette,
                 seed=9100, front=FRONT, restart_every=80, log=print):
        self.ex, self.out, self.classes, self.comfy = export_dir, out_dir, classes, comfy
        self.entries, self.palette, self.seed, self.front, self.log = entries, palette, seed, front, log
        self.restart_every, self.edits, self.started, self.maps = restart_every, 0, False, None
        self.mtab = materials.table(classes)
        if not census:
            # the census orders a set (the most area first: its seed) and picks a copy's source
            log("redraw: no census.csv: sets and copies ordered by name, their seeds differ from a run with it")
        self.res, self.orig, self.plan = export_plan(export_dir, entries, rows, classes, census, leads)
        self.state_path = os.path.join(out_dir, '.texpack', 'redraw.json')
        os.makedirs(os.path.dirname(self.state_path), exist_ok=True)
        self.state = {}
        if os.path.exists(self.state_path):
            with open(self.state_path) as f:
                self.state = json.load(f)

    def _save(self):
        tmp = self.state_path + '.tmp'      # written whole, then put in place: a stopped run leaves the last state
        with open(tmp, 'w') as f:
            json.dump(self.state, f, indent=1, sort_keys=True)
        os.replace(tmp, self.state_path)

    def _path(self, where, stem, suffix):
        return os.path.join(self.out, where, stem + suffix)

    def _write(self, where, stem, suffix, data):
        path = self._path(where, stem, suffix)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, 'wb' if isinstance(data, bytes) else 'w', **({} if isinstance(data, bytes) else {'newline': '\n'})) as f:
            f.write(data)

    @staticmethod
    def _copy(src, dst):
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copyfile(src, dst)

    def _edit(self, rgb, prompt, seed, w, h, ref=None):
        if not self.started:            # only when a texture needs the model (--reuse may never)
            self.comfy.start()
            self.started = True
        elif self.restart_every and self.edits and self.edits % self.restart_every == 0:
            # ComfyUI's caches grow over a long run until a texture takes four times as long (5.8)
            self.comfy.stop()
            self.comfy.start(log=lambda m: None)
        name = self.comfy.upload('redraw_in.png', pipeline.png_bytes(rgb))
        ref_name = ref_size = None
        if ref is not None:
            ref_name = self.comfy.upload('redraw_ref.png', pipeline.png_bytes(ref))
            ref_size = gen_size(ref.shape[1], ref.shape[0], pixels=REF_PIXELS)
        t0 = time.time()
        out = self.comfy.run(workflow(self.classes['redraw'], name, prompt, seed, w, h,
                                      self.classes['models']['upscaler'], ref_name, ref_size))
        self.edits += 1
        return np.asarray(Image.open(io.BytesIO(out)).convert('RGB')).copy(), time.time() - t0

    def _geometry(self, stem):
        e = self.entries[stem]
        m = pipeline.margin_for(e.w, e.h)
        ph, pw = e.h + 2 * m + (e.h + 2 * m) % 2, e.w + 2 * m + (e.w + 2 * m) % 2      # pipeline.pad(..., even=True)'s
        return m, ph, pw

    def _hero_tile(self, hero):
        """The hero's drawing, its tile without the padding: the reference image."""
        a = np.asarray(Image.open(self._path('work', hero, '_A.png')).convert('RGB'))
        e = self.entries[hero]
        m, ph, pw = self._geometry(hero)
        H, W = a.shape[:2]
        return a[round(m * H / ph):round((m + e.h) * H / ph), round(m * W / pw):round((m + e.w) * W / pw)].copy()

    def seed_of(self, stem):
        """A set's seed: the first `seed=` override among its members (in the set's order), else
        from its first member's name."""
        g = self.plan.group[stem]
        for s in g:
            if 'seed' in self.res[s].params:
                return int(self.res[s].params['seed'])
        return pipeline.seed_for(anim_base(g[0]), self.seed)

    def draw(self, stem, force=False, reuse=False):
        """Draws one texture into work\\ unless it was made from the same model image and stages:
        two keys, the model image's (the source, the prompt, the seed, the size, the hero's
        drawing) and the stages' (the class's and the materials' values, the models, --front, this
        tool's version). reuse: the model's image from an earlier run (work\\<stem>_A.png, its
        key and time kept), the stages after it redone."""
        e, res = self.entries[stem], self.res[stem]
        p, tier = res.params, res.tier
        src = os.path.join(self.ex, e.file)
        with open(src, 'rb') as f:
            png = f.read()
        rgb, alpha = pipeline.read_png(src)
        h, w = rgb.shape[:2]
        wrap = True                     # world and liquid textures tile
        m, ph, pw = self._geometry(stem)
        gw, gh = gen_size(pw, ph)
        seed = self.seed_of(stem)
        hero = self.plan.hero.get(stem)
        prompt = PROMPT_A[tier].format(desc=res.description or p['noun'], noun=p['noun'])
        ref, ref_hash = None, None
        if hero:
            prompt += PROMPT_REF.format(noun=p['noun'])
            with open(self._path('work', hero, '_A.png'), 'rb') as f:
                ref_hash = hashlib.sha1(f.read()).hexdigest()
        models = self.classes['models']
        a_key = _key(png, {'prompt': prompt, 'gen': [self.classes['redraw'][k] for k in ('generator', 'text_encoder', 'vae')] + [GEN],
                           'seed': seed, 'size': [gw, gh], 'upscaler': models['upscaler'], 'wrap': wrap, 'ref': ref_hash})
        idx, names, values = materials.texel_materials(rgb, alpha, self.palette, res.regions, p, self.mtab)
        # which texel is which material, not only the materials' names: moving gold from one ramp
        # to another must redo the stages (the code review's)
        stage_key = _key(b'', {'v': REDRAW_VERSION, 'tier': tier, 'params': p, 'maps': [models['normal'], models['roughness']],
                               'front': self.front, 'palette': self.palette is not None, 'materials': [names, values],
                               'texels': hashlib.sha1(idx.tobytes()).hexdigest(), 'alpha': e.alpha, 'cavity': materials.CAVITY})
        maps = p['maps'] and tier not in AS_DRAWN
        want = list(MAPS if maps else MAPS[:1])
        old = self.state.get(stem, {})
        # the model's image too: a hero's is its members' reference
        have = all(os.path.exists(self._path('work', stem, s)) for s in want + ['_A.png'])
        if not force and not reuse and have and old.get('a_key') == a_key and old.get('stage_key') == stage_key:
            return dict(old, skipped=True)
        bled = pipeline.bleed(rgb, alpha, m)        # what the model saw: no black under the holes
        padded = pipeline.pad(bled, m, wrap, even=True)
        assert padded.shape[:2] == (ph, pw)
        apath = self._path('work', stem, '_A.png')
        if os.path.exists(apath) and (reuse or (old.get('a_key') == a_key and not force)):
            # an earlier run's image (--reuse, or only the stages changed): its key and time are
            # what it was made from, not what the row says now
            a_img = np.asarray(Image.open(apath).convert('RGB'))
            seconds, a_key = old.get('seconds'), old.get('a_key', a_key)
        else:
            if hero:
                ref = self._hero_tile(hero)
            a_img, ta = self._edit(padded, prompt, seed, gw, gh, ref)
            self._write('work', stem, '_A.png', pipeline.png_bytes(a_img))
            seconds = round(ta, 1)
        big = np.asarray(Image.fromarray(a_img).resize((pw * 4, ph * 4), Image.LANCZOS))
        h4, w4, m4 = 4 * h, 4 * w, 4 * m
        broad_drawn = broad_step(blend_seams(big, m4, h4, w4))
        # the broad light out on the whole padded drawing, where the falloff is smooth and doesn't
        # wrap; the seams are blended after it (on the tile alone it doubled the seams, M68)
        big = light_guard(padded, big)
        lit = blend_seams(big, m4, h4, w4)
        a4 = pipeline.upscale_alpha(alpha, w4, h4, e.alpha) if alpha is not None else None
        mask = (a4 > 0) if a4 is not None else None
        W = materials.weights(idx, len(values), 4, wrap, guide=(lit.astype(np.float64) @ LUM) / 255.0)
        info = {'a_key': a_key, 'stage_key': stage_key, 'tier': tier, 'class': res.cls, 'size': [w4, h4], 'gen_size': [gw, gh],
                'seconds': seconds, 'seed': seed, 'hero': hero, 'materials': [n for i, n in enumerate(names) if W[i].any()]}
        r = max(4, min(h4, w4) // 12)
        alb, normal, rough_raw, cz = lit, None, None, 0.0
        if maps:
            if self.maps is None:
                self.maps = pipeline.Maps(os.path.join(self.comfy.home, 'models', 'pbrify'), models)
            normal = pipeline.finish_normal(self.maps.run('normal', lit, wrap), True)   # PBRify's are DirectX (M38)
            rough_raw = self.maps.run('roughness', lit, wrap)
            _, info['light_dir_before'], info['light_all_before'] = shading_fit(lit, normal, r, mask, wrap)
            alb, c = take_light_out(lit, normal, r, self.front, mask, wrap)
            cz = float(c[2])
            info['shade_fit'] = [round(float(v), 2) for v in c]
            _, info['light_dir_after'], info['light_all_after'] = shading_fit(alb, normal, r, mask, wrap)
        info['broad_drawn'] = broad_drawn
        target, whole = materials.targets(rgb, alpha, idx, values)
        alb, gains = materials.match(alb, a4, W, values, target, whole, contrast=True)
        info['gains'] = [round(g, 3) for i, g in enumerate(gains) if W[i].any()]
        info['target'] = whole          # the albedo's mean linear luminance wanted (finish's _rebalance)
        # the colors after the brightness: darkening scales Oklab's a and b with the lightness, so a
        # guard before it would hand the original's chroma to a lighter color (rtex324's robe lost a
        # third of it); every tier: lava's average color lights its room. The guard moves the
        # luminance a little (a and b at a fixed L): the materials' gains put it back on the targets.
        info['drift_before'] = color_drift(bled, alb)
        alb = color_guard(bled, alb, *COLOR_GUARD[tier], wrap, mask)
        alb, _ = materials.match(alb, a4, W, values, target, whole, contrast=False)
        special = special_mask(rgb, self.palette) if tier not in AS_DRAWN else None
        if special is not None:
            if special.mean() <= SPECIAL_MAX:
                alb = keep_special(alb, bled, special, wrap)
            info['special_texels'] = int(special.sum())
        self._write('work', stem, '.png', pipeline.png_bytes(np.dstack([alb, a4]) if a4 is not None else alb))
        if maps:
            rough = materials.roughness(rough_raw, W, values, mask)
            surf = materials.surface(alb, W, mask) if tier in materials.CAVITY_TIERS else np.ones(rough.shape)
            rough = 1.0 - surf * (1.0 - rough)
            occ = materials.occlusion(normal, cz, surf)
            met = materials.metallic(W, values)
            self._write('work', stem, '_n.png', pipeline.png_bytes(normal))
            self._write('work', stem, '_orm.png', pipeline.png_bytes(pipeline.make_orm(rough, met, occ)))
            info['cavity_share'] = float((surf < 0.5).mean())
            info['occlusion_mean'] = float(occ.mean())
        # the .mat: a metal class with parts that aren't metal takes the dielectric specular (metals
        # ignore it, M43's 0.04 keeps the rest from a grey veil)
        pm = dict(p)
        present = [values[i] for i in range(len(values)) if W[i].any()]
        if pm['metallic'] >= 0.5 and any(v['metallic'] < 0.5 for v in present):
            pm['specular'] = self.classes['defaults']['specular']
        mat = pipeline.mat_text(types.SimpleNamespace(params=pm, cls=res.cls, description=res.description))
        info['mat'] = mat.replace('(story 5.8)', f'(story 9.4, {tier})') if mat else None
        info.update({
            'lum_ratio': pipeline.mean_linear_luminance(alb, a4) / max(pipeline.mean_linear_luminance(rgb, alpha), 1e-9),
            'seam_orig': seam_score(rgb),
            'seam': seam_score(alb),
            'broad_orig': broad_step(rgb),
            'broad': broad_step(alb),
            'drift': color_drift(bled, alb),
            'layout': layout_kept(bled, alb),
            'dark20_ratio': dark_fraction(alb, mask=mask) / max(dark_fraction(np.asarray(Image.fromarray(rgb).resize((w4, h4), Image.NEAREST)), mask=mask), 1e-6),
            'files': want,
        })
        info = {k: (round(v, 4) if isinstance(v, float) and k != 'target' else v) for k, v in info.items()}
        self.state[stem] = info
        self._save()
        return info

    def finish(self, draw, write):
        """textures\\ from work\\: a set's members unified (its order: the shared texels take the
        earlier member's), a copy its source's files; each with its .mat. Redone where what a
        texture's files come from changed (out_key) or a file is missing."""
        made = 0
        out_key = {}
        for s in draw:
            g = self.plan.group[s]
            j = g.index(s)
            out_key[s] = _key(b'', {'v': FINISH_VERSION, 'from': [[self.state[t]['a_key'], self.state[t]['stage_key']] for t in g[:j + 1]]})
        for s in write:
            r = self.plan.rep[s]
            out_key.setdefault(s, _key(b'', {'v': FINISH_VERSION, 'copy': out_key[r]}))

        def current(s):
            st, src = self.state.get(s, {}), self.state[self.plan.rep[s]]
            want = src['files'] + (['.mat'] if src.get('mat') else [])
            return (st.get('out_key') == out_key[s] and all(os.path.exists(self._path('textures', s, f)) for f in want)
                    and not any(os.path.exists(self._path('textures', s, f)) for f in STALE if f not in want))
        done = set()
        for s in draw:
            g = self.plan.group[s]
            if g[0] in done:
                continue
            done.add(g[0])
            if all(current(t) for t in g):
                continue
            files = [f for f in MAPS if all(f in self.state[t]['files'] for t in g)]
            if len(g) == 1:
                for f in files:
                    self._copy(self._path('work', s, f), self._path('textures', s, f))
            else:
                # the albedo first in each list: unify's scale() rebalances it
                files = ['.png'] + [f for f in files if f != '.png'] if '.png' in files else files
                ims = {t: [_rgba(self._path('work', t, f)) for f in files] for t in g}
                uni, factors = groups.unify(g, {t: self.orig[t] for t in g}, ims, 4, True,
                                            files.index('_n.png') if '_n.png' in files else None,
                                            (lambda j, own, comp: self._rebalance(g[j], own, comp)) if '.png' in files else None)
                for t in g:
                    self.state[t]['rebalance'] = round(factors.get(t, 1.0), 3)
                for t in g:
                    for f, im in zip(files, uni[t]):
                        if f == '.png' and im.shape[-1] == 4 and self.entries[t].alpha == 'coverage':
                            im[..., 3] = np.where(im[..., 3] >= 128, 255, 0)
                        self._write('textures', t, f, pipeline.png_bytes(im))
                    for f in self.state[t]['files']:
                        if f not in files:      # a map only some members have: as it is
                            self._copy(self._path('work', t, f), self._path('textures', t, f))
            for t in g:
                self._finish_one(t, t, out_key[t])
                made += 1
        for s in write:
            r = self.plan.rep[s]
            if r != s and not current(s):
                for f in self.state[r]['files']:
                    self._copy(self._path('textures', r, f), self._path('textures', s, f))
                self._finish_one(s, r, out_key[s])
                made += 1
        self._save()
        return made

    def _rebalance(self, t, own, comp):
        """A set member's brightness after unify (groups.unify's scale): the factor for its own
        drawing that brings the whole, its own texels and those it takes from earlier members, to
        its brightness target again. Its gain was for its whole drawing, and the texels it shares
        now carry another member's: without this the brightest frame of a rune's pulse came out
        at 0.66 of its target. own: its own texels' weight; comp: the albedo so far."""
        y = _lin(comp[..., :3]) @ LUM
        op = (comp[..., 3] > 0) if comp.shape[-1] == 4 else np.ones(y.shape, bool)
        mine = float((own * y)[op].mean())
        if mine <= 0:
            return 1.0
        return float(np.clip(1.0 + (self.state[t]['target'] - float(y[op].mean())) / mine, 0.25, 4.0))

    def _finish_one(self, s, r, key):
        """The .mat (the source's: a copy has its label) and no file an earlier run made that this one doesn't."""
        st = self.state[r]
        want = st['files'] + (['.mat'] if st.get('mat') else [])
        if st.get('mat'):
            self._write('textures', s, '.mat', st['mat'])
        for suf in STALE:
            p = self._path('textures', s, suf)
            if suf not in want and os.path.exists(p):
                os.remove(p)
        if s != r:
            self.state[s] = {'copy_of': r, 'files': st['files'], 'mat': st.get('mat')}
        self.state[s]['out_key'] = key

    def run(self, stems, force=False, reuse=False):
        draw, write = self.plan.closure(stems)
        extra = len(set(write) - set(stems))
        self.log(f"redraw: {len(stems)} selected" + (f", {extra} more for their sets, copies and heroes" if extra else '')
                 + f": {len(draw)} to draw, {len(write)} to write")
        made = skipped = 0
        for i, s in enumerate(draw, 1):
            info = self.draw(s, force, reuse)
            if info.get('skipped'):
                skipped += 1
                self.log(f"[{i}/{len(draw)}] {s}: unchanged")
                continue
            made += 1
            g = self.plan.group[s]
            self.log(f"[{i}/{len(draw)}] {s}: {info['tier']}, {info['class']}"
                     + (f", set of {len(g)} ({g[0]}'s seed)" if len(g) > 1 else '') + (f", hero {info['hero']}" if info['hero'] else '')
                     + f", {'+'.join(info['materials'])}, gains {info['gains']}, layout {info['layout']}, seam {info['seam']}"
                     + (f" (original {info['seam_orig']})" if info['seam'] > 2 else '')
                     + (f", {info['seconds']} s" if info['seconds'] is not None else ', model image reused'), )
        written = self.finish(draw, write)
        return made, skipped, written


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
    classes = manifest.load_classes(os.path.join(HERE, 'classes.toml'))
    rows = manifest.load_manifest(a.manifest)
    packs = [p.split('=', 1) for p in a.packs.split(',')]
    os.makedirs(a.out, exist_ok=True)
    H = a.height

    def show(img, nearest=False, lift=False):
        k = H / img.height
        img = img.resize((max(1, int(img.width * k)), H), Image.NEAREST if nearest else Image.LANCZOS)
        if lift and a.lift != 1.0:
            img = Image.fromarray(_enc(_lin(np.asarray(img)) * a.lift))
        return img
    for stem in a.stems.split(','):
        e = entries[stem]
        res = manifest.resolve(stem, e.kind, rows, classes)
        cols = [('original', show(Image.open(os.path.join(ex, e.file)).convert('RGB'), True, True), '')]
        if a.base and os.path.exists(os.path.join(a.base, 'textures', stem + '.png')):
            cols.append(('5.8 pack', show(Image.open(os.path.join(a.base, 'textures', stem + '.png')).convert('RGB'), lift=True), ''))
        for name, path in packs:
            meas = {}
            for mp in (os.path.join(path, '.texpack', 'redraw.json'), os.path.join(path, 'measure.json')):    # 9.4's, 9.1's
                if os.path.exists(mp):
                    with open(mp) as f:
                        meas = json.load(f).get(stem, {})
                    break
            note = ' '.join(f"{k.replace('_ratio', '')}={meas[k]}" for k in ('light_dir_after', 'seam', 'drift', 'layout', 'dark20_ratio') if meas.get(k) is not None)
            for p, label, lift in ((os.path.join(path, 'work', stem + '_A.png'), 'model', False),
                                   (os.path.join(path, 'textures', stem + '.png'), 'albedo', True),
                                   (os.path.join(path, 'textures', stem + '_n.png'), 'normal', False),
                                   (os.path.join(path, 'textures', stem + '_orm.png'), 'orm', False)):
                if os.path.exists(p):
                    cols.append((f'{name} {label}', show(Image.open(p).convert('RGB'), lift=lift), note if label == 'albedo' else ''))
            p = os.path.join(path, 'textures', stem + '.png')
            if os.path.exists(p) and e.kind != 'skin':
                im = Image.open(p).convert('RGB')
                t = Image.new('RGB', (im.width * 2, im.height * 2))
                for dx in (0, 1):
                    for dy in (0, 1):
                        t.paste(im, (dx * im.width, dy * im.height))
                cols.append((f'{name} tiled', show(t, lift=True), ''))
        W = sum(c[1].width + 6 for c in cols) + 6
        img = Image.new('RGB', (W, H + 52), (32, 32, 32))
        d = ImageDraw.Draw(img)
        d.text((6, 4), f"{stem}  [{res.tier}, {res.cls}, {res.regions}]  {res.description[:150]}"
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
    p = sub.add_parser('sheet', help='a comparison PNG per texture (texpack.py run makes the packs)')
    p.add_argument('--packs', required=True, help='name=folder,name=folder')
    p.add_argument('--stems', required=True)
    p.add_argument('--base', help='the 5.8 pack to show beside them')
    p.add_argument('--out', required=True)
    p.add_argument('--export')
    p.add_argument('--manifest', default=os.path.join(HERE, 'materials.csv'))
    p.add_argument('--height', type=int, default=320)
    p.add_argument('--lift', type=float, default=1.0, help='brighten the albedos and originals on the sheet (linear factor)')
    p.set_defaults(fn=cmd_sheet)
    a = ap.parse_args(argv)
    return a.fn(a)


if __name__ == '__main__':
    sys.exit(main())
