"""#283: build a lld-link /order file (hot functions first, packed together) from perf samples.

usage: python -I nhl_order.py <out.order> <NHLLegacy.map> <perfrec.txt> <maps.txt> [<perfrec> <maps> ...]
perfrec = record.sh output on the Deck ("<count> <event>: <ip hex> <tid>"), maps = /proc/<pid>/maps
of the same run. Only cycles samples are used. Functions are written hottest first; the map must
belong to the exe that was sampled.
"""
import bisect, collections, re, sys

out_path, map_path = sys.argv[1], sys.argv[2]
pairs = list(zip(sys.argv[3::2], sys.argv[4::2]))

LINE = re.compile(r"^\s*[0-9a-fA-F]{4}:[0-9a-fA-F]{8}\s+(\S+)\s+([0-9a-fA-F]{16})\s+(.*)$")
syms = []
for line in open(map_path, errors="replace"):
    m = LINE.match(line)
    if m:
        va = int(m.group(2), 16)
        if va >= 0x140000000:
            syms.append((va - 0x140000000, m.group(1)))
syms.sort()
keys = [s[0] for s in syms]

hits = collections.Counter()
for rec_path, maps_path in pairs:
    base = None
    for line in open(maps_path, errors="replace"):
        p = line.split()
        if len(p) > 5 and p[-1].lower().endswith("nhllegacy.exe"):
            lo = int(p[0].split("-")[0], 16)
            base = lo if base is None else min(base, lo)
    if base is None:
        sys.exit(f"{maps_path}: no NHLLegacy.exe mapping")
    for line in open(rec_path, errors="replace"):
        p = line.split()
        if len(p) < 4 or not p[1].startswith("cycles"):
            continue
        rva = int(p[2], 16) - base
        i = bisect.bisect_right(keys, rva) - 1
        if 0 <= i < len(keys) - 1 and rva < keys[i + 1]:
            hits[syms[i][1]] += int(p[0])

names = [n for n, _ in hits.most_common()]
with open(out_path, "w", newline="\n") as f:
    for n in names:
        f.write(n + "\n")
total = sum(hits.values())
print(f"{len(names)} functions, {total} samples -> {out_path}")
