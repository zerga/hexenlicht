"""texpack's heroes (story 9.5; DECISIONS M72): each family's look picked by the owner from
candidates.

A family's lead (families.csv) is the texture redrawn first, its drawing the reference for the
family's layout members (M70). 9.5's spike found that the seed changes only a drawing's detail
(six seeds of a hero: the same material and finish, its layout member drawn with each almost the
same), while words about the finish in the description change its character and carry over to
the members that get them. So a family's candidates are finishes: the label as it is, a rougher
and a finer finish (`--finishes`, a CSV of family, rougher, finer, written per family from
STYLE.md), each with two seeds (the first the one a run uses now); each drawn through every stage
as a run makes it, the lead and the family's largest layout member that takes it as its
reference, both with the candidate's finish. Candidate k is a pack of its own, `<out>\\c<k>`,
whose textures\\ hold their files as each drew itself (no set unified), so `views_run.ps1 -Pack`
can shoot them in the game; `<out>\\heroes.json` names the families, leads, members and each
candidate's finish and seed. `page` writes the owner's page, `apply` takes its picks: the
family's `finish` into families.csv (the redraw adds it to every reimagine and layout member's
description) and the lead's `seed=` into the manifest as the owner's. Single textures (the
review's `candidates`) get seeds only.

  texpack.py heroes --out <dir> --finishes <csv> [--family a,b | --hub blackmarsh]
  texpack.py heroes --out <dir> --page heroes.html [--shots <dir>]
  texpack.py heroes --out <dir> --apply texpack_heroes.csv
"""
import csv
import html
import json
import os

import numpy as np
from PIL import Image

import groups
import pipeline
from imaging import _enc, _lin
from pipeline import LUM

SEEDS = 6               # candidates of a single texture (seeds only)
FINISH_SEEDS = 2        # seeds of each finish


def families(plan, res, fams, census, only=None, hubs=None):
    """[(family, hub, lead, member)] for each family whose lead is redrawn as a hero: the lead
    (its copy's source where it has one) and its layout member with the most area that takes
    it as its reference (None where it has none). only: family names; hubs: families.csv's hubs."""
    out = []
    for f, row in sorted(fams.items()):
        lead = (row.get('lead') or '').strip()
        if not lead or lead not in plan.rep or (only and f not in only) or (hubs and row.get('hub') not in hubs):
            continue
        lead = plan.rep[lead]
        if res[lead].tier not in groups.SET_TIERS:
            continue
        ms = [s for s, h in plan.hero.items() if h == lead and plan.rep.get(s) == s]
        member = min(ms, key=lambda s: (-groups._area(census, s), s)) if ms else None
        out.append((f, row.get('hub', ''), lead, member))
    return out


def textures(plan, res, cen, stems):
    """[(stem, hub, stem, None)]: candidates for single textures (the review's verdict
    `candidates`), each a "family" of its own on the page, seeds only."""
    import review
    out = []
    for s in stems:
        r = plan.rep.get(s)
        while r in plan.crop:       # a crop's drawing is its source's (M81)
            r = plan.crop[r][0]
        if r is None or res[r].tier not in groups.SET_TIERS + ('glass', 'liquid', 'lava'):
            continue
        out.append((r, review.page_hub(cen, r), r, None))
    return sorted(set(out))


def seeds_for(rd, lead, n):
    """n seeds: the one a run uses now, then the name's with the next bases. A crop or a whole's
    piece is drawn from its source's drawing (M81): its source's seed."""
    import redraw
    rd.seeds = {}
    root = rd.root_of(lead)
    out = [rd.seed_of(root)]
    base, k = redraw.anim_base(root if root in rd.wholes else rd.plan.group[root][0]), 1
    while len(out) < n:
        s = pipeline.seed_for(base, rd.seed + k)
        k += 1
        if s not in out:
            out.append(s)
    return out


