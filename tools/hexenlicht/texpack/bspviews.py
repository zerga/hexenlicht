"""Views of textures in the maps (story 9.1; since 9.2 also the reader and the picker of the census,
census.py): a camera in front of a face of each texture, from the maps' BSPs in the game's paks
(BSP 29, Hexen II's models with 8 hulls).

A face's texture is named as the export names it: the BSP's name lowercased, `*` as `#`, and
`~<crc>` (the CRC-16 of mip 0's pixels, as tex_names.ps1 and the engine) where the export has
the name qualified. Faces the game doesn't draw (triggers, weather volumes, invisible plaques,
breakables and walls, glowing trains: drawn()) don't count. A texture's faces are tried in the
map where it covers the most area first, in each map by their area, a dark face counting less
(its lightmap's mean against half the bright end of the texture's faces there) and a brush
entity's less than the world's (a door, button, plaque or breakable 0.7, a platform or train
0.3: they move), leaving out brush entities a single-player game of the Paladin at skill 1
doesn't spawn; the first face the camera can see gets the view (DECISIONS M55). The close view
looks at the face's middle along its normal from 0.7 of its size (48 to 220 units; floors and
ceilings from 55 degrees), nearer or turned when that is blocked (30 and 60 degrees about the
vertical, 25 up or down; floors and ceilings from 70 or 65 and 40 degrees); the wide view is on
the same line about 2.5 times as far, else turned. The view is checked as the game will show
it: the player's origin at whole units, its box touching no trigger or door's field (noclip
still touches them: a teleporter would move it), the angles in the protocol's 360/256 steps
(the ray at the view's center must meet the face) and within the client's pitch clamp (70 up,
80 down); the eye in open space (the world's hull 0; under water or slime only where no dry
view exists, the whole line then in the face's liquid) and the line clear of the world and of
every drawn brush entity's solid (exact traces through their nodes). Strict passes first also
keep the line off the boxes of monsters and props with models (torches, pots, statues,
pickups) and the eye 8 units from solid. A brush entity with an origin brush
(func_door_rotating: qbsp stores its geometry around its `origin` key) is moved to that origin,
a train to its first path_corner (`setorigin (self, targ.origin - self.mins)`, plats.hc; the
corner itself for the mission pack's USE_ORIGIN trains). Prints `vk_setpos` lines (the
player's origin: the eye is 50 units above it) as `<stem> <map> x y z pitch yaw`, the close view.

  python bspviews.py --data <game data> --export <export> --maps demo1,meso9 --stems a,b > views.txt
"""
import argparse
import collections
import math
import os
import re
import struct
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

EYE = 50.0                  # Hexen II's view_ofs: the eye above the player's origin
CONTENTS_EMPTY, CONTENTS_SOLID, CONTENTS_WATER, CONTENTS_SLIME = -1, -2, -3, -4
OPEN = (CONTENTS_EMPTY,)
WET = (CONTENTS_EMPTY, CONTENTS_WATER, CONTENTS_SLIME)      # the fallback: a view under water
NOT_SOLID = (CONTENTS_EMPTY, CONTENTS_WATER, CONTENTS_SLIME, -5, -6)   # inside a brush entity, only its solid blocks
PITCH_UP, PITCH_DOWN = -70.0, 80.0      # the client's clamp of the view's pitch (cl_input.c)
TEX_SPECIAL = 1             # texinfo flag: the sky and liquids, no lightmap
MONSTER_BOX = (np.array([-32.0, -32.0, -24.0]), np.array([32.0, 32.0, 64.0]))
# point entities with a model in the way of a view (a torch's flame, a pot, a pickup): their
# spot's box, larger for the big props
PROP_PREFIXES = ('light_', 'obj_', 'item_', 'art_', 'wp_', 'puzzle_', 'ring_', 'misc_', 'rider_', 'trap_')
PROP_BOX = (np.array([-16.0, -16.0, -24.0]), np.array([16.0, 16.0, 48.0]))
BIG_PROPS = ('obj_statue', 'obj_tree', 'obj_fountain', 'misc_fountain', 'obj_catapult', 'obj_ballista', 'obj_cart',
             'obj_shiva', 'obj_samurai', 'obj_demon_statue', 'obj_bell', 'obj_skeleton_throne')
