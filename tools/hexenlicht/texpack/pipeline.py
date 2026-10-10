"""texpack's stages (story 5.8): export index, padding, luminance match, alpha,
the maps, the .mat, the per-texture driver. The diffusion stage is ComfyUI's
(comfy.py); the maps run in this process (PBRify's 1x models through spandrel).
"""
import csv
import hashlib
import io
import json
import os
import time
import zlib

import numpy as np
from PIL import Image

TOOL_VERSION = '5.8.2'      # bump when a stage's output changes: it invalidates the state
LUM = np.array([0.2126, 0.7152, 0.0722])
GAMMA = 2.2                 # the engine's default color transfer (MATERIALS.md, R104)
LARGE = 1100                # a padded image over this many px is encoded and decoded in tiles


# -- the export ----------------------------------------------------------------

class Entry:
    def __init__(self, row):
        self.file = row['file']                         # textures/models/imp.mdl_0.png
        self.stem = row['file'][len('textures/'):-len('.png')]
        self.name = row['name']
        self.w, self.h = int(row['width']), int(row['height'])
        self.kind = row['kind']
        self.alpha = row['alpha']
        self.used_in = row['used in'].split()
        self.paks = (row.get('from') or '').split()     # data1/pak0.pak ...: the paks with this version


def load_export(export_dir):
    path = os.path.join(export_dir, 'textures.csv')
    with open(path, newline='', encoding='utf-8') as f:
        return {e.stem: e for e in (Entry(r) for r in csv.DictReader(f))}


def select(entries, stems=None, kind=None, map_=None, glob=None, listfile=None, limit=None):
    import fnmatch
    out = list(entries.values())
    want = []
    if listfile:
        with open(listfile, encoding='utf-8') as f:
            # '# ' starts a comment (a texture's name may start with #: #lava000)
            want += [s for s in (ln.split(' #')[0].strip() for ln in f if not ln.startswith('# ')) if s]
    if stems:
        want += [s for s in stems.split(',') if s]
    if want:
        missing = [s for s in want if s not in entries]
        if missing:
            raise SystemExit(f"not in the export: {', '.join(missing)}")
        out = [entries[s] for s in want]
    if kind:
        out = [e for e in out if e.kind in kind.split(',')]
    if map_:
        out = [e for e in out if map_ in e.used_in]
    if glob:
        out = [e for e in out if fnmatch.fnmatchcase(e.stem, glob)]
    out.sort(key=lambda e: e.stem)
    return out[:limit] if limit else out


def read_png(path):
    im = Image.open(path)
    if im.mode == 'RGBA':
        a = np.asarray(im)
        return a[..., :3].copy(), a[..., 3].copy()
    return np.asarray(im.convert('RGB')).copy(), None


def png_bytes(arr):
    b = io.BytesIO()
    Image.fromarray(arr).save(b, 'PNG', compress_level=6)
    return b.getvalue()


# -- stages --------------------------------------------------------------------

def mean_linear_luminance(rgb, alpha=None):
    lin = (rgb.astype(np.float64) / 255.0) ** GAMMA
    lum = lin @ LUM
    if alpha is not None:
        m = alpha > 0           # holes are alpha 0; a translucent skin's 0.33 is not one
        if m.any():
            return float(lum[m].mean())
    return float(lum.mean())


def _shift(a, dy, dx):
    """a moved by (dy, dx) with zeros shifted in: no wrap-around, a skin's edges aren't neighbours."""
    out = np.zeros_like(a)
    h, w = a.shape[:2]
    out[max(dy, 0):h + min(dy, 0), max(dx, 0):w + min(dx, 0)] = a[max(-dy, 0):h + min(-dy, 0), max(-dx, 0):w + min(-dx, 0)]
    return out


