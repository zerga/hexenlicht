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
check('hero=none (9.5): an override of its own', M.resolve('x', 'world', [row('x', 'stone', '', 'hero=none')], classes).params.get('hero') == 'none')
check('mirror=x and mirror=xy (9.5): overrides of their own', M.resolve('x', 'world', [row('x', 'stone', '', 'mirror=x')], classes).params.get('mirror') == 'x'
      and M.resolve('x', 'world', [row('x', 'stone', '', 'mirror=xy')], classes).params.get('mirror') == 'xy')
check('join (9.5): a stem, or a stem and its ramps',
      M.resolve('x', 'world', [row('x', 'stone', '', 'join=rtex388~6280')], classes).params['join'] == ('rtex388~6280', ())
      and M.resolve('x', 'world', [row('x', 'stone', '', 'join=y@green,olive;tile=0')], classes).params['join'] == ('y', ('green', 'olive')))
for what, rr in (('an unknown class', row('x', 'nope')), ('an unknown override key', row('x', 'stone', '', 'denoize=1')),
                 ('a hero other than none', row('x', 'stone', '', 'hero=rtex251')), ('tile other than 0 or 1', row('x', 'stone', '', 'tile=2')),
                 ('a mirror other than x or xy', row('x', 'stone', '', 'mirror=y')), ('a join without its stem', row('x', 'stone', '', 'join=@green')),
                 ('a join with an empty ramp', row('x', 'stone', '', 'join=y@green,'))):
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
bigx = np.concatenate([big, big[:, -4:]], 1)     # pad's one extra column at the right
mx = R.mirror_x(bigx, m4, w4)
tx = mx[m4:m4 + h4, m4:m4 + w4]
check('mirror_x (9.5): the tile left-right symmetric, its left half and the padding beside it as drawn, the extra column too',
      np.array_equal(tx, tx[:, ::-1]) and np.array_equal(mx[:, :m4 + w4 // 2], bigx[:, :m4 + w4 // 2])
      and np.array_equal(mx[:, m4 + w4 // 2:2 * m4 + w4], bigx[:, :m4 + w4 // 2][:, ::-1]) and np.array_equal(mx[:, -4:], bigx[:, -4:]))

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
import types  # noqa: E402
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
    with open(fpath, 'w', encoding='utf-8', newline='') as f:
        f.write('family,hub,lead,what\nmd-x,models,,skins\n')
    for q, s in zip(lr, ('models/a.mdl_0', 'models/b.mdl_0')):
        q.update(family='md-x', pattern=s)
    big = types.SimpleNamespace(w=300, h=200)
    L.set_leads(fpath, lr, {}, {'models/a.mdl_0': types.SimpleNamespace(w=100, h=50), 'models/b.mdl_0': big})
    check('set_leads: a skin family\'s lead is its biggest atlas', L.load_families(fpath)['md-x']['lead'] == 'models/b.mdl_0')
fam_file = L.load_families(os.path.join(HERE, 'families.csv'))
PREFIX = {'blackmarsh': 'bm', 'mazaera': 'mz', 'thysis': 'th', 'septimus': 'sp', 'cathedral': 'ef', 'keep': 'kp', 'tulku': 'tk', 'other': 'ot',
          'models': 'md'}
check('families.csv: hubs are census hubs (or other, or models for the skins), names carry their hub\'s prefix',
      fam_file and all(PREFIX.get(f['hub']) == f['family'].split('-')[0] for f in fam_file.values()),
      str([f['family'] for f in fam_file.values() if PREFIX.get(f['hub']) != f['family'].split('-')[0]]))
check('families.csv: no row has more cells than the header (a `what` with commas is quoted); finishes only for world families (9.5)',
      all(None not in f for f in fam_file.values()) and not any(f['hub'] == 'models' and (f.get('finish') or '').strip() for f in fam_file.values()),
      str([f['family'] for f in fam_file.values() if None in f or (f['hub'] == 'models' and f.get('finish'))]))


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
with tempfile.TemporaryDirectory() as tmp:
    Image.fromarray(tex).save(os.path.join(tmp, 'k1.png'))
    Image.fromarray(tex).resize((8, 8), Image.NEAREST).save(os.path.join(tmp, 'k2.png'))   # the same shares, no texels shared
    kk = [_F('k1'), _F('k2')]
    kk[0].w = kk[0].h = 4
    kk[1].w = kk[1].h = 8
    kinds = [{k for v in L.candidates(tmp, kk, pal, {}, skins=c).values() for _, k, _ in v} for c in (False, True)]
    # two atlases of one size, mostly black, their pieces different: shared texels only for world textures
    at = np.zeros((10, 10, 3), np.uint8)
    at[0, :2] = 200
    Image.fromarray(at).save(os.path.join(tmp, 'm1.png'))
    at[0, :2] = 90
    Image.fromarray(at).save(os.path.join(tmp, 'm2.png'))
    mm2 = [_F('m1'), _F('m2')]
    for q in mm2:
        q.w = q.h = 10
    tl = [L.candidates(tmp, mm2, pal, {}, skins=c).get('m1', []) for c in (False, True)]
check('candidates: color links not for skins; a skin\'s shared texels leave out the black both have',
      kinds == [{'colors'}, set()] and [k for _, k, _ in tl[0]] == ['texels'] and tl[1] == [], f'{kinds} {tl}')
check('skin_model: a skin\'s model and index, the export\'s ~crc dropped',
      L.skin_model('models/imp.mdl_2') == ('models/imp.mdl', 2) and L.skin_model('models/puzzle/scepter.mdl_0~4107') == ('models/puzzle/scepter.mdl', 0))
# mdlview: a model of three quads, front (x 10, facing +x), back (x 0) and left side (y 25), each with its texels
import mdlview as MV  # noqa: E402
import struct  # noqa: E402


def _mdl(rapo, group=False):
    quads = [  # corners (x, y, z), s per corner
        ([(10, 0, 0), (10, 20, 0), (10, 20, 20), (10, 0, 20)], [0, 3, 3, 0]),     # front: texels 0-3 (10 10 11 11)
        ([(0, 0, 0), (0, 20, 0), (0, 20, 20), (0, 0, 20)], [4, 7, 7, 4]),         # back: texels 4-7 (20)
        ([(0, 25, 0), (10, 25, 0), (10, 25, 20), (0, 25, 20)], [8, 11, 11, 8])]   # left side: texels 8-11 (30)
    verts, st, tris = [], [], []
    for corners, ss in quads:
        b = len(verts)
        verts += corners
        st += [(0, s, 0) for s in ss]
        tris += [(1, (b, b + 1, b + 2)), (1, (b, b + 2, b + 3))]
    sw, sh_ = 12, 1
    skin = bytes([10, 10, 11, 11, 20, 20, 20, 20, 30, 30, 30, 30])
    head = struct.pack('<4si3f3ff3f6iiif', b'RAPO' if rapo else b'IDPO', 6, 1, 1, 1, 0, 0, 0, 30, 0, 0, 0,
                       1, sw, sh_, len(verts), len(tris), 1, 0, 0, 0)
    order = list(range(len(st)))[::-1] if rapo else list(range(len(st)))   # RAPO: its own order of texture coordinates
    body = (struct.pack('<i', len(st)) if rapo else b'') + struct.pack('<i', 0) + skin
    body += b''.join(struct.pack('<3i', *st[i]) for i in order)
    for front, vi in tris:
        if rapo:
            body += struct.pack('<i3H3H', front, *vi, *(order.index(v) for v in vi))
        else:
            body += struct.pack('<i3i', front, *vi)
    pose = bytes(8) + b'f0'.ljust(16, b'\0') + b''.join(struct.pack('<4B', x, y, z, 0) for x, y, z in verts)
    if group:       # a frame group of two poses
        body += struct.pack('<ii', 1, 2) + bytes(8) + struct.pack('<2f', 0.1, 0.2) + pose + pose
    else:
        body += struct.pack('<i', 0) + pose
    return head + body


gpal = np.repeat(np.arange(256, dtype=np.uint8)[:, None], 3, axis=1)
for rapo in (False, True):
    mm = MV.read_mdl(_mdl(rapo))
    sk = MV.skin_rgb(mm, 0, gpal)
    v0, v180, v90 = (MV.render(mm, sk, 0, yaw, 0, 64, bg=(0, 0, 0))[..., 0] for yaw in (0, 180, 90))
    row0 = v0[32][v0[32] > 0]
    check(f'mdlview ({"RAPO" if rapo else "IDPO"}): the front at yaw 0, its +y on the right, the back at 180, the left side at 90',
          mm['rapo'] == rapo and len(mm['tris']) == 6 and row0[0] == 10 and row0[-1] == 11
          and set(np.unique(v180[v180 > 0])) == {20} and 30 in set(np.unique(v90)) and 10 not in set(np.unique(v90)),
          f'{row0[:3]} {row0[-3:]} {np.unique(v180)} {np.unique(v90)}')
gm = MV.read_mdl(_mdl(False, group=True), max_frames=5)
check('mdlview: a frame group\'s poses each counted and read', gm['numframes'] == 2 and len(gm['frames']) == 2, str(gm['numframes']))
with tempfile.TemporaryDirectory() as tmp:
    vers = []
    for game, rapo in (('data1', False), ('portals', True)):
        b = _mdl(rapo)
        with open(os.path.join(tmp, game + '.mdl'), 'wb') as f:
            f.write(b)
        vers.append((game, os.path.join(tmp, game + '.mdl'), 0, len(b)))
    fl = {'models/t.mdl': vers}
    lv = [MV.load(fl, 'models/t.mdl', (12, 1), g) for g in ('data1', 'portals', None)]
    try:
        MV.load(fl, 'models/t.mdl', (13, 1))
        bad = False
    except ValueError:
        bad = True
    check('mdlview.load: the export\'s game\'s version of the skin\'s size, the last game\'s without one, none of another size',
          [(m_['game'], m_['rapo']) for m_ in lv] == [('data1', False), ('portals', True), ('portals', True)] and bad, str([m_['game'] for m_ in lv]))
seam = {'sw': 8, 'st': np.array([[1, 2, 0], [0, 3, 0], [0, 3, 4]]), 'tris': [(0, (0, 1, 2), (0, 1, 2)), (1, (0, 1, 2), (0, 1, 2))]}
check('mdlview.tri_uv: a back face\'s onseam corner half a skin to the right, texel centers',
      MV.tri_uv(seam, 0).tolist() == [[6.5, 0.5], [3.5, 0.5], [3.5, 4.5]] and MV.tri_uv(seam, 1)[0].tolist() == [2.5, 0.5])
with tempfile.TemporaryDirectory() as tmp:
    os.makedirs(os.path.join(tmp, 'gamecode', 'hc', 'h2'))
    with open(os.path.join(tmp, 'gamecode', 'hc', 'h2', 'imp.hc'), 'w', encoding='latin-1') as f:
        f.write('/*QUAKED monster_imp (1 0 0) (-16 -16 0) (16 16 55)\nan imp\n*/\nvoid monster_imp ()\n{\n'
                '\tsetmodel (self, "models/imp.mdl");\n}\n\nvoid() imp_think =\n{\n\tif (self.model == "models/h_imp.mdl")\n'
                '\t\tsetmodel (self, "models/Fireball.mdl");\n};\n'
                '// setmodel (self, "models/commented.mdl");\n/*\nvoid() old =\n{\n\tsetmodel (self, "models/block.mdl");\n};\n*/\n'
                'void init_beast (float which, void() th) [++ $a .. $b]\n{\n\tsetmodel (self, "models/beast.mdl"); // "models/x.mdl"\n}\n'
                '/*QUAKED monster_beast_fire (1 0 0) (-16 -16 0) (16 16 55)\n*/\nvoid monster_beast_fire ()\n{\n\tinit_beast (0, SUB_Null);\n}\n'
                '/*QUAKED monster_beast_ice (1 0 0) (-16 -16 0) (16 16 55)\n*/\nvoid() monster_beast_ice =\n{\n\tinit_beast(1, SUB_Null);\n};\n')
    us = MV.gamecode_users(tmp, ('h2',))
    check('gamecode_users: a spawn class from the QUAKED comment heading its function, a function\'s name, comparisons left out',
          us.get('models/imp.mdl') == {('imp.hc', 'monster_imp', 'monster_imp')} and us.get('models/fireball.mdl') == {('imp.hc', 'imp_think', '')}
          and 'models/h_imp.mdl' not in us, str(dict(us)))
    check('gamecode_users: comments left out; a frame-macro helper with a void() parameter credited to the spawn functions calling it',
          not {'models/commented.mdl', 'models/block.mdl', 'models/x.mdl'} & set(us)
          and us.get('models/beast.mdl') == {('imp.hc', 'init_beast', 'monster_beast_fire'), ('imp.hc', 'init_beast', 'monster_beast_ice')},
          str(dict(us)))
    ents = ({'monster_imp': {'demo1', 'demo2'}}, {'scepter': {('Vajra Scepter', 'tibet8')}})
    check('users_text: spawn classes and their maps, a puzzle item\'s name',
          MV.users_text('models/imp.mdl', us, ents) == 'spawned as monster_imp | maps demo1 demo2'
          and MV.users_text('models/puzzle/scepter.mdl', {}, ents) == 'puzzle item "Vajra Scepter" | maps tibet8', MV.users_text('models/imp.mdl', us, ents))
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

# -- story 9.4: materials per region, groups, the redraw's driver --------------------------
import groups as G  # noqa: E402
import imaging as IM  # noqa: E402
import materials as MT  # noqa: E402
pal9 = np.stack([np.arange(256), 255 - np.arange(256), (np.arange(256) * 7) % 256], axis=1).astype(np.uint8)
pal9[:32] = (np.arange(32) * 4)[:, None]    # a grey ramp from black, as the game's; all 256 distinct
tex9 = pal9[np.full((16, 16), 10)]          # grey ramp (0-31): stone
tex9[4:12, 4:12] = pal9[165]                # yellow ramp (160-175): gold
mtab = MT.table(classes)
sg = M.resolve('t', 'world', [dict(lrow('t', 'layout', 'stone', 'claude'), regions='*=stone;yellow=gold')], classes)
idx9, names9, vals9 = MT.texel_materials(tex9, None, pal9, sg.regions, sg.params, mtab)
check('texel_materials: a ramp\'s material, `*` the rest', names9 == ['(class)', 'stone', 'gold'] and idx9[5, 5] == 2 and idx9[0, 0] == 1
      and vals9[2]['metallic'] == 1.0 and vals9[1]['contrast'] == 1.5, f'{names9} {idx9[5, 5]} {idx9[0, 0]}')
idx0, _, _ = MT.texel_materials(tex9, None, pal9, 'yellow=gold', sg.params, mtab)
hole9 = np.full((16, 16), 255, np.uint8)
hole9[0] = 0
idxh, _, _ = MT.texel_materials(tex9, hole9, pal9, sg.regions, sg.params, mtab)
check('texel_materials: ramps no region names take the class\'s values, holes none', idx0[0, 0] == 0 and idx0[5, 5] == 1 and idxh[0, 3] == -1)
texb = tex9.copy()
texb[0, :4] = pal9[[0, 1, 2, 3]]            # the grey ramp's darkest: three near-black, one grey
idxb, namesb, _ = MT.texel_materials(texb, None, pal9, 'grey=iron;black=wood;*=stone', sg.params, mtab)
idxg, _, _ = MT.texel_materials(texb, None, pal9, 'grey=iron;*=stone', sg.params, mtab)
check('texel_materials: `black` takes the near-black (indices 0-2) from grey; without it they stay grey\'s',
      [namesb[i] for i in idxb[0, :4]] == ['wood', 'wood', 'wood', 'iron'] and namesb[idxb[5, 5]] == 'stone'
      and len(set(idxg[0, :4].tolist())) == 1, f'{namesb} {idxb[0, :4]} {idxg[0, :4]}')
check('check_row: `black` names no ramp but passes', not [p for p in L.check_row('x', 'world', dict(good, regions='grey=iron;black,amber,slate=wood'), sh, classes, fams) if 'black' in p])
W9 = MT.weights(idx9, 3, 4, True)
check('weights sum to 1 and follow the materials', float(np.abs(W9.sum(0) - 1).max()) < 1e-9 and W9[2][32, 32] > 0.99 and W9[1][2, 2] > 0.99)
guide9 = np.zeros((64, 64))
guide9[14:50, 14:50] = 1.0                  # the redraw drew the square 2 px larger than 4x its texels
Wg9 = MT.weights(idx9, 3, 4, True, guide=guide9)
check('weights: the guided filter moves a material\'s edge towards the drawing\'s', Wg9[2][32, 15] > W9[2][32, 15] + 0.1
      and Wg9[2][32, 14] > W9[2][32, 14] and Wg9[2][32, 40] > 0.99, f'{W9[2][32, 15]:.2f} -> {Wg9[2][32, 15]:.2f}')
rng9 = np.random.default_rng(9)
alb9 = rng9.integers(10, 200, (64, 64, 3), dtype=np.uint8)
orig9 = rng9.integers(10, 120, (16, 16, 3), dtype=np.uint8)
one9 = np.zeros((16, 16), np.int32)
v1 = [MT.class_values(sg.params)]
t1, whole1 = MT.targets(orig9, None, one9, v1)
a1, _ = MT.match(alb9, None, MT.weights(one9, 1, 4, True), v1, t1, whole1)
a2, _ = P.match_luminance(alb9, None, P.mean_linear_luminance(orig9) * v1[0]['albedo_ratio'], v1[0]['contrast'])
check('match: one material is 5.8\'s brightness match', int(np.abs(a1.astype(int) - a2.astype(int)).max()) <= 1)
t2, whole2 = MT.targets(tex9, None, idx9, vals9)
a3, _ = MT.match(rng9.integers(20, 220, (64, 64, 3), dtype=np.uint8), None, W9, vals9, t2, whole2)
y3 = IM._luma(a3)
cg, cs = W9[2] >= 0.99, W9[1] >= 0.99
p99g = float(np.percentile(IM._lin(a3)[cg].max(axis=-1), 99))
check('match: each material\'s mean reaches its own target (a metal, or stops before a channel clips)',
      (abs(y3[cg].mean() / t2[2] - 1) < 0.1 or abs(p99g - MT.NO_CLIP) < 0.03) and abs(y3[cs].mean() / t2[1] - 1) < 0.1,
      f'gold x{y3[cg].mean() / t2[2]:.3f} (p99 channel {p99g:.3f}), stone x{y3[cs].mean() / t2[1]:.3f}')
# 9.5 (M75): a metal's ratio 5, at most a real metal's reflectance; a dark gold region reaches its target
check('metals (9.5): Raven\'s dark iron 5 times as bright, a bright gilding at most real gold\'s reflectance, a dielectric as it was',
      MT.effective_ratio(0.011, mtab['iron']) == 5.0 and abs(MT.effective_ratio(0.5, mtab['gold']) - 0.74 / 0.5) < 1e-9
      and MT.effective_ratio(0.05, mtab['stone']) == 1.2 and 'reflectance' not in mtab['stone'])
dark_gold = np.full((64, 64, 3), 12, np.uint8)
dark_gold[..., 2] = 4                       # a dark yellow
a4g, _ = MT.match(dark_gold, None, W9, vals9, t2, whole2, contrast=False)
wg = (W9[2] * IM._luma(a4g)).sum() / W9[2].sum()
check('metals (9.5): a dark gilding reaches its target (capped at real gold\'s reflectance), no channel clipped',
      abs(wg / t2[2] - 1) < 0.1 and t2[2] == 0.74 and IM._lin(a4g)[cg].max() < 0.96, f'gold x{wg / t2[2]:.3f}, max {IM._lin(a4g)[cg].max():.3f}')
r9 = MT.roughness(rng9.random((64, 64, 3)), W9, vals9)
check('roughness: each material inside its range', vals9[2]['rough_min'] - 1e-9 <= r9[cg].min() and r9[cg].max() <= vals9[2]['rough_max'] + 1e-9
      and vals9[1]['rough_min'] - 1e-9 <= r9[cs].min() and r9[cs].max() <= vals9[1]['rough_max'] + 1e-9)
m9 = MT.metallic(W9, vals9)
check('metallic per texel: the gold 1, the stone 0', m9[cg].min() > 0.99 and m9[cs].max() < 0.01)
gap9 = np.full((64, 64, 3), 120, np.uint8)
gap9[:, 30:34] = 3
sf9 = MT.surface(gap9, np.ones((1, 64, 64)))
check('surface: a near-black gap 0, the face 1', sf9[:, 31].max() < 0.01 and sf9[:, 10].min() > 0.99)
nrm9 = np.zeros((64, 64, 3), np.uint8)
nrm9[...] = (128, 128, 255)
nrm9[:, 28:30] = (200, 128, 220)            # a slope
oc9 = MT.occlusion(nrm9, 4.0, sf9)
check('occlusion: 1 on a face, less on a slope, 0 in the gap', oc9[0, 10] > 0.99 and oc9[0, 28] < 0.9 and oc9[0, 31] < 0.01, f'{oc9[0, 28]:.2f}')
flat9 = np.uint8(np.clip(field(64, 64, 3, 21) * 300 - 30, 10, 255))
lit9 = R._enc(R._lin(flat9) * np.linspace(0.5, 1.5, 64)[:, None, None])      # a light falling off down the drawing
lg9 = R.light_guard(flat9[1::4, 1::4], lit9)
check('light_guard takes a light across the whole drawing out, the detail stays',
      R.broad_step(lit9) > 0.6 and R.broad_step(lg9) < 0.25 and R._corr(R._highpass(ly(lit9), 4), R._highpass(ly(lg9), 4)) > 0.9,
      f'broad step {R.broad_step(flat9):.2f} -> lit {R.broad_step(lit9):.2f} -> {R.broad_step(lg9):.2f}')
# tile=y (M89): a strip that repeats only top to bottom
ty9 = np.arange(4 * 6 * 3, dtype=np.uint8).reshape(4, 6, 3)
py9 = P.pad(ty9, 2, 'y')
big_y9 = np.random.default_rng(24).integers(0, 255, (48, 56, 3)).astype(np.uint8)
by9, bx9 = R.blend_seams(big_y9, 8, 32, 40, x=False), R.blend_seams(big_y9, 8, 32, 40)
step9 = np.zeros((16, 16, 3), np.uint8)
step9[:, 8:] = 200
check('tile=y (9.5): padded wrapped top to bottom, mirrored at the sides; only the top band blended; its seam score the rows\'',
      M.resolve('x', 'world', [row('x', 'stone', '', 'tile=y')], classes).params['tile'] == 'y'
      and (py9[:2, 2:8] == ty9[-2:]).all() and (py9[2:6, :2] == ty9[:, 1::-1]).all() and (py9[2:6, -2:] == ty9[:, :-3:-1]).all()
      and (by9[8:, :4] == big_y9[16:40, 8:12]).all() and not (bx9[8:, :4] == big_y9[16:40, 8:12]).all()
      and R.seam_score(step9, 'y') == 0.0 and R.seam_score(step9) > 1.0)
o9 = P.make_orm(np.full((4, 4), 0.5), np.array([[0, 1, 0.5, 0]] * 4, float), np.full((4, 4), 0.25))
check('make_orm: the occlusion in R, a metallic map in B; without them 5.8\'s', o9[0, 0, 0] == 64 and o9[0, 1, 2] == 255 and o9[0, 2, 2] == 128
      and (P.make_orm(np.zeros((2, 2)), 1)[..., 0] == 255).all() and (P.make_orm(np.zeros((2, 2)), 1)[..., 2] == 255).all())
# 5.8's state key, pinned: the upscaled textures of a pack (the 5.9 skins: 2.3 hours) must not be
# redone because the redraw added something to the tables they hash (9.4's code review: [models])
imp9 = M.resolve('models/imp.mdl_0', 'skin', [row('models/imp.mdl_0', 'skin', 'an imp')], classes)
k58 = P.state_key(b'\x89PNG fixed bytes', imp9, classes['models'], P.seed_for('models/imp.mdl_0', 5800))
check('5.8\'s state key is the one before 9.4 (bump TOOL_VERSION to change it on purpose)', k58 == '8d3636e677564fe38521cdd7e939de917f8ec403', k58)
fh = MT.fill_holes(np.array([[1, -1, -1, 2], [1, -1, 2, 2]]), False)
check('fill_holes: a hole takes its nearest opaque texel\'s material, not the class\'s', (fh >= 1).all() and fh[0, 1] == 1 and fh[0, 2] == 2, str(fh))
rs9 = M.resolve('x', 'world', [row('x', 'stone', '', 'seed=1234')], classes)
check('a seed= override is an int; without one no seed key (5.8\'s state keys stay)',
      rs9.params['seed'] == 1234 and 'seed' not in M.resolve('x', 'world', [row('x', 'stone')], classes).params)

# the groups: copies, sets (shared texels, animations), heroes, the closure, unify
g9 = np.random.default_rng(3)


def rnd(seed, n=16):
    return pal9[np.random.default_rng(seed).integers(0, 32, (n, n))]


def variant(base, frac, seed):
    v = rnd(seed, base.shape[0])
    k = int(base.shape[0] * base.shape[1] * frac)
    v.reshape(-1, 3)[:k] = base.reshape(-1, 3)[:k]
    return v


fr = lambda tier='layout', fam='', d='x': types.SimpleNamespace(tier=tier, cls='stone', description=d, regions='*=stone',  # noqa: E731
                                                                 family=fam, params={'maps': True})
base9, h9 = rnd(100), rnd(101)
pix9 = {'a1': rnd(1), 'w1': variant(base9, 0.6, 2), 'w2': variant(base9, 0.6, 3), 'w3': variant(base9, 0.1, 4),
        '+0rn': rnd(5), '+1rn': rnd(6), 'h': h9, 'm3': variant(h9, 0.5, 7), 'm1': rnd(8), 'm2': rnd(9),
        'm4': variant(h9, 0.5, 10), 'h2': rnd(11)}
pix9['a2'] = pix9['a1'].copy()
res9 = {s: fr() for s in pix9}
res9.update({'w1': fr('layout', 'f'), 'w2': fr('reimagine', 'f'), 'h': fr('layout', 'f'), 'm1': fr('layout', 'f'),
             'm2': fr('faithful', 'f'), 'm3': fr('layout', 'f'), '+0rn': fr('faithful'), '+1rn': fr('faithful'),
             'm4': fr('layout', 'g'), 'h2': fr('layout', 'g')})
cen9 = {'a2': {'area': '100'}, 'a1': {'area': '10'}, 'w1': {'area': '50'}, 'w2': {'area': '40'}, 'h': {'area': '1000'},
        '+0rn': {'anim': '+0rn +1rn'}, '+1rn': {'anim': '+0rn +1rn'}}
plan9 = G.Plan(sorted(pix9), res9, pix9, cen9, {'f': 'h', 'g': 'h2'})
check('groups: a copy is drawn as its twin with the most area', plan9.rep['a1'] == 'a2' and plan9.rep['a2'] == 'a2')
check('groups: a set by shared texels (a fifth or more), the most area first; an animation\'s frames joined',
      plan9.group['w2'] == ['w1', 'w2'] and plan9.group['w3'] == ['w3'] and plan9.group['+1rn'] == ['+0rn', '+1rn'], str(plan9.group['w2']))
check('groups: the hero for layout members, not reimagine nor faithful ones nor its own set',
      plan9.hero.get('m1') == 'h' and plan9.hero.get('w1') == 'h' and 'w2' not in plan9.hero and 'm2' not in plan9.hero
      and 'm3' not in plan9.hero and 'h' not in plan9.hero)
res9n = dict(res9, m1=types.SimpleNamespace(**dict(vars(res9['m1']), params={'maps': True, 'hero': 'none'})))
check('groups (9.5): a row with hero=none gets no hero', 'm1' not in G.Plan(sorted(pix9), res9n, pix9, cen9, {'f': 'h', 'g': 'h2'}).hero
      and plan9.hero.get('m1') == 'h')
holes9 = {}
for i, s in enumerate(('g1', 'g2')):        # three quarters holes, black under them in both
    a = np.zeros((16, 16), np.uint8)
    a[:4] = 255
    holes9[s] = np.dstack([np.where(a[..., None] > 0, rnd(30 + i), 0).astype(np.uint8), a])
holes9.update({'L': rnd(32), 'M': rnd(33)})
res_h = {'g1': fr('layout'), 'g2': fr('layout'), 'L': fr('layout', 'f'), 'M': fr('layout', 'f')}
plan_h = G.Plan(sorted(holes9), res_h, holes9, {}, {'f': 'L', 'g': 'M'})     # M leads g but is labeled into f
check('groups: holes are no shared texels; a lead never takes another family\'s hero',
      plan_h.group['g2'] == ['g2'] and 'M' not in plan_h.hero and 'L' not in plan_h.hero, f"{plan_h.group['g2']} {plan_h.hero}")
d9, w9 = plan9.closure(['w2', 'a1'])
check('closure: every hero first (its set holds another family\'s member, whose hero comes too), sets whole, copies written',
      d9[:2] == ['h', 'h2'] and set(d9) == {'h', 'h2', 'm3', 'm4', 'w1', 'w2', 'a2'} and plan9.hero.get('m4') == 'h2'
      and set(w9) == {'h', 'h2', 'm3', 'm4', 'w1', 'w2', 'a1', 'a2'}, f'{d9} {w9}')
import reseed as RS9  # noqa: E402
res9b = dict(res9, a1=types.SimpleNamespace(**dict(vars(res9['a1']), params={'maps': True, 'seed': 5})))
plan9b = G.Plan(sorted(pix9), res9b, pix9, cen9, {'f': 'h', 'g': 'h2'})
check('twins (9.5): a seed goes onto the copies (the same pixels, even labeled apart now) and the animation\'s frames',
      RS9.twins(plan9b, cen9, 'a1') == ['a1', 'a2'] and plan9b.rep['a1'] != plan9b.rep['a2']
      and RS9.twins(plan9, cen9, '+0rn') == ['+0rn', '+1rn'] and RS9.twins(plan9, cen9, 'w1') == ['w1'])
o1, o2 = pix9['w1'], pix9['w2']
n1 = np.zeros((64, 64, 3), np.uint8)
n1[...] = (128, 128, 255)
n2 = np.zeros((64, 64, 3), np.uint8)
n2[...] = (204, 128, 229)                   # (0.6, 0, 0.8)
u9, _ = G.unify(['w1', 'w2'], {'w1': o1, 'w2': o2}, {'w1': [np.full((64, 64, 3), 50, np.uint8), n1], 'w2': [np.full((64, 64, 3), 200, np.uint8), n2]},
             4, True, normal_at=1)
inner9 = G.shared_interior(o2, o1, 4)
far9 = G.feather((o1 == o2).all(-1), 4, True) == 0
vec9 = u9['w2'][1].astype(float) / 127.5 - 1
check('unify: the shared texels take the earlier member\'s, the others stay, blended normals unit length',
      inner9.any() and (u9['w2'][0][inner9] == 50).all() and far9.any() and (u9['w2'][0][far9] == 200).all()
      and (u9['w2'][1][inner9] == n1[inner9]).all() and abs(np.linalg.norm(vec9, axis=2) - 1).max() < 0.02 and (u9['w1'][0] == 50).all())
# the margin (9.5): q differs from p in columns 7-8 and 0 (the edge, wrapped onto column 15)
p9 = rnd(40)
q9 = p9.copy()
q9[:, 7:9] = rnd(41)[:, 7:9]
q9[:, 0] = rnd(42)[:, 0]
eqw, eqn = G.equal(q9, p9), G.equal(q9, p9, wrap=False)
check('equal (9.5): a texel beside a difference is not shared, around the wrap too (not without it)',
      not eqw[:, [0, 1, 6, 7, 8, 9, 15]].any() and eqw[:, [2, 5, 10, 14]].all() and eqn[:, 15].all() and not eqn[:, 1].any()
      and G.equal(q9, p9, margin=0)[:, 6].all())
um, _ = G.unify(['p', 'q'], {'p': p9, 'q': q9}, {'p': [np.full((64, 64, 3), 50, np.uint8)], 'q': [np.full((64, 64, 3), 200, np.uint8)]}, 4)
inner_m = G.shared_interior(q9, p9, 4)
check('unify (9.5): beside a difference the later member keeps its own drawing; shared_interior agrees',
      (um['q'][0][:, 26:38] == 200).all() and (um['q'][0][:, 12:18] == 50).all() and not inner_m[:, 22:42].any()
      and inner_m[:, 12:18].all() and (um['q'][0][inner_m] == 50).all())
# joins (9.5, M85): a texture takes its neighbour's finished drawing where it shares its texels
res9j = dict(res9, m2=types.SimpleNamespace(**dict(vars(res9['m2']), params={'maps': True, 'join': ('h2', ())})))
plan9j = G.Plan(sorted(pix9), res9j, pix9, cen9, {'f': 'h', 'g': 'h2'})
dj, _ = plan9j.closure(['m2'])
bad9 = []
for t, why in (('m2', 'itself'), ('w2', 'size'), ('nope', 'not drawn')):
    pj = dict(pix9, w2=rnd(60, 12)) if why == 'size' else pix9     # 12 doesn't tile into 16 (8 would: M88)
    try:
        G.Plan(sorted(pj), dict(res9, m2=types.SimpleNamespace(**dict(vars(res9['m2']), params={'maps': True, 'join': (t, ())}))),
               pj, cen9, {'f': 'h', 'g': 'h2'})
    except ValueError:
        bad9.append(why)
check('join (9.5): the plan names the joined texture and the closure draws it; itself, a size that doesn\'t tile or no texture refused',
      plan9j.join == {'m2': ('h2', ())} and 'h2' in dj and plan9j.group['m2'] == ['m2'] and bad9 == ['itself', 'size', 'not drawn'], f'{plan9j.join} {dj} {bad9}')
# a smaller neighbour joins tiled (M88): the member holds it 2 by 2 in its left half
small9 = rnd(61, 8)
big9 = rnd(62)
big9[1:15, :8] = np.vstack([small9, small9])[1:15]      # no whole cell of it: that would make it a crop
pix9s = dict(pix9, m2=big9, h2=small9)
plan9s = G.Plan(sorted(pix9s), dict(res9, m2=types.SimpleNamespace(**dict(vars(res9['m2']), params={'maps': True, 'join': ('h2', ())}))),
                pix9s, cen9, {'f': 'h', 'g': 'h2'})
src9s = G.sources(['m2'], pix9s, 0, True, {'m2': ('h2', None)})
dr_small = np.random.default_rng(63).integers(0, 255, (32, 32, 3), dtype=np.uint8)
us, _ = G.unify(['m2'], pix9s, {'m2': [np.full((64, 64, 3), 7, np.uint8)], 'h2': [dr_small]}, 4, joins={'m2': ('h2', None)})
check('join (9.5): a neighbour whose size divides the member\'s joins tiled; its drawing taken tiled',
      plan9s.join == {'m2': ('h2', ())} and (src9s[2:14, 1:7] == 1).all() and (src9s[:, 7:] == 0).all() and (src9s[[0, 15]] == 0).all()
      and (us['m2'][0][12:52, 8:24] == np.vstack([dr_small, dr_small])[12:52, 8:24]).all() and (us['m2'][0][:, 40:] == 7).all()
      and (G.fit(small9, (16, 16)) == np.tile(small9, (2, 2, 1))).all() and G.fit(small9, (12, 16)) is small9)
# a set's later member takes the earlier one's join where it takes its texels (M88)
ja, jt = rnd(70), rnd(72)
jb = ja.copy()
jb[:, 8:] = rnd(71)[:, 8:]                  # b shares a's left half
jt[:, :8] = ja[:, :8]                       # the joined texture holds it too
pj9 = {'a': ja, 'b': jb, 't': jt}
sb = G.sources(['a', 'b'], pj9, 1, True, {'a': ('t', None)})
sa = G.sources(['a', 'b'], pj9, 0, True, {'a': ('t', None)})
check('join (9.5): a set\'s later member takes the joined drawing where it takes the joining member\'s texels',
      (sa[:, 1:7] == 2).all() and (sb[:, 1:7] == 2).all() and (sb[:, 9:15] == 1).all())
# the code review's: a copy of a crop doesn't repeat either; a set with a tile=y strip wraps top to bottom only
bigc = rnd(80, 32)
pixc = {'bigc': bigc, 'qc': bigc[16:, 16:].copy()}       # a cell of bigc's grid: a crop
pixc['qc2'] = pixc['qc'].copy()
resc = {s: fr('faithful') for s in pixc}
planc = G.Plan(sorted(pixc), resc, pixc, {}, {})
ty_res = {'s1': types.SimpleNamespace(params={'tile': 'y'}), 's2': types.SimpleNamespace(params={}),
          's0': types.SimpleNamespace(params={'tile': 0})}
ea = rnd(81)
eb = ea.copy()
eb[5, 15] = (eb[5, 15] + 1) % 255          # a difference in the last column only
check('repeats and tiles (9.5): a crop\'s copy doesn\'t repeat; tile=y sets wrap top to bottom only, its margin not across',
      planc.rep['qc2'] == 'qc' and 'qc' in planc.crop and 'qc2' not in planc.crop
      and not planc.repeats('qc') and not planc.repeats('qc2') and planc.repeats('bigc')
      and G.tiles(ty_res, ['s2', 's1']) == 'y' and G.tiles(ty_res, ['s1', 's0']) is False and G.tiles(ty_res, ['s2']) is True
      and G.equal(ea, eb, True)[5, 0] == False and G.equal(ea, eb, 'y')[5, 0] == True and G.equal(ea, eb, 'y')[5, 14] == False  # noqa: E712
      and (G.feather(np.eye(8, dtype=bool)[::-1], 2, 'y') >= 0).all())
mj = np.zeros((16, 16), bool)
mj[:, 10:] = True
mj[5, 12] = False                           # a gap among the blades
mj[2, 2] = True                             # a stray texel
jm = G.join_mask(mj, 2, 0.02)
res9t = dict(res9, w1=types.SimpleNamespace(**dict(vars(res9['w1']), params={'maps': True, 'tile': 0})))
check('groups (9.5): a member that doesn\'t repeat comes after those that do (it has the most area)',
      plan9.group['w1'] == ['w1', 'w2'] and G.Plan(sorted(pix9), res9t, pix9, cen9, {'f': 'h', 'g': 'h2'}).group['w1'] == ['w2', 'w1'])
check('join_mask (9.5): the gaps closed, the specks dropped', jm[5, 12] and not jm[2, 2] and jm[:, 10:].all() and not jm[:, :8].any(), str(jm.astype(int)))
L9 = rnd(50)
pj9 = L9.copy()
pj9[:, :8] = rnd(51)[:, :8]                 # its left half its own, its right half Raven's paste of L
own9, lawn9 = [np.full((64, 64, 3), 50, np.uint8)], [np.full((64, 64, 3), 200, np.uint8)]
uj, _ = G.unify(['p'], {'p': pj9, 'L': L9}, {'p': own9, 'L': lawn9}, 4, False, joins={'p': ('L', None)})
inner_j = G.shared_interior(pj9, L9, 4, False)
mr = np.zeros((16, 16), bool)
mr[:, 12:] = True
ur, fr9 = G.unify(['p'], {'p': pj9, 'L': L9}, {'p': list(own9), 'L': lawn9}, 4, False, scale=lambda j, own, comp: 2.0, joins={'p': ('L', mr)})
# a set (a, b share their left halves) whose b joins L everywhere: the shared texels stay a's
a9 = rnd(52)
b9 = rnd(53)
b9[:, :8] = a9[:, :8]
us, _ = G.unify(['a', 'b'], {'a': a9, 'b': b9, 'L': L9}, {'a': [np.full((64, 64, 3), 10, np.uint8)], 'b': [np.full((64, 64, 3), 90, np.uint8)], 'L': lawn9},
                4, False, joins={'b': ('L', np.ones((16, 16), bool))})
inner_ab = G.shared_interior(b9, a9, 4, False)
check('unify (9.5): a join takes the joined drawing where the originals are equal, or on its texels; a set\'s shared texels stay the set\'s; '
      'a first member that joins is rebalanced',
      inner_j.any() and (uj['p'][0][inner_j] == 200).all() and (uj['p'][0][:, :24] == 50).all()
      and (ur['p'][0][:, 52:] == 200).all() and fr9.get('p') == 2.0 and (ur['p'][0][:, :40] > 50).all()
      and inner_ab.any() and (us['b'][0][inner_ab] == 10).all() and (us['b'][0][:, 44:] == 200).all() and (us['a'][0] == 10).all(),
      f"{fr9} {ur['p'][0][0, ::8, 0]}")


class FakeComfy9:
    """The edit hands back its image scaled to the workflow's size; it notes the seed and a reference."""
    home = ''

    def __init__(self):
        self.ims, self.calls, self.prompts = {}, [], []

    def start(self, log=None):
        pass

    def stop(self):
        pass

    def upload(self, name, data):
        self.ims[name] = data
        return name

    def run(self, wf):
        import io
        im = Image.open(io.BytesIO(self.ims[wf['4']['inputs']['image']])).convert('RGB')
        seed = wf['19']['inputs']['noise_seed']
        self.calls.append((seed, '40' in wf))
        self.prompts.append(wf['11']['inputs']['text'])
        self.inputs = getattr(self, 'inputs', []) + [self.ims[wf['4']['inputs']['image']]]
        b = io.BytesIO()
        im = np.asarray(im.resize((wf['9']['inputs']['width'], wf['9']['inputs']['height']), Image.BICUBIC)).astype(int)
        Image.fromarray(np.clip(im + seed % 5, 0, 255).astype(np.uint8)).save(b, 'PNG')     # another seed, another image
        return b.getvalue()


class FakeMaps9:
    def run(self, which, rgb, wrap):
        g = np.random.default_rng(rgb.shape[0])
        if which == 'normal':       # PBRify's DirectX green: finish_normal flips it
            n = np.dstack([R._smooth(g.random(rgb.shape[:2]), 3) * 0.3 + 0.35, R._smooth(g.random(rgb.shape[:2]), 3) * 0.3 + 0.35,
                           np.full(rgb.shape[:2], 0.95)])
            return n
        return g.random(rgb.shape)


import redraw as RD  # noqa: E402
import verify as V  # noqa: E402
with tempfile.TemporaryDirectory() as tmp:
    ex, out = os.path.join(tmp, 'ex'), os.path.join(tmp, 'pack')
    os.makedirs(os.path.join(ex, 'textures'))
    smooth = lambda seed: pal9[np.clip(np.round(R._smooth(np.random.default_rng(seed).random((32, 32)), 4) * 60 - 14), 4, 31).astype(int)]  # noqa: E731
    t9 = {'h': smooth(11), 'm1': smooth(12), 'w1': smooth(13), 'c1': smooth(14)}
    t9['w2'] = t9['w1'].copy()
    t9['w2'][:12] = smooth(15)[:12]           # a variant: two thirds of w1's texels
    t9['c2'] = t9['c1'].copy()
    t9['h'][10:13, :] = pal9[0]                 # a black joint
    t9['m1'][8:16, 8:16] = pal9[166]            # gold
    lines = ['file,name,crc,width,height,kind,alpha,variants,used in,from']
    for s, im in t9.items():
        Image.fromarray(im).save(os.path.join(ex, 'textures', s + '.png'))
        lines.append(f'textures/{s}.png,{s},0000,32,32,world,none,1,m1,data1/pak0.pak')
    with open(os.path.join(ex, 'textures.csv'), 'w', encoding='utf-8') as f:
        f.write('\n'.join(lines) + '\n')
    lab9 = lambda s, tier, fam='', reg='*=stone', o='': dict(lrow(s, tier, 'stone', 'claude', 'a wall', o), family=fam, regions=reg)  # noqa: E731
    rows9 = [lab9('h', 'layout', 'f'), lab9('m1', 'layout', 'f', '*=stone;yellow=gold'), lab9('w1', 'reimagine'), lab9('w2', 'reimagine'),
             lab9('c1', 'faithful'), lab9('c2', 'faithful')]
    cen9 = {'h': {'area': '900'}, 'w1': {'area': '50'}, 'c1': {'area': '5'}, 'c2': {'area': '7'}}
    ents9 = P.load_export(ex)

    def run9(rows, stems):
        fc = FakeComfy9()
        rd = RD.Redraw(ex, out, classes, fc, ents9, rows, cen9, {'f': 'h'}, pal9, restart_every=0, log=lambda m: None)
        rd.maps = FakeMaps9()
        return rd, fc, rd.run(stems)
    rd9, fc9, (made9, same9, wrote9) = run9(rows9, ['m1', 'w2', 'c1'])
    tx = lambda s, suf='.png': os.path.join(out, 'textures', s + suf)  # noqa: E731
    check('redraw run: the hero, the set and a copy\'s source drawn, every file written',
          made9 == 5 and wrote9 == 6 and all(os.path.exists(tx(s, f)) for s in t9 for f in ('.png', '_n.png', '_orm.png', '.mat')), f'{made9} {wrote9}')
    seeds = dict(zip([s for s in rd9.plan.closure(['m1', 'w2', 'c1'])[0]], fc9.calls))
    check('redraw run: the member gets the hero as a reference, the hero none; a set one seed',
          seeds['m1'][1] and not seeds['h'][1] and seeds['w1'][0] == seeds['w2'][0] != seeds['h'][0], str(seeds))
    check('redraw run: a copy\'s files are its source\'s', all(open(tx('c1', f), 'rb').read() == open(tx('c2', f), 'rb').read() for f in ('.png', '_n.png', '_orm.png')))
    ia, ib = (np.asarray(Image.open(tx(s))) for s in ('w1', 'w2'))
    inner = G.shared_interior(t9['w1'], t9['w2'], 4)
    check('redraw run: a set\'s shared texels identical', inner.any() and (ia[inner] == ib[inner]).all() and not (ia[~inner] == ib[~inner]).all())
    orm_m1 = np.asarray(Image.open(tx('m1', '_orm.png')))
    check('redraw run: metallic per material (m1\'s gold 1, its stone 0)', orm_m1[48, 48, 2] == 255 and orm_m1[4, 4, 2] == 0, str(orm_m1[48, 48]))
    orm_h = np.asarray(Image.open(tx('h', '_orm.png')))
    check('redraw run: the black joint at roughness 1 and occluded', orm_h[46, 60, 1] >= 250 and orm_h[46, 60, 0] <= 10, str(orm_h[46, 60]))
    _, _, (made_b, same_b, wrote_b) = run9(rows9, ['m1', 'w2', 'c1'])
    check('redraw run: a second run makes nothing', made_b == 0 and same_b == 5 and wrote_b == 0, f'{made_b} {same_b} {wrote_b}')
    rows9b = [dict(r, overrides='seed=77') if r['pattern'] == 'h' else r for r in rows9]
    rd_c, fc_c, (made_c, _, wrote_c) = run9(rows9b, ['m1'])
    check('redraw run: a new seed for the hero redraws it and its family\'s member, nothing else', made_c == 2 and fc_c.calls[0][0] == 77, f'{made_c} {fc_c.calls}')
    open(tx('m1', '_r.png'), 'wb').write(b'stale')
    os.remove(tx('m1', '.mat'))
    _, _, (made_d, _, wrote_d) = run9(rows9b, ['m1'])
    check('redraw run: a missing file is written again, a stale map removed', made_d == 0 and wrote_d == 1 and os.path.exists(tx('m1', '.mat'))
          and not os.path.exists(tx('m1', '_r.png')), f'{made_d} {wrote_d}')
    rows9e = [dict(r, regions='*=stone;orange=gold') if r['pattern'] == 'm1' else r for r in rows9b]
    _, fc_e, (made_e, _, _) = run9(rows9e, ['m1'])
    check('redraw run: the gold named on another ramp redoes the stages, not the model\'s image', made_e == 1 and not fc_e.calls, f'{made_e} {fc_e.calls}')
    a_h = os.path.join(out, 'work', 'h_A.png')
    os.remove(a_h)
    _, fc_f, (made_f, _, _) = run9([dict(r, overrides='seed=0') if r['pattern'] == 'h' else r for r in rows9e], ['m1'])
    check('redraw run: a hero without its model image is drawn again; seed=0 is a seed', os.path.exists(a_h) and fc_f.calls and fc_f.calls[0][0] == 0,
          f'{made_f} {fc_f.calls}')
    # painting (9.5): faithful but for the prompt; a set member, never a hero's member, its shapes checked
    fc_p = FakeComfy9()
    rows9p = [dict(r, tier='painting') if r['pattern'] in ('c1', 'c2', 'm1') else r for r in rows9]
    rd_p = RD.Redraw(ex, os.path.join(tmp, 'paint'), classes, fc_p, ents9, rows9p, cen9, {'f': 'h'}, pal9, restart_every=0, log=lambda m: None)
    rd_p.maps = FakeMaps9()
    made_p, _, _ = rd_p.run(['c1', 'm1'])
    check('painting (9.5): its own prompt, no hero reference, a set member, shapes checked as faithful',
          made_p == 2 and len(fc_p.prompts) == 2 and all('not modelled in 3D' in p for p in fc_p.prompts) and not any(c[1] for c in fc_p.calls)
          and 'm1' not in rd_p.plan.hero and 'painting' in G.SET_TIERS and V.LAYOUT['painting'] == V.LAYOUT['faithful']
          and 'painting' in V.SHAPE_TIERS and 'painting' in classes['labels']['tiers'], f'{made_p} {fc_p.calls}')
    # tile=0 (9.5): a picture that doesn't repeat is padded mirrored, not wrapped round
    fc_t0 = FakeComfy9()
    rows9t = [dict(r, overrides='tile=0') if r['pattern'] == 'm1' else r for r in rows9]
    rd_t0 = RD.Redraw(ex, os.path.join(tmp, 'tile0'), classes, fc_t0, ents9, rows9t, cen9, {'f': 'h'}, pal9, restart_every=0, log=lambda m: None)
    rd_t0.maps = FakeMaps9()
    made_t0, _, _ = rd_t0.run(['m1'])
    import io as _io
    pin = np.asarray(Image.open(_io.BytesIO(fc_t0.inputs[-1])).convert('RGB'))
    m9 = P.margin_for(32, 32)
    check('tile=0 (9.5): padded mirrored, drawn and written; a set of only tiling members wraps',
          M.resolve('x', 'world', [row('x', 'stone', '', 'tile=0')], classes).params['tile'] == 0 and made_t0 == 2
          and (pin[m9:m9 + 32, m9 - 1] == t9['m1'][:, 0]).all() and not (pin[m9:m9 + 32, m9 - 1] == t9['m1'][:, -1]).all()
          and os.path.exists(os.path.join(tmp, 'tile0', 'textures', 'm1.png'))
          and G.tiles(rd_p.res, ['c1', 'm1']) and not G.tiles(rd_t0.res, ['h', 'm1']), f'{made_t0}')
    # join (9.5): c1 takes w2's finished drawing on its grey texels (all of them), its copy c2 too;
    # a new drawing of w2 rewrites them
    rows9j = [dict(r, overrides='join=w2@grey') if r['pattern'] in ('c1', 'c2') else r for r in rows9]

    def run9j(rows, stems):
        rd = RD.Redraw(ex, os.path.join(tmp, 'join'), classes, FakeComfy9(), ents9, rows, cen9, {'f': 'h'}, pal9, restart_every=0, log=lambda m: None)
        rd.maps = FakeMaps9()
        return rd.run(stems)
    jx = lambda s: np.asarray(Image.open(os.path.join(tmp, 'join', 'textures', s + '.png')))  # noqa: E731
    made_j, _, wrote_j = run9j(rows9j, ['c1'])
    same_j = (jx('c1') == jx('w2')).all() and (jx('c2') == jx('w2')).all()
    rd_jv = RD.Redraw(ex, os.path.join(tmp, 'join'), classes, None, ents9, rows9j, cen9, {'f': 'h'}, pal9, restart_every=0, log=lambda m: None)
    gc_ok = V.groups_check(os.path.join(tmp, 'join'), rd_jv.plan, ['c1', 'c2'], rd_jv.orig, pal9)
    bad_c1 = jx('c1').copy()
    bad_c1[40:44, 40:44] = 0
    Image.fromarray(bad_c1).save(os.path.join(tmp, 'join', 'textures', 'c1.png'))
    gc_bad = V.groups_check(os.path.join(tmp, 'join'), rd_jv.plan, ['c1'], rd_jv.orig, pal9)
    check('join (9.5): verify holds a join to its neighbour\'s texels', gc_ok == {} and any('a join' in x for x in gc_bad.get('c1', [])),
          f'{gc_ok} {gc_bad}')
    os.remove(os.path.join(tmp, 'join', 'textures', 'c1.png'))      # the run below writes it again
    w2_old = jx('w2')
    rows9k = [dict(r, overrides='seed=78') if r['pattern'] == 'w1' else r for r in rows9j]
    made_k, _, wrote_k = run9j(rows9k, ['c1'])
    check('join (9.5): drawn after the joined set, its finished drawing taken, copies too; a new drawing of the joined rewrites it',
          made_j == 3 and same_j and made_k == 2 and wrote_k == 4 and not (jx('w2') == w2_old).all() and (jx('c1') == jx('w2')).all(),
          f'{made_j} {wrote_j} {made_k} {wrote_k}')
    # crops and wholes (9.5, M81): q is big's right half; pa over pb make the picture `pic`
    ex2, out2 = os.path.join(tmp, 'ex2'), os.path.join(tmp, 'pack2')
    os.makedirs(os.path.join(ex2, 'textures'))
    big2 = np.hstack([smooth(21), smooth(22)])
    t2 = {'big': big2, 'q': big2[:, 32:].copy(), 'pa': smooth(23), 'pb': smooth(24)}
    t2['qa'] = t2['pa'][:16, 16:].copy()       # a crop of a piece
    lines2 = ['file,name,crc,width,height,kind,alpha,variants,used in,from']
    for s, im in t2.items():
        Image.fromarray(im).save(os.path.join(ex2, 'textures', s + '.png'))
        lines2.append(f'textures/{s}.png,{s},0000,{im.shape[1]},{im.shape[0]},world,none,1,m1,data1/pak0.pak')
    with open(os.path.join(ex2, 'textures.csv'), 'w', encoding='utf-8') as f:
        f.write('\n'.join(lines2) + '\n')
    rows2 = [lab9('big', 'faithful'), lab9('q', 'faithful'), lab9('pa', 'faithful', o='whole=pic@0,0'),
             lab9('pb', 'faithful', o='whole=pic@32,0'), lab9('pic', 'faithful', o='seed=7;tile=0'),
             lab9('qa', 'faithful')]
    ents2 = P.load_export(ex2)
    fc2 = FakeComfy9()
    rd2 = RD.Redraw(ex2, out2, classes, fc2, ents2, rows2, {}, {}, pal9, restart_every=0, log=lambda m: None)
    rd2.maps = FakeMaps9()
    d2, w2 = rd2.plan.closure(['q', 'qa'])
    made2, _, _ = rd2.run(['q', 'qa'])
    tx2 = lambda s, suf='.png': np.asarray(Image.open(os.path.join(out2, 'textures', s + suf)))  # noqa: E731
    wk2 = np.asarray(Image.open(os.path.join(out2, 'work', 'pic.png')))
    check('crops and wholes (9.5): found, drawn once each (the whole with its seed and twice the model pixels), cut into the textures',
          rd2.plan.crop == {'q': ('big', 0, 32), 'qa': ('pa', 0, 16)} and rd2.plan.whole == {'pa': ('pic', 0, 0), 'pb': ('pic', 32, 0)}
          and set(d2) == {'big', 'q', 'pa', 'pb', 'qa'} and made2 == 5 and (tx2('qa') == tx2('pa')[:64, 64:]).all() and len(fc2.calls) == 2 and (7, False) in fc2.calls
          and rd2._gen_scale('pic') == 2.0 and 'pic' not in ents2 and not os.path.exists(os.path.join(out2, 'textures', 'pic.png'))
          and (tx2('q') == tx2('big')[:, 128:]).all() and (tx2('q', '_n.png') == tx2('big', '_n.png')[:, 128:]).all()
          and (np.vstack([tx2('pa'), tx2('pb')]) == wk2).all() and not rd2.plan.repeats('q') and not rd2.plan.repeats('pa')
          and rd2.plan.repeats('big') and V.groups_check(out2, rd2.plan, ['q'], rd2.orig) == {}, f'{made2} {fc2.calls} {d2}')
    made2b, skipped2b, _ = rd2.run(['q', 'qa'])
    Image.fromarray(np.zeros_like(tx2('q'))).save(os.path.join(out2, 'textures', 'q.png'))
    check('crops and wholes (9.5): a second run draws nothing; verify fails a crop that isn\'t its region',
          made2b == 0 and skipped2b == 5 and bool(V.groups_check(out2, rd2.plan, ['q'], rd2.orig).get('q')))
    for what, rows_bad in (('a gap', [r if r['pattern'] != 'pb' else lab9('pb', 'faithful', o='whole=pic@40,0') for r in rows2]),
                           ('no row of its own', [r for r in rows2 if r['pattern'] != 'pic'])):
        try:
            RD.Redraw(ex2, out2, classes, FakeComfy9(), ents2, rows_bad, {}, {}, pal9, restart_every=0, log=lambda m: None)
            check(f'wholes (9.5): {what} is refused', False)
        except ValueError:
            check(f'wholes (9.5): {what} is refused', True)
    # pieces that overlap (M86): pb 8 rows into pa, the picture's there; verify measures pa against it
    rows3 = [r if r['pattern'] != 'pb' else lab9('pb', 'faithful', o='whole=pic@24,0') for r in rows2]
    out3 = os.path.join(tmp, 'pack3')
    rd3 = RD.Redraw(ex2, out3, classes, FakeComfy9(), ents2, rows3, {}, {}, pal9, restart_every=0, log=lambda m: None)
    rd3.maps = FakeMaps9()
    rd3.run(['pa'])
    tx3 = lambda s: np.asarray(Image.open(os.path.join(out3, 'textures', s + '.png')))  # noqa: E731
    res_pa = M.resolve('pa', 'world', rows3, classes)
    e_ok, _ = V.check_redraw(ex2, out3, ents2['pa'], res_pa, pal9, MT.table(classes), None, False, rd3.orig['pic'][:32])
    e_bad, _ = V.check_redraw(ex2, out3, ents2['pa'], res_pa, pal9, MT.table(classes), None, False, t2['big'][:, :32])
    check('wholes (9.5): pieces may overlap, the lower one is the picture there; the upper\'s rows there are cut from it; '
          'verify measures a piece against its region of the picture',
          rd3.orig['pic'].shape[:2] == (56, 32) and (rd3.orig['pic'][:24] == t2['pa'][:24]).all() and (rd3.orig['pic'][24:] == t2['pb']).all()
          and (tx3('pa')[96:] == tx3('pb')[:32]).all() and not any('shapes moved' in x for x in e_ok)
          and any('shapes moved' in x for x in e_bad), f'{e_ok} {e_bad}')
    # a quarter mirrored into its picture (M87): `mirror=xy` on the picture's row
    rows4 = [r for r in rows2 if r['pattern'] not in ('pa', 'pb')] + [
        lab9('pa', 'faithful', o='whole=hp@0,0'), lab9('pb', 'faithful'), lab9('hp', 'faithful', o='seed=5;tile=0;mirror=xy')]
    out4 = os.path.join(tmp, 'pack4')
    rd4 = RD.Redraw(ex2, out4, classes, FakeComfy9(), ents2, rows4, {}, {}, pal9, restart_every=0, log=lambda m: None)
    rd4.maps = FakeMaps9()
    rd4.run(['pa'])
    hp = rd4.orig['hp']
    wk4 = np.asarray(Image.open(os.path.join(out4, 'work', 'hp.png')))
    tx4 = np.asarray(Image.open(os.path.join(out4, 'textures', 'pa.png')))
    rd5 = RD.Redraw(ex2, os.path.join(tmp, 'pack5'), classes, FakeComfy9(), ents2,
                    [r if r['pattern'] != 'pb' else lab9('pb', 'faithful', o='mirror=xy') for r in rows4], {}, {}, pal9,
                    restart_every=0, log=lambda m: None)
    rd5.maps = FakeMaps9()
    try:
        rd5.draw('pb')
        refused = False
    except ValueError:
        refused = True
    check('wholes (9.5): mirror=xy draws a quarter as its picture mirrored right and down, at twice the pixels (the cap); '
          'refused on a row that is no picture\'s',
          hp.shape[:2] == (64, 64) and (hp[:32, :32] == t2['pa']).all() and (hp[:, 32:] == hp[:, 31::-1]).all()
          and (hp[32:] == hp[31::-1]).all() and rd4._gen_scale('hp') == RD.WHOLE_SCALE == 2.0 and tx4.shape[:2] == (128, 128)
          and (tx4[..., :3] == wk4[:128, :128, :3]).all() and refused)
    rows9b = [dict(r, overrides='seed=0') if r['pattern'] == 'h' else r for r in rows9e]
    res_v = {s: M.resolve(s, 'world', rows9b, classes) for s in t9}
    sel_v = [ents9[s] for s in sorted(t9)]
    import contextlib
    import io as _io
    with contextlib.redirect_stdout(_io.StringIO()) as buf:
        rc = V.run(ex, out, sel_v, res_v, pal9, classes, rd_c.plan, rd_c.orig)
    check('verify passes the redrawn pack', rc == 0, buf.getvalue()[-600:])
    Image.fromarray(np.where(inner[..., None], 0, ib).astype(np.uint8)).save(tx('w2'))
    errs = V.groups_check(out, rd_c.plan, ['w2'], rd_c.orig)
    check('verify: a set\'s differing shared texels fail', bool(errs.get('w2')), str(errs))

    # -- story 9.5: hero candidates, reseeds, the review, the liquids' checks --------------
    import heroes as HE
    import reseed as RS
    import review as RV
    quiet = lambda m: None  # noqa: E731
    cand = os.path.join(tmp, 'cand')

    fc_h = FakeComfy9()
    rd_h = RD.Redraw(ex, os.path.join(cand, 'c1'), classes, fc_h, ents9, rows9b, cen9, {'f': 'h'}, pal9, restart_every=0, log=quiet)
    rd_h.maps = FakeMaps9()
    items_h = HE.families(rd_h.plan, rd_h.res, {'f': {'family': 'f', 'hub': 'blackmarsh', 'lead': 'h'}}, cen9)
    check('heroes: a family\'s lead and its layout member that takes it as the reference', items_h == [('f', 'blackmarsh', 'h', 'm1')], str(items_h))
    info_h = HE.draw(rd_h, cand, items_h, {'f': [('rougher', 'flaking rust'), ('finer', 'an even patina')]}, log=quiet)
    cs = info_h['f']['cands']
    sd = [c['seed'] for c in cs]
    check('heroes: the label, the rougher and the finer finish with two seeds each, the first the seed a run uses (the row\'s seed=0)',
          [c['name'] for c in cs] == ['as labeled'] * 2 + ['rougher'] * 2 + ['finer'] * 2 and sd[0] == 0 and sd[0::2] == [0] * 3
          and sd[1::2] == [sd[1]] * 3 and sd[1] != 0, str(cs))
    pr_h = fc_h.prompts
    check('heroes: each candidate the lead with its seed and finish, then the member with a reference and the same finish',
          [c[0] for c in fc_h.calls[0::2]] == sd and all(c[1] for c in fc_h.calls[1::2]) and not any(c[1] for c in fc_h.calls[0::2])
          and not any('flaking' in p or 'patina' in p for p in pr_h[:4]) and all('flaking rust' in p for p in pr_h[4:8])
          and all('an even patina' in p for p in pr_h[8:12])
          and all(os.path.exists(os.path.join(cand, f'c{k}', 'textures', s + f)) for k in range(1, 7) for s in ('h', 'm1')
                  for f in ('.png', '_n.png', '_orm.png', '.mat')), str(fc_h.calls))
    check('heroes: the candidates leave the seeds, finishes and pack as they were', rd_h.seeds == {} and rd_h.finishes == {}
          and rd_h.seed_of('h') == 0 and rd_h.out == os.path.join(cand, 'c1'))
    fc_t = FakeComfy9()
    rd_t = RD.Redraw(ex, os.path.join(tmp, 'cand2', 'c1'), classes, fc_t, ents9, rows9b, cen9, {'f': 'h'}, pal9, restart_every=0, log=quiet)
    rd_t.maps = FakeMaps9()
    HE.draw(rd_t, os.path.join(tmp, 'cand2'), HE.textures(rd_t.plan, rd_t.res, cen9, ['m1']), None, log=quiet, pack=os.path.join(cand, 'c1'))
    check('heroes --stems: a layout member drawn with its hero\'s drawing from the pack as the reference, six seeds (the code review\'s)',
          len(fc_t.calls) == 6 and all(c[1] for c in fc_t.calls) and len({c[0] for c in fc_t.calls}) == 6
          and os.path.exists(os.path.join(tmp, 'cand2', 'c6', 'textures', 'm1.png')), str(fc_t.calls))
    pg = os.path.join(tmp, 'heroes.html')
    with contextlib.redirect_stdout(_io.StringIO()):
        HE.page(ex, ents9, {'h': rd_h.res['h']}, info_h, cand, pg)
    text = open(pg, encoding='utf-8').read()
    check('heroes page: a family\'s six candidates and none, the member\'s row, the finishes',
          text.count('type="radio" name="f"') == 7 and 'its layout member m1' in text and 'flaking rust' in text and '6. finer' in text)
    fam95 = os.path.join(tmp, 'families.csv')
    with open(fam95, 'w', encoding='utf-8', newline='') as f:
        f.write('# a comment\r\nfamily,hub,lead,what\r\nf,blackmarsh,h,"a wall, grey"\r\ng,keep,x,other\r\n')
    rows95 = [lrow('h', 'layout', 'stone', 'claude', 'a wall', 'bump=2'), lrow('x', 'layout', 'stone', 'claude', 'x')]
    pk = os.path.join(tmp, 'picks.csv')
    with open(pk, 'w', encoding='utf-8', newline='') as f:
        f.write('family,lead,finish,seed,note\nf,h,flaking rust,42,\ng,x,,,darker please\n')
    ch95 = HE.apply(pk, fam95, rows95, log=quiet)
    import labels as LB
    ff = LB.load_families(fam95)
    check('heroes apply: the family\'s finish into families.csv (comments and line ends kept), the lead\'s seed as the owner\'s, a none left',
          ff['f']['finish'] == 'flaking rust' and not ff['g'].get('finish') and ff['f']['what'] == 'a wall, grey'
          and open(fam95, encoding='utf-8', newline='').read().startswith('# a comment\r\nfamily,hub,lead,what,finish\r\n')
          and rows95[0]['overrides'] == 'bump=2;seed=42' and rows95[0]['source'] == 'human' and rows95[1]['source'] == 'claude'
          and len(ch95) == 1 and LB.finishes(ff) == {'f': 'flaking rust'}, str(ff))
    rd_f = RD.Redraw(ex, out, classes, None, ents9, rows9b, cen9, {'f': 'h'}, pal9, restart_every=0, log=quiet,
                     finishes={'f': 'flaking rust'})
    check('redraw: a family\'s finish in its layout and reimagine members\' descriptions, not elsewhere',
          rd_f.description('m1') == 'a wall; flaking rust' and rd_f.description('w1') == 'a wall' and rd_f.description('c1') == 'a wall')

    # reseed: m1 failed on its shapes (another seed is tried and written), h on a seam with the owner's seed (left alone)
    rows_r = [dict(r) for r in rows9b]
    rows_r = [dict(r, source='human') if r['pattern'] == 'h' else r for r in rows_r]
    os.makedirs(os.path.join(out, '.texpack'), exist_ok=True)

    def roll_half(stem):        # the pack's albedo shifted by half: its shapes moved, as verify sees it
        im = np.asarray(Image.open(tx(stem)))
        Image.fromarray(np.roll(im, (im.shape[0] // 2, im.shape[1] // 2), axis=(0, 1))).save(tx(stem))
    roll_half('m1')
    roll_half('h')
    # c1's row in verify.csv is older than the pack (it passes now): left as it is
    V._save_report(out, {'m1': {'stem': 'm1', 'tier': 'layout', 'result': 'FAIL', 'errors': 'the shapes moved: layout kept 0.10'},
                         'h': {'stem': 'h', 'tier': 'layout', 'result': 'FAIL', 'errors': 'the shapes moved: layout kept 0.05'},
                         'c1': {'stem': 'c1', 'tier': 'faithful', 'result': 'FAIL', 'errors': 'the shapes moved: layout kept 0.05'},
                         'w1': {'stem': 'w1', 'tier': 'reimagine', 'result': 'FAIL', 'errors': 'stone: roughness outside 0.6..0.9 in 9 % of its texels'}})
    man_r = os.path.join(tmp, 'mr.csv')
    M.write_manifest(man_r, rows_r)
    fc_r = FakeComfy9()
    rd_r = RD.Redraw(ex, out, classes, fc_r, ents9, rows_r, cen9, {'f': 'h'}, pal9, restart_every=0, log=quiet)
    rd_r.maps = FakeMaps9()
    recs = RS.run(rd_r, ex, out, rows_r, man_r, tries=2, log=quiet)
    want_seed = P.seed_for('m1', 9100 + RS.OFFSET + 1)
    got_r = {q['pattern']: q for q in M.load_manifest(man_r)}
    check('reseed: a failure another seed may fix gets the first seed that passes, in the manifest; the owner\'s seed, other failures '
          'and a stale verify.csv row left',
          [r['set'] for r in recs] == ['c2', 'h', 'm1'] and 'passes now' in recs[0]['result'] and 'owner' in recs[1]['result']
          and recs[2]['seed'] == want_seed and os.path.exists(os.path.join(out, '.texpack', 'reseed.csv'))
          and got_r['m1']['overrides'] == f'seed={want_seed}' and got_r['h']['overrides'] == 'seed=0' and fc_r.calls == [(want_seed, True)]
          and not os.path.exists(os.path.join(out, '.texpack', 'reseed', 'm1')) and rd_r.out == out, f'{recs} {fc_r.calls}')
    # again: m1 continues after reseed's seed it has now; w2's set is drawn whole and unified before its check
    # (w1 and w2 layout here: a reimagine texture has no shapes to keep, and their originals don't tile, so no seam check)
    rows_r2 = [dict(r, tier='layout') if r['pattern'] in ('w1', 'w2') else dict(r) for r in rows_r]
    roll_half('w2')
    V._save_report(out, {'m1': {'stem': 'm1', 'tier': 'layout', 'result': 'FAIL', 'errors': 'the shapes moved: layout kept 0.20'},
                         'w2': {'stem': 'w2', 'tier': 'layout', 'result': 'FAIL', 'errors': 'the shapes moved: layout kept 0.10'}})
    fc_r2 = FakeComfy9()
    rd_r2 = RD.Redraw(ex, out, classes, fc_r2, ents9, rows_r2, cen9, {'f': 'h'}, pal9, restart_every=0, log=quiet)
    rd_r2.maps = FakeMaps9()
    recs2 = RS.run(rd_r2, ex, out, rows_r2, man_r, tries=1, log=quiet)
    s_m1, s_w = P.seed_for('m1', 9100 + RS.OFFSET + 2), P.seed_for('w1', 9100 + RS.OFFSET + 1)
    check('reseed: the next seed after an earlier reseed\'s; a set drawn whole, its seed on its first member',
          fc_r2.calls == [(s_m1, True), (s_w, False), (s_w, False)] and [r['seed'] for r in recs2] == [s_m1, s_w]
          and next(r for r in rows_r2 if r['pattern'] == 'w1')['overrides'] == f'seed={s_w}', f'{recs2} {fc_r2.calls}')
    rows_p = [lrow('w1', 'reimagine', 'stone', 'claude', 'a wall', 'seed=5'), lrow('w2', 'reimagine', 'stone', 'claude', 'a wall')]
    pk2 = os.path.join(tmp, 'picks2.csv')
    with open(pk2, 'w', encoding='utf-8', newline='') as f:
        f.write('family,lead,finish,seed,note\nzz,w2,,9,\n')
    HE.apply(pk2, fam95, rows_p, rd_r.plan, log=quiet)
    check('heroes apply: another member\'s seed in the lead\'s set is taken off (it would hide the pick)',
          rows_p[0]['overrides'] == '' and rows_p[1]['overrides'] == 'seed=9' and rd_r.plan.group['w2'] == ['w1', 'w2'], str(rows_p))

    # the review
    cen_v = {'a': {'hubs': 'blackmarsh keep'}, 'b': {'hubs': 'deathmatch'}, 'c': {'hubs': '', 'anim': 'c d'}, 'd': {'hubs': 'tulku', 'anim': 'c d'}}
    check('review: a texture\'s page is its home hub, the deathmatch maps\' other, an unseen frame its animation\'s',
          [RV.page_hub(cen_v, s) for s in 'abce'] == ['blackmarsh', 'other', 'tulku', 'other'])
    cards9 = RV.cards(sorted(t9), rd_r.res, cen9, {'f': {'lead': 'h', 'what': ''}})
    check('review: a family together, its hero first, then the rest by area', cards9[:3] == [('f', 'h'), ('f', 'm1'), ('', 'w1')]
          and len(cards9) == len(t9), str(cards9))
    p1, p2 = RV.split(cards9, 1, 2), RV.split(cards9, 2, 2)
    check('review: a page in parts keeps a family whole', p1 + p2 == cards9 and ('f', 'm1') in p1 and len(p2) > 0, f'{p1} {p2}')
    s1 = RV.next_seed(rd_r, 'w1')
    rows_v = [dict(r) for r in rows_r]
    RS.set_seed(next(r for r in rows_v if r['pattern'] == 'w1'), s1)
    rd_v = RD.Redraw(ex, out, classes, None, ents9, rows_v, cen9, {'f': 'h'}, pal9, restart_every=0, log=quiet)
    s2 = RV.next_seed(rd_v, 'w2')
    check('review: "another seed" steps through the review\'s seeds of the set',
          s1 == P.seed_for('w1', 9100 + RV.REVIEW_SEED + 1) and s2 == P.seed_for('w1', 9100 + RV.REVIEW_SEED + 2), f'{s1} {s2}')
    vd = os.path.join(tmp, 'verdicts.csv')
    with open(vd, 'w', encoding='utf-8', newline='') as f:
        f.write('pattern,verdict,tier,regions,description,note\nm1,label,,,an oak door,the iron is bronze\nw2,seed,,,,\nc1,candidates,,,,\nh,ok,,,,\n'
                'c2,label,,,a copied panel,\n')
    rows_a = [dict(r) for r in rows_r]
    lab_a, ch_a = RV.apply(rd_r, rows_a, vd, out, log=quiet)
    check('review apply (9.5): a label edit goes onto the copies too', sorted(r['pattern'] for r in lab_a if r['description'] == 'a copied panel') == ['c1', 'c2'],
          str(lab_a))
    lab_a = [r for r in lab_a if r['pattern'] not in ('c1', 'c2')]
    check('review apply: a label edit for merge (every label column), another seed on the set\'s seed row, candidates listed',
          lab_a == [{'tier': 'layout', 'regions': rd_r.res['m1'].regions, 'description': 'an oak door', 'pattern': 'm1'}] and ch_a
          and next(r for r in rows_a if r['pattern'] == 'w1')['overrides'] == f'seed={s1}'
          and open(os.path.join(out, '.texpack', 'candidates.txt')).read() == 'c1\n', f'{lab_a}')
    man_l = os.path.join(tmp, 'ml.csv')
    M.write_manifest(man_l, [dict(lrow('m1', 'layout', 'stone', 'claude', 'a wall'), regions='*=stone;yellow=gold')])
    lab_csv2 = os.path.join(tmp, 'review_labels.csv')
    import csv
    with open(lab_csv2, 'w', encoding='utf-8', newline='') as f:
        w = csv.DictWriter(f, ['pattern', 'tier', 'regions', 'description', 'source'], lineterminator='\n')
        w.writeheader()
        w.writerows([dict(r, source='human') for r in lab_a])
    with contextlib.redirect_stdout(_io.StringIO()):
        TP.cmd_merge(types.SimpleNamespace(edits=lab_csv2, manifest=man_l))
    ml = M.load_manifest(man_l)[0]
    check('review apply + merge: the edit lands, the tier and regions stay (the code review\'s)',
          ml['description'] == 'an oak door' and ml['tier'] == 'layout' and ml['regions'] == rd_r.res['m1'].regions and ml['source'] == 'human', str(ml))

    # packviews: a run's shots, its hits and the brightness against the same run's originals
    run_d = os.path.join(tmp, 'run')
    os.makedirs(run_d)
    with open(os.path.join(run_d, 'views.csv'), 'w', encoding='utf-8', newline='') as f:
        f.write('name,stem,map,close,wide\nh,h,demo1,0 0 0 0 0,0 0 0 0 0\nm1,m1,demo1,0 0 0 0 0,0 0 0 0 0\n')
    with open(os.path.join(run_d, 'debug_h2.log'), 'w', encoding='latin-1') as f:
        f.write('\n'.join(['T92_VIEW h close', 'here: world opaque, 90 units away: h (material 1)',
                           '  files: textures/h~0000 (this texture\'s pixels only) or textures/h (every texture of the name)',
                           'T92_VIEW h wide', 'T92_VIEW m1 close', 'here: world opaque, 90 units away: zz (material 2)',
                           '  files: textures/zz~0000 (this texture\'s pixels only) or textures/zz (every texture of the name)', 'T92_END']) + '\n')
    for name, v in (('h_close', 80), ('h_close_orig', 40), ('h_wide', 60), ('h_wide_orig', 60), ('m1_close', 50)):
        Image.fromarray(np.full((8, 8, 3), v, np.uint8)).save(os.path.join(run_d, name + '.tga'))
    shots95 = os.path.join(tmp, 'shots')
    pv = RV.read_runs([run_d], shots95, {}, None, log=quiet)
    rh = (80 / 40) ** 2.2
    check('packviews: the PNGs, a hit, a miss named, the brightness ratios against the run\'s originals',
          pv['h']['hit'] == 'ok' and abs(float(pv['h']['close']) - rh) < 0.01 and abs(float(pv['h']['wide']) - 1) < 0.01
          and 'zz' in pv['m1']['hit'] and 'close' not in pv['m1'] and os.path.exists(os.path.join(shots95, 'h_close_orig.png'))
          and os.path.exists(os.path.join(shots95, 'packviews.csv')), str(pv))
    pr = os.path.join(tmp, 'review.html')
    with contextlib.redirect_stdout(_io.StringIO()):
        nr = RV.page(ex, ents9, rd_r.res, cen9, {'f': {'lead': 'h', 'what': 'a wall'}}, cards9, out, shots95, None,
                     pr, 'test', {'m1': {'result': 'FAIL', 'errors': 'the shapes moved'}}, {'w1': 'a crack drawn across'})
    text = open(pr, encoding='utf-8').read()
    check('review page: every card, a verdict each, verify\'s failure and Claude\'s flag shown',
          nr == len(cards9) and text.count('value="candidates"') == nr and 'the shapes moved' in text and 'a crack drawn across' in text
          and 'brightness: close x' in text)

# the liquids: their caustic against the original's (6.16, X29)
dark = np.uint8(np.clip(field(64, 64, 3, 31) * 80 + 10, 0, 255))
flatl = np.uint8(np.clip((dark.astype(float) - dark.mean()) * 0.3 + dark.mean(), 0, 255))
glint = dark.copy()
glint.reshape(-1, 3)[::40] = 255            # 2.5 % of the texels white
check('caustic: none for the original itself, less of it and glints at the cap noted',
      V.caustic_notes(dark, dark) == [] and any('spread' in n for n in V.caustic_notes(dark, flatl))
      and any('cap' in n for n in V.caustic_notes(dark, glint)), f'{V.caustic(dark)} {V.caustic(flatl)} {V.caustic(glint)}')
check('verify: liquids and lava fail where their shapes moved', {'liquid', 'lava'} <= set(V.SHAPE_TIERS))
sys.exit(1 if fails else 0)
