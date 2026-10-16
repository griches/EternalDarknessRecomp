#!/usr/bin/env python3
"""Ink versions of the button and key icons for the menus' parchment:
res/ui/glyphs/<set>/*.png -> res/ui/glyphs/ink/<set>/*.png. The white parts
of each icon become the menus' dark ink and coloured parts (face buttons,
the d-pad's lit direction) stay as they are, so the icons need no plate
behind them and their cut-out letters show the parchment. The in-game
prompts keep the white originals. python3 tools/ui/make_ink_glyphs.py"""
import os

from PIL import Image, ImageDraw, ImageFilter

GLYPHS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "res", "ui", "glyphs")
INK = (42, 24, 16)  # menu.rcss's dark ink, #2a1810


def holes(im):
    """The transparent areas enclosed by the icon (a letter cut out of a
    button), grown by two pixels to take in their anti-aliased edges."""
    w, h = im.size
    alpha = im.getchannel("A")
    mask = Image.new("L", (w + 2, h + 2), 0)
    mask.paste(alpha.point(lambda a: 255 if a >= 128 else 0), (1, 1))
    ImageDraw.floodfill(mask, (0, 0), 128)  # the outside
    inside = mask.crop((1, 1, w + 1, h + 1)).point(lambda v: 255 if v == 0 else 0)
    return inside.filter(ImageFilter.MaxFilter(5))


def ink(src, dst):
    im = Image.open(src).convert("RGBA")
    px = im.load()
    coloured = 0
    for y in range(im.height):
        for x in range(im.width):
            r, g, b, a = px[x, y]
            if a == 0:
                px[x, y] = (0, 0, 0, 0)
            elif max(r, g, b) - min(r, g, b) <= 40:
                px[x, y] = INK + (a,)  # white (or grey): ink, edges keep their alpha
            elif a >= 128:
                coloured += 1
    # A coloured button's letter, cut out of it, is drawn in ink: parchment
    # through a yellow or green button reads poorly.
    if coloured:
        hole = holes(im).load()
        for y in range(im.height):
            for x in range(im.width):
                if hole[x, y]:
                    r, g, b, a = px[x, y]
                    k = a / 255.0
                    px[x, y] = (round(r * k + INK[0] * (1 - k)), round(g * k + INK[1] * (1 - k)),
                                round(b * k + INK[2] * (1 - k)), 255)
    im.save(dst, optimize=True)


def main():
    for name in sorted(os.listdir(GLYPHS)):
        src_dir = os.path.join(GLYPHS, name)
        if name == "ink" or not os.path.isdir(src_dir):
            continue
        dst_dir = os.path.join(GLYPHS, "ink", name)
        os.makedirs(dst_dir, exist_ok=True)
        for f in sorted(os.listdir(src_dir)):
            if f.endswith(".png"):
                ink(os.path.join(src_dir, f), os.path.join(dst_dir, f))
    print(f"ink glyphs written to {os.path.join(GLYPHS, 'ink')}")


if __name__ == "__main__":
    main()
