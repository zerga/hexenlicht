"""texpack reseed (story 9.5; README "The world pack", DECISIONS M73): another seed for a texture
whose drawing `verify` failed in a way another drawing may not (a seam where the original tiles,
shapes that moved: 9.4's test run had 4 such of 108, all what klein drew).

For each such texture its set (the texture alone, or the set whose one seed it shares: M69) is
drawn again with the next seeds (after its current one where an earlier reseed gave it), each
try into a folder of its own under `<pack>\\.texpack\\reseed\\` (the hero's drawing the layout
members' reference, copied from the pack), unified as the run writes it, and checked as `verify`
checks it (a member where it drew itself: a set's member passed alone and failed unified); the first seed
under which no member fails that way is written into the manifest as the `seed=` of the set's
seed row (the member whose `seed=` the set takes, else its first), and `texpack.py run` then
redraws the set with it. A set whose seed is the owner's (a hero's pick: a `human` row with
`seed=`) is left to the owner, as is a set no try fixes. `<pack>\\.texpack\\reseed.csv` keeps
what was tried.

  texpack.py verify --pack <pack> ...; texpack.py reseed --pack <pack> [--tries 4]; texpack.py run ...
"""
import csv
import os
import shutil

import groups
import manifest
import materials
import pipeline
import verify

TRIES = 4
OFFSET = 1000           # the tries' seeds: the name's with the bases after this (the heroes' candidates take 1-5)


def reseedable(errors):
    return [e for e in errors if e.startswith(verify.REDRAWN)]


def seed_row(rd, g):
    """The member whose row's `seed=` the set takes (redraw.seed_of), else the first."""
    for s in g:
        if 'seed' in rd.res[s].params:
            return s
    return g[0]


def labeled(rows, stem):
    """The texture's own labeled row in the manifest (the one resolve() took)."""
    import labels
    return labels.labeled_row(stem, rows)


def set_seed(row, seed):
    """seed None: the row's seed= taken off."""
    over = manifest.parse_overrides(row.get('overrides', ''))
    if seed is None:
        over.pop('seed', None)
    else:
        over['seed'] = str(seed)
    row['overrides'] = ';'.join(f'{k}={v}' for k, v in over.items())


def twins(plan, census, stem):
    """The textures labeled as `stem` is (labels check holds them to it): its copies (the same
    pixels: plan.same, whatever their labels say now) and their animations' frames (the census's
    `anim`), stem among them."""
    census = census or {}
    anim = lambda s: set((census.get(s) or {}).get('anim', '').split())  # noqa: E731
    own = anim(stem)
    # another animation's frame with the same pixels is labeled on its own (two animations can
    # share a first frame: M59)
    out = {s for s in getattr(plan, 'same', {}).get(stem, [stem]) if not anim(s) or s in own} | {stem}
    for s in list(out):
        out |= anim(s)
    return sorted(out)


def set_seeds(rows, plan, census, stem, seed, source=None):
    """The seed (None: none) on the labeled rows of `stem` and its twins, their source set where
    given; returns the rows changed."""
    import labels
    out = []
    for s in twins(plan, census, stem):
        row = labels.labeled_row(s, rows)
        if row is None:
            continue
        before = (row.get('overrides', ''), row.get('source'))
        set_seed(row, seed)
        if source:
            row['source'] = source
        if (row.get('overrides', ''), row.get('source')) != before:
            out.append(row)
    return out


