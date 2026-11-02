"""Scan EA archives for embedded SPU ELF images, compression markers and file-name tables.

Usage: python -I bigscan.py <out.txt> <file> [<file> ...]
"""
import re
import sys
from collections import Counter

CHUNK = 64 << 20
ELF32BE = re.compile(rb"\x7fELF\x01\x02\x01")
NAME = re.compile(rb"[\w/\\.\-]{3,100}\.(elf|fx|cgfx|vpo|fpo|lua|xml|rx2|rx3|sbs|sbr|ast|dds|big|bin|db)\b")
MARKERS = {b"chunkzip": "chunkzip", b"\x10\xfb": None}  # refpack header is too short to count reliably


def scan(path):
    spu, other_elf, names, chunkzip = 0, Counter(), Counter(), 0
    with open(path, "rb") as f:
        pos, tail = 0, b""
        while True:
            buf = f.read(CHUNK)
            if not buf:
                break
            data = tail + buf
            for m in ELF32BE.finditer(data):
                if m.start() + 20 <= len(data):
                    machine = int.from_bytes(data[m.start() + 18:m.start() + 20], "big")
                    if machine == 0x17:
                        spu += 1
                    else:
                        other_elf[machine] += 1
            for m in NAME.finditer(data):
                names[m.group(1).decode()] += 1
            chunkzip += data.count(b"chunkzip")
            tail = data[-128:]
            pos += len(buf)
    return spu, other_elf, names, chunkzip


def main():
    out = open(sys.argv[1], "w")
    for p in sys.argv[2:]:
        spu, other, names, cz = scan(p)
        line = f"{p}\n  spu_elf={spu} other_elf={dict(other)} chunkzip={cz} name_exts={dict(names.most_common(12))}\n"
        out.write(line)
        out.flush()
        print(line, end="", flush=True)


if __name__ == "__main__":
    main()
