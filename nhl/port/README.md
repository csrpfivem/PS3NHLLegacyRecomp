# nhl/port

The CMake project that builds `NHLLegacy.exe` from the lifted game code. It started as
ps3recomp's project template.

| File | Purpose |
|---|---|
| `CMakeLists.txt` | Builds the ps3recomp runtime, the lifted PPU/SPU code and the port glue. It also sets up the PGO, ISA and SPU-dual options. |
| `boot_main.cpp` | Entry point and boot harness: runtime boot, debug console, profiling |
| `nhl_config.cpp` | Standalone settings: turns `nhl_settings.ini` into the switches and paths the game needs |
| `nhl_hle.cpp` | NHL-specific HLE hooks called from the lifted code (heap slack, RefPack, timing, ...) |
| `nhl_vp6_bridge.cpp` | Decodes the game's VP6 movies on the host through FFmpeg |
| `nhl_edge_native.c` | Native AVX2 replacement for the SPU Edge geometry job ("Fast player geometry", #292) |
| `nhl_edge.c` | Edge job research hooks and logging (#268) |
| `nhl_spu_isa.c` | Picks AVX-512 or AVX2 SPU code at startup |
| `stubs.cpp`, `config.toml` | Template-level game hooks and lifter settings |
| `nhl_hot.order` | Optional hot-function link order (`NHL_ORDER_FILE`) |
| `handwritten/ppu_recomp_preamble.h` | The hand-written top of the lifted `ppu_recomp.h` |

`recompiled/`, `spu/` and `spu_avx2/` are generated from your own EBOOT and are not part
of the repository. See [BUILDING.md](../../BUILDING.md).
