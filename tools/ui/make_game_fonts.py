#!/usr/bin/env python3
"""Redraws Eternal Darkness's two text font sheets at 4x from free fonts.

The game's text comes from UFonts.tpl: two 512x512 CMPR sheets of 32x32
cells, character c (Windows-1252) at column c>>4 and row 15-(c&15), each
glyph stored upside down. Sheet 0 is the plain font (white with a black
outline), sheet 1 the handwritten one (white), whose first two columns are
the GameCube button icons.

For each style this writes res/fonts/game/<style>-<sheet>.png (2048x2048, in
the sheet's own orientation) holding only our renders, and <style>-<sheet>.cells,
one '1' per character it redrew. Each glyph is fitted to the original glyph's
ink box (same left edge, width and baseline), so the game's spacing still
holds. The runtime (runtime/host/fonts.cpp) takes the button icons and every
cell not redrawn from the game's own sheet. The disc is read for metrics
only; nothing of it is written out except the sheets' signatures.

  python3 tools/ui/make_game_fonts.py [dump/GEDP01.iso] [--preview out.png]
"""
import argparse
import os
import struct
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
OUT = os.path.join(ROOT, "res", "fonts", "game")
FONTS = os.path.join(HERE, "fonts")
SCALE = 4
CELL = 32
# Cells that hold a picture rather than their character (sheet 1, 0x9C: a
# small block of text art in the place of "oe").
SKIP = {1: {0x9C}}

# style -> (sheet 0 font, sheet 1 font). (path, size-finding weight axis, outline)
STYLES = {
    "sharp": (
        (os.path.join(FONTS, "Arimo-Variable.ttf"), 600, True),
        (os.path.join(FONTS, "ReenieBeanie.ttf"), None, False),
    ),
    "tome": (
        (os.path.join(ROOT, "res", "fonts", "IMFellEnglish-Regular.ttf"), None, True),
        (os.path.join(FONTS, "LaBelleAurore.ttf"), None, False),
    ),
}


def read_disc_file(iso, name):
    with open(iso, "rb") as f:
        f.seek(0x424)
        fst_off, fst_size = struct.unpack(">II", f.read(8))
        f.seek(fst_off)
        fst = f.read(fst_size)
        count = struct.unpack_from(">I", fst, 8)[0]
        strings = fst[count * 12:]
        for i in range(1, count):
            w0, off, size = struct.unpack_from(">III", fst, i * 12)
            if w0 >> 24:
                continue
            o = w0 & 0xFFFFFF
            if strings[o:strings.index(b"\0", o)].decode("latin-1") == name:
                f.seek(off)
                return f.read(size)
    raise SystemExit(f"{name} not found on {iso}")


def rgb565(c):
    return ((c >> 11) & 31) * 255 // 31, ((c >> 5) & 63) * 255 // 63, (c & 31) * 255 // 31