def load_finishes(path):
    """family -> [(name, words)]: the candidates' finishes (a CSV of family, rougher, finer)."""
    if not path:
        return {}
    with open(path, newline='', encoding='utf-8') as f:
        return {r['family']: [('rougher', r['rougher'].strip()), ('finer', r['finer'].strip())]
                for r in csv.DictReader(l for l in f if not l.startswith('# ')) if r.get('family')}


def candidates(rd, family, lead, fins):
    """[{'name', 'finish', 'seed'}]: the label as it is and each finish, FINISH_SEEDS seeds each;
    without finishes SEEDS seeds of the label."""
    if not fins:
        return [{'name': 'as labeled', 'finish': '', 'seed': s} for s in seeds_for(rd, lead, SEEDS)]
    seeds = seeds_for(rd, lead, FINISH_SEEDS)
    return [{'name': n, 'finish': w, 'seed': s} for n, w in [('as labeled', '')] + list(fins) for s in seeds]


def draw(rd, out, items, finishes=None, force=False, log=print, pack=None):
    """Each family's candidates into <out>\\c1 .. c<n>; heroes.json written (and kept: a later
    call for other families adds to it). finishes: family -> [(name, words)]; pack: where a
    single texture's hero was drawn (its reference). The Redraw's pack, seeds and finishes are
    as they were after it."""
    path = os.path.join(out, 'heroes.json')
    info = {}
    if os.path.exists(path):
        with open(path) as f:
            info = json.load(f)
    base = dict(rd.finishes)
    for f, hub, lead, member in items:
        info[f] = {'hub': hub, 'lead': lead, 'member': member, 'cands': candidates(rd, f, lead, (finishes or {}).get(f))}
    os.makedirs(out, exist_ok=True)
    with open(path, 'w', newline='\n') as fh:
        json.dump(info, fh, indent=1, sort_keys=True)
    n = max(len(info[f]['cands']) for f, _, _, _ in items) if items else 0
    total = sum(len(info[f]['cands']) * (2 if m else 1) for f, _, _, m in items)
    i = 0
    home = rd.out
    try:
        for k in range(n):
            ck = os.path.join(out, f'c{k + 1}')
            rd.open(ck)
            for f, hub, lead, member in items:
                cands = info[f]['cands']
                if k >= len(cands):
                    continue
                c = cands[k]
                rd.seeds = {rd.root_of(lead): c['seed']}    # a part's: its source's (the code review's)
                rd.finishes = dict(base, **{f: c['finish']}) if c['finish'] else {x: v for x, v in base.items() if x != f}
                for s in [lead] + ([member] if member else []):
                    i += 1
                    h = rd.plan.hero.get(s)
                    if h and h != lead:
                        # a single texture's hero isn't drawn here: its drawing as the pack has it (the code review's)
                        src = os.path.join(pack, 'work', rd.root_of(h) + '_A.png') if pack else None
                        if not src or not os.path.exists(src):
                            log(f"[{i}/{total}] c{k + 1} {s}: its hero {h} has no drawing" + (f" in {pack}" if pack else ' (--pack)') + ': skipped')
                            continue
                        rd._copy(src, os.path.join(ck, 'work', rd.root_of(h) + '_A.png'))
                    r = rd.draw(s, force)
                    rd.write_alone(s)
                    what = 'unchanged' if r.get('skipped') else f"layout {r['layout']}, seam {r['seam']}, {r['seconds']} s"
                    log(f"[{i}/{total}] c{k + 1} {f}: {s}{' (hero ' + lead + ')' if s != lead else ''}, {c['name']}, "
                        f"seed {rd.seed_of(s)}: {what}")
    finally:
        rd.seeds, rd.finishes = {}, base
        rd.open(home)
    return info


# -- the owner's page --------------------------------------------------------------------------

