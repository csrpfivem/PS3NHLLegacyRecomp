# Printable strings in the update EBOOT that mention the crowd, with guest addresses.
# usage: crowdstr.py <elf> [regex]
import re, struct, sys
d = open(sys.argv[1], 'rb').read()
pat = re.compile((sys.argv[2] if len(sys.argv) > 2 else r'crowd|audience|fans?\b').encode(), re.I)
phoff = struct.unpack_from('>Q', d, 0x20)[0]; phnum = struct.unpack_from('>H', d, 0x38)[0]
segs = []
for i in range(phnum):
    p = phoff + i * 0x38
    t, _, off, va, _, fsz, _, _ = struct.unpack_from('>IIQQQQQQ', d, p)
    if t == 1: segs.append((off, off + fsz, va))
def va_of(o):
    for a, b, v in segs:
        if a <= o < b: return v + o - a
    return None
seen = 0
for m in re.finditer(rb'[\x20-\x7e]{4,}', d):
    s = m.group()
    if pat.search(s):
        v = va_of(m.start())
        print(f'{v if v is None else hex(v)} {s[:120].decode("latin1")}')
        seen += 1
print(seen, 'strings')
