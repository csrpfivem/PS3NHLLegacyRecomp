"""Lift NHL's stage-0 SPU loader / Job Manager kernel into nhl/port/spu.

The stage-0 ELF (EBOOT VA 0x1E5DE80, 5,140 bytes, see SPU_RECON.md) has THREE
executable PT_LOADs, and spu_lifter/find_spu_functions only handle the first. So
each segment is lifted from a copy of the ELF in which only that segment keeps
PF_X and e_entry points at the segment's entry:

  A  LS 0x00080-0x00230  init: entry 0x90, one DMA, then `bra 0x3F000`
  B  LS 0x3EF80-0x3FE00  the kernel: main loop 0x3F01C, job-queue atomics
  C  LS 0x3FF80-0x40000  DMA helper + "load program at 0x80, br 0x3FBDC"

Only B and C are registered for address dispatch. Stage-0 loads every program
over LS 0x80, so A's bytes do not stay resident. The runtime never invalidates
lifted code that a DMA overwrites, so a registered A would shadow program code
at the same addresses. A is reached only directly, as the workload entry.

PROGRAMS (header type 0, see SPU_RECON.md) are found by scanning the EBOOT's SPU
code area for the body header. Stage-0 DMAs each one to LS 0x80, and they are
absolute-linked there. Each is lifted from a one-segment synthetic SPU ELF at
vaddr 0x80 and registered as an overlay region keyed by its EBOOT source EA
(spu_overlay_register_region). The MFC GET path marks it resident when stage-0
loads it, and dispatch inside its span then serves only its functions.

Writes nhl/port/spu/{nhl_jm_s0a,nhl_jm_s0b,nhl_jm_s0c,nhl_prog_<ea>}/spu_recomp.{c,h}
and nhl/port/spu/spu_workloads.c. Run with the port venv: python -I nhl_spu_lift.py
"""
import concurrent.futures, os, re, struct, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
W = os.path.dirname(HERE)
SDK_TOOLS = os.path.join(W, "ps3recomp", "ps3recomp-master", "tools")
EBOOT = os.path.join(W, "bin", "EBOOT_v101.elf")
SPU = os.path.join(W, "nhl", "port", "spu")
STAGE0_FILE_OFF, STAGE0_SIZE = 0x1E4DE80, 5140

SEGMENTS = [  # (name, segment LS vaddr, entry, extra entries, register for dispatch)
    ("nhl_jm_s0a", 0x00080, 0x00090, "", False),
    ("nhl_jm_s0b", 0x3EF80, 0x3F000, "", True),
    ("nhl_jm_s0c", 0x3FF80, 0x3FF80, "0x3FF98", True),
]

# EBOOT VAs scanned for SPU bodies. recon1 (front end only) saw 0x01DCD000..0x01E87000;
# the in-game programs/jobs live all over 0x01B00500..0x020DF860 (416 bodies in all,
# play47), so scan the whole text segment above the PPU code.
SPU_CODE_LO, SPU_CODE_HI = 0x01B00000, 0x020E0000
EBOOT_TEXT_VA = 0x10000                              # text segment: file offset = VA - this
BODY_MAGIC = 0x55011055
PROGRAM_LS_BASE = 0x80
PROGRAM_IMAGE_ID0 = 100   # clear of the ids the SDK special-cases (2, 6, 22, 200+)
# Span overrides: 0x01E6E580's last 0x1E0 bytes (LS 0x11DE0..) are overwritten by a
# separate GET from 0x01E802E0 (a shuffle-mask table). The GET path drops a resident
# region that a foreign DMA overlaps, so keep the span short of that tail.
PROGRAM_SPAN_OVERRIDE = {0x01E6E580: 0x11DE0 - PROGRAM_LS_BASE}
# Programs left interpreted by default (none). 0x01E26780 used to be listed: its
# breakage was the runtime's resident-region rejoin, not the lift (LOCAL_PATCHES #40/#41).
PROGRAM_SKIP_DEFAULT = ""
# JOBS (header type 1): position-independent, linked at 0, DMA'd by program
# 0x01E26780's job runner to runtime-chosen LSAs. Lifted --relocatable at base 0
# and registered as relocatable regions (spu_overlay_register_reloc_region).
JOB_IMAGE_ID0 = 1000      # 1000..: clear of the programs (100..) and the SDK's special ids

