"""texpack's image helpers (story 9.1, out of redraw.py in 9.4): the engine's color transfer,
box blurs with wrapped or mirrored edges, Oklab. Arrays are numpy, colors 8-bit RGB.
"""
import numpy as np

from pipeline import GAMMA, LUM


def _lin(rgb):
    return (rgb.astype(np.float64) / 255.0) ** GAMMA


def _enc(lin):
    return np.clip(np.round(np.clip(lin, 0.0, 1.0) ** (1.0 / GAMMA) * 255.0), 0, 255).astype(np.uint8)


def _luma(rgb):
    """Linear luminance."""
    return _lin(rgb) @ LUM


def _box(a, f):
    """a downsampled by an integer factor f (mean of f x f blocks; the rest cut off)."""
    h, w = a.shape[0] // f * f, a.shape[1] // f * f
    a = a[:h, :w]
    return a.reshape(h // f, f, w // f, f, *a.shape[2:]).mean(axis=(1, 3))


def _blur(y, r, wrap=False):
    """A (2r+1)-wide box blur of a 2-D array, the edges wrapped or mirrored."""
    k = 2 * r + 1
    p = np.pad(y, r, mode='wrap' if wrap else 'symmetric')
    c = np.vstack([np.zeros((1, p.shape[1])), np.cumsum(p, axis=0)])
    p = (c[k:] - c[:-k]) / k
    c = np.hstack([np.zeros((p.shape[0], 1)), np.cumsum(p, axis=1)])
    return (c[:, k:] - c[:, :-k]) / k


def _smooth(y, r, wrap=False):
    """Three box blurs of radius r // 2: close to a Gaussian of sigma about r / 2."""
    for _ in range(3):
        y = _blur(y, max(1, r // 2), wrap)
    return y


def _highpass(y, r, wrap=False):
    return y - _blur(y, r, wrap)


def _corr(a, b, mask=None):
    if mask is not None:
        a, b = a[mask], b[mask]
    a, b = a - a.mean(), b - b.mean()
    d = np.sqrt((a * a).sum() * (b * b).sum())
    return float((a * b).sum() / d) if d > 0 else 0.0


_M1 = np.array([[0.4122214708, 0.5363325363, 0.0514459929], [0.2119034982, 0.6806995451, 0.1073969566],
                [0.0883024619, 0.2817188376, 0.6299787005]])
_M2 = np.array([[0.2104542553, 0.7936177850, -0.0040720468], [1.9779984951, -2.4285922050, 0.4505937099],
                [0.0259040371, 0.7827717662, -0.8086757660]])


def _oklab(lin):
    return np.cbrt(np.maximum(lin @ _M1.T, 0)) @ _M2.T


def _from_oklab(lab):
    return np.maximum((lab @ np.linalg.inv(_M2).T) ** 3, 0) @ np.linalg.inv(_M1).T


def upscale(a, k, wrap):
    """A 2-D float array k times larger, bilinear, its edges wrapped (a tiling texture) or held:
    each texel's value at its 4x block's centre."""
    h, w = a.shape
    p = np.pad(a.astype(np.float32), 1, mode='wrap' if wrap else 'edge')
    from PIL import Image
    big = np.asarray(Image.fromarray(p, 'F').resize(((w + 2) * k, (h + 2) * k), Image.BILINEAR), np.float64)
    return big[k:k + h * k, k:k + w * k]


def guided(guide, p, r, eps, wrap=False):
    """He, Sun and Tang's guided filter: p smoothed but following guide's edges (both 2-D)."""
    mi, mp = _blur(guide, r, wrap), _blur(p, r, wrap)
    cov = _blur(guide * p, r, wrap) - mi * mp
    var = _blur(guide * guide, r, wrap) - mi * mi
    a = cov / (var + eps)
    b = mp - a * mi
    return _blur(a, r, wrap) * guide + _blur(b, r, wrap)
