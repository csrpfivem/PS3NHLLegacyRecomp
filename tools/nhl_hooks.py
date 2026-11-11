"""Re-applies the port's hand hooks to freshly lifted code in nhl/port/recompiled.

A relift (ppu_lifter.py) regenerates every ppu_recomp_*.cpp and drops anything added by hand.
This script puts the hooks the port depends on back, anchored on function/label names (which
come from guest addresses, so they survive a relift with a different chunking). Idempotent:
a hook whose marker is already present is skipped.

    python nhl_hooks.py            apply missing hooks
    python nhl_hooks.py --check    report only; exit 1 if any hook is missing

Hooks:
  VP6BRIDGE   func_00D79E90 exit -> nhl_vp6_publish (nhl_vp6_bridge.cpp): host-decoded movie
              frame over the guest's planes. Without it the movies show block corruption.
  (APTEXTFILL, the #35 type-2 fill tag, was REMOVED 11-01 by #303: it overwrote the fill's
   colour-transform add term, which carries the team-colour bars and the dark text-panel box.)
  HEAPSLACK   func_00C9DEC8 entry -> nhl_heap_slack (nhl_hle.cpp): pads large allocations so
              the game's own 24-28 byte overrun on uncompressed mod logos (LOCAL_PATCHES #237)
              does not corrupt the next heap chunk. Without it modded logos hang the game.
  REFPACK     func_016040E0 entry -> nhl_refpack_inplace (nhl_hle.cpp): in-place RefPack
              streams decoded on the host from a copy of the source, output capped
              (LOCAL_PATCHES #238). Mod streams that are not in-place safe overran the buffer.
  STEPS       func_014C80F4 entry: counts sim steps for [HZ] (LOCAL_PATCHES #269).
  EDGE        lifted SPU program 01DBF980: Edge job recon (LOCAL_PATCHES #268).
  NOCROWD     func_0156905C / func_0145AD30 entry: NHL_NO_CROWD switch (LOCAL_PATCHES #294).
  SAVEBACK    func_00FBBF18 entry: back out of the save list without the failed-load dialog
              (LOCAL_PATCHES #296).
  SKIPINTRO   func_00FB4804 (FlowBoot::Start): NHL_SKIP_INTRO switch, beq at 0x00FB4834 always
              taken (LOCAL_PATCHES #297; the data byte is written in boot_main.cpp).

After a relift, also run nhl_ppu_flags.py over recompiled/ppu_recomp_*.cpp (#272) and put back
ppu_recomp.h's hand-written memory paths (#147, #261, #271).
"""
import glob
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
RECOMP = os.environ.get("NHL_RECOMP_DIR") or os.path.join(HERE, "..", "nhl", "port", "recompiled")

