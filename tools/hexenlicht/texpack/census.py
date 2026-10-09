"""The texture census and the views of every world texture (story 9.2; README "Census and views",
TESTING.md "Texture census and views (9.2)", DECISIONS M54-M56). Run through texpack.py:

  texpack.py census       read every map of both games, write census.csv (a row per world,
                          liquid and sky texture of the export) and pick each texture's views
  texpack.py views        the views to shoot, as views_run.ps1's lines
  texpack.py checkviews   read views_run.ps1's runs: a view counts where `vk_materials here`
                          named its texture; its shots become PNGs; a miss gets the next face

A row: the texture as the export names it, the maps and area it covers (square units of its
drawn faces), how much is floor, wall or ceiling (the face's normal, |z| > 0.7), how much is on
which kind of brush entity, its scale (world units per texel, texinfo) and the size of its
typical face in tiles, how dark it usually is (its faces' lightmaps), the textures it meets at
shared edges (on the same plane, `beside`: trims and their walls; at an angle, `corner`),
its animation's frames, and its views (bspviews.py's picker) with their state.
"""
import collections
import csv
import math
import os
import re
import sys
import time

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import bspviews as B  # noqa: E402

COLUMNS = ['stem', 'name', 'kind', 'size', 'game', 'hubs', 'area', 'faces', 'maps', 'floor', 'wall', 'ceiling',
           'entities', 'scale', 'tiles', 'light', 'light_lo', 'light_hi', 'beside', 'corner', 'anim',
           'view', 'view_rank', 'view_map', 'view_close', 'view_wide', 'view_hit']
KINDS = ('world', 'liquid', 'sky')
NEIGHBOURS = 6              # the textures listed in beside and corner
MAX_RANK = 8                # the faces a texture's view is tried at before it is given up
# the hubs by their maps' names; a map no pattern matches is 'other' (the mission pack's monsters, thomas)
HUBS = (('blackmarsh', r'demo[1-3]|village\d|rider1a'), ('mazaera', r'meso\d|rider2c'), ('thysis', r'egypt\d'),
        ('septimus', r'romeric\d'), ('cathedral', r'cath|tower|castle\d|eidolon'), ('deathmatch', r'ravdm\d'),
        ('tulku', r'tibet\d+'), ('keep', r'keep\d'))


def default_census():
    return os.path.join(HERE, 'census.csv')


def hub_of(mapname):
    for hub, pattern in HUBS:
        if re.fullmatch(pattern, mapname):
            return hub
    return 'other'


def orientation(n):
    return 'floor' if n[2] > 0.7 else 'ceiling' if n[2] < -0.7 else 'wall'


def texel_scale(tx):
    """World units per texel along s and t (texinfo's vectors are texels per unit)."""
    return 1.0 / max(1e-6, float(np.linalg.norm(tx[0:3]))), 1.0 / max(1e-6, float(np.linalg.norm(tx[4:7])))


def tile_extent(verts, tx, w, h):
    """How many tiles of the texture a face spans along s and t."""
    s = verts @ np.array(tx[0:3]) + tx[3]
    t = verts @ np.array(tx[4:7]) + tx[7]
    return float(s.max() - s.min()) / max(1, w), float(t.max() - t.min()) / max(1, h)


def edge_key(a, b):
    """A polygon edge by its ends (to a tenth of a unit), either direction."""
    ka, kb = tuple(round(float(x), 1) for x in a), tuple(round(float(x), 1) for x in b)
    return (ka, kb) if ka <= kb else (kb, ka)


def neighbours(edges):
    """(beside, corner): {stem: Counter(other stem: shared length)} from edges, a dict of edge
    key -> [(stem, normal, length)]: faces on the same plane (normals within 8 degrees) are
    beside each other, faces at an angle meet at a corner."""
    beside, corner = collections.defaultdict(collections.Counter), collections.defaultdict(collections.Counter)
    for lst in edges.values():
        for i in range(len(lst)):
            for j in range(i + 1, len(lst)):
                (s1, n1, ln), (s2, n2, _) = lst[i], lst[j]
                if s1 == s2:
                    continue
                d = beside if float(np.dot(n1, n2)) > 0.99 else corner
                d[s1][s2] += ln
                d[s2][s1] += ln
    return beside, corner


