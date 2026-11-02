# Changed LS ranges between consecutive SNAPs of one segment. usage: ranges.py <seg.bin> [gap]
import sys, struct
sys.path.insert(0, __import__('os').path.dirname(__file__))
from corpus import load
ch = load(sys.argv[1]); gap = int(sys.argv[2]) if len(sys.argv) > 2 else 32
snaps = [(st, d) for tag, st, d in ch if tag == 'SNAP']
rec = ch[0][2]
print('REC', rec[:0x20].hex(), '... 0x11C:', rec[0x11C:0x120].hex())
for (s0, a), (s1, b) in zip(snaps, snaps[1:]):
    la, lb = a[2048:], b[2048:]
    r = a[:2048]; regs = [struct.unpack_from('<4I', r, i * 16)[0] for i in range(3, 9)]
    rs = []; i = 0; n = len(la)
    idx = []
    for o in range(0, n, 16):
        if la[o:o+16] != lb[o:o+16]:
            idx += [o + k for k in range(16) if la[o + k] != lb[o + k]]
    if len(idx):
        start = prev = idx[0]
        for q in idx[1:]:
            if q - prev > gap: rs.append((start, prev)); start = q
            prev = q
        rs.append((start, prev))
    print(f'st {s0:2d}->{s1:2d} r3..r8 ' + ' '.join(f'{x:05X}' for x in regs) + f' | {len(idx)} B in {len(rs)} ranges: ' +
          ' '.join(f'{s:05X}-{e:05X}' for s, e in rs[:12]) + (' ...' if len(rs) > 12 else ''))
