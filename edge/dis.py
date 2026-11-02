# Disassemble an SPU ELF's PT_LOAD segments at their vaddr. usage: dis.py <elf> > out
import os, sys, struct
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'ps3recomp', 'ps3recomp-master', 'tools'))
from spu_disasm import disassemble_spu
b = open(sys.argv[1], 'rb').read()
phoff, = struct.unpack_from('>I', b, 0x1C); phnum, = struct.unpack_from('>H', b, 0x2C)
for i in range(phnum):
    t, off, va, pa, fsz, msz, fl, al = struct.unpack_from('>8I', b, phoff + i * 32)
    if t != 1 or not (fl & 1): continue   # PT_LOAD, executable
    print(f'; segment off {off:X} vaddr {va:05X} size {fsz:X}')
    for ins in disassemble_spu(b[off:off + fsz], va): print(ins)
