#!/usr/bin/env python3
"""Derive the host's per-build guest constants from the user's main.dol.

The host needs a handful of data addresses and structure offsets that differ
between builds (the PAL disc moves its small-data area, and this game's GX
library is older than the one the Strikers host was written for). Each one is
read from the instruction that uses it, in a function placed by symbols.py, so
the same code works for GEDE01 and GEDP01.

usage: gamedata.py <main.dol> <symbols.tsv> <out game_data.h>
"""
import struct
import sys


class Dol:
    def __init__(self, path):
        data = open(path, "rb").read()
        self.entry = struct.unpack(">I", data[0xE0:0xE4])[0]
        offs = struct.unpack(">18I", data[0:72])
        addrs = struct.unpack(">18I", data[72:144])
        sizes = struct.unpack(">18I", data[144:216])
        self.sections = [(o, b, s) for o, b, s in zip(offs, addrs, sizes) if s]
        self.data = data

    def word(self, a):
        for o, b, s in self.sections:
            if b <= a < b + s:
                return struct.unpack(">I", self.data[o + a - b:o + a - b + 4])[0]
        raise KeyError(f"0x{a:08X} is outside the DOL")


def dform(w):
    imm = w & 0xFFFF
    return w >> 26, (w >> 21) & 31, (w >> 16) & 31, imm - 0x10000 if imm & 0x8000 else imm


def branch_target(a, w):
    li = w & 0x03FFFFFC
    if li & 0x02000000:
        li -= 0x04000000
    return a + li


def registers(dol):
    """r2 and r13 from __init_registers, the first call of __start."""
    w = dol.word(dol.entry)
    init = branch_target(dol.entry, w)
    hi, value = {}, {}
    for i in range(16):
        op, rt, ra, simm = dform(dol.word(init + 4 * i))
        if op == 15 and ra == 0:
            hi[rt] = (simm & 0xFFFF) << 16
        elif op == 24 and rt in hi:  # ori rA, rS, imm (rS in the rt field)
            value[ra] = hi[rt] | (dol.word(init + 4 * i) & 0xFFFF)
    return value[2], value[13]


def gx_data(dol, gx_begin):
    """GXBegin: lwz rX, off(r2|r13) loads the GX state pointer, then lwz r0, dirty(rX)."""
    base = None
    for i in range(24):
        op, rt, ra, simm = dform(dol.word(gx_begin + 4 * i))
        if op == 32 and base is None and ra in (2, 13):
            base, reg, offset = ra, rt, simm
        elif op == 32 and base is not None and ra == reg and simm > 0x100:
            return base, offset, simm
    raise SystemExit("gamedata: GX state access not found in GXBegin")


def flush_prim_when_zero(dol, func, flush_prim):
    """GXCallDisplayList: lwz r0, 0(state); cmplwi r0, 0; then either bne over
    (flush when zero) or beq over (flush when non-zero) the bl __GXSendFlushPrim."""
    for i in range(32):
        a = func + 4 * i
        w = dol.word(a)
        if w >> 26 == 18 and (w & 1) and branch_target(a, w) == flush_prim:
            cond = dol.word(a - 4)
            bo = (cond >> 21) & 31
            return 1 if bo == 4 else 0   # bc 4,2 = bne: skips the flush unless zero
    raise SystemExit("gamedata: __GXSendFlushPrim call not found in GXCallDisplayList")


def r13_arg_before_call(dol, func, callee, r13):
    """addi r3, off(r13) right before bl callee inside func."""
    last = None
    for i in range(96):
        a = func + 4 * i
        w = dol.word(a)
        op, rt, ra, simm = dform(w)
        if op == 14 and rt == 3 and ra == 13:
            last = r13 + simm
        if op == 18 and (w & 1) and branch_target(a, w) == callee and last is not None:
            return last & 0xFFFFFFFF
    raise SystemExit("gamedata: queue argument not found")


def first_r13_store(dol, func, r13):
    for i in range(8):
        op, rt, ra, simm = dform(dol.word(func + 4 * i))
        if op == 36 and ra == 13:
            return (r13 + simm) & 0xFFFFFFFF
    raise SystemExit("gamedata: r13 store not found")


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    dol = Dol(sys.argv[1])
    sym = {}
    for line in open(sys.argv[2]):
        name, _usa, addr = line.split("\t")
        sym.setdefault(name, int(addr, 16))

    r2, r13 = registers(dol)
    gx_base, gx_offset, gx_dirty = gx_data(dol, sym["GXBegin"])
    finish_queue = r13_arg_before_call(dol, sym["GXDrawDone"], sym["OSSleepThread"], r13)
    dsp_done = first_r13_store(dol, sym["dspInitCallback"], r13)
    flush_zero = flush_prim_when_zero(dol, sym["GXCallDisplayList"], sym["__GXSendFlushPrim"])

    values = [
        ("ED_GUEST_R2", r2, "small-data base (r2), set by __init_registers"),
        ("ED_GUEST_R13", r13, "small-data base (r13), set by __init_registers"),
        ("ED_GX_DATA_BASE_REG", gx_base, "register GXBegin loads the GX state pointer through"),
        ("ED_GX_DATA_SDA_OFFSET", gx_offset, "its offset from that register (signed)"),
        ("ED_GX_DIRTY_STATE_OFFSET", gx_dirty, "dirty-state word in the GX state block"),
        ("ED_GX_FINISH_QUEUE", finish_queue, "FinishQueue, the OSThreadQueue GXDrawDone sleeps on"),
        ("ED_MUSYX_DSP_DONE", dsp_done, "salDspIsDone, set by dspInitCallback"),
        ("ED_GX_FLUSH_PRIM_WHEN_ZERO", flush_zero,
         "1: GX sends the flush primitive when the state block's first word is 0"),
    ]
    with open(sys.argv[3], "w") as f:
        f.write("// Generated by tools/gamedata.py from the user's main.dol -- do not edit.\n")
        f.write("#pragma once\n\n")
        for name, value, note in values:
            literal = f"({value})" if value < 0 else f"0x{value:08X}u"
            f.write(f"#define {name} {literal}  // {note}\n")
    for name, value, _ in values:
        print(f"gamedata: {name} = {value if value < 0 or value < 64 else hex(value)}")


if __name__ == "__main__":
    main()
