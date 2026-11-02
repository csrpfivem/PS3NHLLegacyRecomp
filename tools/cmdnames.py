"""List render-command vtables in [lo, hi): slot2 GetName -> string, slot5 Execute,
and mark whether Execute appears in the [icall] histogram of a run log.
usage: cmdnames.py <elf> <lifted dir> <log> <lo hex> <hi hex>"""
import struct, sys, re, os
elf, ldir, log = sys.argv[1], sys.argv[2], sys.argv[3]
lo, hi = int(sys.argv[4], 16), int(sys.argv[5], 16)
data = open(elf, 'rb').read(); TOC = 0x02180DF8
phoff, = struct.unpack('>Q', data[0x20:0x28]); phn, = struct.unpack('>H', data[0x38:0x3A])
segs = []
for i in range(phn):
    o = phoff + i * 56
    t, fl, off, va, pa, fs, ms, al = struct.unpack('>IIQQQQQQ', data[o:o + 56])
    if t == 1: segs.append((off, va, fs))
def va2o(v):
    for off, va, fs in segs:
        if va <= v < va + fs: return off + v - va
def rd(v):
    o = va2o(v); return struct.unpack('>I', data[o:o + 4])[0] if o is not None else 0
def code_of(opd):
    return rd(opd) if opd and rd(opd + 4) == TOC else 0
def cstr(v):
    o = va2o(v)
    if o is None: return None
    e = data.index(b'\0', o); return data[o:e].decode('ascii', 'replace')
# function bodies from lifted code
bodies = {}
for f in os.listdir(ldir):
    if not f.endswith('.cpp'): continue
    t = open(os.path.join(ldir, f), errors='replace').read()
    for m in re.finditer(r'^void func_([0-9A-F]{8})\(ppu_context\* ctx\) \{(.*?)^\}', t, re.M | re.S):
        bodies[int(m.group(1), 16)] = m.group(2)
def name_of(fn):
    b = bodies.get(fn, '')
    m = re.search(r'\(uint32_t\)0x([0-9A-F]+) << 16\);.*?\(int64_t\)\((-?\d+)\)', b, re.S)
    if not m: return None
    return cstr((int(m.group(1), 16) << 16) + int(m.group(2)))
calls = {}
for line in open(log, errors='replace'):
    m = re.search(r'\[icall\] tid=(\d+) target=0x([0-9A-F]+) calls=(\d+)', line)
    if m: calls[int(m.group(2), 16)] = calls.get(int(m.group(2), 16), 0) + int(m.group(3))
v = lo
while v < hi:
    s = [code_of(rd(v + 4 * i)) for i in range(6)]
    if all(s) and code_of(rd(v - 4)) == 0:
        n = name_of(s[2]) or name_of(s[3])
        ex = s[5]
        print(f'vt 0x{v:08X} {n!s:40} exec 0x{ex:08X} calls={calls.get(ex, 0)}')
        v += 24
    else:
        v += 4
