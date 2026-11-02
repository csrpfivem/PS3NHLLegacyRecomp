# NHL Legacy Edition: native PC feasibility (Phase 1)

Date: 2026-10-17. Target: BLUS31540, update 01.01 (the EBOOT RPCS3 actually boots).

## What the game is made of

| Area | Finding | Porting impact |
|---|---|---|
| PPU code | Decrypted EBOOT (`bin/EBOOT_v101.elf`) has ~27.6 MB of `.text`, about 7M PowerPC instructions. It's a single static ELF and the game ships no PRX modules of its own. | This is huge. Every function has to be lifted and then debugged. |
| System API surface | 30 imported modules and 371 functions, all resolved (`recon/imports.txt`): cellGcmSys (33), cellSpurs (13), cellAudio, cellAdec, libvdec, sys_fs, sys_io (pad), sysPrxForUser, sceNp*/Commerce/HTTP/SSL (online). | This part is small and conventional. Most of it is already HLE'd in ps3recomp. |
| SPU | About 425 SPU job programs, embedded in the EBOOT without ELF headers (likely jobbin2 format). They run on EA's own "Job Manager" (6 raw SPU thread groups), with SPURS only lightly used. The jobs cover animation (ANT/HumanIK, ~200 kernels), physics/collision, AI, audio DSP (EAAudioCore, ~50), render culling/skinning, Edge geometry, and zlib/LZMA. | This is the hardest part. Core gameplay depends on SPU code, and every job must be found, lifted and scheduled correctly. In the RPCS3 log each SPU was busy for 69 of 96 seconds. |
| Graphics | Raw libgcm command buffers, Cg shaders precompiled for RSX (`.vpo`/`.fpo` inside `.fx`), swizzled/DXT textures, Edge geometry. | An RSX→D3D12/Vulkan translator is required. ps3recomp has one, but it hasn't been proven on a large 3D title. |
| Engine | EA Sports in-house stack: RenderWare-derived `rw::core`, ANT animation, EAAudioCore 7, Lynx, BlazeSDK (online, servers now offline), DirtySDK. Built with SN Systems (ProDG). There are no symbols, but C++ type names and source paths survive. | Function names have to be recovered by hand. Source paths and type names help with orientation. |
| Assets | EA `EB` v3 archives (chunkzip-compressed), `BIG4`/`BIGF` (your mod uses these), `SBKS`/`SBKR` sound banks, `.ast` streams, `MBX0` music box, NPDRM `secure0.big.sdat`. | A recompiled binary reads these unchanged, so no asset conversion is needed. |

## Ecosystem (as of October 2026)

- **[ps3recomp](https://github.com/sp00nznet/ps3recomp)** (MIT, v0.15.0, Oct 2026) is the only PS3 static-recomp toolkit. It has a PPU lifter, an SPU lifter, about 93 HLE modules and a D3D12 RSX backend. Its best results are two small titles: The Simpsons Arcade Game is playable, and Tornado Outbreak has its first level running at 10–16 fps. Several others reach menus or loading screens. No large 3D sports game has been attempted. [Coverage](https://www.generationamiga.com/2026/06/15/ps3recomp-the-new-tool-bringing-native-playstation-3-ports-closer-to-pc/) calls it a developer toolkit, not a launcher.
- The **Xbox 360 tooling** (XenonRecomp, ReXGlue) is more mature, with about 8 playable PC ports, including the Unleashed Recompiled Sonic port ([list](https://heldgames.com/guides/xbox-360-recompiled-games-list), [ReXGlue](https://heldgames.com/guides/xbox-360-recompilation-rexglue)). NHL Legacy also shipped on 360, and that build has **no SPU code**.
- **Community:** this search found no PC port, decompilation or active NHL Legacy modding project. The only lead was a [2024 forum thread](https://www.tapatalk.com/groups/nhl04rebuilt/viewtopic.php?p=111180) that mentions a Discord.
- No RPCS3 or Xenia compatibility entry for this game turned up in the search.

## Paths

| Path | What you get | Effort | Odds of reaching "play a full game" |
|---|---|---|---|
| **A. Seamless launcher** | You double-click NHL (desktop or Steam shortcut, fullscreen, controller) and never see RPCS3. Tuned config. | Hours | ~100% |
| **B. PS3 static recomp (ps3recomp)** | A real native `.exe` built from your PS3 copy | Many months, open-ended | Low. This would be the largest and most SPU-heavy title anyone has tried. |
| **C. Xbox 360 recomp (ReXGlue/XenonRecomp)** | A native `.exe` from the 360 build (no SPUs, more mature tools) | Months | Low to moderate. You would need to own and dump the 360 version. |

Suggested gate if B is chosen: the first milestone is lifting the EBOOT and booting to the EA logo with GCM init working. If that takes more than a few weeks of iteration, stop.
