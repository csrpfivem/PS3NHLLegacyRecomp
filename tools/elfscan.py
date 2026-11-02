"""Recon scanner for a decrypted PS3 PPU ELF.

Usage: python -I elfscan.py <eboot.elf> <rpcs3.exe> <out_dir>

Writes:
  summary.txt     - headers, segments, sections, SPU ELF list
  imports.txt     - imported modules + functions (FNIDs resolved via names harvested from rpcs3.exe)
  strings_*.txt   - interesting string buckets
  spu/*.elf       - embedded SPU ELF images
"""
import hashlib
import os
import re
import struct
import sys
from collections import defaultdict

FNID_SUFFIX = bytes.fromhex("6759659904250490566427499489741A")


def fnid(name: str) -> int:
    return struct.unpack("<I", hashlib.sha1(name.encode() + FNID_SUFFIX).digest()[:4])[0]


def harvest_names(exe_path):
    data = open(exe_path, "rb").read()
    names = set(m.group().decode() for m in re.finditer(rb"[A-Za-z_][A-Za-z0-9_]{3,80}", data))
    return {fnid(n): n for n in names}


class Elf:
    def __init__(self, path):
        self.d = open(path, "rb").read()
        d = self.d
        assert d[:4] == b"\x7fELF" and d[4] == 2 and d[5] == 2, "expected ELF64 big-endian"
        (self.type, self.machine, _, self.entry, self.phoff, self.shoff, _, _, phentsz, self.phnum,
         shentsz, self.shnum, self.shstrndx) = struct.unpack_from(">HHIQQQIHHHHHH", d, 16)
        self.ph = [struct.unpack_from(">IIQQQQQQ", d, self.phoff + i * phentsz) for i in range(self.phnum)]
        self.sh = []
        if self.shoff and self.shnum:
            raw = [struct.unpack_from(">IIQQQQIIQQ", d, self.shoff + i * shentsz) for i in range(self.shnum)]
            strtab = raw[self.shstrndx]
            for s in raw:
                n = d[strtab[4] + s[0]:].split(b"\0", 1)[0].decode(errors="replace")
                self.sh.append((n,) + s)

    def va2off(self, va):
        for p_type, _, off, vaddr, _, filesz, memsz, _ in self.ph:
            if p_type == 1 and vaddr <= va < vaddr + filesz:
                return off + (va - vaddr)
        return None

    def u32(self, va):
        o = self.va2off(va)
        return None if o is None else struct.unpack_from(">I", self.d, o)[0]

    def cstr(self, va):
        o = self.va2off(va)
        return None if o is None else self.d[o:self.d.index(b"\0", o)].decode(errors="replace")


def scan_imports(elf, names):
    prx = [p for p in elf.ph if p[0] == 0x60000002]
    if not prx:
        return None, []
    off = prx[0][2]
    size, magic, version, _, libent_start, libent_end, libstub_start, libstub_end, ver = struct.unpack_from(
        ">IIIIIIIIH", elf.d, off)
    mods = []
    va = libstub_start
    while va < libstub_end:
        o = elf.va2off(va)
        # sys_libstub: s_size u8, pad u8, version u16, attr u16, num_func u16, num_var u16, num_tls u16,
        # hashes/pad 4 bytes, then name, func_nid, func_stub, var_nid, var_stub pointers
        s_size = elf.d[o]
        nfunc, nvar = struct.unpack_from(">HH", elf.d, o + 6)
        name_va, fnid_va = struct.unpack_from(">II", elf.d, o + 0x10)
        name = elf.cstr(name_va)
        funcs = []
        for i in range(nfunc):
            f = elf.u32(fnid_va + 4 * i)
            funcs.append((f, names.get(f, "?")))
        mods.append((name, funcs, nvar))
        va += s_size
    return dict(magic=hex(magic), version=version, sdk=hex(ver)), mods


