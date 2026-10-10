"""The art direction's labels (story 9.3, E9; README "Labels", DECISIONS M57-): what every
world texture and skin is, written by Claude from 9.2's census and shots, then reviewed, and
the owner's answers. The manifest's columns `purpose`, `tier`, `family` and `regions`
(manifest.py) are checked here against their vocabularies (classes.toml's [labels] and
[materials]) and the palette:

  regions   which of the palette's ramps is which material: Raven shaded with ramps of 16
            colors (a few of 8, 15 and 32) and spread a material over several of them, so a
            region is a set of ramps (`grey,taupe=iron;orange,amber=wood`); `*` is every ramp
            not named (`*=stone`: all of it; `*=stone;yellow=gold`); `a/b` where one ramp
            carries two materials, the larger first. Every redrawn texture has them (tiers
            reimagine, layout, faithful, skin), so that 9.4 knows each texel's material.

Run through texpack.py:

  texpack.py cards      the pages a labeler reads: per texture the original, its ramp map,
                        9.2's close, wide and albedo shots, and its census facts as text;
                        per skin the atlas, its ramp map, its model drawn with it from three
                        sides (mdlview.py) and what the game does with the model
  texpack.py families   candidate families: textures that share texels (Raven's variants of
                        one texture), meet at long edges, or have the same ramps in one hub
  texpack.py labels     `check` the labels, `apply` a labels CSV to the manifest
"""
import collections
import csv
import fnmatch
import io
import math
import os
import struct
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import manifest  # noqa: E402

# The palette's ramps (gfx/palette.lmp): indices 0-31 one grey ramp from black to white; then
# ramps of 16, but row 12 is two of 8 and row 14 one of 15 and a single purple; 240-254 the
# saturated colors (DECISIONS M49: glowing eyes, gems, kept as drawn); 255 white (a hole in a
# masked texture). Hue names, not materials', so that `yellow=gold` reads.
RAMPS = (('grey', 0, 32), ('slate', 32, 48), ('taupe', 48, 64), ('sage', 64, 80), ('sand', 80, 96),
         ('green', 96, 112), ('orange', 112, 128), ('red', 128, 144), ('violet', 144, 160),
         ('yellow', 160, 176), ('olive', 176, 192), ('maroon', 192, 200), ('brown', 200, 208),
         ('tan', 208, 224), ('amber', 224, 239), ('purple', 239, 240), ('special', 240, 255))
RAMP_NAMES = tuple(r[0] for r in RAMPS)
# false colors of the ramp map, one per ramp, far apart
FALSE = ((150, 150, 150), (70, 110, 230), (140, 95, 70), (130, 200, 150), (235, 200, 110), (20, 150, 40),
         (255, 120, 0), (230, 20, 40), (170, 90, 255), (255, 240, 0), (160, 190, 20), (150, 30, 90),
         (95, 50, 15), (255, 225, 190), (205, 120, 70), (255, 0, 255), (0, 255, 255))
LABELED = ('claude', 'human')
UNIT_M = 0.031          # a world unit in metres (the player, 56 units, is about 1.75 m)


def ramp_of_index():
    t = np.zeros(256, np.int32)
    for i, (_, a, b) in enumerate(RAMPS):
        t[a:b] = i
    t[255] = 0
    return t


def texel_ramps(rgba, palette):
    """The ramp of every texel (RAMPS' index), -1 where it is transparent or off the palette (a
    duplicate color counts as its first index, as the engine's own lookups)."""
    rgb = rgba[..., :3]
    key = rgb[..., 0].astype(np.int64) << 16 | rgb[..., 1].astype(np.int64) << 8 | rgb[..., 2]
    pk = palette[:, 0].astype(np.int64) << 16 | palette[:, 1].astype(np.int64) << 8 | palette[:, 2]
    first = {}
    for i, k in enumerate(pk.tolist()):
        first.setdefault(k, i)
    u, inv = np.unique(key, return_inverse=True)
    idx = np.array([first.get(k, -1) for k in u.tolist()], np.int64)[inv].reshape(key.shape)
    r = np.where(idx >= 0, ramp_of_index()[np.clip(idx, 0, 255)], -1)
    if rgba.shape[-1] == 4:
        r = np.where(rgba[..., 3] > 0, r, -1)
    return r


