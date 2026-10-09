"""Views of textures in the maps (story 9.1; the start of 9.2's census): per texture of a list,
a camera in front of its largest face that the camera can see, from the maps' BSPs in the
game's paks (BSP 29, Hexen II's models with 8 hulls).

A face's texture is named as the export names it: the BSP's name lowercased, `*` as `#`, and
`~<crc>` (the CRC-16 of mip 0's pixels, as tex_names.ps1 and the engine) where the export has
the name qualified. The camera looks at the face's middle along its normal from about 0.7 of
its size (48 to 220 units), floors and ceilings from the side at 55 degrees; its eye must be
in open space (the world's hull 0) and the line to the face clear of the world and of every
brush entity's box but triggers'. A brush entity with an origin brush (func_door_rotating:
qbsp stores its geometry around its `origin` key) is moved to that origin. Prints `vk_setpos`
lines (the player's origin: the eye is 50 units above it) as `<stem> <map> x y z pitch yaw`.

  python bspviews.py --data <game data> --export <export> --maps demo1,meso9 --stems a,b > views.txt
"""
import argparse
import math
import os
import re
import struct
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

EYE = 50.0                  # Hexen II's view_ofs: the eye above the player's origin
CONTENTS_EMPTY = -1


def crc16(data):
    crc = 0xffff
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xffff if crc & 0x8000 else (crc << 1) & 0xffff
    return crc


def pak_files(data_dir, pattern_prefix='maps/'):
    """name -> (pak path, offset, length) for every file under the prefix; later paks and
    the mission pack's folder win, as the game's search path."""
    out = {}
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
                if name.startswith(pattern_prefix):
                    out[name] = (p, pos, ln)
    return out


def parse_entities(text):
    """The entities lump as a list of key -> value dicts."""
    ents = []
    for block in re.findall(r'\{([^{}]*)\}', text):
        ents.append(dict(re.findall(r'"([^"]*)"\s+"([^"]*)"', block)))
    return ents


