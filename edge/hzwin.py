# Steps over one time window. usage: hzwin.py <err.log> <t0> <t1> ...
import re, statistics, sys
rx = re.compile(r'\[HZ\] t=(\d+)s.*render_frame (\d+)ms.*\| steps (\d+)')
rows = [(int(m[1]), int(m[2]), int(m[3])) for m in map(rx.search, open(sys.argv[1], errors='replace')) if m]
for a, b in zip(sys.argv[2::2], sys.argv[3::2]):
    w = [r for r in rows if int(a) <= r[0] <= int(b)]
    s = [r[2] for r in w]
    print('t=%s-%s (%d s): steps mean %.1f median %.0f min %d, seconds under 46: %d | render_frame %.0f' % (
        a, b, len(w), statistics.mean(s), statistics.median(s), min(s), sum(v < 46 for v in s), statistics.mean(r[1] for r in w)))