BIG_PROP_BOX = (np.array([-40.0, -40.0, -24.0]), np.array([40.0, 40.0, 128.0]))
NO_MODEL = ('light_thunderstorm', 'light_globe')     # lights without a model of their own (plain `light` too)
EYE_CLEARANCE = 8.0         # the eye this far from solid along each axis: not pressed against a wall
PLAYER_BOX = (np.array([-17.0, -17.0, -1.0]), np.array([17.0, 17.0, 57.0]))   # client.hc's size, SV_LinkEdict's 1 around it
DOOR_FIELD = np.array([60.0, 60.0, 8.0])        # a door's trigger field around it (doors.hc, spawn_field)
CLOSE_MIN, CLOSE_MAX = 48.0, 220.0
# what a single-player game of the Paladin at skill 1 (views_run.ps1's) leaves out: ED_LoadFromFile's
# SPAWNFLAG_NOT_SINGLE, _NOT_PALADIN, _NOT_MEDIUM
NOT_SPAWNED = 0x20000 | 0x100 | 0x2000

# brush entity classes by what they are to a texture (the census's `entities`); the rest is 'other'
CATEGORIES = {
    'door': ('func_door', 'func_door_rotating', 'func_door_secret'),
    'button': ('func_button', 'func_angletrigger', 'func_pressure'),
    'plaque': ('plaque',),
    'mover': ('func_plat', 'func_newplat', 'func_train', 'func_train_mp', 'func_crusher',
              'func_rotating', 'brush_pushable', 'rider_quake'),
    'breakable': ('breakable_brush',),
    'static': ('func_wall', 'func_illusionary'),
}
CATEGORY_OF = {c: k for k, v in CATEGORIES.items() for c in v}
CATEGORY_WEIGHT = {'world': 1.0, 'static': 1.0, 'door': 0.7, 'button': 0.7, 'plaque': 0.7,
                   'breakable': 0.7, 'other': 0.7, 'mover': 0.3}


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


def _int(s):
    try:
        return int(float(s or 0))
    except ValueError:
        return 0


def _vec(s):
    try:
        v = [float(x) for x in s.split()[:3]]
    except (ValueError, AttributeError):
        return None
    return np.array(v) if len(v) == 3 else None


def drawn(ent, portals=False, mission=False):
    """The game draws a brush entity's model: the gamecode clears a trigger's and a weather
    volume's (InitTrigger, weather.hc, fx_friction_change) and gives EF_NODRAW to an invisible
    plaque (spawnflags 1) or breakable (128, BREAK_INVISIBLE) and a glowing train (1,
    TRAIN_GLOW: a sprite instead); the mission pack's progs (portals) also to a func_wall with
    spawnflags 2 and, in its own maps (mission: worldspawn's spawnflags 1), to a train that
    carries a model (`weaponmodel`)."""
    c = ent.get('classname', '')
    f = _int(ent.get('spawnflags'))
    if c.startswith(('trigger_', 'weather_')) or c in ('fx_friction_change', 'rider_trigger_once'):
        return False
    if (c == 'plaque' and f & 1) or (c == 'breakable_brush' and f & 128) or (c in ('func_train', 'func_train_mp') and f & 1):
        return False
    if portals and c == 'func_wall' and f & 2:
        return False
    return not (portals and mission and c in ('func_train', 'func_train_mp') and ent.get('weaponmodel'))


def spawns(ent):
    """The entity is there in views_run.ps1's game (single player, the Paladin, skill 1)."""
    return not _int(ent.get('spawnflags')) & NOT_SPAWNED


def is_trigger(ent):
    """A brush entity that does something when the player touches it."""
    c = ent.get('classname', '')
    return c.startswith('trigger_') or c == 'rider_trigger_once'


def door_field(ent):
    """A door that opens when the player comes near spawns a trigger field around it (LinkDoors:
    not a shootable, fired, linked-off or puzzle door)."""
    if ent.get('classname') not in ('func_door', 'func_door_rotating'):
        return False
    if (_int(ent.get('health')) and not _int(ent.get('thingtype'))) or ent.get('targetname') or _int(ent.get('spawnflags')) & 4:
        return False
    return not any(ent.get(f'puzzle_piece_{k}') for k in range(1, 5))


def category(classname):
    return 'world' if classname == 'worldspawn' else CATEGORY_OF.get(classname, 'other')


