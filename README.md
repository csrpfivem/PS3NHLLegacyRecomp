# NHL Legacy Native

A native Windows port of **NHL Legacy Edition** (PS3, BLUS31540, update 1.01), built by
*static recompilation*: the game's PowerPC (PPU) and SPU code is translated ahead of time
into C/C++ and compiled into a normal x86-64 executable. It is not an emulator. The game's
own code runs natively, with Sony's system libraries replaced by host implementations
(HLE) and the RSX graphics chip replaced by a Direct3D 12 renderer.

> **This repository contains no game code or game data.** It contains the port's hand-written
> source (runtime, renderer, HLE, launcher, tools), the build scripts, and the research
> notes. You need **your own legal copy of the game** (disc ISO or extracted folder, plus
> the official 1.01 update) to produce a working build. See [BUILDING.md](BUILDING.md).

Built on [ps3recomp](https://github.com/sp00nznet/ps3recomp) by sp00nznet (Ned Heller),
with many NHL-specific changes to its runtime, lifters and renderer (logged in
[docs/notes/LOCAL_PATCHES.md](docs/notes/LOCAL_PATCHES.md)).

As a note this port was developed for about two weeks (Oct 17 - Nov 1 2026) using Claude. It 
was built on sp00nznet's ps3recomp toolkit with about 300 logged fixes and changes, many of them 
from bugs I (HeckYouMan) found while play-testing

Join our discord! https://discord.gg/UU9bpt4Qmx

---

## Features

### Playing
- **Full matches, menus, saves and rosters**, running natively on Windows 10/11 x64.
  Saves load on both PC and Steam Deck.
- **Steam Deck support** (via Proton). On a Steam Deck OLED a full match runs at about
  **88% of full speed** with the Low-end preset.
- **Movies**: the game's VP6 videos are decoded on the host through an FFmpeg bridge.
- **Mods and roster packs**: drop a mod's `.zip`, folder or file on the launcher.
  Mods are applied in priority order, so the game's files are never modified.
- **Your own music** (v1.0.1): playlists in the `music` folder appear in the game's
  Custom Audio screen (menu music, goal songs, goal horns...) through a native
  `cellSearch` / `cellMusicDecode2`, with an adjustable volume.
- **EA Trax** (v1.0.1): up to 60 extra songs in Customize EA Sports Trax, with cover
  art and even loudness. The launcher rebuilds the music bank when you press PLAY.
- **Audio packs** (v1.0.1): share playlists plus your Custom Audio choices as one
  `.nhlaudio` file; dropping one on the launcher installs it.

### Graphics options (beyond the PS3)
| Option | Choices |
|---|---|
| Render resolution | Native (your screen), 720p (PS3), 1.5x, and lower options for weak GPUs |
| Anti-aliasing (MSAA) | Off, 2x (PS3), 4x, 8x |
| Texture filtering | Off (PS3), 2x to 16x anisotropic |
| Shadow quality | PS3, High (2x), Ultra (4x) shadow maps |
| Lighting and effects | PS3 (half/quarter-res), High, Full-resolution glow/DOF/player lighting |
| Ice light on players | On (as the PS3), or off for flatter jerseys |
| Display | Fullscreen/window (F11), variable refresh rate (FreeSync/G-SYNC), 60 fps menus and cutscenes |

### Performance options
- **Presets**: *Default quality* and *Low-end PC* (30 fps cap, 720p, light settings).
- **Fast player geometry**: a native replacement for the game's SPU "Edge" geometry jobs,
  for 4-core and older CPUs (+14-18% in matches on the Deck).
- **Frame cap at 30** and **No crowd**, both for slow CPUs.

### Game options
- **Skip the intro**: goes straight to the main menu (from Puck Peak's RPCS3 patch).

### Launcher (`NHLLauncher.exe`)
- **Installer**: takes your disc (`.iso` or extracted folder) and the 1.01 update
  (`.pkg` or folder), then decrypts the EBOOT, extracts the files and builds the
  first-boot cache. No RPCS3 is needed.
- **Settings**: every option above. Settings are saved to `nhl_settings.ini`.
- **Mods**: install, enable or disable, and reorder mods.
- **Report a problem**: builds one zip with your note, your PC's specs (CPU, GPU and
  Windows version), the settings, the last two game logs and any F9 frame captures.
  You choose where to send it.

### Under the hood
- **Whole-program static recompilation** of the PPU (about 106,000 functions) and the SPU
  Job Manager kernels and programs (419 SPU images).
- **Direct3D 12 RSX renderer**: it translates the game's vertex/fragment programs to HLSL,
  emulates the RSX surfaces, and adds native-resolution rendering and MSAA.
- **CPU performance work**:
  - profile-guided optimisation (PGO; the profile ships in `nhl/pgo/`);
  - x86-64-v3/FMA code generation;
  - dead-flag elimination on the lifted PPU code;
  - SSE/AVX VMX helpers;
  - AVX-512 SPU code, plus an AVX2 copy that is picked at startup, so one exe runs on
    both kinds of CPU (including the Steam Deck);
  - a native SPU Edge geometry path;
  - native EALayer3 audio decoding stages and a native graphics command-table scan
    in place of the lifted SPU loops (v1.0.1).
- **Rendering fixes found by diffing against RPCS3's shader logs**, for example:
  - the goal light (the vertex-program `RSQ(0)` must give infinity);
  - lineup team-colour bars and dark text panels (Apt colour-transform add term).

---

## Requirements

**To play** (once built):
- Windows 10/11 x64, or Steam Deck / Linux via Proton.
- A CPU with AVX2 + FMA (x86-64-v3: Intel Haswell or AMD Zen and newer).
- A GPU with Direct3D 12.
- Your own copy of NHL Legacy Edition (BLUS31540) and its official 1.01 update.
- `ffmpeg.exe` for the movies. The installer puts its path in `nhl_settings.ini`;
  the default is `ffmpeg\ffmpeg.exe`.

**To build**: see [BUILDING.md](BUILDING.md).

---

## Repository layout

```
ps3recomp/ps3recomp-master/   the ps3recomp toolkit (runtime, HLE libs, D3D12 RSX backend, lifters),
                              with this port's changes
nhl/port/                     the NHL project: CMake, boot/HLE glue, VP6 bridge, native Edge, SPU ISA pick
nhl/port/handwritten/         hand-written preamble of the lifted ppu_recomp.h (restored after a relift)
nhl/launcher/                 NHLLauncher: installer, settings, mods, bug report (Dear ImGui + D3D11)
nhl/pgo/release296.profdata   PGO profile used for release builds
tools/                        port scripts: post-lift hooks, flag pass, SPU lift, run/profiling helpers
edge/                         research scripts and a C reference for the SPU Edge geometry jobs
docs/notes/                   the full engineering log (see below)
```

Not included (you generate these yourself from your own copy of the game):
- `nhl/port/recompiled/`: the lifted PPU code;
- `nhl/port/spu/` and `nhl/port/spu_avx2/`: the lifted SPU code;
- any EBOOT, disc or update files.

---

## Research notes

The port was built step by step, and every finding, test run and fix was written down.

| File | What it is |
|---|---|
| [docs/notes/NEXT_STEPS.md](docs/notes/NEXT_STEPS.md) | The working log: every investigation, test run and result, in order |
| [docs/notes/LOCAL_PATCHES.md](docs/notes/LOCAL_PATCHES.md) | Every change made to ps3recomp and the lifted code, numbered (#1-#303), with the reason for each |
| [docs/notes/FEASIBILITY.md](docs/notes/FEASIBILITY.md) | The first study of whether a static recomp of this game was possible |
| [docs/notes/SPU_RECON.md](docs/notes/SPU_RECON.md) | How the game's SPU Job Manager, stage-0 loader and programs are laid out |
| [docs/notes/RESOLUTION_SCALING_PLAN.md](docs/notes/RESOLUTION_SCALING_PLAN.md) | The design of native-resolution rendering |

The notes are a raw engineering log, not polished documentation. Search them for a
`#number` from LOCAL_PATCHES to find the full story behind a change.

---

## Legal

This is an unofficial, non-commercial fan project. It is not affiliated with or endorsed
by Electronic Arts, the NHL, or Sony Interactive Entertainment. NHL Legacy Edition and all
related marks belong to their owners. No game code, assets or keys specific to the game
are included. You must own the game to use this.

`nhl/launcher/self_decrypt.cpp`, `ps3recomp/ps3recomp-master/libs/filesystem/edat.c` and
`ps3recomp/ps3recomp-master/runtime/ppu/sdata_decrypt.h` contain the generic, publicly
documented PS3 SELF/EDAT platform keys, as used by RPCS3 and other open-source PS3
tools. They are needed to decrypt your own copy at install time.

## License

The code written for this port is released under the [MIT License](LICENSE).
Third-party components keep their own licenses:
- **ps3recomp**: MIT, © Ned Heller / sp00nznet (`ps3recomp/ps3recomp-master/LICENSE`);
- **Dear ImGui**: MIT;
- **miniz**: MIT;
- **Archivo** and **Big Shoulders Display** fonts: SIL Open Font License 1.1 (in `nhl/launcher/third_party/`);
- the libraries bundled inside ps3recomp's `third_party/`: see their headers.

## Credits

- **sp00nznet (Ned Heller)**: ps3recomp, the toolkit this port is built on.
- **Puck Peak**: the skip-intro patch (MIT), ported from RPCS3 patch form.
- **RPCS3 team**: the reference emulator and documentation, used throughout to compare behaviour.