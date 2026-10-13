"""texpack: AI-upscaled texture packs from a manifest of material descriptions
(story 5.8; README.md). Run with ComfyUI's bundled Python:

  %TEXPACK_HOME%\\ComfyUI_windows_portable\\python_embeded\\python.exe texpack.py <command> ...

Commands: check-env, draft, montage, sheet, merge, run, verify, calibrate; census, views,
checkviews (story 9.2: census.py); cards, families, labels (story 9.3: labels.py).
"""
import argparse
import collections
import json
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


def default_data():
    return os.environ.get('HEXENLICHT_DATA') or os.path.join(os.path.dirname(REPO), 'Hexenlicht-data')


def default_export():
    data = default_data()
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
    # the redraw's (9.1, 9.4): in ComfyUI's folders or a base_path of extra_model_paths.yaml
    extra = []
    yaml = os.path.join(home, 'extra_model_paths.yaml')
    if os.path.exists(yaml):
        with open(yaml, encoding='utf-8') as f:
            extra = [ln.split(':', 1)[1].strip() for ln in f if ln.strip().startswith('base_path:')]
    for k, sub in (('generator', 'diffusion_models'), ('text_encoder', 'text_encoders'), ('vae', 'vae')):
        cands = [os.path.join(b, sub, classes['redraw'][k]) for b in [comfy] + extra]
        need.append((next((c for c in cands if os.path.exists(c)), cands[0]), k))
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


def add_label_filters(p):
    p.add_argument('--tier', help='only textures of these tiers (comma-separated)')
    p.add_argument('--family', help='only these families\' textures (comma-separated)')
    p.add_argument('--hub', help='only textures whose home (the hub where they cover the most) is one of these')


def label_filter(a, sel, res):
    """--tier, --family, --hub on the resolved labels (a hub: the texture's home in the census)."""
    if a.tier:
        sel = [e for e in sel if res[e.stem].tier in a.tier.split(',')]
    if a.family:
        sel = [e for e in sel if res[e.stem].family in a.family.split(',')]
    if a.hub:
        import census
        cen = census.load(census.default_census())
        want = set(a.hub.split(','))
        sel = [e for e in sel if ((cen.get(e.stem, {}).get('hubs') or 'none').split() or ['none'])[0] in want]
    return sel


def cmd_run(a):
    """The world and liquid textures of the redrawn tiers through redraw.py (9.1's pipeline, since
    9.4), the rest (skins until 9.6, fx, sprites, anything unlabeled) through 5.8's."""
    from comfy import Comfy
    import redraw
    ex, entries, sel = selection(a)
    classes = manifest.load_classes(os.path.join(HERE, 'classes.toml'))
    rows = manifest.load_manifest(a.manifest)
    res = resolve_all(sel, rows, classes)
    sel = label_filter(a, sel, res)
    v2 = [e for e in sel if e.kind in ('world', 'liquid') and res[e.stem].tier in redraw.TIERS]
    sel = [e for e in sel if e not in v2]
    out = os.path.abspath(a.out)
    print(f"texpack run: {len(v2) + len(sel)} textures from {ex} into {out}" + (f" ({len(v2)} redrawn)" if v2 else ''))
    done = skipped = since = 0
    t0 = time.time()
    with Comfy(texpack_home(), attach=a.attach, deterministic=not a.nondeterministic) as c:
        if v2:
            import census
            import labels
            data = a.data or default_data()
            palette = redraw.load_palette(data)
            if palette is None:
                raise SystemExit(f"no data1\\pak0.pak in {data} (--data): the redraw's materials and special colors need the palette")
            fams = labels.load_families(a.families)
            leads = {f: r['lead'] for f, r in fams.items() if r.get('lead')}
            rd = redraw.Redraw(ex, out, classes, c, entries, rows, census.load(census.default_census()), leads, palette,
                               seed=9100 if a.seed is None else a.seed, front=a.front, restart_every=a.restart_every,
                               finishes=labels.finishes(fams))
            made, unchanged, written = rd.run([e.stem for e in v2], force=a.force, reuse=a.reuse)
            done, skipped = done + made, skipped + unchanged
            print(f"redraw: {made} drawn, {unchanged} unchanged, {written} written to textures\\")
        if sel:
            c.start()
        r = pipeline.Runner(ex, out, classes, c, seed=5800 if a.seed is None else a.seed, flip_y=FLIP_Y)
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


