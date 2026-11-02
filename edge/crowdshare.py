# Crowd share of the draws in frame captures. usage: crowdshare.py <capture dir> ...
# Crowd = FP 0x0F39B481 / 0x0F39BB01 (one draw per crowd figure, notes: CROWD-SHOT SLOWDOWN).
import os, re, sys, collections
CROWD = {'0x0F39B481', '0x0F39BB01'}
rx = re.compile(r'DRAW .*?fp=(0x[0-9A-Fa-f]+).*? n=(\d+)')
for cap in sys.argv[1:]:
    rows = []
    for d in sorted(os.listdir(cap), key=lambda s: int(re.sub(r'\D', '', s) or 0)):
        m = os.path.join(cap, d, 'manifest.txt')
        if not os.path.isfile(m): continue
        draws = crowd = idx = cidx = 0; fps = collections.Counter()
        for line in open(m, errors='replace'):
            x = rx.search(line)
            if not x: continue
            n = int(x[2]); draws += 1; idx += n; fps[x[1]] += 1
            if x[1] in CROWD: crowd += 1; cidx += n
        rows.append((d, draws, crowd, idx, cidx, fps.most_common(2)))
    print(os.path.basename(cap.rstrip('\\/')), len(rows), 'frames')
    for r in rows:
        print('  %-10s draws %6d crowd %6d (%4.1f%%)  indices %9d crowd %8d (%4.1f%%)  top %s' % (
            r[0], r[1], r[2], 100 * r[2] / max(r[1], 1), r[3], r[4], 100 * r[4] / max(r[3], 1), r[5]))
    t = [sum(r[i] for r in rows) for i in (1, 2, 3, 4)]
    if t[0]: print('  TOTAL draws %d crowd %d (%.1f%%), indices %.1f%% crowd' % (t[0], t[1], 100 * t[1] / t[0], 100 * t[3] / max(t[2], 1)))
