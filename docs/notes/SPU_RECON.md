# NHL Legacy SPU code layout (recon, 2026-10-18)

Sources:
- runs `perf1`, `perf2` and `recon1`, with `SPU_PCPROF` (LOCAL_PATCHES #37) and `SPU_CODEGET_LOG` (#38);
- the static scan `nhl/runs/recon1.jobs.txt`.

Every SPU instruction the game runs lives in the EBOOT's text segment between 0x01DCD180 and 0x01E86F00, about 730 KB. The game never builds or downloads SPU code at runtime, so all of it can be lifted offline from `bin/EBOOT_v101.elf` (EBOOT VA = file offset + 0x10000).

## Three tiers

1. **Stage-0 loader / Job Manager kernel.** This is the only real SPU ELF: VA 0x1E5DE80, 5,140 bytes, entry 0x90.
   - It has 4 PT_LOADs: LS 0x80 (0x1B0, code), 0x3EDC0 (0x20, data), 0x3EF80 (0xE80, code) and 0x3FF80 (0x80, code).
   - The main loop is at 0x3F01C. It calls 0x3F450, which polls the job queue with GETLLAR/PUTLLC atomic loops (0x3F2C0-0x3F448), so this is mostly the IDLE spin.
   - 0x3FF80 is the DMA helper that loads programs (pc 0x3FF8C in the trace).
   - It accounts for about 37% of all interpreted SPU time at the title screen.
2. **Programs** (header type 0; 18 found). They are loaded whole at **fixed LS 0x80** by stage-0 (header flags low bits = 0x80).
   - Examples: 0x01DCD180 (0x6470 bytes), 0x01E26780 (0x3280), and 0x01E6E580 (0x11FC0, the largest, own flags 0x00000080).
   - Programs 0x01DCD180 and 0x01E26780 contain the job runner: it streams jobs in at pc 0x031A4.
3. **Jobs** (header type 1; 40 found, about 0x60380 = 394 KB). They are loaded by the program's job runner at **variable LS addresses**; the same body lands at up to 5 different addresses (e.g. 0x01DF7500 at 0x3800, 0x8800, 0x9800, 0xA580 and 0xBA80).
   - The jobs are **position-independent**, linked at 0. Their entry code does `ila $r16, X; brsl $r6, X; sf $r79, $r16, $r6` to get the load base in $r79, then forms every internal pointer as base + offset.
   - Jobs call back into the program through function pointers (`bisl $r0, $r8`).

## Body header (offsets from the body start)

```
+0x00  4 x `ila $r2, imm`   signature
+0x10  entry offset          +0x14  code size        +0x18  stack/bss size
+0x1C  flags (0x80000000 = job, base 0 / 0x80000080 = program at LS 0x80)
+0x20  size                  +0x24  ?                +0x28  type (1 = job, 0 = program)
+0x2C  magic 0x55011055
```

## What this means for lifting

- Stage-0 and the programs have fixed addresses, so the existing lifter (`tools/spu_lifter.py --base`) handles them. They register as images or overlays keyed by source EA (`spu_overlay_register_region`).
- Jobs need a **relocatable lift**: lift each one at base 0, and at runtime run it with `code_base = actual load LSA`.
  - Lifted `brsl`/`bisl` must write `code_base + offset`, not a constant.
  - Dispatch has to translate `pc - code_base` within the job's span.
  - Data accesses need no change: they use runtime register values.
  - This is an SDK lifter/runtime extension. The alternative, one lift per (job, base) pair, does not scale: bases depend on what else is resident.
- The pure interpreter never hands control to lifted code (`image_id < 0`). Stage-0 must therefore be lifted first, or nothing downstream gets used.