def redraw_setup(a, entries, ex, comfy=None, out=None):
    """A redraw.Redraw over the export (its plan of copies, sets and heroes), and the manifest's rows."""
    import census
    import labels
    import redraw
    classes = manifest.load_classes(os.path.join(HERE, 'classes.toml'))
    rows = manifest.load_manifest(a.manifest)
    data = a.data or default_data()
    palette = redraw.load_palette(data)
    if palette is None:
        raise SystemExit(f"no data1\\pak0.pak in {data} (--data): the redraw's materials and special colors need the palette")
    fams = labels.load_families(a.families)
    leads = {f: r['lead'] for f, r in fams.items() if r.get('lead')}
    cen = census.load(census.default_census())
    rd = redraw.Redraw(ex, out or os.path.abspath(a.out), classes, comfy, entries, rows, cen, leads, palette,
                       seed=9100 if getattr(a, 'seed', None) is None else a.seed, front=getattr(a, 'front', redraw.FRONT),
                       finishes=labels.finishes(fams))
    return rd, cen, rows


def cmd_heroes(a):
    """9.5's hero candidates (heroes.py): each family's lead with the label's and --finishes'
    finishes, two seeds each, and its largest layout member with each; --page: the owner's page
    from them; --apply: its picks into families.csv and the manifest."""
    import heroes
    import labels
    ex = a.export or default_export()
    entries = pipeline.load_export(ex)
    if a.apply:
        # the plan: the lead's set, whose other members' seeds would hide the pick
        import census
        import redraw
        rows = manifest.load_manifest(a.manifest)
        leads = {f: r['lead'] for f, r in labels.load_families(a.families).items() if r.get('lead')}
        cen = census.load(census.default_census())
        _, _, plan = redraw.export_plan(ex, entries, rows, manifest.load_classes(os.path.join(HERE, 'classes.toml')), cen, leads)
        if heroes.apply(a.apply, a.families, rows, plan, census=cen):
            manifest.write_manifest(a.manifest, rows)
        return 0
    if not a.out:
        raise SystemExit('heroes: --out (or --apply)')
    out = os.path.abspath(a.out)
    if a.page:
        with open(os.path.join(out, 'heroes.json')) as f:
            info = json.load(f)
        classes = manifest.load_classes(os.path.join(HERE, 'classes.toml'))
        rows = manifest.load_manifest(a.manifest)
        res = {d['lead']: manifest.resolve(d['lead'], entries[d['lead']].kind, rows, classes) for d in info.values()}
        return heroes.page(ex, entries, res, info, out, a.page, a.views or os.path.join(texpack_home(), 'views'), a.shots)
    from comfy import Comfy
    with Comfy(texpack_home(), attach=a.attach) as c:
        rd, cen, _ = redraw_setup(a, entries, ex, c, os.path.join(out, 'c1'))
        if a.stems:
            stems = a.stems.split(',') if not a.stems.startswith('@') else [
                ln.split('#')[0].strip() for ln in open(a.stems[1:], encoding='utf-8') if ln.split('#')[0].strip()]
            items = heroes.textures(rd.plan, rd.res, cen, stems)
        else:
            fams = labels.load_families(a.families)
            items = heroes.families(rd.plan, rd.res, fams, cen, set(a.family.split(',')) if a.family else None,
                                    set(a.hub.split(',')) if a.hub else None)
        fins = heroes.load_finishes(a.finishes)
        print(f"texpack heroes: {len(items)} families ({sum(1 for i in items if i[0] in fins)} with finishes), "
              f"{sum(1 for i in items if i[3])} with a layout member, into {out}")
        t0 = time.time()
        heroes.draw(rd, out, items, fins, force=a.force, pack=os.path.abspath(a.pack) if a.pack else None)
    print(f"{time.time() - t0:.0f} s")
    return 0


