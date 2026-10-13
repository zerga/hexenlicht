"""texpack's review of a pack in the game (story 9.5; README "The world pack", DECISIONS M72-).

  texpack.py packviews   read views_run.ps1 -Pack runs: each view's shots as PNGs, whether
                         `vk_materials here` named the texture, the view's brightness against the
                         original's (the same run's `_orig` shots, else 9.2's)
  texpack.py review      the owner's page of a hub: per texture the original and the pack at
                         9.2's views, the tiles, the numbers and Claude's flags, a verdict each

The brightness of a shot is its mean linear luminance (the 8-bit values decoded with imaging's
2.2, Rec. 709's weights) over the whole frame, and the mean of its darkest fifth; a view's
ratio is the pack's over the original's. The shots are derived from Raven's textures: they stay
outside the repository.
"""
import csv
import os

import numpy as np
from PIL import Image

import census
from imaging import _lin
from pipeline import LUM

SHOTS = ('close', 'wide', 'albedo', 'close_orig', 'wide_orig')
COLUMNS = ['stem', 'map', 'hit', 'close', 'wide', 'close_dark', 'wide_dark']


def brightness(path):
    """(mean, darkest fifth's mean) of a shot's linear luminance."""
    y = _lin(np.asarray(Image.open(path).convert('RGB'))) @ LUM
    y = np.sort(y.ravel())
    return float(y.mean()), float(y[:max(1, y.size // 5)].mean())


def load(path):
    if not os.path.exists(path):
        return {}
    with open(path, newline='', encoding='utf-8') as f:
        return {r['stem']: r for r in csv.DictReader(f)}


def read_runs(runs, shots, cen, orig_views=None, log=print):
    """Each run's views into shots\\: `<stem>_<close|wide|albedo|close_orig|wide_orig>.png` and a
    row of packviews.csv (a later run's view of a stem replaces an earlier one's): hit (`ok`
    where the close view's `vk_materials here` named the texture, else what it named), the
    brightness ratios of the close and the wide view and of their darkest fifths (empty without
    an original to compare with: the run's `_orig` shot, else 9.2's in orig_views)."""
    os.makedirs(shots, exist_ok=True)
    path = os.path.join(shots, 'packviews.csv')
    rows = load(path)
    n = ok = 0
    for run in runs:
        with open(os.path.join(run, 'views.csv'), newline='', encoding='utf-8') as f:
            shot = list(csv.DictReader(f))
        with open(os.path.join(run, 'debug_h2.log'), encoding='latin-1') as f:
            hits = census.parse_log(f.read().splitlines())
        for v in shot:
            close = hits.get((v['name'], 'close'))
            if close is None or close['what'] == 'no map':
                continue            # not shot: the run ended before it, or its map didn't load
            r = cen.get(v['stem'], {})
            group = set(r['anim'].split()) if r.get('anim') else None
            hit = 'ok' if census.hit_matches(v['stem'], close, group) else close['what']
            row = {'stem': v['stem'], 'map': v['map'], 'hit': hit}
            for which in SHOTS:
                src = os.path.join(run, f"{v['name']}_{which}.tga")
                if os.path.exists(src):
                    census._png(src, os.path.join(shots, f"{v['stem']}_{which}.png"))
            for which in ('close', 'wide'):
                new = os.path.join(shots, f"{v['stem']}_{which}.png")
                old = os.path.join(shots, f"{v['stem']}_{which}_orig.png")
                if not os.path.exists(old) and orig_views:
                    old = os.path.join(orig_views, f"{v['stem']}_{which}.png")
                if os.path.exists(new) and os.path.exists(old):
                    (m1, d1), (m0, d0) = brightness(new), brightness(old)
                    row[which] = f'{m1 / max(m0, 1e-6):.3f}'
                    row[which + '_dark'] = f'{d1 / max(d0, 1e-6):.3f}'
            rows[v['stem']] = row
            n += 1
            ok += hit == 'ok'
    with open(path, 'w', newline='', encoding='utf-8') as f:
        w = csv.DictWriter(f, COLUMNS, lineterminator='\n')
        w.writeheader()
        for s in sorted(rows):
            w.writerow({c: rows[s].get(c, '') for c in COLUMNS})
    log(f"packviews: {n} views read ({ok} named their texture) into {shots}; {len(rows)} in packviews.csv")
    return rows


# -- the owner's page of a hub ------------------------------------------------------------------

def _shot(shots, views, stem, which, original):
    """A view's shot: the pack's from packviews' folder, or the original's, the same run's
    (-Before) else 9.2's; None where there is none."""
    if original:
        paths = [os.path.join(d, f'{stem}_{which}{x}.png') for d, x in ((shots, '_orig'), (views, '')) if d]
    else:
        paths = [os.path.join(shots, f'{stem}_{which}.png')] if shots else []
    return next((p for p in paths if os.path.exists(p)), None)


HUB_ORDER = ['blackmarsh', 'mazaera', 'thysis', 'septimus', 'cathedral', 'keep', 'tulku', 'other']


def page_hub(cen, stem):
    """The page a texture is on: its home hub (the census's first), the deathmatch maps' and the
    test map's on `other`; a texture on no drawn face on its animation's page, else `other`."""
    hubs = (cen.get(stem, {}).get('hubs') or '').split()
    if not hubs:
        for f in (cen.get(stem, {}).get('anim') or '').split():
            hubs = hubs or (cen.get(f, {}).get('hubs') or '').split()
    h = hubs[0] if hubs else 'none'
    return h if h in HUB_ORDER else 'other'


def cards(stems, res, cen, fams, hub=None):
    """The order on a page: each family together (its hero first, then by area; the hub's own
    families first), then the textures of no family by area; an animation once, as its frame
    with the view."""
    import groups
    seen, out = set(), []
    by_fam = {}
    for s in stems:
        by_fam.setdefault(res[s].family, []).append(s)
    lead = {f: (r.get('lead') or '') for f, r in fams.items()}
    for f in sorted((k for k in by_fam if k), key=lambda k: ((fams.get(k) or {}).get('hub') != hub, k)):
        ms = sorted(by_fam[f], key=lambda s: (s != lead.get(f), -groups._area(cen, s), s))
        out += [(f, s) for s in ms]
    out += [('', s) for s in sorted(by_fam.get('', []), key=lambda s: (-groups._area(cen, s), s))]
    keep = []
    for f, s in out:
        anim = (cen.get(s, {}).get('anim') or '').split()
        if anim and set(anim) & seen:
            continue
        seen.add(s)
        keep.append((f, s))
    return keep


def page(ex, entries, res, cen, fams, cardlist, pack, shots, views, out_path, title, report=None, flags=None,
         size=240):
    """The owner's page of a hub (or a part of one): per texture its original and the pack's albedo
    tiled 2x2 (brightened alike), the pack and the original at 9.2's close and wide views, the
    brightness ratios, `verify`'s result and Claude's flag; a verdict each (ok, another seed,
    candidates, or the label edited) and a note. "Download verdicts" saves the rows with a verdict,
    for `texpack.py review apply`."""
    import html
    from heroes import _lift_for, _tile, _uri
    report = report or {}
    flags = flags or {}
    pv = load(os.path.join(shots, 'packviews.csv')) if shots else {}
    body = []
    fam_now = None
    n = 0
    for f, s in cardlist:
        if f != fam_now:
            fam_now = f
            what = (fams.get(f) or {}).get('what', '') if f else 'textures of no family'
            fin = (fams.get(f) or {}).get('finish') if f else ''
            body.append(f'<h2>{html.escape(f or "no family")}<span class="what"> {html.escape(what)}'
                        + (f'; finish: {html.escape(fin)}' if fin else '') + '</span></h2>')
        r, e = res[s], entries[s]
        n += 1
        orig, _ = pipeline_read(os.path.join(ex, e.file))
        lift = _lift_for(orig)
        imgs = [('original', _uri(_tile(orig, lift, size, True), 'PNG'))]
        p = os.path.join(pack, 'textures', s + '.png')
        if os.path.exists(p):
            a, _ = pipeline_read(p)
            imgs.append(('the pack', _uri(_tile(a, lift, size))))
        for which in ('close', 'wide'):
            for label, path in ((f'{which}: the pack', _shot(shots, views, s, which, False)),
                                (f'{which}: the original', _shot(shots, views, s, which, True))):
                if path:
                    im = Image.open(path).convert('RGB')
                    imgs.append((label, _uri(im.resize((size * 16 // 9, size), Image.LANCZOS))))
        v = pv.get(s, {})
        nums = []
        if v:
            nums.append(f"brightness: close x{v.get('close') or '?'}, wide x{v.get('wide') or '?'}"
                        f" (darkest fifth x{v.get('close_dark') or '?'}, x{v.get('wide_dark') or '?'})")
            if v.get('hit') != 'ok':
                nums.append(f"the view saw: {html.escape(v.get('hit', ''))}")
        vr = report.get(s)
        if vr:
            if vr.get('errors'):
                nums.append(f'<span class="fail">verify: {html.escape(vr["errors"])}</span>')
            if vr.get('notes'):
                nums.append(f'notes: {html.escape(vr["notes"])}')
        fl = flags.get(s)
        # the flags CSV says whose each flag is (Claude's notes, the Sonnet pre-review's): as written
        flag = ''.join(f'<div class="flag">{html.escape(x.strip())}</div>' for x in fl.split('||')) if fl else ''
        anim = (cen.get(s, {}).get('anim') or '').split()
        cls = ' flagged' if fl or (vr and vr.get('result') == 'FAIL') else ''
        body.append(
            f'<div class="card{cls}" data-stem="{html.escape(s)}">'
            f'<div class="q"><span class="n">{n}.</span> <b>{html.escape(s)}</b> {e.w}x{e.h} [{html.escape(r.tier)}, '
            f'{html.escape(r.cls)}]' + (f' an animation of {len(anim)} frames' if len(anim) > 1 else '')
            + f' <span class="lift">tiles x{lift:.1f} brighter</span></div>{flag}'
            f'<div class="imgs">' + ''.join(f'<figure><img src="{u}"><figcaption>{html.escape(c)}</figcaption></figure>' for c, u in imgs)
            + f'</div><div class="nums">{"<br>".join(nums)}</div>'
            f'<div class="verdict">' + ''.join(
                f'<label><input type="radio" name="v{n}" value="{k}">{t}</label>'
                for k, t in (('ok', 'ok'), ('seed', 'another seed'), ('candidates', 'candidates'), ('label', 'edit the label')))
            + f'</div><div class="fields">'
            f'<label>tier<input data-f="tier" value="{html.escape(r.tier)}"></label>'
            f'<label>regions<input data-f="regions" value="{html.escape(r.regions)}"></label></div>'
            f'<label>description<textarea data-f="description" rows="2">{html.escape(r.description)}</textarea></label>'
            f'<label>note<textarea data-f="note" rows="1" placeholder="what is wrong, what you want"></textarea></label></div>')
    text = (REVIEW_TEMPLATE.replace('@@BODY@@', ''.join(body)).replace('@@COUNT@@', str(n))
            .replace('@@TITLE@@', html.escape(title)))
    with open(out_path, 'w', encoding='utf-8') as fh:
        fh.write(text)
    print(f"texpack review: {n} textures -> {out_path} ({os.path.getsize(out_path) / 1e6:.1f} MB)")
    return n


def png_pages(ex, entries, res, cen, fams, cardlist, pack, shots, views, prefix, report=None, per=5, size=200):
    """Claude's pre-review (9.5): PNG pages of the hub's textures, `per` a page, a row each: the
    original and the pack tiled 2x2 (brightened alike), a corner of the pack's albedo at full size,
    and the original and the pack at the close and the wide view; beside each page a .txt with
    the labels, verify's result and the views' brightness. Returns the pages written."""
    from PIL import ImageDraw
    from heroes import _crop, _lift_for, _tile
    report = report or {}
    pv = load(os.path.join(shots, 'packviews.csv')) if shots else {}
    order = [s for _, s in cardlist]
    vw, vh = size * 16 // 9, size
    pages = []
    for i in range(0, len(order), per):
        chunk = order[i:i + per]
        img = Image.new('RGB', (3 * (size + 6) + 4 * (vw + 6) + 6, len(chunk) * (size + 22) + 4), (28, 28, 30))
        d = ImageDraw.Draw(img)
        lines = []
        for j, s in enumerate(chunk):
            r, e = res[s], entries[s]
            y = 4 + j * (size + 22)
            orig, _ = pipeline_read(os.path.join(ex, e.file))
            lift = _lift_for(orig)
            d.text((6, y), f"{i + j + 1}. {s}  [{r.tier}, {r.cls}, {r.family or 'no family'}]  tiles x{lift:.1f}", fill=(255, 220, 90))
            ims = [_tile(orig, lift, size, True)]
            p = os.path.join(pack, 'textures', s + '.png')
            if os.path.exists(p):
                a, _ = pipeline_read(p)
                ims += [_tile(a, lift, size), _crop(a, lift, size)]
            else:
                ims += [None, None]
            for which in ('close', 'wide'):
                for orig_shot in (True, False):
                    q = _shot(shots, views, s, which, orig_shot)
                    ims.append(Image.open(q).convert('RGB').resize((vw, vh), Image.LANCZOS) if q else None)
            x = 6
            for k, im in enumerate(ims):
                w = size if k < 3 else vw
                if im is not None:
                    img.paste(im, (x, y + 16))
                x += w + 6
            v, vr = pv.get(s, {}), report.get(s, {})
            lines.append(f"{i + j + 1}. {s} ({e.w}x{e.h}) tier {r.tier}, class {r.cls}, family {r.family or '-'}, regions {r.regions}\n"
                         f"   description: {r.description}\n"
                         + (f"   finish: {(fams.get(r.family) or {}).get('finish')}\n" if (fams.get(r.family) or {}).get('finish') else '')
                         + (f"   views: close x{v.get('close')}, wide x{v.get('wide')} (darkest fifth x{v.get('close_dark')}, x{v.get('wide_dark')})"
                            + (f", the view saw {v['hit']}" if v.get('hit') and v.get('hit') != 'ok' else '') + '\n' if v else '')
                         + (f"   verify: {vr.get('result')}" + (f": {vr['errors']}" if vr.get('errors') else '')
                            + (f"; notes: {vr['notes']}" if vr.get('notes') else '') + '\n' if vr else ''))
        path = f'{prefix}_{i // per + 1:02d}.png'
        img.save(path)
        with open(path[:-4] + '.txt', 'w', encoding='utf-8', newline='\n') as f:
            f.write('columns: original tiled | the pack tiled | the pack, a corner at full size | close: original | close: pack | '
                    'wide: original | wide: pack\n' + ''.join(lines))
        pages.append(path)
    return pages


def split(cardlist, part, parts):
    """Part `part` (1-based) of `parts` of a page's cards, families kept whole, about equal."""
    if parts <= 1:
        return cardlist
    groups_, cur = [], None
    for f, s in cardlist:
        if not groups_ or f != cur or not f:
            groups_.append([])
            cur = f
        groups_[-1].append((f, s))
    size = len(cardlist) / parts
    out, n = [[] for _ in range(parts)], 0
    for g in groups_:
        out[min(parts - 1, int(n / size))].extend(g)
        n += len(g)
    return out[part - 1]


REVIEW_SEED = 2000      # the review's "another seed": the name's with the bases after this (reseed's 1000-, heroes' 1-5)


def next_seed(rd, holder):
    """The set's next seed for the review's "another seed": after the one it has now among the
    review's seeds, the first otherwise."""
    import redraw
    import pipeline
    g = rd.plan.group[rd.plan.rep[holder]]
    base = redraw.anim_base(g[0])
    now = rd.seed_of(holder)
    k = next((k for k in range(1, 100) if pipeline.seed_for(base, rd.seed + REVIEW_SEED + k) == now), 0)
    return pipeline.seed_for(base, rd.seed + REVIEW_SEED + k + 1)


def apply(rd, rows, verdicts, pack, log=print):
    """The owner's verdicts (the page's CSV): `label` rows' edited fields into the manifest's rows as
    the owner's (returned for `merge`: a CSV of them is written), `seed` a new seed on the set's seed
    row, `candidates` into <pack>\\.texpack\\candidates.txt (for `heroes --stems @file`), the notes
    printed. Returns (label rows, changed): the caller writes the manifest when changed."""
    import reseed
    out_labels, cand, changed = [], [], False
    with open(verdicts, newline='', encoding='utf-8') as f:
        vs = list(csv.DictReader(f))
    counts = {}
    for v in vs:
        s, what = v['pattern'], v['verdict']
        counts[what] = counts.get(what, 0) + 1
        if v.get('note'):
            log(f"note {s} ({what}): {v['note']}")
        if what == 'label':
            edit = {k: v[k] for k in ('tier', 'regions', 'description') if v.get(k)}
            if edit and s in rd.res:
                # every label column given (merge writes the columns the CSV names: an empty cell
                # would blank the row's tier or regions, and the texture would drop out of the run);
                # its copies and frames too, labeled as it is (9.5: rtex423~994d's edit alone split
                # it from its copies, and their old drawing came back over it as a set)
                for t in reseed.twins(rd.plan, rd.census, s):
                    if t in rd.res:
                        r = rd.res[t]
                        out_labels.append(dict({'tier': r.tier, 'regions': r.regions, 'description': r.description}, pattern=t, **edit))
            elif edit:
                log(f"{s}: not a redrawn texture, its label left as it is")
        elif what == 'seed':
            if s not in rd.plan.rep:
                log(f"{s}: not a redrawn texture")
                continue
            if rd.root_of(rd.plan.rep[s]) != rd.plan.rep[s]:     # a crop or a whole's piece (M81)
                log(f"{s}: cut from {rd.root_of(rd.plan.rep[s])}'s drawing: its seed is that row's, left as it is")
                continue
            holder = reseed.seed_row(rd, rd.plan.group[rd.plan.rep[s]])
            if 'seed' in rd.res[holder].params and next((r for r in rows if r['pattern'] == holder and r['tier']), {}).get('source') == 'human':
                log(f"{s}: its set's seed is the owner's pick ({holder}'s): replaced by another, as asked")
            seed = next_seed(rd, holder)
            reseed.set_seeds(rows, rd.plan, rd.census, holder, seed)     # its copies and frames too
            changed = True
            log(f"{s}: seed {seed} on {holder}'s row")
        elif what == 'candidates':
            cand.append(s)
    if cand:
        os.makedirs(os.path.join(pack, '.texpack'), exist_ok=True)
        with open(os.path.join(pack, '.texpack', 'candidates.txt'), 'w', newline='\n') as f:
            f.write('\n'.join(cand) + '\n')
        log(f"candidates for {len(cand)}: texpack.py heroes --stems @{os.path.join(pack, '.texpack', 'candidates.txt')}")
    log('verdicts: ' + ', '.join(f'{n} {k}' for k, n in sorted(counts.items())))
    return out_labels, changed


def pipeline_read(path):
    import pipeline
    return pipeline.read_png(path)


REVIEW_TEMPLATE = r'''<!doctype html><html><head><meta charset="utf-8"><title>texpack review: @@TITLE@@</title><style>
body{font:14px system-ui,sans-serif;margin:0;background:#1b1b1d;color:#ddd}
header{position:sticky;top:0;background:#111;padding:8px 14px;z-index:5;display:flex;gap:14px;align-items:center;flex-wrap:wrap}
h2{margin:18px 14px 4px;color:#fff}.what{font-size:13px;color:#999;font-weight:normal}
.card{background:#26262a;border-radius:6px;padding:10px;margin:10px 14px}
.card.flagged{border-left:4px solid #c05040}.card.done{outline:2px solid #3a9a50}.card.redo{outline:2px solid #e0a030}
.imgs{display:flex;gap:6px;flex-wrap:wrap;align-items:flex-start}figure{margin:0}figcaption{font-size:11px;color:#999}
.q{color:#f0d070;font-size:15px}.q .n{color:#fff}.lift{font-size:12px;color:#999}
.flag{color:#ff9a80;margin:4px 0}.nums{font-size:12px;color:#aaa;margin:6px 0}.fail{color:#ff7060}
.verdict{display:flex;flex-wrap:wrap;gap:8px 20px;margin:8px 0;padding:6px 10px;background:#1f1f22;border-radius:4px}
.fields{display:flex;gap:8px;flex-wrap:wrap}
label{display:flex;flex-direction:column;font-size:12px;color:#999;margin-top:4px;flex:1;min-width:140px}
.verdict label{flex-direction:row;align-items:center;gap:6px;flex:none;min-width:0;margin:0;color:#ddd;font-size:13px;white-space:nowrap;cursor:pointer}
input[type=radio]{margin:0;width:16px;height:16px;accent-color:#3a9a50;cursor:pointer}
input:not([type=radio]),textarea{background:#1b1b1d;color:#ddd;border:1px solid #444;border-radius:3px;padding:4px;font:13px system-ui,sans-serif}
button{background:#3a6ea5;color:#fff;border:0;border-radius:4px;padding:6px 12px;cursor:pointer}
</style></head><body><header><b>texpack review: @@TITLE@@</b> @@COUNT@@ textures <span id="n">0 judged</span>
<button onclick="dl()">Download verdicts (CSV)</button><span style="color:#999;font-size:12px">red edge: verify failed or Claude flagged it</span></header>
@@BODY@@<script>
const cards=[...document.querySelectorAll('.card')];
const F=c=>[...c.querySelectorAll('[data-f]')];
// the verdicts and edits are kept in the browser (per page) as they are made: a reload keeps them
const KEY='texpack_review:'+document.title;
function save(){try{const st={};cards.forEach(c=>{const v=c.querySelector('input[type=radio]:checked');const f={};
 F(c).forEach(x=>{if(x.value!==x._v)f[x.dataset.f]=x.value});
 if(v||Object.keys(f).length)st[c.dataset.stem]={v:v?v.value:'',f:f}});localStorage.setItem(KEY,JSON.stringify(st))}catch(e){}}
function load(){try{const st=JSON.parse(localStorage.getItem(KEY)||'{}');cards.forEach(c=>{const s=st[c.dataset.stem];if(!s)return;
 if(s.v){const r=c.querySelector('input[type=radio][value="'+s.v+'"]');if(r)r.checked=true}
 F(c).forEach(x=>{if(s.f&&x.dataset.f in s.f)x.value=s.f[x.dataset.f]})})}catch(e){}}
function upd(){cards.forEach(c=>{const v=c.querySelector('input[type=radio]:checked');
 c.classList.toggle('done',!!v&&v.value==='ok');c.classList.toggle('redo',!!v&&v.value!=='ok')});
 document.getElementById('n').textContent=document.querySelectorAll('.card.done,.card.redo').length+' judged'}
cards.forEach(c=>{c.querySelectorAll('input[type=radio]').forEach(i=>i.addEventListener('change',()=>{upd();save()}));
 F(c).forEach(x=>{x._v=x.value;x.addEventListener('input',()=>{if(x.dataset.f!=='note'){
  const l=c.querySelector('input[value=label]');if(!l.checked){l.checked=true;upd()}}save()})})});
load();upd();
function q(s){return /[",\n]/.test(s)?'"'+s.replace(/"/g,'""')+'"':s}
function dl(){let t='pattern,verdict,tier,regions,description,note\n';
 cards.forEach(c=>{const v=c.querySelector('input[type=radio]:checked');if(!v)return;const f={};
  F(c).forEach(x=>f[x.dataset.f]=(x.dataset.f==='note'||x.value!==x._v)?x.value:'');
  t+=[c.dataset.stem,v.value,f.tier,f.regions,f.description,f.note].map(q).join(',')+'\n'});
 const a=document.createElement('a');a.href=URL.createObjectURL(new Blob([t],{type:'text/csv'}));a.download='texpack_review_'+document.title.replace(/^texpack review: /,'').replace(/[^A-Za-z0-9]+/g,'_')+'.csv';a.click()}
</script></body></html>'''
