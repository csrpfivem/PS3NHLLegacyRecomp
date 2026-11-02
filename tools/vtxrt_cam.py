"""Parse [VTXRT] dumps; per draw: world pos of vertex 0 (c36-39), camera position
from view rows c0-c3 (row-vector convention: v = p*R + t), and clip of vertex 0."""
import re, sys
num = r'(-?[\d.]+(?:e[-+]?\d+)?|-?inf|nan)'
cre = re.compile(r'c(\d+)=\(' + ','.join([num]*4) + r'\)')
are = re.compile(r'a0\[[^\]]*\]=\(' + ','.join([num]*4) + r'\)')
fpre = re.compile(r'fp=0x([0-9A-F]+) vs=0x([0-9A-F]+)')
def f(x): return float(x)
cur = None; batch = None
for line in open(sys.argv[1], errors='replace'):
    if '[VTXRT] --- batch' in line:
        batch = line.split('t=')[1].strip(); print('=== batch', batch); continue
    if '[VTXRT] fp=' in line:
        m = are.search(line); fm = fpre.search(line)
        cur = (fm.group(1), fm.group(2), [f(x) for x in m.groups()] if m else None); continue
    if '[VTXRT]   consts' in line and cur:
        c = {int(m.group(1)): [f(x) for x in m.groups()[1:]] for m in cre.finditer(line)}
        fp, vs, p = cur; cur = None
        if p is None: print(fp, vs, 'no a0'); continue
        w = [p[0]*c[36][k] + p[1]*c[37][k] + p[2]*c[38][k] + c[39][k] for k in range(4)]
        R = [c[0][:3], c[1][:3], c[2][:3]]; t = c[3][:3]
        cam = [-sum(t[i]*R[j][i] for i in range(3)) for j in range(3)]
        v = [w[0]*R[0][k] + w[1]*R[1][k] + w[2]*R[2][k] + w[3]*t[k] for k in range(3)] + [1]
        clip = [sum(v[i]*c[4+i][k] for i in range(4)) for k in range(4)]
        ndc = [clip[0]/clip[3], clip[1]/clip[3]] if abs(clip[3]) > 1e-9 else ['inf', 'inf']
        print(f"fp={fp} vs={vs} pos=({p[0]:.1f},{p[1]:.1f},{p[2]:.1f}) world=({w[0]:.2f},{w[1]:.2f},{w[2]:.2f},{w[3]:.2f}) "
              f"cam=({cam[0]:.1f},{cam[1]:.1f},{cam[2]:.1f}) fwd=({R[0][2]:.2f},{R[1][2]:.2f},{R[2][2]:.2f}) "
              f"clipw={clip[3]:.2f} ndc=({ndc[0] if isinstance(ndc[0], str) else round(ndc[0], 2)},{ndc[1] if isinstance(ndc[1], str) else round(ndc[1], 2)}) "
              f"c4x={c[4][0]:.3g} c5y={c[5][1]:.3g}")