def cmd_reseed(a):
    """9.5: the next seeds for what `verify` failed on a seam or moved shapes (reseed.py)."""
    import reseed
    from comfy import Comfy
    ex = a.export or default_export()
    entries = pipeline.load_export(ex)
    pack = os.path.abspath(a.pack)
    with Comfy(texpack_home(), attach=a.attach) as c:
        rd, _, rows = redraw_setup(a, entries, ex, c, pack)
        reseed.run(rd, ex, pack, rows, a.manifest, set(a.stems.split(',')) if a.stems else None, a.tries)
    return 0


def cmd_review(a):
    """The owner's page of a hub (review.py), or --apply the verdicts it downloaded."""
    import labels
    import review
    import redraw
    import verify
    ex = a.export or default_export()
    entries = pipeline.load_export(ex)
    pack = os.path.abspath(a.pack)
    rd, cen, rows = redraw_setup(a, entries, ex, None, pack)
    if a.apply:
        lab, changed = review.apply(rd, rows, a.apply, pack)
        if changed:
            manifest.write_manifest(a.manifest, rows)
        if lab:
            path = os.path.join(pack, '.texpack', 'review_labels.csv')
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, 'w', newline='', encoding='utf-8') as f:
                import csv
                w = csv.DictWriter(f, ['pattern', 'tier', 'regions', 'description', 'source'], lineterminator='\n')
                w.writeheader()
                for r in lab:
                    w.writerow(dict(r, source='human'))
            cmd_merge(argparse.Namespace(edits=path, manifest=a.manifest))
        return 0
    if not a.hub or not a.out:
        raise SystemExit('review: --hub and --out (or --apply)')
    # the export's textures: a whole (M81) has a row but no file, and its pieces are on the page
    stems = [s for s in rd.res if s in entries and review.page_hub(cen, s) == a.hub]
    fams = labels.load_families(a.families)
    part, parts = (int(x) for x in a.part.split('/')) if a.part else (1, 1)
    cards = review.split(review.cards(stems, rd.res, cen, fams, a.hub), part, parts)
    flags = {}
    if a.flags:
        import csv
        with open(a.flags, newline='', encoding='utf-8') as f:
            flags = {r['stem']: r['flag'] for r in csv.DictReader(f) if r.get('flag')}
    views = a.views or os.path.join(texpack_home(), 'views')
    if a.png:
        pages = review.png_pages(ex, entries, rd.res, cen, fams, cards, pack, a.shots, views, a.out,
                                 verify.load_report(pack))
        print(f"texpack review: {len(cards)} textures on {len(pages)} pages, {a.out}_01.png ...")
        return 0
    title = a.hub + (f' {part}/{parts}' if parts > 1 else '')
    review.page(ex, entries, rd.res, cen, fams, cards, pack, a.shots, views, a.out, title,
                verify.load_report(pack), flags)
    return 0


def cmd_packviews(a):
    import census
    import review
    review.read_runs(a.runs, a.shots, census.load(census.default_census()),
                     a.views or os.path.join(texpack_home(), 'views'))
    return 0


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
    import redraw
    ex, entries, sel = selection(a)
    classes = manifest.load_classes(os.path.join(HERE, 'classes.toml'))
    rows = manifest.load_manifest(a.manifest)
    res = resolve_all(sel, rows, classes)
    sel = label_filter(a, sel, res)
    palette = plan = orig = None
    if any(e.kind in ('world', 'liquid') and res[e.stem].tier in redraw.TIERS for e in sel):
        import census
        import labels
        data = a.data or default_data()
        palette = redraw.load_palette(data)
        if palette is None:
            raise SystemExit(f"no data1\\pak0.pak in {data} (--data): the redraw's materials per region need the palette")
        leads = {f: r['lead'] for f, r in labels.load_families(a.families).items() if r.get('lead')}
        _, orig, plan = redraw.export_plan(ex, entries, rows, classes, census.load(census.default_census()), leads)
    return verify.run(ex, os.path.abspath(a.pack), sel, res, palette, classes, plan, orig)