def shares(ramps):
    """Each ramp's share of the opaque texels."""
    v = ramps[ramps >= 0]
    c = np.bincount(v.ravel(), minlength=len(RAMPS)).astype(np.float64)
    return {RAMP_NAMES[i]: c[i] / max(1, v.size) for i in range(len(RAMPS)) if c[i]}


def shares_text(sh, floor=0.02):
    return ' '.join(f'{n} {round(v * 100)}' for n, v in sorted(sh.items(), key=lambda kv: -kv[1]) if v >= floor)


def ramp_map(ramps):
    pal = np.array(FALSE + ((24, 24, 24),), np.uint8)
    return pal[np.where(ramps >= 0, ramps, len(FALSE))]


def parse_regions(text):
    """`grey,taupe=iron;orange,amber=wood` -> [(('grey', 'taupe'), ('iron',)), ...]; the ramp `*`
    is every ramp not named; a material `iron/stone` is a ramp carrying both, the larger first."""
    out = []
    for part in text.split(';'):
        part = part.strip()
        if not part:
            continue
        if '=' not in part:
            raise ValueError(f"region '{part}' is not ramps=material")
        ramps, mat = part.split('=', 1)
        out.append((tuple(r.strip() for r in ramps.split(',') if r.strip()), tuple(m.strip() for m in mat.split('/'))))
    return out


def vocab(classes):
    lab = classes.get('labels', {})
    return (set(lab.get('purposes_world', [])), set(lab.get('purposes_skin', [])), set(lab.get('tiers', [])),
            set(classes.get('materials', {})))


# the tiers whose class is fixed, and the classes only those tiers take
TIER_CLASS = {'glass': {'glass'}, 'liquid': {'liquid'}, 'lava': {'lava'}, 'skip': {'skip'}, 'fx': {'fx'},
              'skin': {'skin', 'skin_metal'}}
CLASS_TIER = {c: t for t, cs in TIER_CLASS.items() for c in cs}
REGIONS_NEEDED = ('reimagine', 'layout', 'faithful', 'skin')


def check_row(stem, kind, r, sh, classes, families):
    """The problems of one texture's labeled row (r: the manifest row; sh: its ramp shares)."""
    pw, ps, tiers, mats = vocab(classes)
    p = []
    purposes = ps if kind == 'skin' else pw
    if r['purpose'] not in purposes:
        p.append(f"purpose '{r['purpose']}' is not one of {kind == 'skin' and 'purposes_skin' or 'purposes_world'}")
    if r['tier'] not in tiers:
        p.append(f"tier '{r['tier']}' is not one of the tiers")
    cls = r['class']
    if cls and cls not in classes['class']:
        p.append(f"class '{cls}' is not in classes.toml")
    if cls and r['tier'] in TIER_CLASS and cls not in TIER_CLASS[r['tier']]:
        p.append(f"tier {r['tier']} takes class {'/'.join(sorted(TIER_CLASS[r['tier']]))}, not {cls}")
    if cls in CLASS_TIER and r['tier'] != CLASS_TIER[cls]:
        p.append(f"class {cls} goes with tier {CLASS_TIER[cls]}, not {r['tier']}")
    if r['tier'] != 'skip' and not r['description']:
        p.append('no description')
    try:
        manifest.parse_overrides(r['overrides'])
    except ValueError as e:
        p.append(str(e))
    if not r['regions'] and r['tier'] in REGIONS_NEEDED:
        p.append('no regions (`*=<material>` for a texture of one material)')
    if r['regions']:
        try:
            regs = parse_regions(r['regions'])
        except ValueError as e:
            regs = []
            p.append(str(e))
        seen = set()
        for ramps, mat in regs:
            if not ramps or not all(mat):
                p.append(f"region '{','.join(ramps)}={'/'.join(mat)}' needs ramps and a material")
            for m in mat:
                if m not in mats:
                    p.append(f"material '{m}' is not in classes.toml's [materials]")
            for rn in ramps:
                if rn == '*':
                    if len(ramps) > 1:
                        p.append("'*' stands alone: `*=<material>`")
                    elif '*' in seen:
                        p.append("'*' named twice")
                    seen.add('*')
                elif rn not in RAMP_NAMES:
                    p.append(f"'{rn}' is not a ramp")
                elif rn in seen:
                    p.append(f"ramp {rn} named twice")
                elif sh.get(rn, 0) < 0.01:
                    p.append(f"ramp {rn} is {sh.get(rn, 0) * 100:.1f} % of the texture")
                seen.add(rn)
        left = [f'{n} {v * 100:.0f} %' for n, v in sh.items() if v >= 0.05 and n not in seen and n != 'special' and '*' not in seen]
        if left:
            p.append('ramps without a material: ' + ', '.join(left))
    if r['family'] and r['family'] not in families:
        p.append(f"family '{r['family']}' is not in families.csv")
    return p