sys.path.insert(0, SDK_TOOLS)
import build_spu_workloads as bsw   # fnv1a64 / img_size / text_fp / debase_includes


def be32(b, o): return struct.unpack_from(">I", b, o)[0]


def find_bodies(eboot, body_type):
    """[(ea, entry_off, code_size)] for every body header of `body_type`
    (0 = program, 1 = job; see SPU_RECON.md)."""
    w = lambda va: be32(eboot, va - EBOOT_TEXT_VA)
    is_ila_r2 = lambda x: (x >> 25) == 0x21 and (x & 0x7F) == 2
    out, va = [], SPU_CODE_LO
    while va < SPU_CODE_HI:
        if all(is_ila_r2(w(va + 4 * i)) for i in range(4)) and w(va + 0x2C) == BODY_MAGIC:
            if w(va + 0x28) == body_type:
                out.append((va, w(va + 0x10), w(va + 0x14)))
            va += 0x30
        else:
            va += 0x10
    return out


def jump_table_entries(body, base):
    """Extra function entries for EA's switch jump tables: 12-byte stubs
    `ori rX,rY,0 ; brsl r0,<case> ; br <join>` reached by a computed bi (table +
    idx*12), which --auto-functions cannot see. Seeds each stub's start and its
    brsl return address (stub+8). Without them, a case function's return into the
    table left lifted code for unlifted LS, and the nested-interpreter fallback
    unwound the whole SPU thread (play49: job 0x0207AD00, 231 stubs; 5 of 6 Job
    Manager SPUs died in the EA SPORTS transition)."""
    w = lambda o: be32(body, o)
    out = []
    for off in range(0x30, len(body) - 12, 4):
        w0, w1, w2 = w(off), w(off + 4), w(off + 8)
        if ((w0 >> 24) == 0x04 and ((w0 >> 14) & 0x3FF) == 0          # ori rt, ra, 0
                and (w1 >> 23) == 0x066 and (w1 & 0x7F) == 0          # brsl r0, case
                and (w2 >> 23) == 0x064):                             # br join
            out += [base + off, base + off + 8]
    return out + offset_table_entries(body, base)


def offset_table_entries(body, base):
    """Case entries for the other switch shape: a table of 32-bit offsets relative
    to the table itself.
        ila   rT, TABLE              (jobs: a body offset, rebased by `a rT, rT, r126`)
        clgti rC, rIdx, N            (N+1 cases; out of range -> default branch)
        lqx/rotqby  word = TABLE[idx] ; a rD, word, rT ; bi rD
    The case targets (TABLE + word) are only reachable through that bi, so
    --auto-functions never lifts them and the job ended in the interpreter
    (play64: job 0x01F01480, table 0x58EC, 65 cases, `branched into unlifted LS
    0x16AEC` in the match). Every entry must be an in-body, word-aligned target or
    the candidate is rejected."""
    w = lambda o: be32(body, o)
    out = []
    n = len(body)
    for off in range(0x30, n - 4, 4):
        i0 = w(off)
        if (i0 >> 25) != 0x21:                                     # ila rT, imm18
            continue
        tbl, rt = (i0 >> 7) & 0x3FFFF, i0 & 0x7F
        toff = tbl - base
        if tbl & 3 or toff < 0x30 or toff >= n:
            continue
        cases = None
        for k in range(1, 9):                                      # clgti within 8 insns
            if off + 4 * k >= n:
                break
            ik = w(off + 4 * k)
            if (ik >> 24) == 0x5C:
                cases = ((ik >> 14) & 0x3FF) + 1
                break
        if not cases or cases > 512 or toff + 4 * cases > n:
            continue
        if not any((w(off + 4 * k) >> 21) == 0x1A8 for k in range(1, 16) if off + 4 * k < n):   # bi
            continue
        if not any(((w(off + 4 * k) >> 21) == 0x0C0 and                                  # a rD, rT, r126 / rT used
                    ((w(off + 4 * k) >> 7) & 0x7F) == rt or ((w(off + 4 * k) >> 14) & 0x7F) == rt)
                   for k in range(1, 6) if off + 4 * k < n):
            continue
        tgts = []
        for c in range(cases):
            word = struct.unpack_from(">i", body, toff + 4 * c)[0]
            t = tbl + word
            if t & 3 or not (base + 0x30 <= t < base + n):
                tgts = None
                break
            tgts.append(t)
        if tgts:
            out += sorted(set(tgts))
    return out


