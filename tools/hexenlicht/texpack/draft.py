"""texpack draft (story 5.8): drafts manifest rows with a local vision-language
model (Qwen3-VL-4B-Instruct, Apache-2.0, in $TEXPACK_HOME\\models). One row per
texture: class and a short description. Human rows are never touched; draft
rows are redone only with --redo. The result is a starting point: review it
with `texpack sheet` and correct it.
"""
import fnmatch
import json
import os
import re

import numpy as np
from PIL import Image

import manifest
import pipeline

CLASSES_BY_KIND = {
    'world': ['stone', 'brick', 'plaster', 'wood', 'metal', 'rust', 'cloth', 'earth', 'foliage', 'ice', 'bone', 'glass', 'rune', 'world'],
    'liquid': ['liquid', 'lava'],
    'skin': ['skin', 'skin_metal'],
    'sprite': ['sprite'],
}

PROMPT = (
    "This is a small texture from the 1997 video game Hexen II (a dark fantasy game with castle, Egyptian, "
    "Meso-American and Greek settings). It is a {kind} texture named '{stem}'{hint}. "
    "Say what material it is and describe the surface. Answer with JSON only, no other text: "
    '{{"class": one of {classes}, "description": "at most 10 words: what it is made of and how it looks, '
    'nothing about the game"}}')


def _image(path):
    im = Image.open(path).convert('RGBA')
    bg = Image.new('RGBA', im.size, (128, 128, 128, 255))
    im = Image.alpha_composite(bg, im).convert('RGB')
    s = max(1, int(np.ceil(336 / min(im.size))))
    return im.resize((im.size[0] * s, im.size[1] * s), Image.LANCZOS)


def parse_reply(text, allowed):
    m = re.search(r'\{.*\}', text, re.S)
    if not m:
        return None
    try:
        j = json.loads(m.group(0))
    except json.JSONDecodeError:
        return None
    c = str(j.get('class', '')).strip().lower()
    d = ' '.join(str(j.get('description', '')).split())
    if c not in allowed or not d:
        return None
    return c, d.rstrip('.')


def run(home, ex, sel, classes, manifest_path, redo=False):
    import torch
    from transformers import AutoModelForImageTextToText, AutoProcessor
    path = os.path.join(home, 'models', classes['models']['vlm'])
    rows = manifest.load_manifest(manifest_path)
    by_pattern = {r['pattern']: r for r in rows}
    todo = []
    for e in sel:
        hits = [r for r in rows if fnmatch.fnmatchcase(e.stem, r['pattern'])]
        if any(r['source'] != 'draft' for r in hits):       # a human row, exact or a glob, labels it
            continue
        if hits and not redo:
            continue
        todo.append(e)
    print(f"texpack draft: {len(todo)} of {len(sel)} textures to draft ({len(sel) - len(todo)} have rows)")
    if not todo:
        return 0
    proc = AutoProcessor.from_pretrained(path)
    model = AutoModelForImageTextToText.from_pretrained(path, dtype=torch.bfloat16).to('cuda').eval()
    bad = 0
    for i, e in enumerate(todo, 1):
        allowed = CLASSES_BY_KIND.get(e.kind)
        if not allowed:
            continue
        hint = ''
        if e.kind == 'skin':
            hint = f", the skin of the model '{e.stem.split('/')[-1].split('.mdl')[0]}'" if '.mdl' in e.stem else ''
        elif e.used_in:
            hint = f", used in the maps {', '.join(e.used_in[:6])}"
        msgs = [{'role': 'user', 'content': [
            {'type': 'image', 'image': _image(os.path.join(ex, e.file))},
            {'type': 'text', 'text': PROMPT.format(kind=e.kind, stem=e.stem, hint=hint, classes=json.dumps(allowed))}]}]
        inp = proc.apply_chat_template(msgs, tokenize=True, add_generation_prompt=True, return_dict=True, return_tensors='pt').to('cuda')
        with torch.no_grad():
            out = model.generate(**inp, max_new_tokens=80, do_sample=False)
        text = proc.batch_decode(out[:, inp['input_ids'].shape[1]:], skip_special_tokens=True)[0]
        got = parse_reply(text, allowed)
        if not got:
            bad += 1
            print(f"[{i}/{len(todo)}] {e.stem}: unusable reply: {text!r}")
            continue
        by_pattern[e.stem] = {'pattern': e.stem, 'class': got[0], 'description': got[1], 'overrides': '', 'source': 'draft'}
        print(f"[{i}/{len(todo)}] {e.stem}: {got[0]}, {got[1]}")
        if i % 25 == 0:
            manifest.write_manifest(manifest_path, list(by_pattern.values()))
    manifest.write_manifest(manifest_path, list(by_pattern.values()))
    print(f"{len(todo) - bad} drafted, {bad} unusable")
    return 0