def load_families(path):
    if not os.path.exists(path):
        return {}
    return {r['family']: r for r in manifest.read_rows(path)[1] if r.get('family')}


def labeled_row(stem, rows):
    """The texture's own labeled row (a literal pattern with a tier; the owner's over Claude's)."""
    best = None
    for r in rows:
        if r['pattern'] == stem and r['tier'] and r['source'] in LABELED:
            if best is None or (r['source'] == 'human' and best['source'] != 'human'):
                best = r
    return best


def check(entries, sel, rows, classes, palette, ex, census, families):
    """Every selected texture's labeled row and its problems; an animation's frames labeled
    alike; textures of the same pixels (the mission pack's copies under other names) labeled
    alike, animations aside (their first frames can match); a family's lead one of its
    members. Returns (labeled, problems by stem)."""
    probs = collections.defaultdict(list)
    lab = {}
    twins = collections.defaultdict(list)
    for e in sel:
        r = labeled_row(e.stem, rows)
        if r is None:
            probs[e.stem].append('not labeled')
            continue
        lab[e.stem] = r
        rgba = np.asarray(Image.open(os.path.join(ex, e.file)).convert('RGBA'))
        probs[e.stem] += check_row(e.stem, e.kind, r, shares(texel_ramps(rgba, palette)), classes, families)
        if not census.get(e.stem, {}).get('anim'):
            twins[(rgba.shape, rgba.tobytes())].append(e.stem)
    keys = ('purpose', 'tier', 'class', 'family', 'regions', 'description', 'overrides')
    for g in twins.values():
        for s in g[1:]:
            diff = [k for k in keys if lab[s][k] != lab[g[0]][k]]
            if diff:
                probs[s].append(f"the same pixels as {g[0]}, labeled otherwise: {', '.join(diff)}")
    done = set()
    for stem, c in census.items():
        frames = [s for s in c.get('anim', '').split() if s in lab]
        if len(frames) > 1 and frames[0] not in done:
            done.update(frames)
            first = lab[frames[0]]
            for s in frames[1:]:
                diff = [k for k in keys if lab[s][k] != first[k]]
                if diff:
                    probs[s].append(f"frame of {frames[0]}'s animation labeled otherwise: {', '.join(diff)}")
    # a family's members among all labeled rows (a check of a few textures sees the others too)
    members = collections.defaultdict(list)
    for r in rows:
        if r.get('tier') and r.get('source') in LABELED and r['family'] and labeled_row(r['pattern'], rows) is r:
            members[r['family']].append(r['pattern'])
    for fam, f in families.items():
        if f.get('lead') and f['lead'] not in members.get(fam, []):
            probs[f['lead']].append(f"lead of family {fam} is not one of its members ({len(members.get(fam, []))})")
    return lab, {s: p for s, p in probs.items() if p}


