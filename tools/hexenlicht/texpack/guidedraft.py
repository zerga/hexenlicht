"""A draft of each hub's guide steps from the maps' entities (story 10.1), to check a recording
against, and the counts the story asks for.

For every hub of both games (census.py's HUBS), from the BSPs' entity lumps in the game's paks
(bspviews.py's reader): the puzzle pieces (`puzzle_piece`, `puzzle_static_piece`: their
`puzzle_id`, map and place), the locks (any entity with `puzzle_piece_1..4`: doors, triggers;
by hl_guide.c's key: a door's brush model `*N`, a trigger's `classname:x,y,z`, its box's
middle), the crosslevel bits (`trigger_crosslevel` sets them, `trigger_crosslevel_target`
fires on a level's start when set: spawnflags 1-128 are bits 1-8) and the level changes (`trigger_changelevel`'s `map`; a rider boss's `map` ends the hub:
its death clears the crosslevel bits and the puzzle pieces, rider.hc). Prints only ids,
classnames, map names, models and positions: never Raven's text (`message`, `netname`,
`no_puzzle_msg`, infolist.txt). The output stays outside the repository (DECISIONS N4).

Counts: maps whose crosslevel targets wait on a bit set in another map; locks that need a piece
found in another map (or in no map: given by the gamecode).

With --check <file.hlguide>: which locks and crosslevel bits of the file's hub the recording
covers (a lock by a step on its model, `gone`/`moved`/`field`, or by an `item` step for one of
its pieces; a bit by a `flag` step).

  python -I guidedraft.py --data <game data> [--hub blackmarsh] [--check maps\\blackmarsh.hlguide]
"""

import argparse
import collections
import math
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bspviews as B  # noqa: E402
import census  # noqa: E402

PIECES = ('puzzle_piece', 'puzzle_static_piece')
LOCK_KEYS = ('puzzle_piece_1', 'puzzle_piece_2', 'puzzle_piece_3', 'puzzle_piece_4')
RIDERS = ('rider_famine', 'rider_death', 'rider_pestilence', 'rider_war', 'monster_eidolon')