def _lift_for(orig_rgb):
    """The display's brightening, the same for the original and every candidate of a family: its
    original's mean linear luminance to 0.18 (Hexen II's albedos are about 0.04), 1 to 8 times."""
    m = float((_lin(orig_rgb[..., :3]) @ LUM).mean())
    return float(np.clip(0.18 / max(m, 1e-4), 1.0, 8.0))


def _tile(rgb, lift, size, nearest=False):
    """The texture tiled 2x2, brightened in linear light, fitted into size x size."""
    t = np.tile(rgb[..., :3], (2, 2, 1))
    if lift != 1.0:
        t = _enc(_lin(t) * lift)
    im = Image.fromarray(t)
    s = size / max(im.width, im.height)
    return im.resize((max(1, round(im.width * s)), max(1, round(im.height * s))), Image.NEAREST if nearest else Image.LANCZOS)


def _crop(rgb, lift, size):
    """A corner of the texture at its own resolution (the 4x one's detail), brightened."""
    c = rgb[:min(size, rgb.shape[0]), :min(size, rgb.shape[1]), :3]
    return Image.fromarray(_enc(_lin(c) * lift) if lift != 1.0 else c)


def _uri(im, fmt='JPEG'):
    import base64
    import io
    b = io.BytesIO()
    im.convert('RGB').save(b, fmt, **({'quality': 88} if fmt == 'JPEG' else {}))
    return f"data:image/{fmt.lower()};base64," + base64.b64encode(b.getvalue()).decode()


def _shot(shots, k, stem, size):
    """Candidate k's close view in the game (views_run.ps1 -Pack's run, read by `packviews`)."""
    if not shots:
        return None
    p = os.path.join(shots, f'c{k}', f"{stem}_close.png")
    if not os.path.exists(p):
        return None
    im = Image.open(p).convert('RGB')
    return im.resize((size, round(im.height * size / im.width)), Image.LANCZOS)


def _numbers(out_dir, stem, k, flat=False):
    """A candidate's layout kept and seam (its redraw.json), and what `verify` would fail it for:
    moved shapes (a layout, faithful, liquid or lava texture under 0.3, unless its original is
    flat: verify.flat) or a seam where the original tiles (over 2 and twice the original's)."""
    import verify
    p = os.path.join(out_dir, f'c{k}', '.texpack', 'redraw.json')
    if not os.path.exists(p):
        return ''
    with open(p) as f:
        st = json.load(f).get(stem)
    if not st or st.get('layout') is None:
        return ''
    bad = []
    if st.get('tier') in verify.SHAPE_TIERS and st['layout'] < verify.SHAPES and not flat:
        bad.append('shapes moved')
    # a picture that doesn't repeat (`tile=0`, a crop, a piece) has no seam check (verify's)
    if st.get('repeats', True) and st.get('seam_orig', 9) <= 2.0 and st.get('seam', 0) > max(2.0, 2 * st['seam_orig']):
        bad.append('seam')
    text = f"layout {st['layout']:.2f}, seam {st.get('seam', 0):.2f}" + (f" ({st['seam_orig']:.2f})" if 'seam_orig' in st else '')
    return f'<div class="num{" bad" if bad else ""}">{text}{"; verify fails: " + ", ".join(bad) if bad else ""}</div>'


def _cells(out_dir, stem, k, lift, size, shots, cap, flat=False):
    p = os.path.join(out_dir, f'c{k}', 'work', stem + '.png')
    cell = f'<div class="cell">{cap}{_numbers(out_dir, stem, k, flat)}'
    if os.path.exists(p):
        a, _ = pipeline.read_png(p)
        cell += f'<img src="{_uri(_tile(a, lift, size))}"><img src="{_uri(_crop(a, lift, size))}" title="a corner at full size">'
    sh = _shot(shots, k, stem, size)
    if sh is not None:
        cell += f'<img src="{_uri(sh)}" title="in the game">'
    return cell + '</div>'


