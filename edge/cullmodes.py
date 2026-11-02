# Which cull setups does the corpus hit? usage: cullmodes.py <seg.bin>...
import sys, struct, collections
sys.path.insert(0, __import__('os').path.dirname(__file__))
from corpus import load
seen = collections.Counter()
ex = {}
totals = [0, 0, 0]
for p in sys.argv[1:]:
    ch = load(p)
    s6 = next((d for t, s, d in ch if t == 'SNAP' and s == 6), None)
    s7 = next((d for t, s, d in ch if t == 'SNAP' and s == 7), None)
    if s6 is None or s7 is None:
        seen['no snap'] += 1
        continue
    regs = s6[:2048]; ls = s6[2048:]
    r = [struct.unpack_from('<I', regs, i * 16)[0] for i in range(8)]
    ctx = r[3]
    c = ls[ctx:ctx + 0x200]
    be = lambda o: struct.unpack_from('>I', c, o)[0]
    key = (r[4], r[5], r[6], r[7], c[3], c[12], c[13], c[0x14D], be(0x184), be(0x180))
    seen[key] += 1
    ex.setdefault(key, p)
    ni = c[10] << 8 | c[11]
    nv = c[8] << 8 | c[9]
    r7regs = s7[:2048]
    ret = struct.unpack_from('<I', r7regs, 3 * 16)[0]
    totals[0] += ni; totals[1] += nv
for k, n in seen.most_common():
    print(n, k if isinstance(k, str) else 'r4=%X r5=%X r6=%X r7=%X c3=%02X c12=%02X c13=%02X aa=%02X cb=%08X %08X' % k)
print('indexes', totals[0], 'verts', totals[1])
d = next(d for t, s, d in load(sys.argv[1]) if t == 'SNAP' and s == 6)
ctx = struct.unpack_from('<I', d, 3 * 16)[0]
ls = d[2048:]
for o in (0xD0, 0xE0, 0xF0, 0x100, 0x110, 0x120, 0x130, 0x140, 0x180, 0x190):
    print(f'ctx+{o:03X}: ' + ls[ctx + o:ctx + o + 16].hex(' ', 4))
for a, n in ((0xB520, 0x50), (0xB570, 0x40), (0xBB70, 0x10), (0xBBD0, 0x20), (0xBC00, 0x20), (0xBC30, 0x10), (0xBCB0, 0x10), (0xBD20, 0x20), (0xBD50, 0x40)):
    for o in range(a, a + n, 16):
        print(f'{o:05X}: ' + ls[o:o + 16].hex(' ', 4))