def cmd_draft(a):
    import draft
    ex, entries, sel = selection(a)
    classes = manifest.load_classes(os.path.join(HERE, 'classes.toml'))
    return draft.run(texpack_home(), ex, sel, classes, a.manifest, redo=a.redo)


def cmd_merge(a):
    import census
    rows = manifest.load_manifest(a.manifest)
    by = {r['pattern']: r for r in rows}
    n = 0
    # a sheet's edits name the columns they change; an empty overrides cell keeps the row's
    # (5.8's sheet wrote the column without ever filling it)
    given = set(manifest.read_rows(a.edits)[0]) - {'pattern', 'source'}
    cen = census.load(census.default_census())
    label_cols = ('purpose', 'tier', 'class', 'family', 'regions', 'description')
    for r in manifest.load_manifest(a.edits):
        old = by.get(r['pattern'])
        if old:
            old.update({k: r[k] for k in manifest.COLUMNS if k in given and (k != 'overrides' or r[k])})
            old['source'] = 'human'
        else:
            old = by[r['pattern']] = dict(r, source='human')
        n += 1
        # an animation's frames are labeled alike (the owner's page shows one of them)
        for s in (cen.get(r['pattern'], {}).get('anim') or '').split():
            if s != r['pattern'] and s in by and by[s].get('tier'):
                by[s].update({k: old[k] for k in label_cols})
                by[s]['source'] = 'human'
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
    if a.questions:
        asked = {r['pattern']: r['question'].strip() for r in manifest.read_rows(a.questions)[1] if (r.get('question') or '').strip()}
        missing = [s for s in asked if s not in entries]
        if missing:
            raise SystemExit(f"not in the export: {', '.join(missing)}")
        sel = [entries[s] for s in asked]
        models = None
        if any(e.kind == 'skin' for e in sel):
            import labels
            models = labels.Models(a.data or default_data())
        return sheet.questions(ex, sel, resolve_all(sel, rows, classes), classes, asked,
                               a.views or os.path.join(texpack_home(), 'views'), a.out, models)
    return sheet.run(ex, sel, resolve_all(sel, rows, classes), classes, a.out, pack=a.pack, group=a.group)


def label_inputs(a):
    import census
    import labels
    from redraw import load_palette
    ex, entries, sel = selection(a)
    palette = load_palette(a.data or default_data())
    if palette is None:
        raise SystemExit(f"no data1\\pak0.pak in {a.data or default_data()} (--data): the palette's ramps need it")
    cen = census.load(a.census or census.default_census())
    if getattr(a, 'hub', None):
        want = set(a.hub.split(','))
        sel = [e for e in sel if ((cen.get(e.stem, {}).get('hubs') or 'none').split() or ['none'])[0] in want]
    return labels, ex, entries, sel, palette, cen


