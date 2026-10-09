"""texpack sheet (story 5.8): one self-contained HTML page of the selected
textures, grouped by map, kind or class, each with its class and description
(editable: a class list and a text field) and, with --pack, the result beside
the original. "Download edits" saves the changed rows as CSV; `texpack merge`
puts them into the manifest as human rows. Open the file in a browser; it
embeds the textures, so keep it local (they are Raven's). With --questions
(story 9.3) it is the owner's page: the textures asked about, with 9.2's shots,
the labels editable and an answer each.
"""
import base64
import html
import io
import json
import os

from PIL import Image

import pipeline

THUMB = 256


def _data_uri(path):
    im = Image.open(path).convert('RGBA')
    bg = Image.new('RGBA', im.size, (60, 60, 60, 255))
    im = Image.alpha_composite(bg, im).convert('RGB')
    w, h = im.size
    if max(w, h) > THUMB:
        s = THUMB / max(w, h)
        im = im.resize((max(1, round(w * s)), max(1, round(h * s))), Image.LANCZOS)
    b = io.BytesIO()
    im.save(b, 'PNG', optimize=True)
    return 'data:image/png;base64,' + base64.b64encode(b.getvalue()).decode()


def montage(ex, sel, resolved, out_prefix, cols=8, rows=5, cell=128):
    """PNG pages of the textures, cols x rows to a page, each with its stem, class and the start
    of its description under it (yellow-green: a human row, blue: a draft or the default):
    the quickest way to review labels by eye, a few dozen at a glance (the sheet's HTML is for
    editing). Writes <out_prefix>_01.png and on."""
    from PIL import ImageDraw
    per = cols * rows
    pages = (len(sel) + per - 1) // per
    for p in range(pages):
        img = Image.new('RGB', (cols * (cell + 8), rows * (cell + 34)), (24, 24, 24))
        d = ImageDraw.Draw(img)
        for i, e in enumerate(sel[p * per:(p + 1) * per]):
            x, y = (i % cols) * (cell + 8), (i // cols) * (cell + 34)
            t = Image.open(os.path.join(ex, e.file)).convert('RGBA')
            t = Image.alpha_composite(Image.new('RGBA', t.size, (70, 70, 70, 255)), t).convert('RGB')
            s = min(cell / t.width, cell / t.height)
            t = t.resize((max(1, round(t.width * s)), max(1, round(t.height * s))), Image.NEAREST if s >= 1 else Image.LANCZOS)
            img.paste(t, (x + 4, y + 30))
            r = resolved[e.stem]
            human = bool(r.rows) and r.rows[-1]['source'] != 'draft'
            d.text((x + 4, y + 2), e.stem[:22], fill=(255, 255, 0))
            d.text((x + 4, y + 14), f'{r.cls}: {r.description[:18]}', fill=(130, 255, 150) if human else (120, 220, 255))
        img.save(f'{out_prefix}_{p + 1:02d}.png')
    print(f"texpack montage: {len(sel)} textures, {pages} pages -> {out_prefix}_NN.png")
    return 0


def _group(e, key, res):
    if key == 'map':
        return e.used_in[0] if e.used_in else e.kind
    if key == 'class':
        return res.cls
    return e.kind


def run(ex, sel, resolved, classes, out, pack=None, group='map'):
    names = sorted(classes['class'])
    groups = {}
    for e in sel:
        groups.setdefault(_group(e, group, resolved[e.stem]), []).append(e)
    parts = []
    for g in sorted(groups):
        cards = []
        for e in groups[g]:
            r = resolved[e.stem]
            human = r.rows[-1]['source'] if r.rows else 'default'
            imgs = f'<img src="{_data_uri(os.path.join(ex, e.file))}" title="original {e.w}x{e.h}" style="width:{max(e.w, 96) * 2 if e.w < 128 else e.w}px">'
            if pack:
                p = os.path.join(pack, 'textures', e.stem + '.png')
                if os.path.exists(p):
                    imgs += f'<img src="{_data_uri(p)}" title="result" style="width:{max(e.w, 96) * 2 if e.w < 128 else e.w}px">'
            opts = ''.join(f'<option{" selected" if n == r.cls else ""}>{n}</option>' for n in names)
            cards.append(
                f'<div class="card" data-stem="{html.escape(e.stem)}"">'
                f'<div class="imgs">{imgs}</div>'
                f'<div class="meta"><b>{html.escape(e.stem)}</b> <span>{e.w}x{e.h} {e.kind}{" · " + e.alpha if e.alpha != "none" else ""}</span>'
                f'<span class="src {human}">{human}</span></div>'
                f'<select>{opts}</select>'
                f'<input type="text" value="{html.escape(r.description)}" placeholder="what is it?">'
                f'<div class="used">{html.escape(" ".join(e.used_in[:8]))}</div></div>')
        parts.append(f'<h2>{html.escape(g)} <small>{len(cards)}</small></h2><div class="grid">{"".join(cards)}</div>')
    page = TEMPLATE.replace('@@BODY@@', ''.join(parts)).replace('@@COUNT@@', str(len(sel)))
    with open(out, 'w', encoding='utf-8') as f:
        f.write(page)
    print(f"texpack sheet: {len(sel)} textures in {len(groups)} groups -> {out} ({os.path.getsize(out) / 1e6:.1f} MB)")
    return 0


TEMPLATE = r'''<!doctype html><html><head><meta charset="utf-8"><title>texpack sheet</title><style>
body{font:13px system-ui,sans-serif;margin:0;background:#1b1b1d;color:#ddd}
header{position:sticky;top:0;background:#111;padding:8px 14px;z-index:5;display:flex;gap:14px;align-items:center}
h2{margin:18px 14px 6px;font-size:15px}h2 small{color:#888}
.grid{display:flex;flex-wrap:wrap;gap:10px;padding:0 14px}
.card{background:#26262a;border-radius:6px;padding:8px;max-width:560px}
.card.changed{outline:2px solid #e0a030}
.imgs{display:flex;gap:6px;align-items:flex-start}.imgs img{image-rendering:pixelated;max-width:260px;height:auto}
.meta{margin:6px 0 4px}.meta span{color:#999;margin-left:6px}
.src{padding:0 5px;border-radius:3px;font-size:11px}.src.draft{background:#4a3b12;color:#eac66a}.src.human{background:#14412a;color:#7fd9a2}.src.default{background:#333;color:#999}
select,input{width:100%;box-sizing:border-box;margin-top:3px;background:#1b1b1d;color:#ddd;border:1px solid #444;border-radius:3px;padding:3px}
.used{color:#777;font-size:11px;margin-top:4px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
button{background:#3a6ea5;color:#fff;border:0;border-radius:4px;padding:6px 12px;cursor:pointer}
</style></head><body><header><b>texpack sheet</b> @@COUNT@@ textures <span id="n">0 edited</span>
<button onclick="dl()">Download edits (CSV)</button></header>@@BODY@@<script>
const cards=[...document.querySelectorAll('.card')];
cards.forEach(c=>{c._cls=c.querySelector('select').value;c._desc=c.querySelector('input').value;
 const f=()=>{const ch=c.querySelector('select').value!==c._cls||c.querySelector('input').value!==c._desc;
  c.classList.toggle('changed',ch);document.getElementById('n').textContent=document.querySelectorAll('.changed').length+' edited'};
 c.querySelector('select').onchange=f;c.querySelector('input').oninput=f});
function q(s){return /[",\n]/.test(s)?'"'+s.replace(/"/g,'""')+'"':s}
function dl(){let t='pattern,class,description,source\n';
 document.querySelectorAll('.changed').forEach(c=>{t+=[c.dataset.stem,c.querySelector('select').value,c.querySelector('input').value,'human'].map(q).join(',')+'\n'});
 const a=document.createElement('a');a.href=URL.createObjectURL(new Blob([t],{type:'text/csv'}));a.download='texpack_edits.csv';a.click()}
</script></body></html>'''


def _jpeg_uri(path, width=420):
    im = Image.open(path).convert('RGB')
    if im.width > width:
        im = im.resize((width, round(im.height * width / im.width)), Image.LANCZOS)
    b = io.BytesIO()
    im.save(b, 'JPEG', quality=85)
    return 'data:image/jpeg;base64,' + base64.b64encode(b.getvalue()).decode()


def questions(ex, sel, resolved, classes, asked, views, out):
    """The owner's page (story 9.3): per texture with a question, the original and 9.2's close,
    wide and albedo shots, the question, the labels as editable fields and an answer. "Download
    answers" saves every row answered or changed (the manifest's columns and `answer`);
    `texpack merge` takes the fields as the owner's (source human), the answers are read by hand."""
    lab = classes.get('labels', {})
    purposes = sorted(set(lab.get('purposes_world', [])) | set(lab.get('purposes_skin', [])))
    tiers, names = lab.get('tiers', []), sorted(classes['class'])

    def sel_html(field, value, opts):
        # an empty or unknown value stays as it is (a select would show its first option)
        opts = [''] + [n for n in opts if n] + ([value] if value and value not in opts else [])
        o = ''.join(f'<option{" selected" if n == value else ""}>{html.escape(n)}</option>' for n in opts)
        return f'<label>{field}<select data-f="{field}">{o}</select></label>'

    cards = []
    for e in sel:
        r = resolved[e.stem]
        imgs = f'<img class="orig" src="{_data_uri(os.path.join(ex, e.file))}" title="original {e.w}x{e.h}">'
        for v in ('close', 'wide', 'albedo'):
            p = os.path.join(views, f'{e.stem}_{v}.png')
            if os.path.exists(p):
                imgs += f'<img src="{_jpeg_uri(p)}" title="{v}">'
        cards.append(
            f'<div class="card" data-stem="{html.escape(e.stem)}"><div class="imgs">{imgs}</div>'
            f'<div class="q"><b>{html.escape(e.stem)}</b> {e.w}x{e.h}: {html.escape(asked[e.stem])}</div>'
            f'<div class="fields">{sel_html("purpose", r.purpose, purposes)}{sel_html("tier", r.tier, tiers)}'
            f'{sel_html("class", r.cls, names)}'
            f'<label>family<input data-f="family" value="{html.escape(r.family)}"></label>'
            f'<label>regions<input data-f="regions" value="{html.escape(r.regions)}"></label></div>'
            f'<label>description<textarea data-f="description" rows="2">{html.escape(r.description)}</textarea></label>'
            f'<label>answer<textarea data-f="answer" rows="2" placeholder="your answer"></textarea></label></div>')
    page = Q_TEMPLATE.replace('@@BODY@@', ''.join(cards)).replace('@@COUNT@@', str(len(sel)))
    with open(out, 'w', encoding='utf-8') as f:
        f.write(page)
    print(f"texpack sheet: {len(sel)} questions -> {out} ({os.path.getsize(out) / 1e6:.1f} MB)")
    return 0


Q_TEMPLATE = r'''<!doctype html><html><head><meta charset="utf-8"><title>texpack questions</title><style>
body{font:14px system-ui,sans-serif;margin:0;background:#1b1b1d;color:#ddd}
header{position:sticky;top:0;background:#111;padding:8px 14px;z-index:5;display:flex;gap:14px;align-items:center}
.card{background:#26262a;border-radius:6px;padding:10px;margin:12px 14px}
.card.changed{outline:2px solid #e0a030}
.imgs{display:flex;gap:6px;align-items:flex-start;flex-wrap:wrap}.imgs img{max-width:420px;height:auto}
.imgs img.orig{image-rendering:pixelated;width:192px}
.q{margin:8px 0;color:#f0d070}.q b{color:#fff}
.fields{display:flex;gap:8px;flex-wrap:wrap}
label{display:flex;flex-direction:column;font-size:12px;color:#999;margin-top:4px;flex:1;min-width:140px}
select,input,textarea{background:#1b1b1d;color:#ddd;border:1px solid #444;border-radius:3px;padding:4px;font:13px system-ui,sans-serif}
button{background:#3a6ea5;color:#fff;border:0;border-radius:4px;padding:6px 12px;cursor:pointer}
</style></head><body><header><b>texpack questions</b> @@COUNT@@ textures <span id="n">0 answered or edited</span>
<button onclick="dl()">Download answers (CSV)</button></header>@@BODY@@<script>
const cards=[...document.querySelectorAll('.card')];
const F=c=>[...c.querySelectorAll('[data-f]')];
cards.forEach(c=>{F(c).forEach(x=>{x._v=x.value;x.addEventListener('input',()=>{
 c.classList.toggle('changed',F(c).some(y=>y.value!==y._v));
 document.getElementById('n').textContent=document.querySelectorAll('.changed').length+' answered or edited'})})});
function q(s){return /[",\n]/.test(s)?'"'+s.replace(/"/g,'""')+'"':s}
function dl(){const cols=['purpose','tier','class','family','regions','description','answer'];
 let t='pattern,'+cols.join(',')+',source\n';
 document.querySelectorAll('.changed').forEach(c=>{const v={};F(c).forEach(x=>v[x.dataset.f]=x.value);
  t+=[c.dataset.stem,...cols.map(k=>v[k]),'human'].map(q).join(',')+'\n'});
 const a=document.createElement('a');a.href=URL.createObjectURL(new Blob([t],{type:'text/csv'}));a.download='texpack_answers.csv';a.click()}
</script></body></html>'''