def bleed(rgb, alpha, iters):
    """Fills the colors under transparent texels from their neighbours, so the
    upscaler doesn't draw dark halos around holes."""
    if alpha is None:
        return rgb
    valid = alpha > 0
    if valid.all() or not valid.any():
        return rgb
    out = rgb.astype(np.float32)
    for _ in range(iters):
        if valid.all():
            break
        acc = np.zeros_like(out)
        cnt = np.zeros(valid.shape, np.float32)
        for dy, dx in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            v = _shift(valid, dy, dx)
            acc += _shift(out, dy, dx) * v[..., None]
            cnt += v
        fill = (~valid) & (cnt > 0)
        out[fill] = acc[fill] / cnt[fill][:, None]
        valid = valid | fill
    return np.clip(out + 0.5, 0, 255).astype(np.uint8)


def margin_for(w, h):
    return max(4, int(round(0.25 * min(w, h) / 2)) * 2)


def pad(rgb, m, wrap, even=False):
    """Pads m texels on every side; with even, one more at the bottom and right where
    the padded size would be odd (the VAE trims to multiples of 8: 4x an odd size isn't)."""
    h, w = rgb.shape[:2]
    eh, ew = ((h + 2 * m) % 2, (w + 2 * m) % 2) if even else (0, 0)
    return np.pad(rgb, ((m, m + eh), (m, m + ew), (0, 0)), mode='wrap' if wrap else 'symmetric')


def match_luminance(rgb, alpha, target, contrast=1.0):
    """Scales the albedo in linear light so its mean luminance is target. contrast above 1
    first raises the linear luminance to that power, each texel's color scaled with it (its
    hue and saturation stay): the darks go darker, the lights lighter (relative to the mean
    the scale then restores). A power on each channel would raise the saturation."""
    lin0 = (rgb.astype(np.float64) / 255.0) ** GAMMA
    if contrast != 1.0:
        y = np.maximum(lin0 @ LUM, 1e-9)[..., None]
        lin0 = lin0 * (y ** (contrast - 1.0))
    cur = float((lin0 @ LUM)[alpha > 0].mean()) if alpha is not None and (alpha > 0).any() else float((lin0 @ LUM).mean())
    k = target / cur if cur > 0 else 1.0
    lin = np.clip(lin0 * k, 0.0, 1.0)
    out = np.clip(np.round(lin ** (1.0 / GAMMA) * 255.0), 0, 255).astype(np.uint8)
    return out, k


def upscale_alpha(alpha, w4, h4, kind):
    up = Image.fromarray(alpha).resize((w4, h4), Image.BICUBIC)
    a = np.asarray(up)
    if kind == 'coverage':
        a = np.where(a >= 128, 255, 0).astype(np.uint8)
    return a


# -- the maps ------------------------------------------------------------------

class Maps:
    """PBRify's 1x models: normal, roughness, height, from the albedo."""

    def __init__(self, models_dir, names):
        import torch
        from spandrel import ModelLoader
        self.torch = torch
        self.models = {}
        for k in ('normal', 'roughness', 'height'):
            m = ModelLoader().load_from_file(os.path.join(models_dir, names[k]))
            self.models[k] = m.model.to('cuda').eval()

    def run(self, which, rgb, wrap, margin=16):
        torch = self.torch
        p = pad(rgb, margin, wrap)
        x = torch.from_numpy(p).permute(2, 0, 1)[None].float().div(255.0).to('cuda')
        with torch.no_grad():
            y = self.models[which](x)
        y = y[0].permute(1, 2, 0).clamp(0, 1).float().cpu().numpy()
        # a 4x skin is a 3000 px image through a 1x model: gigabytes of activations that this
        # process's allocator would keep reserved, squeezing ComfyUI into offloading (4 s a
        # texture became 16 s after the first large one)
        del x
        torch.cuda.empty_cache()
        return y[margin:-margin, margin:-margin]


