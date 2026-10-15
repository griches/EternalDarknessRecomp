# ppcd.py <main.dol> <addr> [count]: minimal D-form/branch decoder
import struct, sys
dol = open(sys.argv[1],'rb').read()
offs = struct.unpack('>18I', dol[0:72]); addrs = struct.unpack('>18I', dol[72:144]); sizes = struct.unpack('>18I', dol[144:216])
def rd(a):
    for o,b,s in zip(offs,addrs,sizes):
        if s and b <= a < b+s: return struct.unpack('>I', dol[o+a-b:o+a-b+4])[0]
D = {14:'addi',15:'addis',32:'lwz',33:'lwzu',34:'lbz',36:'stw',37:'stwu',38:'stb',40:'lhz',44:'sth',48:'lfs',50:'lfd',24:'ori',10:'cmpli',11:'cmpi'}
a = int(sys.argv[2],16); n = int(sys.argv[3]) if len(sys.argv)>3 else 40
for i in range(n):
    w = rd(a); op = w>>26; rt=(w>>21)&31; ra=(w>>16)&31; imm = w & 0xffff; simm = imm-0x10000 if imm & 0x8000 else imm
    if op == 18:
        li = w & 0x03fffffc; li = li - 0x4000000 if li & 0x2000000 else li
        s = 'b%s 0x%08x' % ('l' if w&1 else '', a+li if not w&2 else li)
    elif w == 0x4E800020: s = 'blr'
    elif op in D: s = '%s r%d, %d(r%d)  [0x%x]' % (D[op], rt, simm, ra, imm)
    else: s = 'op%d' % op
    print('%08x %08x  %s' % (a, w, s)); a += 4