def apply(labels_path, manifest_path, entries, classes, census=None):
    """A labels CSV (pattern = a stem, the manifest's columns; a `question` column is ignored)
    into the manifest as literal rows with source `claude` (unless it says `human`): the class
    and overrides the texture had are folded in (a label's own overrides win), so that a
    labeled row stands alone (manifest.resolve takes only it); an animation's other frames
    (the census's `anim`) without a row of their own get a copy; then the rows that matched a texture labeled now and match no unlabeled one are
    dropped. A labeled row of the owner's (`human`) is kept unless the CSV's row is `human` too.
    Returns (rows applied, rows dropped, the owner's rows kept, old human rows folded in)."""
    rows = manifest.load_manifest(manifest_path)
    new = manifest.load_manifest(labels_path)
    have = {r['pattern'] for r in new}
    for r in list(new):
        for s in ((census or {}).get(r['pattern'], {}).get('anim') or '').split():
            if s not in have and s in entries:
                new.append(dict(r, pattern=s))
                have.add(s)
    unknown = [r['pattern'] for r in new if r['pattern'] not in entries]
    if unknown:
        raise SystemExit(f"not in the export: {', '.join(unknown[:10])}")
    old = [r for r in rows if not (r['tier'] and r['source'] in LABELED)]
    by = {r['pattern']: r for r in rows if r['tier'] and r['source'] in LABELED}
    owners = folded = applied = 0
    for r in new:
        e = entries[r['pattern']]
        prev = by.get(r['pattern'])
        if prev is not None and prev['source'] == 'human' and r['source'] != 'human':
            owners += 1
            continue
        applied += 1
        if prev is None:
            folded += any(x['source'] == 'human' for x in old if fnmatch.fnmatchcase(e.stem, x['pattern']))
            prev = manifest.resolve(e.stem, e.kind, old, classes)
            base = {'class': prev.cls, 'overrides': manifest.parse_overrides(';'.join(x['overrides'] for x in prev.rows))}
        else:
            base = {'class': prev['class'], 'overrides': manifest.parse_overrides(prev['overrides'])}
        over = dict(base['overrides'])
        over.update(manifest.parse_overrides(r['overrides']))
        by[r['pattern']] = dict(r, **{'class': r['class'] or base['class'],
                                      'overrides': ';'.join(f'{k}={v}' for k, v in over.items()),
                                      'source': r['source'] if r['source'] in LABELED else 'claude'})
    unlabeled = [s for s in entries if s not in by]
    kept = [r for r in old if not any(fnmatch.fnmatchcase(s, r['pattern']) for s in by)
            or any(fnmatch.fnmatchcase(s, r['pattern']) for s in unlabeled)]
    manifest.write_manifest(manifest_path, kept + list(by.values()))
    return applied, len(old) - len(kept), owners, folded


# --- the labeler's pages --------------------------------------------------------------------

def _font(size):
    for name in ('arial.ttf', 'DejaVuSans.ttf'):
        try:
            return ImageFont.truetype(name, size)
        except OSError:
            pass
    return ImageFont.load_default()


def _fit(im, w, h, nearest):
    s = min(w / im.width, h / im.height)
    if s >= 1:
        s = max(1, math.floor(s))
    size = (max(1, round(im.width * s)), max(1, round(im.height * s)))
    return im.resize(size, Image.NEAREST if nearest or s >= 1 else Image.LANCZOS)


def size_m(c, w, h):
    """The texture's size in the world in metres, by the census's scale (units per texel)."""
    sc = (c.get('scale') or '1').split('x')
    try:
        ss, st = float(sc[0]), float(sc[-1])
    except ValueError:
        ss = st = 1.0
    return w * ss * UNIT_M, h * st * UNIT_M