class Bsp:
    def __init__(self, b):
        ver = struct.unpack_from('<i', b, 0)[0]
        if ver != 29:
            raise ValueError(f'BSP version {ver}, not 29')
        L = [struct.unpack_from('<ii', b, 4 + 8 * i) for i in range(15)]

        def lump(i, fmt):
            ofs, ln = L[i]
            sz = struct.calcsize(fmt)
            return [struct.unpack_from(fmt, b, ofs + k * sz) for k in range(ln // sz)]
        self.planes = np.array([p[:4] for p in lump(1, '<4fi')])
        self.vertexes = np.array(lump(3, '<3f'))
        self.nodes = lump(5, '<i2h6h2H')
        self.texinfo = lump(6, '<8f2i')
        self.faces = lump(7, '<2hi2h4Bi')
        self.leafs = lump(10, '<2i6h2H4B')
        self.edges = lump(12, '<2H')
        self.surfedges = [s[0] for s in lump(13, '<i')]
        self.models = lump(14, '<9f8i3i')        # Hexen II: 8 hull heads
        # per brush model its entity's origin (an origin brush) and classname
        ofs, ln = L[0]
        ents = parse_entities(b[ofs:ofs + ln].split(b'\0')[0].decode('latin-1'))
        self.origin = np.zeros((len(self.models), 3))
        self.trigger = [False] * len(self.models)
        for e in ents:
            mm = re.fullmatch(r'\*(\d+)', e.get('model', ''))
            if mm and int(mm.group(1)) < len(self.models):
                i = int(mm.group(1))
                self.trigger[i] = e.get('classname', '').startswith('trigger_')
                if 'origin' in e:
                    self.origin[i] = [float(v) for v in e['origin'].split()[:3]]
        # the textures: name, size and where mip 0's pixels are (for the CRC, when a name needs it)
        self.bytes = b
        ofs, ln = L[2]
        self.textures = []
        if ln:
            n = struct.unpack_from('<i', b, ofs)[0]
            for i in range(n):
                mo = struct.unpack_from('<i', b, ofs + 4 + 4 * i)[0]
                if mo < 0:
                    self.textures.append(None)
                    continue
                m = ofs + mo
                name = b[m:m + 16].split(b'\0')[0].decode('latin-1').lower().replace('*', '#')
                w, h = struct.unpack_from('<2I', b, m + 16)
                self.textures.append((name, w, h, m + 40))
        self._crc = {}

    def crc(self, ti):
        if ti not in self._crc:
            _, w, h, at = self.textures[ti]
            self._crc[ti] = crc16(self.bytes[at:at + w * h])
        return self._crc[ti]

    def face_polygon(self, fi, model=0):
        f = self.faces[fi]
        vs = []
        for k in range(f[3]):
            se = self.surfedges[f[2] + k]
            e = self.edges[abs(se)]
            vs.append(self.vertexes[e[0] if se >= 0 else e[1]])
        return np.array(vs) + self.origin[model]

    def face_normal(self, fi):
        f = self.faces[fi]
        n = self.planes[f[0], :3].copy()
        return -n if f[1] else n

    def contents(self, p):
        node = self.models[0][9]        # hull 0's head
        while node >= 0:
            pl, c0, c1 = self.nodes[node][0], self.nodes[node][1], self.nodes[node][2]
            nrm, dist = self.planes[pl, :3], self.planes[pl, 3]
            node = c0 if float(np.dot(nrm, p)) - dist >= 0 else c1
        return self.leafs[-node - 1][0]

    def clear(self, a, b, step=4.0, own=-1):
        """The line a-b is open: no solid of the world (hull 0) on it and no brush entity's box
        across it (doors, windows, platforms where they stand at the start; not triggers, which
        aren't drawn; own: the face's model)."""
        d = np.linalg.norm(b - a)
        for t in np.arange(0.0, d, step):
            if self.contents(a + (b - a) * (t / d)) != CONTENTS_EMPTY:
                return False
        for mi in range(1, len(self.models)):
            if mi == own or self.trigger[mi]:
                continue
            o = self.origin[mi]
            if _segment_hits_box(a, b, np.array(self.models[mi][0:3]) + o - 1, np.array(self.models[mi][3:6]) + o + 1):
                return False
        return True


def _segment_hits_box(a, b, lo, hi):
    t0, t1 = 0.0, 1.0
    d = b - a
    for k in range(3):
        if abs(d[k]) < 1e-9:
            if a[k] < lo[k] or a[k] > hi[k]:
                return False
            continue
        u0, u1 = (lo[k] - a[k]) / d[k], (hi[k] - a[k]) / d[k]
        t0, t1 = max(t0, min(u0, u1)), min(t1, max(u0, u1))
        if t0 > t1:
            return False
    return True


def stem_names(export_dir):
    """The export's stems: a name qualified with ~crc there gets it here too."""
    import pipeline
    stems = set(pipeline.load_export(export_dir))
    qualified = {s.split('~')[0] for s in stems if '~' in s}
    return stems, qualified


def views_for(bsp, want, qualified):
    """stem -> (x, y, z, pitch, yaw, area) for the stems of want found in this map."""
    by_stem = {}
    for mi, mdl in enumerate(bsp.models):     # the world first, then the brush entities (doors, windows)
        if bsp.trigger[mi]:
            continue
        for fi in range(mdl[18], mdl[18] + mdl[19]):
            ti = bsp.texinfo[bsp.faces[fi][4]][8]
            tex = bsp.textures[ti] if ti < len(bsp.textures) else None
            if not tex:
                continue
            stem = f'{tex[0]}~{bsp.crc(ti):04x}' if tex[0] in qualified else tex[0]
            if stem in want:
                by_stem.setdefault(stem, []).append((mi, fi))
    out = {}
    for stem, fis in by_stem.items():
        cands = []
        for mi, fi in fis:
            poly = bsp.face_polygon(fi, mi)     # a brush entity where it stands at the start
            c = poly.mean(axis=0)
            area = 0.5 * np.linalg.norm(sum(np.cross(poly[i] - poly[0], poly[i + 1] - poly[0]) for i in range(1, len(poly) - 1)))
            cands.append((mi > 0, -area, mi, fi, c, poly))
        cands.sort(key=lambda t: t[:2])
        for _, area, mi, fi, c, poly in cands[:40]:
            area = -area
            n = bsp.face_normal(fi)
            size = float(np.max(poly.max(axis=0) - poly.min(axis=0)))
            dist = min(220.0, max(48.0, 0.7 * size))
            tries = []
            if abs(n[2]) > 0.7:     # a floor or ceiling: from the side, 55 degrees down or up
                for k in range(8):
                    yaw = k * 45.0
                    hd = np.array([math.cos(math.radians(yaw)), math.sin(math.radians(yaw)), 0.0])
                    tries.append(c + n * dist * math.sin(math.radians(55)) - hd * dist * math.cos(math.radians(55)))
            else:
                tries.append(c + n * dist)
                tries.append(c + n * dist * 0.6)
            for eye in tries:
                if bsp.contents(eye) != CONTENTS_EMPTY or not bsp.clear(eye, c + n * 2.0, own=mi):
                    continue
                d = c - eye
                d /= np.linalg.norm(d)
                pitch = -math.degrees(math.asin(max(-1.0, min(1.0, d[2]))))
                yaw = math.degrees(math.atan2(d[1], d[0]))
                out[stem] = (eye[0], eye[1], eye[2] - EYE, pitch, yaw, area)
                break
            if stem in out:
                break
    return out


def main(argv=None):
    import pipeline
    import texpack
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--data', default=None, help='the game data folder (default: as texpack.py\'s)')
    ap.add_argument('--export', default=None)
    ap.add_argument('--maps', help='comma-separated maps, in the order to visit them first; then the maps the export lists for each texture')
    ap.add_argument('--stems', help='comma-separated stems (default: every texture of the export, in every map that uses it)')
    a = ap.parse_args(argv)
    data = a.data or os.environ.get('HEXENLICHT_DATA') or os.path.join(os.path.dirname(texpack.REPO), 'Hexenlicht-data')
    export = a.export or texpack.default_export()
    stems, qualified = stem_names(export)
    want = set(a.stems.split(',')) if a.stems else stems
    files = pak_files(data)
    maps = [m for m in (a.maps or '').split(',') if m]      # these first, then each texture's own
    unknown = [m for m in maps if f'maps/{m}.bsp' not in files]
    if unknown:
        raise SystemExit(f"not in the paks of {data}: {', '.join(unknown)}")
    entries = pipeline.load_export(export)
    for s in sorted(want):
        for m in (entries[s].used_in if s in entries else []):
            if m not in maps and f'maps/{m}.bsp' in files:
                maps.append(m)
    found = set()
    for mapname in maps:
        if not want - found:
            break
        p, pos, ln = files[f'maps/{mapname}.bsp']
        with open(p, 'rb') as f:
            f.seek(pos)
            bsp = Bsp(f.read(ln))
        for stem, (x, y, z, pitch, yaw, area) in sorted(views_for(bsp, want - found, qualified).items()):
            print(f'{stem} {mapname} {round(x)} {round(y)} {round(z)} {round(pitch)} {round(yaw)}   # face {area:.0f} sq units')
            found.add(stem)
    for s in sorted(want - found) if a.stems else []:
        print(f'# {s}: no view in its maps')
    return 0


if __name__ == '__main__':
    sys.exit(main())