def height_normal_correlation(normal, height):
    """Correlation of the normal map's green with the height's row gradient: positive
    for OpenGL's convention (green up), negative for DirectX's."""
    g = normal[..., 1].astype(np.float64) - 0.5
    h = height.mean(axis=2).astype(np.float64)
    dh = np.roll(h, -1, 0) - np.roll(h, 1, 0)     # a row index grows downwards
    g, dh = g - g.mean(), dh - dh.mean()
    d = np.sqrt((g * g).sum() * (dh * dh).sum())
    return float((g * dh).sum() / d) if d > 0 else 0.0


def finish_normal(n, flip_y):
    v = n * 2.0 - 1.0
    if flip_y:
        v[..., 1] = -v[..., 1]
    ln = np.sqrt((v * v).sum(axis=2, keepdims=True))
    v = v / np.maximum(ln, 1e-6)
    return np.clip(np.round((v * 0.5 + 0.5) * 255.0), 0, 255).astype(np.uint8)


def finish_roughness(r, rmin, rmax):
    g = r.mean(axis=2)
    lo, hi = np.percentile(g, 2), np.percentile(g, 98)
    t = np.clip((g - lo) / (hi - lo), 0.0, 1.0) if hi - lo > 0.02 else np.full(g.shape, 0.5)
    return rmin + (rmax - rmin) * t


def make_orm(rough, metallic, occlusion=None):
    """glTF's packed map: occlusion (R; 1 where none is given), roughness (G), metallic (B: a
    class's 0 or 1, or per texel since 9.4's materials per region)."""
    h, w = rough.shape
    orm = np.empty((h, w, 3), np.uint8)
    orm[..., 0] = 255 if occlusion is None else np.clip(np.round(occlusion * 255.0), 0, 255).astype(np.uint8)
    orm[..., 1] = np.clip(np.round(rough * 255.0), 0, 255).astype(np.uint8)
    if np.ndim(metallic) == 0:
        orm[..., 2] = 255 if metallic >= 0.5 else 0
    else:
        orm[..., 2] = np.clip(np.round(metallic * 255.0), 0, 255).astype(np.uint8)
    return orm


def mat_text(res):
    p = res.params
    lines = [f"# texpack {TOOL_VERSION} (story 5.8): class {res.cls}" + (f", {res.description}" if res.description else '')]
    n = 0
    if p['kind'] != 'regular':
        lines.append(f"kind {p['kind']}")
        n += 1
    if abs(p['bump'] - 1.0) > 1e-6:
        lines.append(f"bump {p['bump']:g}")
        n += 1
    if abs(p['specular'] - 1.0) > 1e-6:
        lines.append(f"specular {p['specular']:g}")
        n += 1
    if p['emissive'] > 0:
        lines.append(f"emissive {p['emissive']:g}")
        n += 1
    return ('\n'.join(lines) + '\n') if n else None


# -- the driver ----------------------------------------------------------------

def expected_suffixes(res):
    """The files a texture's class makes, as suffixes of its stem."""
    p = res.params
    want = ['.png']
    if p['mode'] in ('diffuse', 'skin'):
        if p['maps']:
            want += ['_n.png', '_orm.png']
        if mat_text(res):
            want.append('.mat')
    return want


def seed_for(stem, base):
    return (zlib.crc32(stem.encode()) ^ base) & 0xffffffff


def state_key(png, res, models, seed):
    j = json.dumps({'v': TOOL_VERSION, 'class': res.cls, 'desc': res.description, 'p': res.params,
                    'models': models, 'seed': seed}, sort_keys=True)
    return hashlib.sha1(png + j.encode()).hexdigest()


