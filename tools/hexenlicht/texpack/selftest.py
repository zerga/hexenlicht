"""texpack selftest (story 5.8): the pipeline's pure functions and the manifest's
resolution on synthetic data: no GPU, no game data, no models. Run with
ComfyUI's Python: python selftest.py (exit 1 on a failure)."""
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import manifest as M  # noqa: E402
import pipeline as P  # noqa: E402

fails = []


def check(name, ok, detail=''):
    print(('ok   ' if ok else 'FAIL ') + name + (f': {detail}' if detail else ''))
    if not ok:
        fails.append(name)


rng = np.random.default_rng(1)
rgb = rng.integers(10, 120, (64, 64, 3), dtype=np.uint8)
target = P.mean_linear_luminance(rgb) * 0.4
out, k = P.match_luminance(rgb, None, target)
got = P.mean_linear_luminance(out)
check('luminance match reaches the target', abs(got / target - 1) < 0.01, f'{got:.5f} of {target:.5f}')
out2, _ = P.match_luminance(rgb, None, 0.5)
check('luminance match falls short only by clipping', P.mean_linear_luminance(out2) < 0.5)

a = np.full((8, 8), 255, np.uint8)
a[3:5, 3:5] = 0
c = np.zeros((8, 8, 3), np.uint8)
c[...] = (200, 100, 50)
c[3:5, 3:5] = 0
b = P.bleed(c, a, 4)
check('bleed fills the colors under holes', tuple(b[3, 3]) == (200, 100, 50) and tuple(b[4, 4]) == (200, 100, 50), str(b[3, 3]))
check('bleed leaves opaque texels', (b[0] == c[0]).all())

check('margins are even and a quarter of the short side', [P.margin_for(*s) for s in ((48, 48), (64, 176), (440, 227), (32, 128))] == [12, 16, 56, 8],
      str([P.margin_for(*s) for s in ((48, 48), (64, 176), (440, 227), (32, 128))]))
x = rng.integers(0, 255, (16, 16, 3), dtype=np.uint8)
p = P.pad(x, 4, True)
check('wrap padding is periodic', bool((p[0:4, 4:20] == x[12:16]).all() and (p[4:20, 0:4] == x[:, 12:16]).all()))

o = rng.integers(0, 255, (15, 17, 3), dtype=np.uint8)
po = P.pad(o, 5, False, even=True)
check('padded size is even, the crop offsets unchanged', po.shape[0] % 2 == 0 and po.shape[1] % 2 == 0 and (po[5:20, 5:22] == o).all(), str(po.shape))

up = P.upscale_alpha(np.where(np.arange(16)[None, :] < 8, 255, 0).astype(np.uint8).repeat(16, 0), 64, 64, 'coverage')
check('coverage alpha stays 0 or 255 when upscaled', set(np.unique(up)) <= {0, 255})

n = rng.random((32, 32, 3))
f = P.finish_normal(n, True)
v = f.astype(np.float64) / 255 * 2 - 1
check('finished normals have unit length', abs(float(np.sqrt((v ** 2).sum(axis=2)).mean()) - 1) < 0.02)
r = P.finish_roughness(rng.random((32, 32, 3)), 0.7, 0.95)
check('roughness stays inside the class range', 0.7 - 1e-9 <= r.min() and r.max() <= 0.95 + 1e-9)
orm = P.make_orm(r, 1)
check('_orm channels', (orm[..., 0] == 255).all() and (orm[..., 2] == 255).all())

classes = M.load_classes(os.path.join(HERE, 'classes.toml'))
row = lambda pat, cls='', d='', o='', s='human': {'pattern': pat, 'class': cls, 'description': d, 'overrides': o, 'source': s}  # noqa: E731
rows = [row('models/imp.mdl_*', 'skin', 'imp', 'denoise=0.1'), row('models/imp.mdl_0', '', 'imp, main skin', 'bump=0.5'), row('*', 'stone', '', '', 'draft')]
r = M.resolve('models/imp.mdl_0', 'skin', rows, classes)
check('a literal row beats a glob, a longer glob beats `*`', r.cls == 'skin' and r.description == 'imp, main skin' and r.params['denoise'] == 0.1 and r.params['bump'] == 0.5,
      f'{r.cls} {r.description} {r.params["denoise"]} {r.params["bump"]}')
rows2 = [row('+0swit0', 'stone', 'drafted', '', 'draft'), row('+?swit*', 'rune', 'symbol panel')]
r2 = M.resolve('+0swit0', 'world', rows2, classes)
check('a human glob beats a draft row of the exact name', r2.cls == 'rune' and r2.description == 'symbol panel', f'{r2.cls} {r2.description}')
check('the prompt takes the description', 'imp, main skin' in r.prompt())
check('no row: the export kind decides', M.resolve('mtex1', 'world', [], classes).cls == 'world' and M.resolve('s', 'sky', [], classes).params['mode'] == 'skip')
for what, rr in (('an unknown class', row('x', 'nope')), ('an unknown override key', row('x', 'stone', '', 'denoize=1'))):
    try:
        M.resolve('x', 'world', [rr], classes)
        check(what + ' is refused', False)
    except ValueError:
        check(what + ' is refused', True)
