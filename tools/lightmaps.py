# Eternal Darkness light maps, upscaled 4x into HD texture pack files.
#
#   python3 tools/lightmaps.py OUT_DIR LIST DUMP_DIR...
#
# The game does this itself as each room loads (runtime/host/lightmaps.cpp,
# Options > Game > Baked shadows); this makes the same images as pack files,
# for a machine that would rather not spend the time. Collect them by playing
# with DOL_GXCORE_UV2_LOG=1 (stderr lines naming each texture drawn through the
# second UV set: the light maps) and DOL_GXCORE_TEXTURE_DUMP=<dir> (their
# texels); LIST is a file of those [uv2] lines, the DUMP_DIRs those folders.
# LM_CONTRAST, LM_PASSES and LM_SHARPEN override the settings below.
#
# The rooms' baked light and shadow are CMPR atlases of a few texels per wall.
# Each is evened out (a bilateral pass takes out CMPR's 4x4 colour steps),
# doubled twice with directional cubic convolution (DCCI, Zhou, Fang and Lu
# 2012: along an edge it interpolates with the edge, not across it, so a
# shadow's stair-steps become a clean line while the original texels stay
# where they were), shifted to put texel centres where the GPU samples them,
# lightly sharpened, and written as the pack's tex1_<w>x<h>_<hash>_<fmt>.png.
import os
import sys

import numpy as np
from PIL import Image

CUBIC = np.array([-1.0, 9.0, 9.0, -1.0]) / 16.0


def blend(d1, d2, p1, p2, k=5.0, t=1.15):
    # p1 runs along direction 1, p2 along direction 2; d is each direction's
    # gradient: interpolate along the smoother one, blend where neither wins.
    w1 = 1.0 / (1.0 + d1 ** k)
    w2 = 1.0 / (1.0 + d2 ** k)
    out = (w1[..., None] * p1 + w2[..., None] * p2) / (w1 + w2)[..., None]
    a = (1.0 + d1) > t * (1.0 + d2)  # strong change along 1: an edge lies along 2
    b = (1.0 + d2) > t * (1.0 + d1)
    out = np.where(a[..., None], p2, out)
    out = np.where(b[..., None], p1, out)
    return out


def dcci2x(img):
    h, w, c = img.shape
    pad = 3
    src = np.pad(img, ((pad, pad), (pad, pad), (0, 0)), mode="edge")
    lum = src.mean(axis=2)
    H, W = 2 * h, 2 * w
    out = np.zeros((H + 2 * 2 * pad, W + 2 * 2 * pad, c))
    big_lum = np.zeros(out.shape[:2])
    out[::2, ::2] = src
    big_lum[::2, ::2] = lum

    def at(a, dy, dx):  # original-grid neighbours of each centre cell
        return a[pad - 1 + dy:pad - 1 + dy + h + 1, pad - 1 + dx:pad - 1 + dx + w + 1]

    # Diagonal points: centre of each 2x2 block of originals (i+.5, j+.5).
    # Gradients over the 4x4 window: down-right (d1) and down-left (d2).
    d1 = np.zeros((h + 1, w + 1))
    d2 = np.zeros((h + 1, w + 1))
    for y in range(-1, 2):
        for x in range(-1, 2):
            d1 += np.abs(at(lum, y + 1, x + 1) - at(lum, y, x))
            d2 += np.abs(at(lum, y + 1, x) - at(lum, y, x + 1))
    p1 = sum(CUBIC[k] * at(src, k - 1, k - 1) for k in range(4))       # down-right line
    p2 = sum(CUBIC[k] * at(src, k - 1, 2 - k) for k in range(4))       # down-left line
    diag = blend(d1 / 255.0 * 8, d2 / 255.0 * 8, p1, p2)
    # their places on the 2x grid: (2i+1, 2j+1) for cells starting at i, j
    ys = slice(2 * pad - 1, 2 * pad - 1 + 2 * (h + 1), 2)
    xs = slice(2 * pad - 1, 2 * pad - 1 + 2 * (w + 1), 2)
    out[ys, xs] = diag
    big_lum[ys, xs] = diag.mean(axis=2)

    # Edge points: (2i, 2j+1) and (2i+1, 2j) from their four neighbours at
    # distance 1 and 3 across and down (now all known, on a rotated grid).
    def fill(oy, ox):
        Y, X = out.shape[:2]
        yy = np.arange(oy + 4, Y - 4, 2)
        xx = np.arange(ox + 4, X - 4, 2)
        gy, gx = np.meshgrid(yy, xx, indexing="ij")
        L = big_lum
        dh = (np.abs(L[gy, gx - 1] - L[gy, gx + 1]) + np.abs(L[gy - 2, gx - 1] - L[gy - 2, gx + 1]) +
              np.abs(L[gy + 2, gx - 1] - L[gy + 2, gx + 1]) + np.abs(L[gy - 1, gx - 2] - L[gy - 1, gx]) +
              np.abs(L[gy - 1, gx] - L[gy - 1, gx + 2]) + np.abs(L[gy + 1, gx - 2] - L[gy + 1, gx]) +
              np.abs(L[gy + 1, gx] - L[gy + 1, gx + 2]))
        dv = (np.abs(L[gy - 1, gx] - L[gy + 1, gx]) + np.abs(L[gy - 1, gx - 2] - L[gy + 1, gx - 2]) +
              np.abs(L[gy - 1, gx + 2] - L[gy + 1, gx + 2]) + np.abs(L[gy - 2, gx - 1] - L[gy, gx - 1]) +
              np.abs(L[gy, gx - 1] - L[gy + 2, gx - 1]) + np.abs(L[gy - 2, gx + 1] - L[gy, gx + 1]) +
              np.abs(L[gy, gx + 1] - L[gy + 2, gx + 1]))
        O = out
        ph = sum(CUBIC[k] * O[gy, gx - 3 + 2 * k] for k in range(4))
        pv = sum(CUBIC[k] * O[gy - 3 + 2 * k, gx] for k in range(4))
        v = blend(dh / 255.0 * 8, dv / 255.0 * 8, ph, pv)
        out[gy, gx] = v
        big_lum[gy, gx] = v.mean(axis=2)

    fill(0, 1)
    fill(1, 0)
    o = 2 * pad
    return out[o:o + H, o:o + W]


