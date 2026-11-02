# Dump big-endian words at a guest address (vtables, descriptors). usage: ppcwords.py <elf> <0xaddr> [count]
import struct, sys
d = open(sys.argv[1], 'rb').read()
phoff = struct.unpack_from('>Q', d, 0x20)[0]; phnum = struct.unpack_from('>H', d, 0x38)[0]
segs = []
for i in range(phnum):
    t, fl, off, va, _, fsz, _, _ = struct.unpack_from('>IIQQQQQQ', d, phoff + i * 0x38)
    if t == 1 and fsz: segs.append((va, va + fsz, off))
def rd32(va):
    for a, b, o in segs:
        if a <= va < b - 3: return struct.unpack_from('>I', d, o + va - a)[0]
    return None
a = int(sys.argv[2], 16); n = int(sys.argv[3]) if len(sys.argv) > 3 else 32
for i in range(n):
    w = rd32(a + i * 4)
    if w is None: break
    code = rd32(w) if w and rd32(w) is not None else None   # OPD slot -> code address
    print('%#x: %08X  opd->%s' % (a + i * 4, w, '%08X' % code if code else '-'))
