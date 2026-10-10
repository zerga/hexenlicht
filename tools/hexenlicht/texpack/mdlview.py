"""Alias models for the skins (story 9.3's second half; 9.1's turntable prototype, for 9.6 too):
a model (.mdl) read from the game's paks, Hexen II's own (IDPO, version 6) and the mission
pack's (RAPO: texture coordinates of their own), drawn with a skin in one pose from a few
sides, unlit, so that only the texture shows (no light, no smoothing, nothing of the
renderer's); and what the game does with each model: the gamecode's functions and spawn
classes that set it, the maps whose entities place those, the puzzle items' names.

  python mdlview.py <model> --out <png> [--skin N | --png <file> ...] [--views 0,90,180]
                    [--frame 0] [--size 320] [--pitch 10] [--data <folder>]

  <model>   the name in the paks (models/imp.mdl); --skin draws the model's own skin N with
            the game's palette, --png one or more skin images (an export's, a pack's): a row
            each, so that skins are compared exactly. Yaw 0 is the model's front (+x, the way
            it faces in the game), 90 its left side, 180 its back. A model's own skin is drawn as
            stored, without the engine's flood fill of the atlas's black around the pieces.
"""
import argparse
import collections
import math
import os
import re
import struct
import sys

import numpy as np
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
REPO = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
UNIT_M = 0.031          # a world unit in metres (labels.py's)

# the model flags the labels care about (engine/h2shared/gl_model.h's EF_)
TRAIL = 'leaves a trail (a missile or a gib)'
FLAGS = ((1 << 3, 'rotates (a pickup)'), (1 << 12, 'transparent'), (1 << 14, 'holey (its black is see-through)'),
         (1 << 15, 'translucent'), (1 << 16, 'always faces the viewer'),
         *((1 << b, TRAIL) for b in (0, 1, 2, 4, 5, 6, 7, 8, 9, 11, 13, 17, 19, 20, 21, 22, 23)))


