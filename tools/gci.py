# Memory-card saves in and out of the game's card file, as GCI (the format
# Dolphin and save-sharing sites use: a 64-byte directory entry, then the
# file's blocks).
#
#   python3 tools/gci.py list CARD
#   python3 tools/gci.py export CARD OUT_DIR       every save, as OUT_DIR/<game>-<name>.gci
#   python3 tools/gci.py import CARD FILE...       adds them, replacing a save of the same
#                                                  game and name (the card is copied to
#                                                  CARD.bak first); FILE is a .gci, a
#                                                  MaxDrive .sav or a GameShark .gcs
#   python3 tools/gci.py import --as GEDP01 CARD FILE...
#                                                  the same, retagged for another region's
#                                                  disc (the save data is left as it is)
#   python3 tools/gci.py new CARD                  an empty 4 Mbit card
#
# CARD is the GXRuntime card container (ed-GEDP01-slot-a.dolcard): "DOLCARD1",
# a 40-byte header, then per file a 68-byte record and its bytes, with FNV-1a
# hashes over each file and over the body (GXRuntime src/memory_card.c).
import os
import shutil
import struct
import sys

SECTOR = 8192
MAX_FILES = 127
SYSTEM_BLOCKS = 5


def fnv1a(data):
    h = 2166136261
    for b in data:
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return h


def read_card(path):
    raw = open(path, "rb").read()
    if raw[:8] != b"DOLCARD1":
        raise SystemExit(f"{path}: not a card container")
    version, size_mbits, encoding, sector, serial, count, body = struct.unpack_from(
        ">IHHIQII", raw, 8)
    if version != 1 or sector != SECTOR or body != len(raw) - 40:
        raise SystemExit(f"{path}: unexpected header")
    if struct.unpack_from(">I", raw, 36)[0] != fnv1a(raw[40:]):
        raise SystemExit(f"{path}: body hash does not match")
    files = []
    at = 40
    for _ in range(count):
        file_no, length, mtime = struct.unpack_from(">H2xII", raw, at)
        name = raw[at + 12:at + 44]
        game, company = raw[at + 44:at + 48], raw[at + 48:at + 50]
        banner, perm = raw[at + 50], raw[at + 51]
        icon_addr, icon_fmt, icon_speed, comment_addr, hsh = struct.unpack_from(
            ">IHHII", raw, at + 52)
        data = raw[at + 68:at + 68 + length]
        if fnv1a(data) != hsh:
            raise SystemExit(f"{path}: file {file_no} hash does not match")
        files.append(dict(no=file_no, length=length, time=mtime, name=name, game=game,
                          company=company, banner=banner, perm=perm, icon_addr=icon_addr,
                          icon_fmt=icon_fmt, icon_speed=icon_speed, comment_addr=comment_addr,
                          data=data))
        at += 68 + length
    return dict(size_mbits=size_mbits, encoding=encoding, serial=serial, files=files)


def write_card(path, card):
    body = b""
    for f in sorted(card["files"], key=lambda f: f["no"]):
        body += struct.pack(">H2xII", f["no"], len(f["data"]), f["time"])
        body += f["name"].ljust(32, b"\0")[:32] + f["game"] + f["company"]
        body += bytes([f["banner"], f["perm"]])
        body += struct.pack(">IHHII", f["icon_addr"], f["icon_fmt"], f["icon_speed"],
                            f["comment_addr"], fnv1a(f["data"]))
        body += f["data"]
    head = b"DOLCARD1" + struct.pack(">IHHIQII", 1, card["size_mbits"], card["encoding"],
                                     SECTOR, card["serial"], len(card["files"]), len(body))
    head += struct.pack(">I", fnv1a(body))
    tmp = path + ".tmp"
    with open(tmp, "wb") as out:
        out.write(head + body)
    os.replace(tmp, path)


def name_of(f):
    return f["name"].split(b"\0")[0].decode("latin-1")


def to_gci(f):
    blocks = (len(f["data"]) + SECTOR - 1) // SECTOR
    data = f["data"].ljust(blocks * SECTOR, b"\0")
    entry = f["game"] + f["company"] + b"\xff" + bytes([f["banner"]])
    entry += f["name"].ljust(32, b"\0")[:32]
    entry += struct.pack(">IIHHBBHHHI", f["time"], f["icon_addr"], f["icon_fmt"],
                         f["icon_speed"], f["perm"], 0, 0, blocks, 0xFFFF, f["comment_addr"])
    return entry + data


