#!/usr/bin/env python3
"""Walk a guest stack in a MEM1 dump: backchain.py MEM1.bin SP [symbols.tsv]."""
import bisect
import struct
import subprocess
import sys

mem = open(sys.argv[1], "rb").read()
sp = int(sys.argv[2], 16)
tsv = sys.argv[3] if len(sys.argv) > 3 else "generated/symbols.tsv"
rows = sorted((int(l.split("\t")[2], 16), l.split("\t")[0]) for l in open(tsv))
starts = [a for a, _ in rows]


def rd(a):
    return struct.unpack(">I", mem[a - 0x80000000:a - 0x80000000 + 4])[0]


def name(a):
    i = bisect.bisect_right(starts, a) - 1
    return f"{rows[i][1]}+0x{a - rows[i][0]:X}" if i >= 0 else "?"


for _ in range(24):
    caller = rd(sp)
    if not 0x80000000 <= caller < 0x81800000 or caller <= sp:
        break
    ret = rd(caller + 4)
    print(f"sp=0x{sp:08X} ret=0x{ret:08X} {name(ret)}")
    sp = caller
