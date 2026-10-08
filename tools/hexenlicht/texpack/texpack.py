"""texpack: AI-upscaled texture packs from a manifest of material descriptions
(story 5.8; README.md). Run with ComfyUI's bundled Python:

  %TEXPACK_HOME%\\ComfyUI_windows_portable\\python_embeded\\python.exe texpack.py <command> ...

Commands: check-env, draft, montage, sheet, merge, run, verify, calibrate.
"""
import argparse
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import manifest  # noqa: E402
import pipeline  # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))


def texpack_home():
    return os.environ.get('TEXPACK_HOME') or os.path.join(os.path.dirname(REPO), 'hexenlicht-tools', 'texpack')


def default_export():
    data = os.environ.get('HEXENLICHT_DATA') or os.path.join(os.path.dirname(REPO), 'Hexenlicht-data')
    for sub in ('portals', 'data1'):
        p = os.path.join(data, sub, 'export')
        if os.path.exists(os.path.join(p, 'textures.csv')):
            return p
    return os.path.join(data, 'portals', 'export')


def add_select(p):
    p.add_argument('--export', default=None, help='an r_exporttextures folder (default: the game data folder\'s)')
    p.add_argument('--select', help='a file of texture stems, one per line (# comments)')
    p.add_argument('--stems', help='comma-separated stems')
    p.add_argument('--kind', help='world, liquid, skin, sprite, sky, picture (comma-separated)')
    p.add_argument('--map', help='textures used in this map')
    p.add_argument('--glob', help='fnmatch pattern on the stem')
    p.add_argument('--limit', type=int)


def selection(a):
    ex = a.export or default_export()
    entries = pipeline.load_export(ex)
    return ex, entries, pipeline.select(entries, a.stems, a.kind, a.map, a.glob, a.select, a.limit)


def resolve_all(sel, rows, classes):
    return {e.stem: manifest.resolve(e.stem, e.kind, rows, classes) for e in sel}


def cmd_check_env(a):
    home = texpack_home()
    classes = manifest.load_classes(os.path.join(HERE, 'classes.toml'))
    ok = True
    comfy = os.path.join(home, 'ComfyUI_windows_portable', 'ComfyUI', 'models')
    need = [(os.path.join(comfy, 'upscale_models', classes['models']['upscaler']), 'upscaler'),
            (os.path.join(comfy, 'checkpoints', classes['models']['checkpoint']), 'checkpoint'),
            (os.path.join(comfy, 'controlnet', classes['models']['controlnet']), 'controlnet')]
    need += [(os.path.join(home, 'models', 'pbrify', classes['models'][k]), k) for k in ('normal', 'roughness', 'height')]
    vlm = os.path.join(home, 'models', classes['models']['vlm'])
    for path, what in need:
        print(f"{'ok     ' if os.path.exists(path) else 'MISSING'} {what}: {path}")
        ok = ok and os.path.exists(path)
    print(f"{'ok     ' if os.path.exists(vlm) else 'missing'} vlm (draft only): {vlm}")
    import torch
    print(f"torch {torch.__version__}, cuda {torch.cuda.is_available()}" + (f", {torch.cuda.get_device_name(0)}" if torch.cuda.is_available() else ''))
    ok = ok and torch.cuda.is_available()
    ex = a.export or default_export()
    print(f"{'ok     ' if os.path.exists(os.path.join(ex, 'textures.csv')) else 'MISSING'} export: {ex}")
    return 0 if ok else 1


def cmd_run(a):
    from comfy import Comfy
    ex, entries, sel = selection(a)
    classes = manifest.load_classes(os.path.join(HERE, 'classes.toml'))
    rows = manifest.load_manifest(a.manifest)
    res = resolve_all(sel, rows, classes)
    out = os.path.abspath(a.out)
    print(f"texpack run: {len(sel)} textures from {ex} into {out}")
    done = skipped = since = 0
    t0 = time.time()
    with Comfy(texpack_home(), attach=a.attach, deterministic=not a.nondeterministic) as c:
        c.start()
        r = pipeline.Runner(ex, out, classes, c, seed=a.seed, flip_y=FLIP_Y)
        for i, e in enumerate(sel, 1):
            info = r.process(e, res[e.stem], force=a.force)
            if info is None:
                print(f"[{i}/{len(sel)}] {e.stem}: skipped (class {res[e.stem].cls})")
            elif info.get('skipped'):
                skipped += 1
                print(f"[{i}/{len(sel)}] {e.stem}: unchanged")
            else:
                done += 1
                since += 1
                if a.restart_every and since >= a.restart_every:
                    # ComfyUI's caches grow over a long run until the VRAM is full and a texture takes
                    # four times as long (the first full run: 4 s, then 16 s after about 250)
                    c.stop()
                    c.start(log=lambda m: None)
                    since = 0
                print(f"[{i}/{len(sel)}] {e.stem}: {res[e.stem].cls}, {info['size'][0]}x{info['size'][1]}, "
                      f"luminance x{info['gain']} to {info['lum_out']:.4f} (was {info['lum_in']:.4f}), {info['seconds']} s")
    print(f"{done} made, {skipped} unchanged, {time.time() - t0:.0f} s")
    return 0


# PBRify's NormalV3 against its Height model: `calibrate` measures the sign.
FLIP_Y = True