def lightmap_size(verts, tx):
    """A face's lightmap in samples (s, t): its texture coordinates' extents in 16-texel steps,
    plus one, in single precision as the light compiler computed them: so every lit face of the
    original game's maps fits the file's packing (the gap to the next face's lightmap); in double
    1,768 of the 231,975 faces of both games don't, in single 129 of the mission pack's (its
    compiler reserved a row or column more)."""
    v = np.asarray(verts, np.float32)
    out = []
    for j in (0, 4):
        a = np.asarray(tx[j:j + 4], np.float32)
        s = ((v[:, 0] * a[0] + v[:, 1] * a[1]) + v[:, 2] * a[2]) + a[3]
        out.append(int(np.ceil(s.max() / np.float32(16)) - np.floor(s.min() / np.float32(16))) + 1)
    return out[0], out[1]


def polygon_area(poly):
    return 0.5 * float(np.linalg.norm(sum(np.cross(poly[i] - poly[0], poly[i + 1] - poly[0]) for i in range(1, len(poly) - 1))))


class Bsp:
    def __init__(self, b, game='data1'):
        ver = struct.unpack_from('<i', b, 0)[0]
        if ver != 29:
            raise ValueError(f'BSP version {ver}, not 29')
        L = [struct.unpack_from('<ii', b, 4 + 8 * i) for i in range(15)]

        def lump(i, fmt):
            ofs, ln = L[i]
            sz = struct.calcsize(fmt)
            return [struct.unpack_from(fmt, b, ofs + k * sz) for k in range(ln // sz)]
        self.planes = np.array([p[:4] for p in lump(1, '<4fi')])
        self._planes = [tuple(float(x) for x in p) for p in self.planes]     # for the traces
        self.vertexes = np.array(lump(3, '<3f'))
        self.nodes = lump(5, '<i2h6h2H')
        self.texinfo = lump(6, '<8f2i')
        self.faces = lump(7, '<2hi2h4Bi')
        self.leafs = lump(10, '<2i6h2H4B')
        self.edges = lump(12, '<2H')
        self.surfedges = [s[0] for s in lump(13, '<i')]
        self.models = lump(14, '<9f8i3i')        # Hexen II: 8 hull heads
        ofs, ln = L[8]
        self.lightdata = b[ofs:ofs + ln]
        # per brush model its entity: where it stands at the start, its classname, drawn or not
        # (by the progs of its game: the mission pack's maps run with -portals), there in
        # views_run.ps1's game or not
        ofs, ln = L[0]
        self.entities = parse_entities(b[ofs:ofs + ln].split(b'\0')[0].decode('latin-1'))
        portals = game == 'portals'
        mission = portals and bool(self.entities) and bool(_int(self.entities[0].get('spawnflags')) & 1)
        n = len(self.models)
        self.origin = np.zeros((n, 3))
        self.classname = ['worldspawn'] + [''] * (n - 1)
        self.drawn = [True] * n
        self.present = [True] * n
        self.triggers = []          # boxes the player mustn't touch: triggers and doors' fields
        named = {}
        for e in self.entities:
            if e.get('targetname'):
                named.setdefault(e['targetname'], e)
        for e in self.entities:
            mm = re.fullmatch(r'\*(\d+)', e.get('model', ''))
            if not mm or not 0 < int(mm.group(1)) < n:
                continue
            i = int(mm.group(1))
            self.classname[i] = e.get('classname', '')
            self.drawn[i] = drawn(e, portals, mission)
            self.present[i] = spawns(e)
            o = _vec(e.get('origin', ''))
            if self.classname[i] in ('func_train', 'func_train_mp') and e.get('target') in named:
                corner = _vec(named[e['target']].get('origin', ''))
                if corner is not None:
                    # plats.hc; the mission pack's trains in its maps with USE_ORIGIN (64) and an origin: the corner itself
                    use_origin = mission and _int(e.get('spawnflags')) & 64 and o is not None and o.any()
                    o = corner if use_origin else corner - np.array(self.models[i][0:3])
            if o is not None:
                self.origin[i] = o
            if self.present[i] and (is_trigger(e) or door_field(e)):
                lo, hi = np.array(self.models[i][0:3]) + self.origin[i], np.array(self.models[i][3:6]) + self.origin[i]
                pad = 0.0 if is_trigger(e) else DOOR_FIELD
                self.triggers.append((lo - pad - 1, hi + pad + 1))
        # the boxes of monsters and props where they stand at the start (their models aren't in the BSP)
        self.obstacles = []
        for e in self.entities:
            c, o = e.get('classname', '').lower(), _vec(e.get('origin', ''))
            if o is None or re.fullmatch(r'\*\d+', e.get('model', '')) or not spawns(e):
                continue
            if c.startswith('monster_'):
                box = MONSTER_BOX
            elif c.startswith(BIG_PROPS):
                box = BIG_PROP_BOX
            elif c.startswith(PROP_PREFIXES) and c not in NO_MODEL:
                box = PROP_BOX
            else:
                continue
            self.obstacles.append((o + box[0], o + box[1]))
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

    def stem(self, ti, qualified):
        """The texture's name as the export files it."""
        tex = self.textures[ti] if 0 <= ti < len(self.textures) else None
        if not tex:
            return None
        return f'{tex[0]}~{self.crc(ti):04x}' if tex[0] in qualified else tex[0]

    def vertices(self, fi):
        """A face's polygon where qbsp stored it (a brush entity's around its own origin)."""
        f = self.faces[fi]
        vs = []
        for k in range(f[3]):
            se = self.surfedges[f[2] + k]
            e = self.edges[abs(se)]
            vs.append(self.vertexes[e[0] if se >= 0 else e[1]])
        return np.array(vs)

    def face_polygon(self, fi, model=0):
        return self.vertices(fi) + self.origin[model]

    def face_normal(self, fi):
        f = self.faces[fi]
        n = self.planes[f[0], :3].copy()
        return -n if f[1] else n

    def light(self, fi, verts=None):
        """The mean of a face's lightmap, 0-255, its styles summed (switched lights on) and
        clamped; None for the sky and liquids (no lightmap), 0 for a face the compiler left dark."""
        f = self.faces[fi]
        tx = self.texinfo[f[4]]
        if tx[9] & TEX_SPECIAL:
            return None
        styles = sum(1 for s in f[5:9] if s != 255)
        if f[9] < 0 or not styles:
            return 0.0
        s, t = lightmap_size(self.vertices(fi) if verts is None else verts, tx)
        n = s * t
        a = np.frombuffer(self.lightdata, np.uint8, count=max(0, min(styles * n, len(self.lightdata) - f[9])), offset=f[9])
        k = len(a) // n if n else 0
        if not k:
            return 0.0
        return float(np.minimum(a[:k * n].reshape(k, n).astype(np.int32).sum(axis=0), 255).mean())

    def contents(self, p):
        node = self.models[0][9]        # hull 0's head
        while node >= 0:
            pl, c0, c1 = self.nodes[node][0], self.nodes[node][1], self.nodes[node][2]
            nx, ny, nz, d = self._planes[pl]
            node = c0 if nx * p[0] + ny * p[1] + nz * p[2] - d >= 0 else c1
        return self.leafs[-node - 1][0]

    def trace(self, a, b, ok=OPEN, node=None):
        """Every point of the segment a-b is in a leaf whose contents are in ok, through the
        hull 0 of the world (or of the model whose head node is given): an exact trace
        through the nodes, not samples."""
        if node is None:
            node = self.models[0][9]
        while node >= 0:
            pl, c0, c1 = self.nodes[node][0], self.nodes[node][1], self.nodes[node][2]
            nx, ny, nz, d = self._planes[pl]
            t1 = nx * a[0] + ny * a[1] + nz * a[2] - d
            t2 = nx * b[0] + ny * b[1] + nz * b[2] - d
            if t1 >= 0 and t2 >= 0:
                node = c0
            elif t1 < 0 and t2 < 0:
                node = c1
            else:
                m = tuple(a[k] + (b[k] - a[k]) * (t1 / (t1 - t2)) for k in range(3))
                near, far = (c0, c1) if t1 >= 0 else (c1, c0)
                return self.trace(a, m, ok, near) and self.trace(m, b, ok, far)
        return self.leafs[-node - 1][0] in ok

    def clear(self, a, b, own=-1, ok=OPEN, props=True):
        """The line a-b is open: in ok's contents through the world (hull 0), through no solid
        of a drawn brush entity where it stands at the start (doors, windows, platforms: its own
        hull 0, where the line crosses its box; own: the face's model) and, with props, across
        no box of a monster or prop (obstacles)."""
        if not self.trace(tuple(float(x) for x in a), tuple(float(x) for x in b), ok):
            return False
        for mi in range(1, len(self.models)):
            if mi == own or not self.drawn[mi] or not self.present[mi]:
                continue
            o = self.origin[mi]
            if _segment_hits_box(a, b, np.array(self.models[mi][0:3]) + o - 1, np.array(self.models[mi][3:6]) + o + 1) \
                    and not self.trace(tuple(float(x) for x in a - o), tuple(float(x) for x in b - o), NOT_SOLID, self.models[mi][9]):
                return False
        for lo, hi in self.obstacles if props else ():
            if _segment_hits_box(a, b, lo, hi):
                return False
        return True

    def model_faces(self):
        """(model, face) for every face of a drawn model: the world first, then the brush entities."""
        for mi, mdl in enumerate(self.models):
            if self.drawn[mi]:
                for fi in range(mdl[18], mdl[18] + mdl[19]):
                    yield mi, fi


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


class Maps:
    """The maps of a game data folder's paks, the last few read kept."""
    def __init__(self, data, keep=6):
        self.files = {n[len('maps/'):-len('.bsp')]: v for n, v in pak_files(data).items() if n.endswith('.bsp')}
        self.keep = keep
        self._cache = collections.OrderedDict()

    def names(self):
        return sorted(self.files)

    def game(self, name):
        """data1 or portals: the folder whose pak has the map."""
        return os.path.basename(os.path.dirname(self.files[name][0]))

    def get(self, name):
        if name in self._cache:
            self._cache.move_to_end(name)
            return self._cache[name]
        p, pos, ln = self.files[name]
        with open(p, 'rb') as f:
            f.seek(pos)
            bsp = Bsp(f.read(ln), self.game(name))
        self._cache[name] = bsp
        while len(self._cache) > self.keep:
            self._cache.popitem(last=False)
        return bsp


# -- the views -------------------------------------------------------------------

def _rot_z(v, deg):
    c, s = math.cos(math.radians(deg)), math.sin(math.radians(deg))
    return np.array([v[0] * c - v[1] * s, v[0] * s + v[1] * c, v[2]])


def directions(n):
    """The directions from a face to the eye, in the order they are tried: a wall's normal,
    then turned 30 and 60 degrees about the vertical and tilted 25 degrees up and down; a
    floor or ceiling from 55 degrees at 8 headings, then 70 (floors; 65 ceilings: the pitch
    clamp) and 40. Only directions in front of the face."""
    out = []
    if abs(n[2]) > 0.7:
        up = 1.0 if n[2] > 0 else -1.0
        for el in (55.0, 70.0 if up > 0 else 65.0, 40.0):
            ce, se = math.cos(math.radians(el)), math.sin(math.radians(el))
            for k in range(8):
                yaw = math.radians(k * 45.0)
                out.append(np.array([-math.cos(yaw) * ce, -math.sin(yaw) * ce, up * se]))
    else:
        out.append(n.copy())
        out += [_rot_z(n, a) for a in (30.0, -30.0, 60.0, -60.0)]
        for tilt in (25.0, -25.0):
            out.append(n * math.cos(math.radians(tilt)) + np.array([0.0, 0.0, math.sin(math.radians(tilt))]))
    out = [u / np.linalg.norm(u) for u in out]
    return [u for u in out if float(np.dot(u, n)) > 0.25]


def aim(eye, target):
    """(pitch, yaw) from the eye to the target, None outside the client's pitch clamp."""
    d = target - eye
    d = d / np.linalg.norm(d)
    pitch = -math.degrees(math.asin(max(-1.0, min(1.0, float(d[2])))))
    if not PITCH_UP <= pitch <= PITCH_DOWN:
        return None
    return pitch, math.degrees(math.atan2(d[1], d[0]))


def sent_angle(deg):
    """An angle as the client gets it: a whole degree (the view lines') sent as a byte
    (MSG_WriteAngle: 360/256 steps, rounded half away from zero)."""
    d = round(deg)
    q = int(d * 256.0 / 360.0 + (0.5 if d >= 0 else -0.5))
    return q * 360.0 / 256.0


def forward(pitch, yaw):
    p, y = math.radians(pitch), math.radians(yaw)
    return np.array([math.cos(p) * math.cos(y), math.cos(p) * math.sin(y), -math.sin(p)])


def inside(poly, n, p, margin=0.5):
    """The point p of the polygon's plane is inside it by margin units."""
    s = []
    for i in range(len(poly)):
        e = poly[(i + 1) % len(poly)] - poly[i]
        ln = float(np.linalg.norm(e))
        if ln > 1e-6:
            s.append(float(np.dot(np.cross(e, p - poly[i]), n)) / ln)
    return bool(s) and (all(x >= margin for x in s) or all(x <= -margin for x in s))


def player_touches(bsp, origin):
    """The player's box at origin overlaps a trigger or a door's field (SV_TouchLinks: noclip
    links the player every frame, so a teleporter or a trigger_once would fire)."""
    lo, hi = origin + PLAYER_BOX[0], origin + PLAYER_BOX[1]
    return any((lo <= t_hi).all() and (hi >= t_lo).all() for t_lo, t_hi in bsp.triggers)


def _spot(bsp, eye, c, n, poly, own, ok, strict):
    """A view of the face (its polygon poly, middle c, normal n) from about eye, or None: the
    player's origin at whole units and the angles as the client gets them, the ray at the
    view's center on the face and the line to where it meets the face clear; strict: clear of
    monsters and props too and not pressed against a wall."""
    origin = np.round(eye - np.array([0.0, 0.0, EYE]))
    eye = origin + np.array([0.0, 0.0, EYE])
    if bsp.contents(eye) not in ok or player_touches(bsp, origin):
        return None
    a = aim(eye, c)
    if a is None:
        return None
    d = forward(min(PITCH_DOWN, max(PITCH_UP, sent_angle(a[0]))), sent_angle(a[1]))     # the client clamps what it got
    den = float(np.dot(d, n))
    if den > -1e-3:
        return None
    hit = eye + d * (float(np.dot(c - eye, n)) / den)
    if not inside(poly, n, hit) or not bsp.clear(eye, hit + n * 2.0, own=own, ok=ok, props=strict):
        return None
    for k in range(3) if strict else ():
        for s in (-EYE_CLEARANCE, EYE_CLEARANCE):
            p = eye.copy()
            p[k] += s
            if bsp.contents(p) not in ok:
                return None
    return eye, round(a[0]), round(a[1])


def face_views(bsp, mi, fi, ok=OPEN, strict=True, skip=0):
    """The close and the wide view of a face, each (eye, pitch, yaw), or None; the wide view
    strict where it can be, else not, else the close; skip: the views from that many
    directions before it left out (another side of a statue). Under water (ok not OPEN) the
    eye and the whole line are in the liquid the face is in: a line through the surface meets
    the surface."""
    poly = bsp.face_polygon(fi, mi)
    c = poly.mean(axis=0)
    n = bsp.face_normal(fi)
    if ok != OPEN:
        medium = bsp.contents(c + n * 2.0)
        if medium not in (CONTENTS_WATER, CONTENTS_SLIME):
            return None
        ok = (medium,)
    size = float(np.max(poly.max(axis=0) - poly.min(axis=0)))
    d0 = min(CLOSE_MAX, max(CLOSE_MIN, 0.7 * size))
    dirs = directions(n)
    for i, u in enumerate(dirs):
        for k in (1.0, 0.6, 0.4):
            dist = max(24.0, d0 * k)
            close = _spot(bsp, c + u * dist, c, n, poly, mi, ok, strict)
            if not close:
                continue
            if skip:
                skip -= 1
                break       # the next direction
            for wide_strict in (True, False) if strict else (False,):
                for u2 in [u] + dirs[:i] + dirs[i + 1:]:
                    for f in (2.5, 2.0, 1.6, 1.3):
                        dw = d0 * f
                        if dw < dist * 1.25:
                            break
                        wide = _spot(bsp, c + u2 * dw, c, n, poly, mi, ok, wide_strict)
                        if wide:
                            return close, wide
            return close, close
    return None


def wpercentile(values, weights, q):
    """The q-th percentile of values weighted by weights (the value where the cumulative
    weight reaches q percent)."""
    if not len(values):
        return 0.0
    order = np.argsort(values, kind='stable')
    v = np.asarray(values, float)[order]
    w = np.asarray(weights, float)[order]
    cw = np.cumsum(w)
    if cw[-1] <= 0:
        return float(v[len(v) // 2])
    return float(v[min(len(v) - 1, int(np.searchsorted(cw, q / 100.0 * cw[-1])))])


def rank_faces(faces):
    """A texture's faces in one map, (model, face, area, light, category), in the order they
    are tried: by area times a weight for the light (its mean against half the 90th percentile
    of the texture's faces there, at least 16: full weight at or above it) and the category's."""
    lit = [(f[3], f[2]) for f in faces if f[3] is not None]
    ref = max(16.0, 0.5 * wpercentile([v for v, _ in lit], [w for _, w in lit], 90)) if lit else None

    def score(f):
        wl = 1.0 if f[3] is None or ref is None else min(1.0, max(f[3], 0.0) / ref)
        return f[2] * max(wl, 0.02) * CATEGORY_WEIGHT.get(f[4], 0.7)
    return sorted(faces, key=lambda f: (-score(f), f[0], f[1]))


# the passes over a map's faces: dry and clear of props first; the in-game check catches what a
# lenient view's prop or a wall beside the eye hides
PASSES = ((OPEN, True), (OPEN, False), (WET, True), (WET, False))


def candidate_views(maps, faces_by_map, limit=60):
    """Yields (map, model, face, close, wide) for a texture's faces in the order they are
    tried: faces_by_map's maps in their order, in each the ranked faces' views of each pass
    (PASSES), every face once, then each of those faces from its next direction; faces of
    brush entities the game leaves out at skill 1 for the Paladin aren't tried."""
    for name, faces in faces_by_map:
        bsp = maps.get(name)
        left = rank_faces([f for f in faces if bsp.present[f[0]]])[:limit]
        seen = []
        for ok, strict in PASSES:
            rest = []
            for f in left:
                v = face_views(bsp, f[0], f[1], ok, strict)
                if v:
                    seen.append((f, ok, strict))
                    yield (name, f[0], f[1]) + v
                else:
                    rest.append(f)
            left = rest
        for f, ok, strict in seen:
            v = face_views(bsp, f[0], f[1], ok, strict, skip=1)
            if v:
                yield (name, f[0], f[1]) + v


def spot_text(spot):
    """`vk_setpos`'s numbers for a view: the player's origin (the eye 50 units above it) and the angles."""
    eye, pitch, yaw = spot
    return f'{round(eye[0])} {round(eye[1])} {round(eye[2] - EYE)} {round(pitch)} {round(yaw)}'


def texture_faces(bsp, qualified, want):
    """stem -> [(model, face, area, light, category)] of the drawn faces of the stems in want."""
    out = {}
    for mi, fi in bsp.model_faces():
        stem = bsp.stem(bsp.texinfo[bsp.faces[fi][4]][8], qualified)
        if stem in want:
            out.setdefault(stem, []).append((mi, fi, polygon_area(bsp.face_polygon(fi, mi)), bsp.light(fi), category(bsp.classname[mi])))
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
    maps = Maps(data)
    order = [m for m in (a.maps or '').split(',') if m]      # these first, then each texture's own
    unknown = [m for m in order if m not in maps.files]
    if unknown:
        raise SystemExit(f"not in the paks of {data}: {', '.join(unknown)}")
    entries = pipeline.load_export(export)
    for s in sorted(want):
        for m in (entries[s].used_in if s in entries else []):
            if m not in order and m in maps.files:
                order.append(m)
    found = set()
    for mapname in order:
        if not want - found:
            break
        for stem, faces in sorted(texture_faces(maps.get(mapname), qualified, want - found).items()):
            for _, mi, fi, close, wide in candidate_views(maps, [(mapname, faces)]):
                area = polygon_area(maps.get(mapname).face_polygon(fi, mi))
                print(f'{stem} {mapname} {spot_text(close)}   # face {area:.0f} sq units')
                found.add(stem)
                break
    for s in sorted(want - found) if a.stems else []:
        print(f'# {s}: no view in its maps')
    return 0


if __name__ == '__main__':
    sys.exit(main())
