#!/usr/bin/env python3
"""List the BP/CP/XF register writes and draws of one frame of an ED_FIFO_DUMP:
fifo_regs.py DUMP FRAME"""
import sys

frame, want = 0, int(sys.argv[2])
writes = []
for line in open(sys.argv[1]):
    if line.startswith("---- present"):
        frame += 1
        continue
    if line.startswith("----") or "into RAM" in line or frame != want:
        continue
    _off, size, value = line.split()[:3]
    writes.append((int(size), int(value, 16)))
stream = b"".join(v.to_bytes(s, "big") for s, v in writes)
i, out = 0, []
while i < len(stream):
    op = stream[i]
    if op == 0x61:
        out.append("BP%02X=%06X" % (stream[i + 1], int.from_bytes(stream[i + 2:i + 5], "big"))); i += 5
    elif op == 0x08:
        out.append("CP%02X=%08X" % (stream[i + 1], int.from_bytes(stream[i + 2:i + 6], "big"))); i += 6
    elif op == 0x10:
        hdr = int.from_bytes(stream[i + 1:i + 5], "big"); n = (hdr >> 16) + 1
        out.append("XF%04X x%d" % (hdr & 0xFFFF, n)); i += 5 + 4 * n
    elif op in (0x20, 0x28, 0x30, 0x38):
        out.append("XFIDX%02X" % op); i += 5
    elif op == 0x40:
        out.append("CALLDL"); i += 9
    elif op in (0x00, 0x48):
        i += 1
    elif op >= 0x80:
        out.append("DRAW%02X n=%d" % (op, int.from_bytes(stream[i + 1:i + 3], "big")))
        break  # vertex size unknown here; stop at the first draw
    else:
        out.append("?%02X" % op); break
print(" ".join(out))
