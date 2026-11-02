# Dump LS words from a segment's SNAP. usage: lsdump.py <seg.bin> <stage> <addr> <len> [f]  (f = also floats)
import sys, struct
sys.path.insert(0, __import__('os').path.dirname(__file__))
from corpus import load
ch = load(sys.argv[1]); st = int(sys.argv[2]); a = int(sys.argv[3], 16); n = int(sys.argv[4], 16)
fl = len(sys.argv) > 5
d = next(d for tag, s, d in ch if tag == 'SNAP' and s == st)
ls = d[2048:]
for o in range(a, a + n, 16):
    w = struct.unpack_from('>4I', ls, o)
    line = f'{o:05X}: ' + ' '.join(f'{x:08X}' for x in w)
    if fl: line += '   ' + ' '.join(f'{x:11.5g}' for x in struct.unpack_from('>4f', ls, o))
    print(line)