# Each hook: a marker that proves it is applied, the function it lives in, and a list of
# (anchor line, text inserted after it | text inserted before it) edits within that function.
HOOKS = [
    {
        "name": "VP6BRIDGE",
        "marker": "[NHL-VP6BRIDGE]",
        "func": "void func_00D79E90(ppu_context* ctx) {",
        "before_func": 'extern "C" void nhl_vp6_publish(uint32_t, uint32_t, uint32_t);   /* [NHL-VP6BRIDGE] */',
        "edits": [
            ("void func_00D79E90(ppu_context* ctx) {",
             "        const uint32_t _nhl_desc = (uint32_t)ctx->gpr[9], _nhl_w = (uint32_t)ctx->gpr[6], _nhl_h = (uint32_t)ctx->gpr[7];   /* [NHL-VP6BRIDGE] hand-added */"),
            ("loc_00D7A060:",
             "        nhl_vp6_publish(_nhl_desc, _nhl_w, _nhl_h);   /* [NHL-VP6BRIDGE] hand-added: host-decoded frame over the guest's */"),
        ],
    },
    {
        "name": "HEAPSLACK",
        "marker": "[NHL-HEAPSLACK]",
        "func": "void func_00C9DEC8(ppu_context* ctx) {",
        "before_func": 'extern "C" void nhl_heap_slack(ppu_context* ctx);   /* [NHL-HEAPSLACK] */',
        "edits": [
            ("void func_00C9DEC8(ppu_context* ctx) {",
             "        nhl_heap_slack(ctx);   /* [NHL-HEAPSLACK] hand-added: pad large requests (r4 = size), LOCAL_PATCHES #237 */"),
        ],
    },
    {
        "name": "REFPACK",
        "marker": "[NHL-REFPACK]",
        "func": "void func_016040E0(ppu_context* ctx) {",
        "before_func": 'extern "C" int nhl_refpack_inplace(ppu_context* ctx);   /* [NHL-REFPACK] */',
        "edits": [
            ("void func_016040E0(ppu_context* ctx) {",
             "        if (nhl_refpack_inplace(ctx)) return;   /* [NHL-REFPACK] hand-added: host decode of in-place RefPack streams, LOCAL_PATCHES #238 */"),
        ],
    },
    {
        "name": "STEPS",
        "marker": "[NHL-STEPS]",
        "func": "void func_014C80F4(ppu_context* ctx) {",
        "before_func": 'extern "C" volatile long long g_game_steps;   /* [NHL-STEPS] */',
        "edits": [
            ("void func_014C80F4(ppu_context* ctx) {",
             "        __atomic_fetch_add(&g_game_steps, 1, __ATOMIC_RELAXED);   /* [NHL-STEPS] hand-added: end of one sim step in func_000148F0's loop, LOCAL_PATCHES #269 */"),
        ],
    },
    {
        "name": "NOCROWD-CULL",
        "marker": "[NHL-NOCROWD]",
        "func": "void func_0156905C(ppu_context* ctx) {",
        "before_func": 'extern "C" int nhl_no_crowd(int);   /* [NHL-NOCROWD] */',
        "edits": [
            ("void func_0156905C(ppu_context* ctx) {",
             "        if (nhl_no_crowd(1)) return;   /* [NHL-NOCROWD] hand-added: no cull job, zero visible fans, the draw loops find empty pools, LOCAL_PATCHES #294 */"),
        ],
    },
    {
        "name": "NOCROWD-ANIM",
        "marker": "[NHL-NOCROWD]",
        "func": "void func_0145AD30(ppu_context* ctx) {",
        "before_func": 'extern "C" int nhl_no_crowd(int);   /* [NHL-NOCROWD] */',
        "edits": [
            ("void func_0145AD30(ppu_context* ctx) {",
             "        if (nhl_no_crowd(2)) return;   /* [NHL-NOCROWD] hand-added: UpdateCrowdJob body, crowd rigs stop animating, LOCAL_PATCHES #294 */"),
        ],
    },
    {
        "name": "SAVEBACK",
        "marker": "[NHL-SAVEBACK]",
        "func": "void func_00FBBF18(ppu_context* ctx) {",
        "before_func": 'extern "C" int cellSaveData_take_list_back(void);   /* [NHL-SAVEBACK] */',
        "edits": [
            ("void func_00FBBF18(ppu_context* ctx) {",
             "        if (cellSaveData_take_list_back() && (int32_t)vm_read32(ctx->gpr[3] + 0x28) == 1) { vm_write32(vm_read32(vm_read32(ctx->gpr[3] + 0x18) + 0x4C) + 0x10, 1); vm_write32(ctx->gpr[3] + 0x20, 1); return; }   /* [NHL-SAVEBACK] hand-added: backed out of the save list: answer Cancel (button 1) to the failed-load dialog without opening it, func_00FAF848 polls it next update, LOCAL_PATCHES #296 */"),
        ],
    },
    {
        "name": "SKIPINTRO",
        "marker": "[NHL-SKIPINTRO]",
        "func": "void func_00FB4804(ppu_context* ctx) {",
        "before_func": 'extern "C" int nhl_skip_intro(void);   /* [NHL-SKIPINTRO] */',
        "edits": [
            ("ctx->gpr[3] = vm_read8(ctx->gpr[3] + 0x63);",
             "        if (nhl_skip_intro()) goto loc_00FB4858;   /* [NHL-SKIPINTRO] hand-added: FlowBoot::Start's beq at 0x00FB4834 always taken (step 2, no autosave notice), Puck Peak's intro skip, LOCAL_PATCHES #297 */"),
        ],
    },
]