def find_programs(eboot):
    return find_bodies(eboot, 0)


def program_elf(body, entry_ls, base=PROGRAM_LS_BASE):
    """A minimal SPU ELF: one R+X PT_LOAD of `body` at LS `base`."""
    hdr = bytearray(0x100)
    hdr[0:4] = b"\x7fELF"; hdr[4] = 1; hdr[5] = 2; hdr[6] = 1           # ELF32, MSB, v1
    struct.pack_into(">HHIIIIIHHHHHH", hdr, 0x10, 2, 0x17, 1, entry_ls, 0x34, 0, 0,
                     0x34, 0x20, 1, 0x28, 0, 0)
    struct.pack_into(">IIIIIIII", hdr, 0x34, 1, 0x100, base, base,
                     len(body), len(body), 5, 0x80)
    return bytes(hdr) + body


def lift(name, elf_path, extra="", reloc=False, drop=None):
    out = os.path.join(SPU, name)
    os.makedirs(out, exist_ok=True)
    cmd = [sys.executable, os.path.join(SDK_TOOLS, "spu_lifter.py"), "--auto-functions", elf_path,
           "--symbol-prefix", name + "_", "-o", out,
           # Calls through non-$r0 link registers (brsl $r75/$r79) and conditional
           # returns from split retry loops: make every call's return address an
           # entry so such returns resume by dispatch.
           "--return-entries"]
    if extra: cmd += ["--extra-funcs", extra]
    # Jobs are position-independent and land at runtime-chosen LSAs: lift once at
    # base 0, run at ctx->code_base (LOCAL_PATCHES #43).
    if reloc: cmd += ["--relocatable"]
    # SPU registers in C locals (LOCAL_PATCHES #135); NHL_SPU_PROMOTE=0 = ctx->gpr everywhere.
    if os.environ.get("NHL_SPU_PROMOTE", "1") != "0": cmd += ["--promote-regs"]
    # [LOCAL_PATCHES #144] Contiguous chunks from one call target to the next become ONE C
    # function (branches between them are gotos), so promoted registers stay in host
    # registers across them instead of a full write-back + reload at every chunk boundary
    # (prog 01DBF980's hot loop A71C -> A7C8 -> A9A8 reloaded ~73 and stored ~57 vectors per
    # pass). Every chunk is still emitted and registered on its own for outside entry.
    # OPT-IN (NHL_SPU_MERGE=1). Merging alone is about 25% SLOWER (speed4): the loops get far
    # better, but a merged function has every register of the group promoted, so each call
    # writes back and reloads all of them (0A120: 208 vector moves per call, chunked ~60, even
    # around 5-instruction helpers). --call-summaries is what makes the calls cheap again.
    # NHL_SPU_MERGE_ONLY=<name-part>[,...]: merge + summaries for just those lifts.
    merge_only = [s for s in os.environ.get("NHL_SPU_MERGE_ONLY", "").split(",") if s]
    merged_here = any(s.upper() in name.upper() for s in merge_only)
    if os.environ.get("NHL_SPU_MERGE", "0") == "1" or merged_here: cmd += ["--merge-chunks"]
    # [#144] Per-callee call summaries: OPT-IN (NHL_SPU_SUMMARIES=1).
    if os.environ.get("NHL_SPU_SUMMARIES", "0") == "1" or merged_here: cmd += ["--call-summaries"]
    # NHL_SPU_TRACE=1: per-instruction spu_trace_pc hooks, for the SPU_LOCKCHECK
    # lifted-vs-interpreter lockstep (spu_interp.c). Much slower; checker builds only.
    # NHL_SPU_TRACE_ONLY=<name-part>[,...]: trace just those lifts (e.g. 01E03480), so a
    # play test can lockstep a few suspect jobs while everything else runs at speed.
    only = [s for s in os.environ.get("NHL_SPU_TRACE_ONLY", "").split(",") if s]
    if os.environ.get("NHL_SPU_TRACE") == "1" or any(s.upper() in name.upper() for s in only):
        cmd += ["--trace"]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.stderr.write(f"LIFT FAILED {name}\n" + r.stdout[-2000:] + r.stderr[-2000:])
        return None
    stats = [l.strip() for l in (r.stdout + r.stderr).splitlines()
             if "function(s) lifted" in l or "Function coverage" in l]
    print("LIFT", name, *stats, flush=True)
    for f in ("spu_recomp.h", "spu_recomp.c"):
        bsw.debase_includes(os.path.join(out, f))
    if drop:
        # --auto-functions decodes the body header's data words (entry/size/stack/
        # flags/type/magic) as code. Never register those: a "function" at LS 0x90
        # is where stage-0's entry lives, and spu_lifted_thread_available(0x90) then
        # started the Job Manager threads as that program (run spuall1).
        lo, hi = drop
        src = os.path.join(out, "spu_recomp.c")
        rows = open(src, encoding="utf-8").read().split("\n")
        keep = [r for r in rows
                if not ((m := re.match(r"\s*\{ 0x([0-9A-F]{8})u, ", r)) and lo <= int(m.group(1), 16) < hi)]
        open(src, "w", encoding="utf-8", newline="\n").write("\n".join(keep))
    return open(os.path.join(out, "spu_recomp.h"), encoding="utf-8", errors="ignore").read()


