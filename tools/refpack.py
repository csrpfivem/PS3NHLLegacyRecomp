"""EA RefPack (QFS) decompressor -- ground truth for checking the game's own.

Usage: python -I refpack.py <in> <out>
Header: u16 BE flags (0x10FB; bit 0x8000 = 4-byte sizes, 0x0100 = compressed
size present), then the decompressed size (3 or 4 bytes BE).
"""
import sys


def unpack(src):
    p = 0
    flags = (src[0] << 8) | src[1]
    p = 2
    big = flags & 0x8000
    if flags & 0x0100:          # compressed size present: skip it
        p += 4 if big else 3
    n = 4 if big else 3
    size = int.from_bytes(src[p:p + n], "big")
    p += n
    out = bytearray()
    while p < len(src):
        b0 = src[p]
        if b0 < 0x80:                       # 2-byte: 0-3 literals, copy 3-10
            b1 = src[p + 1]; p += 2
            lit = b0 & 3
            out += src[p:p + lit]; p += lit
            cnt = ((b0 >> 2) & 7) + 3
            off = (((b0 >> 5) & 3) << 8) + b1 + 1
        elif b0 < 0xC0:                     # 3-byte: 0-3 literals, copy 4-67
            b1, b2 = src[p + 1], src[p + 2]; p += 3
            lit = b1 >> 6
            out += src[p:p + lit]; p += lit
            cnt = (b0 & 0x3F) + 4
            off = ((b1 & 0x3F) << 8) + b2 + 1
        elif b0 < 0xE0:                     # 4-byte: 0-3 literals, copy 5-1028
            b1, b2, b3 = src[p + 1], src[p + 2], src[p + 3]; p += 4
            lit = b0 & 3
            out += src[p:p + lit]; p += lit
            cnt = (((b0 >> 2) & 3) << 8) + b3 + 5
            off = (((b0 >> 4) & 1) << 16) + (b1 << 8) + b2 + 1
        elif b0 < 0xFC:                     # literal run 4-112
            lit = ((b0 & 0x1F) << 2) + 4; p += 1
            out += src[p:p + lit]; p += lit
            continue
        else:                               # end: 0-3 trailing literals
            lit = b0 & 3; p += 1
            out += src[p:p + lit]
            break
        for _ in range(cnt):
            out.append(out[-off])
    return size, bytes(out)


def main():
    data = open(sys.argv[1], "rb").read()
    size, out = unpack(data)
    open(sys.argv[2], "wb").write(out)
    print("header size=%d produced=%d match=%s first16=%s" % (size, len(out), size == len(out), out[:16].hex()))


if __name__ == "__main__":
    main()