def cmd_calibrate(a):
    import numpy as np
    ex, entries, sel = selection(a)
    classes = manifest.load_classes(os.path.join(HERE, 'classes.toml'))
    m = pipeline.Maps(os.path.join(texpack_home(), 'models', 'pbrify'), classes['models'])
    from PIL import Image
    tot = []
    for e in sel:
        rgb, _ = pipeline.read_png(os.path.join(ex, e.file))
        big = np.asarray(Image.fromarray(rgb).resize((rgb.shape[1] * 4, rgb.shape[0] * 4), Image.LANCZOS))
        c = pipeline.height_normal_correlation(m.run('normal', big, True), m.run('height', big, True))
        tot.append(c)
        print(f"{e.stem}: correlation {c:+.3f}")
    mean = sum(tot) / len(tot)
    print(f"mean {mean:+.3f}: " + ("OpenGL convention (green up): FLIP_Y = False" if mean > 0 else "DirectX convention (green down): FLIP_Y = True"))
    return 0


def cmd_verify(a):
    import verify
    ex, entries, sel = selection(a)
    classes = manifest.load_classes(os.path.join(HERE, 'classes.toml'))
    rows = manifest.load_manifest(a.manifest)
    return verify.run(ex, os.path.abspath(a.pack), sel, resolve_all(sel, rows, classes))


def cmd_draft(a):
    import draft
    ex, entries, sel = selection(a)
    classes = manifest.load_classes(os.path.join(HERE, 'classes.toml'))
    return draft.run(texpack_home(), ex, sel, classes, a.manifest, redo=a.redo)


def cmd_merge(a):
    rows = manifest.load_manifest(a.manifest)
    by = {r['pattern']: r for r in rows}
    n = 0
    for r in manifest.load_manifest(a.edits):
        old = by.get(r['pattern'])
        if old:
            old.update({'class': r['class'], 'description': r['description'], 'source': 'human'})
        else:
            by[r['pattern']] = dict(r, source='human')
        n += 1
    manifest.write_manifest(a.manifest, list(by.values()))
    print(f"texpack merge: {n} rows from {a.edits} into {a.manifest}")
    return 0


def cmd_montage(a):
    import sheet
    ex, entries, sel = selection(a)
    classes = manifest.load_classes(os.path.join(HERE, 'classes.toml'))
    rows = manifest.load_manifest(a.manifest)
    res = resolve_all(sel, rows, classes)
    if a.cls:
        sel = [e for e in sel if res[e.stem].cls in a.cls.split(',')]
    return sheet.montage(ex, sel, res, a.out)


def cmd_sheet(a):
    import sheet
    ex, entries, sel = selection(a)
    classes = manifest.load_classes(os.path.join(HERE, 'classes.toml'))
    rows = manifest.load_manifest(a.manifest)
    return sheet.run(ex, sel, resolve_all(sel, rows, classes), classes, a.out, pack=a.pack, group=a.group)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('check-env', help='the models, torch and the export are in place')
    p.add_argument('--export')
    p.set_defaults(f=cmd_check_env)
    p = sub.add_parser('run', help='upscale the selected textures into a pack folder')
    add_select(p)
    p.add_argument('--manifest', default=os.path.join(HERE, 'materials.csv'))
    p.add_argument('--out', required=True, help='the pack folder (its textures\\ is what goes into data1)')
    p.add_argument('--seed', type=int, default=5800)
    p.add_argument('--force', action='store_true', help='redo textures that are unchanged')
    p.add_argument('--restart-every', type=int, default=80, help='restart ComfyUI after this many textures made (0: never)')
    p.add_argument('--attach', action='store_true', help='use a ComfyUI already running on port 8199')
    p.add_argument('--nondeterministic', action='store_true', help='without ComfyUI\'s --deterministic')
    p.set_defaults(f=cmd_run)
    p = sub.add_parser('verify', help='check a pack against the spec and the originals')
    add_select(p)
    p.add_argument('--manifest', default=os.path.join(HERE, 'materials.csv'))
    p.add_argument('--pack', required=True)
    p.set_defaults(f=cmd_verify)
    p = sub.add_parser('calibrate', help='measure the normal maps\' convention against the height model')
    add_select(p)
    p.set_defaults(f=cmd_calibrate)
    p = sub.add_parser('draft', help='draft manifest rows with a local vision-language model')
    add_select(p)
    p.add_argument('--manifest', default=os.path.join(HERE, 'materials.csv'))
    p.add_argument('--redo', action='store_true', help='redraft rows that are already drafts')
    p.set_defaults(f=cmd_draft)
    p = sub.add_parser('merge', help='merge the edits a sheet downloaded into the manifest as human rows')
    p.add_argument('edits')
    p.add_argument('--manifest', default=os.path.join(HERE, 'materials.csv'))
    p.set_defaults(f=cmd_merge)
    p = sub.add_parser('montage', help='PNG pages of labelled textures to review the manifest by eye')
    add_select(p)
    p.add_argument('--manifest', default=os.path.join(HERE, 'materials.csv'))
    p.add_argument('--class', dest='cls', help='only textures of these classes (comma-separated)')
    p.add_argument('--out', required=True, help='path prefix: <out>_01.png, ...')
    p.set_defaults(f=cmd_montage)
    p = sub.add_parser('sheet', help='an HTML contact sheet to review (and correct) the manifest, or compare a pack')
    add_select(p)
    p.add_argument('--manifest', default=os.path.join(HERE, 'materials.csv'))
    p.add_argument('--pack', help='show each texture\'s result beside the original')
    p.add_argument('--group', choices=['map', 'kind', 'class'], default='map')
    p.add_argument('--out', required=True)
    p.set_defaults(f=cmd_sheet)
    a = ap.parse_args()
    return a.f(a)


if __name__ == '__main__':
    sys.exit(main())
