"""texpack's groups (story 9.4; DECISIONS M66, M69, M70): the textures a redraw draws once or
alike, and the texels their originals share made identical again.

copies   the same pixels under two names (36 groups of 74 textures in both games, mostly the
         mission pack's renames): drawn once, as the copy with the most area in the maps, and
         its files copied to the others
sets     textures of one size whose originals share at least SHARED of their texels at the same
         places (Raven's variants: the Four Horsemen's frame, a wall with and without its
         chains, a switch's disc with another rune), joined with an animation's frames: each
         drawn alone with one seed, the first's (the set's member with the most area; one that
         doesn't repeat, `tile=0`, after those that do), which
         halved the disagreement at the shared texels in 9.4's spike; then each member's texels
         that equal an earlier member's take the first such member's drawing (`unify`), less a
         texel around where they differ (MARGIN, 9.5: the drawing there carries the other's
         bevels and shadows), its own texels rebalanced to its brightness. A canvas of the set side by side failed beyond four
         framed panels, a member as another's reference copied its runes (M69)
heroes   a family's lead (families.csv) is drawn first; the family's layout members get its
         drawing as a second reference image (the material: an ashlar trim kept its layout 0.63
         with it, 0.22 without), unless they share texels with it (a set: unify does it). Not
         reimagine members: the hero's arrangement leaked into them (a rock face 0.86 -> 0.18, a
         crack across it), nor faithful ones, whose content must not change (M70), nor a row with
         `hero=none` (9.5: a door panel took its hero's ring handle under every seed)
crops    a texture whose original is a cell of a larger one's grid (the same pixels at a multiple
         of its size: the floor sundial's quarters are its halves' right halves) is not drawn: its
         files are cut from the larger one's drawing, so the two meet without a join (9.5, M81)
wholes   pieces of one picture (a row's `whole=<name>@<y>,<x>`: the texel where the piece sits in
         it) are drawn as that picture, labeled by a row of its own name, and cut out of it: the
         sundial's halves meet across its middle (9.5, M81). Pieces may overlap: the one placed
         further down, then right, is the picture there (the upper halves of tibet10's carved
         panels hold rows the maps never show, M86). The picture's row's `mirror=xy` adds its
         pieces mirrored to the right and below (a quarter the maps mirror into a hexagonal slab,
         M87). Crops and pieces are in no set and take no hero
joins    a texture that meets another in the maps takes that one's finished drawing where it
         shares its texels (a row's `join=<stem>`: where their originals are equal, as a set's
         later member does; `join=<stem>@<ramp>,...`: where its original has those ramps, the
         gaps closed and specks dropped), so the two meet without a join: Raven pasted the lawn
         into a path's grassy edge (9.5, M85). Not a set: each keeps its seed, the other's files
         stay as they are. A neighbour whose size divides its own joins tiled (a brick wall
         pasted 2 by 2 into its corner pieces, M88)
"""
import collections
import hashlib

import numpy as np

from imaging import _blur, _enc, _lin

SHARED = 0.2
MARGIN = 1          # texels beside a difference a later member keeps its own drawing in (`equal`)
SET_TIERS = ('reimagine', 'layout', 'faithful', 'painting')
REF_TIERS = ('layout',)
LABEL = ('tier', 'cls', 'description', 'regions')
JOIN_CLOSE = 2      # a ramp join's texels closed by this many (the gaps between grass blades)
JOIN_SPECK = 0.01   # and its pieces of fewer than this share of the texels dropped (stray green among bricks)


