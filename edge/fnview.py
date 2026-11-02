# Compact view of lifted PPU functions: calls and global constants only, or the whole body.
# usage: fnview.py <func hex> [full] (searches nhl/port/recompiled/ppu_recomp_*.cpp, big funcs last)
import glob, re, sys
want = sys.argv[1].upper().replace('FUNC_', '')
full = len(sys.argv) > 2
head = re.compile(r'^void func_%s\(' % want)
for f in sorted(glob.glob(r'_port_work\nhl\port\recompiled\ppu_recomp_*.cpp')):
    if '.pre' in f: continue
    lines = open(f, errors='replace').read().split('\n')
    for i, l in enumerate(lines):
        if head.match(l):
            print(f, i + 1)
            lis = {}
            for j in range(i, len(lines)):
                s = lines[j].strip()
                if j > i and lines[j].startswith('void func_'): sys.exit()
                if full:
                    print(j + 1, s); continue
                m = re.search(r'gpr\[(\d+)\] = \(int64_t\)\(int32_t\)\(\(uint32_t\)0x([0-9A-F]+) << 16\)', s)
                if m: lis[m[1]] = int(m[2], 16) << 16
                m = re.search(r'_a = ctx->gpr\[(\d+)\]; .*\(int64_t\)\((-?(?:0x)?[0-9A-Fa-f]+)\); ctx->gpr\[(\d+)\] = _r', s)
                if m and m[1] in lis:
                    v = lis[m[1]] + int(m[2], 0)
                    lis[m[3]] = v
                    if v >= 0x10000: print(j + 1, '   const r%s = %#x' % (m[3], v))
                m = re.search(r'vm_(read|write)\d+\(ctx->gpr\[(\d+)\] \+ (-?(?:0x)?[0-9A-Fa-f]+)', s)
                if m and m[2] in lis and lis[m[2]] >= 0x10000:
                    print(j + 1, '   %s %#x' % (m[1], lis[m[2]] + int(m[3], 0)))
                if 'func_' in s or 'ps3_indirect' in s or s.startswith('loc_') or 'goto' in s or s == 'return;':
                    print(j + 1, s[:110])
            sys.exit()