def page(ex, entries, res, info, out_dir, page_path, views=None, shots=None, size=200):
    """The owner's page: a section per hub, per family the original lead (and its 9.2 close view),
    each candidate's albedo tiled 2x2 and a corner at full size (brightened alike: the factor is
    shown), its layout kept and seam (red where `verify` would fail it), its close view in the
    game where `shots` has it, and the layout member as each candidate makes it. A pick per family, or none with a note; "Download picks" saves a CSV
    (family, lead, finish, seed, note; a "none" without finish and seed) for `heroes --apply`."""
    import review
    import verify
    order = review.HUB_ORDER
    by_hub = {}
    for f, d in info.items():
        by_hub.setdefault(d['hub'], []).append(f)
    hubs = sorted(by_hub, key=lambda h: (order.index(h) if h in order else len(order), h))
    body, idx = [], 0
    for hub in hubs:
        body.append(f'<h2 id="{html.escape(hub)}">{html.escape(hub)} ({len(by_hub[hub])})</h2>')
        for f in sorted(by_hub[hub]):
            d = info[f]
            idx += 1
            lead, member, cands = d['lead'], d['member'], d['cands']
            orig, orig_a = pipeline.read_png(os.path.join(ex, entries[lead].file))
            lift = _lift_for(orig)
            flat_l = verify.flat(orig, orig_a)
            r = res[lead]
            row = [f'<div class="cell"><div class="cap">original {entries[lead].w}x{entries[lead].h}</div>'
                   f'<img src="{_uri(_tile(orig, lift, size, True), "PNG")}">']
            v = os.path.join(views, f'{lead}_close.png') if views else None
            if v and os.path.exists(v):
                im = Image.open(v).convert('RGB')
                row.append(f'<img src="{_uri(im.resize((size, round(im.height * size / im.width)), Image.LANCZOS))}" title="9.2: the original in the game">')
            row.append('</div>')
            mrow, mlift, flat_m = [], 1.0, False
            if member:
                mo, mo_a = pipeline.read_png(os.path.join(ex, entries[member].file))
                mlift = _lift_for(mo)
                flat_m = verify.flat(mo, mo_a)
                mrow.append(f'<div class="cell"><div class="cap">{html.escape(member)} original</div>'
                            f'<img src="{_uri(_tile(mo, mlift, size, True), "PNG")}"></div>')
            for k, c in enumerate(cands, 1):
                cap = (f'<label class="cap"><input type="radio" name="{html.escape(f)}" value="{k}">'
                       f'{k}. {html.escape(c["name"])}, seed {c["seed"]}</label>')
                row.append(_cells(out_dir, lead, k, lift, size, shots, cap, flat_l))
                if member:
                    mrow.append(_cells(out_dir, member, k, mlift, size, shots, f'<div class="cap">{k}. {html.escape(c["name"])}</div>', flat_m))
            fins = ''.join(f'<div class="fin"><b>{html.escape(n)}</b>: {html.escape(w)}</div>'
                           for n, w in dict((c['name'], c['finish']) for c in cands if c['finish']).items())
            data = html.escape(json.dumps([[c['finish'], c['seed']] for c in cands]))
            body.append(
                f'<div class="fam" data-f="{html.escape(f)}" data-lead="{html.escape(lead)}" data-c="{data}">'
                f'<div class="q"><span class="n">{idx}.</span> <b>{html.escape(f)}</b>: hero {html.escape(lead)}'
                f' [{html.escape(r.tier)}, {html.escape(r.cls)}] <span class="lift">shown x{lift:.1f} brighter</span>'
                f'<div class="desc">{html.escape(r.description)}</div>{fins}</div>'
                f'<div class="row">{"".join(row)}</div>'
                + (f'<div class="sub">its layout member {html.escape(member)}, drawn with each candidate as its reference'
                   f' and the same finish (x{mlift:.1f})</div><div class="row">{"".join(mrow)}</div>' if member else '')
                + f'<div class="pick"><label><input type="radio" name="{html.escape(f)}" value="none">none of them</label>'
                f'<textarea placeholder="a note: what to change" rows="1"></textarea></div></div>')
    toc = ' '.join(f'<a href="#{html.escape(h)}">{html.escape(h)}</a>' for h in hubs)
    text = HEROES_TEMPLATE.replace('@@BODY@@', ''.join(body)).replace('@@COUNT@@', str(len(info))).replace('@@TOC@@', toc)
    with open(page_path, 'w', encoding='utf-8') as fh:
        fh.write(text)
    print(f"texpack heroes: {len(info)} families -> {page_path} ({os.path.getsize(page_path) / 1e6:.1f} MB)")
    return 0


