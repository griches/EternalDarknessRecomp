import struct, sys, hashlib
src, dst = sys.argv[1], sys.argv[2]
with open(src,'rb') as f, open(dst,'wb') as o:
    hdr = f.read(0x8000)
    assert hdr[:4] == b'CISO'
    bs = struct.unpack('<I', hdr[4:8])[0]
    m = hdr[8:]
    n = max(i for i,b in enumerate(m) if b) + 1
    zero = b'\0'*bs
    for i in range(n):
        o.write(f.read(bs) if m[i] else zero)
    o.truncate(1459978240)  # full GameCube disc size
print('blocks', n, 'bs', hex(bs))