def find_file(func_line):
    for path in sorted(glob.glob(os.path.join(RECOMP, "ppu_recomp_*.cpp"))):
        with open(path, "r", encoding="utf-8", errors="surrogateescape", newline="") as f:
            text = f.read()
        if func_line in text:
            return path, text
    return None, None


def apply_hook(hook, check_only):
    path, text = find_file(hook["func"])
    if path is None:
        print(f"  {hook['name']}: FUNCTION NOT FOUND ({hook['func']})")
        return False
    name = os.path.basename(path)
    lines = text.split("\n")
    start = next(i for i, l in enumerate(lines) if l.startswith(hook["func"]))
    end = next((i for i in range(start + 1, len(lines)) if lines[i].rstrip("\r") == "}"), len(lines))
    if any(hook["marker"] in l for l in lines[max(0, start - 1):end]):
        print(f"  {hook['name']}: present in {name}")
        return True
    if check_only:
        print(f"  {hook['name']}: MISSING (would go in {name})")
        return False
    # Edits are applied bottom-up so earlier indices stay valid.
    targets = []
    for anchor, insert in hook["edits"]:
        idx = next((i for i in range(start, end) if lines[i].strip() == anchor.strip()), None)
        if idx is None:
            print(f"  {hook['name']}: anchor '{anchor}' not found in {name}; nothing changed")
            return False
        targets.append((idx, insert))
    eol = "\r" if lines[start].endswith("\r") else ""
    for idx, insert in sorted(targets, reverse=True):
        lines.insert(idx + 1, insert + eol)
    if hook["before_func"]:
        lines.insert(start, hook["before_func"] + eol)
    with open(path, "w", encoding="utf-8", errors="surrogateescape", newline="") as f:
        f.write("\n".join(lines))
    print(f"  {hook['name']}: applied to {name}")
    return True


# Edge Geometry job recon (nhl_edge.c, LOCAL_PATCHES #268). Stage numbers = ST_* there; the
# first line of a lifted SPU function holds its register cache, so hooks go after `_ls`.
EDGE_FILE = os.path.join(HERE, "..", "nhl", "port", "spu", "nhl_prog_01DBF980", "spu_recomp.c")
EDGE_FUNC = "void nhl_prog_01DBF980_spu_func_%08X(spu_context* ctx) {"
EDGE_MARKER = "/* [NHL-EDGE] */"
# NHL_EDGE_SKIP=1: skip the work and report "nothing visible", so 0x1D0 takes Edge's own
# empty-hole path at 0x8E8. Those draws vanish; measures what the job costs the sim.
EDGE_SKIP = " if (nhl_edge_skip()) SPU_RET(ctx);"
# NHL_EDGE_DUMP=<dir> (Edge GPU step 0): regs + LS snapshot at each stage entry while a segment is dumped
def _snap(st): return "nhl_edge_snap((const void*)ctx, ctx->ls, %d); " % st
# NHL_EDGE_NATIVE=1 (#292, nhl_edge_native.c): native decompress + skin + compress
def _native(st): return " if (nhl_edge_native((void*)ctx, ctx->ls, %d)) SPU_RET(ctx);" % st
EDGE_HOOKS = {0x968: "nhl_edge_mark(1);", 0x22F0: _snap(2) + "nhl_edge_mark(2);",
              0xA048: _snap(3) + "nhl_edge_mark(3);" + EDGE_SKIP + _native(3),
              0xA0F8: _snap(4) + "nhl_edge_mark(4);" + EDGE_SKIP + _native(4), 0x7010: _snap(5) + "nhl_edge_mark(5);" + EDGE_SKIP,
              0x5C08: _snap(6) + "nhl_edge_mark(6); if (nhl_edge_skip()) { ctx->gpr[3]._u32[0] = 0; SPU_RET(ctx); }" + _native(6),
              0x2DB0: _snap(7) + "nhl_edge_mark(7);", 0x2A00: _snap(8) + "nhl_edge_mark(8);",
              0x4C70: _snap(9) + "nhl_edge_mark(9);" + _native(9), 0x2BC0: _snap(10) + "nhl_edge_mark(10);",
              0x30B0: _snap(11) + "nhl_edge_mark(11);", 0x3398: _snap(12) + "nhl_edge_mark(12);",
              0x1D0: "nhl_edge_seg(ctx->ls, ctx->gpr[3]._u32[0]); " + _snap(0),
              0xB0F8: "nhl_edge_put(ctx->ls, ctx->gpr[3]._u32[0], ctx->gpr[4]._u32[1], ctx->gpr[5]._u32[0], ctx->gpr[0]._u32[0]);"}