shipped = M.load_manifest(os.path.join(HERE, 'materials.csv'))
check('materials.csv: every row resolves', all(M.resolve(q['pattern'], 'world', shipped, classes) for q in shipped), f'{len(shipped)} rows')
check('rows named with # (the liquids) are not comments', any(q['pattern'] == '#lava000' for q in shipped))
check('every class has keys the pipeline reads', all(set(('mode', 'noun', 'prompt', 'denoise')) <= set({**classes['defaults'], **c}) for c in classes['class'].values()))

# contrast: the darks go darker at the same mean
cl = rng.integers(5, 200, (64, 64, 3), dtype=np.uint8)
t0 = P.mean_linear_luminance(cl)
flat, _ = P.match_luminance(cl, None, t0, 1.0)
hard, _ = P.match_luminance(cl, None, t0, 2.0)
p5 = lambda x: np.percentile(((x.astype(float) / 255) ** 2.2) @ P.LUM, 5)  # noqa: E731
check('contrast darkens the darks at the same mean', p5(hard) < 0.5 * p5(flat) and abs(P.mean_linear_luminance(hard) / t0 - 1) < 0.02,
      f'{p5(flat) * 1000:.2f} -> {p5(hard) * 1000:.2f}, mean x{P.mean_linear_luminance(hard) / t0:.3f}')

lin = lambda x: (x.astype(float) / 255) ** 2.2  # noqa: E731
warm = np.zeros((32, 32, 3), np.uint8)
warm[..., 0] = rng.integers(60, 200, (32, 32))
warm[..., 1] = warm[..., 0] * 0.6
warm[..., 2] = warm[..., 0] * 0.3
w2, _ = P.match_luminance(warm, None, P.mean_linear_luminance(warm), 2.0)
rg0 = (lin(warm)[..., 0] / lin(warm)[..., 1]).mean()
rg2 = (lin(w2)[..., 0] / np.maximum(lin(w2)[..., 1], 1e-9)).mean()
check('contrast keeps the hue (red over green stays)', abs(rg2 / rg0 - 1) < 0.05, f'{rg0:.2f} -> {rg2:.2f}')

# translucent skins: alpha 84 is not a hole (bleed leaves it, the mean counts it); holes are alpha 0
ta = np.full((8, 8), 84, np.uint8)
ta[0, :] = 255
tc = np.zeros((8, 8, 3), np.uint8)
tc[...] = (40, 90, 160)
check('bleed leaves translucent texels alone', (P.bleed(tc, ta, 4) == tc).all())
holes = np.full((8, 8), 255, np.uint8)
holes[:, 0] = 0
hc = np.zeros((8, 8, 3), np.uint8)
hc[...] = (10, 20, 30)
hc[:, 7] = (200, 200, 200)
check('bleed does not wrap around the edge', tuple(P.bleed(hc, holes, 1)[3, 0]) == (10, 20, 30), str(P.bleed(hc, holes, 1)[3, 0]))
check('luminance counts translucent texels', abs(P.mean_linear_luminance(tc, ta) - P.mean_linear_luminance(tc)) < 1e-9)


class FakeComfy:
    """Stands in for ComfyUI: hands back the uploaded image at 4x, or fails on demand."""
    home = ''
    fail = False

    def upload(self, name, data):
        self.last = data
        return name

    def run(self, wf):
        if self.fail:
            raise RuntimeError('boom')
        import io
        from PIL import Image
        im = Image.open(io.BytesIO(self.last)).convert('RGB')
        b = io.BytesIO()
        im.resize((im.width * 4, im.height * 4), Image.NEAREST).save(b, 'PNG')
        return b.getvalue()


import tempfile  # noqa: E402
from PIL import Image  # noqa: E402
with tempfile.TemporaryDirectory() as tmp:
    ex, out = os.path.join(tmp, 'ex'), os.path.join(tmp, 'out')
    os.makedirs(os.path.join(ex, 'textures'))
    Image.fromarray(rng.integers(20, 100, (16, 16, 3), dtype=np.uint8)).save(os.path.join(ex, 'textures', 'win.png'))
    entry = P.Entry({'file': 'textures/win.png', 'name': 'win', 'width': '16', 'height': '16', 'kind': 'world', 'alpha': 'none', 'used in': 'm1'})
    glass = M.resolve('win', 'world', [row('win', 'glass', 'a pane')], classes)      # no maps, a .mat
    fc = FakeComfy()
    r = P.Runner(ex, out, classes, fc)
    base = os.path.join(out, 'textures', 'win')
    first = r.process(entry, glass)
    check('process writes the albedo (4x) and the .mat of its class', Image.open(base + '.png').size == (64, 64) and os.path.exists(base + '.mat') and not first.get('skipped'))
    check('an unchanged texture is skipped', r.process(entry, glass).get('skipped') is True)
    os.remove(base + '.mat')
    check('a missing expected file makes it redo', not r.process(entry, glass).get('skipped') and os.path.exists(base + '.mat'))
    open(base + '_n.png', 'wb').write(b'stale')
    stone = M.resolve('win', 'world', [row('win', 'glass', 'a pane', 'denoise=0.1')], classes)
    r.process(entry, stone)
    check('a stale map of an earlier class is deleted after the new files are made', not os.path.exists(base + '_n.png'))
    open(base + '_orm.png', 'wb').write(b'keep')
    fc.fail = True
    other = M.resolve('win', 'world', [row('win', 'glass', 'a pane', 'denoise=0.2')], classes)
    try:
        r.process(entry, other)
        check('a failing run raises', False)
    except RuntimeError:
        check('a failing run raises and keeps the old files', os.path.exists(base + '_orm.png') and os.path.exists(base + '.png'))
