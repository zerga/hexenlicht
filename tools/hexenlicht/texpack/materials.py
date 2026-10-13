"""texpack's materials per region (story 9.4; DECISIONS M67, M68): which material each texel is,
and what follows from it texel by texel: roughness, metallic, the contrast, the brightness; and
the cavities (roughness 1 and the occlusion).

9.3 labeled each texture's regions (labels.py): the palette's ramps named by material
(`*=stone;yellow=gold`). A material's values are classes.toml's [materials] (rough_min,
rough_max, metallic, contrast, albedo_ratio; [defaults]' where one is left out). Each texel of
the original takes its ramp's material (a ramp of two, `a/b`, the first); ramps no region names,
and textures without regions, take the class's own values, as the whole texture did in 5.8 and
9.1. At 4x the materials are weights that sum to 1: the texels' materials upscaled smoothly and
snapped to the redraw's edges by a guided filter (a gold inlay's edge follows the drawn one).
"""
import numpy as np

import labels
from imaging import _enc, _lin, _luma, guided, upscale
from pipeline import LUM

KEYS = ('rough_min', 'rough_max', 'metallic', 'contrast', 'albedo_ratio')
CORE = 0.5          # a texel counts as its material's for the statistics where its weight is at least this
SMALL = 0.02        # a material on fewer of the texels than this takes the texture's overall gain
CAVITY = (0.05, 0.15)   # near black (roughness 1, occluded): the albedo's linear luminance against
                        # its material's median, from fully (below the first) to not at all (above)
CAVITY_TIERS = ('reimagine', 'layout')  # a faithful picture's black is paint, not a gap
NO_CLIP = 0.95      # a metal's brightest colour channel at most this (its 99th percentile; M75)


def table(classes):
    """Every material's values, [defaults]' filled in; a metal's `reflectance` only where it has
    one (a key every material had would change every texture's stage key)."""
    d = classes['defaults']
    return {m: dict({k: float(v.get(k, d[k])) for k in KEYS}, **({'reflectance': float(v['reflectance'])} if 'reflectance' in v else {}))
            for m, v in classes.get('materials', {}).items() if isinstance(v, dict)}


def class_values(params):
    return {k: float(params[k]) for k in KEYS}


def texel_materials(rgb, alpha, palette, regions, params, tab):
    """Per texel of the original an index into the returned names and values (-1 where it is a
    hole); index 0 is the class's own values (the ramps no region names). Without the palette or
    regions, every texel is the class's."""
    names, values = ['(class)'], [class_values(params)]
    idx = np.zeros(rgb.shape[:2], np.int32)
    if palette is not None and regions:
        ramps = labels.texel_ramps(rgb, palette)
        named, star, black = set(), None, None
        for rs, mats in labels.parse_regions(regions):
            m = mats[0]
            if m not in tab:
                raise ValueError(f"material '{m}' is not in classes.toml's [materials]")
            if m not in names:
                names.append(m)
                values.append(tab[m])
            k = names.index(m)
            for r in rs:
                if r == '*':
                    star = k
                elif r == 'black':
                    black = k
                else:
                    named.add(labels.RAMP_NAMES.index(r))
                    idx[ramps == labels.RAMP_NAMES.index(r)] = k
        if star is not None:
            idx[~np.isin(ramps, sorted(named))] = star
        if black is not None:       # the near-black, out of grey and `*` (labels.BLACK)
            pi = labels.texel_index(rgb, palette)
            idx[(pi >= 0) & (pi < labels.BLACK)] = black
    if alpha is not None:
        idx[alpha == 0] = -1
    return idx, names, values


GUIDE_EPS = 1e-4    # the guided filter's: below a guide's local variance the weights follow its edges


