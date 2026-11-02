# Prints the [HZ]/[HITCH]/[MARK] lines of a run log with every rva=0x... replaced
# by its function name from the link map (LOCAL_PATCHES #122).
# Usage: python -I tools\hitch_sym.py nhl\runs\<tag>.err.log [--map nhl\build\NHLLegacy.map] [--no-hz]
import bisect, re, sys, os

def load_map(path):
    syms = []
    for line in open(path, encoding="latin-1"):
        p = line.split()
        # " 0001:19b3b4b0       render_frame    0000000159b3c4b0   obj"
        if len(p) >= 3 and p[0].startswith("0001:") and re.fullmatch(r"[0-9a-f]{16}", p[2]):
            syms.append((int(p[2], 16) - 0x140000000, p[1]))
    syms.sort()
    return [s[0] for s in syms], [s[1] for s in syms]

def main():
    args = sys.argv[1:]
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    mp = os.path.join(here, "nhl", "build", "NHLLegacy.map")
    if "--map" in args:
        i = args.index("--map"); mp = args[i + 1]; del args[i:i + 2]
    no_hz = "--no-hz" in args
    if no_hz: args.remove("--no-hz")
    addrs, names = load_map(mp)
    def sym(m):
        rva = int(m.group(1), 16)
        i = bisect.bisect_right(addrs, rva) - 1
        if i < 0: return m.group(0)
        off = rva - addrs[i]
        return names[i] + ("+0x%X" % off if off else "")
    pat = re.compile(r"rva=0x([0-9A-Fa-f]+)")
    for line in open(args[0], encoding="latin-1"):
        if line.startswith(("[HITCH]", "[MARK]", "[HZPROF]", "[FXCPAR]")) or (line.startswith("[HZ]") and not no_hz):
            sys.stdout.write(pat.sub(sym, line))

main()