# the fx class: the 4x upscale only
fx = M.resolve('models/flame1.mdl_0', 'skin', [row('models/flame1.mdl_*', 'fx')], classes).params
check('fx: sprite mode, no maps, ratio 1, contrast 1', fx['mode'] == 'sprite' and not fx['maps'] and fx['albedo_ratio'] == 1.0 and fx['contrast'] == 1.0)
sk = M.resolve('models/imp.mdl_0', 'skin', [], classes).params
check('a skin keeps the dielectric defaults', sk['mode'] == 'skin' and sk['contrast'] == 1.5 and sk['specular'] == 0.04)

# verify: a translucent skin keeps its mean opacity AND its alpha shape
import verify  # noqa: E402
with tempfile.TemporaryDirectory() as tmp:
    ex, pk = os.path.join(tmp, 'ex'), os.path.join(tmp, 'pk')
    for d in (ex, pk):
        os.makedirs(os.path.join(d, 'textures'))
    oa = np.zeros((16, 16), np.uint8)
    oa[:, :8] = 84
    oa[:, 8:] = 255
    orgb = np.dstack([rng.integers(30, 90, (16, 16, 3), dtype=np.uint8), oa])
    Image.fromarray(orgb).save(os.path.join(ex, 'textures', 't.png'))
    good = np.asarray(Image.fromarray(oa).resize((64, 64), Image.BICUBIC))
    smear = np.full((64, 64), int(oa.mean()), np.uint8)             # same mean, no shape
    for name, a in (('good', good), ('smear', smear)):
        rgb4 = np.asarray(Image.fromarray(orgb[..., :3]).resize((64, 64), Image.NEAREST))
        Image.fromarray(np.dstack([rgb4, a])).save(os.path.join(pk, 'textures', 't.png'))
        ent = P.Entry({'file': 'textures/t.png', 'name': 't', 'width': '16', 'height': '16', 'kind': 'skin', 'alpha': 'translucent', 'used in': ''})
        res = M.resolve('t', 'skin', [row('t', 'fx')], classes)
        errs, _ = verify.check(ex, pk, ent, res)
        bad = any('alpha' in e or 'opacity' in e for e in errs)
        check(f'verify translucent: the {name} alpha is {"refused" if name == "smear" else "accepted"}', bad == (name == 'smear'), str(errs))

# redraw (story 9.1): the stages after the model
import redraw as R  # noqa: E402
import bspviews  # noqa: E402


def field(h, w, r, seed):
    g = np.random.default_rng(seed)
    return np.dstack([R._smooth(g.random((h, w)), r) for _ in range(3)])


per = (np.sin(np.arange(64) * 2 * np.pi / 64)[None, :] * np.cos(np.arange(64) * 2 * np.pi / 64)[:, None] * 0.4 + 0.5)
check('seam score: a periodic texture tiles (about 1)', R.seam_score(np.uint8(np.dstack([per] * 3) * 255)) < 1.3)
m4, h4, w4 = 16, 64, 64
big = np.uint8(np.clip(field(h4 + 2 * m4, w4 + 2 * m4, 5, 3) * 1.6 - 0.3, 0, 1) * 255)
plain = big[m4:m4 + h4, m4:m4 + w4]
blended = R.blend_seams(big, m4, h4, w4)
check('blend_seams: a crop of a random field has a seam, the blend none',
      R.seam_score(plain) > 2 and R.seam_score(blended) < 1.5, f'{R.seam_score(plain):.2f} -> {R.seam_score(blended):.2f}')

# shading: an albedo times the light of a height field's normals from the top left; taking the
# directional part out leaves (almost) no correlation with the normals' x and y
hgt = R._smooth(np.random.default_rng(4).random((96, 96)), 3) * 40
gy, gx = np.gradient(hgt)
n = np.dstack([-gx, gy, np.ones_like(gx)])          # OpenGL: green up (a row index grows downwards)
n /= np.linalg.norm(n, axis=2, keepdims=True)
n8 = np.uint8(np.round((n * 0.5 + 0.5) * 255))
light = np.clip(n @ np.array([-0.5, 0.5, 0.7071]), 0.05, 1)
alb = 0.2 + 0.3 * field(96, 96, 10, 5)
lit = R._enc(alb * light[..., None])
c, rd0, _ = R.shading_fit(lit, n8, 8)
out, _ = R.take_light_out(lit, n8, 8, front=1.0)
_, rd1, _ = R.shading_fit(out, n8, 8)
check('take_light_out: the light from a side goes', rd0 > 0.5 and rd1 < 0.25, f'directional fit {rd0:.2f} -> {rd1:.2f}')
check('take_light_out keeps the mean brightness', abs(P.mean_linear_luminance(out) / P.mean_linear_luminance(lit) - 1) < 0.02)

