# Draws per 5 s ([GPUVTX]) next to the [HZ] second and steps. usage: drawrate.py <err.log> [t0 t1]
import re, sys
t0 = int(sys.argv[2]) if len(sys.argv) > 2 else 0
t1 = int(sys.argv[3]) if len(sys.argv) > 3 else 1 << 30
t = steps = 0
for line in open(sys.argv[1], errors='replace'):
    m = re.search(r'\[HZ\] t=(\d+)s.*\| steps (\d+)', line)
    if m: t, steps = int(m[1]), int(m[2])
    m = re.search(r'\[GPUVTX\] 5 s: (\d+) draws, (\d+) indices', line)
    if m and t0 <= t <= t1:
        print('t=%-4d steps %-3d draws/5s %7s indices/5s %10s' % (t, steps, m[1], m[2]))
