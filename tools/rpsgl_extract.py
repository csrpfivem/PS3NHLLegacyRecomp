"""Extract level 0 of the rasters in an NHL RW4 PS3 texture library (.rpsgl) to PNG.
Layout read from the file (texlib_1_5_4.rpsgl): raster descriptors (0x38 B) whose +0x30 = u16 w, u16 h,
+0x34 = mip count; a table of 0x30-byte entries {data offset, 0, data size, align, 1, 0x10030, desc offset,
...} follows the name strings; raster data sits in one body at the end of the file.
Format is inferred from the data size (0.5 B/px = DXT1, 1 B/px = DXT5).
usage: rw4_extract.py <file.rpsgl> <out_dir> [crop x y w h scale]"""
import os, struct, sys, zlib

def png_write(path, w, h, rows):
    raw = b"".join(b"\x00" + r for r in rows)
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))

def c565(c):
    return ((c >> 11) & 31) * 255 // 31, ((c >> 5) & 63) * 255 // 63, (c & 31) * 255 // 31

def decode(data, off, w, h, kind):
    """-> list of bytearray rows, RGBA"""
    rows = [bytearray(w * 4) for _ in range(h)]
    bs = 8 if kind == "DXT1" else 16
    p = off
    for by in range(h // 4):
        for bx in range(w // 4):
            blk = data[p:p + bs]; p += bs
            alpha = [255] * 16
            if kind == "DXT5":
                a0, a1 = blk[0], blk[1]
                bits = int.from_bytes(blk[2:8], "little")
                pal = [a0, a1] + ([((6 - i) * a0 + (1 + i) * a1) // 7 for i in range(6)] if a0 > a1
                                  else [((4 - i) * a0 + (1 + i) * a1) // 5 for i in range(4)] + [0, 255])
                alpha = [pal[(bits >> (3 * i)) & 7] for i in range(16)]
                blk = blk[8:]
            c0, c1, idx = struct.unpack_from("<HHI", blk, 0)
            p0, p1 = c565(c0), c565(c1)
            if c0 > c1 or kind == "DXT5":
                p2 = tuple((2 * a + b) // 3 for a, b in zip(p0, p1)); p3 = tuple((a + 2 * b) // 3 for a, b in zip(p0, p1))
            else:
                p2 = tuple((a + b) // 2 for a, b in zip(p0, p1)); p3 = (0, 0, 0)
            pal = (p0, p1, p2, p3)
            for i in range(16):
                r, g, b = pal[(idx >> (2 * i)) & 3]
                row = rows[by * 4 + i // 4]; x = (bx * 4 + i % 4) * 4
                row[x:x + 4] = bytes((r, g, b, alpha[i]))
    return rows

def main():
    path, out = sys.argv[1], sys.argv[2]
    data = open(path, "rb").read()
    os.makedirs(out, exist_ok=True)
    names = [n.decode() for n in data[0x300:0x390].split(b"\x00") if n.endswith(b".Raster")]
    ents = []
    o = 0x388
    while len(ents) < len(names):
        doff, _, size, align, one, typ, desc = struct.unpack_from(">7I", data, o)
        ents.append((doff, size, desc)); o += 0x30
    total = ents[-1][0] + ents[-1][1]
    base = len(data) - total
    base -= base % 0x80   # body is 0x80-aligned
    print(f"body base 0x{base:X}")
    for name, (doff, size, desc) in zip(names, ents):
        w, h = struct.unpack_from(">HH", data, desc + 0x30)
        mips = data[desc + 0x34]
        bpp = size / (w * h * 4 / 3)
        kind = "DXT1" if bpp < 0.75 else "DXT5"
        print(f"{name}: {w}x{h} mips={mips} size={size} ({bpp:.2f} B/px -> {kind}) data@0x{base + doff:X}")
        if len(sys.argv) > 3 and sys.argv[3] not in name:
            continue
        lvl = int(os.environ.get("LEVEL", "0"))   # mip level: levels are packed back to back
        p, lw, lh = base + doff, w, h
        for _ in range(lvl):
            p += max(1, lw // 4) * max(1, lh // 4) * (8 if kind == "DXT1" else 16)
            lw, lh = max(4, lw // 2), max(4, lh // 2)
        rows = decode(data, p, lw, lh, kind)
        png_write(os.path.join(out, name.replace(".Raster", f"{'_L%d' % lvl if lvl else ''}.png")), lw, lh, rows)

main()