def find_spu_elfs(d):
    out = []
    for m in re.finditer(rb"\x7fELF\x01\x02\x01", d):
        o = m.start()
        e_machine = struct.unpack_from(">H", d, o + 18)[0]
        if e_machine != 0x17:
            continue
        phoff, shoff = struct.unpack_from(">II", d, o + 28)
        phentsz, phnum, shentsz, shnum = struct.unpack_from(">HHHH", d, o + 42)
        end = 0
        for i in range(phnum):
            _, poff, _, _, filesz, _, _, _ = struct.unpack_from(">IIIIIIII", d, o + phoff + i * phentsz)
            end = max(end, poff + filesz)
        if shoff and shnum:
            end = max(end, shoff + shnum * shentsz)
        out.append((o, end))
    return out


STRING_BUCKETS = {
    "paths": rb"[A-Za-z]:[\\/][\x20-\x7e]{4,200}|/(?:home|usr|dev_\w+|app_home)/[\x20-\x7e]{3,200}|[\x20-\x7e]*\.(?:cpp|h|hpp|c|pdb|obj)\b",
    "engine": rb"[\x20-\x7e]*(?:rw::|RenderWare|Ignite|EAAudio|EA::|eastl|EASTL|Gamebryo|Havok|Scaleform|GFx|Bink|DirtySDK|Blaze|Fifa|FIFA|NHL1\d|ANT|Dime|DIME|Lua|lua_|SNC|GCC|ProDG|buildbot|Build:|version)[\x20-\x7e]*",
    "shaders": rb"[\x20-\x7e]*(?:\.vpo|\.fpo|\.cgb|cgc|sce_cg|\.fx\b|\.shader|technique)[\x20-\x7e]*",
}


def main():
    elf_path, exe_path, out = sys.argv[1:4]
    os.makedirs(os.path.join(out, "spu"), exist_ok=True)
    elf = Elf(elf_path)
    names = harvest_names(exe_path)
    with open(os.path.join(out, "summary.txt"), "w") as f:
        f.write(f"type={elf.type:#x} machine={elf.machine:#x} entry={elf.entry:#x}\n\nPROGRAM HEADERS\n")
        for p in elf.ph:
            f.write("  type=%#010x flags=%#x off=%#x va=%#x filesz=%#x memsz=%#x\n" % (p[0], p[1], p[2], p[3], p[5], p[6]))
        f.write(f"\nSECTIONS ({len(elf.sh)})\n")
        for s in elf.sh:
            f.write("  %-28s type=%#x addr=%#x size=%#x\n" % (s[0], s[1], s[4], s[6]))
        hdr, mods = scan_imports(elf, names)
        f.write(f"\nPRX PARAM {hdr}\nIMPORTED MODULES ({len(mods)}), total funcs {sum(len(m[1]) for m in mods)}\n")
        for name, funcs, nvar in mods:
            unk = sum(1 for _, n in funcs if n == "?")
            f.write("  %-24s funcs=%4d vars=%d unresolved=%d\n" % (name, len(funcs), nvar, unk))
        spus = find_spu_elfs(elf.d)
        f.write(f"\nEMBEDDED SPU ELFS ({len(spus)}), total {sum(e for _, e in spus)} bytes\n")
        for i, (o, size) in enumerate(spus):
            f.write("  #%03d off=%#x size=%#x\n" % (i, o, size))
            open(os.path.join(out, "spu", "spu_%03d_%x.elf" % (i, o)), "wb").write(elf.d[o:o + size])
    with open(os.path.join(out, "imports.txt"), "w") as f:
        for name, funcs, _ in mods:
            f.write(f"[{name}]\n")
            for fid, n in funcs:
                f.write(f"  {fid:08x} {n}\n")
    for bucket, pat in STRING_BUCKETS.items():
        hits = defaultdict(int)
        for m in re.finditer(pat, elf.d):
            hits[m.group().decode(errors="replace").strip()] += 1
        with open(os.path.join(out, f"strings_{bucket}.txt"), "w") as f:
            for s in sorted(hits):
                f.write(s + "\n")
    print(open(os.path.join(out, "summary.txt")).read())


if __name__ == "__main__":
    main()
