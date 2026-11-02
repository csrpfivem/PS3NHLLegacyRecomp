"""Puts the port's hand-written preamble back on a freshly lifted ppu_recomp.h.
The lifter writes ppu_recomp.h as a preamble (types, memory access, VMX helpers) followed by one
`void func_XXXXXXXX(ppu_context* ctx);` declaration per lifted function. The port edited that
preamble by hand (LOCAL_PATCHES #147/#261/#271: memory paths, SSE VMX helpers); a relift drops
those edits. This keeps the fresh declarations and swaps the preamble for
nhl/port/handwritten/ppu_recomp_preamble.h. Idempotent.
    python -I nhl_restore_preamble.py [path/to/ppu_recomp.h]
"""
import os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
W = os.path.dirname(HERE)
PRE = os.path.join(W, "nhl", "port", "handwritten", "ppu_recomp_preamble.h")
HDR = sys.argv[1] if len(sys.argv) > 1 else os.path.join(W, "nhl", "port", "recompiled", "ppu_recomp.h")

pre = open(PRE, encoding="latin-1").read().splitlines()
raw = open(HDR, encoding="latin-1", newline="").read()
eol = "\r\n" if "\r\n" in raw else "\n"   # keep the lifter's line endings
lines = raw.splitlines()
first = next((i for i, l in enumerate(lines) if l.startswith("void func_")), None)
if first is None:
    sys.exit(f"no `void func_` declarations in {HDR}")
open(HDR, "w", encoding="latin-1", newline="").write(eol.join(pre + lines[first:]) + eol)
print(f"{HDR}: preamble {len(pre)} lines + {len(lines) - first} declaration lines")
