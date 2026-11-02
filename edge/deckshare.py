# Where Deck cycles go, grouped by owner, for two perf samples side by side.
# usage: deckshare.py <label> <perfrec> <maps> <map> [<label> <perfrec> <maps> <map>] [--event cycles:u]
import bisect, collections, re, sys

LINE = re.compile(r"^\s*[0-9a-fA-F]{4}:[0-9a-fA-F]{8}\s+(\S+)\s+([0-9a-fA-F]{16})\s+(.*)$")


def owner(name, obj):
    o = obj.split(":")[-1]
    short = o.replace(".c.obj", "").replace(".cpp.obj", "")
    m = re.match(r"nhl_(prog|job)_([0-9A-Fa-f]+)", o)
    if m: return ("SPU code", f"{m[1]} {m[2]}")
    if o.startswith("ppu_recomp"): return ("PPU lifted", "ppu_recomp")
    if o.startswith("nhl_edge_native"): return ("Native Edge", "nhl_edge_native")
    if "spu" in o.lower(): return ("SPU runtime", short)
    if "rsx" in o.lower() or "gcm" in o.lower() or "vp_" in o: return ("RSX / render", short)
    return ("Other runtime", short)


def load(perfrec, maps, mapfile, event):
    regions, exe_lo = [], None
    for line in open(maps, errors="replace"):
        p = line.split()
        lo, hi = (int(x, 16) for x in p[0].split("-"))
        n = p[5] if len(p) > 5 else "[anon]"
        regions.append((lo, hi, n))
        if n.lower().endswith("nhllegacy.exe"): exe_lo = lo if exe_lo is None else min(exe_lo, lo)
    regions.sort(); starts = [r[0] for r in regions]
    syms = []
    for line in open(mapfile, errors="replace"):
        m = LINE.match(line)
        if m:
            va = int(m[2], 16)
            if va >= 0x140000000: syms.append((va - 0x140000000, m[1], m[3].strip()))
    syms.sort(); keys = [s[0] for s in syms]; end = keys[-1] + 0x10000
    base = exe_lo if exe_lo is not None else 0x140000000
    group, detail, fn, tids = collections.Counter(), collections.Counter(), collections.Counter(), collections.Counter()
    total = 0
    for line in open(perfrec, errors="replace"):
        p = line.split()
        if len(p) < 4 or p[1].rstrip(":") != event: continue
        c, ip, tid = int(p[0]), int(p[2], 16), p[3]
        total += c; tids[tid] += c
        rva = ip - base
        if 0 <= rva < end:
            i = bisect.bisect_right(keys, rva) - 1
            if i >= 0:
                g, d = owner(syms[i][1], syms[i][2])
                group[g] += c; detail[(g, d)] += c; fn[syms[i][1]] += c
                continue
        i = bisect.bisect_right(starts, ip) - 1
        n = regions[i][2].rsplit("/", 1)[-1] if i >= 0 and regions[i][0] <= ip < regions[i][1] else "?"
        group["Outside exe"] += c; detail[("Outside exe", n)] += c
    return total, group, detail, fn, tids


args = sys.argv[1:]
event = "cycles:u"
if "--event" in args:
    i = args.index("--event"); event = args[i + 1]; del args[i:i + 2]
runs = [(args[i], load(args[i + 1], args[i + 2], args[i + 3], event)) for i in range(0, len(args), 4)]
print(event)
print("%-34s" % "GROUP" + "".join("%12s" % l for l, _ in runs))
for g in sorted({g for _, r in runs for g in r[1]}, key=lambda g: -runs[-1][1][1][g]):
    print("%-34s" % g + "".join("%11.1f%%" % (100 * r[1][g] / r[0]) for _, r in runs))
print()
keys = sorted({k for _, r in runs for k in r[2]}, key=lambda k: -runs[-1][1][2][k])[:34]
for k in keys:
    print("%-34s" % f"{k[0][:12]}: {k[1]}"[:34] + "".join("%11.1f%%" % (100 * r[2][k] / r[0]) for _, r in runs))
label, r = runs[-1]
print(f"\ntop functions in {label}:")
for n, c in r[3].most_common(24):
    print(f"  {100 * c / r[0]:5.2f}%  {n[:70]}")
print(f"\nthreads in {label} (share of all samples): " + " ".join(f"{t}:{100 * c / r[0]:.1f}" for t, c in r[4].most_common(16)))
