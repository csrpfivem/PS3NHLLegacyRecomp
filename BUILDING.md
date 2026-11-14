# Building NHL Legacy Native

This repository has no game code in it. A build has three stages:

1. **Install your copy with the launcher.** This gives you a decrypted EBOOT.
2. **Lift the game.** This turns your EBOOT into C/C++ (the PPU and SPU code).
3. **Compile** the lifted code together with the port.

Stage 1 needs nothing from the game to build. Stages 2 and 3 need your EBOOT.

> **Status:** these steps were tested from a clean copy of this repository. Every hand
> change to lifted code is either:
> - in the lifter templates (`ps3recomp/.../tools/ppu_lifter.py`, `spu_lifter.py`), or
> - re-applied by a script (`tools/nhl_hooks.py`, `tools/nhl_ppu_flags.py`,
>   `tools/nhl_restore_preamble.py`).
>
> The full history of those changes is in `docs/notes/LOCAL_PATCHES.md`.

---

## Prerequisites (Windows x64)

- **Visual Studio 2022 Build Tools**: the "Desktop development with C++" workload, for the
  MSVC headers/libs and the Windows SDK. The compiler itself is clang-cl.
- **LLVM for Windows** (clang-cl + lld-link). Development used LLVM 23.1.3.
  - `tools/devenv.cmd` expects it extracted to
    `toolchain/clang+llvm-23.1.3-x86_64-pc-windows-msvc/` at the repo root.
  - It also expects the VS 2022 Build Tools at their default path.
  - Edit both lines in `devenv.cmd` if your setup differs.
  - The PGO profile needs a clang at least as new as the one that wrote it.
- **CMake** 3.20+ and **Ninja**.
- **Python** 3.10+.
- About 40 GB free disk space. The lifted sources are large, and a full build takes a while.

Every command below runs from the repo root through `tools\devenv.cmd`, which sets up
the MSVC environment and puts the LLVM toolchain on `PATH`. To put the commands in a
`.bat` file, write `call tools\devenv.cmd ...`; without `call`, the batch file stops after
the first one.

---

## 1. Build the launcher and install your copy

```bat
tools\devenv.cmd cmake -S nhl\launcher -B nhl\build_launcher -G Ninja -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_BUILD_TYPE=Release
tools\devenv.cmd cmake --build nhl\build_launcher
```

1. Run `nhl\build_launcher\NHLLauncher.exe` and point it at:
   - your disc (`.iso` or a folder holding `PS3_GAME`);
   - the official 1.01 update (`.pkg` or a folder).
2. When it finishes, the install folder holds `data\eboot.elf` (the decrypted 1.01 EBOOT)
   and the game files laid out for the native exe.
3. Copy `NHLLegacy.exe` (from stage 3) into the install folder, beside `NHLLauncher.exe`.

## 2. Lift the game

First copy your decrypted EBOOT (`<install>\data\eboot.elf`) to `bin\EBOOT_v101.elf` at
the repo root. Every lift step reads it from there (`bin/` is git-ignored).

**PPU.** Use the ps3recomp loader and lifter with exactly these options. `--code-end` and
`--chunks 256` matter: without them the lifter promotes data past the end of the code to
functions and writes a different split.

```bat
python ps3recomp\ps3recomp-master\tools\ppu_loader.py bin\EBOOT_v101.elf -o nhl\out
python ps3recomp\ps3recomp-master\tools\ppu_lifter.py bin\EBOOT_v101.elf --functions nhl\out\EBOOT_v101.functions.json --hle-stubs nhl\out\EBOOT_v101.imports.json --code-end 0x1a62e4c --chunks 256 --output nhl\port\recompiled
```

Then run the post-lift steps and the SPU lift, in this order. `nhl_hooks.py` patches the
SPU code as well, so it has to come after the SPU lift.

```bat
python -I tools\nhl_restore_preamble.py
python -I tools\nhl_ppu_flags.py "nhl\port\recompiled\ppu_recomp_*.cpp"
python -I tools\nhl_spu_lift.py
python -I tools\nhl_hooks.py
python -I tools\nhl_hooks.py --check
```

The steps do this:
- `nhl_restore_preamble.py` puts the hand-tuned preamble back on `ppu_recomp.h`, from
  `nhl/port/handwritten/`.