# the color guard: a grey redraw of a red original gets its red back, the detail stays
orig = np.zeros((16, 16, 3), np.uint8)
orig[...] = (120, 40, 30)
grey = np.uint8(np.clip(np.dstack([field(64, 64, 2, 6)[..., 0]] * 3) * 200, 0, 255))
guarded = R.color_guard(orig, grey, 1.0, 1 / 24)
check('color_guard brings the broad hue back', R.color_drift(orig, guarded) < 0.3 * R.color_drift(orig, grey),
      f'{R.color_drift(orig, grey):.3f} -> {R.color_drift(orig, guarded):.3f}')
ly = lambda x: R._lin(x) @ P.LUM  # noqa: E731
check('color_guard keeps the lightness detail', R._corr(R._highpass(ly(grey), 4), R._highpass(ly(guarded), 4)) > 0.9)

# the palette's special row: found by exact color, kept over the result
pal = np.zeros((256, 3), np.uint8)
pal[:, 0] = np.arange(256)
pal[243] = (40, 250, 40)
src = np.zeros((8, 8, 3), np.uint8)
src[2, 3] = pal[243]
sm = R.special_mask(src, pal)
check('special_mask finds the special row\'s texels only', sm is not None and sm.sum() == 1 and sm[2, 3])
kept = R.keep_special(np.zeros((32, 32, 3), np.uint8), src, sm)
check('keep_special puts their color back', kept[2 * 4 + 1, 3 * 4 + 1, 1] > 150, str(kept[9, 13]))
check('special_mask: none without the row', R.special_mask(np.zeros((4, 4, 3), np.uint8), pal) is None)

check('bspviews.crc16 is CRC-16/CCITT (0xffff, not reflected): 123456789 -> 29b1', bspviews.crc16(b'123456789') == 0x29b1)
check('anim_base: the frames of an animation (and their variants) share a seed',
      R.anim_base('+0rune1') == R.anim_base('+4rune1') == R.anim_base('+arune1~1a2b') == 'rune1' and R.anim_base('rtex022') == 'rtex022')
ents = bspviews.parse_entities('{\n"classname" "worldspawn"\n}\n{\n"model" "*3"\n"origin" "16 -8 4"\n"classname" "func_door_rotating"\n}\n')
check('bspviews.parse_entities reads a door\'s origin', ents[1].get('origin') == '16 -8 4' and ents[0]['classname'] == 'worldspawn')
check('bspviews: a segment through a box hits it, one beside it not',
      bspviews._segment_hits_box(np.array([0., 0, 0]), np.array([10., 0, 0]), np.array([4., -1, -1]), np.array([6., 1, 1]))
      and not bspviews._segment_hits_box(np.array([0., 5, 0]), np.array([10., 5, 0]), np.array([4., -1, -1]), np.array([6., 1, 1])))

# the census (story 9.2)
import census as C  # noqa: E402
check('bspviews.drawn: triggers, weather volumes and invisible plaques are not drawn, a door and a plaque are',
      not bspviews.drawn({'classname': 'trigger_once'}) and not bspviews.drawn({'classname': 'weather_snow'})
      and not bspviews.drawn({'classname': 'plaque', 'spawnflags': '1'}) and bspviews.drawn({'classname': 'plaque', 'spawnflags': '2'})
      and bspviews.drawn({'classname': 'func_door'}))
check('bspviews.drawn: an invisible breakable (128), a glowing train (1), the mission pack\'s func_wall 2 and its trains with a model are not',
      not bspviews.drawn({'classname': 'breakable_brush', 'spawnflags': '129'}) and bspviews.drawn({'classname': 'breakable_brush', 'spawnflags': '1'})
      and not bspviews.drawn({'classname': 'func_train', 'spawnflags': '1'})
      and bspviews.drawn({'classname': 'func_wall', 'spawnflags': '2'}) and not bspviews.drawn({'classname': 'func_wall', 'spawnflags': '2'}, portals=True)
      and bspviews.drawn({'classname': 'func_train', 'weaponmodel': 'models/x.mdl'}, portals=True)
      and not bspviews.drawn({'classname': 'func_train', 'weaponmodel': 'models/x.mdl'}, portals=True, mission=True))
check('bspviews.spawns: single player, the Paladin, skill 1',
      bspviews.spawns({'spawnflags': '4096'}) and not bspviews.spawns({'spawnflags': '256'})
      and not bspviews.spawns({'spawnflags': '8192'}) and not bspviews.spawns({'spawnflags': str(0x20000)}))
check('bspviews.door_field: a door that opens on its own, not a fired, shootable or puzzle one',
      bspviews.door_field({'classname': 'func_door'}) and not bspviews.door_field({'classname': 'func_door', 'targetname': 't1'})
      and not bspviews.door_field({'classname': 'func_door', 'health': '10'}) and not bspviews.door_field({'classname': 'func_door', 'puzzle_piece_1': 'x'})
      and not bspviews.door_field({'classname': 'func_button'}))
