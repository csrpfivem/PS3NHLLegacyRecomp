"""List (and optionally extract) a BIGF archive carved out of a larger file.

Usage: python -I bigf_list.py <file> <offset hex> <size> [<extract dir>]
BIGF: 'BIGF', u32 total size (LE), u32 count (BE), u32 header size (BE), then
count x { u32 offset (BE), u32 size (BE), name\\0 }. Offsets are relative to the
start of the BIGF. Prints each entry with the first 16 bytes of its data.
"""
import os
import struct
import sys


def main():
    path, off, size = sys.argv[1], int(sys.argv[2], 16), int(sys.argv[3])
    out = sys.argv[4] if len(sys.argv) > 4 else None
    with open(path, "rb") as f:
        f.seek(off)
        blob = f.read(size)
    if blob[:4] != b"BIGF":
        print("not BIGF:", blob[:4])
        return
    count, hdr = struct.unpack_from(">II", blob, 8)
    print("BIGF count=%d header=0x%X total=%d" % (count, hdr, len(blob)))
    p = 16
    for _ in range(count):
        eo, es = struct.unpack_from(">II", blob, p)
        p += 8
        e = blob.index(b"\0", p)
        name = blob[p:e].decode("latin-1")
        p = e + 1
        data = blob[eo:eo + es]
        print("  0x%06X %8d  %-50s %s" % (eo, es, name, data[:16].hex()))
        if out:
            dst = os.path.join(out, name.replace("/", os.sep))
            os.makedirs(os.path.dirname(dst) or out, exist_ok=True)
            with open(dst, "wb") as o:
                o.write(data)


if __name__ == "__main__":
    main()