def facts(e, c, sh, res, related):
    """One line of census facts for the labeler."""
    if not c:
        return f"{e.stem} {e.w}x{e.h} {e.kind} | ramps {shares_text(sh)} | was {res.cls}: {res.description}"
    hubs = c['hubs'].split()
    maps = ' '.join(m.split(':')[0] for m in c['maps'].split()[:4])
    sw, sh_m = size_m(c, e.w, e.h)
    ent = f" | entities {c['entities']}" if c['entities'] else ''
    light = f" | light {c['light']} ({c['light_lo']}-{c['light_hi']})" if c['light'] else ''
    beside = ' '.join(c['beside'].split()[:4])
    corner = ' '.join(c['corner'].split()[:3])
    anim = f" | anim {c['anim']}" if c['anim'] else ''
    view = '' if c['view'] == 'ok' else f" | view {c['view']}"
    rel = f" | related {related_text(related)}" if related else ''
    return (f"{e.stem} {e.w}x{e.h} {e.kind} | home {hubs[0] if hubs else '-'} ({' '.join(hubs[1:]) or 'only'}) | maps {maps}"
            f" | floor {c['floor']} wall {c['wall']} ceiling {c['ceiling']}{ent} | scale {c['scale']}, about {sw:.1f} x {sh_m:.1f} m,"
            f" tiles {c['tiles']}{light} | beside {beside or '-'} | corner {corner or '-'}{anim}{view}"
            f" | ramps {shares_text(sh)} | was {res.cls}: {res.description}{rel}")


def cards(ex, sel, rows, classes, palette, census, views_dir, out_prefix, per_page=6, related=None):
    """Label pages: per texture its stem, the original (whole texels where it fits), the ramp
    map with each ramp's share, and 9.2's close, wide and albedo shots; a text file per page
    with the census facts. Writes <out_prefix>_NN.png and .txt (Raven's pixels: keep them local)."""
    related = related or {}
    W0, SH = 176, 176
    VW, VH = 300, 169
    CW, CH = 2 * W0 + 3 * VW + 4 * 8, 22 + SH + 20
    font, small = _font(15), _font(13)
    pages = (len(sel) + per_page - 1) // per_page
    for p in range(pages):
        batch = sel[p * per_page:(p + 1) * per_page]
        img = Image.new('RGB', (CW, len(batch) * (CH + 6)), (16, 16, 16))
        d = ImageDraw.Draw(img)
        lines = []
        for i, e in enumerate(batch):
            y = i * (CH + 6)
            rgba = np.asarray(Image.open(os.path.join(ex, e.file)).convert('RGBA'))
            rr = texel_ramps(rgba, palette)
            sh = shares(rr)
            orig = Image.alpha_composite(Image.new('RGBA', (rgba.shape[1], rgba.shape[0]), (70, 70, 70, 255)),
                                         Image.fromarray(rgba)).convert('RGB')
            img.paste(_fit(orig, W0, SH, True), (0, y + 22))
            img.paste(_fit(Image.fromarray(ramp_map(rr)), W0, SH, True), (W0 + 8, y + 22))
            x = 2 * W0 + 16
            for v in ('close', 'wide', 'albedo'):
                path = os.path.join(views_dir, f'{e.stem}_{v}.png')
                if os.path.exists(path):
                    img.paste(_fit(Image.open(path).convert('RGB'), VW, VH, False), (x, y + 22))
                else:
                    d.text((x + 8, y + 90), f'no {v} shot', fill=(120, 120, 120), font=small)
                x += VW + 8
            res = manifest.resolve(e.stem, e.kind, rows, classes)
            d.text((0, y + 2), f'{p * per_page + i + 1}. {e.stem}', fill=(255, 235, 60), font=font)
            d.text((W0 + 8 + 200, y + 3), f'{e.w}x{e.h}', fill=(170, 170, 170), font=small)
            lx = W0 + 8
            for n, v in sorted(sh.items(), key=lambda kv: -kv[1]):
                if v < 0.03:
                    continue
                t = f'{n} {round(v * 100)}'
                tw = d.textlength(t, font=small)
                if lx + 14 + tw > 2 * W0 + 3 * VW:
                    break
                d.rectangle((lx, y + 24 + SH, lx + 10, y + 34 + SH), fill=FALSE[RAMP_NAMES.index(n)])
                d.text((lx + 13, y + 22 + SH), t, fill=(220, 220, 220), font=small)
                lx += 13 + tw + 12
            lines.append(f'{p * per_page + i + 1}. ' + facts(e, census.get(e.stem), sh, res, related.get(e.stem)))
        img.save(f'{out_prefix}_{p + 1:02d}.png')
        with open(f'{out_prefix}_{p + 1:02d}.txt', 'w', encoding='utf-8', newline='\n') as f:
            f.write('\n'.join(lines) + '\n')
    return pages


