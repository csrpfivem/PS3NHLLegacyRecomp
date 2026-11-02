"""Removes dead flag updates from the lifted PPU code (LOCAL_PATCHES #272).

The lifter writes XER.CA on every carry-form add (NHL's SN compiler uses addic as a plain add)
and a whole CR field on every compare, then tests the field with a shift. Nearly all of those
writes are never read. This pass runs a backward liveness analysis over each lifted function,
on the generated text, so the output that has been played stays exactly as it is otherwise:

  * a CA update nothing can read is dropped;
  * a compare whose field is read only by the branch right after it becomes `if (a < b) goto`
    (the lifter's own CR fusion stopped matching when compares gained the SO bit);
  * any other CR write nothing can read is dropped.

What counts as a read at function edges follows the PPC64 ABI:
  * a call (`func_X(ctx)`, ps3_indirect_call): XER.CA and CR0/1/5/6/7 are volatile, so they are
    dead going in. CR2-4 are preserved by the callee, so they stay live through it.
  * `return` (blr): volatile fields are dead, CR2-4 are live (the caller may read them).
  * anything else that leaves the function (trampoline tail call, syscall, unknown shapes)
    keeps everything live, because a lifted fragment can continue in another function.
A line this pass does not understand is treated as reading everything, so it can only keep a
write it could have dropped. Idempotent: the rewritten lines carry no flag writes to find.

    python -I nhl_ppu_flags.py <file.cpp>...   rewrite in place; prints per-file counts
    python -I nhl_ppu_flags.py --dry <file>    counts only
"""
import concurrent.futures
import glob
import re
import sys

ALL = (1 << 9) - 1            # bit 0-7 = CR fields 0-7, bit 8 = XER.CA
CA = 1 << 8
CR_VOLATILE = 0b11100011      # CR0, CR1, CR5, CR6, CR7
CR_PRESERVED = 0b00011100     # CR2, CR3, CR4
FUNC = re.compile(r"^(?:static )?void func_[0-9A-F]{8}\(ppu_context\* ctx\) \{")
LABEL = re.compile(r"^\s*(loc_[0-9A-F]+):")
GOTO = re.compile(r"\bgoto (loc_[0-9A-F]+);")
COND_GOTO = re.compile(r"^\s*if \(.*\) goto loc_[0-9A-F]+;\s*(/\*.*\*/)?\s*$")
UNCOND_GOTO = re.compile(r"^\s*goto loc_[0-9A-F]+;\s*$")
CA_WRITE = re.compile(r"ctx->xer = \(ctx->xer & ~\(1u << 29\)\) \| \(.*?\? \(1u << 29\) : 0u?\); ?")
CA_READ = re.compile(r"\(ctx->xer >> 29\)")
SO_READ = re.compile(r"\(\(ctx->xer >> 31\) & 1u\)")
CR_SET = re.compile(r"^(\s*)\{ (\w+) a = (.*?); (\w+) b = (.*?); uint32_t cr_val = "
                    r"\(\(a < b\) \? 8 : \(a > b\) \? 4 : 2\) \| \(\(ctx->xer >> 31\) & 1u\); "
                    r"ctx->cr = \(ctx->cr & ~\(0xFu << (\d+)\)\) \| \(cr_val << \6\); \}\s*$")
CR_FIELD_WRITE = re.compile(r"ctx->cr = \(ctx->cr & ~\(0xFu << (\d+)\)\)")
CR_BR = re.compile(r"^(\s*)if \(+(!?)\(*ctx->cr >> (\d+)\) & (0x[0-9A-Fa-f]+|\d+)\)+ (goto loc_[0-9A-F]+;)\s*$")
CR_READ = re.compile(r"ctx->cr >> (\d+)\)")
CALL = re.compile(r"^\s*ctx->lr = [^;]+; func_[0-9A-F]{8}\(ctx\); DRAIN_TRAMPOLINE\(ctx\);\s*$")
ICALL = re.compile(r"^\s*ctx->lr = [^;]+; if \(.*\) \{ ps3_indirect_call\(ctx\); DRAIN_TRAMPOLINE\(ctx\); \}\s*$")
RETURN = re.compile(r"^\s*return;\s*$")
CTX_PASSED = re.compile(r"\bctx\b(?!->)")


