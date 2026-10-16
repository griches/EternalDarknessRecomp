#!/usr/bin/env python3
"""Menu versions of the button and key icons for the parchment:
res/ui/glyphs/<set>/*.png -> res/ui/glyphs/ink/<set>/*.png. A button with
white in it becomes a white key with a rim of the menus' dark ink and its
cut-out letters in ink (white alone fades into the parchment, solid ink read
as black blots); coloured parts (face buttons, the d-pad's lit direction)
stay as they are, and a wholly coloured button keeps its colour with its
letter in ink. The in-game prompts keep the originals.
python3 tools/ui/make_ink_glyphs.py"""
import os

from PIL import Image, ImageChops, ImageDraw, ImageFilter

GLYPHS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "res", "ui", "glyphs")
INK = (42, 24, 16)  # menu.rcss's dark ink, #2a1810
PAPER = (250, 246, 236)  # the keys' white, a little warm for the parchment
SS = 4  # masks are made at 4x and scaled down, for smooth edges
RIM = 3  # rim width in source pixels (128 px icons, shown at 32dp)


def holes(alpha):
    """The transparent areas enclosed by the icon (a letter cut out of a
    button), grown by two pixels to take in their anti-aliased edges."""
    w, h = alpha.size
    mask = Image.new("L", (w + 2, h + 2), 0)
    mask.paste(alpha.point(lambda a: 255 if a >= 128 else 0), (1, 1))
    ImageDraw.floodfill(mask, (0, 0), 128)  # the outside
    inside = mask.crop((1, 1, w + 1, h + 1)).point(lambda v: 255 if v == 0 else 0)
    return inside.filter(ImageFilter.MaxFilter(5))


def solid(color, size):
    return Image.new("RGBA", size, color + (255,))


def ink(src, dst):
    im = Image.open(src).convert("RGBA")
    alpha = im.getchannel("A")
    px = im.load()
    coloured = Image.new("L", im.size, 0)
    cp = coloured.load()
    white = False
    for y in range(im.height):
        for x in range(im.width):
            r, g, b, a = px[x, y]
            if a == 0:
                continue
            if max(r, g, b) - min(r, g, b) <= 40:
                white = white or a >= 128
            else:
                cp[x, y] = a
    hole = holes(alpha)
    # The letters: where the button is cut away, inside it.
    letters = ImageChops.multiply(hole, ImageChops.invert(alpha))
    out = Image.new("RGBA", im.size, (0, 0, 0, 0))
    if white:
        # The key: its outline with the letters filled in, the paper inside a
        # rim of ink.
        big = (im.width * SS, im.height * SS)
        sil = ImageChops.lighter(alpha, hole).resize(big, Image.BICUBIC).point(
            lambda v: 255 if v >= 128 else 0)
        inner = sil.filter(ImageFilter.MinFilter(2 * RIM * SS + 1))
        sil = sil.resize(im.size, Image.LANCZOS)
        inner = inner.resize(im.size, Image.LANCZOS)
        out.paste(solid(INK, im.size), (0, 0), sil)
        out.paste(solid(PAPER, im.size), (0, 0), inner)
    out.paste(solid(INK, im.size), (0, 0), letters)
    # Coloured parts over the key; a coloured button's letter, cut out of it,
    # in ink (parchment through a yellow or green button reads poorly).
    top = im.copy()
    top.putalpha(coloured)
    out.alpha_composite(top)
    if not white:
        out.paste(solid(INK, im.size), (0, 0), letters)
    out.save(dst, optimize=True)


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
    print(f"menu glyphs written to {os.path.join(GLYPHS, 'ink')}")


if __name__ == "__main__":
    main()