def skin_model(stem):
    """A skin's model and index: models/imp.mdl_2 -> ('models/imp.mdl', 2); the export's `~crc`
    (a name with two versions: models/puzzle/scepter.mdl_0~4107) dropped."""
    name, _, i = stem.split('~')[0].rpartition('_')
    return name, int(i)


class Models:
    """The alias models behind the skins (mdlview.py): read from the paks once each, and what the
    game does with them (the gamecode's spawn classes and functions, the maps, puzzle names)."""
    def __init__(self, data):
        import mdlview
        self.mv = mdlview
        self.files = mdlview.model_files(data)
        self.users = mdlview.gamecode_users()
        self.ents = mdlview.map_entities(data)
        self.cache = {}

    def get(self, e):
        name, _ = skin_model(e.stem)
        game = e.paks[0].split('/')[0] if getattr(e, 'paks', None) else None
        key = (name, e.w, e.h, game)
        if key not in self.cache:
            try:
                self.cache[key] = self.mv.load(self.files, name, (e.w, e.h), game)
            except (KeyError, ValueError, struct.error):
                self.cache[key] = None
        return self.cache[key]

    def facts(self, e, sh, res, related):
        name, i = skin_model(e.stem)
        m = self.get(e)
        if m is None:
            return f"{e.stem} {e.w}x{e.h} skin | model {name} not read | ramps {shares_text(sh)} | was {res.cls}: {res.description}"
        L, W, H = self.mv.size_m(m)
        fl = ', '.join(self.mv.flag_names(m['flags']))
        rel = f" | related {related_text(related)}" if related else ''
        game = m['game']
        if game == 'data1' and any(v[0] == 'portals' for v in self.files.get(name, ())):
            game = "data1's, which the mission pack's replaces"
        return (f"{e.stem} {e.w}x{e.h} skin | model {name} ({game}), skin {i + 1} of {len(m['skins'])},"
                f" {len(m['tris'])} triangles, {m['numframes']} frames{', flags ' + fl if fl else ''}"
                f" | about {L:.1f} x {W:.1f} x {H:.1f} m (front to back, side to side, high)"
                f" | {self.mv.users_text(name, self.users, self.ents)}"
                f" | ramps {shares_text(sh)} | was {res.cls}: {res.description}{rel}")