check('sent_angle: whole degrees in 360/256 steps, half away from zero', bspviews.sent_angle(45.2) == 45.0
      and abs(bspviews.sent_angle(-55) + 54.84375) < 1e-9 and abs(bspviews.sent_angle(10) - 9.84375) < 1e-9)
sq = np.array([[0., 0, 0], [16, 0, 0], [16, 16, 0], [0, 16, 0]])
check('inside: a point of the polygon by the margin, not one at its edge or outside',
      bspviews.inside(sq, np.array([0., 0, 1]), np.array([8., 8, 0])) and not bspviews.inside(sq, np.array([0., 0, 1]), np.array([16.2, 8, 0]))
      and not bspviews.inside(sq, np.array([0., 0, 1]), np.array([8., 0.2, 0])) and bspviews.inside(sq[::-1], np.array([0., 0, 1]), np.array([8., 8, 0])))
tb = bspviews.Bsp.__new__(bspviews.Bsp)
tb.triggers = [(np.array([100., -10, 0]), np.array([120., 10, 64]))]
check('player_touches: the player\'s box (16 around, 56 up, 1 more) against a trigger\'s',
      bspviews.player_touches(tb, np.array([84., 0, 0])) and not bspviews.player_touches(tb, np.array([82., 0, 0]))
      and bspviews.player_touches(tb, np.array([110., 0, -56])) and not bspviews.player_touches(tb, np.array([110., 0, -59])))
check('bspviews.category', bspviews.category('func_door_rotating') == 'door' and bspviews.category('breakable_brush') == 'breakable'
      and bspviews.category('func_train_mp') == 'mover' and bspviews.category('worldspawn') == 'world' and bspviews.category('obj_x') == 'other')
quad = np.array([[0., 0, 0], [64, 0, 0], [64, 0, 32], [0, 0, 32]])     # a wall 64 wide, 32 high, in the x-z plane
tx = (1., 0, 0, 0, 0, 0, -1, 0)                                        # s along x, t down z, one texel per unit
check('lightmap_size: 64x32 texels are 5x3 samples (CalcSurfaceExtents)', bspviews.lightmap_size(quad, tx) == (5, 3),
      str(bspviews.lightmap_size(quad, tx)))
check('lightmap_size: an offset of 8 texels needs a sample more', bspviews.lightmap_size(quad, (1., 0, 0, 8, 0, 0, -1, 0)) == (6, 3))
check('texel_scale: texinfo of 0.5 texels per unit is 2 units per texel', C.texel_scale((0.5, 0, 0, 0, 0, 0, -1, 0)) == (2.0, 1.0))
check('tile_extent: the wall spans 1 x 0.5 tiles of a 64x64 texture', C.tile_extent(quad, tx, 64, 64) == (1.0, 0.5))
check('orientation by the normal', C.orientation((0, 0, 1)) == 'floor' and C.orientation((0, 0.8, -0.6)) == 'wall'
      and C.orientation((0.3, 0, -0.95)) == 'ceiling')
up, side = np.array([0., 0, 1]), np.array([1., 0, 0])
ed = {('k1',): [('trim', side, 64.0), ('wall', side, 64.0)], ('k2',): [('wall', side, 32.0), ('floor', up, 32.0)],
      ('k3',): [('wall', side, 16.0), ('wall', side, 16.0)]}
bs, co = C.neighbours(ed)
check('neighbours: the same plane is beside, an angle a corner, the texture itself neither',
      bs['trim']['wall'] == 64 and bs['wall']['trim'] == 64 and co['wall']['floor'] == 32 and 'wall' not in bs['wall'] and not co['trim'])
check('edge_key: an edge is the same either way round', C.edge_key(np.array([1., 2, 3]), np.array([4., 5, 6])) == C.edge_key(np.array([4., 5, 6]), np.array([1., 2, 3])))
check('wpercentile weighs by area', bspviews.wpercentile([10, 200], [9, 1], 50) == 10 and bspviews.wpercentile([10, 200], [1, 9], 50) == 200)
ranked = bspviews.rank_faces([(0, 1, 1000.0, 2.0, 'world'), (0, 2, 600.0, 120.0, 'world'), (3, 4, 2000.0, 120.0, 'mover')])
check('rank_faces: a lit face before a larger dark one, a platform after', [f[1] for f in ranked] == [2, 4, 1], str([f[1] for f in ranked]))
check('directions: a wall is seen along its normal first; a floor from above, a ceiling within the pitch clamp',
      np.allclose(bspviews.directions(np.array([0., 1, 0]))[0], [0, 1, 0])
      and all(u[2] > 0 for u in bspviews.directions(np.array([0., 0, 1])))
      and all(bspviews.aim(u * 100, np.zeros(3)) for u in bspviews.directions(np.array([0., 0, -1]))))
check('aim: straight up is outside the clamp, 45 degrees down is pitch 45',
      bspviews.aim(np.array([0., 0, 0]), np.array([0., 0, 10])) is None
      and abs(bspviews.aim(np.array([0., 0, 10]), np.array([10., 0, 0]))[0] - 45) < 1e-6)
