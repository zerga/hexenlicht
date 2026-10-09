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
sys.exit(1 if fails else 0)
