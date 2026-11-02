"""Merge + call summaries on the hot Edge programs only (LOCAL_PATCHES #277, parked).

Lifts everything into nhl/spu_merge_tmp with NHL_SPU_MERGE_ONLY, checks the other images are
byte-identical to nhl/port/spu, and copies just the two hot images in with a fresh timestamp,
so a build recompiles only those. Then run nhl_spu_dual.py and nhl_hooks.py, and build.

Result so far (speed5): a tie with the chunked code, so it is not the default. To undo, copy the
two images back from nhl/spu_pre275 and touch them, or ninja keeps the merged objects.

    python -I nhl_spu_merge_hot.py
"""
import filecmp
import os
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))   # python -I leaves it out
import nhl_spu_lift as L

HOT = ("nhl_prog_01DBF980", "nhl_prog_01EA4600")


def main():
    os.environ.pop("NHL_SPU_MERGE", None)
    os.environ.pop("NHL_SPU_SUMMARIES", None)
    os.environ["NHL_SPU_MERGE_ONLY"] = "01DBF980,01EA4600"
    port = L.SPU
    out = os.path.join(L.W, "nhl", "spu_merge_tmp")
    os.makedirs(out, exist_ok=True)
    L.SPU = out
    L.main()
    bad = []
    for d in sorted(os.listdir(out)):
        a, b = os.path.join(out, d, "spu_recomp.c"), os.path.join(port, d, "spu_recomp.c")
        if os.path.isfile(a) and not filecmp.cmp(a, b, shallow=False):
            bad.append(d)
    if set(bad) - set(HOT):
        sys.exit("unexpected differences, nothing copied: %s" % bad)
    for d in HOT:
        for f in ("spu_recomp.c", "spu_recomp.h"):
            shutil.copy2(os.path.join(out, d, f), os.path.join(port, d, f))
            os.utime(os.path.join(port, d, f))
        print("copied", d)
    shutil.rmtree(out)


if __name__ == "__main__":
    main()