EDGE_DECLS = ("void nhl_edge_mark(unsigned stage); void nhl_edge_seg(const uint8_t* ls, uint32_t lsa); "
              "void nhl_edge_put(const uint8_t* ls, uint32_t lsa, uint32_t ea, uint32_t size, uint32_t lr); "
              "void nhl_edge_snap(const void* regs, const uint8_t* ls, unsigned stage); "
              "int nhl_edge_native(void* c, uint8_t* ls, unsigned stage); "
              "int nhl_edge_skip(void);   " + EDGE_MARKER)


def apply_edge_hooks(check_only):
    with open(EDGE_FILE, "r", encoding="utf-8", errors="surrogateescape", newline="") as f:
        lines = f.read().split("\n")
    if EDGE_DECLS in lines and all(any(c in l for l in lines) for c in EDGE_HOOKS.values()):
        print("  EDGE: present")
        return True
    if check_only:
        print("  EDGE: MISSING")
        return False
    lines = [l for l in lines if EDGE_MARKER not in l]   # an older hook set
    index = {l.rstrip("\r"): i for i, l in enumerate(lines) if l.startswith("void nhl_prog_")}
    targets = []
    for addr, call in EDGE_HOOKS.items():
        start = index.get(EDGE_FUNC % addr)
        if start is None:
            print("  EDGE: function %X not found; nothing changed" % addr)
            return False
        ls_line = next(i for i in range(start, start + 4) if "_ls = ctx->ls" in lines[i])
        targets.append((ls_line, "        " + call + "   " + EDGE_MARKER))
    for i, text in sorted(targets, reverse=True):
        lines.insert(i + 1, text)
    first = next(i for i, l in enumerate(lines) if l.startswith("void nhl_prog_"))
    lines.insert(first, EDGE_DECLS)
    with open(EDGE_FILE, "w", encoding="utf-8", errors="surrogateescape", newline="") as f:
        f.write("\n".join(lines))
    print("  EDGE: applied")
    return True


# EALayer3 decoder job recon / corpus capture (nhl_eal3.c; NEXT_STEPS "DE-EMULATION"). Stage
# numbers = ES_* there. The job is relocatable, so its functions carry `_cb` (load base).
EAL3_FILE = os.path.join(HERE, "..", "nhl", "port", "spu", "nhl_job_01DE9800", "spu_recomp.c")
EAL3_FUNC = "void nhl_job_01DE9800_spu_func_%08X(spu_context* ctx) {"
EAL3_MARKER = "/* [NHL-EAL3] */"
def _eal3(st): return "nhl_eal3_stage((const void*)ctx, ctx->ls, _cb, %d);" % st
# NHL_EAL3_NATIVE=1 (#305, nhl_eal3_native.c): native IMDCT + synthesis on the same LS buffers
def _eal3n(st): return " if (nhl_eal3_native((const void*)ctx, ctx->ls, _cb, %d)) SPU_RET(ctx);" % st
EAL3_HOOKS = {0x9E18: _eal3(1), 0x8C40: _eal3(2), 0x9FA0: _eal3(3), 0x90A8: _eal3(4) + _eal3n(4), 0xA110: _eal3(5),
              0xA998: _eal3(6) + _eal3n(6), 0xA168: _eal3(7), 0xA1AC: _eal3(8), 0xA3A8: _eal3(9)}
EAL3_DECLS = ("void nhl_eal3_stage(const void* regs, const uint8_t* ls, uint32_t code_base, unsigned stage); "
              "int nhl_eal3_native(const void* regs, uint8_t* ls, uint32_t code_base, unsigned stage);   " + EAL3_MARKER)