def skin_cards(ex, sel, rows, classes, palette, models, out_prefix, per_page=4, related=None):
    """Label pages for skins: per skin its stem, the atlas (whole texels where it fits), the ramp
    map with each ramp's share, and the model drawn with it from the front, its left side and the
    back (mdlview.py: frame 0, unlit); a text file per page with the model's facts. Writes
    <out_prefix>_NN.png and .txt (Raven's pixels: keep them local)."""
    related = related or {}
    AW, AH, R = 300, 200, 200
    CW, CH = 2 * AW + 3 * R + 4 * 8, 22 + AH + 20
    font, small = _font(15), _font(13)
    pages = (len(sel) + per_page - 1) // per_page
    for p in range(pages):
        batch = sel[p * per_page:(p + 1) * per_page]
        img = Image.new('RGB', (CW, len(batch) * (CH + 6)), (16, 16, 16))
        d = ImageDraw.Draw(img)
        lines = []
        for i, e in enumerate(batch):
            y = i * (CH + 6)
            rgba = np.asarray(Image.open(os.path.join(ex, e.file)).convert('RGBA'))
            rr = texel_ramps(rgba, palette)
            sh = shares(rr)
            orig = Image.alpha_composite(Image.new('RGBA', (rgba.shape[1], rgba.shape[0]), (70, 70, 70, 255)),
                                         Image.fromarray(rgba)).convert('RGB')
            img.paste(_fit(orig, AW, AH, True), (0, y + 22))
            img.paste(_fit(Image.fromarray(ramp_map(rr)), AW, AH, True), (AW + 8, y + 22))
            m = models.get(e)
            x = 2 * AW + 16
            for yaw, what in ((0, 'front'), (90, 'left side'), (180, 'back')):
                if m is not None:
                    img.paste(Image.fromarray(models.mv.render(m, np.asarray(orig), 0, yaw, 10, R)), (x, y + 22))
                    d.text((x + 4, y + 24), what, fill=(150, 150, 150), font=small)
                else:
                    d.text((x + 8, y + 90), 'no model', fill=(120, 120, 120), font=small)
                x += R + 8
            res = manifest.resolve(e.stem, e.kind, rows, classes)
            d.text((0, y + 2), f'{p * per_page + i + 1}. {e.stem}', fill=(255, 235, 60), font=font)
            d.text((AW + 8 + 200, y + 3), f'{e.w}x{e.h}', fill=(170, 170, 170), font=small)
            lx = AW + 8
            for n, v in sorted(sh.items(), key=lambda kv: -kv[1]):
                if v < 0.03:
                    continue
                t = f'{n} {round(v * 100)}'
                tw = d.textlength(t, font=small)
                if lx + 14 + tw > CW:
                    break
                d.rectangle((lx, y + 24 + AH, lx + 10, y + 34 + AH), fill=FALSE[RAMP_NAMES.index(n)])
                d.text((lx + 13, y + 22 + AH), t, fill=(220, 220, 220), font=small)
                lx += 13 + tw + 12
            lines.append(f'{p * per_page + i + 1}. ' + models.facts(e, sh, res, related.get(e.stem)))
        img.save(f'{out_prefix}_{p + 1:02d}.png')
        with open(f'{out_prefix}_{p + 1:02d}.txt', 'w', encoding='utf-8', newline='\n') as f:
            f.write('\n'.join(lines) + '\n')
    return pages


# --- candidate families ---------------------------------------------------------------------

def candidates(ex, sel, palette, census, shared_min=0.2, beside_min=0.15, similar_min=0.97, skins=False):
    """Textures that may read as one material, per texture the strongest links: `texels` (the
    same palette index at the same place in a texture of the same size: Raven's variants of one
    texture; for skins, of the texels not black in both: any two atlases share their unused
    black), `beside` (their shared edges on one plane, as a fraction of the texture's own
    shared edges), `colors` (their ramp shares' cosine, same home hub; not for skins, which have
    no hub and whose shares are mostly the atlas's black). Returns {stem: [(other, kind,
    value)]}, the strongest first."""
    idx = {}
    sh = {}
    for e in sel:
        rgba = np.asarray(Image.open(os.path.join(ex, e.file)).convert('RGBA'))
        rgb = rgba[..., :3]
        key = rgb[..., 0].astype(np.int64) << 16 | rgb[..., 1].astype(np.int64) << 8 | rgb[..., 2]
        idx[e.stem] = key.ravel()
        sh[e.stem] = shares(texel_ramps(rgba, palette))
    links = collections.defaultdict(dict)
    by_size = collections.defaultdict(list)
    for e in sel:
        by_size[(e.w, e.h)].append(e.stem)
    for stems in by_size.values():
        if len(stems) < 2:
            continue
        A = np.stack([idx[s] for s in stems])
        for i, s in enumerate(stems):
            if skins:
                live = (A != 0) | (A[i] != 0)            # 0: black
                eq = ((A == A[i]) & live).sum(axis=1) / np.maximum(live.sum(axis=1), 1)
            else:
                eq = (A == A[i]).mean(axis=1)
            for j in np.nonzero(eq >= shared_min)[0]:
                if j != i:
                    links[s][stems[j]] = ('texels', float(eq[j]))
    for s in idx:
        c = census.get(s)
        if not c or not c.get('beside'):
            continue
        pairs = [(t.rsplit(':', 1)[0], float(t.rsplit(':', 1)[1])) for t in c['beside'].split()]
        tot = sum(v for _, v in pairs)
        for t, v in pairs:
            if t in idx and t != s and v / max(tot, 1) >= beside_min and t not in links[s]:
                links[s][t] = ('beside', v / tot)
    vec = {s: np.array([sh[s].get(n, 0) for n in RAMP_NAMES]) for s in idx}
    home = {s: (census.get(s, {}).get('hubs') or '-').split()[0] for s in idx}
    by_home = collections.defaultdict(list)
    for s in idx:
        by_home[home[s]].append(s)
    for stems in (by_home.values() if not skins else ()):
        V = np.stack([vec[s] / max(1e-9, np.linalg.norm(vec[s])) for s in stems])
        C = V @ V.T
        for i, s in enumerate(stems):
            order = np.argsort(-C[i])
            n = 0
            for j in order:
                if j == i or C[i, j] < similar_min or n >= 3:
                    continue
                if stems[j] not in links[s]:
                    links[s][stems[j]] = ('colors', float(C[i, j]))
                n += 1
    out = {}
    for s, l in links.items():
        items = sorted(l.items(), key=lambda kv: ({'texels': 0, 'beside': 1, 'colors': 2}[kv[1][0]], -kv[1][1]))[:6]
        out[s] = [(t, k, v) for t, (k, v) in items]
    return out


