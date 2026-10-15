#!/usr/bin/env python3
"""Draws the Tome menu art into res/ui/tome/: parchment and leather textures,
the selection mark, the dividers and the leader dots. Procedural, so the
repository needs no third-party images: python3 tools/ui/make_tome_art.py"""
import os
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "res", "ui", "tome")
RED_INK = (142, 27, 18)
BROWN_INK = (122, 90, 48)


def smooth_noise(size, scale, rng):
    """Value noise: random cells blown up with a smooth filter."""
    w, h = size
    small = rng.random((max(2, h // scale), max(2, w // scale)))
    img = Image.fromarray((small * 255).astype(np.uint8), "L").resize((w, h), Image.BICUBIC)
    return np.asarray(img.filter(ImageFilter.GaussianBlur(scale / 4)), dtype=np.float32) / 255.0


def vignette(size, power):
    w, h = size
    y, x = np.mgrid[0:h, 0:w].astype(np.float32)
    dx = (x - w / 2) / (w / 2)
    dy = (y - h / 2) / (h / 2)
    return np.clip(np.sqrt(dx * dx * 0.9 + dy * dy), 0, 1.5) ** power


def parchment(rng):
    size = (1024, 1024)
    centre = np.array([230, 214, 176], np.float32)
    edge = np.array([176, 146, 94], np.float32)
    v = vignette(size, 2.2)[..., None]
    base = centre * (1 - v * 0.85) + edge * (v * 0.85)
    blotch = (smooth_noise(size, 96, rng) - 0.5)[..., None]
    mottle = (smooth_noise(size, 24, rng) - 0.5)[..., None]
    grain = (rng.random((size[1], size[0], 1)).astype(np.float32) - 0.5)
    img = base + blotch * 26 + mottle * 12 + grain * 10
    # Faint fibres: thin dark streaks along the page.
    fibres = Image.new("L", size, 0)
    d = ImageDraw.Draw(fibres)
    for _ in range(900):
        x, y = rng.integers(0, size[0]), rng.integers(0, size[1])
        length = rng.integers(8, 40)
        angle = rng.normal(0, 0.35)
        d.line([(x, y), (x + length * np.cos(angle), y + length * np.sin(angle))],
               fill=int(rng.integers(20, 60)), width=1)
    f = np.asarray(fibres.filter(ImageFilter.GaussianBlur(0.6)), np.float32)[..., None] / 255.0
    img = img * (1 - f * 0.18)
    return Image.fromarray(np.clip(img, 0, 255).astype(np.uint8), "RGB")


def leather(rng):
    size = (1024, 640)
    base = np.array([34, 22, 13], np.float32)
    v = vignette(size, 1.6)[..., None]
    pores = (smooth_noise(size, 6, rng) - 0.5)[..., None]
    folds = (smooth_noise(size, 64, rng) - 0.5)[..., None]
    img = base * (1.08 - v * 0.75) + pores * 14 + folds * 18
    return Image.fromarray(np.clip(img, 0, 255).astype(np.uint8), "RGB")


def antialiased(size, draw_fn, scale=4):
    big = Image.new("RGBA", (size[0] * scale, size[1] * scale), (0, 0, 0, 0))
    draw_fn(ImageDraw.Draw(big), scale)
    return big.resize(size, Image.LANCZOS)


def mark():
    def draw(d, s):
        c = 16 * s
        d.polygon([(c, 1 * s), (31 * s, c), (c, 31 * s), (1 * s, c)], fill=RED_INK + (255,))
        r = 3.6 * s
        d.ellipse([c - r, c - r, c + r, c + r], fill=(230, 214, 176, 255))
    return antialiased((32, 32), draw)


def divider():
    def draw(d, s):
        w, h = 600 * s, 36 * s
        cy = h / 2
        col = RED_INK + (220,)
        d.line([(0, cy), (w / 2 - 34 * s, cy)], fill=col, width=2 * s)
        d.line([(w / 2 + 34 * s, cy), (w, cy)], fill=col, width=2 * s)
        cx = w / 2
        d.polygon([(cx, cy - 15 * s), (cx + 15 * s, cy), (cx, cy + 15 * s), (cx - 15 * s, cy)],
                  outline=col, width=2 * s)
        r = 4 * s
        d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=col)
        for side in (-1, 1):
            x = cx + side * 26 * s
            d.ellipse([x - 2.5 * s, cy - 2.5 * s, x + 2.5 * s, cy + 2.5 * s], fill=col)
    return antialiased((600, 36), draw)


def rule():
    def draw(d, s):
        w, h = 240 * s, 24 * s
        cy = h / 2
        col = BROWN_INK + (210,)
        d.line([(0, cy), (w / 2 - 14 * s, cy)], fill=col, width=2 * s)
        d.line([(w / 2 + 14 * s, cy), (w, cy)], fill=col, width=2 * s)
        r = 6 * s
        d.ellipse([w / 2 - r, cy - r, w / 2 + r, cy + r], outline=col, width=2 * s)
    return antialiased((240, 24), draw)


def leader(colour, alpha):
    def draw(d, s):
        r = 1.6 * s
        cx, cy = 6 * s, 26 * s
        d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=colour + (alpha,))
    return antialiased((12, 32), draw)


def main():
    os.makedirs(OUT, exist_ok=True)
    rng = np.random.default_rng(1847)
    parchment(rng).save(os.path.join(OUT, "parchment.png"), optimize=True)
    leather(rng).save(os.path.join(OUT, "leather.png"), optimize=True)
    mark().save(os.path.join(OUT, "mark.png"))
    divider().save(os.path.join(OUT, "divider.png"))
    rule().save(os.path.join(OUT, "rule.png"))
    leader((90, 58, 32), 150).save(os.path.join(OUT, "leader.png"))
    leader(RED_INK, 170).save(os.path.join(OUT, "leader-red.png"))
    print("wrote", os.path.normpath(OUT))
    return 0


if __name__ == "__main__":
    sys.exit(main())