def entities(data_dir):
    """map -> (game folder, entity dicts, brush models' mins and maxs) for every map in the paks."""
    out = {}
    for name, (pak, pos, ln) in sorted(B.pak_files(data_dir).items()):
        if not name.endswith('.bsp'):
            continue
        with open(pak, 'rb') as f:
            f.seek(pos)
            b = f.read(ln)
        lumps = struct.unpack_from('<i30i', b)[1:]
        ofs, size = lumps[0], lumps[1]
        text = b[ofs:ofs + size].split(b'\0')[0].decode('latin-1')
        ofs, size = lumps[28], lumps[29]
        models = [struct.unpack_from('<6f', b, ofs + i * 80) for i in range(size // 80)]   # Hexen II: 8 hulls
        out[name[len('maps/'):-len('.bsp')]] = (os.path.basename(os.path.dirname(pak)), B.parse_entities(text), models)
    return out


def bits(ent):
    return [b + 1 for b in range(8) if B._int(ent.get('spawnflags')) & (1 << b)]


def place(ent):
    v = B._vec(ent.get('origin', '0 0 0'))
    return '%d %d %d' % tuple(int(math.floor(x + 0.5)) for x in v) if v is not None else '?'


def key(ent, models):
    """hl_guide.c's key: a brush model's *N, a trigger's (InitTrigger clears its model)
    classname:x,y,z, the middle of its box."""
    model = ent.get('model', '')
    m = re.fullmatch(r'\*(\d+)', model)
    if not m:
        return model or '-'
    if not ent.get('classname', '').startswith('trigger_'):
        return model
    lo = models[int(m.group(1))]
    o = B._vec(ent.get('origin', '0 0 0'))
    o = o if o is not None else (0, 0, 0)
    c = [(lo[i] + lo[i + 3]) / 2 + o[i] for i in range(3)]
    return '%s:%d,%d,%d' % ((ent['classname'],) + tuple(int(math.floor(x + 0.5)) for x in c))


class Hub:
    def __init__(self, name):
        self.name = name
        self.maps = []
        self.pieces = []      # (id, map, classname, place)
        self.locks = []       # (map, classname, model, ids)
        self.setters = collections.defaultdict(list)   # bit -> maps
        self.targets = collections.defaultdict(list)   # bit -> maps
        self.changes = []     # (map, to map, spawnflags)
        self.ends = []        # (map, rider, next map)


def scan(ents):
    hubs = collections.OrderedDict()
    for mapname, (game, es, models) in ents.items():
        hub = census.hub_of(mapname)
        if hub in ('other', 'deathmatch'):
            continue
        h = hubs.setdefault((game, hub), Hub(hub))
        h.maps.append(mapname)
        for e in es:
            cls = e.get('classname', '')
            if cls in PIECES and e.get('puzzle_id'):
                h.pieces.append((e['puzzle_id'].lower(), mapname, cls, place(e)))
            ids = [e[k].lower() for k in LOCK_KEYS if e.get(k)]
            if ids:
                h.locks.append((mapname, cls, key(e, models), ids))
            if cls == 'trigger_crosslevel':
                for b in bits(e):
                    h.setters[b].append(mapname)
            elif cls == 'trigger_crosslevel_target':
                for b in bits(e):
                    h.targets[b].append(mapname)
            elif cls == 'trigger_changelevel' and e.get('map'):
                h.changes.append((mapname, e['map'].lower(), B._int(e.get('spawnflags'))))
            elif cls in RIDERS and e.get('map'):
                h.ends.append((mapname, cls, e['map'].lower()))
    return hubs


def counts(h):
    """(maps waiting on a bit set elsewhere, locks needing a piece from elsewhere or nowhere)"""
    waiting = set()
    for b, maps in h.targets.items():
        for m in maps:
            if any(s != m for s in h.setters.get(b, [])):
                waiting.add(m)
    where = collections.defaultdict(set)
    for pid, m, _, _ in h.pieces:
        where[pid].add(m)
    remote = [lk for lk in h.locks if any(lk[0] not in where.get(i, ()) for i in lk[3])]
    return sorted(waiting), remote


def read_guide(path):
    """(hub, [(step map, condition words)])"""
    hub, steps, cur = None, [], None
    with open(path, encoding='latin-1') as f:
        for line in f:
            w = re.sub(r'(^|\s)//.*$', '', line).split()
            if not w:
                continue
            if w[0] == 'hub' and len(w) > 1:
                hub = w[1]
            elif w[0] == 'step':
                m = re.match(r'\s*step\s+"[^"]*"\s+(\S+)', line)
                cur = m.group(1) if m else None
            elif w[0] == 'done' and cur:
                steps.append((cur, w[1:]))
    return hub, steps


def check(h, path):
    hub, steps = read_guide(path)
    items = {s[1][1].lower() for s in steps if s[1][:1] == ['item'] and len(s[1]) > 1}
    flags = {int(s[1][1]) for s in steps if s[1][:1] == ['flag'] and len(s[1]) > 1}
    keyed = {(m, w[1]) for m, w in steps if w[:1] in (['gone'], ['moved'], ['field']) and len(w) > 1}
    print(f'\ncheck {path} (hub {hub}): {len(steps)} steps')
    missing = 0
    for m, cls, model, ids in h.locks:
        by_model = (m, model) in keyed
        by_item = [i for i in ids if i in items]
        state = 'lock step' if by_model else ('item step ' + ','.join(by_item)) if by_item else 'MISSING'
        missing += state == 'MISSING'
        print(f'  lock {m} {cls} {model or "-"} needs {",".join(ids)}: {state}')
    for b in sorted(set(h.setters) | set(h.targets)):
        state = 'flag step' if b in flags else 'MISSING'
        missing += state == 'MISSING'
        print(f'  bit {b}: set in {",".join(h.setters.get(b, [])) or "-"}, waited on in '
              f'{",".join(h.targets.get(b, [])) or "-"}: {state}')
    print(f'  {missing} not covered')


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--data', required=True, help='the game data folder: the one holding data1 and portals')
    ap.add_argument('--hub', help='only this hub')
    ap.add_argument('--check', help='a recorded .hlguide to check against its hub')
    a = ap.parse_args(argv)

    hubs = scan(entities(a.data))
    total = collections.Counter()
    for (game, name), h in hubs.items():
        waiting, remote = counts(h)
        total[game, 'maps'] += len(h.maps)
        total[game, 'waiting'] += len(waiting)
        total[game, 'locks'] += len(h.locks)
        total[game, 'remote'] += len(remote)
        if a.hub and a.hub != name:
            continue
        print(f'== {game} {name}: {len(h.maps)} maps ({" ".join(sorted(h.maps))})')
        for pid, m, cls, p in sorted(h.pieces):
            print(f'  piece {pid} {m} {cls} at {p}')
        for m, cls, model, ids in sorted(h.locks):
            print(f'  lock {m} {cls} {model or "-"} needs {",".join(ids)}{"  (from another map)" if (m, cls, model, ids) in remote else ""}')
        for b in sorted(set(h.setters) | set(h.targets)):
            print(f'  bit {b}: set in {",".join(sorted(set(h.setters.get(b, [])))) or "-"}; '
                  f'targets in {",".join(sorted(set(h.targets.get(b, [])))) or "-"}')
        for m, to, sf in sorted(h.changes):
            print(f'  change {m} -> {to}' + (f' (spawnflags {sf})' if sf else ''))
        for m, cls, to in h.ends:
            print(f'  end {m} {cls} -> {to}')
        print(f'  {len(waiting)} maps wait on a bit set in another map; '
              f'{len(remote)} of {len(h.locks)} locks need a piece from another map or none')
        if a.check and (a.hub or read_guide(a.check)[0] == name):    # the file's own hub
            check(h, a.check)
    print('\ntotals:')
    for game in sorted({g for g, _ in total}):
        print(f'  {game}: {total[game, "maps"]} maps, {total[game, "waiting"]} wait on crosslevel bits set elsewhere; '
              f'{total[game, "remote"]} of {total[game, "locks"]} locks need a piece from another map or none')


if __name__ == '__main__':
    main()