def fld(shift):
    """CR field number for a shift that may point at any bit inside it."""
    return 7 - (int(shift) // 4)


def classify(line):
    """(reads, kills) of one line. Reading everything is the safe default."""
    if CTX_PASSED.search(line):
        return ALL, 0   # a helper that gets the context may read any flag
    if "ctx->cr" not in line and "ctx->xer" not in line:
        if "func_" in line or "trampoline" in line or "ps3_indirect" in line or "lv2_syscall" in line or "return" in line:
            return ALL, 0
        return 0, 0
    if SO_READ.search(line) or "ctx->xer" in line or "ctx->cr" in line:
        r = k = 0
        rest = line
        m = CA_WRITE.search(rest)
        if m:
            k |= CA
            rest = rest[:m.start()] + rest[m.end():]
        if CA_READ.search(rest):
            r |= CA
            rest = CA_READ.sub("", rest)
        for m in CR_READ.finditer(rest):
            r |= 1 << fld(m[1])
        w = CR_FIELD_WRITE.findall(rest)
        for s in w:
            k |= 1 << fld(s)
        rest = CR_READ.sub("", rest)
        rest = re.sub(r"ctx->cr = \(ctx->cr & ~\(0xFu << \d+\)\) \| \(\w+ << \d+\);", "", rest)
        rest = SO_READ.sub("", rest)
        if "ctx->cr" in rest or "ctx->xer" in rest:   # mfcr, mtcr, &ctx->xer, crand...
            return ALL, 0
        return r, k & ~r
    return ALL, 0


def process(body):
    """body: function lines without the header and closing brace. Returns (lines, stats)."""
    n = len(body)
    labels = {}
    for i, l in enumerate(body):
        m = LABEL.match(l)
        if m:
            labels[m[1]] = i
    reads, kills, succ, gen_exit = [0] * n, [0] * n, [()] * n, [0] * n
    for i, l in enumerate(body):
        fall = (i + 1,) if i + 1 < n else ()
        reads[i], kills[i] = classify(l)
        if RETURN.match(l):
            succ[i], reads[i] = (), CR_PRESERVED
        elif CALL.match(l) or ICALL.match(l):
            # volatile state is dead into a call; CR2-4 pass through it untouched
            succ[i], reads[i], kills[i] = fall, 0, CA | CR_VOLATILE
        elif UNCOND_GOTO.match(l):
            t = labels.get(GOTO.search(l)[1])
            if t is None:
                succ[i], reads[i] = (), ALL
            else:
                succ[i] = (t,)
        elif COND_GOTO.match(l) and GOTO.search(l):
            t = labels.get(GOTO.search(l)[1])
            if t is None:
                reads[i] = ALL
                succ[i] = fall
            else:
                succ[i] = fall + (t,)
        elif "goto" in l:
            succ[i], reads[i] = fall + tuple(labels[x] for x in GOTO.findall(l) if x in labels), ALL
        else:
            if "return" in l:   # conditional blr, hooks: any field may be read past it
                reads[i] = ALL
            succ[i] = fall
        if not succ[i] and not RETURN.match(l):
            gen_exit[i] = ALL   # falling off the end continues in the next lifted fragment
    live = [0] * (n + 1)
    changed = True
    while changed:
        changed = False
        for i in range(n - 1, -1, -1):
            out = gen_exit[i]
            for s in succ[i]:
                out |= live[s]
            v = reads[i] | (out & ~kills[i])
            if v != live[i]:
                live[i] = v
                changed = True
    def live_out(i):
        out = gen_exit[i]
        for s in succ[i]:
            out |= live[s]
        return out
    st = {"ca": 0, "fused": 0, "cr": 0}
    out = []
    i = 0
    while i < n:
        l = body[i]
        m = CR_SET.match(l)
        if m:
            f = 1 << fld(m[6])
            br = CR_BR.match(body[i + 1]) if i + 1 < n else None
            if br and fld(br[3]) == fld(m[6]) and not live_out(i + 1) & f:
                mask = int(br[4], 0)
                # the branch's shift names the field's low bit; a mask of 8/4/2 picks LT/GT/EQ
                sh = int(br[3]) - int(m[6])
                bit = (mask << sh) if sh >= 0 else 0
                op = {8: "<", 4: ">", 2: "==", 10: "<=", 12: ">="}.get(bit)
                if op:
                    cond = "(%s)(%s) %s (%s)(%s)" % (m[2], m[3], op, m[4], m[5])
                    out.append("%sif (%s(%s)) %s" % (br[1], "!" if br[2] else "", cond, br[5]))
                    st["fused"] += 1
                    i += 2
                    continue
            if not live_out(i) & f:
                st["cr"] += 1
                i += 1
                continue
        if CA_WRITE.search(l) and not CA_READ.search(l) and not live_out(i) & CA:
            l = CA_WRITE.sub("", l)
            st["ca"] += 1
        out.append(l)
        i += 1
    return out, st


def rewrite(text):
    lines = text.split("\n")
    out, i, n = [], 0, len(lines)
    tot = {"ca": 0, "fused": 0, "cr": 0, "funcs": 0}
    while i < n:
        out.append(lines[i])
        if FUNC.match(lines[i]):
            j = i + 1
            while j < n and lines[j].rstrip("\r") != "}":
                j += 1
            eol = "\r" if lines[i].endswith("\r") else ""
            body = [x.rstrip("\r") for x in lines[i + 1:j]]
            new, st = process(body)
            out.extend(x + eol for x in new)
            for k, v in st.items():
                tot[k] += v
            tot["funcs"] += 1
            i = j
            continue
        i += 1
    return "\n".join(out), tot


def one(job):
    p, dry = job
    with open(p, "r", encoding="utf-8", errors="surrogateescape", newline="") as f:
        text = f.read()
    new, st = rewrite(text)
    if not dry and new != text:
        with open(p, "w", encoding="utf-8", errors="surrogateescape", newline="") as f:
            f.write(new)
    return st


def main():
    args = [f for a in sys.argv[1:] if a != "--dry" for f in sorted(glob.glob(a))]
    dry = "--dry" in sys.argv
    grand = {"ca": 0, "fused": 0, "cr": 0, "funcs": 0}
    with concurrent.futures.ProcessPoolExecutor(max_workers=12) as ex:
        for st in ex.map(one, [(p, dry) for p in args]):
            for k, v in st.items():
                grand[k] += v
    print("functions %d | CA writes removed %d | compares fused %d | CR writes removed %d" % (
        grand["funcs"], grand["ca"], grand["fused"], grand["cr"]))


if __name__ == "__main__":
    main()