# a one-plane world: x >= 0 open, x < 0 solid
w = bspviews.Bsp.__new__(bspviews.Bsp)
w._planes = [(1.0, 0.0, 0.0, 0.0)]
w.nodes = [(0, -1, -2)]
w.leafs = [(bspviews.CONTENTS_EMPTY,), (bspviews.CONTENTS_SOLID,)]
w.models = [tuple([0] * 9 + [0])]
check('trace: a segment in the open is clear, one into the solid not',
      w.trace((1., 0, 0), (50., 3, 0)) and not w.trace((10., 0, 0), (-0.01, 0, 0)) and w.contents((5., 0, 0)) == bspviews.CONTENTS_EMPTY)
check('hub_of', C.hub_of('village3') == 'blackmarsh' and C.hub_of('rider2c') == 'mazaera' and C.hub_of('tibet10') == 'tulku'
      and C.hub_of('castle4') == 'cathedral' and C.hub_of('thomas') == 'other')
log = ['T92_VIEW rtex022 close ', 'here: world opaque, 120 units away: rtex022 (material 4)',
       '  files: textures/rtex022~1a2b (this texture\'s pixels only) or textures/rtex022 (every texture of the name)',
       'T92_VIEW rtex022 wide ', 'here: *3 opaque, 300 units away: rtex100 (material 9)',
       '  files: textures/rtex100~0c0c (this texture\'s pixels only) or textures/rtex100 (every texture of the name)',
       'T92_VIEW p0rune1 close ', 'here: world opaque, 80 units away: +2rune1 (material 12)',
       '  files: textures/+2rune1~77aa (this texture\'s pixels only) or textures/+2rune1 (every texture of the name)',
       'T92_VIEW sky close ', 'vk_materials here: nothing at the view\'s center', 'T92_END']
hits = C.parse_log(log)
check('parse_log reads each view\'s hit', hits[('rtex022', 'close')]['name'] == 'rtex022' and hits[('rtex022', 'wide')]['crc'] == '0c0c'
      and hits[('sky', 'close')]['name'] is None and 'nothing' in hits[('sky', 'close')]['what'], str(hits))
check('hit_matches: the name, the CRC where the stem has one, any frame of an animation',
      C.hit_matches('rtex022', hits[('rtex022', 'close')]) and C.hit_matches('rtex022~1a2b', hits[('rtex022', 'close')])
      and not C.hit_matches('rtex022~ffff', hits[('rtex022', 'close')]) and not C.hit_matches('rtex022', hits[('rtex022', 'wide')])
      and C.hit_matches('+0rune1', hits[('p0rune1', 'close')], {'+0rune1', '+2rune1~77aa'})
      and not C.hit_matches('+0rune1', hits[('p0rune1', 'close')]) and not C.hit_matches('x', hits[('sky', 'close')]))
