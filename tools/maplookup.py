"""Resolve RVAs to the nearest preceding symbol in an lld-link /MAP file.

Usage: python -I maplookup.py <NHLLegacy.map> <rva> [<rva> ...]
"""
import bisect
import re
import sys

# MSVC-style publics: " 0001:15fc1900       name   0000000155fc2900     lib:obj"
LINE = re.compile(r"^\s*[0-9a-fA-F]{4}:[0-9a-fA-F]{8}\s+(\S+)\s+([0-9a-fA-F]{16})\s+(.*)$")


def main():
    path, rvas = sys.argv[1], [int(a, 16) for a in sys.argv[2:]]
    base = 0x140000000
    syms = []
    with open(path, errors="replace") as f:
        for line in f:
            m = LINE.match(line)
            if not m:
                continue
            va = int(m.group(2), 16)
            if va >= base:
                syms.append((va - base, m.group(1), m.group(3).strip()))
    syms.sort()
    keys = [s[0] for s in syms]
    for rva in rvas:
        i = bisect.bisect_right(keys, rva) - 1
        if i < 0:
            print(f"{rva:#x}: ?")
            continue
        a, name, obj = syms[i]
        print(f"{rva:#x}: {name} +{rva - a:#x}  ({obj})")


if __name__ == "__main__":
    main()
