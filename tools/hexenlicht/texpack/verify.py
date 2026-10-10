"""texpack verify (story 5.8; the redraw's checks since 9.4): a pack against MATERIALS.md and the
originals.

Per texture: the files exist and are named in lowercase; the albedo is four times the original
in the original's aspect ratio (and a multiple of 4, for BC compression); alpha holes are kept
(coverage kinds are 0 or 255 only, within 2 % of the original's holes; a translucent skin's mean
opacity and shape); the normal map's vectors have unit length and point out of the surface; the
.mat has only keys MATERIALS.md knows and the class's kind.
5.8's upscale: the albedo's mean luminance at the class's albedo_ratio (a note), the _orm's R is
255, G inside the class's roughness range, B the class's metallic.
The redraw (redraw.py, 9.4): per material of the texture's regions (materials.py) the roughness
inside its range where the albedo isn't near black, roughness 1 and the occlusion low where it
is (reimagine and layout), the metallic, the brightness against the original's there times its
ratio (a note); the seam where the original tiles; 9.1's numbers as notes beyond the pilot's
ranges (the light left in, the color drift, the layout kept); a set's shared texels identical
in every map, a copy's files its source's.
"""
import os

import numpy as np
from PIL import Image

import materials
import pipeline
from imaging import _blur

MAT_KEYS = {'kind', 'roughness', 'metallic', 'bump', 'specular', 'emissive'}
# 9.1's numbers (DECISIONS M49, M71): a note beyond these
LIGHT_LEFT = 0.15           # the directional fit's correlation after the light is taken out (the pilot's at most 0.12)
DRIFT = 0.012               # the broad colors' Oklab distance (the pilot's 0.001-0.011; lava 0.027)
LAYOUT = {'reimagine': 0.5, 'layout': 0.4, 'faithful': 0.6, 'glass': 0.4, 'liquid': 0.8, 'lava': 0.8}
SHAPES = 0.3                # a layout or faithful texture under this has lost its shapes: a failure (9.4's mtex464,
                            # drawn as a slab with a margin, 0.03; the pilot's layout and faithful ones 0.5-0.9)
CORE = 3                    # a texel is its material's for the checks this far (px) inside its texels (the run's guided
                            # filter softens an edge by half a texel)


def _load(path):
    return np.asarray(Image.open(path))


def _common(ex, pack, entry, res, errs):
    """The checks every pack's texture takes; returns (orig_rgb, orig_a, albedo rgb, alpha, base)
    or None without an albedo."""
    base = os.path.join(pack, 'textures', entry.stem)
    if entry.stem != entry.stem.lower():
        errs.append('name not lowercase')
    if not os.path.exists(base + '.png'):
        errs.append('no albedo')
        return None
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
    return orig_rgb, orig_a, rgb, alpha, base


def _normal(base, shape, errs):
    n = _load(base + '_n.png').astype(np.float64) / 255 * 2 - 1
    if n.shape[:2] != shape:
        errs.append('normal map size differs')
    ln_ = np.sqrt((n ** 2).sum(axis=2))
    if abs(float(ln_.mean()) - 1.0) > 0.03:
        errs.append(f'normal vectors average length {ln_.mean():.3f}')
    if float(n[..., 2].mean()) <= 0:
        errs.append('normals point into the surface')
    return n


def _mat(base, p, errs):
    if os.path.exists(base + '.mat'):
        for ln in open(base + '.mat', encoding='utf-8'):
            ln = ln.split('#')[0].strip()
            if ln and ln.split()[0] not in MAT_KEYS:
                errs.append(f'.mat key {ln.split()[0]}')
    if p['kind'] != 'regular':
        mat = base + '.mat'
        text = open(mat, encoding='utf-8').read() if os.path.exists(mat) else ''
        if f"kind {p['kind']}" not in text:
            errs.append(f"the class's kind {p['kind']} is not in the .mat")


def check(ex, pack, entry, res):
    """5.8's upscale (and the classes' skins, fx and sprites)."""
    errs, notes = [], []
    p = res.params
    got = _common(ex, pack, entry, res, errs)
    if got is None:
        return errs, notes
    orig_rgb, orig_a, rgb, alpha, base = got
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
            _normal(base, rgb.shape[:2], errs)
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
    if p['mode'] in ('diffuse', 'skin'):
        _mat(base, p, errs)
    return errs, notes