def fill_holes(idx, wrap):
    """A hole's texel (-1) takes the material of the nearest opaque one (grown a texel at a
    time), so the upscale doesn't blend the class's values into the bars beside a hole."""
    idx = idx.copy()
    for _ in range(max(idx.shape)):
        hole = idx < 0
        if not hole.any() or hole.all():
            break
        for dy, dx in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            if wrap:
                nb = np.roll(idx, (dy, dx), axis=(0, 1))
            else:
                nb = np.full_like(idx, -1)
                h, w = idx.shape
                nb[max(dy, 0):h + min(dy, 0), max(dx, 0):w + min(dx, 0)] = idx[max(-dy, 0):h + min(-dy, 0), max(-dx, 0):w + min(-dx, 0)]
            take = hole & (nb >= 0) & (idx < 0)
            idx[take] = nb[take]
    return np.where(idx < 0, 0, idx)


def weights(idx, n, k, wrap, guide=None, eps=GUIDE_EPS):
    """The materials' weights at k times the size, (n, h*k, w*k), summing to 1: each material's
    texels upscaled bilinearly (a hole as its nearest opaque texel) and, with a guide (the redraw's
    luminance, 0..1), snapped to its edges by a guided filter of half a texel's radius (on 9.1's
    oak door with iron straps a little closer to the drawn iron than the plain upscale; where the
    drawing has no edge the weights soften by that radius)."""
    idx = fill_holes(idx, wrap)
    present = [i for i in range(n) if (idx == i).any()]
    W = np.zeros((n, idx.shape[0] * k, idx.shape[1] * k))
    if len(present) == 1:
        W[present[0]] = 1.0
        return W
    for i in present:
        W[i] = upscale((idx == i).astype(np.float64), k, wrap)
    if guide is not None:
        G = np.zeros_like(W)
        for i in present:
            G[i] = np.clip(guided(guide, W[i], max(1, k // 2), eps, wrap), 0, None)
        s = G.sum(axis=0)
        ok = s > 1e-6
        W = np.where(ok, G / np.where(ok, s, 1.0), W)
    return W / np.maximum(W.sum(axis=0), 1e-9)


def _per(W, values, key):
    return np.tensordot(np.array([v[key] for v in values]), W, axes=1)


def effective_ratio(mean, v):
    """A material's albedo ratio, at most what brings its mean to a real metal's reflectance (9.5,
    M75: a metal's albedo is its reflectance, and Raven painted metals 20 to 50 times darker than
    any real metal reflects, so their ratio is 5; a bright gilding stays under real gold's)."""
    r = v['albedo_ratio']
    f0 = v.get('reflectance', 0.0)
    return min(r, f0 / mean) if f0 > 0 and mean > 0 else r


def ratio_map(rgb, idx, values):
    """Per texel of the original its material's effective albedo ratio (1 in a hole): the
    original's luminance times it is what the run aims for, so a layout measure that compares
    them sees the shapes, not the metals made 5 times brighter than the stone around them (M75)."""
    y = _luma(rgb)
    eff = [effective_ratio(float(y[idx == i].mean()) if (idx == i).any() else 0.0, v) for i, v in enumerate(values)]
    return np.where(idx >= 0, np.array(eff)[np.maximum(idx, 0)], 1.0)


def targets(rgb, alpha, idx, values, plain=False):
    """Each material's wanted mean linear luminance: the original's there times its albedo_ratio
    (a metal's at most its reflectance; None for a material on fewer than SMALL of the texels),
    and the whole texture's. plain: the original's alone (ratio 1: the floor of a metal's
    no-clip cap in match)."""
    y = _luma(rgb)
    op = idx >= 0
    eff = [1.0 if plain else effective_ratio(float(y[idx == i].mean()) if (idx == i).any() else 0.0, v)
           for i, v in enumerate(values)]
    ratio = np.array(eff)[np.maximum(idx, 0)]
    whole = float((y * ratio)[op].mean()) if op.any() else 0.0
    out = []
    for i in range(len(values)):
        sel = idx == i
        out.append(float(y[sel].mean()) * eff[i] if sel.sum() >= max(8, SMALL * op.sum()) else None)
    return out, whole


def match(rgb, a4, W, values, want, whole, contrast=True, floor=None):
    """5.8's brightness match (M37, M43) per material: the contrast (a power on the linear
    luminance, the color scaled with it) by each texel's material, then each material's mean
    brought to its target, the gains blended by the weights. With one material it is
    pipeline.match_luminance. A metal's gain stops before its brightest channel clips, but not
    below what brings it to `floor` (targets(plain=True): the original's own brightness, as
    before 9.5; glinting gold coins came out darker than that). Returns the albedo and the gains."""
    lin = _lin(rgb)
    if contrast:
        c = _per(W, values, 'contrast')
        if not np.allclose(c, 1.0):
            lin = lin * (np.maximum(lin @ LUM, 1e-9) ** (c - 1.0))[..., None]
    y = lin @ LUM
    op = (a4 > 0) if a4 is not None and (a4 > 0).any() else np.ones(y.shape, bool)
    cur = float(y[op].mean())
    g_all = whole / cur if cur > 0 else 1.0
    gains = []
    for i in range(len(values)):
        w = W[i] * op
        s = w.sum()
        cur_i = float((w * y).sum() / s) if s > 0 else 0.0
        g = want[i] / cur_i if want[i] is not None and cur_i > 0 else g_all
        if values[i].get('reflectance', 0.0) > 0:
            # a metal's gain stops before its brightest colour channel clips (5 times the coins' gold
            # washed out to silvery white): its hue kept (M75)
            core = (W[i] >= CORE) & op
            if core.any():
                cap = NO_CLIP / max(float(np.percentile(lin[core].max(axis=-1), 99)), 1e-6)
                low = floor[i] / cur_i if floor is not None and floor[i] is not None and cur_i > 0 else 0.0
                g = min(g, max(cap, low))
        gains.append(g)
    G = np.tensordot(np.array(gains), W, axes=1)
    return _enc(np.clip(lin * G[..., None], 0.0, 1.0)), gains


def roughness(raw, W, values, op=None):
    """The roughness model's output (h, w, 3, 0..1) stretched per material between its 2nd and
    98th percentile there over the material's range (5.8's finish_roughness, M38, per region)."""
    g = raw.mean(axis=2) if raw.ndim == 3 else raw
    op = np.ones(g.shape, bool) if op is None else op
    out = np.zeros(g.shape)
    for i, v in enumerate(values):
        if not W[i].any():
            continue
        sel = (W[i] >= CORE) & op
        if sel.sum() < 16:
            sel = (W[i] > 0) & op
        if not sel.any():
            sel = op
        lo, hi = np.percentile(g[sel], 2), np.percentile(g[sel], 98)
        t = np.clip((g - lo) / (hi - lo), 0.0, 1.0) if hi - lo > 0.02 else np.full(g.shape, 0.5)
        out += W[i] * (v['rough_min'] + (v['rough_max'] - v['rough_min']) * t)
    return out


def metallic(W, values):
    return _per(W, values, 'metallic')


def surface(rgb, W, op=None, lo=CAVITY[0], hi=CAVITY[1]):
    """1 on a surface, 0 in a near-black cavity (a gap between planks, a joint the redraw drew
    black), smooth between: the albedo's linear luminance against its material's median."""
    y = _luma(rgb)
    op = np.ones(y.shape, bool) if op is None else op
    med = np.zeros(y.shape)
    for i in range(W.shape[0]):
        if not W[i].any():
            continue
        sel = (W[i] >= CORE) & op
        med += W[i] * float(np.median(y[sel] if sel.any() else y[op]))
    t = np.clip((y / np.maximum(med, 1e-9) - lo) / (hi - lo), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def occlusion(normal, cz, surf):
    """The occlusion (_orm's red, 1 unoccluded): the frontal shading term of M47 against a face
    turned to the viewer, exp(cz (n_z - 1)) with cz the redraw's own fitted coefficient (its soft
    frontal light showed how much darker a slope is), times surface()'s factor, so that a
    near-black gap is fully occluded (9.7 makes it absorb). Not physically based: geometry the
    normal map doesn't have."""
    nz = normal[..., 2].astype(np.float64) / 127.5 - 1.0
    return np.clip(np.exp(max(float(cz), 0.0) * np.minimum(nz - 1.0, 0.0)), 0.0, 1.0) * surf
