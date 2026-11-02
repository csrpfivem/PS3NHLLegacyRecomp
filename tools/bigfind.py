"""Find which archives contain a byte string (e.g. an asset path).

Usage: python -I bigfind.py <needle> <file> [<file> ...]
Prints each file with its hit count and the first few offsets.
"""
import sys

CHUNK = 64 << 20


def main():
    needle = sys.argv[1].encode()
    for path in sys.argv[2:]:
        hits, pos, tail = [], 0, b""
        try:
            with open(path, "rb") as f:
                while True:
                    buf = f.read(CHUNK)
                    if not buf:
                        break
                    data = tail + buf
                    base = pos - len(tail)
                    i = data.find(needle)
                    while i >= 0:
                        if not hits or base + i != hits[-1]:
                            hits.append(base + i)
                        i = data.find(needle, i + 1)
                    tail = data[-(len(needle) - 1):] if len(needle) > 1 else b""
                    pos += len(buf)
        except OSError as e:
            print("%s: %s" % (path, e))
            continue
        print("%-60s hits=%d %s" % (path, len(hits), " ".join("0x%X" % h for h in hits[:4])))


if __name__ == "__main__":
    main()