def cmd_cards(a):
    labels, ex, entries, sel, palette, cen = label_inputs(a)
    if not a.frames:
        # an animation's frames are labeled alike: one card for it, the frame its view shows
        shown = set()
        for e in sel:
            anim = cen.get(e.stem, {}).get('anim', '').split()
            if anim:
                ok = [s for s in anim if cen.get(s, {}).get('view') == 'ok'] or anim
                shown.add(ok[0])
        sel = [e for e in sel if not cen.get(e.stem, {}).get('anim') or e.stem in shown]
    skins = bool(sel) and all(e.kind == 'skin' for e in sel)
    if not skins and any(e.kind == 'skin' for e in sel):
        raise SystemExit('cards: skins have pages of their own (the model drawn with them): select them apart (--kind skin)')
    kinds = ('skin',) if skins else ('world', 'liquid', 'sky')
    rel = labels.candidates(ex, [e for e in entries.values() if e.kind in kinds], palette, cen, skins=skins)
    # related textures next to each other: by the first of their chain of shared texels
    root = {e.stem: e.stem for e in sel}
    for e in sel:
        for o, k, _ in rel.get(e.stem, []):
            if k == 'texels' and o in root:
                ra, rb = root[e.stem], root[o]
                while root[ra] != ra:
                    ra = root[ra]
                while root[rb] != rb:
                    rb = root[rb]
                root[max(ra, rb)] = min(ra, rb)

    def top(s):
        while root[s] != s:
            s = root[s]
        return s
    sel.sort(key=lambda e: (top(e.stem), e.stem))
    rows = manifest.load_manifest(a.manifest)
    classes = manifest.load_classes(os.path.join(HERE, 'classes.toml'))
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    if a.family:
        fam = set(a.family.split(','))
        sel = [e for e in sel if manifest.resolve(e.stem, e.kind, rows, classes).family in fam]
        sel.sort(key=lambda e: (manifest.resolve(e.stem, e.kind, rows, classes).family, e.stem))
    if a.overview:
        n = labels.overview(ex, sel, rows, classes, a.out)
        print(f"texpack cards: {len(sel)} textures on {n} overview pages -> {a.out}_NN.png")
        return 0
    if skins:
        n = labels.skin_cards(ex, sel, rows, classes, palette, labels.Models(a.data or default_data()), a.out,
                              per_page=a.per_page or 4, related=rel)
    else:
        n = labels.cards(ex, sel, rows, classes, palette, cen, a.views or os.path.join(texpack_home(), 'views'), a.out,
                         per_page=a.per_page or 6, related=rel)
    print(f"texpack cards: {len(sel)} textures on {n} pages -> {a.out}_NN.png and .txt")
    return 0


def cmd_families(a):
    labels, ex, entries, sel, palette, cen = label_inputs(a)
    rel = labels.candidates(ex, sel, palette, cen, skins=bool(sel) and all(e.kind == 'skin' for e in sel))
    for e in sel:
        if e.stem in rel:
            print(f"{e.stem}: {labels.related_text(rel[e.stem])}")
    return 0


def cmd_labels(a):
    labels, ex, entries, sel, palette, cen = label_inputs(a)
    classes = manifest.load_classes(os.path.join(HERE, 'classes.toml'))
    if a.action == 'apply':
        if not a.labels:
            raise SystemExit('labels apply: name the labels CSV (--labels)')
        n, dropped, owners, folded = labels.apply(a.labels, a.manifest, entries, classes, cen)
        print(f"texpack labels: {n} rows from {a.labels} into {a.manifest}, {dropped} rows it covered dropped"
              f" ({folded} labeled textures had a human row of the earlier kind, folded in)"
              + (f"; {owners} of the owner's labeled rows kept (the CSV's rows aren't `human`)" if owners else ''))
        return 0
    rows = manifest.load_manifest(a.manifest)
    if a.action == 'leads':
        done = labels.set_leads(a.families, rows, cen, entries)
        print(f"texpack labels: {len(done)} families given a lead (the member with the most area, or the biggest skin) in {a.families}")
        return 0
    if a.labels:
        # a labels CSV checked as if applied, the manifest untouched
        extra = manifest.load_manifest(a.labels)
        unknown = [r['pattern'] for r in extra if r['pattern'] not in entries]
        if unknown:
            print(f"not in the export: {', '.join(unknown)}")
        for r in extra:
            r['source'] = r['source'] if r['source'] in labels.LABELED else 'claude'
        stems = {r['pattern'] for r in extra}
        rows = [r for r in rows if r['pattern'] not in stems] + extra
        sel = [e for e in sel if e.stem in stems]
    fams = labels.load_families(a.families)
    lab, probs = labels.check(entries, sel, rows, classes, palette, ex, cen, fams)
    for s in sorted(probs):
        print(f"{s}: {'; '.join(probs[s])}")
    by = collections.Counter(r['source'] for r in lab.values())
    print(f"texpack labels: {len(lab)} of {len(sel)} labeled ({', '.join(f'{k} {v}' for k, v in sorted(by.items()))}), "
          f"{len(probs)} with problems; {len(fams)} families")
    return 1 if probs else 0


