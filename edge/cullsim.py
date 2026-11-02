# Apply the cull rules to the SPU's own vertex records (scratch table in SNAP 7) and show any triangle
# where the rules disagree with the SPU's index list. usage: cullsim.py <seg.bin>...
import sys, struct, os, math
sys.path.insert(0, os.path.dirname(__file__))
from corpus import load
for p in sys.argv[1:]:
    ch = load(p)
    s6 = next(d for t, s, d in ch if t == 'SNAP' and s == 6)
    s7 = next(d for t, s, d in ch if t == 'SNAP' and s == 7)
    regs, a = s6[:2048], s6[2048:]
    b = s7[2048:]
    ctx = struct.unpack_from('<I', regs, 3 * 16)[0]
    want = struct.unpack_from('<I', s7, 81 * 16)[0]
    if struct.unpack_from('<I', regs, 4 * 16)[0] == 0:
        print(os.path.basename(p), 'done, mode 0 (no cull)')
        continue
    be = lambda o: struct.unpack_from('>I', a, ctx + o)[0]
    ni =a[ctx + 0x190] << 8 | a[ctx + 0x191]
    idx, scr = be(0x50), be(0x10 + (a[ctx] & 15) * 4)
    step = 2.0 if a[ctx + 0x140] in (3, 4) else 1.0
    V = {}
    def vert(i):
        if i not in V:
            x, y, f = struct.unpack_from('>ffI', b, scr + i * 16)
            V[i] = (x, y, f, int(x * step + 0.5), int(y * step + 0.5))
        return V[i]
    out = []
    kept = [struct.unpack_from('>H', b, idx + i * 2)[0] for i in range(want)]
    n = 0
    for t in range(ni // 3):
        ids = [struct.unpack_from('>H', a, idx + (t * 3 + k) * 2)[0] for k in range(3)]
        A, B, C = [vert(i) for i in ids]
        al, an = A[2] & B[2] & C[2], A[2] | B[2] | C[2]
        area = (B[0] - A[0]) * (C[1] - A[1]) - (B[1] - A[1]) * (C[0] - A[0])
        drop = (al & 0xFC) != 0
        same = (A[3] == B[3] == C[3]) or (A[4] == B[4] == C[4])
        if (al & 2) and not (an & 4):
            drop = drop or same or area < 0
        spu_kept = kept[n:n + 3] == ids
        if spu_kept != (not drop):
            print(os.path.basename(p), 'tri', t, ids, 'spu', 'keep' if spu_kept else 'drop', 'rules', 'drop' if drop else 'keep', 'area %.9g same %s step %g' % (area, same, step))
            for v in (A, B, C):
                print('   %.6f %.6f flags %02X  grid %d %d  scaled %.6f %.6f' % (v[0], v[1], v[2], v[3], v[4], v[0] * step + .5, v[1] * step + .5))
            if not spu_kept: pass
        if spu_kept:
            n += 3
    print(os.path.basename(p), 'done, walked', n, 'of', want)