def related_text(links):
    return ' '.join(f'{t}({k} {v * 100:.0f})' for t, k, v in links)


def overview(ex, sel, rows, classes, out_prefix, cols=10, rows_per_page=6, cell=112):
    """Pages of the originals only, many to a page, each with its stem and, once labeled, its
    family and tier (else its class): to see a hub at once, or a family's members side by side.
    Writes <out_prefix>_NN.png."""
    font = _font(12)
    per = cols * rows_per_page
    pages = (len(sel) + per - 1) // per
    for p in range(pages):
        img = Image.new('RGB', (cols * (cell + 6), rows_per_page * (cell + 32)), (16, 16, 16))
        d = ImageDraw.Draw(img)
        for i, e in enumerate(sel[p * per:(p + 1) * per]):
            x, y = (i % cols) * (cell + 6), (i // cols) * (cell + 32)
            rgba = Image.open(os.path.join(ex, e.file)).convert('RGBA')
            im = Image.alpha_composite(Image.new('RGBA', rgba.size, (70, 70, 70, 255)), rgba).convert('RGB')
            img.paste(_fit(im, cell, cell, True), (x, y + 30))
            res = manifest.resolve(e.stem, e.kind, rows, classes)
            d.text((x, y + 1), e.stem[:18], fill=(255, 235, 60), font=font)
            sub = f'{res.family or "-"} {res.tier}' if res.tier else f'{e.w}x{e.h} {res.cls}'
            d.text((x, y + 15), sub[:20], fill=(150, 220, 255), font=font)
        img.save(f'{out_prefix}_{p + 1:02d}.png')
    return pages


def set_leads(path, rows, census, entries=None):
    """Each family without a lead gets the labeled member with the most area in the maps, or for
    skins (in no map) the biggest atlas (its comments and order kept). Returns the families
    given a lead."""
    entries = entries or {}
    with open(path, newline='', encoding='utf-8') as f:
        text = f.read()
    members = collections.defaultdict(list)
    for r in rows:
        if r['tier'] and r['source'] in LABELED and r['family']:
            members[r['family']].append(r['pattern'])
    out, done = [], []
    nl = '\r\n' if '\r\n' in text else '\n'
    for ln in text.splitlines():
        cells = next(csv.reader([ln]), []) if ln and not ln.startswith('# ') else []
        if len(cells) >= 4 and cells[0] != 'family' and not cells[2] and members.get(cells[0]):
            cells[2] = max(members[cells[0]], key=lambda s: (int((census.get(s) or {}).get('area') or 0),
                                                            entries[s].w * entries[s].h if s in entries else 0))
            done.append(cells[0])
            buf = io.StringIO()
            csv.writer(buf, lineterminator='').writerow(cells)
            ln = buf.getvalue()
        out.append(ln)
    with open(path, 'w', newline='', encoding='utf-8') as f:
        f.write(nl.join(out) + nl)
    return done
