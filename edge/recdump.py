# Print a segment's record + the ctx at stage 3 entry. usage: recdump.py <seg.bin>
import sys, struct
sys.path.insert(0, __import__('os').path.dirname(__file__))
from corpus import load
ch = load(sys.argv[1])
rec = ch[0][2]
print('tags', [(t, s, len(d)) for t, s, d in ch][:6])
for o in range(0, len(rec), 16):
    print(f'{o:03X}: ' + rec[o:o+16].hex(' ', 4))
for want in (3,):
    d = next(d for t, s, d in ch if t == 'SNAP' and s == want)
    regs = d[:2048]; ls = d[2048:]
    for i in (0, 1, 3, 4, 5, 6, 7, 8):
        print(f'r{i}: ' + ' '.join(f'{x:08X}' for x in struct.unpack_from('<4I', regs, i * 16)))
    r3 = struct.unpack_from('<I', regs, 3 * 16)[0]
    for rd in range(3):
        pass
    print('ctx @', hex(r3))
    for o in range(r3, r3 + 0x200, 16):
        print(f'{o:05X}: ' + ls[o:o+16].hex(' ', 4))
