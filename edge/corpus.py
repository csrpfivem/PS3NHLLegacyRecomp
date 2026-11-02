# Edge corpus reader (NHL_EDGE_DUMP files). usage: corpus.py <dir> [file-index]
import sys, os, struct, glob, collections
def load(path):
    b = open(path, 'rb').read(); o = 0; ch = []
    while o + 12 <= len(b):
        tag, st, n = b[o:o+4].decode('latin-1'), *struct.unpack_from('<II', b, o + 4)
        ch.append((tag, st, b[o+12:o+12+n])); o += 12 + n
    return ch
def be32(b, o): return struct.unpack_from('>I', b, o)[0]
def be16(b, o): return struct.unpack_from('>H', b, o)[0]
if __name__ == '__main__':
    files = sorted(glob.glob(sys.argv[1] + '/seg_*.bin'))
    if len(sys.argv) > 2:
        ch = load(files[int(sys.argv[2])])
        for tag, st, d in ch:
            if tag == 'PUT ': lsa, ea, size, lr = struct.unpack_from('<IIII', d); print(f'PUT kind {st} lsa {lsa:05X} ea {ea:08X} size {size} lr {lr:X}')
            elif tag == 'SNAP':
                regs = d[:2048]; r = [struct.unpack_from('<4I', regs, i * 16) for i in range(12)]
                print(f'SNAP st {st:2d} r3..r8 ' + ' '.join(f'{r[i][0]:08X}' for i in range(3, 9)))
            else: print(tag, st, len(d), d[:16].hex())
        sys.exit()
    keys = collections.Counter(); stages = collections.Counter(); puts = collections.Counter(); bad = 0
    for f in files:
        ch = load(f)
        if not ch or ch[0][0] != 'REC ': bad += 1; continue
        r = ch[0][2]; keys[(r[:8].hex(), be32(r, 0x11C) != 0)] += 1
        for tag, st, d in ch:
            if tag == 'SNAP': stages[st] += 1
            if tag == 'PUT ': puts[st] += 1
    print(len(files), 'files,', bad, 'bad,', len(keys), 'keys')
    print('snap stages:', dict(sorted(stages.items())))
    print('puts by kind:', dict(puts))
