#!/usr/bin/env python3
"""Name guest addresses: addr2sym.py [symbols.tsv] ADDR... (default generated/symbols.tsv)."""
import bisect
import sys

args = sys.argv[1:]
path = "generated/symbols.tsv"
if args and args[0].endswith(".tsv"):
    path = args.pop(0)
rows = sorted((int(line.split("\t")[2], 16), line.split("\t")[0]) for line in open(path))
starts = [a for a, _ in rows]
for arg in args:
    a = int(arg, 16)
    i = bisect.bisect_right(starts, a) - 1
    print(f"{a:08X}  {rows[i][1]}+0x{a - rows[i][0]:X}" if i >= 0 else f"{a:08X}  ?")
