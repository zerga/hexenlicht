"""The texpack manifest (story 5.8): the human-fed context for each texture.

materials.csv, columns: pattern, class, description, overrides, source.
  pattern      a texture's file stem as the export names it (no textures/ and no
               .png: rtex022, rtex343~ad81, #lava000, models/imp.mdl_0), or an
               fnmatch glob (rtex02*, models/imp.mdl_*, *lava*)
  class        a class of classes.toml, or empty to keep the one a less specific
               row (or the export's kind) gave
  description  a few words for the prompt ("worn grey castle stone blocks")
  overrides    key=value pairs separated by ; that change the class's keys
  source       human or draft: `draft` never overwrites a human row
Blank lines and lines starting with '# ' (a hash and a space: the liquids' names
start with # themselves, #lava000) are skipped; `draft` and `merge` rewrite the file
sorted and without comments. Resolution: every row whose
pattern matches applies, least specific first (draft rows first, then human
rows; within each a literal pattern beats a glob, a glob with more literal
characters beats one with fewer); a later row's
class/description replaces an earlier one's, its overrides add to them.
"""
import csv
import fnmatch
import os
import tomllib

COLUMNS = ['pattern', 'class', 'description', 'overrides', 'source']
GLOB_CHARS = '*?['


def load_classes(path):
    with open(path, 'rb') as f:
        return tomllib.load(f)


def load_manifest(path):
    rows = []
    if not path or not os.path.exists(path):
        return rows
    with open(path, newline='', encoding='utf-8') as f:
        lines = [ln for ln in f if ln.strip() and not (ln.startswith('# ') or ln.strip() == '#')]
    for r in csv.DictReader(lines):
        r = {c: (r.get(c) or '').strip() for c in COLUMNS}
        if r['pattern']:
            rows.append(r)
    return rows


def write_manifest(path, rows):
    with open(path, 'w', newline='', encoding='utf-8') as f:
        w = csv.DictWriter(f, COLUMNS, lineterminator='\n')
        w.writeheader()
        for r in sorted(rows, key=lambda r: r['pattern']):
            w.writerow({c: r.get(c, '') for c in COLUMNS})


def _specificity(pattern):
    if not any(c in pattern for c in GLOB_CHARS):
        return (2, len(pattern))
    return (1, sum(1 for c in pattern if c not in GLOB_CHARS))


def parse_overrides(text):
    out = {}
    for part in text.split(';'):
        part = part.strip()
        if not part:
            continue
        if '=' not in part:
            raise ValueError(f"override '{part}' is not key=value")
        k, v = part.split('=', 1)
        out[k.strip()] = v.strip()
    return out


def _cast(value, like, key):
    if isinstance(like, bool):
        return value.lower() in ('1', 'true', 'yes')
    if isinstance(like, int):
        return int(value)
    if isinstance(like, float):
        return float(value)
    return value


class Resolved:
    def __init__(self, stem, kind, cls, description, params, rows):
        self.stem = stem
        self.kind = kind
        self.cls = cls
        self.description = description
        self.params = params
        self.rows = rows            # the manifest rows that matched, least specific first

    def prompt(self):
        noun = self.params['noun']
        return self.params['prompt'].format(desc=self.description or noun, noun=noun)


def resolve(stem, kind, rows, classes):
    cls = classes['kind_default'].get(kind, 'world')
    desc = ''
    over = {}
    matched = [r for r in rows if fnmatch.fnmatchcase(stem, r['pattern'])]
    # a human row beats a draft whatever its pattern; then a literal beats a glob
    matched.sort(key=lambda r: (r['source'] != 'draft', _specificity(r['pattern'])))
    for r in matched:
        if r['class']:
            cls = r['class']
        if r['description']:
            desc = r['description']
        over.update(parse_overrides(r['overrides']))
    if cls not in classes['class']:
        raise ValueError(f"{stem}: unknown class '{cls}' (classes.toml has {', '.join(sorted(classes['class']))})")
    params = dict(classes['defaults'])
    params.update(classes['class'][cls])
    for k, v in over.items():
        if k not in params:
            raise ValueError(f"{stem}: unknown override key '{k}'")
        params[k] = _cast(v, params[k], k)
    return Resolved(stem, kind, cls, desc, params, matched)
