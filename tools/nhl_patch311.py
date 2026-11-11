# #311: add the inline MFC_WrTagMask / MFC_WrTagUpdate fast paths to every generated spu_recomp.h
# (same text as the spu_lifter.py template, so a re-lift produces it too). Idempotent.
# Also mirrors tag completion into spu_context.mfc_tag_pending in runtime/spu/spu_dma.h.
import glob, os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
OLD = "    case MFC_TagID: ctx->mfc_tag  = v._u32[0] & 0x1F; return;\n    default:        spu_wrch(ctx, ch, v); return;"
NEW = ("    case MFC_TagID: ctx->mfc_tag  = v._u32[0] & 0x1F; return;\n"
       "    /* [NHL local patch #311] the tag wait after every transfer: DMA is synchronous, so the update is \"completed tags & mask\"; mfc_tag_pending mirrors ~mfc->tag_completed (spu_dma.h). The tag-event producer (event mask bit 0) keeps the real function. */\n"
       "    case MFC_WrTagMask: ctx->mfc_tag_mask = v._u32[0]; return;\n"
       "    case MFC_WrTagUpdate: if (__builtin_expect(v._u32[0] != 0 && (ctx->event_mask & 0x1u), 0)) { spu_wrch(ctx, ch, v); return; } ctx->mfc_tag_status = ~ctx->mfc_tag_pending & ctx->mfc_tag_mask; return;\n"
       "    default:        spu_wrch(ctx, ch, v); return;")
def rw(path, fn):
    with open(path, "r", encoding="utf-8", errors="surrogateescape", newline="") as f: t = f.read()
    crlf = "\r\n" in t
    u = fn(t.replace("\r\n", "\n"))
    if u is None: return False
    if crlf: u = u.replace("\n", "\r\n")
    with open(path, "w", encoding="utf-8", errors="surrogateescape", newline="") as f: f.write(u)
    return True
n = done = already = 0
for h in glob.glob(os.path.join(HERE, "..", "nhl", "port", "spu*", "*", "spu_recomp.h")):
    n += 1
    def fn(t):
        global already
        if "MFC_WrTagUpdate: if (__builtin_expect" in t: already += 1; return None
        return t.replace(OLD, NEW) if OLD in t else None
    if rw(h, fn): done += 1
print("headers %d: patched %d, already %d, untouched %d" % (n, done, already, n - done - already))
dma = os.path.join(HERE, "..", "ps3recomp", "ps3recomp-master", "runtime", "spu", "spu_dma.h")
SUBS = [
    ("    if (!(spu->list_stall_mask & (1u << t)))      /* ran to the end: tag done */\n        mfc->tag_completed |= (1u << t);",
     "    if (!(spu->list_stall_mask & (1u << t))) {    /* ran to the end: tag done */\n        mfc->tag_completed |= (1u << t);\n        spu->mfc_tag_pending = ~mfc->tag_completed;   /* #311 mirror */\n    }"),
    ("    /* Mark tag as in-progress */\n    mfc->tag_completed &= ~(1u << tag);",
     "    /* Mark tag as in-progress */\n    mfc->tag_completed &= ~(1u << tag);\n    spu->mfc_tag_pending = ~mfc->tag_completed;   /* #311 mirror */"),
    ("        mfc->tag_completed |= (1u << tag);\n        return 0;\n    }",
     "        mfc->tag_completed |= (1u << tag);\n        spu->mfc_tag_pending = ~mfc->tag_completed;   /* #311 mirror */\n        return 0;\n    }"),
    ("        ((cmd & 0xFFFFu) != MFC_SDCRZ_CMD || rc == 0))\n        mfc->tag_completed |= (1u << tag);\n\n    return rc;",
     "        ((cmd & 0xFFFFu) != MFC_SDCRZ_CMD || rc == 0))\n        mfc->tag_completed |= (1u << tag);\n    spu->mfc_tag_pending = ~mfc->tag_completed;   /* #311 mirror */\n\n    return rc;"),
]
def fn2(t):
    if "#311 mirror" in t: print("spu_dma.h: already patched"); return None
    for a, b in SUBS:
        if t.count(a) != 1: print("spu_dma.h: pattern count %d for: %s" % (t.count(a), a[:50])); sys.exit(1)
        t = t.replace(a, b)
    print("spu_dma.h: 4 mirror sites added"); return t
rw(dma, fn2)
