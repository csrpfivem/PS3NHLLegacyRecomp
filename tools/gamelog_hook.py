"""Insert/remove the [GAMELOG] diagnostic in func_004EEED4 (ppu_recomp_043.cpp).

func_004EEED4 is one of the game's logging channels: it vsnprintf's the message
(func_00BC79E4) into a stack buffer at r29 (= r1+0x70), then hands it to the
channel's sink, which does nothing in the retail build. The hook prints the
formatted buffer when NHL_GAMELOG=1. Latin-1 round trip keeps the file's bytes.
usage: python -I gamelog_hook.py <ppu_recomp_043.cpp> [--remove]
"""
import sys
path = sys.argv[1]
src = open(path, encoding="latin-1").read()
anchor = "ctx->lr = 0x004EEF50; func_00BC79E4(ctx); DRAIN_TRAMPOLINE(ctx);"
hook = ("\n        /* [DIAG GAMELOG] */ { static int on = -1; if (on < 0) on = getenv(\"NHL_GAMELOG\") ? 1 : 0;"
        " if (on) { char b[0x301]; int i = 0; for (; i < 0x300; i++) { b[i] = (char)vm_read8(ctx->gpr[29] + i); if (!b[i]) break; }"
        " b[i < 0x300 ? i : 0x300] = 0; fprintf(stderr, \"[GAMELOG] %s\\n\", b); } }")
if "--remove" in sys.argv:
    if hook not in src: sys.exit("hook not present")
    src = src.replace(anchor + hook, anchor)
else:
    if src.count(anchor) != 1: sys.exit(f"anchor count {src.count(anchor)}")
    if "[DIAG GAMELOG]" in src: sys.exit("already hooked")
    src = src.replace(anchor, anchor + hook)
    if "#include <stdlib.h>" not in src:
        src = src.replace("#include <stdio.h>", "#include <stdio.h>\n#include <stdlib.h>", 1)
open(path, "w", encoding="latin-1", newline="").write(src)
print("ok")