def decode_cmpr(w, h, data):
    out = np.zeros((h, w, 4), np.uint8)
    i = 0
    for ty in range(0, h, 8):
        for tx in range(0, w, 8):
            for sb in range(4):
                sx, sy = tx + (sb & 1) * 4, ty + (sb >> 1) * 4
                c0, c1, bits = struct.unpack_from(">HHI", data, i)
                i += 8
                a, b = rgb565(c0), rgb565(c1)
                if c0 > c1:
                    pal = [a + (255,), b + (255,),
                           tuple((2 * x + y) // 3 for x, y in zip(a, b)) + (255,),
                           tuple((x + 2 * y) // 3 for x, y in zip(a, b)) + (255,)]
                else:
                    pal = [a + (255,), b + (255,),
                           tuple((x + y) // 2 for x, y in zip(a, b)) + (255,), (0, 0, 0, 0)]
                for y in range(4):
                    for x in range(4):
                        out[sy + y, sx + x] = pal[(bits >> (30 - 2 * (y * 4 + x))) & 3]
    return out


def sheets(iso):
    d = read_disc_file(iso, "UFonts.tpl")
    _, count, table = struct.unpack_from(">III", d, 0)
    result = []
    for k in range(count):
        image_header, _ = struct.unpack_from(">II", d, table + 8 * k)
        h, w, fmt, off = struct.unpack_from(">HHII", d, image_header)
        assert (w, h, fmt) == (512, 512, 14), (w, h, fmt)
        result.append(decode_cmpr(w, h, d[off:]))
    return result


def signature(sheet):
    """FNV-1a over the sheet's opaque/transparent mask, as stored (runtime twin
    in fonts.cpp): identifies the sheet whatever the colour decode rounds to."""
    h = 0xCBF29CE484222325
    for v in (sheet[..., 3] >= 128).astype(np.uint8).ravel():
        h = ((h ^ int(v)) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def upright_cell(sheet, code):
    up = sheet[::-1]
    col, row = code >> 4, code & 15
    return up[row * CELL:(row + 1) * CELL, col * CELL:(col + 1) * CELL]


def ink_box(cell):
    m = cell[..., 3] > 0
    ys, xs = np.nonzero(m)
    if len(xs) == 0:
        return None
    return xs.min(), ys.min(), xs.max(), ys.max()


def is_text_cell(cell):
    """White (and black outline) only: an icon or picture has colour."""
    px = cell[cell[..., 3] > 0][:, :3].astype(int)
    return len(px) > 0 and int(np.max(np.ptp(px, axis=1))) <= 24


def load_font(path, weight, size):
    font = ImageFont.truetype(path, size)
    if weight is not None:
        font.set_variation_by_axes([weight])
    return font


def render(font, ch, outline):
    """Glyph on a large canvas, baseline at y=BASE: (RGBA array, ink box)."""
    W = H = 512
    BASE = 340
    im = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    kw = dict(font=font, fill=(255, 255, 255, 255), anchor="ls")
    if outline:
        kw.update(stroke_width=SCALE, stroke_fill=(0, 0, 0, 255))
    d.text((64, BASE), ch, **kw)
    a = np.asarray(im)
    box = ink_box(a)
    return a, box, BASE


def missing(font, ch):
    probe = "￿"
    return font.getmask(ch).getbbox() is None or (
        list(font.getmask(ch)) == list(font.getmask(probe)))


def make(sheet, index, font_path, weight, outline):
    up_out = np.zeros((512 * SCALE, 512 * SCALE, 4), np.uint8)
    covered = ["0"] * 256
    h_box = ink_box(upright_cell(sheet, ord("H")))
    baseline = (h_box[3] + 1) * SCALE
    cap = (h_box[3] - h_box[1] + 1) * SCALE
    # Font size: the new 'H' as tall (with its outline) as the old one.
    lo, hi = 8, 400
    while lo < hi:
        mid = (lo + hi + 1) // 2
        _, box, _ = render(load_font(font_path, weight, mid), "H", outline)
        if box[3] - box[1] + 1 <= cap:
            lo = mid
        else:
            hi = mid - 1
    font = load_font(font_path, weight, lo)
    for code in range(0x21, 0x100):
        try:
            ch = bytes([code]).decode("cp1252")
        except UnicodeDecodeError:
            continue
        cell = upright_cell(sheet, code)
        box = ink_box(cell)
        if (box is None or code in SKIP.get(index, ()) or not is_text_cell(cell)
                or missing(font, ch)):
            continue
        glyph, gbox, base = render(font, ch, outline)
        if gbox is None:
            continue
        gx0, gy0, gx1, gy1 = gbox
        crop = Image.fromarray(glyph[gy0:gy1 + 1, gx0:gx1 + 1])
        tx0, tx1 = box[0] * SCALE, (box[2] + 1) * SCALE
        want = tx1 - tx0
        have = gx1 - gx0 + 1
        # Fit the width to the old glyph's (the game spaces by its own
        # widths); past a sensible stretch, keep the shape and centre it.
        sx = min(max(want / have, 0.7), 1.45)
        new_w = max(1, round(have * sx))
        crop = crop.resize((new_w, crop.size[1]), Image.LANCZOS)
        x = tx0 + (want - new_w) // 2
        y = baseline - (base - gy0)
        canvas = Image.new("RGBA", (CELL * SCALE, CELL * SCALE), (0, 0, 0, 0))
        canvas.paste(crop, (x, y), crop)  # clipped to the cell
        col, row = code >> 4, code & 15
        up_out[row * CELL * SCALE:(row + 1) * CELL * SCALE,
               col * CELL * SCALE:(col + 1) * CELL * SCALE] = np.asarray(canvas)
        covered[code] = "1"
    return up_out[::-1].copy(), "".join(covered)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("iso", nargs="?", default=os.path.join(ROOT, "dump", "GEDP01.iso"))
    ap.add_argument("--preview", help="write a before/after sheet image here")
    args = ap.parse_args()
    originals = sheets(args.iso)
    os.makedirs(OUT, exist_ok=True)
    with open(os.path.join(OUT, "signatures.txt"), "w") as f:
        for k, s in enumerate(originals):
            f.write(f"{k} {signature(s):016x}\n")
    previews = []
    for style, fonts in STYLES.items():
        for k, (path, weight, outline) in enumerate(fonts):
            img, cells = make(originals[k], k, path, weight, outline)
            Image.fromarray(img).save(os.path.join(OUT, f"{style}-{k}.png"), optimize=True)
            with open(os.path.join(OUT, f"{style}-{k}.cells"), "w") as f:
                f.write(cells + "\n")
            print(f"{style}-{k}: {cells.count('1')} glyphs")
            previews.append((style, k, img))
    if args.preview:
        tiles = []
        for k, s in enumerate(originals):
            big = Image.fromarray(s[::-1]).resize((1024, 1024), Image.NEAREST)
            tiles.append(big)
            for style, kk, img in previews:
                if kk == k:
                    tiles.append(Image.fromarray(img[::-1]).resize((1024, 1024), Image.LANCZOS))
        sheet = Image.new("RGB", (1024 * 3, 1024 * 2), (70, 40, 40))
        for i, t in enumerate(tiles):
            bg = Image.new("RGBA", t.size, (70, 40, 40, 255))
            bg.alpha_composite(t.convert("RGBA"))
            sheet.paste(bg.convert("RGB"), ((i % 3) * 1024, (i // 3) * 1024))
        sheet.save(args.preview)
    return 0


if __name__ == "__main__":
    sys.exit(main())
