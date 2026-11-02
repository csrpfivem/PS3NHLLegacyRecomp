"""String -> VA -> lifted PPU sites that build that address (lis hi; addi/ori lo).
usage: tocref.py <elf> <lifted dir> <string> [<string>...]"""
import struct, sys, os, re
elf, ldir = sys.argv[1], sys.argv[2]
data = open(elf, 'rb').read()
phoff, = struct.unpack('>Q', data[0x20:0x28]); phn, = struct.unpack('>H', data[0x38:0x3A])
segs = []
for i in range(phn):
    o = phoff + i * 56
    t, fl, off, va, pa, fs, ms, al = struct.unpack('>IIQQQQQQ', data[o:o + 56])
    if t == 1: segs.append((off, va, fs))
def o2va(o):
    for off, va, fs in segs:
        if off <= o < off + fs: return va + o - off
files = sorted(f for f in os.listdir(ldir) if f.endswith('.cpp'))
texts = {f: open(os.path.join(ldir, f), errors='replace').read() for f in files}
funcre = re.compile(r'^void (func_[0-9A-F]+)\(', re.M)
def func_at(t, pos):
    best = None
    for m in funcre.finditer(t, 0, pos): best = m.group(1)
    return best
for s in sys.argv[3:]:
    for m in re.finditer(re.escape(s.encode()) + b'\x00', data):
        o = m.start()
        if o > 0 and data[o - 1] != 0: continue
        va = o2va(o)
        if va is None: continue
        hi_a = (va + 0x8000) >> 16; lo = va & 0xFFFF; lo_s = lo - 0x10000 if lo >= 0x8000 else lo
        print(f'== "{s}" @ VA 0x{va:08X}  (lis 0x{hi_a:X} + {lo_s:#x})')
        lis = re.compile(r'\(uint32_t\)0x%X << 16\)' % hi_a)
        los = ['(int64_t)(%d)' % lo_s, '(int64_t)(%s)' % (('0x%X' % lo_s) if lo_s >= 0 else ('-0x%X' % -lo_s)),
               '0x%X)' % lo]
        for f, t in texts.items():
            for mm in lis.finditer(t):
                win = t[mm.end(): mm.end() + 600]
                if any(l in win for l in los):
                    ln = t.count('\n', 0, mm.start()) + 1
                    print(f'   {f}:{ln} in {func_at(t, mm.start())}')