- `nhl_ppu_flags.py` removes dead CR/XER flag writes (LOCAL_PATCHES #272). Keep the
  pattern in quotes: the script expands it itself, and 257 file names are too long for
  one Windows command line.
- `nhl_spu_lift.py` lifts the Job Manager stage-0 kernel, the 56 SPU programs and the
  360 jobs into `nhl/port/spu/`. It then runs `nhl_spu_dual.py` to write the AVX2 twin
  into `nhl/port/spu_avx2/`. This takes a few minutes. `docs/notes/SPU_RECON.md` explains
  the layout.
- `nhl_hooks.py` re-inserts the port's hooks into the lifted PPU and SPU code: VP6 bridge,
  heap slack, RefPack, timing steps, native Edge, no-crowd, save-back and skip-intro.
  `--check` must report every hook as present.

A clean run of these steps on the 1.01 EBOOT matches the release tree byte for byte:
- all SPU code is identical;
- 249 of the 258 PPU files are identical;
- the other 9 differ only by old diagnostic probes, which are switched off in the release
  tree and absent from a fresh lift, and by one comment character.

## 3. Compile

```bat
tools\devenv.cmd cmake -S nhl\port -B nhl\build -G Ninja ^
  -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_BUILD_TYPE=Release ^
  "-DPS3RECOMP_DIR=%CD%\ps3recomp\ps3recomp-master" ^
  "-DNHL_PGO_USE=%CD%\nhl\pgo\release296.profdata"
tools\devenv.cmd cmake --build nhl\build --target NHLLegacy
```

Keep the quotes: without them a repo path with spaces breaks the configure step. The build
compiles about 1 GB of generated C/C++ and takes a while; on a 14-thread CPU, plan on
roughly 20-40 minutes.

These are the release settings, and they are also the defaults:
- `NHL_PPU_FMA=ON`, `NHL_PPU_V3=ON`: the lifted PPU code is built for x86-64-v3 (AVX2/FMA).
- `NHL_SPU_ARCH=ON`, `NHL_SPU_AVX512=ON`, `NHL_SPU_DUAL=ON`: AVX-512 SPU code plus an AVX2
  copy, picked at startup.
- `NHL_SPU_FAST_FLOAT=ON`: the AVX2 SPU copy rounds floats like RPCS3 does.
- `NHL_SPU_SCALAR_INT=OFF`.

Optional settings:
- `NHL_PGO_USE`: drop it for a plain build (slower).
- `NHL_PGO_GEN=ON`: builds an instrumented exe, if you want to record your own profile.
  Note that the shipped profile only matches the lift it was recorded on, so a different
  lift gets less out of it.
- `NHL_ORDER_FILE=nhl\port\nhl_hot.order`: an optional hot-function link order (#283).
  Release builds leave it empty.

## Running

The easiest way: put `NHLLegacy.exe` in the launcher's install folder and press **PLAY**.

The install folder also needs, beside the exes (the release zip has both):
- `ffmpeg\ffmpeg.exe` - movies and EA Trax conversion (any recent FFmpeg build with the
  `mp3_mf` encoder and the `ebur128` / `alimiter` filters, e.g. BtbN's win64 LGPL build);
- `tools\ealayer3\ealayer3.exe` + `libmpg123.dll` - Ben Moench's EALayer3 encoder, used
  for the EA Trax songs. Without it the game runs; only the extra Trax songs are skipped.
- `SDL3.dll` (optional, the official x64 build from github.com/libsdl-org/SDL releases;
  the release zip ships 3.4.20). It handles controllers that are neither XInput nor
  DualSense/DS4. Without it those pads are not seen; everything else works.

For development:
- `tools\run_nhl.ps1` boots `nhl\build\NHLLegacy.exe` against a sandboxed VFS under
  `nhl\vfs` / `nhl\hdd0`, with the environment defaults the game needs. Its header
  explains each variable.
- The exe also takes the decrypted ELF path as its first argument.

## Useful tools

| Script | What it does |
|---|---|
| `tools/nhl_hooks.py` | Re-applies the hooks to lifted code (`--check` reports only) |
| `tools/nhl_ppu_flags.py` | Dead flag-write removal for lifted PPU code |
| `tools/nhl_spu_lift.py` / `nhl_spu_dual.py` | SPU lift and AVX2 twin generation |
| `tools/nhl_restore_preamble.py` | Restores the hand-written `ppu_recomp.h` preamble |
| `tools/run_nhl.ps1` | Dev boot with logs and a debug-console dump at the end |
| `tools/prof_hot.ps1`, `prof_agg.ps1`, `nhl_order.py` | Profiling helpers |
| `tools/deck_profmatch.sh` | Records a match profile on a Steam Deck over SSH (fill in your own host) |
| `tools/eb_extract.py`, `vp6carve.py`, `refpack.py` | Readers for the game's archive, movie and compression formats |
