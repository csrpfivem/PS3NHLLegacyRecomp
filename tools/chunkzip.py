"""chunkzip v2 -> raw. Header: 'chunkzip' u32 ver u32 usize u32 chunk u32 nchunks u32 align
pad to 0x28, then per chunk (aligned): u32 csize u32 type(1=raw deflate) data.
usage: chunkzip.py <in> <out>"""
import struct, sys, zlib
d = open(sys.argv[1], 'rb').read()
ver, usize, chunk, n, align = struct.unpack('>IIIII', d[8:28])
pos = 0x28; out = b''
for i in range(n):
    cs, typ = struct.unpack('>II', d[pos:pos + 8]); pos += 8
    blob = d[pos:pos + cs]; pos += cs
    pos = (pos + 8 + align - 1) // align * align - 8   # next header sits 8 bytes before an aligned data start
    out += zlib.decompress(blob, -15) if typ == 1 else blob
open(sys.argv[2], 'wb').write(out)
print(len(out), 'of', usize)
