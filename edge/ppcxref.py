# Who references a guest address in the EBOOT: lis+addi pairs, TOC slots, data pointers.
# usage: ppcxref.py <elf> <map> <regex on strings | 0xADDR> ...
import bisect, re, struct, sys
elf, mapf = sys.argv[1:3]
d = open(elf, 'rb').read()
phoff = struct.unpack_from('>Q', d, 0x20)[0]; phnum = struct.unpack_from('>H', d, 0x38)[0]
entry = struct.unpack_from('>Q', d, 0x18)[0]
segs = []
for i in range(phnum):
    t, fl, off, va, _, fsz, _, _ = struct.unpack_from('>IIQQQQQQ', d, phoff + i * 0x38)
    if t == 1: segs.append((va, va + fsz, off, fl))
def rd32(va):
    for a, b, o, _ in segs:
        if a <= va < b - 3: return struct.unpack_from('>I', d, o + va - a)[0]
    return None
toc = rd32(entry + 4)
print('entry %#x toc %#x' % (entry, toc))
funcs = sorted({int(m[1], 16) for m in re.finditer(r'func_([0-9A-Fa-f]{8})\b', open(mapf, errors='replace').read())})
def fn(va):
    i = bisect.bisect_right(funcs, va) - 1
    return 'func_%08X+%X' % (funcs[i], va - funcs[i]) if i >= 0 else '?'

names = {}
for q in sys.argv[3:]:
    if q.startswith('0x'):
        names[int(q, 16)] = q
    else:
        rx = re.compile(q.encode())
        for m in re.finditer(rb'[\x20-\x7e]{4,}', d):
            if rx.search(m.group()):
                for a, b, o, _ in segs:
                    if o <= m.start() < o + b - a:
                        names[a + m.start() - o] = m.group()[:60].decode('latin1')
print(len(names), 'targets')
hits = {}
for a, b, o, fl in segs:
    words = struct.unpack_from('>%dI' % ((b - a) // 4), d, o)
    if fl & 1:   # code
        hi = {}
        for i, w in enumerate(words):
            va = a + i * 4; op = w >> 26
            rt = (w >> 21) & 31; ra = (w >> 16) & 31; imm = w & 0xFFFF; s = imm - 0x10000 if imm & 0x8000 else imm
            if op == 15 and ra == 0:
                hi[rt] = (i, (imm << 16) & 0xFFFFFFFF); continue
            if op == 15 and ra == 2:
                hi[rt] = (i, (toc + (s << 16)) & 0xFFFFFFFF); continue
            if ra == 2 and op == 14:
                t = toc + s
                if t in names: hits.setdefault(t, []).append(va)
            elif ra == 2 and op in (32, 58):
                t = rd32(toc + (s & ~3))
                if t in names: hits.setdefault(t, []).append(va)
            elif op in (12, 14, 24, 32, 34, 40, 36, 38, 44, 58) and ra in hi and i - hi[ra][0] < 64:
                v = hi[ra][1]
                t = (v | imm) if op == 24 else (v + s) & 0xFFFFFFFF
                if t in names: hits.setdefault(t, []).append(va)
                if op in (12, 14): hi[rt] = (hi[ra][0], t)   # anchor + offset chains
    else:        # data pointers
        for i, w in enumerate(words):
            if w in names: hits.setdefault(w, []).append(-(a + i * 4))
for t in sorted(names):
    r = hits.get(t, [])
    print('%#x %s' % (t, names[t]))
    for v in r[:24]:
        print('   ', ('data %#x' % -v) if v < 0 else '%#x %s' % (v, fn(v)))