check('fmt: whole above 10, a decimal above 1, two below', [C.fmt(x) for x in (123.4, 3.25, 1.0, 0.25, 0.5)] == ['123', '3.2', '1', '0.25', '0.5'])
# story 9.3: the labels
import labels as L  # noqa: E402
pal = np.stack([np.arange(256), 255 - np.arange(256), np.arange(256) // 2], axis=1).astype(np.uint8)  # 256 colors, all distinct
ix = np.zeros((4, 4), np.int64)
ix[0] = 5            # grey
ix[1] = 40           # slate
ix[2] = 230          # amber
ix[3, :2] = 245      # special
ix[3, 2:] = 255      # white: grey
tex = np.concatenate([pal[ix], np.full((4, 4, 1), 255, np.uint8)], axis=2)
tex[3, 3, 3] = 0     # a hole
rr = L.texel_ramps(tex, pal)
check('texel_ramps: a texel\'s ramp by its palette index, holes -1',
      [L.RAMP_NAMES[i] for i in rr[:3, 0]] == ['grey', 'slate', 'amber'] and L.RAMP_NAMES[rr[3, 0]] == 'special'
      and L.RAMP_NAMES[rr[3, 2]] == 'grey' and rr[3, 3] == -1, str(rr.tolist()))
sh = L.shares(rr)
check('shares of the opaque texels', abs(sh['grey'] - 5 / 15) < 1e-9 and abs(sh['slate'] - 4 / 15) < 1e-9 and abs(sum(sh.values()) - 1) < 1e-9, str(sh))
check('every palette index has a ramp', len(L.RAMPS) == len(L.RAMP_NAMES) == len(L.FALSE) and L.ramp_of_index().max() == len(L.RAMPS) - 1
      and sorted(set(range(255))) == sorted(i for _, a, b in L.RAMPS for i in range(a, b)))
check('parse_regions: ramp sets, `*`, two materials of a ramp',
      L.parse_regions('grey,taupe=iron; *=wood ;red=iron/stone') == [(('grey', 'taupe'), ('iron',)), (('*',), ('wood',)), (('red',), ('iron', 'stone'))])
fams = {'bm-doors': {'family': 'bm-doors', 'lead': ''}}
good = {'purpose': 'door', 'tier': 'layout', 'class': 'wood', 'family': 'bm-doors', 'regions': 'grey=iron;amber,slate=wood',
        'description': 'an oak door', 'overrides': ''}
check('check_row: a good row', L.check_row('x', 'world', good, sh, classes, fams) == [], str(L.check_row('x', 'world', good, sh, classes, fams)))
bad = [(dict(good, regions=''), 'no regions'), (dict(good, regions='grey=iron'), 'without a material'),
       (dict(good, regions='grey=iron;amber=wood;slate=wood;red=gold'), 'red is 0.0 %'),
       (dict(good, regions='grey=iron;*=wood;*=gold'), "'*' named twice"), (dict(good, regions='grey=nope;*=wood'), "material 'nope'"),
       (dict(good, tier='glass'), 'tier glass takes class glass'), (dict(good, **{'class': 'lava'}), 'class lava goes with tier lava'),
       (dict(good, purpose='creature'), 'purpose'), (dict(good, family='zz'), "family 'zz'"), (dict(good, description=''), 'no description')]
for rowb, want in bad:
    got = L.check_row('x', 'world', rowb, sh, classes, fams)
    check(f'check_row finds: {want}', any(want in g for g in got), str(got))
check('check_row: `*` covers the rest, a skin\'s purposes',
      L.check_row('x', 'world', dict(good, regions='*=wood'), sh, classes, fams) == []
      and L.check_row('m', 'skin', dict(good, purpose='creature', tier='skin', **{'class': 'skin'}), sh, classes, fams) == [])
lrow = lambda pat, tier, cls, src, d='', o='': {'pattern': pat, 'purpose': 'fill', 'tier': tier, 'class': cls, 'family': '', 'regions': '*=stone',  # noqa: E731
                                                'description': d, 'overrides': o, 'source': src}
rows3 = [row('rtex*', 'wood', 'old', 'bump=0.3'), lrow('rtex1', 'reimagine', 'stone', 'claude', 'new'), lrow('rtex1', 'reimagine', 'brick', 'human', 'owner')]
r3 = M.resolve('rtex1', 'world', rows3, classes)
check('a labeled row stands alone, the owner\'s over Claude\'s', r3.cls == 'brick' and r3.description == 'owner' and r3.tier == 'reimagine'
      and r3.params['bump'] == classes['defaults']['bump'], f'{r3.cls} {r3.description} {r3.params["bump"]}')
check('without a label the old rows apply', M.resolve('rtex2', 'world', rows3, classes).cls == 'wood' and M.resolve('rtex2', 'world', rows3, classes).tier == '')


class _E:
    def __init__(self, stem, kind='world'):
        self.stem, self.kind = stem, kind


with tempfile.TemporaryDirectory() as tmp:
    man, lab_csv = os.path.join(tmp, 'm.csv'), os.path.join(tmp, 'l.csv')
    M.write_manifest(man, [row('rtex*', 'wood', 'old', 'bump=0.3'), row('ttex*', 'stone', 'kept'), row('+0a', 'rune', 'draft', '', 'draft')])
    with open(lab_csv, 'w', encoding='utf-8', newline='') as f:
        f.write('pattern,purpose,tier,class,family,regions,description,overrides,question\n'
                'rtex1,fill,reimagine,,,*=wood,planks,denoise=0.2,is it?\n+0a,symbol,faithful,rune,,*=stone,a rune,,\n')
    ents = {s: _E(s) for s in ('rtex1', '+0a', '+1a', 'ttex1')}
    n, dropped, _, folded = L.apply(lab_csv, man, ents, classes, {'+0a': {'anim': '+0a +1a'}})
    got = {q['pattern']: q for q in M.load_manifest(man)}
    check('apply: the old class and overrides folded in, the label\'s own added',
          got['rtex1']['class'] == 'wood' and M.parse_overrides(got['rtex1']['overrides']) == {'bump': '0.3', 'denoise': '0.2'}
          and got['rtex1']['source'] == 'claude', str(got.get('rtex1')))
    check('apply: an animation\'s other frames copied, covered rows dropped, the others kept',
          got['+1a']['description'] == 'a rune' and 'rtex*' not in got and 'ttex*' in got and n == 3 and dropped == 2, f'{sorted(got)} {n} {dropped}')
    fpath = os.path.join(tmp, 'f.csv')
    with open(fpath, 'w', encoding='utf-8', newline='') as f:
        f.write('# c\nfamily,hub,lead,what\nbm-x,blackmarsh,,"a, b"\nbm-y,blackmarsh,keep,c\n')
    lr = [lrow('a1', 'reimagine', 'stone', 'claude'), lrow('a2', 'reimagine', 'stone', 'claude')]
    for q in lr:
        q['family'] = 'bm-x'
    done = L.set_leads(fpath, lr, {'a1': {'area': '10'}, 'a2': {'area': '300'}})
    fl = L.load_families(fpath)
    check('set_leads: the member with the most area, a set lead and the comments kept',
          done == ['bm-x'] and fl['bm-x']['lead'] == 'a2' and fl['bm-x']['what'] == 'a, b' and fl['bm-y']['lead'] == 'keep'
          and open(fpath, encoding='utf-8').read().startswith('# c'), str(fl))
fam_file = L.load_families(os.path.join(HERE, 'families.csv'))
PREFIX = {'blackmarsh': 'bm', 'mazaera': 'mz', 'thysis': 'th', 'septimus': 'sp', 'cathedral': 'ef', 'keep': 'kp', 'tulku': 'tk', 'other': 'ot'}
check('families.csv: hubs are census hubs (or other), names carry their hub\'s prefix',
      fam_file and all(PREFIX.get(f['hub']) == f['family'].split('-')[0] for f in fam_file.values()),
      str([f['family'] for f in fam_file.values() if PREFIX.get(f['hub']) != f['family'].split('-')[0]]))


class _F(_E):
    def __init__(self, stem):
        super().__init__(stem)
        self.file = stem + '.png'


with tempfile.TemporaryDirectory() as tmp:
    for s in ('t1', 't2', 'a1', 'a2'):
        Image.fromarray(tex).save(os.path.join(tmp, s + '.png'))
    ents = {s: _F(s) for s in ('t1', 't2', 'a1', 'a2')}
    lr = [dict(good, pattern='t1', source='claude', regions='*=stone'), dict(good, pattern='t2', source='claude', regions='*=stone', family=''),
          dict(good, pattern='a1', source='claude', regions='*=stone'), dict(good, pattern='a2', source='claude', regions='*=wood')]
    cen = {'a1': {'anim': 'a1 a2'}, 'a2': {'anim': 'a1 a2'}}
    _, probs = L.check(ents, [ents['t1'], ents['t2']], lr, classes, pal, tmp, cen, {'bm-doors': {'family': 'bm-doors', 'lead': 'a1'}})
    check('check: textures of the same pixels labeled alike, the lead counted among all labeled rows',
          list(probs) == ['t2'] and 'same pixels as t1' in probs['t2'][0], str(probs))
    _, probs = L.check(ents, [ents['a1'], ents['a2']], lr, classes, pal, tmp, cen, fams)
    check('check: an animation\'s frames alike, not held to the same-pixels rule', list(probs) == ['a2'] and len(probs['a2']) == 1 and 'frame of a1' in probs['a2'][0], str(probs))
check('check_row: a region needs ramps and a material',
      all(any('needs ramps and a material' in g for g in L.check_row('x', 'world', dict(good, regions=rg), sh, classes, fams))
          for rg in ('grey=;*=wood', '*=wood/', '=wood;grey,amber,slate=iron')))
import types  # noqa: E402
import texpack as TP  # noqa: E402
with tempfile.TemporaryDirectory() as tmp:
    p = os.path.join(tmp, 'q.csv')
    with open(p, 'w', encoding='utf-8', newline='') as f:
        f.write('# a comment, "quoted"\npattern,description\nx,"two\n\n# not a comment, ""q"""\ny,one\n')
    head, recs = M.read_rows(p)
    check('read_rows: comments skipped after the parse, a quoted cell keeps blank and # lines',
          head == ['pattern', 'description'] and recs[0]['description'] == 'two\n\n# not a comment, "q"' and recs[1]['pattern'] == 'y', str(recs))
    man, lab_csv = os.path.join(tmp, 'm.csv'), os.path.join(tmp, 'l.csv')
    M.write_manifest(man, [dict(lrow('rtex1', 'reimagine', 'stone', 'human', 'owner\'s'), overrides='bump=2'),
                           lrow('rtex2', 'reimagine', 'stone', 'claude', 'old'), row('sk*', 'skin', 'a skin'), row('zz*', 'stone', 'elsewhere')])
    with open(lab_csv, 'w', encoding='utf-8', newline='') as f:
        f.write('pattern,purpose,tier,class,family,regions,description,overrides\n'
                'rtex1,fill,reimagine,stone,,*=stone,Claude\'s,\nrtex2,fill,reimagine,brick,,*=brick,new,\n')
    ents = {s: _E(s) for s in ('rtex1', 'rtex2', 'sk1')}
    res = L.apply(lab_csv, man, ents, classes, {})
    got = {q['pattern']: q for q in M.load_manifest(man)}
    check('apply: the owner\'s labeled row kept, Claude\'s replaced, rows of other textures and exports kept',
          got['rtex1']['description'] == 'owner\'s' and got['rtex1']['source'] == 'human' and got['rtex2']['class'] == 'brick'
          and 'sk*' in got and 'zz*' in got and res[0] == 1 and res[2] == 1, f'{res} {sorted(got)}')
    # merge: a 5.8 sheet's empty overrides keep the row's; an animation's frames follow (the census's +0rune1)
    M.write_manifest(man, [dict(lrow('+0rune1', 'faithful', 'rune', 'claude', 'a rune'), overrides='bump=2'),
                           lrow('+1rune1', 'faithful', 'rune', 'claude', 'a rune')])
    edits = os.path.join(tmp, 'e.csv')
    with open(edits, 'w', encoding='utf-8', newline='') as f:
        f.write('pattern,class,description,overrides,source\n+0rune1,rune,"the owner\'s rune, red",,human\n')
    TP.cmd_merge(types.SimpleNamespace(edits=edits, manifest=man))
    got = {q['pattern']: q for q in M.load_manifest(man)}
    check('merge: an empty overrides cell keeps the row\'s; the animation\'s other frames follow',
          got['+0rune1']['overrides'] == 'bump=2' and got['+0rune1']['source'] == 'human'
          and got['+1rune1']['description'] == 'the owner\'s rune, red' and got['+1rune1']['source'] == 'human', str(got))
sys.exit(1 if fails else 0)
