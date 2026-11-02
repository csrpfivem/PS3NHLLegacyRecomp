# Rebuild the cull in Python from SNAP 6 and compare with the scratch vertex table + index list in
# SNAP 7. usage: cullcheck.py <seg.bin>...
import sys, struct, os
sys.path.insert(0, os.path.dirname(__file__))
from corpus import load
for p in sys.argv[1:]:
    ch = load(p)
    s6 = next(d for t, s, d in ch if t == 'SNAP' and s == 6)
    s7 = next(d for t, s, d in ch if t == 'SNAP' and s == 7)
    regs, a = s6[:2048], s6[2048:]
    b = s7[2048:]
    ctx = struct.unpack_from('<I', regs, 3 * 16)[0]
    r81 = struct.unpack_from('<I', s7, 81 * 16)[0]
    be = lambda o: struct.unpack_from('>I', a, ctx + o)[0]
    f = lambda buf, o: struct.unpack_from('>f', buf, o)[0]
    nv = a[ctx + 8] << 8 | a[ctx + 9]
    ni = a[ctx + 0x190] << 8 | a[ctx + 0x191]
    print(os.path.basename(p), 'nv', nv, 'ni', ni, 'kept', r81, 'ctx0', a[ctx:ctx + 16].hex(), 'aa', a[ctx + 0x140], 'fmt', a[ctx + 6] >> 4)
    print(' tables', [hex(be(0x10 + i * 4)) for i in range(5)], 'pos', hex(be(0x5C)), 'idx', hex(be(0x50)), 'free', hex(be(0x58)), '->', hex(struct.unpack_from('>I', b, ctx + 0x58)[0]))
    scratch = be(0x10 + (a[ctx] & 15) * 4)
    for v in (0, 1, 2, nv // 2):
        print(' v', v, 'pos', [round(f(a, be(0x5C) + v * 16 + k * 4), 4) for k in range(4)],
              'scratch', [round(f(b, scratch + v * 16 + k * 4), 3) for k in range(2)], b[scratch + v * 16 + 8:scratch + v * 16 + 16].hex())
    idx = be(0x50)
    print(' in ', [struct.unpack_from('>H', a, idx + i * 2)[0] for i in range(12)])
    print(' out', [struct.unpack_from('>H', b, idx + i * 2)[0] for i in range(12)])
    for o in (0xA0, 0xB0, 0xC0):
        print(f' ctx+{o:03X}: ' + a[ctx + o:ctx + o + 16].hex(' ', 4))