def shift(img, axis, frac_pixels):
    # move content by -frac (a half pixel) with the same cubic
    n = img.shape[axis]
    pad = [(0, 0)] * 3
    pad[axis] = (2, 2)
    p = np.pad(img, pad, mode="edge")
    taps = [np.take(p, np.arange(k, k + n), axis=axis) for k in range(1, 5)]
    return sum(CUBIC[k] * taps[k] for k in range(4))


CONTRAST = float(os.environ.get("LM_CONTRAST", "6"))
PASSES = int(os.environ.get("LM_PASSES", "1"))
SHARPEN = float(os.environ.get("LM_SHARPEN", "1.2"))


def deblock(img, r=2, spatial=1.2, contrast=CONTRAST, passes=PASSES):
    # CMPR keeps four colours a 4x4 block, so the glows come out in flat steps
    # that an edge-keeping upscale would sharpen into contours. A bilateral
    # filter evens out differences of a few levels and leaves a shadow's edge.
    for _ in range(passes):
        h, w, _ = img.shape
        p = np.pad(img, ((r, r), (r, r), (0, 0)), mode="edge")
        lum = img.mean(axis=2)
        pl = np.pad(lum, r, mode="edge")
        acc = np.zeros_like(img)
        weights = np.zeros((h, w))
        for dy in range(-r, r + 1):
            for dx in range(-r, r + 1):
                n = p[r + dy:r + dy + h, r + dx:r + dx + w]
                nl = pl[r + dy:r + dy + h, r + dx:r + dx + w]
                wgt = np.exp(-(dx * dx + dy * dy) / (2 * spatial ** 2) -
                             (nl - lum) ** 2 / (2 * contrast ** 2))
                acc += wgt[..., None] * n
                weights += wgt
        img = acc / weights[..., None]
    return img


def sharpen(img, amount, sigma=1.5):
    # unsharp mask on the 4x image: a shadow's edge a little steeper
    if amount <= 0:
        return img
    r = 4
    x = np.arange(-r, r + 1)
    k = np.exp(-x * x / (2 * sigma * sigma))
    k /= k.sum()
    blur = img
    for axis in (0, 1):
        pad = [(0, 0)] * 3
        pad[axis] = (r, r)
        p = np.pad(blur, pad, mode="edge")
        n = img.shape[axis]
        blur = sum(k[i] * np.take(p, np.arange(i, i + n), axis=axis) for i in range(2 * r + 1))
    return img + amount * (img - blur)


def upscale4(rgba):
    img = deblock(rgba.astype(np.float64))
    big = sharpen(dcci2x(dcci2x(img)), SHARPEN)
    # The 4x grid holds texel i's value at 4i; the GPU wants texel centres
    # at 4i + 1.5: two whole pixels right and down, then back half a pixel
    # (shift samples half a pixel on).
    big = np.roll(big, 2, axis=0)
    big[:2] = big[2]
    big = np.roll(big, 2, axis=1)
    big[:, :2] = big[:, 2:3]
    big = shift(big, 0, 0.5)
    big = shift(big, 1, 0.5)
    return np.clip(np.rint(big), 0, 255).astype(np.uint8)


def main():
    out_dir = sys.argv[1]
    os.makedirs(out_dir, exist_ok=True)
    wanted = {line.split()[-1].split("_")[2] for line in open(sys.argv[2]) if "tex1_" in line}
    seen = set()
    for d in sys.argv[3:]:
        for name in sorted(os.listdir(d)):
            if not name.endswith(".rgba"):
                continue
            parts = name[:-5].split("_")
            fmt, (w, h), dhash = parts[0], map(int, parts[1].split("x")), parts[4][1:]
            if dhash not in wanted or dhash in seen:
                continue
            seen.add(dhash)
            raw = np.fromfile(os.path.join(d, name), dtype=np.uint8).reshape(h, w, 4)
            big = upscale4(raw)
            Image.fromarray(big, "RGBA").save(
                os.path.join(out_dir, f"tex1_{w}x{h}_{dhash}_{int(fmt[1:], 16)}.png"))
    print(f"{len(seen)} of {len(wanted)} light maps upscaled into {out_dir}")


if __name__ == "__main__":
    main()
