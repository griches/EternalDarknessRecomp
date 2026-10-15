#!/usr/bin/env python3
"""Turn the user's Eternal Darkness disc image into recompiled C.

  1. Read the disc header and check it is Eternal Darkness (GEDE01 or GEDP01).
  2. Extract main.dol (plain ISO/GCM, or a CISO).
  3. Build DolRecomp if needed and translate main.dol into generated/.
  4. Apply the constant-time dispatch fix (tools/fix_generated.py).
  5. Build the symbol table the host's intercepts use (tools/symbols.py).
  6. Derive the per-build guest constants (tools/gamedata.py).

Nothing here is committed: generated/ holds code translated from the user's disc.

usage: generate.py <disc image> [--out generated] [--jobs N]
"""
import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

from fix_generated import fix_generated_sources

ROOT = Path(__file__).resolve().parent.parent
GAME_IDS = {b"GEDE01": "USA", b"GEDP01": "Europe"}


class Disc:
    """Random access to a plain ISO/GCM or a CISO image."""

    def __init__(self, path):
        self.f = open(path, "rb")
        head = self.f.read(0x8000)
        self.ciso = head[:4] == b"CISO"
        if self.ciso:
            self.block = struct.unpack("<I", head[4:8])[0]
            present = head[8:]
            self.index, n = [], 0
            for flag in present:
                self.index.append(n if flag else None)
                n += 1 if flag else 0

    def read(self, offset, size):
        if not self.ciso:
            self.f.seek(offset)
            return self.f.read(size)
        out = bytearray()
        while size:
            blk, inner = divmod(offset, self.block)
            take = min(size, self.block - inner)
            slot = self.index[blk] if blk < len(self.index) else None
            if slot is None:
                out += bytes(take)
            else:
                self.f.seek(0x8000 + slot * self.block + inner)
                out += self.f.read(take)
            offset += take
            size -= take
        return bytes(out)


def extract_dol(disc_path, out_path):
    disc = Disc(disc_path)
    game_id = disc.read(0, 6)
    if game_id not in GAME_IDS:
        sys.exit(f"error: {disc_path} is {game_id!r}, not Eternal Darkness (GEDE01/GEDP01)")
    dol_off = struct.unpack(">I", disc.read(0x420, 4))[0]
    head = disc.read(dol_off, 0x100)
    offs = struct.unpack(">18I", head[0:72])
    sizes = struct.unpack(">18I", head[144:216])
    end = max(o + s for o, s in zip(offs, sizes) if s)
    out_path.write_bytes(disc.read(dol_off, end))
    print(f"disc: {game_id.decode()} ({GAME_IDS[game_id]}), main.dol {end} bytes")
    return game_id.decode()


def dolrecomp_binary():
    src = ROOT / ".deps" / "DolRecomp"
    binary = src / "build" / "dolrecomp"
    if not binary.is_file():
        subprocess.run(["cmake", "-S", src, "-B", src / "build", "-G", "Ninja",
                        "-DCMAKE_BUILD_TYPE=Release"], check=True)
        subprocess.run(["ninja", "-C", src / "build", "dolrecomp"], check=True)
    return binary


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("disc", type=Path)
    ap.add_argument("--out", type=Path, default=ROOT / "generated")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    args = ap.parse_args()
    out = args.out.resolve()

    with tempfile.TemporaryDirectory(prefix="ed_gen_") as tmp:
        tmp = Path(tmp)
        dol = tmp / "main.dol"
        game_id = extract_dol(args.disc, dol)
        subprocess.run([dolrecomp_binary(), "--gamecube", dol, tmp, "-j", str(args.jobs)],
                       check=True, stdout=subprocess.DEVNULL)
        if out.exists():
            shutil.rmtree(out)
        shutil.move(str(tmp / "generated"), str(out))
        shutil.copy2(dol, out / "main.dol")
    (out / "game_id.txt").write_text(game_id + "\n")
    print(f"dispatch fixes: {fix_generated_sources(out)}")
    subprocess.run([sys.executable, ROOT / "tools" / "symbols.py",
                    ROOT / "config" / "GEDE01" / "symbols.txt", out / "main.dol", out], check=True)
    subprocess.run([sys.executable, ROOT / "tools" / "gamedata.py",
                    out / "main.dol", out / "symbols.tsv", out / "game_data.h"], check=True)
    print(f"done: {out}")


if __name__ == "__main__":
    main()