def fit(a, shape):
    """An image tiled up to (h, w) when those are whole multiples of its own size (a join's
    smaller neighbour), else as it is."""
    h, w = shape[:2]
    if a.shape[:2] == (h, w) or h % a.shape[0] or w % a.shape[1]:
        return a
    return np.tile(a, (h // a.shape[0], w // a.shape[1]) + (1,) * (a.ndim - 2))


def divides(small, big):
    """Whether a texture of shape small tiles a whole number of times into one of shape big."""
    return small[0] <= big[0] and small[1] <= big[1] and not big[0] % small[0] and not big[1] % small[1]


def compose(at, pix):
    """A whole's picture from its pieces [(stem, y, x)] (uint8 originals in pix): those placed
    further down, then right, cover the others where they overlap (M86). Returns (the image,
    how many pieces hold each texel)."""
    H = max(y + pix[s].shape[0] for s, y, x in at)
    W = max(x + pix[s].shape[1] for s, y, x in at)
    c = {pix[s].shape[-1] for s, _, _ in at}
    if len(c) != 1:
        raise ValueError(f"{', '.join(s for s, _, _ in at)}: pieces of one picture mix textures with and without alpha")
    img = np.zeros((H, W, c.pop()), np.uint8)
    cover = np.zeros((H, W), np.int32)
    for s, y, x in sorted(at, key=lambda t: (t[1], t[2])):
        h, w = pix[s].shape[:2]
        img[y:y + h, x:x + w] = pix[s]
        cover[y:y + h, x:x + w] += 1
    return img, cover


def mirror_xy(img):
    """A whole's `mirror=xy` (M87): the picture with its mirror image to the right, and the two
    mirrored below."""
    img = np.concatenate([img, img[:, ::-1]], axis=1)
    return np.concatenate([img, img[::-1]], axis=0)


def _area(census, s):
    try:
        return int((census.get(s) or {}).get('area') or 0)
    except ValueError:
        return 0


def _order(census, stems):
    """Most area in the maps first, then by name."""
    return sorted(stems, key=lambda s: (-_area(census, s), s))


def find_crops(stems, pix):
    """{stem: (big, y, x)}: each texture whose original is a cell of another's grid (the same
    pixels at an offset that is a multiple of its own size; the first such big one by name)."""
    sizes = sorted({pix[s].shape[:2] for s in stems})
    cells = {}
    for s in sorted(stems):
        p = pix[s]
        H, W = p.shape[:2]
        for h, w in sizes:
            if (h, w) == (H, W) or H % h or W % w:
                continue
            for y in range(0, H, h):
                for x in range(0, W, w):
                    c = p[y:y + h, x:x + w]
                    cells.setdefault((c.shape, hashlib.sha1(c.tobytes()).hexdigest()), (s, y, x))
    out = {}
    for s in stems:
        hit = cells.get((pix[s].shape, hashlib.sha1(pix[s].tobytes()).hexdigest()))
        if hit:
            out[s] = hit
    return out


class Plan:
    """stems: every texture the run may draw (the export's redrawn tiers, not only the selection);
    res: their resolved rows; pix: their original pixels (uint8, RGB or RGBA); census: census.csv's
    rows; leads: family -> its lead."""

    def __init__(self, stems, res, pix, census, leads, shared=SHARED):
        self.res = res
        # copies: the same pixels and the same label
        by = collections.defaultdict(list)
        for s in stems:
            p = pix[s]
            by[(p.shape, p.tobytes())].append(s)
        # the same pixels whatever their labels (9.5: a seed written on one is written on all)
        self.same = {s: sorted(g) for g in by.values() for s in g}
        self.rep = {}
        for group in by.values():
            left = list(group)
            while left:
                lab = tuple(getattr(res[left[0]], k) for k in LABEL) + (repr(sorted(res[left[0]].params.items())),)
                same = [s for s in left if tuple(getattr(res[s], k) for k in LABEL) + (repr(sorted(res[s].params.items())),) == lab]
                first = _order(census, same)[0]
                for s in same:
                    self.rep[s] = first
                left = [s for s in left if s not in same]
        reps = sorted({r for r in self.rep.values()})
        # the pieces of wholes (a row's `whole=`) and the crops: cut from another drawing (9.5)
        self.whole = {s: res[s].params['whole'] for s in reps if 'whole' in res[s].params}
        self.pieces = collections.defaultdict(list)
        for s, (name, _, _) in sorted(self.whole.items()):
            self.pieces[name].append(s)
        # (a crop's source may be a piece: the sundial's quarters are its halves' halves)
        self.crop = {s: c for s, c in find_crops(reps, pix).items() if s not in self.whole}
        parts = set(self.whole) | set(self.crop)
        # sets: shared texels (same size), and animations' frames
        parent = {s: s for s in reps}

        def find(x):
            while parent[x] != x:
                parent[x] = parent[parent[x]]
                x = parent[x]
            return x

        def join(a, b):
            ra, rb = find(a), find(b)
            if ra != rb:
                parent[max(ra, rb)] = min(ra, rb)
        cand = [s for s in reps if res[s].tier in SET_TIERS and s not in parts]
        by_size = collections.defaultdict(list)
        for s in cand:
            by_size[pix[s].shape].append(s)
        for group in by_size.values():
            if len(group) < 2:
                continue
            A = np.stack([pix[s].reshape(-1, pix[s].shape[-1]).astype(np.int64) @ (256 ** np.arange(pix[s].shape[-1])) for s in group])
            for i, s in enumerate(group):       # a hole is no shared texel: two grates' holes don't make a set
                if pix[s].shape[-1] == 4:
                    A[i][pix[s][..., 3].ravel() == 0] = -1 - i
            for i in range(len(group)):
                eq = (A == A[i]).mean(axis=1)
                for j in np.nonzero(eq >= shared)[0]:
                    if j > i:
                        join(group[i], group[j])
        for s in cand:
            for f in (census.get(s) or {}).get('anim', '').split():
                f = self.rep.get(f, f)
                if f in parent and f != s and res[f].tier in SET_TIERS and pix[f].shape == pix[s].shape and f not in parts:
                    join(s, f)
        members = collections.defaultdict(list)
        for s in reps:
            members[find(s)].append(s)
        self.group = {}
        for ms in members.values():
            # a member that doesn't repeat (`tile=0`) after those that do: its drawing doesn't
            # tile, so a repeating member that took its texels wouldn't either (9.5: rtex455~155e's
            # wall took rtex456~b5c4's path bricks, seam 0.53 -> 2.85); a strip that repeats only
            # top to bottom (`tile=y`) between them
            rank = {1: 0, 'y': 1, 0: 2}
            ms = sorted(_order(census, ms), key=lambda s: rank.get(res[s].params.get('tile', 1), 0))
            for s in ms:
                self.group[s] = ms
        # a crop of a set's member would miss what unify does to its source: drawn as itself
        self.crop = {s: c for s, c in self.crop.items() if len(self.group[c[0]]) == 1}
        # heroes; a lead is never another's member (a lead labeled into another family would need
        # that family's hero first: closure() draws every hero before the rest)
        self.hero = {}
        heads = {self.rep.get(v, v) for v in leads.values() if v}
        for s in reps:
            lead = leads.get(res[s].family)
            lead = self.rep.get(lead, lead) if lead else None
            if (lead and lead in self.group and lead != s and s not in heads and res[s].tier in REF_TIERS
                    and lead not in self.group[s] and self.res[lead].tier in SET_TIERS
                    and res[s].params.get('hero') != 'none' and s not in parts):
                self.hero[s] = lead
        # joins: a neighbour's finished drawing on the texels it shares (9.5)
        self.join = {}
        for s in reps:
            if 'join' not in res[s].params:
                continue
            t, ramps = res[s].params['join']
            t = self.rep.get(t, t)
            why = ('is not a redrawn texture' if t not in self.group else
                   'is itself or in its set' if t in self.group[s] else
                   f'has a size that doesn\'t tile into its own ({pix[t].shape[1]}x{pix[t].shape[0]})'
                   if not divides(pix[t].shape[:2], pix[s].shape[:2]) else
                   'is a crop or a piece' if s in parts or t in parts else
                   'joins another itself' if 'join' in res[t].params else '')
            if why:
                raise ValueError(f"{s}: join={res[s].params['join'][0]}: {t} {why}")
            self.join[s] = (t, ramps)

    def repeats(self, s):
        """Whether a texture tiles: not a crop nor a piece of a whole (their edges are the larger
        picture's; a copy of one is drawn as it is), nor a row's `tile=0`."""
        r = self.rep.get(s, s)
        return r not in self.crop and r not in self.whole and self.res[s].params.get('tile', 1) != 0

    def closure(self, stems):
        """What a selection needs: the representatives of its copies, their sets whole, their
        heroes and the heroes' sets, and so on (a hero's set may hold another family's member).
        Returns (draw: the representatives in the order they are drawn, every hero first (a
        hero has no hero of its own), then the rest a set's members together; write: every
        texture whose files are made, the drawn ones and all their copies)."""
        want = set()
        todo = [self.rep[s] for s in stems if s in self.rep]
        while todo:
            r = todo.pop()
            for t in self.group[r]:
                if t not in want:
                    want.add(t)
                    if t in self.hero:
                        todo.append(self.hero[t])
                    if t in self.crop:          # cut from it: its drawing first
                        todo.append(self.crop[t][0])
                    if t in self.whole:         # one picture: its pieces cut together
                        todo.extend(self.pieces[self.whole[t][0]])
                    if t in self.join:          # its neighbour's finished drawing
                        todo.append(self.join[t][0])
        heroes = sorted({self.hero[s] for s in want if s in self.hero}, key=lambda s: (self.group[s][0], self.group[s].index(s)))
        draw = list(heroes)
        done = set(heroes)
        for first in sorted(want, key=lambda s: (self.group[s][0], s)):
            for t in self.group[first]:
                if t not in done:
                    done.add(t)
                    draw.append(t)
        drawn = set(draw)
        return draw, sorted(s for s, r in self.rep.items() if r in drawn)


def feather(eq, k, wrap):
    """The weight of the shared texels at k times the size: 1 inside, 0 outside, a ramp of about a
    texel across the edge (half inside, half outside)."""
    m = np.kron(eq.astype(np.float64), np.ones((k, k)))
    w = _blur(m, max(1, k // 2), wrap)
    w = np.where(w > 1 - 1e-9, 1.0, np.where(w < 1e-9, 0.0, w))
    return w


def join_mask(m, close=JOIN_CLOSE, speck=JOIN_SPECK):
    """A ramp join's texels: m (the texels of its ramps) closed by `close` texels (a square, not
    wrapped: the shared part is at an edge) and its 4-connected pieces of fewer than `speck` of
    the texels dropped."""
    h, w = m.shape

    def grow(a, op):
        p = np.pad(a, close, mode='edge')
        out = np.zeros_like(a) if op is np.logical_or else np.ones_like(a)
        for dy in range(2 * close + 1):
            for dx in range(2 * close + 1):
                out = op(out, p[dy:dy + a.shape[0], dx:dx + a.shape[1]])
        return out
    if close:
        # on a canvas a ring of `close` empty texels larger: a piece touching an edge keeps it,
        # one that doesn't isn't grown into it
        c = np.pad(m, close)
        m = grow(grow(c, np.logical_or), np.logical_and)[close:close + h, close:close + w]
    else:
        m = m.copy()
    out = np.zeros_like(m)
    seen = np.zeros_like(m)
    for y0, x0 in zip(*np.nonzero(m)):
        if seen[y0, x0]:
            continue
        seen[y0, x0] = True
        stack, piece = [(y0, x0)], []
        while stack:
            y, x = stack.pop()
            piece.append((y, x))
            for yy, xx in ((y + 1, x), (y - 1, x), (y, x + 1), (y, x - 1)):
                if 0 <= yy < h and 0 <= xx < w and m[yy, xx] and not seen[yy, xx]:
                    seen[yy, xx] = True
                    stack.append((yy, xx))
        if len(piece) >= speck * m.size:
            ys, xs = zip(*piece)
            out[list(ys), list(xs)] = True
    return out


def tiles(res, g):
    """The wrap a set's steps use: True where its members all repeat, False with a `tile=0`
    member, 'y' with a `tile=y` strip (wrapped top to bottom only; the code review's: wrapped
    across, a strip's dark left edge was held against its pale right one)."""
    t = [res[s].params.get('tile', 1) for s in g]
    return False if 0 in t else 'y' if 'y' in t else True


def equal(a, b, wrap=True, margin=None):
    """The texels where originals a and b have the same color, less those within `margin` texels
    (a square) of one where they differ: the drawing beside a part one has and the other lacks
    carries that part's bevel and shadow (9.5: marble took marbleseam's highlights along its
    seams). Two members equal throughout a texel's margin take the same first member there."""
    eq = (a == b).all(axis=-1)
    m = MARGIN if margin is None else margin
    if m <= 0 or eq.all():
        return eq
    diff = ~eq
    # wrap 'y': wrapped top to bottom only (the sides padded, then cut off again)
    py, px = (0 if wrap else m), (m if wrap in (False, 'y') else 0)
    diff = np.pad(diff, ((py, py), (px, px)), mode='edge')
    near = np.zeros_like(diff)
    for dy in range(-m, m + 1):
        for dx in range(-m, m + 1):
            near |= np.roll(diff, (dy, dx), axis=(0, 1))
    near = near[py:near.shape[0] - py, px:near.shape[1] - px]
    return eq & ~near


def sources(stems, origs, j, wrap=True, joins=None):
    """For member j of a set, per texel the index of the first member (in the set's order) whose
    original has the same color there and around it (`equal`): j itself where no earlier one does.
    joins: member -> (the texture it joins, its texels or None: where their originals are
    `equal`); the joined textures numbered after the set (len(stems) + their order in `joins`).
    A member's texels it shares with an earlier member stay that member's, joined there or not,
    so the set's shared texels stay identical (M88: a set whose shared bricks are a wall pasted
    into all of them joins that wall in all of them)."""
    s = stems[j]
    n = len(stems)
    src = np.full(origs[s].shape[:2], j)
    for i in range(j - 1, -1, -1):          # backwards: an earlier member overwrites a later one
        t = stems[i]
        if origs[t].shape == origs[s].shape:
            src[equal(origs[s], origs[t], wrap)] = i
    if not joins:
        return src
    if s in joins:
        t, m = joins[s]
        m = equal(origs[s], fit(origs[t], origs[s].shape), wrap) if m is None else m.copy()
        src[m & (src == j)] = n + _joined(joins).index(t)
    # where it takes an earlier member's drawing and that member took a joined texture's there
    for i in sorted(set(np.unique(src).tolist()) - {j}):
        if i < n and any(u in joins for u in stems[:i + 1]):
            si = sources(stems, origs, i, wrap, joins)
            take = (src == i) & (si >= n)
            src[take] = si[take]
    return src


def _joined(joins):
    """The textures joined, in a fixed order (sources' indices after the set's)."""
    return sorted({t for t, _ in joins.values()})


def unify(stems, origs, images, k, wrap=True, normal_at=None, scale=None, joins=None):
    """stems: a set in its order; origs: stem -> its original (1x); images: stem -> a list of its
    maps at k times the size (uint8, h x w x c), as each member was drawn. Each member's texel
    takes the drawing of the first member whose original has the same color there (its own where
    none earlier has), the drawings ramped over half a texel on each side where that member
    changes. Two members equal at a texel have the same first member there, so the texels they
    share end identical (less the ramp at the edge of what they share), whatever the order they
    were drawn in. normal_at: the index of a normal map in the lists (renormalized where
    blended). scale(j, own, composite): for a member after the first, a factor for its own
    drawing's first map in linear light (the albedo's brightness, given the weight of its own
    texels and the composite so far); its drawing is scaled before any later member takes texels
    from it, so they stay identical. joins: member -> (the texture it joins, its texels or None),
    that texture's original and maps in origs and images, as they are (`sources`; one whose size
    divides the member's tiled, `fit`); a first member that joins one is scaled too. Returns the
    new lists and the factors."""
    out, factors = {}, {}
    names = list(stems) + (_joined(joins) if joins else [])
    for j, s in enumerate(stems):
        src = sources(stems, origs, j, wrap, joins)
        ws = {i: feather(src == i, k, wrap) for i in np.unique(src)}

        def compose():
            acc = [np.zeros(im.shape) for im in images[s]]
            for i, w in ws.items():
                for n in range(len(acc)):
                    acc[n] += w[..., None] * fit(images[names[i]][n], acc[n].shape).astype(np.float64)
            return acc
        acc = compose()
        if scale is not None and (j or (joins and s in joins)) and j in ws:
            f = scale(j, ws[j], np.clip(np.round(acc[0]), 0, 255).astype(np.uint8))
            if f != 1.0:
                factors[s] = f
                im = images[s][0].copy()
                im[..., :3] = _enc(_lin(im[..., :3]) * f)
                images[s] = [im] + list(images[s][1:])
                acc = compose()
        blended = np.zeros(acc[0].shape[:2], bool)
        for w in ws.values():
            blended |= (w > 0) & (w < 1)
        res = [np.clip(np.round(a), 0, 255).astype(np.uint8) for a in acc]
        if normal_at is not None and blended.any():
            v = acc[normal_at] / 127.5 - 1.0
            v = v / np.maximum(np.linalg.norm(v, axis=2, keepdims=True), 1e-6)
            nb = np.clip(np.round((v * 0.5 + 0.5) * 255.0), 0, 255).astype(np.uint8)
            res[normal_at] = np.where(blended[..., None], nb, res[normal_at])
        out[s] = res
    return out, factors


def shared_interior(a, b, k, wrap=True):
    """The texels at k times the size that unify makes exactly equal for originals a and b: their
    equal texels (`equal`, the margin left out), less the feather at the edge."""
    return feather(equal(a, b, wrap), k, wrap) >= 1.0
