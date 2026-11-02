# Unique stream descriptors across the corpus. usage: descs.py <dir>
import sys, struct, glob, collections
sys.path.insert(0, __import__('os').path.dirname(__file__))
from corpus import load, be32, be16
un = collections.Counter(); ex = {}
for f in sorted(glob.glob(sys.argv[1] + '/seg_*.bin')):
    ch = load(f); rec = ch[0][2]
    d = next(d for t, s, d in ch if t == 'SNAP' and s == 3)
    regs, ls = d[:2048], d[2048:]
    r3 = struct.unpack_from('<I', regs, 3 * 16)[0]
    ctx = ls[r3:r3 + 0x200]
    w = [be32(ctx, o) for o in range(0, 0x200, 4)]
    p60, p70 = w[0x6C // 4], w[0x7C // 4]
    pri = ls[p60:p60 + 0x40] if p60 else b''
    sec = ls[p70:p70 + 0x40] if p70 else b''
    key = (rec[:8].hex(), pri.hex(), sec.hex())
    un[key] += 1
    ex.setdefault(key, (f[-12:], be16(rec, 8), be16(rec, 10), [hex(x) for x in w[0x50 // 4:0x80 // 4]], rec[0x10:0x50].hex()))
for k, n in un.most_common():
    print(n, 'cfg', k[0]); print('  pri', k[1]); print('  sec', k[2]); print('  ex', ex[k][:3]); print('  ctx50', ex[k][3]); print('  rec10', ex[k][4])
