"""EB v3 archive: list / extract by name. Header: 'EB' u16 ver, u32 count, u32 align,
u32 table bytes, u32 names bytes, u8 name len, ..., entries @0x30 x16
{u32 off/16, u32 0, u32 size, u32 hash}; names @ (0x30?) table end, fixed len, 1 prefix byte.
usage: eb_extract.py <big> <outdir> [name substring...]"""
import struct, sys, os
p, out = sys.argv[1], sys.argv[2]; want = sys.argv[3:]
d = open(p, 'rb').read()
cnt, align, tbl, nms = struct.unpack('>IIII', d[4:20]); nlen = d[20]
names_at = tbl
for i in range(cnt):
    off16, z, size, h = struct.unpack('>IIII', d[0x30 + i * 16: 0x40 + i * 16])
    raw = d[names_at + i * nlen: names_at + (i + 1) * nlen]
    name = raw[1:].split(b'\0')[0].lstrip(bytes(range(32))).decode('ascii', 'replace')
    if want and not any(w in name for w in want): continue
    blob = d[off16 * 16: off16 * 16 + size]
    print(f'{i:4} {name:44} off=0x{off16*16:X} size={size} head={blob[:16].hex()}')
    if want:
        os.makedirs(out, exist_ok=True)
        open(os.path.join(out, name.replace('/', '_')), 'wb').write(blob)