def apply(picks, families_path, rows, plan=None, log=print, census=None):
    """The page's picks: each family's finish into families.csv, the lead's seed onto its labeled
    row as the owner's (source human; its other overrides kept), and off the other members of
    the lead's set (plan: groups.Plan; a set takes the first `seed=` in its order, which would hide
    the pick); the lead's copies and animation frames take the seed too (census: their `anim`;
    copies and frames are labeled alike). Two families whose leads share a set share its seed: where their picks' seeds
    differ, the pick of the lead first in the set's order sets it (the other family keeps its
    finish). "none" rows' notes printed. Returns the rows changed (the caller writes the
    manifest)."""
    import labels
    import manifest
    import reseed
    with open(picks, newline='', encoding='utf-8') as f:
        ps = list(csv.DictReader(f))
    if plan is not None:
        first = {}
        for p in ps:
            if p.get('seed') and p['lead'] in plan.rep:
                g = plan.group[plan.rep[p['lead']]]
                first.setdefault(g[0], []).append((g.index(plan.rep[p['lead']]), p))
        for g0, members in first.items():
            members.sort(key=lambda t: t[0])
            win = members[0][1]
            for _, p in members[1:]:
                if p['seed'] != win['seed']:
                    log(f"{p['family']}: its lead {p['lead']} shares a set with {win['lead']} ({win['family']}): "
                        f"the set's seed is {win['family']}'s pick {win['seed']}, not {p['seed']}")
                    p['seed'] = win['seed']
    fins, changed = {}, []
    for p in ps:
        if p.get('note'):
            log(f"note {p['family']} ({'none' if not p.get('seed') else 'picked'}): {p['note']}")
        if not p.get('seed'):
            continue
        fins[p['family']] = p.get('finish', '')
        if labels.labeled_row(p['lead'], rows) is None:
            log(f"{p['family']}: no labeled row of {p['lead']}")
            continue
        if plan is None or p['lead'] not in plan.rep:
            if plan is not None:
                log(f"{p['family']}: {p['lead']} isn't redrawn now: its seed set on its row only")
            row = labels.labeled_row(p['lead'], rows)
            reseed.set_seed(row, int(p['seed']))
            row['source'] = 'human'
            changed.append(row)
            continue
        r = plan.rep[p['lead']]
        if r in getattr(plan, 'whole', {}) or r in getattr(plan, 'crop', {}):
            # a whole's piece or a crop is cut from its picture's or source's drawing, whose row's
            # seed the run takes (M81; the code review's: on the piece's row it did nothing)
            src = plan.whole[r][0] if r in plan.whole else plan.crop[r][0]
            if r in plan.whole:
                row = labels.labeled_row(src, rows)
                reseed.set_seed(row, int(p['seed']))
                row['source'] = 'human'
                changed.append(row)
            else:
                changed += reseed.set_seeds(rows, plan, census, src, int(p['seed']), 'human')
            log(f"{p['family']}: its lead {p['lead']} is cut from {src}'s drawing: the seed on {src}'s row")
            continue
        # the lead and its copies and frames (labeled alike), the owner's
        changed += reseed.set_seeds(rows, plan, census, p['lead'], int(p['seed']), 'human')
        mine = set(reseed.twins(plan, census, p['lead']))
        for s in plan.group[plan.rep.get(p['lead'], p['lead'])]:
            other = labels.labeled_row(s, rows) if s not in mine else None
            over = manifest.parse_overrides(other.get('overrides', '')) if other else {}
            if 'seed' in over and int(over['seed']) != int(p['seed']):
                changed += reseed.set_seeds(rows, plan, census, s, None)
                log(f"{p['family']}: {s}'s seed taken off (the set takes the pick's)")
    done = labels.set_finishes(families_path, fins)
    log(f"heroes: {len(fins)} picks, {len(done)} families' finishes changed, {len(changed)} rows changed")
    return changed


