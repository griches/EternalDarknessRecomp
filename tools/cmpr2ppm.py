#!/usr/bin/env python3
"""Decode a GameCube CMPR texture from a MEM1 dump: cmpr2ppm.py MEM1 PHYS W H OUT.ppm"""
import struct
import sys

mem = open(sys.argv[1], "rb").read()
base, w, h = int(sys.argv[2], 16), int(sys.argv[3]), int(sys.argv[4])
img = bytearray(w * h * 3)


def rgb(c):
    return ((c >> 11) & 31) * 255 // 31, ((c >> 5) & 63) * 255 // 63, (c & 31) * 255 // 31


off = base
for ty in range(0, h, 8):
    for tx in range(0, w, 8):
        for sub in range(4):
            c0, c1, bits = struct.unpack(">HHI", mem[off:off + 8])
            off += 8
            a, b = rgb(c0), rgb(c1)
            if c0 > c1:
                pal = [a, b, tuple((2 * x + y) // 3 for x, y in zip(a, b)), tuple((x + 2 * y) // 3 for x, y in zip(a, b))]
            else:
                pal = [a, b, tuple((x + y) // 2 for x, y in zip(a, b)), (0, 0, 0)]
            sx, sy = tx + (sub & 1) * 4, ty + (sub >> 1) * 4
            for py in range(4):
                for px in range(4):
                    idx = (bits >> (30 - 2 * (py * 4 + px))) & 3
                    x, y = sx + px, sy + py
                    if x < w and y < h:
                        img[(y * w + x) * 3:(y * w + x) * 3 + 3] = bytes(pal[idx])
with open(sys.argv[5], "wb") as f:
    f.write(b"P6\n%d %d\n255\n" % (w, h) + img)