def apply_spu_hooks(tag, path, func_fmt, marker, hooks, decls, check_only):
    with open(path, "r", encoding="utf-8", errors="surrogateescape", newline="") as f:
        lines = f.read().split("\n")
    if decls in lines and all(any(c.replace("<pre>", "") in l for l in lines) for c in hooks.values()):
        if not tag.startswith("DMALOOP"): print("  %s: present" % tag)
        return True
    if check_only:
        print("  %s: MISSING" % tag)
        return False
    lines = [l for l in lines if marker not in l]   # an older hook set
    prefix = func_fmt.split("%")[0]
    index = {l.rstrip("\r"): i for i, l in enumerate(lines) if l.startswith(prefix)}
    targets = []
    for addr, call in hooks.items():
        start = index.get(func_fmt % addr)
        if start is None:
            print("  %s: function %X not found; nothing changed" % (tag, addr))
            return False
        if call.startswith("<pre>"):   # before the register cache is loaded (the hook edits ctx->gpr)
            targets.append((start, "        " + call[5:] + "   " + marker))
            continue
        ls_line = next(i for i in range(start, start + 4) if "_ls = ctx->ls" in lines[i])
        targets.append((ls_line, "        " + call + "   " + marker))
    for i, text in sorted(targets, reverse=True):
        lines.insert(i + 1, text)
    first = next(i for i, l in enumerate(lines) if l.startswith(prefix))
    lines.insert(first, decls)
    with open(path, "w", encoding="utf-8", errors="surrogateescape", newline="") as f:
        f.write("\n".join(lines))
    print("  %s: applied" % tag)
    return True


# Native Huffman pair loop of the EALayer3 decoder (#314, nhl/port/nhl_eal3_huff.inc): the include
# carries the code (built with the job's own helpers), the hook sits at the loop head.
EAL3H_MARKER = "/* [NHL-EAL3H] */"
EAL3H_DECLS = '#include "../../nhl_eal3_huff.inc"   ' + EAL3H_MARKER
EAL3H_HOOKS = {0x4E3C: "<pre>{ const int _hm = nhl_eal3_huff_mode(); if (_hm == 1) nhl_eal3_pairs(ctx); "
                       "else if (_hm >= 2) nhl_eal3_huff_check(ctx, ctx->gpr, ctx->ls, nhl_eal3_pair_dry); }"}


# Native table scan in eacgfx_base_command_dispatch_job (nhl_cmdscan.c, #307). Lifted names = LS address.
CMDSCAN_FILE = os.path.join(HERE, "..", "nhl", "port", "spu", "nhl_prog_01DB3300", "spu_recomp.c")
CMDSCAN_FUNC = "void nhl_prog_01DB3300_spu_func_%08X(spu_context* ctx) {"
CMDSCAN_MARKER = "/* [NHL-CMDSCAN] */"
CMDSCAN_HOOKS = {0xB36C: "<pre>nhl_cmdscan((void*)ctx, ctx->ls, 0);", 0xB348: "nhl_cmdscan((void*)ctx, ctx->ls, 1);"}
CMDSCAN_DECLS = "void nhl_cmdscan(void* regs, const uint8_t* ls, unsigned site);   " + CMDSCAN_MARKER


