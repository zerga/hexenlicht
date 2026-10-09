"""texpack verify (story 5.8): a pack against MATERIALS.md and the originals.

Per texture: the files exist and are named in lowercase; the albedo is four
times the original in the original's aspect ratio (and a multiple of 4, for BC
compression) with its mean luminance at the class's albedo_ratio; alpha holes
are kept (coverage kinds are 0 or 255 only, within 2 % of the original's
holes); the normal map's vectors have unit length and point out of the
surface; the _orm's R is 255, G is inside the class's roughness range, B is the
class's metallic; the .mat has only keys MATERIALS.md knows.
"""
import os

import numpy as np
from PIL import Image

import pipeline

MAT_KEYS = {'kind', 'roughness', 'metallic', 'bump', 'specular', 'emissive'}


def _load(path):
    return np.asarray(Image.open(path))


def check(ex, pack, entry, res):
    errs, notes = [], []
    p = res.params
    base = os.path.join(pack, 'textures', entry.stem)
    if entry.stem != entry.stem.lower():
        errs.append('name not lowercase')
    if not os.path.exists(base + '.png'):
        return ['no albedo'], notes
    orig_rgb, orig_a = pipeline.read_png(os.path.join(ex, entry.file))
    h, w = orig_rgb.shape[:2]
    a = _load(base + '.png')
    if a.shape[0] != 4 * h or a.shape[1] != 4 * w:
        errs.append(f'albedo {a.shape[1]}x{a.shape[0]}, expected {4 * w}x{4 * h}')
    if a.shape[0] % 4 or a.shape[1] % 4:
        errs.append('size not a multiple of 4')
    rgb = a[..., :3]
    alpha = a[..., 3] if a.ndim == 3 and a.shape[2] == 4 else None
    if orig_a is not None and alpha is None:
        errs.append('the original has alpha, the albedo has none')
    if orig_a is not None and alpha is not None:
        if entry.alpha == 'coverage' and not np.isin(alpha, (0, 255)).all():
            errs.append('coverage alpha has values between 0 and 255')
        if entry.alpha == 'translucent':
            # opacity, not holes: a smooth edge between 84 and 255 moves texels across 128
            d = abs(float(alpha.mean()) - float(orig_a.mean())) / 255
            if d > 0.01:
                errs.append(f'mean opacity differs by {d * 100:.1f} % of full')
            # and the shape: the original's alpha stretched to the albedo's size, texel for texel
            ref = np.asarray(Image.fromarray(orig_a).resize((alpha.shape[1], alpha.shape[0]), Image.BICUBIC)).astype(float)
            e = float(np.abs(alpha.astype(float) - ref).mean()) / 255
            if e > 0.02:
                errs.append(f'alpha shape differs from the original by {e * 100:.1f} % of full')
        else:
            d = abs(float((alpha < 128).mean()) - float((orig_a < 128).mean()))
            if d > 0.02:
                errs.append(f'hole area differs by {d * 100:.1f} %')
    lo = pipeline.mean_linear_luminance(orig_rgb, orig_a)
    ln = pipeline.mean_linear_luminance(rgb, alpha)
    ratio = ln / lo if lo > 0 else 0
    if abs(ratio - p['albedo_ratio']) > 0.05 * p['albedo_ratio']:
        notes.append(f'luminance x{ratio:.2f} of the original (class {p["albedo_ratio"]:g}; highlights clipped?)')
    want_maps = p['maps'] and p['mode'] in ('diffuse', 'skin')
    if want_maps:
        for suf in ('_n.png', '_orm.png'):
            if not os.path.exists(base + suf):
                errs.append('missing ' + suf)
        if os.path.exists(base + '_n.png'):
            n = _load(base + '_n.png').astype(np.float64) / 255 * 2 - 1
            if n.shape[:2] != a.shape[:2]:
                errs.append('normal map size differs')
            ln_ = np.sqrt((n ** 2).sum(axis=2))
            if abs(float(ln_.mean()) - 1.0) > 0.03:
                errs.append(f'normal vectors average length {ln_.mean():.3f}')
            if float(n[..., 2].mean()) <= 0:
                errs.append('normals point into the surface')
        if os.path.exists(base + '_orm.png'):
            o = _load(base + '_orm.png')
            if not (o[..., 0] == 255).all():
                errs.append('_orm R is not 255')
            g = o[..., 1] / 255.0
            if g.min() < p['rough_min'] - 0.01 or g.max() > p['rough_max'] + 0.01:
                errs.append(f'roughness {g.min():.2f}..{g.max():.2f} outside {p["rough_min"]:g}..{p["rough_max"]:g}')
            want = 255 if p['metallic'] >= 0.5 else 0
            if not (o[..., 2] == want).all():
                errs.append(f'_orm B is not {want}')
    elif any(os.path.exists(base + s) for s in ('_n.png', '_orm.png')):
        errs.append('maps written for a class without maps')
    if os.path.exists(base + '.mat'):
        for ln in open(base + '.mat', encoding='utf-8'):
            ln = ln.split('#')[0].strip()
            if ln and ln.split()[0] not in MAT_KEYS:
                errs.append(f'.mat key {ln.split()[0]}')
    if p['mode'] in ('diffuse', 'skin') and p['kind'] != 'regular':
        mat = base + '.mat'
        text = open(mat, encoding='utf-8').read() if os.path.exists(mat) else ''
        if f"kind {p['kind']}" not in text:
            errs.append(f"the class's kind {p['kind']} is not in the .mat")
    return errs, notes


def run(ex, pack, sel, resolved):
    bad = 0
    n = 0
    for e in sel:
        res = resolved[e.stem]
        if res.params['mode'] == 'skip':
            continue
        n += 1
        errs, notes = check(ex, pack, e, res)
        if errs:
            bad += 1
        print(f"{'FAIL' if errs else 'ok  '} {e.stem} ({res.cls})" + ''.join(f"\n       {x}" for x in errs) + ''.join(f"\n       note: {x}" for x in notes))
    print(f"{n - bad} of {n} textures pass")
    return 1 if bad else 0
