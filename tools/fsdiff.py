"""Diff the file-open sequences of an RPCS3 log and a native run's err.log.

usage: python -I fsdiff.py <RPCS3.log> <native err.log> [anchor]

Both are reduced to an ordered list of opened game paths (cache/hdd0/disc
prefixes stripped, so the same asset matches whichever device served it).
`anchor` (default: the lineup subscreen) marks the match start in both; the
report lists what RPCS3 opened after it that the native run never opened, in
RPCS3's order -- the first such entries are where the native run stops.
"""
import re, sys

PREFIX = re.compile(r"^/dev_[a-z0-9_]+/(?:cache/BLUS31540/|game/BLUS31540/USRDIR/|PS3_GAME/USRDIR/|PSN_GAME/USRDIR/)?", re.I)
PATH = re.compile(r"(/dev_(?:hdd0|hdd1|bdvd|usb\d+|flash)\S*?)(?=[\"'\s,)]|$)")


def norm(p):
    return PREFIX.sub("", p).lower()


def rpcs3_opens(path):
    out = []
    # Successful opens only: "sys_fs_open(): fd=3, Regular file, '/dev_bdvd/...', Mode: ..."
    for line in open(path, encoding="utf-8", errors="ignore"):
        m = re.search(r"sys_fs_open\(\): fd=\d+, [^']*'(/dev_[^']+)'", line)
        if m:
            out.append(norm(m.group(1)))
    return out


def native_opens(path):
    out = []
    for line in open(path, encoding="utf-8", errors="ignore"):
        m = re.match(r"\[fs\] open '([^']+)' -> fd", line)
        if m:
            out.append(norm(m.group(1)))
    return out


def after(seq, anchor):
    for i, p in enumerate(seq):
        if anchor in p:
            return seq[i:]
    return []


def main():
    r, n = rpcs3_opens(sys.argv[1]), native_opens(sys.argv[2])
    anchor = sys.argv[3].lower() if len(sys.argv) > 3 else "rendering/icesurface/centerlogo"
    print(f"RPCS3 opens: {len(r)}   native opens: {len(n)}   anchor: {anchor!r}")
    ra, na = after(r, anchor), after(n, anchor)
    print(f"after anchor -- RPCS3: {len(ra)}   native: {len(na)}")
    if not ra:
        print("anchor not found in the RPCS3 log; last 40 RPCS3 opens:")
        for p in r[-40:]:
            print("   ", p)
        return
    nset = set(n)
    missing, seen = [], set()
    for p in ra:
        if p not in nset and p not in seen:
            seen.add(p)
            missing.append(p)
    print(f"\nopened by RPCS3 after the anchor, NEVER by native ({len(missing)}):")
    for p in missing[:80]:
        print("   ", p)
    print("\nlast 25 native opens after the anchor:")
    for p in na[-25:]:
        print("   ", p)


if __name__ == "__main__":
    main()
