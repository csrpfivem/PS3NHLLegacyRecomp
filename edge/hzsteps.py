# [HZ] steps/presents per second, plus the match window (longest run of seconds with steps < 58).
# usage: hzsteps.py <err.log> [<err.log> ...]
import sys, re
rx = re.compile(r'\[HZ\] t=(\d+)s pres=(\d+).*?render_frame (\d+)ms.*\| steps (\d+)')
for path in sys.argv[1:]:
    rows = [(int(m[1]), int(m[2]), int(m[3]), int(m[4])) for m in map(rx.search, open(path, errors='replace')) if m]
    best = (0, 0); start = None
    for i, r in enumerate(rows + [(0, 0, 0, 99)]):
        if r[3] < 58 and r[3] > 0:
            if start is None: start = i
        else:
            if start is not None and i - start > best[1] - best[0]: best = (start, i)
            start = None
    w = rows[best[0]:best[1]]
    print(path.split('\\')[-1])
    print('  series:', ' '.join(str(r[3]) for r in rows))
    if w:
        n = len(w)
        print(f'  match t={w[0][0]}-{w[-1][0]} ({n} s): steps {sum(r[3] for r in w)/n:.1f} (min {min(r[3] for r in w)}), '
              f'presents {sum(r[1] for r in w)/n:.1f}, render_frame {sum(r[2] for r in w)/n:.0f} ms/s')
