"""texpack's groups (story 9.4; DECISIONS M66, M69, M70): the textures a redraw draws once or
alike, and the texels their originals share made identical again.

copies   the same pixels under two names (36 groups of 74 textures in both games, mostly the
         mission pack's renames): drawn once, as the copy with the most area in the maps, and
         its files copied to the others
sets     textures of one size whose originals share at least SHARED of their texels at the same
         places (Raven's variants: the Four Horsemen's frame, a wall with and without its
         chains, a switch's disc with another rune), joined with an animation's frames: each
         drawn alone with one seed, the first's (the set's member with the most area), which
         halved the disagreement at the shared texels in 9.4's spike; then each member's texels
         that equal an earlier member's take the first such member's drawing (`unify`), its own
         texels rebalanced to its brightness. A canvas of the set side by side failed beyond four
         framed panels, a member as another's reference copied its runes (M69)
heroes   a family's lead (families.csv) is drawn first; the family's layout members get its
         drawing as a second reference image (the material: an ashlar trim kept its layout 0.63
         with it, 0.22 without), unless they share texels with it (a set: unify does it). Not
         reimagine members: the hero's arrangement leaked into them (a rock face 0.86 -> 0.18, a
         crack across it), nor faithful ones, whose content must not change (M70)
"""
import collections

import numpy as np

from imaging import _blur, _enc, _lin

SHARED = 0.2
SET_TIERS = ('reimagine', 'layout', 'faithful')
REF_TIERS = ('layout',)
LABEL = ('tier', 'cls', 'description', 'regions')


def _area(census, s):
    try:
        return int((census.get(s) or {}).get('area') or 0)
    except ValueError:
        return 0


def _order(census, stems):
    """Most area in the maps first, then by name."""
    return sorted(stems, key=lambda s: (-_area(census, s), s))


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
        cand = [s for s in reps if res[s].tier in SET_TIERS]
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
                if f in parent and f != s and res[f].tier in SET_TIERS and pix[f].shape == pix[s].shape:
                    join(s, f)
        members = collections.defaultdict(list)
        for s in reps:
            members[find(s)].append(s)
        self.group = {}
        for ms in members.values():
            ms = _order(census, ms)
            for s in ms:
                self.group[s] = ms
        # heroes; a lead is never another's member (a lead labeled into another family would need
        # that family's hero first: closure() draws every hero before the rest)
        self.hero = {}
        heads = {self.rep.get(v, v) for v in leads.values() if v}
        for s in reps:
            lead = leads.get(res[s].family)
            lead = self.rep.get(lead, lead) if lead else None
            if (lead and lead in self.group and lead != s and s not in heads and res[s].tier in REF_TIERS
                    and lead not in self.group[s] and self.res[lead].tier in SET_TIERS):
                self.hero[s] = lead

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


def sources(stems, origs, j):
    """For member j of a set, per texel the index of the first member (in the set's order) whose
    original has the same color there: j itself where no earlier one does."""
    s = stems[j]
    src = np.full(origs[s].shape[:2], j)
    for i in range(j - 1, -1, -1):          # backwards: an earlier member overwrites a later one
        t = stems[i]
        if origs[t].shape == origs[s].shape:
            src[(origs[s] == origs[t]).all(axis=-1)] = i
    return src


def unify(stems, origs, images, k, wrap=True, normal_at=None, scale=None):
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
    from it, so they stay identical. Returns the new lists and the factors."""
    out, factors = {}, {}
    for j, s in enumerate(stems):
        src = sources(stems, origs, j)
        ws = {i: feather(src == i, k, wrap) for i in np.unique(src)}

        def compose():
            acc = [np.zeros(im.shape) for im in images[s]]
            for i, w in ws.items():
                for n in range(len(acc)):
                    acc[n] += w[..., None] * images[stems[i]][n].astype(np.float64)
            return acc
        acc = compose()
        if scale is not None and j and j in ws:
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
    equal texels, less the feather at the edge."""
    eq = (a == b).all(axis=-1)
    return feather(eq, k, wrap) >= 1.0
