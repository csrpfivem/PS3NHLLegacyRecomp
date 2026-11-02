"""Summarise an rsxcap manifest's per-draw GPU queries (#108) for one render target.

usage: python -I qstats_sum.py <frame_dir>/manifest.txt [rt=0x02980000]

Per (VS hash, FP key, depth key, cull) group: draws, vertices in (ia), primitives
into/out of the clipper (cin/cout), pixel-shader invocations (ps) and samples that
passed depth/stencil (occ).  Reading it:
  ps == 0, cin > 0   -> rasterised nothing: culled, off-screen or degenerate
  ps > 0,  occ == 0  -> rasterised but every sample failed depth/stencil
  occ > 0, invisible -> passed; the colour write/blend/shader output is the issue
"""
import re, sys, collections

f = sys.argv[1]
rt = sys.argv[2] if len(sys.argv) > 2 else '0x02980000'
hdr = open(f, encoding='utf-8', errors='replace').readline().strip()
print(hdr)
g = collections.OrderedDict()
for line in open(f, encoding='utf-8', errors='replace'):
    if not line.startswith('op') or ' DRAW ' not in line or ('rt=' + rt) not in line:
        continue
    d = dict(re.findall(r'(\w+)=(\S+)', line))
    op = int(re.match(r'op(\d+)', line).group(1))
    ctl = int(d.get('ctl', '0'), 16)
    dk = (ctl >> 20) & 0x1F
    k = (d.get('vsh'), d.get('key'), dk, d.get('cull', '?'), d.get('cmask'), d.get('blend'))
    e = g.setdefault(k, {'n': 0, 'v': 0, 'ia': 0, 'cin': 0, 'cout': 0, 'ps': 0, 'occ': 0,
                         'noq': 0, 'zero_ps': 0, 'a': op, 'b': op})
    e['n'] += 1; e['v'] += int(d.get('n', 0)); e['b'] = op
    if 'ps' in d:
        for q in ('ia', 'cin', 'cout', 'ps', 'occ'):
            e[q] += int(d[q])
        if int(d['ps']) == 0:
            e['zero_ps'] += 1
    else:
        e['noq'] += 1
print(f'{"vsh":10} {"key":10} dk cull cm bl {"draws":>5} {"ia":>7} {"cin":>7} {"cout":>7} {"ps":>8} {"occ":>8} zps noq ops')
for (vsh, key, dk, cull, cm, bl), e in g.items():
    print(f'{vsh:10} {key:10} {dk:02X} {cull:>4} {cm:>2} {bl:>2} {e["n"]:5d} {e["ia"]:7d} {e["cin"]:7d} '
          f'{e["cout"]:7d} {e["ps"]:8d} {e["occ"]:8d} {e["zero_ps"]:3d} {e["noq"]:3d} {e["a"]}-{e["b"]}')
