"""Carve EA VP6 movie streams (MVhd + MV0K/MV0F chunks) out of the game's archives.

EA movie chunks are <4-byte tag><u32 size incl. 8-byte header>. Movies sit in the
.big archives uncompressed, so each one is a contiguous run of chunks starting at
an MVhd. The size field's byte order is detected from the MVhd chunk itself.

Usage: python -I vp6carve.py <out_dir> <archive> [<archive> ...]
Writes <out_dir>/<archive>_<offset hex>.vp6 and prints offset, size, frame count
and the header's width x height for each.
"""
import os
import struct
import sys

TAGS = {b"MVhd", b"MV0K", b"MV0F", b"MVIh", b"MVIf", b"SCHl", b"SCCl", b"SCDl", b"SCEl",
        b"MPCh", b"pQGT", b"pIQT"}
CHUNK = 64 << 20


def chunk_size(buf, pos, little):
    return struct.unpack_from("<I" if little else ">I", buf, pos + 4)[0]


def walk(f, start, little):
    """Follow chunks from start while the tags look like a movie; return (end, frames)."""
    pos, frames = start, 0
    while True:
        f.seek(pos)
        hdr = f.read(8)
        if len(hdr) < 8 or hdr[:4] not in TAGS:
            return pos, frames
        size = struct.unpack("<I" if little else ">I", hdr[4:])[0]
        if size < 8 or size > (64 << 20):
            return pos, frames
        if hdr[:4] in (b"MV0K", b"MV0F"):
            frames += 1
        pos += size


def main():
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    for path in sys.argv[2:]:
        base = os.path.basename(path)
        found = []
        with open(path, "rb") as f:
            off, tail = 0, b""
            while True:
                buf = f.read(CHUNK)
                if not buf:
                    break
                data = tail + buf
                start = off - len(tail)
                i = data.find(b"MVhd")
                while i >= 0:
                    if i + 32 <= len(data):
                        found.append(start + i)
                    i = data.find(b"MVhd", i + 4)
                tail = data[-32:]
                off += len(buf)
            seen = set()
            for pos in sorted(set(found)):
                if pos in seen:
                    continue
                f.seek(pos)
                hdr = f.read(32)
                le, be = struct.unpack_from("<I", hdr, 4)[0], struct.unpack_from(">I", hdr, 4)[0]
                little = le < be
                end, frames = walk(f, pos, little)
                if frames == 0:
                    continue
                # MVhd payload: codec fourcc, then u16 width, u16 height (same byte order)
                w, h = struct.unpack_from("<HH" if little else ">HH", hdr, 12)
                name = "%s_%08X.vp6" % (os.path.splitext(base)[0], pos)
                f.seek(pos)
                with open(os.path.join(out, name), "wb") as o:
                    o.write(f.read(end - pos))
                print("%-40s off=0x%08X size=%9d frames=%5d %dx%d codec=%r" %
                      (name, pos, end - pos, frames, w, h, hdr[8:12]))


if __name__ == "__main__":
    main()