def fmt(x):
    """A number for the table: whole above 10, one decimal above 1, two below."""
    if x >= 10:
        return str(int(round(x)))
    return f'{x:.1f}'.rstrip('0').rstrip('.') if x >= 1 else f'{x:.2f}'.rstrip('0').rstrip('.') or '0'


def fmt_pair(a, b):
    a, b = fmt(a), fmt(b)
    return a if a == b else f'{a}x{b}'


class Stats:
    def __init__(self):
        self.area = 0.0
        self.faces = 0
        self.maps = collections.Counter()
        self.games = set()
        self.hubs = collections.Counter()
        self.orient = collections.Counter()
        self.cats = collections.Counter()
        self.scale, self.tiles, self.light = [], [], []      # (value(s), area)
        self.by_map = collections.defaultdict(list)          # map -> [(model, face, area, light, category)]
        self.hidden = collections.Counter()                  # area on brush entities the game doesn't draw, by class

    def add(self, mapname, game, mi, fi, area, n, light, cat, scale, tiles):
        self.area += area
        self.faces += 1
        self.maps[mapname] += area
        self.games.add(game)
        self.hubs[hub_of(mapname)] += area
        self.orient[orientation(n)] += area
        self.cats[cat] += area
        self.scale.append((scale, area))
        self.tiles.append((tiles, area))
        if light is not None:
            self.light.append((light, area))
        self.by_map[mapname].append((mi, fi, area, light, cat))


class Groups:
    """Union-find over stems: the frames of an animation (`+0name`.. `+9name`, `+aname`.. in one BSP)."""
    def __init__(self):
        self.parent = {}

    def find(self, s):
        self.parent.setdefault(s, s)
        while self.parent[s] != s:
            self.parent[s] = self.parent[self.parent[s]]
            s = self.parent[s]
        return s

    def union(self, members):
        roots = [self.find(m) for m in members]
        for r in roots[1:]:
            self.parent[r] = roots[0]

    def members(self):
        out = collections.defaultdict(set)
        for s in self.parent:
            out[self.find(s)].add(s)
        return {s: out[self.find(s)] for s in self.parent if len(out[self.find(s)]) > 1}


def scan(maps, entries, qualified, log=print):
    """Every drawn face of every map into Stats per stem; the animations; the neighbours."""
    stats = {s: Stats() for s, e in entries.items() if e.kind in KINDS}
    groups = Groups()
    beside, corner = collections.defaultdict(collections.Counter), collections.defaultdict(collections.Counter)
    unknown = collections.Counter()
    for name in maps.names():
        bsp = maps.get(name)
        game = maps.game(name)
        frames = collections.defaultdict(set)
        for ti, tex in enumerate(bsp.textures):
            if tex and tex[0].startswith('+') and len(tex[0]) > 2:
                frames[tex[0][2:]].add(bsp.stem(ti, qualified))
        for members in frames.values():
            groups.union(sorted(members))
        edges = collections.defaultdict(list)
        for mi, mdl in enumerate(bsp.models):
            for fi in range(mdl[18], mdl[18] + mdl[19]):
                tx = bsp.texinfo[bsp.faces[fi][4]]
                stem = bsp.stem(tx[8], qualified)
                if stem not in stats:
                    unknown[stem] += 1
                    continue
                v = bsp.vertices(fi)
                area = B.polygon_area(v)
                if not bsp.drawn[mi]:
                    stats[stem].hidden[bsp.classname[mi]] += area
                    continue
                n = bsp.face_normal(fi)
                tex = bsp.textures[tx[8]]
                stats[stem].add(name, game, mi, fi, area, n, bsp.light(fi, v), B.category(bsp.classname[mi]),
                                texel_scale(tx), tile_extent(v, tx, tex[1], tex[2]))
                for k in range(len(v)):
                    a, b = v[k], v[(k + 1) % len(v)]
                    edges[(mi,) + edge_key(a, b)].append((stem, n, float(np.linalg.norm(b - a))))
        bs, co = neighbours(edges)
        for s, c in bs.items():
            beside[s].update(c)
        for s, c in co.items():
            corner[s].update(c)
    if unknown:
        log(f"faces whose texture the export doesn't list: {sum(unknown.values())} ({', '.join(sorted(str(k) for k in unknown)[:8])})")
    return stats, groups.members(), beside, corner