def model_files(data_dir, prefix='models/'):
    """name -> [(game, pak, offset, length)] for every file under the prefix, in the game's search
    order (data1's paks, then the mission pack's; the last wins). A name can have several versions:
    the mission pack replaced some models (models/puzzle/scepter.mdl), and the export names their
    skins apart (`~crc`)."""
    out = collections.defaultdict(list)
    for game in ('data1', 'portals'):
        for i in range(10):
            p = os.path.join(data_dir, game, f'pak{i}.pak')
            if not os.path.exists(p):
                continue
            with open(p, 'rb') as f:
                _, dofs, dlen = struct.unpack('<4sii', f.read(12))
                f.seek(dofs)
                d = f.read(dlen)
            for e in range(dlen // 64):
                name, pos, ln = struct.unpack_from('<56sii', d, e * 64)
                name = name.split(b'\0')[0].decode('latin-1').lower()
                if name.startswith(prefix):
                    out[name].append((game, p, pos, ln))
    return out


def read_file(where):
    _, p, pos, ln = where
    with open(p, 'rb') as f:
        f.seek(pos)
        return f.read(ln)


def read_mdl(b, max_frames=1):
    """An alias model: skin size, skins (palette indices; a group's first), texture coordinates
    (onseam, s, t), triangles (facesfront, vertex indices, texture coordinate indices), the
    first poses' vertices in model units, flags, the number of poses (a frame group's each)."""
    rapo = b[:4] == b'RAPO'
    if b[:4] not in (b'IDPO', b'RAPO'):
        raise ValueError('not an alias model')
    scale = np.array(struct.unpack_from('<3f', b, 8))
    origin = np.array(struct.unpack_from('<3f', b, 20))
    numskins, sw, sh, nv, nt, nf = struct.unpack_from('<6i', b, 48)
    flags = struct.unpack_from('<i', b, 76)[0]
    nst = struct.unpack_from('<i', b, 84)[0] if rapo else nv
    p = 88 if rapo else 84
    skins = []
    for _ in range(numskins):
        if struct.unpack_from('<i', b, p)[0] == 0:
            skins.append(np.frombuffer(b, np.uint8, sw * sh, p + 4).reshape(sh, sw))
            p += 4 + sw * sh
        else:
            c = struct.unpack_from('<i', b, p + 4)[0]
            skins.append(np.frombuffer(b, np.uint8, sw * sh, p + 8 + c * 4).reshape(sh, sw))
            p += 8 + c * 4 + c * sw * sh
    st = np.array(struct.unpack_from(f'<{nst * 3}i', b, p), np.int64).reshape(nst, 3)
    p += nst * 12
    tris = []
    for t in range(nt):
        front = struct.unpack_from('<i', b, p + t * 16)[0]
        if rapo:
            vi = struct.unpack_from('<3H', b, p + t * 16 + 4)
            si = struct.unpack_from('<3H', b, p + t * 16 + 10)
        else:
            vi = si = struct.unpack_from('<3i', b, p + t * 16 + 4)
        tris.append((front, tuple(vi), tuple(si)))
    p += nt * 16
    frames = []
    poses = 0

    def verts(q):
        return np.frombuffer(b, np.uint8, nv * 4, q).reshape(nv, 4)[:, :3].astype(np.float64) * scale + origin
    for _ in range(nf):
        ftype = struct.unpack_from('<i', b, p)[0]
        p += 4
        cnt = 1
        if ftype != 0:
            cnt = struct.unpack_from('<i', b, p)[0]
            p += 4 + 8 + cnt * 4
        for _ in range(cnt):
            if len(frames) < max_frames:
                frames.append(verts(p + 8 + 16))
            p += 8 + 16 + nv * 4
        poses += cnt
    return dict(rapo=rapo, sw=sw, sh=sh, skins=skins, st=st, tris=tris, frames=frames,
                flags=flags, numframes=poses, numverts=nv)


def load(files, name, size=None, game=None, max_frames=1):
    """The model `name` from model_files' table: the version of `game` (data1, portals: the
    export's `from`) whose skins are `size` (w, h), else any game's of that size; the game's own
    (the last) without a size. A size no version has raises ValueError."""
    vers = files.get(name.lower())
    if not vers:
        raise KeyError(f'{name} is not in the paks')
    order = list(reversed(vers))
    if game:
        order.sort(key=lambda w: w[0] != game)      # stable: that game's versions first, the last of each first
    for where in order:
        m = read_mdl(read_file(where), max_frames)
        if size is None or (m['sw'], m['sh']) == tuple(size):
            m['game'] = where[0]
            return m
    raise ValueError(f'{name}: no version has skins of {size[0]}x{size[1]}')


def flag_names(flags):
    return sorted({n for bit, n in FLAGS if flags & bit})


def tri_uv(m, t):
    """A triangle's corners in the atlas, in texels as the engine (gl_mesh.c): (s + 0.5, t + 0.5),
    a back face's onseam vertex half a skin to the right."""
    front, vi, si = m['tris'][t]
    out = []
    for s in si:
        onseam, ss, tt = m['st'][s]
        if not front and onseam:
            ss += m['sw'] // 2
        out.append((ss + 0.5, tt + 0.5))
    return np.array(out, float)


def bilinear(img, x, y):
    h, w = img.shape[:2]
    x = np.clip(x - 0.5, 0, w - 1.001)
    y = np.clip(y - 0.5, 0, h - 1.001)
    x0, y0 = np.floor(x).astype(int), np.floor(y).astype(int)
    fx, fy = (x - x0)[:, None], (y - y0)[:, None]
    return (img[y0, x0] * (1 - fx) * (1 - fy) + img[y0, x0 + 1] * fx * (1 - fy)
            + img[y0 + 1, x0] * (1 - fx) * fy + img[y0 + 1, x0 + 1] * fx * fy)


def frame_box(m, frame=0):
    """The pose's center and its span for every yaw: the horizontal radius around the center and
    the height, so that every view of a model is drawn at one scale."""
    V = m['frames'][frame]
    lo, hi = V.min(0), V.max(0)
    c = (lo + hi) / 2
    r = np.sqrt(((V[:, :2] - c[:2]) ** 2).sum(1)).max()
    return c, max(2 * r, hi[2] - lo[2], 1e-3)


def render(m, skin, frame=0, yaw=0.0, pitch=10.0, size=320, bg=(40, 40, 40), nearest=True):
    """The model in one pose, seen from `yaw` degrees around it (0: its front) and `pitch` above,
    the skin (an RGB array, any multiple of the model's skin size) drawn unlit; the whole model
    framed at the scale every yaw shares. Returns size x size x 3 uint8."""
    V = m['frames'][frame]
    c, span = frame_box(m, frame)
    span *= 1.08
    ya, pa = math.radians(yaw), math.radians(pitch)
    # rotate the model so that its yaw direction points at the viewer (+x of the view: y right, z up)
    Rz = np.array([[math.cos(ya), math.sin(ya), 0], [-math.sin(ya), math.cos(ya), 0], [0, 0, 1]])
    Ry = np.array([[math.cos(pa), 0, math.sin(pa)], [0, 1, 0], [-math.sin(pa), 0, math.cos(pa)]])
    P = (Ry @ Rz @ (V - c).T).T
    sx = P[:, 1] / span * size + size / 2
    sy = -P[:, 2] / span * size + size / 2
    depth = P[:, 0]
    img = np.empty((size, size, 3))
    img[:] = bg
    zbuf = np.full((size, size), -1e18)
    sk = skin[..., :3].astype(np.float64)
    k = np.array([skin.shape[1] / m['sw'], skin.shape[0] / m['sh']])
    for t, (front, vi, si) in enumerate(m['tris']):
        vi = list(vi)
        x, y, z = sx[vi], sy[vi], depth[vi]
        d = (y[1] - y[2]) * (x[0] - x[2]) + (x[2] - x[1]) * (y[0] - y[2])
        if abs(d) < 1e-9:
            continue
        x0, x1 = int(max(0, math.floor(x.min()))), int(min(size - 1, math.ceil(x.max())))
        y0, y1 = int(max(0, math.floor(y.min()))), int(min(size - 1, math.ceil(y.max())))
        if x1 < x0 or y1 < y0:
            continue
        gx, gy = np.meshgrid(np.arange(x0, x1 + 1) + 0.5, np.arange(y0, y1 + 1) + 0.5)
        l0 = ((y[1] - y[2]) * (gx - x[2]) + (x[2] - x[1]) * (gy - y[2])) / d
        l1 = ((y[2] - y[0]) * (gx - x[2]) + (x[0] - x[2]) * (gy - y[2])) / d
        l2 = 1 - l0 - l1
        inside = (l0 >= 0) & (l1 >= 0) & (l2 >= 0)
        if not inside.any():
            continue
        iy, ix = np.nonzero(inside)
        iy, ix = iy + y0, ix + x0
        zs = (l0 * z[0] + l1 * z[1] + l2 * z[2])[inside]
        closer = zs > zbuf[iy, ix]
        if not closer.any():
            continue
        iy, ix, zs = iy[closer], ix[closer], zs[closer]
        bc = np.stack([l0[inside][closer], l1[inside][closer], l2[inside][closer]], 1)
        uv = tri_uv(m, t) * k
        u, v = bc @ uv[:, 0], bc @ uv[:, 1]
        if nearest:
            col = sk[np.clip(v.astype(int), 0, sk.shape[0] - 1), np.clip(u.astype(int), 0, sk.shape[1] - 1)]
        else:
            col = bilinear(sk, u, v)
        zbuf[iy, ix] = zs
        img[iy, ix] = col
    return np.clip(np.round(img), 0, 255).astype(np.uint8)


def skin_rgb(m, i, palette):
    return palette[m['skins'][i]]


def size_m(m, frame=0):
    """The pose's length (x, front to back), width (y) and height (z) in metres."""
    V = m['frames'][frame]
    return tuple((V.max(0) - V.min(0)) * UNIT_M)


# --- what the game does with a model ----------------------------------------------------------

_PARAMS = r'\((?:[^()]|\(\))*\)'          # a parameter list, `void()` parameters in it
_FUNC = re.compile(r'^\s*(?:void|float|entity|vector|string)\s*(?:' + _PARAMS + r'\s*([A-Za-z_]\w*)\s*=|([A-Za-z_]\w*)\s*'
                   + _PARAMS + r'\s*(?:=\s*)?(?:\{|\[|$))')
_QUAKED = re.compile(r'/\*QUAKED\s+(\w+)')
_MODEL = re.compile(r'"(models/[\w/\-.]+\.mdl)"', re.I)
_CALL = re.compile(r'\b([A-Za-z_]\w*)\s*\(')
HELPER_MAX = 4          # a helper called by at most this many spawn functions is theirs (init_imp: the three imps)


def code_lines(text):
    """QuakeC's lines with their comments blanked: // to the end of the line, /* */ across lines,
    neither inside a string."""
    out, block = [], False
    for ln in text.splitlines():
        res, i, n, string = [], 0, len(ln), False
        while i < n:
            if block:
                j = ln.find('*/', i)
                if j < 0:
                    break
                block, i = False, j + 2
                continue
            ch = ln[i]
            if string:
                res.append(ch)
                string = ch != '"'
                i += 1
            elif ch == '"':
                res.append(ch)
                string = True
                i += 1
            elif ln.startswith('//', i):
                break
            elif ln.startswith('/*', i):
                block, i = True, i + 2
            else:
                res.append(ch)
                i += 1
        out.append(''.join(res))
    return out


def gamecode_users(repo=REPO, games=('h2', 'portals')):
    """model -> {(file, function, spawn class or '')}: the gamecode functions that name the model
    (outside comments and comparisons; precache.hc, which names every model, left out), each with
    the spawn class whose /*QUAKED*/ comment heads it, or, for a helper called by up to
    HELPER_MAX spawn functions, a row for each of them."""
    out = collections.defaultdict(set)
    calls = collections.defaultdict(set)
    spawners = set()
    for game in games:
        d = os.path.join(repo, 'gamecode', 'hc', game)
        if not os.path.isdir(d):
            continue
        for fn in sorted(os.listdir(d)):
            if not fn.lower().endswith('.hc') or fn.lower() == 'precache.hc':
                continue
            with open(os.path.join(d, fn), encoding='latin-1') as f:
                text = f.read()
            func, cls, pending = '', '', ''
            for raw, ln in zip(text.splitlines(), code_lines(text)):
                q = _QUAKED.search(raw)
                if q:
                    pending = q.group(1)
                mf = _FUNC.match(ln)
                if mf and not ln.rstrip().endswith(';'):
                    func = mf.group(1) or mf.group(2)
                    cls, pending = (pending if pending == func else ''), ''
                    if cls:
                        spawners.add(cls)
                    ln = ln[mf.end():]
                if func:
                    calls[func].update(c.group(1) for c in _CALL.finditer(ln))
                for mm in _MODEL.finditer(ln):
                    if not re.search(r'[=!]=\s*$', ln[:mm.start()]):      # a comparison doesn't use it
                        out[mm.group(1).lower()].add((fn, func, cls))
    callers = collections.defaultdict(set)
    for c in spawners:
        for f in calls[c]:
            callers[f].add(c)
    for us in out.values():
        for fn, func, cls in list(us):
            if not cls and 0 < len(callers.get(func, ())) <= HELPER_MAX:
                us.discard((fn, func, cls))
                us.update((fn, func, c) for c in callers[func])
    return out


def map_entities(data_dir):
    """From every map's entities: spawn class -> maps, puzzle id -> {(name, map)} (the puzzle
    pieces' netname)."""
    import bspviews
    by_class = collections.defaultdict(set)
    puzzles = collections.defaultdict(set)
    for name, (p, pos, ln) in sorted(bspviews.pak_files(data_dir).items()):
        if not name.endswith('.bsp'):
            continue
        mp = name[len('maps/'):-len('.bsp')]
        with open(p, 'rb') as f:
            f.seek(pos)
            b = f.read(ln)
        eo, el = struct.unpack_from('<ii', b, 4)
        for e in bspviews.parse_entities(b[eo:eo + el].decode('latin-1')):
            c = e.get('classname', '')
            by_class[c].add(mp)
            if c.startswith('puzzle') and e.get('puzzle_id'):
                puzzles[e['puzzle_id'].lower()].add((e.get('netname', ''), mp))
    return by_class, puzzles


def users_text(model, users, ents, limit_maps=6):
    """One line of what the game does with a model, for a label card."""
    by_class, puzzles = ents
    us = sorted(users.get(model, ()))
    classes = sorted({c for _, _, c in us if c})
    funcs = sorted({f'{fn[:-3]}.{func}' for fn, func, c in us if func and not c})
    parts = []
    if classes:
        parts.append('spawned as ' + ' '.join(classes[:6]) + (' ...' if len(classes) > 6 else ''))
    if funcs:
        parts.append('code ' + ' '.join(funcs[:5]) + (' ...' if len(funcs) > 5 else ''))
    maps = set()
    for c in classes:
        maps |= by_class.get(c, set())
    pid = os.path.splitext(os.path.basename(model))[0] if model.startswith('models/puzzle/') else None
    if pid and puzzles.get(pid):
        names = sorted({n for n, _ in puzzles[pid] if n})
        maps |= {mp for _, mp in puzzles[pid]}
        parts.append('puzzle item ' + ('"' + '", "'.join(names) + '"' if names else '(no name)'))
    if maps:
        ms = sorted(maps)
        parts.append(f'maps {" ".join(ms[:limit_maps])}' + (f' (+{len(ms) - limit_maps})' if len(ms) > limit_maps else ''))
    return ' | '.join(parts) or 'not set by the gamecode by name'


# --- the turntable sheet ----------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('model')
    ap.add_argument('--out', required=True)
    ap.add_argument('--skin', type=int, help="the model's own skin N (default 0 when no --png)")
    ap.add_argument('--png', nargs='+', help='skin images, label=path or path: a row each')
    ap.add_argument('--views', default='0,90,180', help='yaws in degrees (0: the front)')
    ap.add_argument('--frame', type=int, default=0)
    ap.add_argument('--pitch', type=float, default=10.0)
    ap.add_argument('--size', type=int, default=320)
    ap.add_argument('--smooth', action='store_true', help='bilinear texels (the game\'s), not nearest')
    ap.add_argument('--data', help='the game data folder (default: Hexenlicht-data beside the repository)')
    a = ap.parse_args()
    data = a.data or os.environ.get('HEXENLICHT_DATA') or os.path.join(os.path.dirname(REPO), 'Hexenlicht-data')
    files = model_files(data)
    rows = []
    if a.png:
        for spec in a.png:
            label, path = spec.split('=', 1) if '=' in spec and not os.path.exists(spec) else (os.path.basename(spec), spec)
            rows.append((label, np.asarray(Image.open(path).convert('RGB'))))
    m = load(files, a.model, max_frames=a.frame + 1)
    if a.frame >= len(m['frames']):
        raise SystemExit(f"{a.model} has {m['numframes']} poses: --frame {a.frame} is past them")
    if not a.png or a.skin is not None:
        from redraw import load_palette
        pal = load_palette(data)
        if pal is None:
            raise SystemExit(f'no data1\\pak0.pak in {data} (--data): the skin needs the palette')
        rows.insert(0, (f'skin {a.skin or 0}', skin_rgb(m, a.skin or 0, pal)))
    views = [float(v) for v in a.views.split(',')]
    S = a.size
    out = Image.new('RGB', (len(views) * (S + 4), len(rows) * (S + 18)), (20, 20, 20))
    d = ImageDraw.Draw(out)
    for r, (label, skin) in enumerate(rows):
        for c, yaw in enumerate(views):
            im = render(m, skin, a.frame, yaw, a.pitch, S, nearest=not a.smooth)
            out.paste(Image.fromarray(im), (c * (S + 4), r * (S + 18) + 16))
            d.text((c * (S + 4) + 4, r * (S + 18) + 2), f'{label}, yaw {yaw:g}', fill=(255, 255, 0))
    out.save(a.out)
    L, W, H = size_m(m, a.frame)
    print(f"{a.model} ({m['game']}{', RAPO' if m['rapo'] else ''}): {len(m['skins'])} skins {m['sw']}x{m['sh']}, "
          f"{len(m['tris'])} triangles, {m['numframes']} frames, {L:.2f} x {W:.2f} x {H:.2f} m -> {a.out}")
    return 0


if __name__ == '__main__':
    sys.exit(main())