class Runner:
    def __init__(self, export_dir, out_dir, classes, comfy, seed=5800, flip_y=False, log=print):
        self.export_dir = export_dir
        self.out = out_dir
        self.classes = classes
        self.comfy = comfy
        self.seed = seed
        self.flip_y = flip_y
        self.log = log
        self.maps = None
        self.state_path = os.path.join(out_dir, '.texpack', 'state.json')
        os.makedirs(os.path.dirname(self.state_path), exist_ok=True)
        self.state = json.load(open(self.state_path)) if os.path.exists(self.state_path) else {}

    def save_state(self):
        with open(self.state_path, 'w') as f:
            json.dump(self.state, f, indent=1, sort_keys=True)

    def _write(self, stem, suffix, data):
        path = os.path.join(self.out, 'textures', stem + suffix)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        mode = 'wb' if isinstance(data, bytes) else 'w'
        with open(path, mode, **({} if mode == 'wb' else {'newline': '\n'})) as f:
            f.write(data)

    def process(self, entry, res, force=False):
        p = res.params
        if p['mode'] == 'skip':
            return None
        src = os.path.join(self.export_dir, entry.file)
        png = open(src, 'rb').read()
        names = self.classes['models']
        seed = seed_for(entry.stem, self.seed)
        key = state_key(png, res, names, seed)
        want = expected_suffixes(res)
        have = all(os.path.exists(os.path.join(self.out, 'textures', entry.stem + suf)) for suf in want)
        if not force and have and self.state.get(entry.stem, {}).get('key') == key:
            return dict(self.state[entry.stem], skipped=True)
        t0 = time.time()
        rgb, alpha = read_png(src)
        h, w = rgb.shape[:2]
        wrap = p['mode'] != 'skin' and p['mode'] != 'sprite'
        m = margin_for(w, h)
        rgb_in = bleed(rgb, alpha, m)
        padded = pad(rgb_in, m, wrap, even=True)
        ph, pw = padded.shape[:2]
        wp = dict(p, prompt_text=res.prompt(), upscaler=names['upscaler'], checkpoint=names['checkpoint'],
                  controlnet=names['controlnet'])
        from comfy import build_workflow
        name = self.comfy.upload('texpack_in.png', png_bytes(padded))
        wf = build_workflow(name, wp, seed, large=max(pw, ph) * 4 > LARGE)
        out_png = self.comfy.run(wf)
        up = np.asarray(Image.open(io.BytesIO(out_png)).convert('RGB'))
        if up.shape[0] != ph * 4 or up.shape[1] != pw * 4:
            raise RuntimeError(f"{entry.stem}: the upscaler returned {up.shape[1]}x{up.shape[0]}, expected {pw * 4}x{ph * 4}")
        out = up[4 * m:4 * m + 4 * h, 4 * m:4 * m + 4 * w].copy()
        a4 = upscale_alpha(alpha, 4 * w, 4 * h, entry.alpha) if alpha is not None else None
        target = mean_linear_luminance(rgb, alpha) * p['albedo_ratio']
        out, gain = match_luminance(out, a4, target, p['contrast'])
        albedo = np.dstack([out, a4]) if a4 is not None else out
        for suf in ('_n.png', '_orm.png', '.mat'):     # what an earlier class made and this one doesn't
            old = os.path.join(self.out, 'textures', entry.stem + suf)
            if suf not in want and os.path.exists(old):
                os.remove(old)
        self._write(entry.stem, '.png', png_bytes(albedo))
        info = {'key': key, 'class': res.cls, 'size': [4 * w, 4 * h], 'gain': round(gain, 3),
                'lum_in': round(mean_linear_luminance(rgb, alpha), 5),
                'lum_out': round(mean_linear_luminance(out, a4), 5)}
        if p['mode'] in ('diffuse', 'skin'):
            if p['maps']:
                if self.maps is None:
                    self.maps = Maps(os.path.join(self.comfy.home, 'models', 'pbrify'), names)
                n = self.maps.run('normal', out, wrap)
                r = self.maps.run('roughness', out, wrap)
                self._write(entry.stem, '_n.png', png_bytes(finish_normal(n, self.flip_y)))
                self._write(entry.stem, '_orm.png', png_bytes(make_orm(finish_roughness(r, p['rough_min'], p['rough_max']), p['metallic'])))
            text = mat_text(res)        # a class without maps can still have a kind (glass)
            if text:
                self._write(entry.stem, '.mat', text)
        info['seconds'] = round(time.time() - t0, 1)
        self.state[entry.stem] = info
        self.save_state()
        return info
