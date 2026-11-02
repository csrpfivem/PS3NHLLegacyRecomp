"""Resolve exe rvas (from `prof` output) to symbols via the MSVC-style link map.
usage: python -I maprva.py <NHLLegacy.map> <prof text file>   (resolves every rva=0x... in it)
"""
import bisect, re, sys
syms = []
for line in open(sys.argv[1], encoding="latin-1"):
    m = re.match(r"\s*0001:[0-9a-f]{8}\s+(\S+)\s+([0-9a-f]{16})\s+(\S+)", line)
    if m:
        syms.append((int(m.group(2), 16) - 0x140000000, m.group(1), m.group(3)))
syms.sort()
keys = [s[0] for s in syms]
for line in open(sys.argv[2], encoding="latin-1"):
    m = re.search(r"rva=0x([0-9A-Fa-f]+)", line)
    if not m:
        print(line.rstrip()); continue
    rva = int(m.group(1), 16)
    i = bisect.bisect_right(keys, rva) - 1
    name = f"{syms[i][1]}+0x{rva - syms[i][0]:X} [{syms[i][2]}]" if i >= 0 else "?"
    print(line.rstrip() + "   " + name)