def from_gci(raw):
    if len(raw) < 64 or (len(raw) - 64) % SECTOR:
        raise SystemExit("not a GCI file (64-byte header plus whole 8 KiB blocks)")
    game, company, banner = raw[0:4], raw[4:6], raw[7]
    name = raw[8:40]
    mtime, icon_addr, icon_fmt, icon_speed, perm, _copies, _first, blocks, _pad, comment_addr = \
        struct.unpack_from(">IIHHBBHHHI", raw, 40)
    if blocks * SECTOR != len(raw) - 64:
        raise SystemExit("GCI block count does not match its size")
    return dict(no=-1, time=mtime, name=name, game=game, company=company, banner=banner,
                perm=perm, icon_addr=icon_addr, icon_fmt=icon_fmt, icon_speed=icon_speed,
                comment_addr=comment_addr, data=raw[64:])


def from_file(raw):
    # MaxDrive (.sav): a 128-byte header, then the directory entry with bytes 6
    # and 7 swapped and 0x2C-0x3F in swapped byte pairs. GameShark (.gcs): a
    # 0x110-byte header, then the entry as it is.
    if raw.startswith(b"DATELGC_SAVE"):
        entry = bytearray(raw[0x80:0xC0])
        entry[6], entry[7] = entry[7], entry[6]
        for i in range(0x2C, 0x40, 2):
            entry[i], entry[i + 1] = entry[i + 1], entry[i]
        raw = bytes(entry) + raw[0xC0:]
    elif raw.startswith(b"GCSAVE"):
        raw = raw[0x110:]
    return from_gci(raw)


def main():
    args = sys.argv[1:]
    retag = None
    if "--as" in args:
        i = args.index("--as")
        retag = args[i + 1].encode("latin-1")
        del args[i:i + 2]
        if len(retag) != 6:
            raise SystemExit("--as takes a 6-letter game ID, such as GEDP01")
    if len(args) < 2 or args[0] not in ("list", "export", "import", "new"):
        raise SystemExit("usage: gci.py list|export|import|new CARD ... (see the top of the file)")
    cmd, card_path = args[0], args[1]
    if cmd == "new":
        if os.path.exists(card_path):
            raise SystemExit(f"{card_path} exists")
        write_card(card_path, dict(size_mbits=4, encoding=0,
                                   serial=int.from_bytes(os.urandom(8), "big"), files=[]))
        return
    card = read_card(card_path)
    if cmd == "list":
        used = sum((len(f["data"]) + SECTOR - 1) // SECTOR for f in card["files"])
        print(f"{card_path}: {card['size_mbits']} Mbit, {len(card['files'])} files, "
              f"{used} of {card['size_mbits'] * 16 - SYSTEM_BLOCKS} blocks")
        for f in card["files"]:
            print(f"  {f['game'].decode('latin-1')}{f['company'].decode('latin-1')} "
                  f"{name_of(f)!r} {len(f['data'])} bytes")
    elif cmd == "export":
        os.makedirs(args[2], exist_ok=True)
        for f in card["files"]:
            out = os.path.join(args[2], f"{f['game'].decode('latin-1')}-{name_of(f)}.gci")
            open(out, "wb").write(to_gci(f))
            print(out)
    else:
        shutil.copy2(card_path, card_path + ".bak")
        for path in args[2:]:
            new = from_file(open(path, "rb").read())
            if retag is not None:
                new["game"], new["company"] = retag[:4], retag[4:]
            card["files"] = [f for f in card["files"]
                             if not (f["game"] == new["game"] and f["name"] == new["name"])]
            taken = {f["no"] for f in card["files"]}
            new["no"] = next(i for i in range(MAX_FILES) if i not in taken)
            card["files"].append(new)
            print(f"{path}: imported as file {new['no']} ({name_of(new)!r})")
        used = sum((len(f["data"]) + SECTOR - 1) // SECTOR for f in card["files"])
        if used > card["size_mbits"] * 16 - SYSTEM_BLOCKS:
            raise SystemExit("not enough room on the card (it is unchanged)")
        write_card(card_path, card)


if __name__ == "__main__":
    main()
