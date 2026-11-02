# Internal resolution scaling (#248) -- plan

**Goal:** render the game at N x 1280x720 (1x, 1.5x, 2x; 2x = 2560x1440, the user's screen) instead of upscaling 720p. The user compared RPCS3 at 200% and says it "looks so much better" than ours.

**Toggle:** launcher setting "Render resolution", written to `[video] render_scale` and read as `NHL_RENDER_SCALE`. Default 1x until it is verified, then Native.
- **Native** (the target default, user 2026-10-28: "pure native resolution ... absolute highest quality"): S = the screen's height / 720, so the game renders exactly at the monitor's resolution and the present is 1:1 with no resampling. 1440p = 2.0, 4K = 3.0, 1080p = 1.5, 1600p = 2.22, Steam Deck 800p = 1.11.
  - S may be fractional. Every scaled size is rounded: half-res chain = round(gw * S); viewports and scissors are computed from the rounded RT size, not from S * guest.
  - Non-16:9 screens (ultrawide, 16:10) render the game's 16:9 at the screen height and keep the black bars. The game's own aspect is fixed.
- **Supersample** (optional, "Ultra"): S = 1.5 x Native, downsampled to the screen. Edge quality beats native, at ~2.25x the GPU cost. 4K on a 1440p screen = S 3.0.
- Plus fixed 1x / 1.5x / 2x for slow GPUs (the Deck may need 1x). FXAA and the sharp upscale (#247) become unneeded at 2x; anisotropic filtering stays.

## Xbox 360 recomp: what its docs actually say (CHECKED 2026-10-28; replaces the guesses below)

Sources: docs/vulkan-enhancements-kickoff-prompt.md and docs/fidelityfx-enablement-notes.md.

- **Resolution:** it renders at NATIVE 720p. `draw_resolution_scale_x/y = 1` in a 1920x1080 window; integer 2x/3x is planned but untested. They worry about the EDRAM scaling cost, the texture cache, resolves and readback at higher scales.
- **Render-target path:** it must use `render_target_path_vulkan=fsi` (per-tile EDRAM); rov/host render targets render the 3D scene black. `readback_resolve=full` is needed (goalie gear).
- **MSAA:** `native_2x_msaa` / the SDK MSAA path exists, "unmeasured".
- **Image enhancements:** only a present-time scaler, AMD FidelityFX `present_effect` = bilinear / cas / fsr / fsr2 / fsr3 with sharpness sliders (`nhl_enhancements.ini`: ffx_effect, ffx_cas_sharpness, ffx_fsr_sharpness). It builds and loads, but working output is "pending" visual confirmation. Same idea as our #247 upscale + sharpen.
- **Not mentioned:** anisotropic filtering, post effects.
- **Textures:** HD texture replacement via loose .dds -> .rx2 files (UnionDevice / LooseTreeDevice).
- **Conclusion:** the Xbox recomp does NOT render at a higher resolution yet. If it and Xenia look better, it is mostly the 360 BUILD of the game itself and its hardware MSAA (EDRAM MSAA is cheap on 360; the PS3 build asks for 4x MSAA that this port drops), plus any Xenia resolution scale the user set when emulating. Possible build differences (the PS3's half-res lighting pre-pass, texture sizes) are not compared yet.
- **What would close the gap here:** (1) real 4x MSAA (step 8) -- the closest match to what the 360 shows; (2) native resolution (steps 1-7), which goes beyond what the 360 recomp ships today.

## Earlier guesses (superseded by the checked section above)

- The 360 recomp (puckhead73/nhl-legacy-recomp) and the Xenia emulator share Xenia's GPU backend. It emulates the 360's EDRAM render targets and has a built-in resolution scale ("draw resolution scale", host render targets). Their docs mention its rtv/rov / kHostRenderTargets paths. So they get resolution scaling for free.
- The 360 game renders its scene with MSAA in EDRAM, and Xenia keeps that MSAA on the host. This port renders the PS3 scene single-sampled, although the PS3 build also asks for 4x MSAA (0x02980000, pitch 10240) plus alpha-to-coverage, so our edges are stair-stepped and dotted.
- Possibly also asset/post differences between the two builds (PS3 half-res lighting pre-pass 640x360, Cell-side effects). Not checked.
- => To match or beat it: (1) native-resolution rendering (this plan), and (2) real MSAA for the scene RT and its depth (step 8 below), so alpha-to-coverage edges are smooth too.

## Step 8: real MSAA for the scene -- DONE FIRST as #249 (2026-10-28; see LOCAL_PATCHES / NEXT_STEPS). The guest asks 2x (AA mode 3), not 4x. Scaling must give OffRT.ms the scaled size too.

- The scene RT 0x02980000 and depth_buffer2 become 4x MSAA D3D12 resources; PSOs drawing into them get SampleDesc.Count 4 (part of the PSO key).
- The game's resolve (the #85 blit into 0x047D0000) becomes ResolveSubresource.
- Depth reads (#209 snapshot / depthcol) read sample 0 or resolve via a shader.
- Alpha-to-coverage then works as on hardware. Toggle NHL_MSAA=0/4.

## What was already checked (2026-10-28)

- The vertex shader's viewport epilogue keeps x/y in NDC (`vpx[0..1] = 1`, `rsx_d3d12_backend.c` ~6851). Geometry is placed relative to whatever D3D viewport is set, so it scales for free once render targets and viewports are scaled.
- The draw loop's per-pass viewport and scissor are set from the RT's own size (`vp.Width = off_rt[want].w`, ~9412-9434). This is one central place to scale.
- Resolution-dependent code sites: 74 off_rt `.w/.h` uses, 46 `s_d3d.width/height`, 15 `depth_buffer2`, 8 viewport/scissor sets, 23 SV_Position/WPOS uses.

## Design

1. **OffRT keeps two sizes.**
   - `gw, gh` = guest size: used for every guest-semantics test, such as the AA-factor math in `rsx_d3d12_gpu_blit`, `wb_reject` display-sized / msaa-layout, `depthcol_snapshot_maybe` display-size, and the RT lookups by size.
   - `w, h` = the D3D resource size = guest size x S for scalable targets.
   - Scalable = derived from the display:
     - the scene RT 0x02980000;
     - the post-process RTs 0x043F0000 / 0x047D0000;
     - the half-res chain 640x360 (0x038C/039D/03DF/03FF/041F);
     - the 320x180 DOF chain;
     - the display buffers.
   - Rule: the guest size equals the display size / 2^k.
   - Everything else stays 1x:
     - equipment, name and number composites (640x760, 512x512, 1024x512, which are written back to guest memory);
     - shadow maps (#190, SHADOWZ);
     - the Apt/UI scratch surfaces.
2. **Display buffers and depth.**
   - `render_targets[]` (sc_init) and the main depth buffer at S x the display size.
   - `depth_buffer2` (shared offscreen depth, 2048x2048) at 2048 x S (4096 at 2x).
   - The `s_zsnap` depth snapshot and its #209 colour copies sized from it.
   - `s_screen_copy` and `s_en_tex` at S x the display size.
   - The present blit then reads a 2560x1440 frame (1:1 on the user's screen).
3. **Viewports and scissors.**
   - Scale the per-draw sub-viewport and scissor (subvp_note, reduced-viewport passes, scissored clears, the `dvp`/`zvp` sites at ~9315/9823/9922) by the target's S.
   - Viewport = the full RT (already true); guest scissor rects x S.
4. **Fragment-position inputs.**
   - Where the FP translation reads `input.position.xy` (WPOS), divide by S so shaders that build screen UVs from pixel coords x constant (1/1280) still land right. RPCS3 does the same.
   - The #209 depthcol PS `t.Load(p.xy)` and other host passes that use Load with pixel coords must use the matching size.
5. **Copies.**
   - #85 GPU blit: source and destination rectangles x S (only between scalable RTs).
   - Copies between a scaled and an unscaled target: scaled draw / StretchRect instead of CopyTextureRegion.
   - NV3089 CPU copies out of scaled RTs: the guest-memory copy stays 1x (it reads guest memory, untouched).
6. **Write-back and readback.**
   - `wb_readback` of a scaled RT (rare: display-sized and msaa surfaces are already rejected) downsamples to the guest size first.
   - F9 captures write the scaled image (bigger BMPs, fine).
7. **Texture binding of RTs** (off_rt_find in the SRV pass): unchanged. Shaders sample with normalized UVs, so a 2x RT reads correctly.
   - `rsx_pt` / #240 clamp helpers take the size from GetDimensions, so they adapt.
   - Shader constants holding texel offsets (blur kernels of 1/1280) keep the same UV distances = the same look, just sharper.

## Risks to watch (each has a fix site above)

- Half-res lighting buffer / light pre-pass misaligned (WPOS or depth snapshot size).
- Bloom and DOF shifted or doubled (copies or WPOS).
- Jersey, helmet or name composites wrong: a composite wrongly classed as scalable means a size mismatch on write-back.
- Menus / Apt UI: UI drawn into the display buffer scales fine; UI scratch surfaces stay 1x.
- VRAM: 2x = about 4x memory for the scaled targets (~150 MB), fine on 16 GB; the Steam Deck may need 1.5x.
- Performance: about 4x the pixel work at 2x. The RX 9070 XT has plenty of headroom; check the [HZ] render_frame on the Deck.

## Steps (each builds + smokes on its own; env-gated, default 1x until verified)

1. `NHL_RENDER_SCALE` + OffRT `gw/gh` split (no behaviour change at 1x; smoke = identical captures).
2. Scale the display buffers, main depth and present path.
3. Scale offscreen scalable RTs + depth_buffer2 + the viewport/scissor sites.
4. WPOS divide in the FP translation; host passes (depthcol, DOF snapshot, screen copy).
5. #85 blits / copies between scaled and unscaled targets; write-back downsample.
6. Launcher setting + `[video] render_scale`.
7. One user check per area, short:
   - title / menus;
   - one face-off and a few seconds of play (lighting, bloom, DOF, jerseys, numbers);
   - a replay;
   - the Be a Pro editor face.
   - F9 if anything is off. Compare against RPCS3 at 200%.

**Estimate:** 2-4 work sessions + 2-3 short user checks. Release does not wait on it: ship #247, then add scaling as an update.
