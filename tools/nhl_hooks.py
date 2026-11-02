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


def main():
    check_only = "--check" in sys.argv
    ok = all([apply_hook(h, check_only) for h in HOOKS]) and apply_edge_hooks(check_only)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