def run(rd, ex, pack, rows, manifest_path, stems=None, tries=TRIES, log=print):
    """rd: a redraw.Redraw over the export (its plan, rows and ComfyUI); stems: only these
    (default: every failure in the pack's verify.csv that another seed may fix)."""
    import redraw
    report = verify.load_report(pack)
    want = []
    for s, r in sorted(report.items()):
        if r['result'] == 'FAIL' and reseedable(r['errors'].split(' | ')) and (not stems or s in stems) and s in rd.plan.rep:
            want.append(s)
    tab = materials.table(rd.classes)
    done, out = set(), []
    scratch = os.path.join(pack, '.texpack', 'reseed')
    path = os.path.join(pack, '.texpack', 'reseed.csv')
    cols = ['set', 'members', 'failed', 'errors', 'seed_row', 'old_seed', 'tried', 'seed', 'result']
    old = []
    if os.path.exists(path):
        with open(path, newline='', encoding='utf-8') as f:
            old = list(csv.DictReader(f))

    def save():
        # after every set: a stopped or failed reseed keeps the seeds it found (the code review's)
        with open(path, 'w', newline='', encoding='utf-8') as f:
            w = csv.DictWriter(f, cols, lineterminator='\n')
            w.writeheader()
            for r in old + out:
                w.writerow({c: r.get(c, '') for c in cols})
    for t in want:
        g = rd.plan.group[rd.plan.rep[t]]
        if g[0] in done:
            continue
        done.add(g[0])
        rd.seeds = {}
        rd.open(pack)
        holder = seed_row(rd, g)
        row = labeled(rows, holder)
        rec = {'set': g[0], 'members': ' '.join(g), 'failed': t, 'errors': report[t]['errors'], 'seed_row': holder,
               'old_seed': rd.seed_of(holder), 'tried': '', 'seed': '', 'result': ''}
        # verify.csv may be older than the pack (a run redrew the set since): the pack's texture as it is now
        j = g.index(rd.plan.rep[t])
        errs_now, _ = verify.check_redraw(ex, pack, rd.entries[t], rd.res[t], rd.palette, tab, repeats=rd.plan.repeats(t), own=
                                          groups.sources(g, rd.orig, j, groups.tiles(rd.res, g)) == j if len(g) > 1 else None)
        missing = [h for h in (rd.plan.hero.get(m) for m in g) if h and not os.path.exists(os.path.join(pack, 'work', rd.root_of(h) + '_A.png'))]
        if rd.root_of(t) != t:
            rec['result'] = f"cut from {rd.root_of(t)}'s drawing: another seed is its row's, left to the owner"
        elif any(m in rd.plan.join for m in g):
            # its unified files need the joined texture's finished ones, which a try's folder hasn't
            # (the code review's: a KeyError that stopped the rest of the list)
            rec['result'] = 'a set with a join: left to the owner'
        elif not reseedable(errs_now):
            rec['result'] = 'passes now (verify.csv was older): left as it is'
        elif missing:
            rec['result'] = f"its hero {missing[0]} has no drawing in the pack: run first"
        elif row is None:
            rec['result'] = f'no labeled row of {holder}'
        elif row['source'] == 'human' and 'seed' in manifest.parse_overrides(row.get('overrides', '')):
            rec['result'] = 'the owner\'s seed (a hero\'s pick): left to the owner'
        else:
            tried = []
            base = redraw.anim_base(g[0])
            # after the set's current seed where it is one of reseed's (an earlier reseed's)
            k0 = next((k for k in range(1, 100) if pipeline.seed_for(base, rd.seed + OFFSET + k) == rec['old_seed']), 0)
            for k in range(k0 + 1, k0 + tries + 1):
                seed = pipeline.seed_for(base, rd.seed + OFFSET + k)
                tried.append(str(seed))
                folder = os.path.join(scratch, g[0].replace('/', '_').replace('#', '_').replace('+', 'p'), f't{k}')
                rd.open(folder)
                rd.seeds = {g[0]: seed}
                for m in g:
                    h = rd.plan.hero.get(m)
                    if h:   # the hero's drawing, the reference, as the pack has it (a part's: its source's)
                        a = rd.root_of(h) + '_A.png'
                        rd._copy(os.path.join(pack, 'work', a), os.path.join(folder, 'work', a))
                    rd.draw(m)
                # the set as the run writes it (unified), each member checked where it drew itself, as verify does
                rd.finish(list(g), list(g))
                bad = []
                for j, m in enumerate(g):
                    own = groups.sources(g, rd.orig, j, groups.tiles(rd.res, g)) == j if len(g) > 1 else None
                    errs, _ = verify.check_redraw(ex, folder, rd.entries[m], rd.res[m], rd.palette, tab, own, rd.plan.repeats(m))
                    bad += [f'{m}: {e}' for e in reseedable(errs)]
                log(f"reseed {g[0]}{' (set of %d)' % len(g) if len(g) > 1 else ''}: seed {seed}: "
                    + ('ok' if not bad else '; '.join(bad)))
                if not bad:
                    set_seeds(rows, rd.plan, rd.census, holder, seed)
                    manifest.write_manifest(manifest_path, rows)
                    rec['seed'], rec['result'] = seed, f'ok at try {k}'
                    break
            rec['tried'] = ' '.join(tried)
            if not rec['result']:
                rec['result'] = f'no seed of {tries} passed: left to the owner'
            shutil.rmtree(os.path.join(scratch, g[0].replace('/', '_').replace('#', '_').replace('+', 'p')), ignore_errors=True)
        out.append(rec)
        save()
    rd.seeds = {}
    rd.open(pack)
    save()
    fixed = sum(1 for r in out if r['seed'])
    log(f"reseed: {len(want)} failures in {len(out)} sets; {fixed} sets got a new seed in the manifest, "
        f"{len(out) - fixed} left to the owner ({path}); now `texpack.py run` redraws them")
    return out