# Native loop for EA's chunked DMA transfer helper (#309, nhl_cmdscan.c). The same helper sits in
# 68 lifted images; DMALOOP_STEP is its lifted code, chunk for chunk, on a register file in place.
DMALOOP_MARKER = "/* [NHL-DMALOOP] */"
DMALOOP_STEP = (
    "int nhl_dma_mode(void); void nhl_dma_check(void* ctx, void* g, int (*step)(void*, void*, int)); "
    "static int nhl_dma_step(void* cv, void* gv, int live) { spu_context* ctx = (spu_context*)cv; u128* g = (u128*)gv; "
    "g[3] = spu_ori(g[9], 0); g[10] = spu_shlqbyi(g[8], 4); g[4] = spu_clgti(g[5], 3); g[6] = spu_clgti(g[5], 1); "
    "if (g[5]._u32[0] == 0) return 0; "
    "g[30] = spu_il(1); g[31] = spu_il(2); g[27] = spu_ceqi(g[4], 0); if (live) spu_wrch(ctx, MFC_LSA, g[3]); "
    "g[26] = spu_selb(g[30], g[31], g[6]); g[15] = spu_shlqbyi(g[8], 0); g[29] = spu_clgti(g[5], 7); g[28] = spu_il(4); "
    "g[25] = spu_clgti(g[5], 15); g[23] = spu_ceqi(g[29], 0); g[22] = spu_selb(g[28], g[26], g[27]); "
    "if (live) spu_wrch(ctx, MFC_EAH, g[15]); g[24] = spu_il(8); if (live) spu_wrch(ctx, MFC_EAL, g[10]); "
    "g[21] = spu_clgt(g[5], g[14]); g[18] = spu_andi(g[5], -16); g[19] = spu_ceqi(g[25], 0); "
    "g[20] = spu_selb(g[24], g[22], g[23]); g[6] = spu_ceqi(g[21], 0); g[17] = spu_selb(g[18], g[20], g[19]); "
    "g[16] = spu_il(16384); g[7] = spu_selb(g[16], g[17], g[6]); g[5] = spu_sf(g[7], g[5]); "
    "g[4] = spu_rotqmbyi(g[7], -4); g[9] = spu_a(g[9], g[7]); "
    "if (live) { spu_wrch(ctx, MFC_Size, g[7]); spu_wrch(ctx, MFC_TagID, g[12]); spu_wrch(ctx, MFC_Cmd, g[11]); } "
    "g[3] = spu_cg(g[8], g[4]); g[2] = spu_shufb(g[3], g[3], g[13]); g[2] = spu_addx(g[8], g[4], g[2]); "
    "g[8] = spu_ori(g[2], 0); return 1; }   " + DMALOOP_MARKER)
DMALOOP_CALL = ("<pre>{ const int _dm = nhl_dma_mode(); if (_dm == 1) { while (nhl_dma_step(ctx, ctx->gpr, 1)) { } SPU_RET(ctx); } "
                "if (_dm >= 2) nhl_dma_check(ctx, ctx->gpr, nhl_dma_step); }")


def apply_dmaloop_hooks(check_only):
    sys.path.insert(0, os.path.join(HERE, "..", "x360"))
    saved = sys.argv
    sys.argv = ["dmahelper_scan.py", os.path.join(HERE, "..", "bin", "EBOOT_v101.elf"), os.path.join(HERE, "..", "nhl", "port", "spu")]
    try:
        import dmahelper_scan
    finally:
        sys.argv = saved
    ok = True; done = 0
    for kind, ea, cond in dmahelper_scan.hits:
        name = "nhl_%s_%08X" % (kind, ea)
        path = os.path.join(HERE, "..", "nhl", "port", "spu", name, "spu_recomp.c")
        fmt = "void " + name + "_spu_func_%08X(spu_context* ctx) {"
        if apply_spu_hooks("DMALOOP " + name, path, fmt, DMALOOP_MARKER, {cond: DMALOOP_CALL}, DMALOOP_STEP, check_only): done += 1
        else: ok = False
    print("  DMALOOP: %d of %d images" % (done, len(dmahelper_scan.hits)))
    return ok


def main():
    check_only = "--check" in sys.argv
    ok = all([apply_hook(h, check_only) for h in HOOKS]) and apply_edge_hooks(check_only)
    ok = apply_spu_hooks("EAL3", EAL3_FILE, EAL3_FUNC, EAL3_MARKER, EAL3_HOOKS, EAL3_DECLS, check_only) and ok
    ok = apply_spu_hooks("EAL3H", EAL3_FILE, EAL3_FUNC, EAL3H_MARKER, EAL3H_HOOKS, EAL3H_DECLS, check_only) and ok
    ok = apply_spu_hooks("CMDSCAN", CMDSCAN_FILE, CMDSCAN_FUNC, CMDSCAN_MARKER, CMDSCAN_HOOKS, CMDSCAN_DECLS, check_only) and ok
    ok = apply_dmaloop_hooks(check_only) and ok
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