def map_order(st):
    """A texture's maps, most area first."""
    return [m for m, _ in sorted(st.maps.items(), key=lambda kv: (-kv[1], kv[0]))]


def pick(maps, st, rank):
    """The rank-th (1-based) view of a texture: (map, close, wide) or None."""
    k = 0
    for name, mi, fi, close, wide in B.candidate_views(maps, [(m, st.by_map[m]) for m in map_order(st)]):
        k += 1
        if k == rank:
            return name, close, wide
        if k >= MAX_RANK:
            break
    return None


def row_of(stem, e, st, group, beside, corner):
    r = {c: '' for c in COLUMNS}
    r.update(stem=stem, name=e.name, kind=e.kind, size=f'{e.w}x{e.h}', area=str(int(round(st.area))), faces=str(st.faces))
    r['game'] = ' '.join(sorted(st.games))
    r['hubs'] = ' '.join(h for h, _ in sorted(st.hubs.items(), key=lambda kv: (-kv[1], kv[0])))
    r['maps'] = ' '.join(f'{m}:{int(round(st.maps[m]))}' for m in map_order(st))
    if st.area > 0:
        for o in ('floor', 'wall', 'ceiling'):
            r[o] = str(int(round(100 * st.orient[o] / st.area)))
        ent = [(c, 100 * a / st.area) for c, a in st.cats.items() if c != 'world' and a > 0]
        r['entities'] = ' '.join(f'{c}:{fmt(p)}' for c, p in sorted(ent, key=lambda t: (-t[1], t[0])))
        w = [a for _, a in st.scale]
        r['scale'] = fmt_pair(B.wpercentile([s[0] for s, _ in st.scale], w, 50), B.wpercentile([s[1] for s, _ in st.scale], w, 50))
        r['tiles'] = fmt_pair(B.wpercentile([t[0] for t, _ in st.tiles], w, 50), B.wpercentile([t[1] for t, _ in st.tiles], w, 50))
    if st.light:
        v, w = [x for x, _ in st.light], [a for _, a in st.light]
        r['light'] = str(int(round(sum(x * a for x, a in st.light) / max(1e-9, sum(w)))))
        r['light_lo'], r['light_hi'] = str(int(round(B.wpercentile(v, w, 10)))), str(int(round(B.wpercentile(v, w, 90))))
    r['beside'] = ' '.join(f'{s}:{int(round(n))}' for s, n in beside.get(stem, collections.Counter()).most_common(NEIGHBOURS))
    r['corner'] = ' '.join(f'{s}:{int(round(n))}' for s, n in corner.get(stem, collections.Counter()).most_common(NEIGHBOURS))
    if group:
        r['anim'] = ' '.join(sorted(group))
    return r


def load(path):
    if not os.path.exists(path):
        return {}
    with open(path, newline='', encoding='utf-8') as f:
        return {r['stem']: r for r in csv.DictReader(f)}


def save(path, rows):
    with open(path, 'w', newline='', encoding='utf-8') as f:
        w = csv.DictWriter(f, COLUMNS, lineterminator='\r\n' if os.name == 'nt' else '\n')
        w.writeheader()
        for s in sorted(rows):
            w.writerow({c: rows[s].get(c, '') for c in COLUMNS})


