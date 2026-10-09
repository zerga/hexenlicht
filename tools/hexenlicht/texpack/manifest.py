"""The texpack manifest (story 5.8): the human-fed context for each texture.

materials.csv, columns: pattern, purpose, tier, class, family, regions, description,
overrides, source.
  pattern      a texture's file stem as the export names it (no textures/ and no
               .png: rtex022, rtex343~ad81, #lava000, models/imp.mdl_0), or an
               fnmatch glob (rtex02*, models/imp.mdl_*, *lava*)
  purpose      what the texture is for (story 9.3: classes.toml's [labels])
  tier         how far a redraw may depart from it (redraw.py's tiers, fx, skip)
  class        a class of classes.toml, or empty to keep the one a less specific
               row (or the export's kind) gave
  family       the textures that should read as one material (families.csv)
  regions      which palette ramps are which material (labels.py)
  description  what it is, for the prompt ("worn grey castle stone blocks")
  overrides    key=value pairs separated by ; that change the class's keys
  source       draft (a vision model's), claude (9.3's labels) or human: `draft`
               never overwrites another row
Blank lines and lines starting with '# ' (a hash and a space: the liquids' names
start with # themselves, #lava000) are skipped; `draft` and `merge` rewrite the file
sorted and without comments. A labeled row (9.3: a literal pattern with a tier, from
claude or human) stands alone: the texture takes it and nothing else, the owner's over
Claude's (`texpack.py labels apply` folds the class and overrides it had into it). Otherwise:
every row whose
pattern matches applies, least specific first (draft rows first, then human
rows; within each a literal pattern beats a glob, a glob with more literal
characters beats one with fewer); a later row's
class/description replaces an earlier one's, its overrides add to them.
"""
import csv
import fnmatch
import os
import tomllib

COLUMNS = ['pattern', 'purpose', 'tier', 'class', 'family', 'regions', 'description', 'overrides', 'source']
LABELED = ('claude', 'human')
GLOB_CHARS = '*?['


def load_classes(path):
    with open(path, 'rb') as f:
        return tomllib.load(f)


def read_rows(path):
    """A CSV's rows as dicts by its header: records whose first cell starts with '# ' (or is '#')
    and empty records are skipped after the parse, so a quoted cell may hold blank lines and
    lines starting with '# ' (an answer typed on the owner's page)."""
    with open(path, newline='', encoding='utf-8-sig') as f:
        recs = [r for r in csv.reader(f) if any(c.strip() for c in r) and not (r[0].startswith('# ') or r[0].strip() == '#')]
    if not recs:
        return [], []
    head = [h.strip() for h in recs[0]]
    return head, [dict(zip(head, r)) for r in recs[1:]]


def load_manifest(path):
    rows = []
    if not path or not os.path.exists(path):
        return rows
    for r in read_rows(path)[1]:
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
    def __init__(self, stem, kind, cls, description, params, rows, label=None):
        self.stem = stem
        self.kind = kind
        self.cls = cls
        self.description = description
        self.params = params
        self.rows = rows            # the manifest rows that matched, least specific first
        label = label or {}
        self.purpose, self.tier, self.family, self.regions = (label.get(k, '') for k in ('purpose', 'tier', 'family', 'regions'))

    def prompt(self):
        noun = self.params['noun']
        return self.params['prompt'].format(desc=self.description or noun, noun=noun)


def resolve(stem, kind, rows, classes):
    cls = classes['kind_default'].get(kind, 'world')
    desc = ''
    over = {}
    matched = [r for r in rows if fnmatch.fnmatchcase(stem, r['pattern'])]
    labeled = [r for r in matched if r['pattern'] == stem and r.get('tier') and r.get('source') in LABELED]
    if labeled:
        # a labeled row stands alone, the owner's over Claude's
        matched = [max(labeled, key=lambda r: r['source'] == 'human')]
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
    return Resolved(stem, kind, cls, desc, params, matched, matched[0] if labeled else None)