def check_redraw(ex, pack, entry, res, palette, tab, own=None):
    """The redraw's (redraw.py, 9.4). own: the texels (1x) the texture drew itself, where a set's
    member took no other member's drawing (groups.sources): its materials are checked there only,
    as the texels it shares came with their source's maps (groups_check holds them to them)."""
    import redraw
    errs, notes = [], []
    p, tier = res.params, res.tier
    got = _common(ex, pack, entry, res, errs)
    if got is None:
        return errs, notes
    orig_rgb, orig_a, rgb, alpha, base = got
    h4, w4 = rgb.shape[:2]
    if (h4, w4) != (4 * orig_rgb.shape[0], 4 * orig_rgb.shape[1]):
        return errs, notes          # _common reported the size; the maps can't be compared texel by texel
    maps = p['maps'] and tier not in redraw.AS_DRAWN
    idx, names, values = materials.texel_materials(orig_rgb, orig_a, palette, res.regions, p, tab)
    # the run's weights followed its drawing's edges (the guided filter on the model's image):
    # the albedo's are the nearest the pack has
    W = materials.weights(idx, len(values), 4, True, guide=(rgb.astype(np.float64) @ pipeline.LUM) / 255.0)
    mine = np.ones((h4, w4)) if own is None else np.kron(own.astype(np.float64), np.ones((4, 4)))
    core = [_blur(((W[i] >= 0.999) & (mine > 0)).astype(np.float64), CORE, True) >= 0.999 for i in range(len(values))]
    op = (alpha > 0) if alpha is not None else np.ones((h4, w4), bool)
    # the brightness per material: a note, as 5.8's (the contrast curve clips the brightest texels)
    target, whole = materials.targets(orig_rgb, orig_a, idx, values)
    y = (rgb.astype(np.float64) / 255.0) ** pipeline.GAMMA @ pipeline.LUM
    ratio = float(y[op].mean()) / max(whole, 1e-9)
    if abs(ratio - 1) > 0.05:
        notes.append(f'luminance x{ratio:.2f} of the target (the original\'s times its materials\' ratios)')
    for i, t in enumerate(target):
        # the run matched each material's mean weighted by its weights, soft edges and all (a
        # material's cores are lighter than its dark outlines): the same here; a set's member is
        # balanced as a whole (redraw's _rebalance), so only where it drew everything itself
        w = W[i] * op
        if t is not None and (own is None or own.all()) and w.sum() > 64:
            r = float((w * y).sum() / w.sum()) / t
            if abs(r - 1) > 0.15:
                notes.append(f'{names[i]}: luminance x{r:.2f} of its target')
    if maps:
        for suf in ('_n.png', '_orm.png'):
            if not os.path.exists(base + suf):
                errs.append('missing ' + suf)
        normal = _normal(base, (h4, w4), errs) if os.path.exists(base + '_n.png') else None
        if os.path.exists(base + '_orm.png'):
            o = _load(base + '_orm.png').astype(np.float64) / 255.0
            surf = materials.surface(rgb, W, op) if tier in materials.CAVITY_TIERS else np.ones((h4, w4))
            gap = (surf <= 0.0) & op & (_blur(mine, CORE, True) >= 0.999)
            # a set's member: its cavities were found against its whole drawing's medians, which its
            # shared texels no longer show; checked where it drew nearly all itself
            if gap.sum() > 16 and (own is None or own.mean() >= 0.9):
                if float((o[..., 1][gap] >= 0.98).mean()) < 0.9:
                    errs.append(f'near-black cavities not at roughness 1 ({float((o[..., 1][gap] >= 0.98).mean()) * 100:.0f} %)')
                if float((o[..., 0][gap] <= 0.05).mean()) < 0.9:
                    errs.append(f'near-black cavities not occluded ({float((o[..., 0][gap] <= 0.05).mean()) * 100:.0f} %)')
            if float(o[..., 0][op].mean()) < 0.5:
                notes.append(f'occlusion mean {float(o[..., 0][op].mean()):.2f}')
            for i, v in enumerate(values):
                sel = core[i] & op & (surf >= 1.0)
                if sel.sum() < 64:
                    continue
                g = o[..., 1][sel]
                out = float(((g < v['rough_min'] - 0.02) | (g > v['rough_max'] + 0.02)).mean())
                if out > 0.05:
                    errs.append(f"{names[i]}: roughness outside {v['rough_min']:g}..{v['rough_max']:g} in {out * 100:.0f} % of its texels")
                b = o[..., 2][core[i]]
                off = float((np.abs(b - v['metallic']) > 0.05).mean())
                if off > 0.05:
                    errs.append(f"{names[i]}: metallic not {v['metallic']:g} in {off * 100:.0f} % of its texels")
        if normal is not None:
            _, light, _ = redraw.shading_fit(rgb, ((normal + 1) * 127.5).astype(np.uint8), max(4, min(h4, w4) // 12),
                                             op if alpha is not None else None, True)
            if light > LIGHT_LEFT:
                notes.append(f'light from a side left in: {light:.2f}')
    elif any(os.path.exists(base + s) for s in ('_n.png', '_orm.png')):
        errs.append('maps written for a class without maps')
    _mat(base, p, errs)
    seam0, seam = redraw.seam_score(orig_rgb), redraw.seam_score(rgb)
    if seam0 <= 2.0 and seam > max(2.0, 2 * seam0):
        errs.append(f'a seam: {seam:.2f} against the original\'s {seam0:.2f}')
    bled = pipeline.bleed(orig_rgb, orig_a, pipeline.margin_for(*orig_rgb.shape[1::-1]))
    drift, layout = redraw.color_drift(bled, rgb), redraw.layout_kept(bled, rgb)
    if drift > DRIFT:
        notes.append(f'color drift {drift:.4f}')
    # an original without detail (a flat color) has no layout to keep: layout_kept is about 0 there
    y0 = redraw._lin(bled) @ pipeline.LUM
    flat = float(redraw._highpass(y0, max(2, min(y0.shape) // 8)).std()) < 0.01 * max(float(y0.mean()), 1e-6)
    if flat:
        pass
    elif tier in ('layout', 'faithful') and layout < SHAPES:
        errs.append(f'the shapes moved: layout kept {layout:.2f}')
    elif layout < LAYOUT.get(tier, 0.5):
        notes.append(f'layout kept {layout:.2f}')
    return errs, notes


def _bytes(path):
    with open(path, 'rb') as f:
        return f.read()


def groups_check(pack, plan, stems, orig):
    """A set's shared texels identical in every map (less the feather at their edges), a copy's files
    its source's. Returns {stem: [errors]}."""
    import groups
    out = {}
    done = set()
    for s in stems:
        r = plan.rep.get(s)
        if r is None:
            continue
        if r != s:
            for suf in ('.png', '_n.png', '_orm.png', '.mat'):
                a, b = (os.path.join(pack, 'textures', x + suf) for x in (s, r))
                if os.path.exists(a) != os.path.exists(b) or (os.path.exists(a) and _bytes(a) != _bytes(b)):
                    out.setdefault(s, []).append(f'{suf} is not {r}\'s (a copy)')
        g = plan.group[r]
        if len(g) < 2 or g[0] in done:
            continue
        done.add(g[0])
        ims = {}
        for t in g:
            if os.path.exists(os.path.join(pack, 'textures', t + '.png')):
                ims[t] = [_load(os.path.join(pack, 'textures', t + suf)) if os.path.exists(os.path.join(pack, 'textures', t + suf)) else None
                          for suf in ('.png', '_n.png', '_orm.png')]
        for j, t in enumerate(g):
            for u in g[:j]:
                if t not in ims or u not in ims or orig[t].shape != orig[u].shape:
                    continue
                inner = groups.shared_interior(orig[t], orig[u], 4)
                # only where t took u's texels: an earlier member equal there takes precedence, and
                # is equal to u there too
                for k, suf in enumerate(('.png', '_n.png', '_orm.png')):
                    a, b = ims[t][k], ims[u][k]
                    if a is None or b is None or not inner.any():
                        continue
                    if not (a[inner] == b[inner]).all():
                        out.setdefault(t, []).append(f'{suf}: texels shared with {u} differ ({float((a[inner] != b[inner]).any(axis=-1).mean()) * 100:.1f} %)')
    return out


def run(ex, pack, sel, resolved, palette=None, classes=None, plan=None, orig=None):
    import redraw
    bad = 0
    n = 0
    tab = materials.table(classes) if classes else {}
    grp = groups_check(pack, plan, [e.stem for e in sel], orig) if plan is not None else {}
    for e in sel:
        res = resolved[e.stem]
        if res.params['mode'] == 'skip' or res.tier == 'skip':
            continue
        n += 1
        if e.kind in ('world', 'liquid') and res.tier in redraw.TIERS:
            own = None
            if plan is not None and e.stem in plan.rep:
                import groups
                g = plan.group[plan.rep[e.stem]]
                j = g.index(plan.rep[e.stem])
                own = groups.sources(g, orig, j) == j
            errs, notes = check_redraw(ex, pack, e, res, palette, tab, own)
        else:
            errs, notes = check(ex, pack, e, res)
        errs += grp.get(e.stem, [])
        if errs:
            bad += 1
        print(f"{'FAIL' if errs else 'ok  '} {e.stem} ({res.tier + ', ' if res.tier else ''}{res.cls})"
              + ''.join(f"\n       {x}" for x in errs) + ''.join(f"\n       note: {x}" for x in notes))
    print(f"{n - bad} of {n} textures pass")
    return 1 if bad else 0