def cmd_census(a):
    import census
    return census.run_census(a.data or default_data(), a.export or default_export(), a.census, reset=a.reset)


def cmd_views(a):
    import bspviews
    import census
    maps = bspviews.Maps(a.data or default_data())
    rows = census.load(a.census)
    unknown = sorted({r['view_map'] for r in rows.values() if r['view_map'] and r['view_map'] not in maps.files})
    if unknown:
        raise SystemExit(f"not in the paks of {a.data or default_data()}: {', '.join(unknown)}")
    lines = census.view_lines(rows, a.todo, a.game, a.hub, set(a.stems.split(',')) if a.stems else None, maps.game)
    with open(a.out, 'w', newline='', encoding='utf-8') as f:
        f.write(f'# game {a.game}: views_run.ps1' + (' -Portals' if a.game == 'portals' else '') + '\r\n')
        f.writelines(ln + '\r\n' for ln in lines)
    print(f"texpack views: {len(lines)} textures into {a.out}")
    return 0


def cmd_checkviews(a):
    import census
    return census.check_runs(a.runs, a.census, a.shots or os.path.join(texpack_home(), 'views'),
                             a.data or default_data(), a.export or default_export())


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('check-env', help='the models, torch and the export are in place')
    p.add_argument('--export')
    p.set_defaults(f=cmd_check_env)
    p = sub.add_parser('run', help='redraw or upscale the selected textures into a pack folder')
    add_select(p)
    add_label_filters(p)
    p.add_argument('--manifest', default=os.path.join(HERE, 'materials.csv'))
    p.add_argument('--families', default=os.path.join(HERE, 'families.csv'))
    p.add_argument('--data', help='the game data folder (the palette from data1\\pak0.pak; default: Hexenlicht-data beside the repository)')
    p.add_argument('--out', required=True, help='the pack folder (its textures\\ is what goes into data1)')
    p.add_argument('--seed', type=int, help='the seeds\' base (default: 9100 for the redraw, 5800 for 5.8\'s upscale)')
    p.add_argument('--front', type=float, default=0.5, help='the redraw: how much of the frontal (cavity) shading stays in the albedo (0..1)')
    p.add_argument('--reuse', action='store_true', help='the redraw: keep the model images of an earlier run, redo the stages after them')
    p.add_argument('--force', action='store_true', help='redo textures that are unchanged')
    p.add_argument('--restart-every', type=int, default=80, help='restart ComfyUI after this many textures made (0: never)')
    p.add_argument('--attach', action='store_true', help='use a ComfyUI already running on port 8199')
    p.add_argument('--nondeterministic', action='store_true', help='without ComfyUI\'s --deterministic')
    p.set_defaults(f=cmd_run)
    p = sub.add_parser('heroes', help='candidates for each family\'s hero (9.5), or the owner\'s page of them')
    p.add_argument('--export')
    p.add_argument('--manifest', default=os.path.join(HERE, 'materials.csv'))
    p.add_argument('--families', default=os.path.join(HERE, 'families.csv'))
    p.add_argument('--data', help='the game data folder (the palette from data1\\pak0.pak)')
    p.add_argument('--out', help='the candidates\' folder: c1 .. c<n>, each a pack, and heroes.json')
    p.add_argument('--family', help='only these families (comma-separated)')
    p.add_argument('--hub', help='only families of these hubs (families.csv\'s)')
    p.add_argument('--stems', help='candidates for these textures instead, seeds only (comma-separated, or @file: the review\'s `candidates`)')
    p.add_argument('--finishes', help='the candidates\' finishes: a CSV of family, rougher, finer (without: seeds only)')
    p.add_argument('--pack', help='--stems: the pack whose heroes\' drawings are the layout members\' references')
    p.add_argument('--apply', help='the picks the page downloaded (texpack_heroes.csv): finishes and the leads\' seeds')
    p.add_argument('--force', action='store_true')
    p.add_argument('--attach', action='store_true', help='use a ComfyUI already running on port 8199')
    p.add_argument('--page', help='write the owner\'s page (HTML) from the candidates drawn')
    p.add_argument('--views', help='--page: 9.2\'s shots of the originals (default: $TEXPACK_HOME\\views)')
    p.add_argument('--shots', help='--page: the candidates in the game (`packviews` of views_run.ps1 -Pack runs: <shots>\\c<k>)')
    p.set_defaults(f=cmd_heroes)
    p = sub.add_parser('reseed', help='the next seeds for what verify failed on a seam or moved shapes (9.5), into the manifest')
    p.add_argument('--export')
    p.add_argument('--manifest', default=os.path.join(HERE, 'materials.csv'))
    p.add_argument('--families', default=os.path.join(HERE, 'families.csv'))
    p.add_argument('--data', help='the game data folder (the palette from data1\\pak0.pak)')
    p.add_argument('--pack', required=True, help='the pack whose .texpack\\verify.csv names the failures (verify writes it)')
    p.add_argument('--stems', help='only these failures (comma-separated)')
    p.add_argument('--tries', type=int, default=4)
    p.add_argument('--seed', type=int, help='the seeds\' base, as the run\'s (default 9100)')
    p.add_argument('--front', type=float, default=0.5, help='as the run\'s')
    p.add_argument('--attach', action='store_true', help='use a ComfyUI already running on port 8199')
    p.set_defaults(f=cmd_reseed)
    p = sub.add_parser('review', help='the owner\'s page of a hub with the pack at 9.2\'s views (9.5), or --apply its verdicts')
    p.add_argument('--export')
    p.add_argument('--manifest', default=os.path.join(HERE, 'materials.csv'))
    p.add_argument('--families', default=os.path.join(HERE, 'families.csv'))
    p.add_argument('--data', help='the game data folder (the palette from data1\\pak0.pak)')
    p.add_argument('--pack', required=True)
    p.add_argument('--hub', help='blackmarsh, mazaera, thysis, septimus, cathedral, keep, tulku or other (the deathmatch maps, the test map, the unseen)')
    p.add_argument('--part', help='i/n: a part of the hub\'s page (families kept whole)')
    p.add_argument('--shots', help='packviews\' folder (the pack in the game)')
    p.add_argument('--views', help='9.2\'s shots of the originals (default: $TEXPACK_HOME\\views)')
    p.add_argument('--flags', help='Claude\'s flags: a CSV of stem, flag')
    p.add_argument('--out', help='the page (HTML), or with --png a path prefix')
    p.add_argument('--png', action='store_true', help='PNG pages and their .txt for Claude\'s pre-review instead (5 textures a page)')
    p.add_argument('--apply', help='the verdicts the page downloaded (texpack_review.csv)')
    p.set_defaults(f=cmd_review)
    p = sub.add_parser('packviews', help='read views_run.ps1 -Pack runs: the shots as PNGs, the hits, the brightness against the originals')
    p.add_argument('runs', nargs='+', help='views_run.ps1\'s output folders (-Out\\<Tag>)')
    p.add_argument('--shots', required=True, help='where the PNGs and packviews.csv go')
    p.add_argument('--views', help='9.2\'s shots of the originals, for runs without -Before (default: $TEXPACK_HOME\\views)')
    p.set_defaults(f=cmd_packviews)
    p = sub.add_parser('verify', help='check a pack against the spec and the originals')
    add_select(p)
    add_label_filters(p)
    p.add_argument('--manifest', default=os.path.join(HERE, 'materials.csv'))
    p.add_argument('--data', help='the game data folder (the palette: the redraw\'s materials per region)')
    p.add_argument('--families', default=os.path.join(HERE, 'families.csv'), help='as the run\'s (its sets and heroes)')
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
    p.add_argument('--questions', help='the owner\'s page: a labels CSV whose `question` column asks about its rows')
    p.add_argument('--views', help='--questions: 9.2\'s shots (default: $TEXPACK_HOME\\views)')
    p.add_argument('--data', help='--questions about skins: the game data folder whose paks have the models')
    p.add_argument('--out', required=True)
    p.set_defaults(f=cmd_sheet)
    for name, f, hlp in (('cards', cmd_cards, 'the pages a labeler reads: original, ramp map, the three shots, census facts'),
                         ('families', cmd_families, 'candidate families: shared texels, long shared edges, the same ramps'),
                         ('labels', cmd_labels, 'check the labels, or apply a labels CSV to the manifest')):
        p = sub.add_parser(name, help=hlp)
        if name == 'labels':
            p.add_argument('action', choices=['check', 'apply', 'leads'])
            p.add_argument('--labels', help='the labels CSV (the manifest\'s columns, a stem per row): apply it, or check it as if applied')
            p.add_argument('--families', default=os.path.join(HERE, 'families.csv'))
        add_select(p)
        p.add_argument('--hub', help='only textures whose home (the hub where they cover the most) is one of these; none: on no drawn face')
        p.add_argument('--data', help='the game data folder (the palette from data1\\pak0.pak)')
        p.add_argument('--census')
        p.add_argument('--manifest', default=os.path.join(HERE, 'materials.csv'))
        if name == 'cards':
            p.add_argument('--out', required=True, help='path prefix: <out>_01.png and .txt, ...')
            p.add_argument('--views', help='9.2\'s shots (default: $TEXPACK_HOME\\views)')
            p.add_argument('--per-page', type=int, help='textures a page (default: 6; skins 4)')
            p.add_argument('--frames', action='store_true', help='a card for every frame of an animation')
            p.add_argument('--overview', action='store_true', help='the originals only, 60 to a page, with their stems and labels')
            p.add_argument('--family', help='only these families (comma-separated), members side by side')
        p.set_defaults(f=f)
    p = sub.add_parser('census', help='the census of every world texture from the maps, and its views (census.csv)')
    p.add_argument('--export')
    p.add_argument('--data', help='the game data folder whose paks have the maps (default: Hexenlicht-data beside the repository)')
    p.add_argument('--census', help='the file (default: census.csv here)')
    p.add_argument('--reset', action='store_true', help='every view from the first face again, unchecked (after the picker changed)')
    p.set_defaults(f=cmd_census)
    p = sub.add_parser('views', help='the census\'s views as views_run.ps1\'s lines')
    p.add_argument('--census')
    p.add_argument('--out', required=True)
    p.add_argument('--todo', action='store_true', help='only the views not checked yet')
    p.add_argument('--game', choices=['data1', 'portals'], required=True,
                   help='the views in this game\'s maps (a run of views_run.ps1 each: portals with -Portals)')
    p.add_argument('--hub', help='only views in this hub\'s maps (census.py\'s HUBS)')
    p.add_argument('--stems', help='comma-separated stems')
    p.add_argument('--data', help='the game data folder (for --game: which pak has the map)')
    p.set_defaults(f=cmd_views)
    p = sub.add_parser('checkviews', help='read views_run.ps1\'s runs into the census, the shots as PNGs')
    p.add_argument('runs', nargs='+', help='views_run.ps1\'s output folders (-Out\\<Tag>)')
    p.add_argument('--census')
    p.add_argument('--shots', help='where the PNGs go (default: $TEXPACK_HOME\\views)')
    p.add_argument('--export')
    p.add_argument('--data')
    p.set_defaults(f=cmd_checkviews)
    a = ap.parse_args()
    if a.cmd in ('census', 'views', 'checkviews') and not a.census:
        import census
        a.census = census.default_census()
    return a.f(a)


if __name__ == '__main__':
    sys.exit(main())
