# Same-method Deck table: match start = first 3 s in a row under 58 steps, then to the last second
# under 58 steps. usage: hzmatch.py <err.log> ...
import sys, re, os
rx = re.compile(r'\[HZ\] t=(\d+)s pres=(\d+).*?render_frame (\d+)ms.*?present (\d+)ms.*\| steps (\d+)')
for path in sys.argv[1:]:
    rows = [(int(m[1]), int(m[2]), int(m[3]), int(m[4]), int(m[5])) for m in map(rx.search, open(path, errors='replace')) if m]
    s = next((i for i in range(len(rows) - 2) if all(0 < rows[i + k][4] < 58 for k in range(3))), None)
    name = os.path.basename(path)[:10]
    if s is None:
        print(name, 'no match window'); continue
    e = max(i for i, r in enumerate(rows) if 0 < r[4] < 58) + 1
    w = rows[s:e]
    avg = lambda c, n: sum(r[c] for r in w[:n]) / min(n, len(w))
    out = [f'{name}: t={w[0][0]}-{w[-1][0]} ({len(w)} s) whole {avg(4, len(w)):.1f}']
    for n in (30, 66, 108):
        if len(w) >= n: out.append(f'{n}s {avg(4, n):.1f}')
    out.append(f'min {min(r[4] for r in w)} | presents {avg(1, len(w)):.1f} | render_frame {avg(2, len(w)):.0f} | present {avg(3, len(w)):.0f}')
    print(' | '.join(out))