def set_view(r, v, rank):
    """A row's view columns for the rank-th view v (or none left)."""
    r['view_rank'] = str(rank)
    if v is None:
        r.update(view='none', view_map='', view_close='', view_wide='')
    else:
        r.update(view='picked', view_map=v[0], view_close=B.spot_text(v[1]), view_wide=B.spot_text(v[2]))


def run_census(data, export, out, reset=False, log=print):
    """census.csv from the maps; a view checked before at the same spots keeps its state, each
    texture keeps its rank (the face it is at) unless reset."""
    import pipeline
    t0 = time.time()
    entries = pipeline.load_export(export)
    _, qualified = B.stem_names(export)
    maps = B.Maps(data)
    stats, groups, beside, corner = scan(maps, entries, qualified, log)
    log(f"{len(maps.files)} maps read in {time.time() - t0:.0f} s")
    old = load(out)
    rows = {}
    counts = collections.Counter()
    for stem, st in sorted(stats.items(), key=lambda kv: (map_order(kv[1])[:1], kv[0])):    # the views map by map
        e = entries[stem]
        r = row_of(stem, e, st, groups.get(stem), beside, corner)
        if e.kind == 'sky':
            r['view'] = 'sky'
        elif not st.faces:
            seen = any(stats[g].faces for g in groups.get(stem, ()) if g in stats)
            r['view'] = 'frame' if seen else 'unused'
            if st.hidden:
                r['view_hit'] = 'only on ' + ' '.join(sorted(st.hidden))
        else:
            o = {} if reset else old.get(stem, {})
            rank = int(o.get('view_rank') or 1)
            set_view(r, pick(maps, st, rank), rank)
            if r['view'] == 'picked' and o.get('view_close') == r['view_close'] and o.get('view_wide') == r['view_wide'] \
                    and o.get('view_map') == r['view_map'] and o.get('view') in ('ok', 'picked'):
                r['view'], r['view_hit'] = o['view'], o.get('view_hit', '')      # checked before at the same spots
            elif r['view'] == 'none' and o.get('view') == 'none':
                r['view_hit'] = o.get('view_hit', '')       # why the last face it got to missed
        counts[r['view']] += 1
        rows[stem] = r
    save(out, rows)
    log(f"census: {len(rows)} textures into {out}, {time.time() - t0:.0f} s; views: " +
        ', '.join(f'{n} {k}' for k, n in sorted(counts.items())))
    return 0


# -- the views to shoot, and their check ----------------------------------------

def view_lines(rows, todo=False, game=None, hub=None, stems=None, game_of=None):
    """views_run.ps1's lines: `<stem> <map> <close: x y z pitch yaw> <wide: x y z pitch yaw>`, by
    map; game: only views in that game's maps (game_of: map -> data1 or portals)."""
    out = []
    for s, r in rows.items():
        if not r['view_close'] or (todo and r['view'] != 'picked'):
            continue
        if game and game_of(r['view_map']) != game:
            continue
        if hub and hub_of(r['view_map']) != hub:
            continue
        if stems and s not in stems:
            continue
        out.append((r['view_map'], s, f"{s} {r['view_map']} {r['view_close']} {r['view_wide']}"))
    return [ln for _, _, ln in sorted(out)]


HERE_LINE = re.compile(r'^here: (.*), (\d+) units away: (.*) \(material \d+\)\s*$')
FILES_LINE = re.compile(r'^\s*files: textures/(\S+?)~([0-9a-f]{4}) \(this')