def segment_elf(elf, vaddr, entry):
    b = bytearray(elf)
    phoff, phes, phn = be32(b, 0x1C), struct.unpack_from(">H", b, 0x2A)[0], struct.unpack_from(">H", b, 0x2C)[0]
    found = False
    for i in range(phn):
        o = phoff + i * phes
        if be32(b, o) != 1: continue
        flags = be32(b, o + 0x18)
        if be32(b, o + 8) == vaddr:
            found = True
            struct.pack_into(">I", b, o + 0x18, flags | 1)
        else:
            struct.pack_into(">I", b, o + 0x18, flags & ~1)
    if not found: sys.exit(f"no PT_LOAD at {vaddr:#x}")
    struct.pack_into(">I", b, 0x18, entry)
    return bytes(b)


def main():
    eboot = open(EBOOT, "rb").read()
    elf = eboot[STAGE0_FILE_OFF:STAGE0_FILE_OFF + STAGE0_SIZE]
    assert bsw.is_spu_elf(elf)
    fp = bsw.fnv1a64(elf[:bsw.img_size(elf)])
    tfp = bsw.text_fp(elf)
    os.makedirs(os.path.join(SPU, "images"), exist_ok=True)
    open(os.path.join(SPU, "images", "nhl_jm_stage0.elf"), "wb").write(elf)
    for name, vaddr, entry, extra, _ in SEGMENTS:
        seg = os.path.join(SPU, "images", name + ".elf.seg")
        open(seg, "wb").write(segment_elf(elf, vaddr, entry))
        hdr = lift(name, seg, extra)
        if not hdr or f"{name}_spu_func_{entry:08X}(" not in hdr: sys.exit(f"{name}: entry {entry:#x} not lifted")

    # Programs (fixed LS 0x80) and jobs (relocatable, base 0). 416 bodies: lift them in
    # parallel; a body that fails to lift (or loses its entry) is skipped -- it simply
    # stays interpreted -- instead of aborting the whole run.
    tasks = []      # (kind, name, ea, span, image_id, elf_path, entry, reloc, extra entries)
    for i, (ea, entry_off, csz) in enumerate(find_programs(eboot)):
        name = f"nhl_prog_{ea:08X}"
        entry_ls = PROGRAM_LS_BASE + entry_off
        body = eboot[ea - EBOOT_TEXT_VA: ea - EBOOT_TEXT_VA + csz]
        pelf = os.path.join(SPU, "images", name + ".elf")
        open(pelf, "wb").write(program_elf(body, entry_ls))
        tasks.append(("prog", name, ea, PROGRAM_SPAN_OVERRIDE.get(ea, csz), PROGRAM_IMAGE_ID0 + i,
                      pelf, entry_ls, False, jump_table_entries(body, PROGRAM_LS_BASE)))
    for i, (ea, entry_off, csz) in enumerate(find_bodies(eboot, 1)):
        name = f"nhl_job_{ea:08X}"
        body = eboot[ea - EBOOT_TEXT_VA: ea - EBOOT_TEXT_VA + csz]
        jelf = os.path.join(SPU, "images", name + ".elf")
        open(jelf, "wb").write(program_elf(body, entry_off, base=0))
        tasks.append(("job", name, ea, csz, JOB_IMAGE_ID0 + i, jelf, entry_off, True,
                      jump_table_entries(body, 0)))
    assert PROGRAM_IMAGE_ID0 + sum(t[0] == "prog" for t in tasks) <= JOB_IMAGE_ID0

    def run(t):
        # Programs: drop header-word "functions" (keep LS 0x80, the ila r2 run the
        # 19 front-end programs always registered).
        drop = None if t[7] else (PROGRAM_LS_BASE + 4, PROGRAM_LS_BASE + 0x30)
        hdr = lift(t[1], t[5], extra=",".join(f"0x{a:X}" for a in t[8]), reloc=t[7], drop=drop)
        return t if hdr and f"{t[1]}_spu_func_{t[6]:08X}(" in hdr else None
    jobs_n = int(os.environ.get("NHL_SPU_LIFT_JOBS", str(os.cpu_count() or 4)))
    with concurrent.futures.ThreadPoolExecutor(jobs_n) as ex:
        done = list(ex.map(run, tasks))
    failed = [t[1] for t, d in zip(tasks, done) if d is None]
    programs = [(t[1], t[2], t[3], t[4]) for t in done if t and t[0] == "prog"]
    jobs = [(t[1], t[2], t[3], t[4]) for t in done if t and t[0] == "job"]
    print(f"{len(programs)} programs, {len(jobs)} jobs lifted; {len(failed)} skipped {failed}")

    a = SEGMENTS[0][0]
    L = ["/* spu_workloads.c -- GENERATED by tools/nhl_spu_lift.py; do not edit.",
         " * Registers NHL's lifted stage-0 SPU kernel (see SPU_RECON.md). Segments B and C",
         " * are registered for address dispatch under image 1. Segment A is entry-only: stage-0",
         " * loads programs over LS 0x80, so a registered A would shadow program code.",
         f" * The {len(programs)} programs are overlay regions keyed by their EBOOT source EA (images",
         f" * {PROGRAM_IMAGE_ID0}..{PROGRAM_IMAGE_ID0 + len(programs) - 1}).",
         " * NHL_SPU_LIFT: unset/2 = kernel + programs (default), 1 = kernel only, 0 = nothing",
         " * (the stage-0 threads then fall back to the interpreter, RD_SPU_INTERP). */",
         '#include "spu_workload.h"', "#include <stdlib.h>", "",
         "extern void spu_begin_image(int image_id);",
         f"extern void {a}_spu_func_{SEGMENTS[0][2]:08X}(spu_context*);"]
    for name, _, _, _, reg in SEGMENTS:
        if reg: L.append(f"extern void {name}_spu_recomp_register(void);")
    L.append("extern void spu_overlay_register_region_at(uint32_t content_ea, uint32_t span, int image_id, uint32_t load_lsa);")
    L.append("extern void spu_overlay_register_reloc_region(uint32_t content_ea, uint32_t span, int image_id);")
    for name, _, _, _ in programs + jobs:
        L.append(f"extern void {name}_spu_recomp_register(void);")
    L += ["",
          "/* NHL_SPU_PROG_SKIP=<ea>[,<ea>...] (hex): leave those programs interpreted. Unset =",
          f" * the known-bad default \"{PROGRAM_SKIP_DEFAULT}\"; set it empty to lift every program. */",
          "static int nhl_prog_skipped(uint32_t ea)",
          "{",
          '    const char* e = getenv("NHL_SPU_PROG_SKIP");',
          f'    if (!e) e = "{PROGRAM_SKIP_DEFAULT}";',
          "    while (e && *e) {",
          "        char* end; unsigned long v = strtoul(e, &end, 16);",
          "        if (end == e) break;",
          "        if ((uint32_t)v == ea) return 1;",
          "        e = (*end == ',') ? end + 1 : end;",
          "    }",
          "    return 0;",
          "}",
          "",
          "/* NHL_SPU_JOB_SKIP=<ea>[,<ea>...] (hex): leave those jobs interpreted (bisecting). */",
          "static int nhl_job_skipped(uint32_t ea)",
          "{",
          '    const char* e = getenv("NHL_SPU_JOB_SKIP");',
          "    while (e && *e) {",
          "        char* end; unsigned long v = strtoul(e, &end, 16);",
          "        if (end == e) break;",
          "        if ((uint32_t)v == ea) return 1;",
          "        e = (*end == ',') ? end + 1 : end;",
          "    }",
          "    return 0;",
          "}"]
    L += ["", "void nhl_spu_register_all(void)", "{",
          '    { const char* e = getenv("NHL_SPU_LIFT"); if (e && *e == \'0\') return; }',
          "    spu_begin_image(1);"]
    for name, _, _, _, reg in SEGMENTS:
        if reg: L.append(f"    {name}_spu_recomp_register();")
    L.append(f"    spu_workload_register_img(0x{fp:016X}ULL, {a}_spu_func_{SEGMENTS[0][2]:08X}, 1, \"nhl_jm_stage0\");")
    if tfp:
        L.append(f"    spu_workload_register_img(0x{tfp:016X}ULL, {a}_spu_func_{SEGMENTS[0][2]:08X}, 1, \"nhl_jm_stage0.text\");")
    L.append("    /* Programs are absolute-linked at LS 0x80 and only marked resident when a GET lands")
    L.append("     * them there (a type-0 body streamed anywhere else stays interpreted).")
    L.append("     * Programs (LS 0x80, one resident at a time) are on by default; NHL_SPU_LIFT=1 keeps")
    L.append("     * only the kernel. They are instruction-exact (SPU_LOCKCHECK: 3.7G instructions, 0")
    L.append("     * divergences). The post-intro derail once blamed on them was the game's heap overwriting")
    L.append("     * the injected GCM ctrl->ref at 0x20000000 (boot_main.cpp now puts it at 0xBF000000). */")
    L.append('    { const char* e = getenv("NHL_SPU_LIFT"); if (e && *e == \'1\') { spu_begin_image(0); return; } }')
    for name, ea, span, iid in programs:
        L.append(f"    if (!nhl_prog_skipped(0x{ea:08X}u)) {{ spu_begin_image({iid}); {name}_spu_recomp_register(); "
                 f"spu_overlay_register_region_at(0x{ea:08X}u, 0x{span:X}u, {iid}, 0x{PROGRAM_LS_BASE:X}u); }}")
    L.append(f"    /* The {len(jobs)} jobs (images {JOB_IMAGE_ID0}..{JOB_IMAGE_ID0 + len(jobs) - 1}) are relocatable regions:")
    L.append("     * one lift at base 0 runs at whatever LSA the job runner loads them to")
    L.append("     * (LOCAL_PATCHES #43). On by default; NHL_SPU_JOBS=0 leaves them interpreted.")
    L.append("     * They only run compiled with SPU_RESIDENT_REJOIN=1 (run_nhl.ps1 sets it; SDK #44). */")
    L.append('    { const char* e = getenv("NHL_SPU_JOBS"); if (e && *e == \'0\') { spu_begin_image(0); return; } }')
    for name, ea, span, iid in jobs:
        L.append(f"    if (!nhl_job_skipped(0x{ea:08X}u)) {{ spu_begin_image({iid}); {name}_spu_recomp_register(); "
                 f"spu_overlay_register_reloc_region(0x{ea:08X}u, 0x{span:X}u, {iid}); }}")
    L += ["    spu_begin_image(0);", "}", "",
          "/* #257: a dual build compiles this file twice, NHL_SPU_VARIANT=512 (AVX-512) and =2",
          " * (AVX2, tools/nhl_spu_dual.py renames the symbols); only the set matching the CPU",
          " * registers. Without NHL_SPU_VARIANT (single-ISA build) it always registers. */",
          "#ifdef NHL_SPU_VARIANT",
          "extern int nhl_spu_isa_avx512(void);",
          "#endif",
          "__attribute__((constructor)) static void nhl_spu_register_all_ctor(void)",
          "{",
          "#ifdef NHL_SPU_VARIANT",
          "    if (nhl_spu_isa_avx512() != (NHL_SPU_VARIANT == 512)) return;",
          "#endif",
          "    nhl_spu_register_all();", "}", ""]
    open(os.path.join(SPU, "spu_workloads.c"), "w", encoding="utf-8", newline="\n").write("\n".join(L))
    print(f"wrote spu_workloads.c (fp {fp:016X}, text fp {tfp:016X})")
    sys.path.insert(0, HERE)   # -I leaves the script's own folder off sys.path
    import nhl_spu_dual
    nhl_spu_dual.main()


if __name__ == "__main__":
    main()