HEROES_TEMPLATE = r'''<!doctype html><html><head><meta charset="utf-8"><title>texpack heroes</title><style>
body{font:14px system-ui,sans-serif;margin:0;background:#1b1b1d;color:#ddd}
header{position:sticky;top:0;background:#111;padding:8px 14px;z-index:5;display:flex;gap:14px;align-items:center;flex-wrap:wrap}
header a{color:#8bf}
h2{margin:18px 14px 4px;color:#fff}
.fam{background:#26262a;border-radius:6px;padding:10px;margin:12px 14px}
.fam.picked{outline:2px solid #3a9a50}.fam.none{outline:2px solid #e0a030}
.row{display:flex;gap:6px;flex-wrap:wrap;align-items:flex-start;margin-top:6px}
.cell{display:flex;flex-direction:column;gap:3px}.cell img{display:block}
.cap{font-size:12px;color:#bbb;display:flex;gap:4px;align-items:center}
.num{font-size:11px;color:#888}.num.bad{color:#ff7060}
.q{color:#f0d070;font-size:16px}.q .n{color:#fff}.lift{font-size:12px;color:#999}
.desc{font-size:12px;color:#aaa;margin-top:2px}.fin{font-size:12px;color:#9cf}.sub{font-size:12px;color:#9c9;margin-top:8px}
.pick{display:flex;gap:10px;align-items:center;margin-top:8px}.pick textarea{flex:1}
textarea{background:#1b1b1d;color:#ddd;border:1px solid #444;border-radius:3px;padding:4px;font:13px system-ui,sans-serif}
button{background:#3a6ea5;color:#fff;border:0;border-radius:4px;padding:6px 12px;cursor:pointer}
</style></head><body><header><b>texpack heroes</b> @@COUNT@@ families <span id="n">0 picked</span>
<button onclick="dl()">Download picks (CSV)</button> @@TOC@@</header>@@BODY@@<script>
const fams=[...document.querySelectorAll('.fam')];
function upd(){fams.forEach(f=>{const c=f.querySelector('input:checked');
 f.classList.toggle('picked',!!c&&c.value!=='none');f.classList.toggle('none',!!c&&c.value==='none')});
 document.getElementById('n').textContent=document.querySelectorAll('.fam.picked,.fam.none').length+' picked'}
fams.forEach(f=>f.querySelectorAll('input').forEach(i=>i.addEventListener('change',upd)));
function q(s){s=String(s);return /[",\n]/.test(s)?'"'+s.replace(/"/g,'""')+'"':s}
function dl(){let t='family,lead,finish,seed,note\n';
 fams.forEach(f=>{const c=f.querySelector('input:checked');if(!c)return;
  const note=f.querySelector('textarea').value;
  if(c.value==='none'){t+=[f.dataset.f,f.dataset.lead,'','',note||'none'].map(q).join(',')+'\n';return}
  const [fin,seed]=JSON.parse(f.dataset.c)[+c.value-1];
  t+=[f.dataset.f,f.dataset.lead,fin,seed,note].map(q).join(',')+'\n'});
 const a=document.createElement('a');a.href=URL.createObjectURL(new Blob([t],{type:'text/csv'}));a.download='texpack_heroes.csv';a.click()}
</script></body></html>'''