def parse_log(lines):
    """(name, 'close'|'wide') -> {'name': material file name or None, 'crc', 'what'} from a
    run's log: the lines after each `T92_VIEW <name> <which>` up to the next marker."""
    out = {}
    cur = None
    for ln in lines:
        if ln.startswith('T92_'):
            t = ln.split()
            cur = (t[1], t[2]) if t[0] == 'T92_VIEW' and len(t) >= 3 else None
            if cur:
                out[cur] = {'name': None, 'crc': None, 'what': 'nothing'}
            continue
        if not cur:
            continue
        m = HERE_LINE.match(ln)
        if m:
            out[cur]['what'] = f'{m.group(1)}: {m.group(3)}'
        elif ln.startswith('vk_materials here:'):
            out[cur]['what'] = ln.split(':', 1)[1].strip()
        m = FILES_LINE.match(ln)
        if m and out[cur]['name'] is None:
            out[cur]['name'], out[cur]['crc'] = m.group(1), m.group(2)
    return out


def hit_matches(stem, hit, group=None):
    """The hit is the texture: its name, and its CRC where the stem is qualified; for an
    animation any frame of it (the one shown now)."""
    if not hit or not hit.get('name'):
        return False
    for s in (group or {stem}) | {stem}:
        n, _, c = s.partition('~')
        if hit['name'] == n and (not c or c == hit['crc']):
            return True
    return False


def _png(src, dst):
    from PIL import Image
    with Image.open(src) as im:
        im.convert('RGB').save(dst)


def check_runs(runs, census, shots, data, export, log=print):
    import pipeline
    rows = load(census)
    os.makedirs(shots, exist_ok=True)
    maps = stats = None
    counts = collections.Counter()
    for run in runs:
        with open(os.path.join(run, 'views.csv'), newline='', encoding='utf-8') as f:
            shot = list(csv.DictReader(f))
        with open(os.path.join(run, 'debug_h2.log'), encoding='latin-1') as f:
            hits = parse_log(f.read().splitlines())
        for v in shot:
            r = rows.get(v['stem'])
            if not r or r['view_close'] != v['close'] or r['view_wide'] != v['wide'] or r['view_map'] != v['map']:
                counts['stale'] += 1        # the census has moved on (a later pick)
                continue
            if r['view'] != 'picked':
                counts['checked before'] += 1       # an ok view isn't taken back by a later run
                continue
            close, wide = hits.get((v['name'], 'close')), hits.get((v['name'], 'wide'))
            if close is None or close['what'] == 'no map':
                counts['not shot'] += 1     # the run ended before it, or its map didn't load
                continue
            group = set(r['anim'].split()) if r['anim'] else None
            if hit_matches(v['stem'], close, group):
                r['view'] = 'ok'
                r['view_hit'] = '' if hit_matches(v['stem'], wide, group) else f"wide: {wide['what'] if wide else 'not shot'}"
                for which in ('close', 'wide', 'albedo'):
                    src = os.path.join(run, f"{v['name']}_{which}.tga")
                    if os.path.exists(src):
                        _png(src, os.path.join(shots, f"{v['stem']}_{which}.png"))
                counts['ok' if not r['view_hit'] else 'ok, the wide missed'] += 1
                continue
            if maps is None:        # a miss: the next face, from the maps
                entries = pipeline.load_export(export)
                _, qualified = B.stem_names(export)
                maps = B.Maps(data)
                stats, _, _, _ = scan(maps, {s: entries[s] for s in entries if s in rows}, qualified, log)
            rank = int(r['view_rank'] or 1) + 1
            nxt = pick(maps, stats[v['stem']], rank) if rank <= MAX_RANK else None
            set_view(r, nxt, rank)
            r['view_hit'] = f"rank {rank - 1}: {close['what']}"
            counts['miss, next face' if nxt else 'miss, none left'] += 1
    save(census, rows)
    log('checkviews: ' + ', '.join(f'{n} {k}' for k, n in sorted(counts.items())))
    left = collections.Counter(r['view'] for r in rows.values())
    log('the census now: ' + ', '.join(f'{n} {k}' for k, n in sorted(left.items())))
    return 0
