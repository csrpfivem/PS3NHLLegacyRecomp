"""code addr -> OPD addr(s) -> vtables containing that OPD; print the vtable's slots
as code addresses. usage: vtfind.py <elf> <code hex>..."""
import struct, sys, re
elf = sys.argv[1]; data = open(elf, 'rb').read()
TOC = 0x02180DF8
phoff, = struct.unpack('>Q', data[0x20:0x28]); phn, = struct.unpack('>H', data[0x38:0x3A])
segs = []
for i in range(phn):
    o = phoff + i * 56
    t, fl, off, va, pa, fs, ms, al = struct.unpack('>IIQQQQQQ', data[o:o + 56])
    if t == 1: segs.append((off, va, fs))
def o2va(o):
    for off, va, fs in segs:
        if off <= o < off + fs: return va + o - off
def va2o(v):
    for off, va, fs in segs:
        if va <= v < va + fs: return off + v - va
def rd(v):
    o = va2o(v); return struct.unpack('>I', data[o:o + 4])[0] if o is not None else None
opd_of = {}
for m in re.finditer(re.escape(struct.pack('>I', TOC)), data):
    o = m.start() - 4
    if o % 4: continue
    va = o2va(o)
    if va is None: continue
    code, = struct.unpack('>I', data[o:o + 4]); opd_of.setdefault(code, []).append(va)
def code_of(opd):
    c = rd(opd); t = rd(opd + 4)
    return c if t == TOC else None
for a in sys.argv[2:]:
    code = int(a, 16)
    for opd in opd_of.get(code, []):
        print(f'code 0x{code:08X} opd 0x{opd:08X}')
        for m in re.finditer(re.escape(struct.pack('>I', opd)), data):
            if m.start() % 4: continue
            va = o2va(m.start())
            if va is None: continue
            # walk back to vtable start (stop at a non-OPD word)
            s = va
            while code_of(rd(s - 4) or 0): s -= 4
            slots = []
            v = s
            while True:
                c = code_of(rd(v) or 0)
                if not c: break
                slots.append(c); v += 4
                if len(slots) > 40: break
            print(f'  vtable @0x{s:08X} (ref @0x{va:08X} = slot {(va - s)//4}):', ' '.join(f'{i}:{c:08X}' for i, c in enumerate(slots)))
