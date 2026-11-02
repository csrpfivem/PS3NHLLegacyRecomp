/*
 * ps3recomp - D3D12 RSX Backend
 *
 * Translates RSX GPU state to D3D12 rendering commands.
 *
 * Phase 1 implementation:
 *   - Win32 window + D3D12 device + swap chain
 *   - Clear render target to RSX clear color
 *   - Present with vsync
 *   - Basic vertex-colored triangle rendering
 *
 * This file is C with COM calls (D3D12 is a COM API).
 * We use the C interface (__uuidof not available in C, so we
 * define GUIDs manually).
 */

#ifdef _WIN32

#include <time.h>
#include "rsx_d3d12_backend.h"
#include "rsx_primitives.h"
#include "rsx_vertex_fetch.h"
#include "rsx_texture_layout.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

/* D3D12 headers */
#include <d3d12.h>
#include <d3d12sdklayers.h>   /* ID3D12Debug / ID3D12InfoQueue */
#include <dxgi1_4.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <io.h>   /* _chsize_s, _fileno: #123 shader cache file */

/* We need these GUIDs — define them here to avoid uuid.lib dependency */
#include <initguid.h>

/* [#74] RSX_FPRINT state, defined in rsx_commands.c (-1 = env not read yet) */
extern int g_rsx_fprint_on;
extern unsigned long long g_rsx_fprint_hash;
extern unsigned int g_rsx_fprint_n;

/* Link libraries */
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

/* ---------------------------------------------------------------------------
 * Constants
 * -----------------------------------------------------------------------*/

#define FRAME_COUNT         2   /* double buffering */
/* [NHL local patch #105] 393216 -> 1048576: an NHL match frame converts well over
 * 600k vertices (shadow cascades, SSAO, reflections, the main scene); the main
 * pass's late draws -- the players -- were truncated to nothing. */
/* play143: [VBFULL] showed a match frame asking ~1.95M vertices -> 2.5M. */
#define MAX_VERTICES    2621440 /* per-frame vertex buffer (dbgfont submits
                                 * ~7.5k verts/frame; leave generous headroom) */
#define MAX_DRAWS         4096  /* [NHL #77: was 2048; NHL gameplay frames overflow it]
                                 * per-frame draw records. Sized from the
                                 * measured worst case (~1841, FRAME_BUDGET);
                                 * the per-draw constant buffer is
                                 * VP_CB_STRIDE * MAX_DRAWS * 2 bytes, so 16384
                                 * asked for a 264 MB upload allocation. */
/* Per-draw VP constant-buffer slot: vp_c[512] + posscale + posoffset
 * (514 vec4 = 8224 B) rounded up to D3D12's 256-byte CBV alignment.
 * Constants are snapshotted at RECORD time -- wave's passes each set their
 * own texScale/offset uniforms, so one per-frame snapshot ran every pass
 * with the LAST pass's constants. */
/* [NHL local patch #125] + the GPU vertex-fetch attribute table at vec4 528:
 * vp_attr[16] (uint4), vp_attr_def[16] (float4), vp_vtx (uint4) = 561 vec4
 * -> 9216 (36 x 256). Mirrored by rsx_vp_decompiler.c's gpu_fetch cbuffer. */
#define VP_CB_STRIDE      9216
#define VP_CB_ATTR_VEC4   528
#define VP_RAW_BYTES      (96u << 20)   /* [#125] raw vertex bytes per frame half */
/* b1 per-draw FP slot: rsx_texscale[4] + rsx_alphatest + fp_k[64]
 * = 69 float4 = 1104 B, rounded to D3D12's 256-byte CBV alignment. */
#define VP_FPCB_STRIDE    4352   /* [NHL #83: was 1280 (fp_k[64]); now 5 + 256 float4, 256-B aligned] */
#define VERTEX_STRIDE       36  /* bytes per host vertex: pos3 + col4 + uv2 */
/* [NHL local patch #51] Texture units a guest FP can sample: all 16 RSX units.
 * Was 4 (t0-t3 / s0-s3), so any program that read a higher unit -- NHL's
 * in-game shaders sample t9/t10 and up to t11 -- failed CreateGraphicsPipeline-
 * State with E_INVALIDARG (the shader binds registers outside the root
 * signature) and every draw of it was dropped. */
#define D3D_TEX_UNITS       16
#define VP_MAX_INDICES      (2u * 1024u * 1024u)   /* [NHL #62] per frame half */

typedef struct {
    u32 vb_byte_offset; /* offset into vb_mapped where this draw's verts live */
    u32 vertex_count;
    u32 topology;       /* rsx_topology (NOT a D3D value; see topo_to_d3d) */
    int textured;       /* 1 = sample the bound font/atlas texture (dbgfont) */
    int is_vp;          /* 1 = real VP path: vb_byte_offset indexes vp_vb (float4) */
    /* VP path per-draw shader/texture state, captured at draw_arrays time. */
    u32 fp_addr;        /* SET_SHADER_PROGRAM value (guest FP ucode location)   */
    const u8* fp_snap;  /* [#82] FP ucode captured at record time (batch arena) */
    u32 fp_snap_len;
    u32 fp_key;         /* [#89] PSO program key (uhash) used at execution, for the rsxcap manifest */
    int vp6_idx;        /* [#179] guest movie frame index of a bound movie Y plane at record time, -1 = none */
    u32 vp6_wh;         /* [#179] that movie's (w << 16) | h */
    int is_blit;        /* [#85] NV3089 copy between offscreen RTs (also is_clear=1 so draw loops skip it) */
    u32 blit_src, blit_dst;                     /* off_rt offsets */
    u32 blit_sx, blit_sy, blit_dx, blit_dy, blit_w, blit_h;   /* RT pixel coords */
    int fp_exp32;       /* SET_SHADER_CONTROL 32-bit-exports bit at draw time   */
    u32 alpha_ctl;      /* alpha test: enable<<16 | (func&0xFF)<<8 | ref */
    u32 begin_epoch;    /* SET_BEGIN_END generation, for batch concatenation */
    u32 cull;           /* packed face culling: bit0 enable, bit1 cull FRONT
                         * (else BACK), bit2 front face is CCW. RSX culls back
                         * faces on most solid geometry; rendering everything
                         * double-sided lets a shell's interior faces show
                         * through and shade unlit (black). */
    u32 cmask;          /* D3D write mask from SET_COLOR_MASK at draw time
                         * (wave's sim passes write single lanes of the height
                         * maps; ignoring the mask stomped persistent state) */
    /* Per-unit textures (t0-t3): decompiled FPs sample up to 4 units
     * (demosaic's interpolation passes read 3). Captured at draw time. */
    struct {
        u32 off;        /* resolved vm offset (guest upload source), 0 = none */
        u32 raw;        /* raw RSX offset (offscreen-RT matching) */
        u32 w, h, fmt;  /* dims + RSX base format */
        u32 ctrl1;      /* NV4097 TEXTURE_CONTROL1: component remap crossbar */
        u32 pitch;      /* [#173] TEXTURE_CONTROL3 [19:0]: row pitch of a linear texture */
        u32 filter;     /* [#176] NV4097 TEXTURE_FILTER: min [18:16], mag [26:24] */
        u32 addr, ctl0, dim;   /* [#190] TEXTURE_ADDRESS (zfunc [31:28]), TEXTURE_CONTROL0, dimension */
        const u8* snap; /* [#179] movie plane bytes captured at record time (NULL = read guest memory) */
        u32 mips;       /* SET_TEXTURE_FORMAT bits 16..31: mipmap level count.
                         * Cube faces sit one whole mip pyramid apart, so this
                         * is what sets the face stride. */
        int cube;       /* SET_TEXTURE_FORMAT bit 2: a cube texture. Sampled
                         * with a 3-component direction, not a 2D uv -- treating
                         * one as 2D is what makes an environment-mapped chrome
                         * surface come out with black patches and banding. */
        int set;
    } tex[D3D_TEX_UNITS];
    int tex_rt[D3D_TEX_UNITS];  /* pre-pass: OffRT index sampled by unit, -1 = none */
    u32 srv_win;        /* [#185] draw index whose SRV window this draw binds (itself, or an
                         * earlier draw this frame with identical texture bindings) */
    /* [#190] shadow maps: is_depth 1 = a depth-only (shadow-pass) draw, 2 = its depth
     * clear (value in cc[0]); both also set is_clear so the colour paths skip them.
     * zeta_off / z_w / z_h = the depth surface. zmask = units bound to a shadow map. */
    int is_depth;
    u32 zeta_off, z_w, z_h;
    u32 zmask;
    int dcz;            /* [#209] depth surface (index + 1) sampled as ARGB8 by this draw, 0 = none */
    int tex_slot;       /* legacy single-slot path (atlas); -1 = none */
    int vs_idx;         /* VPVSEntry slot for this draw's vertex program (-1 = primary) */
    int blend;          /* guest blend enable at draw time */
    u32 blend_key;      /* packed guest blend state (factors+equation), PSO key */
    /* Render-to-texture: which colour surface this op targets. 0 = a display
     * buffer (the swap-chain backbuffer); else the resolved vm offset of an
     * offscreen surface (demosaic chains its effect passes through local-
     * memory buffers and composites from them). */
    u32 rt_off;
    /* Which per-draw constant slot this record's constants were written to.
     * vp_record_cb writes them keyed by the RECORD index, so any pass that
     * reorders or compacts s_d3d.draws (DRAW_KEEP_TEX, DRAW_LAST_TEX,
     * DRAW_LIMIT) would otherwise make every surviving draw read some other
     * object's MVP -- silently transforming it somewhere else entirely. */
    u32 cb_slot;
    u32 rt_mrt[3];      /* colour targets B,C,D (MRT1/2/3), 0 = none. Deferred
                         * shading G-buffers write 3-4 targets in one pass. */
    u32 rt_w, rt_h;     /* surface clip dims at record time (offscreen RT size) */
    u32 rt_fmt;         /* RSX surface colour format (SET_SURFACE_FORMAT [4:0]) */
    /* Guest viewport rect at draw time (target pixels). Sub-viewport layouts
     * (wave's debug tiles) position quads with this, not with constants --
     * forcing full-target viewports drew every tile window-sized. */
    u32 vp_x, vp_y, vp_w, vp_h;
    /* Ordered clear op (offscreen surfaces only; display clears stay the
     * frame-start backbuffer clear). is_clear records also set is_vp so the
     * legacy replay pass skips them. */
    int   is_clear;
    float cc[4];
    /* [NHL local patch #223] guest scissor at clear time: RSX clears only inside it. */
    u32 clr_x, clr_y, clr_w, clr_h;
    /* [NHL local patch] guest stencil state at draw time: packed PSO key
     * (0 = test off) and the reference value set per draw. */
    u32 stencil_key;
    u32 stencil_ref;
    int placeholder;    /* [NHL local patch] unclaimed Apt image-slot fill (see s_vp_placeholder) */
    u32 surf_raw;       /* [NHL local patch] raw colour surface offset + target at record time (diag) */
    u32 surf_ct;
    /* [NHL local patch #62] GPU-indexed VP draw: vertex_count = INDEX count,
     * vb_byte_offset = base vertex record, ib_first = first u32 in vp_ib. */
    int indexed;
    u32 ib_first;
    int gpu_vtx;        /* [#125] VS fetches the raw guest arrays (vb_byte_offset unused, 0) */
    /* [NHL local patch #108] guest viewport y/z transform + SHADER_WINDOW at
     * record time (diag: the backend maps x/y as identity, so a positive
     * scale.y or a bottom origin would mirror the winding against the guest's). */
    float vps_y, vpo_y, vps_z, vpo_z;
    u32 winorg;
    /* [#114] exec-loop progress in the last render pass: 0 never reached,
     * 1 reached, 2 dropped as placeholder, 3 PSO looked up, 4 drawn;
     * exec_pass = which render_frame pass (s_exec_pass) touched it last. */
    u32 exec, exec_pass;
    /* [#117 diag] TEXRACE_VS: texture checksums of units 0-3 at RECORD time */
    u32 rec_csum[4];
    int rec_csum_on;
    /* [#118 diag] what the pre-pass bound per unit 0-3: kind 0 none/null, 1 RT, 2 slot,
     * 3 screen copy / sub-viewport; id = RT index or texture slot */
    u8  bound_kind[4];
    u16 bound_id[4];
} D3D12DrawRecord;

/* Offscreen render target (render-to-texture). Persistent across frames --
 * pass N's output is sampled by pass N+1 and possibly by later frames. */
typedef struct {
    ID3D12Resource*       res;
    ID3D12Resource*       up;          /* init-upload staging (kept alive) */
    u32                   off, w, h;   /* raw RSX offset + dims */
    u32                   dxgi;        /* DXGI_FORMAT of the resource */
    D3D12_RESOURCE_STATES st;          /* tracked resource state */
    int                   used;        /* referenced this frame */
    u64                   blit_frame;  /* #213: frame_count + 1 of the last GPU blit into it, 0 = none */
    u32                   last_flip;   /* #243: guest flip of the last use, for LRU eviction */
    ID3D12Resource*       ms;          /* #249: multisampled twin (guest AA surfaces), NULL = none */
    D3D12_RESOURCE_STATES ms_st;       /* #249: its tracked state */
    int                   ms_dirty;    /* #249: drawn since its last resolve into res */
    u32                   rw, rh;      /* #253: D3D resource size (w x h = the guest's; differ when scaled) */
} OffRT;

/* [NHL local patch #253] Internal resolution scaling. The game renders 1280x720; the display
 * buffers and every offscreen surface derived from the display size (1280x720, 640x360, 320x180
 * ... = the scene, post chain, half-res lighting and DOF chains) are D3D resources S times
 * larger, so the frame reaches the screen at native resolution instead of a 720p upscale.
 * Guest-facing sizes (OffRT.w/h, s_d3d.width/height, record-time viewports) stay 1x; only the
 * resource sizes (OffRT.rw/rh, s_dw/s_dh) and the viewports / scissors / copy boxes set on the
 * GPU are scaled. Composites (640x760, 512x512 ...), shadow maps and the Apt scratch surfaces
 * stay 1x: they are written back to guest memory or sampled at fixed texel sizes.
 * NHL_RENDER_SCALE = native (the screen: min(screen w / 1280, screen h / 720)) or a factor
 * 1..4; unset = 1. The vertex programs keep x/y in NDC, so geometry follows the viewport;
 * fragment programs that read WPOS get it divided back to guest pixels (fp_alpha.w). */
static float s_rs = 1.0f;   /* render scale S */
static u32 s_dw, s_dh;      /* display surfaces' resource size */
static u32 rs_up(u32 g) { return (u32)((float)g * s_rs + 0.5f); }
/* [NHL local patch #254] Effects scale (NHL_FX_SCALE 1, 2, 4; default 1): display-derived surfaces
 * SMALLER than the display (the half-res lighting / bloom / DOF chains) get this extra factor, capped
 * at the display size -- 2 renders the half-res passes at full res, 4 the quarter-res ones too. */
static int s_fxs = 1;
static int rs_active(void) { return s_rs != 1.0f || s_fxs > 1; }
static int shadow_scale(void);   /* #254, with the shadow maps */
/* the shared depth buffers cover every RT: 2048 (the largest guest surface) or the scaled display */
static u32 zbuf_w(void) { return s_dw > 2048u ? s_dw : 2048u; }
static u32 zbuf_h(void) { return s_dh > 2048u ? s_dh : 2048u; }
/* a surface derived from the display size (display / 2^k) renders scaled */
static int rs_scalable(u32 w, u32 h, u32 dw, u32 dh)
{
    return w && h && w <= dw && h <= dh && dw % w == 0 && dh % h == 0 && dw / w == dh / h;
}
static const char* wb_kind(u32 off);   /* #244, defined with the write-back table */
/* [NHL local patch #116] 16 -> 64. NHL's match uses ~30 colour surfaces
 * (jersey/number composites 640x760, 512x512, 1024x512 ...); with 16, a
 * surface that was rendered once and is only SAMPLED afterwards (a jersey) was
 * evicted -- its contents destroyed -- whenever a new one appeared: flashing
 * jerseys. RT SRVs move to 5..68 and the per-draw windows start after them. */
#define MAX_OFF_RTS  64  /* demosaic double-buffers its 6-surface pass chain */
#define RT_SRV_BASE  5   /* SRV heap slots 5..5+MAX_OFF_RTS-1 hold offscreen-RT SRVs */
/* Per-draw SRV windows: each VP draw gets 4 consecutive descriptors (t0-t3)
 * so multi-unit fragment programs see all their textures (demosaic's
 * interpolation passes sample 3 units). */
#define DRAW_SRV_BASE (RT_SRV_BASE + MAX_OFF_RTS + 3)
/* [NHL local patch #116] vp_upload_tex_slot's own per-slot SRV. It was written
 * at heap[1 + slot]: fine for the original 4 slots, but with VP_TEX_SLOTS 1024
 * (#100) every upload into slot 4..19 overwrote an offscreen-RT SRV and every
 * slot >= 31 overwrote a per-draw window already filled this frame (draws
 * 0..63) -- textures swapping on other draws whenever one was (re)uploaded. */
#define SLOT_SRV_BASE (DRAW_SRV_BASE + MAX_DRAWS * D3D_TEX_UNITS)
#define SRV_HEAP_SIZE (SLOT_SRV_BASE + VP_TEX_SLOTS)

/* Per-frame VP texture slot: a guest texture uploaded for this frame's VP
 * draws (re-uploaded every frame -- gcm/cube's plasma animates in guest
 * memory). SRV lives at heap index 1+slot. */
typedef struct {
    ID3D12Resource* res;
    ID3D12Resource* up;
    u32 off, w, h, fmt; /* current contents (resource reused when dims match) */
    u32 csum;           /* sparse checksum of the source bytes last uploaded */
    int cube;           /* resource is a 6-face cube, sampled as TextureCube */
    u32 key;            /* the ORIGINAL bound offset -- the cache lookup key.
                         * off is the RESOLVED source after TEX_OFF_BIAS/TEX_REMAP
                         * shift it, so keying on off never matches the raw offset
                         * a later draw arrives with, and every draw re-uploads. */
    int used;           /* referenced this frame */
    u32 nl;             /* [#119] mip levels in res */
    u32 scsum;          /* [#134] sampled checksum of the same bytes */
    u32 ok_frame;       /* [#134] frame of the last full checksum match */
    u32 stable;         /* [#134] full checksum matches since the last upload */
    u32 last_frame;     /* [#136] frame this slot was last bound (LRU eviction) */
    u32 pitch;          /* [#173] guest row pitch the contents were decoded with (0 = packed) */
    int hp_heap; u64 hp_off, hp_size;   /* [#220] placed-resource block (hp_size 0 = committed) */
    u32 hbytes;         /* [#239] source bytes csum/scsum cover (whole mip chain when nl > 1) */
} VPTexSlot;
/* Distinct textures uploadable per frame. Four was enough for a UI/atlas-style
 * frame but not for a scene: this title binds ~20 distinct textures per frame,
 * so every draw past the fourth got slot -1 from vp_upload_tex_slot ("out of
 * slots") and drew UNTEXTURED. That reads as a correctly-lit but solid black
 * object, which is indistinguishable from a missing texture upload. */
/* [NHL local patch #100] 96 -> 1024. A match frame binds far more than 96
 * distinct textures (arena, 40 players' jerseys/faces/gear, crowd): once the
 * slots ran out, vp_upload_tex_slot returned -1 and every later draw sampled
 * a NULL SRV -- the ice and arena (drawn late in the frame) came out black. */
#define VP_TEX_SLOTS 1024

/* Decompiled-VS cache: one entry per distinct vertex-program ucode seen at
 * draw time (hashed). Apps switch VPs between draws (gcm/cube: its MVP cube VP
 * vs dbgfont-gcm's text VP); compiling only the first left later draws
 * transformed by the wrong program (text offscreen). */
typedef struct {
    u32 hash;               /* FNV-1a of the ucode (^ VS_GPU_SALT for the #125 variant) */
    ID3DBlob* vs;
    int uses_c03;
    int gpu;                /* [#125] reads raw vertex arrays itself: PSO has no input layout */
    int ni;                 /* [#225b] RSX instruction count (tiny = full-screen pass VS) */
} VPVSEntry;
#define VS_GPU_SALT 0x5A3C96E1u
/* [NHL local patch #58] 16 -> 1024, and eviction now replaces one slot in
 * place (round robin) instead of shifting the array. NHL's gameplay uses ~80
 * distinct VPs: the 16-slot cache was full all the time, every new VP shifted
 * every slot, and draws already recorded this frame ran a DIFFERENT vertex
 * program -- the in-game TDR (play31-33, DRED: device hung on a 36-vertex draw
 * running another object's branchy skinning VP). */
#define VP_VS_CACHE 1024 /* wave uses 5+ distinct VPs; at 4 the cache
                         * thrashed every frame and eviction shifted slots
                         * under recorded vs_idx values -- the display mesh
                         * drew with the WRONG vertex program (sub-rect +
                         * edge slivers instead of fullscreen) */

/* Compiled guest-FP pipeline cache: decompiled VS (by cache slot) + decompiled
 * PS (fragment ucode at fp_addr). */
typedef struct {
    u32 fp_addr;
    int vs_idx;             /* VPVSEntry slot this PSO's VS came from */
    u32 vs_hash;            /* validates the slot hasn't been evicted */
    u32 gen;
    u32 blend;              /* packed guest blend key (enable+factors+eq, PSO key) */
    int nrt;                /* bound colour target count (PSO key)       */
    u32 rtfmt;              /* DXGI format of the colour targets (PSO key) */
    int exp32;              /* 32-bit-exports control bit (PSO key)       */
    u32 ucode_hash;         /* FNV-1a of the FP ucode: apps re-patch inline
                             * constants per frame (wave's stamp position/
                             * amplitude) -- address-only keying served the
                             * stale compile forever. */
    u32 cmask;              /* colour write mask (PSO key) */
    u32 cull;               /* packed face culling (PSO key) */
    u32 cube_mask;          /* which units are cube textures (PSO key): the HLSL
                             * declares those samplers as TextureCube, so a cube
                             * and a 2D variant of the same program are different
                             * pipelines and must not share a cache entry. */
    u32 stencil;            /* [NHL local patch] packed stencil key (PSO key) */
    ID3D12PipelineState* pso;
} VPFPEntry;
/* Guest-FP PSO cache. 16 entries thrashed badly: this title needs far more
 * distinct (fp, vs, blend, rt, cmask, ucode-hash) combinations than that in a
 * single frame, so the FIFO evicted entries that were needed again immediately
 * and ~17 shaders were recompiled EVERY frame -- about half the frame's CPU
 * time. Entries are small (a PSO pointer plus key fields). */
#define VP_FP_CACHE 16384  /* [NHL #58: 256 -> 1024; #80: -> 16384 + hash index -- play73's
                            * in-match working set overflowed 1024 and the FIFO
                            * eviction thrashed (300-500 misses per 5 s)] */
#define VP_FP_BUCKETS 8192 /* power of two; chains keyed by ucode_hash */

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

typedef struct {
    /* Window */
    HWND hwnd;
    u32  width;
    u32  height;
    int  window_closed;

    /* D3D12 core */
    ID3D12Device*              device;
    ID3D12CommandQueue*        cmd_queue;
    IDXGISwapChain3*           swap_chain;
    ID3D12DescriptorHeap*      rtv_heap;
    u32                        rtv_descriptor_size;
    ID3D12Resource*            render_targets[FRAME_COUNT];
    ID3D12CommandAllocator*    cmd_allocators[FRAME_COUNT];
    ID3D12GraphicsCommandList* cmd_list;

    /* Synchronization */
    ID3D12Fence* fence;
    HANDLE       fence_event;
    u64          fence_values[FRAME_COUNT];
    u32          frame_index;

    /* Pipeline */
    ID3D12RootSignature*  root_signature;
    ID3D12PipelineState*  pipeline_state;         /* triangle class — default */
    ID3D12PipelineState*  pipeline_state_lines;   /* line class */
    ID3D12PipelineState*  pipeline_state_points;  /* point class */

    /* Depth/stencil */
    ID3D12DescriptorHeap* dsv_heap;
    ID3D12Resource*       depth_buffer;
    ID3D12Resource*       depth_buffer2;   /* [NHL local patch] offscreen-RT depth/stencil (DSV heap [1]) */

    /* Dynamic vertex buffer (upload heap) */
    ID3D12Resource*       vertex_buffer;
    D3D12_VERTEX_BUFFER_VIEW vb_view;
    void*                 vb_mapped;      /* persistently mapped */
    u32                   vb_offset;      /* current write position */

    int                   pipeline_ready; /* 1 if root sig + PSO created */

    /* Debug: copy the presented backbuffer to disk as BMP for the first N
     * frames (enabled by env CELLMARK_DUMP). Lets us verify what actually
     * rendered without racing the window/process lifetime. */
    ID3D12Resource*       readback_buf;
    u32                   readback_pitch;
    int                   dump_frames_left;
    int                   dump_skip_left;   /* CELLMARK_DUMP_SKIP: presents to ignore first */

    /* Textured pipeline (dbgfont / 2D atlas quads). The font atlas is an 8-bit
     * coverage texture uploaded as R8_UNORM and sampled in the pixel shader. */
    ID3D12PipelineState*  pipeline_state_tex;   /* triangle + texture + blend */
    ID3D12DescriptorHeap* srv_heap;             /* shader-visible, 1 SRV slot  */
    ID3D12Resource*       tex_resource;         /* current atlas texture       */
    ID3D12Resource*       tex_upload;           /* staging buffer for uploads  */
    u32                   tex_w, tex_h;         /* dims of tex_resource         */
    int                   tex_ready;            /* 1 once an atlas is uploaded  */
    u32                   tex_src_offset;       /* guest RSX offset of atlas    */
    int                   tex_bound;            /* a texture is bound for draws */
    int                   tex_dirty;            /* re-upload needed this frame  */

    /* Real RSX vertex-program path: the captured VP is decompiled to HLSL and
     * used as the vertex shader, fed the raw float4 attrib0 + the vp_c[]
     * constant bank. This produces exact position + texcoord (vs. the frac
     * approximation). Used for the 2D/dbgfont quad draws. */
    ID3D12RootSignature*  vp_root_sig;          /* CBV(b0) + SRV table + sampler */
    ID3D12PipelineState*  pipeline_state_vp;    /* decompiled VS + atlas PS      */
    ID3D12PipelineState*  pipeline_state_vp_color; /* decompiled VS + colour PS (untextured 3D) */
    ID3D12Resource*       vp_vb;                /* raw float4 attrib0, per-frame */
    ID3D12Resource*       vp_ib;                /* [NHL #62] u32 indices for GPU-indexed VP draws, x2 parity */
    void*                 vp_ib_mapped;
    u32                   vp_ib_offset;         /* in indices, this frame's half */
    ID3D12Resource*       vp_raw;               /* [NHL #125] raw guest vertex bytes, x2 parity */
    u8*                   vp_raw_mapped;
    u32                   vp_raw_offset;        /* bytes, this frame's half */
    void*                 vp_vb_mapped;
    u32                   vp_vb_offset;
    ID3D12Resource*       vp_cb;                /* per-draw VP constant slots    */
    void*                 vp_cb_mapped;
    ID3D12Resource*       vp_fpcb;              /* per-draw FP texscale (b1)     */
    void*                 vp_fpcb_mapped;
    int                   vp_ready;             /* VS+PSO compiled ok            */
    u32                   vp_compiled_bytes;    /* ucode size when last compiled */
    ID3DBlob*             vp_vs_blob;           /* kept for guest-FP PSO builds  */
    u32                   vp_gen;               /* bumped per VP recompile       */
    int                   vp_uses_c03;          /* VS references vp_c[0..3]      */
    VPTexSlot             vp_tex[VP_TEX_SLOTS]; /* per-frame VP texture slots    */
    VPVSEntry             vp_vs[VP_VS_CACHE];   /* per-draw decompiled VS cache  */
    int                   vp_vs_n;
    VPFPEntry             vp_fp[VP_FP_CACHE];   /* guest-FP PSO cache            */
    int                   vp_fp_n;
    u32                   srv_inc;              /* CBV_SRV_UAV descriptor size   */
    /* VP path: latest texture bound per unit (t0-t3). */
    struct { u32 off, raw, w, h, fmt, ctrl1, mips, pitch, filter, addr, ctl0, dim; int cube; int set; } cur_texs[D3D_TEX_UNITS];   /* #190 addr/ctl0/dim */

    /* Render-to-texture: offscreen RT pool + their RTV heap. */
    OffRT                 off_rt[MAX_OFF_RTS];
    /* DRAW_ARRAYS batch merging (see d3d12_draw_arrays): the vertex index one
     * past the last batch, and whether the current BEGIN/END may still be
     * extended. */
    u32                   merge_first_end;
    int                   merge_prev_draw;
    ID3D12DescriptorHeap* rt_rtv_heap;          /* MAX_OFF_RTS RTVs (CPU only)   */

    /* Frame-parity double buffering for the per-draw upload streams (vp_vb
     * vertices, vp_cb constants, vp_fpcb texscales): records for frame N+1
     * are written while frame N's GPU work may still be reading -- writes go
     * to the other half. Toggled at the end of render_frame. */
    int                   vp_parity;

    /* Per-frame draw recording */
    int                   frame_recording; /* 1 if cmd list is open for recording */
    u32                   draw_count;      /* draws this frame */
    D3D12DrawRecord       draws[MAX_DRAWS];

    /* Pointer to current RSX state (set before draw calls) */
    const rsx_state*      current_rsx_state;

    /* Current frame state */
    float clear_color[4];  /* RGBA float */

    /* Stats */
    u64 frame_count;
    u64 last_fps_time;
    u32 fps;

    int initialized;
} D3D12State;

static D3D12State s_d3d;
char g_rsx_title_base[128] = "ps3recomp";
static u32 s_dbg_last_draws = 0;
static u64 s_req_verts = 0, s_req_draws = 0, s_drop_draws = 0;
/* Raw bind offset of the duck's texture, identified by CONTENT (duck.tga is
 * overwhelmingly yellow, avg 243,191,23). VRAM offsets move between runs, so a
 * hard-coded offset in a filter silently matches nothing and the run looks like
 * "renders nothing" -- which cost real time. DRAW_KEEP_TEX=duck resolves here. */
/* Copy of the last completed frame, bound wherever a draw samples a
 * DISPLAY-SIZED texture. On RSX this title renders its reflection into a corner
 * of the render surface and then samples that surface as a texture; our backend
 * renders into a D3D backbuffer, so the guest memory behind that sampler is
 * never written and it reads empty -- which is why the water had no reflection.
 * Feeding it the previous frame costs one frame of latency, which for a water
 * reflection is not visible. */
static ID3D12Resource* s_screen_copy = NULL;
/* Sub-viewport render-to-texture. This title renders its reflection/refraction
 * pre-pass into a REGION of the same surface (vp 0,208 512x512 and 0,208
 * 1024x512, cmask=F) and then samples a texture of exactly that size. On
 * hardware the pass lands in the buffer that texture points at; our backend
 * renders into D3D resources, so that guest buffer is never written and every
 * fluid sampler reads zero. Capture each region into a matching resource and
 * bind it for samplers whose guest source is empty and whose size matches.
 *
 * SUBVP_RTT=1 to enable. OFF by default because it cannot be validated on this
 * title yet: the fluid draws that would consume these captures never rasterize
 * (see below), so binding them changes nothing observable. The capture and
 * binding themselves are confirmed working -- SUBVP_DBG shows the 512x512 and
 * 256x256 regions captured and handed to fp 0x5DB81/0x5EA81/0x5EF81/0x5E381. */
#define SUBVP_SLOTS 4
static struct { ID3D12Resource* res; u32 x, y, w, h; } s_subvp[SUBVP_SLOTS];
static int s_subvp_n = 0;

static void subvp_note(u32 x, u32 y, u32 w, u32 h)
{
    if (!w || !h || w > s_d3d.width || h > s_d3d.height) return;
    if (w == s_d3d.width && h == s_d3d.height) return;      /* full-screen pass */
    for (int i = 0; i < s_subvp_n; i++)
        if (s_subvp[i].w == w && s_subvp[i].h == h) { s_subvp[i].x = x; s_subvp[i].y = y; return; }
    if (s_subvp_n >= SUBVP_SLOTS) return;
    s_subvp[s_subvp_n].x = x; s_subvp[s_subvp_n].y = y;
    s_subvp[s_subvp_n].w = w; s_subvp[s_subvp_n].h = h;
    s_subvp[s_subvp_n].res = NULL;
    s_subvp_n++;
}

/* Copy each noted region out of the backbuffer. Called at the same moment as
 * screen_copy_capture -- after the reduced-viewport passes and before the
 * full-screen scene overwrites them. */
static void subvp_capture(u32 fi)
{
    static int en = -1;
    if (en < 0) { const char* e = getenv("SUBVP_RTT"); en = e ? atoi(e) : 0; }
    if (!en || !s_d3d.device || !s_subvp_n || s_rs != 1.0f) return;   /* #253: guest-size boxes */
    for (int i = 0; i < s_subvp_n; i++) {
        if (!s_subvp[i].res) {
            D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC td = {0};
            td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            td.Width = s_subvp[i].w; td.Height = s_subvp[i].h;
            td.DepthOrArraySize = 1; td.MipLevels = 1;
            td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
            td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            if (FAILED(s_d3d.device->lpVtbl->CreateCommittedResource(
                    s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &td,
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, NULL,
                    &IID_ID3D12Resource, (void**)&s_subvp[i].res)))
                { s_subvp[i].res = NULL; continue; }
        }
        D3D12_RESOURCE_BARRIER bb[2] = {0};
        bb[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        bb[0].Transition.pResource   = s_d3d.render_targets[fi];
        bb[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        bb[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
        bb[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        bb[1] = bb[0];
        bb[1].Transition.pResource   = s_subvp[i].res;
        bb[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        bb[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
        s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 2, bb);

        D3D12_TEXTURE_COPY_LOCATION dstl = {0}, srcl = {0};
        dstl.pResource = s_subvp[i].res;
        dstl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dstl.SubresourceIndex = 0;
        srcl.pResource = s_d3d.render_targets[fi];
        srcl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; srcl.SubresourceIndex = 0;
        D3D12_BOX box; box.left = s_subvp[i].x; box.top = s_subvp[i].y;
        box.front = 0; box.right = s_subvp[i].x + s_subvp[i].w;
        box.bottom = s_subvp[i].y + s_subvp[i].h; box.back = 1;
        if (box.right > s_d3d.width)  box.right  = s_d3d.width;
        if (box.bottom > s_d3d.height) box.bottom = s_d3d.height;
        s_d3d.cmd_list->lpVtbl->CopyTextureRegion(s_d3d.cmd_list, &dstl, 0, 0, 0, &srcl, &box);

        bb[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        bb[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
        bb[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        bb[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 2, bb);
        { static int _n = 0; if (getenv("SUBVP_DBG") && _n++ < 8)
            fprintf(stderr, "[SUBVP] captured %ux%u from (%u,%u)%c",
                    s_subvp[i].w, s_subvp[i].h, s_subvp[i].x, s_subvp[i].y, 10); }
    }
}
static void screen_copy_capture(u32 fi);   /* fwd */
static int  s_sc_dump_pending = 0;
static u32 s_duck_raw = 0;   /* as BOUND (what draw records carry) */
static u32 s_duck_off = 0;   /* as RESOLVED (what the uploader sees)   */
/* PERF=1: where a frame's CPU time actually goes. Guessing at this is how you
 * spend an afternoon optimising the wrong loop. */
static double s_perf_tex = 0.0, s_perf_frame = 0.0, s_perf_vtx = 0.0, s_perf_rf = 0.0;
static double s_perf_gpu = 0.0, s_perf_pre = 0.0, s_perf_srv = 0.0, s_perf_pso = 0.0;
static int s_perf_pso_calls = 0, s_perf_pso_miss = 0, s_perf_pso_hashbytes = 0;
/* [NHL local patch] PERF vertex-path breakdown: which upload path each
 * converted vertex came from (printed with [PERF] as [PERFVTX]). */
enum { VP_QUADS, VP_TRIS, VP_STRIP, VP_QUADS_IDX, VP_TRIS_IDX_CPU, VP_TRIS_IDX_GPU, VP_STRIP_IDX, VP_NPATH };
static int s_vpath = VP_TRIS;
static u64 s_vcnt[VP_NPATH];
static u64 s_vcalls[VP_NPATH];
static u64 s_idx_fb_partial, s_idx_fb_sparse, s_idx_fb_room;
static u64    s_perf_nverts = 0;
static u64    s_perf_texbytes = 0;
static int    s_perf_ntex = 0;
static double perf_now(void)
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
}
/* [NHL local patch #122] Always-on hitch counters (QPC microseconds + counts):
 * FXC compiles (VS / PS), CreateGraphicsPipelineState, texture binds+uploads,
 * render_frame. boot_main.cpp's HITCH=1 monitor prints them per second and per
 * long frame. F8 (global hotkey with HITCH=1) sets g_hitch_mark_req. */
extern unsigned long long ps3_qpc_us(void);
volatile long long g_hitch_vs_us = 0, g_hitch_vs_n = 0, g_hitch_ps_us = 0, g_hitch_ps_n = 0;
volatile long long g_hitch_pso_us = 0, g_hitch_pso_n = 0, g_hitch_tex_us = 0, g_hitch_tex_n = 0;
volatile long long g_hitch_rf_us = 0;
/* [#136] texture uploads by cause: new key, changed bytes, slot taken from
 * another texture (eviction), GPU resource (re)creations. */
volatile long long g_hitch_texup_new = 0, g_hitch_texup_chg = 0, g_hitch_texup_evict = 0,
                   g_hitch_texup_create = 0;
/* [#219] host time in texture GPU allocations (CreateCommittedResource) and in decoding
 * guest bytes into the staging memory */
volatile long long g_hitch_texup_alloc_us = 0, g_hitch_texup_decode_us = 0;
volatile long g_hitch_mark_req = 0, g_hitch_mark_tick = 0;   /* tick = GetMessageTime() ms */
#define HITCH_ADD(var, v) __atomic_fetch_add(&(var), (long long)(v), __ATOMIC_RELAXED)

/* [NHL local patch #123] Persistent FXC bytecode cache. Guest VP/FP -> HLSL ->
 * FXC cost ~30 ms (PS) / ~80 ms (VS) per program, all on the RSX ticker thread;
 * the match start compiles ~700 of them (play172: a 24 s freeze). Compiled
 * bytecode is kept in memory and appended to SHADER_CACHE_DIR/fxc_cache.bin
 * (default "shadercache" under the working directory), keyed by two 64-bit
 * hashes of the final HLSL text + its length + profile/flags, so any change to
 * the decompiler output is simply a different key. Same text => same bytecode,
 * so a hit cannot change what is drawn. SHADER_CACHE=0 disables it.
 * Record: u32 'FXC1', u32 kind, u64 k1, u64 k2, u32 hlsl_len, u32 blob_len,
 * u32 blob_fnv, blob. A torn tail record (crash mid-write) is ignored. */
#define FXC_REC_MAGIC 0x31435846u
enum { FXC_HT = 1 << 16 };
typedef struct { unsigned long long k1, k2; u32 kind, len; ID3DBlob* b; } FxcEnt;
static FxcEnt* s_fxc_ht = NULL;   /* open addressing, FXC_HT slots */
static int s_fxc_n = 0, s_fxc_state = 0;   /* 0 = not loaded, 1 = on, -1 = off */
static FILE* s_fxc_file = NULL;
static CRITICAL_SECTION s_fxc_cs;
static unsigned long long fxc_fnv(const char* s, size_t n, unsigned long long h)
{ for (size_t i = 0; i < n; i++) { h ^= (u8)s[i]; h *= 1099511628211ull; } return h; }
static u32 fxc_fnv32(const u8* p, size_t n)
{ u32 h = 2166136261u; for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; } return h; }
static FxcEnt* fxc_slot(unsigned long long k1, unsigned long long k2, u32 kind, u32 len)
{
    for (u32 i = (u32)(k1 & (FXC_HT - 1)), n = 0; n < FXC_HT; n++, i = (i + 1) & (FXC_HT - 1)) {
        FxcEnt* e = &s_fxc_ht[i];
        if (!e->b || (e->k1 == k1 && e->k2 == k2 && e->kind == kind && e->len == len)) return e;
    }
    return NULL;   /* full */
}
static void fxc_put(unsigned long long k1, unsigned long long k2, u32 kind, u32 len, ID3DBlob* b)
{
    if (s_fxc_n >= FXC_HT * 3 / 4) return;   /* keep probes short; still compiled, just not cached */
    FxcEnt* e = fxc_slot(k1, k2, kind, len);
    if (!e || e->b) return;
    e->k1 = k1; e->k2 = k2; e->kind = kind; e->len = len; e->b = b; b->lpVtbl->AddRef(b);
    s_fxc_n++;
}
static void fxc_load(void)
{
    const char* on = getenv("SHADER_CACHE");
    if (on && atoi(on) == 0) { s_fxc_state = -1; return; }
    InitializeCriticalSection(&s_fxc_cs);
    s_fxc_ht = (FxcEnt*)calloc(FXC_HT, sizeof(FxcEnt));
    if (!s_fxc_ht) { s_fxc_state = -1; return; }
    const char* dir = getenv("SHADER_CACHE_DIR"); if (!dir || !*dir) dir = "shadercache";
    CreateDirectoryA(dir, NULL);
    char path[MAX_PATH]; snprintf(path, sizeof path, "%s/fxc_cache.bin", dir);
    long good_end = 0; int nload = 0, nbad = 0;
    FILE* f = fopen(path, "rb");
    if (f) {
        for (;;) {
            u32 hdr[3]; unsigned long long k[2]; u32 tail[3];
            if (fread(hdr, 4, 2, f) != 2 || hdr[0] != FXC_REC_MAGIC) break;
            if (fread(k, 8, 2, f) != 2 || fread(tail, 4, 3, f) != 3) break;
            if (tail[1] == 0 || tail[1] > (16u << 20)) break;
            ID3DBlob* b = NULL;
            if (FAILED(D3DCreateBlob(tail[1], &b)) || !b) break;
            if (fread(b->lpVtbl->GetBufferPointer(b), 1, tail[1], f) != tail[1]) { b->lpVtbl->Release(b); break; }
            if (fxc_fnv32((const u8*)b->lpVtbl->GetBufferPointer(b), tail[1]) != tail[2]) { nbad++; b->lpVtbl->Release(b); break; }
            fxc_put(k[0], k[1], hdr[1], tail[0], b);
            b->lpVtbl->Release(b);   /* the table holds its own reference */
            nload++; good_end = ftell(f);
        }
        fclose(f);
    }
    /* Reopen for append, cutting any torn tail so new records follow good ones. */
    f = fopen(path, "r+b"); if (!f) f = fopen(path, "w+b");
    if (f) {
        _chsize_s(_fileno(f), good_end);
        fseek(f, good_end, SEEK_SET);
        s_fxc_file = f;
    }
    fprintf(stderr, "[SHADERCACHE] %s: %d compiled shaders loaded%s%s%c", path, nload,
            nbad ? " (stopped at a corrupt record)" : "", s_fxc_file ? "" : " -- cannot write, read-only", 10);
    s_fxc_state = 1;
}
/* D3DCompile through the cache. kind: 0 = VS, 1 = PS (bit 8+ = flags). */
static HRESULT fxc_compile(const char* hlsl, size_t len, const char* name, const char* profile,
                           u32 kind, UINT flags, ID3DBlob** out, ID3DBlob** err)
{
    if (!s_fxc_state) fxc_load();
    const u32 fk = kind | (flags << 8);
    const unsigned long long k1 = fxc_fnv(hlsl, len, 1469598103934665603ull);
    const unsigned long long k2 = fxc_fnv(hlsl, len, 0x9E3779B97F4A7C15ull ^ fk);
    if (s_fxc_state > 0) {
        EnterCriticalSection(&s_fxc_cs);
        FxcEnt* e = fxc_slot(k1, k2, fk, (u32)len);
        ID3DBlob* hit = (e && e->b) ? e->b : NULL;
        if (hit) hit->lpVtbl->AddRef(hit);
        LeaveCriticalSection(&s_fxc_cs);
        if (hit) { *out = hit; if (err) *err = NULL; return S_OK; }
    }
    const unsigned long long t0 = ps3_qpc_us();
    HRESULT hr = D3DCompile(hlsl, len, name, NULL, NULL, "main", profile, flags, 0, out, err);
    const unsigned long long dt = ps3_qpc_us() - t0;
    if (kind == 0) { HITCH_ADD(g_hitch_vs_us, dt); HITCH_ADD(g_hitch_vs_n, 1); }
    else           { HITCH_ADD(g_hitch_ps_us, dt); HITCH_ADD(g_hitch_ps_n, 1); }
    if (SUCCEEDED(hr) && *out && s_fxc_state > 0) {
        ID3DBlob* b = *out;
        const u32 bl = (u32)b->lpVtbl->GetBufferSize(b);
        EnterCriticalSection(&s_fxc_cs);
        fxc_put(k1, k2, fk, (u32)len, b);
        if (s_fxc_file) {
            const u32 hdr[2] = { FXC_REC_MAGIC, fk };
            const unsigned long long k[2] = { k1, k2 };
            const u32 tail[3] = { (u32)len, bl, fxc_fnv32((const u8*)b->lpVtbl->GetBufferPointer(b), bl) };
            fwrite(hdr, 4, 2, s_fxc_file); fwrite(k, 8, 2, s_fxc_file); fwrite(tail, 4, 3, s_fxc_file);
            fwrite(b->lpVtbl->GetBufferPointer(b), 1, bl, s_fxc_file);
            fflush(s_fxc_file);
        }
        LeaveCriticalSection(&s_fxc_cs);
    }
    return hr;
}
/* [NHL local patch #128] Persistent PSO cache (ID3D12PipelineLibrary). Each
 * guest-FP pipeline costs the driver 2-16 ms to build (the #125 fetch VS made it
 * worse) and ~900 are built at every puck drop (play176: ~6 s of the stall).
 * The library keeps the driver's compiled pipelines across boots in
 * SHADER_CACHE_DIR/pso_lib.bin, keyed by a hash of the whole pipeline desc
 * (bytecode contents included). A library from another driver version is
 * rejected by the runtime at load and simply starts empty. Saved when new
 * pipelines have been stored and none for 3 s (at most every 20 s), from
 * render_frame. PSO_CACHE=0 disables. */
static ID3D12PipelineLibrary* s_pl = NULL;
static void* s_pl_blob = NULL;           /* must outlive s_pl */
static int s_pl_state = 0;               /* 0 untried, 1 on, -1 off */
static int s_pl_dirty = 0;
static ULONGLONG s_pl_last_store = 0, s_pl_last_save = 0;
static u64 s_pl_hits = 0, s_pl_miss = 0;
static void pl_path(char* p, size_t n)
{
    const char* dir = getenv("SHADER_CACHE_DIR"); if (!dir || !*dir) dir = "shadercache";
    CreateDirectoryA(dir, NULL);
    snprintf(p, n, "%s/pso_lib.bin", dir);
}
static void pl_init(void)
{
    s_pl_state = -1;
    const char* on = getenv("PSO_CACHE");
    if ((on && atoi(on) == 0) || !s_d3d.device) return;
    ID3D12Device1* d1 = NULL;
    if (FAILED(s_d3d.device->lpVtbl->QueryInterface(s_d3d.device, &IID_ID3D12Device1, (void**)&d1)) || !d1) return;
    char path[MAX_PATH]; pl_path(path, sizeof path);
    size_t len = 0;
    FILE* f = fopen(path, "rb");
    if (f) {
        fseek(f, 0, SEEK_END); long l = ftell(f); fseek(f, 0, SEEK_SET);
        if (l > 0 && (s_pl_blob = malloc((size_t)l)) != NULL && fread(s_pl_blob, 1, (size_t)l, f) == (size_t)l) len = (size_t)l;
        fclose(f);
    }
    HRESULT hr = len ? d1->lpVtbl->CreatePipelineLibrary(d1, s_pl_blob, len, &IID_ID3D12PipelineLibrary, (void**)&s_pl) : E_FAIL;
    if (FAILED(hr)) {   /* none, or written by another driver/runtime: start empty */
        if (len) fprintf(stderr, "[PSOCACHE] %s rejected (0x%08lX: driver changed?) -- starting empty%c", path, (unsigned long)hr, 10);
        s_pl = NULL; free(s_pl_blob); s_pl_blob = NULL; len = 0;
        hr = d1->lpVtbl->CreatePipelineLibrary(d1, NULL, 0, &IID_ID3D12PipelineLibrary, (void**)&s_pl);
    }
    d1->lpVtbl->Release(d1);
    if (FAILED(hr) || !s_pl) { s_pl = NULL; fprintf(stderr, "[PSOCACHE] pipeline libraries unsupported (0x%08lX)%c", (unsigned long)hr, 10); return; }
    fprintf(stderr, "[PSOCACHE] %s: %zu KB loaded%c", path, len / 1024, 10);
    s_pl_state = 1;
}
static void pl_save(int force)
{
    if (s_pl_state <= 0 || !s_pl_dirty) return;
    const ULONGLONG t = GetTickCount64();
    if (!force && (t - s_pl_last_store < 3000 || t - s_pl_last_save < 20000)) return;
    const SIZE_T sz = s_pl->lpVtbl->GetSerializedSize(s_pl);
    void* buf = sz ? malloc(sz) : NULL;
    if (!buf) return;
    if (SUCCEEDED(s_pl->lpVtbl->Serialize(s_pl, buf, sz))) {
        char path[MAX_PATH], tmp[MAX_PATH + 8]; pl_path(path, sizeof path);
        snprintf(tmp, sizeof tmp, "%s.tmp", path);
        FILE* f = fopen(tmp, "wb");
        if (f) {
            const int ok = fwrite(buf, 1, sz, f) == sz; fclose(f);
            if (ok && MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING)) {
                fprintf(stderr, "[PSOCACHE] saved %zu KB (hits %llu, built %llu this boot)%c",
                        (size_t)sz / 1024, s_pl_hits, s_pl_miss, 10);
                s_pl_dirty = 0;
            }
        }
    }
    free(buf);
    s_pl_last_save = t;
}
/* CreateGraphicsPipelineState through the library. */
static HRESULT pl_create(const D3D12_GRAPHICS_PIPELINE_STATE_DESC* pd, ID3D12PipelineState** out)
{
    if (!s_pl_state) pl_init();
    WCHAR name[40] = { 0 };
    if (s_pl_state > 0) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC k = *pd;
        unsigned long long h = 1469598103934665603ull;
        h = fxc_fnv((const char*)pd->VS.pShaderBytecode, pd->VS.BytecodeLength, h);
        h = fxc_fnv((const char*)pd->PS.pShaderBytecode, pd->PS.BytecodeLength, h);
        if (pd->InputLayout.NumElements)
            h = fxc_fnv((const char*)pd->InputLayout.pInputElementDescs,
                        pd->InputLayout.NumElements * sizeof(D3D12_INPUT_ELEMENT_DESC), h);
        k.pRootSignature = NULL; k.VS.pShaderBytecode = NULL; k.PS.pShaderBytecode = NULL;
        k.InputLayout.pInputElementDescs = NULL;
        h = fxc_fnv((const char*)&k, sizeof k, h);
        swprintf(name, 40, L"p%016llx", h);
        if (SUCCEEDED(s_pl->lpVtbl->LoadGraphicsPipeline(s_pl, name, pd, &IID_ID3D12PipelineState, (void**)out)) && *out) {
            s_pl_hits++; return S_OK; }
    }
    const unsigned long long t0 = ps3_qpc_us();
    HRESULT hr = s_d3d.device->lpVtbl->CreateGraphicsPipelineState(s_d3d.device, pd, &IID_ID3D12PipelineState, (void**)out);
    HITCH_ADD(g_hitch_pso_us, ps3_qpc_us() - t0); HITCH_ADD(g_hitch_pso_n, 1);
    if (SUCCEEDED(hr) && *out && s_pl_state > 0 &&
        SUCCEEDED(s_pl->lpVtbl->StorePipeline(s_pl, name, *out))) {
        s_pl_miss++; s_pl_dirty = 1; s_pl_last_store = GetTickCount64(); }
    return hr;
}

/* [NHL local patch #124] Parallel FXC. A shader nobody has compiled yet is
 * QUEUED instead of compiled on the spot, and the queue is compiled on
 * FXC_THREADS workers (default 8; FXC_THREADS=1 = the old serial path) at two
 * points in render_frame: on entry (vertex shaders queued while the FIFO was
 * recorded -- the draw keeps its vp_vs slot, whose blob is filled in here, and
 * nothing reads that blob before render_frame) and after the PSO prepass
 * (fragment shaders the prepass met; it runs in collect mode, so a miss
 * returns without a PSO and the exec loop's own vp_get_fp_pso then finds the
 * bytecode in the cache). Nothing is skipped or drawn with a stand-in: the
 * frame waits for its shaders, just ~8x less long. */
typedef struct { char* hlsl; size_t len; u32 kind; int slot; u32 vhash;
                 unsigned long long k1; ID3DBlob* out; HRESULT hr; char err[200]; } FxcJob;
static FxcJob* s_fxc_jobs = NULL;
static int s_fxc_njobs = 0, s_fxc_capjobs = 0;
static volatile LONG s_fxc_next = 0;
static int s_fxc_collect = 0;   /* vp_get_fp_pso: queue PS misses instead of compiling */
static int fxc_threads(void)
{
    static int n = -1;
    if (n < 0) { const char* e = getenv("FXC_THREADS"); n = e ? atoi(e) : 8; if (n < 1) n = 1; if (n > 32) n = 32; }
    return n;
}
/* Queue-and-batch needs the cache (the exec loop finds queued PS bytecode there). */
static int fxc_par_on(void)
{
    if (!s_fxc_state) fxc_load();
    return s_fxc_state > 0 && fxc_threads() > 1;
}
/* Cached bytecode for this text, or NULL (the caller then queues or compiles). */
static ID3DBlob* fxc_lookup(const char* hlsl, size_t len, u32 kind)
{
    if (!s_fxc_state) fxc_load();
    if (s_fxc_state <= 0) return NULL;
    const unsigned long long k1 = fxc_fnv(hlsl, len, 1469598103934665603ull);
    const unsigned long long k2 = fxc_fnv(hlsl, len, 0x9E3779B97F4A7C15ull ^ kind);
    EnterCriticalSection(&s_fxc_cs);
    FxcEnt* e = fxc_slot(k1, k2, kind, (u32)len);
    ID3DBlob* hit = (e && e->b) ? e->b : NULL;
    if (hit) hit->lpVtbl->AddRef(hit);
    LeaveCriticalSection(&s_fxc_cs);
    return hit;
}
/* [NHL local patch #225] Asynchronous shader compilation. #124 made the frame
 * wait for its new shaders on 8 threads; a fresh install still froze 4-5 s at
 * the first match start (~300 PS + 70 VS + 170 PSOs; the game's fence watchdog
 * fired). Now a shader needed only by a NON-essential draw goes to a persistent
 * pool of background compilers and that draw is skipped until its bytecode (and
 * PSO) exist -- it pops in a few frames late instead of the game freezing.
 * Essential draws keep the synchronous path: anything drawn to the display and
 * full-screen passes (<= 6 vertices: post chain, composites), and every non-
 * GPU-fetch draw. New PSOs for non-essential draws are also limited to
 * NHL_ASYNC_PSO_US (default 4000 us) of creation time per frame. Bytecode lands
 * in the #123 cache; finished VS are handed to their vp_vs slots at the start
 * of render_frame (async_drain). NHL_ASYNC_SHADERS=0 = the old behaviour. */
static int s_async_sync = 1;   /* the current request must be satisfied now */
static int async_on(void)
{
    static int v = -1;
    if (v < 0) { const char* e = getenv("NHL_ASYNC_SHADERS"); v = !(e && *e == '0'); }
    return v && fxc_par_on();
}
typedef struct { char* hlsl; size_t len; u32 kind; u32 vhash; unsigned long long k1;
                 ID3DBlob* out; volatile LONG state; } AsyncJob;   /* state -1 free, 0 queued, 1 busy, 2 done */
enum { ASYNC_SLOTS = 2048 };
static AsyncJob s_aj[ASYNC_SLOTS];
static CRITICAL_SECTION s_aj_cs;
static HANDLE s_aj_sem = NULL;
static int s_aj_init = 0;
static volatile LONG s_async_queued = 0, s_async_compiled = 0, s_async_skipped = 0, s_async_pso_deferred = 0;
static unsigned long long s_pso_frame_us = 0;   /* PSO creation time this frame (non-essential budget) */
static DWORD WINAPI async_worker(LPVOID unused)
{
    (void)unused;
    for (;;) {
        WaitForSingleObject(s_aj_sem, INFINITE);
        AsyncJob* j = NULL;
        EnterCriticalSection(&s_aj_cs);
        for (int i = 0; i < ASYNC_SLOTS; i++) if (s_aj[i].state == 0) { j = &s_aj[i]; j->state = 1; break; }
        LeaveCriticalSection(&s_aj_cs);
        if (!j) continue;
        ID3DBlob* out = NULL; ID3DBlob* e = NULL;
        fxc_compile(j->hlsl, j->len, j->kind ? "guest_fp" : "guest_vp2", j->kind ? "ps_5_0" : "vs_5_0",
                    j->kind, 0, &out, &e);   /* stores into the #123 cache */
        if (e) { if (!out) { static int n = 0; if (n++ < 4)
                     fprintf(stderr, "[ASYNC] %s compile FAIL: %.300s%c", j->kind ? "PS" : "VS",
                             (const char*)e->lpVtbl->GetBufferPointer(e), 10); }
                 e->lpVtbl->Release(e); }
        EnterCriticalSection(&s_aj_cs);
        j->out = out; j->state = 2;
        LeaveCriticalSection(&s_aj_cs);
        InterlockedIncrement(&s_async_compiled);
    }
}
/* Queue one compile in the background pool; 0 = not queued (caller goes synchronous). */
static int async_queue(const char* hlsl, size_t len, u32 kind, u32 vhash)
{
    if (!s_aj_init) {
        s_aj_init = 1;
        InitializeCriticalSection(&s_aj_cs);
        for (int i = 0; i < ASYNC_SLOTS; i++) s_aj[i].state = -1;
        s_aj_sem = CreateSemaphoreA(NULL, 0, ASYNC_SLOTS, NULL);
        const char* e = getenv("NHL_ASYNC_THREADS");
        int nt = e ? atoi(e) : 4; if (nt < 1) nt = 1; if (nt > 16) nt = 16;
        for (int i = 0; i < nt && s_aj_sem; i++) {
            HANDLE h = CreateThread(NULL, 4u << 20, async_worker, NULL, 0, NULL);
            if (h) { SetThreadPriority(h, THREAD_PRIORITY_BELOW_NORMAL); CloseHandle(h); }
        }
        fprintf(stderr, "[ASYNC] background shader compilation on (%d threads)%c", nt, 10);
    }
    if (!s_aj_sem) return 0;
    const unsigned long long k1 = fxc_fnv(hlsl, len, 1469598103934665603ull);
    int freei = -1, dup = 0;
    EnterCriticalSection(&s_aj_cs);
    for (int i = 0; i < ASYNC_SLOTS; i++) {
        if (s_aj[i].state < 0) { if (freei < 0) freei = i; continue; }
        if (s_aj[i].kind == kind && s_aj[i].k1 == k1 && s_aj[i].len == len) { dup = 1; break; }
    }
    char* copy = (!dup && freei >= 0) ? (char*)malloc(len + 1) : NULL;
    if (copy) {
        memcpy(copy, hlsl, len); copy[len] = 0;
        AsyncJob* j = &s_aj[freei];
        j->hlsl = copy; j->len = len; j->kind = kind; j->vhash = vhash; j->k1 = k1; j->out = NULL;
        j->state = 0;
    }
    LeaveCriticalSection(&s_aj_cs);
    if (copy) { InterlockedIncrement(&s_async_queued); ReleaseSemaphore(s_aj_sem, 1, NULL); }
    return dup || copy;
}
/* [NHL local patch #227] Waiting draws skip the decompile. A non-essential draw whose
 * PS is still compiling in the background (or whose PSO the per-frame budget put off)
 * went through the FULL decompile + HLSL edits + text hashing on every call -- twice a
 * frame (prepass + exec loop), for every waiting draw: at puck drop ~2700 draws, one
 * 522 ms frame (#226: prepass 333 + record 188 ms, GPU waits 0). The pipeline key of
 * such a draw is remembered with its final HLSL's #123 cache keys; until the bytecode
 * exists (or the PSO budget has room) later calls return at once. Entries expire after
 * FPP_TTL frames so a failed compile is retried as before. NHL_FP_PENDMEMO=0 = off. */
typedef struct { u32 uhash, vs_hash, blend, rtfmt, exp32, cmask, cull, cube_mask, stencil, gen; int vs_idx, nrt; } FppKey;
typedef struct { FppKey k; unsigned long long k1, k2; u32 len, frame, used; } FppEnt;
enum { FPP_N = 4096, FPP_PROBE = 8, FPP_TTL = 30 };
static FppEnt s_fpp[FPP_N];
static volatile LONG s_fpp_hits = 0;
static int fpp_enabled(void)
{
    static int v = -1;
    if (v < 0) { const char* e = getenv("NHL_FP_PENDMEMO"); v = !(e && *e == '0'); }
    return v;
}
static int fpp_live(const FppEnt* e) { return e->used && (u32)s_d3d.frame_count - e->frame < FPP_TTL; }
static FppEnt* fpp_find(const FppKey* k)
{
    const u32 h = fxc_fnv32((const u8*)k, sizeof *k);
    for (u32 n = 0, i = h & (FPP_N - 1); n < FPP_PROBE; n++, i = (i + 1) & (FPP_N - 1))
        if (fpp_live(&s_fpp[i]) && !memcmp(&s_fpp[i].k, k, sizeof *k)) return &s_fpp[i];
    return NULL;
}
static void fpp_store(const FppKey* k, const char* hlsl, size_t len)
{
    const u32 h = fxc_fnv32((const u8*)k, sizeof *k);
    FppEnt* e = &s_fpp[h & (FPP_N - 1)];   /* window full: overwrite the home slot */
    for (u32 n = 0, i = h & (FPP_N - 1); n < FPP_PROBE; n++, i = (i + 1) & (FPP_N - 1))
        if (!fpp_live(&s_fpp[i]) || !memcmp(&s_fpp[i].k, k, sizeof *k)) { e = &s_fpp[i]; break; }
    e->k = *k;
    e->k1 = fxc_fnv(hlsl, len, 1469598103934665603ull);   /* = fxc_lookup's keys for a PS */
    e->k2 = fxc_fnv(hlsl, len, 0x9E3779B97F4A7C15ull ^ 1u);
    e->len = (u32)len; e->frame = (u32)s_d3d.frame_count; e->used = 1;
}
/* the #123 cache holds this PS bytecode */
static int fxc_has(unsigned long long k1, unsigned long long k2, u32 kind, u32 len)
{
    if (s_fxc_state <= 0) return 0;
    EnterCriticalSection(&s_fxc_cs);
    const FxcEnt* e = fxc_slot(k1, k2, kind, len);
    const int r = e && e->b;
    LeaveCriticalSection(&s_fxc_cs);
    return r;
}
static unsigned long long async_pso_budget(void)   /* #225 NHL_ASYNC_PSO_US */
{
    static long long budget = -1;
    if (budget < 0) { const char* e = getenv("NHL_ASYNC_PSO_US"); budget = e ? atoll(e) : 4000; }
    return (unsigned long long)budget;
}
/* render_frame entry: finished vertex shaders into every slot waiting for that
 * program; free finished jobs (pixel shaders are found through the cache). */
static void async_drain(void)
{
    s_pso_frame_us = 0;
    if (!s_aj_init) return;
    EnterCriticalSection(&s_aj_cs);
    for (int i = 0; i < ASYNC_SLOTS; i++) {
        AsyncJob* j = &s_aj[i];
        if (j->state != 2) continue;
        if (j->kind == 0 && j->out)
            for (int s = 0; s < s_d3d.vp_vs_n; s++)
                if (s_d3d.vp_vs[s].hash == j->vhash && !s_d3d.vp_vs[s].vs) {
                    j->out->lpVtbl->AddRef(j->out); s_d3d.vp_vs[s].vs = j->out; }
        if (j->out) j->out->lpVtbl->Release(j->out);
        free(j->hlsl); j->hlsl = NULL; j->out = NULL; j->state = -1;
    }
    LeaveCriticalSection(&s_aj_cs);
    { static ULONGLONG t0 = 0; const ULONGLONG t = GetTickCount64(); if (!t0) t0 = t;
      if (t - t0 >= 5000) {
          if (s_async_queued || s_async_skipped || s_async_pso_deferred)
              fprintf(stderr, "[ASYNC] 5 s: %ld queued, %ld compiled, %ld draws waited, %ld PSOs deferred, %ld decompiles skipped%c",
                      (long)s_async_queued, (long)s_async_compiled, (long)s_async_skipped, (long)s_async_pso_deferred,
                      (long)s_fpp_hits, 10);
          s_async_queued = s_async_compiled = s_async_skipped = s_async_pso_deferred = s_fpp_hits = 0; t0 = t; } }
}
/* [NHL local patch #229] An essential draw (see rt_oneoff) whose own VS sits in the
 * background pool must not be skipped: take the job and compile it here if no worker has
 * started it, else wait for the worker; then hand the bytecode to every waiting slot. */
static volatile LONG s_async_vs_waits = 0;
static void async_wait_vs(u32 vhash)
{
    if (!s_aj_init) return;
    const unsigned long long t0 = ps3_qpc_us();
    for (;;) {
        AsyncJob* j = NULL; int claimed = 0;
        EnterCriticalSection(&s_aj_cs);
        for (int i = 0; i < ASYNC_SLOTS; i++)
            if (s_aj[i].state >= 0 && s_aj[i].kind == 0 && s_aj[i].vhash == vhash) { j = &s_aj[i]; break; }
        if (j && j->state == 0) { j->state = 1; claimed = 1; }
        if (j && j->state == 2) {   /* done: attach (async_drain frees the job later) */
            if (j->out)
                for (int s = 0; s < s_d3d.vp_vs_n; s++)
                    if (s_d3d.vp_vs[s].hash == vhash && !s_d3d.vp_vs[s].vs) {
                        j->out->lpVtbl->AddRef(j->out); s_d3d.vp_vs[s].vs = j->out; }
            j = NULL;
        }
        LeaveCriticalSection(&s_aj_cs);
        if (!j) break;   /* attached, or no such job (failed / already drained) */
        if (claimed) {
            ID3DBlob* out = NULL; ID3DBlob* e = NULL;
            fxc_compile(j->hlsl, j->len, "guest_vp2", "vs_5_0", 0, 0, &out, &e);
            if (e) e->lpVtbl->Release(e);
            EnterCriticalSection(&s_aj_cs);
            j->out = out; j->state = 2;
            LeaveCriticalSection(&s_aj_cs);
            InterlockedIncrement(&s_async_compiled);
            continue;   /* attach on the next pass */
        }
        if (ps3_qpc_us() - t0 > 5000000ull) break;   /* a worker stuck 5 s: give up (draw falls back) */
        Sleep(1);
    }
    if (InterlockedIncrement(&s_async_vs_waits) <= 40)
        fprintf(stderr, "[ASYNCWAIT] essential draw took VS 0x%08X from the pool (%.1f ms)%c",
                vhash, (ps3_qpc_us() - t0) / 1000.0, 10);
}
/* NHL_ASYNC_ALL=1 (test): treat every GPU-fetch draw as non-essential, to exercise
 * the background path where nothing is offscreen (the title). */
static int async_all(void)
{
    static int v = -1;
    if (v < 0) v = getenv("NHL_ASYNC_ALL") ? 1 : 0;
    return v;
}
/* [#225b] Largest VS (RSX instructions) still treated as a full-screen-pass VS. */
static int async_tiny_vs(void)
{
    static int v = -1;
    if (v < 0) { const char* e = getenv("NHL_ASYNC_TINY_VS"); v = e ? atoi(e) : 16; }
    return v;
}
static int s_rec_small = 0;   /* the GPU-fetch draw being recorded has <= 6 vertices */
static int rt_oneoff(u32 rt);   /* #229, defined with the write-back bookkeeping */
/* A draw that must not be skipped: on the display, or a full-screen pass
 * (<= 6 vertices with a tiny VS -- #225b: small scene quads with big VSs wait),
 * or into a one-off target (#229: equipment composites, names, numbers). */
static int draw_essential(const D3D12DrawRecord* dr)
{
    if (async_all()) return !dr->gpu_vtx;
    if (dr->rt_off == 0 || !dr->gpu_vtx) return 1;
    if (rt_oneoff(dr->rt_off)) return 1;   /* #229 */
    if (dr->vertex_count > 6) return 0;
    const int v = dr->vs_idx;
    return !(v >= 0 && v < s_d3d.vp_vs_n) || s_d3d.vp_vs[v].ni <= async_tiny_vs();
}

static void fxc_queue(const char* hlsl, size_t len, u32 kind, int slot, u32 vhash)
{
    if (async_on() && !s_async_sync && async_queue(hlsl, len, kind, vhash)) return;   /* #225 */
    const unsigned long long k1 = fxc_fnv(hlsl, len, 1469598103934665603ull);
    EnterCriticalSection(&s_fxc_cs);
    int dup = 0;
    if (kind == 1)   /* the same PS text often comes from several draws */
        for (int i = 0; i < s_fxc_njobs; i++)
            if (s_fxc_jobs[i].kind == 1 && s_fxc_jobs[i].k1 == k1 && s_fxc_jobs[i].len == len) { dup = 1; break; }
    if (!dup) {
        if (s_fxc_njobs == s_fxc_capjobs) {
            const int nc = s_fxc_capjobs ? s_fxc_capjobs * 2 : 256;
            FxcJob* nj = (FxcJob*)realloc(s_fxc_jobs, (size_t)nc * sizeof(FxcJob));
            if (nj) { s_fxc_jobs = nj; s_fxc_capjobs = nc; }
        }
        char* copy = (s_fxc_njobs < s_fxc_capjobs) ? (char*)malloc(len + 1) : NULL;
        if (copy) {
            memcpy(copy, hlsl, len); copy[len] = 0;
            FxcJob* j = &s_fxc_jobs[s_fxc_njobs++];
            memset(j, 0, sizeof *j);
            j->hlsl = copy; j->len = len; j->kind = kind; j->slot = slot; j->vhash = vhash; j->k1 = k1;
        }
    }
    LeaveCriticalSection(&s_fxc_cs);
}
static DWORD WINAPI fxc_worker(LPVOID unused)
{
    (void)unused;
    for (;;) {
        const LONG i = InterlockedIncrement(&s_fxc_next) - 1;
        if (i >= s_fxc_njobs) break;
        FxcJob* j = &s_fxc_jobs[i];
        ID3DBlob* e = NULL;
        j->hr = fxc_compile(j->hlsl, j->len, j->kind ? "guest_fp" : "guest_vp2",
                            j->kind ? "ps_5_0" : "vs_5_0", j->kind, 0, &j->out, &e);
        j->err[0] = 0;
        if (e) { if (!j->out) snprintf(j->err, sizeof j->err, "%s", (const char*)e->lpVtbl->GetBufferPointer(e));
                 e->lpVtbl->Release(e); }
    }
    return 0;
}
static void fxc_run_jobs(void)
{
    /* Queueing and running both happen on the RSX ticker thread (FIFO record
     * and render_frame), so the job array is not touched concurrently; the
     * workers only share the cache, which fxc_compile locks. */
    if (!s_fxc_njobs) return;
    const unsigned long long t0 = ps3_qpc_us();
    s_fxc_next = 0;
    int nt = fxc_threads(); if (nt > s_fxc_njobs) nt = s_fxc_njobs;
    HANDLE th[32]; int nth = 0;
    for (int i = 1; i < nt; i++) {
        th[nth] = CreateThread(NULL, 4u << 20, fxc_worker, NULL, 0, NULL);
        if (th[nth]) nth++;
    }
    fxc_worker(NULL);   /* this thread works too */
    if (nth) WaitForMultipleObjects((DWORD)nth, th, TRUE, INFINITE);
    for (int i = 0; i < nth; i++) CloseHandle(th[i]);
    int nvs = 0, nps = 0, nfail = 0;
    for (int i = 0; i < s_fxc_njobs; i++) {
        FxcJob* j = &s_fxc_jobs[i];
        if (j->kind == 0) {
            nvs++;
            /* the slot still holds this program (no eviction since the queue) */
            if (j->out && j->slot >= 0 && j->slot < s_d3d.vp_vs_n &&
                s_d3d.vp_vs[j->slot].hash == j->vhash && !s_d3d.vp_vs[j->slot].vs) {
                s_d3d.vp_vs[j->slot].vs = j->out; j->out = NULL;
            } else if (!j->out) {
                nfail++;
                { static int _e = 0; if (_e++ < 4) printf("[VP] per-draw VS compile FAIL (hash=0x%08X): %s\n", j->vhash, j->err); }
            }
        } else { nps++; if (!j->out) { nfail++;
            /* [#176] PS errors were silent: play238's s16 sampler failed every compile */
            { static int _e = 0; if (_e++ < 4) fprintf(stderr, "[FP] PS compile FAIL: %.400s%c", j->err, 10); } } }
        if (j->out) j->out->lpVtbl->Release(j->out);
        free(j->hlsl);
    }
    { static int n = 0; if (n++ < 200)
        fprintf(stderr, "[FXCPAR] %d VS + %d PS compiled on %d threads in %llu ms (%d failed)%c",
                nvs, nps, nt, (ps3_qpc_us() - t0) / 1000ull, nfail, 10); }
    s_fxc_njobs = 0;
}
static int perf_on(void)
{
    static int v = -1;
    if (v < 0) { const char* e = getenv("PERF"); v = e ? atoi(e) : 0; }
    return v;
}
/* DBG_LOCK: running clip-space centroid of the tracked mesh (RSX_DBG_VTX's
 * texture), so the debug camera can follow it. The ducks are driven by the
 * physics sim and drift every frame, so a fixed DBG_CENTER loses them as soon
 * as the magnification is high enough to make one recognisable. */
static float s_lock_x = 0.0f, s_lock_y = 0.0f;
static int   s_lock_valid = 0;
/* Accumulated over one frame, published at the frame boundary. Averaging as the
 * frame is recorded makes the aim wander while it is being used, which at high
 * magnification walks the target off-screen entirely. */
static double s_lock_sx = 0.0, s_lock_sy = 0.0;
static u32    s_lock_n  = 0;

/* ---------------------------------------------------------------------------
 * Win32 window
 * -----------------------------------------------------------------------*/

/* [NHL local patch #107] F9 in the game window: rsxcap the next 4 consecutive
 * presents (frame_50.., then frame_54.., ...) -- the user captures exactly the
 * moment on screen instead of guessing a frame number for RSX_CAP. */
static volatile LONG s_cap_hot_req = 0;
static volatile LONG s_shadow_lit = -1;   /* [#189b] shadow_lit_bind state (#188, NHL_SHADOW_LIT=1) */
static volatile LONG s_shadows_live = -1; /* [#190b] shadows_on() state; F5 (NHL_LIVEKEYS=1) toggles */
static volatile LONG s_texgamma_live = 0;  /* [#214 diag] F4 (NHL_LIVEKEYS=1): texture-gamma sRGB view mode 0/1/2 */
static volatile LONG s_shine_live = 0;     /* [#216 diag] F3 (NHL_LIVEKEYS=1): blank one texture role on player draws, 0..5 */
static const char* const s_shine_names[6] = {
    "all textures as normal",
    "NO 2D environment reflection map (128x128 DXT5, 1 mip)",
    "NO bump/spot detail map (256x256 DXT5, mipped)",
    "NO cube reflection maps",
    "NO shared 512x512 DXT5 gamma texture",
    "NO 2048x2048 DXT5 normal map (the green texture)" };
/* [#112] CAP_HOT_N=<n>: presents per F9 burst (default 4, max 200). */
static LONG cap_hot_n(void)
{
    static LONG n = 0;
    if (!n) { const char* e = getenv("CAP_HOT_N"); n = e ? atol(e) : 4; if (n < 1) n = 1; if (n > 200) n = 200; }
    return n;
}
/* [#199] the same capture burst, requested by game-side probes (nhl_hle.cpp). */
void rsx_d3d12_cap_request(void)
{
    InterlockedExchange(&s_cap_hot_req, cap_hot_n());
}
/* [NHL local patch #108] live A/B toggles for the user's run (no restart):
 * F6 cycles face culling 0 = guest, 1 = winding flipped, 2 = culling off;
 * F7 toggles the depth test off. Both act through the PSO key, so the
 * variants are built on demand and the next frame shows the result. */
static volatile LONG s_live_cull = 0;
static volatile LONG s_live_depth = 0;

/* [#163] scaled present / fullscreen state, defined with the blit below */
static int s_sc_on; static int s_fs; static volatile LONG s_sc_resize;
static void sc_toggle_fullscreen(HWND hwnd);

static LRESULT CALLBACK d3d12_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CLOSE:
        s_d3d.window_closed = 1;
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_SIZE:     /* [#163] the swap chain follows the client area */
        InterlockedExchange(&s_sc_resize, 1);
        break;
    case WM_SETCURSOR:   /* [#163] no cursor over the fullscreen picture */
        if (s_fs && LOWORD(lp) == HTCLIENT) { SetCursor(NULL); return TRUE; }
        break;
    case WM_HOTKEY:   /* [#120] CAP_GLOBAL_F9=1: F9 works without window focus */
        if (wp == 0x4E48) InterlockedExchange(&s_cap_hot_req, cap_hot_n());
        if (wp == 0x4E4A) {   /* #190b F5: rendered shadow/depth surfaces (#190) on/off */
            const LONG v = (s_shadows_live > 0) ? 0 : 1;   /* -1 (unread) counts as on */
            InterlockedExchange(&s_shadows_live, (s_shadows_live < 0) ? 0 : v);
            fprintf(stderr, "[LIVEKEY] F5 depth/shadow surfaces (#190) -> %s%c", v ? "ON" : "OFF (dropped, #113)", 10); }
        if (wp == 0x4E4C) {   /* #216 F3: blank one texture role on player draws */
            const LONG m = (s_shine_live + 1) % 6;
            InterlockedExchange(&s_shine_live, m);
            fprintf(stderr, "[LIVEKEY] F3 shine test -> mode %ld (%s)%c", (long)m, s_shine_names[m], 10); }
        if (wp == 0x4E4B) {   /* #214 F4: texture-gamma sRGB view mode */
            const LONG m = (s_texgamma_live + 1) % 3;
            InterlockedExchange(&s_texgamma_live, m);
            fprintf(stderr, "[LIVEKEY] F4 texture gamma -> mode %ld (%s)%c", (long)m,
                    m == 0 ? "sRGB views as normal" : m == 1 ? "NO sRGB view for DXT5 gamma textures" : "NO sRGB view for any gamma texture", 10); }
        if (wp == 0x4E49) {   /* #122 F8; the message time = when the key was pressed */
            g_hitch_mark_tick = (long)GetMessageTime(); InterlockedIncrement(&g_hitch_mark_req); }
        return 0;
    case WM_SYSKEYDOWN:   /* F10 arrives as a system key (it would open the window menu) */
        if (wp == VK_F10) return 0;   /* (#251: it toggled FXAA, now removed) */
        break;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) {
            s_d3d.window_closed = 1;
            DestroyWindow(hwnd);
        }
        if (wp == VK_F11 && !(lp & (1 << 30)) && s_sc_on) sc_toggle_fullscreen(hwnd);   /* #163 */
        if (wp == VK_F9 && !(lp & (1 << 30)))   /* ignore auto-repeat */
            InterlockedExchange(&s_cap_hot_req, cap_hot_n());
        if (wp == VK_F8 && !(lp & (1 << 30))) {   /* #122 slowdown marker (focused) */
            g_hitch_mark_tick = (long)GetMessageTime(); InterlockedIncrement(&g_hitch_mark_req); }
        if (wp == VK_F6 && !(lp & (1 << 30))) {
            static const char* names[3] = { "guest", "FLIPPED winding", "OFF" };
            const LONG m = (s_live_cull + 1) % 3;
            InterlockedExchange(&s_live_cull, m);
            fprintf(stderr, "[LIVEKEY] F6 culling -> %s%c", names[m], 10);
        }
        if (wp == VK_F7 && !(lp & (1 << 30))) {
            const LONG m = !s_live_depth;
            InterlockedExchange(&s_live_depth, m);
            fprintf(stderr, "[LIVEKEY] F7 depth test -> %s%c", m ? "OFF" : "guest", 10);
        }
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static HWND create_window(u32 width, u32 height, const char* title)
{
    WNDCLASSEXA wc = {0};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = d3d12_wndproc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "ps3recomp_d3d12";
    RegisterClassExA(&wc);

    RECT wr = {0, 0, (LONG)width, (LONG)height};
    AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);

    /* Keep the display awake (see rsx_null_backend.c). */
    SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED);
    return CreateWindowExA(
        0, "ps3recomp_d3d12",
        title ? title : "ps3recomp (D3D12)",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT,
        wr.right - wr.left, wr.bottom - wr.top,
        NULL, NULL, GetModuleHandle(NULL), NULL);
}

/* ---------------------------------------------------------------------------
 * [NHL local patch #163] Scaled present, borderless fullscreen (F11) and VRR.
 *
 * The guest display surfaces stay render_targets[] at the guest size (every
 * copy, readback and capture in this file assumes that), but they are now our
 * own textures; the swap chain is sized to the window's client area and gets
 * them through one letterboxed linear blit just before Present. In borderless
 * fullscreen, Present uses sync interval 0 + DXGI_PRESENT_ALLOW_TEARING, which
 * is what FreeSync / G-SYNC / Adaptive-Sync need to refresh the display when
 * each frame arrives: at ~60 guest flips on a 165 Hz screen, vsync showed each
 * frame for 2 or 3 refreshes (judder). Knobs: NHL_FULLSCREEN=1 (start
 * fullscreen), NHL_VRR=0 (keep vsync in fullscreen: fixed-refresh screens would
 * tear), NHL_DIRECT_PRESENT=1 (the old swap-chain-is-the-surface path).
 * -----------------------------------------------------------------------*/
static void wait_for_gpu(void);
static int s_sc_on;                         /* scaled present active */
static int s_sc_tearing;                    /* DXGI tearing supported -> swap chain flag set */
static ID3D12Resource* s_sc_buf[FRAME_COUNT];
static ID3D12DescriptorHeap* s_sc_rtv_heap; /* swap-chain buffer RTVs */
static ID3D12DescriptorHeap* s_sc_srv_heap; /* shader-visible SRVs of render_targets[] */
static ID3D12RootSignature* s_sc_rs;
static ID3D12PipelineState* s_sc_pso;
static u32 s_sc_w, s_sc_h;                  /* swap chain buffer size */
static volatile LONG s_sc_resize;           /* WM_SIZE seen: resize before the next blit */
static int s_fs;                            /* borderless fullscreen now */
static LONG s_fs_style; static RECT s_fs_rect;

static UINT sc_flags(void) { return s_sc_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u; }

static void sc_client_size(u32* w, u32* h)
{
    RECT r = {0}; GetClientRect(s_d3d.hwnd, &r);
    *w = (u32)(r.right - r.left); *h = (u32)(r.bottom - r.top);
    if (*w < 8) *w = 8; if (*h < 8) *h = 8;   /* minimised: keep a valid size */
}

static int sc_get_buffers(void)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h;
    s_sc_rtv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_sc_rtv_heap, &h);
    for (u32 i = 0; i < FRAME_COUNT; i++) {
        if (FAILED(s_d3d.swap_chain->lpVtbl->GetBuffer(s_d3d.swap_chain, i, &IID_ID3D12Resource, (void**)&s_sc_buf[i])))
            return -1;
        s_d3d.device->lpVtbl->CreateRenderTargetView(s_d3d.device, s_sc_buf[i], NULL, h);
        h.ptr += s_d3d.rtv_descriptor_size;
    }
    return 0;
}

/* [NHL local patch #247] FXAA present pass: removed by #251 (MSAA #249 covers the scene edges; FXAA
 * smeared text/UI, user 2026-10-28). #250 had already removed its bicubic + sharpen upscale. */

/* own guest-size surfaces (in render_targets[] + rtv_heap, as the swap-chain
 * buffers were), the swap-chain RTVs, the SRVs and the blit pipeline */
static int sc_init(u32 width, u32 height)
{
    ID3D12Device* dev = s_d3d.device;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv;
    s_d3d.rtv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.rtv_heap, &rtv);
    D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td = {0};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; td.Width = width; td.Height = height;
    td.DepthOrArraySize = 1; td.MipLevels = 1; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1; td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    for (u32 i = 0; i < FRAME_COUNT; i++) {
        /* PRESENT (= COMMON) like a fresh swap-chain buffer: the frame code's barriers are unchanged */
        if (FAILED(dev->lpVtbl->CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &td,
                D3D12_RESOURCE_STATE_PRESENT, NULL, &IID_ID3D12Resource, (void**)&s_d3d.render_targets[i])))
            return -1;
        dev->lpVtbl->CreateRenderTargetView(dev, s_d3d.render_targets[i], NULL, rtv);
        rtv.ptr += s_d3d.rtv_descriptor_size;
    }
    D3D12_DESCRIPTOR_HEAP_DESC hd = {0};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = FRAME_COUNT;
    if (FAILED(dev->lpVtbl->CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void**)&s_sc_rtv_heap))) return -1;
    if (sc_get_buffers() != 0) return -1;
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hd.NumDescriptors = FRAME_COUNT;
    if (FAILED(dev->lpVtbl->CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void**)&s_sc_srv_heap))) return -1;
    {   D3D12_CPU_DESCRIPTOR_HANDLE h;
        s_sc_srv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_sc_srv_heap, &h);
        const UINT inc = dev->lpVtbl->GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        for (u32 i = 0; i < FRAME_COUNT; i++, h.ptr += inc)
            dev->lpVtbl->CreateShaderResourceView(dev, s_d3d.render_targets[i], NULL, h); }
    {   D3D12_DESCRIPTOR_RANGE range = {0};
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; range.NumDescriptors = 1;
        D3D12_ROOT_PARAMETER rp = {0};
        rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rp.DescriptorTable.NumDescriptorRanges = 1; rp.DescriptorTable.pDescriptorRanges = &range;
        rp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_STATIC_SAMPLER_DESC ss = {0};
        ss.Filter = getenv("NHL_SCALE_POINT") ? D3D12_FILTER_MIN_MAG_MIP_POINT : D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        ss.AddressU = ss.AddressV = ss.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        ss.MaxLOD = D3D12_FLOAT32_MAX; ss.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC rd = {0};
        rd.NumParameters = 1; rd.pParameters = &rp; rd.NumStaticSamplers = 1; rd.pStaticSamplers = &ss;
        ID3DBlob* sig = NULL; ID3DBlob* err = NULL;
        if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err))) {
            if (err) err->lpVtbl->Release(err); return -1; }
        HRESULT hr = dev->lpVtbl->CreateRootSignature(dev, 0, sig->lpVtbl->GetBufferPointer(sig),
                         sig->lpVtbl->GetBufferSize(sig), &IID_ID3D12RootSignature, (void**)&s_sc_rs);
        sig->lpVtbl->Release(sig);
        if (FAILED(hr)) return -1; }
    {   static const char vs[] =
            "struct O { float4 p : SV_Position; float2 uv : TEXCOORD0; };\n"
            "O main(uint id : SV_VertexID) { O o; o.uv = float2((id << 1) & 2, id & 2);\n"
            "  o.p = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1); return o; }\n";
        static const char ps[] =
            "Texture2D t : register(t0); SamplerState s : register(s0);\n"
            "float4 main(float4 p : SV_Position, float2 uv : TEXCOORD0) : SV_Target { return float4(t.Sample(s, uv).rgb, 1); }\n";
        ID3DBlob* vb = NULL; ID3DBlob* pb = NULL; ID3DBlob* e = NULL;
        if (FAILED(D3DCompile(vs, sizeof(vs) - 1, "sc_vs", NULL, NULL, "main", "vs_5_0", 0, 0, &vb, &e)) ||
            FAILED(D3DCompile(ps, sizeof(ps) - 1, "sc_ps", NULL, NULL, "main", "ps_5_0", 0, 0, &pb, &e))) {
            if (e) { printf("[SCALE] blit shader: %s\n", (const char*)e->lpVtbl->GetBufferPointer(e)); e->lpVtbl->Release(e); }
            return -1; }
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {0};
        pd.pRootSignature = s_sc_rs;
        pd.VS.pShaderBytecode = vb->lpVtbl->GetBufferPointer(vb); pd.VS.BytecodeLength = vb->lpVtbl->GetBufferSize(vb);
        pd.PS.pShaderBytecode = pb->lpVtbl->GetBufferPointer(pb); pd.PS.BytecodeLength = pb->lpVtbl->GetBufferSize(pb);
        pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pd.SampleMask = 0xFFFFFFFFu;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.RasterizerState.DepthClipEnable = TRUE;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1; pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM; pd.SampleDesc.Count = 1;
        HRESULT hr = dev->lpVtbl->CreateGraphicsPipelineState(dev, &pd, &IID_ID3D12PipelineState, (void**)&s_sc_pso);
        vb->lpVtbl->Release(vb); pb->lpVtbl->Release(pb);
        if (FAILED(hr)) return -1; }
    return 0;
}

/* swap chain to the window's current client size (ticker thread, GPU idle first) */
static void sc_resize(void)
{
    u32 w, h; sc_client_size(&w, &h);
    if (w == s_sc_w && h == s_sc_h) return;
    wait_for_gpu();
    for (u32 i = 0; i < FRAME_COUNT; i++) if (s_sc_buf[i]) { s_sc_buf[i]->lpVtbl->Release(s_sc_buf[i]); s_sc_buf[i] = NULL; }
    HRESULT hr = s_d3d.swap_chain->lpVtbl->ResizeBuffers(s_d3d.swap_chain, FRAME_COUNT, w, h,
                                                          DXGI_FORMAT_R8G8B8A8_UNORM, sc_flags());
    if (FAILED(hr)) fprintf(stderr, "[SCALE] ResizeBuffers %ux%u failed 0x%08lX%c", w, h, (long)hr, 10);
    else { s_sc_w = w; s_sc_h = h; }
    sc_get_buffers();
    fprintf(stderr, "[SCALE] swap chain %ux%u%s%c", s_sc_w, s_sc_h, s_fs ? " (fullscreen)" : "", 10);
}

/* F11 / NHL_FULLSCREEN: borderless window over the whole monitor, and back (window thread) */
static void sc_toggle_fullscreen(HWND hwnd)
{
    if (!s_fs) {
        s_fs_style = GetWindowLongA(hwnd, GWL_STYLE); GetWindowRect(hwnd, &s_fs_rect);
        MONITORINFO mi = {0}; mi.cbSize = sizeof(mi);
        GetMonitorInfoA(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
        s_fs = 1;
        SetWindowLongA(hwnd, GWL_STYLE, (LONG)(WS_POPUP | WS_VISIBLE));
        SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
    } else {
        s_fs = 0;
        SetWindowLongA(hwnd, GWL_STYLE, s_fs_style);
        SetWindowPos(hwnd, HWND_NOTOPMOST, s_fs_rect.left, s_fs_rect.top, s_fs_rect.right - s_fs_rect.left,
                     s_fs_rect.bottom - s_fs_rect.top, SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
    }
    InterlockedExchange(&s_sc_resize, 1);
}

/* #296: the save list panel over the finished frame (cellSaveData.c draws it).
 * Back buffer RENDER_TARGET -> COPY_DEST here. 1 = panel copied. */
extern const u32* savedata_picker_frame(u32* w, u32* h);
static ID3D12Resource* s_ov_buf[FRAME_COUNT];
static UINT64 s_ov_cap;
static int sc_overlay(ID3D12GraphicsCommandList* cl, u32 fi, ID3D12Resource* bb)
{
    u32 pw, ph;
    const u32* px = savedata_picker_frame(&pw, &ph);
    if (!px) return 0;
    u32 sc = s_sc_h / ph;
    if (sc > s_sc_w / pw) sc = s_sc_w / pw;
    if (sc < 1) sc = 1;
    const u32 w = pw * sc < s_sc_w ? pw * sc : s_sc_w, h = ph * sc < s_sc_h ? ph * sc : s_sc_h;
    const u32 pitch = (w * 4 + 255) & ~255u;
    const UINT64 need = (UINT64)pitch * h;
    if (need > s_ov_cap) {
        wait_for_gpu();
        D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC rd = {0};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; rd.Width = need; rd.Height = 1;
        rd.DepthOrArraySize = 1; rd.MipLevels = 1; rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        s_ov_cap = need;
        for (u32 i = 0; i < FRAME_COUNT; i++) {
            if (s_ov_buf[i]) { s_ov_buf[i]->lpVtbl->Release(s_ov_buf[i]); s_ov_buf[i] = NULL; }
            if (FAILED(s_d3d.device->lpVtbl->CreateCommittedResource(s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE,
                    &rd, D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource, (void**)&s_ov_buf[i]))) {
                s_ov_buf[i] = NULL; s_ov_cap = 0;
            }
        }
    }
    ID3D12Resource* buf = s_ov_buf[fi];
    unsigned char* dst;
    if (!buf || FAILED(buf->lpVtbl->Map(buf, 0, NULL, (void**)&dst))) return 0;
    for (u32 y = 0; y < h; y++) {
        const u32* s = px + (y / sc) * pw;
        u32* d = (u32*)(dst + (UINT64)y * pitch);
        for (u32 x = 0; x < w; x++) d[x] = s[x / sc];
    }
    buf->lpVtbl->Unmap(buf, 0, NULL);
    D3D12_RESOURCE_BARRIER b = {0};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = bb;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    cl->lpVtbl->ResourceBarrier(cl, 1, &b);
    D3D12_TEXTURE_COPY_LOCATION dl = {0}, sl = {0};
    dl.pResource = bb; dl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    sl.pResource = buf; sl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    sl.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sl.PlacedFootprint.Footprint.Width = w;
    sl.PlacedFootprint.Footprint.Height = h;
    sl.PlacedFootprint.Footprint.Depth = 1;
    sl.PlacedFootprint.Footprint.RowPitch = pitch;
    cl->lpVtbl->CopyTextureRegion(cl, &dl, (s_sc_w - w) / 2, (s_sc_h - h) / 2, 0, &sl, NULL);
    return 1;
}

/* render_targets[fi] (PRESENT) -> the swap chain's current buffer, aspect kept, black bars */
static void sc_blit(u32 fi)
{
    if (InterlockedExchange(&s_sc_resize, 0)) sc_resize();
    const u32 bi = s_d3d.swap_chain->lpVtbl->GetCurrentBackBufferIndex(s_d3d.swap_chain);
    ID3D12GraphicsCommandList* cl = s_d3d.cmd_list;
    D3D12_RESOURCE_BARRIER b[2] = {0};
    b[0].Type = b[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b[0].Transition.pResource = s_d3d.render_targets[fi];
    b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    b[0].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    b[1].Transition.pResource = s_sc_buf[bi];
    b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    b[1].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b[0].Transition.Subresource = b[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cl->lpVtbl->ResourceBarrier(cl, 2, b);
    const UINT srv_inc = s_d3d.device->lpVtbl->GetDescriptorHandleIncrementSize(s_d3d.device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv;
    s_sc_rtv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_sc_rtv_heap, &rtv);
    rtv.ptr += (SIZE_T)bi * s_d3d.rtv_descriptor_size;
    static const float black[4] = {0, 0, 0, 1};
    cl->lpVtbl->ClearRenderTargetView(cl, rtv, black, 0, NULL);
    cl->lpVtbl->OMSetRenderTargets(cl, 1, &rtv, FALSE, NULL);
    /* the largest rectangle with the guest's aspect ratio, centred */
    float vw = (float)s_sc_w, vh = (float)s_sc_w * (float)s_d3d.height / (float)s_d3d.width;
    if (vh > (float)s_sc_h) { vh = (float)s_sc_h; vw = (float)s_sc_h * (float)s_d3d.width / (float)s_d3d.height; }
    D3D12_VIEWPORT vp = { ((float)s_sc_w - vw) * 0.5f, ((float)s_sc_h - vh) * 0.5f, vw, vh, 0.0f, 1.0f };
    D3D12_RECT sc = { 0, 0, (LONG)s_sc_w, (LONG)s_sc_h };
    cl->lpVtbl->RSSetViewports(cl, 1, &vp);
    cl->lpVtbl->RSSetScissorRects(cl, 1, &sc);
    cl->lpVtbl->SetGraphicsRootSignature(cl, s_sc_rs);
    cl->lpVtbl->SetPipelineState(cl, s_sc_pso);   /* plain bilinear stretch (#250: no sharp upscale) */
    cl->lpVtbl->SetDescriptorHeaps(cl, 1, &s_sc_srv_heap);
    D3D12_GPU_DESCRIPTOR_HANDLE g;
    s_sc_srv_heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(s_sc_srv_heap, &g);
    g.ptr += (UINT64)fi * srv_inc;
    cl->lpVtbl->SetGraphicsRootDescriptorTable(cl, 0, g);
    cl->lpVtbl->IASetPrimitiveTopology(cl, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cl->lpVtbl->DrawInstanced(cl, 3, 1, 0, 0);
    b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    b[0].Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    b[1].Transition.StateBefore = sc_overlay(cl, fi, s_sc_buf[bi]) ? D3D12_RESOURCE_STATE_COPY_DEST
                                                                   : D3D12_RESOURCE_STATE_RENDER_TARGET;
    b[1].Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    cl->lpVtbl->ResourceBarrier(cl, 2, b);
}

/* [#163] VRR present: borderless fullscreen, tearing supported, NHL_VRR != 0 */
static u32 d3d12_batch_draws(void) { return s_d3d.draw_count; }   /* #167 trace */
static int vrr_active(void)
{
    static int s_vrr = -1; if (s_vrr < 0) { const char* e = getenv("NHL_VRR"); s_vrr = (e && *e == '0') ? 0 : 1; }
    return s_sc_on && s_fs && s_sc_tearing && s_vrr;
}
static void move_to_next_frame(void);
static void dred_dump(const char* where);
/* [NHL local patch #164] the held VRR present (see render_frame) */
static int s_pp_pending;
static void present_now(int vrr)
{
    HRESULT ph = s_d3d.swap_chain->lpVtbl->Present(s_d3d.swap_chain, vrr ? 0u : 1u, vrr ? DXGI_PRESENT_ALLOW_TEARING : 0u);
    if (ph == (HRESULT)0x887A0005L || ph == (HRESULT)0x887A0007L) dred_dump("Present");
}
static unsigned long long s_pp_present_us, s_pp_wait_us;   /* #226: last pp_issue split */
static int pp_issue(void)
{
    if (!s_pp_pending) return 0;
    s_pp_pending = 0;
    { extern void gcm_ftl(const char*, u32, u32, u32); gcm_ftl("ppiss", 0, 0, 0); }   /* #167 */
    const unsigned long long t0 = ps3_qpc_us();
    present_now(vrr_active());   /* F11 back to windowed in between: vsync */
    const unsigned long long t1 = ps3_qpc_us();
    move_to_next_frame();
    s_pp_present_us = t1 - t0; s_pp_wait_us = ps3_qpc_us() - t1;
    return 1;
}
/* ticker thread, at each vblank tick: issue the held present. 1 = presented now. */
int rsx_d3d12_backend_vblank(void) { return s_d3d.initialized ? pp_issue() : 0; }
/* 1 while presents are held for the vblank grid (the ticker's frame-pacing stats then
 * count them at rsx_d3d12_backend_vblank instead of after render) */
int rsx_d3d12_backend_present_held(void) { return s_pp_pending; }

/* ---------------------------------------------------------------------------
 * D3D12 initialization
 * -----------------------------------------------------------------------*/

static int init_d3d12(u32 width, u32 height)
{
    HRESULT hr;

    /* Enable debug layer in debug builds */
    /* Debug layer: on in debug builds, or in any build when D3D12_DBG is set
     * (so a Release run can capture exact PSO/validation errors). */
    if (
#ifdef NDEBUG
        getenv("D3D12_DBG")
#else
        1
#endif
    ) {
        ID3D12Debug* debug_controller = NULL;
        hr = D3D12GetDebugInterface(&IID_ID3D12Debug, (void**)&debug_controller);
        if (SUCCEEDED(hr) && debug_controller) {
            debug_controller->lpVtbl->EnableDebugLayer(debug_controller);
            debug_controller->lpVtbl->Release(debug_controller);
            printf("[D3D12] Debug layer enabled\n");
        } else {
            printf("[D3D12] Debug layer requested but unavailable (0x%08lX) -- "
                   "install 'Graphics Tools' optional feature\n", hr);
        }
    }

    /* [NHL local patch #57] DRED (Device Removed Extended Data): auto
     * breadcrumbs + page-fault tracking, part of the core runtime (no SDK
     * layers needed). On device removal dred_dump() names the GPU op that died
     * and the guest draw it belongs to. On by default; D3D12_DRED=0 disables. */
    { const char* e = getenv("D3D12_DRED");
      if (!e || atoi(e)) {
          ID3D12DeviceRemovedExtendedDataSettings* ds = NULL;
          if (SUCCEEDED(D3D12GetDebugInterface(&IID_ID3D12DeviceRemovedExtendedDataSettings,
                                               (void**)&ds)) && ds) {
              ds->lpVtbl->SetAutoBreadcrumbsEnablement(ds, D3D12_DRED_ENABLEMENT_FORCED_ON);
              ds->lpVtbl->SetPageFaultEnablement(ds, D3D12_DRED_ENABLEMENT_FORCED_ON);
              ds->lpVtbl->Release(ds);
              printf("[D3D12] DRED breadcrumbs + page faults enabled\n");
          } else printf("[D3D12] DRED unavailable\n");
      } }

    /* Create DXGI factory */
    IDXGIFactory4* factory = NULL;
    hr = CreateDXGIFactory1(&IID_IDXGIFactory4, (void**)&factory);
    if (FAILED(hr)) {
        printf("[D3D12] ERROR: CreateDXGIFactory1 failed (0x%08lX)\n", hr);
        return -1;
    }

    /* Create D3D12 device. On a dual-GPU laptop a NULL adapter usually lands on
     * the integrated GPU; explicitly pick the high-performance (discrete)
     * adapter via IDXGIFactory6. Set CELLMARK_IGPU to force the low-power one
     * (for A/B testing a suspected iGPU-driver stall). */
    {
        IDXGIFactory6* factory6 = NULL;
        if (SUCCEEDED(factory->lpVtbl->QueryInterface(
                factory, &IID_IDXGIFactory6, (void**)&factory6))) {
            DXGI_GPU_PREFERENCE pref = getenv("CELLMARK_IGPU")
                ? DXGI_GPU_PREFERENCE_MINIMUM_POWER
                : DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE;
            IDXGIAdapter1* adapter = NULL;
            for (UINT ai = 0; factory6->lpVtbl->EnumAdapterByGpuPreference(
                     factory6, ai, pref, &IID_IDXGIAdapter1, (void**)&adapter)
                     != DXGI_ERROR_NOT_FOUND; ai++) {
                DXGI_ADAPTER_DESC1 ad;
                adapter->lpVtbl->GetDesc1(adapter, &ad);
                if (!(ad.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
                    SUCCEEDED(D3D12CreateDevice((IUnknown*)adapter,
                        D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device,
                        (void**)&s_d3d.device))) {
                    printf("[D3D12] adapter: %ls (VRAM %llu MB)\n", ad.Description,
                           (unsigned long long)(ad.DedicatedVideoMemory >> 20));
                    adapter->lpVtbl->Release(adapter);
                    break;
                }
                adapter->lpVtbl->Release(adapter);
                adapter = NULL;
            }
            factory6->lpVtbl->Release(factory6);
        }
    }
    if (!s_d3d.device) {
        hr = D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0,
                               &IID_ID3D12Device, (void**)&s_d3d.device);
        if (FAILED(hr)) {
            printf("[D3D12] ERROR: D3D12CreateDevice failed (0x%08lX)\n", hr);
            factory->lpVtbl->Release(factory);
            return -1;
        }
        printf("[D3D12] Device created on default adapter (feature level 11.0)\n");
    }

    /* Create command queue */
    D3D12_COMMAND_QUEUE_DESC queue_desc = {0};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    hr = s_d3d.device->lpVtbl->CreateCommandQueue(
        s_d3d.device, &queue_desc, &IID_ID3D12CommandQueue, (void**)&s_d3d.cmd_queue);
    if (FAILED(hr)) {
        printf("[D3D12] ERROR: CreateCommandQueue failed\n");
        factory->lpVtbl->Release(factory);
        return -1;
    }

    /* Create swap chain */
    DXGI_SWAP_CHAIN_DESC1 sc_desc = {0};
    /* [#163] scaled present: the swap chain follows the window, tearing allowed if supported */
    s_sc_on = getenv("NHL_DIRECT_PRESENT") ? 0 : 1;
    if (s_sc_on) {
        IDXGIFactory5* f5 = NULL; BOOL tear = FALSE;
        if (SUCCEEDED(factory->lpVtbl->QueryInterface(factory, &IID_IDXGIFactory5, (void**)&f5))) {
            if (FAILED(f5->lpVtbl->CheckFeatureSupport(f5, DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tear, sizeof(tear)))) tear = FALSE;
            f5->lpVtbl->Release(f5);
        }
        s_sc_tearing = tear ? 1 : 0;
        sc_client_size(&s_sc_w, &s_sc_h);
        sc_desc.Flags = sc_flags();
        printf("[SCALE] scaled present %ux%u -> window %ux%u, tearing %s\n", width, height, s_sc_w, s_sc_h,
               s_sc_tearing ? "supported" : "NOT supported (no VRR present)");
        /* #253: the render scale (needs the scaled present: the display surfaces are our own) */
        { const char* e = getenv("NHL_RENDER_SCALE"); float s = 1.0f; const char* how = "";
          if (e && !_stricmp(e, "native")) {
              MONITORINFO mi = {0}; mi.cbSize = sizeof mi;
              if (GetMonitorInfoA(MonitorFromWindow(s_d3d.hwnd, MONITOR_DEFAULTTOPRIMARY), &mi)) {
                  const float mw = (float)(mi.rcMonitor.right - mi.rcMonitor.left);
                  const float mh = (float)(mi.rcMonitor.bottom - mi.rcMonitor.top);
                  s = mw / (float)width < mh / (float)height ? mw / (float)width : mh / (float)height;
              }
              how = " (native: the screen)";
          } else if (e && *e) s = (float)atof(e);
          /* [NHL local patch #267] down to 0.5 (360p) for weak GPUs; the scale is applied the
           * same way both directions (resource sizes, viewports, scissors, copy boxes). */
          if (!(s >= 0.5f)) s = 1.0f;
          if (s > 4.0f) s = 4.0f;
          s_rs = s; }
    }
    s_dw = rs_up(width); s_dh = rs_up(height);
    if (s_rs != 1.0f) printf("[RSCALE] render scale %.3f: display %ux%u -> %ux%u\n", s_rs, width, height, s_dw, s_dh);
    if (s_sc_on) {   /* #254: effects scale (half-res chains toward full res) */
        const char* e = getenv("NHL_FX_SCALE"); const int f = e ? atoi(e) : 1;
        s_fxs = f >= 4 ? 4 : f >= 2 ? 2 : 1;
        if (s_fxs > 1) printf("[RSCALE] effects scale %d: reduced-size passes up to %dx, capped at the display\n", s_fxs, s_fxs);
    }
    (void)shadow_scale();   /* #254: before any fragment program is translated */
    sc_desc.Width = s_sc_on ? s_sc_w : width;
    sc_desc.Height = s_sc_on ? s_sc_h : height;
    sc_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sc_desc.SampleDesc.Count = 1;
    sc_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sc_desc.BufferCount = FRAME_COUNT;
    sc_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    IDXGISwapChain1* swap_chain1 = NULL;
    hr = factory->lpVtbl->CreateSwapChainForHwnd(
        factory, (IUnknown*)s_d3d.cmd_queue,
        s_d3d.hwnd, &sc_desc, NULL, NULL, &swap_chain1);
    if (FAILED(hr)) {
        printf("[D3D12] ERROR: CreateSwapChainForHwnd failed (0x%08lX)\n", hr);
        factory->lpVtbl->Release(factory);
        return -1;
    }

    /* Disable Alt+Enter fullscreen toggle */
    factory->lpVtbl->MakeWindowAssociation(factory, s_d3d.hwnd, DXGI_MWA_NO_ALT_ENTER);
    factory->lpVtbl->Release(factory);

    /* Query SwapChain3 interface */
    hr = swap_chain1->lpVtbl->QueryInterface(
        swap_chain1, &IID_IDXGISwapChain3, (void**)&s_d3d.swap_chain);
    swap_chain1->lpVtbl->Release(swap_chain1);
    if (FAILED(hr)) {
        printf("[D3D12] ERROR: QueryInterface for SwapChain3 failed\n");
        return -1;
    }

    s_d3d.frame_index = s_d3d.swap_chain->lpVtbl->GetCurrentBackBufferIndex(s_d3d.swap_chain);

    /* Create RTV descriptor heap */
    D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc = {0};
    rtv_heap_desc.NumDescriptors = FRAME_COUNT;
    rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hr = s_d3d.device->lpVtbl->CreateDescriptorHeap(
        s_d3d.device, &rtv_heap_desc, &IID_ID3D12DescriptorHeap, (void**)&s_d3d.rtv_heap);
    if (FAILED(hr)) return -1;

    s_d3d.rtv_descriptor_size = s_d3d.device->lpVtbl->GetDescriptorHandleIncrementSize(
        s_d3d.device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    /* Create RTVs for each frame */
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle;
    s_d3d.rtv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.rtv_heap, &rtv_handle);

    if (s_sc_on) {   /* [#163] own guest-size surfaces + blit pipeline */
        if (sc_init(s_dw, s_dh) != 0) { printf("[SCALE] init FAILED\n"); return -1; }   /* #253: resource size */
    } else
    for (u32 i = 0; i < FRAME_COUNT; i++) {
        hr = s_d3d.swap_chain->lpVtbl->GetBuffer(
            s_d3d.swap_chain, i, &IID_ID3D12Resource, (void**)&s_d3d.render_targets[i]);
        if (FAILED(hr)) return -1;

        s_d3d.device->lpVtbl->CreateRenderTargetView(
            s_d3d.device, s_d3d.render_targets[i], NULL, rtv_handle);
        rtv_handle.ptr += s_d3d.rtv_descriptor_size;
    }

    /* ---------------------------------------------------------------
     * Depth/stencil buffer
     * 24-bit depth + 8-bit stencil (DXGI_FORMAT_D24_UNORM_S8_UINT).
     * One shared depth texture across both frames — RSX games on PS3
     * typically use a single zeta surface.
     * ---------------------------------------------------------------*/
    {
        D3D12_DESCRIPTOR_HEAP_DESC dsv_heap_desc = {0};
        dsv_heap_desc.NumDescriptors = 3;   /* [NHL local patch] [1] = offscreen-RT depth/stencil, [2] = its #249 MSAA twin */
        dsv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        hr = s_d3d.device->lpVtbl->CreateDescriptorHeap(
            s_d3d.device, &dsv_heap_desc, &IID_ID3D12DescriptorHeap,
            (void**)&s_d3d.dsv_heap);
        if (FAILED(hr)) {
            printf("[D3D12] DSV heap creation failed\n");
            return -1;
        }

        D3D12_HEAP_PROPERTIES heap_props = {0};
        heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC depth_desc = {0};
        depth_desc.Dimension          = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        /* Sized to cover the LARGEST render target, not just the window:
         * D3D12 clips rasterization to the smallest bound attachment, and
         * this DSV is shared by every pass -- with a window-sized depth
         * buffer, draws into larger offscreen RTs (wave's 1920x1080 input
         * image) were silently cropped to the window rect. */
        depth_desc.Width              = zbuf_w();   /* #253: 2048, or the scaled display if larger */
        depth_desc.Height             = zbuf_h();
        depth_desc.DepthOrArraySize   = 1;
        depth_desc.MipLevels          = 1;
        depth_desc.Format             = DXGI_FORMAT_D24_UNORM_S8_UINT;
        depth_desc.SampleDesc.Count   = 1;
        depth_desc.Layout             = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        depth_desc.Flags              = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

        D3D12_CLEAR_VALUE depth_clear = {0};
        depth_clear.Format                       = DXGI_FORMAT_D24_UNORM_S8_UINT;
        depth_clear.DepthStencil.Depth           = 1.0f;
        depth_clear.DepthStencil.Stencil         = 0;

        hr = s_d3d.device->lpVtbl->CreateCommittedResource(
            s_d3d.device, &heap_props, D3D12_HEAP_FLAG_NONE,
            &depth_desc, D3D12_RESOURCE_STATE_DEPTH_WRITE, &depth_clear,
            &IID_ID3D12Resource, (void**)&s_d3d.depth_buffer);
        if (FAILED(hr)) {
            printf("[D3D12] Depth buffer creation failed (0x%08lX)\n", hr);
            return -1;
        }

        D3D12_DEPTH_STENCIL_VIEW_DESC dsv_desc = {0};
        dsv_desc.Format         = DXGI_FORMAT_D24_UNORM_S8_UINT;
        dsv_desc.ViewDimension  = D3D12_DSV_DIMENSION_TEXTURE2D;

        D3D12_CPU_DESCRIPTOR_HANDLE dsv_handle;
        s_d3d.dsv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.dsv_heap, &dsv_handle);
        s_d3d.device->lpVtbl->CreateDepthStencilView(
            s_d3d.device, s_d3d.depth_buffer, &dsv_desc, dsv_handle);

        /* [NHL local patch] A second depth/stencil buffer for offscreen render
         * targets. RSX gives every surface its own zeta buffer; with one shared
         * buffer cleared on every target switch, a render-to-texture pass in
         * the middle of the display pass wiped the display's stencil, and
         * everything Apt masked afterwards (team logos, the description box)
         * failed its stencil test and vanished. NO_OFF_DSV=1 disables. */
        if (!getenv("NO_OFF_DSV")) {
            /* [#209b] typeless so the scene depth can be snapshotted and read (depth as colour) */
            depth_desc.Format = DXGI_FORMAT_R24G8_TYPELESS;
            hr = s_d3d.device->lpVtbl->CreateCommittedResource(
                s_d3d.device, &heap_props, D3D12_HEAP_FLAG_NONE,
                &depth_desc, D3D12_RESOURCE_STATE_DEPTH_WRITE, &depth_clear,
                &IID_ID3D12Resource, (void**)&s_d3d.depth_buffer2);
            if (SUCCEEDED(hr)) {
                D3D12_CPU_DESCRIPTOR_HANDLE h2 = dsv_handle;
                h2.ptr += s_d3d.device->lpVtbl->GetDescriptorHandleIncrementSize(
                    s_d3d.device, D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
                s_d3d.device->lpVtbl->CreateDepthStencilView(
                    s_d3d.device, s_d3d.depth_buffer2, &dsv_desc, h2);
            } else s_d3d.depth_buffer2 = NULL;
        }

        printf("[D3D12] Depth buffer created (%ux%u D24S8)\n", width, height);
    }

    /* Create command allocators and command list */
    for (u32 i = 0; i < FRAME_COUNT; i++) {
        hr = s_d3d.device->lpVtbl->CreateCommandAllocator(
            s_d3d.device, D3D12_COMMAND_LIST_TYPE_DIRECT,
            &IID_ID3D12CommandAllocator, (void**)&s_d3d.cmd_allocators[i]);
        if (FAILED(hr)) return -1;
    }

    hr = s_d3d.device->lpVtbl->CreateCommandList(
        s_d3d.device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        s_d3d.cmd_allocators[0], NULL,
        &IID_ID3D12GraphicsCommandList, (void**)&s_d3d.cmd_list);
    if (FAILED(hr)) return -1;

    /* Close the command list (it starts in recording state) */
    s_d3d.cmd_list->lpVtbl->Close(s_d3d.cmd_list);

    /* Create fence */
    hr = s_d3d.device->lpVtbl->CreateFence(
        s_d3d.device, 0, D3D12_FENCE_FLAG_NONE,
        &IID_ID3D12Fence, (void**)&s_d3d.fence);
    if (FAILED(hr)) return -1;

    s_d3d.fence_event = CreateEvent(NULL, FALSE, FALSE, NULL);
    memset(s_d3d.fence_values, 0, sizeof(s_d3d.fence_values));

    /* ---------------------------------------------------------------
     * Create root signature with 16 root constants (one mat4 MVP at b0).
     * Visible only to the vertex shader — pixel shader doesn't need it.
     * ---------------------------------------------------------------*/
    {
        /* param 0: mat4 MVP as 16 root constants at b0 (vertex shader).
         * param 1: one SRV (t0) descriptor table for the atlas texture (pixel).
         * static sampler s0: linear clamp. The plain (untextured) PSO simply
         * never references t0/s0, so it ignores them. */
        D3D12_DESCRIPTOR_RANGE srv_range = {0};
        srv_range.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srv_range.NumDescriptors     = 1;
        srv_range.BaseShaderRegister = 0;   /* t0 */
        srv_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_ROOT_PARAMETER root_params[2] = {0};
        root_params[0].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        root_params[0].Constants.ShaderRegister = 0;   /* b0 */
        root_params[0].Constants.RegisterSpace  = 0;
        root_params[0].Constants.Num32BitValues = 16;  /* mat4 */
        root_params[0].ShaderVisibility         = D3D12_SHADER_VISIBILITY_VERTEX;
        root_params[1].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        root_params[1].DescriptorTable.NumDescriptorRanges = 1;
        root_params[1].DescriptorTable.pDescriptorRanges   = &srv_range;
        root_params[1].ShaderVisibility         = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_STATIC_SAMPLER_DESC samp = {0};
        samp.Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        samp.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.ShaderRegister   = 0;   /* s0 */
        samp.MaxLOD           = D3D12_FLOAT32_MAX;
        samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC rs_desc = {0};
        rs_desc.NumParameters     = 2;
        rs_desc.pParameters       = root_params;
        rs_desc.NumStaticSamplers = 1;
        rs_desc.pStaticSamplers   = &samp;
        rs_desc.Flags         = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT
                              | D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS
                              | D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS
                              | D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

        ID3DBlob* signature_blob = NULL;
        ID3DBlob* error_blob = NULL;
        hr = D3D12SerializeRootSignature(&rs_desc, D3D_ROOT_SIGNATURE_VERSION_1,
                                          &signature_blob, &error_blob);
        if (FAILED(hr)) {
            printf("[D3D12] Root signature serialization failed: %s\n",
                   error_blob ? (const char*)error_blob->lpVtbl->GetBufferPointer(error_blob) : "?");
            if (error_blob) error_blob->lpVtbl->Release(error_blob);
            return -1;
        }

        hr = s_d3d.device->lpVtbl->CreateRootSignature(
            s_d3d.device, 0,
            signature_blob->lpVtbl->GetBufferPointer(signature_blob),
            signature_blob->lpVtbl->GetBufferSize(signature_blob),
            &IID_ID3D12RootSignature, (void**)&s_d3d.root_signature);
        signature_blob->lpVtbl->Release(signature_blob);
        if (FAILED(hr)) {
            printf("[D3D12] Root signature creation failed\n");
            return -1;
        }
    }

    /* ---------------------------------------------------------------
     * Compile shaders and create PSO
     * ---------------------------------------------------------------*/
    {
        /* Basic vertex-colored shader.
         * The MVP matrix arrives via root constants as 4 vec4 columns
         * (PS3/OpenGL column-major convention). We multiply explicitly so
         * we don't depend on HLSL's matrix packing — matches PS3 semantics
         * `gl_Position = MVP * vec4(pos, 1.0)`. */
        static const char vs_hlsl[] =
            "cbuffer cb0 : register(b0) {\n"
            "    float4 mvp_col0;\n"
            "    float4 mvp_col1;\n"
            "    float4 mvp_col2;\n"
            "    float4 mvp_col3;\n"
            "};\n"
            "struct VSInput  { float3 pos : POSITION; float4 col : COLOR; };\n"
            "struct VSOutput { float4 pos : SV_POSITION; float4 col : COLOR; };\n"
            "VSOutput main(VSInput i) {\n"
            "    VSOutput o;\n"
            "    float4 p = float4(i.pos, 1.0);\n"
            "    o.pos = mvp_col0 * p.x + mvp_col1 * p.y + mvp_col2 * p.z + mvp_col3 * p.w;\n"
            "    o.col = i.col;\n"
            "    return o;\n"
            "}\n";
        static const char ps_hlsl[] =
            "struct PSInput { float4 pos : SV_POSITION; float4 col : COLOR; };\n"
            "float4 main(PSInput i) : SV_TARGET { return i.col; }\n";

        ID3DBlob* vs_blob = NULL;
        ID3DBlob* ps_blob = NULL;
        ID3DBlob* err = NULL;

        hr = D3DCompile(vs_hlsl, sizeof(vs_hlsl) - 1, "vs_basic", NULL, NULL,
                        "main", "vs_5_0", 0, 0, &vs_blob, &err);
        if (FAILED(hr)) {
            printf("[D3D12] VS compile failed: %s\n",
                   err ? (const char*)err->lpVtbl->GetBufferPointer(err) : "unknown");
            if (err) err->lpVtbl->Release(err);
        }

        hr = D3DCompile(ps_hlsl, sizeof(ps_hlsl) - 1, "ps_basic", NULL, NULL,
                        "main", "ps_5_0", 0, 0, &ps_blob, &err);
        if (FAILED(hr)) {
            printf("[D3D12] PS compile failed: %s\n",
                   err ? (const char*)err->lpVtbl->GetBufferPointer(err) : "unknown");
            if (err) err->lpVtbl->Release(err);
        }

        if (vs_blob && ps_blob) {
            D3D12_INPUT_ELEMENT_DESC input_layout[] = {
                {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
                 D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12,
                 D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 28,
                 D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            };

            D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc = {0};
            pso_desc.pRootSignature = s_d3d.root_signature;
            pso_desc.VS.pShaderBytecode = vs_blob->lpVtbl->GetBufferPointer(vs_blob);
            pso_desc.VS.BytecodeLength = vs_blob->lpVtbl->GetBufferSize(vs_blob);
            pso_desc.PS.pShaderBytecode = ps_blob->lpVtbl->GetBufferPointer(ps_blob);
            pso_desc.PS.BytecodeLength = ps_blob->lpVtbl->GetBufferSize(ps_blob);
            pso_desc.InputLayout.pInputElementDescs = input_layout;
            pso_desc.InputLayout.NumElements = 3;
            pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
            pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            pso_desc.BlendState.RenderTarget[0].RenderTargetWriteMask =
                D3D12_COLOR_WRITE_ENABLE_ALL;
            pso_desc.SampleMask = UINT_MAX;
            pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            pso_desc.NumRenderTargets = 1;
            pso_desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
            pso_desc.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
            /* Depth test enabled, write enabled, LESS func.
             * Games with depth_test_enable=0 in RSX state can still render —
             * LESS just means new-z must be less than existing — but future
             * work should mirror RSX depth state into a PSO cache. */
            pso_desc.DepthStencilState.DepthEnable    = TRUE;
            pso_desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
            pso_desc.DepthStencilState.DepthFunc      = D3D12_COMPARISON_FUNC_LESS_EQUAL;
            pso_desc.DepthStencilState.StencilEnable  = FALSE;
            pso_desc.SampleDesc.Count = 1;

            hr = s_d3d.device->lpVtbl->CreateGraphicsPipelineState(
                s_d3d.device, &pso_desc,
                &IID_ID3D12PipelineState, (void**)&s_d3d.pipeline_state);
            if (SUCCEEDED(hr)) {
                s_d3d.pipeline_ready = 1;
                printf("[D3D12] Pipeline state created (triangle class)\n");
            } else {
                printf("[D3D12] PSO TRIANGLE creation failed (0x%08lX)\n", hr);
            }

            /* Line-class PSO — same shader, LINE topology type. */
            pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
            hr = s_d3d.device->lpVtbl->CreateGraphicsPipelineState(
                s_d3d.device, &pso_desc,
                &IID_ID3D12PipelineState, (void**)&s_d3d.pipeline_state_lines);
            if (SUCCEEDED(hr)) printf("[D3D12] Pipeline state created (line class)\n");
            else printf("[D3D12] PSO LINE creation failed (0x%08lX)\n", hr);

            /* Point-class PSO. */
            pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
            hr = s_d3d.device->lpVtbl->CreateGraphicsPipelineState(
                s_d3d.device, &pso_desc,
                &IID_ID3D12PipelineState, (void**)&s_d3d.pipeline_state_points);
            if (SUCCEEDED(hr)) printf("[D3D12] Pipeline state created (point class)\n");
            else printf("[D3D12] PSO POINT creation failed (0x%08lX)\n", hr);

            vs_blob->lpVtbl->Release(vs_blob);
            ps_blob->lpVtbl->Release(ps_blob);
        }

        /* -----------------------------------------------------------------
         * Textured triangle PSO (dbgfont / 2D atlas quads). Same MVP VS but
         * carrying UV; the PS samples the single-channel atlas as coverage
         * and modulates the vertex color, with straight alpha blending so
         * glyph edges composite over the framebuffer. Depth off (2D overlay).
         * ----------------------------------------------------------------*/
        {
            static const char vs_tex[] =
                "cbuffer cb0 : register(b0){float4 c0;float4 c1;float4 c2;float4 c3;};\n"
                "struct VSIn { float3 pos:POSITION; float4 col:COLOR; float2 uv:TEXCOORD; };\n"
                "struct VSOut{ float4 pos:SV_POSITION; float4 col:COLOR; float2 uv:TEXCOORD; };\n"
                "VSOut main(VSIn i){ VSOut o; float4 p=float4(i.pos,1.0);\n"
                "  o.pos=c0*p.x+c1*p.y+c2*p.z+c3*p.w; o.col=i.col; o.uv=i.uv; return o; }\n";
            static const char ps_tex[] =
                "Texture2D tex : register(t0);\n"
                "SamplerState smp : register(s0);\n"
                "struct PSIn{ float4 pos:SV_POSITION; float4 col:COLOR; float2 uv:TEXCOORD; };\n"
                "float4 main(PSIn i):SV_TARGET{\n"
                /* dbgfont texcoords are compressed ~10x (U) / ~8x (V) vs. the
                 * atlas; scale to recover glyph cells. Exact per-glyph offset
                 * still being calibrated. */
                "  float2 uv2 = float2(i.uv.x*10.0, i.uv.y*8.0 + 0.59);\n"
                "  float cov = tex.Sample(smp, uv2).r;\n"
                "  return float4(i.col.rgb, i.col.a * cov); }\n";

            ID3DBlob *vtb = NULL, *ptb = NULL, *e2 = NULL;
            hr = D3DCompile(vs_tex, sizeof(vs_tex) - 1, "vs_tex", NULL, NULL, "main", "vs_5_0", 0, 0, &vtb, &e2);
            if (FAILED(hr)) printf("[D3D12] VS(tex) compile failed: %s\n", e2 ? (const char*)e2->lpVtbl->GetBufferPointer(e2) : "?");
            hr = D3DCompile(ps_tex, sizeof(ps_tex) - 1, "ps_tex", NULL, NULL, "main", "ps_5_0", 0, 0, &ptb, &e2);
            if (FAILED(hr)) printf("[D3D12] PS(tex) compile failed: %s\n", e2 ? (const char*)e2->lpVtbl->GetBufferPointer(e2) : "?");

            if (vtb && ptb) {
                D3D12_INPUT_ELEMENT_DESC il[] = {
                    {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                    {"COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                    {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 28, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                };
                D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {0};
                pd.pRootSignature = s_d3d.root_signature;
                pd.VS.pShaderBytecode = vtb->lpVtbl->GetBufferPointer(vtb);
                pd.VS.BytecodeLength  = vtb->lpVtbl->GetBufferSize(vtb);
                pd.PS.pShaderBytecode = ptb->lpVtbl->GetBufferPointer(ptb);
                pd.PS.BytecodeLength  = ptb->lpVtbl->GetBufferSize(ptb);
                pd.InputLayout.pInputElementDescs = il;
                pd.InputLayout.NumElements = 3;
                pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
                pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
                pd.BlendState.RenderTarget[0].BlendEnable    = TRUE;
                pd.BlendState.RenderTarget[0].SrcBlend       = D3D12_BLEND_SRC_ALPHA;
                pd.BlendState.RenderTarget[0].DestBlend      = D3D12_BLEND_INV_SRC_ALPHA;
                pd.BlendState.RenderTarget[0].BlendOp        = D3D12_BLEND_OP_ADD;
                pd.BlendState.RenderTarget[0].SrcBlendAlpha  = D3D12_BLEND_ONE;
                pd.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
                pd.BlendState.RenderTarget[0].BlendOpAlpha   = D3D12_BLEND_OP_ADD;
                pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
                pd.SampleMask = UINT_MAX;
                pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
                pd.NumRenderTargets = 1;
                pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
                pd.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
                pd.DepthStencilState.DepthEnable = FALSE;
                pd.DepthStencilState.StencilEnable = FALSE;
                pd.SampleDesc.Count = 1;
                hr = s_d3d.device->lpVtbl->CreateGraphicsPipelineState(
                    s_d3d.device, &pd, &IID_ID3D12PipelineState, (void**)&s_d3d.pipeline_state_tex);
                if (SUCCEEDED(hr)) printf("[D3D12] Pipeline state created (textured class)\n");
                else printf("[D3D12] PSO TEX creation failed (0x%08lX)\n", hr);
            }
            if (vtb) vtb->lpVtbl->Release(vtb);
            if (ptb) ptb->lpVtbl->Release(ptb);
        }

        /* SRV descriptor heap (shader-visible). Layout: slot 0 = legacy atlas
         * (dbgfont / textured 2D path), slots 1-4 = per-draw VP textures,
         * 5-20 = offscreen render targets (render-to-texture), rest spare.
         * All slots start as null SRVs so any 4-wide table window (the
         * root signature binds t0-t3) is valid on tier-1 hardware. */
        {
            D3D12_DESCRIPTOR_HEAP_DESC hd = {0};
            hd.Type  = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            hd.NumDescriptors = SRV_HEAP_SIZE;
            hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            hr = s_d3d.device->lpVtbl->CreateDescriptorHeap(
                s_d3d.device, &hd, &IID_ID3D12DescriptorHeap, (void**)&s_d3d.srv_heap);
            if (FAILED(hr)) printf("[D3D12] SRV heap creation failed (0x%08lX)\n", hr);
            else {
                s_d3d.srv_inc = s_d3d.device->lpVtbl->GetDescriptorHandleIncrementSize(
                    s_d3d.device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                D3D12_SHADER_RESOURCE_VIEW_DESC nv = {0};
                nv.Format = DXGI_FORMAT_R8_UNORM;
                nv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                nv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                nv.Texture2D.MipLevels = 1;
                D3D12_CPU_DESCRIPTOR_HANDLE hh;
                s_d3d.srv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.srv_heap, &hh);
                for (int _i = 0; _i < SRV_HEAP_SIZE; _i++) {
                    s_d3d.device->lpVtbl->CreateShaderResourceView(s_d3d.device, NULL, &nv, hh);
                    hh.ptr += s_d3d.srv_inc;
                }
            }
        }

        /* RTV heap for offscreen render targets (CPU-visible only). */
        {
            D3D12_DESCRIPTOR_HEAP_DESC hd = {0};
            hd.Type  = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            hd.NumDescriptors = MAX_OFF_RTS;
            hr = s_d3d.device->lpVtbl->CreateDescriptorHeap(
                s_d3d.device, &hd, &IID_ID3D12DescriptorHeap, (void**)&s_d3d.rt_rtv_heap);
            if (FAILED(hr)) printf("[D3D12] offscreen RTV heap creation failed (0x%08lX)\n", hr);
        }
    }

    /* ---------------------------------------------------------------
     * Create dynamic vertex buffer (upload heap, 4MB)
     * ---------------------------------------------------------------*/
    {
        D3D12_HEAP_PROPERTIES heap_props = {0};
        heap_props.Type = D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC buf_desc = {0};
        buf_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buf_desc.Width = MAX_VERTICES * VERTEX_STRIDE;
        buf_desc.Height = 1;
        buf_desc.DepthOrArraySize = 1;
        buf_desc.MipLevels = 1;
        buf_desc.SampleDesc.Count = 1;
        buf_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        hr = s_d3d.device->lpVtbl->CreateCommittedResource(
            s_d3d.device, &heap_props, D3D12_HEAP_FLAG_NONE,
            &buf_desc, D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
            &IID_ID3D12Resource, (void**)&s_d3d.vertex_buffer);
        if (SUCCEEDED(hr)) {
            D3D12_RANGE read_range = {0, 0};
            s_d3d.vertex_buffer->lpVtbl->Map(
                s_d3d.vertex_buffer, 0, &read_range, &s_d3d.vb_mapped);
            s_d3d.vb_view.BufferLocation =
                s_d3d.vertex_buffer->lpVtbl->GetGPUVirtualAddress(s_d3d.vertex_buffer);
            s_d3d.vb_view.StrideInBytes = VERTEX_STRIDE;
            s_d3d.vb_view.SizeInBytes = MAX_VERTICES * VERTEX_STRIDE;
            printf("[D3D12] Vertex buffer created (%u KB)\n",
                   (MAX_VERTICES * VERTEX_STRIDE) / 1024);
        }
    }

    /* ---------------------------------------------------------------
     * Real vertex-program path resources: root signature (CBV b0 for the
     * vp_c[] bank + SRV t0 + static sampler s0), a raw-float4 vertex buffer,
     * and the constant-bank buffer. The PSO itself is built lazily once the
     * game uploads its VP microcode (render_frame).
     * ---------------------------------------------------------------*/
    {
        /* 4-descriptor SRV table (t0-t3) so decompiled fragment programs can
         * sample up to 4 texture units; the hardcoded atlas/colour PSs use only
         * t0 and are unaffected. Matching 4 static samplers s0-s3. */
        D3D12_DESCRIPTOR_RANGE srv_range = {0};
        srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srv_range.NumDescriptors = D3D_TEX_UNITS;   /* t0-t15 (#51) */
        srv_range.BaseShaderRegister = 0;
        srv_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_ROOT_PARAMETER rp[4] = {0};
        rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;   /* b0 = vp_c bank */
        rp[0].Descriptor.ShaderRegister = 0;
        rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rp[1].DescriptorTable.NumDescriptorRanges = 1;
        rp[1].DescriptorTable.pDescriptorRanges = &srv_range;
        rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        rp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;   /* b1 = FP texscale */
        rp[2].Descriptor.ShaderRegister = 1;
        rp[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        rp[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;   /* [#125] t24 = raw vertex bytes */
        rp[3].Descriptor.ShaderRegister = 24;                  /* vs_5_0: no register spaces */
        rp[3].Descriptor.RegisterSpace = 0;
        rp[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

        D3D12_STATIC_SAMPLER_DESC samp[D3D_TEX_UNITS] = {0};
        for (int _s = 0; _s < D3D_TEX_UNITS; _s++) {
            samp[_s].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
            samp[_s].AddressU = samp[_s].AddressV = samp[_s].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
            samp[_s].ShaderRegister = (UINT)_s;
            samp[_s].MaxLOD = D3D12_FLOAT32_MAX;
            samp[_s].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        }
        /* s0 keeps point/clamp: the dbgfont atlas PS samples glyph cells and
         * linear filtering bleeds neighbouring glyphs. */
        /* [NHL local patch #181] s0 linear/wrap like the rest (default; NHL_S0_WRAP=0 =
         * the old point/clamp). Guest FP unit 0 samples through s0, so a tiled texture
         * on unit 0 clamped to its edge texels: the smeared solid-white goal net
         * (play247/248, fixed in play249). */
        /* [NHL local patch #247] Enhanced image: anisotropic filtering for every guest unit.
         * Trilinear drops to small mips fast on surfaces seen at a distance or an angle (the
         * Hurricanes' hem "flags" blur into dots a few metres out, user F9 2026-10-28; RPCS3
         * looks the same without its anisotropic override). NHL_ANISO=<1..16> (default 16,
         * 1 = plain trilinear; #251: always on, no longer tied to NHL_ENHANCE). Point (#176) and
         * explicit-level fetches are unaffected: they pass a level. Static samplers, so it is
         * fixed for the session. */
        { const char* an = getenv("NHL_ANISO");
          int a = 16;
          if (an && *an) a = atoi(an);
          if (a > 16) a = 16;
          if (a > 1) {
              for (int _s = 0; _s < D3D_TEX_UNITS; _s++) { samp[_s].Filter = D3D12_FILTER_ANISOTROPIC; samp[_s].MaxAnisotropy = (UINT)a; }
              printf("[ENHANCE] anisotropic filtering %dx on all texture units\n", a);
          } else printf("[ENHANCE] anisotropic filtering off (trilinear)\n"); }   /* #252: launcher setting */
        { const char* s0w = getenv("NHL_S0_WRAP"); if (s0w && *s0w == '0') {
            samp[0].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
            samp[0].AddressU = samp[0].AddressV = samp[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        } }

        D3D12_ROOT_SIGNATURE_DESC rd = {0};
        rd.NumParameters = 4; rd.pParameters = rp;
        rd.NumStaticSamplers = D3D_TEX_UNITS; rd.pStaticSamplers = samp;
        rd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        ID3DBlob* sig = NULL; ID3DBlob* err = NULL;
        hr = D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
        if (SUCCEEDED(hr)) {
            s_d3d.device->lpVtbl->CreateRootSignature(s_d3d.device, 0,
                sig->lpVtbl->GetBufferPointer(sig), sig->lpVtbl->GetBufferSize(sig),
                &IID_ID3D12RootSignature, (void**)&s_d3d.vp_root_sig);
            sig->lpVtbl->Release(sig);
        } else if (err) { printf("[D3D12] VP root sig: %s\n", (const char*)err->lpVtbl->GetBufferPointer(err)); err->lpVtbl->Release(err); }

        /* raw float4 vertex buffer + constant bank (both UPLOAD, persistently mapped) */
        D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC bd = {0};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Height = 1;
        bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_RANGE nr = {0,0};

        bd.Width = (u64)MAX_VERTICES * 256 * 2;  /* generic VP vertex: 16 float4 slots,
                                          * DOUBLE-buffered by frame parity so a
                                          * new frame's upload never overwrites
                                          * vertices the in-flight GPU frame is
                                          * still reading (was: torn/missing
                                          * triangles mixing stale+new verts) */
        /* Failure here leaves vp_vb_mapped NULL and every VP draw silently
         * declines to upload -- the scene just does not render, with no error
         * anywhere. Report the size too: this buffer is
         * MAX_VERTICES * 256 * 2 bytes and grows fast. */
        fprintf(stderr, "[VPVB] per-frame vertex buffer: %llu MB (%u verts, x2 parity)%c",
                (unsigned long long)(bd.Width >> 20), (unsigned)MAX_VERTICES, 10);
        if (SUCCEEDED(s_d3d.device->lpVtbl->CreateCommittedResource(s_d3d.device, &hp,
                D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
                &IID_ID3D12Resource, (void**)&s_d3d.vp_vb)))
            s_d3d.vp_vb->lpVtbl->Map(s_d3d.vp_vb, 0, &nr, &s_d3d.vp_vb_mapped);
        else
            fprintf(stderr, "[VPVB] ALLOCATION FAILED -- no VP geometry will render%c", 10);
        {   /* [NHL local patch #62] index buffer for GPU-indexed VP draws */
            D3D12_RESOURCE_DESC ib = bd;
            ib.Width = (u64)VP_MAX_INDICES * 4u * 2u;
            if (SUCCEEDED(s_d3d.device->lpVtbl->CreateCommittedResource(s_d3d.device, &hp,
                    D3D12_HEAP_FLAG_NONE, &ib, D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
                    &IID_ID3D12Resource, (void**)&s_d3d.vp_ib)))
                s_d3d.vp_ib->lpVtbl->Map(s_d3d.vp_ib, 0, &nr, &s_d3d.vp_ib_mapped);
            else fprintf(stderr, "[VPIB] allocation failed -- indexed draws stay CPU-expanded%c", 10);
        }
        {   /* [NHL local patch #125] raw guest vertex bytes for GPU vertex fetch */
            D3D12_RESOURCE_DESC rb = bd;
            rb.Width = (u64)VP_RAW_BYTES * 2u;
            void* m = NULL;
            if (SUCCEEDED(s_d3d.device->lpVtbl->CreateCommittedResource(s_d3d.device, &hp,
                    D3D12_HEAP_FLAG_NONE, &rb, D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
                    &IID_ID3D12Resource, (void**)&s_d3d.vp_raw)) &&
                SUCCEEDED(s_d3d.vp_raw->lpVtbl->Map(s_d3d.vp_raw, 0, &nr, &m)))
                s_d3d.vp_raw_mapped = (u8*)m;
            else fprintf(stderr, "[GPUVTX] raw vertex buffer allocation failed -- CPU vertex path only%c", 10);
        }

        bd.Width = (u64)VP_CB_STRIDE * MAX_DRAWS * 2;   /* per-draw constant slots, x2 parity */
        /* A failure here is silent and catastrophic: vp_record_cb returns early,
         * every draw loses its constants, and the whole scene transforms by
         * zeros -- indistinguishable from a broken vertex program. Say so. */
        fprintf(stderr, "[VPCB] per-draw constant buffer: %llu MB (%d draws)%c",
                (unsigned long long)(bd.Width >> 20), MAX_DRAWS, 10);
        if (SUCCEEDED(s_d3d.device->lpVtbl->CreateCommittedResource(s_d3d.device, &hp,
                D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
                &IID_ID3D12Resource, (void**)&s_d3d.vp_cb)))
            s_d3d.vp_cb->lpVtbl->Map(s_d3d.vp_cb, 0, &nr, &s_d3d.vp_cb_mapped);

        bd.Width = (u64)VP_FPCB_STRIDE * MAX_DRAWS * 2;  /* per-draw FP slots (b1), x2 parity */
        if (SUCCEEDED(s_d3d.device->lpVtbl->CreateCommittedResource(s_d3d.device, &hp,
                D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
                &IID_ID3D12Resource, (void**)&s_d3d.vp_fpcb)))
            s_d3d.vp_fpcb->lpVtbl->Map(s_d3d.vp_fpcb, 0, &nr, &s_d3d.vp_fpcb_mapped);

        printf("[D3D12] VP path resources: rootsig=%p vb=%p cb=%p\n",
               (void*)s_d3d.vp_root_sig, (void*)s_d3d.vp_vb, (void*)s_d3d.vp_cb);
    }

    printf("[D3D12] Initialization complete (%ux%u, %u buffers, pipeline=%s)\n",
           width, height, FRAME_COUNT,
           s_d3d.pipeline_ready ? "ready" : "NOT ready");
    return 0;
}

/* ---------------------------------------------------------------------------
 * Frame sync helpers
 * -----------------------------------------------------------------------*/

/* [NHL local patch #57] Per-frame draw log for DRED: the k-th Draw* of the
 * command list named "fN" is s_dlog[N & 3][k]. The command list is renamed
 * each frame at Reset, so a breadcrumb node's name says which frame died. */
typedef struct { u32 fp, rec, n, rt, t0; u32 mrt[3]; u32 vbo, ctl, cmask;
                 struct { u32 raw, off, w, h, fmt, set, rtslot; } tex[D3D_TEX_UNITS]; } DLogEntry;
static DLogEntry s_dlog[4][MAX_DRAWS + 64];
static u32       s_dlog_n[4];
static u64       s_dlog_frame[4];
static void dlog_add(u64 frame, u32 fp, u32 rec, u32 n, u32 rt, u32 t0)
{
    u32 s = (u32)(frame & 3u);
    if (s_dlog_frame[s] != frame) { s_dlog_frame[s] = frame; s_dlog_n[s] = 0; }
    if (s_dlog_n[s] < MAX_DRAWS + 64) {
        DLogEntry* e = &s_dlog[s][s_dlog_n[s]++];
        memset(e, 0, sizeof *e);
        e->fp = fp; e->rec = rec; e->n = n; e->rt = rt; e->t0 = t0;
        if (fp < 0xFFFFFFFEu && rec < MAX_DRAWS) {   /* guest-FP draw: full binding snapshot */
            const D3D12DrawRecord* dr = &s_d3d.draws[rec];
            for (int m = 0; m < 3; m++) e->mrt[m] = dr->rt_mrt[m];
            e->vbo = dr->vb_byte_offset; e->ctl = (u32)dr->fp_exp32; e->cmask = dr->cmask;
            for (int u = 0; u < D3D_TEX_UNITS; u++) {
                e->tex[u].raw = dr->tex[u].raw; e->tex[u].off = dr->tex[u].off;
                e->tex[u].w = dr->tex[u].w; e->tex[u].h = dr->tex[u].h;
                e->tex[u].fmt = dr->tex[u].fmt; e->tex[u].set = (u32)dr->tex[u].set;
                e->tex[u].rtslot = (u32)dr->tex_rt[u];
            }
        }
    }
}

static const char* dred_op_name(D3D12_AUTO_BREADCRUMB_OP op)
{
    switch (op) {
    case D3D12_AUTO_BREADCRUMB_OP_DRAWINSTANCED:        return "DrawInstanced";
    case D3D12_AUTO_BREADCRUMB_OP_DRAWINDEXEDINSTANCED: return "DrawIndexedInstanced";
    case D3D12_AUTO_BREADCRUMB_OP_CLEARRENDERTARGETVIEW: return "ClearRTV";
    case D3D12_AUTO_BREADCRUMB_OP_CLEARDEPTHSTENCILVIEW: return "ClearDSV";
    case D3D12_AUTO_BREADCRUMB_OP_COPYTEXTUREREGION:    return "CopyTextureRegion";
    case D3D12_AUTO_BREADCRUMB_OP_COPYBUFFERREGION:     return "CopyBufferRegion";
    case D3D12_AUTO_BREADCRUMB_OP_RESOURCEBARRIER:      return "ResourceBarrier";
    case D3D12_AUTO_BREADCRUMB_OP_COPYRESOURCE:         return "CopyResource";
    case D3D12_AUTO_BREADCRUMB_OP_RESOLVESUBRESOURCE:   return "ResolveSubresource";
    default: return "op";
    }
}

/* Print the DRED report once: removal reason, the last op each in-flight
 * command list completed (and the guest draw it maps to), page-fault VA and
 * the allocations around it. */
static void dred_dump(const char* where)
{
    static int done = 0;
    if (done || !s_d3d.device) return;
    done = 1;
    HRESULT rr = s_d3d.device->lpVtbl->GetDeviceRemovedReason(s_d3d.device);
    printf("[DRED] device removed (%s), reason=0x%08lX\n", where, (long)rr);
    ID3D12DeviceRemovedExtendedData* d = NULL;
    if (FAILED(s_d3d.device->lpVtbl->QueryInterface(s_d3d.device,
            &IID_ID3D12DeviceRemovedExtendedData, (void**)&d)) || !d) {
        printf("[DRED] no DRED interface\n"); fflush(stdout); return;
    }
    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT bo;
    if (SUCCEEDED(d->lpVtbl->GetAutoBreadcrumbsOutput(d, &bo))) {
        for (const D3D12_AUTO_BREADCRUMB_NODE* n = bo.pHeadAutoBreadcrumbNode; n; n = n->pNext) {
            UINT last = n->pLastBreadcrumbValue ? *n->pLastBreadcrumbValue : 0;
            unsigned frame = 0;
            if (n->pCommandListDebugNameW) swscanf(n->pCommandListDebugNameW, L"f%u", &frame);
            printf("[DRED] cmdlist f%u: %u of %u ops completed%s\n", frame, last,
                   n->BreadcrumbCount, last < n->BreadcrumbCount ? "  <-- INCOMPLETE" : "");
            if (last >= n->BreadcrumbCount || !n->pCommandHistory) continue;
            u32 draws_before = 0;
            for (UINT i = 0; i < last; i++)
                if (n->pCommandHistory[i] == D3D12_AUTO_BREADCRUMB_OP_DRAWINSTANCED ||
                    n->pCommandHistory[i] == D3D12_AUTO_BREADCRUMB_OP_DRAWINDEXEDINSTANCED)
                    draws_before++;
            UINT lo = last >= 4 ? last - 4 : 0, hi = last + 4 < n->BreadcrumbCount ? last + 4 : n->BreadcrumbCount;
            for (UINT i = lo; i < hi; i++)
                printf("[DRED]   op %u %s%s\n", i, dred_op_name(n->pCommandHistory[i]),
                       i == last ? "   <-- first op NOT completed" : "");
            u32 s = frame & 3u;
            if (s_dlog_frame[s] == frame) {
                for (u32 k = draws_before >= 2 ? draws_before - 2 : 0;
                     k < draws_before + 3 && k < s_dlog_n[s]; k++) {
                    const DLogEntry* e = &s_dlog[s][k];
                    printf("[DRED]   draw %u: rec=%u fp=0x%08X n=%u rt=0x%08X t0=0x%08X%s\n",
                           k, e->rec, e->fp, e->n, e->rt, e->t0,
                           k == draws_before ? "   <-- suspect" : "");
                    if (k == draws_before) {
                        printf("[DRED]     mrt=0x%X/0x%X/0x%X vbo=0x%X ctl=0x%X cmask=0x%X\n",
                               e->mrt[0], e->mrt[1], e->mrt[2], e->vbo, e->ctl, e->cmask);
                        for (int u = 0; u < D3D_TEX_UNITS; u++)
                            if (e->tex[u].set)
                                printf("[DRED]     t%d raw=0x%08X off=0x%08X %ux%u fmt=0x%02X offrt=%d\n",
                                       u, e->tex[u].raw, e->tex[u].off, e->tex[u].w, e->tex[u].h,
                                       e->tex[u].fmt, (int)e->tex[u].rtslot);
                    }
                }
            } else printf("[DRED]   (draw log for f%u already overwritten)\n", frame);
        }
    }
    D3D12_DRED_PAGE_FAULT_OUTPUT po;
    if (SUCCEEDED(d->lpVtbl->GetPageFaultAllocationOutput(d, &po))) {
        printf("[DRED] page fault VA=0x%016llX\n", (unsigned long long)po.PageFaultVA);
        int k = 0;
        for (const D3D12_DRED_ALLOCATION_NODE* a = po.pHeadExistingAllocationNode; a && k < 8; a = a->pNext, k++)
            printf("[DRED]   existing alloc type=%d name=%s\n", (int)a->AllocationType,
                   a->ObjectNameA ? a->ObjectNameA : "?");
        k = 0;
        for (const D3D12_DRED_ALLOCATION_NODE* a = po.pHeadRecentFreedAllocationNode; a && k < 8; a = a->pNext, k++)
            printf("[DRED]   recently FREED alloc type=%d name=%s\n", (int)a->AllocationType,
                   a->ObjectNameA ? a->ObjectNameA : "?");
    }
    d->lpVtbl->Release(d);
    fflush(stdout);
}

static void wait_for_gpu(void)
{
    u32 fi = s_d3d.frame_index;
    s_d3d.fence_values[fi]++;
    s_d3d.cmd_queue->lpVtbl->Signal(s_d3d.cmd_queue, s_d3d.fence, s_d3d.fence_values[fi]);

    if (s_d3d.fence->lpVtbl->GetCompletedValue(s_d3d.fence) < s_d3d.fence_values[fi]) {
        s_d3d.fence->lpVtbl->SetEventOnCompletion(
            s_d3d.fence, s_d3d.fence_values[fi], s_d3d.fence_event);
        /* NEVER wait unbounded here: this runs on the vblank ticker, which also
         * drives the guest's vblank handlers, the FIFO drain, and the fence
         * publication. A device removal (TDR) leaves the D3D12 fence unsignaled
         * forever and an INFINITE wait silently froze the ENTIRE emulation --
         * observed on LBP as a boot that died the moment the 2048x2048
         * offscreen RT was created (last log line), every guest thread parked.
         * Degrade loudly instead: bounded waits + the removal reason, then
         * carry on (subsequent D3D calls fail visibly but the game keeps
         * running headless). */
        for (int tries = 0; tries < 3; tries++) {
            if (WaitForSingleObject(s_d3d.fence_event, 2000) != WAIT_TIMEOUT)
                return;
            HRESULT rr = s_d3d.device->lpVtbl->GetDeviceRemovedReason(s_d3d.device);
            printf("[D3D12] wait_for_gpu STUCK %ds: want %llu got %llu removed=0x%08lX\n",
                   2 * (tries + 1),
                   (unsigned long long)s_d3d.fence_values[fi],
                   (unsigned long long)s_d3d.fence->lpVtbl->GetCompletedValue(s_d3d.fence),
                   (long)rr);
            fflush(stdout);
            if (rr != 0 /* S_OK: device still alive, keep waiting a bit */) {
                dred_dump("wait_for_gpu");
                break;
            }
        }
    }
}

static void move_to_next_frame(void)
{
    u64 current_fence = s_d3d.fence_values[s_d3d.frame_index];
    s_d3d.cmd_queue->lpVtbl->Signal(s_d3d.cmd_queue, s_d3d.fence, current_fence);

    s_d3d.frame_index = s_d3d.swap_chain->lpVtbl->GetCurrentBackBufferIndex(s_d3d.swap_chain);

    if (s_d3d.fence->lpVtbl->GetCompletedValue(s_d3d.fence) < s_d3d.fence_values[s_d3d.frame_index]) {
        s_d3d.fence->lpVtbl->SetEventOnCompletion(
            s_d3d.fence, s_d3d.fence_values[s_d3d.frame_index], s_d3d.fence_event);
        if (WaitForSingleObject(s_d3d.fence_event, 2000) == WAIT_TIMEOUT) {
            HRESULT rr = s_d3d.device->lpVtbl->GetDeviceRemovedReason(s_d3d.device);
            printf("[D3D12] FENCE STUCK 2s: want %llu got %llu removed=0x%08lX\n",
                   (unsigned long long)s_d3d.fence_values[s_d3d.frame_index],
                   (unsigned long long)s_d3d.fence->lpVtbl->GetCompletedValue(s_d3d.fence),
                   (long)rr);
            if (rr != 0) dred_dump("move_to_next_frame");
        }
    }

    s_d3d.fence_values[s_d3d.frame_index] = current_fence + 1;
}

/* ---------------------------------------------------------------------------
 * Render a frame (clear + present)
 * -----------------------------------------------------------------------*/

/* Write the mapped readback buffer (R8G8B8A8, row pitch = readback_pitch) out
 * as a 24-bit bottom-up BMP. Debug-only. */
/* Name prefix for the next readback dump: lets the same writer emit the
 * backbuffer and the screen-copy texture under different filenames. */
static const char* s_dump_name = NULL;

static void dump_backbuffer_bmp(void)
{
    if (!s_d3d.readback_buf) return;
    void* mapped = NULL;
    D3D12_RANGE rr = {0, (SIZE_T)s_d3d.readback_pitch * s_dh};   /* #253: display resource size */
    if (FAILED(s_d3d.readback_buf->lpVtbl->Map(s_d3d.readback_buf, 0, &rr, &mapped)) || !mapped)
        return;

    static int idx = 0;
    char path[512];
    const char* dir = getenv("CELLMARK_DUMP_DIR");   /* default: current dir */
    snprintf(path, sizeof(path), "%s%s%s_%03d.bmp",
             dir ? dir : "", dir ? "/" : "", s_dump_name ? s_dump_name : "frame", idx++);
    FILE* f = fopen(path, "wb");
    if (f) {
        u32 w = s_dw, h = s_dh;
        u32 padded = (w * 3 + 3) & ~3u;
        u32 imgsz  = padded * h;
        u32 filesz = 54 + imgsz;
        unsigned char hdr[54] = {0};
        hdr[0] = 'B'; hdr[1] = 'M';
        hdr[2]=filesz&0xFF; hdr[3]=(filesz>>8)&0xFF; hdr[4]=(filesz>>16)&0xFF; hdr[5]=(filesz>>24)&0xFF;
        hdr[10]=54; hdr[14]=40;
        hdr[18]=w&0xFF; hdr[19]=(w>>8)&0xFF; hdr[20]=(w>>16)&0xFF; hdr[21]=(w>>24)&0xFF;
        hdr[22]=h&0xFF; hdr[23]=(h>>8)&0xFF; hdr[24]=(h>>16)&0xFF; hdr[25]=(h>>24)&0xFF;
        hdr[26]=1; hdr[28]=24;
        hdr[34]=imgsz&0xFF; hdr[35]=(imgsz>>8)&0xFF; hdr[36]=(imgsz>>16)&0xFF; hdr[37]=(imgsz>>24)&0xFF;
        fwrite(hdr, 1, 54, f);
        unsigned char* row = (unsigned char*)malloc(padded);
        if (row) {
            memset(row, 0, padded);
            for (int y = (int)h - 1; y >= 0; y--) {   /* BMP is bottom-up */
                unsigned char* src = (unsigned char*)mapped + (u32)y * s_d3d.readback_pitch;
                for (u32 x = 0; x < w; x++) {
                    row[x*3+0] = src[x*4+2]; /* B */
                    row[x*3+1] = src[x*4+1]; /* G */
                    row[x*3+2] = src[x*4+0]; /* R */
                }
                fwrite(row, 1, padded, f);
            }
            free(row);
        }
        fclose(f);
        printf("[D3D12] dumped %s (%ux%u)\n", path, s_dw, s_dh);
    }
    D3D12_RANGE wr = {0, 0};
    s_d3d.readback_buf->lpVtbl->Unmap(s_d3d.readback_buf, 0, &wr);
}

/* ---------------------------------------------------------------------------
 * RSX frame capture ("rsxcap") -- a purpose-built GPU debugger. RSX_CAP=start
 * [:count[:stride]] snapshots whole frames to <RSX_CAP_DIR>/frame_NN/: a text
 * manifest of every op (in submission order, with full surface/shader/texture
 * state) plus a BMP of the backbuffer and every offscreen colour RT used that
 * frame. One frame, captured atomically -- no cross-frame guessing.
 * -----------------------------------------------------------------------*/

/* Shared 24-bit BMP writer for readbacks. R8G8B8A8_UNORM straight; the half/
 * float RT formats are |v|-tonemapped so signed/HDR data is visible. */
/* [NHL local patch] PIXEL_PROBE state (see the probe in the frame end): the
 * pixel to read back and its last value as 0xRRGGBB. */
static u32 s_probe_x = 0xFFFFFFFFu, s_probe_y = 0xFFFFFFFFu, s_probe_px = 0;
static int s_probe_mode = -1;      /* -1 unparsed, 0 off/watching, 1 bisecting, 2 done */
static int s_probe_k = -1;         /* draw index skipped this frame (-1 = none) */
static u32 s_probe_target = 0;

static int s_cap_alpha_mode = 0;   /* [#86] cap_write_bmp writes alpha as grey */
static void cap_write_bmp(const char* path, const void* mapped, u32 w, u32 h,
                          u32 pitch, u32 dxgi)
{
    FILE* f = fopen(path, "wb");
    if (!f) return;
    u32 rowb = (w * 3 + 3) & ~3u, datasz = rowb * h;
    u8 hdr[54] = {0};
    hdr[0] = 'B'; hdr[1] = 'M';
    *(u32*)(hdr+2) = 54 + datasz; *(u32*)(hdr+10) = 54; *(u32*)(hdr+14) = 40;
    *(int*)(hdr+18) = (int)w; *(int*)(hdr+22) = (int)h;
    *(u16*)(hdr+26) = 1; *(u16*)(hdr+28) = 24; *(u32*)(hdr+34) = datasz;
    fwrite(hdr, 1, 54, f);
    u8* line = (u8*)malloc(rowb);
    if (line) for (int y = (int)h - 1; y >= 0; y--) {   /* BMP is bottom-up */
        const u8* srow = (const u8*)mapped + (u64)y * pitch;
        memset(line, 0, rowb);
        for (u32 x = 0; x < w; x++) {
            float rv, gv, bv;
            if (dxgi == DXGI_FORMAT_R16G16B16A16_FLOAT) {
                const u16* hp = (const u16*)(srow + (u64)x * 8);
                float v[3];
                for (int c = 0; c < 3; c++) {
                    u16 hv = hp[c]; u32 s = (hv>>15)&1, e = (hv>>10)&0x1F, m = hv&0x3FF; float fv;
                    if (e == 0) fv = (float)m / 16777216.0f;
                    else { u32 fb = (s<<31)|((e-15+127)<<23)|(m<<13); memcpy(&fv,&fb,4); }
                    v[c] = fv;
                }
                rv = v[0]; gv = v[1]; bv = v[2];
            } else if (dxgi == DXGI_FORMAT_R32G32B32A32_FLOAT) {
                const float* fp = (const float*)(srow + (u64)x * 16);
                rv = fp[0]; gv = fp[1]; bv = fp[2];
            } else {
                const u8* p = srow + (u64)x * 4;
                rv = p[0]/255.0f; gv = p[1]/255.0f; bv = p[2]/255.0f;
                if (s_cap_alpha_mode) rv = gv = bv = p[3]/255.0f;   /* #86 CAP_ALPHA */
            }
            float ar = rv<0?-rv:rv, ag = gv<0?-gv:gv, ab = bv<0?-bv:bv;
            if (ar>1) ar=1; if (ag>1) ag=1; if (ab>1) ab=1;
            line[x*3+0] = (u8)(ab*255.0f);
            line[x*3+1] = (u8)(ag*255.0f);
            line[x*3+2] = (u8)(ar*255.0f);
        }
        fwrite(line, 1, rowb, f);
    }
    free(line);
    fclose(f);
    /* [#86] CAP_ALPHA=1: also write <name>_a.bmp with the alpha channel as grey
     * (RGBA8 targets) -- masks and encoded-HDR multipliers live there. */
    if (!s_cap_alpha_mode && dxgi == DXGI_FORMAT_R8G8B8A8_UNORM && getenv("CAP_ALPHA")) {
        char ap[600]; size_t L = strlen(path);
        if (L > 4 && L + 3 < sizeof ap) {
            memcpy(ap, path, L - 4); strcpy(ap + L - 4, "_a.bmp");
            s_cap_alpha_mode = 1;
            cap_write_bmp(ap, mapped, w, h, pitch, dxgi);
            s_cap_alpha_mode = 0;
        }
    }
}

/* Copy one GPU resource into a fresh readback buffer and write it as a BMP.
 * Self-contained: waits, resets the frame command list, copies, executes,
 * waits, maps. `prior` is the resource's current tracked state (restored after
 * the copy so the caller's state tracking stays valid). */
static void cap_readback_write(ID3D12Resource* res, u32 w, u32 h, u32 dxgi,
                               D3D12_RESOURCE_STATES prior, u32 fi, const char* path)
{
    if (!res || !w || !h) return;
    u32 bpp = (dxgi == DXGI_FORMAT_R16G16B16A16_FLOAT) ? 8 :
              (dxgi == DXGI_FORMAT_R32G32B32A32_FLOAT) ? 16 : 4;
    u32 pitch = (w * bpp + 255) & ~255u;

    ID3D12Resource* rb = NULL;
    D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC rd = {0};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = (u64)pitch * h; rd.Height = 1; rd.DepthOrArraySize = 1;
    rd.MipLevels = 1; rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(s_d3d.device->lpVtbl->CreateCommittedResource(
            s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_COPY_DEST, NULL,
            &IID_ID3D12Resource, (void**)&rb)) || !rb)
        return;

    wait_for_gpu();
    s_d3d.cmd_allocators[fi]->lpVtbl->Reset(s_d3d.cmd_allocators[fi]);
    s_d3d.cmd_list->lpVtbl->Reset(s_d3d.cmd_list, s_d3d.cmd_allocators[fi], NULL);

    D3D12_RESOURCE_BARRIER b = {0};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    if (prior != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        b.Transition.StateBefore = prior;
        b.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
        s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &b);
    }

    D3D12_TEXTURE_COPY_LOCATION dst = {0}, src = {0};
    dst.pResource = rb; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint.Format = (DXGI_FORMAT)dxgi;
    dst.PlacedFootprint.Footprint.Width = w; dst.PlacedFootprint.Footprint.Height = h;
    dst.PlacedFootprint.Footprint.Depth = 1; dst.PlacedFootprint.Footprint.RowPitch = pitch;
    src.pResource = res; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    s_d3d.cmd_list->lpVtbl->CopyTextureRegion(s_d3d.cmd_list, &dst, 0, 0, 0, &src, NULL);
        /* TEXUP_DBG=1: texture uploads per second. A cache that misses every
         * draw re-uploads megabytes per frame, which is the kind of GPU work
         * that trips a 2-second TDR while looking like nothing in a draw count. */
        { static int td = -1; if (td < 0) td = getenv("TEXUP_DBG") ? 1 : 0;
          if (td) { static long long n = 0, bytes = 0; static clock_t t0 = 0;
              n++; bytes += (long long)pitch * h;
              const clock_t now = clock();
              if (!t0) t0 = now;
              if (now - t0 >= CLOCKS_PER_SEC) {
                  fprintf(stderr, "[TEXUP] %lld uploads/s, %.1f MB/s\n",
                          n, bytes / 1048576.0);
                  n = 0; bytes = 0; t0 = now; } } }

    if (prior != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b.Transition.StateAfter  = prior;
        s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &b);
    }

    s_d3d.cmd_list->lpVtbl->Close(s_d3d.cmd_list);
    ID3D12CommandList* cl[] = { (ID3D12CommandList*)s_d3d.cmd_list };
    s_d3d.cmd_queue->lpVtbl->ExecuteCommandLists(s_d3d.cmd_queue, 1, cl);
    wait_for_gpu();

    void* mp = NULL; D3D12_RANGE rr = {0, (SIZE_T)pitch * h};
    if (SUCCEEDED(rb->lpVtbl->Map(rb, 0, &rr, &mp)) && mp) {
        if (path) cap_write_bmp(path, mp, w, h, pitch, dxgi);
        else if (s_probe_x < w && s_probe_y < h) {   /* [NHL local patch] PIXEL_PROBE readback */
            const u8* px = (const u8*)mp + (size_t)s_probe_y * pitch + (size_t)s_probe_x * 4u;
            s_probe_px = (u32)px[0] << 16 | (u32)px[1] << 8 | px[2];
        }
        D3D12_RANGE wr = {0, 0};
        rb->lpVtbl->Unmap(rb, 0, &wr);
    }
    rb->lpVtbl->Release(rb);
}

/* [NHL local patch #108] Per-draw GPU queries in F9-captured presents: a
 * pipeline-statistics query (vertices in, primitives into/out of the clipper,
 * pixel-shader invocations) and an occlusion query (samples passing depth/
 * stencil) around every guest draw, appended to the manifest's DRAW lines.
 * Separates "never rasterised" (culled / off-screen / degenerate) from
 * "rasterised but failed depth" from "passed but wrote no visible colour" for
 * every draw at once. QSTATS=0 disables. */
static ID3D12QueryHeap* s_q_ps = NULL;
static ID3D12QueryHeap* s_q_oc = NULL;
static ID3D12Resource*  s_q_rb = NULL;
static int s_q_state = 0;   /* 0 untried, 1 ready, -1 unavailable */
static int s_q_frame = 0;   /* recording queries in the current render pass */
static int s_q_ready = 0;   /* s_q_rb holds the last recorded pass */
static u32 s_q_n = 0;       /* highest queried draw index + 1 */
static u32 s_exec_loop_count = 0;   /* [#114] draw_count the last VP exec loop iterated */
static u8  s_q_used[MAX_DRAWS];
#define Q_OCC_BASE ((u64)MAX_DRAWS * sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS))
static int q_init(void)
{
    if (s_q_state) return s_q_state > 0;
    s_q_state = -1;
    { const char* e = getenv("QSTATS"); if (e && e[0] == '0') return 0; }
    D3D12_QUERY_HEAP_DESC qd = {0};
    qd.Type = D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS; qd.Count = MAX_DRAWS;
    if (FAILED(s_d3d.device->lpVtbl->CreateQueryHeap(s_d3d.device, &qd,
            &IID_ID3D12QueryHeap, (void**)&s_q_ps)) || !s_q_ps) {
        printf("[QSTATS] pipeline-statistics heap unavailable\n"); return 0; }
    qd.Type = D3D12_QUERY_HEAP_TYPE_OCCLUSION;
    if (FAILED(s_d3d.device->lpVtbl->CreateQueryHeap(s_d3d.device, &qd,
            &IID_ID3D12QueryHeap, (void**)&s_q_oc)) || !s_q_oc) {
        printf("[QSTATS] occlusion heap unavailable\n"); return 0; }
    D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC rd = {0};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = Q_OCC_BASE + (u64)MAX_DRAWS * 8u; rd.Height = 1; rd.DepthOrArraySize = 1;
    rd.MipLevels = 1; rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(s_d3d.device->lpVtbl->CreateCommittedResource(s_d3d.device, &hp,
            D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, NULL,
            &IID_ID3D12Resource, (void**)&s_q_rb)) || !s_q_rb) {
        printf("[QSTATS] readback buffer failed\n"); return 0; }
    s_q_state = 1;
    printf("[QSTATS] per-draw queries ready (F9 captures)\n");
    return 1;
}

static void rsx_capture_frame(u32 fi, u32 ndraws, u32 capidx)
{
    /* [#108] this pass's query results (resolved before its Close) */
    const D3D12_QUERY_DATA_PIPELINE_STATISTICS* qps = NULL; const u64* qoc = NULL;
    if (s_q_ready && s_q_rb) {
        wait_for_gpu();
        void* qp = NULL; D3D12_RANGE rr = {0, (SIZE_T)(Q_OCC_BASE + (u64)MAX_DRAWS * 8u)};
        if (SUCCEEDED(s_q_rb->lpVtbl->Map(s_q_rb, 0, &rr, &qp)) && qp) {
            qps = (const D3D12_QUERY_DATA_PIPELINE_STATISTICS*)qp;
            qoc = (const u64*)((const u8*)qp + Q_OCC_BASE);
        }
    }
    char dir[300]; const char* base = getenv("RSX_CAP_DIR");
    if (!base || !*base) base = "rsxcap";
    CreateDirectoryA(base, NULL);
    snprintf(dir, sizeof dir, "%s/frame_%02u", base, capidx);
    CreateDirectoryA(dir, NULL);
    char path[512];

    /* Manifest: every op in submission order with full state. */
    snprintf(path, sizeof path, "%s/manifest.txt", dir);
    FILE* mf = fopen(path, "w");
    if (mf) {
        { extern u32 s_present_id_get(void);
          fprintf(mf, "frame_count=%llu draws=%u backbuffer=%ux%u flip_id=%u live_cull=%ld live_depth=%ld qstats=%d exec_loop_count=%u draw_count_now=%u\n",
                  (unsigned long long)s_d3d.frame_count, ndraws, s_d3d.width, s_d3d.height,
                  s_present_id_get(), (long)s_live_cull, (long)s_live_depth, qps ? 1 : 0,
                  s_exec_loop_count, s_d3d.draw_count); }
        for (int i = 0; i < MAX_OFF_RTS; i++)
            if (s_d3d.off_rt[i].res && s_d3d.off_rt[i].used)
                fprintf(mf, "offrt[%d] off=0x%08X %ux%u dxgi=%u\n", i, s_d3d.off_rt[i].off,
                        s_d3d.off_rt[i].w, s_d3d.off_rt[i].h, s_d3d.off_rt[i].dxgi);
        /* [#244] the whole offscreen RT table (unused slots too) for bug reports: stale
         * entries and evictions were behind the stuck image and the garbage mod logos */
        { extern unsigned cellGcm_flip_request_count(void);
          const u32 fc = cellGcm_flip_request_count(); int n = 0;
          for (int i = 0; i < MAX_OFF_RTS; i++) if (s_d3d.off_rt[i].res) n++;
          fprintf(mf, "offrt-table %d/%d slots in use, flip %u\n", n, MAX_OFF_RTS, fc);
          for (int i = 0; i < MAX_OFF_RTS; i++) {
              const OffRT* r = &s_d3d.off_rt[i];
              if (!r->res) continue;
              fprintf(mf, "  slot %2d off=0x%08X %ux%u dxgi=%u age=%u flips%s%s%s\n", i, r->off, r->w, r->h, r->dxgi,
                      fc - r->last_flip, r->used ? " used" : "", r->blit_frame ? " blit-dst" : "", wb_kind(r->off));
          } }
        fprintf(mf, "--- ops ---\n");
        for (u32 d = 0; d < ndraws && d < MAX_DRAWS; d++) {
            const D3D12DrawRecord* r = &s_d3d.draws[d];
            if (r->is_clear)
                fprintf(mf, "op%03u CLEAR rt=0x%08X mrt=0x%X/0x%X/0x%X cc=(%.3f,%.3f,%.3f,%.3f)\n",
                        d, r->rt_off, r->rt_mrt[0], r->rt_mrt[1], r->rt_mrt[2],
                        r->cc[0], r->cc[1], r->cc[2], r->cc[3]);
            else {
                fprintf(mf, "op%03u DRAW  rt=0x%08X mrt=0x%X/0x%X/0x%X vp=%u,%u,%ux%u fp=0x%08X vs=%d "
                            "n=%u cmask=%X blend=%d bk=0x%X t0=0x%08X t1=0x%08X t2=0x%08X t3=0x%08X"
                            " ph=%d st=0x%08X/%02X at=0x%X ctl=0x%X surf=0x%X ct=0x%X key=0x%08X vsh=0x%08X",
                        d, r->rt_off, r->rt_mrt[0], r->rt_mrt[1], r->rt_mrt[2],
                        r->vp_x, r->vp_y, r->vp_w, r->vp_h,
                        r->fp_addr, r->vs_idx, r->vertex_count, r->cmask, r->blend, r->blend_key,
                        r->tex[0].raw, r->tex[1].raw, r->tex[2].raw, r->tex[3].raw,
                        r->placeholder, r->stencil_key, r->stencil_ref, r->alpha_ctl,
                        (u32)r->fp_exp32, r->surf_raw, r->surf_ct, r->fp_key,
                        /* [#107] VS hash (vsdump/vs_<hash>.hlsl) */
                        (r->vs_idx >= 0 && r->vs_idx < VP_VS_CACHE) ? s_d3d.vp_vs[r->vs_idx].hash : 0u);
                /* [#108] cull key, guest viewport y/z, SHADER_WINDOW, then the GPU's
                 * counts: ia = vertices in, cin/cout = primitives into/out of the
                 * clipper, ps = pixel-shader invocations, occ = samples passing
                 * depth/stencil. "q=-" = no query (draw skipped or not recorded). */
                fprintf(mf, " cull=%X vpsy=%g vpoy=%g vpsz=%g vpoz=%g win=0x%X isvp=%d ex=%u expass=%u",
                        r->cull, r->vps_y, r->vpo_y, r->vps_z, r->vpo_z, r->winorg,
                        r->is_vp, r->exec, r->exec_pass);
                { static const char kc[4] = { '-', 'R', 'S', 'C' };   /* #118 bound resource per unit */
                  fprintf(mf, " bnd=%c%u,%c%u,%c%u,%c%u",
                          kc[r->bound_kind[0] & 3], r->bound_id[0], kc[r->bound_kind[1] & 3], r->bound_id[1],
                          kc[r->bound_kind[2] & 3], r->bound_id[2], kc[r->bound_kind[3] & 3], r->bound_id[3]); }
                if (qps && d < s_q_n && s_q_used[d])
                    fprintf(mf, " ia=%llu vsi=%llu cin=%llu cout=%llu ps=%llu occ=%llu",
                            (unsigned long long)qps[d].IAVertices, (unsigned long long)qps[d].VSInvocations,
                            (unsigned long long)qps[d].CInvocations, (unsigned long long)qps[d].CPrimitives,
                            (unsigned long long)qps[d].PSInvocations, (unsigned long long)qoc[d]);
                else if (qps) fprintf(mf, " q=-");
                fprintf(mf, "\n");
                /* [#108] CAP_VSC=<slot,slot,...> (+ CAP_VSC_RT=<hex rt>): those VS
                 * constants as the GPU reads them for this draw (exec-time slot). */
                { static int nv = -1; static int vs_[16]; static u32 vrt = 0;
                  if (nv < 0) { nv = 0; const char* e = getenv("CAP_VSC");
                      while (e && *e && nv < 16) { char* end; long v = strtol(e, &end, 10);
                          if (end == e) break; if (v >= 0 && v < RSX_MAX_VERTEX_CONSTANTS) vs_[nv++] = (int)v;
                          e = (*end == ',') ? end + 1 : end; }
                      const char* r2 = getenv("CAP_VSC_RT"); vrt = r2 ? (u32)strtoul(r2, NULL, 16) : 0u; }
                  if (nv > 0 && s_d3d.vp_cb_mapped && (!vrt || r->rt_off == vrt)) {
                      const float* c = (const float*)((const char*)s_d3d.vp_cb_mapped
                          + ((u64)(s_d3d.vp_parity ^ 1) * MAX_DRAWS + r->cb_slot) * VP_CB_STRIDE);
                      fprintf(mf, "      vsc");
                      for (int i = 0; i < nv; i++)
                          fprintf(mf, " c%d=(%.4g,%.4g,%.4g,%.4g)", vs_[i], c[vs_[i]*4], c[vs_[i]*4+1],
                                  c[vs_[i]*4+2], c[vs_[i]*4+3]);
                      fprintf(mf, "\n"); }
                  /* CAP_VTX=<n>: n input positions (attr0, as uploaded for the GPU)
                   * spread over the draw, each with its NDC under c12-c15 (the
                   * scene view-projection the skinned-character VS uses directly). */
                  static int nvx = -1;
                  if (nvx < 0) { const char* e = getenv("CAP_VTX"); nvx = e ? atoi(e) : 0; if (nvx > 32) nvx = 32; }
                  if (nvx > 0 && s_d3d.vp_vb_mapped && r->vertex_count && (!vrt || r->rt_off == vrt)) {
                      const float* c = (const float*)((const char*)s_d3d.vp_cb_mapped
                          + ((u64)(s_d3d.vp_parity ^ 1) * MAX_DRAWS + r->cb_slot) * VP_CB_STRIDE);
                      fprintf(mf, "      vtx");
                      for (int k = 0; k < nvx; k++) {
                          const u32 p = (u32)((u64)k * r->vertex_count / (u32)nvx);
                          u32 rec = p;
                          /* both views are per-parity halves; capture runs after the flip */
                          const u64 par = (u64)(s_d3d.vp_parity ^ 1);
                          if (r->indexed && s_d3d.vp_ib_mapped)
                              rec = ((const u32*)s_d3d.vp_ib_mapped)[par * VP_MAX_INDICES + r->ib_first + p];
                          if ((u64)r->vb_byte_offset / 256u + rec >= (u64)MAX_VERTICES) break;
                          const float* v = (const float*)((const u8*)s_d3d.vp_vb_mapped
                              + par * MAX_VERTICES * 256u + r->vb_byte_offset + (u64)rec * 256u);
                          float cl[4];
                          for (int q = 0; q < 4; q++)
                              cl[q] = v[0]*c[12*4+q] + v[1]*c[13*4+q] + v[2]*c[14*4+q] + c[15*4+q];
                          fprintf(mf, " (%.3g,%.3g,%.3g,%.3g|%.2f,%.2f,w%.3g)", v[0], v[1], v[2], v[3],
                                  cl[3] != 0.0f ? cl[0]/cl[3] : 0.0f, cl[3] != 0.0f ? cl[1]/cl[3] : 0.0f, cl[3]);
                      }
                      fprintf(mf, "\n");
                      /* every triangle (list order) projected with c12-c15: how many
                       * are on screen, and their NDC winding (y up). D3D sees the
                       * same winding: our NDC y+ is the top of the target. */
                      {   /* the VP path submits every draw as a triangle list */
                          const u64 par = (u64)(s_d3d.vp_parity ^ 1);
                          u32 on = 0, ccw = 0, cwn = 0, deg = 0, behind = 0;
                          for (u32 t = 0; t + 2 < r->vertex_count && t < 300000u; t += 3) {
                              float sx[3], sy[3]; int bad = 0, offs = 0;
                              for (int q = 0; q < 3; q++) {
                                  u32 rec = t + (u32)q;
                                  if (r->indexed && s_d3d.vp_ib_mapped)
                                      rec = ((const u32*)s_d3d.vp_ib_mapped)[par * VP_MAX_INDICES + r->ib_first + t + q];
                                  if ((u64)r->vb_byte_offset / 256u + rec >= (u64)MAX_VERTICES) { bad = 1; break; }
                                  const float* v = (const float*)((const u8*)s_d3d.vp_vb_mapped
                                      + par * MAX_VERTICES * 256u + r->vb_byte_offset + (u64)rec * 256u);
                                  const float x = v[0]*c[48] + v[1]*c[52] + v[2]*c[56] + c[60];
                                  const float y = v[0]*c[49] + v[1]*c[53] + v[2]*c[57] + c[61];
                                  const float w = v[0]*c[51] + v[1]*c[55] + v[2]*c[59] + c[63];
                                  if (!(w > 1e-6f)) { bad = 2; break; }
                                  sx[q] = x / w; sy[q] = y / w;
                                  if (sx[q] < -1 || sx[q] > 1 || sy[q] < -1 || sy[q] > 1) offs++;
                              }
                              if (bad == 2) { behind++; continue; }
                              if (bad) break;
                              if (offs == 3) continue;
                              on++;
                              const float a = (sx[1]-sx[0])*(sy[2]-sy[0]) - (sx[2]-sx[0])*(sy[1]-sy[0]);
                              if (a > 1e-9f) ccw++; else if (a < -1e-9f) cwn++; else deg++;
                          }
                          fprintf(mf, "      tri12 topo=%u on=%u ccw=%u cw=%u deg=%u behind=%u\n",
                                  r->topology, on, ccw, cwn, deg, behind);
                      } } }
                { int any = 0;   /* [#86] the units past 3 (scene FPs use up to 16) */
                  for (int u = 4; u < D3D_TEX_UNITS; u++)
                      if (r->tex[u].raw) {
                          if (!any) { fprintf(mf, "      units4+:"); any = 1; }
                          fprintf(mf, " t%d=0x%08X%s", u, r->tex[u].raw, r->tex[u].set ? "" : "(unset)");
                      }
                  if (any) fprintf(mf, "\n"); }
                for (int u = 0; u < 4; u++)
                    if (r->tex[u].set)
                        fprintf(mf, "      t%d off=0x%08X fmt=0x%02X (%s%s) %ux%u remap=0x%04X\n", u,
                                r->tex[u].off, r->tex[u].fmt,
                                (r->tex[u].fmt & 0x20) ? "LN" : "SZ",
                                (r->tex[u].fmt & 0x40) ? ",NR" : "",
                                r->tex[u].w, r->tex[u].h, r->tex[u].ctrl1 & 0xFFFF);
            }
        }
        /* Per-draw VP constant banks for the first few offscreen-RT geometry
         * draws: c[0..3] is the projection row-major for most CG VPs; also scan
         * for the largest non-zero slot so a mis-placed MVP is obvious. Records
         * live at slot==draw-index; parity is post-^1 here (capture is after the
         * frame's vp_parity flip), so read the other half. */
        if (s_d3d.vp_cb_mapped) {
            u32 par = (u32)(s_d3d.vp_parity ^ 1);
            fprintf(mf, "--- vp constants (first offscreen geometry draws) ---\n");
            int shown = 0;
            for (u32 d = 0; d < ndraws && d < MAX_DRAWS && shown < 4; d++) {
                const D3D12DrawRecord* r = &s_d3d.draws[d];
                if (r->is_clear || !r->rt_off) continue;
                const float* c = (const float*)((const char*)s_d3d.vp_cb_mapped
                    + ((u64)par * MAX_DRAWS + d) * VP_CB_STRIDE);
                int last_nz = -1; float maxabs = 0.0f;
                for (int i = 0; i < RSX_MAX_VERTEX_CONSTANTS * 4; i++) {
                    float a = c[i] < 0 ? -c[i] : c[i];
                    if (a > 1e-9f) last_nz = i;
                    if (a > maxabs) maxabs = a;
                }
                fprintf(mf, "op%03u vs=%d lastNZslot=%d maxabs=%.3f\n",
                        d, r->vs_idx, last_nz / 4, maxabs);
                if (shown == 0) {   /* full non-zero slot dump for the 1st draw */
                    for (int s = 0; s <= last_nz / 4; s++) {
                        const float* v = c + s * 4;
                        if ((v[0]||v[1]||v[2]||v[3]))
                            fprintf(mf, "   c[%3d] = %9.4f %9.4f %9.4f %9.4f\n",
                                    s, v[0], v[1], v[2], v[3]);
                    }
                }
                shown++;
            }
        }
        fclose(mf);
    }
    if (qps) { D3D12_RANGE wr = {0, 0}; s_q_rb->lpVtbl->Unmap(s_q_rb, 0, &wr); }
    s_q_ready = 0;

    /* Backbuffer (in PRESENT state at capture time) + every used offscreen RT. */
    snprintf(path, sizeof path, "%s/backbuffer.bmp", dir);
    cap_readback_write(s_d3d.render_targets[fi], s_dw, s_dh,   /* #253 */
                       DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_STATE_PRESENT, fi, path);
    { static int bbo = -1; if (bbo < 0) bbo = getenv("CAP_BB_ONLY") ? 1 : 0;   /* [#112] fast bursts */
      if (bbo) { printf("[RSXCAP] frame %llu -> %s (%u ops, backbuffer only)\n",
                        (unsigned long long)s_d3d.frame_count, dir, ndraws); return; } }
    for (int i = 0; i < MAX_OFF_RTS; i++) {
        OffRT* r = &s_d3d.off_rt[i];
        if (!r->res || !r->used) continue;
        snprintf(path, sizeof path, "%s/rt_%08X.bmp", dir, r->off);
        cap_readback_write(r->res, r->rw, r->rh, r->dxgi, r->st, fi, path);   /* #253 */
    }
    printf("[RSXCAP] frame %llu -> %s (%u ops)\n",
           (unsigned long long)s_d3d.frame_count, dir, ndraws);
}

/* Decompile the captured RSX vertex program to HLSL and build the VP PSO
 * (decompiled VS + atlas alpha-test PS). One-shot per program. */
static void compile_vp(void)
{
    extern int rsx_vp_decompile(const uint8_t*, u32, char*, u32);
    const rsx_state* st = s_d3d.current_rsx_state;
    if (!st || st->vp_ucode_bytes < 16 || !s_d3d.vp_root_sig) return;

    static char hlsl[64 * 1024];
    int ni = rsx_vp_decompile(st->vp_ucode, st->vp_ucode_bytes, hlsl, sizeof hlsl);
    /* Does this VP read vp_c[0..3]? Gates the garbage-projection fallback:
     * programs keeping their MVP elsewhere (gcm/cube: c[256..259]) must not
     * have c[0..3] stomped nor the viewport z-lane overridden. */
    s_d3d.vp_uses_c03 = (strstr(hlsl, "vp_c[0]") || strstr(hlsl, "vp_c[1]") ||
                         strstr(hlsl, "vp_c[2]") || strstr(hlsl, "vp_c[3]")) ? 1 : 0;
    if (ni <= 0) { printf("[VP] decompile failed (%d)\n", ni); s_d3d.vp_compiled_bytes = st->vp_ucode_bytes; return; }
    if (getenv("VP_DUMP")) {
        FILE* vf = fopen("vp_dump.hlsl", "w");
        if (vf) { fwrite(hlsl, 1, strlen(hlsl), vf); fclose(vf);
                  printf("[VP] dumped vp_dump.hlsl (%d instrs)\n", ni); }
        printf("[VPRAW] first 3 instrs (%u ucode bytes):\n", st->vp_ucode_bytes);
        for (u32 _q = 0; _q < 48 && _q < st->vp_ucode_bytes; _q += 16)
            printf("[VPRAW]  d0=%02X%02X%02X%02X d1=%02X%02X%02X%02X d2=%02X%02X%02X%02X d3=%02X%02X%02X%02X\n",
                st->vp_ucode[_q+0],st->vp_ucode[_q+1],st->vp_ucode[_q+2],st->vp_ucode[_q+3],
                st->vp_ucode[_q+4],st->vp_ucode[_q+5],st->vp_ucode[_q+6],st->vp_ucode[_q+7],
                st->vp_ucode[_q+8],st->vp_ucode[_q+9],st->vp_ucode[_q+10],st->vp_ucode[_q+11],
                st->vp_ucode[_q+12],st->vp_ucode[_q+13],st->vp_ucode[_q+14],st->vp_ucode[_q+15]);
    }

    /* Pixel shader mirrors dbgfont's FP: sample the atlas coverage at TEXCOORD0
     * (VP output o[7]), alpha-test at 0.5, output the vertex color (o[1]). */
    static const char ps[] =
        "Texture2D tex : register(t0); SamplerState smp : register(s0);\n"
        "struct PSIn{ float4 pos:SV_Position; float4 col0:COLOR0; float4 col1:COLOR1; float4 fog:FOG;\n"
        "  float4 t0:TEXCOORD0; float4 t1:TEXCOORD1; float4 t2:TEXCOORD2; float4 t3:TEXCOORD3;\n"
        "  float4 t4:TEXCOORD4; float4 t5:TEXCOORD5; float4 t6:TEXCOORD6; float4 t7:TEXCOORD7; };\n"
        "float4 main(PSIn i):SV_TARGET{ float cov = tex.Sample(smp, i.t0.xy).r;\n"
        "  if (cov <= 0.5) discard; return float4(i.col0.rgb, 1); }\n";

    ID3DBlob *vb=NULL,*pb=NULL,*e=NULL;
    HRESULT hr = D3DCompile(hlsl, strlen(hlsl), "vp", NULL, NULL, "main", "vs_5_0", 0, 0, &vb, &e);
    if (FAILED(hr)) { printf("[VP] VS compile FAIL: %s\n", e?(const char*)e->lpVtbl->GetBufferPointer(e):"?"); if(e)e->lpVtbl->Release(e); s_d3d.vp_compiled_bytes=st->vp_ucode_bytes; return; }
    hr = D3DCompile(ps, sizeof(ps)-1, "vpps", NULL, NULL, "main", "ps_5_0", 0, 0, &pb, &e);
    if (FAILED(hr)) { printf("[VP] PS compile FAIL: %s\n", e?(const char*)e->lpVtbl->GetBufferPointer(e):"?"); if(e)e->lpVtbl->Release(e); if(vb)vb->lpVtbl->Release(vb); s_d3d.vp_compiled_bytes=st->vp_ucode_bytes; return; }

    /* The decompiled VS declares inputs a0:ATTR0 .. a15:ATTR15 and the HLSL
     * compiler keeps whichever the program body reads. D3D12 requires EVERY VS
     * input to have a matching input-layout element (by semantic name+index),
     * so declare all 16 ATTR slots. (The old single "POSITION" element matched
     * NO VS input once the decompiler switched to ATTR semantics -> PSO failed
     * with E_INVALIDARG for every VP, blanking cellmark's text and vkcube.)
     * The VP path uploads only attrib0 to vp_vb (one float4/vertex), so every
     * slot reads that same float4 at offset 0; attributes other than position
     * therefore alias attrib0 for now (colours/uv wrong until the VP path
     * uploads multiple attributes), but geometry is correct and the PSO is
     * valid. */
    D3D12_INPUT_ELEMENT_DESC il[16];
    for (int _e = 0; _e < 16; _e++) {
        il[_e].SemanticName = "ATTR";
        il[_e].SemanticIndex = _e;
        il[_e].Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        il[_e].InputSlot = 0;
        /* 256-byte generic VP vertex: every ATTRi is a float4 slot at i*16
         * (read_vp_vertex converts each enabled RSX attrib by type; disabled
         * slots hold (0,0,0,1)). Covers tiny3d (a0/a3/a8), SDK gcm samples
         * (a0/a1/a2), dbgfont -- no aliasing. */
        il[_e].AlignedByteOffset = (UINT)(_e * 16);
        il[_e].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
        il[_e].InstanceDataStepRate = 0;
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {0};
    pd.pRootSignature = s_d3d.vp_root_sig;
    pd.VS.pShaderBytecode = vb->lpVtbl->GetBufferPointer(vb); pd.VS.BytecodeLength = vb->lpVtbl->GetBufferSize(vb);
    pd.PS.pShaderBytecode = pb->lpVtbl->GetBufferPointer(pb); pd.PS.BytecodeLength = pb->lpVtbl->GetBufferSize(pb);
    pd.InputLayout.pInputElementDescs = il; pd.InputLayout.NumElements = 16;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = UINT_MAX;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1; pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pd.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    /* Depth test ON (LESS_EQUAL, matching the guest's rsxtiny_DepthTestFunc 515 /
     * CELL_GCM_LEQUAL). Without it the cube's faces drew in submission order and
     * back faces overwrote the front -> you saw through the near face to the
     * darker interior. The depth buffer is bound + cleared to 1.0 each frame. */
    pd.DepthStencilState.DepthEnable = TRUE;
    pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    pd.DepthStencilState.StencilEnable = FALSE;
    pd.SampleDesc.Count = 1;
    hr = s_d3d.device->lpVtbl->CreateGraphicsPipelineState(s_d3d.device, &pd, &IID_ID3D12PipelineState, (void**)&s_d3d.pipeline_state_vp);

    /* Second PSO: same VS but a COLOUR-only PS (no texture sample, no alpha
     * discard) for untextured 3D geometry (vkcube's cube). The atlas PS above
     * samples a texture and discards where coverage<=0.5, which blanks any draw
     * with no atlas bound. */
    {
        static const char ps_col[] =
            "struct PSIn{ float4 pos:SV_Position; float4 col0:COLOR0; float4 col1:COLOR1; float4 fog:FOG;\n"
            "  float4 t0:TEXCOORD0; float4 t1:TEXCOORD1; float4 t2:TEXCOORD2; float4 t3:TEXCOORD3;\n"
            "  float4 t4:TEXCOORD4; float4 t5:TEXCOORD5; float4 t6:TEXCOORD6; float4 t7:TEXCOORD7; };\n"
            "float4 main(PSIn i):SV_TARGET{ return float4(i.col0.rgb, 1); }\n";
        ID3DBlob *pcb=NULL,*ec=NULL;
        if (SUCCEEDED(D3DCompile(ps_col, sizeof(ps_col)-1, "vppsc", NULL, NULL,
                                 "main", "ps_5_0", 0, 0, &pcb, &ec)) && pcb) {
            pd.PS.pShaderBytecode = pcb->lpVtbl->GetBufferPointer(pcb);
            pd.PS.BytecodeLength  = pcb->lpVtbl->GetBufferSize(pcb);
            HRESULT hr2 = s_d3d.device->lpVtbl->CreateGraphicsPipelineState(
                s_d3d.device, &pd, &IID_ID3D12PipelineState,
                (void**)&s_d3d.pipeline_state_vp_color);
            printf(SUCCEEDED(hr2) ? "[VP] colour pipeline ready\n"
                                  : "[VP] colour PSO FAIL (0x%08lX)\n", hr2);
            pcb->lpVtbl->Release(pcb);
        }
        if (ec) ec->lpVtbl->Release(ec);
    }
    /* Keep the VS bytecode for guest-FP PSO builds (vp_get_fp_pso); bump the
     * generation so cached FP PSOs built against the old VS are rebuilt. */
    if (s_d3d.vp_vs_blob) s_d3d.vp_vs_blob->lpVtbl->Release(s_d3d.vp_vs_blob);
    s_d3d.vp_vs_blob = vb;
    s_d3d.vp_gen++;
    pb->lpVtbl->Release(pb);
    s_d3d.vp_compiled_bytes = st->vp_ucode_bytes;
    if (SUCCEEDED(hr)) { s_d3d.vp_ready = 1; printf("[VP] pipeline ready (%d instrs)\n", ni); }
    else {
        printf("[VP] PSO creation FAIL (0x%08lX)\n", hr);
        /* Drain the debug layer's message queue to get the EXACT validation
         * reason (E_INVALIDARG is otherwise opaque). One-shot. */
        ID3D12InfoQueue* iq = NULL;
        if (SUCCEEDED(s_d3d.device->lpVtbl->QueryInterface(
                s_d3d.device, &IID_ID3D12InfoQueue, (void**)&iq)) && iq) {
            UINT64 n = iq->lpVtbl->GetNumStoredMessages(iq);
            for (UINT64 mi = 0; mi < n; mi++) {
                SIZE_T len = 0;
                iq->lpVtbl->GetMessage(iq, mi, NULL, &len);
                D3D12_MESSAGE* m = (D3D12_MESSAGE*)malloc(len);
                if (m && SUCCEEDED(iq->lpVtbl->GetMessage(iq, mi, m, &len)))
                    printf("[VP][DBG] %s\n", m->pDescription);
                free(m);
            }
            iq->lpVtbl->Release(iq);
        }
    }
}

/* Build (or fetch) a PSO pairing the current decompiled VS with the guest's
 * FRAGMENT program at fp_addr: read the FP ucode from guest memory, decompile
 * to HLSL (rsx_fp_decompiler), compile, and cache. This replaces the two
 * hardcoded pixel shaders for draws whose FP we can translate -- e.g.
 * gcm/cube's plasma FP `c = tex2D(t0, uv).x; out = (c, 0, c, 1)`. */
#include "rsx_fp_decompiler.h"

/* Hash + compile the CURRENT rsx_state vertex program into the VS cache;
 * returns the cache slot (or -1). Called at draw-record time so each draw
 * carries the VP that was loaded when it was submitted. */
static u32 vp_hash_ucode(const u8* p, u32 n)
{
    u32 h = 2166136261u;
    for (u32 i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h ? h : 1u;
}

/* gpu = 1: the #125 GPU vertex-fetch variant of the same program (its own slot,
 * keyed by hash ^ VS_GPU_SALT, so both variants can be live at once). */
static int vp_get_vs_ex(const rsx_state* st, int gpu);
static int vp_get_vs(const rsx_state* st) { return vp_get_vs_ex(st, 0); }
static u32 current_rt_off(u32* out_w, u32* out_h, u32 out_mrt[3]);   /* defined below */
static int s_depth_rec;   /* #190 (defined with the draw entry points) */
/* [NHL local patch #225d] The CPU-converted vertex paths (draw types GPU fetch does
 * not take) asked for their VS synchronously: the last 0.5-1.3 s match-start
 * stalls after #225c. Same rule as the GPU-fetch record: display draws (not the
 * shadow pass) compile now, a <= 6-vertex draw only with a tiny VS; the rest
 * compile in the background and the exec loop skips them until ready. */
static int vp_get_vs_rec(u32 count)
{
    { const u32 rt = current_rt_off(NULL, NULL, NULL);   /* #229: one-off targets compile now too */
      s_async_sync = !async_all() && !s_depth_rec && (rt == 0 || rt_oneoff(rt)); }
    s_rec_small = !async_all() && count <= 6;
    const int idx = vp_get_vs(s_d3d.current_rsx_state);
    s_async_sync = 1; s_rec_small = 0;
    return idx;
}
static int vp_get_vs_slow(const rsx_state* st, int gpu, u32* out_hash);
/* [NHL local patch #183] Per-draw VS lookup memo: while the microcode store and
 * program start are unchanged (rsx_state.vp_gen), the draw's VS is the previous
 * draw's -- skip the program walk, the hash and the cache search. A hash -> slot
 * index replaces the linear search over up to VP_VS_CACHE entries (play251: the
 * 10k-draw pre-game cinematic). Both verify the slot's hash, so evictions are
 * safe. NHL_VS_MEMO=0 = old path. */
static int vp_get_vs_ex(const rsx_state* st, int gpu)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("NHL_VS_MEMO"); on = (e && *e == '0') ? 0 : 1; }
    if (!on) return vp_get_vs_slow(st, gpu, NULL);
    static const rsx_state* m_st; static u32 m_gen, m_hash, m_bytes, m_start; static int m_gpu = -1, m_idx = -1;
    if (st && st == m_st && st->vp_gen == m_gen && gpu == m_gpu && st->vp_ucode_bytes == m_bytes &&
        st->transform_program_start == m_start &&
        m_idx >= 0 && m_idx < s_d3d.vp_vs_n && s_d3d.vp_vs[m_idx].hash == m_hash)
        return m_idx;
    u32 h = 0;
    const int idx = vp_get_vs_slow(st, gpu, &h);
    if (idx >= 0 && idx < s_d3d.vp_vs_n && s_d3d.vp_vs[idx].hash == h) {
        m_st = st; m_gen = st->vp_gen; m_gpu = gpu; m_idx = idx; m_hash = h;
        m_bytes = st->vp_ucode_bytes; m_start = st->transform_program_start;
    } else m_idx = -1;
    return idx;
}
static int s_vs_index[4096];   /* #183: hash -> slot + 1 (verified on use) */
static int vp_get_vs_slow(const rsx_state* st, int gpu, u32* out_hash)
{
    extern int rsx_vp_decompile(const uint8_t*, u32, char*, u32);
    extern int rsx_vp_decompile_gpu(const uint8_t*, u32, char*, u32);
    if (!st || st->vp_ucode_bytes < 16) return -1;
    /* Start at the instruction SET_TRANSFORM_PROGRAM_START selects, not at 0.
     * The microcode store holds every resident program -- this title keeps its
     * scene, fluid, caustics and droplet programs in it at once -- so
     * decompiling from 0 ran the scene's transform for every draw. That is why
     * the fluid's 114300 vertices rasterized nothing: they were being pushed
     * through the wrong program.
     * VP_START_OFF=1 restores the old always-from-zero behaviour. */
    u32 vstart = 0;
    { static int off = -1; if (off < 0) off = getenv("VP_START_OFF") ? 1 : 0;
      if (!off) vstart = st->transform_program_start * 16u; }
    if (vstart >= st->vp_ucode_bytes) vstart = 0;
    const u8* vuc = st->vp_ucode + vstart;
    u32 vlen = st->vp_ucode_bytes - vstart;
    /* [NHL local patch #81] Key on THIS program's instructions only (through
     * its D3.end), the same bytes rsx_vp_decompile reads. Hashing to the end
     * of the whole microcode store made every resident program's key change
     * whenever the title loaded any other program behind it -- in the match
     * that re-compiled known VSs and re-built their PSOs nonstop (play74:
     * "vs" was the top PSO-miss reason, 300-600 misses per 5 s).
     * VP_HASH_STORE=1 restores the old whole-store hash. */
    { static int whole = -1; if (whole < 0) whole = getenv("VP_HASH_STORE") ? 1 : 0;
      if (!whole) { extern u32 rsx_vp_program_size_instrs(const u8*, u32);
                    u32 ni = rsx_vp_program_size_instrs(vuc, vlen);
                    if (ni) vlen = ni * 16u; } }
    u32 hash = vp_hash_ucode(vuc, vlen) ^ (gpu ? VS_GPU_SALT : 0u);
    if (out_hash) *out_hash = hash;
    { const int c = s_vs_index[hash & 4095u] - 1;   /* #183 */
      if (c >= 0 && c < s_d3d.vp_vs_n && s_d3d.vp_vs[c].hash == hash) return c; }
    for (int i = 0; i < s_d3d.vp_vs_n; i++)
        if (s_d3d.vp_vs[i].hash == hash) { s_vs_index[hash & 4095u] = i + 1; return i; }
    /* [#81 diag] PSOMISSWHY: count VS compiles, and how many of them are a
     * program already seen at a DIFFERENT store position (same instruction
     * bytes once branch-target fields are masked out -> position-dependent
     * keys, the fixable case). */
    { extern u32 g_vs_compiles, g_vs_moved;
      g_vs_compiles++;
      static u32 seen_h[4096], seen_start[4096]; static int nseen = 0;
      u32 hm = 2166136261u;   /* hash with D2/D3 word low bits that carry branch targets masked */
      for (u32 i = 0; i + 16 <= vlen; i += 16) {
          for (u32 b = 0; b < 16; b++) {
              u8 x = vuc[i + b];
              if (b >= 8) x = 0;   /* coarse: ignore words D2/D3 entirely */
              hm ^= x; hm *= 16777619u;
          }
      }
      int found = 0;
      for (int i = 0; i < nseen; i++)
          if (seen_h[i] == hm) { found = 1; if (seen_start[i] != vstart) g_vs_moved++; break; }
      if (!found && nseen < 4096) { seen_h[nseen] = hm; seen_start[nseen] = vstart; nseen++; } }

    static char hlsl[262144];
    int ni = gpu ? rsx_vp_decompile_gpu(vuc, vlen, hlsl, sizeof hlsl)
                 : rsx_vp_decompile(vuc, vlen, hlsl, sizeof hlsl);
    if (ni <= 0) return -1;
    /* [#86] VP_DUMP_ALL=1: every compiled per-draw VS -> vsdump/vs_<hash>.hlsl
     * (first 512); [VTXRT] prints the same hash per draw. */
    if (getenv("VP_DUMP_ALL")) { static int nd = 0;
        if (nd < 512) { nd++;
            CreateDirectoryA("vsdump", NULL);
            char fn[64]; snprintf(fn, sizeof fn, "vsdump/vs_%08X.hlsl", hash);
            FILE* f = fopen(fn, "w");
            if (f) { fprintf(f, "/* vs hash=0x%08X start=%u instrs=%d */\n%s", hash, vstart / 16u, ni, hlsl); fclose(f); } } }
    if (getenv("VP_DUMP")) { static int _d=0; if (_d++ < 4) {
        FILE* f = fopen("vp2_dump.hlsl", _d==1 ? "w" : "a");
        if (f) { fprintf(f, "/* per-draw VS hash pending, %d instrs */%s%s", ni, hlsl, "\n"); fclose(f); } } }
    /* RSX_DBG_VP=<hex tex0 offset>: write the vertex program belonging to the draws
     * that bind that texture. vp2_dump.hlsl holds whichever four programs were
     * compiled first, which need not include the one under investigation -- and
     * reading the wrong program sends you chasing the wrong constants. */
    { static const char* dp = (const char*)1; static u32 wantp = 0; static int done = 0;
      if (dp == (const char*)1) { dp = getenv("RSX_DBG_VP");
                                  wantp = dp ? (u32)strtoul(dp, NULL, 16) : 0; }
      if (wantp && !done && s_d3d.cur_texs[0].raw == wantp) {
          done = 1;
          FILE* f = fopen("duck_vp.hlsl", "w");
          if (f) { fprintf(f, "/* duck VP, %d instrs, tex0=0x%X */%s", ni, wantp, hlsl);
                   fclose(f); }
          fprintf(stderr, "[DUCKVP] wrote duck_vp.hlsl (%d instrs)%c", ni, 10);
      } }
    /* VP_BYPASS=1: replace the guest transform with a direct map of attribute 0
     * into clip space. If geometry appears with this on, everything from the
     * input layout through raster/depth/present is sound and the fault is the
     * transform (constants or the decompiled program); if it stays blank, the
     * fault is upstream of the shader -- the attribute binding itself. Patched
     * in place, space-padded to the original length so offsets stay valid. */
    /* VP_W_ONE=1: keep the guest transform but force the clip-space w to 1.
     * Different from VP_BYPASS, which replaces the whole position: this tests
     * only the hypothesis that w is wrong. This title's VP ends
     *   { float4 _v = (float4)((vp_c[467]).xxxx); o[0].w = _v.w; }
     * with c467 = (-1, 1, 0, 0), so w = -1 and the clipper discards every
     * vertex. Patched in place, space-padded so offsets stay valid. */
    if (getenv("VP_W_ONE")) {
        static const char anchor[] = "o[0].w = _v.w;";
        static const char repl[]   = "o[0].w = 1.0;";
        char* at = hlsl;
        while ((at = strstr(at, anchor)) != NULL) {
            size_t n = sizeof(anchor) - 1, m = sizeof(repl) - 1;
            memcpy(at, repl, m);
            memset(at + m, ' ', n - m);
            at += n;
        }
    }
    if (getenv("VP_BYPASS")) {
        static const char anchor[] =
            "Out.pos = float4(_p.xyz * vp_posscale.xyz + _p.w * vp_posoffset.xyz, _p.w);";
        static const char repl[] = "Out.pos = float4(v[0].xy * 0.5, 0.5, 1.0);";
        char* at = hlsl;
        while ((at = strstr(at, anchor)) != NULL) {
            size_t n = sizeof(anchor) - 1, m = sizeof(repl) - 1;
            memcpy(at, repl, m);
            memset(at + m, ' ', n - m);
            at += n;
        }
    }
    ID3DBlob* vb = NULL; ID3DBlob* e = NULL;
    const size_t hlen = strlen(hlsl);
    int vs_queued = 0;   /* #124: compiled in render_frame's batch, the slot's blob filled there */
    HRESULT hr = S_OK;
    if (fxc_par_on()) { vb = fxc_lookup(hlsl, hlen, 0); vs_queued = !vb; }
    else hr = fxc_compile(hlsl, hlen, "guest_vp2", "vs_5_0", 0, 0, &vb, &e);   /* #123 */
    if (e) e->lpVtbl->Release(e);
    if (!vs_queued && (FAILED(hr) || !vb)) {
        static int _e=0; if (_e++<4) printf("[VP] per-draw VS compile FAIL (hash=0x%08X)\n", hash);
        return -1;
    }
    int slot;
    if (s_d3d.vp_vs_n < VP_VS_CACHE) slot = s_d3d.vp_vs_n++;
    else {  /* [NHL local patch #58] evict one slot in place, never shift */
        static int s_vs_evict = 0;
        slot = s_vs_evict; s_vs_evict = (s_vs_evict + 1) % VP_VS_CACHE;
        if (s_d3d.vp_vs[slot].vs) s_d3d.vp_vs[slot].vs->lpVtbl->Release(s_d3d.vp_vs[slot].vs);
        { static int w = 0; if (w++ < 4) printf("[VP] VS cache full (%d): evicting slot %d\n", VP_VS_CACHE, slot); }
    }
    s_d3d.vp_vs[slot].hash = hash;
    s_vs_index[hash & 4095u] = slot + 1;   /* #183 */
    s_d3d.vp_vs[slot].vs   = vb;   /* NULL while #124-queued */
    s_d3d.vp_vs[slot].gpu  = gpu;   /* #125 */
    s_d3d.vp_vs[slot].ni   = ni;    /* #225b */
    if (vs_queued) {
        /* [#225b] a small draw (<= 6 vertices) counts as a full-screen pass only with a
         * tiny VS; scene sprites/flares/decals use big ones (the branchy glow VS ~0.5 s) */
        const int keep = s_async_sync;
        if (!s_async_sync && s_rec_small && ni <= async_tiny_vs()) s_async_sync = 1;
        { static int n = 0; if (s_rec_small && n++ < 30)
              fprintf(stderr, "[ASYNCVS] hash=0x%08X instrs=%d small draw -> %s%c", hash, ni,
                      s_async_sync ? "compile now" : "background", 10); }
        /* #225d: every VS that still compiles synchronously, and why */
        { static int n = 0; if (s_async_sync && async_on() && n++ < 40)
              fprintf(stderr, "[ASYNCSYNC] VS hash=0x%08X instrs=%d gpu=%d depth=%d display=%d small=%d%c",
                      hash, ni, gpu, s_depth_rec, current_rt_off(NULL, NULL, NULL) == 0, s_rec_small, 10); }
        fxc_queue(hlsl, hlen, 0, slot, hash);
        s_async_sync = keep;
    }
    s_d3d.vp_vs[slot].uses_c03 =
        (strstr(hlsl, "vp_c[0]") || strstr(hlsl, "vp_c[1]") ||
         strstr(hlsl, "vp_c[2]") || strstr(hlsl, "vp_c[3]")) ? 1 : 0;
    { static int _n=0; if (_n++<6) printf("[VP] per-draw VS cached (hash=0x%08X, %d instrs, slot %d)\n", hash, ni, slot); }
    /* [NHL local patch] VS_DUMP_ALL=1: every per-draw VS to vpdump/vs_<hash>_slot<N>.hlsl
     * (slot = the vs_idx that draw records, DRED reports and capture manifests show). */
    if (getenv("VS_DUMP_ALL")) {
        CreateDirectoryA("vpdump", NULL);
        char fn[96]; snprintf(fn, sizeof fn, "vpdump/vs_%08X_slot%d.hlsl", hash, slot);
        FILE* f = fopen(fn, "w");
        if (f) { fprintf(f, "/* hash=0x%08X instrs=%d slot=%d */\n%s", hash, ni, slot, hlsl); fclose(f); }
    }
    /* Build the base VP pipeline here if it does not exist yet. render_frame's
     * trigger reads s_d3d.current_rsx_state, which by frame end no longer has
     * the microcode -- so for a title that only ever compiles per-draw VS the
     * base pipeline was never built, s_d3d.vp_ready stayed 0, and the VP draw
     * pass it gates dropped every recorded is_vp draw. Here we are at RECORD
     * time with a live state that definitely has microcode. */
    if (!s_d3d.vp_ready && st && st->vp_ucode_bytes >= 16) {
        const rsx_state* prev = s_d3d.current_rsx_state;
        s_d3d.current_rsx_state = st;
        compile_vp();
        s_d3d.current_rsx_state = prev;
    }
    return slot;
}

/* --- Guest blend state -> packed PSO key ----------------------------------
 * RSX blend factors/equation carry GL enums; each SFACTOR/DFACTOR/EQUATION
 * word packs (alpha<<16)|colour. The key packs D3D12 enums so the PSO cache
 * distinguishes real blend modes: bit0 enable, [5:1] srcC, [10:6] dstC,
 * [15:11] srcA, [20:16] dstA, [23:21] opC, [26:24] opA. DeferredShading's
 * light accumulation is additive (ONE,ONE) -- the old hardcoded straight-
 * alpha PSO turned each light pass into a screen-blanking flash. */
static u8 gl_blend_factor_d3d(u32 f, int is_alpha)
{
    switch (f & 0xFFFF) {
    case 0x0000: return D3D12_BLEND_ZERO;
    case 0x0001: return D3D12_BLEND_ONE;
    case 0x0300: return is_alpha ? D3D12_BLEND_SRC_ALPHA     : D3D12_BLEND_SRC_COLOR;
    case 0x0301: return is_alpha ? D3D12_BLEND_INV_SRC_ALPHA : D3D12_BLEND_INV_SRC_COLOR;
    case 0x0302: return D3D12_BLEND_SRC_ALPHA;
    case 0x0303: return D3D12_BLEND_INV_SRC_ALPHA;
    case 0x0304: return D3D12_BLEND_DEST_ALPHA;
    case 0x0305: return D3D12_BLEND_INV_DEST_ALPHA;
    case 0x0306: return is_alpha ? D3D12_BLEND_DEST_ALPHA     : D3D12_BLEND_DEST_COLOR;
    case 0x0307: return is_alpha ? D3D12_BLEND_INV_DEST_ALPHA : D3D12_BLEND_INV_DEST_COLOR;
    case 0x0308: return D3D12_BLEND_SRC_ALPHA_SAT;
    case 0x8001: return D3D12_BLEND_BLEND_FACTOR;       /* CONSTANT_COLOR   */
    case 0x8002: return D3D12_BLEND_INV_BLEND_FACTOR;
    case 0x8003: return D3D12_BLEND_BLEND_FACTOR;       /* CONSTANT_ALPHA ~ */
    case 0x8004: return D3D12_BLEND_INV_BLEND_FACTOR;
    default:     return D3D12_BLEND_ONE;
    }
}
static u8 gl_blend_op_d3d(u32 e)
{
    switch (e & 0xFFFF) {
    case 0x8007: return D3D12_BLEND_OP_MIN;
    case 0x8008: return D3D12_BLEND_OP_MAX;
    case 0x800A: return D3D12_BLEND_OP_SUBTRACT;
    case 0x800B: return D3D12_BLEND_OP_REV_SUBTRACT;
    default:
        /* [NHL local patch] Say which equation fell through: the RSX signed
         * modes (0xF005 REV_SUB_SIGNED, 0xF006 ADD_SIGNED, 0xF007
         * REV_ADD_SIGNED) have no D3D12 op and silently became ADD. */
        if ((e & 0xFFFF) && (e & 0xFFFF) != 0x8006) {
            static u32 seen[8]; static int ns = 0; int f = 0;
            for (int i = 0; i < ns; i++) if (seen[i] == (e & 0xFFFF)) f = 1;
            if (!f && ns < 8) { seen[ns++] = e & 0xFFFF;
                fprintf(stderr, "[BLEND] unmapped blend equation 0x%04X -> ADD%c", e & 0xFFFF, 10); }
        }
        return D3D12_BLEND_OP_ADD;   /* 0x8006 FUNC_ADD / unset */
    }
}
/* Pack the guest's face-culling state for the PSO key.
 * bit0 = cull enabled, bit1 = cull FRONT (else BACK), bit2 = front face is CCW.
 * RSX/GL enums: CULL_FACE FRONT=0x0404 BACK=0x0405 FRONT_AND_BACK=0x0408;
 * FRONT_FACE CW=0x0900 CCW=0x0901. rsx_commands seeds cull_face/front_face with
 * plain 1/0 before any register arrives, so treat those defaults as BACK/CW. */
static u32 rsx_cull_key(const rsx_state* st)
{
    if (!st || !st->cull_face_enable) return 0;
    u32 k = 1u;
    if (st->cull_face == 0x0404u || st->cull_face == 0x0408u) k |= 2u;  /* FRONT */
    if (st->front_face == 0x0901u) k |= 4u;                             /* CCW */
    return k;
}

/* [NHL local patch #110] NV4097_SET_COLOR_MASK -> D3D write mask (R=1 G=2 B=4
 * A=8). Each channel is a whole byte (B bits 0-7, G 8-15, R 16-23, A 24-31)
 * and any non-zero byte enables it, as RPCS3 and this SDK's Vulkan/Metal/
 * live-draw paths decode it; testing only bit 0 of each byte dropped channels
 * whose byte is e.g. 0x10. Each distinct raw value is logged once ([CMASK]). */
static u32 rsx_cmask_d3d(u32 cm)
{
    { static u32 seen[16]; static int ns = 0; int f = 0;
      for (int i = 0; i < ns; i++) if (seen[i] == cm) f = 1;
      if (!f && ns < 16) { seen[ns++] = cm;
          fprintf(stderr, "[CMASK] guest colour mask 0x%08X%c", cm, 10); } }
    return ((cm & 0x00FF0000u) ? 1u : 0u) | ((cm & 0x0000FF00u) ? 2u : 0u)
         | ((cm & 0x000000FFu) ? 4u : 0u) | ((cm & 0xFF000000u) ? 8u : 0u);
}

/* [NHL local patch #122] Render-target write-back. RT contents live only in
 * D3D12; nothing ever reached guest memory. NHL composites each jersey (base +
 * name/number) by RENDERING into a 1024x512 / 512x512 A8R8G8B8 surface and then
 * DXT-compresses that surface on the CPU/SPU from guest memory -- which held
 * zeros, so the jerseys came out black (DXT1 with all-zero colour endpoints,
 * exactly 50% zero bytes) with garbage specks, and swapped as composites were
 * redone (play170/171). RPCS3 copies a render target back whenever the CPU or
 * SPU touches it. Here: offscreen surfaces drawn in a batch that the GPU never
 * samples and that are not redrawn every frame are read back after the batch
 * renders (and the batch is rendered early when the FIFO writes a sync label,
 * which is what the consumer waits for). RSX_RT_WRITEBACK=0 disables. */
/* [NHL local patch #169] ...the original #122 write-back was never wired up (only this
 * bookkeeping existed, nothing called it). Completed here: every recorded draw notes its
 * colour target (wb_note_draw, from vp_record_cb); before the FIFO publishes a label or
 * semaphore the guest can wait on (g_gcm_label_hook, cellGcmSys.c / rsx_commands.c),
 * wb_sync() renders the batch so far and copies each CANDIDATE surface back into guest
 * memory as A8R8G8B8 bytes A,R,G,B at its guest pitch. Candidate = a 32-bit colour
 * surface drawn since the last sync that is NOT redrawn most frames (drawn in <= 2 of the
 * last 8 frames: one-off composites -- jersey names/numbers, team-coloured equipment --
 * not the per-frame HDR/shadow/post targets), at most 2048x2048. [RTWB] logs each copy,
 * [RTWB-SKIP] each surface skipped once with the reason. RSX_RT_WRITEBACK=0 disables. */
/* [NHL local patch #172] gaddr/gbytes/gsnap: sampled checksum of the guest memory under
 * the surface, taken after our own write-back and, for surfaces written back before, when
 * the game starts drawing into them again. If those bytes change later, something else
 * (SPU upload, a new texture) owns the memory now and the GPU-side RT is stale -- see
 * rt_alias_reject. Only written-back surfaces (wb_ever): per-frame targets also receive
 * our own guest-side NV3089 copies (the MSAA resolve into 0x043F0000), which must not
 * read as a foreign rewrite. */
typedef struct { u32 off, pitch, fmt, w, h, last_frame, logged_skip; unsigned long long hist; int pending, sampled;
                 u32 gaddr, gbytes, gsnap, chk_flip; int gsnap_ok, chk_alias, wb_ever, perframe;
                 int deferred; /* #218: GPU-sampled, drawn since its last write-back; written back when a blit reads it */ } WBEnt;
static WBEnt s_wb[128]; static int s_wb_n = 0; static int s_wb_any_pending = 0;
static u32 tex_csum_sampled(const u8* base, u32 nbytes);   /* defined with the texture cache */
extern u32 cellGcmResolveLocated(int local, u32 offset);
static u32 s_wb_snap_rt = 0, s_wb_snap_fc = 0;              /* #172: last surface fingerprinted */
static void wb_snap(WBEnt* e)
{
    extern u8* vm_base;
    const u32 a = cellGcmResolveLocated(1, e->off);
    const u32 nb = (e->pitch ? e->pitch : e->w * 4u) * e->h;
    e->chk_flip = 0xFFFFFFFFu;
    if (!a || a == 0xFFFFFFFFu || !vm_base || nb < 256u || nb > (16u << 20)) { e->gsnap_ok = 0; return; }
    e->gaddr = a; e->gbytes = nb; e->gsnap = tex_csum_sampled(vm_base + a, nb); e->gsnap_ok = 1;
}
static int wb_on(void) { static int on = -1; if (on < 0) { const char* e = getenv("RSX_RT_WRITEBACK"); on = (e && *e == '0') ? 0 : 1; } return on; }
/* [NHL local patch #177] wb_ent runs for every bound texture unit of every recorded
 * draw (wb_note_draw's "sampled" marking) and almost always misses -- a full scan of up
 * to 128 entries each time. s_wb_seen = offsets ever entered (bits never cleared, so a
 * clear bit is a guaranteed miss); s_wb_hint = last index per hash bucket. */
static u32 s_wb_seen[1024 / 32]; static u8 s_wb_hint[256];
static u32 wb_hash(u32 off) { u32 x = off * 0x9E3779B1u; return x >> 22; }   /* 10 bits */
static WBEnt* wb_ent(u32 off, int create)
{
    const u32 hb = wb_hash(off);
    if (s_wb_seen[hb >> 5] & (1u << (hb & 31u))) {
        const int hi = s_wb_hint[hb & 255u] - 1;
        if (hi >= 0 && hi < s_wb_n && s_wb[hi].off == off) return &s_wb[hi];
        for (int i = 0; i < s_wb_n; i++) if (s_wb[i].off == off) { s_wb_hint[hb & 255u] = (u8)(i + 1); return &s_wb[i]; }
    }
    if (!create) return NULL;
    s_wb_seen[hb >> 5] |= 1u << (hb & 31u);
    if (s_wb_n < 128) { WBEnt* e = &s_wb[s_wb_n++]; memset(e, 0, sizeof *e); e->off = off;
                        s_wb_hint[hb & 255u] = (u8)s_wb_n; return e; }
    /* full: recycle the stalest non-pending entry */
    int best = -1; u32 bf = 0xFFFFFFFFu;
    for (int i = 0; i < s_wb_n; i++) if (!s_wb[i].pending && s_wb[i].last_frame < bf) { bf = s_wb[i].last_frame; best = i; }
    if (best < 0) return NULL;
    memset(&s_wb[best], 0, sizeof s_wb[best]); s_wb[best].off = off;
    s_wb_hint[hb & 255u] = (u8)(best + 1); return &s_wb[best];
}
/* #244: write-back class of a surface, for the capture manifest's RT table */
static const char* wb_kind(u32 off)
{
    const WBEnt* e = wb_ent(off, 0);
    if (!e) return "";
    return e->perframe ? " perframe" : e->deferred ? " deferred" : e->wb_ever ? " written-back" : "";
}
static int wb_popcount64(unsigned long long v) { int n = 0; while (v) { n += (int)(v & 1u); v >>= 1; } return n; }
static int wb_perframe_flips(void)   /* #233 */
{
    static int v = -1;
    if (v < 0) { const char* e = getenv("NHL_WB_PERFRAME"); v = e ? atoi(e) : 48; if (v < 1) v = 1; if (v > 64) v = 64; }
    return v;
}
/* NULL = candidate, else why not */
static const char* wb_reject(WBEnt* e)
{
    const u32 f = e->fmt & 0x1Fu;   /* 32-bit ARGB/XRGB colour surfaces only */
    if (!(f == 0x08u || f == 0x04u || f == 0x05u)) return "format";
    if (!e->w || !e->h || e->w > 2048u || e->h > 2048u) return "size";
    /* [NHL local patch #177] Write-back waste: play239 made ~9000 readbacks (each a
     * mid-frame render_frame + two GPU waits + up to 3.6 MB copied) for ~70 real
     * composites. The per-frame targets (HDR scene 0x02980000, the 4x-MSAA half-res
     * 0x03DF0000 that always read back black, the 640x360 post chain) lose their
     * 64-flip history whenever play stops drawing them (replays, pauses, menus), then
     * look like one-off composites for 12 flips. (1) A surface once seen drawn in most
     * frames stays rejected (sticky). (2) Display-sized surfaces are never candidates
     * (NHL's composites are at most 1024x512 / 640x760). NHL_WB_OLDREJECT=1 = old rules. */
    static int old = -1;
    if (old < 0) old = getenv("NHL_WB_OLDREJECT") ? 1 : 0;
    if (!old) {
        if (e->perframe) return "drawn-most-frames(sticky)";
        if ((u64)e->w * e->h >= (u64)s_d3d.width * s_d3d.height) return "display-sized";
        /* (3, #177b) a guest pitch wider than the surface = an MSAA sample layout (the
         * 2x 640x360 post targets: pitch 5120). We render single-sampled, so the copy
         * cannot match it -- it read back black (play230) and was ~1/5 of play240's
         * remaining write-backs. Every NHL composite has pitch == w * 4. */
        if (e->pitch > e->w * 4u) return "msaa-layout";
    }
    /* drawn in most of the last 64 guest flips = a per-frame target (sticky from here on).
     * [NHL local patch #233] Was "more than 12": NHL builds a team's name/number masks and
     * equipment composites ONE PER FLIP in the same scratch surface, so the 13th player in
     * a row marked the composite surface per-frame (sticky) and every later player's
     * composite stayed GPU-side (wrong jerseys/helmets in some matches; game_20261028_001838
     * t=184: 0x0CA10000 640x760, flips=13/64). Real per-frame targets are drawn in ~every
     * flip; a burst is at most a roster long. NHL_WB_PERFRAME=<n> (default 48). */
    if (wb_popcount64(e->hist) >= wb_perframe_flips()) { e->perframe = 1; return "drawn-most-frames"; }
    /* [NHL local patch #218] A surface the GPU samples is written back only when a blit
     * reads it (wb_blit_src), not at every sync: play321's t=108 433 ms frame was 36 sync
     * write-backs (3 per flip for 12 flips, until #177's sticky rule) of the 320x180 DOF
     * chain 0x05050000 / 0x050B0000, which only the GPU ever reads; t=80 likewise. The
     * equipment composites leave through NV3089 (play231), so they are still written back,
     * just at the blit. NHL_WB_ONDEMAND=0 = old (sync-time) write-back. */
    { static int od = -1; if (od < 0) { const char* ev = getenv("NHL_WB_ONDEMAND"); od = (ev && *ev == '0') ? 0 : 1; }
      if (od && !old && e->sampled) return "gpu-sampled(deferred to blit)"; }
    { static int ss = -1; if (ss < 0) ss = getenv("NHL_WB_SKIP_SAMPLED") ? 1 : 0;
      if (ss && e->sampled) return "sampled-by-gpu"; }
    return NULL;
}
static u32 current_rt_off(u32* out_w, u32* out_h, u32 out_mrt[3]);   /* defined below */
static int off_rt_find(u32 off);                                     /* defined below */
/* [NHL local patch #243] #172's guest fingerprint for one-off composites that are never
 * written back too (GPU-sampled, #218 deferred): without it nothing notices when the game
 * reuses their memory for an uncompressed texture, and the stale composite is sampled in
 * its place. The user's ARGB mod logos after Moments Live (F9 frames 90/94, 2026-10-28:
 * opponent / Flames logo = noise); stock DXT logos were safe via the "dxt" rule. Excludes
 * what the game's own guest-side copies rewrite every frame: per-frame and display-sized
 * targets, MSAA layouts, and destinations of a #85 GPU blit. NHL_RT_ALIAS_ONEOFF=0 = off. */
static int wb_snap_oneoff(const WBEnt* e)
{
    static int on = -1;
    if (on < 0) { const char* v = getenv("NHL_RT_ALIAS_ONEOFF"); on = (v && *v == '0') ? 0 : 1; }
    if (!on || e->perframe || wb_popcount64(e->hist) >= wb_perframe_flips()) return 0;
    if ((u64)e->w * e->h >= (u64)s_d3d.width * s_d3d.height || e->pitch > e->w * 4u) return 0;
    const int s = off_rt_find(e->off);
    return s >= 0 && !s_d3d.off_rt[s].blit_frame;
}
static void wb_note_draw(const rsx_state* st, const D3D12DrawRecord* dr)
{
    if (!wb_on() || !st || !dr) return;
    for (int u = 0; u < 16; u++)   /* a surface a later draw samples on the GPU */
        if (dr->tex[u].set) { WBEnt* s = wb_ent(dr->tex[u].raw, 0); if (s) s->sampled = 1; }
    const u32 rt = dr->rt_off;
    if (!rt || !dr->rt_w || !dr->rt_h) { s_wb_snap_rt = 0; return; }
    WBEnt* e = wb_ent(rt, 1); if (!e) return;
    /* history per GUEST FLIP: s_d3d.frame_count also counts mid-frame batch flushes
     * (wb_sync's own included), which made per-frame targets look occasional (play224) */
    extern unsigned cellGcm_flip_request_count(void);
    const u32 fc = cellGcm_flip_request_count();
    if (e->last_frame != fc || !e->hist) {
        const u32 d = fc - e->last_frame;
        e->hist = ((d >= 64u) ? 0ull : (e->hist << d)) | 1ull; e->last_frame = fc;
    }
    e->pitch = st->surface_color_pitch[st->color_target == 2 ? 1 : 0];
    e->fmt = st->surface_format; e->w = dr->rt_w; e->h = dr->rt_h; e->pending = 1;
    s_wb_any_pending = 1;
    /* #172: fingerprint once per run of draws into this surface */
    if (rt != s_wb_snap_rt || fc != s_wb_snap_fc) { s_wb_snap_rt = rt; s_wb_snap_fc = fc; if (e->wb_ever || wb_snap_oneoff(e)) wb_snap(e); }
}
/* [NHL local patch #229] #225's background compiles skip a draw until its shader / PSO is
 * ready, which only costs a frame of pop-in when the target is redrawn every frame. A
 * one-off target (an equipment composite, a name or number) is drawn once and copied out:
 * a skipped layer stays missing and the player keeps whatever the scratch surface held
 * (WSH vs CAR: mixed white/red helmets, user capture 2026-10-27 23:44). One-off = not
 * known as per-frame by the write-back history (#169/#177: drawn in <= 12 of the last 64
 * flips, not sticky), unknown targets included. NHL_ASYNC_ONEOFF=0 = old rule. */
static int rt_oneoff(u32 rt)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("NHL_ASYNC_ONEOFF"); on = (e && *e == '0') ? 0 : 1; }
    if (!on || !rt || !wb_on()) return 0;
    const WBEnt* e = wb_ent(rt, 0);
    return !e || (!e->perframe && __builtin_popcountll(e->hist) < wb_perframe_flips());   /* #233 */
}

/* [#172] Should a texture bind at an offscreen RT's address sample guest memory instead?
 * off_rt_find matches on the raw offset only, and NHL's equipment composites are built in
 * scratch surfaces all over VRAM (local 0x06..0x0E000000, play230-232). Once the game
 * reuses that memory for a real texture (puck, helmet, socks: DXT uploads), the stale RT
 * -- holding the last item composited there -- was sampled instead (play232: helmet with
 * decal glyphs, socks with letters, wrong pants and puck). Reject the RT when the bind is
 * a compressed format (an RT never is), or when the guest bytes under it changed since
 * the game last drew into it / we wrote it back. NHL_RT_ALIAS=0 = log only; [RTALIAS] logs the first 64. */
static int rt_alias_reject(const D3D12DrawRecord* dr, int u, int rt)
{
    static int mode = -1;
    if (mode < 0) { const char* e = getenv("NHL_RT_ALIAS"); mode = (e && *e == '0') ? 0 : 1; }
    const OffRT* r = &s_d3d.off_rt[rt];
    const u32 bf = dr->tex[u].fmt & 0x9Fu;
    const char* why = NULL;
    if (bf >= 0x86u && bf <= 0x88u) why = "dxt";
    else {
        WBEnt* e = wb_ent(r->off, 0);
        if (e && e->gsnap_ok) {
            extern unsigned cellGcm_flip_request_count(void);
            extern u8* vm_base;
            const u32 fc = cellGcm_flip_request_count();
            if (e->chk_flip != fc) {
                e->chk_flip = fc;
                e->chk_alias = tex_csum_sampled(vm_base + e->gaddr, e->gbytes) != e->gsnap;
            }
            if (e->chk_alias) why = "guest-rewritten";
        }
    }
    if (!why) return 0;
    { static int n = 0;
      if (n < 64) { n++;
          fprintf(stderr, "[RTALIAS] %s unit %d fp=0x%X tex raw=0x%08X %ux%u fmt=0x%02X vs rt %ux%u dxgi=%u: %s%c",
                  mode ? "guest" : "kept-rt(log only)", u, dr->fp_addr, dr->tex[u].raw, dr->tex[u].w,
                  dr->tex[u].h, dr->tex[u].fmt, r->w, r->h, r->dxgi, why, 10); } }
    return mode;
}
/* [NHL local patch #108] Record the guest viewport y/z transform and
 * SHADER_WINDOW with each draw, and log every new (rt, scale.y sign, origin,
 * cull) combination once as [WINORG]. */
static void dr_note_view(D3D12DrawRecord* dr)
{
    /* [NHL local patch #114] Draw records are reused frame to frame without a
     * memset, and only the blit path (#85) ever writes is_blit -- so a draw that
     * landed in a slot a blit had used stayed is_blit=1 and the exec loop
     * skipped it as a "blit" (players/equipment flashing, play163: 15-65 draws
     * a frame, ex=1). Every VP draw record passes through here: clear it. */
    dr->is_blit = 0;
    dr->is_depth = 0; dr->zmask = 0; dr->dcz = 0;   /* #190/#209: same reuse hazard */
    const rsx_state* st = s_d3d.current_rsx_state;
    if (!st) return;
    dr->vps_y = st->viewport_scale[1];  dr->vpo_y = st->viewport_offset[1];
    dr->vps_z = st->viewport_scale[2];  dr->vpo_z = st->viewport_offset[2];
    dr->winorg = st->shader_window;
    static u32 seen[64]; static int ns = 0;
    const u32 rt = st->surface_color_offset[st->color_target == 2 ? 1 : 0];
    const u32 k = (rt >> 12) ^ (dr->vps_y < 0.0f ? 0x80000000u : 0u)
                ^ ((st->shader_window >> 12) & 0xFu) << 24 ^ (rsx_cull_key(st) << 28);
    for (int i = 0; i < ns; i++) if (seen[i] == k) return;
    if (ns < 64) { seen[ns++] = k;
        fprintf(stderr, "[WINORG] rt=0x%08X scale=(%g,%g,%g) offset=(%g,%g,%g) window=0x%08X"
                " (origin=%s centre=%u h=%u) cull=0x%X (enable=%u face=0x%X front=0x%X)%c",
                rt, st->viewport_scale[0], st->viewport_scale[1], st->viewport_scale[2],
                st->viewport_offset[0], st->viewport_offset[1], st->viewport_offset[2],
                st->shader_window, ((st->shader_window >> 12) & 0xF) ? "bottom" : "top",
                (st->shader_window >> 16) & 0xF, st->shader_window & 0xFFF,
                rsx_cull_key(st), st->cull_face_enable, st->cull_face, st->front_face, 10); }
}

/* [NHL local patch #54] Per-draw shader-control + depth key, carried in the
 * draw record's fp_exp32 (a PSO key): bits 0-15 = SET_SHADER_CONTROL (export
 * width, depth export), bits 20-24 = guest depth state: bit0 test enable,
 * bits1-3 compare (GL 0x200+n order), bit4 depth write. The backend used to
 * force depth test LEQUAL + write on every guest-FP draw: NHL's 2D layers then
 * depth-occluded each other (the copyright slide's far-away cloud billboard
 * failed against the arena layer drawn first) and full-screen passes that the
 * game depth-rejects painted over everything (the black Starting Lineup).
 * NHL_DEPTH_LEGACY=1 restores the forced state. */
/* [NHL local patch #174] Alpha-to-coverage emulation mode (NHL_A2C):
 *  0 off; 1 = #103's 4x4 ordered-dither discard; 2 (default) = blend: a
 *  blend-off A2C draw becomes SRC_ALPHA / INV_SRC_ALPHA, which is what 4x MSAA
 *  A2C averages to after the resolve, and alpha < 1/8 (no sample covered) is
 *  discarded so it writes no depth; 3 = alpha test at 0.5. We render the 4x MSAA
 *  scene single-sampled, so the dither was never averaged out and stayed on
 *  screen: speckled glyph edges + a dotted outline around every jersey
 *  name/number decal and helmet decal (play233/234). */
static int a2c_mode(void)
{
    static int m = -1;
    if (m < 0) { const char* e = getenv("NHL_A2C"); m = (e && e[0] >= '0' && e[0] <= '3') ? e[0] - '0' : 2; }
    return m;
}

static int rsx_ctrl_key(const rsx_state* st)
{
    if (!st) return 0x40 | (0x17 << 20);
    static int legacy = -1;
    if (legacy < 0) legacy = getenv("NHL_DEPTH_LEGACY") ? 1 : 0;
    u32 dk;
    if (legacy) dk = 0x17u;   /* enable | LEQUAL | write: the old forced state */
    else {
        u32 fn = st->depth_func;
        fn = (fn >= 0x200) ? ((fn - 0x200) & 7u) : (fn & 7u);
        dk = (st->depth_test_enable ? 1u : 0u) | (fn << 1) | (st->depth_mask ? 0x10u : 0u);
    }
    /* [NHL local patch #103] bit 25 = alpha-to-coverage (NV4097 AA control bit 4):
     * the PS gets a dithered discard (see vp_get_fp_pso). NHL_A2C=0 disables
     * (#174: the mode is a2c_mode()). */
    const u32 a2c_bit = (a2c_mode() && (st->aa_control & 0x10u)) ? (1u << 25) : 0u;
    /* [#208] bit 26 = framebuffer sRGB output (NHL_FB_SRGB=0 disables) */
    static int fbs = -1;
    if (fbs < 0) { const char* e = getenv("NHL_FB_SRGB"); fbs = (e && *e == '0') ? 0 : 1; }
    const u32 srgb_bit = (fbs && rsx_fp_srgb_output_enabled(st->shader_packer, st->shader_control,
                                                            st->surface_format & 0x1Fu)) ? (1u << 26) : 0u;
    return (int)((st->shader_control & 0xFFFFu) | (dk << 20) | a2c_bit | srgb_bit);
}

static u32 rsx_blend_key(const rsx_state* st, int enable)
{
    { static int _bd = -1; if (_bd < 0) _bd = getenv("BLENDDBG") ? 1 : 0;
      if (_bd) { static u32 seen[32]; static int ns=0;
        u32 sk = (enable?0x80000000u:0u) | (st ? (st->blend_sfactor & 0xFFFFu) : 0u);
        int f=0; for (int k2=0;k2<ns;k2++) if (seen[k2]==sk) f=1;
        if (!f && ns<32) { seen[ns++]=sk;
            fprintf(stderr, "[BLEND] enable=%d sfactor=0x%X dfactor=0x%X%c", enable,
                    st?st->blend_sfactor:0, st?st->blend_dfactor:0, 10); } } }
    if (!enable) return 0;
    /* Factors never programmed: keep the legacy straight-alpha behaviour
     * (dbgfont-style text enables blending without setting factors). */
    if (!st || (st->blend_sfactor == 0 && st->blend_dfactor == 0))
        return 1u
             | ((u32)D3D12_BLEND_SRC_ALPHA     << 1)
             | ((u32)D3D12_BLEND_INV_SRC_ALPHA << 6)
             | ((u32)D3D12_BLEND_ONE           << 11)
             | ((u32)D3D12_BLEND_INV_SRC_ALPHA << 16)
             | ((u32)D3D12_BLEND_OP_ADD << 21) | ((u32)D3D12_BLEND_OP_ADD << 24);
    return 1u
         | ((u32)gl_blend_factor_d3d(st->blend_sfactor,       0) << 1)
         | ((u32)gl_blend_factor_d3d(st->blend_dfactor,       0) << 6)
         | ((u32)gl_blend_factor_d3d(st->blend_sfactor >> 16, 1) << 11)
         | ((u32)gl_blend_factor_d3d(st->blend_dfactor >> 16, 1) << 16)
         | ((u32)gl_blend_op_d3d(st->blend_equation)       << 21)
         | ((u32)gl_blend_op_d3d(st->blend_equation >> 16) << 24);
}

/* Bound colour target count for a draw record: 1 (target A) + the contiguous
 * prefix of MRT B/C/D offsets. */
static int dr_num_rts(const D3D12DrawRecord* dr)
{
    int n = 1;
    while (n < 4 && dr->rt_mrt[n - 1]) n++;
    return n;
}

/* Replace every occurrence of `find` with `repl` inside a NUL-terminated buffer,
 * shifting the tail. Used to retarget generated HLSL (2D sampler -> cube). */
static void hlsl_replace_all(char* buf, size_t cap, const char* find, const char* repl)
{
    size_t fl = strlen(find), rl = strlen(repl);
    if (!fl) return;
    char* at = buf;
    while ((at = strstr(at, find)) != NULL) {
        size_t used = strlen(buf) + 1;
        if (used + rl - fl > cap) return;
        memmove(at + rl, at + fl, used - (size_t)(at - buf) - fl);
        memcpy(at, repl, rl);
        at += rl;
    }
}

/* Which of a draw's texture units are cube textures, as a 4-bit mask. */
/* [#186] set by d3d12_flush_if_full: the next batch continues these surfaces
 * (colour target + 3 MRTs), so its first switch to them keeps the depth buffer */
static int s_depth_cont;
static u32 s_depth_cont_rt[4];
/* [NHL local patch #185] Two draws whose texture bindings are identical in every
 * field the SRV pre-pass reads resolve to identical descriptors within one frame. */
static int dr_tex_same(const D3D12DrawRecord* a, const D3D12DrawRecord* b)
{
    for (int u = 0; u < D3D_TEX_UNITS; u++) {
        if (a->tex[u].set != b->tex[u].set) return 0;
        if (!a->tex[u].set) continue;
        if (a->tex[u].off != b->tex[u].off || a->tex[u].raw != b->tex[u].raw ||
            a->tex[u].w != b->tex[u].w || a->tex[u].h != b->tex[u].h ||
            a->tex[u].fmt != b->tex[u].fmt || a->tex[u].ctrl1 != b->tex[u].ctrl1 ||
            a->tex[u].pitch != b->tex[u].pitch || a->tex[u].filter != b->tex[u].filter ||
            a->tex[u].snap != b->tex[u].snap || a->tex[u].mips != b->tex[u].mips ||
            a->tex[u].cube != b->tex[u].cube || a->tex[u].addr != b->tex[u].addr ||
            a->tex[u].ctl0 != b->tex[u].ctl0) return 0;   /* #190: shadow compare state */
    }
    return 1;
}
static u32 dr_cube_mask(const D3D12DrawRecord* dr)
{
    /* ON by default (RSX_CUBE_TEX=0 to disable). This was off while the cube path
     * cost 2262 PSO misses per 20 frames and 0.38 fps -- both caused by inline
     * fragment-program constants baking into the shader source. With those
     * constants hoisted into b1 the cube path costs 0 PSO misses, and with the
     * per-face stride fixed (each face is a whole mip pyramid, not one mip-0
     * image) all six faces decode as clean environment art. */
    static int en = -1;
    if (en < 0) { const char* e = getenv("RSX_CUBE_TEX"); en = e ? atoi(e) : 1; }
    u32 m = 0;
    if (en) for (int u = 0; u < D3D_TEX_UNITS; u++) if (dr->tex[u].set && dr->tex[u].cube) m |= 1u << u;
    /* [NHL local patch #176] bits 16..31: units 1-15 the guest filters NEAREST (mag
     * filter 1). The static samplers are all linear, which blends lookup textures:
     * NHL's jersey shader reads a 256x256 ARGB indirection map (unit 6) for the
     * name/number slot id (.z, floored) and the glyph's atlas uv (.xy) -- filtered,
     * slot ids blend across slot edges (dotted rectangle round every number) and
     * atlas uvs point between glyphs (shredded letters). Such units get the point
     * sampler s16 (vp_get_fp_pso). Part of the PSO key through this mask.
     * NHL_TEX_FILTER=0 = old all-linear. */
    static int tf = -1;
    if (tf < 0) { const char* e = getenv("NHL_TEX_FILTER"); tf = (e && *e == '0') ? 0 : 1; }
    if (tf) for (int u = 1; u < D3D_TEX_UNITS; u++)
        if (dr->tex[u].set && ((dr->tex[u].filter >> 24) & 7u) == 1u) m |= 1u << (16 + u);
    return m;
}

/* [NHL local patch] Guest stencil. Every pipeline was built with stencil off,
 * so Apt's masks (mask shape drawn colour-masked into stencil, content drawn
 * with a stencil test) lost the clip: the content drew unmasked -- NHL's
 * breadcrumb showed a solid teal gradient box instead of teal-filled text.
 * Packed key: bit0 enable, [1:3] func, [4:11] read mask, [12:19] write mask,
 * [20:22]/[23:25]/[26:28] fail/zfail/zpass op. STENCIL_OFF=1 restores the old
 * always-off behaviour. */
static u32 rsx_stencil_op_idx(u32 op)
{
    switch (op) {
    case 0x1E00: return 0;   /* KEEP */
    case 0x0000: return 1;   /* ZERO */
    case 0x1E01: return 2;   /* REPLACE */
    case 0x1E02: return 3;   /* INCR (saturating) */
    case 0x1E03: return 4;   /* DECR (saturating) */
    case 0x150A: return 5;   /* INVERT */
    case 0x8507: return 6;   /* INCR_WRAP */
    case 0x8508: return 7;   /* DECR_WRAP */
    default:     return 0;
    }
}

/* [NHL local patch] Set by read_vp_vertex when a flat-fill vertex carries the
 * NHL placeholder marker in attr3 (written by the hand patch in lifted
 * func_0160C4B0 for an image slot no texture provider claimed); consumed by
 * the next draw record. */
static int s_vp_placeholder = 0;

static void dr_set_stencil(D3D12DrawRecord* dr)
{
    dr->placeholder = s_vp_placeholder; s_vp_placeholder = 0;
    dr->stencil_key = 0; dr->stencil_ref = 0;
    static int off = -1;
    if (off < 0) off = getenv("STENCIL_OFF") ? 1 : 0;
    const rsx_state* st = s_d3d.current_rsx_state;
    dr->indexed = 0; dr->ib_first = 0;   /* [NHL #62] set by d3d12_draw_indexed after this */
    dr->gpu_vtx = 0;                     /* [#125] set by the GPU-fetch record path after this */
    dr->surf_ct  = st ? st->color_target : 0xFFu;
    dr->surf_raw = st ? st->surface_color_offset[(st->color_target == 2) ? 1 : 0] : 0;
    { extern void depthcol_note_zeta_ext(u32 off);   /* #209: every zeta in use is a depth-as-colour alias */
      if (st && (st->depth_test_enable || st->depth_mask)) depthcol_note_zeta_ext(st->surface_zeta_offset); }
    /* [NHL local patch #55] SET_SURFACE_COLOR_TARGET = NONE (0): a depth-only
     * pass -- no colour target is written. (Runs after cmask is computed at
     * every record site.) NHL's Starting Lineup ends each frame with such a
     * pass (fp 0x0FDFD681, depth ALWAYS + depth export from the depth texture);
     * drawn in colour it painted RT 0 (black) over the whole screen.
     * NHL_CT_NONE_OLD=1 restores the old colour write. */
    { static int ct_old = -1;
      if (ct_old < 0) ct_old = getenv("NHL_CT_NONE_OLD") ? 1 : 0;
      if (!ct_old && st && st->color_target_set && st->color_target == 0) {
          dr->cmask = 0;
          static int n = 0;
          if (n++ < 4) fprintf(stderr, "[CTNONE] depth-only draw fp=0x%X: colour writes off%c",
                               st->shader_program, 10);
      } }
    if (off || !st || !st->stencil_test_enable) return;
    u32 fn = st->stencil_func;
    fn = (fn >= 0x200) ? ((fn - 0x200) & 7u) : (fn & 7u);
    u32 wm = st->stencil_write_mask_set ? (st->stencil_write_mask & 0xFFu) : 0xFFu;
    dr->stencil_key = 1u | (fn << 1) | ((st->stencil_mask & 0xFFu) << 4) | (wm << 12)
                    | (rsx_stencil_op_idx(st->stencil_op_fail)  << 20)
                    | (rsx_stencil_op_idx(st->stencil_op_zfail) << 23)
                    | (rsx_stencil_op_idx(st->stencil_op_zpass) << 26);
    dr->stencil_ref = st->stencil_ref & 0xFFu;
    { static u32 seen[64]; static int ns = 0; int k;
      u32 key = dr->stencil_key ^ (dr->stencil_ref << 29) ^ ((st->color_mask ? 1u : 0u) << 31);
      for (k = 0; k < ns && seen[k] != key; k++) {}
      if (k == ns && ns < 64) { seen[ns++] = key;
        fprintf(stderr, "[STENCIL] new state: func=%u ref=0x%02X rmask=0x%02X wmask=0x%02X ops=%X/%X/%X cmask=0x%08X fp=0x%08X rt=0x%08X\n",
                fn, dr->stencil_ref, st->stencil_mask & 0xFFu, wm,
                st->stencil_op_fail, st->stencil_op_zfail, st->stencil_op_zpass,
                st->color_mask, st->shader_program, dr->rt_off); } }
}

static int s_fp_buffered = -1;   /* [#78] NHL_FP_BAKED=1 -> 0 */
/* [#86] NHL_FP_SOLID=<hex rt offset>: draws into that offscreen RT output solid
 * magenta (colour target 0) -- does the geometry rasterize at all, independent
 * of what its shaders compute? s_fp_cur_rt is set around vp_get_fp_pso. */
static u32 s_fp_cur_rt = 0;
static u32 s_fp_wrap_id = 0;   /* [#89] program key for NHL_FP_SOLID_MODE=id */
static u32 s_fp_last_key = 0;  /* [#89] key of the last vp_get_fp_pso result (manifest) */
static int fp_solid_wrap(char* hlsl, size_t cap)
{
    static const char a1[] = "float4 main(PSInput input) : SV_TARGET {\n";
    static const char a2[] = "PSOutput main(PSInput input) {\n";
    char* p = strstr(hlsl, a1); int strct = 0;
    if (!p) { p = strstr(hlsl, a2); strct = 1; }
    if (!p) return 0;
    const char* r1 = strct ? "PSOutput _rsx_inner(PSInput input) {\n" : "float4 _rsx_inner(PSInput input) {\n";
    const size_t ol = strct ? sizeof(a2) - 1 : sizeof(a1) - 1, nl = strlen(r1);
    const size_t tail = strlen(p + ol) + 1;
    /* NHL_FP_SOLID_MODE=quad: instead of magenta, four inputs by screen quadrant
     * (TL frac(tc0.xy), TR tex1 at tc0, BL vertex colour, BR frac(tc2.xy/tc2.w))
     * -- which shader input is black, in one run. */
    static int quad = -1;
    if (quad < 0) { const char* e = getenv("NHL_FP_SOLID_MODE");
                    quad = (e && !strcmp(e, "quad")) ? 1 : (e && !strcmp(e, "albedo")) ? 2 :
                           (e && !strcmp(e, "id")) ? 3 : (e && !strcmp(e, "out")) ? 4 :
                           (e && !strcmp(e, "lm")) ? 5 : 0; }
    /* [#97] lm (5): the rink FP's lightmap (unit 2 at tc0.zw) by quadrant --
     * TL rgb, TR alpha, BL frac(tc0.zw), BR saturate(rgb * a * 7.5). */
    if (quad == 5) {
        static const char lb1[] =
            "\nfloat4 main(PSInput input) : SV_TARGET { float4 q = _rsx_inner(input); float2 p = input.position.xy;"
            " float4 l = rsx_tex[2].Sample(rsx_samp[2], input.tc0.zw);"
            " if (p.x < 640 && p.y < 360) return float4(l.rgb, 1) + 0 * q;"
            " if (p.y < 360) return float4(l.aaa, 1);"
            " if (p.x < 640) return float4(frac(input.tc0.zw), 0, 1);"
            " return float4(saturate(l.rgb * l.a * 7.5), 1); }\n";
        static const char lb2[] =
            "\nPSOutput main(PSInput input) { PSOutput _q = _rsx_inner(input); float2 p = input.position.xy;"
            " float4 l = rsx_tex[2].Sample(rsx_samp[2], input.tc0.zw);"
            " if (p.x < 640 && p.y < 360) _q.t0 = float4(l.rgb, 1);"
            " else if (p.y < 360) _q.t0 = float4(l.aaa, 1);"
            " else if (p.x < 640) _q.t0 = float4(frac(input.tc0.zw), 0, 1);"
            " else _q.t0 = float4(saturate(l.rgb * l.a * 7.5), 1); return _q; }\n";
        const char* r1 = strct ? "PSOutput _rsx_inner(PSInput input) {\n" : "float4 _rsx_inner(PSInput input) {\n";
        const size_t ol = strct ? sizeof(a2) - 1 : sizeof(a1) - 1, nl = strlen(r1);
        const char* lb = strct ? lb2 : lb1;
        const size_t tail = strlen(p + ol) + 1;
        if ((size_t)(p - hlsl) + nl + tail + strlen(lb) + 2 > cap) return 0;
        memmove(p + nl, p + ol, tail);
        memcpy(p, r1, nl);
        strcat(hlsl, lb);
        return 1;
    }
    /* [#94] out (4): the program's REAL colour-0 output by quadrant -- TL as is,
     * TR x16, BL its alpha, BR the vertex colour x4. Is the black match scene
     * zero, tiny, alpha-0, or NaN-guarded? */
    if (quad == 4) {
        static const char ob1[] =
            "\nfloat4 main(PSInput input) : SV_TARGET { float4 q = _rsx_inner(input); float2 p = input.position.xy;"
            " if (p.x < 640 && p.y < 360) return float4(saturate(q.rgb), 1);"
            " if (p.y < 360) return float4(saturate(q.rgb * 16), 1);"
            " if (p.x < 640) return float4(q.aaa, 1);"
            " return float4(saturate(input.col0.rgb * 4), 1); }\n";
        static const char ob2[] =
            "\nPSOutput main(PSInput input) { PSOutput _q = _rsx_inner(input); float4 q = _q.t0; float2 p = input.position.xy;"
            " if (p.x < 640 && p.y < 360) _q.t0 = float4(saturate(q.rgb), 1);"
            " else if (p.y < 360) _q.t0 = float4(saturate(q.rgb * 16), 1);"
            " else if (p.x < 640) _q.t0 = float4(q.aaa, 1);"
            " else _q.t0 = float4(saturate(input.col0.rgb * 4), 1); return _q; }\n";
        const char* r1 = strct ? "PSOutput _rsx_inner(PSInput input) {\n" : "float4 _rsx_inner(PSInput input) {\n";
        const size_t ol = strct ? sizeof(a2) - 1 : sizeof(a1) - 1, nl = strlen(r1);
        const char* ob = strct ? ob2 : ob1;
        const size_t tail = strlen(p + ol) + 1;
        if ((size_t)(p - hlsl) + nl + tail + strlen(ob) + 2 > cap) return 0;
        memmove(p + nl, p + ol, tail);
        memcpy(p, r1, nl);
        strcat(hlsl, ob);
        return 1;
    }
    /* id (3): a flat colour per program, from s_fp_wrap_id (logged as [FPID]) --
     * which program paints which surface, read back from one capture. */
    char qid[200];
    if (quad == 3) {
        u32 hsh = s_fp_wrap_id * 2654435761u; hsh ^= hsh >> 13; hsh *= 0x5BD1E995u; hsh ^= hsh >> 15;
        const u32 r = ((hsh >> 16) & 0x7F) | 0x80, g = ((hsh >> 8) & 0x7F) | 0x80, b = (hsh & 0x7F) | 0x80;
        snprintf(qid, sizeof qid, "float4 _rsx_dbg(PSInput input) { return float4(%.4f, %.4f, %.4f, 1); }\n",
                 r / 255.0, g / 255.0, b / 255.0);
        fprintf(stderr, "[FPID] key=0x%08X rgb=%02X%02X%02X%c", s_fp_wrap_id, r, g, b, 10);
    }
    /* quad: four inputs by quadrant; albedo (2): the unlit surface texture
     * (unit 1 at tc0) everywhere -- the scene without its lighting. */
    static const char qbody1[] =
        "float4 _rsx_dbg(PSInput input) { float2 p = input.position.xy;"
        " if (p.x < 640 && p.y < 360) return float4(frac(input.tc0.xy), 0, 1);"
        " if (p.y < 360) return float4(rsx_tex[1].Sample(rsx_samp[1], input.tc0.xy).rgb, 1);"
        " if (p.x < 640) return float4(saturate(input.col0.rgb), 1);"
        " return float4(frac(input.tc2.xy / input.tc2.w), 0, 1); }\n";
    static const char qbody2[] =
        "float4 _rsx_dbg(PSInput input) { return float4(rsx_tex[1].Sample(rsx_samp[1], input.tc0.xy).rgb, 1); }\n";
    const char* qbody = (quad == 3) ? qid : (quad == 2) ? qbody2 : qbody1;
    const char* wrap = quad
        ? (strct
           ? "\nPSOutput main(PSInput input) { PSOutput _q = _rsx_inner(input); _q.t0 = _rsx_dbg(input); return _q; }\n"
           : "\nfloat4 main(PSInput input) : SV_TARGET { float4 _q = _rsx_inner(input); return _rsx_dbg(input) + 0 * _q; }\n")
        : (strct
           ? "\nPSOutput main(PSInput input) { PSOutput _q = _rsx_inner(input); _q.t0 = float4(1,0,1,1); return _q; }\n"
           : "\nfloat4 main(PSInput input) : SV_TARGET { float4 _q = _rsx_inner(input); return float4(1,0,1,1) + 0 * _q; }\n");
    const size_t qlen = quad ? strlen(qbody) + 1 : 0;
    if ((size_t)(p - hlsl) + nl + tail + strlen(wrap) + qlen + 2 > cap) return 0;
    memmove(p + nl, p + ol, tail);
    memcpy(p, r1, nl);
    if (quad) { strcat(hlsl, "\n"); strcat(hlsl, qbody); }   /* before the wrapper main that calls it */
    strcat(hlsl, wrap);
    return 1;
}
/* [#84] buffered-key hashes of programs rsx_fp_decompile_buffered_ex failed on;
 * they are keyed and decompiled in baked mode from then on. */
static u32 s_fp_force_baked[4096]; static int s_fp_force_baked_n = 0;
static u32 s_fp_failed[4096]; static int s_fp_failed_n = 0;   /* [#84] keys that fail in every mode */
u32 g_vs_compiles = 0, g_vs_moved = 0;   /* [#81 diag] see vp_get_vs */

/* [#82] Record-time FP snapshots: a per-batch arena of program bytes. Reset
 * when render_frame consumes the batch. NHL_FP_SNAP=0 reads VRAM at build time
 * as before. */
#define FP_SNAP_ARENA (16u << 20)
/* [NHL local patch #127] Longest fragment program read (bytes). Was 4096 in both
 * the snapshot and the VRAM fallback: NHL's in-match programs of ~188
 * instructions + ~70 inline constants are exactly 4096 bytes BEFORE their END,
 * so the END was never found -- no snapshot, the buffered decompile failed
 * (#84 "kc=0 len=4096"), and the baked fallback compiled a TRUNCATED program
 * with its constants as literals: wrong output, and a new FXC compile for every
 * new constant set (~390 PS at every puck drop, 3-10/s in play, never cached). */
#define FP_MAX_BYTES  16384u
static u8* s_fp_arena; static u32 s_fp_arena_off;
static u32 s_fp_arena_gen;   /* #162: bumped when the arena is reused (batch consumed) */
static const u8* s_fp_snap_cur; static u32 s_fp_snap_cur_len;   /* set around vp_get_fp_pso */
/* [#190] per-unit shadow-compare state for the FP being compiled (fp_shadow_from_draw) */
static rsx_fp_shadow_desc s_fp_cur_shadow[16];
static int s_fp_cur_shadow_on;
static u32 s_fp_cur_clamp;   /* [#240] bit u = unit u clamps s (U), bit 16+u = clamps t (V) */
static u32 s_fp_cur_ptmin;   /* [#245] 2 bits per #176 nearest-mag unit: its MIN filter class (see fp_shadow_from_draw) */
u32 g_fp_snap_changed = 0;
static void fp_snapshot(D3D12DrawRecord* dr)
{
    dr->fp_snap = NULL; dr->fp_snap_len = 0;
    static int on = -1;
    if (on < 0) { const char* e = getenv("NHL_FP_SNAP"); on = (e && *e == '0') ? 0 : 1; }
    extern uint8_t* vm_base;
    if (!on || !dr->fp_addr || !vm_base) return;
    extern u32 cellGcmResolveLocated(int local, u32 offset);
    const u32 off = cellGcmResolveLocated((dr->fp_addr & 0x3u) == 1, dr->fp_addr & ~0x3u);
    if (off == 0xFFFFFFFFu) return;
    const u8* src = vm_base + off;
    u32 len = rsx_fp_program_size(src, FP_MAX_BYTES);   /* #127: was 4096 */
    if (!len) return;
    /* Copy a tail past END too: parts of the decompiler (its constant
     * pre-walk) read beyond the last instruction, and an exact-length copy made
     * some programs fail to decompile (play77: 16+ "[FP] decompile fail", none
     * when reading VRAM). The program part is what the snapshot pins. */
    len += 256u; if (len > FP_MAX_BYTES) len = FP_MAX_BYTES;
    /* consecutive draws usually share the program: reuse an identical copy.
     * [#183] the last 8 programs this batch (by source address), not only the last:
     * draws alternating between a few programs copied a new snapshot every draw and
     * so missed vp_get_fp_pso's walk memo too. */
    static struct { const u8* src; const u8* copy; u32 len, gen; } rc[8]; static unsigned rn;
    for (int i = 0; i < 8; i++)
        if (rc[i].copy && rc[i].src == src && rc[i].len == len && rc[i].gen == s_fp_arena_gen &&
            memcmp(rc[i].copy, src, len) == 0) {
            dr->fp_snap = rc[i].copy; dr->fp_snap_len = len; return;
        }
    if (!s_fp_arena) s_fp_arena = (u8*)malloc(FP_SNAP_ARENA);
    if (!s_fp_arena || s_fp_arena_off + len > FP_SNAP_ARENA) return;   /* falls back to VRAM */
    u8* d = s_fp_arena + s_fp_arena_off;
    memcpy(d, src, len);
    s_fp_arena_off += (len + 15u) & ~15u;
    dr->fp_snap = d; dr->fp_snap_len = len;
    { const unsigned k = rn++ & 7u; rc[k].src = src; rc[k].copy = d; rc[k].len = len; rc[k].gen = s_fp_arena_gen; }
}
/* [#80] guest-FP PSO cache index: 1-based entry index, 0 = end of chain */
static int s_fph_head[VP_FP_BUCKETS];
static int s_fph_next[VP_FP_CACHE];

/* [#78] Point the buffered decompiler's constant block at the backend's b1
 * layout (vp_record_cb): rsx_texscale[4] at float4 0..3, the alpha-test word at
 * 4, fp_k[64] from 5. The decompiler declares fp_constants[N] then fp_alpha;
 * nothing in this backend reads fp_alpha (alpha test is not applied through
 * the decompiler here). Returns 0 if the declaration was not found. */
static int fp_rebind_constants(char* hlsl, size_t cap)
{
    char* b = strstr(hlsl, "cbuffer PSConstants : register(b1) {");
    if (!b) return 0;
    char* e = strstr(b, "};\n");
    if (!e) return 0;
    e += 3;
    static const char decl[] =
        "cbuffer PSConstants : register(b1) {\n"
        "    float4 rsx_texscale[4];\n"
        "    float4 fp_alpha;\n"
        "    float4 fp_constants[256];\n"
        "};\n";
    const size_t oldlen = (size_t)(e - b), newlen = sizeof(decl) - 1, tail = strlen(e) + 1;
    if ((size_t)(b - hlsl) + newlen + tail > cap) return 0;
    memmove(b + newlen, e, tail);
    memcpy(b, decl, newlen);
    return 1;
}

static ID3D12PipelineState* vp_get_fp_pso(int vs_idx, u32 fp_addr, u32 blend, int nrt,
                                          DXGI_FORMAT rtfmt, int exp32, u32 cmask, u32 cull,
                                          u32 cube_mask, u32 stencil)
{
    if (nrt < 1) nrt = 1; if (nrt > 4) nrt = 4;
    if (rtfmt == 0) rtfmt = DXGI_FORMAT_R8G8B8A8_UNORM;
    extern uint8_t* vm_base;
    extern u32 cellGcmResolveOffset(u32);
    if (!fp_addr || !vm_base) return NULL;
    { static int _off=-1; if(_off<0)_off=getenv("FP_OFF")?1:0; if(_off) return NULL; }
    /* Resolve the VS: the draw's cached per-VP blob, else the primary. */
    ID3DBlob* vsb = NULL; u32 vs_hash = 0;
    if (vs_idx >= 0 && vs_idx < s_d3d.vp_vs_n) {
        vsb = s_d3d.vp_vs[vs_idx].vs; vs_hash = s_d3d.vp_vs[vs_idx].hash;
        /* #225: its own VS is still compiling in the background -- wait, don't
         * draw it with the primary VS */
        if (!vsb && async_on() && s_async_sync) {   /* #229: an essential draw waits for it instead */
            async_wait_vs(vs_hash); vsb = s_d3d.vp_vs[vs_idx].vs; }
        if (!vsb && async_on()) { if (!s_fxc_collect) InterlockedIncrement(&s_async_skipped); return NULL; }
    }
    if (!vsb) { vsb = s_d3d.vp_vs_blob; vs_idx = -1; }
    if (!vsb) return NULL;

    /* SET_SHADER_PROGRAM low bits = location+1 (same as textures): 1 = LOCAL
     * (VRAM), 2 = MAIN (gcm/cube emits 0x00B90001 for its VRAM-resident FP). */
    extern u32 cellGcmResolveLocated(int local, u32 offset);
    u32 off = cellGcmResolveLocated((fp_addr & 0x3u) == 1, fp_addr & ~0x3u);
    if (off == 0xFFFFFFFFu) return NULL;
    /* [NHL local patch #82] Build from the program bytes captured when the draw
     * was RECORDED (fp_snapshot), not from VRAM now: NHL streams FPs through a
     * VRAM ring, and by the time the batch renders the slot may hold the next
     * program -- a half-overwritten program hashes as "new" every time and
     * compiles the wrong shader. g_fp_snap_changed counts how often VRAM no
     * longer matched the snapshot. */
    const u8* fpuc = vm_base + off; u32 fpmax = FP_MAX_BYTES;   /* #127: was 4096 */
    /* [NHL local patch #162] The VRAM-vs-snapshot compare only feeds PSOMISSWHY's
     * fp_vram_changed counter: a program walk + memcmp per draw, now only with PSOMISSWHY. */
    static int s_snapdiag = -1; if (s_snapdiag < 0) s_snapdiag = getenv("PSOMISSWHY") ? 1 : 0;
    if (s_fp_snap_cur && s_fp_snap_cur_len) {
        if (s_snapdiag) { const u32 plen = rsx_fp_program_size(s_fp_snap_cur, s_fp_snap_cur_len);
          if (plen && memcmp(s_fp_snap_cur, fpuc, plen) != 0) g_fp_snap_changed++; }
        fpuc = s_fp_snap_cur; fpmax = s_fp_snap_cur_len;
    }

    int fp_use_buffered = 0;   /* [#78] set below */
    /* Content hash: inline constants are patched in place per frame (wave's
     * stamp), so identity is the BYTES, not the address. */
    /* Hash the CODE, not the constants: with fp_k[] hoisted into the per-draw
     * constant buffer the compiled shader is invariant under constant changes,
     * so hashing them made the pipeline key move every draw. */
    u32 uhash;
    /* [NHL local patch #162] Consecutive draws share one snapshot copy (fp_snapshot), and a
     * snapshot never changes until the arena is reused: remember the program walks
     * (size, constant count, code hash) for the last snapshot instead of redoing all
     * three per draw. NHL_FP_MEMO=0 = walk every draw. */
    static int s_memo_on = -1; if (s_memo_on < 0) { const char* e = getenv("NHL_FP_MEMO"); s_memo_on = (e && *e == '0') ? 0 : 1; }
    /* [#183] 8 snapshots remembered, not 1 (draws alternating between programs) */
    typedef struct { const u8* p; u32 len, gen, usz, code; int k, valid; } FpMemo;
    static FpMemo s_fpm[8]; static unsigned s_fpm_n;
    FpMemo* me = NULL;
    if (s_memo_on && s_fp_snap_cur && fpuc == s_fp_snap_cur)
        for (int i = 0; i < 8; i++)
            if (s_fpm[i].valid && s_fpm[i].p == fpuc && s_fpm[i].len == fpmax && s_fpm[i].gen == s_fp_arena_gen) { me = &s_fpm[i]; break; }
    const int memo_hit = me != NULL;
    {
        u32 usz = 0, code_walk = 0;   /* #289: size + code hash in one walk on a memo miss */
        if (memo_hit) usz = me->usz; else code_walk = rsx_fp_code_hash_size(fpuc, fpmax, &usz);
        if (usz == 0) usz = 64;
        s_perf_pso_hashbytes += (int)usz;
        /* [NHL local patch #59] Default: hash ALL program bytes, constants
         * included. The decompiler in use (rsx_fp_decompile) bakes inline
         * constants into the HLSL; it never reads fp_k[] from b1, so a
         * code-only hash let programs differing only in constants share a
         * PSO. NHL_FP_CODEHASH=1 restores the code-only hash. */
        static int codehash = -1;
        if (codehash < 0) codehash = getenv("NHL_FP_CODEHASH") ? 1 : 0;
        /* [NHL local patch #78] Buffered constants (default; NHL_FP_BAKED=1 =
         * the old literal mode): the decompiler reads inline constants from
         * fp_constants[] in b1 (filled per draw by vp_record_cb), so the shader
         * does not change when the title re-patches a constant and the key is
         * the code alone. play71: in the match the RSX thread spent ~70% in
         * D3DCompile recompiling programs whose constants changed every frame;
         * the game's GPU-fence waits timed out (DumpGraphicsError) and it ran
         * at ~0.5 fps. Programs with more constants than b1 holds (64) keep
         * the baked literals and the full-content key. */
        if (s_fp_buffered < 0) s_fp_buffered = getenv("NHL_FP_BAKED") ? 0 : 1;
        fp_use_buffered = 0;
        if (s_fp_buffered) {
            static float kscratch[4 * 300];
            const int nk = memo_hit ? me->k : rsx_fp_extract_consts(fpuc, fpmax, kscratch, 300);
            fp_use_buffered = nk <= 256;   /* #83: was 64 */
            if (s_memo_on && !memo_hit && s_fp_snap_cur && fpuc == s_fp_snap_cur) {   /* #162: remember this snapshot */
                me = &s_fpm[s_fpm_n++ & 7u];
                me->p = fpuc; me->len = fpmax; me->gen = s_fp_arena_gen; me->usz = usz; me->k = nk;
                me->code = code_walk; me->valid = 1;
            }
        }
        if (fp_use_buffered) {
            uhash = (memo_hit ? me->code : code_walk) ^ 0x9E3779B9u;   /* never equal to a baked key */
            /* [#84] programs the buffered decompile already failed on go baked */
            for (int _b = 0; _b < s_fp_force_baked_n; _b++)
                if (s_fp_force_baked[_b] == uhash) { fp_use_buffered = 0; break; }
        }
        if (fp_use_buffered) {
            /* keyed above */
        } else
        if (!codehash || (getenv("FP_CONSTBUF") && getenv("FP_CONSTBUF")[0] == '0')) {
            uhash = 2166136261u;
            const u8* up = fpuc;
            for (u32 i = 0; i < usz; i++) { uhash ^= up[i]; uhash *= 16777619u; }
        } else {
            uhash = memo_hit ? me->code : code_walk;
        }
    }
    const u32 uhash_tap = uhash;   /* #204: the program key before the solid/shadow variant xors (FP tap) */

    int fp_solid = 0;   /* [#86] NHL_FP_SOLID */
    { static u32 solid_rt = 0xFFFFFFFFu;
      if (solid_rt == 0xFFFFFFFFu) { const char* e = getenv("NHL_FP_SOLID"); solid_rt = e ? (u32)strtoul(e, NULL, 16) : 0u; }
      if (solid_rt && s_fp_cur_rt == solid_rt) { fp_solid = 1; uhash ^= 0x5011D000u; } }
    if (s_fp_cur_shadow_on) {   /* #190: the shadow-compare variant is a different shader */
        for (int u = 0; u < 16; u++)
            if (s_fp_cur_shadow_on & (1 << u))
                uhash ^= (0x5AD0F00Du + (u32)u * 0x9E3779B1u) ^ ((u32)s_fp_cur_shadow[u].func << 8) ^
                         ((u32)s_fp_cur_shadow[u].linear << 12) ^ ((u32)s_fp_cur_shadow[u].unnormalized << 13);
    }
    if (s_fp_cur_clamp) uhash ^= 0xC1A40000u ^ (s_fp_cur_clamp * 0x85EBCA77u);   /* #240: clamp-sampled variant */
    if (s_fp_cur_ptmin) uhash ^= 0x3A1F0000u ^ (s_fp_cur_ptmin * 0xC2B2AE35u);   /* #245: min-filter variant */
    s_perf_pso_calls++;
    /* [NHL local patch #59] Key on CONTENT, not address: NHL streams its
     * in-game fragment programs through a VRAM ring, so the same program sits
     * at a new fp_addr frame to frame. Keyed by address, ~95 draws per frame
     * missed and re-decompiled + recompiled (play34: 81% of frame time in PSO
     * creation, 0.7 fps). */
    /* [#80] walk only this program's hash chain (s_fph_head/s_fph_next). */
    for (int i = s_fph_head[uhash & (VP_FP_BUCKETS - 1)] - 1; i >= 0; i = s_fph_next[i] - 1)
        if (s_d3d.vp_fp[i].ucode_hash != uhash) continue;   /* cheap reject first */
        else if (s_d3d.vp_fp[i].vs_idx == vs_idx &&
            s_d3d.vp_fp[i].vs_hash == vs_hash &&
            /* [#79] gen tracks only the PRIMARY VS blob (vs_idx -1); a per-VP
             * entry is identified by vs_hash. Requiring gen everywhere threw the
             * whole cache away on every primary-VS rebuild. */
            (vs_idx >= 0 || s_d3d.vp_fp[i].gen == s_d3d.vp_gen) &&
            s_d3d.vp_fp[i].blend == blend && s_d3d.vp_fp[i].nrt == nrt &&
            s_d3d.vp_fp[i].rtfmt == (u32)rtfmt && s_d3d.vp_fp[i].exp32 == exp32 &&
            s_d3d.vp_fp[i].ucode_hash == uhash && s_d3d.vp_fp[i].cmask == cmask &&
            s_d3d.vp_fp[i].cull == cull &&
            s_d3d.vp_fp[i].cube_mask == cube_mask &&
            s_d3d.vp_fp[i].stencil == stencil)
            { s_fp_last_key = uhash; return s_d3d.vp_fp[i].pso; }   /* #89 */
    s_perf_pso_miss++;      /* falls through to a full decompile + D3DCompile */
    for (int _q = 0; _q < s_fp_failed_n; _q++) if (s_fp_failed[_q] == uhash) return NULL;   /* [#84] */
    /* [#227] a draw already known to be waiting: no decompile until its PS / PSO can be made */
    FppKey fpk; memset(&fpk, 0, sizeof fpk);
    const int fpp_on = fpp_enabled() && async_on() && !s_async_sync;
    if (fpp_on) {
        fpk.uhash = uhash; fpk.vs_hash = vs_hash; fpk.blend = blend; fpk.rtfmt = (u32)rtfmt;
        fpk.exp32 = (u32)exp32; fpk.cmask = cmask; fpk.cull = cull; fpk.cube_mask = cube_mask;
        fpk.stencil = stencil; fpk.gen = vs_idx < 0 ? s_d3d.vp_gen : 0u; fpk.vs_idx = vs_idx; fpk.nrt = nrt;
        const FppEnt* pe = fpp_find(&fpk);
        if (pe) {
            if (!fxc_has(pe->k1, pe->k2, 1u, pe->len)) {   /* PS still compiling */
                InterlockedIncrement(&s_fpp_hits);
                if (!s_fxc_collect) InterlockedIncrement(&s_async_skipped);
                return NULL;
            }
            if (s_pso_frame_us > async_pso_budget()) {     /* PSO still over this frame's budget */
                InterlockedIncrement(&s_fpp_hits);
                InterlockedIncrement(&s_async_pso_deferred);
                return NULL;
            }
        }   /* bytecode ready and budget left: the full path below builds the PSO */
    }
    /* [#79] PSOMISSWHY=1: every 5 s, why misses missed. For each miss, an entry
     * with the same program hash is compared field by field ("new" = no entry
     * for that program at all). Unlike PSOMISSDBG this matches by content, so
     * NHL's ring-moving fp_addr does not hide every miss. */
    { static int mw = -1; if (mw < 0) mw = getenv("PSOMISSWHY") ? 1 : 0;
      if (mw) {
          static u32 c_new, c_vs, c_gen, c_blend, c_rt, c_exp, c_cmask, c_cull, c_cube, c_sten, c_tot, c_baked;
          static ULONGLONG t0 = 0;
          c_tot++; if (!fp_use_buffered) c_baked++;
          VPFPEntry* q = NULL;
          for (int i = 0; i < s_d3d.vp_fp_n; i++) if (s_d3d.vp_fp[i].ucode_hash == uhash) { q = &s_d3d.vp_fp[i]; break; }
          if (!q) c_new++;
          else {
              if (q->vs_idx != vs_idx || q->vs_hash != vs_hash) c_vs++;
              if (q->gen != s_d3d.vp_gen) c_gen++;
              if (q->blend != blend) c_blend++;
              if (q->nrt != nrt || q->rtfmt != (u32)rtfmt) c_rt++;
              if (q->exp32 != exp32) c_exp++;
              if (q->cmask != cmask) c_cmask++;
              if (q->cull != cull) c_cull++;
              if (q->cube_mask != cube_mask) c_cube++;
              if (q->stencil != stencil) c_sten++;
          }
          const ULONGLONG t = GetTickCount64(); if (!t0) t0 = t;
          if (t - t0 >= 5000) {
              extern u32 g_vs_compiles, g_vs_moved;
              fprintf(stderr, "[PSOMISSWHY] misses=%u new_program=%u | vs=%u gen=%u blend=%u rt=%u ctl=%u cmask=%u cull=%u cube=%u stencil=%u | cache=%d gen_now=%u | vs_compiles=%u vs_moved=%u vs_cache=%d | fp_vram_changed=%u baked=%u\n",
                      c_tot, c_new, c_vs, c_gen, c_blend, c_rt, c_exp, c_cmask, c_cull, c_cube, c_sten,
                      s_d3d.vp_fp_n, (unsigned)s_d3d.vp_gen, g_vs_compiles, g_vs_moved, s_d3d.vp_vs_n,
                      g_fp_snap_changed, c_baked);
              g_vs_compiles = g_vs_moved = 0; g_fp_snap_changed = 0;
              c_new = c_vs = c_gen = c_blend = c_rt = c_exp = c_cmask = c_cull = c_cube = c_sten = c_tot = c_baked = 0;
              t0 = t;
          } } }
    /* PSOMISSDBG=1: on a miss, name the key field that differs from an existing
     * entry for the same program. "the cache misses" is not actionable; "it
     * misses on ucode_hash" is. */
    /* HASHDBG=1: how many DISTINCT ucode hashes each program produces. Inline
     * constants are patched into the shader bytes, so a program whose constants
     * animate yields a new hash -- and a new pipeline -- every time. */
    { static int hd = -1;
      if (hd < 0) { const char* e = getenv("HASHDBG"); hd = e ? atoi(e) : 0; }
      if (hd) { static u32 fps[32]; static u32 cnt[32]; static int nf = 0;
        int idx = -1;
        for (int i = 0; i < nf; i++) if (fps[i] == fp_addr) idx = i;
        if (idx < 0 && nf < 32) { idx = nf++; fps[idx] = fp_addr; cnt[idx] = 0; }
        if (idx >= 0) { cnt[idx]++;
          if ((cnt[idx] % 200) == 0)
            fprintf(stderr, "[HASHDBG] fp=0x%X has produced %u distinct compiles%c",
                    fp_addr, cnt[idx], 10); } } }
    { static int md = -1;
      if (md < 0) { const char* e = getenv("PSOMISSDBG"); md = e ? atoi(e) : 0; }
      if (md) { static int n = 0;
        for (int i = 0; i < s_d3d.vp_fp_n && n < 12; i++) {
            VPFPEntry* q = &s_d3d.vp_fp[i];
            if (q->fp_addr != fp_addr) continue;
            n++;
            fprintf(stderr, "[PSOMISS] fp=0x%X differs:%s%s%s%s%s%s%s%s%c", fp_addr,
                    q->vs_idx != vs_idx        ? " vs_idx"     : "",
                    q->vs_hash != vs_hash      ? " vs_hash"    : "",
                    q->gen != s_d3d.vp_gen     ? " gen"        : "",
                    q->blend != blend          ? " blend"      : "",
                    q->nrt != nrt              ? " nrt"        : "",
                    q->rtfmt != (u32)rtfmt     ? " rtfmt"      : "",
                    q->ucode_hash != uhash     ? " ucode_hash" : "",
                    q->cube_mask != cube_mask  ? " cube_mask"  : "", 10);
            break;
        } } }
    /* CUBEKEYDBG=1: which (program, cube mask) pairs are being compiled. If one
     * program shows several masks, the mask is unstable and is the reason the
     * pipeline cache thrashes with cube support on. */
    { static int ck = -1;
      if (ck < 0) { const char* e = getenv("CUBEKEYDBG"); ck = e ? atoi(e) : 0; }
      if (ck) { static u32 seen[64][2]; static int ns = 0; int known = 0;
        for (int i = 0; i < ns; i++)
            if (seen[i][0] == fp_addr && seen[i][1] == cube_mask) known = 1;
        if (!known && ns < 64) { seen[ns][0] = fp_addr; seen[ns][1] = cube_mask; ns++;
            fprintf(stderr, "[CUBEKEY] fp=0x%X cube_mask=0x%X (distinct pairs=%d)%c",
                    fp_addr, cube_mask, ns, 10); } } }
    static char hlsl[RSX_FP_HLSL_BUFFER_SIZE];
    /* Signature changed when the newer decompiler was adopted: the third
     * parameter is now SET_SHADER_CONTROL, not an exports-32 flag. This
     * backend does not track that register, so let the decompiler infer it. */
    /* [NHL local patch #54] exp32 now carries the draw's real SET_SHADER_CONTROL
     * word (low 16 bits): it picks h0 vs r0 colour exports the way the RSX does
     * and enables depth export (r1.z -> SV_Depth). NHL_FP_CTRL_AUTO=1 restores
     * the decompiler's old written-register heuristic. */
    static int fp_auto = -1;
    if (fp_auto < 0) fp_auto = getenv("NHL_FP_CTRL_AUTO") ? 1 : 0;
    const u32 fp_ctrl = fp_auto ? RSX_FP_CTRL_AUTO : ((u32)exp32 & 0xFFFFu);
    int n;
    /* [NHL local patch #191] Hand the guest's cube units to the decompiler. It was given 0,
     * so every program declared one Texture2D rsx_tex[16] array and sampled cube units with
     * a 2D .xy -- while the pre-pass binds those units with a TEXTURECUBE SRV (dimension
     * mismatch: undefined, reads 0 on this GPU). The backend's later "cube retarget" string
     * edit only matched the OLD decompiler's per-unit declarations, so it never fired. The
     * player shader's units 5/6 are its environment/ambient cubes (play258 dump: cube mask
     * 0x0060, sampled as Texture2D). NHL_FP_CUBE=0 = the old 0 mask. */
    static int s_fpcube = -1;
    if (s_fpcube < 0) { const char* e = getenv("NHL_FP_CUBE"); s_fpcube = (e && *e == '0') ? 0 : 1; }
    const u32 dcube = s_fpcube ? (cube_mask & 0xFFFFu) : 0u;
    if (fp_use_buffered) {   /* [#78] constants from b1 */
        u32 kc = 0;
        n = s_fp_cur_shadow_on   /* #190: shadow-map units compare in the shader */
            ? rsx_fp_decompile_buffered_shadow_ex(fpuc, fpmax, fp_ctrl, dcube, s_fp_cur_shadow, hlsl, sizeof(hlsl), &kc)
            : rsx_fp_decompile_buffered_ex(fpuc, fpmax, fp_ctrl, dcube, hlsl, sizeof(hlsl), &kc);
        if (n > 0 && kc > 0 && !fp_rebind_constants(hlsl, sizeof(hlsl))) {
            /* No constant block to rebind (or no room): this key is the
             * code-only hash, so a baked shader would be wrong for other
             * constant values. Refuse rather than cache it. */
            static int _rb = 0; if (_rb++ < 8)
                fprintf(stderr, "[FP] fp=0x%08X: buffered constant block not rebound -- skipped%c", fp_addr, 10);
            return NULL;
        }
        { static int _once = 0; if (!_once++) fprintf(stderr, "[FP] buffered fragment constants on (#78)%c", 10); }
        if (n <= 0) {
            /* [#84] Buffered decompile failed (play79: 1000+ misses per 5 s,
             * none cached, drawn with the fallback PS). Remember the program,
             * dump its bytes for offline debugging (fpfail/, first 16), and
             * decompile it baked under the full-content key instead. */
            if (s_fp_force_baked_n < (int)(sizeof s_fp_force_baked / sizeof s_fp_force_baked[0]))
                s_fp_force_baked[s_fp_force_baked_n++] = uhash;
            { static int nd = 0;
              if (nd < 16) { nd++;
                  CreateDirectoryA("fpfail", NULL);
                  char fn[64]; snprintf(fn, sizeof fn, "fpfail/fp_%08X.bin", uhash);
                  FILE* f = fopen(fn, "wb"); if (f) { fwrite(fpuc, 1, fpmax, f); fclose(f); }
                  fprintf(stderr, "[FP] buffered decompile failed (fp=0x%08X kc=%u len=%u) -> baked; dumped %s%c",
                          fp_addr, kc, fpmax, fn, 10); } }
            uhash = 2166136261u;
            { const u32 usz2 = rsx_fp_program_size(fpuc, fpmax);
              for (u32 i = 0; i < (usz2 ? usz2 : 64u); i++) { uhash ^= fpuc[i]; uhash *= 16777619u; } }
            n = rsx_fp_decompile_ex(fpuc, fpmax, fp_ctrl, dcube, hlsl, sizeof(hlsl));   /* #191 */
        }
    } else
        n = rsx_fp_decompile_ex(fpuc, fpmax, fp_ctrl, dcube, hlsl, sizeof(hlsl));
    if (n <= 0) {
        /* [#84] negative cache: never re-decompile a program that cannot be */
        for (int _q = 0; _q < s_fp_failed_n; _q++) if (s_fp_failed[_q] == uhash) return NULL;
        if (s_fp_failed_n < 4096) s_fp_failed[s_fp_failed_n++] = uhash;
        static int _e=0; if(_e++<16) printf("[FP] decompile fail (fp=0x%08X)\n", fp_addr); return NULL; }
    s_fp_wrap_id = uhash;   /* #89 id mode */
    if (fp_solid && !fp_solid_wrap(hlsl, sizeof(hlsl))) {
        static int _w = 0; if (_w++ < 4) fprintf(stderr, "[FP_SOLID] could not wrap fp=0x%08X%c", fp_addr, 10); }
    /* [NHL local patch #253] At a render scale != 1, WPOS (input.position) reads guest pixels again:
     * x fp_alpha.w (= 1/scale of the draw's target, written per draw). Needs the backend's b1
     * layout (#78 rebind; also done here for programs without constants). Only at scale != 1, so
     * 1x shader text -- and the disk shader cache -- are unchanged. */
    if (rs_active() && strstr(hlsl, "input.position")) {
        static const char lay[] = "    float4 rsx_texscale[4];\n    float4 fp_alpha;\n";
        if (!strstr(hlsl, lay)) fp_rebind_constants(hlsl, sizeof(hlsl));
        if (strstr(hlsl, lay)) {
            static const char from[] = "input.position", to[] = "(input.position * float4(fp_alpha.ww, 1, 1))";
            const size_t fl = sizeof(from) - 1, tl = sizeof(to) - 1;
            int nrep = 0;
            for (char* at = strstr(hlsl, from); at; at = strstr(at + tl, from)) {
                const size_t tail = strlen(at + fl) + 1;
                if ((size_t)(at - hlsl) + tl + tail > sizeof(hlsl)) break;
                memmove(at + tl, at + fl, tail); memcpy(at, to, tl); nrep++;
            }
            { static int n = 0; if (nrep && n++ < 8) fprintf(stderr, "[RSCALE] WPOS scaled in fp=0x%08X key=0x%08X (%d reads)%c", fp_addr, uhash, nrep, 10); }
        } else { static int n = 0; if (n++ < 8) fprintf(stderr, "[RSCALE] fp=0x%08X reads WPOS but has no b1 block: left at resource pixels%c", fp_addr, 10); }
    }
    /* [#99] NHL_FP_TAP_KEY=<hex key> + NHL_FP_TAP="E0|E1|E2|E3": for that one
     * program, colour output 0 becomes four HLSL expressions (float3, in the
     * decompiled program's own registers) by screen quadrant TL|TR|BL|BR --
     * which intermediate term of a shader goes to zero. */
    { static u32 tk = 0xFFFFFFFFu; static char tx[4][256];
      static u32 tks[16]; static int ntk = 0;   /* #205: NHL_FP_TAP_KEY may list up to 16 keys (comma separated) */
      if (tk == 0xFFFFFFFFu) { const char* e = getenv("NHL_FP_TAP_KEY"); tk = e ? (u32)strtoul(e, NULL, 16) : 0u;
          for (const char* c = e; c && *c && ntk < 16; ) { char* end; u32 v = (u32)strtoul(c, &end, 16);
              if (end == c) break; tks[ntk++] = v; c = (*end == ',') ? end + 1 : end; }
          const char* t = getenv("NHL_FP_TAP"); for (int q = 0; q < 4; q++) strcpy(tx[q], "0,0,0");
          for (int q = 0; t && *t && q < 4; q++) { const char* bar = strchr(t, '|'); size_t n = bar ? (size_t)(bar - t) : strlen(t);
              if (n > 255) n = 255; memcpy(tx[q], t, n); tx[q][n] = 0; t = bar ? bar + 1 : NULL; } }
      int hit = 0;
      for (int _k = 0; _k < ntk; _k++) if (uhash == tks[_k] || uhash_tap == tks[_k]) hit = 1;
      if (tk && hit) {
          static const char s1[] = "    _out.t0 = (_o0 == _o0) ? _o0 : (float4)0;\n";
          static const char s2[] = "    return (_o == _o) ? _o : (float4)0;\n";
          char rep[1400];
          char* at = strstr(hlsl, s1); int strct = 1;
          if (!at) { at = strstr(hlsl, s2); strct = 0; }
          if (at) {
              snprintf(rep, sizeof rep,
                  "    { float2 _tp = input.position.xy; float4 _tv = (_tp.y < 360) ? ((_tp.x < 640) ? float4(%s, 1) : float4(%s, 1))"
                  " : ((_tp.x < 640) ? float4(%s, 1) : float4(%s, 1)); _tv = (_tv == _tv) ? _tv : float4(1,0,1,1); %s }\n",
                  tx[0], tx[1], tx[2], tx[3], strct ? "_out.t0 = _tv;" : "return _tv;");
              const size_t ol = strct ? sizeof(s1) - 1 : sizeof(s2) - 1, nl = strlen(rep), tail = strlen(at + ol) + 1;
              if ((size_t)(at - hlsl) + nl + tail < sizeof(hlsl)) {
                  memmove(at + nl, at + ol, tail); memcpy(at, rep, nl);
                  fprintf(stderr, "[FPTAP] key=0x%08X fp=0x%08X tapped%c", uhash, fp_addr, 10);
              } } } }
    /* [#203] NHL_FP_PROJ0=1: projective samples "X.xy / X.w" of tc6-tc9 (NHL's spot
     * light cookies) sample uv (0,0) when X.w == 0 instead of 0/0 = NaN -- with zero
     * light matrices (warmup entrances) D3D12 returns ~0 there; RSX may not. */
    { static int pz = -1; if (pz < 0) { const char* e = getenv("NHL_FP_PROJ0"); pz = e ? atoi(e) : 0; }
      if (pz && strstr(hlsl, ").xyzw).w)")) {
          int nrep = 0;
          for (int tcn = 6; tcn <= 9; tcn++) {
              char pat[96], rep[64];
              snprintf(pat, sizeof pat, "((input.tc%d).xyzw).xy / ((input.tc%d).xyzw).w", tcn, tcn);
              snprintf(rep, sizeof rep, "rsx_pz(input.tc%d)", tcn);
              for (char* at = strstr(hlsl, pat); at; at = strstr(at, pat)) {
                  const size_t ol = strlen(pat), nl = strlen(rep), tail = strlen(at + ol) + 1;
                  memmove(at + nl, at + ol, tail); memcpy(at, rep, nl); at += nl; nrep++;
              }
          }
          if (nrep) {
              static const char fn[] = "float2 rsx_pz(float4 t) { return (t.w == 0.0) ? float2(0.0, 0.0) : t.xy / t.w; }\n";
              char* m = strstr(hlsl, "PSOutput main(");
              if (!m) m = strstr(hlsl, "float4 main(");
              if (m) { const size_t nl = sizeof(fn) - 1, tail = strlen(m) + 1;
                  if ((size_t)(m - hlsl) + nl + tail < sizeof(hlsl)) { memmove(m + nl, m, tail); memcpy(m, fn, nl); } }
              static int _n = 0; if (_n++ < 8) fprintf(stderr, "[PROJ0] fp=0x%08X key=0x%08X %d projective samples guarded%c", fp_addr, uhash, nrep, 10);
          } } }
    /* [#202] NHL_FP_NAN=1: colour output 0 of EVERY program shows MAGENTA where any
     * component is NaN (the decompiler's guard zeroes NaN lanes -> pure black), and
     * NHL_FP_NAN=2 also flags +-Inf -- the warmup players render exactly 0,0,0. */
    { static int nn = -1; if (nn < 0) { const char* e = getenv("NHL_FP_NAN"); nn = e ? atoi(e) : 0; }
      if (nn) {
          static const char s1[] = "    _out.t0 = (_o0 == _o0) ? _o0 : (float4)0;\n";
          const char* rep = nn == 2
              ? "    _out.t0 = (all(_o0 == _o0) && all(abs(_o0) < 1e30)) ? _o0 : float4(1,0,1,1);\n"
              : "    _out.t0 = all(_o0 == _o0) ? _o0 : float4(1,0,1,1);\n";
          char* at = strstr(hlsl, s1);
          if (at) {
              const size_t ol = sizeof(s1) - 1, nl = strlen(rep), tail = strlen(at + ol) + 1;
              if ((size_t)(at - hlsl) + nl + tail < sizeof(hlsl)) { memmove(at + nl, at + ol, tail); memcpy(at, rep, nl); }
          } } }
    /* [NHL local patch #103] Alpha-to-coverage emulation. NHL draws the rink
     * glass and the net with blending OFF and alpha-to-coverage ON (2x MSAA): the
     * hardware keeps a share of the samples proportional to alpha. This backend
     * renders single-sampled, so A2C was ignored and the glass drew OPAQUE --
     * the "doors" hiding the ice, boards and players. Emulated as an ordered
     * 4x4 dither discard on colour-0 alpha (the Xbox port used a plain alpha
     * test for its net). */
    if (((u32)exp32 >> 25) & 1u) {
        static const char s1[] = "    _out.t0 = (_o0 == _o0) ? _o0 : (float4)0;\n";
        static const char s2[] = "    return (_o == _o) ? _o : (float4)0;\n";
        static const char d1[] =
            "    _out.t0 = (_o0 == _o0) ? _o0 : (float4)0;\n"
            "    { const float _bay[16] = { 0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5 };"
            " uint2 _dq = uint2(input.position.xy) & 3u;"
            " if (saturate(_out.t0.a) <= (_bay[_dq.y * 4u + _dq.x] + 0.5) / 16.0) discard; }\n";
        static const char d2[] =
            "    { float4 _a2c = (_o == _o) ? _o : (float4)0;"
            " const float _bay[16] = { 0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5 };"
            " uint2 _dq = uint2(input.position.xy) & 3u;"
            " if (saturate(_a2c.a) <= (_bay[_dq.y * 4u + _dq.x] + 0.5) / 16.0) discard; return _a2c; }\n";
        /* #174: modes 2/3 discard only below a fixed alpha (blend mode: no sample covered) */
        char t1[192], t2[192];
        const char* thr = a2c_mode() == 3 ? "0.5" : "0.125";
        snprintf(t1, sizeof t1, "    _out.t0 = (_o0 == _o0) ? _o0 : (float4)0;\n"
                                "    if (saturate(_out.t0.a) < %s) discard;\n", thr);
        snprintf(t2, sizeof t2, "    { float4 _a2c = (_o == _o) ? _o : (float4)0;"
                                " if (saturate(_a2c.a) < %s) discard; return _a2c; }\n", thr);
        char* at = strstr(hlsl, s1); int strct = 1;
        if (!at) { at = strstr(hlsl, s2); strct = 0; }
        if (at) {
            const char* rep = a2c_mode() == 1 ? (strct ? d1 : d2) : (strct ? t1 : t2);
            const size_t ol = strct ? sizeof(s1) - 1 : sizeof(s2) - 1, nl = strlen(rep), tail = strlen(at + ol) + 1;
            if ((size_t)(at - hlsl) + nl + tail < sizeof(hlsl)) {
                memmove(at + nl, at + ol, tail); memcpy(at, rep, nl);
                static int _n = 0; if (_n++ < 8)
                    fprintf(stderr, "[A2C] mode %d alpha-to-coverage on fp=0x%08X key=0x%08X%c", a2c_mode(), fp_addr, uhash, 10);
            }
        } else { static int _w = 0; if (_w++ < 4)
            fprintf(stderr, "[A2C] could not find the output of fp=0x%08X%c", fp_addr, 10); }
    }
    /* [NHL local patch #212] The guest ALPHA TEST was never applied: vp_record_cb uploads
     * fp_alpha = (enable, ref/255, func & 7) into b1[4] every draw, but no shader read it.
     * NHL draws its rink glass in two passes -- alpha > 196 WITH depth write (the opaque
     * frame/scuffs), then alpha <= 196 without -- so here the clear glass wrote depth over
     * the whole low-camera intro shot and every later light / projected logo / spark failed
     * the depth test (play308 frame_88: glass pass 921600 px; play309 with that write off:
     * sparks back, colours right). Tested at run time on colour 0's alpha like RPCS3
     * (comparison_passes(col0.a, alpha_ref, func)), so it needs no pipeline variants. Only
     * programs with the rebound b1 layout (#78) have fp_alpha at b1[4]. NO_ALPHATEST=1 = off. */
    if (strstr(hlsl, "    float4 rsx_texscale[4];\n    float4 fp_alpha;\n")) {
        static const char at_s1[] = "    _out.t0 = (_o0 == _o0) ? _o0 : (float4)0;\n";
        static const char at_s2[] = "    return (_o == _o) ? _o : (float4)0;\n";
        static const char at_tst[] =
            "    if (fp_alpha.x != 0.0) { const float _aa = %s.a, _ar = fp_alpha.y; const uint _af = (uint)fp_alpha.z;"
            " const bool _ap = (_af == 1u) ? (_aa < _ar) : (_af == 2u) ? (_aa == _ar) : (_af == 3u) ? (_aa <= _ar) :"
            " (_af == 4u) ? (_aa > _ar) : (_af == 5u) ? (_aa != _ar) : (_af == 6u) ? (_aa >= _ar) : (_af == 7u);"
            " if (!_ap) discard; }\n";
        char rep[640]; int nrep = 0;
        char* at = strstr(hlsl, at_s1); size_t ol = sizeof(at_s1) - 1;
        if (at) {
            const int k = snprintf(rep, sizeof rep, "%s", at_s1);
            nrep = k + snprintf(rep + k, sizeof rep - (size_t)k, at_tst, "_out.t0");
        } else if ((at = strstr(hlsl, at_s2)) != NULL) {
            ol = sizeof(at_s2) - 1;
            const int k = snprintf(rep, sizeof rep, "    float4 _rsx_at = (_o == _o) ? _o : (float4)0;\n");
            const int k2 = snprintf(rep + k, sizeof rep - (size_t)k, at_tst, "_rsx_at");
            nrep = k + k2 + snprintf(rep + k + k2, sizeof rep - (size_t)(k + k2), "    return _rsx_at;\n");
        }
        if (at && nrep > 0 && (size_t)nrep < sizeof rep) {
            const size_t tail = strlen(at + ol) + 1;
            if ((size_t)(at - hlsl) + (size_t)nrep + tail < sizeof(hlsl)) {
                memmove(at + nrep, at + ol, tail); memcpy(at, rep, (size_t)nrep);
                static int _n = 0; if (_n++ < 4)
                    fprintf(stderr, "[ALPHATEST] run-time alpha test in fp=0x%08X key=0x%08X%c", fp_addr, uhash, 10);
            }
        } else { static int _w = 0; if (_w++ < 8)
            fprintf(stderr, "[ALPHATEST] no colour-0 output found in fp=0x%08X key=0x%08X%c", fp_addr, uhash, 10); }
    }
    /* [NHL local patch #208] Framebuffer sRGB (NV4097_SET_SHADER_PACKER 0x1FEC): key bit 26
     * (rsx_ctrl_key) -> linear-to-sRGB on every colour target's RGB, the upstream draw
     * engine's rsx_fp_apply_srgb_output (RPCS3: _ENABLE_FRAMEBUFFER_SRGB). The D3D12 path
     * never did it, so every 8-bit surface the game marks sRGB was written LINEAR: the
     * whole game darker than RPCS3, and dim-light players (~0.01 linear) ~10x too dark. */
    if (((u32)exp32 >> 26) & 1u) {
        const int r = rsx_fp_apply_srgb_output(hlsl, sizeof(hlsl));
        static int _n = 0;
        if (r < 0) { static int _w = 0; if (_w++ < 8)
            fprintf(stderr, "[FBSRGB] could not convert the output of fp=0x%08X key=0x%08X%c", fp_addr, uhash, 10); }
        else if (_n++ < 8)
            fprintf(stderr, "[FBSRGB] sRGB output on fp=0x%08X key=0x%08X%c", fp_addr, uhash, 10);
    }
    /* [NHL local patch] FP_DUMP_ALL=1: every distinct (program, ucode) compiled
     * goes to fpdump/fp_<addr>_<hash>.hlsl with its pipeline key (first 512). */
    /* [#175] FP_DUMP_KEY=<hex>[,...] (up to 8): only programs with that key (the [FPID] /
     * [TEXUSE] key; an id-mode key ^ 0x5011D000 matches too), so a busy match cannot use
     * up FP_DUMP_ALL's 512 slots first. */
    int fp_dump_key = 0;
    { static int nk = -1; static u32 ks[8];
      if (nk < 0) { nk = 0; const char* e = getenv("FP_DUMP_KEY");
          while (e && *e && nk < 8) { char* end; unsigned long v = strtoul(e, &end, 16);
              if (end == e) break; ks[nk++] = (u32)v; e = (*end == ',') ? end + 1 : end; } }
      for (int k = 0; k < nk; k++) if (uhash == ks[k] || uhash_tap == ks[k] || (uhash ^ 0x5011D000u) == ks[k]) fp_dump_key = 1; }   /* #206: + pre-variant key */
    if (fp_dump_key || getenv("FP_DUMP_ALL")) { static u32 seen[512][2]; static int ns = 0; int known = 0;
        for (int i = 0; i < ns; i++) if (seen[i][0] == fp_addr && seen[i][1] == uhash) known = 1;
        if (!known && ns < 512) { seen[ns][0] = fp_addr; seen[ns][1] = uhash; ns++;
            CreateDirectoryA("fpdump", NULL);
            char fn[96]; snprintf(fn, sizeof fn, "fpdump/fp_rt%08X_%08X_%08X.hlsl", s_fp_cur_rt, fp_addr, uhash);   /* #86: + target RT */
            FILE* f = fopen(fn, "w");
            if (f) { fprintf(f, "/* fp=0x%08X instrs=%d nrt=%d rtfmt=%d blend=0x%08X cmask=0x%X ctl=0x%X"
                                " stencil=0x%08X cull=%u cube=0x%X vs_idx=%d */\n%s",
                             fp_addr, n, nrt, (int)rtfmt, blend, cmask, (u32)exp32, stencil, cull,
                             cube_mask, vs_idx, hlsl);
                     fclose(f); } } }
    /* FP_LIST=1: every program the title actually compiles, with its size. A
     * fragment program that hangs the GPU shows up here as an implausible
     * instruction count long before it shows up as a TDR. */
    { static int fl = -1; if (fl < 0) fl = getenv("FP_LIST") ? 1 : 0;
      if (fl) fprintf(stderr, "[FPLIST] fp=0x%08X instrs=%d hlsl=%u bytes%c",
                      fp_addr, n, (unsigned)strlen(hlsl), 10); }
    if (getenv("FP_DUMP")) { static int _d=0; if (_d++ < 4) {
        FILE* f = fopen("fp_dump.hlsl", _d==1 ? "w" : "a");
        if (f) {
            const u8* uc = fpuc;
            fprintf(f, "/* fp_addr=0x%08X vmoff=0x%08X raw:", fp_addr, off);
            for (int _b = 0; _b < 32; _b++) fprintf(f, " %02X", uc[_b]);
            fprintf(f, " */\n%s\n", hlsl); fclose(f);
        } } }

    /* FP_PICK=<hex fp_addr>: write just that program, whatever order it compiles
     * in. fp_dump.hlsl keeps only the first four, same trap as vp2_dump.hlsl. */
    { static const char* fpk = (const char*)1; static u32 wantf = 0; static int done = 0;
      if (fpk == (const char*)1) { fpk = getenv("FP_PICK");
                                   wantf = fpk ? (u32)strtoul(fpk, NULL, 16) : 0; }
      if (wantf && !done && fp_addr == wantf) { done = 1;
          FILE* f = fopen("duck_fp.hlsl", "w");
          if (f) { fprintf(f, "/* fp_addr=0x%08X, %d instrs */%s", fp_addr, n, hlsl);
                   fclose(f); }
          fprintf(stderr, "[DUCKFP] wrote duck_fp.hlsl (fp=0x%08X, %d instrs)%c",
                  fp_addr, n, 10);
      } }

    /* [NHL local patch] NHL_FP_DEBUG=<hex fp_addr>,<mode>: rewrite that
     * program's colour output (anchor: the "float4 _o0 = r[0];" line the
     * current decompiler emits -- FP_ONE/FP_SHOW/FP_TEX anchor on "_po.c0 = ",
     * which it no longer produces, so they silently do nothing).
     *   mode 1: branch map -- R = (tc0.w == 0), G = (tc0.w != 0), B = tex0.a <= 0
     *   mode 2: raw unit-0 sample at tc0.xy, alpha forced 1 */
    { static const char* fd = (const char*)1; static u32 fda = 0; static int fdm = 0;
      if (fd == (const char*)1) { fd = getenv("NHL_FP_DEBUG");
          if (fd) { fda = (u32)strtoul(fd, NULL, 16); const char* c = strchr(fd, ','); fdm = c ? atoi(c + 1) : 1; } }
      if (fd && fda == fp_addr) {
          char* rp = strstr(hlsl, "float4 _o0 = r[0];");
          if (rp) {
              /* mode 3: discard the flat-fill branch (tc0.w == 0), keep textured
               * output unchanged -- shows what the placeholder fills cover. */
              const char* rep = (fdm == 3)
                  ? "if (input.tc0.w == 0.0) discard; float4 _o0 = r[0];"
                  : (fdm == 2)
                  ? "float4 _o0 = float4(rsx_tex[0].Sample(rsx_samp[0], input.tc0.xy).rgb, 1.0);"
                  : "float4 _o0 = float4(input.tc0.w == 0.0 ? 1.0 : 0.0, input.tc0.w != 0.0 ? 1.0 : 0.0,"
                    " rsx_tex[0].Sample(rsx_samp[0], input.tc0.xy).a <= 0.0 ? 1.0 : 0.0, 1.0);";
              size_t oldlen = strlen("float4 _o0 = r[0];"), newlen = strlen(rep);
              size_t tail = strlen(rp + oldlen) + 1;
              if ((size_t)(rp - hlsl) + newlen + tail < sizeof(hlsl)) {
                  memmove(rp + newlen, rp + oldlen, tail);
                  memcpy(rp, rep, newlen);
                  fprintf(stderr, "[NHL_FP_DEBUG] fp=0x%08X output rewritten (mode %d)%c", fp_addr, fdm, 10);
              }
          } else fprintf(stderr, "[NHL_FP_DEBUG] fp=0x%08X: anchor not found%c", fp_addr, 10);
      } }

    /* Debug FP_ONE=<hex fp_addr>: force that program's colour output to
     * all-ones (e.g. wave's colour-detect -> full mask -> island borders
     * everywhere -> the water sim must visibly radiate if it works). */
    { const char* f1 = getenv("FP_ONE");
      if (f1 && (u32)strtoul(f1, NULL, 16) == fp_addr) {
          /* Match the assignment, not one exact spelling of it: the final
           * register is r[N] for some programs and h[N] for others, and the old
           * fixed "= r[0];" anchor silently did nothing for every h[] shader --
           * so this switch appeared to have no effect and was useless for
           * identifying which program draws what. Rewrite the expression with a
           * proper length-changing splice. */
          char* rp = strstr(hlsl, "_po.c0 = ");
          if (rp) {
              char* semi = strchr(rp, ';');
              if (semi) {
                  static const char rep[] = "_po.c0 = (1.0).xxxx";
                  size_t oldlen = (size_t)(semi - rp), newlen = sizeof(rep) - 1;
                  size_t tail = strlen(semi) + 1;
                  if ((rp - hlsl) + newlen + tail < sizeof(hlsl)) {
                      memmove(rp + newlen, semi, tail);
                      memcpy(rp, rep, newlen);
                  }
                  (void)oldlen;
              }
          }
      } }
    /* FP_TEX=1: make every program output its unit-0 texture sample verbatim.
     * Separates "the texture is wrong" from "the shading tints it" in one run,
     * without having to identify which program draws which surface first. */
    /* FP_SHOW=<hlsl expr>: replace a program's colour output with an arbitrary
     * expression (e.g. FP_SHOW=h[1]) so an intermediate register can be looked
     * at directly. Restrict with FP_TEX_FP=<hex>. Reading a 69-instruction
     * shader to guess which term carries a bad value is slower and less certain
     * than displaying the terms. */
    /* FP_KILL=<hex fp_addr>: make that program discard every fragment, so what
     * it covers becomes visible. Distinguishes "this surface shades black" from
     * "this surface is correct and something else is missing behind it". */
    { const char* kf = getenv("FP_KILL");
      if (kf && (u32)strtoul(kf, NULL, 16) == fp_addr) {
          char* rp = strstr(hlsl, "PSOut _po;");
          if (rp) {
              const char* ins = "discard; ";
              size_t nl = strlen(ins), tail = strlen(rp) + 1;
              if ((size_t)(rp - hlsl) + nl + tail < sizeof(hlsl)) {
                  memmove(rp + nl, rp, tail);
                  memcpy(rp, ins, nl);
                  fprintf(stderr, "[FPKILL] fp=0x%X discards%c", fp_addr, 10);
              }
          }
      } }
    { const char* sh = getenv("FP_SHOW");
      const char* only = getenv("FP_TEX_FP");
      if (sh && (!only || (u32)strtoul(only, NULL, 16) == fp_addr)) {
          char* rp = strstr(hlsl, "_po.c0 = ");
          if (rp) { char* semi = strchr(rp, ';');
            if (semi) {
                char rep[256];
                snprintf(rep, sizeof rep, "_po.c0 = float4((%s).xyz, 1.0)", sh);
                size_t newlen = strlen(rep), tail = strlen(semi) + 1;
                if ((size_t)(rp - hlsl) + newlen + tail < sizeof(hlsl)) {
                    memmove(rp + newlen, semi, tail);
                    memcpy(rp, rep, newlen);
                    fprintf(stderr, "[FPSHOW] fp=0x%X output replaced with %s%c",
                            fp_addr, sh, 10);
                }
            } }
      } }
    /* Retarget the samplers the guest bound as CUBE textures. The decompiler
     * always emits 2D samplers; a cube is sampled with a 3-component direction
     * and no texel-scale, and the emitted coordinate always ends in the fixed
     * tail ".xy * rsx_texscale[N].xy", so both edits are exact replacements. */
    /* [#240] units the guest CLAMPS (s_fp_cur_clamp, see fp_shadow_from_draw): the uv is
     * clamped to the edge texel centres of the coarser of the two levels the sample reads
     * (implicit LOD via CalculateLevelOfDetail), so the wrap sampler never reaches the far
     * edge. Per-axis variants _u/_v/_uv; nearest units (#176) get a clamped point sample. */
    if (s_fp_cur_clamp && strstr(hlsl, "SamplerState rsx_samp[16] : register(s0);\n")) {
        static const char* const sfx[4] = { "", "_u", "_v", "_uv" };
        char helpers[4096]; int hp = snprintf(helpers, sizeof helpers,
            "SamplerState rsx_samp[16] : register(s0);\n"
            "float2 rsx_cuv(float2 uv, float2 sz, float2 m) { float2 hc = 0.5 / sz; return lerp(uv, clamp(uv, hc, 1.0 - hc), m); }\n"
            "float4 rsx_clm(Texture2D t, SamplerState s, float2 uv, float lod, float2 m) { uint w, h, n; t.GetDimensions(0, w, h, n);"
            " float lc = clamp(ceil(lod), 0.0, (float)n - 1.0); return t.SampleLevel(s, rsx_cuv(uv, max(float2(w, h) / exp2(lc), 1.0), m), lod); }\n");
        for (int v = 1; v < 4 && hp < (int)sizeof helpers - 512; v++) {
            const char* mm = v == 1 ? "float2(1.0, 0.0)" : v == 2 ? "float2(0.0, 1.0)" : "float2(1.0, 1.0)";
            hp += snprintf(helpers + hp, sizeof helpers - hp,
                "float4 rsx_cl%s(Texture2D t, SamplerState s, float2 uv) { return rsx_clm(t, s, uv, t.CalculateLevelOfDetail(s, uv), %s); }\n"
                "float4 rsx_cll%s(Texture2D t, SamplerState s, float2 uv, float lod) { return rsx_clm(t, s, uv, lod, %s); }\n"
                "#define rsx_ptc%s(t, s, uv) rsx_pt(t, s, lerp((uv), clamp((uv), 0.0, 0.999999), %s))\n"
                "#define rsx_ptlc%s(t, s, uv, l) rsx_ptl(t, s, lerp((uv), clamp((uv), 0.0, 0.999999), %s), l)\n",
                sfx[v], mm, sfx[v], mm, sfx[v], mm, sfx[v], mm);
        }
        hlsl_replace_all(hlsl, sizeof hlsl, "SamplerState rsx_samp[16] : register(s0);\n", helpers);
        u32 done = 0;
        for (int _u = 0; _u < D3D_TEX_UNITS; _u++) {
            const int v = ((s_fp_cur_clamp >> _u) & 1u) | (((s_fp_cur_clamp >> (16 + _u)) & 1u) << 1);
            if (!v || (cube_mask & (1u << _u))) continue;
            const int nearest = _u > 0 && (cube_mask & (1u << (16 + _u)));
            static const char* const form[2] = { "rsx_tex%d.%s(rsx_samp[%d], ", "rsx_tex[%d].%s(rsx_samp[%d], " };
            static const char* const rform[2] = { "%s%s(rsx_tex%d, rsx_samp[%d], ", "%s%s(rsx_tex[%d], rsx_samp[%d], " };
            for (int fo = 0; fo < 2; fo++) {
                char f[64], r[64];
                snprintf(f, sizeof f, form[fo], _u, "Sample", _u);
                snprintf(r, sizeof r, rform[fo], nearest ? "rsx_ptc" : "rsx_cl", sfx[v], _u, _u);
                if (strstr(hlsl, f)) { hlsl_replace_all(hlsl, sizeof hlsl, f, r); done |= 1u << _u; }
                snprintf(f, sizeof f, form[fo], _u, "SampleLevel", _u);
                snprintf(r, sizeof r, rform[fo], nearest ? "rsx_ptlc" : "rsx_cll", sfx[v], _u, _u);
                if (strstr(hlsl, f)) { hlsl_replace_all(hlsl, sizeof hlsl, f, r); done |= 1u << _u; }
            }
        }
        { static int _n = 0; if (done && _n++ < 12)
            fprintf(stderr, "[TEXCLAMP] fp=0x%X clamp mask 0x%08X, rewritten units 0x%04X%c", fp_addr, s_fp_cur_clamp, done, 10); }
    }
    /* [#176] units the guest filters NEAREST (cube_mask bits 16..31) are point-sampled:
     * the uv snaps to its texel centre and is read at one mip level through the unit's
     * own sampler (bilinear at a texel centre = that texel). ps_5_0 has only 16 sampler
     * slots, so a separate point sampler (s16) does not compile -- the first #176 build
     * failed every such PS and recompiled it per draw (play238 hang). */
    if ((cube_mask >> 16) && strstr(hlsl, "SamplerState rsx_samp[16] : register(s0);\n")) {
        hlsl_replace_all(hlsl, sizeof hlsl, "SamplerState rsx_samp[16] : register(s0);\n",
            "SamplerState rsx_samp[16] : register(s0);\n"
            "float4 rsx_pt(Texture2D t, SamplerState s, float2 uv) { float w, h; t.GetDimensions(w, h);"
            " return t.SampleLevel(s, (floor(uv * float2(w, h)) + 0.5) / float2(w, h), 0.0); }\n"
            "float4 rsx_ptl(Texture2D t, SamplerState s, float2 uv, float lod) { uint w, h, n; t.GetDimensions(0, w, h, n);"
            " float l = clamp(round(lod), 0.0, (float)n - 1.0); float2 sz = max(float2(w, h) / exp2(l), 1.0);"
            " return t.SampleLevel(s, (floor(uv * sz) + 0.5) / sz, l); }\n"
            /* #245: magnified = point (mag NEAREST), minified = the guest min filter class 1/2/3.
             * Both results are computed and selected, so no gradient op sits in flow control. */
            "float4 rsx_pt1(Texture2D t, SamplerState s, float2 uv) { float l = t.CalculateLevelOfDetailUnclamped(s, uv);"
            " float4 a = rsx_pt(t, s, uv), b = rsx_ptl(t, s, uv, max(l, 0.0)); return l > 0.0 ? b : a; }\n"
            "float4 rsx_pt2(Texture2D t, SamplerState s, float2 uv) { float l = t.CalculateLevelOfDetailUnclamped(s, uv);"
            " float4 a = rsx_pt(t, s, uv), b = t.SampleLevel(s, uv, 0.0); return l > 0.0 ? b : a; }\n"
            "float4 rsx_pt3(Texture2D t, SamplerState s, float2 uv) { float l = t.CalculateLevelOfDetailUnclamped(s, uv);"
            " float4 a = rsx_pt(t, s, uv), b = t.Sample(s, uv); return l > 0.0 ? b : a; }\n"
            "float4 rsx_ptl2(Texture2D t, SamplerState s, float2 uv, float lod) {"
            " return lod > 0.0 ? t.SampleLevel(s, uv, 0.0) : rsx_pt(t, s, uv); }\n"
            "float4 rsx_ptl3(Texture2D t, SamplerState s, float2 uv, float lod) {"
            " return lod > 0.0 ? t.SampleLevel(s, uv, lod) : rsx_pt(t, s, uv); }\n");
        for (int _u = 1; _u < D3D_TEX_UNITS; _u++) {
            if (!(cube_mask & (1u << (16 + _u))) || (cube_mask & (1u << _u))) continue;   /* not cubes */
            const u32 cls = (s_fp_cur_ptmin >> (2 * _u)) & 3u;   /* #245 */
            static const char* const pt[4] = { "rsx_pt", "rsx_pt1", "rsx_pt2", "rsx_pt3" };
            static const char* const ptl[4] = { "rsx_ptl", "rsx_ptl", "rsx_ptl2", "rsx_ptl3" };
            char f[64], r[64];
            snprintf(f, sizeof f, "rsx_tex[%d].Sample(rsx_samp[%d], ", _u, _u);
            snprintf(r, sizeof r, "%s(rsx_tex[%d], rsx_samp[%d], ", pt[cls], _u, _u);
            hlsl_replace_all(hlsl, sizeof hlsl, f, r);
            snprintf(f, sizeof f, "rsx_tex[%d].SampleLevel(rsx_samp[%d], ", _u, _u);
            snprintf(r, sizeof r, "%s(rsx_tex[%d], rsx_samp[%d], ", ptl[cls], _u, _u);
            hlsl_replace_all(hlsl, sizeof hlsl, f, r);
            snprintf(f, sizeof f, "rsx_tex%d.Sample(rsx_samp[%d], ", _u, _u);
            snprintf(r, sizeof r, "%s(rsx_tex%d, rsx_samp[%d], ", pt[cls], _u, _u);
            hlsl_replace_all(hlsl, sizeof hlsl, f, r);
        }
        { static int _n = 0; if (_n++ < 40)
            fprintf(stderr, "[TEXFILTER] fp=0x%X point-sampled units mask 0x%04X, min classes 0x%08X%c", fp_addr, cube_mask >> 16, s_fp_cur_ptmin, 10); }
    }
    if (cube_mask & 0xFFFFu) {
        for (int _u = 0; _u < D3D_TEX_UNITS; _u++) {
            if (!(cube_mask & (1u << _u))) continue;
            char f1[64], r1[64], f2[64], r2[64];
            snprintf(f1, sizeof f1, "Texture2D    rsx_tex%d", _u);
            snprintf(r1, sizeof r1, "TextureCube  rsx_tex%d", _u);
            hlsl_replace_all(hlsl, sizeof hlsl, f1, r1);
            snprintf(f1, sizeof f1, "Texture2D rsx_tex%d", _u);
            snprintf(r1, sizeof r1, "TextureCube rsx_tex%d", _u);
            hlsl_replace_all(hlsl, sizeof hlsl, f1, r1);
            snprintf(f2, sizeof f2, ".xy * rsx_texscale[%d].xy", _u);
            snprintf(r2, sizeof r2, ".xyz");
            hlsl_replace_all(hlsl, sizeof hlsl, f2, r2);
        }
        { static int _n = 0; if (_n++ < 4)
            fprintf(stderr, "[CUBE] fp=0x%X compiled with cube units mask 0x%X%c",
                    fp_addr, cube_mask, 10); }
    }
    /* FP_IDCOLOR=1: give every fragment program a distinct flat colour derived
     * from its address. One frame then shows which program paints which surface,
     * instead of one run per candidate to test them by elimination. */
    if (getenv("FP_IDCOLOR")) {
        char* rp = strstr(hlsl, "_po.c0 = ");
        if (rp) {
            char* semi = strchr(rp, ';');
            if (semi) {
                /* Mix properly: every fp address in a title tends to share low
                 * bits (they all end 0x01/0x81 here), so a single multiply left
                 * one channel constant across all programs and made distinct
                 * shaders look like the same colour. */
                u32 hsh = fp_addr * 2654435761u;
                hsh ^= hsh >> 13; hsh *= 0x5BD1E995u; hsh ^= hsh >> 15;
                /* Force the top bit on in each channel: a hash that happens to
                 * land near black is indistinguishable from "this surface was
                 * never painted", which is the exact question the mode exists
                 * to answer. */
                float cr = (float)(((hsh >> 16) & 0x7F) | 0x80) / 255.0f;
                float cg = (float)(((hsh >>  8) & 0x7F) | 0x80) / 255.0f;
                float cb = (float)(((hsh      ) & 0x7F) | 0x80) / 255.0f;
                char rep[128];
                snprintf(rep, sizeof rep, "_po.c0 = float4(%.3f,%.3f,%.3f,1.0)", cr, cg, cb);
                size_t newlen = strlen(rep), tail = strlen(semi) + 1;
                if ((size_t)(rp - hlsl) + newlen + tail < sizeof(hlsl)) {
                    memmove(rp + newlen, semi, tail);
                    memcpy(rp, rep, newlen);
                    fprintf(stderr, "[FPID] fp=0x%X -> rgb(%.0f,%.0f,%.0f)%c",
                            fp_addr, cr*255, cg*255, cb*255, 10);
                }
            }
        }
    }
    /* FP_TEX=<unit>: output that unit's sample verbatim. FP_TEX_FP=<hex> limits
     * it to one program, so one surface can be inspected without repainting the
     * whole scene. FP_TEX_UV=1 samples with the same texcoord the program uses
     * for that unit rather than tc0. */
    if (getenv("FP_TEX") && (!getenv("FP_TEX_FP") ||
        (u32)strtoul(getenv("FP_TEX_FP"), NULL, 16) == fp_addr)) {
        int _un = atoi(getenv("FP_TEX"));
        if (_un < 0 || _un > 3) _un = 0;
        char* rp = strstr(hlsl, "_po.c0 = ");
        if (rp) {
            char* semi = strchr(rp, ';');
            if (semi) {
                char rep[160];
                /* FP_TEX_UV=1: sample at a fixed centre UV instead of the
                 * interpolated texcoord. If the texture is populated but the
                 * texcoords never arrive, the normal path samples texel (0,0)
                 * -- usually black -- and looks exactly like an empty texture.
                 * This tells the two apart. */
                if (getenv("FP_TEX_UV"))
                    snprintf(rep, sizeof rep,
                             "_po.c0 = rsx_tex%d.Sample(rsx_samp%d, float2(0.5, 0.5))",
                             _un, _un);
                else
                snprintf(rep, sizeof rep,
                         "_po.c0 = rsx_tex%d.Sample(rsx_samp%d, input.tc0.xy * rsx_texscale[%d].xy)",
                         _un, _un, _un);
                size_t newlen = strlen(rep), tail = strlen(semi) + 1;
                fprintf(stderr, "[FP_TEX] program 0x%X rewritten to sample unit %d%c",
                        fp_addr, _un, 10);
                if ((size_t)(rp - hlsl) + newlen + tail < sizeof(hlsl)) {
                    memmove(rp + newlen, semi, tail);
                    memcpy(rp, rep, newlen);
                }
            }
        }
    }
    /* FP_FORCE=1: replace the translated body with solid magenta -- isolates
     * geometry/transform problems from texture/blend problems. */
    if (getenv("FP_FORCE")) {
        char* rp = strstr(hlsl, "return r[0];");
        if (rp) memcpy(rp, "return float4(1,0,1,1)", 23);
    }
    ID3DBlob* pb = NULL; ID3DBlob* e = NULL;
    /* [NHL local patch #79] Compiled-PS cache keyed by the final HLSL text: a
     * PSO miss for a new blend/RT/VS combination reuses the bytecode instead of
     * running FXC again (FXC was the in-match bottleneck, play71/72). Same text
     * => same bytecode, so this cannot change what is drawn. */
    HRESULT hr = S_OK;
    { enum { PSB_N = 4096 };
      static struct { unsigned long long k; u32 len; ID3DBlob* b; } s_psb[PSB_N];
      static int s_psb_n = 0, s_psb_next = 0;
      const size_t hl = strlen(hlsl);
      unsigned long long hk = 1469598103934665603ull;
      for (size_t _q = 0; _q < hl; _q++) { hk ^= (u8)hlsl[_q]; hk *= 1099511628211ull; }
      for (int _q = 0; _q < s_psb_n; _q++)
          if (s_psb[_q].k == hk && s_psb[_q].len == (u32)hl) {
              pb = s_psb[_q].b; pb->lpVtbl->AddRef(pb); break; }
      if (!pb && (s_fxc_collect || (async_on() && !s_async_sync)) && fxc_par_on()) {   /* #124 prepass: batch it; #225: or compile in the background */
          pb = fxc_lookup(hlsl, hl, 1);
          if (!pb) { fxc_queue(hlsl, hl, 1, -1, 0);
                     if (fpp_on) fpp_store(&fpk, hlsl, hl);   /* #227 */
                     if (!s_fxc_collect) InterlockedIncrement(&s_async_skipped);
                     return NULL; }
      }
      if (!pb) {
          hr = fxc_compile(hlsl, hl, "guest_fp", "ps_5_0", 1, 0, &pb, &e);   /* #123 */
          if (SUCCEEDED(hr) && pb) {
              int slot = s_psb_n < PSB_N ? s_psb_n++ : (s_psb_next++ % PSB_N);
              if (s_psb[slot].b) s_psb[slot].b->lpVtbl->Release(s_psb[slot].b);
              s_psb[slot].k = hk; s_psb[slot].len = (u32)hl; s_psb[slot].b = pb;
              pb->lpVtbl->AddRef(pb);   /* the cache's reference */
          }
      } }
    if (FAILED(hr) || !pb) {
        static int _e2=0; if (_e2++<16)
            printf("[FP] PS compile FAIL (fp=0x%08X): %s\n", fp_addr,
                   e ? (const char*)e->lpVtbl->GetBufferPointer(e) : "?");
        if (e) e->lpVtbl->Release(e);
        return NULL;
    }
    if (e) e->lpVtbl->Release(e);

    D3D12_INPUT_ELEMENT_DESC il[16];
    for (int _i = 0; _i < 16; _i++) {
        il[_i].SemanticName = "ATTR"; il[_i].SemanticIndex = _i;
        il[_i].Format = DXGI_FORMAT_R32G32B32A32_FLOAT; il[_i].InputSlot = 0;
        il[_i].AlignedByteOffset = (UINT)(_i * 16);
        il[_i].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
        il[_i].InstanceDataStepRate = 0;
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {0};
    pd.pRootSignature = s_d3d.vp_root_sig;
    pd.VS.pShaderBytecode = vsb->lpVtbl->GetBufferPointer(vsb);
    pd.VS.BytecodeLength  = vsb->lpVtbl->GetBufferSize(vsb);
    pd.PS.pShaderBytecode = pb->lpVtbl->GetBufferPointer(pb);
    pd.PS.BytecodeLength  = pb->lpVtbl->GetBufferSize(pb);
    pd.InputLayout.pInputElementDescs = il; pd.InputLayout.NumElements = 16;
    if (vs_idx >= 0 && s_d3d.vp_vs[vs_idx].gpu) {   /* #125: the VS fetches its own vertices */
        pd.InputLayout.pInputElementDescs = NULL; pd.InputLayout.NumElements = 0; }
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    /* Guest face culling. CULL_OFF=1 forces double-sided, the old
     * unconditional behaviour -- geometry that vanishes under culling is a
     * winding/front-face problem, not a missing draw. */
    { static int _co = -1; if (_co < 0) _co = getenv("CULL_OFF") ? 1 : 0;
      pd.RasterizerState.CullMode = (!_co && (cull & 1u))
          ? ((cull & 2u) ? D3D12_CULL_MODE_FRONT : D3D12_CULL_MODE_BACK)
          : D3D12_CULL_MODE_NONE;
      /* CULL_FLIP=1: invert the front-face convention. Our VP path may hand
       * D3D the opposite winding from the guest's, in which case honouring
       * CULL_FACE culls exactly the faces it should keep. */
      static int _cfl = -1; if (_cfl < 0) _cfl = getenv("CULL_FLIP") ? 1 : 0;
      pd.RasterizerState.FrontCounterClockwise =
          (((cull & 4u) ? 1 : 0) ^ _cfl) ? TRUE : FALSE; }
    /* Blend per the guest's packed key (see rsx_blend_key): dbgfont text needs
     * straight alpha; demosaic's effect passes blend OFF; DeferredShading's
     * light accumulation is additive ONE,ONE. */
    pd.BlendState.RenderTarget[0].BlendEnable    = (blend & 1) ? TRUE : FALSE;
    pd.BlendState.RenderTarget[0].SrcBlend       = (blend & 1) ? (D3D12_BLEND)((blend >> 1)  & 0x1F) : D3D12_BLEND_ONE;
    pd.BlendState.RenderTarget[0].DestBlend      = (blend & 1) ? (D3D12_BLEND)((blend >> 6)  & 0x1F) : D3D12_BLEND_ZERO;
    pd.BlendState.RenderTarget[0].BlendOp        = (blend & 1) ? (D3D12_BLEND_OP)((blend >> 21) & 0x7) : D3D12_BLEND_OP_ADD;
    pd.BlendState.RenderTarget[0].SrcBlendAlpha  = (blend & 1) ? (D3D12_BLEND)((blend >> 11) & 0x1F) : D3D12_BLEND_ONE;
    pd.BlendState.RenderTarget[0].DestBlendAlpha = (blend & 1) ? (D3D12_BLEND)((blend >> 16) & 0x1F) : D3D12_BLEND_ZERO;
    pd.BlendState.RenderTarget[0].BlendOpAlpha   = (blend & 1) ? (D3D12_BLEND_OP)((blend >> 24) & 0x7) : D3D12_BLEND_OP_ADD;
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    /* #174: A2C blend mode -- a blend-off A2C draw blends by its alpha (colour and alpha
     * alike: the resolved MSAA result is a*src + (1-a)*dst per channel) */
    if ((((u32)exp32 >> 25) & 1u) && a2c_mode() == 2 && !(blend & 1)) {
        D3D12_RENDER_TARGET_BLEND_DESC* b = &pd.BlendState.RenderTarget[0];
        b->BlendEnable = TRUE;
        b->SrcBlend = D3D12_BLEND_SRC_ALPHA; b->DestBlend = D3D12_BLEND_INV_SRC_ALPHA; b->BlendOp = D3D12_BLEND_OP_ADD;
        b->SrcBlendAlpha = D3D12_BLEND_SRC_ALPHA; b->DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA; b->BlendOpAlpha = D3D12_BLEND_OP_ADD;
    }
    pd.SampleMask = UINT_MAX;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = (UINT)nrt;
    for (int _r = 0; _r < nrt; _r++) {
        pd.RTVFormats[_r] = rtfmt;
        /* Zero-init leaves RenderTargetWriteMask 0 on secondary targets --
         * every MRT-B write would be masked off. Mirror RT0's blend state. */
        pd.BlendState.RenderTarget[_r] = pd.BlendState.RenderTarget[0];
    }
    /* Guest colour write mask (RGBA nibble, already D3D-ordered). cmask is 0xF
     * by default (unset colour_mask register decodes to all-on); a literal 0
     * therefore means the guest EXPLICITLY masked every channel -- a depth-only
     * pass (e.g. DeferredShading's shadow-map generation). Honour it: forcing 0
     * back to 0xF splatters the depth pass's fragment colour onto the target. */
    /* CMASK_FORCE=1: ignore the guest colour mask (write RGBA on every draw).
     * A mis-decoded mask makes geometry rasterize correctly and write nothing,
     * which is indistinguishable from "the draw never happened". */
    { static int _cf = -1; if (_cf < 0) _cf = getenv("CMASK_FORCE") ? 1 : 0;
      if (_cf) cmask = 0xF; }
    for (int _r = 0; _r < nrt; _r++)
        pd.BlendState.RenderTarget[_r].RenderTargetWriteMask = (UINT8)(cmask & 0xF);
    pd.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    {   /* [NHL local patch #54] guest depth state (see rsx_ctrl_key) */
        u32 dk = ((u32)exp32 >> 20) & 0x1Fu;
        /* [#211] NHL_NODEPTHWRITE_KEY=<hex>[,..] (diag): no depth write for those programs
         * (the lights-off intro: a full-screen blended glass pass, key A95F0E47, writes depth
         * and every later light/logo/spark draw fails the depth test) */
        { static u32 nk[8]; static int nn = -1;
          if (nn < 0) { nn = 0; const char* e = getenv("NHL_NODEPTHWRITE_KEY");
              while (e && *e && nn < 8) { char* end; const u32 v = (u32)strtoul(e, &end, 16);
                  if (end == e) break; nk[nn++] = v; e = (*end == ',') ? end + 1 : end; } }
          for (int _k = 0; _k < nn; _k++) if ((uhash_tap == nk[_k] || uhash == nk[_k]) && (dk & 0x10u)) {
              dk &= ~0x10u; static int _n = 0;
              if (_n++ < 4) fprintf(stderr, "[NODEPTHWRITE] fp=0x%08X key=0x%08X: depth write off%c", fp_addr, uhash_tap, 10); } }
        pd.DepthStencilState.DepthEnable = (dk & 1u) ? TRUE : FALSE;
        pd.DepthStencilState.DepthWriteMask = (dk & 0x10u) ? D3D12_DEPTH_WRITE_MASK_ALL
                                                           : D3D12_DEPTH_WRITE_MASK_ZERO;
        pd.DepthStencilState.DepthFunc = (D3D12_COMPARISON_FUNC)(((dk >> 1) & 7u) + 1u);
    }
    pd.DepthStencilState.StencilEnable = FALSE;
    if (stencil & 1u) {   /* [NHL local patch] guest stencil, see dr_set_stencil */
        static const D3D12_STENCIL_OP ops[8] = {
            D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_ZERO, D3D12_STENCIL_OP_REPLACE,
            D3D12_STENCIL_OP_INCR_SAT, D3D12_STENCIL_OP_DECR_SAT, D3D12_STENCIL_OP_INVERT,
            D3D12_STENCIL_OP_INCR, D3D12_STENCIL_OP_DECR };
        D3D12_DEPTH_STENCILOP_DESC so;
        so.StencilFailOp      = ops[(stencil >> 20) & 7u];
        so.StencilDepthFailOp = ops[(stencil >> 23) & 7u];
        so.StencilPassOp      = ops[(stencil >> 26) & 7u];
        so.StencilFunc        = (D3D12_COMPARISON_FUNC)(((stencil >> 1) & 7u) + 1u);
        pd.DepthStencilState.StencilEnable    = TRUE;
        pd.DepthStencilState.StencilReadMask  = (UINT8)((stencil >> 4) & 0xFFu);
        pd.DepthStencilState.StencilWriteMask = (UINT8)((stencil >> 12) & 0xFFu);
        pd.DepthStencilState.FrontFace = so;
        pd.DepthStencilState.BackFace  = so;
    }
    /* DEPTH_OFF=1: drop the depth test for guest-FP draws, so submission order
     * alone decides what is on top. Paired with DRAW_LAST_TEX this puts one
     * object in front of everything and answers "is it merely occluded?" --
     * which reordering alone cannot, because a relocated draw still fails the
     * depth test against whatever is already in the buffer. Diagnostic. */
    { static int doff = -1;
      if (doff < 0) { const char* e = getenv("DEPTH_OFF"); doff = e ? atoi(e) : 0; }
      if (doff) { pd.DepthStencilState.DepthEnable = FALSE;
                  pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO; } }
    /* #249: an MSAA target (ms_exp: bits 28-29 = log2 samples, bit 27 = real alpha-to-coverage) */
    pd.SampleDesc.Count = 1u << (((u32)exp32 >> 28) & 3u);
    if (((u32)exp32 >> 27) & 1u) pd.BlendState.AlphaToCoverageEnable = TRUE;

    ID3D12PipelineState* pso = NULL;
    /* #225: non-essential draws share a per-frame PSO creation budget */
    if (async_on() && !s_async_sync && s_pso_frame_us > async_pso_budget()) {
        pb->lpVtbl->Release(pb);
        if (fpp_on) fpp_store(&fpk, hlsl, strlen(hlsl));   /* #227 */
        InterlockedIncrement(&s_async_pso_deferred);
        return NULL; }
    const unsigned long long _pso_t0 = ps3_qpc_us();
    hr = pl_create(&pd, &pso);   /* #128 pipeline library (+ #122 timing inside) */
    s_pso_frame_us += ps3_qpc_us() - _pso_t0;
    if (hr == (HRESULT)0x887A0005L) dred_dump("CreateGraphicsPipelineState");
    if (FAILED(hr) && hr != (HRESULT)0x887A0005L) {
        /* [NHL local patch #50] Name the rejected field. Without the debug
         * layer E_INVALIDARG is opaque, so retry with one setting relaxed at a
         * time and report which variant the runtime accepts. Once per program;
         * the HLSL goes to fp_fail_<addr>.hlsl. The failure is then cached
         * below (pso = NULL), so a rejected program is not re-decompiled and
         * re-compiled on every draw of every frame. */
        static u32 seen[64]; static int ns = 0; int known = 0;
        for (int i = 0; i < ns; i++) if (seen[i] == fp_addr) known = 1;
        if (!known && ns < 64) {
            seen[ns++] = fp_addr;
            printf("[FP] PSO FAIL (fp=0x%08X, 0x%08lX) nrt=%d rtfmt=%d blend=0x%08X cmask=0x%X "
                   "stencil=0x%08X cull=%u cube=0x%X vs_idx=%d\n", fp_addr, hr, nrt, (int)rtfmt,
                   blend, cmask, stencil, cull, cube_mask, vs_idx);
            { char fn[64]; snprintf(fn, sizeof fn, "fp_fail_%08X.hlsl", fp_addr);
              FILE* f = fopen(fn, "w"); if (f) { fputs(hlsl, f); fclose(f); } }
            static const char* names[] = { "blend-off", "stencil-off", "nrt=1", "rgba8",
                                           "depth-off", "trivial-ps", "primary-vs", "all-of-1..5" };
            static const char triv[] =
                "float4 main(float4 p:SV_Position):SV_Target{return (float4)0;}";
            ID3DBlob* tb = NULL;
            for (int v = 0; v < 8; v++) {
                D3D12_GRAPHICS_PIPELINE_STATE_DESC q = pd;
                if (v == 0 || v == 7) for (int r2 = 0; r2 < 8; r2++) q.BlendState.RenderTarget[r2].BlendEnable = FALSE;
                if (v == 1 || v == 7) q.DepthStencilState.StencilEnable = FALSE;
                if (v == 2 || v == 7) { q.NumRenderTargets = 1; for (int r2 = 1; r2 < 8; r2++) q.RTVFormats[r2] = DXGI_FORMAT_UNKNOWN; }
                if (v == 3 || v == 7) for (UINT r2 = 0; r2 < q.NumRenderTargets; r2++) q.RTVFormats[r2] = DXGI_FORMAT_R8G8B8A8_UNORM;
                if (v == 4 || v == 7) { q.DepthStencilState.DepthEnable = FALSE;
                                        q.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO; }
                if (v == 5) {
                    if (!tb) D3DCompile(triv, sizeof(triv) - 1, "triv", NULL, NULL, "main", "ps_5_0", 0, 0, &tb, NULL);
                    if (!tb) continue;
                    q.PS.pShaderBytecode = tb->lpVtbl->GetBufferPointer(tb);
                    q.PS.BytecodeLength  = tb->lpVtbl->GetBufferSize(tb);
                }
                if (v == 6) {
                    if (!s_d3d.vp_vs_blob || s_d3d.vp_vs_blob == vsb) continue;
                    q.VS.pShaderBytecode = s_d3d.vp_vs_blob->lpVtbl->GetBufferPointer(s_d3d.vp_vs_blob);
                    q.VS.BytecodeLength  = s_d3d.vp_vs_blob->lpVtbl->GetBufferSize(s_d3d.vp_vs_blob);
                }
                ID3D12PipelineState* tp = NULL;
                HRESULT h2 = s_d3d.device->lpVtbl->CreateGraphicsPipelineState(
                    s_d3d.device, &q, &IID_ID3D12PipelineState, (void**)&tp);
                printf("[FP]   retry %-11s -> %s\n", names[v], SUCCEEDED(h2) ? "OK" : "fail");
                if (tp) tp->lpVtbl->Release(tp);
            }
            if (tb) tb->lpVtbl->Release(tb);
        }
    }
    pb->lpVtbl->Release(pb);
    if (FAILED(hr)) pso = NULL;   /* cached below as a negative entry */
    else { static int _ok=0; if (_ok++<32) printf("[FP] guest FP pipeline ready (fp=0x%08X)\n", fp_addr); }

    /* insert. [#80] Entries never move (the hash chains index them). If the
     * table ever fills, start over; the old PSOs are deliberately NOT released
     * here because frames in flight may still reference them (the old
     * evict-oldest path released them immediately, which was unsafe). */
    if (s_d3d.vp_fp_n >= VP_FP_CACHE) {
        static int _w = 0; if (_w++ < 4)
            fprintf(stderr, "[PSO] guest-FP cache full (%d) -- reset (old PSOs leaked)\n", VP_FP_CACHE);
        s_d3d.vp_fp_n = 0;
        memset(s_fph_head, 0, sizeof(s_fph_head));
    }
    { const int slot = s_d3d.vp_fp_n, b = (int)(uhash & (VP_FP_BUCKETS - 1));
      s_fph_next[slot] = s_fph_head[b]; s_fph_head[b] = slot + 1; }
    s_d3d.vp_fp[s_d3d.vp_fp_n].fp_addr = fp_addr;
    s_d3d.vp_fp[s_d3d.vp_fp_n].vs_idx  = vs_idx;
    s_d3d.vp_fp[s_d3d.vp_fp_n].vs_hash = vs_hash;
    s_d3d.vp_fp[s_d3d.vp_fp_n].gen     = s_d3d.vp_gen;
    s_d3d.vp_fp[s_d3d.vp_fp_n].blend   = blend;
    s_d3d.vp_fp[s_d3d.vp_fp_n].nrt     = nrt;
    s_d3d.vp_fp[s_d3d.vp_fp_n].rtfmt   = (u32)rtfmt;
    s_d3d.vp_fp[s_d3d.vp_fp_n].exp32   = exp32;
    s_d3d.vp_fp[s_d3d.vp_fp_n].ucode_hash = uhash;
    s_d3d.vp_fp[s_d3d.vp_fp_n].cmask   = cmask;
    s_d3d.vp_fp[s_d3d.vp_fp_n].cull    = cull;
    s_d3d.vp_fp[s_d3d.vp_fp_n].cube_mask = cube_mask;
    s_d3d.vp_fp[s_d3d.vp_fp_n].stencil   = stencil;
    s_d3d.vp_fp[s_d3d.vp_fp_n].pso     = pso;
    s_d3d.vp_fp_n++;
    s_fp_last_key = uhash;   /* #89 */
    return pso;
}

/* Upload the guest texture for a VP draw into a per-frame texture slot
 * (re-uploaded every frame: gcm/cube's plasma animates in guest memory).
 * Returns the slot index (SRV at heap 1+slot) or -1. Must run while the
 * command list is open, before the draw passes. */
/* NV4097 TEXTURE_CONTROL1 component remap -> D3D12 Shader4ComponentMapping.
 * The crossbar decode itself is RSX semantics and lives in
 * rsx_texture_layout.c; all that is left here is packing the four selectors
 * into D3D12's word. Its force-zero/force-one encodings are 4 and 5, which is
 * what RSX_REMAP_ZERO/ONE already are, so the selectors pass straight through.
 *
 * out[] arrives in the crossbar's field order A,R,G,B; D3D12 wants
 * destR | destG<<3 | destB<<6 | destA<<9 | valid<<12. */
static u32 rsx_remap_to_d3d(u32 c1, u32 basef)
{
    u8 out[4];
    rsx_texture_component_remap(c1, basef, out);
    /* TEX_REMAP_ID=<n>: override the derived crossbar with a fixed mapping, to
     * test channel order directly. The resource holds the guest's A8R8G8B8
     * bytes straight through, so its components are (R=A, G=R, B=G, A=B) and
     * the shader needs destR=1, destG=2, destB=3, destA=0 to see real RGBA.
     *   1 = identity   2 = rotate (the ARGB fix)   3 = BGRA swap */
    { static int fixed = -1;
      if (fixed < 0) { const char* e = getenv("TEX_REMAP_ID"); fixed = e ? atoi(e) : 0; }
      if (fixed == 1) return 0u | (1u<<3) | (2u<<6) | (3u<<9) | (1u<<12);
      if (fixed == 2) return 1u | (2u<<3) | (3u<<6) | (0u<<9) | (1u<<12);
      if (fixed == 3) return 2u | (1u<<3) | (0u<<6) | (3u<<9) | (1u<<12); }
    /* D3D12 mapping: destR | destG<<3 | destB<<6 | destA<<9 | valid bit */
    { static int _rd = -1; if (_rd < 0) _rd = getenv("REMAPDBG") ? 1 : 0;
      if (_rd) { static u32 seen[32]; static int ns=0;
        u32 k = (basef << 16) | (c1 & 0xFFFFu); int f=0;
        for (int i2=0;i2<ns;i2++) if (seen[i2]==k) f=1;
        if (!f && ns<32) { seen[ns++]=k;
            fprintf(stderr, "[REMAP] basef=0x%02X c1=0x%04X -> destR=%u destG=%u destB=%u destA=%u%c",
                    basef, c1 & 0xFFFFu, out[1], out[2], out[3], out[0], 10); } } }
    return out[1] | (out[2] << 3) | (out[3] << 6) | (out[0] << 9) | (1u << 12);
}

/* Morton/Z-order texel offset for RSX swizzled textures (LN bit clear).
 * Interleaves x (even bit positions) and y (odd) until the smaller dimension's
 * bits run out, then the larger dimension's remaining bits ride above -- the
 * NV40 layout (matches RPCS3 convert_linear_swizzle). POT dims only, which is
 * what the hardware requires for swizzled textures. */

/* Sparse FNV-1a over a texture's source bytes -- enough to notice an animated
 * surface changing, cheap enough to run on every bind. Kept a function rather
 * than a statement-expression macro so the file still compiles under MSVC;
 * ps3recomp builds with clang-cl, but titles like Tokyo Jungle drive the build
 * through the Visual Studio generator. */
static u32 tex_csum(const u8* base, u32 nbytes)
{
    /* [NHL local patch] Every byte, not 1024 samples. The sparse walk read 4
     * bytes in every nbytes/1024 (4 of every 128 in a 512x256 B8 atlas), so a
     * glyph the title rasterised into its font cache between sample points --
     * or team logos streamed into a texture after first use -- never changed
     * the hash and the GPU kept the stale copy: NHL's main menu lost letters
     * ("CLAS", "CODDCNITS") and its team grid stayed white. It runs at most
     * once per texture per frame (the c->used check), and four independent
     * 64-bit lanes keep it at memory speed. TEX_CSUM_SPARSE=1 restores the
     * old sampling. */
    static int sparse = -1;
    if (sparse < 0) sparse = getenv("TEX_CSUM_SPARSE") ? 1 : 0;
    if (sparse) {
        u32 h = 2166136261u;
        u32 step = nbytes > 4096u ? nbytes / 1024u : 4u;
        for (u32 i = 0; i + 3 < nbytes; i += step) {
            u32 w32; memcpy(&w32, base + i, sizeof w32);
            h ^= w32; h *= 16777619u;
        }
        return h;
    }
    const u64 P = 0x100000001B3ull;
    u64 h0 = 0xCBF29CE484222325ull, h1 = h0 ^ 1, h2 = h0 ^ 2, h3 = h0 ^ 3;
    u32 i = 0;
    for (; i + 32 <= nbytes; i += 32) {
        u64 a, b, c, d;
        memcpy(&a, base + i, 8); memcpy(&b, base + i + 8, 8);
        memcpy(&c, base + i + 16, 8); memcpy(&d, base + i + 24, 8);
        h0 = (h0 ^ a) * P; h1 = (h1 ^ b) * P; h2 = (h2 ^ c) * P; h3 = (h3 ^ d) * P;
    }
    for (; i < nbytes; i++) h0 = (h0 ^ base[i]) * P;
    u64 h = h0 ^ (h1 * 31) ^ (h2 * 961) ^ (h3 * 29791) ^ nbytes;
    return (u32)(h ^ (h >> 32));
}

/* [NHL local patch #134] Texture binds were 15% of the RSX thread (play185):
 * every texture a frame used was hashed in full every frame (tex_csum, 11%)
 * after a linear walk of the 1024 slots. Now:
 *  - a hint table maps the bind key to its slot, so a hit skips the walk;
 *  - a texture whose full checksum has matched twice in a row since its last
 *    upload is fully re-hashed only every TEX_LAZY (default 8) to TEX_LAZY+7
 *    frames (staggered by slot). In between, 512 sampled words plus the first
 *    and last 64 bytes must match, which catches whole-image changes (streamed
 *    logos, movie frames) at once; a small edit between sample points (a glyph
 *    rasterised into the font cache) shows up at the next full check.
 * Textures up to 32 KB are always hashed in full. TEX_LAZY=0 = full every frame. */
#define TEX_HINT_N 4096u
static short s_tex_hint[TEX_HINT_N];   /* slot + 1, 0 = none */
static u32 tex_hint_at(u32 key, u32 w, u32 h, u32 fmt, int cube)
{
    u32 x = key * 0x9E3779B1u ^ ((w << 16) | h) * 0x85EBCA77u ^ fmt * 0xC2B2AE3Du ^ (u32)cube;
    x ^= x >> 15; x *= 0x2C1B3C6Du; x ^= x >> 12;
    return x & (TEX_HINT_N - 1u);
}
static u32 tex_csum_sampled(const u8* base, u32 nbytes)
{
    u64 h = 0xCBF29CE484222325ull ^ nbytes; const u64 P = 0x100000001B3ull;
    if (nbytes < 256u) return tex_csum(base, nbytes);
    for (u32 i = 0; i < 64u; i += 8) { u64 a, b; memcpy(&a, base + i, 8); memcpy(&b, base + nbytes - 64u + i, 8);
                                       h = (h ^ a) * P; h = (h ^ b) * P; }
    const u32 step = (nbytes - 8u) / 512u;
    for (u32 i = 0, o = 0; i < 512u; i++, o += step) { u64 a; memcpy(&a, base + o, 8); h = (h ^ a) * P; }
    return (u32)(h ^ (h >> 32));
}
static int tex_lazy(void)
{
    static int v = -1;
    if (v < 0) { const char* e = getenv("TEX_LAZY"); v = e ? atoi(e) : 8; }
    return v;
}

static u32 s_texdump_draw_rt = 0;   /* [#175] colour target of the draw being bound (NHL_TEXDUMP filter) */
int (*g_vp6_frame_of_hook)(u32 ea, u32* wh) = 0;   /* [#178] set by the NHL VP6 bridge (NHL_VP6_SHOW=1) */
/* [NHL local patch #179] Movie planes are read when the draw is RECORDED, not when the
 * batch executes. The game reuses a movie ring slot once the GPU has consumed it -- for
 * us, when the FIFO passes its fence -- but the upload used to read guest memory only at
 * render_frame, after the job thread (or the VP6 bridge) had already written the NEXT
 * frame into that slot: the screen showed a future frame, then stepped back (the [vp6show]
 * "B3" pattern, the intro/Visotron jitter). g_vp6_plane_hook (set by the bridge) says
 * whether an address is a movie Y/U/V plane; its bytes are copied into a ring of host
 * buffers at record time and uploaded from there. NHL_VP6_SNAP=0 = old (read at execute). */
int (*g_vp6_plane_hook)(u32 ea, u32* wh, int* idx) = 0;
static const u8* s_tex_snap = NULL;   /* consumed by the next vp_upload_tex_slot */
static void vp6_snap_record(D3D12DrawRecord* dr)
{
    dr->vp6_idx = -1; dr->vp6_wh = 0;
    for (int u = 0; u < D3D_TEX_UNITS; u++) dr->tex[u].snap = NULL;
    if (!g_vp6_plane_hook) return;
    static int on = -1;
    if (on < 0) { const char* e = getenv("NHL_VP6_SNAP"); on = (e && *e == '0') ? 0 : 1; }
    extern uint8_t* vm_base;
    enum { RING = 64 };
    static struct { u8* p; size_t cap; } ring[RING]; static u32 rn = 0;
    /* the same (plane, frame) bound by several draws shares one snapshot while it is in the ring */
    static struct { u32 off; int idx; u32 seq; } recent[24]; static u32 rr = 0;
    for (int u = 0; u < D3D_TEX_UNITS; u++) {
        if (!dr->tex[u].set || !dr->tex[u].off || (dr->tex[u].fmt & 0x9Fu) != 0x81u) continue;
        u32 wh = 0; int idx = -1;
        const int plane = g_vp6_plane_hook(dr->tex[u].off, &wh, &idx);
        if (plane < 0) continue;
        if (plane == 0 && idx > dr->vp6_idx) { dr->vp6_idx = idx; dr->vp6_wh = wh; }
        if (!on) continue;
        int reuse = -1;
        for (int i = 0; i < 24; i++)
            if (recent[i].off == dr->tex[u].off && recent[i].idx == idx && rn - recent[i].seq < RING - 8) { reuse = (int)(recent[i].seq % RING); break; }
        if (reuse >= 0 && ring[reuse].p) { dr->tex[u].snap = ring[reuse].p; continue; }
        const size_t row = (dr->tex[u].pitch > dr->tex[u].w) ? dr->tex[u].pitch : dr->tex[u].w;
        const size_t n = row * dr->tex[u].h;
        if (!n || n > (8u << 20)) continue;
        const u32 seq = rn++, k = seq % RING;
        if (ring[k].cap < n) { free(ring[k].p); ring[k].p = (u8*)malloc(n); ring[k].cap = ring[k].p ? n : 0; }
        if (!ring[k].p) continue;
        memcpy(ring[k].p, vm_base + dr->tex[u].off, n);
        dr->tex[u].snap = ring[k].p;
        { const u32 ri = rr++ % 24u; recent[ri].off = dr->tex[u].off; recent[ri].idx = idx; recent[ri].seq = seq; }
    }
}
/* [NHL local patch #219] Shared upload ring for texture uploads. Every NEW texture used to
 * create two committed resources: the texture and its own persistent UPLOAD buffer (as big
 * as the texture + mips, kept forever). NHL creates ~100 textures at once at the match-intro
 * start and before puck drop (play321/322: 92-110 per second, ~90 ms of texture path). Now
 * the staging bytes go into one persistently mapped ring; a region is reused once the fence
 * value stamped at its submission (upr_on_submit, after render_frame's ExecuteCommandLists)
 * has completed. Allocation never waits: if the ring is full of unsubmitted/in-flight data
 * the old per-slot buffer is used. NHL_UPLOAD_RING=0 = off, NHL_UPLOAD_RING_MB = size (256). */
typedef struct { u64 bytes, stamp; } UprEnt;   /* stamp 0 = not yet submitted */
static ID3D12Resource* s_upr = NULL; static u8* s_upr_map = NULL;
static u64 s_upr_size = 0, s_upr_head = 0, s_upr_used = 0;
static UprEnt s_upr_q[8192]; static u32 s_upr_qh = 0, s_upr_qn = 0;
static int upr_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char* e = getenv("NHL_UPLOAD_RING"); on = (e && *e == '0') ? 0 : 1;
        if (on && s_d3d.device) {
            const char* m = getenv("NHL_UPLOAD_RING_MB");
            u64 mb = m ? (u64)atoi(m) : 256u; if (mb < 16u) mb = 16u;
            D3D12_HEAP_PROPERTIES hu = {0}; hu.Type = D3D12_HEAP_TYPE_UPLOAD;
            D3D12_RESOURCE_DESC bd = {0};
            bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = mb << 20;
            bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc.Count = 1;
            bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            void* p = NULL; D3D12_RANGE nr = {0, 0};
            if (FAILED(s_d3d.device->lpVtbl->CreateCommittedResource(s_d3d.device, &hu, D3D12_HEAP_FLAG_NONE, &bd,
                    D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource, (void**)&s_upr)) || !s_upr ||
                FAILED(s_upr->lpVtbl->Map(s_upr, 0, &nr, &p)) || !p) {
                if (s_upr) { s_upr->lpVtbl->Release(s_upr); s_upr = NULL; }
                on = 0;
            } else { s_upr_map = (u8*)p; s_upr_size = mb << 20; }
            fprintf(stderr, "[UPRING] texture upload ring %s (%llu MB)%c", on ? "on" : "FAILED -> per-slot buffers",
                    (unsigned long long)mb, 10);
        }
    }
    return on && s_upr;
}
static void upr_retire(void)
{
    const u64 done = s_d3d.fence->lpVtbl->GetCompletedValue(s_d3d.fence);
    while (s_upr_qn && s_upr_q[s_upr_qh].stamp && s_upr_q[s_upr_qh].stamp <= done) {
        s_upr_used -= s_upr_q[s_upr_qh].bytes;
        s_upr_qh = (s_upr_qh + 1u) % 8192u; s_upr_qn--;
    }
}
/* the command list holding every unstamped region was just submitted: any fence value
 * signalled from now on is above the current one */
static void upr_on_submit(void)
{
    if (!s_upr) return;
    const u64 stamp = s_d3d.fence_values[s_d3d.frame_index] + 1u;
    for (u32 i = 0; i < s_upr_qn; i++) {
        UprEnt* e = &s_upr_q[(s_upr_qh + i) % 8192u];
        if (!e->stamp) e->stamp = stamp;
    }
}
static int upr_push(u64 bytes)
{
    if (s_upr_qn >= 8192u) return 0;
    UprEnt* e = &s_upr_q[(s_upr_qh + s_upr_qn) % 8192u]; s_upr_qn++;
    e->bytes = bytes; e->stamp = 0; s_upr_used += bytes;
    return 1;
}
/* offset of n bytes (512-aligned) in the ring, or -1 = use a per-slot buffer */
static long long upr_alloc(u64 n)
{
    if (!upr_on()) return -1;
    n = (n + 511u) & ~(u64)511u;
    if (n > s_upr_size / 4u) return -1;
    upr_retire();
    const u64 pad = (s_upr_head + n > s_upr_size) ? s_upr_size - s_upr_head : 0u;
    if (s_upr_used + pad + n > s_upr_size || s_upr_qn + 2u > 8192u) {
        static int n_full = 0; if (n_full++ < 4) fprintf(stderr, "[UPRING] full (%llu MB in flight) -> per-slot buffer%c",
                                                         (unsigned long long)(s_upr_used >> 20), 10);
        return -1;
    }
    if (pad) { upr_push(pad); s_upr_head = 0; }
    const u64 off = s_upr_head;
    upr_push(n);
    s_upr_head += n; if (s_upr_head >= s_upr_size) s_upr_head = 0;
    return (long long)off;
}

/* [NHL local patch #220] Texture resources placed in pre-allocated heaps. After #219 the
 * remaining burst cost was CreateCommittedResource for the texture itself, ~0.45 ms each
 * (play323: 44 ms for 98 new textures at the intro start / before puck drop). Textures are
 * now CreatePlacedResource'd into 64 MB DEFAULT heaps (first-fit free lists, coalescing).
 * A replaced texture's block AND its resource object are released only once the fence
 * value stamped at the next submission has completed, so a frame still sampling the old
 * texture never sees its memory reused. > 32 MB or any failure -> committed, as before.
 * NHL_TEX_HEAPS=0 = off. [TEXHEAP] logs heap creation. */
#define TXH_MAX 48
#define TXH_SIZE (64ull << 20)
#define TXH_FREE_MAX 2048
typedef struct { u64 off, size; } TxhBlk;
typedef struct { ID3D12Heap* heap; TxhBlk fr[TXH_FREE_MAX]; int nfr; } TxHeap;
static TxHeap s_txh[TXH_MAX]; static int s_txh_n = 0;
typedef struct { ID3D12Resource* res; int heap; u64 off, size, stamp; } TxhDefer;
static TxhDefer s_txd[4096]; static u32 s_txd_h = 0, s_txd_n = 0;
static int txh_on(void) { static int on = -1; if (on < 0) { const char* e = getenv("NHL_TEX_HEAPS"); on = (e && *e == '0') ? 0 : 1; } return on; }
static void txh_free_now(int hi, u64 off, u64 size)
{
    TxHeap* h = &s_txh[hi];
    int i = 0; while (i < h->nfr && h->fr[i].off < off) i++;
    /* merge with the previous and/or next free block */
    const int prev = (i > 0 && h->fr[i - 1].off + h->fr[i - 1].size == off);
    const int next = (i < h->nfr && off + size == h->fr[i].off);
    if (prev && next) { h->fr[i - 1].size += size + h->fr[i].size;
                        memmove(&h->fr[i], &h->fr[i + 1], (size_t)(h->nfr - i - 1) * sizeof(TxhBlk)); h->nfr--; }
    else if (prev) h->fr[i - 1].size += size;
    else if (next) { h->fr[i].off = off; h->fr[i].size += size; }
    else if (h->nfr < TXH_FREE_MAX) {
        memmove(&h->fr[i + 1], &h->fr[i], (size_t)(h->nfr - i) * sizeof(TxhBlk));
        h->fr[i].off = off; h->fr[i].size = size; h->nfr++;
    }   /* (free list full: the block is leaked -- never seen; 2048 fragments per heap) */
}
static void txh_retire(void)
{
    const u64 done = s_d3d.fence->lpVtbl->GetCompletedValue(s_d3d.fence);
    while (s_txd_n && s_txd[s_txd_h].stamp && s_txd[s_txd_h].stamp <= done) {
        TxhDefer* d = &s_txd[s_txd_h];
        if (d->res) d->res->lpVtbl->Release(d->res);
        if (d->size) txh_free_now(d->heap, d->off, d->size);
        s_txd_h = (s_txd_h + 1u) % 4096u; s_txd_n--;
    }
}
static void txh_on_submit(void)
{
    if (!s_txd_n) return;
    txh_retire();
    const u64 stamp = s_d3d.fence_values[s_d3d.frame_index] + 1u;
    for (u32 i = 0; i < s_txd_n; i++) { TxhDefer* d = &s_txd[(s_txd_h + i) % 4096u]; if (!d->stamp) d->stamp = stamp; }
}
/* release a slot's texture: placed blocks (and, with heaps on, every resource) wait for the fence */
static void txh_release(VPTexSlot* t)
{
    if (!t->res) return;
    if (txh_on() && s_txd_n < 4096u) {
        TxhDefer* d = &s_txd[(s_txd_h + s_txd_n) % 4096u]; s_txd_n++;
        d->res = t->res; d->heap = t->hp_heap; d->off = t->hp_off; d->size = t->hp_size; d->stamp = 0;
    } else {
        t->res->lpVtbl->Release(t->res);
        if (t->hp_size) txh_free_now(t->hp_heap, t->hp_off, t->hp_size);   /* (queue full: old immediate behaviour) */
    }
    t->res = NULL; t->hp_size = 0;
}
/* create t->res as a placed resource; 0 = not done (caller falls back to committed) */
static int txh_create(VPTexSlot* t, const D3D12_RESOURCE_DESC* td)
{
    if (!txh_on()) return 0;
    txh_retire();
    D3D12_RESOURCE_ALLOCATION_INFO ai = {0};   /* C binding: aggregate return through an out pointer */
    s_d3d.device->lpVtbl->GetResourceAllocationInfo(s_d3d.device, &ai, 0, 1, td);
    if (!ai.SizeInBytes || ai.SizeInBytes > TXH_SIZE / 2u || ai.Alignment > (64u << 10)) return 0;
    const u64 al = ai.Alignment ? ai.Alignment : (64u << 10);
    for (int pass = 0; pass < 2; pass++) {
        for (int hi = 0; hi < s_txh_n; hi++) {
            TxHeap* h = &s_txh[hi];
            for (int i = 0; i < h->nfr; i++) {
                const u64 a = (h->fr[i].off + al - 1u) & ~(al - 1u);
                if (a + ai.SizeInBytes > h->fr[i].off + h->fr[i].size) continue;
                if (FAILED(s_d3d.device->lpVtbl->CreatePlacedResource(s_d3d.device, h->heap, a, td,
                        D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void**)&t->res)) || !t->res) {
                    t->res = NULL; return 0;
                }
                /* carve [a, a + size) out of block i (head gap stays free) */
                const u64 bo = h->fr[i].off, bs = h->fr[i].size, end = a + ai.SizeInBytes;
                if (a > bo) { h->fr[i].size = a - bo;
                              if (end < bo + bs) txh_free_now(hi, end, bo + bs - end); }
                else if (end < bo + bs) { h->fr[i].off = end; h->fr[i].size = bo + bs - end; }
                else { memmove(&h->fr[i], &h->fr[i + 1], (size_t)(h->nfr - i - 1) * sizeof(TxhBlk)); h->nfr--; }
                t->hp_heap = hi; t->hp_off = a; t->hp_size = ai.SizeInBytes;
                return 1;
            }
        }
        if (pass || s_txh_n >= TXH_MAX) break;
        /* no room: one more heap */
        D3D12_HEAP_DESC hd = {0};
        hd.SizeInBytes = TXH_SIZE; hd.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
        hd.Alignment = 0; hd.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES;
        ID3D12Heap* heap = NULL;
        if (FAILED(s_d3d.device->lpVtbl->CreateHeap(s_d3d.device, &hd, &IID_ID3D12Heap, (void**)&heap)) || !heap) return 0;
        TxHeap* nh = &s_txh[s_txh_n++]; nh->heap = heap; nh->nfr = 1; nh->fr[0].off = 0; nh->fr[0].size = TXH_SIZE;
        fprintf(stderr, "[TEXHEAP] heap %d created (%d x 64 MB)%c", s_txh_n - 1, s_txh_n, 10);
    }
    return 0;
}

static int vp_upload_tex_slot(u32 off, u32 w, u32 h, u32 fmt, int cube, u32 mips, u32 tex_pitch)
{
    const u8* const snap = s_tex_snap;   /* [#179] record-time movie plane bytes, if any */
    s_tex_snap = NULL;
    /* TEX_BUDGET=<n>: cap texture uploads per frame. Off by default -- it was
     * tried against the TDR and does not help, because PERF shows the upload
     * path costing 0.01s for 775 calls. Kept because it is the right lever if a
     * title ever does hit an upload-bound frame. */
    { static int budget = -1;
      if (budget < 0) { const char* e = getenv("TEX_BUDGET"); budget = e ? atoi(e) : 0; }
      if (budget > 0) {
          static u64 last_frame = ~0ull; static int used = 0;
          if (last_frame != (u64)s_d3d.frame_count) { last_frame = (u64)s_d3d.frame_count; used = 0; }
          if (++used > budget) return -1;   /* try again next frame */
      } }
    extern uint8_t* vm_base;

    if (!off || !w || !h || !vm_base || !s_d3d.srv_heap) return -1;
    /* Format classes (base = fmt & 0x9F). The LBP loading screen uses:
     * 0x85 A8R8G8B8 (swizzled UI art), 0x8B G8B8 (the 1024x2048 linear FONT
     * atlas -- without it no text renders at all), 0x86/87/88 DXT1/23/45
     * (512x512 detail/LUT layers bound at t1/t3 on every draw). */
    /* Format class, row sizes and swizzling are RSX semantics: rsx_texture_
     * layout.c owns them so the Metal backend can read a guest texture without
     * re-deriving any of it. Only the DXGI mapping below is ours. */
    rsx_tex_layout tl;
    rsx_texture_layout(fmt, w, h, &tl);
    /* [NHL local patch #173] SET_TEXTURE_CONTROL3's row pitch. A LINEAR texture
     * (LN bit) whose pitch is wider than its packed row stores its rows that far
     * apart -- e.g. a strip cut out of a wider atlas. Decoding it packed shears it
     * into repeated slivers. tpitch = the source row stride when it differs (0 =
     * packed); it is part of the cache key. Uncompressed rows go through the
     * layout's pitched form, DXT block rows are copied with the stride below.
     * NHL_TEX_PITCH=0 = old packed decode. [TEXPITCH] logs distinct cases. */
    u32 tpitch = 0;
    { static int on = -1;
      if (on < 0) { const char* e = getenv("NHL_TEX_PITCH"); on = (e && *e == '0') ? 0 : 1; }
      if (on && tex_pitch > tl.row_bytes && (fmt & 0x20u) && !cube) {
          tpitch = tex_pitch;
          if (!tl.compressed) rsx_texture_layout_pitched(fmt, w, h, tex_pitch, &tl);
          static u32 seen[64]; static int ns = 0; int k = 0;
          const u32 sk = off ^ (tex_pitch << 4) ^ fmt;
          for (int i = 0; i < ns; i++) if (seen[i] == sk) k = 1;
          if (!k && ns < 64) { seen[ns++] = sk;
              fprintf(stderr, "[TEXPITCH] off=0x%08X %ux%u fmt=0x%02X pitch=%u (packed row %u)%c",
                      off, w, h, fmt, tex_pitch, tl.compressed ? tl.row_bytes : w * tl.bytes_per_texel, 10); }
      } }
    u32 basef = fmt & 0x9F;              /* still needed for the SRV remap */
    int argb = (tl.fmt == RSX_TEXFMT_R8G8B8A8);
    int dxt  = tl.compressed;
    /* Compressed rows are counted in blocks, so keep bpp at 1 for the byte
     * arithmetic the diagnostics below do. */
    u32 bpp = tl.compressed ? 1u : tl.bytes_per_texel;
    /* The DECODED image's format, which is not always the guest's: the packed
     * 16-bit colour formats arrive here unpacked to RGBA8, and DEPTH24_D8 as a
     * float. rsx_texture_layout.h says which class each format byte lands in. */
    DXGI_FORMAT dxfmt = (tl.fmt == RSX_TEXFMT_R8G8B8A8) ? DXGI_FORMAT_R8G8B8A8_UNORM
                      : (tl.fmt == RSX_TEXFMT_R8G8)     ? DXGI_FORMAT_R8G8_UNORM
                      : (tl.fmt == RSX_TEXFMT_BC1)      ? DXGI_FORMAT_BC1_UNORM
                      : (tl.fmt == RSX_TEXFMT_BC2)      ? DXGI_FORMAT_BC2_UNORM
                      : (tl.fmt == RSX_TEXFMT_BC3)      ? DXGI_FORMAT_BC3_UNORM
                      : (tl.fmt == RSX_TEXFMT_R16)      ? DXGI_FORMAT_R16_UNORM
                      : (tl.fmt == RSX_TEXFMT_R16G16)   ? DXGI_FORMAT_R16G16_UNORM
                      : (tl.fmt == RSX_TEXFMT_R16G16F)  ? DXGI_FORMAT_R16G16_FLOAT
                      : (tl.fmt == RSX_TEXFMT_R16G16B16A16F) ? DXGI_FORMAT_R16G16B16A16_FLOAT
                      : (tl.fmt == RSX_TEXFMT_R32F)     ? DXGI_FORMAT_R32_FLOAT
                      : (tl.fmt == RSX_TEXFMT_R32G32B32A32F) ? DXGI_FORMAT_R32G32B32A32_FLOAT
                      : DXGI_FORMAT_R8_UNORM;
    /* DXT data is stored as linear 4x4 block rows (compressed formats are
     * never Morton-swizzled on RSX). Row of blocks = (w/4)*blocksize. */
    u32 blkrow  = dxt ? tl.row_bytes : 0;
    u32 blkrows = dxt ? tl.rows      : 0;
    const u32 key_off = off;         /* lookup key: the offset as bound */
    /* [NHL local patch #207] NHL_COOKIE_WHITE=1 (diag): the 4x4 ARGB default spot cookie at
     * local 0x0147D000 (bound on every unused spot unit) is all zero in guest memory here.
     * With zero light matrices (away walk-out) the player FPs' spot masks pass and that texel
     * IS the light -> force it white to see whether the walk-out then matches RPCS3. */
    { static int cw = -1; if (cw < 0) cw = getenv("NHL_COOKIE_WHITE") ? 1 : 0;
      if (cw && off == 0xC147D000u && w == 4 && h == 4 && (fmt & 0x9Fu) == 0x85u) {
          memset(vm_base + off, 0xFF, 64);
          static int once = 0; if (!once++) fprintf(stderr, "[COOKIEWHITE] default cookie 0x%08X forced white%c", off, 10); } }
    /* Sparse checksum of a texture's source bytes -- enough to notice an
     * animated surface changing, cheap enough to run on every bind. */
    /* (portable helper tex_csum() -- see above; a statement-expression macro
     * here was a GCC/Clang extension MSVC rejects.) */
    #define TEX_CSUM(base, nbytes) tex_csum((base), (nbytes))
    int slot = -1, freeslot = -1, hit = -1;
    #define TEX_MATCH(c) ((c)->res && (c)->key == key_off && (c)->w == w && (c)->h == h && \
                          (c)->fmt == fmt && (c)->cube == cube && (c)->pitch == tpitch)
    const u32 hk = tex_hint_at(key_off, w, h, fmt, cube);   /* #134 */
    { const int hs = s_tex_hint[hk] - 1;
      if (hs >= 0 && TEX_MATCH(&s_d3d.vp_tex[hs])) hit = hs; }
    /* [NHL local patch #136] Victim choice. The first slot not yet bound THIS
     * frame used to be taken -- even with hundreds of slots still empty -- and
     * at the start of a frame that is usually a texture the same frame binds
     * later (arena, jerseys). It then missed and evicted the next one: a
     * cascade of resource re-creations + decodes + full hashes whenever a batch
     * of new textures appeared (puck drop, replays: 450-580 ms/s of binds in
     * play188). Now an empty slot first, else the least recently bound one,
     * never one bound this frame. TEX_EVICT_OLD=1 = the old first-unused pick. */
    if (hit < 0) {
        static int evict_old = -1;
        if (evict_old < 0) evict_old = getenv("TEX_EVICT_OLD") ? 1 : 0;
        const u32 fcur = (u32)s_d3d.frame_count;
        int empty = -1, lru = -1; u32 lru_age = 0;
        for (int i = 0; i < VP_TEX_SLOTS; i++) {
            VPTexSlot* c = &s_d3d.vp_tex[i];
            if (TEX_MATCH(c)) { hit = i; s_tex_hint[hk] = (short)(i + 1); break; }
            if (c->used) continue;
            if (evict_old) { if (freeslot < 0) freeslot = i; continue; }
            if (!c->res) { if (empty < 0) empty = i; continue; }
            const u32 age = fcur - c->last_frame;
            if (lru < 0 || age > lru_age) { lru = i; lru_age = age; }
        }
        if (!evict_old) freeslot = empty >= 0 ? empty : lru;
    }
    #undef TEX_MATCH
    u32 prehash = 0, prehash_nb = 0; int have_prehash = 0;   /* #136: stale path's full hash (#239: + its length) */
    if (hit >= 0) {
        VPTexSlot* c = &s_d3d.vp_tex[hit];
        if (c->used) return hit;                  /* already bound this frame */
        c->last_frame = (u32)s_d3d.frame_count;   /* #136 */
        { static int nocache = -1;                /* TEX_NOCACHE=1: always re-upload */
          if (nocache < 0) { const char* e = getenv("TEX_NOCACHE");
                             nocache = e ? atoi(e) : 0; }
          if (nocache) slot = hit; }
        if (slot < 0) {
            /* Held from an earlier frame: re-upload only if the guest bytes
             * changed. Most of this scene's textures are static, and converting
             * every one of them every frame was ~70% of the frame's CPU time. */
            const u32 nb = (c->hbytes && !snap) ? c->hbytes   /* #239: what the stored checksums cover */
                         : tpitch ? tpitch * (dxt ? blkrows : h)   /* #173 */
                                  : dxt ? (blkrow * blkrows) : (w * h * bpp);
            const u32 fc = (u32)s_d3d.frame_count;
            const int lazy = tex_lazy();
            if (!snap && lazy > 0 && nb > 32768u && c->stable >= 2 &&
                fc - c->ok_frame < (u32)lazy + ((u32)hit & 7u) &&
                tex_csum_sampled(vm_base + c->off, nb) == c->scsum) {
                /* [NHL local patch #178] TEX_LAZY_VERIFY=1: check every lazy "unchanged" with
                 * the full hash; a miss (sampled words equal, bytes changed = a stale texture
                 * on screen, e.g. a movie plane) is counted and re-uploaded. [TEXLAZY] every 5 s. */
                static int lv = -1; if (lv < 0) lv = getenv("TEX_LAZY_VERIFY") ? 1 : 0;
                if (!lv) { c->used = 1; return hit; }
                static unsigned nh = 0, nm = 0; static u32 mw = 0, mh = 0, mf = 0, mo = 0; static ULONGLONG lt = 0;
                const int miss = TEX_CSUM(vm_base + c->off, nb) != c->csum;
                if (miss) { nm++; if (w * h >= mw * mh) { mw = w; mh = h; mf = fmt; mo = c->off; } } else nh++;
                const ULONGLONG t = GetTickCount64();
                if (t - lt >= 5000) { lt = t;
                    fprintf(stderr, "[TEXLAZY] lazy-unchanged %u, of them actually changed %u (largest %ux%u fmt=0x%02X off=0x%08X)%c",
                            nh + nm, nm, mw, mh, mf, mo, 10);
                    nh = nm = 0; mw = mh = mf = mo = 0; }
                if (!miss) { c->used = 1; return hit; }
            }
            prehash = TEX_CSUM(snap ? snap : vm_base + c->off, nb);
            prehash_nb = nb;
            if (prehash == c->csum) {
                c->used = 1; c->ok_frame = fc; if (c->stable < 255u) c->stable++;
                return hit;
            }
            have_prehash = 1;
            slot = hit;                           /* stale: fall through and redo */
        }
    }
    /* TEX_OFF_BIAS=1: sample the image one whole level-0 BELOW the bound offset.
     * Measured on the Rubber Ducky demo: at the bind offset every texture reads
     * as all-zero, while [off - w*h*bpp, off) holds the image (3800-4160 of 4298
     * sampled bytes non-zero, for four different textures at three sizes). So
     * the offset the guest programs points PAST level 0 rather than at it. */
    /* TEX_OFF_BIAS: 1 = one level-0 below the bound offset, 2 = one full MIP
     * CHAIN above it. The measured upload-to-bind deltas on the Rubber Ducky
     * demo are 0x2AAB00 for a 2MB level 0 and 0x555580 for a 4MB one -- both
     * exactly the mipmap pyramid total (L0 * 4/3), which is what the guest
     * reserves per texture. */
    { static int bias = -1;
      if (bias < 0) { const char* e = getenv("TEX_OFF_BIAS"); bias = e ? atoi(e) : 0; }
      if (bias == 1) { u32 sz = w * h * bpp; if (off > sz) off -= sz; }
      else if (bias >= 2) {
          u32 chain = 0;
          for (u32 mw = w, mh = h; mw && mh; mw >>= 1, mh >>= 1) {
              chain += mw * mh * bpp;
              if (mw == 1 && mh == 1) break;
          }
          /* bias 3: only shift when the bound offset really is empty, so a
           * texture that IS bound correctly keeps its own data. Applying the
           * chain unconditionally fixes the mis-bound majority but breaks any
           * correctly-bound minority, which shows up as objects going black. */
          int shift = 1;
          if (bias >= 3) {
              u32 nz = 0, sz = w * h * bpp;
              for (u32 i = 0; i < sz && i < 0x20000u; i += 97) if (vm_base[off + i]) nz++;
              shift = (nz == 0);
          }
          if (shift) off += chain;
      } }
    /* TEX_REMAP=1: if the bound offset holds nothing, look the image up by size
     * in the VRAM upload registry (cellGcmSys). For a title whose upload address
     * and SET_TEXTURE_OFFSET disagree this puts the real bytes under the sampler
     * without guessing a delta. */
    { static int remap = -1;
      if (remap < 0) { const char* e = getenv("TEX_REMAP"); remap = e ? atoi(e) : 0; }
      if (remap) {
        u32 nz = 0, sz = w * h * bpp;
        for (u32 i = 0; i < sz && i < 0x20000u; i += 97) if (vm_base[off + i]) nz++;
        if (!nz) {
            extern u32 rsx_find_vram_upload(u32);
            u32 alt = rsx_find_vram_upload(sz);
            if (alt) {
                static int _n = 0;
                if (_n++ < 8) fprintf(stderr, "[TEXREMAP] 0x%08X (%ux%u, %u bytes) -> 0x%08X%c",
                                      off, w, h, sz, alt, 10);
                off = alt;
            }
        } } }
    /* A stale cache entry (slot >= 0) re-uses its own slot; otherwise take a
     * free one. Without this the stale path fell through to freeslot and either
     * duplicated the texture into a second slot or bailed when none was free. */
    if (slot < 0 && freeslot < 0) {               /* out of slots this frame */
        static ULONGLONG t_last = 0; static u32 n_full = 0; n_full++;   /* [#100] never silent again */
        const ULONGLONG t = GetTickCount64();
        if (t - t_last >= 5000) { t_last = t;
            fprintf(stderr, "[TEXSLOTS] out of %d texture slots: %u binds dropped (null SRV -> black)%c",
                    VP_TEX_SLOTS, n_full, 10); n_full = 0; }
        return -1;
    }
    /* TEX_SRCDBG=<N>: is the guest memory this slot uploads FROM actually
     * populated? An empty source and a broken sampler both render flat. */
    { static int cap = -1, n = 0;
      if (cap < 0) { const char* e = getenv("TEX_SRCDBG"); cap = e ? atoi(e) : 0; }
      if (cap && n < cap) { n++;
        u32 nz = 0, tot = 0;
        for (u32 i = 0; i < w * h * bpp && i < 0x40000u; i += 61) { tot++; if (vm_base[off + i]) nz++; }
        /* Several of this title's textures are bound at EXACTLY the end address
         * of an upload, so also sample the block immediately BELOW the bind
         * offset: if that holds the image, the bind is one whole texture high. */
        u32 nzb = 0, totb = 0;
        { u32 sz = w * h * bpp; u32 base = (off > sz) ? off - sz : 0;
          for (u32 i = 0; i < sz && i < 0x40000u; i += 61) { totb++; if (vm_base[base + i]) nzb++; } }
        long nearest = 0; u32 found = 0;
        if (!nz) {   /* empty: where IS the data? scan out from the bind offset */
            for (long d = 0x1000; d <= 0x1000000 && !found; d += 0x1000) {
                for (int s = 0; s < 2 && !found; s++) {
                    long a = (long)off + (s ? -d : d);
                    if (a < 0x1000) continue;
                    u32 c = 0;
                    for (u32 i = 0; i < 0x1000u; i += 61) if (vm_base[a + i]) c++;
                    if (c > 8) { nearest = (s ? -d : d); found = c; }
                }
            }
        }
        fprintf(stderr, "[TEXSRC] off=0x%08X %ux%u fmt=0x%02X bpp=%u nonzero=%u/%u"
                        "%s%+ld (page nonzero=%u)\n",
                off, w, h, fmt, bpp, nz, tot,
                nz ? "" : "  nearest data at ", nz ? 0L : nearest, found);
        fprintf(stderr, "[TEXSRC]    block BELOW bind (off-%u): nonzero=%u/%u%c",
                w * h * bpp, nzb, totb, 10);
        /* Walk DOWN in 4KB steps to the first populated page whose predecessor
         * is empty: that is where this texture's data actually begins, and the
         * delta from the bound offset is the rule we are missing. */
        { u32 step = 0x1000, prev_pop = 0; long start_delta = 0;
          for (long d = 0; d >= -(long)(w * h * bpp) * 2; d -= step) {
              u32 a = (u32)((long)off + d); u32 pop = 0;
              if (a < 0x1000) break;
              for (u32 i = 0; i < step; i += 53) if (vm_base[a + i]) pop++;
              if (!pop && prev_pop) { start_delta = d + step; break; }
              prev_pop = pop;
          }
          fprintf(stderr, "[TEXSRC]    data starts at off%+ld (level0=%u bytes)%c",
                  start_delta, w * h * bpp, 10); } } }
    if (slot < 0) slot = freeslot;
    VPTexSlot* t = &s_d3d.vp_tex[slot];
    if (hit >= 0) HITCH_ADD(g_hitch_texup_chg, 1);                  /* #136 */
    else { HITCH_ADD(g_hitch_texup_new, 1); if (t->res) HITCH_ADD(g_hitch_texup_evict, 1); }
    /* The upload buffer's rows hold the DECODED image, so they are sized from
     * dst_row_bytes -- w * bpp is the source row, which is narrower for any
     * format rsx_texture_decode unpacks. */
    u32 pitch = (tl.dst_row_bytes + 255) & ~255u;
    int fresh = 0;
    /* [NHL local patch #119] Upload the guest's MIP CHAIN. Only level 0 was ever
     * uploaded (MipLevels = 1), so a detailed texture far from the camera was
     * point-sampled across dozens of texels per pixel: NHL's jerseys (stripes,
     * crests, numbers) on ~40-px players sparkled through every colour while
     * plain pants/socks looked fine. RSX packs a swizzled or DXT texture's levels
     * back to back after level 0; each is decoded into its own 512-aligned slice
     * of the staging buffer and copied to its own subresource. 2D only; BC levels
     * stop at 4x4. TEX_NO_MIPS=1 restores level-0-only. */
    u32 nl = 1;
    u64 chain_src = 0;   /* [#239] guest bytes of the levels uploaded (0 = level 0 only) */
    u32 lvl_off[16] = {0}, lvl_pitch[16] = {0}, lvl_w[16] = {0}, lvl_h[16] = {0}, lvl_src[16] = {0};
    u64 stage_bytes = (u64)pitch * (dxt ? blkrows : h) * (cube ? 6 : 1);
    /* [NHL local patch #215] Cube maps get their mip chain too. They were level 0 only
     * (and the cube SRV MipLevels = 1), so NHL's environment cube on the player/gear
     * shaders was always sampled at full sharpness: on bump-mapped cloth the reflection
     * vector swings per pixel, real hardware drops to the small blurred levels, we showed
     * crisp high-contrast arena reflections = the "too detailed" speckled/smeared gear vs
     * RPCS3 (user screenshots 2026-10-27). Each face is its own pyramid in guest memory
     * (_face_bytes below); staged as one per-face block of face_stage bytes. NHL_CUBE_MIPS=0
     * = old level-0-only cubes. */
    u64 face_stage = 0;
    static int s_cube_mips = -1;
    if (s_cube_mips < 0) { const char* e = getenv("NHL_CUBE_MIPS"); s_cube_mips = (e && *e == '0') ? 0 : 1; }
    { static int nom = -1; if (nom < 0) nom = getenv("TEX_NO_MIPS") ? 1 : 0;
      if (!nom && (!cube || s_cube_mips) && !tpitch && mips > 1 && (tl.compressed || tl.swizzled)) {
          u64 so = 0, gs = 0; u32 lw = w, lh = h;
          for (u32 l = 0; l < mips && l < 16; l++) {
              if (tl.compressed && (lw < 4 || lh < 4)) break;
              rsx_tex_layout tll; rsx_texture_layout(fmt, lw, lh, &tll);
              const u32 lp = (tll.dst_row_bytes + 255) & ~255u;
              const u32 lr = tll.compressed ? tll.rows : lh;
              so = (so + 511) & ~(u64)511;
              lvl_off[l] = (u32)so; lvl_pitch[l] = lp; lvl_w[l] = lw; lvl_h[l] = lh; lvl_src[l] = (u32)gs;
              so += (u64)lp * lr;
              gs += tll.compressed ? (u64)tll.row_bytes * tll.rows : (u64)lw * lh * tll.bytes_per_texel;
              nl = l + 1;
              lw = lw > 1 ? lw >> 1 : 1; lh = lh > 1 ? lh >> 1 : 1;
          }
          if (nl > 1) {
              if (cube) { face_stage = (so + 511) & ~(u64)511; stage_bytes = face_stage * 6u; }   /* #215 */
              else { stage_bytes = so; chain_src = gs; }   /* #239 */
          } else nl = 1;
      } }

    /* Also recreate when the CUBE-ness changes: a slot holding a 2D texture of
     * the same dimensions would otherwise be reused with 1 array slice while the
     * cube path copies 6 subresources and writes a 6x upload buffer -- which
     * crashes. */
    if (t->res && (t->w != w || t->h != h || t->fmt != fmt || t->cube != cube || t->nl != nl)) {
        txh_release(t);   /* #220: fence-deferred */
        if (t->up) { t->up->lpVtbl->Release(t->up); t->up = NULL; }
    }
    if (!t->res) {
        D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC td = {0};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = w; td.Height = h;
        td.DepthOrArraySize = cube ? 6 : 1;   /* cube = 6 array slices */
        td.MipLevels = (UINT16)nl;   /* #119 */
        /* [#210] typeless for the formats a texture-gamma bind views as *_SRGB (every SRV
         * of these slots names its format explicitly) */
        td.Format = (dxfmt == DXGI_FORMAT_R8G8B8A8_UNORM) ? DXGI_FORMAT_R8G8B8A8_TYPELESS
                  : (dxfmt == DXGI_FORMAT_BC1_UNORM)      ? DXGI_FORMAT_BC1_TYPELESS
                  : (dxfmt == DXGI_FORMAT_BC2_UNORM)      ? DXGI_FORMAT_BC2_TYPELESS
                  : (dxfmt == DXGI_FORMAT_BC3_UNORM)      ? DXGI_FORMAT_BC3_TYPELESS : dxfmt;
        td.SampleDesc.Count = 1;
        td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        const long long _a0 = ps3_qpc_us();   /* #219 */
        if (!txh_create(t, &td)) {   /* #220: placed in a pre-allocated heap, else committed */
            t->hp_size = 0;
            if (FAILED(s_d3d.device->lpVtbl->CreateCommittedResource(
                    s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &td,
                    D3D12_RESOURCE_STATE_COPY_DEST, NULL,
                    &IID_ID3D12Resource, (void**)&t->res)))
                return -1;
        }
        HITCH_ADD(g_hitch_texup_alloc_us, ps3_qpc_us() - _a0);
        fresh = 1;
        HITCH_ADD(g_hitch_texup_create, 1);   /* #136 */
    }
    /* [#219] staging: a region of the shared upload ring, else this slot's own buffer */
    const long long ro = upr_alloc(stage_bytes);
    if (ro < 0 && !t->up) {
        const long long _a0 = ps3_qpc_us();
        D3D12_HEAP_PROPERTIES hu = {0}; hu.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC bd = {0};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = stage_bytes;   /* #119: was pitch * rows * faces (level 0 only) */
        bd.Height = 1; bd.DepthOrArraySize = 1;
        bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(s_d3d.device->lpVtbl->CreateCommittedResource(
                s_d3d.device, &hu, D3D12_HEAP_FLAG_NONE, &bd,
                D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
                &IID_ID3D12Resource, (void**)&t->up))) {
            txh_release(t); return -1;   /* #220 */
        }
        HITCH_ADD(g_hitch_texup_alloc_us, ps3_qpc_us() - _a0);
    }
    const long long _dec0 = ps3_qpc_us();

    void* mapped = NULL; D3D12_RANGE nr = {0,0};
    if (ro >= 0) mapped = s_upr_map + ro;
    else if (FAILED(t->up->lpVtbl->Map(t->up, 0, &nr, &mapped)) || !mapped) return -1;
    { static int _tp=0; if (getenv("RTT_DUMP") && _tp++ < 6) {
        const u8* sp = vm_base + off;
        fprintf(stderr, "[TEXUP] off=0x%X fmt=0x%X row0:", off, fmt);
        for (int _b=0;_b<8;_b++) fprintf(stderr, " %02X", sp[_b]);
        fprintf(stderr, "  row240:");
        for (int _b=0;_b<8;_b++) fprintf(stderr, " %02X", sp[240*w*bpp+_b]);
        fprintf(stderr, "  mid:");
        for (int _b=0;_b<8;_b++) fprintf(stderr, " %02X", sp[240*w*bpp+320*bpp+_b]);
        fprintf(stderr, "%s", "\n");
    } }
    /* Swizzled (LN bit 0x20 clear): texels live in Morton/Z-order, not rows.
     * Uploading them as linear rows produced the diagonal-stripe garbage on
     * LBP's loading screen (every texture there is 0x85 swizzled). POT dims
     * only -- the hardware requires that for swizzled textures anyway. */
    int swz = tl.swizzled;
    /* Cube textures store their 6 faces consecutively. Convert each into its own
     * slice of the upload buffer; the conversion below is unchanged and simply
     * runs once per face with off/mapped rebased. */
    const u32 _face_rows  = dxt ? blkrows : h;
    /* The face stride is NOT one mip-0 image: RSX stores each cube face as its
     * own complete mip pyramid, so face f starts a whole pyramid (128-byte
     * aligned) in. Assuming mip-0-sized strides made face 1 land inside face 0's
     * mip chain -- which is exactly what the dumps showed, every face after the
     * first a progressively smaller copy of the first. */
    u32 _face_bytes = dxt ? (blkrow * blkrows) : (w * h * bpp);
    if (cube) {
        /* [NHL local patch #217] DXT block size from the format. bpp is 1 for every
         * compressed format here, so "bpp == 8 ? 16 : 8" gave 8-byte blocks to DXT3/DXT5
         * too: the face stride came out half size and faces 1-5 of every DXT5 cube were
         * read from the middle of other faces' data. NHL's player/gear reflection cubes are
         * DXT5 HDR-in-alpha (shader: rgb / (a + k)), so the garbage alpha blew the
         * reflection up into the hard "very shiny" gear (play316 F3 mode 3 = no cube
         * looked right). NHL_CUBE_BLK_OLD=1 = old stride. */
        static int s_cube_blk_old = -1;
        if (s_cube_blk_old < 0) s_cube_blk_old = getenv("NHL_CUBE_BLK_OLD") ? 1 : 0;
        const u32 blk_bytes = s_cube_blk_old ? (bpp == 8 ? 16u : 8u) : (tl.fmt == RSX_TEXFMT_BC1 ? 8u : 16u);
        u32 total = 0;
        for (u32 lw = w, lh = h, l = 0; l < (mips ? mips : 1u); l++) {
            total += dxt ? (((lw + 3) / 4) * blk_bytes * ((lh + 3) / 4))
                         : (lw * lh * bpp);
            if (lw > 1) lw >>= 1;
            if (lh > 1) lh >>= 1;
        }
        _face_bytes = (total + 127u) & ~127u;
    }
    const u32 _nfaces     = cube ? 6u : 1u;
    const u32 _off0 = off; u8* const _map0 = (u8*)mapped;
    for (u32 _f = 0; _f < _nfaces; _f++) {
    off    = _off0 + _f * _face_bytes;
    if (cube && getenv("CUBEDBG")) { u32 _s=0; const u8* _p = vm_base + off;
        for (u32 _i = 0; _i < _face_bytes; _i += 97) _s = _s*131 + _p[_i];
        fprintf(stderr, "[CUBEFACE] f=%u off=0x%X bytes=%u bpp=%u swz=%d csum=%08X%c",
                _f, off, _face_bytes, bpp, swz, _s, 10); }
    mapped = _map0 + (face_stage ? (u64)_f * face_stage : (u64)_f * pitch * _face_rows);   /* #215 */
    /* Pixel conversion -- deswizzle, channel order, block copy -- is RSX
     * semantics and lives in rsx_texture_layout.c, so a second backend gets
     * it by calling this rather than porting the loops again. */
    if (tpitch && tl.compressed) {   /* #173: DXT block rows tpitch apart */
        for (u32 y = 0; y < tl.rows; y++)
            memcpy((u8*)mapped + (size_t)y * pitch, vm_base + off + (size_t)y * tpitch, tl.row_bytes);
    } else
    rsx_texture_decode(mapped, pitch, (snap && _f == 0) ? snap : vm_base + off, w, h, &tl,
                       rsx_texture_argb_is_rgba());
    /* [#175] NHL_TEXDUMP=1: each distinct (offset, size, format) level 0 as uploaded, to
     * texdump\ (DXT as .dds, RGBA as .bmp; first 1500, >= 32x16, after NHL_TEXUSE_AFTER s).
     * File names carry the RESOLVED offset ([TEXUSE] off=). */
    if (_f == 0 && !cube && w >= 32 && h >= 16) {
        static int td = -1; static u32 td_rt = 0; static u32 seen[1536]; static int ns = 0; static u32 td_after = 0; static ULONGLONG td_t0 = 0;
        if (td < 0) { const char* d = getenv("NHL_TEXDUMP"); td = d ? 1 : 0;
                      td_rt = (d && *d && *d != '1') ? (u32)strtoul(d, NULL, 16) : 0x02980000u;   /* scene RT */
                      const char* e = getenv("NHL_TEXUSE_AFTER");
                      td_after = e ? (u32)atoi(e) : 0; td_t0 = GetTickCount64(); }
        if (td && ns < 1500 && s_texdump_draw_rt == td_rt && GetTickCount64() - td_t0 >= (ULONGLONG)td_after * 1000ull) {
            const u32 k = off ^ (w << 20) ^ (h << 8) ^ fmt ^ (tpitch << 3);
            int f = 0; for (int i = 0; i < ns; i++) if (seen[i] == k) { f = 1; break; }
            if (!f) {
                seen[ns++] = k; CreateDirectoryA("texdump", NULL);
                char pn[128];
                if (dxt) {
                    snprintf(pn, sizeof pn, "texdump/tex_%08X_%ux%u_%02X.dds", off, w, h, fmt);
                    FILE* fd = fopen(pn, "wb");
                    if (fd) { u32 hd[32] = {0};
                        hd[0] = 0x20534444u; hd[1] = 124; hd[2] = 0x00081007u; hd[3] = h; hd[4] = w; hd[5] = blkrow * blkrows;
                        hd[19] = 32; hd[20] = 0x4u;
                        hd[21] = (tl.fmt == RSX_TEXFMT_BC1) ? 0x31545844u : (tl.fmt == RSX_TEXFMT_BC2) ? 0x33545844u : 0x35545844u;
                        hd[27] = 0x1000u; fwrite(hd, 4, 32, fd);
                        for (u32 r = 0; r < blkrows; r++) fwrite((const u8*)mapped + (u64)r * pitch, 1, blkrow, fd);
                        fclose(fd); }
                } else if (dxfmt == DXGI_FORMAT_R8G8B8A8_UNORM) {
                    snprintf(pn, sizeof pn, "texdump/tex_%08X_%ux%u_%02X.bmp", off, w, h, fmt);
                    cap_write_bmp(pn, mapped, w, h, pitch, (u32)DXGI_FORMAT_R8G8B8A8_UNORM);
                } else {
                    snprintf(pn, sizeof pn, "texdump/tex_%08X_%ux%u_%02X_rowbytes%u.raw", off, w, h, fmt, tl.dst_row_bytes);
                    FILE* fr = fopen(pn, "wb");
                    if (fr) { for (u32 r = 0; r < h; r++) fwrite((const u8*)mapped + (u64)r * pitch, 1, tl.dst_row_bytes, fr); fclose(fr); }
                }
            }
        }
    }
    /* [#98] TEX_DXTNZ=1: per DXT upload of the first 400 distinct offsets (and
     * every re-upload of them), how many source bytes are non-zero. Is the
     * rink lightmap (0x07065400 / 0x07093400, DXT5 256x256) empty in guest
     * VRAM when we read it? */
    if (dxt && _f == 0 && getenv("TEX_DXTNZ")) {
        static u32 seen[400], ups[400]; static int ns = 0; int k = -1;
        for (int i = 0; i < ns; i++) if (seen[i] == off) k = i;
        if (k < 0 && ns < 400) { k = ns; seen[ns++] = off; ups[k] = 0; }
        if (k >= 0 && ups[k]++ < 6) {
            const u32 nb = blkrow * blkrows; u32 nz = 0;
            for (u32 i2 = 0; i2 < nb; i2++) if (vm_base[off + i2]) nz++;
            fprintf(stderr, "[TEXNZ] off=0x%08X %ux%u fmt=0x%02X up#%u nonzero=%u/%u frame=%llu%c",
                    off, w, h, fmt, ups[k], nz, nb, (unsigned long long)s_d3d.frame_count, 10);
        }
    }
    /* [NHL local patch] TEX_SAVE_DXT=1: write each distinct DXT texture of
     * width >= 256 (first 48) as dxt_<off>_<w>x<h>.dds from the DECODED bytes
     * the GPU receives -- viewable/convertible with ffmpeg. */
    /* [#104] TEX_SAVE_OFF=<hex>[,<hex>...] (raw RSX offsets, up to 8): save only those
     * DXT textures (re-saved on every re-upload, as dxt_<off>_<w>x<h>_<n>.dds). */
    { static int nso = -1; static u32 so[8]; static int cnt[8];
      if (nso < 0) { nso = 0; const char* e = getenv("TEX_SAVE_OFF");
          while (e && *e && nso < 8) { char* end; unsigned long v = strtoul(e, &end, 16); if (end == e) break;
              so[nso] = (u32)v | 0xC0000000u; cnt[nso] = 0; nso++; e = (*end == ',') ? end + 1 : end; } }
      for (int i = 0; dxt && _f == 0 && i < nso; i++) if (off == so[i] && cnt[i] < 4) {
          char pn[128]; snprintf(pn, sizeof pn, "dxt_%08X_%ux%u_%d.dds", off, w, h, cnt[i]++);
          FILE* f = fopen(pn, "wb");
          if (f) { u32 hd[32] = {0};
              hd[0] = 0x20534444u; hd[1] = 124; hd[2] = 0x00081007u; hd[3] = h; hd[4] = w; hd[5] = blkrow * blkrows;
              hd[19] = 32; hd[20] = 0x4u;
              hd[21] = (tl.fmt == RSX_TEXFMT_BC1) ? 0x31545844u : (tl.fmt == RSX_TEXFMT_BC2) ? 0x33545844u : 0x35545844u;
              hd[27] = 0x1000u; fwrite(hd, 4, 32, f);
              for (u32 r = 0; r < blkrows; r++) fwrite(mapped + (u64)r * pitch, 1, blkrow, f);
              fclose(f);
              fprintf(stderr, "[TEXSAVE] wrote %s (frame %llu)%c", pn, (unsigned long long)s_d3d.frame_count, 10); } } }
    if (dxt && _f == 0 && w >= 256 && getenv("TEX_SAVE_DXT")) {
        static u32 seen[48]; static int ns = 0; int known = 0;
        for (int i = 0; i < ns; i++) if (seen[i] == off) known = 1;
        if (!known && ns < 48) { seen[ns++] = off;
            char pn[96]; snprintf(pn, sizeof pn, "dxt_%08X_%ux%u.dds", off, w, h);
            FILE* f = fopen(pn, "wb");
            if (f) {
                u32 hd[32] = {0};
                hd[0] = 0x20534444u;                      /* "DDS " */
                hd[1] = 124; hd[2] = 0x00081007u;          /* caps|h|w|pf|linearsize */
                hd[3] = h; hd[4] = w; hd[5] = blkrow * blkrows;
                hd[19] = 32; hd[20] = 0x4u;                /* pixel format: FOURCC */
                hd[21] = (tl.fmt == RSX_TEXFMT_BC1) ? 0x31545844u
                       : (tl.fmt == RSX_TEXFMT_BC2) ? 0x33545844u : 0x35545844u;
                hd[27] = 0x1000u;                          /* DDSCAPS_TEXTURE */
                fwrite(hd, 4, 32, f);
                for (u32 r = 0; r < blkrows; r++) fwrite(mapped + (u64)r * pitch, 1, blkrow, f);
                fclose(f);
                fprintf(stderr, "[TEXDXT] wrote %s (frame %llu)%c", pn,
                        (unsigned long long)s_d3d.frame_count, 10);
            }
        }
    }

    /* The diagnostics below only apply to the linear 8-bit case (Bink video
     * planes); they used to sit inside that branch of the conversion. */
    if (!tl.compressed && tl.bytes_per_texel == 1 && !tl.swizzled) {
        /* MOVIE_FIND=1: when a 640x360 movie plane uploads ZERO, scan a wide
         * guest window for the REAL frame (a 640-wide region with content) so
         * we learn where the video decode actually wrote vs where the texture
         * points -- ends the frame-buffer-address guessing. */
        if (getenv("MOVIE_FIND") && w == 640 && h == 360) {
            extern uint8_t* vm_base;
            const u8* here = vm_base + off;
            u32 hz = 0; for (u32 i=0;i<w*h;i+=137) if (here[i]) { hz=1; break; }
            static int _mf = 0;
            if (!hz && _mf++ < 3) {
                fprintf(stderr, "[movie-find] bound plane ea=0x%X is ZERO; full scan...\n", off);
                /* Scan FULL VRAM + main heap in 0x8000 steps for a 640x360
                 * content block -- NO early cap (the previous found<6 stopped in
                 * the low-VRAM display buffers before ever reaching the movie
                 * region ~0x40E80000). Report the near-plane hits explicitly so
                 * ring-desync (frame present at a DIFFERENT slot) is separable
                 * from decode-never-ran (region entirely zero). */
                struct { u32 lo, hi; const char* tag; } rng[] = {
                    {0x40000000u, 0x41000000u, "VRAM"}, {0x00100000u, 0x10000000u, "MAIN"},
                    {0x48000000u, 0x4A000000u, "BINK"} };  /* Bink working set (handle ~0x4849E760, SPU I/O 0x4847/0x4945) */
                u32 plane_lo = (off > 0x400000u) ? off - 0x400000u : 0;
                u32 plane_hi = off + 0x400000u;
                int found = 0, nearc = 0;
                for (int r = 0; r < (int)(sizeof(rng)/sizeof(rng[0])); r++) {
                    u32 best_a = 0, best_nz = 0; u32 best_span = 0;
                    for (u32 a = rng[r].lo; a + w*h < rng[r].hi; a += 0x8000) {
                        const u8* p = vm_base + a;
                        u32 nz = 0; u8 mn2 = 255, mx2 = 0;
                        for (u32 i = 0; i < w*h; i += 257) { u8 v = p[i]; if (v) nz++; if (v<mn2) mn2=v; if (v>mx2) mx2=v; }
                        u32 span = mx2 - mn2;
                        if (nz > best_nz) { best_nz = nz; best_a = a; best_span = span; }
                        if (nz > 400 && span > 60) {  /* looks like image content */
                            int isnear = (a >= plane_lo && a <= plane_hi);
                            if (isnear) nearc++;
                            if (found < 40)
                                fprintf(stderr, "[movie-find]   CONTENT @0x%08X (%s) nz=%u span=%u%s\n",
                                        a, rng[r].tag, nz, span, isnear ? " <== NEAR bound plane" : "");
                            found++;
                        }
                    }
                    /* Always report the densest block in this range even below the
                     * content threshold -- the MM intro fades in from black, so the
                     * real first frames are low-span and would otherwise be invisible. */
                    fprintf(stderr, "[movie-find]   %s best @0x%08X nz=%u span=%u%s\n",
                            rng[r].tag, best_a, best_nz, best_span,
                            (best_a >= plane_lo && best_a <= plane_hi) ? " <== NEAR bound plane" : "");
                    /* Dump the densest block as a 640x360 grayscale BMP so the
                     * "is this the decoded movie frame" question is answered by
                     * eye. One dump per range per process. */
                    if (best_nz > 400 && best_span > 40) {
                        static int _bd[4] = {0,0,0,0};
                        if (r < 4 && !_bd[r]) { _bd[r] = 1;
                            const u8* sp = vm_base + best_a;
                            char pn[128];
                            snprintf(pn, sizeof(pn), "binkscan_%s_%08X.bmp", rng[r].tag, best_a);
                            FILE* f = fopen(pn, "wb");
                            if (f) {
                                u32 rowb = w*3, rowp = (rowb+3)&~3u, fsz = 54 + rowp*h;
                                u8 hd[54] = {'B','M'};
                                hd[2]=(u8)fsz; hd[3]=(u8)(fsz>>8); hd[4]=(u8)(fsz>>16); hd[5]=(u8)(fsz>>24);
                                hd[10]=54; hd[14]=40;
                                hd[18]=(u8)w; hd[19]=(u8)(w>>8); hd[22]=(u8)h; hd[23]=(u8)(h>>8);
                                hd[26]=1; hd[28]=24;
                                fwrite(hd,1,54,f);
                                for (int y=(int)h-1; y>=0; y--) {
                                    const u8* srow = sp + (u64)y*w;
                                    for (u32 x=0;x<w;x++){ u8 px[3]; px[0]=px[1]=px[2]=srow[x]; fwrite(px,1,3,f); }
                                    { u8 z[3]={0,0,0}; fwrite(z,1,rowp-rowb,f); }
                                }
                                fclose(f);
                                fprintf(stderr, "[movie-find]   dumped %s\n", pn);
                            }
                        }
                    }
                }
                fprintf(stderr, "[movie-find]   total=%d near-plane=%d (window 0x%X..0x%X)\n",
                        found, nearc, plane_lo, plane_hi);
                if (!found) fprintf(stderr, "[movie-find]   no 640x360 content block (>thresh) found anywhere\n");
            }
        }
        /* TEX_SAVE=1: also dump wide B8 uploads (Bink video planes are B8 --
         * the Y plane IS the movie frame in grayscale) + a content stat, so
         * "is the decoder producing pixels" is answerable by looking at a BMP. */
        if (getenv("TEX_SAVE") && w >= 256) {
            static int _bs = 0;
            const u8* sp = vm_base + off;
            u32 nz = 0; u8 mn = 255, mx = 0;
            for (u32 i = 0; i < w * h; i += 17) {
                u8 v = sp[i]; if (v) nz++; if (v < mn) mn = v; if (v > mx) mx = v;
            }
            fprintf(stderr, "[TEXB8] off=0x%X %ux%u nz=%u/%u min=%u max=%u\n",
                    off, w, h, nz, (w * h) / 17, mn, mx);
            if (_bs < 8 && mx > mn) { _bs++;
                char pn[128];
                snprintf(pn, sizeof(pn), "texb8_%08X_%ux%u_%d.bmp", off, w, h, _bs);
                FILE* f = fopen(pn, "wb");
                if (f) {
                    u32 rowb = w * 3, rowp = (rowb + 3) & ~3u;
                    u32 fsz = 54 + rowp * h;
                    u8 hd[54] = {'B','M'};
                    hd[2]=(u8)fsz; hd[3]=(u8)(fsz>>8); hd[4]=(u8)(fsz>>16); hd[5]=(u8)(fsz>>24);
                    hd[10]=54; hd[14]=40;
                    hd[18]=(u8)w; hd[19]=(u8)(w>>8); hd[22]=(u8)h; hd[23]=(u8)(h>>8);
                    hd[26]=1; hd[28]=24;
                    fwrite(hd, 1, 54, f);
                    for (int y = (int)h - 1; y >= 0; y--) {
                        const u8* srow = sp + (u64)y * w;
                        for (u32 x = 0; x < w; x++) {
                            u8 px[3]; px[0]=px[1]=px[2]=srow[x];
                            fwrite(px, 1, 3, f);
                        }
                        { u8 z[3] = {0,0,0}; fwrite(z, 1, rowp - rowb, f); }
                    }
                    fclose(f);
                    fprintf(stderr, "[TEX_SAVE] %s\n", pn);
                }
            }
        }
    }
    /* TEX_SAVE=1: dump the first few converted ARGB uploads as BMPs (rgb +
     * alpha channel separately) -- ground-truth for "is the guest texture
     * wrong or is the sampling wrong" questions (wave's hue palette). */
    /* [NHL local patch] TEX_SAVE_LN=1: only LINEAR uploads count, up to 32 (the
     * intro's swizzled art spends the 8 otherwise; NHL's runtime-built UI
     * textures -- team logos, menu art -- are the linear ones). */
    static int s_tsln = -1;
    if (s_tsln < 0) s_tsln = getenv("TEX_SAVE_LN") ? 1 : 0;
    if (argb && _f + 1 == _nfaces && getenv("TEX_SAVE") && (!s_tsln || !swz)) { static int _ts = 0; if (_ts < (s_tsln ? 32 : 8)) { _ts++;
        /* For a cube, write every face: the point of the dump is to check the
         * assumed 6-face layout against the game's cubemap art. */
        for (u32 _face = 0; _face < _nfaces; _face++)
        for (int pass = 0; pass < 2; pass++) {
            char pn[128];
            if (_nfaces > 1)
                snprintf(pn, sizeof(pn), "cube_%08X_%ux%u_f%u_%s.bmp", _off0, w, h,
                         _face, pass ? "a" : "rgb");
            else
                snprintf(pn, sizeof(pn), "tex_%08X_%ux%u_%s.bmp", off, w, h,
                     pass ? "a" : "rgb");
            FILE* f = fopen(pn, "wb");
            if (f) {
                u32 rowb = w * 3, rowp = (rowb + 3) & ~3u;
                u32 fsz = 54 + rowp * h;
                u8 hd[54] = {'B','M'};
                hd[2]=(u8)fsz; hd[3]=(u8)(fsz>>8); hd[4]=(u8)(fsz>>16); hd[5]=(u8)(fsz>>24);
                hd[10]=54; hd[14]=40;
                hd[18]=(u8)w; hd[19]=(u8)(w>>8); hd[22]=(u8)h; hd[23]=(u8)(h>>8);
                hd[26]=1; hd[28]=24;
                fwrite(hd, 1, 54, f);
                for (int y = (int)h - 1; y >= 0; y--) {
                    const u8* srow = _map0 + (u64)_face * pitch * _face_rows
                                     + (u64)y * pitch;
                    for (u32 x = 0; x < w; x++) {
                        u8 px[3];
                        if (pass) { px[0]=px[1]=px[2]=srow[x*4+3]; }
                        else { px[0]=srow[x*4+2]; px[1]=srow[x*4+1]; px[2]=srow[x*4+0]; }
                        fwrite(px, 1, 3, f);
                    }
                    { u8 z[3] = {0,0,0}; fwrite(z, 1, rowp - rowb, f); }
                }
                fclose(f);
                fprintf(stderr, "[TEX_SAVE] %s\n", pn);
            }
        }
    } }
    /* TEX_AVGDBG=1: one line per distinct bound texture offset with its average
     * source colour. duck.tga is overwhelmingly yellow (avg ~243,191,23), so this
     * says whether the duck's image is bound at all -- and if it is, the duck is
     * being drawn somewhere and the question is where, not whether. */
    { static int avgd = -1;
      if (avgd < 0) { const char* e = getenv("TEX_AVGDBG"); avgd = e ? atoi(e) : 0; }
      if (avgd && !dxt && bpp == 4) {
        static u32 seen[64]; static int nseen = 0;
        int known = 0;
        for (int i2 = 0; i2 < nseen; i2++) if (seen[i2] == off) { known = 1; break; }
        if (!known && nseen < 64) {
            seen[nseen++] = off;
            /* Report the source as R,G,B,A -- the layout TEX_RGBA uploads (PSGL
             * writes GL_RGBA/UNSIGNED_INT_8_8_8_8, so bytes land R,G,B,A). Alpha
             * matters as much as colour here: a blended surface whose alpha is a
             * flat 255 renders opaque no matter how correct the blend state is. */
            u64 sr = 0, sg = 0, sb = 0, sa = 0; u32 cnt = 0, amin = 255, amax = 0;
            for (u32 i2 = 0; i2 + 3 < w * h * 4u; i2 += 4 * 37) {
                const u8* q = vm_base + off + i2;
                sr += q[0]; sg += q[1]; sb += q[2]; sa += q[3]; cnt++;
                if (q[3] < amin) amin = q[3];
                if (q[3] > amax) amax = q[3];
            }
            if (cnt) fprintf(stderr, "[TEXAVG] raw=0x%08X %4ux%-4u fmt=0x%02X"
                                     " avgRGBA=(%3u,%3u,%3u,%3u) alpha[%u..%u]%c",
                             key_off, w, h, fmt, (u32)(sr/cnt), (u32)(sg/cnt),
                             (u32)(sb/cnt), (u32)(sa/cnt), amin, amax, 10);
        }
      } }
    /* Content-identify the duck's texture so filters can say "duck". */
    if (!dxt && bpp == 4 && w == 512 && h == 512 && s_duck_raw != key_off) {
        u64 ar = 0, ag = 0, ab = 0; u32 n = 0;
        for (u32 i2 = 0; i2 + 3 < w * h * 4u; i2 += 4 * 211) {
            const u8* q = vm_base + off + i2;
            ar += q[0]; ag += q[1]; ab += q[2]; n++;
        }
        if (n && ar/n > 200 && ag/n > 140 && ab/n < 90) {
            s_duck_off = key_off;
            { static u32 last = 0; if (last != key_off) { last = key_off;
                fprintf(stderr, "[DUCKTEX] resolved=0x%08X identified by content%c", key_off, 10); } }
        }
    }
    /* TEX_HILITE=1: paint the duck's texture pure red. duck.tga is overwhelmingly
     * yellow (avg 243,191,23); with the R,G,B,A source order that is bytes
     * s[0] high, s[1] mid, s[2] low. Flooding it with an unmistakable colour
     * answers "where on screen is the duck drawn" directly, which neither the
     * vertex data nor a pixel count can. Diagnostic only. */
    { static int hil = -1;
      if (hil < 0) { const char* e = getenv("TEX_HILITE"); hil = e ? atoi(e) : 0; }
      if (hil && !dxt && bpp == 4 && w == 512 && h == 512) {
        u64 sr = 0, sg = 0, sb = 0; u32 cnt = 0;
        for (u32 i2 = 0; i2 + 3 < w * h * 4u; i2 += 4 * 37) {
            const u8* q = vm_base + off + i2;
            sr += q[0]; sg += q[1]; sb += q[2]; cnt++;
        }
        if (cnt) {
            u32 ar = (u32)(sr/cnt), ag = (u32)(sg/cnt), ab = (u32)(sb/cnt);
            if (ar > 200 && ag > 140 && ab < 90) {
                for (u32 y = 0; y < h; y++) {
                    u8* drow = (u8*)mapped + (u64)y * pitch;
                    for (u32 x = 0; x < w; x++) {
                        drow[x*4+0] = 0xFF; drow[x*4+1] = 0x00;
                        drow[x*4+2] = 0x00; drow[x*4+3] = 0xFF;
                    }
                }
                { static int _n = 0; if (_n++ < 4)
                    fprintf(stderr, "[TEXHILITE] duck texture off=0x%08X avg=(%u,%u,%u) -> red%c",
                            off, ar, ag, ab, 10); }
            }
        }
      } }
    /* TEX_MARKEMPTY=1: paint any texture whose guest source is entirely zero a
     * flat magenta instead of uploading the empty bytes. An unresolved texture
     * and a legitimately black surface are indistinguishable on screen, so this
     * says WHICH geometry is missing its image -- diagnostic only. */
    { static int mark = -1;
      if (mark < 0) { const char* e = getenv("TEX_MARKEMPTY"); mark = e ? atoi(e) : 0; }
      if (mark && !dxt) {
        u32 nz = 0, sz = w * h * bpp;
        for (u32 i2 = 0; i2 < sz && i2 < 0x40000u; i2 += 61) if (vm_base[off + i2]) nz++;
        if (!nz) {
            for (u32 y = 0; y < h; y++) {
                u8* drow = (u8*)mapped + (u64)y * pitch;
                for (u32 x = 0; x < w; x++) {
                    /* Pure RED. Magenta collided with this title's wall tint,
                     * so the marker was indistinguishable from real geometry. */
                    drow[x*4+0] = 0xFF; drow[x*4+1] = 0x00;
                    drow[x*4+2] = 0x00; drow[x*4+3] = 0xFF;
                }
            }
            { static int _n = 0; if (_n++ < 8)
                fprintf(stderr, "[TEXEMPTY] off=0x%08X %ux%u fmt=0x%02X -> magenta%c",
                        off, w, h, fmt, 10); }
        }
      } }
    }   /* end per-face conversion */
    off = _off0; mapped = _map0;
    /* [#119] mip levels 1.. (level 0 was converted above at offset 0) */
    for (u32 _f = 0; _f < (face_stage ? _nfaces : 1u); _f++)   /* #215: every cube face */
    for (u32 l = 1; l < nl; l++) {
        rsx_tex_layout tll; rsx_texture_layout(fmt, lvl_w[l], lvl_h[l], &tll);
        rsx_texture_decode(_map0 + (u64)_f * face_stage + lvl_off[l], lvl_pitch[l],
                           vm_base + _off0 + _f * _face_bytes + lvl_src[l],
                           lvl_w[l], lvl_h[l], &tll, rsx_texture_argb_is_rgba());
    }
    if (ro < 0) t->up->lpVtbl->Unmap(t->up, 0, NULL);   /* #219: the ring stays mapped */
    HITCH_ADD(g_hitch_texup_decode_us, ps3_qpc_us() - _dec0);
    const u64 src_base = ro >= 0 ? (u64)ro : 0u;

    if (!fresh) {   /* reused resource: PSR -> COPY_DEST first */
        D3D12_RESOURCE_BARRIER b = {0};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = t->res;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        b.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &b);
    }
    D3D12_TEXTURE_COPY_LOCATION dst = {0}, src = {0};
    dst.pResource = t->res; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.pResource = ro >= 0 ? s_upr : t->up;  src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Footprint.Format   = dxfmt;
    src.PlacedFootprint.Footprint.Width    = w;
    src.PlacedFootprint.Footprint.Height   = h;
    src.PlacedFootprint.Footprint.Depth    = 1;
    src.PlacedFootprint.Footprint.RowPitch = pitch;
    if (nl > 1) {   /* [#119] one copy per mip level; #215: per cube face (subresource = level + face * levels) */
        for (u32 _f = 0; _f < (face_stage ? _nfaces : 1u); _f++)
        for (u32 l = 0; l < nl; l++) {
            dst.SubresourceIndex = l + _f * nl;
            src.PlacedFootprint.Footprint.Width    = lvl_w[l];
            src.PlacedFootprint.Footprint.Height   = lvl_h[l];
            src.PlacedFootprint.Footprint.RowPitch = lvl_pitch[l];
            src.PlacedFootprint.Offset             = src_base + (u64)_f * face_stage + lvl_off[l];
            s_d3d.cmd_list->lpVtbl->CopyTextureRegion(s_d3d.cmd_list, &dst, 0, 0, 0, &src, NULL);
        }
    } else
    for (u32 _f = 0; _f < _nfaces; _f++) {
        dst.SubresourceIndex        = _f;      /* cube face = array slice */
        src.PlacedFootprint.Offset  = src_base + (u64)_f * pitch * _face_rows;
        s_d3d.cmd_list->lpVtbl->CopyTextureRegion(s_d3d.cmd_list, &dst, 0, 0, 0, &src, NULL);
    }
    {
        D3D12_RESOURCE_BARRIER b = {0};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = t->res;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &b);
    }
    /* SRV at heap slot 1+slot */
    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {0};
    sv.Format = dxfmt;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    /* RSX B8 replicates the byte into all four channels (dbgfont's FP reads
     * coverage from .w); DXGI R8 defaults to (r,0,0,1), so swizzle (R,R,R,R)
     * = encoded 0x1000 (component 0 in all lanes + always-set bit). ARGB8
     * keeps the identity mapping. */
    sv.Shader4ComponentMapping = (basef == 0x81) ? 0x1000 : D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = nl;   /* #119 */
    D3D12_CPU_DESCRIPTOR_HANDLE sh;
    s_d3d.srv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.srv_heap, &sh);
    sh.ptr += (u64)(SLOT_SRV_BASE + slot) * s_d3d.srv_inc;   /* #116: was 1 + slot */
    if (cube) {
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        sv.TextureCube.MipLevels = nl;   /* #215 */
        sv.TextureCube.MostDetailedMip = 0;
        sv.TextureCube.ResourceMinLODClamp = 0.0f;
    }
    s_d3d.device->lpVtbl->CreateShaderResourceView(s_d3d.device, t->res, &sv, sh);

    t->off = off; t->key = key_off; t->w = w; t->h = h; t->fmt = fmt;
    t->cube = cube; t->used = 1; t->nl = nl;   /* #119 */
    t->last_frame = (u32)s_d3d.frame_count;   /* #136 */
    t->pitch = tpitch;                        /* #173 */
    { u32 nb = tpitch ? tpitch * (dxt ? blkrows : h) : dxt ? (blkrow * blkrows) : (w * h * bpp);
      /* [NHL local patch #239] The checksums covered level 0 only, so mips written
       * after the first upload (or changed while level 0 stayed equal) were never
       * re-uploaded: the Flyers home jersey's 1024x1024 DXT1 kept stale lower levels
       * -> salmon body / grey sleeves at playing distance, right with TEX_NO_MIPS=1
       * (2026-10-28). Now they cover every level uploaded. Movie snapshots (#179)
       * hold level 0 only and keep the old length. NHL_TEX_HASH_MIPS=0 = old. */
      { static int hm = -1; if (hm < 0) { const char* e = getenv("NHL_TEX_HASH_MIPS"); hm = (e && *e == '0') ? 0 : 1; }
        if (hm && !snap && chain_src > nb && chain_src < 0x10000000ull) nb = (u32)chain_src; }
      t->hbytes = nb;
      /* #136: a stale re-upload already hashed these same bytes (taken before
       * the decode, so a write racing it shows up as a change next frame). */
      t->csum = (have_prehash && off == key_off && prehash_nb == nb) ? prehash : TEX_CSUM(snap ? snap : vm_base + off, nb);
      t->scsum = nb > 32768u ? tex_csum_sampled(snap ? snap : vm_base + off, nb) : 0;   /* #134 */
      t->ok_frame = (u32)s_d3d.frame_count; t->stable = 0;
      s_tex_hint[hk] = (short)(slot + 1); }
    #undef TEX_CSUM
    /* [NHL local patch] TEX_UPLOG=1: every upload of a LINEAR A8R8G8B8 texture,
     * with how much of the guest image is pure-white opaque and its first two
     * texels -- shows whether a texture that renders white was uploaded white
     * (guest memory overwritten) or uploaded right and sampled wrong. */
    { static int ul = -1; if (ul < 0) ul = getenv("TEX_UPLOG") ? 1 : 0;
      if (ul && (fmt & 0x9F) == 0x85 && !swz) {
          static unsigned n = 0;
          if (n++ < 400) {
              const u8* g = vm_base + off; u32 white = 0, tot = 0;
              for (u32 i = 0; i + 3 < w * h * 4u; i += 4u * 7u) { tot++; if (g[i] == 0xFF && g[i+1] == 0xFF && g[i+2] == 0xFF && g[i+3] == 0xFF) white++; }
              fprintf(stderr, "[TEXUP] frame=%llu slot=%d off=0x%08X %ux%u fmt=0x%02X white=%u%% px0=%02X%02X%02X%02X px1=%02X%02X%02X%02X csum=%08X\n",
                      (unsigned long long)s_d3d.frame_count, slot, off, w, h, fmt, tot ? white * 100u / tot : 0u,
                      g[0], g[1], g[2], g[3], g[4], g[5], g[6], g[7], t->csum);
          } } }
    return slot;
}

/* ---------------------------------------------------------------------------
 * Render-to-texture: offscreen RT pool.
 *
 * Draws/clears targeting a non-display surface (SET_SURFACE_COLOR_xOFFSET
 * not registered via cellGcmSetDisplayBuffer) render into a pooled RGBA8
 * texture keyed by the surface's resolved vm offset; a later draw binding a
 * texture at that offset samples the RT directly instead of guest memory.
 * RGBA8 for every RT (even half-float guest surfaces) keeps the existing
 * PSO/RTV formats -- values clamp to [0,1], which demosaic's RGB data fits.
 * -----------------------------------------------------------------------*/

/* Snapshot the VP constant bank + viewport epilogue for one draw into its
 * vp_cb slot. Runs at record time so every draw keeps the constants that
 * were live when the guest issued it. */
static void vp_record_cb(u32 slot, int vs_idx, const D3D12DrawRecord* dr)
{
    wb_note_draw(s_d3d.current_rsx_state, dr);   /* #169 */
    const rsx_state* st = s_d3d.current_rsx_state;
    if (!s_d3d.vp_cb_mapped || !st || slot >= MAX_DRAWS) return;
    /* [#117 diag] TEXRACE_VS=<hex vs>[,...]: for draws of those vertex programs,
     * checksum units 0-3 NOW (record time); the render pre-pass compares and
     * logs [TEXRACE] when guest texture memory changed in between -- i.e. the
     * game reused a texture after our early fence said the RSX was done. */
    { static int nv = -1; static u32 vs_l[8];
      if (nv < 0) { nv = 0; const char* e = getenv("TEXRACE_VS");
          while (e && *e && nv < 8) { char* end; unsigned long v = strtoul(e, &end, 16);
              if (end == e) break; vs_l[nv++] = (u32)v; e = (*end == ',') ? end + 1 : end; } }
      D3D12DrawRecord* w = (D3D12DrawRecord*)dr; w->rec_csum_on = 0;
      if (nv > 0 && vs_idx >= 0 && vs_idx < s_d3d.vp_vs_n) {
          const u32 vh = s_d3d.vp_vs[vs_idx].hash; int hit = 0;
          for (int k = 0; k < nv; k++) if (vs_l[k] == vh) hit = 1;
          extern uint8_t* vm_base;
          if (hit && vm_base) { w->rec_csum_on = 1;
              for (int u = 0; u < 4; u++) {
                  w->rec_csum[u] = 0;
                  if (!dr->tex[u].set || !dr->tex[u].off || !dr->tex[u].w || !dr->tex[u].h) continue;
                  rsx_tex_layout tl; rsx_texture_layout(dr->tex[u].fmt, dr->tex[u].w, dr->tex[u].h, &tl);
                  const u32 nb = tl.compressed ? tl.row_bytes * tl.rows : dr->tex[u].w * dr->tex[u].h * tl.bytes_per_texel;
                  w->rec_csum[u] = tex_csum(vm_base + dr->tex[u].off, nb);
              } } } }
    /* FP texcoord scale (b1): 1/size for UNnormalized textures (fmt bit
     * 0x40 -- wave samples everything in texel space), 1.0 otherwise. */
    if (s_d3d.vp_fpcb_mapped) {
        float* ts = (float*)((char*)s_d3d.vp_fpcb_mapped
            + ((u64)s_d3d.vp_parity * MAX_DRAWS + slot) * VP_FPCB_STRIDE);
        for (int _u = 0; _u < 4; _u++) {
            float sx = 1.0f, sy = 1.0f;
            if (dr && dr->tex[_u].set && (dr->tex[_u].fmt & 0x40) &&
                dr->tex[_u].w && dr->tex[_u].h) {
                sx = 1.0f / (float)dr->tex[_u].w;
                sy = 1.0f / (float)dr->tex[_u].h;
            }
            ts[_u*4+0] = sx; ts[_u*4+1] = sy; ts[_u*4+2] = 0.0f; ts[_u*4+3] = 0.0f;
        }
        /* rsx_alphatest (b1[4]): x=enable, y=ref/255, z=func-0x200. D3D12
         * has no fixed alpha test; guest FPs discard on it dynamically
         * (wave's colour wheel is a disc via alpha test -- without it the
         * palette drew as an opaque square). */
        static int s_no_at = -1;   /* #187: was a getenv per draw */
        if (s_no_at < 0) s_no_at = getenv("NO_ALPHATEST") ? 1 : 0;
        if (dr && !s_no_at) {
            { static int _at = 0;
              if (((dr->alpha_ctl >> 16) & 1u) && _at++ < 4)
                  printf("[ALPHATEST] enable func=%u ref=%u\n",
                         (dr->alpha_ctl >> 8) & 0xFFu, dr->alpha_ctl & 0xFFu); }
            ts[16] = (float)((dr->alpha_ctl >> 16) & 1u);
            ts[17] = (float)(dr->alpha_ctl & 0xFFu) / 255.0f;
            ts[18] = (float)((dr->alpha_ctl >> 8) & 0x07u);
            ts[19] = 0.0f;
        } else {
            ts[16] = 0.0f; ts[17] = 0.0f; ts[18] = 7.0f; ts[19] = 0.0f;
        }

        /* fp_k[]: the program's inline fragment constants, re-read from the
         * guest ucode every draw. The decompiler emits fp_k[i] lookups instead
         * of baking these in as literals, so the compiled shader no longer
         * changes when the title re-patches them -- which is what forced the
         * pipeline cache to key on a hash of the ucode bytes. */
        if (dr && dr->fp_addr) {
            extern uint8_t* vm_base;
            extern u32 cellGcmResolveLocated(int, u32);
            u32 foff = cellGcmResolveLocated((dr->fp_addr & 0x3u) == 1,
                                             dr->fp_addr & ~0x3u);
            /* #127: the record-time snapshot when there is one (the bytes the
             * shader was decompiled from), and programs longer than 4 KB */
            if (dr->fp_snap && dr->fp_snap_len) {
                /* [NHL local patch #187] A snapshot never changes until the arena is
                 * reused (s_fp_arena_gen), so its constants are extracted once and
                 * copied after that: the crowd's ~10k draws per frame share a few
                 * programs and re-walked them every draw. NHL_FPK_MEMO=0 = walk always. */
                static int km = -1;
                if (km < 0) { const char* e = getenv("NHL_FPK_MEMO"); km = (e && *e == '0') ? 0 : 1; }
                typedef struct { const u8* p; u32 len, gen; int n; float k[256 * 4]; } FpkMemo;
                static FpkMemo* s_fpk; static unsigned s_fpk_n;
                if (km && !s_fpk) s_fpk = (FpkMemo*)calloc(8, sizeof(FpkMemo));
                FpkMemo* hit = NULL;
                if (km && s_fpk)
                    for (int i = 0; i < 8; i++)
                        if (s_fpk[i].p == dr->fp_snap && s_fpk[i].len == dr->fp_snap_len &&
                            s_fpk[i].gen == s_fp_arena_gen) { hit = &s_fpk[i]; break; }
                if (hit) memcpy(&ts[20], hit->k, (size_t)hit->n * 16u);
                else {
                    const int n = rsx_fp_extract_consts(dr->fp_snap, dr->fp_snap_len, &ts[20], 256);   /* fp_k[256] (#83) */
                    if (km && s_fpk) {
                        FpkMemo* m = &s_fpk[s_fpk_n++ & 7u];
                        m->p = dr->fp_snap; m->len = dr->fp_snap_len; m->gen = s_fp_arena_gen; m->n = n;
                        memcpy(m->k, &ts[20], (size_t)n * 16u);
                    }
                }
            }
            else if (vm_base && foff != 0xFFFFFFFFu)
                rsx_fp_extract_consts(vm_base + foff, FP_MAX_BYTES, &ts[20], 256);
            /* [#92] FPK_RT=<hex rt>: fp_k[0..11] of draws into that target, a
             * batch of 150 every FPK_EVERY s (default 30). Is the scene's light
             * colour (an inline FP constant) zero in the match? */
            { static u32 krt = 0xFFFFFFFFu; static int kevery = 30, kn = 0; static ULONGLONG klast = 0;
              if (krt == 0xFFFFFFFFu) { const char* e = getenv("FPK_RT");
                                        /* [#170] FPK_RT=comp: every draw into a one-off composite (#169) */
                                        krt = e ? (!strcmp(e, "comp") ? 1u : (u32)strtoul(e, NULL, 16)) : 0u;
                                        e = getenv("FPK_EVERY"); if (e) kevery = atoi(e); }
              int kmatch = krt && dr->rt_off == krt;
              if (krt == 1u && dr->rt_off) { const WBEnt* we = wb_ent(dr->rt_off, 0);
                  kmatch = we && wb_popcount64(we->hist) <= 12; }
              if (kmatch && krt == 1u) {   /* [#170] FIFO context of the first composite draws */
                  static int nd = 0; static u32 last_rt = 0;
                  if (nd < 8 && dr->rt_off != last_rt) { nd++; last_rt = dr->rt_off;
                      extern void gcm_evring_dump(const char*, u32); char tg[64];
                      snprintf(tg, sizeof tg, "composite rt=0x%08X fp=0x%08X", dr->rt_off, dr->fp_addr);
                      gcm_evring_dump(tg, 200); } }
              if (kmatch) {
                  const ULONGLONG t = GetTickCount64();
                  if (t - klast >= (ULONGLONG)kevery * 1000u) { klast = t; kn = 0;
                      fprintf(stderr, "[FPKRT] --- batch t=%.1f%c", t / 1000.0, 10); }
                  if (kn < (krt == 1u ? 3000 : 150)) { kn++;
                      char b[1600]; int p = snprintf(b, sizeof b, "[FPKRT] rt=0x%08X fp=0x%08X vs=%d n=%u t0=%08X t1=%08X t2=%08X t3=%08X:",
                                                      dr->rt_off, dr->fp_addr, vs_idx, dr->vertex_count, dr->tex[0].set ? dr->tex[0].raw : 0,
                                                      dr->tex[1].set ? dr->tex[1].raw : 0, dr->tex[2].set ? dr->tex[2].raw : 0, dr->tex[3].set ? dr->tex[3].raw : 0);
                      for (int k = 0; k < 12 && p < (int)sizeof b - 64; k++)
                          p += snprintf(b + p, sizeof b - p, " k%d=(%.3g,%.3g,%.3g,%.3g)", k,
                                        ts[20 + k*4], ts[21 + k*4], ts[22 + k*4], ts[23 + k*4]);
                      fprintf(stderr, "%s%c", b, 10); } } }
            /* FPK_DBG=1: the fragment constants as the shader will see them.
             * A program whose only output is fp_k[0].xxxx renders black if
             * those constants never arrive, which looks identical from the
             * outside to geometry that never rasterised. */
            { static int fk = -1; if (fk < 0) fk = getenv("FPK_DBG") ? 1 : 0;
              static int n = 0;
              if (fk && n < 12 && dr->fp_addr != 0x00EC2B02u) { n++;
                  fprintf(stderr, "[FPK] tex0=0x%08X fp=0x%08X k0=(%g %g %g %g)"
                                  " k1=(%g %g %g %g)%c",
                          dr->tex[0].raw, dr->fp_addr,
                          ts[20], ts[21], ts[22], ts[23],
                          ts[24], ts[25], ts[26], ts[27], 10);
                  const u32 raw = dr->fp_addr & ~0x3u;
                  const u32 ea_main  = cellGcmResolveLocated(0, raw);
                  const u32 ea_local = cellGcmResolveLocated(1, raw);
                  fprintf(stderr, "[FPK]   ucode main@0x%08X:", ea_main);
                  for (int b = 0; b < 48; b++) fprintf(stderr, "%s%02X", (b%16)?" ":"  |", vm_base[ea_main + b]);
                  fputc(10, stderr); }
              /* distinct fragment programs seen -- if every draw shares one,
               * SET_SHADER_PROGRAM is not being tracked per draw. */
              if (fk) { static u32 seen[16]; static int ns = 0; int f = 0;
                  for (int q = 0; q < ns; q++) if (seen[q] == dr->fp_addr) f = 1;
                  if (!f && ns < 16) { seen[ns++] = dr->fp_addr;
                      fprintf(stderr, "[FPK] distinct fp #%d = 0x%08X%c", ns, dr->fp_addr, 10); } } }
        }
    }
    char* dst = (char*)s_d3d.vp_cb_mapped
        + ((u64)s_d3d.vp_parity * MAX_DRAWS + slot) * VP_CB_STRIDE;
    memcpy(dst, st->vertex_constants, RSX_MAX_VERTEX_CONSTANTS * 16);
    /* VP_MVP=<N>: the constant bank as the shader will see it, for the first N
     * draws. A snapshot taken from a stale rsx_state looks exactly like a broken
     * vertex program from the outside -- both give zero fragments. */
    { static int cap = -1, seen = 0;
      if (cap < 0) { const char* e = getenv("VP_MVP"); cap = e ? atoi(e) : 0; }
      if (cap && seen < cap) { seen++;
        const float* c = (const float*)dst;
        int last_nz = -1;
        for (int i = 0; i < RSX_MAX_VERTEX_CONSTANTS * 4; i++)
            if (c[i] > 1e-9f || c[i] < -1e-9f) last_nz = i / 4;
        fprintf(stderr, "[VPMVP] slot=%u lastNZ=c%d  c260=(%g %g %g %g) c467=(%g %g %g %g)\n",
                slot, last_nz,
                c[260*4+0], c[260*4+1], c[260*4+2], c[260*4+3],
                c[467*4+0], c[467*4+1], c[467*4+2], c[467*4+3]); } }
    /* RSX_DBG_CB=<hex tex0 offset>: the constants as they land in the per-draw
     * constant buffer for that texture's draws -- i.e. exactly what the shader
     * samples, not what the CPU-side rsx_state holds. Those two agreeing is an
     * assumption worth checking directly when a draw transforms to nothing. */
    const float* vpx_dbg = (const float*)(dst + RSX_MAX_VERTEX_CONSTANTS * 16);
    { static const char* dc = (const char*)1; static u32 wantc = 0; static int n = 0;
      if (dc == (const char*)1) { dc = getenv("RSX_DBG_CB");
                                  wantc = dc ? (u32)strtoul(dc, NULL, 16) : 0; }
      if (wantc && dr && dr->tex[0].raw == wantc && n < 4) { n++;
          const float* c = (const float*)dst;
          fprintf(stderr, "[DUCKCB] slot=%u vs_idx=%d posscale=(%g %g %g) posoffset=(%g %g %g)%c",
                  slot, vs_idx, vpx_dbg[0], vpx_dbg[1], vpx_dbg[2],
                  vpx_dbg[4], vpx_dbg[5], vpx_dbg[6], 10);
          for (int r = 256; r <= 259; r++)
              fprintf(stderr, "[DUCKCB]   c%d=(%g %g %g %g)%c", r,
                      c[r*4+0], c[r*4+1], c[r*4+2], c[r*4+3], 10);
      } }

    /* Viewport epilogue (see the render_frame notes this logic came from):
     * x/y identity, z lane remaps GL clip z when the guest programs one. */
    float* vpx = (float*)(dst + RSX_MAX_VERTEX_CONSTANTS * 16);
    const float* vs_ = st->viewport_scale;
    const float* vo_ = st->viewport_offset;
    vpx[0] = vpx[1] = vpx[3] = 1.0f;
    vpx[4] = vpx[5] = vpx[7] = 0.0f;
    if (vs_[2] != 0.0f) { vpx[2] = vs_[2]; vpx[6] = vo_[2]; }
    else                { vpx[2] = 1.0f;   vpx[6] = 0.0f;   }
    /* DBG_ZOOM=<k> with DBG_CENTER="cx,cy": a debug camera applied in CLIP space,
     * not a framebuffer crop -- the geometry is re-rasterized at full resolution.
     * The shader epilogue already computes
     *     pos.xyz = _p.xyz * vp_posscale + _p.w * vp_posoffset
     * so recentring NDC point c and magnifying by k is exactly
     *     pos.xy = _p.xy * k + _p.w * (-c * k).
     * Rubber Ducky frames its ducks near the bottom edge of the viewport and its
     * camera only moves from the analog sticks, so without this there is no way
     * to look at the subject of the demo. It changes the view, nothing else:
     * same draws, same transform, same textures. */
    { static int z = -1; static float k = 1.0f, cx = 0.0f, cy = 0.0f;
      if (z < 0) { const char* e = getenv("DBG_ZOOM");
                   k = e ? (float)atof(e) : 1.0f;
                   z = (e && k > 0.0f) ? 1 : 0;
                   const char* ce = getenv("DBG_CENTER");
                   if (ce) { double a = 0, b = 0; sscanf(ce, "%lf,%lf", &a, &b);
                             cx = (float)a; cy = (float)b; } }
      if (z) { float ux = cx, uy = cy;
               static int lock = -1;
               if (lock < 0) { const char* le = getenv("DBG_LOCK"); lock = le ? atoi(le) : 0; }
               if (lock && s_lock_valid) { ux = s_lock_x; uy = s_lock_y; }
               vpx[0] = k; vpx[1] = k; vpx[4] = -ux * k; vpx[5] = -uy * k; } }

    /* Garbage-projection fallback (vkcube; see the original comment). */
    int uses_c03 = (vs_idx >= 0 && vs_idx < s_d3d.vp_vs_n)
                       ? s_d3d.vp_vs[vs_idx].uses_c03 : s_d3d.vp_uses_c03;
    float* c = (float*)dst;
    float c00 = c[0];
    int garbage = !(c00 > 0.0f && c00 < 8.0f);
    static int s_nofix = -1, s_fix = -1;   /* [#228] were two getenv calls per draw */
    if (s_nofix < 0) { s_nofix = getenv("VP_NOFIXPROJ") ? 1 : 0; s_fix = getenv("VP_FIXPROJ") ? 1 : 0; }
    if (s_nofix) garbage = 0;
    if ((garbage && uses_c03) || s_fix) {
        { static int _fb = 0; if (_fb++ < 6)
            printf("[VPFB] fallback proj on draw slot %u (c00=%g vs_idx=%d)\n",
                   slot, c00, vs_idx); }
        for (int _i = 0; _i < 16; _i++) c[_i] = 0.0f;
        c[0]=1.358f; c[5]=2.414f; c[11]=1.0f;
        c[10]=1.0f/99.0f; c[14]=-1.0f/99.0f;
        vpx[2] = 1.0f; vpx[6] = 0.0f;
    }
}

/* Which offscreen surface (if any) does the current RSX state render to?
 * Returns 0 for a display buffer (backbuffer), else the surface's RAW RSX
 * offset. RTs are keyed by raw offset -- a texture bound at the same raw
 * offset is the same buffer (surface and texture registers share the offset
 * space; location bits differ but one title doesn't alias local vs main at
 * one offset), which sidesteps guessing the surface's context DMA location. */
static u32 current_rt_off(u32* out_w, u32* out_h, u32 out_mrt[3])
{
    extern int cellGcmOffsetIsDisplay(u32 offset);
    const rsx_state* st = s_d3d.current_rsx_state;
    if (out_w) *out_w = 0;
    if (out_h) *out_h = 0;
    if (out_mrt) out_mrt[0] = out_mrt[1] = out_mrt[2] = 0;
    if (!st) return 0;
    /* SET_SURFACE_COLOR_TARGET: 1 = A, 2 = B, 0x13 = MRT1 (A+B),
     * 0x17 = MRT2 (A+B+C), 0x1F = MRT3 (A+B+C+D). */
    int sel = (st->color_target == 2) ? 1 : 0;
    u32 raw = st->surface_color_offset[sel];
    if (out_mrt) {
        if (st->color_target >= 0x13) out_mrt[0] = st->surface_color_offset[1];
        if (st->color_target >= 0x17) out_mrt[1] = st->surface_color_offset[2];
        if (st->color_target >= 0x1F) out_mrt[2] = st->surface_color_offset[3];
    }
    { static int _rs = -1; if (_rs < 0) _rs = getenv("RT_SEQDBG") ? 1 : 0;
      static u32 _last = 0; static int _n2 = 0;
      if (_rs && st->surface_color_offset[0] != _last && _n2 < 400000) {
          _last = st->surface_color_offset[0]; _n2++;
          fprintf(stderr, "[RTSEQ] #%d -> 0x%X%c", _n2, _last, 10); } }
    { static int _ro = -1; if (_ro < 0) _ro = getenv("RT_OFFDBG") ? 1 : 0;
      static u32 _seen[32]; static int _ns = 0;
      if (_ro) { u32 _k = st->surface_color_offset[0] ^ (st->color_target << 24)
                        ^ (st->surface_clip_w << 8) ^ st->surface_format;
        int _f = 0; for (int _i = 0; _i < _ns; _i++) if (_seen[_i] == _k) _f = 1;
        if (!_f && _ns < 32) { _seen[_ns++] = _k;
        fprintf(stderr, "[RTOFF] fmt=0x%X tgt=0x%X off[0]=0x%X off[1]=0x%X zeta=0x%X clip=%ux%u disp=%d -> 0x%X\n",
                st->surface_format, st->color_target, st->surface_color_offset[0], st->surface_color_offset[1],
                st->surface_zeta_offset, st->surface_clip_w, st->surface_clip_h,
                cellGcmOffsetIsDisplay(raw), cellGcmOffsetIsDisplay(raw) ? 0 : raw); } } }
    if (cellGcmOffsetIsDisplay(raw)) return 0;
    /* RT_DISPLAY_BY_SIZE=1: also treat a single-target surface whose clip
     * exactly matches the display resolution as the backbuffer.
     *
     * cellGcmSetDisplayBuffer only registers the buffers the FLIP may point at;
     * a guest is free to render into a different surface of the same size and
     * flip to it later (PSGL does). Matching offsets alone then classifies every
     * draw as offscreen, has_display stays 0, and render_frame() -- which is
     * what draws ALL recorded geometry -- is never called. The backbuffer shows
     * only the clear, which looks exactly like "nothing rasterizes". */
    { static int _bs = -1; if (_bs < 0) _bs = getenv("RT_DISPLAY_BY_SIZE") ? 1 : 0;
      if (_bs && st->color_target < 0x13 &&
          st->surface_clip_w == s_d3d.width && st->surface_clip_h == s_d3d.height)
          return 0; }
    /* Surface clip dims when sane; else the window size. Any size works --
     * passes draw normalized full-surface quads -- this only picks resolution. */
    u32 w = st->surface_clip_w, h = st->surface_clip_h;
    if (w < 16 || w > 2048 || h < 16 || h > 2048) { w = 0; h = 0; }
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
    return raw;
}

/* RSX surface colour format (SET_SURFACE_FORMAT bits [4:0]) -> DXGI. Float
 * targets matter: wave's water height maps store SIGNED values (F_W16..),
 * demosaic's differential planes likewise -- RGBA8 clamps them to zero. */
static DXGI_FORMAT rsx_surface_dxgi(u32 fmt)
{
    switch (fmt & 0x1F) {
    case 0x0B: return DXGI_FORMAT_R16G16B16A16_FLOAT; /* F_W16Z16Y16X16 */
    case 0x0C: return DXGI_FORMAT_R32G32B32A32_FLOAT; /* F_W32Z32Y32X32 */
    case 0x0D: return DXGI_FORMAT_R32_FLOAT;          /* F_X32          */
    default:   return DXGI_FORMAT_R8G8B8A8_UNORM;
    }
}

/* [#177] off_rt_find runs for every bound texture unit of every executed draw; most are
 * not RTs. Same scheme as wb_ent: a seen-bitmap (set in off_rt_get, never cleared) makes
 * a miss one bit test, a per-bucket hint makes a hit one probe. */
static u32 s_ort_seen[1024 / 32]; static s8 s_ort_hint[256];
static int off_rt_find(u32 off)
{
    if (!off) return -1;
    const u32 hb = wb_hash(off);
    if (!(s_ort_seen[hb >> 5] & (1u << (hb & 31u)))) return -1;
    const int hi = s_ort_hint[hb & 255u] - 1;
    if (hi >= 0 && s_d3d.off_rt[hi].res && s_d3d.off_rt[hi].off == off) return hi;
    for (int i = 0; i < MAX_OFF_RTS; i++)
        if (s_d3d.off_rt[i].res && s_d3d.off_rt[i].off == off) { s_ort_hint[hb & 255u] = (s8)(i + 1); return i; }
    return -1;
}

/* Ensure an RT resource exists for this surface; (re)creates the RTV at
 * rt_rtv_heap[i] and the SRV at srv_heap[RT_SRV_BASE+i]. */
static int off_rt_get(u32 off, u32 w, u32 h, u32 rsx_fmt)
{
    if (!off || !s_d3d.rt_rtv_heap || !s_d3d.srv_heap) return -1;
    if (!w) w = s_d3d.width;
    if (!h) h = s_d3d.height;
    DXGI_FORMAT want_fmt = rsx_surface_dxgi(rsx_fmt);
    /* RT_FORCE_RGBA8=1: allocate every offscreen target as R8G8B8A8 rather than
     * its native format. A title whose final composite reads a half-float
     * surface cannot be shown with a plain CopyTextureRegion, and giving it an
     * 8-bit target costs HDR precision but makes the composite a same-format
     * copy. A stopgap for looking at the frame, not a rendering fix. */
    { static int f8 = -1;
      if (f8 < 0) { const char* e = getenv("RT_FORCE_RGBA8"); f8 = e ? atoi(e) : 0; }
      if (f8) want_fmt = DXGI_FORMAT_R8G8B8A8_UNORM; }
    extern unsigned cellGcm_flip_request_count(void);
    const u32 flip_now = cellGcm_flip_request_count();
    int slot = off_rt_find(off);
    if (slot >= 0) {
        OffRT* r = &s_d3d.off_rt[slot];
        if (r->w == w && r->h == h && r->dxgi == (u32)want_fmt) { r->used = 1; r->last_flip = flip_now; return slot; }
        /* The frame in flight still references this resource -- releasing it here
         * is "was deleted prior to closing the command list" (D3D12 message 921),
         * and every draw recorded against it silently lands nowhere while the
         * replacement reads back empty. Let the GPU finish first. Recreations are
         * rare (a surface changing size), so the stall costs nothing. */
        wait_for_gpu();
        r->res->lpVtbl->Release(r->res); r->res = NULL;   /* dims/format changed */
        if (r->up) { r->up->lpVtbl->Release(r->up); r->up = NULL; }
        if (r->ms) { r->ms->lpVtbl->Release(r->ms); r->ms = NULL; r->ms_dirty = 0; }   /* #249 */
    } else {
        for (int i = 0; i < MAX_OFF_RTS; i++)
            if (!s_d3d.off_rt[i].res) { slot = i; break; }
        if (slot < 0) {   /* evict an entry not used this frame */
            /* [NHL local patch #243] Least recently used, by guest flip. "used" is per BATCH,
             * and NHL builds its equipment composites in many small offscreen batches, so the
             * first unused slot was the per-frame post target 0x043F0000 (slot 0), evicted and
             * recreated from stale guest memory for every composite (game_20261028_191956
             * t=719-723) -- and a GPU resolve recorded into an RT evicted before the batch ran
             * is dropped (the Moments Live stuck image, game_20261028_180421 frame_86).
             * NHL_OFFRT_LRU=0 = old first-unused rule. */
            static int lru = -1;
            if (lru < 0) { const char* e = getenv("NHL_OFFRT_LRU"); lru = (e && *e == '0') ? 0 : 1; }
            u32 best_age = 0;
            for (int i = 0; i < MAX_OFF_RTS; i++) {
                if (s_d3d.off_rt[i].used) continue;
                if (!lru) { slot = i; break; }
                const u32 age = flip_now - s_d3d.off_rt[i].last_flip;
                if (slot < 0 || age > best_age) { slot = i; best_age = age; }
            }
            if (slot < 0) {
                static int nf = 0;   /* #242 */
                if (nf++ < 20) fprintf(stderr, "[OFFRT] table full (%d in use this batch): no RT for 0x%08X %ux%u%c",
                                       MAX_OFF_RTS, off, w, h, 10);
                return -1;
            }
            { static int ne = 0; const OffRT* v = &s_d3d.off_rt[slot];   /* #242 */
              if (ne < 300 && (v->blit_frame || ne < 40)) { ne++;
                  fprintf(stderr, "[OFFRT] evict slot %d 0x%08X %ux%u%s for 0x%08X %ux%u%c", slot, v->off, v->w, v->h,
                          v->blit_frame ? " (GPU-blit destination)" : "", off, w, h, 10); } }
            wait_for_gpu();   /* same hazard as above: the frame may still use it */
            s_d3d.off_rt[slot].res->lpVtbl->Release(s_d3d.off_rt[slot].res);
            s_d3d.off_rt[slot].res = NULL;
            if (s_d3d.off_rt[slot].up) {
                s_d3d.off_rt[slot].up->lpVtbl->Release(s_d3d.off_rt[slot].up);
                s_d3d.off_rt[slot].up = NULL;
            }
            if (s_d3d.off_rt[slot].ms) {   /* #249 */
                s_d3d.off_rt[slot].ms->lpVtbl->Release(s_d3d.off_rt[slot].ms);
                s_d3d.off_rt[slot].ms = NULL; s_d3d.off_rt[slot].ms_dirty = 0;
            }
        }
    }
    OffRT* r = &s_d3d.off_rt[slot];
    /* #253: display-derived surfaces get the scaled resource size */
    /* #254: + the effects factor for the smaller ones, up to the display size */
    int scaled = rs_active() && rs_scalable(w, h, s_d3d.width, s_d3d.height);
    u32 res_w = w, res_h = h;
    if (scaled) {
        u32 fx = (u32)s_fxs;
        while (fx > 1 && w * fx > s_d3d.width) fx >>= 1;
        const float k = s_rs * (float)fx;
        res_w = (u32)((float)w * k + 0.5f); res_h = (u32)((float)h * k + 0.5f);
        scaled = res_w != w || res_h != h;
    }
    D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td = {0};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = res_w; td.Height = res_h; td.DepthOrArraySize = 1; td.MipLevels = 1;
    /* [#210c] 8-bit targets typeless: a gamma-flagged read of one gets an *_SRGB view */
    td.Format = (want_fmt == DXGI_FORMAT_R8G8B8A8_UNORM) ? DXGI_FORMAT_R8G8B8A8_TYPELESS : want_fmt;
    td.SampleDesc.Count = 1;
    td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE cv = {0};
    cv.Format = want_fmt;
    if (FAILED(s_d3d.device->lpVtbl->CreateCommittedResource(
            s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &td,
            D3D12_RESOURCE_STATE_COPY_DEST, &cv,
            &IID_ID3D12Resource, (void**)&r->res)))
        return -1;
    r->off = off; r->w = w; r->h = h; r->rw = res_w; r->rh = res_h; r->dxgi = (u32)want_fmt; r->blit_frame = 0;
    { const u32 hb = wb_hash(off); s_ort_seen[hb >> 5] |= 1u << (hb & 31u);   /* #177 */
      s_ort_hint[hb & 255u] = (s8)((r - s_d3d.off_rt) + 1); }
    r->st = D3D12_RESOURCE_STATE_COPY_DEST;
    r->used = 1; r->last_flip = flip_now;

    /* Populate the new RT from guest memory: titles CPU-initialise their
     * render-target buffers (wave fills the height fields with texels of
     * (0,0,0, 1.0f) -- the .w is 'inverse of mass'; from an all-zero GPU
     * resource the water simulation can never boot). Guest data is
     * big-endian and tightly packed. */
    {
        extern uint8_t* vm_base;
        extern u32 cellGcmResolveOffset(u32);
        u32 bpp = (want_fmt == DXGI_FORMAT_R16G16B16A16_FLOAT) ? 8u :
                  (want_fmt == DXGI_FORMAT_R32G32B32A32_FLOAT) ? 16u :
                  (want_fmt == DXGI_FORMAT_R32_FLOAT) ? 4u : 4u;
        u32 pitch = (w * bpp + 255) & ~255u;
        u32 src = cellGcmResolveOffset(off);
        /* #253: a scaled surface starts zeroed (committed resources are): its guest bytes are
         * 1x, and display-derived surfaces are cleared / fully drawn every frame anyway */
        if (vm_base && src != 0xFFFFFFFFu && !scaled) {
            D3D12_HEAP_PROPERTIES hu = {0}; hu.Type = D3D12_HEAP_TYPE_UPLOAD;
            D3D12_RESOURCE_DESC bd = {0};
            bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            bd.Width = (u64)pitch * h; bd.Height = 1; bd.DepthOrArraySize = 1;
            bd.MipLevels = 1; bd.SampleDesc.Count = 1;
            bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if (SUCCEEDED(s_d3d.device->lpVtbl->CreateCommittedResource(
                    s_d3d.device, &hu, D3D12_HEAP_FLAG_NONE, &bd,
                    D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
                    &IID_ID3D12Resource, (void**)&r->up))) {
                void* mp = NULL; D3D12_RANGE nr = {0,0};
                if (SUCCEEDED(r->up->lpVtbl->Map(r->up, 0, &nr, &mp)) && mp) {
                    const u8* sp = vm_base + src;
                    for (u32 y = 0; y < h; y++) {
                        const u8* srow = sp + (u64)y * w * bpp;
                        u8* drow = (u8*)mp + (u64)y * pitch;
                        if (want_fmt == DXGI_FORMAT_R8G8B8A8_UNORM) {
                            /* guest A8R8G8B8 (bytes A,R,G,B) -> R,G,B,A */
                            for (u32 x = 0; x < w; x++) {
                                drow[x*4+0] = srow[x*4+1];
                                drow[x*4+1] = srow[x*4+2];
                                drow[x*4+2] = srow[x*4+3];
                                drow[x*4+3] = srow[x*4+0];
                            }
                        } else if (want_fmt == DXGI_FORMAT_R16G16B16A16_FLOAT) {
                            for (u32 x = 0; x < w * 4; x++) {   /* u16 halves, BE */
                                drow[x*2+0] = srow[x*2+1];
                                drow[x*2+1] = srow[x*2+0];
                            }
                        } else {                                 /* u32 floats, BE */
                            u32 nw = (w * bpp) / 4;
                            for (u32 x = 0; x < nw; x++) {
                                drow[x*4+0] = srow[x*4+3];
                                drow[x*4+1] = srow[x*4+2];
                                drow[x*4+2] = srow[x*4+1];
                                drow[x*4+3] = srow[x*4+0];
                            }
                        }
                    }
                    r->up->lpVtbl->Unmap(r->up, 0, NULL);
                    D3D12_TEXTURE_COPY_LOCATION cdst = {0}, csrc = {0};
                    cdst.pResource = r->res;
                    cdst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                    csrc.pResource = r->up;
                    csrc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                    csrc.PlacedFootprint.Footprint.Format   = want_fmt;
                    csrc.PlacedFootprint.Footprint.Width    = w;
                    csrc.PlacedFootprint.Footprint.Height   = h;
                    csrc.PlacedFootprint.Footprint.Depth    = 1;
                    csrc.PlacedFootprint.Footprint.RowPitch = pitch;
                    s_d3d.cmd_list->lpVtbl->CopyTextureRegion(s_d3d.cmd_list, &cdst, 0, 0, 0, &csrc, NULL);
                }
            }
        }
        {
            D3D12_RESOURCE_BARRIER b = {0};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource   = r->res;
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            b.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &b);
            r->st = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        }
    }

    D3D12_CPU_DESCRIPTOR_HANDLE rh;
    s_d3d.rt_rtv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.rt_rtv_heap, &rh);
    rh.ptr += (u64)slot * s_d3d.device->lpVtbl->GetDescriptorHandleIncrementSize(
        s_d3d.device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    { D3D12_RENDER_TARGET_VIEW_DESC rv = {0};   /* #210c: explicit (the resource may be typeless) */
      rv.Format = want_fmt; rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
      s_d3d.device->lpVtbl->CreateRenderTargetView(s_d3d.device, r->res, &rv, rh); }

    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {0};
    sv.Format = want_fmt;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE sh;
    s_d3d.srv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.srv_heap, &sh);
    sh.ptr += (u64)(RT_SRV_BASE + slot) * s_d3d.srv_inc;
    s_d3d.device->lpVtbl->CreateShaderResourceView(s_d3d.device, r->res, &sv, sh);

    static int _log = 0;
    if (_log++ < 8)
        printf("[D3D12] offscreen RT %d: off=0x%X %ux%u%s (render-to-texture)\n",
               slot, off, w, h, scaled ? " (scaled)" : "");
    return slot;
}

static D3D12_CPU_DESCRIPTOR_HANDLE off_rt_rtv(int slot)
{
    D3D12_CPU_DESCRIPTOR_HANDLE rh;
    s_d3d.rt_rtv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.rt_rtv_heap, &rh);
    rh.ptr += (u64)slot * s_d3d.device->lpVtbl->GetDescriptorHandleIncrementSize(
        s_d3d.device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    return rh;
}

/* Write a texture SRV (or a null SRV when res == NULL) at an absolute SRV
 * heap slot. Used to fill per-draw t0-t3 descriptor windows. */
static void srv_write_ex(u32 heap_slot, ID3D12Resource* res, DXGI_FORMAT fmt,
                         UINT mapping, int cube)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {0};
    sv.Format = fmt;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = mapping;
    sv.Texture2D.MipLevels = res ? (UINT)-1 : 1;   /* [#119] every level the resource has */
    if (cube) {   /* must match the TextureCube declaration in the HLSL */
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        sv.TextureCube.MipLevels = res ? (UINT)-1 : 1;   /* #215: every level the cube has */
        sv.TextureCube.MostDetailedMip = 0;
        sv.TextureCube.ResourceMinLODClamp = 0.0f;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE h;
    s_d3d.srv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.srv_heap, &h);
    h.ptr += (u64)heap_slot * s_d3d.srv_inc;
    s_d3d.device->lpVtbl->CreateShaderResourceView(s_d3d.device, res, &sv, h);
}

static void srv_write(u32 heap_slot, ID3D12Resource* res, DXGI_FORMAT fmt, UINT mapping)
{
    srv_write_ex(heap_slot, res, fmt, mapping, 0);
}

/* [NHL local patch #188] A depth-format texture (RSX DEPTH24_D8 / DEPTH16 and their FLOAT
 * forms) is a shadow map: RSX compares each sample's z against the stored depth and
 * returns lit (1) / shadowed (0). The depth-only shadow pass is not rendered (#113) and
 * depth formats were never uploaded, so the unit got a null SRV, every tap read 0 =
 * fully shadowed, and NHL's player shader (key 0xC486122E, 16-tap PCF on unit 15 =
 * zeta 0x01490000) drew both teams BLACK in the pre-game warmup, where the key light is
 * the shadowed one. Bind a 1x1 texture whose view forces every component to 1 instead:
 * lit, no shadows (until the shadow pass is emulated).
 * [#189b] play259: the user found the intro lighting WORSE with this (and live play maybe
 * better); the black players were really the missing TEXCOORD8/9 (#189). So it is OFF by
 * default (NHL_SHADOW_LIT=1 = on), and F5 toggles it live (global hotkey) to compare. */
static ID3D12Resource* s_one_tex;
static int shadow_lit_bind(u32 wslot)
{
    if (s_shadow_lit < 0) { const char* e = getenv("NHL_SHADOW_LIT"); InterlockedExchange(&s_shadow_lit, (e && *e == '1') ? 1 : 0); }
    if (!s_shadow_lit) return 0;
    if (!s_one_tex) {
        D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd = {0};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; rd.Width = 1; rd.Height = 1;
        rd.DepthOrArraySize = 1; rd.MipLevels = 1; rd.Format = DXGI_FORMAT_R8_UNORM;
        rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        if (FAILED(s_d3d.device->lpVtbl->CreateCommittedResource(s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &rd,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, NULL, &IID_ID3D12Resource, (void**)&s_one_tex))) {
            s_one_tex = NULL; InterlockedExchange(&s_shadow_lit, 0); return 0;
        }
    }
    const UINT one = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(
        D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1, D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1,
        D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1, D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1);
    srv_write(wslot, s_one_tex, DXGI_FORMAT_R8_UNORM, one);
    return 1;
}

/* [NHL local patch #190] Shadow maps, emulated. NHL renders its shadow passes with NO
 * colour target into a 1024x1024 depth surface (zeta 0x01490000 and others) and samples
 * that surface as a DEPTH24_D8 texture with hardware compare (16-tap PCF in the player
 * shader). #113 dropped those passes, so every shadow tap read "shadowed": the warmup's
 * spotlit players went dark (play258-260; with everything forced lit (#188/F5) they came
 * closer to the user's reference shots). Now each depth-only draw/clear is recorded
 * (is_depth) and executed into a D32 depth texture per zeta surface; a unit that samples
 * that surface binds it as R32_FLOAT, and its fragment program is decompiled in the
 * decompiler's shadow-compare mode (rsx_fp_shadow_desc_from_texture). No depth bias yet
 * (RSX polygon offset is not tracked). NHL_SHADOWS=0 = the old drop. */
static int shadows_on(void)
{
    if (s_shadows_live < 0) { const char* e = getenv("NHL_SHADOWS"); InterlockedExchange(&s_shadows_live, (e && *e == '0') ? 0 : 1); }
    return (int)s_shadows_live;
}
typedef struct { u32 off, w, h; ID3D12Resource* res; D3D12_RESOURCE_STATES st; u32 rw, rh; } ShadowZ;   /* #254: rw/rh = resource size */
/* [NHL local patch #254] Sharper shadows: square depth-only surfaces (NHL's shadow maps, 1024x1024)
 * are held at NHL_SHADOW_SCALE (1, 2 or 4; default 1) x their guest size. Their draws' viewports
 * scale with them; shaders sample them with normalized coordinates (the decompiler's unnormalized
 * path divides by the guest size via g_rsx_fp_shadow_scale). Non-square depth surfaces (the
 * 1280x720 depth pass) stay 1x. Capped at 8192. */
static int shadow_scale(void)
{
    static int s = -1;
    if (s < 0) { const char* e = getenv("NHL_SHADOW_SCALE"); s = e ? atoi(e) : 1;
                 s = s >= 4 ? 4 : s >= 2 ? 2 : 1;
                 g_rsx_fp_shadow_scale = (float)s;
                 if (s > 1) fprintf(stderr, "[SHADOWZ] shadow maps at %dx their size%c", s, 10); }
    return s;
}
#define MAX_SHADOWZ 8
static ShadowZ s_shz[MAX_SHADOWZ];
static int s_shz_n;
static ID3D12DescriptorHeap* s_shz_heap;
static u32 s_shz_inc;
static int shadowz_find(u32 off)
{
    for (int i = 0; i < s_shz_n; i++) if (s_shz[i].off == off && s_shz[i].res) return i;
    return -1;
}
static D3D12_CPU_DESCRIPTOR_HANDLE shadowz_dsv(int i)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h;
    s_shz_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_shz_heap, &h);
    h.ptr += (u64)i * s_shz_inc;
    return h;
}
static int shadowz_get(u32 off, u32 w, u32 h)
{
    int i = shadowz_find(off);
    if (i >= 0 && s_shz[i].w == w && s_shz[i].h == h) return i;
    if (!w || !h || w > 4096 || h > 4096) return -1;
    if (!s_shz_heap) {
        D3D12_DESCRIPTOR_HEAP_DESC hd = {0};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV; hd.NumDescriptors = MAX_SHADOWZ;
        if (FAILED(s_d3d.device->lpVtbl->CreateDescriptorHeap(s_d3d.device, &hd, &IID_ID3D12DescriptorHeap, (void**)&s_shz_heap)))
            { s_shz_heap = NULL; return -1; }
        s_shz_inc = s_d3d.device->lpVtbl->GetDescriptorHandleIncrementSize(s_d3d.device, D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    }
    if (i < 0) { if (s_shz_n >= MAX_SHADOWZ) return -1; i = s_shz_n++; }
    /* #254: a square shadow map at shadow_scale() x */
    u32 k = (w == h) ? (u32)shadow_scale() : 1u;
    while (k > 1 && w * k > 8192u) k >>= 1;
    /* a resized surface: the old texture may still be in flight -- leak it (rare) */
    D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {0};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; rd.Width = w * k; rd.Height = h * k;
    rd.DepthOrArraySize = 1; rd.MipLevels = 1; rd.Format = DXGI_FORMAT_R32_TYPELESS;
    rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE cv = {0}; cv.Format = DXGI_FORMAT_D32_FLOAT; cv.DepthStencil.Depth = 1.0f;
    ID3D12Resource* res = NULL;
    if (FAILED(s_d3d.device->lpVtbl->CreateCommittedResource(s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv, &IID_ID3D12Resource, (void**)&res)) || !res) {
        s_shz[i].res = NULL; return -1;
    }
    D3D12_DEPTH_STENCIL_VIEW_DESC dv = {0};
    dv.Format = DXGI_FORMAT_D32_FLOAT; dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    s_d3d.device->lpVtbl->CreateDepthStencilView(s_d3d.device, res, &dv, shadowz_dsv(i));
    s_shz[i].off = off; s_shz[i].w = w; s_shz[i].h = h; s_shz[i].res = res;
    s_shz[i].rw = w * k; s_shz[i].rh = h * k;
    s_shz[i].st = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    fprintf(stderr, "[SHADOWZ] depth surface %d: zeta 0x%08X %ux%u (held %ux%u)%c", i, off, w, h, w * k, h * k, 10);
    return i;
}
static void shadowz_transition(int i, D3D12_RESOURCE_STATES to)
{
    ShadowZ* z = &s_shz[i];
    if (!z->res || z->st == to) return;
    D3D12_RESOURCE_BARRIER b = {0};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = z->res; b.Transition.StateBefore = z->st; b.Transition.StateAfter = to;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &b);
    z->st = to;
}
/* depth-only pipelines: the draw's VS, no pixel shader, no colour target, D32 */
typedef struct { int vs_idx; u32 vs_hash, dk, cull; ID3D12PipelineState* pso; } DepthPSO;
static DepthPSO s_dpso[256];
static int s_dpso_n;
static ID3D12PipelineState* depth_pso_get(int vs_idx, u32 dk, u32 cull)
{
    if (vs_idx < 0 || vs_idx >= s_d3d.vp_vs_n || !s_d3d.vp_vs[vs_idx].vs) return NULL;
    const u32 vh = s_d3d.vp_vs[vs_idx].hash;
    for (int i = 0; i < s_dpso_n; i++)
        if (s_dpso[i].vs_idx == vs_idx && s_dpso[i].vs_hash == vh && s_dpso[i].dk == dk && s_dpso[i].cull == cull)
            return s_dpso[i].pso;
    D3D12_INPUT_ELEMENT_DESC il[16];
    for (int _i = 0; _i < 16; _i++) {
        il[_i].SemanticName = "ATTR"; il[_i].SemanticIndex = _i;
        il[_i].Format = DXGI_FORMAT_R32G32B32A32_FLOAT; il[_i].InputSlot = 0;
        il[_i].AlignedByteOffset = (UINT)(_i * 16);
        il[_i].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA; il[_i].InstanceDataStepRate = 0;
    }
    ID3DBlob* vsb = s_d3d.vp_vs[vs_idx].vs;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {0};
    pd.pRootSignature = s_d3d.vp_root_sig;
    pd.VS.pShaderBytecode = vsb->lpVtbl->GetBufferPointer(vsb);
    pd.VS.BytecodeLength  = vsb->lpVtbl->GetBufferSize(vsb);
    pd.InputLayout.pInputElementDescs = il; pd.InputLayout.NumElements = 16;
    if (s_d3d.vp_vs[vs_idx].gpu) { pd.InputLayout.pInputElementDescs = NULL; pd.InputLayout.NumElements = 0; }
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = (cull & 1u) ? ((cull & 2u) ? D3D12_CULL_MODE_FRONT : D3D12_CULL_MODE_BACK)
                                              : D3D12_CULL_MODE_NONE;
    pd.RasterizerState.FrontCounterClockwise = (cull & 4u) ? TRUE : FALSE;
    pd.SampleMask = UINT_MAX;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 0;
    pd.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pd.DepthStencilState.DepthEnable = (dk & 1u) ? TRUE : FALSE;
    pd.DepthStencilState.DepthWriteMask = (dk & 0x10u) ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    pd.DepthStencilState.DepthFunc = (D3D12_COMPARISON_FUNC)(((dk >> 1) & 7u) + 1u);
    pd.SampleDesc.Count = 1;
    ID3D12PipelineState* pso = NULL;
    const HRESULT hr = s_d3d.device->lpVtbl->CreateGraphicsPipelineState(s_d3d.device, &pd, &IID_ID3D12PipelineState, (void**)&pso);
    if (FAILED(hr)) { static int n = 0; if (n++ < 4) fprintf(stderr, "[SHADOWZ] depth PSO FAIL 0x%08lX (vs %d)%c", (unsigned long)hr, vs_idx, 10); pso = NULL; }
    const int k = (s_dpso_n < 256) ? s_dpso_n++ : (int)(vh & 255u);   /* full: overwrite (old PSO leaked) */
    s_dpso[k].vs_idx = vs_idx; s_dpso[k].vs_hash = vh; s_dpso[k].dk = dk; s_dpso[k].cull = cull; s_dpso[k].pso = pso;
    return pso;
}
static void fp_shadow_from_draw(const D3D12DrawRecord* dr)
{
    s_fp_cur_shadow_on = 0;
    /* [NHL local patch #240] Guest CLAMP address modes. Every static sampler wraps, so a
     * unit the guest clamps (TEXTURE_ADDRESS wrap_s [3:0] / wrap_t [11:8]: 3 CLAMP_TO_EDGE,
     * 4 BORDER, 5 CLAMP) blended the OPPOSITE edge into its edge texels wherever the
     * filter footprint reached past it -- at lower mips. NHL's jersey shader samples the
     * number atlas (unit 2, clamped) at a fixed corner uv for every non-number pixel with
     * the cloth's own LOD: the Flyers atlas has an opaque white letter band along the far
     * edge, so at playing distance the whole jersey got ~half a white number layer
     * (salmon body, grey sleeves; right with TEX_NO_MIPS=1, 2026-10-28). These units' samples
     * clamp the uv to the edge texel centre of the level read (vp_get_fp_pso); the mask is
     * part of the program key. Cubes and shadow-compare units are left alone (BORDER as
     * edge clamp). NHL_TEX_CLAMP=0 = old all-wrap. */
    /* [NHL local patch #245] #176 point-samples units whose MAG filter is NEAREST -- at mip 0 at
     * every distance. On RSX the MIN filter still applies when the texture is minified, and NHL
     * pairs nearest-mag with linear/mipmapped min on its skin and cloth detail maps: shrunk on
     * screen they must be filtered. Read at mip 0 point-sampled they alias into the crosshatch /
     * checkerboard on faces and the dotted weave on jerseys (user F9 2026-10-28: Be a Pro editor
     * face, Cup-celebration close-up). Class per unit (2 bits, unit u at bit 2u):
     * 0 = min NEAREST (point, mip 0, the old behaviour), 1 = NEAREST_MIPMAP_* (point at the
     * nearest mip), 2 = LINEAR (bilinear, mip 0), 3 = LINEAR_MIPMAP_* / convolution (the unit's
     * trilinear sampler). Magnified (lod <= 0) stays point-sampled. Part of the program key.
     * NHL_TEX_MINFILTER=0 = old #176 behaviour. */
    s_fp_cur_ptmin = 0;
    { static int on = -1; if (on < 0) { const char* e = getenv("NHL_TEX_MINFILTER"); on = (e && *e == '0') ? 0 : 1; }
      if (on && dr) for (int u = 1; u < 16 && u < D3D_TEX_UNITS; u++) {
          if (!dr->tex[u].set || dr->tex[u].cube || ((dr->tex[u].filter >> 24) & 7u) != 1u) continue;
          const u32 mn = (dr->tex[u].filter >> 16) & 7u;
          const u32 cls = (mn == 2u) ? 2u : (mn == 3u || mn == 5u) ? 1u : (mn == 4u || mn >= 6u) ? 3u : 0u;
          s_fp_cur_ptmin |= cls << (2 * u);
      } }
    s_fp_cur_clamp = 0;
    { static int on = -1; if (on < 0) { const char* e = getenv("NHL_TEX_CLAMP"); on = (e && *e == '0') ? 0 : 1; }
      if (on && dr) for (int u = 0; u < 16 && u < D3D_TEX_UNITS; u++) {
          if (!dr->tex[u].set || dr->tex[u].cube || dr->tex[u].dim > 2u || (dr->zmask & (1u << u))) continue;
          const u32 ws = dr->tex[u].addr & 0xFu, wt = (dr->tex[u].addr >> 8) & 0xFu;
          if (ws >= 3u && ws <= 5u) s_fp_cur_clamp |= 1u << u;
          if (wt >= 3u && wt <= 5u) s_fp_cur_clamp |= 1u << (16 + u);
      } }
    if (!dr || !dr->zmask) return;
    memset(s_fp_cur_shadow, 0, sizeof s_fp_cur_shadow);
    for (int u = 0; u < 16 && u < D3D_TEX_UNITS; u++) {
        if (!(dr->zmask & (1u << u))) continue;
        rsx_fp_shadow_desc_from_texture(&s_fp_cur_shadow[u], dr->tex[u].fmt, dr->tex[u].dim, (u32)dr->tex[u].cube,
                                        dr->tex[u].mips, dr->tex[u].filter, dr->tex[u].addr, dr->tex[u].ctl0);
        if (s_fp_cur_shadow[u].enabled == 1) s_fp_cur_shadow_on |= 1 << u;
        else s_fp_cur_shadow[u].enabled = 0;   /* unsupported layout / compare off: plain sample */
    }
}

/* [NHL local patch #209] A depth surface sampled with a COLOUR format. NHL's depth of
 * field / fog passes read the 1x scene depth (0x00B70000, tile 1) as A8R8G8B8 with remap
 * 0xAA93 and rebuild z from the bytes (floor(rgb * 255) . k). Nothing here ever wrote that
 * address, so the passes read zero depth and blurred the WHOLE frame (play301: the scene
 * RT sharp, the composite 0x043F0000 all out of focus). RPCS3's surface cache hands such a
 * fetch the depth surface's D24S8 bytes; here a fullscreen pass repacks our D32 depth
 * surface into an RGBA8 texture laid out like the guest's ARGB8 decode (R = z[15:8],
 * G = z[7:0], B = stencil 0, A = z[23:16]) and the unit samples it through its normal
 * remap. The source is the shadowz surface at that offset, else the one of the same size
 * (the 1280x720 depth pass, zeta 0x03120000). Aliases: any zeta a draw used, plus
 * NHL_DEPTHCOL_ALIAS (default 00B70000,00F30000). NHL_DEPTHCOL=0 = old guest-memory read. */
static int depthcol_on(void)
{
    static int v = -1;
    if (v < 0) { const char* e = getenv("NHL_DEPTHCOL"); v = (e && *e == '0') ? 0 : 1; }
    return v;
}
static u32 s_zseen[32];
static int s_zseen_n;
static void depthcol_note_zeta(u32 off)
{
    if (!off) return;
    for (int i = 0; i < s_zseen_n; i++) if (s_zseen[i] == off) return;
    if (s_zseen_n < 32) { s_zseen[s_zseen_n++] = off; fprintf(stderr, "[DEPTHCOL] zeta 0x%08X in use%c", off, 10); }
}
void depthcol_note_zeta_ext(u32 off) { depthcol_note_zeta(off); }
static int depthcol_alias(u32 raw)
{
    for (int i = 0; i < s_zseen_n; i++) if (s_zseen[i] == raw) return 1;
    static u32 al[8]; static int na = -1;
    if (na < 0) { na = 0; const char* e = getenv("NHL_DEPTHCOL_ALIAS"); if (!e) e = "00B70000,00F30000";
        while (*e && na < 8) { char* end; const u32 v = (u32)strtoul(e, &end, 16);
            if (end == e) break; al[na++] = v; e = (*end == ',') ? end + 1 : end; } }
    for (int i = 0; i < na; i++) if (al[i] == raw) return 1;
    return 0;
}
/* [#209b] Source MAX_SHADOWZ = a snapshot of the shared offscreen depth buffer (depth_buffer2)
 * taken when the colour pass with the most depth-writing draws this frame ends -- the scene's
 * real depth. play302: the depth-only surface (0x03120000 in shadowz) is not the scene depth
 * (the scene's colour draws write depth_buffer2), so the DOF stayed blurred.
 * NHL_DEPTHCOL_SRC=shadowz = #209's depth-only surface instead. */
#define DC_SNAP MAX_SHADOWZ
static u32 s_shz_gen[MAX_SHADOWZ + 1];   /* bumped by every depth draw/clear executed into the surface */
static ID3D12Resource* s_zsnap;
static u32 s_zsnap_w, s_zsnap_h, s_zsnap_frame, s_zsnap_ok;
static u32 s_span_zdraws, s_zbest_frame, s_zbest_n;
static int s_span_rt = -1;   /* #210c: the offscreen RT those depth draws went to (depth_buffer2) */
static int depthcol_snap_src(void)
{
    static int v = -1;
    if (v < 0) { const char* e = getenv("NHL_DEPTHCOL_SRC"); v = (e && !strcmp(e, "shadowz")) ? 0 : 1; }
    return v;
}
static int depthcol_snap_alloc(void);
static int s_span_ms;                           /* #249: those depth draws went to the MSAA depth */
static int ms_depth_resolve(u32 w, u32 h);      /* #249, defined with the MSAA helpers */
/* the colour pass on off_rt[rt] ends: keep its depth if it is the frame's biggest so far.
 * #249: returns 1 when it ran the MSAA depth resolve, which leaves its own root signature,
 * pipeline, heap and a depth-only target bound -- the caller restores the pass state. */
static int depthcol_snapshot_maybe(int rt)
{
    const u32 fc = (u32)s_d3d.frame_count;
    if (fc != s_zbest_frame) { s_zbest_frame = fc; s_zbest_n = 0; }
    const u32 n = s_span_zdraws;
    const int ms = s_span_ms;
    s_span_zdraws = 0; s_span_rt = -1; s_span_ms = 0;
    if (!depthcol_on() || !depthcol_snap_src() || rt < 0 || !s_d3d.depth_buffer2 || n <= s_zbest_n) return 0;
    /* [#210] only a display-size pass is the scene: play303 also picked 640x360 / 640x760 passes,
     * re-creating (and leaking) the colour copy ~100x a minute and feeding those frames wrong depth */
    if (s_d3d.off_rt[rt].w != s_d3d.width || s_d3d.off_rt[rt].h != s_d3d.height) return 0;
    if (!depthcol_snap_alloc()) return 0;
    s_zbest_n = n;
    /* #249: the scene drew into the MSAA depth -- sample 0 into depth_buffer2 first */
    const int resolved = ms && ms_depth_resolve(s_d3d.off_rt[rt].rw, s_d3d.off_rt[rt].rh);   /* #253 */
    ID3D12GraphicsCommandList* cl = s_d3d.cmd_list;
    D3D12_RESOURCE_BARRIER b[2] = {{0}};
    b[0].Type = b[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b[0].Transition.pResource = s_d3d.depth_buffer2;
    b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE; b[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b[1].Transition.pResource = s_zsnap;
    b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE; b[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    b[0].Transition.Subresource = b[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cl->lpVtbl->ResourceBarrier(cl, 2, b);
    cl->lpVtbl->CopyResource(cl, s_zsnap, s_d3d.depth_buffer2);
    for (int i = 0; i < 2; i++) { const D3D12_RESOURCE_STATES t = b[i].Transition.StateBefore;
        b[i].Transition.StateBefore = b[i].Transition.StateAfter; b[i].Transition.StateAfter = t; }
    cl->lpVtbl->ResourceBarrier(cl, 2, b);
    s_zsnap_w = s_d3d.off_rt[rt].rw; s_zsnap_h = s_d3d.off_rt[rt].rh;   /* #253: the region the scene drew */
    s_zsnap_frame = fc; s_zsnap_ok = 1; s_shz_gen[DC_SNAP]++;
    { static int nl = 0; if (nl++ < 6) fprintf(stderr, "[DEPTHCOL] scene depth snapshot of RT 0x%08X %ux%u (%u depth draws)%s%c",
                                           s_d3d.off_rt[rt].off, s_zsnap_w, s_zsnap_h, n, resolved ? " from MSAA depth" : "", 10); }
    return resolved;
}
/* the snapshot buffer, created on first use (display size until the first snapshot sets it) */
static int depthcol_snap_alloc(void)
{
    if (!s_zsnap_w) { s_zsnap_w = s_dw; s_zsnap_h = s_dh; }   /* #253: resource size */
    if (!s_zsnap) {
        D3D12_RESOURCE_DESC rd = {0};   /* = depth_buffer2's desc (init code), minus the DSV flag */
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; rd.Width = zbuf_w(); rd.Height = zbuf_h();   /* #253 */
        rd.DepthOrArraySize = 1; rd.MipLevels = 1; rd.Format = DXGI_FORMAT_R24G8_TYPELESS;
        rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        if (FAILED(s_d3d.device->lpVtbl->CreateCommittedResource(s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &rd,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, NULL, &IID_ID3D12Resource, (void**)&s_zsnap))) {
            s_zsnap = NULL; return 0; }
        fprintf(stderr, "[DEPTHCOL] scene depth snapshot buffer %llux%u%c", (unsigned long long)rd.Width, rd.Height, 10);
    }
    return 1;
}
static int depthcol_source(u32 raw, u32 w, u32 h)
{
    /* #210d: always the snapshot -- this frame's is taken (exec order) before its DOF reads.
     * The old "snapshot <= 2 frames old" test fell back to the depth-only surface for the first
     * frame after a camera cut, and the game's smoothed focus then needed ~1 s to recover
     * (play306/307: "blurry for a second, then it figures it out"). */
    if (depthcol_snap_src()) return DC_SNAP;
    const int zi = shadowz_find(raw);
    if (zi >= 0) return zi;
    int best = -1;
    for (int i = 0; i < s_shz_n; i++)
        if (s_shz[i].res && s_shz[i].w == w && s_shz[i].h == h && (best < 0 || s_shz_gen[i] > s_shz_gen[best])) best = i;
    return best;
}
typedef struct { ID3D12Resource* res; ID3D12Resource* src; u32 w, h, gen; D3D12_RESOURCE_STATES st; } DepthCol;
static DepthCol s_dc[MAX_SHADOWZ + 1];
static ID3D12DescriptorHeap* s_dc_rtv;
static ID3D12DescriptorHeap* s_dc_srv;
static u32 s_dc_rtv_inc, s_dc_srv_inc;
static ID3D12RootSignature* s_dc_rs;
static ID3D12PipelineState* s_dc_pso;
static int s_dc_init = 0;   /* 1 ok, -1 failed */
static int depthcol_init(void)
{
    if (s_dc_init) return s_dc_init > 0;
    s_dc_init = -1;
    ID3D12Device* dev = s_d3d.device;
    D3D12_DESCRIPTOR_HEAP_DESC hd = {0};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = MAX_SHADOWZ + 1;   /* + DC_SNAP */
    if (FAILED(dev->lpVtbl->CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void**)&s_dc_rtv))) return 0;
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(dev->lpVtbl->CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void**)&s_dc_srv))) return 0;
    s_dc_rtv_inc = dev->lpVtbl->GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    s_dc_srv_inc = dev->lpVtbl->GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    {   D3D12_DESCRIPTOR_RANGE range = {0};
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; range.NumDescriptors = 1;
        D3D12_ROOT_PARAMETER rp = {0};
        rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rp.DescriptorTable.NumDescriptorRanges = 1; rp.DescriptorTable.pDescriptorRanges = &range;
        rp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC rd = {0};
        rd.NumParameters = 1; rd.pParameters = &rp;
        ID3DBlob* sig = NULL; ID3DBlob* err = NULL;
        if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err))) {
            if (err) err->lpVtbl->Release(err); return 0; }
        const HRESULT hr = dev->lpVtbl->CreateRootSignature(dev, 0, sig->lpVtbl->GetBufferPointer(sig),
                               sig->lpVtbl->GetBufferSize(sig), &IID_ID3D12RootSignature, (void**)&s_dc_rs);
        sig->lpVtbl->Release(sig);
        if (FAILED(hr)) return 0; }
    {   static const char vs[] =
            "float4 main(uint id : SV_VertexID) : SV_Position {\n"
            "  float2 uv = float2((id << 1) & 2, id & 2); return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); }\n";
        static const char ps[] =
            "Texture2D<float> t : register(t0);\n"
            "float4 main(float4 p : SV_Position) : SV_Target {\n"
            "  uint w, h; t.GetDimensions(w, h);\n"
            "  const float z = t.Load(int3(min(uint2(p.xy), uint2(w - 1, h - 1)), 0));\n"
            "  const uint d = (uint)(saturate(z) * 16777215.0 + 0.5);\n"
            "  return float4((d >> 8) & 255u, d & 255u, 0, d >> 16) / 255.0; }\n";
        ID3DBlob* vb = NULL; ID3DBlob* pb = NULL; ID3DBlob* e = NULL;
        if (FAILED(D3DCompile(vs, sizeof(vs) - 1, "dc_vs", NULL, NULL, "main", "vs_5_0", 0, 0, &vb, &e)) ||
            FAILED(D3DCompile(ps, sizeof(ps) - 1, "dc_ps", NULL, NULL, "main", "ps_5_0", 0, 0, &pb, &e))) {
            if (e) { fprintf(stderr, "[DEPTHCOL] shader: %s%c", (const char*)e->lpVtbl->GetBufferPointer(e), 10); e->lpVtbl->Release(e); }
            return 0; }
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {0};
        pd.pRootSignature = s_dc_rs;
        pd.VS.pShaderBytecode = vb->lpVtbl->GetBufferPointer(vb); pd.VS.BytecodeLength = vb->lpVtbl->GetBufferSize(vb);
        pd.PS.pShaderBytecode = pb->lpVtbl->GetBufferPointer(pb); pd.PS.BytecodeLength = pb->lpVtbl->GetBufferSize(pb);
        pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pd.SampleMask = 0xFFFFFFFFu;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.RasterizerState.DepthClipEnable = TRUE;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1; pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM; pd.SampleDesc.Count = 1;
        const HRESULT hr = dev->lpVtbl->CreateGraphicsPipelineState(dev, &pd, &IID_ID3D12PipelineState, (void**)&s_dc_pso);
        vb->lpVtbl->Release(vb); pb->lpVtbl->Release(pb);
        if (FAILED(hr)) return 0; }
    s_dc_init = 1;
    return 1;
}
/* the RGBA8 copy of source zi (a shadowz surface, or DC_SNAP = the scene depth snapshot),
 * created / resized on demand; NULL on failure */
static DepthCol* depthcol_get(int zi)
{
    if (!depthcol_init()) return NULL;
    ID3D12Resource* sres; u32 sw, sh, soff; DXGI_FORMAT sfmt;
    if (zi == DC_SNAP) {
        if (!depthcol_snap_alloc()) return NULL;   /* #210d: exists before the first snapshot */
        sres = s_zsnap; sw = s_zsnap_w; sh = s_zsnap_h; soff = 0; sfmt = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    } else {
        if (zi < 0 || zi >= s_shz_n || !s_shz[zi].res) return NULL;
        sres = s_shz[zi].res; sw = s_shz[zi].rw; sh = s_shz[zi].rh; soff = s_shz[zi].off; sfmt = DXGI_FORMAT_R32_FLOAT;   /* #254 */
    }
    DepthCol* c = &s_dc[zi];
    if (!c->res || c->w != sw || c->h != sh) {
        /* a resized surface: the old copy may still be in flight -- leak it (rare) */
        D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd = {0};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; rd.Width = sw; rd.Height = sh;
        rd.DepthOrArraySize = 1; rd.MipLevels = 1; rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        rd.SampleDesc.Count = 1; rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        ID3D12Resource* res = NULL;
        if (FAILED(s_d3d.device->lpVtbl->CreateCommittedResource(s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &rd,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, NULL, &IID_ID3D12Resource, (void**)&res)) || !res)
            return NULL;
        D3D12_CPU_DESCRIPTOR_HANDLE h;
        s_dc_rtv->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_dc_rtv, &h);
        h.ptr += (u64)zi * s_dc_rtv_inc;
        s_d3d.device->lpVtbl->CreateRenderTargetView(s_d3d.device, res, NULL, h);
        c->res = res; c->w = sw; c->h = sh; c->gen = 0; c->src = NULL;
        c->st = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        if (zi == DC_SNAP) fprintf(stderr, "[DEPTHCOL] colour copy of the scene depth snapshot %ux%u%c", sw, sh, 10);
        else fprintf(stderr, "[DEPTHCOL] colour copy of depth surface %d (zeta 0x%08X %ux%u)%c", zi, soff, sw, sh, 10);
    }
    if (c->src != sres) {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd = {0};
        sd.Format = sfmt; sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; sd.Texture2D.MipLevels = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE h;
        s_dc_srv->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_dc_srv, &h);
        h.ptr += (u64)zi * s_dc_srv_inc;
        s_d3d.device->lpVtbl->CreateShaderResourceView(s_d3d.device, sres, &sd, h);
        c->src = sres; c->gen = 0;
    }
    return c;
}
static void dc_barrier(DepthCol* c, D3D12_RESOURCE_STATES to)
{
    if (c->st == to) return;
    D3D12_RESOURCE_BARRIER b = {0};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = c->res; b.Transition.StateBefore = c->st; b.Transition.StateAfter = to;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &b);
    c->st = to;
}
/* Record the repack of surface zi. Leaves the pass's root signature, heap and target
 * bound: the caller restores the guest-draw state and forces a target rebind. */
static void depthcol_convert(int zi)
{
    DepthCol* c = &s_dc[zi];
    ID3D12GraphicsCommandList* cl = s_d3d.cmd_list;
    if (zi != DC_SNAP) shadowz_transition(zi, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);   /* the snapshot stays PSR */
    dc_barrier(c, D3D12_RESOURCE_STATE_RENDER_TARGET);
    cl->lpVtbl->SetGraphicsRootSignature(cl, s_dc_rs);
    cl->lpVtbl->SetPipelineState(cl, s_dc_pso);
    ID3D12DescriptorHeap* heaps[] = { s_dc_srv };
    cl->lpVtbl->SetDescriptorHeaps(cl, 1, heaps);
    D3D12_GPU_DESCRIPTOR_HANDLE g;
    s_dc_srv->lpVtbl->GetGPUDescriptorHandleForHeapStart(s_dc_srv, &g);
    g.ptr += (u64)zi * s_dc_srv_inc;
    cl->lpVtbl->SetGraphicsRootDescriptorTable(cl, 0, g);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv;
    s_dc_rtv->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_dc_rtv, &rtv);
    rtv.ptr += (u64)zi * s_dc_rtv_inc;
    cl->lpVtbl->OMSetRenderTargets(cl, 1, &rtv, FALSE, NULL);
    D3D12_VIEWPORT vp = {0, 0, (float)c->w, (float)c->h, 0.0f, 1.0f};
    D3D12_RECT sc = {0, 0, (LONG)c->w, (LONG)c->h};
    cl->lpVtbl->RSSetViewports(cl, 1, &vp);
    cl->lpVtbl->RSSetScissorRects(cl, 1, &sc);
    cl->lpVtbl->IASetPrimitiveTopology(cl, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cl->lpVtbl->DrawInstanced(cl, 3, 1, 0, 0);
    dc_barrier(c, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    c->gen = s_shz_gen[zi] + 1u;
    { static u32 nc = 0; static ULONGLONG t0 = 0; nc++;
      const ULONGLONG t = GetTickCount64(); if (!t0) t0 = t;
      if (t - t0 >= 5000) { fprintf(stderr, "[DEPTHCOL] %u depth->colour repacks / 5 s%c", nc, 10); nc = 0; t0 = t; } }
}

static void off_rt_transition(int slot, D3D12_RESOURCE_STATES to)
{
    OffRT* r = &s_d3d.off_rt[slot];
    if (r->st == to) return;
    D3D12_RESOURCE_BARRIER b = {0};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource   = r->res;
    b.Transition.StateBefore = r->st;
    b.Transition.StateAfter  = to;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &b);
    r->st = to;
}

/* [NHL local patch #249] Real MSAA for the guest's antialiased surfaces. NHL renders its scene
 * into 0x02980000 with SET_SURFACE_FORMAT antialias 3 = diagonal-centred 2 samples (pitch
 * 10240 = 1280 x 2 samples x 4 bytes; older notes called it 4x), the 640x360 chain
 * 0x03DF/03FF/041F0000 likewise, and draws the glass, net and decals with alpha-to-coverage.
 * The backend drew all of it single-sampled: stair-stepped edges, A2C as a blend (#103/#174).
 * Now an offscreen RT bound by a draw whose surface has a non-zero AA mode renders into a
 * multisampled twin (OffRT.ms, NHL_MSAA samples: 0/1 = off, 2, 4, 8; default 4) with a shared
 * MSAA depth/stencil (DSV [2]); those PSOs carry the sample count and real
 * AlphaToCoverageEnable (ctl key bits 27-29, set by ms_exp). OffRT.res stays the single-sample
 * surface everything else reads (SRVs, #85 blits, write-backs, captures): ms_resolve() runs
 * ResolveSubresource when the pass leaves the twin, before a copy reads it, and at the end of
 * the batch. The #209 scene-depth snapshot first copies sample 0 of the MSAA depth into
 * depth_buffer2 (ms_depth_resolve). 8 is the ceiling: D3D12 guarantees 8 samples for these
 * formats, 16 is optional (AMD has none) and adds nothing visible at 1280x720. */
static int s_ms_n = -1;     /* session sample count, 1 = off */
static u8 s_ms_fmt_ok[4];   /* RGBA8, RGBA16F, RGBA32F, R32F take s_ms_n samples */
static int ms_fmt_supported(DXGI_FORMAT f, UINT n)
{
    D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS q = {0};
    q.Format = f; q.SampleCount = n;
    return SUCCEEDED(s_d3d.device->lpVtbl->CheckFeatureSupport(s_d3d.device,
               D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &q, sizeof q)) && q.NumQualityLevels > 0;
}
static int msaa_count(void)
{
    if (s_ms_n >= 0) return s_ms_n;
    const char* e = getenv("NHL_MSAA");
    int n = e ? atoi(e) : 4;
    n = n >= 8 ? 8 : n >= 4 ? 4 : n >= 2 ? 2 : 1;
    const int asked = n;
    /* the colour format of NHL's AA surfaces and the depth format must both take it */
    while (n > 1 && !(ms_fmt_supported(DXGI_FORMAT_R8G8B8A8_UNORM, (UINT)n) &&
                      ms_fmt_supported(DXGI_FORMAT_D24_UNORM_S8_UINT, (UINT)n))) n >>= 1;
    if (n > 1) {
        s_ms_fmt_ok[0] = 1;
        s_ms_fmt_ok[1] = (u8)ms_fmt_supported(DXGI_FORMAT_R16G16B16A16_FLOAT, (UINT)n);
        s_ms_fmt_ok[2] = (u8)ms_fmt_supported(DXGI_FORMAT_R32G32B32A32_FLOAT, (UINT)n);
        s_ms_fmt_ok[3] = (u8)ms_fmt_supported(DXGI_FORMAT_R32_FLOAT, (UINT)n);
    }
    s_ms_n = n;
    fprintf(stderr, "[MSAA] %dx for the game's antialiased surfaces (asked %d)%c", n, asked, 10);
    return n;
}
/* samples for this record's colour target: 1 = single-sampled */
static int dr_ms(const D3D12DrawRecord* dr)
{
    if (!dr->rt_off || !((dr->rt_fmt >> 12) & 0xFu) || !s_d3d.depth_buffer2) return 1;
    const int n = msaa_count();
    if (n <= 1) return 1;
    const DXGI_FORMAT f = rsx_surface_dxgi(dr->rt_fmt);
    const int fi = f == DXGI_FORMAT_R16G16B16A16_FLOAT ? 1 : f == DXGI_FORMAT_R32G32B32A32_FLOAT ? 2 :
                   f == DXGI_FORMAT_R32_FLOAT ? 3 : 0;
    return s_ms_fmt_ok[fi] ? n : 1;
}
/* the draw's ctl key for an MSAA target: emulated A2C (bit 25) becomes real A2C (bit 27),
 * bits 28-29 = log2(samples) -> vp_get_fp_pso's SampleDesc */
static int ms_exp(const D3D12DrawRecord* dr, int exp)
{
    const int n = dr_ms(dr);
    if (n <= 1) return exp;
    u32 x = (u32)exp;
    if (x & (1u << 25)) x = (x & ~(1u << 25)) | (1u << 27);
    return (int)(x | ((u32)(n == 2 ? 1 : n == 4 ? 2 : 3) << 28));
}
static ID3D12DescriptorHeap* s_ms_rtv;
static ID3D12Resource* s_ms_z;
static D3D12_RESOURCE_STATES s_ms_z_st;
static int ms_off(const char* what)
{
    fprintf(stderr, "[MSAA] %s failed -- MSAA off for this session%c", what, 10);
    s_ms_n = 1;
    return 0;
}
static D3D12_CPU_DESCRIPTOR_HANDLE dsv_at(int i)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h;
    s_d3d.dsv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.dsv_heap, &h);
    h.ptr += (u64)i * s_d3d.device->lpVtbl->GetDescriptorHandleIncrementSize(s_d3d.device, D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    return h;
}
static D3D12_CPU_DESCRIPTOR_HANDLE ms_rtv(int slot)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h;
    s_ms_rtv->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_ms_rtv, &h);
    h.ptr += (u64)slot * s_d3d.device->lpVtbl->GetDescriptorHandleIncrementSize(s_d3d.device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    return h;
}
/* the shared MSAA depth/stencil: depth_buffer2's twin (2048x2048, typeless for the #209 resolve) */
static int ms_depth_ensure(void)
{
    if (s_ms_z) return 1;
    D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {0};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; rd.Width = zbuf_w(); rd.Height = zbuf_h();   /* #253 */
    rd.DepthOrArraySize = 1; rd.MipLevels = 1; rd.Format = DXGI_FORMAT_R24G8_TYPELESS;
    rd.SampleDesc.Count = (UINT)s_ms_n; rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE cv = {0};
    cv.Format = DXGI_FORMAT_D24_UNORM_S8_UINT; cv.DepthStencil.Depth = 1.0f;
    if (FAILED(s_d3d.device->lpVtbl->CreateCommittedResource(s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv, &IID_ID3D12Resource, (void**)&s_ms_z)) || !s_ms_z) {
        s_ms_z = NULL; return ms_off("MSAA depth buffer"); }
    D3D12_DEPTH_STENCIL_VIEW_DESC dv = {0};
    dv.Format = DXGI_FORMAT_D24_UNORM_S8_UINT; dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMS;
    s_d3d.device->lpVtbl->CreateDepthStencilView(s_d3d.device, s_ms_z, &dv, dsv_at(2));
    s_ms_z_st = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    fprintf(stderr, "[MSAA] depth/stencil %ux%u x%d%c", zbuf_w(), zbuf_h(), s_ms_n, 10);
    return 1;
}
/* create off_rt[slot]'s multisampled twin (cleared to 0) and its RTV */
static int ms_ensure(int slot)
{
    OffRT* r = &s_d3d.off_rt[slot];
    if (r->ms) return 1;
    if (!ms_depth_ensure()) return 0;
    if (!s_ms_rtv) {
        D3D12_DESCRIPTOR_HEAP_DESC hd = {0};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = MAX_OFF_RTS;
        if (FAILED(s_d3d.device->lpVtbl->CreateDescriptorHeap(s_d3d.device, &hd, &IID_ID3D12DescriptorHeap, (void**)&s_ms_rtv))) {
            s_ms_rtv = NULL; return ms_off("MSAA RTV heap"); }
    }
    D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td = {0};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; td.Width = r->rw; td.Height = r->rh;   /* #253 */
    td.DepthOrArraySize = 1; td.MipLevels = 1; td.Format = (DXGI_FORMAT)r->dxgi;
    td.SampleDesc.Count = (UINT)s_ms_n; td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE cv = {0};
    cv.Format = (DXGI_FORMAT)r->dxgi;
    if (FAILED(s_d3d.device->lpVtbl->CreateCommittedResource(s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &td,
            D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, &IID_ID3D12Resource, (void**)&r->ms)) || !r->ms) {
        r->ms = NULL; return ms_off("MSAA colour target"); }
    D3D12_RENDER_TARGET_VIEW_DESC rv = {0};
    rv.Format = (DXGI_FORMAT)r->dxgi; rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMS;
    s_d3d.device->lpVtbl->CreateRenderTargetView(s_d3d.device, r->ms, &rv, ms_rtv(slot));
    s_d3d.cmd_list->lpVtbl->ClearRenderTargetView(s_d3d.cmd_list, ms_rtv(slot), cv.Color, 0, NULL);
    r->ms_st = D3D12_RESOURCE_STATE_RENDER_TARGET; r->ms_dirty = 0;
    { static int n = 0; if (n++ < 16) fprintf(stderr, "[MSAA] x%d twin for RT %d 0x%08X %ux%u%c", s_ms_n, slot, r->off, r->w, r->h, 10); }
    return 1;
}
static void ms_transition(OffRT* r, D3D12_RESOURCE_STATES to)
{
    if (r->ms_st == to) return;
    D3D12_RESOURCE_BARRIER b = {0};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r->ms; b.Transition.StateBefore = r->ms_st; b.Transition.StateAfter = to;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &b);
    r->ms_st = to;
}
/* the twin's samples -> res (what SRVs, copies and write-backs read) */
static void ms_resolve(int slot)
{
    OffRT* r = &s_d3d.off_rt[slot];
    if (!r->ms || !r->ms_dirty || !r->res) return;
    ms_transition(r, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
    off_rt_transition(slot, D3D12_RESOURCE_STATE_RESOLVE_DEST);
    s_d3d.cmd_list->lpVtbl->ResolveSubresource(s_d3d.cmd_list, r->res, 0, r->ms, 0, (DXGI_FORMAT)r->dxgi);
    r->ms_dirty = 0;
}
/* every drawn twin except the (up to 4) slots about to be bound multisampled */
static void ms_resolve_except(int a, const int* m)
{
    for (int i = 0; i < MAX_OFF_RTS; i++) {
        if (!s_d3d.off_rt[i].ms_dirty) continue;
        if (i == a || (m && (i == m[0] || i == m[1] || i == m[2]))) continue;
        ms_resolve(i);
    }
}
/* #209 snapshot helper: sample 0 of the MSAA depth -> depth_buffer2 (an SV_Depth pass on the
 * #209 root signature). 1 = done; the caller restores the guest-draw state. */
static ID3D12PipelineState* s_msz_pso;
static ID3D12DescriptorHeap* s_msz_srv;
static int s_msz_bad;
static int ms_depth_resolve(u32 w, u32 h)
{
    if (!s_ms_z || s_msz_bad || !depthcol_init()) return 0;
    ID3D12Device* dev = s_d3d.device;
    if (!s_msz_pso) {
        s_msz_bad = 1;
        D3D12_DESCRIPTOR_HEAP_DESC hd = {0};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors = 1;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(dev->lpVtbl->CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void**)&s_msz_srv))) return 0;
        D3D12_SHADER_RESOURCE_VIEW_DESC sd = {0};
        sd.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS; sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        D3D12_CPU_DESCRIPTOR_HANDLE ch;
        s_msz_srv->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_msz_srv, &ch);
        dev->lpVtbl->CreateShaderResourceView(dev, s_ms_z, &sd, ch);
        static const char vs[] =
            "float4 main(uint id : SV_VertexID) : SV_Position {\n"
            "  float2 uv = float2((id << 1) & 2, id & 2); return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); }\n";
        static const char ps[] =
            "Texture2DMS<float> t : register(t0);\n"
            "float main(float4 p : SV_Position) : SV_Depth { return t.Load(int2(p.xy), 0); }\n";
        ID3DBlob* vb = NULL; ID3DBlob* pb = NULL; ID3DBlob* e = NULL;
        if (FAILED(D3DCompile(vs, sizeof(vs) - 1, "msz_vs", NULL, NULL, "main", "vs_5_0", 0, 0, &vb, &e)) ||
            FAILED(D3DCompile(ps, sizeof(ps) - 1, "msz_ps", NULL, NULL, "main", "ps_5_0", 0, 0, &pb, &e))) {
            if (e) { fprintf(stderr, "[MSAA] depth resolve shader: %s%c", (const char*)e->lpVtbl->GetBufferPointer(e), 10); e->lpVtbl->Release(e); }
            if (vb) vb->lpVtbl->Release(vb);
            return 0; }
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {0};
        pd.pRootSignature = s_dc_rs;
        pd.VS.pShaderBytecode = vb->lpVtbl->GetBufferPointer(vb); pd.VS.BytecodeLength = vb->lpVtbl->GetBufferSize(vb);
        pd.PS.pShaderBytecode = pb->lpVtbl->GetBufferPointer(pb); pd.PS.BytecodeLength = pb->lpVtbl->GetBufferSize(pb);
        pd.SampleMask = 0xFFFFFFFFu;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.RasterizerState.DepthClipEnable = TRUE;
        pd.DepthStencilState.DepthEnable = TRUE;
        pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        pd.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 0; pd.SampleDesc.Count = 1;
        const HRESULT hr = dev->lpVtbl->CreateGraphicsPipelineState(dev, &pd, &IID_ID3D12PipelineState, (void**)&s_msz_pso);
        vb->lpVtbl->Release(vb); pb->lpVtbl->Release(pb);
        if (FAILED(hr)) { s_msz_pso = NULL; fprintf(stderr, "[MSAA] depth resolve PSO failed (0x%08lX)%c", hr, 10); return 0; }
        s_msz_bad = 0;
    }
    ID3D12GraphicsCommandList* cl = s_d3d.cmd_list;
    D3D12_RESOURCE_BARRIER b = {0};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = s_ms_z; b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = s_ms_z_st; b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    cl->lpVtbl->ResourceBarrier(cl, 1, &b);
    cl->lpVtbl->SetGraphicsRootSignature(cl, s_dc_rs);
    cl->lpVtbl->SetPipelineState(cl, s_msz_pso);
    ID3D12DescriptorHeap* heaps[] = { s_msz_srv };
    cl->lpVtbl->SetDescriptorHeaps(cl, 1, heaps);
    D3D12_GPU_DESCRIPTOR_HANDLE g;
    s_msz_srv->lpVtbl->GetGPUDescriptorHandleForHeapStart(s_msz_srv, &g);
    cl->lpVtbl->SetGraphicsRootDescriptorTable(cl, 0, g);
    const D3D12_CPU_DESCRIPTOR_HANDLE z2 = dsv_at(1);
    cl->lpVtbl->OMSetRenderTargets(cl, 0, NULL, FALSE, &z2);
    D3D12_VIEWPORT vp = {0, 0, (float)w, (float)h, 0.0f, 1.0f};
    D3D12_RECT sc = {0, 0, (LONG)w, (LONG)h};
    cl->lpVtbl->RSSetViewports(cl, 1, &vp);
    cl->lpVtbl->RSSetScissorRects(cl, 1, &sc);
    cl->lpVtbl->IASetPrimitiveTopology(cl, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cl->lpVtbl->DrawInstanced(cl, 3, 1, 0, 0);
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE; b.Transition.StateAfter = s_ms_z_st;
    cl->lpVtbl->ResourceBarrier(cl, 1, &b);
    return 1;
}
/* the VP pass's shared state (render_frame sets these at the start of the pass), rebound after
 * a host pass (#249 depth resolve) replaced the root signature, heap and input assembly */
static D3D12_VERTEX_BUFFER_VIEW s_vpr_vbv;
static D3D12_GPU_DESCRIPTOR_HANDLE s_vpr_gh;
static void vp_pass_restore(void)
{
    ID3D12GraphicsCommandList* cl = s_d3d.cmd_list;
    cl->lpVtbl->SetGraphicsRootSignature(cl, s_d3d.vp_root_sig);
    cl->lpVtbl->IASetPrimitiveTopology(cl, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D12DescriptorHeap* heaps[] = { s_d3d.srv_heap };
    cl->lpVtbl->SetDescriptorHeaps(cl, 1, heaps);
    cl->lpVtbl->SetGraphicsRootDescriptorTable(cl, 1, s_vpr_gh);
    cl->lpVtbl->IASetVertexBuffers(cl, 0, 1, &s_vpr_vbv);
    if (s_d3d.vp_ib) {
        D3D12_INDEX_BUFFER_VIEW ibv;
        ibv.BufferLocation = s_d3d.vp_ib->lpVtbl->GetGPUVirtualAddress(s_d3d.vp_ib)
                           + (u64)s_d3d.vp_parity * VP_MAX_INDICES * 4u;
        ibv.SizeInBytes = VP_MAX_INDICES * 4u;
        ibv.Format = DXGI_FORMAT_R32_UINT;
        cl->lpVtbl->IASetIndexBuffer(cl, &ibv);
    }
    if (s_d3d.vp_raw)
        cl->lpVtbl->SetGraphicsRootShaderResourceView(cl, 3,
            s_d3d.vp_raw->lpVtbl->GetGPUVirtualAddress(s_d3d.vp_raw) + (u64)s_d3d.vp_parity * VP_RAW_BYTES);
}

/* [NHL local patch #85] NV3089 blits out of an offscreen RT, done on the GPU.
 * NHL resolves its 4x-MSAA HDR scene (0x02980000, guest pitch 10240 = 2560 px)
 * into 0x047D0000 (pitch 5120) with two 2:1-horizontal NV3089 strips (1023 + 257
 * px; the engine is limited to 1024 wide) every gameplay frame, and the post
 * chain samples 0x047D0000. nv3089_blit is a guest-memory CPU copy, but the
 * scene exists only in the D3D12 RT -- so the post chain read nothing and the
 * tonemap turned it into the WHITE screen (play81/82). This backend keeps the
 * MSAA surface already resolved (single-sample, 1280 wide), so a resolve blit
 * whose scale equals the AA factor is a 1:1 copy of our RT. Recorded as an
 * ordered op in the batch; the destination becomes an RT, which texture
 * fetches of its address then bind (off_rt_find in the SRV pass).
 * Returns 1 if recorded. NHL_GPU_BLIT=0 disables. */
/* #242: 1 the first time this (source, destination, reason) is skipped; 1024 distinct max */
static int gpublit_skip_new(u32 src, u32 dst, const char* why)
{
    static struct { u32 src, dst; const char* why; } seen[1024]; static int n = 0;
    for (int i = 0; i < n; i++)
        if (seen[i].src == src && seen[i].dst == dst && !strcmp(seen[i].why, why)) return 0;
    if (n >= 1024) return 0;
    seen[n].src = src; seen[n].dst = dst; seen[n].why = why; n++;
    return 1;
}
int rsx_d3d12_gpu_blit(u32 in_off, u32 in_pitch, u32 u0, u32 v0, u32 ds_dx, u32 dt_dy,
                       u32 dst_base, u32 dst_pitch, u32 dx, u32 dy, u32 w, u32 h)
{
    static int en = -1;
    if (en < 0) { const char* e = getenv("NHL_GPU_BLIT"); en = (e && *e == '0') ? 0 : 1; }
    if (!en || !s_d3d.initialized || !in_pitch || !dst_pitch || !w || !h) return 0;
    /* [NHL local patch #242] Every skip is logged once per (source, destination, reason):
     * the old first-4-only messages were used up at t=42 by an unrelated blit, and the
     * Moments Live stuck image (game_20261028_180421 F9 frame_86: scene RT 0x02980000 live,
     * post chain reading a stale 0x047D0000) left no trace of why its resolve was dropped. */
#define GPUBLIT_SKIP(why, fmt, ...) do { if (gpublit_skip_new(in_off, dst_base, why)) \
        fprintf(stderr, "[GPUBLIT] skip (%s): src 0x%08X pitch %u -> dst 0x%08X pitch %u %ux%u at %u,%u ds=%X dt=%X | " fmt "%c", \
                why, in_off, in_pitch, dst_base, dst_pitch, w, h, dx, dy, ds_dx, dt_dy, __VA_ARGS__, 10); return 0; } while (0)
    /* source: the offscreen RT whose guest rows contain in_off */
    int si = -1;
    for (int i = 0; i < MAX_OFF_RTS; i++) {
        const OffRT* r = &s_d3d.off_rt[i];
        if (!r->res || in_off < r->off) continue;
        if (in_off - r->off < (u64)in_pitch * r->h) { si = i; break; }
    }
    if (si < 0) GPUBLIT_SKIP("no-src-rt", "no offscreen RT holds the source (u0 %u v0 %u)", u0, v0);
    const OffRT* S = &s_d3d.off_rt[si];
    const u32 rel = in_off - S->off;
    const u32 gpx = (rel % in_pitch) / 4u + u0, gpy = rel / in_pitch + v0;
    u32 fa = 1;   /* horizontal AA factor between guest layout and our RT */
    if (S->w && (in_pitch / 4u) >= 2u * S->w && (in_pitch / 4u) % S->w == 0) fa = (in_pitch / 4u) / S->w;
    /* only copies that are 1:1 in our RT's space (resolve scale == AA factor) */
    if ((u64)w * ds_dx != (u64)w * fa * 0x100000u || dt_dy != 0x100000u)
        GPUBLIT_SKIP("scale", "src RT 0x%08X %ux%u, AA factor %u", S->off, S->w, S->h, fa);
    const u32 sx = gpx / fa, sy = gpy;
    if (sx >= S->w || sy >= S->h)
        GPUBLIT_SKIP("outside", "src RT 0x%08X %ux%u, start %u,%u", S->off, S->w, S->h, sx, sy);
    const u32 dw = dst_pitch / 4u, dh = S->h;
    const int di = off_rt_get(dst_base, dw, dh, 0x08u);
    if (di < 0 || di == si)
        GPUBLIT_SKIP("no-dst-rt", "dst RT slot %d (src slot %d), want %ux%u", di, si, dw, dh);
    if (s_d3d.off_rt[di].dxgi != S->dxgi)
        GPUBLIT_SKIP("format", "dxgi %u -> %u", S->dxgi, s_d3d.off_rt[di].dxgi);
    { extern void d3d12_flush_if_full_fwd(void); d3d12_flush_if_full_fwd(); }
    if (s_d3d.draw_count >= MAX_DRAWS)
        GPUBLIT_SKIP("batch-full", "%u records", s_d3d.draw_count);
#undef GPUBLIT_SKIP
    D3D12DrawRecord* dr = &s_d3d.draws[s_d3d.draw_count++];
    memset(dr, 0, sizeof(*dr));
    dr->is_vp = 1; dr->is_clear = 1; dr->is_blit = 1; dr->tex_slot = -1;
    dr->rt_off = dst_base;
    dr->blit_src = S->off; dr->blit_dst = dst_base;
    dr->blit_sx = sx; dr->blit_sy = sy; dr->blit_dx = dx; dr->blit_dy = dy;
    dr->blit_w = w; dr->blit_h = h;
    s_d3d.off_rt[di].blit_frame = s_d3d.frame_count + 1u;   /* #213 */
    { static int n = 0;
      if (n++ < 6) fprintf(stderr, "[GPUBLIT] RT 0x%08X (%u,%u) -> RT 0x%08X (%u,%u) %ux%u (AA factor %u)%c",
                           S->off, sx, sy, dst_base, dx, dy, w, h, fa, 10); }
    return 1;
}

/* Set by the present entry point: 1 = this batch ends with a swapchain
 * Present, 0 = execute the recorded draws only. An OFFSCREEN-only batch (a
 * render-to-texture pass) must still run -- it is what fills the texture a
 * later batch samples -- but presenting it would show a half-built frame. */
static int s_present_this_frame = 1;

/* Resolve a DRAW_*_TEX value: a hex raw offset, or the literal "duck" for the
 * content-identified duck texture. */
static u32 draw_filter_tex(const char* e)
{
    if (!e) return 0;
    if (e[0] == 'd' && e[1] == 'u') return s_duck_raw;
    return (u32)strtoul(e, NULL, 16);
}

/* Copy the current backbuffer into s_screen_copy. Called mid-frame the moment
 * the reduced-viewport passes finish -- this title renders its reflection into
 * the TOP-LEFT REGION of the render target and the main scene then overwrites
 * it, so a snapshot taken at end of frame contains the scene, not the
 * reflection. Also called at end of frame as a fallback for frames with no
 * reduced-viewport pass. */
static void screen_copy_capture(u32 fi)
{
    static int en = -1;
    if (en < 0) { const char* e = getenv("SCREEN_AS_TEX"); en = e ? atoi(e) : 1; }
    if (!en || !s_d3d.device) return;
            if (!s_screen_copy) {
                D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
                D3D12_RESOURCE_DESC td = {0};
                td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
                td.Width = s_dw; td.Height = s_dh;   /* #253: = render_targets[] */
                td.DepthOrArraySize = 1; td.MipLevels = 1;
                td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
                td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
                if (FAILED(s_d3d.device->lpVtbl->CreateCommittedResource(
                        s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &td,
                        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, NULL,
                        &IID_ID3D12Resource, (void**)&s_screen_copy)))
                    s_screen_copy = NULL;
            }
            if (s_screen_copy) {
                D3D12_RESOURCE_BARRIER bb[2] = {0};
                bb[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                bb[0].Transition.pResource   = s_d3d.render_targets[fi];
                bb[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
                bb[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
                bb[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                bb[1] = bb[0];
                bb[1].Transition.pResource   = s_screen_copy;
                bb[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                bb[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
                s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 2, bb);

                s_d3d.cmd_list->lpVtbl->CopyResource(s_d3d.cmd_list,
                    s_screen_copy, s_d3d.render_targets[fi]);

                bb[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
                bb[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
                bb[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
                bb[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 2, bb);

                /* SCREENCOPY_DUMP=1: stage the captured image into the readback
                 * buffer so it can be written out and LOOKED AT. Claiming a
                 * capture happened is not the same as showing what is in it. */
                { static int sd = -1;
                  if (sd < 0) { const char* e = getenv("SCREENCOPY_DUMP"); sd = e ? atoi(e) : 0; }
                  if (sd && s_d3d.readback_buf) {
                      D3D12_RESOURCE_BARRIER t = {0};
                      t.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                      t.Transition.pResource   = s_screen_copy;
                      t.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                      t.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
                      t.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                      s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &t);

                      D3D12_TEXTURE_COPY_LOCATION cd = {0}, cs = {0};
                      cd.pResource = s_d3d.readback_buf;
                      cd.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                      cd.PlacedFootprint.Footprint.Format   = DXGI_FORMAT_R8G8B8A8_UNORM;
                      cd.PlacedFootprint.Footprint.Width    = s_dw;
                      cd.PlacedFootprint.Footprint.Height   = s_dh;
                      cd.PlacedFootprint.Footprint.Depth    = 1;
                      cd.PlacedFootprint.Footprint.RowPitch = s_d3d.readback_pitch;
                      cs.pResource = s_screen_copy;
                      cs.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                      s_d3d.cmd_list->lpVtbl->CopyTextureRegion(s_d3d.cmd_list, &cd, 0, 0, 0, &cs, NULL);

                      t.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
                      t.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                      s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &t);
                      s_sc_dump_pending = 1;
                  } }
            }

}

/* rsx_topology -> D3D_PRIMITIVE_TOPOLOGY. Spelled out rather than relying on
 * the two enums happening to agree: the draw record carries the neutral value
 * so a second backend can read it, and this is the one place it becomes a D3D
 * one. */
static D3D12_PRIMITIVE_TOPOLOGY topo_to_d3d(u32 t)
{
    switch (t) {
    case RSX_TOPOLOGY_POINTS:         return D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
    case RSX_TOPOLOGY_LINES:          return D3D_PRIMITIVE_TOPOLOGY_LINELIST;
    case RSX_TOPOLOGY_LINE_STRIP:     return D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
    case RSX_TOPOLOGY_TRIANGLE_STRIP: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
    case RSX_TOPOLOGY_TRIANGLES:      return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    default:                          return D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
    }
}

/* GCM_GUEST_FB=<off>,<w>,<h>,<pitch> -- present the guest display buffer.
 *
 * A title that composites its final image with the RSX 2D engine assembles it
 * in GUEST memory: nv3089_blit is a CPU copy and never touches D3D12, and the
 * draw pass below only runs when a recorded draw targets the display surface,
 * which for such a title never happens. The frame is finished, in the wrong
 * memory, and the swapchain shows only the clear. Copy it across.
 *
 * Off unless the offset and geometry are given, so nothing else changes. */
/* GPU-side composite of an offscreen RT onto the backbuffer.
 *
 * A title that finishes its frame with the RSX 2D engine blits a rendered
 * surface into the display buffer. That surface lives in guest RAM, which the
 * D3D12 path never writes -- the scene is on the GPU -- so the CPU copy in
 * nv3089_blit faithfully moves zeros. The backend already holds the same
 * surface as a texture, so do the composite here instead. Triggered from
 * cellGcmSys under GCM_COMPOSITE_RT. */
static u32 s_composite_src = 0;
static int s_last_drawn_slot = -1;
static u32 s_last_drawn_off  = 0;

void rsx_d3d12_composite_from_offscreen(u32 raw_off) { s_composite_src = raw_off; }

static void composite_present(u32 fi)
{
    u32 want = s_composite_src;
    s_composite_src = 0;
    if (!want) return;
    /* want == 2 means "whatever this frame's draws targeted" -- the one thing
     * guaranteed to hold geometry, and immune to the offset collision above. */
    int i;
    if (want == 2u) { i = s_last_drawn_slot; want = s_last_drawn_off; }
    else            { i = off_rt_find(want); }
    if (i < 0 || !s_d3d.off_rt[i].res) return;
    OffRT* r = &s_d3d.off_rt[i];
    /* Same-format copy only; a half-float source would need a converting draw. */
    if (r->dxgi != DXGI_FORMAT_R8G8B8A8_UNORM) {
        static int w1 = 0;
        if (w1++ < 2)
            fprintf(stderr, "[COMPOSITE] RT 0x%08X is dxgi=%u, not R8G8B8A8 -- needs a draw%c",
                    want, r->dxgi, 10);
        return;
    }
    u32 cw = r->rw < s_dw ? r->rw : s_dw;   /* #253: resource sizes */
    u32 ch = r->rh < s_dh ? r->rh : s_dh;
    if (!cw || !ch) return;
    { static int n = 0;
      if (n++ < 3) fprintf(stderr, "[COMPOSITE] RT 0x%08X %ux%u -> backbuffer%c",
                           want, cw, ch, 10); }

    D3D12_RESOURCE_BARRIER b[2] = {0};
    b[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b[0].Transition.pResource   = r->res;
    b[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b[0].Transition.StateBefore = r->st;
    b[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b[1] = b[0];
    b[1].Transition.pResource   = s_d3d.render_targets[fi];
    b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
    s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 2, b);

    D3D12_TEXTURE_COPY_LOCATION dl = {0}, sl = {0};
    dl.pResource = s_d3d.render_targets[fi];
    dl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dl.SubresourceIndex = 0;
    sl.pResource = r->res;
    sl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; sl.SubresourceIndex = 0;
    D3D12_BOX box = {0};
    box.left = 0; box.top = 0; box.front = 0;
    box.right = cw; box.bottom = ch; box.back = 1;
    s_d3d.cmd_list->lpVtbl->CopyTextureRegion(s_d3d.cmd_list, &dl, 0, 0, 0, &sl, &box);

    b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b[0].Transition.StateAfter  = r->st;
    b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
    s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 2, b);
}


static void guest_fb_present(u32 fi)
{
    static int inited = 0;
    static u32 off, gw, gh, gpitch, up_pitch;
    static ID3D12Resource* up = NULL;
    if (!inited) {
        inited = 1;
        const char* e = getenv("GCM_GUEST_FB");
        unsigned a = 0, b = 0, c = 0, d = 0;
        if (!e || sscanf(e, "%i,%u,%u,%u", &a, &b, &c, &d) != 4 || !b || !c) return;
        off = a; gw = b; gh = c; gpitch = d;
        up_pitch = (gw * 4u + 255u) & ~255u;      /* D3D12 wants 256-byte rows */
        D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC bd = {0};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width  = (u64)up_pitch * gh; bd.Height = 1;
        bd.DepthOrArraySize = 1; bd.MipLevels = 1;
        bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(s_d3d.device->lpVtbl->CreateCommittedResource(
                s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &bd,
                D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
                &IID_ID3D12Resource, (void**)&up)))
            up = NULL;
        fprintf(stderr, "[GUESTFB] presenting guest 0x%08X %ux%u pitch %u -> backbuffer%c",
                off, gw, gh, gpitch, 10);
    }
    if (!up) return;
    { extern u8* vm_base;
      if (!vm_base) return;
      /* RSX local memory is mapped at guest 0xC0000000. */
      const u8* srow = vm_base + 0xC0000000u + off;
      void* m = NULL; D3D12_RANGE nr = {0, 0};
      if (FAILED(up->lpVtbl->Map(up, 0, &nr, &m)) || !m) return;
      for (u32 y = 0; y < gh; y++) {
          const u8* s = srow + (size_t)y * gpitch;
          u8* dp = (u8*)m + (size_t)y * up_pitch;
          for (u32 x = 0; x < gw; x++) {   /* guest B,G,R,A -> backbuffer R,G,B,A */
              dp[x*4+0] = s[x*4+2]; dp[x*4+1] = s[x*4+1];
              dp[x*4+2] = s[x*4+0]; dp[x*4+3] = 0xFF;
          }
      }
      D3D12_RANGE wr = {0, (SIZE_T)up_pitch * gh};
      up->lpVtbl->Unmap(up, 0, &wr); }

    D3D12_RESOURCE_BARRIER gb = {0};
    gb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    gb.Transition.pResource   = s_d3d.render_targets[fi];
    gb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    gb.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    gb.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
    s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &gb);

    D3D12_TEXTURE_COPY_LOCATION dl = {0}, sl = {0};
    dl.pResource = s_d3d.render_targets[fi];
    dl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dl.SubresourceIndex = 0;
    sl.pResource = up; sl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    sl.PlacedFootprint.Offset = 0;
    sl.PlacedFootprint.Footprint.Format   = DXGI_FORMAT_R8G8B8A8_UNORM;
    sl.PlacedFootprint.Footprint.Width    = gw;
    sl.PlacedFootprint.Footprint.Height   = gh;
    sl.PlacedFootprint.Footprint.Depth    = 1;
    sl.PlacedFootprint.Footprint.RowPitch = up_pitch;
    s_d3d.cmd_list->lpVtbl->CopyTextureRegion(s_d3d.cmd_list, &dl, 0, 0, 0, &sl, NULL);

    gb.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    gb.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
    s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &gb);
}


/* [NHL local patch #132] Persistent display buffers. On the RSX a display buffer is plain
 * memory: a frame that does not colour-clear it and does not cover it keeps whatever was
 * there. The backend starts every presented batch from a cleared backbuffer, so a frame in
 * which NHL skips its Apt scene composite (occasionally, under load) showed the bare clear
 * colour -- the short black flashes left after #131 (play184). Each display buffer (by its
 * colour offset, NHL rotates 3) keeps a copy of its last presented image; a batch that did
 * not colour-clear the display starts from that copy instead. NHL_DISP_KEEP=0 disables. */
static int present_on_flip(void);   /* #131, defined with d3d12_clear */
enum { DISP_KEEP_N = 4 };
static ID3D12Resource* s_keep_res[DISP_KEEP_N];
static u32 s_keep_off[DISP_KEEP_N];
static int s_keep_valid[DISP_KEEP_N];
static int s_disp_colour_cleared = 0;   /* d3d12_clear (#131): a display colour clear in this batch */
static int disp_keep_on(void)
{
    static int v = -1;
    if (v < 0) { const char* e = getenv("NHL_DISP_KEEP"); v = (e && e[0] == '0') ? 0 : 1; }
    return v;
}
/* The batch's display buffer: the colour offset most of its display draws target (#56). */
static u32 disp_batch_offset(void)
{
    u32 offs[4] = {0}, cnt[4] = {0}; int n = 0;
    for (u32 i = 0; i < s_d3d.draw_count && i < MAX_DRAWS; i++) {
        const D3D12DrawRecord* r = &s_d3d.draws[i];
        if (r->is_clear || r->rt_off != 0 || !r->surf_raw) continue;
        int f = -1; for (int k = 0; k < n; k++) if (offs[k] == r->surf_raw) { f = k; break; }
        if (f < 0 && n < 4) { f = n++; offs[f] = r->surf_raw; }
        if (f >= 0) cnt[f]++;
    }
    int best = -1; for (int k = 0; k < n; k++) if (best < 0 || cnt[k] > cnt[best]) best = k;
    return best >= 0 ? offs[best] : 0;
}
/* Slot for a display offset (created on first use, COPY_SOURCE state); -1 if none. */
static int disp_keep_slot(u32 off, int create)
{
    if (!off) return -1;
    for (int k = 0; k < DISP_KEEP_N; k++) if (s_keep_res[k] && s_keep_off[k] == off) return k;
    if (!create) return -1;
    for (int k = 0; k < DISP_KEEP_N; k++) {
        if (s_keep_res[k]) continue;
        D3D12_RESOURCE_DESC d = {0};   /* = the swap chain buffers (init_d3d12: R8G8B8A8, width x height) */
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = s_dw; d.Height = s_dh; d.DepthOrArraySize = 1; d.MipLevels = 1;   /* #253 */
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count = 1;
        d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN; d.Flags = D3D12_RESOURCE_FLAG_NONE;
        D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        if (FAILED(s_d3d.device->lpVtbl->CreateCommittedResource(s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &d,
                D3D12_RESOURCE_STATE_COPY_SOURCE, NULL, &IID_ID3D12Resource, (void**)&s_keep_res[k]))) {
            s_keep_res[k] = NULL; return -1; }
        s_keep_off[k] = off; s_keep_valid[k] = 0;
        return k;
    }
    return -1;
}
static void disp_barrier(ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
{
    D3D12_RESOURCE_BARRIER br = {0};
    br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    br.Transition.pResource = r; br.Transition.StateBefore = a; br.Transition.StateAfter = b;
    br.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &br);
}

/* [NHL local patch #226] RFSLOW_MS (default 200, 0 = off): one [RFSLOW] line per
 * render_frame slower than that, split by phase, to tell a GPU/driver wait
 * (present, the frame fence, the two full drains) from CPU pre-pass/recording. */
static unsigned long long s_rf_t[8];
#define RF_MARK(i) (s_rf_t[i] = ps3_qpc_us())
static void rf_slow_report(void)
{
    static long long lim = -1;
    if (lim < 0) { const char* e = getenv("RFSLOW_MS"); lim = (e ? atoll(e) : 200) * 1000; }
    RF_MARK(7);
    for (int i = 1; i < 8; i++) if (s_rf_t[i] < s_rf_t[i - 1]) s_rf_t[i] = s_rf_t[i - 1];   /* mark not reached */
    if (lim <= 0 || s_rf_t[7] - s_rf_t[0] < (unsigned long long)lim) return;
    static int n = 0; if (n++ >= 200) return;
    #define RFD(a, b) ((s_rf_t[b] - s_rf_t[a]) / 1000.0)
    fprintf(stderr, "[RFSLOW] %.0f ms: drain+vsjobs %.1f | present %.1f (Present %.1f, fence %.1f) | gpuwait %.1f"
                    " | setup %.1f | gpuwait2 %.1f | prepass %.1f | record %.1f | draws %u%c",
            RFD(0, 7), RFD(0, 1), RFD(1, 2), s_pp_present_us / 1000.0, s_pp_wait_us / 1000.0, RFD(2, 3),
            RFD(3, 4), RFD(4, 5), RFD(5, 6), RFD(6, 7), s_d3d.draw_count, 10);
    #undef RFD
}

static void render_frame(void)
{
    double _rf0 = perf_on() ? perf_now() : 0.0;
    const unsigned long long _hzrf = ps3_qpc_us();   /* #122 */
    memset(s_rf_t, 0, sizeof s_rf_t); s_rf_t[0] = _hzrf;   /* #226 */
    s_pp_present_us = s_pp_wait_us = 0;
    async_drain();    /* #225: background-compiled VS into their slots, PSO budget reset */
    fxc_run_jobs();   /* #124: vertex shaders queued while this batch was recorded */
    /* [NHL local patch] RSX_FRAMESTAT=1: once a second, what the frame being
     * presented holds -- draws per render target (offset, size, format; 0 = a
     * display buffer), clears, and whether it reaches the swap chain. For a
     * scene that renders but never shows up on screen. */
    { static int fs = -1; static ULONGLONG s_t = 0;
      if (fs < 0) { const char* e = getenv("RSX_FRAMESTAT"); fs = e ? atoi(e) : 0; }
      ULONGLONG now = GetTickCount64();
      if (fs && now - s_t >= 1000) { s_t = now;
          u32 off[8], w[8], h[8], fmt[8], n[8], clr[8]; int nt = 0; u32 other = 0;
          for (u32 i = 0; i < s_d3d.draw_count && i < MAX_DRAWS; i++) {
              const D3D12DrawRecord* dr = &s_d3d.draws[i]; int f = -1;
              for (int k = 0; k < nt; k++) if (off[k] == dr->rt_off) { f = k; break; }
              if (f < 0) { if (nt == 8) { other++; continue; }
                           f = nt++; off[f] = dr->rt_off; w[f] = dr->rt_w; h[f] = dr->rt_h;
                           fmt[f] = dr->rt_fmt; n[f] = 0; clr[f] = 0; }
              if (dr->is_clear) clr[f]++; else n[f]++;
          }
          fprintf(stderr, "[framestat] records=%u targets:", s_d3d.draw_count);
          for (int k = 0; k < nt; k++)
              fprintf(stderr, " 0x%X(%ux%u f%X)=%u+%uc", off[k], w[k], h[k], fmt[k], n[k], clr[k]);
          fprintf(stderr, "%s%c", other ? " +more" : "", 10);
      } }
    RF_MARK(1);
    pp_issue();   /* [#164] a held present goes out before the next frame records */
    RF_MARK(2);
    u32 fi = s_d3d.frame_index;

    /* Drain the GPU before touching shared upload resources (vp_vb vertices,
     * vp_cb constants, per-frame texture staging): the previous frame's draws
     * may still be reading them, and overwriting mid-flight tears geometry
     * (gcm/cube: triangles mixing stale and new vertices -> missing/sliver
     * polygons). These workloads are a few draws/frame, so full serialisation
     * costs little. */
    wait_for_gpu();
    RF_MARK(3);

    /* Compile the real vertex program once its microcode is captured, and keep
     * the constant bank uploaded for the VS. */
    /* The base VP pipeline gates the ENTIRE vertex-program draw pass below
     * (`if (s_d3d.vp_ready ...)`). The old trigger also required the microcode
     * size to differ from the last compile, so a title whose first captured
     * program has the same byte count as vp_compiled_bytes never built the base
     * pipeline -- and then every is_vp draw record was silently dropped: draws
     * recorded, constants uploaded, per-draw VS compiled and cached, and not one
     * of them submitted. While !vp_ready there is nothing to be stale against,
     * so only the "have microcode" test belongs here. */
    if (s_d3d.current_rsx_state && !s_d3d.vp_ready &&
        s_d3d.current_rsx_state->vp_ucode_bytes >= 16)
        compile_vp();
    /* Per-draw VP constants are snapshotted at record time (vp_record_cb). */

    /* Lazily create the readback buffer the first time a dump is requested. */
    if (s_d3d.dump_frames_left > 0 && !s_d3d.readback_buf) {
        s_d3d.readback_pitch = (s_dw * 4 + 255) & ~255u;   /* #253: display resource size */
        D3D12_HEAP_PROPERTIES hp = {0};
        hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC rd = {0};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = (u64)s_d3d.readback_pitch * s_dh;
        rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        s_d3d.device->lpVtbl->CreateCommittedResource(
            s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_COPY_DEST, NULL,
            &IID_ID3D12Resource, (void**)&s_d3d.readback_buf);
    }

    /* Reset command allocator and list */
    s_d3d.cmd_allocators[fi]->lpVtbl->Reset(s_d3d.cmd_allocators[fi]);
    s_d3d.cmd_list->lpVtbl->Reset(s_d3d.cmd_list, s_d3d.cmd_allocators[fi], NULL);
    { wchar_t nm[32];   /* [NHL local patch #57] DRED: name the list per frame */
      swprintf(nm, 32, L"f%u", (unsigned)s_d3d.frame_count);
      s_d3d.cmd_list->lpVtbl->SetName(s_d3d.cmd_list, nm);
      s_dlog_frame[s_d3d.frame_count & 3u] = s_d3d.frame_count; s_dlog_n[s_d3d.frame_count & 3u] = 0; }

    /* Upload the bound font atlas into an R8_UNORM texture and create its SRV.
     * The atlas is a linear 8-bit coverage map, so a straight row copy (no
     * deswizzle) suffices. It is a DYNAMIC glyph cache -- the game rasterizes
     * new glyphs into it over time -- so re-upload whenever it is dirty (set on
     * every bind), not just once. Uploading only on first bind left every glyph
     * added after frame 1 sampling stale texels -> torn text. The backend is
     * synchronous (wait_for_gpu each frame), so recreating the resource per
     * dirty frame is safe. */
    if (s_d3d.tex_bound && (!s_d3d.tex_ready || s_d3d.tex_dirty) && s_d3d.srv_heap && s_d3d.pipeline_state_tex) {
        extern uint8_t* vm_base;
        u32 w = s_d3d.tex_w, h = s_d3d.tex_h;
        u32 pitch = (w + 255) & ~255u;   /* D3D12 requires 256-byte row pitch */

        if (s_d3d.tex_resource) { s_d3d.tex_resource->lpVtbl->Release(s_d3d.tex_resource); s_d3d.tex_resource = NULL; }
        if (s_d3d.tex_upload)   { s_d3d.tex_upload->lpVtbl->Release(s_d3d.tex_upload);     s_d3d.tex_upload = NULL; }

        D3D12_HEAP_PROPERTIES hp_def = {0}; hp_def.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC td = {0};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = w; td.Height = h; td.DepthOrArraySize = 1; td.MipLevels = 1;
        td.Format = DXGI_FORMAT_R8_UNORM; td.SampleDesc.Count = 1;
        td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        HRESULT thr = s_d3d.device->lpVtbl->CreateCommittedResource(
            s_d3d.device, &hp_def, D3D12_HEAP_FLAG_NONE, &td,
            D3D12_RESOURCE_STATE_COPY_DEST, NULL,
            &IID_ID3D12Resource, (void**)&s_d3d.tex_resource);

        D3D12_HEAP_PROPERTIES hp_up = {0}; hp_up.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC ud = {0};
        ud.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        ud.Width = (u64)pitch * h; ud.Height = 1; ud.DepthOrArraySize = 1; ud.MipLevels = 1;
        ud.SampleDesc.Count = 1; ud.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (SUCCEEDED(thr))
            thr = s_d3d.device->lpVtbl->CreateCommittedResource(
                s_d3d.device, &hp_up, D3D12_HEAP_FLAG_NONE, &ud,
                D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
                &IID_ID3D12Resource, (void**)&s_d3d.tex_upload);

        if (SUCCEEDED(thr) && vm_base) {
            void* mapped = NULL;
            D3D12_RANGE nr = {0, 0};
            if (SUCCEEDED(s_d3d.tex_upload->lpVtbl->Map(s_d3d.tex_upload, 0, &nr, &mapped)) && mapped) {
                const u8* srcbase = vm_base + s_d3d.tex_src_offset;
                for (u32 y = 0; y < h; y++)
                    memcpy((u8*)mapped + (u64)y * pitch, srcbase + (u64)y * w, w);
                s_d3d.tex_upload->lpVtbl->Unmap(s_d3d.tex_upload, 0, NULL);

                D3D12_TEXTURE_COPY_LOCATION dst = {0}, src = {0};
                dst.pResource = s_d3d.tex_resource;
                dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                dst.SubresourceIndex = 0;
                src.pResource = s_d3d.tex_upload;
                src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                src.PlacedFootprint.Offset = 0;
                src.PlacedFootprint.Footprint.Format   = DXGI_FORMAT_R8_UNORM;
                src.PlacedFootprint.Footprint.Width    = w;
                src.PlacedFootprint.Footprint.Height   = h;
                src.PlacedFootprint.Footprint.Depth    = 1;
                src.PlacedFootprint.Footprint.RowPitch = pitch;
                s_d3d.cmd_list->lpVtbl->CopyTextureRegion(s_d3d.cmd_list, &dst, 0, 0, 0, &src, NULL);

                D3D12_RESOURCE_BARRIER tb = {0};
                tb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                tb.Transition.pResource   = s_d3d.tex_resource;
                tb.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
                tb.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                tb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &tb);

                D3D12_SHADER_RESOURCE_VIEW_DESC sv = {0};
                sv.Format = DXGI_FORMAT_R8_UNORM;
                sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                sv.Texture2D.MipLevels = 1;
                D3D12_CPU_DESCRIPTOR_HANDLE sh;
                s_d3d.srv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.srv_heap, &sh);
                s_d3d.device->lpVtbl->CreateShaderResourceView(s_d3d.device, s_d3d.tex_resource, &sv, sh);

                s_d3d.tex_ready = 1;
                s_d3d.tex_dirty = 0;   /* content now in sync with guest atlas */
                { static int _au = 0; if (_au++ < 3)
                    printf("[D3D12] atlas uploaded (%ux%u R8) -> textured\n", w, h); }
            }
        }
    }

    /* Frames with no ops (init/boot presents) don't count against the cap --
     * PNG-decode-heavy titles burn hundreds of empty presents before the first
     * real draw. */
    if (getenv("RTT_DUMP") && s_d3d.draw_count > 0) { static int _f=0; int _cap = atoi(getenv("RTT_DUMP")); if (_cap < 2) _cap = 14; if (_f++ < _cap) {
        fprintf(stderr, "[RTT] frame %d: %u ops\n", _f, s_d3d.draw_count);
        for (u32 _d = 0; _d < s_d3d.draw_count && _d < MAX_DRAWS; _d++) {
            D3D12DrawRecord* r = &s_d3d.draws[_d];
            fprintf(stderr, "[RTT]  op%02u %s rt=0x%X mrt=0x%X/0x%X/0x%X t0=0x%X fp=0x%X n=%u cmask=%X vp=%u,%u %ux%u blend=%d\n",
                _d, r->is_clear?"CLR ":(r->is_vp?"draw":"leg "), r->rt_off,
                r->rt_mrt[0], r->rt_mrt[1], r->rt_mrt[2],
                r->tex[0].raw, r->fp_addr, r->vertex_count,
                r->cmask, r->vp_x, r->vp_y, r->vp_w, r->vp_h, r->blend);
        }
    } }

    /* Debug: RTT_PASS=N shows pass N's output directly on screen (drops later
     * ops, retargets pass N to the backbuffer). */
    { const char* rp = getenv("RTT_PASS");
      if (rp) {
        int keep = atoi(rp), seen = 0; u32 cut = s_d3d.draw_count;
        int viewrt = getenv("RTT_VIEWRT") != NULL;
        for (u32 _d = 0; _d < s_d3d.draw_count && _d < MAX_DRAWS; _d++) {
            D3D12DrawRecord* r = &s_d3d.draws[_d];
            if (r->is_clear || !r->is_vp) continue;
            if (seen == keep) {
                /* Alone: retarget op N to the backbuffer. With RTT_VIEWRT:
                 * keep op N intact and pull the display composite forward so
                 * the chosen RT is shown as of this point in the chain. */
                if (!viewrt) { r->rt_off = 0; r->rt_mrt[0] = r->rt_mrt[1] = r->rt_mrt[2] = 0; }
                cut = _d + 1;
                break;
            }
            seen++;
        }
        if (viewrt) {
            for (u32 _j = cut; _j < s_d3d.draw_count && _j < MAX_DRAWS; _j++) {
                D3D12DrawRecord* r = &s_d3d.draws[_j];
                if (r->is_vp && !r->is_clear && r->rt_off == 0) {
                    if (cut < MAX_DRAWS) s_d3d.draws[cut++] = *r;
                    break;
                }
            }
        }
        s_d3d.draw_count = (cut < s_d3d.draw_count) ? cut : s_d3d.draw_count;
      } }

    /* [NHL local patch #56] One display buffer per frame. Every draw into ANY
     * registered display buffer lands on our single backbuffer, but the guest
     * shows only the buffer it flips. NHL's Starting Lineup draws its UI into
     * 0x10000 and, in the same frame, a black RT-0 copy (fp 0x0FDFD681) into
     * 0x3D0000 -- merged, the copy blacked out the lineup. Keep the display
     * buffer most of this frame's guest-FP draws target and drop the guest-FP
     * draws aimed at the other display buffers. NHL_DISP_MAJORITY=0 disables. */
    { static int dm = -1;
      if (dm < 0) { const char* e = getenv("NHL_DISP_MAJORITY"); dm = e ? atoi(e) : 1; }
      if (dm && s_d3d.draw_count > 1) {
        extern int cellGcmOffsetIsDisplay(u32 offset);
        u32 bufs[4] = {0}, cnt[4] = {0}; int nb = 0;
        for (u32 _d = 0; _d < s_d3d.draw_count && _d < MAX_DRAWS; _d++) {
            const D3D12DrawRecord* r = &s_d3d.draws[_d];
            if (r->is_clear || !r->is_vp || r->rt_off) continue;
            int k = 0; while (k < nb && bufs[k] != r->surf_raw) k++;
            if (k == nb) { if (nb == 4) continue; bufs[nb++] = r->surf_raw; }
            cnt[k]++;
        }
        if (nb > 1) {
            int best = 0;
            for (int k = 1; k < nb; k++) if (cnt[k] > cnt[best]) best = k;
            u32 keep = 0, dropped = 0;
            for (u32 _d = 0; _d < s_d3d.draw_count && _d < MAX_DRAWS; _d++) {
                const D3D12DrawRecord* r = &s_d3d.draws[_d];
                if (!r->is_clear && r->is_vp && !r->rt_off && r->surf_raw != bufs[best] &&
                    cellGcmOffsetIsDisplay(r->surf_raw)) { dropped++; continue; }
                if (keep != _d) s_d3d.draws[keep] = s_d3d.draws[_d];
                keep++;
            }
            s_d3d.draw_count = keep;
            static int n = 0;
            if (dropped && n++ < 4)
                fprintf(stderr, "[DISPBUF] kept display buffer 0x%X (%u draws), dropped %u draw(s) "
                                "aimed at other display buffers%c", bufs[best], cnt[best], dropped, 10);
        }
      } }

    /* DRAW_LIMIT=<N>: keep only the first N recorded ops this frame. The duck's
     * draws are ops 00..~03 of every frame, so a small limit shows the ducks
     * alone -- separating "the ducks never rasterize" from "later geometry is
     * painted over them". Diagnostic only. */
    { static int lim = -1;
      if (lim < 0) { const char* e = getenv("DRAW_LIMIT"); lim = e ? atoi(e) : 0; }
      if (lim > 0 && s_d3d.draw_count > (u32)lim) s_d3d.draw_count = (u32)lim; }

    /* [NHL local patch] PIXEL_PROBE bisect: while bisecting, drop draw k this
     * frame (the frame-end probe reads the pixel back and logs what k was). */
    static char s_probe_info[256];
    s_probe_info[0] = 0;
    if (s_probe_mode == 1 && s_probe_k >= 0 && (u32)s_probe_k < s_d3d.draw_count && s_probe_k < MAX_DRAWS) {
        const D3D12DrawRecord* r = &s_d3d.draws[s_probe_k];
        if (r->is_clear)
            snprintf(s_probe_info, sizeof s_probe_info, "CLEAR rt=0x%08X cc=(%.2f,%.2f,%.2f,%.2f)",
                     r->rt_off, r->cc[0], r->cc[1], r->cc[2], r->cc[3]);
        else
            snprintf(s_probe_info, sizeof s_probe_info, "DRAW rt=0x%08X fp=0x%08X n=%u blend=%d bk=0x%X t0=0x%08X fmt=0x%02X %ux%u remap=0x%04X",
                     r->rt_off, r->fp_addr, r->vertex_count, r->blend, r->blend_key, r->tex[0].off,
                     r->tex[0].fmt, r->tex[0].w, r->tex[0].h, r->tex[0].ctrl1 & 0xFFFF);
        for (u32 _d = (u32)s_probe_k; _d + 1 < s_d3d.draw_count && _d + 1 < MAX_DRAWS; _d++)
            s_d3d.draws[_d] = s_d3d.draws[_d + 1];
        s_d3d.draw_count--;
    }

    /* [NHL local patch] DRAW_KEEP_FMT=<hex fmt byte, e.g. A5>: keep only ops
     * whose unit-0 texture has that full format byte (plus clears). Isolates
     * one texture class on an otherwise empty frame. Diagnostic. */
    { static int kf = -2;
      if (kf == -2) { const char* e = getenv("DRAW_KEEP_FMT"); kf = e ? (int)strtoul(e, 0, 16) : -1; }
      if (kf >= 0) {
        u32 keep = 0;
        for (u32 _d = 0; _d < s_d3d.draw_count && _d < MAX_DRAWS; _d++) {
            if (!s_d3d.draws[_d].is_clear && s_d3d.draws[_d].tex[0].fmt != (u32)kf) continue;
            if (keep != _d) s_d3d.draws[keep] = s_d3d.draws[_d];
            keep++;
        }
        s_d3d.draw_count = keep;
      } }

    /* DRAW_SKIP_TEX=<hex raw offset>: drop every op binding that texture on
     * unit 0. The ducks sit behind the water's slab geometry, so removing the
     * occluder is the way to see whether the duck itself renders. Diagnostic. */
    { static const char* sk = (const char*)1; static u32 want = 0;
      if (sk == (const char*)1) sk = getenv("DRAW_SKIP_TEX");
      want = draw_filter_tex(sk);
      if (want) {
        u32 keep = 0;
        for (u32 _d = 0; _d < s_d3d.draw_count && _d < MAX_DRAWS; _d++) {
            if (!s_d3d.draws[_d].is_clear && s_d3d.draws[_d].tex[0].raw == want) continue;
            if (keep != _d) s_d3d.draws[keep] = s_d3d.draws[_d];
            keep++;
        }
        s_d3d.draw_count = keep;
      } }

    /* [NHL local patch] DRAW_SKIP_FP=<hex fp>[,<hex fp>...]: drop every draw using
     * one of those fragment programs (by fp_addr, as the rsxcap manifest prints it).
     * NHL's EA SPORTS transition is a full-screen YUV movie draw (fp 0x025B56C1)
     * composited over the in-game frame; skipping it shows what is underneath.
     * Diagnostic. */
    { static int n = -1; static u32 fps[8];
      if (n < 0) { n = 0; const char* e = getenv("DRAW_SKIP_FP");
                   while (e && *e && n < 8) { char* end; unsigned long v = strtoul(e, &end, 16);
                                              if (end == e) break; fps[n++] = (u32)v;
                                              e = (*end == ',') ? end + 1 : end; } }
      if (n > 0) {
        u32 keep = 0;
        for (u32 _d = 0; _d < s_d3d.draw_count && _d < MAX_DRAWS; _d++) {
            int skip = 0;
            if (!s_d3d.draws[_d].is_clear)
                for (int k = 0; k < n; k++) if (s_d3d.draws[_d].fp_addr == fps[k]) skip = 1;
            if (skip) continue;
            if (keep != _d) s_d3d.draws[keep] = s_d3d.draws[_d];
            keep++;
        }
        s_d3d.draw_count = keep;
      } }

    /* DRAW_KEEP_TEX=<hex raw offset>: the inverse -- keep only the ops binding
     * that texture (plus clears). Isolates one object from everything drawn over
     * it, which is what finally shows the duck on its own. Diagnostic. */
    { static const char* kp = (const char*)1; static u32 want = 0;
      if (kp == (const char*)1) kp = getenv("DRAW_KEEP_TEX");
      want = draw_filter_tex(kp);
      /* DRAW_KEEP_NOCLEAR=1: also drop the clears. Keeping them preserves their
       * ORIGINAL position in the batch, so a clear that originally ran after the
       * kept draws still runs after them -- and wipes the very geometry being
       * isolated. That makes an object that renders fine look like it renders
       * nothing. */
      static int noclr = -1;
      if (noclr < 0) { const char* e = getenv("DRAW_KEEP_NOCLEAR"); noclr = e ? atoi(e) : 0; }
      /* DRAW_KEEP_BLEND=0|1: further restrict DRAW_KEEP_TEX to draws with that
       * blend flag. A mesh drawn twice -- once opaque, once blended -- is two
       * different passes (body vs reflection shell) that a texture filter alone
       * cannot separate. */
      static int kblend = -2;
      if (kblend == -2) { const char* e = getenv("DRAW_KEEP_BLEND");
                          kblend = e ? atoi(e) : -1; }
      if (want) {
        /* Emit the CLEARS FIRST, then the kept draws. Preserving the original
         * order means a clear that ran after them still runs after them and
         * wipes the isolated geometry; dropping the clears entirely (noclr)
         * leaves stale DEPTH that rejects most of its fragments. Neither shows
         * the object properly -- clear, then draw, does. */
        static D3D12DrawRecord kept[MAX_DRAWS];
        u32 nk = 0;
        if (!noclr)
            for (u32 _d = 0; _d < s_d3d.draw_count && _d < MAX_DRAWS; _d++)
                if (s_d3d.draws[_d].is_clear && nk < MAX_DRAWS) kept[nk++] = s_d3d.draws[_d];
        for (u32 _d = 0; _d < s_d3d.draw_count && _d < MAX_DRAWS; _d++)
            if (!s_d3d.draws[_d].is_clear && s_d3d.draws[_d].tex[0].raw == want
                && (kblend < 0 || s_d3d.draws[_d].blend == kblend)
                && nk < MAX_DRAWS)
                kept[nk++] = s_d3d.draws[_d];
        for (u32 _k = 0; _k < nk; _k++) s_d3d.draws[_k] = kept[_k];
        s_d3d.draw_count = nk;
      } }

    /* DRAW_LAST_TEX=<hex raw offset>: move the ops binding that texture to the
     * END of the batch so nothing is drawn over them. The duck's draws are ops
     * 00..03 of every frame and the tub wall is drawn afterwards across the same
     * screen area, so anything that defeats the depth sort hides the duck
     * completely. Reordering separates "occluded" from "not rendered". */
    { static const char* lt = (const char*)1; static u32 want = 0;
      if (lt == (const char*)1) lt = getenv("DRAW_LAST_TEX");
      want = draw_filter_tex(lt);
      if (want && s_d3d.draw_count > 1) {
        static D3D12DrawRecord moved[MAX_DRAWS];
        u32 nm = 0, keep = 0;
        for (u32 _d = 0; _d < s_d3d.draw_count && _d < MAX_DRAWS; _d++) {
            if (!s_d3d.draws[_d].is_clear && s_d3d.draws[_d].tex[0].raw == want) {
                if (nm < MAX_DRAWS) moved[nm++] = s_d3d.draws[_d];
                continue;
            }
            if (keep != _d) s_d3d.draws[keep] = s_d3d.draws[_d];
            keep++;
        }
        for (u32 _m = 0; _m < nm && keep < MAX_DRAWS; _m++)
            s_d3d.draws[keep++] = moved[_m];
        s_d3d.draw_count = keep;
      } }

    /* Render-to-texture pre-pass: make sure an offscreen RT resource exists for
     * every non-display surface targeted this frame (so draws binding it as a
     * texture can resolve to it below, whatever the op order). */
    for (int _i = 0; _i < MAX_OFF_RTS; _i++) s_d3d.off_rt[_i].used = 0;
    for (u32 _d = 0; _d < s_d3d.draw_count && _d < MAX_DRAWS; _d++) {
        D3D12DrawRecord* dr = &s_d3d.draws[_d];
        if (dr->is_vp && dr->rt_off)
            off_rt_get(dr->rt_off, dr->rt_w, dr->rt_h, dr->rt_fmt);
        for (int _m = 0; _m < 3; _m++)
            if (dr->is_vp && dr->rt_mrt[_m])
                off_rt_get(dr->rt_mrt[_m], dr->rt_w, dr->rt_h, dr->rt_fmt);
    }

    /* Per-frame VP textures + guest-FP pipelines: for each VP draw, upload the
     * texture it had bound at submit time into a slot (SRV heap 1+slot; plasma
     * animates so contents re-upload every frame) and pre-build its FP PSO.
     * A texture whose offset matches an offscreen RT samples the RT directly
     * (tex_slot 1000+idx) -- no guest-memory upload. */
    double _pre0 = perf_on() ? perf_now() : 0.0;
    RF_MARK(4);
    /* [NHL local patch #115] Wait for the previous frame's GPU work before this
     * pre-pass. It CPU-writes shared per-frame state that frame may still be
     * reading: the per-draw SRV windows (DRAW_SRV_BASE + d*16, one set, no
     * parity) and the texture-slot staging/resources (a slot unused this frame
     * is re-filled with another texture). With two frames in flight the GPU
     * sampled the NEXT frame's descriptors -- textures flickering on stable
     * models (play164). RSX_FRAME_OVERLAP=1 restores the overlap. */
    { static int ov = -1; if (ov < 0) ov = getenv("RSX_FRAME_OVERLAP") ? 1 : 0;
      if (!ov) wait_for_gpu(); }
    RF_MARK(5);
    for (int _i = 0; _i < VP_TEX_SLOTS; _i++) s_d3d.vp_tex[_i].used = 0;
    /* [#185] NHL_SRV_REUSE=0: every draw writes its own 16 descriptors (old path) */
    static int s_srv_reuse = -1;
    if (s_srv_reuse < 0) { const char* e = getenv("NHL_SRV_REUSE"); s_srv_reuse = (e && *e == '0') ? 0 : 1; }
    int _prev_d = -1;
    for (u32 _d = 0; _d < s_d3d.draw_count && _d < MAX_DRAWS; _d++) {
        D3D12DrawRecord* dr = &s_d3d.draws[_d];
        dr->srv_win = _d;   /* #185 */
        /* [#121 diag] JERSEYDBG=<hex vs hash>[,...]: once per distinct texture those
         * draws bind (units 0-6): size/format, zero fraction of the guest bytes,
         * and whether the address range overlaps a surface we rendered into
         * (RT contents never reach guest memory -- a CPU/SPU consumer would read
         * zeros). */
        { static int nj = -1; static u32 jl[8];
          if (nj < 0) { nj = 0; const char* e = getenv("JERSEYDBG");
              while (e && *e && nj < 8) { char* end; unsigned long v = strtoul(e, &end, 16);
                  if (end == e) break; jl[nj++] = (u32)v; e = (*end == ',') ? end + 1 : end; } }
          extern uint8_t* vm_base;
          if (nj > 0 && vm_base && dr->is_vp && !dr->is_clear && dr->vs_idx >= 0 && dr->vs_idx < s_d3d.vp_vs_n) {
              const u32 vh = s_d3d.vp_vs[dr->vs_idx].hash; int hit = 0;
              for (int k = 0; k < nj; k++) if (jl[k] == vh) hit = 1;
              static u32 seen[256]; static int ns = 0;
              for (int u = 0; hit && u < 7; u++) {
                  if (!dr->tex[u].set || !dr->tex[u].off || !dr->tex[u].w) continue;
                  int f = 0; for (int i = 0; i < ns; i++) if (seen[i] == dr->tex[u].raw) f = 1;
                  if (f || ns >= 256) continue;
                  seen[ns++] = dr->tex[u].raw;
                  rsx_tex_layout tl; rsx_texture_layout(dr->tex[u].fmt, dr->tex[u].w, dr->tex[u].h, &tl);
                  const u32 nb = tl.compressed ? tl.row_bytes * tl.rows : dr->tex[u].w * dr->tex[u].h * tl.bytes_per_texel;
                  u32 z = 0; for (u32 i = 0; i < nb; i++) if (!vm_base[dr->tex[u].off + i]) z++;
                  int ov = -1; u32 ovoff = 0;
                  for (int r = 0; r < MAX_OFF_RTS; r++) {
                      const OffRT* R = &s_d3d.off_rt[r];
                      if (!R->res) continue;
                      const u32 rs = R->off, re = R->off + R->w * R->h * 4u;
                      if (dr->tex[u].raw < re && dr->tex[u].raw + nb > rs) { ov = r; ovoff = R->off; break; }
                  }
                  fprintf(stderr, "[JERSEY] vs=0x%08X unit %d raw=0x%08X %ux%u fmt=0x%02X mips=%u bytes=%u zero=%.1f%% %s%c",
                          vh, u, dr->tex[u].raw, dr->tex[u].w, dr->tex[u].h, dr->tex[u].fmt, dr->tex[u].mips, nb,
                          nb ? 100.0 * z / nb : 0.0, ov >= 0 ? "OVERLAPS-RT" : "", 10);
                  if (ov >= 0) fprintf(stderr, "[JERSEY]    overlaps RT %d at 0x%08X %ux%u%c", ov, ovoff,
                                       s_d3d.off_rt[ov].w, s_d3d.off_rt[ov].h, 10);
              } } }
        /* [#117 diag] TEXRACE_VS: did the texture bytes change since record? */
        if (dr->rec_csum_on && dr->is_vp && !dr->is_clear) {
            extern uint8_t* vm_base;
            static u32 n_chk = 0, n_bad = 0, n_log = 0; static ULONGLONG t0 = 0;
            for (int u = 0; u < 4 && vm_base; u++) {
                if (!dr->tex[u].set || !dr->tex[u].off || !dr->tex[u].w || !dr->tex[u].h) continue;
                rsx_tex_layout tl; rsx_texture_layout(dr->tex[u].fmt, dr->tex[u].w, dr->tex[u].h, &tl);
                const u32 nb = tl.compressed ? tl.row_bytes * tl.rows : dr->tex[u].w * dr->tex[u].h * tl.bytes_per_texel;
                const u32 cs = tex_csum(vm_base + dr->tex[u].off, nb);
                n_chk++;
                if (cs != dr->rec_csum[u]) { n_bad++;
                    if (n_log++ < 40)
                        fprintf(stderr, "[TEXRACE] frame %llu draw %u unit %d off=0x%08X %ux%u fmt=0x%02X changed between record and upload%c",
                                (unsigned long long)s_d3d.frame_count, _d, u, dr->tex[u].off,
                                dr->tex[u].w, dr->tex[u].h, dr->tex[u].fmt, 10); }
            }
            const ULONGLONG t = GetTickCount64(); if (!t0) t0 = t;
            if (t - t0 >= 5000) { fprintf(stderr, "[TEXRACE] %u of %u checked texture binds changed between record and upload (5 s)%c",
                                          n_bad, n_chk, 10); n_bad = n_chk = 0; t0 = t; }
        }
        if (!dr->is_vp || dr->is_clear) continue;
        /* Debug: RTT_VIEWRT=<hex raw offset> makes display draws sample that
         * offscreen RT at t0 (the composite blit then shows it fullscreen). */
        { static const char* vr = (const char*)1;     /* hoisted: this runs per DRAW */
          if (vr == (const char*)1) vr = getenv("RTT_VIEWRT");
          if (vr && dr->rt_off == 0) {
              dr->tex[0].raw = (u32)strtoul(vr, NULL, 16);
              dr->tex[0].off = 0;
              dr->tex[0].set = 1;
          } }
        /* SCREENTEX=1: report draws that sample a DISPLAY-SIZED texture. On RSX
         * a title renders a reflection/refraction into a surface and then
         * samples it; our backend renders into a D3D backbuffer, so the guest
         * memory that sampler reads is never written and the texture comes back
         * empty. Finding which geometry does it is the first step to feeding it
         * the rendered image instead. */
        /* EMPTYTEX=1: which geometry binds a texture whose source is all zero.
         * An unresolved texture renders untextured, so this names the surfaces
         * affected instead of leaving it to guesswork. */
        { static int ed = -1;
          if (ed < 0) { const char* e = getenv("EMPTYTEX"); ed = e ? atoi(e) : 0; }
          extern uint8_t* vm_base;
          if (ed && vm_base) for (int _u = 0; _u < 4; _u++) {
              if (!dr->tex[_u].set || !dr->tex[_u].off) continue;
              u32 nz = 0, sz = dr->tex[_u].w * dr->tex[_u].h * 4u;
              for (u32 i2 = 0; i2 < sz && i2 < 0x20000u; i2 += 997)
                  if (vm_base[dr->tex[_u].off + i2]) { nz = 1; break; }
              if (!nz) {
                  static u32 seen[24]; static int ns = 0; int known = 0;
                  for (int k = 0; k < ns; k++) if (seen[k] == dr->fp_addr) known = 1;
                  if (!known && ns < 24) { seen[ns++] = dr->fp_addr;
                      /* Scan a window around the resolved address: if the pass
                       * that fills this surface ran but landed elsewhere, the
                       * data is nearby; if the whole window is zero it was never
                       * produced. */
                      /* Same offset through BOTH context DMAs: a texture the
                       * guest put in main memory but that we resolve as LOCAL
                       * reads as untouched VRAM, i.e. all zeros. */
                      { extern u32 cellGcmResolveLocated(int, u32);
                        u32 la = cellGcmResolveLocated(1, dr->tex[_u].raw);
                        u32 ma = cellGcmResolveLocated(0, dr->tex[_u].raw);
                        u32 ln = 0, mn = 0;
                        for (u32 i4 = 0; i4 < 0x20000u; i4 += 997) {
                            if (vm_base[la + i4]) ln++;
                            if (vm_base[ma + i4]) mn++;
                        }
                        fprintf(stderr, "[TEXLOC] raw=0x%08X LOCAL@0x%08X nz=%u  MAIN@0x%08X nz=%u%c",
                                dr->tex[_u].raw, la, ln, ma, mn, 10); }
                      { u32 base = dr->tex[_u].off & ~0xFFFFFu;
                        for (u32 w = 0; w < 0x400000u; w += 0x100000u) {
                            u32 nzc = 0;
                            for (u32 i3 = 0; i3 < 0x100000u; i3 += 1021)
                                if (vm_base[base + w + i3]) nzc++;
                            fprintf(stderr, "[EMPTYSCAN] 0x%08X..+1MB nonzero %u/1027%c",
                                    base + w, nzc, 10);
                        } }
                      fprintf(stderr, "[EMPTYTEX] fp=0x%X unit=%d raw=0x%08X res=0x%08X %ux%u"
                                      " fmt=0x%02X verts=%u vp=%u,%u %ux%u%c",
                              dr->fp_addr, _u, dr->tex[_u].raw, dr->tex[_u].off, dr->tex[_u].w,
                              dr->tex[_u].h, dr->tex[_u].fmt, dr->vertex_count,
                              dr->vp_x, dr->vp_y, dr->vp_w, dr->vp_h, 10); }
              }
          } }
        { static int sd = -1;
          if (sd < 0) { const char* e = getenv("SCREENTEX"); sd = e ? atoi(e) : 0; }
          if (sd) for (int _u = 0; _u < 4; _u++)
              if (dr->tex[_u].set && dr->tex[_u].w == s_d3d.width &&
                  dr->tex[_u].h == s_d3d.height) {
                  static u32 seen[16]; static int ns = 0; int known = 0;
                  for (int k = 0; k < ns; k++) if (seen[k] == dr->fp_addr) known = 1;
                  if (!known && ns < 16) { seen[ns++] = dr->fp_addr;
                      fprintf(stderr, "[SCREENTEX] unit=%d raw=0x%08X %ux%u fmt=0x%02X"
                                      " fp=0x%X verts=%u vp=%u,%u %ux%u%c",
                              _u, dr->tex[_u].raw, dr->tex[_u].w, dr->tex[_u].h,
                              dr->tex[_u].fmt, dr->fp_addr, dr->vertex_count,
                              dr->vp_x, dr->vp_y, dr->vp_w, dr->vp_h, 10); }
              } }
        /* Fill this draw's t0-t3 SRV window (DRAW_SRV_BASE + d*4): each unit
         * resolves to an offscreen RT (sampled directly), an uploaded guest
         * texture, or a null SRV. */
        double _sv0 = perf_on() ? perf_now() : 0.0;
        /* [NHL local patch #185] Same texture bindings as the previous draw: bind its
         * SRV window and copy its resolved per-unit results instead of writing 16
         * descriptors and re-resolving every unit (play253: this loop was ~41% of
         * frame time in the ~10k-draw pre-game cinematic, ~16 CreateShaderResourceView
         * calls per draw). */
        /* [#211] NHL_KEYSTATE=<hex key>: that program's per-draw raster state (first 24 draws) */
        { static u32 kk = 0xFFFFFFFFu; static int kn = 0;
          if (kk == 0xFFFFFFFFu) { const char* e = getenv("NHL_KEYSTATE"); kk = e ? (u32)strtoul(e, NULL, 16) : 0u; }
          if (kk && dr->fp_key == kk && kn < 24) { kn++;
              const u32 x = (u32)dr->fp_exp32, dk = (x >> 20) & 0x1Fu;
              fprintf(stderr, "[KEYSTATE] key=0x%08X fp=0x%08X rt=0x%08X n=%u blend=%d bk=0x%X alphatest=%u func=0x%02X ref=%u a2c=%u depth test=%u func=%u write=%u cmask=0x%X t0=0x%08X%c",
                      dr->fp_key, dr->fp_addr, dr->rt_off, dr->vertex_count, dr->blend, (unsigned)dr->blend_key,
                      (dr->alpha_ctl >> 16) & 1u, (dr->alpha_ctl >> 8) & 0xFFu, dr->alpha_ctl & 0xFFu, (x >> 25) & 1u,
                      dk & 1u, (dk >> 1) & 7u, (dk >> 4) & 1u, (unsigned)dr->cmask, dr->tex[0].raw, 10); } }
        int _reuse = 0;
        if (s_srv_reuse && _prev_d >= 0 && dr_tex_same(dr, &s_d3d.draws[_prev_d])) {
            const D3D12DrawRecord* p = &s_d3d.draws[_prev_d];
            dr->srv_win = p->srv_win;
            memcpy(dr->tex_rt, p->tex_rt, sizeof dr->tex_rt);
            memcpy(dr->bound_kind, p->bound_kind, sizeof dr->bound_kind);
            memcpy(dr->bound_id, p->bound_id, sizeof dr->bound_id);
            dr->zmask = p->zmask;   /* #190 */
            dr->dcz = p->dcz;       /* #209 */
            _reuse = 1;
        }
        if (!_reuse) { dr->zmask = 0; dr->dcz = 0; }   /* #190/#209: set by the unit loop below */
        /* [#216 diag] F3 shine test: on draws that bind a 2048x2048 kit texture (player /
         * gear programs), give one texture role a null SRV (samples 0). */
        /* [NHL local patch #298] NHL_ICE_BOUNCE=0 (launcher "Ice light on players" off): start in
         * mode 4 = no bounce light from the rink image on player / gear draws. The light is
         * authentic (matches RPCS3); off is the user's style choice (play318). */
        static int s_ice_init = 0;
        if (!s_ice_init) {
            s_ice_init = 1;
            const char* e = getenv("NHL_ICE_BOUNCE");
            if (e && *e == '0') { InterlockedExchange(&s_shine_live, 4); fprintf(stderr, "[ICE-BOUNCE] off (#298)%c", 10); }
        }
        const LONG _shm = s_shine_live;
        int _player = 0;
        if (_shm && !_reuse)
            for (int k = 0; k < D3D_TEX_UNITS; k++)
                if (dr->tex[k].set && dr->tex[k].w == 2048 && dr->tex[k].h == 2048) { _player = 1; break; }
        for (int _u = 0; !_reuse && _u < D3D_TEX_UNITS; _u++) {
            u32 wslot = DRAW_SRV_BASE + _d * D3D_TEX_UNITS + (u32)_u;
            dr->tex_rt[_u] = -1;
            if (_u < 4) { dr->bound_kind[_u] = 0; dr->bound_id[_u] = 0; }   /* #118 */
            if (_player && dr->tex[_u].set) {
                const u32 bf = dr->tex[_u].fmt & 0x9Fu, tw = dr->tex[_u].w, th = dr->tex[_u].h;
                const int cb = dr->tex[_u].cube ? 1 : 0;
                const int hit =
                    (_shm == 1 && !cb && tw == 128 && th == 128 && bf == 0x88 && dr->tex[_u].mips <= 1) ||
                    (_shm == 2 && !cb && tw == 256 && th == 256 && bf == 0x88 && dr->tex[_u].mips > 1) ||
                    (_shm == 3 && cb) ||
                    (_shm == 4 && !cb && tw == 512 && th == 512 && bf == 0x88 && ((dr->tex[_u].addr >> 20) & 7u) == 7u) ||
                    (_shm == 5 && !cb && tw == 2048 && th == 2048 && bf == 0x88);
                if (hit) {
                    srv_write_ex(wslot, NULL, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING, cb);
                    continue;
                }
            }
            /* Display-sized sampler source -> the rendered frame. */
            /* [NHL local patch] Never for B8: a display-sized single-channel
             * texture is a video plane, not the colour frame. NHL's VP6 movies
             * bind their 1280x720 Y plane as B8, and swapping in the previous
             * frame fed the movie back into itself -- luma blew out to
             * black/white within a few frames while the (640x360) chroma
             * planes still read correctly. */
            /* [NHL local patch #52] Size alone is not enough either. NHL's boot
             * splashes (the white NHL LEGACY slide, the copyright slide) are
             * 1280x720 DXT1 textures (fmt 0xA6) -- display-sized, so they were
             * swapped for the previous frame (black at boot) and never shown.
             * Compressed formats can never be render targets, and by default
             * the swap now also needs the texture to BE a registered display
             * buffer. SCREEN_AS_TEX=2 restores the size-only match; 0 disables. */
            if (dr->tex[_u].set && s_screen_copy && (dr->tex[_u].fmt & 0x9F) != 0x81 &&
                !((dr->tex[_u].fmt & 0x9F) >= 0x86 && (dr->tex[_u].fmt & 0x9F) <= 0x88) &&
                dr->tex[_u].w == s_d3d.width && dr->tex[_u].h == s_d3d.height) {
                static int en = -1;
                if (en < 0) { const char* e = getenv("SCREEN_AS_TEX"); en = e ? atoi(e) : 1; }
                extern int cellGcmOffsetIsDisplay(u32 offset);
                int is_disp = cellGcmOffsetIsDisplay(dr->tex[_u].raw);
                /* [NHL local patch #213] A display buffer that an NV3089 resolve blit wrote
                 * THIS frame holds that resolve, not the previous presented frame. NHL's
                 * pre-game transition resolves the MSAA scene (0x02980000) straight into
                 * the display buffer it is about to show (0x00790000 / 0x00010000 /
                 * 0x003D0000 in rotation), then its bright pass + composite read that buffer
                 * (play262 frames 50-67). Swapping in the previous frame fed the composite's
                 * own output + bloom back every frame = the runaway white bloom instead of
                 * the fade. Such a read now takes the blit's RT (the path normal frames use
                 * for 0x047D0000). NHL_DISP_RESOLVE=0 = old screen-copy substitution. */
                int disp_resolved = 0;
                if (is_disp) {
                    static int dres = -1;
                    if (dres < 0) { const char* e = getenv("NHL_DISP_RESOLVE"); dres = (e && *e == '0') ? 0 : 1; }
                    const int brt = dres ? off_rt_find(dr->tex[_u].raw) : -1;
                    if (brt >= 0 && s_d3d.off_rt[brt].blit_frame == s_d3d.frame_count + 1u) {
                        static u32 seen[8]; static int ns = 0; int f = 0;
                        for (int i = 0; i < ns; i++) if (seen[i] == dr->tex[_u].raw) f = 1;
                        if (!f && ns < 8) { seen[ns++] = dr->tex[_u].raw;
                            fprintf(stderr, "[DISPRESOLVE] unit %d fp=0x%X reads display 0x%08X = this frame's resolve blit (RT %d)%c",
                                    _u, dr->fp_addr, dr->tex[_u].raw, brt, 10); }
                        disp_resolved = 1;   /* skip the substitution: the RT path below binds it */
                    }
                }
                if (en == 1 && !is_disp) {
                    static u32 seen[16]; static int ns = 0; int f = 0;
                    for (int i = 0; i < ns; i++) if (seen[i] == dr->tex[_u].off) f = 1;
                    if (!f && ns < 16) { seen[ns++] = dr->tex[_u].off;
                        fprintf(stderr, "[SCREENTEX] kept own texture at unit %d for fp=0x%X"
                                        " (%ux%u fmt=0x%02X raw=0x%08X: not a display buffer)%c",
                                _u, dr->fp_addr, dr->tex[_u].w, dr->tex[_u].h, dr->tex[_u].fmt,
                                dr->tex[_u].raw, 10); }
                }
                if (!disp_resolved && (en == 2 || (en == 1 && is_disp))) {
                    { static u32 seen[16]; static int ns = 0; int f = 0;   /* [NHL local patch] per distinct texture */
                      for (int i = 0; i < ns; i++) if (seen[i] == dr->tex[_u].off) f = 1;
                      if (!f && ns < 16) { seen[ns++] = dr->tex[_u].off;
                        fprintf(stderr, "[SCREENTEX] bound frame copy at unit %d for"
                                        " fp=0x%X (%ux%u fmt=0x%02X off=0x%08X)%c", _u, dr->fp_addr,
                                dr->tex[_u].w, dr->tex[_u].h, dr->tex[_u].fmt, dr->tex[_u].off, 10); } }
                    srv_write(wslot, s_screen_copy, DXGI_FORMAT_R8G8B8A8_UNORM,
                              D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING);
                    continue;
                }
            }
            /* A sampler whose guest buffer was never written, at the size of a
             * reduced-viewport pass we captured: that pass IS its producer. */
            if (dr->tex[_u].set && dr->tex[_u].off && s_subvp_n) {
                int hit = -1;
                for (int i = 0; i < s_subvp_n; i++)
                    if (s_subvp[i].res && s_subvp[i].w == dr->tex[_u].w &&
                        s_subvp[i].h == dr->tex[_u].h) { hit = i; break; }
                if (hit >= 0) {
                    extern uint8_t* vm_base;
                    u32 nz = 0, sz = dr->tex[_u].w * dr->tex[_u].h * 4u;
                    for (u32 i5 = 0; i5 < sz && i5 < 0x20000u; i5 += 997)
                        if (vm_base[dr->tex[_u].off + i5]) { nz = 1; break; }
                    if (!nz) {
                        { static int _n = 0; if (getenv("SUBVP_DBG") && _n++ < 8)
                            fprintf(stderr, "[SUBVP] unit %d fp=0x%X <- captured %ux%u%c",
                                    _u, dr->fp_addr, dr->tex[_u].w, dr->tex[_u].h, 10); }
                        srv_write(wslot, s_subvp[hit].res, DXGI_FORMAT_R8G8B8A8_UNORM,
                                  D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING);
                        continue;
                    }
                }
            }
            /* [#200] NHL_CUBEPATH=1: for each distinct cube texture a draw binds, which
             * path it takes (render target / uploaded slot / null SRV) and its format --
             * the player FP's ambient cube (unit 6) samples black. */
            #define CUBEPATH(what) do { static int _cp = -1; if (_cp < 0) _cp = getenv("NHL_CUBEPATH") ? 1 : 0; \
                if (_cp && dr->tex[_u].cube) { static u32 _s[32]; static int _n = 0; int _k = 0; \
                    for (int _i = 0; _i < _n; _i++) if (_s[_i] == dr->tex[_u].off) _k = 1; \
                    if (!_k && _n < 32) { _s[_n++] = dr->tex[_u].off; \
                        fprintf(stderr, "[CUBEPATH] %s unit %d fp=0x%X off=0x%08X raw=0x%08X %ux%u fmt=0x%02X mips=%u cubemask=0x%X%c", \
                                what, _u, dr->fp_addr, dr->tex[_u].off, dr->tex[_u].raw, dr->tex[_u].w, dr->tex[_u].h, \
                                dr->tex[_u].fmt, (unsigned)dr->tex[_u].mips, dr_cube_mask(dr), 10); } } } while (0)
            if (dr->tex[_u].set) {
                int rt = off_rt_find(dr->tex[_u].raw);
                if (rt >= 0 && rt_alias_reject(dr, _u, rt)) rt = -1;   /* #172 */
                if (rt >= 0) {
                    CUBEPATH("render-target(2D SRV)");
                    dr->tex_rt[_u] = rt;
                    { extern unsigned cellGcm_flip_request_count(void);   /* #243: sampled = in use */
                      s_d3d.off_rt[rt].last_flip = cellGcm_flip_request_count(); }
                    DXGI_FORMAT rf = (DXGI_FORMAT)s_d3d.off_rt[rt].dxgi;
                    /* [#210c] a gamma-flagged read of a render target (the post chain reading the
                     * sRGB-encoded scene) decodes too, like a texture (#210) */
                    { static int tg = -1;
                      if (tg < 0) { const char* e = getenv("NHL_TEX_GAMMA"); tg = (e && *e == '0') ? 0 : 1; }
                      const u32 gm = (dr->tex[_u].addr >> 20) & 0xFu;
                      if (gm) { static u32 seen[32]; static int ns = 0; int f = 0;
                          const u32 key = s_d3d.off_rt[rt].off ^ gm;
                          for (int i = 0; i < ns; i++) if (seen[i] == key) f = 1;
                          if (!f && ns < 32) { seen[ns++] = key;
                              fprintf(stderr, "[TEXGAMMA] RT 0x%08X %ux%u gamma=0x%X fp=0x%X%s%c", s_d3d.off_rt[rt].off,
                                      s_d3d.off_rt[rt].w, s_d3d.off_rt[rt].h, gm, dr->fp_addr,
                                      (tg && (gm & 7u) == 7u && rf == DXGI_FORMAT_R8G8B8A8_UNORM) ? " -> sRGB view" : "", 10); } }
                      if (tg && (gm & 7u) == 7u && rf == DXGI_FORMAT_R8G8B8A8_UNORM) rf = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; }
                    srv_write(wslot, s_d3d.off_rt[rt].res, rf, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING);
                    if (_u < 4) { dr->bound_kind[_u] = 1; dr->bound_id[_u] = (u16)rt; }   /* #118 */
                    continue;
                }
                /* [#209] a depth surface read as ARGB8: the repacked colour copy of our depth */
                if (depthcol_on() && (dr->tex[_u].fmt & 0x9F) == 0x85 && !dr->tex[_u].cube &&
                    depthcol_alias(dr->tex[_u].raw)) {
                    const int zi = depthcol_source(dr->tex[_u].raw, dr->tex[_u].w, dr->tex[_u].h);
                    DepthCol* c = (zi >= 0) ? depthcol_get(zi) : NULL;
                    if (c) {
                        srv_write(wslot, c->res, DXGI_FORMAT_R8G8B8A8_UNORM, rsx_remap_to_d3d(dr->tex[_u].ctrl1, 0x85));
                        dr->dcz = zi + 1;
                        static u32 seen[16][2]; static int ns = 0; int f = 0;
                        for (int i = 0; i < ns; i++) if (seen[i][0] == dr->tex[_u].raw && seen[i][1] == dr->fp_addr) f = 1;
                        if (!f && ns < 16) { seen[ns][0] = dr->tex[_u].raw; seen[ns][1] = dr->fp_addr; ns++;
                            fprintf(stderr, "[DEPTHCOL] unit %d fp=0x%X samples 0x%08X %ux%u remap=0x%04X <- depth surface %d (zeta 0x%08X)%c",
                                    _u, dr->fp_addr, dr->tex[_u].raw, dr->tex[_u].w, dr->tex[_u].h, dr->tex[_u].ctrl1 & 0xFFFFu,
                                    zi, zi == DC_SNAP ? 0u : s_shz[zi].off, 10); }   /* surface 8 = scene depth snapshot */
                        continue;
                    }
                }
                u32 _bf = dr->tex[_u].fmt & 0x9F;
                if (_bf >= 0x90 && _bf <= 0x93 && !dr->tex[_u].cube && shadows_on()) {   /* #190 */
                    const int zi = shadowz_find(dr->tex[_u].raw);
                    if (zi >= 0) {
                        srv_write(wslot, s_shz[zi].res, DXGI_FORMAT_R32_FLOAT, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING);
                        dr->zmask |= 1u << _u;
                        static int n = 0;
                        if (n++ < 8) fprintf(stderr, "[SHADOWZ] unit %d fp=0x%X samples shadow surface %d (raw 0x%08X %ux%u fmt=0x%02X addr=0x%08X ctl0=0x%08X)%c",
                                             _u, dr->fp_addr, zi, dr->tex[_u].raw, dr->tex[_u].w, dr->tex[_u].h,
                                             dr->tex[_u].fmt, dr->tex[_u].addr, dr->tex[_u].ctl0, 10);
                        continue;
                    }
                }
                if (_bf >= 0x90 && _bf <= 0x93 && !dr->tex[_u].cube && shadow_lit_bind(wslot)) {   /* #188 */
                    static int n = 0;
                    if (n++ < 8) fprintf(stderr, "[SHADOWTEX] unit %d fp=0x%X raw=0x%08X %ux%u fmt=0x%02X -> lit (no shadow map)%c",
                                         _u, dr->fp_addr, dr->tex[_u].raw, dr->tex[_u].w, dr->tex[_u].h, dr->tex[_u].fmt, 10);
                    continue;
                }
                if (dr->tex[_u].off &&
                    (_bf == 0x81 || _bf == 0x85 || _bf == 0x8B ||
                     (_bf >= 0x86 && _bf <= 0x88))) {
                    double _tt = perf_on() ? perf_now() : 0.0;
                    int _cube = dr->tex[_u].cube && (dr_cube_mask(dr) & (1u << _u));
                    const unsigned long long _hz0 = ps3_qpc_us();   /* #122 */
                    s_texdump_draw_rt = dr->rt_off;                  /* #175 */
                    s_tex_snap = _cube ? NULL : dr->tex[_u].snap;     /* #179 */
                    int ts = vp_upload_tex_slot(dr->tex[_u].off, dr->tex[_u].w,
                                                dr->tex[_u].h, dr->tex[_u].fmt,
                                                _cube, dr->tex[_u].mips, dr->tex[_u].pitch);
                    HITCH_ADD(g_hitch_tex_us, ps3_qpc_us() - _hz0); HITCH_ADD(g_hitch_tex_n, 1);
                    if (perf_on()) { s_perf_tex += perf_now() - _tt; s_perf_ntex++;
                        s_perf_texbytes += (u64)dr->tex[_u].w * dr->tex[_u].h * 4u; }
                    /* Both forms of the offset are in hand here; the filters
                     * compare the BOUND one. */
                    if (s_duck_off && dr->tex[_u].off == s_duck_off)
                        s_duck_raw = dr->tex[_u].raw;
                    if (ts >= 0) {
                        DXGI_FORMAT sf =
                            (_bf == 0x85) ? DXGI_FORMAT_R8G8B8A8_UNORM :
                            (_bf == 0x8B) ? DXGI_FORMAT_R8G8_UNORM :
                            (_bf == 0x86) ? DXGI_FORMAT_BC1_UNORM :
                            (_bf == 0x87) ? DXGI_FORMAT_BC2_UNORM :
                            (_bf == 0x88) ? DXGI_FORMAT_BC3_UNORM :
                                            DXGI_FORMAT_R8_UNORM;
                        /* [NHL local patch #210] Texture gamma (SET_TEXTURE_ADDRESS bits 20-23, libgcm
                         * CELL_GCM_TEXTURE_GAMMA_R/G/B/A = 1/2/4/8; #210b: play304's masks are all 0x7
                         * = RGB, alpha linear): RSX decodes those channels sRGB -> linear
                         * on fetch. Not done here, so since #208's sRGB output every gamma texture
                         * was encoded twice (play303: washed-out ice, salmon Carolina red). R+G+B
                         * flagged -> the *_SRGB view (alpha stays linear, as on RSX); partial masks
                         * are only logged. NHL_TEX_GAMMA=0 = old linear reads. */
                        { static int tg = -1;
                          if (tg < 0) { const char* e = getenv("NHL_TEX_GAMMA"); tg = (e && *e == '0') ? 0 : 1; }
                          const u32 gm = (dr->tex[_u].addr >> 20) & 0xFu;
                          if (gm) {
                              static u32 seen[32]; static int ns = 0; int f = 0;
                              const u32 key = (_bf << 8) | gm;
                              for (int i = 0; i < ns; i++) if (seen[i] == key) f = 1;
                              if (!f && ns < 32) { seen[ns++] = key;
                                  fprintf(stderr, "[TEXGAMMA] fmt=0x%02X gamma=0x%X (R%u G%u B%u A%u) off=0x%08X %ux%u fp=0x%X%s%c",
                                          _bf, gm, gm & 1u, (gm >> 1) & 1u, (gm >> 2) & 1u, (gm >> 3) & 1u, dr->tex[_u].off,
                                          dr->tex[_u].w, dr->tex[_u].h, dr->fp_addr,
                                          (tg && (gm & 0x7u) == 0x7u && sf != DXGI_FORMAT_R8_UNORM && sf != DXGI_FORMAT_R8G8_UNORM) ? " -> sRGB view" : "", 10); }
                          }
                          /* [#214 diag] F4 (NHL_LIVEKEYS) cycles: 0 = as above, 1 = no sRGB view for
                           * DXT5 (0x88) gamma textures, 2 = none for any (gear "detail too strong" A/B) */
                          const LONG gl = s_texgamma_live;
                          if (tg && (gm & 0x7u) == 0x7u && gl != 2 && !(gl == 1 && _bf == 0x88))
                              sf = (sf == DXGI_FORMAT_R8G8B8A8_UNORM) ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
                                 : (sf == DXGI_FORMAT_BC1_UNORM)      ? DXGI_FORMAT_BC1_UNORM_SRGB
                                 : (sf == DXGI_FORMAT_BC2_UNORM)      ? DXGI_FORMAT_BC2_UNORM_SRGB
                                 : (sf == DXGI_FORMAT_BC3_UNORM)      ? DXGI_FORMAT_BC3_UNORM_SRGB : sf; }
                        /* [NHL local patch] B8 goes through the TEXTURE_CONTROL1
                         * crossbar like every other format. Replicating the byte
                         * into all four lanes ignored the guest's force-ZERO/ONE
                         * selects: NHL's movie shader samples Y, U and V as three
                         * B8 planes whose remaps park each value in its own lane
                         * (zero elsewhere) and adds them, so with every lane = the
                         * byte each colour became ~Y+U+V -- the blown-out
                         * black/white movies. B8_REPLICATE=1 restores the old
                         * mapping. */
                        static int s_b8rep = -1;
                        if (s_b8rep < 0) s_b8rep = getenv("B8_REPLICATE") ? 1 : 0;
                        if (_u < 4) { dr->bound_kind[_u] = 2; dr->bound_id[_u] = (u16)ts; }   /* #118 */
                        { const VPTexSlot* c = &s_d3d.vp_tex[ts];   /* #118 [SLOTMISMATCH] */
                          if (c->key != dr->tex[_u].off || c->w != dr->tex[_u].w || c->h != dr->tex[_u].h ||
                              c->fmt != dr->tex[_u].fmt) {
                              static int n = 0;
                              if (n++ < 30) fprintf(stderr, "[SLOTMISMATCH] draw %u unit %d wants off=0x%08X %ux%u fmt=0x%02X; slot %d holds key=0x%08X %ux%u fmt=0x%02X%c",
                                                    _d, _u, dr->tex[_u].off, dr->tex[_u].w, dr->tex[_u].h, dr->tex[_u].fmt,
                                                    ts, c->key, c->w, c->h, c->fmt, 10); } }
                        CUBEPATH(_cube ? "uploaded(cube SRV)" : "uploaded(2D SRV)");
                        srv_write_ex(wslot, s_d3d.vp_tex[ts].res, sf,
                                  (_bf == 0x81 && s_b8rep) ? 0x1000
                                                : rsx_remap_to_d3d(dr->tex[_u].ctrl1, _bf),
                                  _cube);
                        continue;
                    }
                }
            }
            /* TEX_BINDDBG=<N>: a unit that ends up with a NULL SRV. The draw is
             * still recorded, lit and transformed -- it just samples nothing,
             * which renders as a solid black object. */
            { static int cap = -1, n = 0;
              if (cap < 0) { const char* e = getenv("TEX_BINDDBG"); cap = e ? atoi(e) : 0; }
              if (cap && n < cap && dr->tex[_u].set) { n++;
                fprintf(stderr, "[TEXBIND] null SRV unit=%d raw=0x%X off=0x%X %ux%u fmt=0x%02X%c",
                        _u, dr->tex[_u].raw, dr->tex[_u].off, dr->tex[_u].w,
                        dr->tex[_u].h, dr->tex[_u].fmt, 10); } }
            CUBEPATH("NULL SRV");
            #undef CUBEPATH
            srv_write(wslot, NULL, DXGI_FORMAT_R8G8B8A8_UNORM,
                      D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING);
        }
        if (perf_on()) s_perf_srv += perf_now() - _sv0;
        _prev_d = (int)_d;   /* #185 */
        double _ps0 = perf_on() ? perf_now() : 0.0;
        s_fp_snap_cur = dr->fp_snap; s_fp_snap_cur_len = dr->fp_snap_len;   /* #82 */ s_fp_cur_rt = dr->rt_off;   /* #86 */ fp_shadow_from_draw(dr);   /* #190 */
        s_fxc_collect = 1;   /* #124: new PS text is queued, compiled in one batch below */
        s_async_sync = draw_essential(dr);   /* #225 */
        if (dr->fp_addr) vp_get_fp_pso(dr->vs_idx, dr->fp_addr, dr->blend_key,
                                       dr_num_rts(dr),
                                       dr->rt_off ? rsx_surface_dxgi(dr->rt_fmt)
                                                  : DXGI_FORMAT_R8G8B8A8_UNORM,
                                       ms_exp(dr, dr->fp_exp32), dr->cmask, dr->cull, dr_cube_mask(dr), dr->stencil_key);
        s_fxc_collect = 0;
        s_async_sync = 1;
        if (perf_on()) s_perf_pso += perf_now() - _ps0;
    }
    fxc_run_jobs();   /* #124: this frame's new fragment shaders, in parallel */
    RF_MARK(6);

    /* Transition render target to RENDER_TARGET state */
    D3D12_RESOURCE_BARRIER barrier = {0};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = s_d3d.render_targets[fi];
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &barrier);

    /* Get RTV handle for current frame */
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle;
    s_d3d.rtv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.rtv_heap, &rtv_handle);
    rtv_handle.ptr += fi * s_d3d.rtv_descriptor_size;

    /* Get DSV handle (single depth buffer shared across frames) */
    D3D12_CPU_DESCRIPTOR_HANDLE dsv_handle;
    s_d3d.dsv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.dsv_heap, &dsv_handle);

    if (perf_on()) s_perf_pre += perf_now() - _pre0;
    /* Set render target + depth */
    s_d3d.cmd_list->lpVtbl->OMSetRenderTargets(s_d3d.cmd_list, 1, &rtv_handle, FALSE, &dsv_handle);

    /* Clear color and depth -- or, #132, restore this display buffer's last image when
     * the batch did not colour-clear the display (present-on-flip only) */
    const u32 _disp_off = (present_on_flip() && disp_keep_on() && s_present_this_frame) ? disp_batch_offset() : 0u;
    { const int ks = (_disp_off && !s_disp_colour_cleared) ? disp_keep_slot(_disp_off, 0) : -1;
      if (ks >= 0 && s_keep_valid[ks]) {
          disp_barrier(s_d3d.render_targets[fi], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST);
          s_d3d.cmd_list->lpVtbl->CopyResource(s_d3d.cmd_list, s_d3d.render_targets[fi], s_keep_res[ks]);
          disp_barrier(s_d3d.render_targets[fi], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);
      } else
          s_d3d.cmd_list->lpVtbl->ClearRenderTargetView(
              s_d3d.cmd_list, rtv_handle, s_d3d.clear_color, 0, NULL); }
    s_d3d.cmd_list->lpVtbl->ClearDepthStencilView(
        s_d3d.cmd_list, dsv_handle,
        D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
        1.0f, 0, 0, NULL);

    /* Set viewport and scissor */
    D3D12_VIEWPORT viewport = {0, 0, (float)s_dw, (float)s_dh, 0.0f, 1.0f};   /* #253 */
    D3D12_RECT scissor = {0, 0, (LONG)s_dw, (LONG)s_dh};
    s_d3d.cmd_list->lpVtbl->RSSetViewports(s_d3d.cmd_list, 1, &viewport);
    s_d3d.cmd_list->lpVtbl->RSSetScissorRects(s_d3d.cmd_list, 1, &scissor);

    /* Bind pipeline state and push MVP if anything to draw */
    if (s_d3d.pipeline_ready && s_d3d.draw_count > 0) {
        s_d3d.cmd_list->lpVtbl->SetGraphicsRootSignature(s_d3d.cmd_list, s_d3d.root_signature);
        s_d3d.cmd_list->lpVtbl->IASetVertexBuffers(s_d3d.cmd_list, 0, 1, &s_d3d.vb_view);

        /* Make the atlas SRV heap current and bind its table (t0) for textured
         * draws. Safe to set even if no draw is textured. */
        if (s_d3d.tex_ready && s_d3d.srv_heap) {
            ID3D12DescriptorHeap* heaps[] = { s_d3d.srv_heap };
            s_d3d.cmd_list->lpVtbl->SetDescriptorHeaps(s_d3d.cmd_list, 1, heaps);
            D3D12_GPU_DESCRIPTOR_HANDLE gh;
            s_d3d.srv_heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(s_d3d.srv_heap, &gh);
            s_d3d.cmd_list->lpVtbl->SetGraphicsRootDescriptorTable(s_d3d.cmd_list, 1, gh);
        }

        /* Push the MVP matrix from RSX vertex constants slots 0..3.
         * If the game hasn't written any constants (e.g. placeholder data
         * already in clip space), fall back to identity. */
        float mvp[16];
        const rsx_state* st = s_d3d.current_rsx_state;
        int have_mvp = 0;
        if (st) {
            for (u32 r = 0; r < 4; r++) {
                for (u32 c = 0; c < 4; c++) {
                    float v = st->vertex_constants[r][c];
                    mvp[r * 4 + c] = v;
                    if (v != 0.0f) have_mvp = 1;
                }
            }
        }
        if (!have_mvp) {
            memset(mvp, 0, sizeof(mvp));
            mvp[0] = mvp[5] = mvp[10] = mvp[15] = 1.0f; /* identity */
        }
        s_d3d.cmd_list->lpVtbl->SetGraphicsRoot32BitConstants(
            s_d3d.cmd_list, 0 /*root param 0*/, 16, mvp, 0);

        /* D3D12_IQ=1: dump exactly what the GPU is about to see -- the MVP root
         * constants and the first uploaded host vertices. The legacy path draws
         * with no validation errors yet produces zero pixels, so the geometry
         * must be degenerate/off-screen post-VS. */
        { static int _vd = -1;
          if (_vd < 0) { const char* e = getenv("D3D12_IQ"); _vd = e ? 1 : 0; }
          static int _n = 0;
          if (_vd && _n < 3) {
            _n++;
            fprintf(stderr, "[D3D12-DBG] have_mvp=%d draw_count=%u vb_offset=%u\n",
                    have_mvp, s_d3d.draw_count, s_d3d.vb_offset);
            fprintf(stderr, "[D3D12-DBG] mvp rows: [%.2f %.2f %.2f %.2f][%.2f %.2f %.2f %.2f]"
                            "[%.2f %.2f %.2f %.2f][%.2f %.2f %.2f %.2f]\n",
                    mvp[0],mvp[1],mvp[2],mvp[3], mvp[4],mvp[5],mvp[6],mvp[7],
                    mvp[8],mvp[9],mvp[10],mvp[11], mvp[12],mvp[13],mvp[14],mvp[15]);
            if (s_d3d.vb_mapped) {
                /* host vertex = 9 floats (pos3 + col4 + uv2), VERTEX_STRIDE=36 */
                const float* bv = (const float*)s_d3d.vb_mapped;
                for (int k = 0; k < 3; k++) {
                    const float* v = bv + k * 9;
                    fprintf(stderr, "[D3D12-DBG]  hostvert[%d] pos=(%.3f,%.3f,%.3f) col=(%.2f,%.2f,%.2f,%.2f)\n",
                            k, v[0], v[1], v[2], v[3], v[4], v[5], v[6]);
                }
            }
            for (u32 d = 0; d < s_d3d.draw_count && d < 3; d++) {
                const D3D12DrawRecord* dr = &s_d3d.draws[d];
                fprintf(stderr, "[D3D12-DBG]  draw[%u] is_vp=%d is_clear=%d topo=%u cnt=%u vbofs=%u startv=%u rt=%u\n",
                        d, dr->is_vp, dr->is_clear, dr->topology, dr->vertex_count,
                        dr->vb_byte_offset, dr->vb_byte_offset / VERTEX_STRIDE, dr->rt_off);
            }
            fflush(stderr);
          } }

        /* Replay each recorded draw with its own primitive topology and
         * the matching PSO class (triangle / line / point). The PSO class
         * must match the topology or D3D12 rejects the draw. */
        u32 last_topo = 0xFFFFFFFFu;
        ID3D12PipelineState* last_pso = NULL;
        u32 draws = s_d3d.draw_count;
        if (draws > MAX_DRAWS) draws = MAX_DRAWS;
        for (u32 d = 0; d < draws; d++) {
            const D3D12DrawRecord* dr = &s_d3d.draws[d];
            if (dr->is_vp) continue; /* drawn by the VP pass below */

            /* Select PSO: textured triangles (dbgfont) use the atlas PSO;
             * otherwise pick by topology class. */
            ID3D12PipelineState* target_pso = s_d3d.pipeline_state; /* default triangle */
            if (dr->textured && s_d3d.tex_ready && s_d3d.pipeline_state_tex) {
                target_pso = s_d3d.pipeline_state_tex;
            } else if (dr->topology == RSX_TOPOLOGY_POINTS) {
                target_pso = s_d3d.pipeline_state_points
                             ? s_d3d.pipeline_state_points : s_d3d.pipeline_state;
            } else if (dr->topology == RSX_TOPOLOGY_LINES ||
                       dr->topology == RSX_TOPOLOGY_LINE_STRIP) {
                target_pso = s_d3d.pipeline_state_lines
                             ? s_d3d.pipeline_state_lines : s_d3d.pipeline_state;
            }
            if (target_pso != last_pso) {
                s_d3d.cmd_list->lpVtbl->SetPipelineState(s_d3d.cmd_list, target_pso);
                last_pso = target_pso;
            }
            if (dr->topology != last_topo) {
                s_d3d.cmd_list->lpVtbl->IASetPrimitiveTopology(s_d3d.cmd_list, topo_to_d3d(dr->topology));
                last_topo = dr->topology;
            }
            u32 start_vert = dr->vb_byte_offset / VERTEX_STRIDE;
            dlog_add(s_d3d.frame_count, 0xFFFFFFFFu /* legacy fixed-function */, 0, dr->vertex_count, dr->rt_off, 0);
            s_d3d.cmd_list->lpVtbl->DrawInstanced(
                s_d3d.cmd_list, dr->vertex_count, 1, start_vert, 0);
        }
    }

    /* VP pass: real decompiled vertex program + atlas alpha-test PS. Feeds raw
     * float4 attrib0 from vp_vb and the vp_c[] constant bank. */
    if (s_d3d.vp_ready && s_d3d.draw_count > 0) {
        int any = 0;
        for (u32 d = 0; d < s_d3d.draw_count && d < MAX_DRAWS; d++)
            if (s_d3d.draws[d].is_vp) { any = 1; break; }
        /* Textured geometry (dbgfont atlas) uses the sampling PS; untextured 3D
         * (vkcube) uses the colour-only PS. Fall back to whichever exists. */
        ID3D12PipelineState* vpso =
            (s_d3d.tex_ready && s_d3d.pipeline_state_vp) ? s_d3d.pipeline_state_vp
                                                         : s_d3d.pipeline_state_vp_color;
        if (!vpso) vpso = s_d3d.pipeline_state_vp;
        { static int cap = -1, n = 0;
          if (cap < 0) { const char* e = getenv("VP_SUBMIT"); cap = e ? atoi(e) : 0; }
          if (cap && n < cap) { n++;
            u32 nvp = 0, nclr = 0;
            for (u32 d2 = 0; d2 < s_d3d.draw_count && d2 < MAX_DRAWS; d2++) {
                if (s_d3d.draws[d2].is_vp)    nvp++;
                if (s_d3d.draws[d2].is_clear) nclr++;
            }
            fprintf(stderr, "[VPPASS] records=%u is_vp=%u clears=%u any=%d vpso=%p rootsig=%p\n",
                    s_d3d.draw_count, nvp, nclr, any,
                    (void*)vpso, (void*)s_d3d.vp_root_sig); } }
        if (any && vpso) {
            s_d3d.cmd_list->lpVtbl->SetGraphicsRootSignature(s_d3d.cmd_list, s_d3d.vp_root_sig);
            s_d3d.cmd_list->lpVtbl->SetPipelineState(s_d3d.cmd_list, vpso);
            s_d3d.cmd_list->lpVtbl->IASetPrimitiveTopology(s_d3d.cmd_list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            s_d3d.cmd_list->lpVtbl->SetGraphicsRootConstantBufferView(s_d3d.cmd_list, 0,
                s_d3d.vp_cb->lpVtbl->GetGPUVirtualAddress(s_d3d.vp_cb));
            ID3D12DescriptorHeap* heaps[] = { s_d3d.srv_heap };
            s_d3d.cmd_list->lpVtbl->SetDescriptorHeaps(s_d3d.cmd_list, 1, heaps);
            D3D12_GPU_DESCRIPTOR_HANDLE gh;
            s_d3d.srv_heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(s_d3d.srv_heap, &gh);
            s_d3d.cmd_list->lpVtbl->SetGraphicsRootDescriptorTable(s_d3d.cmd_list, 1, gh);
            D3D12_VERTEX_BUFFER_VIEW vbv;
            vbv.BufferLocation = s_d3d.vp_vb->lpVtbl->GetGPUVirtualAddress(s_d3d.vp_vb)
                               + (u64)s_d3d.vp_parity * MAX_VERTICES * 256;
            vbv.SizeInBytes    = MAX_VERTICES * 256;
            vbv.StrideInBytes  = 256;   /* 16 float4 attrib slots */
            s_d3d.cmd_list->lpVtbl->IASetVertexBuffers(s_d3d.cmd_list, 0, 1, &vbv);
            if (s_d3d.vp_ib) {   /* [NHL local patch #62] this frame's index half */
                D3D12_INDEX_BUFFER_VIEW ibv;
                ibv.BufferLocation = s_d3d.vp_ib->lpVtbl->GetGPUVirtualAddress(s_d3d.vp_ib)
                                   + (u64)s_d3d.vp_parity * VP_MAX_INDICES * 4u;
                ibv.SizeInBytes = VP_MAX_INDICES * 4u;
                ibv.Format = DXGI_FORMAT_R32_UINT;
                s_d3d.cmd_list->lpVtbl->IASetIndexBuffer(s_d3d.cmd_list, &ibv);
            }
            if (s_d3d.vp_raw)   /* [#125] this frame's raw vertex half (t0 space1) */
                s_d3d.cmd_list->lpVtbl->SetGraphicsRootShaderResourceView(s_d3d.cmd_list, 3,
                    s_d3d.vp_raw->lpVtbl->GetGPUVirtualAddress(s_d3d.vp_raw)
                    + (u64)s_d3d.vp_parity * VP_RAW_BYTES);
            D3D12_GPU_DESCRIPTOR_HANDLE gh_base;
            s_d3d.srv_heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(s_d3d.srv_heap, &gh_base);
            s_vpr_vbv = vbv; s_vpr_gh = gh_base;   /* #249: vp_pass_restore */
            int cur_ms = 1;                        /* #249: samples of the bound colour target */
            int cur_rt = -1;                       /* target A: -1 = backbuffer */
            float vp_last_w = 0, vp_last_h = 0;    /* for DRAWARGS */
            float dvp_x = 0, dvp_y = 0, dvp_w = 0, dvp_h = 0;  /* per-draw, for DRAWARGS */
            int cur_m[3] = {-1, -1, -1};           /* MRT B/C/D: -1 = unbound   */
            double _rec0 = perf_on() ? perf_now() : 0.0;
            int seen_small_vp = 0, captured = 0;
            /* [#108] an F9 capture is pending: query every draw of this pass */
            s_q_frame = (s_cap_hot_req > 0) && q_init();
            if (s_q_frame) { memset(s_q_used, 0, sizeof s_q_used); s_q_n = 0; }
            static u32 s_exec_pass = 0; s_exec_pass++;   /* [#114] */
            s_exec_loop_count = s_d3d.draw_count;
            for (u32 d = 0; d < s_d3d.draw_count && d < MAX_DRAWS; d++) {
                const D3D12DrawRecord* dr = &s_d3d.draws[d];
                ((D3D12DrawRecord*)dr)->exec = 1; ((D3D12DrawRecord*)dr)->exec_pass = s_exec_pass;
                if (!dr->is_vp) continue;
                if (dr->is_blit) {   /* [#85] ordered GPU copy between offscreen RTs */
                    const int si = off_rt_find(dr->blit_src), di = off_rt_find(dr->blit_dst);
                    if ((si < 0 || di < 0) && gpublit_skip_new(dr->blit_src, dr->blit_dst, "dropped-at-exec"))   /* #243 */
                        fprintf(stderr, "[GPUBLIT] dropped at exec: RT 0x%08X (slot %d) -> RT 0x%08X (slot %d) evicted before the batch ran%c",
                                dr->blit_src, si, dr->blit_dst, di, 10);
                    if (si >= 0 && di >= 0 && si != di &&
                        s_d3d.off_rt[si].dxgi == s_d3d.off_rt[di].dxgi) {
                        const OffRT* S = &s_d3d.off_rt[si]; const OffRT* D = &s_d3d.off_rt[di];
                        u32 w = dr->blit_w, h = dr->blit_h;
                        if (dr->blit_sx + w > S->w) w = S->w - dr->blit_sx;
                        if (dr->blit_sy + h > S->h) h = S->h - dr->blit_sy;
                        if (dr->blit_dx + w > D->w) w = D->w > dr->blit_dx ? D->w - dr->blit_dx : 0;
                        if (dr->blit_dy + h > D->h) h = D->h > dr->blit_dy ? D->h - dr->blit_dy : 0;
                        /* #253: guest rectangles -> resource texels; both ends must share the scale */
                        u32 bsx = dr->blit_sx, bsy = dr->blit_sy, bdx = dr->blit_dx, bdy = dr->blit_dy;
                        if (S->rw != S->w || D->rw != D->w) {
                            if (S->rw * D->w != D->rw * S->w || S->rh * D->h != D->rh * S->h) {
                                static int n = 0;
                                if (n++ < 8) fprintf(stderr, "[RSCALE] GPU blit RT 0x%08X (%ux%u as %ux%u) -> RT 0x%08X (%ux%u as %ux%u): scales differ, skipped%c",
                                                     S->off, S->w, S->h, S->rw, S->rh, D->off, D->w, D->h, D->rw, D->rh, 10);
                                w = h = 0;
                            } else {
                                bsx = bsx * S->rw / S->w; bsy = bsy * S->rh / S->h;
                                bdx = bdx * D->rw / D->w; bdy = bdy * D->rh / D->h;
                                w = w * S->rw / S->w; h = h * S->rh / S->h;
                                if (bsx + w > S->rw) w = S->rw - bsx;
                                if (bsy + h > S->rh) h = S->rh - bsy;
                                if (bdx + w > D->rw) w = D->rw > bdx ? D->rw - bdx : 0;
                                if (bdy + h > D->rh) h = D->rh > bdy ? D->rh - bdy : 0;
                            }
                        }
                        if (w && h) {
                            ms_resolve(si);   /* #249: the copy reads res */
                            if (D->ms) { static int n = 0;   /* #249: res is newer than the twin now */
                                if (n++ < 4) fprintf(stderr, "[MSAA] GPU blit into MSAA RT 0x%08X: its twin keeps the old samples%c", D->off, 10); }
                            off_rt_transition(si, D3D12_RESOURCE_STATE_COPY_SOURCE);
                            off_rt_transition(di, D3D12_RESOURCE_STATE_COPY_DEST);
                            D3D12_TEXTURE_COPY_LOCATION sl = {0}, dl = {0};
                            sl.pResource = S->res; sl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                            dl.pResource = D->res; dl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                            D3D12_BOX box = { bsx, bsy, 0, bsx + w, bsy + h, 1 };   /* #253 */
                            s_d3d.cmd_list->lpVtbl->CopyTextureRegion(s_d3d.cmd_list, &dl,
                                bdx, bdy, 0, &sl, &box);
                        }
                    }
                    cur_rt = -1000;   /* states changed: force the next draw to rebind */
                    for (int _m = 0; _m < 3; _m++) cur_m[_m] = -1000;
                    continue;
                }
                if (dr->is_depth) {   /* [NHL local patch #190] shadow-map pass: depth only */
                    const int zi = shadowz_get(dr->zeta_off, dr->z_w, dr->z_h);
                    if (zi < 0) continue;
                    s_shz_gen[zi]++;   /* #209: its colour copy is stale now */
                    shadowz_transition(zi, D3D12_RESOURCE_STATE_DEPTH_WRITE);
                    D3D12_CPU_DESCRIPTOR_HANDLE zd = shadowz_dsv(zi);
                    if (cur_rt != -2000 - zi) {
                        /* the colour target this interrupts keeps its depth when it returns (#186) */
                        if (cur_rt >= 0 && !s_depth_cont) {
                            s_depth_cont = 1; s_depth_cont_rt[0] = s_d3d.off_rt[cur_rt].off;
                            for (int _m = 0; _m < 3; _m++)
                                s_depth_cont_rt[_m + 1] = cur_m[_m] >= 0 ? s_d3d.off_rt[cur_m[_m]].off : 0u;
                        }
                        s_d3d.cmd_list->lpVtbl->OMSetRenderTargets(s_d3d.cmd_list, 0, NULL, FALSE, &zd);
                        cur_rt = -2000 - zi;
                        for (int _m = 0; _m < 3; _m++) cur_m[_m] = -1000;
                    }
                    const ShadowZ* Z = &s_shz[zi];
                    D3D12_VIEWPORT zvp = {0, 0, (float)Z->w, (float)Z->h, 0.0f, 1.0f};
                    if (dr->vp_w >= 2 && dr->vp_h >= 2 && dr->vp_x + dr->vp_w <= Z->w && dr->vp_y + dr->vp_h <= Z->h) {
                        zvp.TopLeftX = (float)dr->vp_x; zvp.TopLeftY = (float)dr->vp_y;
                        zvp.Width = (float)dr->vp_w;    zvp.Height = (float)dr->vp_h;
                    }
                    if (Z->rw != Z->w || Z->rh != Z->h) {   /* #254: guest rect -> the larger map */
                        const float kx = (float)Z->rw / (float)Z->w, ky = (float)Z->rh / (float)Z->h;
                        zvp.TopLeftX *= kx; zvp.Width *= kx; zvp.TopLeftY *= ky; zvp.Height *= ky;
                    }
                    D3D12_RECT zsc = {(LONG)zvp.TopLeftX, (LONG)zvp.TopLeftY,
                                      (LONG)(zvp.TopLeftX + zvp.Width), (LONG)(zvp.TopLeftY + zvp.Height)};
                    s_d3d.cmd_list->lpVtbl->RSSetViewports(s_d3d.cmd_list, 1, &zvp);
                    s_d3d.cmd_list->lpVtbl->RSSetScissorRects(s_d3d.cmd_list, 1, &zsc);
                    if (dr->is_depth == 2) {
                        s_d3d.cmd_list->lpVtbl->ClearDepthStencilView(s_d3d.cmd_list, zd, D3D12_CLEAR_FLAG_DEPTH,
                                                                       dr->cc[0], 0, 0, NULL);
                        continue;
                    }
                    ID3D12PipelineState* zp = depth_pso_get(dr->vs_idx, ((u32)dr->fp_exp32 >> 20) & 0x1Fu, dr->cull);
                    if (!zp) continue;
                    s_d3d.cmd_list->lpVtbl->SetPipelineState(s_d3d.cmd_list, zp);
                    s_d3d.cmd_list->lpVtbl->SetGraphicsRootConstantBufferView(s_d3d.cmd_list, 0,
                        s_d3d.vp_cb->lpVtbl->GetGPUVirtualAddress(s_d3d.vp_cb)
                        + ((u64)s_d3d.vp_parity * MAX_DRAWS + dr->cb_slot) * VP_CB_STRIDE);
                    if (dr->indexed && s_d3d.vp_ib)
                        s_d3d.cmd_list->lpVtbl->DrawIndexedInstanced(s_d3d.cmd_list,
                            dr->vertex_count, 1, dr->ib_first, (INT)(dr->vb_byte_offset / 256), 0);
                    else
                        s_d3d.cmd_list->lpVtbl->DrawInstanced(s_d3d.cmd_list,
                            dr->vertex_count, 1, dr->vb_byte_offset / 256, 0);
                    ((D3D12DrawRecord*)dr)->exec = 4;
                    { static u32 nz = 0; static ULONGLONG zt0 = 0; nz++;
                      const ULONGLONG zt = GetTickCount64(); if (!zt0) zt0 = zt;
                      if (zt - zt0 >= 5000) { fprintf(stderr, "[SHADOWZ] %u depth draws / 5 s executed%c", nz, 10); nz = 0; zt0 = zt; } }
                    continue;
                }
                /* [#209] first sample of a depth surface as colour since it was last drawn:
                 * repack it, then restore the guest-draw state and rebind the target
                 * (keeping its depth, like the shadow-pass interruption above). */
                if (dr->dcz > 0 && !dr->is_clear) {
                    const int zi = dr->dcz - 1;
                    if (zi == DC_SNAP && s_span_zdraws && s_span_rt >= 0 &&
                        depthcol_snapshot_maybe(s_span_rt)) {   /* #209b: the scene may end right here */
                        if (cur_rt >= 0 && !s_depth_cont) {     /* #249: the MSAA depth resolve unbound it */
                            s_depth_cont = 1; s_depth_cont_rt[0] = s_d3d.off_rt[cur_rt].off;
                            for (int _m = 0; _m < 3; _m++)
                                s_depth_cont_rt[_m + 1] = cur_m[_m] >= 0 ? s_d3d.off_rt[cur_m[_m]].off : 0u;
                        }
                        vp_pass_restore();
                        cur_rt = -1000;
                        for (int _m = 0; _m < 3; _m++) cur_m[_m] = -1000;
                    }
                    if ((zi == DC_SNAP || zi < s_shz_n) && s_dc[zi].res && s_dc[zi].gen != s_shz_gen[zi] + 1u) {
                        if (cur_rt >= 0 && !s_depth_cont) {
                            s_depth_cont = 1; s_depth_cont_rt[0] = s_d3d.off_rt[cur_rt].off;
                            for (int _m = 0; _m < 3; _m++)
                                s_depth_cont_rt[_m + 1] = cur_m[_m] >= 0 ? s_d3d.off_rt[cur_m[_m]].off : 0u;
                        }
                        depthcol_convert(zi);
                        s_d3d.cmd_list->lpVtbl->SetGraphicsRootSignature(s_d3d.cmd_list, s_d3d.vp_root_sig);
                        s_d3d.cmd_list->lpVtbl->IASetPrimitiveTopology(s_d3d.cmd_list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                        s_d3d.cmd_list->lpVtbl->SetDescriptorHeaps(s_d3d.cmd_list, 1, heaps);
                        s_d3d.cmd_list->lpVtbl->SetGraphicsRootDescriptorTable(s_d3d.cmd_list, 1, gh_base);
                        s_d3d.cmd_list->lpVtbl->IASetVertexBuffers(s_d3d.cmd_list, 0, 1, &vbv);
                        if (s_d3d.vp_ib) {
                            D3D12_INDEX_BUFFER_VIEW ibv;
                            ibv.BufferLocation = s_d3d.vp_ib->lpVtbl->GetGPUVirtualAddress(s_d3d.vp_ib)
                                               + (u64)s_d3d.vp_parity * VP_MAX_INDICES * 4u;
                            ibv.SizeInBytes = VP_MAX_INDICES * 4u;
                            ibv.Format = DXGI_FORMAT_R32_UINT;
                            s_d3d.cmd_list->lpVtbl->IASetIndexBuffer(s_d3d.cmd_list, &ibv);
                        }
                        if (s_d3d.vp_raw)
                            s_d3d.cmd_list->lpVtbl->SetGraphicsRootShaderResourceView(s_d3d.cmd_list, 3,
                                s_d3d.vp_raw->lpVtbl->GetGPUVirtualAddress(s_d3d.vp_raw)
                                + (u64)s_d3d.vp_parity * VP_RAW_BYTES);
                        cur_rt = -1000;
                        for (int _m = 0; _m < 3; _m++) cur_m[_m] = -1000;
                    }
                }
                /* The reduced-viewport passes (this title's reflection, drawn
                 * into a region of the same target) finish here -- snapshot
                 * before the full-screen scene overwrites them. */
                if (!dr->is_clear) {
                    if (dr->vp_w && dr->vp_w < s_d3d.width) {
                        seen_small_vp = 1;
                        subvp_note(dr->vp_x, dr->vp_y, dr->vp_w, dr->vp_h);
                    }
                    else if (seen_small_vp && !captured &&
                             dr->vp_w == s_d3d.width && dr->rt_off == 0) {
                        captured = 1;
                        screen_copy_capture(fi);
                        subvp_capture(fi);
                        s_d3d.cmd_list->lpVtbl->OMSetRenderTargets(
                            s_d3d.cmd_list, 1, &rtv_handle, FALSE, &dsv_handle);
                    }
                }
                /* Render-to-texture: retarget when this op's surfaces differ.
                 * Depth is a single shared buffer, so clear it per switch. */
                int want  = dr->rt_off  ? off_rt_find(dr->rt_off)  : -1;
                /* Remember where geometry actually went: off_rt_find() matches
                 * on offset alone and slots share offsets at different sizes,
                 * so the composite cannot just look the source up by address. */
                if (want >= 0) { s_last_drawn_slot = want; s_last_drawn_off = dr->rt_off; }
                int wantm[3];
                for (int _m = 0; _m < 3; _m++)
                    wantm[_m] = dr->rt_mrt[_m] ? off_rt_find(dr->rt_mrt[_m]) : -1;
                /* #249: a guest AA surface renders into its multisampled twin; every target of
                 * the bind needs one, or none is multisampled */
                int wms = want >= 0 ? dr_ms(dr) : 1;
                if (wms > 1) {
                    if (!ms_ensure(want)) wms = 1;
                    for (int _m = 0; _m < 3 && wms > 1 && wantm[_m] >= 0; _m++)
                        if (!ms_ensure(wantm[_m])) wms = 1;
                }
                if (want != cur_rt || wantm[0] != cur_m[0] ||
                    wantm[1] != cur_m[1] || wantm[2] != cur_m[2] || wms != cur_ms) {
                    /* [#209b] the pass on cur_rt ends (before its depth is cleared): snapshot the scene depth */
                    /* (#210b: no reset when returning from a shadow-map pass -- the scene pass
                     * continues. #210c: keyed on the RT the depth draws went to, not cur_rt: after
                     * the scene's resolve blit (#85) cur_rt is -1000 and play305 credited the scene's
                     * draws to the next target, snapshotting it after its depth clear) */
                    if (s_span_zdraws && s_span_rt >= 0 && want != s_span_rt && depthcol_snapshot_maybe(s_span_rt))
                        vp_pass_restore();   /* #249: the MSAA depth resolve ran */
                    /* #249: twins left behind -> res, before anything samples or copies them */
                    ms_resolve_except(wms > 1 ? want : -1, wms > 1 ? wantm : NULL);
                    D3D12_CPU_DESCRIPTOR_HANDLE rh[4];
                    UINT nrt = 1;
                    rh[0] = rtv_handle;
                    D3D12_VIEWPORT vp = {0, 0, (float)s_dw, (float)s_dh, 0.0f, 1.0f};   /* #253 */
                    if (want >= 0) {
                        if (wms > 1) {
                            ms_transition(&s_d3d.off_rt[want], D3D12_RESOURCE_STATE_RENDER_TARGET);
                            s_d3d.off_rt[want].ms_dirty = 1;
                            rh[0] = ms_rtv(want);
                        } else {
                            off_rt_transition(want, D3D12_RESOURCE_STATE_RENDER_TARGET);
                            rh[0] = off_rt_rtv(want);
                        }
                        vp.Width  = (float)s_d3d.off_rt[want].rw;   /* #253: resource size */
                        vp.Height = (float)s_d3d.off_rt[want].rh;
                    }
                    for (int _m = 0; _m < 3 && wantm[_m] >= 0; _m++) {
                        if (wms > 1) {
                            ms_transition(&s_d3d.off_rt[wantm[_m]], D3D12_RESOURCE_STATE_RENDER_TARGET);
                            s_d3d.off_rt[wantm[_m]].ms_dirty = 1;
                            rh[nrt++] = ms_rtv(wantm[_m]);
                        } else {
                            off_rt_transition(wantm[_m], D3D12_RESOURCE_STATE_RENDER_TARGET);
                            rh[nrt++] = off_rt_rtv(wantm[_m]);
                        }
                    }
                    D3D12_RECT sc = {0, 0, (LONG)vp.Width, (LONG)vp.Height};
                    vp_last_w = vp.Width; vp_last_h = vp.Height;
                    /* [NHL local patch] offscreen targets get their own depth/
                     * stencil (DSV [1]); returning to the display keeps the
                     * display's buffer intact instead of clearing it. */
                    D3D12_CPU_DESCRIPTOR_HANDLE dsv_use = dsv_handle;
                    if (wms > 1) dsv_use = dsv_at(2);   /* #249: the MSAA depth/stencil */
                    else if (want >= 0 && s_d3d.depth_buffer2)
                        dsv_use.ptr += s_d3d.device->lpVtbl->GetDescriptorHandleIncrementSize(
                            s_d3d.device, D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
                    s_d3d.cmd_list->lpVtbl->OMSetRenderTargets(s_d3d.cmd_list, nrt, rh, FALSE, &dsv_use);
                    s_d3d.cmd_list->lpVtbl->RSSetViewports(s_d3d.cmd_list, 1, &vp);
                    s_d3d.cmd_list->lpVtbl->RSSetScissorRects(s_d3d.cmd_list, 1, &sc);
                    /* [NHL local patch #186] A batch flushed only because the record
                     * array was full (#77) is not a frame boundary: when this batch's
                     * first switch returns to the surfaces it ended on, keep their
                     * depth. The clear here wiped the arena's depth mid-frame, so in
                     * >4096-draw frames (full-crowd wide shots) the crowd drawn in the
                     * next batch painted over the jumbotron (play254 frame_75).
                     * NHL_DEPTH_CONT=0 = always clear. */
                    int keep_depth = 0;
                    if (s_depth_cont) {
                        static int dc = -1;
                        if (dc < 0) { const char* e = getenv("NHL_DEPTH_CONT"); dc = (e && *e == '0') ? 0 : 1; }
                        keep_depth = dc && want >= 0 && s_d3d.depth_buffer2 &&
                                     dr->rt_off == s_depth_cont_rt[0] && dr->rt_mrt[0] == s_depth_cont_rt[1] &&
                                     dr->rt_mrt[1] == s_depth_cont_rt[2] && dr->rt_mrt[2] == s_depth_cont_rt[3];
                        s_depth_cont = 0;   /* only the batch's first switch */
                    }
                    if ((want >= 0 || !s_d3d.depth_buffer2) && !keep_depth)
                        s_d3d.cmd_list->lpVtbl->ClearDepthStencilView(s_d3d.cmd_list, dsv_use,
                            D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, NULL);
                    /* RT_CLEARDBG=2: magenta-clear the surface the DRAWS bind,
                     * at the moment they bind it. The guest's own clears may go
                     * to a different surface entirely (they do here: clears to
                     * 0xAB0000, draws to 0x1BE0000), so clearing on the clear
                     * path tests the wrong target. Anything not magenta
                     * afterwards was rasterised. */
                    { static int cd2 = -1;
                      if (cd2 < 0) { const char* e = getenv("RT_CLEARDBG"); cd2 = e ? atoi(e) : 0; }
                      if (cd2 >= 2 && want >= 0) {
                          const float mag[4] = {1.0f, 0.0f, 1.0f, 1.0f};
                          s_d3d.cmd_list->lpVtbl->ClearRenderTargetView(
                              s_d3d.cmd_list, off_rt_rtv(want), mag, 0, NULL);
                      } }
                    /* VP_TESTTRI=1: our own triangle, through the game's exact
                     * pipeline -- same vpso, same root signature, same vertex
                     * buffer, same RTV just bound. Written at vertex 1000 so it
                     * cannot collide with the guest's data. Pair with
                     * VP_BYPASS=1 so the shader is a trivial passthrough.
                     *
                     * If this renders, the GPU does read vp_vb and the fault is
                     * in the guest draws' own state. If it does not, nothing
                     * this backend draws reaches a render target at all. */
                    { static int tt = -1;
                      if (tt < 0) { const char* e = getenv("VP_TESTTRI"); tt = e ? atoi(e) : 0; }
                      if (tt && want >= 0 && s_d3d.vp_vb_mapped) {
                          float* vtx = (float*)((char*)s_d3d.vp_vb_mapped
                              + (u64)s_d3d.vp_parity * MAX_VERTICES * 256u
                              + (u64)1000 * 256u);
                          static const float tri[3][4] = {
                              {-2.0f, -2.0f, 0.0f, 1.0f},
                              { 2.0f, -2.0f, 0.0f, 1.0f},
                              { 0.0f,  2.0f, 0.0f, 1.0f} };
                          for (int _v = 0; _v < 3; _v++) {
                              float* dstv = vtx + (size_t)_v * (256u / 4u);
                              dstv[0] = tri[_v][0]; dstv[1] = tri[_v][1];
                              dstv[2] = tri[_v][2]; dstv[3] = tri[_v][3];
                          }
                          dlog_add(s_d3d.frame_count, 0xFFFFFFFEu /* diag triangle */, 0, 3, 0, 0);
                          s_d3d.cmd_list->lpVtbl->DrawInstanced(s_d3d.cmd_list, 3, 1, 1000, 0);
                          { static int _n = 0;
                            if (_n++ < 3) fprintf(stderr, "[TESTTRI] issued on rt=%d off=0x%08X%c",
                                                  want, s_d3d.off_rt[want].off, 10); }
                      } }
                    cur_rt = want;
                    cur_ms = wms;
                    for (int _m = 0; _m < 3; _m++) cur_m[_m] = wantm[_m];
                }
                if (dr->is_clear) {
                    D3D12_CPU_DESCRIPTOR_HANDLE rh =
                        (cur_rt >= 0) ? (cur_ms > 1 ? ms_rtv(cur_rt) : off_rt_rtv(cur_rt)) : rtv_handle;   /* #249 */
                    /* [NHL local patch #223] An RSX colour clear only touches the
                     * scissor rectangle. NHL's post-fight penalty-box split screen
                     * draws both halves into 0x02980000 with a clear before each
                     * half (scissor = that half); a whole-target clear wiped the
                     * left view drawn just before -> left half black (play-test 2,
                     * frame_74/78). Only a proper sub-rectangle of the target is
                     * used; anything else (the 4096x4096 default, empty or garbage
                     * rects) keeps the old full clear. NHL_CLEAR_SCISSOR=0 = old. */
                    D3D12_RECT crect; UINT ncr = 0;
                    { static int on = -1;
                      if (on < 0) { const char* e = getenv("NHL_CLEAR_SCISSOR"); on = !(e && *e == '0'); }
                      if (on && cur_rt >= 0 && dr->clr_w && dr->clr_h) {
                          const u32 tw = s_d3d.off_rt[cur_rt].w, th = s_d3d.off_rt[cur_rt].h;
                          const u32 x0 = dr->clr_x < tw ? dr->clr_x : tw, y0 = dr->clr_y < th ? dr->clr_y : th;
                          const u32 x1 = dr->clr_x + dr->clr_w < tw ? dr->clr_x + dr->clr_w : tw;
                          const u32 y1 = dr->clr_y + dr->clr_h < th ? dr->clr_y + dr->clr_h : th;
                          if (x1 > x0 && y1 > y0 && (x0 > 0 || y0 > 0 || x1 < tw || y1 < th)) {
                              const OffRT* cr = &s_d3d.off_rt[cur_rt];   /* #253: guest rect -> resource texels */
                              crect.left = (LONG)(x0 * cr->rw / tw); crect.top = (LONG)(y0 * cr->rh / th);
                              crect.right = (LONG)(x1 * cr->rw / tw); crect.bottom = (LONG)(y1 * cr->rh / th);
                              ncr = 1;
                              static int n = 0;
                              if (n++ < 8) fprintf(stderr, "[CLRSC] scissored clear rt=0x%08X %ux%u -> %u,%u..%u,%u%c",
                                                   s_d3d.off_rt[cur_rt].off, tw, th, x0, y0, x1, y1, 10);
                          }
                      } }
                    /* RT_CLEARDBG=1: clear offscreen targets to magenta rather
                     * than the guest colour. "0.000% nonzero" cannot tell an
                     * untouched surface from one written black, and that
                     * ambiguity invalidates every read of an offscreen target.
                     * If the surface comes back magenta the clears land and the
                     * draws are writing black; if it stays black, nothing
                     * reaches it at all. */
                    { static int cd = -1;
                      if (cd < 0) { const char* e = getenv("RT_CLEARDBG"); cd = e ? atoi(e) : 0; }
                      if (cd && cur_rt >= 0) {
                          const float mag[4] = {1.0f, 0.0f, 1.0f, 1.0f};
                          s_d3d.cmd_list->lpVtbl->ClearRenderTargetView(s_d3d.cmd_list, rh, mag, 0, NULL);
                      } else
                          s_d3d.cmd_list->lpVtbl->ClearRenderTargetView(s_d3d.cmd_list, rh, dr->cc, ncr, ncr ? &crect : NULL); }
                    for (int _m = 0; _m < 3; _m++)
                        if (cur_m[_m] >= 0) {
                            D3D12_CPU_DESCRIPTOR_HANDLE rhm = cur_ms > 1 ? ms_rtv(cur_m[_m]) : off_rt_rtv(cur_m[_m]);   /* #249 */
                            s_d3d.cmd_list->lpVtbl->ClearRenderTargetView(s_d3d.cmd_list, rhm, dr->cc, ncr, ncr ? &crect : NULL);
                        }
                    continue;
                }
                /* [NHL local patch] An unclaimed Apt image-slot fill draws its
                 * placeholder colour over the art beneath. Keep it only as a
                 * mask write (stencil test NEVER: it writes stencil, never
                 * colour); otherwise drop it. NHL_KEEP_PLACEHOLDERS=1 keeps all. */
                /* [NHL local patch #87] Apt texture-provider emulation. In the match
                 * the 3D view reaches the screen through an Apt external-texture
                 * slot: a full-screen fill a registered texture provider claims and
                 * draws the post-processed scene (RT 0x043F0000, still bound as t0)
                 * into. This port never registers providers (see the menu notes:
                 * renderer+0xFC stays empty), so the slot stays a tagged placeholder
                 * and the display clear shows through -- the WHITE screen (play87:
                 * skipping any single op left the probed pixel white). When an
                 * unclaimed slot draws onto the backbuffer with an offscreen RT of
                 * the backbuffer's size bound at t0, copy that RT there, as the
                 * provider would; the HUD draws after it. NHL_APT_RT_COMPOSITE=0
                 * disables. */
                { static int arc = -1;
                  if (arc < 0) { const char* e = getenv("NHL_APT_RT_COMPOSITE"); arc = (e && *e == '0') ? 0 : 1; }
                  /* tex[0].set is 0 for these untextured fills; .raw still holds
                   * the unit-0 address the game left bound (the scene RT). */
                  if (arc && dr->placeholder && dr->rt_off == 0 && dr->tex[0].raw) {
                      const int srt = off_rt_find(dr->tex[0].raw);
                      if (srt >= 0 && s_d3d.off_rt[srt].w == s_d3d.width && s_d3d.off_rt[srt].h == s_d3d.height &&
                          s_d3d.off_rt[srt].rw == s_dw && s_d3d.off_rt[srt].rh == s_dh &&   /* #253: CopyResource */
                          s_d3d.off_rt[srt].dxgi == (u32)DXGI_FORMAT_R8G8B8A8_UNORM) {
                          ID3D12Resource* bb = s_d3d.render_targets[fi];
                          ms_resolve(srt);   /* #249 */
                          off_rt_transition(srt, D3D12_RESOURCE_STATE_COPY_SOURCE);
                          D3D12_RESOURCE_BARRIER bb1 = {0};
                          bb1.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                          bb1.Transition.pResource = bb;
                          bb1.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                          bb1.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
                          bb1.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
                          s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &bb1);
                          s_d3d.cmd_list->lpVtbl->CopyResource(s_d3d.cmd_list, bb, s_d3d.off_rt[srt].res);
                          bb1.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
                          bb1.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
                          s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &bb1);
                          { static int n = 0;
                            if (n++ < 3) fprintf(stderr, "[APT-RT] slot filled from RT 0x%08X (provider emulation)%c",
                                                 s_d3d.off_rt[srt].off, 10); }
                      }
                  } }
                { static int kp = -1; if (kp < 0) kp = getenv("NHL_KEEP_PLACEHOLDERS") ? 1 : 0;
                  if (!kp && dr->placeholder &&
                      !((dr->stencil_key & 1u) && ((dr->stencil_key >> 1) & 7u) == 0)) {
                      ((D3D12DrawRecord*)dr)->exec = 2; continue; } }
                /* Per-draw pipeline: prefer the guest's own compiled FP; fall
                 * back to the hardcoded atlas/colour PS pair. */
                s_fp_snap_cur = dr->fp_snap; s_fp_snap_cur_len = dr->fp_snap_len;   /* #82 */ s_fp_cur_rt = dr->rt_off;   /* #86 */ fp_shadow_from_draw(dr);   /* #190 */
                /* [#108] F6/F7 live toggles act on the PSO key (cull bit2 = CCW,
                 * bit0 = enable; ctl bits 20-24 = depth key). */
                u32 lcull = dr->cull; int lexp = dr->fp_exp32;
                if (s_live_cull == 1 && (lcull & 1u)) lcull ^= 4u;
                else if (s_live_cull == 2) lcull = 0u;
                if (s_live_depth) lexp &= ~(0x1F << 20);
                s_async_sync = draw_essential(dr);   /* #225 */
                ID3D12PipelineState* dpso =
                    dr->fp_addr ? vp_get_fp_pso(dr->vs_idx, dr->fp_addr, dr->blend_key,
                                                dr_num_rts(dr),
                                                dr->rt_off ? rsx_surface_dxgi(dr->rt_fmt)
                                                           : DXGI_FORMAT_R8G8B8A8_UNORM,
                                                ms_exp(dr, lexp), dr->cmask, lcull,
                                                dr_cube_mask(dr), dr->stencil_key) : NULL;
                s_async_sync = 1;
                ((D3D12DrawRecord*)dr)->fp_key = dr->fp_addr ? s_fp_last_key : 0u;   /* #89 manifest */
                ((D3D12DrawRecord*)dr)->exec = 3;   /* #114 */
                /* #118 [SLOTSTALE]: a slot this draw's descriptor points at was
                 * given to a different texture later in the pre-pass */
                for (int _u = 0; _u < 4; _u++) {
                    if (dr->bound_kind[_u] != 2) continue;
                    const VPTexSlot* c = &s_d3d.vp_tex[dr->bound_id[_u]];
                    if (c->key != dr->tex[_u].off || c->w != dr->tex[_u].w || c->h != dr->tex[_u].h || c->fmt != dr->tex[_u].fmt) {
                        static int n = 0; static u32 tot = 0; tot++;
                        if (n++ < 30) fprintf(stderr, "[SLOTSTALE] frame %llu draw %u unit %d bound slot %u for off=0x%08X, slot now holds key=0x%08X (%u so far)%c",
                                              (unsigned long long)s_d3d.frame_count, d, _u, dr->bound_id[_u],
                                              dr->tex[_u].off, c->key, tot, 10); }
                }
                /* #90 NHL_SKIP_KEY=<hex>[,<hex>...]: drop draws whose FP key (the
                 * [FPID]/manifest key) matches. Diagnostic: hide one object. */
                { static int nk = -1; static u32 ks[8];
                  if (nk < 0) { nk = 0; const char* e = getenv("NHL_SKIP_KEY");
                                while (e && *e && nk < 8) { char* end; unsigned long v = strtoul(e, &end, 16);
                                                            if (end == e) break; ks[nk++] = (u32)v;
                                                            e = (*end == ',') ? end + 1 : end; } }
                  if (nk > 0 && dr->fp_addr) { int sk = 0;
                      for (int k = 0; k < nk; k++) if (dr->fp_key == ks[k]) sk = 1;
                      if (sk) continue; } }
                /* [NHL local patch #175] NHL_TEXUSE=1: every distinct (program key, unit,
                 * texture) a draw executes with -> [TEXUSE] (first 4000, after
                 * NHL_TEXUSE_AFTER seconds). With NHL_FP_SOLID_MODE=id the [FPID] colour
                 * of a surface gives its key, this gives what that program samples. */
                { static int tu = -1; static u32 tu_after = 0; static ULONGLONG tu_t0 = 0;
                  static u32 seen[4096]; static int ns = 0;
                  if (tu < 0) { tu = getenv("NHL_TEXUSE") ? 1 : 0; const char* e = getenv("NHL_TEXUSE_AFTER");
                                tu_after = e ? (u32)atoi(e) : 0; tu_t0 = GetTickCount64(); }
                  if (tu && ns < 4000 && dr->fp_addr && dr->rt_off && GetTickCount64() - tu_t0 >= (ULONGLONG)tu_after * 1000ull) {
                      for (int u = 0; u < D3D_TEX_UNITS && ns < 4000; u++) {
                          if (!dr->tex[u].set) continue;
                          const u32 k = (dr->fp_key * 0x9E3779B1u) ^ (dr->tex[u].raw * 0x85EBCA77u) ^ (u32)u
                                      ^ (dr->tex[u].fmt << 24) ^ (dr->tex[u].w << 12) ^ dr->tex[u].h;
                          int f = 0; for (int i = 0; i < ns; i++) if (seen[i] == k) { f = 1; break; }
                          if (f) continue;
                          seen[ns++] = k;
                          /* NHL_TEXUSE_DUMPKEY=<hex key>: also write every texture this key binds
                           * (level 0 straight from guest memory; DXT as .dds, else raw bytes) */
                          { static u32 dk = 0xFFFFFFFFu;
                            if (dk == 0xFFFFFFFFu) { const char* e = getenv("NHL_TEXUSE_DUMPKEY"); dk = e ? (u32)strtoul(e, NULL, 16) : 0u; }
                            extern uint8_t* vm_base;
                            if (dk && (dr->fp_key == dk || (dr->fp_key ^ 0x5011D000u) == dk) && dr->tex[u].off && vm_base) {
                                const u32 bf = dr->tex[u].fmt & 0x9Fu, tw = dr->tex[u].w, th = dr->tex[u].h;
                                const int isdxt = bf >= 0x86u && bf <= 0x88u;
                                char pn[160]; CreateDirectoryA("texdump", NULL);
                                snprintf(pn, sizeof pn, "texdump/key_%08X_u%d_%08X_%ux%u_%02X.%s", dk, u, dr->tex[u].off,
                                         tw, th, dr->tex[u].fmt, isdxt ? "dds" : "raw");
                                FILE* fd = fopen(pn, "wb");
                                if (fd) {
                                    if (isdxt) {
                                        const u32 bb = bf == 0x86u ? 8u : 16u, rb = ((tw + 3u) / 4u) * bb, nr = (th + 3u) / 4u;
                                        /* [#239b] the whole guest mip chain (levels packed back to back,
                                         * as the #119 upload reads them), DDS mip count = guest mips */
                                        u32 nm = dr->tex[u].mips ? dr->tex[u].mips : 1u; if (nm > 16u) nm = 16u;
                                        size_t chain = 0; { u32 lw = tw, lh = th;
                                            for (u32 l = 0; l < nm; l++) { chain += (size_t)((lw + 3u) / 4u) * bb * ((lh + 3u) / 4u);
                                                lw = lw > 1 ? lw >> 1 : 1; lh = lh > 1 ? lh >> 1 : 1; } }
                                        u32 hd[32] = {0};
                                        hd[0] = 0x20534444u; hd[1] = 124; hd[2] = 0x000A1007u; hd[3] = th; hd[4] = tw; hd[5] = rb * nr;
                                        hd[7] = nm; hd[19] = 32; hd[20] = 0x4u;
                                        hd[21] = bf == 0x86u ? 0x31545844u : bf == 0x87u ? 0x33545844u : 0x35545844u;
                                        hd[27] = 0x401008u; fwrite(hd, 4, 32, fd);
                                        fwrite(vm_base + dr->tex[u].off, 1, chain, fd);
                                    } else {
                                        const u32 nb = tw * th * 4u;
                                        fwrite(vm_base + dr->tex[u].off, 1, nb < (16u << 20) ? nb : (16u << 20), fd);
                                    }
                                    fclose(fd);
                                }
                            } }
                          /* [#214 diag] + guest sampler words, decoded as RPCS3 reads them: FILTER
                           * bias s4.8 [12:0], conv [15:13], min [23:16], mag [26:24], signed ARGB
                           * [31:28]; CONTROL0 min LOD [30:19] / max LOD [18:7] (4.8), aniso [6:4];
                           * ADDRESS gamma nibble [23:20] (R1 G2 B4 A8). */
                          { const u32 fl = dr->tex[u].filter, c0 = dr->tex[u].ctl0, ad = dr->tex[u].addr;
                            int b13 = (int)(fl & 0x1FFFu); if (b13 & 0x1000) b13 -= 0x2000;
                            fprintf(stderr, "[TEXUSE] key=0x%08X fp=0x%08X rt=0x%08X u=%d raw=0x%08X off=0x%08X %ux%u fmt=0x%02X ctrl1=0x%08X pitch=%u mips=%u rtslot=%d"
                                            " | filter=0x%08X bias=%.3f conv=%u min=%u mag=%u signed=0x%X ctl0=0x%08X minlod=%.2f maxlod=%.2f aniso=%u addr=0x%08X gamma=0x%X%c",
                                    dr->fp_key, dr->fp_addr, dr->rt_off, u, dr->tex[u].raw, dr->tex[u].off, dr->tex[u].w,
                                    dr->tex[u].h, dr->tex[u].fmt, dr->tex[u].ctrl1, dr->tex[u].pitch, dr->tex[u].mips,
                                    dr->tex_rt[u], fl, b13 / 256.0, (fl >> 13) & 7u, (fl >> 16) & 0xFFu, (fl >> 24) & 7u, fl >> 28,
                                    c0, ((c0 >> 19) & 0xFFFu) / 256.0, ((c0 >> 7) & 0xFFFu) / 256.0, (c0 >> 4) & 7u,
                                    ad, (ad >> 20) & 0xFu, 10); }
                      }
                  } }
                /* [#96] FPK_KEY=<hex fp key>: fp_k[0..15] of that program's draws
                 * as uploaded for them (8 lines every 10 s). */
                { static u32 fkk = 0xFFFFFFFFu; static ULONGLONG fkt = 0; static int fkn = 0;
                  static u32 fkks[16]; static int nfkk = 0;   /* #206: FPK_KEY may list up to 16 keys */
                  if (fkk == 0xFFFFFFFFu) { const char* e = getenv("FPK_KEY"); fkk = e ? (u32)strtoul(e, NULL, 16) : 0u;
                      for (const char* c = e; c && *c && nfkk < 16; ) { char* end; u32 v = (u32)strtoul(c, &end, 16);
                          if (end == c) break; fkks[nfkk++] = v; c = (*end == ',') ? end + 1 : end; } }
                  int fkhit = 0; for (int _k = 0; _k < nfkk; _k++) if (dr->fp_key == fkks[_k]) fkhit = 1;
                  if (fkk && fkhit && s_d3d.vp_fpcb_mapped) {
                      const ULONGLONG t = GetTickCount64();
                      if (t - fkt >= 10000) { fkt = t; fkn = 0; }
                      static int fkmax = -1; if (fkmax < 0) { const char* e2 = getenv("FPK_KEY_MAX"); fkmax = e2 ? atoi(e2) : 8; }
                      if (fkn < fkmax) { fkn++;
                          const float* ts = (const float*)((const char*)s_d3d.vp_fpcb_mapped
                              + ((u64)s_d3d.vp_parity * MAX_DRAWS + dr->cb_slot) * VP_FPCB_STRIDE);
                          char b[8192]; int p = snprintf(b, sizeof b, "[FPKKEY] key=0x%08X fp=0x%08X n=%u t0=0x%08X t1=0x%08X t2=0x%08X:",
                                                         dr->fp_key, dr->fp_addr, dr->vertex_count, dr->tex[0].raw, dr->tex[1].raw, dr->tex[2].raw);
                          /* [#201] FPK_REGS=<k,k,..> (decimal, < 256): which constants to print
                           * instead of k0-k15 (e.g. the player FP's lighting k61..k94). */
                          /* [#206] FPK_REGS=all: k0-k127 */
                          static int fkr[128], fkrn = -1;
                          if (fkrn < 0) { fkrn = 0; const char* e3 = getenv("FPK_REGS");
                              if (e3 && !strcmp(e3, "all")) { for (fkrn = 0; fkrn < 128; fkrn++) fkr[fkrn] = fkrn; e3 = NULL; }
                              for (const char* q = e3; q && *q && fkrn < 16; ) { const int v = atoi(q);
                                  if (v >= 0 && v < 256) fkr[fkrn++] = v; q = strchr(q, ','); if (q) q++; }
                              if (!fkrn) for (fkrn = 0; fkrn < 16; fkrn++) fkr[fkrn] = fkrn; }
                          for (int i = 0; i < fkrn && p < (int)sizeof b - 64; i++) { const int k = fkr[i];
                              p += snprintf(b + p, sizeof b - p, " k%d=(%.3g,%.3g,%.3g,%.3g)", k,
                                            ts[20 + k*4], ts[21 + k*4], ts[22 + k*4], ts[23 + k*4]); }
                          /* the VS and the constants feeding tc0 (c64/c65 scale-bias, c467) */
                          { const float* vc = (const float*)((const char*)s_d3d.vp_cb_mapped
                                + ((u64)s_d3d.vp_parity * MAX_DRAWS + dr->cb_slot) * VP_CB_STRIDE);
                            const u32 vh = (dr->vs_idx >= 0 && dr->vs_idx < VP_VS_CACHE) ? s_d3d.vp_vs[dr->vs_idx].hash : 0;
                            if (p < (int)sizeof b - 200)
                                p += snprintf(b + p, sizeof b - p, " | vs=0x%08X c64=(%.3g,%.3g,%.3g,%.3g) c65=(%.3g,%.3g,%.3g,%.3g) c467=(%.3g,%.3g,%.3g,%.3g)",
                                              vh, vc[64*4], vc[64*4+1], vc[64*4+2], vc[64*4+3],
                                              vc[65*4], vc[65*4+1], vc[65*4+2], vc[65*4+3],
                                              vc[467*4], vc[467*4+1], vc[467*4+2], vc[467*4+3]);
                            /* exec-time world rows + view translation: the constants the GPU will read */
                            if (p < (int)sizeof b - 300)
                                p += snprintf(b + p, sizeof b - p, " slot=%u c36=(%.3g,%.3g,%.3g) c37=(%.3g,%.3g,%.3g) c38=(%.3g,%.3g,%.3g) c39=(%.3g,%.3g,%.3g) c3=(%.3g,%.3g,%.3g) c4x=%.3g",
                                              dr->cb_slot, vc[36*4], vc[36*4+1], vc[36*4+2], vc[37*4], vc[37*4+1], vc[37*4+2],
                                              vc[38*4], vc[38*4+1], vc[38*4+2], vc[39*4], vc[39*4+1], vc[39*4+2],
                                              vc[3*4], vc[3*4+1], vc[3*4+2], vc[4*4]); }
                          /* [#111] the same program's inline constants read from VRAM
                           * NOW (execution) vs the record-time snapshot the shader got:
                           * a late writer (SPU/PPU patching after the FIFO went past)
                           * shows up as "vram" differing from "snap". */
                          { extern uint8_t* vm_base; extern u32 cellGcmResolveLocated(int, u32);
                            const u32 vo = cellGcmResolveLocated((dr->fp_addr & 3u) == 1, dr->fp_addr & ~3u);
                            static float kv[4 * 64], ks2[4 * 64];
                            const int nv = (vo != 0xFFFFFFFFu && vm_base) ? rsx_fp_extract_consts(vm_base + vo, 4096, kv, 64) : 0;
                            const int ns2 = (dr->fp_snap && dr->fp_snap_len) ? rsx_fp_extract_consts(dr->fp_snap, dr->fp_snap_len, ks2, 64) : 0;
                            int diff = 0;
                            for (int q = 0; q < nv && q < ns2 && q < 64; q++)
                                if (memcmp(&kv[q*4], &ks2[q*4], 16)) diff++;
                            /* FPK_SCAN=1 (once): every other copy of this program in local
                             * memory (same first instruction), with its inline constants --
                             * is a patched copy sitting somewhere we do not bind? */
                            { static int sc = -1; if (sc < 0) sc = getenv("FPK_SCAN") ? 1 : 0;
                              if (sc == 1 && vo != 0xFFFFFFFFu && vm_base) { sc = 2;
                                  const u8* prog = vm_base + vo;
                                  u8 first[16]; memcpy(first, prog, 16);
                                  const u32 lbase = cellGcmResolveLocated(1, 0);
                                  int hits = 0;
                                  for (u32 o2 = 0; o2 + 4096 <= 0x10000000u && hits < 40; o2 += 16) {
                                      const u8* q2 = vm_base + lbase + o2;
                                      if (memcmp(q2, first, 16)) continue;
                                      static float kk[4 * 64];
                                      const int n2 = rsx_fp_extract_consts(q2, 4096, kk, 64);
                                      char kb[600]; int kp = 0;
                                      for (int z = 0; z < n2 && z < 12 && kp < 520; z++)
                                          kp += snprintf(kb + kp, sizeof kb - kp, " k%d=(%.3g,%.3g,%.3g,%.3g)", z,
                                                         kk[z*4], kk[z*4+1], kk[z*4+2], kk[z*4+3]);
                                      fprintf(stderr, "[FPKSCAN] copy at local 0x%08X%s n=%d:%s%c", o2,
                                              (lbase + o2 == vo) ? " (BOUND)" : "", n2, kb, 10);
                                      hits++;
                                  }
                                  fprintf(stderr, "[FPKSCAN] %d copies of fp 0x%08X in local memory%c", hits, dr->fp_addr, 10);
                              } }
                            p += snprintf(b + p, sizeof b - p, " | snap=%d vram=%d differ=%d", ns2, nv, diff);
                            for (int q = 0; q < nv && q < 12 && p < (int)sizeof b - 80; q++)
                                if (memcmp(&kv[q*4], &ks2[q*4], 16))
                                    p += snprintf(b + p, sizeof b - p, " vk%d=(%.3g,%.3g,%.3g,%.3g)", q,
                                                  kv[q*4], kv[q*4+1], kv[q*4+2], kv[q*4+3]); }
                          fprintf(stderr, "%s%c", b, 10); } } }
                /* [#125] a GPU-fetch draw has no CPU-converted vertices: the
                 * fallback PSO would draw garbage from vp_vb, so drop it */
                if (dr->gpu_vtx && !dpso) { ((D3D12DrawRecord*)dr)->exec = 2; continue; }
                /* #225d: a CPU-path draw whose own VS is still compiling must wait too
                 * (the fallback pipeline would draw it with the wrong program) */
                if (!dpso && dr->vs_idx >= 0 && dr->vs_idx < s_d3d.vp_vs_n && !s_d3d.vp_vs[dr->vs_idx].vs &&
                    async_on()) {
                    if (draw_essential(dr)) async_wait_vs(s_d3d.vp_vs[dr->vs_idx].hash);   /* #229 */
                    if (!s_d3d.vp_vs[dr->vs_idx].vs) { ((D3D12DrawRecord*)dr)->exec = 2; continue; }
                }
                /* #249: the pipeline's sample count must match the bound target (a draw whose RT
                 * could not be bound, or the single-sample fallback pipeline on a twin) */
                if (dr_ms(dr) != cur_ms || (cur_ms > 1 && !dpso)) {
                    ((D3D12DrawRecord*)dr)->exec = 2;
                    { static int n = 0; if (n++ < 8) fprintf(stderr, "[MSAA] draw %u skipped: target x%d, draw x%d, pso %p%c",
                                                              d, cur_ms, dr_ms(dr), (void*)dpso, 10); }
                    continue;
                }
                s_d3d.cmd_list->lpVtbl->SetPipelineState(s_d3d.cmd_list,
                                                         dpso ? dpso : vpso);
                if (cur_rt >= 0 && (((u32)dr->fp_exp32 >> 20) & 0x11u) == 0x11u) {   /* #209b: depth test + write */
                    if (s_span_rt != cur_rt) { s_span_zdraws = 0; s_span_rt = cur_rt; }   /* (its depth may be cleared already) */
                    s_span_zdraws++;
                    s_span_ms = cur_ms > 1;   /* #249 */
                }
                if (dr->stencil_key & 1u)   /* [NHL local patch] guest stencil reference */
                    s_d3d.cmd_list->lpVtbl->OMSetStencilRef(s_d3d.cmd_list, dr->stencil_ref);
                /* Per-draw viewport: the guest rect when sane, else the
                 * full target. Scissor tracks the same rect. */
                {
                    float tw = (cur_rt >= 0) ? (float)s_d3d.off_rt[cur_rt].w : (float)s_d3d.width;
                    float th = (cur_rt >= 0) ? (float)s_d3d.off_rt[cur_rt].h : (float)s_d3d.height;
                    D3D12_VIEWPORT dvp = {0, 0, tw, th, 0.0f, 1.0f};
                    if (dr->vp_w >= 2 && dr->vp_h >= 2 &&
                        (float)(dr->vp_x + dr->vp_w) <= tw + 0.5f &&
                        (float)(dr->vp_y + dr->vp_h) <= th + 0.5f) {
                        dvp.TopLeftX = (float)dr->vp_x;
                        dvp.TopLeftY = (float)dr->vp_y;
                        dvp.Width    = (float)dr->vp_w;
                        dvp.Height   = (float)dr->vp_h;
                    }
                    /* #253: the guest rect -> the resource's texels (1 unless scaled) */
                    const float kx = ((cur_rt >= 0) ? (float)s_d3d.off_rt[cur_rt].rw : (float)s_dw) / tw;
                    const float ky = ((cur_rt >= 0) ? (float)s_d3d.off_rt[cur_rt].rh : (float)s_dh) / th;
                    if (kx != 1.0f || ky != 1.0f) {
                        dvp.TopLeftX *= kx; dvp.Width *= kx;
                        dvp.TopLeftY *= ky; dvp.Height *= ky;
                    }
                    /* WPOS back in guest pixels (fp_alpha.w; the #253 HLSL rewrite multiplies by it) */
                    if (rs_active() && s_d3d.vp_fpcb_mapped)
                        ((float*)((char*)s_d3d.vp_fpcb_mapped
                            + ((u64)s_d3d.vp_parity * MAX_DRAWS + dr->cb_slot) * VP_FPCB_STRIDE))[19] = 1.0f / kx;
                    D3D12_RECT dsc = {(LONG)(dvp.TopLeftX + 0.5f), (LONG)(dvp.TopLeftY + 0.5f),
                                      (LONG)(dvp.TopLeftX + dvp.Width + 0.5f),
                                      (LONG)(dvp.TopLeftY + dvp.Height + 0.5f)};
                    dvp_x = dvp.TopLeftX; dvp_y = dvp.TopLeftY;
                    dvp_w = dvp.Width;    dvp_h = dvp.Height;
                    s_d3d.cmd_list->lpVtbl->RSSetViewports(s_d3d.cmd_list, 1, &dvp);
                    s_d3d.cmd_list->lpVtbl->RSSetScissorRects(s_d3d.cmd_list, 1, &dsc);
                }
                /* Per-draw textures: bind this draw's t0-t3 SRV window.
                 * Any sampled offscreen RT transitions to PSR first (never
                 * one of the currently-bound colour targets). */
                for (int _u = 0; _u < D3D_TEX_UNITS; _u++) {
                    int rt = dr->tex_rt[_u];
                    if (rt >= 0 && rt != cur_rt &&
                        rt != cur_m[0] && rt != cur_m[1] && rt != cur_m[2])
                        off_rt_transition(rt, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                    if (dr->zmask & (1u << _u)) {   /* #190: a shadow map this draw samples */
                        const int zi = shadowz_find(dr->tex[_u].raw);
                        if (zi >= 0) shadowz_transition(zi, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                    }
                }
                D3D12_GPU_DESCRIPTOR_HANDLE gh = gh_base;
                gh.ptr += (u64)(DRAW_SRV_BASE + dr->srv_win * D3D_TEX_UNITS) * s_d3d.srv_inc;   /* #185 */
                s_d3d.cmd_list->lpVtbl->SetGraphicsRootDescriptorTable(s_d3d.cmd_list, 1, gh);
                /* Per-draw constants: this draw's vp_cb + FP texscale slots. */
                s_d3d.cmd_list->lpVtbl->SetGraphicsRootConstantBufferView(s_d3d.cmd_list, 0,
                    s_d3d.vp_cb->lpVtbl->GetGPUVirtualAddress(s_d3d.vp_cb)
                    + ((u64)s_d3d.vp_parity * MAX_DRAWS + dr->cb_slot) * VP_CB_STRIDE);
                if (s_d3d.vp_fpcb)
                    s_d3d.cmd_list->lpVtbl->SetGraphicsRootConstantBufferView(s_d3d.cmd_list, 2,
                        s_d3d.vp_fpcb->lpVtbl->GetGPUVirtualAddress(s_d3d.vp_fpcb)
                        + ((u64)s_d3d.vp_parity * MAX_DRAWS + dr->cb_slot) * VP_FPCB_STRIDE);
                /* DRAWARGS=<N>: the arguments actually handed to the GPU, plus
                 * the target they are aimed at. A zero vertex count, or a start
                 * vertex past the buffer, rasterises nothing while every other
                 * check upstream still says the draw is fine. */
                { static int dcap = -1, dn = 0;
                  if (dcap < 0) { const char* e = getenv("DRAWARGS"); dcap = e ? atoi(e) : 0; }
                  if (dcap && dn < dcap) { dn++;
                      D3D12_CPU_DESCRIPTOR_HANDLE _rh =
                          (cur_rt >= 0) ? off_rt_rtv(cur_rt) : rtv_handle;
                      fprintf(stderr, "[DRAWARGS] verts=%u start=%u rt=%d rec=0x%08X bound=0x%08X"
                              " tex0=0x%08X texrt=%d dvp=%.0f,%.0f %.0fx%.0f%c",
                              dr->vertex_count, dr->vb_byte_offset / 256, cur_rt,
                              dr->rt_off,
                              (cur_rt >= 0) ? s_d3d.off_rt[cur_rt].off : 0u,
                              dr->tex[0].raw, dr->tex_rt[0],
                              (double)dvp_x, (double)dvp_y,
                              (double)dvp_w, (double)dvp_h, 10); } }
                /* RT_REBIND=1: re-issue OMSetRenderTargets immediately before
                 * every draw instead of only when the target changes. A clear
                 * proves the RTV descriptor is good but not that the binding is
                 * still live at draw time -- ClearRenderTargetView writes
                 * through the descriptor and never consults the OM. If anything
                 * between the retarget and here drops the binding, this restores
                 * it. */
                { static int rb = -1;
                  if (rb < 0) { const char* e = getenv("RT_REBIND"); rb = e ? atoi(e) : 0; }
                  if (rb) {
                      D3D12_CPU_DESCRIPTOR_HANDLE brh[4];
                      UINT bn = 1;
                      brh[0] = (cur_rt >= 0) ? off_rt_rtv(cur_rt) : rtv_handle;
                      for (int _m = 0; _m < 3; _m++)
                          if (cur_m[_m] >= 0) brh[bn++] = off_rt_rtv(cur_m[_m]);
                      s_d3d.cmd_list->lpVtbl->OMSetRenderTargets(
                          s_d3d.cmd_list, bn, brh, FALSE, &dsv_handle);
                  } }
                dlog_add(s_d3d.frame_count, dr->fp_addr, (u32)d, dr->vertex_count, dr->rt_off, dr->tex[0].raw);
                if (s_q_frame) {   /* [#108] */
                    s_d3d.cmd_list->lpVtbl->BeginQuery(s_d3d.cmd_list, s_q_ps, D3D12_QUERY_TYPE_PIPELINE_STATISTICS, d);
                    s_d3d.cmd_list->lpVtbl->BeginQuery(s_d3d.cmd_list, s_q_oc, D3D12_QUERY_TYPE_OCCLUSION, d);
                }
                if (dr->indexed && s_d3d.vp_ib)   /* [NHL local patch #62] */
                    s_d3d.cmd_list->lpVtbl->DrawIndexedInstanced(s_d3d.cmd_list,
                        dr->vertex_count, 1, dr->ib_first, (INT)(dr->vb_byte_offset / 256), 0);
                else
                s_d3d.cmd_list->lpVtbl->DrawInstanced(s_d3d.cmd_list,
                    dr->vertex_count, 1, dr->vb_byte_offset / 256, 0);
                ((D3D12DrawRecord*)dr)->exec = 4;   /* #114 */
                if (s_q_frame) {
                    s_d3d.cmd_list->lpVtbl->EndQuery(s_d3d.cmd_list, s_q_ps, D3D12_QUERY_TYPE_PIPELINE_STATISTICS, d);
                    s_d3d.cmd_list->lpVtbl->EndQuery(s_d3d.cmd_list, s_q_oc, D3D12_QUERY_TYPE_OCCLUSION, d);
                    s_q_used[d] = 1; if (d + 1 > s_q_n) s_q_n = d + 1;
                }
                /* VP_SUBMIT=<N>: prove the VP pass actually reaches the GPU.
                 * "records exist" and "draws were submitted" are different
                 * claims, and every blank-output investigation conflates them. */
                { static int cap = -1, n = 0;
                  if (cap < 0) { const char* e = getenv("VP_SUBMIT"); cap = e ? atoi(e) : 0; }
                  if (cap && n < cap) { n++;
                    fprintf(stderr, "[VPSUBMIT] draw[%u] verts=%u pso=%s vp=%ux%u tex0=0x%X(set=%d) tex1=0x%X slot=%d fp=0x%X%c",
                            d, dr->vertex_count, dpso ? "guest-fp" : "fallback",
                            dr->vp_w, dr->vp_h, dr->tex[0].raw, dr->tex[0].set,
                            dr->tex[1].raw, dr->tex_slot, dr->fp_addr, 10);
                    fprintf(stderr, "            vs_idx=%d cb_slot=%u vbofs=%u cull=%u blend=%d%c",
                            dr->vs_idx, dr->cb_slot, dr->vb_byte_offset / 256,
                            dr->cull, dr->blend, 10); } }
            }
            if (perf_on()) s_perf_gpu += perf_now() - _rec0;   /* reuse: record time */
            ms_resolve_except(-1, NULL);   /* #249: write-backs, captures and the next batch read res */
            /* Leave the backbuffer bound for the dump/present epilogue. */
            if (cur_rt >= 0) {
                s_d3d.cmd_list->lpVtbl->OMSetRenderTargets(s_d3d.cmd_list, 1, &rtv_handle, FALSE, &dsv_handle);
                D3D12_VIEWPORT vp = {0, 0, (float)s_dw, (float)s_dh, 0.0f, 1.0f};   /* #253 */
                D3D12_RECT sc = {0, 0, (LONG)s_dw, (LONG)s_dh};
                s_d3d.cmd_list->lpVtbl->RSSetViewports(s_d3d.cmd_list, 1, &vp);
                s_d3d.cmd_list->lpVtbl->RSSetScissorRects(s_d3d.cmd_list, 1, &sc);
            }
        }
    }

    if (perf_on()) s_perf_rf += perf_now() - _rf0;
    HITCH_ADD(g_hitch_rf_us, ps3_qpc_us() - _hzrf);   /* #122 */
    rf_slow_report();   /* #226 */
    pl_save(0);   /* #128: write new pipelines once things are quiet */
    /* MEMPEEK=<hex addr>:<n>: n big-endian floats at a guest address each frame,
     * with a min/max so a buffer of constants is distinguishable from real data. */
    { const char* mp = getenv("MEMPEEK");
      if (mp) { extern uint8_t* vm_base; static int _n = 0;
        u32 addr = 0; int cnt = 8; sscanf(mp, "%x:%d", &addr, &cnt);
        if (vm_base && addr && _n++ < 6) {
            float lo = 1e30f, hi = -1e30f; u32 nz = 0;
            for (int i = 0; i < cnt; i++) {
                const u8* q = vm_base + addr + i * 4;
                u32 bits = ((u32)q[0]<<24)|((u32)q[1]<<16)|((u32)q[2]<<8)|q[3];
                float f; memcpy(&f, &bits, 4);
                if (f == f && f < 1e30f && f > -1e30f) { if (f < lo) lo = f; if (f > hi) hi = f; }
                if (bits) nz++;
            }
            fprintf(stderr, "[MEMPEEK] 0x%08X n=%d nonzero=%u range[%g..%g]%c",
                    addr, cnt, nz, lo, hi, 10);
        } } }
    /* VRAMSCAN=1: which 64 KB blocks of guest memory change between frames.
     * "nothing writes the buffer we read" is only half an answer -- this says
     * where the producer IS writing, so the two can be matched up. */
    { static int vs = -1; if (vs < 0) vs = getenv("VRAMSCAN") ? 1 : 0;
      if (vs) { extern uint8_t* vm_base;
        static u32 prev[512]; static int have = 0; static int reported = 0;
        u32 base = 0xC6000000u; u32 nblk = 512;          /* 32 MB window */
        const char* bs = getenv("VRAMSCAN_BASE");
        if (bs) base = (u32)strtoul(bs, NULL, 0);
        if (vm_base && reported < 6) {
            u32 changed = 0; char list[512]; int ln = 0;
            for (u32 b = 0; b < nblk; b++) {
                const u8* p = vm_base + base + b * 0x10000u;
                u32 h = 2166136261u;
                for (u32 i = 0; i < 0x10000u; i += 61) h = (h ^ p[i]) * 16777619u;
                if (have && h != prev[b]) { changed++;
                    if (ln < 400) ln += snprintf(list + ln, sizeof(list) - ln,
                                                 " +0x%X", b * 0x10000u); }
                prev[b] = h;
            }
            if (have && changed) { reported++;
                fprintf(stderr, "[VRAMSCAN] base=0x%08X %u/512 blocks changed:%s%c",
                        base, changed, list, 10); }
            have = 1;
        } } }
    /* FRAME_BUDGET=1: geometry a frame asked for vs what the batch can hold.
     * Reported from render_frame so it works under RSX_ACCUM_FRAME too -- it
     * used to live in the clear-boundary present path, which accum bypasses, so
     * it silently produced nothing in exactly the configuration being used. */
    { static int fb = -1;
      if (fb < 0) { const char* e = getenv("FRAME_BUDGET"); fb = e ? atoi(e) : 0; }
      if (fb && s_req_draws) { static int n = 0; if (n++ < 40)
        fprintf(stderr, "[BUDGET] frame: requested %llu verts / %llu draws, "
                        "buffer holds %u verts, %llu draws truncated%c",
                (unsigned long long)s_req_verts, (unsigned long long)s_req_draws,
                MAX_VERTICES, (unsigned long long)s_drop_draws, 10); }
      /* [#105] never silent: truncated draws are logged every 5 s */
      { static ULONGLONG t_last = 0; static u64 acc = 0; acc += s_drop_draws;
        const ULONGLONG t = GetTickCount64();
        if (acc && t - t_last >= 5000) { t_last = t;
            fprintf(stderr, "[VBFULL] vertex buffer (%u verts/frame) full: %llu draws truncated in the last 5 s (last frame asked %llu verts)%c",
                    MAX_VERTICES, (unsigned long long)acc, (unsigned long long)s_req_verts, 10); acc = 0; } }
      s_req_verts = 0; s_req_draws = 0; s_drop_draws = 0; }
    if (perf_on()) {
        static double s_t_prev = 0.0; static int s_n = 0;
        double now = perf_now();
        if (s_t_prev > 0.0) s_perf_frame += now - s_t_prev;
        s_t_prev = now;
        if (++s_n % 20 == 0) {
            double fr = s_perf_frame > 0 ? s_perf_frame : 1;
            fprintf(stderr, "[PERF] %.2f fps | tex %.2fs (%.0f%%, %d calls) | vtx %.2fs"
                            " (%.0f%%, %.0fk verts) | render_frame %.2fs (%.0f%%)"
                            " | prepass %.2fs (%.0f%%) [srv %.2fs %.0f%% | pso %.2fs %.0f%% %d calls %d MISS %dKB hashed]"
                            " | record %.2fs (%.0f%%, #184) | guest %.2fs (%.0f%%)%c",
                    20.0 / fr,
                    s_perf_tex, 100.0 * s_perf_tex / fr, s_perf_ntex,
                    s_perf_vtx, 100.0 * s_perf_vtx / fr,
                    (double)s_perf_nverts / 1000.0,
                    s_perf_rf, 100.0 * s_perf_rf / fr,
                    s_perf_pre, 100.0 * s_perf_pre / fr,
                    s_perf_srv, 100.0 * s_perf_srv / fr,
                    s_perf_pso, 100.0 * s_perf_pso / fr,
                    s_perf_pso_calls, s_perf_pso_miss, s_perf_pso_hashbytes / 1024,
                    s_perf_gpu, 100.0 * s_perf_gpu / fr,
                    fr - s_perf_rf, 100.0 * (fr - s_perf_rf) / fr, 10);
            fprintf(stderr, "[PERFVTX] verts(calls): quads %llu(%llu) tris %llu(%llu) strip %llu(%llu) "
                            "quads-idx %llu(%llu) tris-idx-cpu %llu(%llu) tris-idx-gpu %llu(%llu) strip-idx %llu(%llu)"
                            " | idx fallback: partial %llu sparse %llu room %llu%c",
                    s_vcnt[0], s_vcalls[0], s_vcnt[1], s_vcalls[1], s_vcnt[2], s_vcalls[2],
                    s_vcnt[3], s_vcalls[3], s_vcnt[4], s_vcalls[4], s_vcnt[5], s_vcalls[5],
                    s_vcnt[6], s_vcalls[6], s_idx_fb_partial, s_idx_fb_sparse, s_idx_fb_room, 10);
            memset(s_vcnt, 0, sizeof s_vcnt); memset(s_vcalls, 0, sizeof s_vcalls);
            s_idx_fb_partial = s_idx_fb_sparse = s_idx_fb_room = 0;
            s_perf_frame = 0.0; s_perf_tex = 0.0; s_perf_vtx = 0.0; s_perf_rf = 0.0;
            s_perf_gpu = 0.0; s_perf_pre = 0.0; s_perf_srv = 0.0; s_perf_pso = 0.0;
            s_perf_pso_calls = 0; s_perf_pso_miss = 0; s_perf_pso_hashbytes = 0;
            s_perf_ntex = 0; s_perf_texbytes = 0; s_perf_nverts = 0;
        }
    }
    { static int dt = -1;
      if (dt < 0) { const char* e = getenv("RSX_DBG_TRACK"); dt = e ? atoi(e) : 0; }
      if (dt) { static int fr = 0; fr++;
        if (s_lock_n)
            fprintf(stderr, "[RSX_DBG_TRACK] frame %d: %u duck draws, ndc=(%.3f,%.3f)%c",
                    fr, s_lock_n, (float)(s_lock_sx/s_lock_n), (float)(s_lock_sy/s_lock_n), 10);
        else if (fr % 20 == 0)
            fprintf(stderr, "[RSX_DBG_TRACK] frame %d: NO duck draws%c", fr, 10);
      } }
    if (s_lock_n) {                       /* publish this frame's centroid */
        s_lock_x = (float)(s_lock_sx / s_lock_n);
        s_lock_y = (float)(s_lock_sy / s_lock_n);
        s_lock_valid = 1;
        s_lock_sx = s_lock_sy = 0.0; s_lock_n = 0;
    }
    s_dbg_last_draws = s_d3d.draw_count;
    s_d3d.vb_offset  = 0; /* reset for next frame */
    s_d3d.vp_vb_offset = 0; s_d3d.vp_ib_offset = 0; s_d3d.vp_raw_offset = 0;
    s_d3d.draw_count = 0;
    s_fp_arena_off = 0; s_fp_snap_cur = NULL; s_fp_snap_cur_len = 0;   /* #82: batch consumed */
    s_fp_arena_gen++;   /* #162: snapshot pointers may now hold other programs */
    s_d3d.vp_parity ^= 1;  /* next frame's records go to the other half */

    /* Debug: RTT_SAVERT=<hex raw offset>[:frame] copies that offscreen RT
     * into a readback buffer this frame and writes rt_save.bmp (half-float
     * RTs are tonemapped |v| -> byte). */
    static ID3D12Resource* s_rtsave_buf = NULL;
    static u32 s_rtsave_state = 0;   /* 1 = copy queued this frame */
    static u32 s_rtsave_w, s_rtsave_h, s_rtsave_pitch, s_rtsave_dxgi;
    static u32 s_rtsave_seq = 0;      /* RTT_SAVERT_EVERY file number */
    { const char* sv = getenv("RTT_SAVERT");
      static int _done = 0;
      static u32 _skip = 0xFFFFFFFFu;
      if (_skip == 0xFFFFFFFFu) { const char* sk = getenv("RTT_SAVERT_SKIP");
          _skip = sk ? (u32)strtoul(sk, NULL, 0) : 0; }   /* frame 60 is far too
          early for a title that takes a minute to reach its menu */
      /* RTT_SAVERT_AFTER=<seconds> triggers on wall time instead of a frame
       * count. This title runs its menu at a few frames a second, so a frame
       * number is not a usable way to say "once it has settled". */
      static u32 _after = 0xFFFFFFFFu;
      if (_after == 0xFFFFFFFFu) { const char* sa = getenv("RTT_SAVERT_AFTER");
          _after = sa ? (u32)strtoul(sa, NULL, 0) : 0; }
      static clock_t _t0 = 0;
      if (!_t0) _t0 = clock();
      /* [NHL local patch] RTT_SAVERT_EVERY=<seconds>: keep saving, one numbered
       * rt_save_NN.bmp per period (up to 99), starting at RTT_SAVERT_AFTER. A
       * user-driven run reaches the screen of interest at an unknown time. */
      static u32 _every = 0xFFFFFFFFu, _next = 0;
      if (_every == 0xFFFFFFFFu) { const char* se = getenv("RTT_SAVERT_EVERY");
          _every = se ? (u32)strtoul(se, NULL, 0) : 0; _next = _after; }
      const u32 _el = (u32)((clock() - _t0) / CLOCKS_PER_SEC);
      const int _due = _every ? (_el >= _next && s_rtsave_seq < 99)
          : _after ? (_el >= _after)
          : (s_d3d.frame_count > 60 + _skip);
      { static int _p1 = 0;
        if (sv && _due && !_p1) { _p1 = 1;
            printf("[RTT_SAVERT] due: want=0x%lX found_rt=%d frame=%u\n",
                   strtoul(sv, NULL, 16), off_rt_find((u32)strtoul(sv, NULL, 16)),
                   (unsigned)s_d3d.frame_count); fflush(stdout); } }
      if (sv && !_done && _due) {
        int rt = off_rt_find((u32)strtoul(sv, NULL, 16));
        fprintf(stderr, "[RTT_SAVERT] rt=%d res=%p\n", rt, rt >= 0 ? (void*)s_d3d.off_rt[rt].res : NULL);
        if (rt >= 0 && s_d3d.off_rt[rt].res && s_d3d.off_rt[rt].rw == s_d3d.off_rt[rt].w) {   /* #253: 1x only (diag) */
            OffRT* r = &s_d3d.off_rt[rt];
            u32 bpp = (r->dxgi == DXGI_FORMAT_R16G16B16A16_FLOAT) ? 8 :
                      (r->dxgi == DXGI_FORMAT_R32G32B32A32_FLOAT) ? 16 : 4;
            u32 pitch = (r->w * bpp + 255) & ~255u;
            fprintf(stderr, "[RTT_SAVERT] rt=%d w=%u h=%u bpp=%u pitch=%u dxgi=%d bytes=%llu\n",
                    rt, r->w, r->h, bpp, pitch, (int)r->dxgi,
                    (unsigned long long)((u64)pitch * r->h));
            if (!s_rtsave_buf) {
                D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_READBACK;
                D3D12_RESOURCE_DESC rd = {0};
                rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                rd.Width = (u64)pitch * r->h; rd.Height = 1; rd.DepthOrArraySize = 1;
                rd.MipLevels = 1; rd.SampleDesc.Count = 1;
                rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                s_d3d.device->lpVtbl->CreateCommittedResource(
                    s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &rd,
                    D3D12_RESOURCE_STATE_COPY_DEST, NULL,
                    &IID_ID3D12Resource, (void**)&s_rtsave_buf);
            }
            if (!s_rtsave_buf) fprintf(stderr, "[RTT_SAVERT] readback buffer alloc FAILED\n");
            if (s_rtsave_buf) {
                off_rt_transition(rt, D3D12_RESOURCE_STATE_COPY_SOURCE);
                D3D12_TEXTURE_COPY_LOCATION cdst = {0}, csrc = {0};
                cdst.pResource = s_rtsave_buf;
                cdst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                cdst.PlacedFootprint.Footprint.Format   = (DXGI_FORMAT)r->dxgi;
                cdst.PlacedFootprint.Footprint.Width    = r->w;
                cdst.PlacedFootprint.Footprint.Height   = r->h;
                cdst.PlacedFootprint.Footprint.Depth    = 1;
                cdst.PlacedFootprint.Footprint.RowPitch = pitch;
                csrc.pResource = r->res;
                csrc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                s_d3d.cmd_list->lpVtbl->CopyTextureRegion(s_d3d.cmd_list, &cdst, 0, 0, 0, &csrc, NULL);
                off_rt_transition(rt, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                s_rtsave_state = 1;
                s_rtsave_w = r->w; s_rtsave_h = r->h;
                s_rtsave_pitch = pitch; s_rtsave_dxgi = r->dxgi;
                fprintf(stderr, "[RTT_SAVERT] copy queued: rt=%d %ux%u pitch=%u t=%us\n",
                        rt, r->w, r->h, pitch, _el);
                if (_every) _next = _el + _every; else _done = 1;
            }
        }
      } }

    /* CELLMARK_DUMP_MINDRAWS=<N>: only consider frames carrying at least N draw
     * records. Most render_frame calls come from the guest flip with a batch the
     * clear boundary already drained, so they present an empty backbuffer -- and
     * sampling those wastes every dump on a black image while the frames that do
     * hold the scene go uncaptured. */
    { static int mind = -1;
      if (mind < 0) { const char* e = getenv("CELLMARK_DUMP_MINDRAWS");
                      mind = e ? atoi(e) : 0; }
      /* draw_count is already reset by here; s_dbg_last_draws holds the
       * count this frame actually rendered. */
      if (mind > 0 && s_dbg_last_draws < (u32)mind) goto skip_dump_consider; }
    if (s_d3d.dump_skip_left > 0 && s_d3d.dump_frames_left > 0) s_d3d.dump_skip_left--;
skip_dump_consider: ;
    guest_fb_present(fi);
    composite_present(fi);
    if (_disp_off) {   /* #132: remember this display buffer's image */
        const int ks = disp_keep_slot(_disp_off, 1);
        if (ks >= 0) {
            disp_barrier(s_d3d.render_targets[fi], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
            disp_barrier(s_keep_res[ks], D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
            s_d3d.cmd_list->lpVtbl->CopyResource(s_d3d.cmd_list, s_keep_res[ks], s_d3d.render_targets[fi]);
            disp_barrier(s_keep_res[ks], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
            disp_barrier(s_d3d.render_targets[fi], D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
            s_keep_valid[ks] = 1;
        }
    }
    s_disp_colour_cleared = 0;
    int dumping = (s_d3d.dump_frames_left > 0 && s_d3d.readback_buf
                   && s_d3d.dump_skip_left == 0);
    { static int mind2 = -1;
      if (mind2 < 0) { const char* e = getenv("CELLMARK_DUMP_MINDRAWS");
                       mind2 = e ? atoi(e) : 0; }
      if (mind2 > 0 && s_dbg_last_draws < (u32)mind2) dumping = 0; }
    /* CELLMARK_DUMP_EVERY=N: after each dump, skip N-1 frames -- samples the whole
     * run instead of one contiguous window, so a short burst of real content
     * can't fall between the dumped frames. */
    if (dumping) {
        static int s_every = -1;
        if (s_every < 0) { const char* e = getenv("CELLMARK_DUMP_EVERY");
                           s_every = e ? atoi(e) : 0; }
        if (s_every > 1) s_d3d.dump_skip_left = s_every - 1;
    }
    if (dumping) {
        /* RT -> COPY_SOURCE, copy into the readback buffer, then -> PRESENT. */
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
        s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &barrier);

        D3D12_TEXTURE_COPY_LOCATION dst = {0}, src = {0};
        dst.pResource = s_d3d.readback_buf;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Offset = 0;
        dst.PlacedFootprint.Footprint.Format   = DXGI_FORMAT_R8G8B8A8_UNORM;
        dst.PlacedFootprint.Footprint.Width    = s_dw;
        dst.PlacedFootprint.Footprint.Height   = s_dh;
        dst.PlacedFootprint.Footprint.Depth    = 1;
        dst.PlacedFootprint.Footprint.RowPitch = s_d3d.readback_pitch;
        src.pResource = s_d3d.render_targets[fi];
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        s_d3d.cmd_list->lpVtbl->CopyTextureRegion(s_d3d.cmd_list, &dst, 0, 0, 0, &src, NULL);

        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
        s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &barrier);
    } else {
        /* End-of-frame fallback snapshot: used when the frame never contained a
         * reduced-viewport pass to capture mid-frame. */
        screen_copy_capture(fi);
        /* Transition render target to PRESENT state */
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &barrier);
    }

    if (s_sc_on && s_present_this_frame) sc_blit(fi);   /* [#163] guest surface -> window */
    /* [#108] resolve this pass's per-draw queries (contiguous runs only:
     * never resolve an index that was not begun and ended) */
    if (s_q_frame) {
        for (u32 a = 0; a < s_q_n; ) {
            if (!s_q_used[a]) { a++; continue; }
            u32 b = a; while (b < s_q_n && s_q_used[b]) b++;
            s_d3d.cmd_list->lpVtbl->ResolveQueryData(s_d3d.cmd_list, s_q_ps,
                D3D12_QUERY_TYPE_PIPELINE_STATISTICS, a, b - a, s_q_rb,
                (u64)a * sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS));
            s_d3d.cmd_list->lpVtbl->ResolveQueryData(s_d3d.cmd_list, s_q_oc,
                D3D12_QUERY_TYPE_OCCLUSION, a, b - a, s_q_rb, Q_OCC_BASE + (u64)a * 8u);
            a = b;
        }
        s_q_ready = (s_q_n > 0);
        s_q_frame = 0;
    }
    /* Close and execute */
    s_d3d.cmd_list->lpVtbl->Close(s_d3d.cmd_list);
    ID3D12CommandList* cmd_lists[] = {(ID3D12CommandList*)s_d3d.cmd_list};
    s_d3d.cmd_queue->lpVtbl->ExecuteCommandLists(s_d3d.cmd_queue, 1, cmd_lists);
    upr_on_submit();   /* #219 */
    txh_on_submit();   /* #220 */

    /* D3D12_IQ=1: drain the debug layer's message queue after submitting the
     * frame. The legacy (is_vp=0) DrawInstanced path silently produced ZERO
     * pixels in flOw's render injection while ClearRenderTargetView worked, and
     * every input (verts, offsets, layout, MVP, PSO, root sig, depth, cull) was
     * verified correct -- so the only remaining explanation is a validation
     * error the debug layer is swallowing. Off unless the env var is set. */
    { static int _iq = -1;
      if (_iq < 0) { const char* e = getenv("D3D12_IQ"); _iq = e ? 1 : 0; }
      if (_iq && s_d3d.device) {
        ID3D12InfoQueue* iq = NULL;
        if (SUCCEEDED(s_d3d.device->lpVtbl->QueryInterface(
                s_d3d.device, &IID_ID3D12InfoQueue, (void**)&iq)) && iq) {
            UINT64 n = iq->lpVtbl->GetNumStoredMessages(iq);
            static int _printed = 0;
            for (UINT64 mi = 0; mi < n && _printed < 80; mi++) {
                SIZE_T len = 0;
                iq->lpVtbl->GetMessage(iq, mi, NULL, &len);
                D3D12_MESSAGE* m = (D3D12_MESSAGE*)malloc(len);
                if (m && SUCCEEDED(iq->lpVtbl->GetMessage(iq, mi, m, &len))) {
                    fprintf(stderr, "[D3D12-IQ][sev=%d id=%d] %s\n",
                            (int)m->Severity, (int)m->ID, m->pDescription);
                    _printed++;
                }
                free(m);
            }
            if (n) iq->lpVtbl->ClearStoredMessages(iq);
            iq->lpVtbl->Release(iq);
            fflush(stderr);
        }
      } }

    if (s_sc_dump_pending) {
        s_sc_dump_pending = 0;
        wait_for_gpu();
        s_dump_name = "screencopy";
        dump_backbuffer_bmp();
        s_dump_name = NULL;
    }
    if (dumping) {
        wait_for_gpu();            /* ensure the copy finished before mapping */
        dump_backbuffer_bmp();
        s_d3d.dump_frames_left--;
    }

    if (s_rtsave_state) {
        fprintf(stderr, "[RTT_SAVERT] writing rt_save.bmp %ux%u\n", s_rtsave_w, s_rtsave_h);
        if (!dumping) { double _g = perf_on() ? perf_now() : 0.0;
                        wait_for_gpu();
                        if (perf_on()) s_perf_gpu += perf_now() - _g; }
        void* mp = NULL; D3D12_RANGE rr = {0, (SIZE_T)s_rtsave_pitch * s_rtsave_h};
        if (SUCCEEDED(s_rtsave_buf->lpVtbl->Map(s_rtsave_buf, 0, &rr, &mp)) && mp) {
            char _fn[32];
            if (getenv("RTT_SAVERT_EVERY")) snprintf(_fn, sizeof _fn, "rt_save_%02u.bmp", s_rtsave_seq++);
            else snprintf(_fn, sizeof _fn, "rt_save.bmp");
            FILE* f = fopen(_fn, "wb");
            if (f) {
                u32 w = s_rtsave_w, h = s_rtsave_h;
                u32 rowb = (w * 3 + 3) & ~3u;
                u32 datasz = rowb * h;
                u8 hdr[54] = {0};
                hdr[0]='B'; hdr[1]='M';
                *(u32*)(hdr+2) = 54 + datasz; *(u32*)(hdr+10) = 54;
                *(u32*)(hdr+14) = 40; *(int*)(hdr+18) = (int)w; *(int*)(hdr+22) = (int)h;
                *(u16*)(hdr+26) = 1; *(u16*)(hdr+28) = 24; *(u32*)(hdr+34) = datasz;
                fwrite(hdr, 1, 54, f);
                u8* line = (u8*)malloc(rowb);
                for (int y = (int)h - 1; y >= 0; y--) {
                    const u8* srow = (const u8*)mp + (u64)y * s_rtsave_pitch;
                    memset(line, 0, rowb);
                    for (u32 x = 0; x < w; x++) {
                        float rv, gv, bv;
                        if (s_rtsave_dxgi == DXGI_FORMAT_R16G16B16A16_FLOAT) {
                            const u16* hp16 = (const u16*)(srow + (u64)x * 8);
                            /* crude half->float: sign|exp|mant */
                            float v[3];
                            for (int c2 = 0; c2 < 3; c2++) {
                                u16 hv = hp16[c2];
                                u32 sign = (hv >> 15) & 1, exp = (hv >> 10) & 0x1F, man = hv & 0x3FF;
                                float fv;
                                if (exp == 0) fv = (float)man / 16777216.0f;
                                else { u32 fb = (sign << 31) | ((exp - 15 + 127) << 23) | (man << 13);
                                       memcpy(&fv, &fb, 4); }
                                v[c2] = fv;
                            }
                            rv = v[0]; gv = v[1]; bv = v[2];
                        } else if (s_rtsave_dxgi == DXGI_FORMAT_R32G32B32A32_FLOAT) {
                            const float* fp32 = (const float*)(srow + (u64)x * 16);
                            rv = fp32[0]; gv = fp32[1]; bv = fp32[2];
                        } else {
                            const u8* p8 = srow + (u64)x * 4;
                            rv = p8[0] / 255.0f; gv = p8[1] / 255.0f; bv = p8[2] / 255.0f;
                        }
                        /* RTT_SAVEA=1: show the alpha lane in RED (gates
                         * like wave's mask.w live there). */
                        if (getenv("RTT_SAVEA")) {
                            float av;
                            if (s_rtsave_dxgi == DXGI_FORMAT_R16G16B16A16_FLOAT) {
                                const u16* hp16 = (const u16*)(srow + (u64)x * 8);
                                u16 hv = hp16[3];
                                u32 sign = (hv >> 15) & 1, exp = (hv >> 10) & 0x1F, man = hv & 0x3FF;
                                if (exp == 0) av = (float)man / 16777216.0f;
                                else { u32 fb = (sign << 31) | ((exp - 15 + 127) << 23) | (man << 13);
                                       memcpy(&av, &fb, 4); }
                            } else if (s_rtsave_dxgi == DXGI_FORMAT_R32G32B32A32_FLOAT) {
                                av = ((const float*)(srow + (u64)x * 16))[3];
                            } else {
                                av = (srow + (u64)x * 4)[3] / 255.0f;
                            }
                            rv = av;
                        }
                        /* |v| tonemap so signed heights are visible */
                        float ar = rv < 0 ? -rv : rv, ag = gv < 0 ? -gv : gv, ab = bv < 0 ? -bv : bv;
                        if (ar > 1) ar = 1; if (ag > 1) ag = 1; if (ab > 1) ab = 1;
                        line[x*3+0] = (u8)(ab * 255.0f);
                        line[x*3+1] = (u8)(ag * 255.0f);
                        line[x*3+2] = (u8)(ar * 255.0f);
                    }
                    fwrite(line, 1, rowb, f);
                }
                free(line);
                fclose(f);
                printf("[D3D12] wrote rt_save.bmp (%ux%u dxgi=%u)\n", w, h, s_rtsave_dxgi);
            }
            s_rtsave_buf->lpVtbl->Unmap(s_rtsave_buf, 0, NULL);
        }
        s_rtsave_state = 0;
    }

    /* rsxcap: snapshot whole frames (backbuffer + every offscreen RT + a draw
     * manifest) at RSX_CAP=start[:count[:stride]] frame_counts. Runs before the
     * present transition would matter -- the backbuffer is in PRESENT here and
     * is restored to PRESENT after each readback. */
    /* [#107] F9 hotkey capture: 4 consecutive presents (a game frame is split
     * over two presents, so this always gets both halves). */
    { static u32 s_hot_idx = 50;
      /* [#108] CAP_HOT_AT=<frame_count>: the F9 capture, fired once at that frame
       * (it starts with the NEXT present, so its draws carry queries) */
      int fired = 0;
      { static long long at = -2; if (at == -2) { const char* e = getenv("CAP_HOT_AT"); at = e ? atoll(e) : -1; }
        if (at >= 0 && s_d3d.frame_count >= (u64)at) { at = -1; fired = 1; InterlockedExchange(&s_cap_hot_req, cap_hot_n());
            fprintf(stderr, "[CAPKEY] CAP_HOT_AT fired at frame %llu%c", (unsigned long long)s_d3d.frame_count, 10); } }
      if (!fired && s_cap_hot_req > 0 && s_dbg_last_draws > 0 && s_hot_idx < 950) {
          if (InterlockedDecrement(&s_cap_hot_req) >= 0) {
              fprintf(stderr, "[CAPKEY] F9 capture frame_%02u (frame_count=%llu draws=%u)%c",
                      s_hot_idx, (unsigned long long)s_d3d.frame_count, s_dbg_last_draws, 10);
              rsx_capture_frame(fi, s_dbg_last_draws, s_hot_idx++);
          }
      } }
    { const char* cap = getenv("RSX_CAP");
      if (cap) {
        static int s_cap_parsed = 0;
        static u32 s_cap_start = 0, s_cap_count = 1, s_cap_stride = 1, s_cap_done = 0;
        if (!s_cap_parsed) {
            s_cap_parsed = 1;
            u32 a = 0, b = 1, c = 1;
            int n = sscanf(cap, "%u:%u:%u", &a, &b, &c);
            s_cap_start  = (n >= 1 && a) ? a : 200;
            s_cap_count  = (n >= 2 && b) ? b : 1;
            s_cap_stride = (n >= 3 && c) ? c : 1;
        }
        /* [NHL local patch] RSX_CAP_MINDRAWS=<n>: ignore the start frame and
         * capture frames carrying at least n draws, every `stride` frames from
         * the first such frame (gameplay has 300+, menus ~70-180). */
        static int s_cap_min = -1; static u64 s_cap_next = 0;
        if (s_cap_min < 0) { const char* e = getenv("RSX_CAP_MINDRAWS"); s_cap_min = e ? atoi(e) : 0; }
        if (s_cap_min > 0 && s_cap_done < s_cap_count) {
            if (s_dbg_last_draws >= (u32)s_cap_min && s_d3d.frame_count >= s_cap_next) {
                rsx_capture_frame(fi, s_dbg_last_draws, s_cap_done);
                s_cap_done++;
                s_cap_next = s_d3d.frame_count + s_cap_stride;
            }
        } else
        if (s_cap_done < s_cap_count) {
            u32 target = s_cap_start + s_cap_done * s_cap_stride;
            if (s_d3d.frame_count >= target && s_dbg_last_draws > 0) {
                rsx_capture_frame(fi, s_dbg_last_draws, s_cap_done);
                s_cap_done++;
            }
        }
      } }

    /* [NHL local patch] PIXEL_PROBE=<x>,<y>,<RRGGBB>: find which draw paints a
     * pixel. Every 30 frames read the pixel back; once it shows the target
     * colour (within 6 per channel), skip one draw per frame (k = 0, 1, ...)
     * and log the pixel each time. The op whose removal changes the colour is
     * the one that paints it. One pass over the frame's ops, then stop. */
    if (s_probe_mode < 0) {
        const char* e = getenv("PIXEL_PROBE");
        s_probe_mode = 2;
        if (e) { u32 x = 0, y = 0, t = 0;
            if (sscanf(e, "%u,%u,%x", &x, &y, &t) == 3) {
                s_probe_x = x; s_probe_y = y; s_probe_target = t; s_probe_mode = 0;
                fprintf(stderr, "[PROBE] watching pixel (%u,%u) for #%06X\n", x, y, t);
            } }
    }
    if (s_probe_mode == 0 || s_probe_mode == 1) {
        int read =(s_probe_mode == 1) || (s_d3d.frame_count % 30u) == 0;
        if (read && s_present_this_frame) {
            cap_readback_write(s_d3d.render_targets[fi], s_dw, s_dh,   /* #253 */
                               DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_STATE_PRESENT, fi, NULL);
            const u32 px = s_probe_px, tg = s_probe_target;
            #define PCH(v, s) ((int)(((v) >> (s)) & 0xFFu))
            const int match = abs(PCH(px,16) - PCH(tg,16)) <= 6 && abs(PCH(px,8) - PCH(tg,8)) <= 6 &&
                              abs(PCH(px,0) - PCH(tg,0)) <= 6;
            #undef PCH
            if (s_probe_mode == 0) {
                if (match) {
                    s_probe_mode = 1; s_probe_k = 0;
                    fprintf(stderr, "[PROBE] frame %llu: pixel is #%06X -- bisecting %u ops\n",
                            (unsigned long long)s_d3d.frame_count, px, s_dbg_last_draws);
                }
            } else {
                fprintf(stderr, "[PROBE] skip op%03d %-150s -> #%06X%s\n", s_probe_k,
                        s_probe_info[0] ? s_probe_info : "(none)", px, match ? "" : "   <== CHANGED");
                s_probe_k++;
                if ((u32)s_probe_k >= s_dbg_last_draws + 1u) {
                    s_probe_mode = 2;
                    fprintf(stderr, "[PROBE] done\n");
                }
            }
            fflush(stderr);
        }
    }

    /* Present */
    /* [NHL local patch] RSX_VSYNC=0: present without waiting for vblank. The ticker
     * thread that presents is also the one that drains the FIFO and writes the RSX
     * fences, so a vsync-blocked Present stalls every guest thread waiting on a
     * fence for up to a refresh. */
    { extern int cellGcm_tl_on(void); extern void cellGcm_tl(const char*, u32, u32, u32, u32);
      extern unsigned cellGcm_flip_request_count(void);
      if (cellGcm_tl_on()) cellGcm_tl("rframe", cellGcm_flip_request_count(), s_d3d.draw_count,
                                      (u32)s_present_this_frame, 0); }
    int held = 0;
    if (s_present_this_frame) {
        static int s_sync = -1;
        if (s_sync < 0) { const char* e = getenv("RSX_VSYNC"); s_sync = (e && *e == '0') ? 0 : 1; }
        /* [#163] fullscreen + VRR: no vsync wait, tearing allowed -> FreeSync/G-SYNC show
         * each frame when it arrives */
        if (vrr_active()) {
            /* [#164] ...and on the vblank grid: the frame is submitted now, the Present
             * waits for the ticker's next vblank (rsx_d3d12_backend_vblank). Presented
             * whenever the ticker noticed the flip, VRR showed that timing jitter
             * directly (7/30 ms gap pairs in play218). NHL_VRR_PACE=0 = present at once. */
            static int s_pace = -1; if (s_pace < 0) { const char* e = getenv("NHL_VRR_PACE"); s_pace = (e && *e == '0') ? 0 : 1; }
            if (s_pace) { s_pp_pending = 1; held = 1; }
            else present_now(1);
        } else
        { HRESULT _ph = s_d3d.swap_chain->lpVtbl->Present(s_d3d.swap_chain, (UINT)s_sync, 0);   /* skipped for an offscreen-only batch */
          if (_ph == (HRESULT)0x887A0005L || _ph == (HRESULT)0x887A0007L) dred_dump("Present"); }
    }

    if (!held) move_to_next_frame();   /* [#164] a held present advances when it is issued */

    s_d3d.frame_count++;

    /* [NHL local patch #74] RSX_FPRINT=1: once a second, how many of the frames
     * finished in that second had distinct vertex-constant streams. 1 distinct
     * of N while the game runs = the same frame is being replayed. */
    if (g_rsx_fprint_on < 0) g_rsx_fprint_on = getenv("RSX_FPRINT") ? 1 : 0;
    if (g_rsx_fprint_on) {
        static ULONGLONG s_ft0 = 0; static u64 s_fh[64]; static u32 s_fnh = 0, s_ff = 0, s_fover = 0;
        static u64 s_flast = 0; static u32 s_frun = 0;
        const u64 h = g_rsx_fprint_hash; const u32 nc = g_rsx_fprint_n;
        g_rsx_fprint_hash = 1469598103934665603ull; g_rsx_fprint_n = 0;
        s_ff++;
        { int seen = 0; for (u32 i = 0; i < s_fnh; i++) if (s_fh[i] == h) { seen = 1; break; }
          if (!seen) { if (s_fnh < 64) s_fh[s_fnh++] = h; else s_fover++; } }
        s_frun = (h == s_flast) ? s_frun + 1 : 0; s_flast = h;
        const ULONGLONG t = GetTickCount64();
        if (s_ft0 == 0) s_ft0 = t;
        if (t - s_ft0 >= 1000) {
            fprintf(stderr, "[fprint] t=%llus frames=%u distinct=%u%s consts=%u last=%016llX same_run=%u draws=%u\n",
                    (unsigned long long)(t / 1000), s_ff, s_fnh, s_fover ? "+" : "", nc,
                    (unsigned long long)h, s_frun, s_dbg_last_draws);
            s_ft0 = t; s_fnh = 0; s_ff = 0; s_fover = 0;
        }
    }

    /* RPCS3-style titlebar stats, refreshed once a second: presented FPS,
     * draw count of the last frame, and the backbuffer size. */
    {
        static ULONGLONG s_tt0 = 0;
        static u64 s_tframes = 0;
        s_tframes++;
        ULONGLONG tnow = GetTickCount64();
        if (s_tt0 == 0) s_tt0 = tnow;
        if (tnow - s_tt0 >= 1000 && s_d3d.hwnd) {
            extern char g_rsx_title_base[128];
            char tb[256];
            snprintf(tb, sizeof(tb), "%s | FPS: %.2f | draws: %u | %ux%u",
                     g_rsx_title_base,
                     s_tframes * 1000.0 / (double)(tnow - s_tt0),
                     s_dbg_last_draws, s_d3d.width, s_d3d.height);
            SetWindowTextA(s_d3d.hwnd, tb);
            s_tframes = 0;
            s_tt0 = tnow;
        }
    }
}

/* ---------------------------------------------------------------------------
 * RSX backend callbacks
 * -----------------------------------------------------------------------*/

static int d3d12_init(void* ud, u32 width, u32 height)
{
    (void)ud;
    printf("[D3D12] Backend init(%ux%u)\n", width, height);
    return 0;
}

static void d3d12_shutdown(void* ud)
{
    (void)ud;
    printf("[D3D12] Backend shutdown\n");
}

static void d3d12_begin_frame(void* ud)
{
    (void)ud;
}

static void d3d12_end_frame(void* ud)
{
    (void)ud;
}

static u32 s_dbg_clears_since_present = 0;   /* CELLMARK_BLINKDBG */
static u32 s_clear_presents = 0;   /* presents issued at clear (frame boundary) */

/* FRAME_BUDGET=1: how much geometry a guest frame actually asks for, versus
 * what the per-frame vertex buffer can hold. A frame that overflows is silently
 * truncated -- the tail of the scene never renders -- which reads as "the object
 * is missing" rather than "the batch is too small". */

static int blink_dbg(void)
{
    static int v = -1;
    if (v < 0) v = getenv("CELLMARK_BLINKDBG") ? 1 : 0;
    return v;
}

static u32 s_present_id = 0xFFu;   /* [NHL local patch] diag: flipped display buffer */
u32 s_present_id_get(void) { return s_present_id; }
static void d3d12_present(void* ud, u32 buffer_id)
{
    (void)ud;
    s_present_id = buffer_id;

    if (blink_dbg())
        printf("[PRESENT] draws=%u clears_since_last=%u\n",
               s_d3d.draw_count, s_dbg_clears_since_present);
    s_dbg_clears_since_present = 0;

    /* An accumulated batch with draws but NONE targeting a display buffer is
     * offscreen pass work only (demosaic flips once per effect pass): showing
     * it would strobe the bare backbuffer clear. Keep accumulating -- the
     * composite draw that targets the display presents the whole chain, in
     * order, in one command list. Empty batches still present (boot/idle). */
    int flip_has_display = (s_d3d.draw_count == 0);
    for (u32 _i = 0; _i < s_d3d.draw_count && _i < MAX_DRAWS; _i++)
        if (!s_d3d.draws[_i].is_clear && s_d3d.draws[_i].rt_off == 0) {
            flip_has_display = 1;
            break;
        }

    /* [NHL local patch #75] RSX_PRESENTDBG=1: once a second, what the flips
     * found -- how many presented vs. were skipped as "no display draws", and
     * for the last skipped batch which render targets its draws went to. A
     * frozen window with flips still counting means every batch is skipped. */
    { static int on = -1; if (on < 0) on = getenv("RSX_PRESENTDBG") ? 1 : 0;
      if (on) {
          static ULONGLONG t0 = 0; static u32 n_flip = 0, n_skip = 0;
          n_flip++; if (!(s_d3d.initialized && flip_has_display)) n_skip++;
          const ULONGLONG t = GetTickCount64(); if (!t0) t0 = t;
          if (t - t0 >= 1000) {
              u32 offs[6] = {0}, cnt[6] = {0}; int nd = 0, ncl = 0;
              for (u32 _i = 0; _i < s_d3d.draw_count && _i < MAX_DRAWS; _i++) {
                  if (s_d3d.draws[_i].is_clear) { ncl++; continue; }
                  u32 o = s_d3d.draws[_i].rt_off; int f = -1;
                  for (int _j = 0; _j < nd; _j++) if (offs[_j] == o) { f = _j; break; }
                  if (f < 0) { if (nd < 6) { offs[nd] = o; cnt[nd] = 1; nd++; } } else cnt[f]++;
              }
              extern int cellGcmOffsetIsDisplay(u32);
              fprintf(stderr, "[presentdbg] flips=%u skipped=%u draws=%u clears=%d has_display=%d buf=%u targets:",
                      n_flip, n_skip, s_d3d.draw_count, ncl, flip_has_display, buffer_id);
              for (int _j = 0; _j < nd; _j++)
                  fprintf(stderr, " 0x%08X x%u%s", offs[_j], cnt[_j],
                          cellGcmOffsetIsDisplay(offs[_j]) ? "(disp)" : "");
              fprintf(stderr, "\n");
              n_flip = n_skip = 0; t0 = t;
          } } }

    if (s_d3d.initialized && flip_has_display)
        render_frame();

    /* FPS tracking */
    ULONGLONG now = GetTickCount64();
    if (now - s_d3d.last_fps_time >= 1000) {
        s_d3d.fps = (u32)s_d3d.frame_count; /* rough estimate */
        s_d3d.last_fps_time = now;
        s_d3d.frame_count = 0;
    }
}

static int rsx_depth_only_pass(int is_clear);   /* #113, defined with the draw entry points */
static int s_depth_rec;   /* #190 (defined with the draw entry points); #225c reads it earlier */
static int present_on_flip(void)   /* #131 */
{
    static int v = -1;
    if (v < 0) { const char* e = getenv("NHL_PRESENT_ON_FLIP"); v = (e && e[0] == '0') ? 0 : 1; }
    return v;
}
static void d3d12_clear(void* ud, u32 flags, u32 color, float depth, u8 stencil)
{
    (void)ud;
    (void)depth;
    (void)stencil;

    /* Convert RSX ARGB u32 to float[4] RGBA */
    float cc[4];
    cc[0] = ((color >> 16) & 0xFF) / 255.0f; /* R */
    cc[1] = ((color >> 8) & 0xFF) / 255.0f;  /* G */
    cc[2] = (color & 0xFF) / 255.0f;          /* B */
    cc[3] = ((color >> 24) & 0xFF) / 255.0f;  /* A */
    /* CLEAR_RGB=r,g,b: override the guest clear colour. Black holes (geometry
     * that never rasterized) and black pixels a shader really wrote look
     * identical against a black clear; this separates them in one run. */
    { static int _cinit = 0; static float _co[3]; static int _con = 0;
      if (!_cinit) { _cinit = 1; const char* e = getenv("CLEAR_RGB");
          if (e && sscanf(e, "%f,%f,%f", &_co[0], &_co[1], &_co[2]) == 3) _con = 1; }
      if (_con) { cc[0] = _co[0]; cc[1] = _co[1]; cc[2] = _co[2]; } }

    if (rsx_depth_only_pass(1)) {   /* #113: a depth-only clear is not a display clear */
        /* [#190] ...but it is the shadow map's clear: record it (depth bit = flags & 1) */
        const rsx_state* zst = s_d3d.current_rsx_state;
        if (shadows_on() && (flags & 1u) && zst) {
            { extern void d3d12_flush_if_full_fwd(void); d3d12_flush_if_full_fwd(); }
            if (s_d3d.draw_count < MAX_DRAWS) {
                D3D12DrawRecord* dr = &s_d3d.draws[s_d3d.draw_count++];
                memset(dr, 0, sizeof(*dr));
                dr->is_vp = 1; dr->is_clear = 1; dr->is_depth = 2; dr->tex_slot = -1;
                dr->zeta_off = zst->surface_zeta_offset; dr->z_w = zst->surface_clip_w; dr->z_h = zst->surface_clip_h;
                dr->cc[0] = (depth >= 0.0f && depth <= 1.0f) ? depth : 1.0f;
                shadowz_get(dr->zeta_off, dr->z_w, dr->z_h);
            }
        }
        return;
    }
    /* [NHL local patch #129] CLEAR_SURFACE with no colour bits (mask & 0xF0 == 0:
     * depth and/or stencil only) is not a colour clear. NHL's Apt UI clears the
     * display's STENCIL (mask 0x2, colour register 0xFFFFFFFF) right before it
     * draws the masked 3D-scene window; taken as a display colour clear it set
     * the clear colour to white AND ended the frame there, so the presented
     * batch held only the overlay drawn before it on a white background -- the
     * in-match white flashes (play177: 145 of 159 white captures had no scene
     * composite; play178 [DCLR]: 1071 such clears). The depth/stencil buffer
     * is reset per batch anyway. CLEAR_COLORLESS_OLD=1 restores the old path. */
    /* #129b: but a screen that NEVER colour-clears the display (the Starting Lineup: only
     * stencil clears, ~60/s) then had no frame boundary at all -- batches presented only when
     * the record array filled (play182: ~2.7 fps, flashing half frames). So a colourless
     * DISPLAY clear still ends the frame when no colour display clear has come in the last
     * few flips; it never changes the clear colour. */
    u32 rt_w = 0, rt_h = 0, mrt[3] = {0, 0, 0};
    u32 rt = current_rt_off(&rt_w, &rt_h, mrt);
    /* [NHL local patch #131] Present on the FLIP, not at display clears (default;
     * NHL_PRESENT_ON_FLIP=0 = the clear-boundary logic below). The FIFO walker stops at
     * the title's flip word and the ticker presents the batch it holds -- exactly one
     * game frame (rsx_d3d12_backend_present). Clear-boundary presenting needs a colour
     * clear at the start of each frame; NHL's match has none (two stencil-only clears per
     * frame, one mid-frame), so every rule built on clears cut frames somewhere: white
     * flashes (play177), the lineup at 2.7 fps (#129, play182), black flashes (#129b,
     * play183b). Here a DISPLAY colour clear only sets the clear colour; a colourless one
     * does nothing. */
    if (present_on_flip() && rt == 0) {
        if (flags & 0xF0u) { memcpy(s_d3d.clear_color, cc, sizeof(cc)); s_disp_colour_cleared = 1; }   /* #132 */
        return;
    }
    int colourless = 0;
    { static int old = -1; if (old < 0) old = getenv("CLEAR_COLORLESS_OLD") ? 1 : 0;
      extern unsigned cellGcm_flip_request_count(void);
      static unsigned s_last_colour_flip = 0; static int s_seen_colour = 0;
      const unsigned fc_now = cellGcm_flip_request_count();
      if (!old && (flags & 0xF0u) && rt == 0) { s_last_colour_flip = fc_now; s_seen_colour = 1; }
      if (!old && !(flags & 0xF0u)) {
          static u32 n = 0, nb = 0; static ULONGLONG t0 = 0;
          const int recent_colour = s_seen_colour && fc_now - s_last_colour_flip <= 4u;
          const ULONGLONG t = GetTickCount64(); if (!t0) t0 = t;
          if (t - t0 >= 10000) { fprintf(stderr, "[CLEAR] colourless clears / 10 s: %u ignored, %u used as display frame boundaries%c", n, nb, 10); n = nb = 0; t0 = t; }
          if (rt != 0 || recent_colour) { n++; return; }
          nb++; colourless = 1;   /* boundary only (below), no clear colour */
      } }

    /* [NHL local patch #75] RSX_PRESENTDBG=1: per second, which way each clear
     * went -- NHL presents only here (d3d12_present is never reached). */
    static u32 s_cd_off = 0, s_cd_nodisp = 0, s_cd_noflip = 0, s_cd_small = 0, s_cd_pres = 0;
    static int s_cd_on = -1;
    if (s_cd_on < 0) s_cd_on = getenv("RSX_PRESENTDBG") ? 1 : 0;
    if (s_cd_on) {
        static ULONGLONG t0 = 0; const ULONGLONG t = GetTickCount64(); if (!t0) t0 = t;
        if (t - t0 >= 1000) {
            extern unsigned cellGcm_flip_request_count(void);
            fprintf(stderr, "[cleardbg] offscreen=%u nodisp=%u noflip=%u small=%u presented=%u | draws=%u fc=%u rt=0x%08X\n",
                    s_cd_off, s_cd_nodisp, s_cd_noflip, s_cd_small, s_cd_pres, s_d3d.draw_count,
                    cellGcm_flip_request_count(), rt);
            s_cd_off = s_cd_nodisp = s_cd_noflip = s_cd_small = s_cd_pres = 0; t0 = t;
        }
    }

    /* An OFFSCREEN clear is just an ordered op in the current frame's pass
     * chain (demosaic clears each effect pass's surface) -- record it, don't
     * touch the frame boundary. */
    if (rt != 0) {
        { extern void d3d12_flush_if_full_fwd(void); d3d12_flush_if_full_fwd(); }   /* #77 */
        if (s_d3d.draw_count < MAX_DRAWS) {
            D3D12DrawRecord* dr = &s_d3d.draws[s_d3d.draw_count++];
            memset(dr, 0, sizeof(*dr));
            dr->is_vp = 1; dr->is_clear = 1; dr->tex_slot = -1;
            dr->rt_off = rt; memcpy(dr->rt_mrt, mrt, sizeof(mrt));
            dr->rt_w = rt_w; dr->rt_h = rt_h;
            dr->rt_fmt = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->surface_format : 0;
            memcpy(dr->cc, cc, sizeof(cc));
            if (s_d3d.current_rsx_state) {   /* #223 */
                const rsx_state* st = s_d3d.current_rsx_state;
                dr->clr_x = st->scissor_x; dr->clr_y = st->scissor_y;
                dr->clr_w = st->scissor_w; dr->clr_h = st->scissor_h;
            }
        }
        s_cd_off++;
        return;
    }

    /* [#129 diag] DCLR_LOG=<n>: every DISPLAY clear (up to n): colour, flags,
     * scissor vs surface clip, display surface, batch size and how many
     * display draws it already holds -- is the white flash a partial
     * (scissored) clear treated as a full-screen frame boundary? */
    { static int dl = -1, dn = 0;
      if (dl < 0) { const char* e = getenv("DCLR_LOG"); dl = e ? atoi(e) : 0; }
      if (dl && dn < dl) { dn++;
          const rsx_state* st = s_d3d.current_rsx_state;
          u32 nd = 0;
          for (u32 i = 0; i < s_d3d.draw_count && i < MAX_DRAWS; i++)
              if (!s_d3d.draws[i].is_clear && s_d3d.draws[i].rt_off == 0) nd++;
          extern unsigned cellGcm_flip_request_count(void);
          fprintf(stderr, "[DCLR] color=0x%08X flags=0x%X scissor=%u,%u %ux%u clip=%u,%u %ux%u surf=0x%X ct=%u batch=%u disp=%u fc=%u%c",
                  color, flags, st ? st->scissor_x : 0, st ? st->scissor_y : 0, st ? st->scissor_w : 0, st ? st->scissor_h : 0,
                  st ? st->surface_clip_x : 0, st ? st->surface_clip_y : 0, st ? st->surface_clip_w : 0, st ? st->surface_clip_h : 0,
                  st ? st->surface_color_offset[0] : 0, st ? st->color_target : 0, s_d3d.draw_count, nd,
                  cellGcm_flip_request_count(), 10); } }
    if (!colourless) memcpy(s_d3d.clear_color, cc, sizeof(cc));   /* #129b */

    /* A DISPLAY clear marks the start of a new visible frame. If a completed
     * frame is still accumulated (the drain gulped across a frame boundary --
     * guaranteed at the FIFO ring wrap, where rest-of-frame-N + clear-N+1
     * arrive in one batch), PRESENT it now instead of discarding it. Only a
     * batch that actually contains DISPLAY draws is a completed frame: a
     * batch of offscreen pass work (render-to-texture) must keep accumulating
     * until its composite draw arrives, or the screen strobes intermediates. */
    int have_display_draws = 0;
    for (u32 i = 0; i < s_d3d.draw_count && i < MAX_DRAWS; i++)
        if (!s_d3d.draws[i].is_clear && s_d3d.draws[i].rt_off == 0) {
            have_display_draws = 1;
            break;
        }
    if (!have_display_draws) {
        s_cd_nodisp++;
        return;   /* keep accumulating the in-progress frame */
    }

    /* A title that DOUBLE-BUFFERS the display (DeferredShading clears both
     * 0x0 and 0x440000 per frame, plus a HUD pass) issues several display
     * clears per real frame -- treating each as a boundary presented the
     * 1-2-draw intermediates, strobing black between the full 146-draw
     * frames. Anchor the boundary to the FLIP instead: only present once a
     * cellGcmSetFlip has landed since the last present. Titles that flip once
     * per frame (wave/cellmark/gcmcube) are unaffected -- their single
     * display clear still follows their single flip. Fall back to the old
     * clear-only heuristic for titles that never flip (fc stays 0). */
    static u32 s_frame_draws_max = 0;   /* running max frame size (typical full frame) */
    if (s_d3d.draw_count > s_frame_draws_max) s_frame_draws_max = s_d3d.draw_count;
    {
        extern unsigned cellGcm_flip_request_count(void);
        static unsigned s_last_present_flip = 0;
        unsigned fc = cellGcm_flip_request_count();
        if (fc != 0) {
            if (fc == s_last_present_flip) {
                s_cd_noflip++;
                return;   /* no flip since last present -> not a real boundary */
            }
            /* A double-buffered title issues several display clears per flip
             * (DeferredShading: clear back-buffer, HUD pass, ...). They can
             * arrive in either order, so a small batch here may be an
             * intermediate that precedes the full frame's clear. Don't present
             * (or consume the flip) until the batch is a substantial fraction
             * of a full frame -- keep accumulating so the complete frame lands
             * in one present. Gauged against the running MAX frame size (not
             * the last, which a leaked tiny batch would poison), so it
             * self-scales per title with no fixed threshold. */
            /* [NHL local patch #76] RSX_PRESENT_GUARD=1: the running max never
             * shrinks, so once a title's frames get permanently smaller than a
             * quarter of its biggest batch (450-draw transition -> lighter
             * gameplay frames) nothing would present again. If nothing has been
             * presented for 500 ms while flips advance, let this batch through. */
            static int s_guard = -1;
            if (s_guard < 0) s_guard = getenv("RSX_PRESENT_GUARD") ? 1 : 0;
            static ULONGLONG s_last_pres_ms = 0;
            const ULONGLONG now_ms = GetTickCount64();
            const int stalled = s_guard && s_last_pres_ms && now_ms - s_last_pres_ms > 500;
            if (stalled) {
                static int n = 0;
                if (n++ < 8) fprintf(stderr, "[present-guard] %llu ms without a present: presenting %u draws (max %u)\n",
                                     (unsigned long long)(now_ms - s_last_pres_ms), s_d3d.draw_count, s_frame_draws_max);
                s_frame_draws_max = s_d3d.draw_count;   /* re-learn the frame size */
            }
            if (!stalled && s_frame_draws_max > 16 && s_d3d.draw_count < s_frame_draws_max / 4) {
                s_cd_small++;
                if (s_cd_on) { static u32 last_max = 0; if (s_frame_draws_max != last_max) {
                    last_max = s_frame_draws_max;
                    fprintf(stderr, "[cleardbg] frame_draws_max=%u\n", s_frame_draws_max); } }
                return;   /* intermediate: keep accumulating, flip stays pending */
            }
            s_last_pres_ms = now_ms;   /* this batch presents */
            s_last_present_flip = fc;
        }
    }

    /* RSX_ACCUM_FRAME=1: never present at a clear boundary, so every draw in a
     * guest frame accumulates into one image. A title that clears several times
     * per frame (render-to-texture passes) otherwise gets presented mid-scene,
     * and each captured frame holds only a slice of the geometry -- the floor in
     * one, a wall in the next -- which makes a single model impossible to see. */
    { static int accum = -1;
      if (accum < 0) { const char* e = getenv("RSX_ACCUM_FRAME"); accum = e ? atoi(e) : 0; }
      if (accum) return; }
    /* [NHL local patch #112] PRESENT_LOG=<n>: one [PRES] line per presented
     * batch (up to n lines, after the first 600 presents): draws, display
     * draws, the main-scene RT count, the top render targets, the flip count
     * and ms since the previous present -- a blinking screen shows up as
     * presents whose batch is short or missing the scene. */
    { static int pl = -1; static int pn = 0, pskip = 0; static ULONGLONG plast = 0;
      if (pl < 0) { const char* e = getenv("PRESENT_LOG"); pl = e ? atoi(e) : 0; }
      if (pl > 0 && pn < pl && ++pskip > 600) {
          pn++;
          u32 offs[6] = {0}, cnt[6] = {0}; int nd = 0; u32 disp = 0, ncl = 0;
          for (u32 _i = 0; _i < s_d3d.draw_count && _i < MAX_DRAWS; _i++) {
              const D3D12DrawRecord* r = &s_d3d.draws[_i];
              if (r->is_clear) { ncl++; continue; }
              if (r->rt_off == 0) { disp++; continue; }
              int f = -1; for (int _j = 0; _j < nd; _j++) if (offs[_j] == r->rt_off) { f = _j; break; }
              if (f < 0) { if (nd < 6) { offs[nd] = r->rt_off; cnt[nd] = 1; nd++; } } else cnt[f]++;
          }
          extern unsigned cellGcm_flip_request_count(void);
          const ULONGLONG t = GetTickCount64();
          char b[400]; int p = snprintf(b, sizeof b, "[PRES] #%d fc=%u dt=%llums draws=%u clears=%u disp=%u rts:",
                                        pn, cellGcm_flip_request_count(), plast ? (unsigned long long)(t - plast) : 0ull,
                                        s_d3d.draw_count, ncl, disp);
          for (int _j = 0; _j < nd && p < (int)sizeof b - 24; _j++) p += snprintf(b + p, sizeof b - p, " %X:%u", offs[_j], cnt[_j]);
          fprintf(stderr, "%s%c", b, 10);
          plast = t;
      } }
    if (s_d3d.initialized) {
        if (blink_dbg())
            printf("[CLEAR] presenting %u accumulated draws at frame boundary\n",
                   s_d3d.draw_count);
        render_frame();
        s_clear_presents++;
        s_cd_pres++;
    }
    s_dbg_clears_since_present++;
    s_d3d.draw_count   = 0;
    s_d3d.vb_offset    = 0;
    s_d3d.vp_vb_offset = 0; s_d3d.vp_ib_offset = 0; s_d3d.vp_raw_offset = 0;
}

static void d3d12_set_render_target(void* ud, const rsx_state* state)
{
    (void)ud;
    s_d3d.current_rsx_state = state;
    /* Log only the first few; set_render_target is called every frame and
     * floods the log otherwise. */
    static int s_count = 0;
    if (s_count < 5) {
        printf("[D3D12] set_render_target(%ux%u)\n",
               state->surface_clip_w, state->surface_clip_h);
        s_count++;
    }
}

static void d3d12_set_viewport(void* ud, const rsx_state* state)
{
    (void)ud;
    /* TODO: update D3D12 viewport from RSX state */
    (void)state;
}

/* Our host vertex layout for the fallback path: position (xyz) + color (rgba)
 * + texcoord (uv), 36 bytes. (The real VP path feeds raw float4 attrib0.) */
typedef struct { float x, y, z; float r, g, b, a; float u, v; } BasicVertex;


/* Read one RSX vertex (by absolute vertex index) from guest memory into our
 * host layout. Position is attrib 0 (float3+), color is attrib 3 (ubyte4 or
 * float4); missing attribs default to opaque white. RSX stores each 32-bit
 * component big-endian, so every lane is byte-swapped. */
static void read_rsx_vertex(const rsx_state* state, u32 vindex, BasicVertex* out)
{
    extern uint8_t* vm_base;
    extern u32 cellGcmResolveOffset(u32);

    out->x = out->y = out->z = 0.0f;
    out->r = out->g = out->b = out->a = 1.0f;
    out->u = out->v = 0.0f;
    if (!state || !vm_base) return;

    const rsx_vertex_attrib* pos = &state->vertex_attribs[0];
    if (pos->enabled && pos->type == 2 /* float */ && pos->size >= 2) {
        u8* src = vm_base + cellGcmResolveOffset(pos->offset + vindex * pos->stride);
        out->x = rsx_rd_bef(src);
        out->y = rsx_rd_bef(src + 4);
        if (pos->size >= 3) out->z = rsx_rd_bef(src + 8);

        /* dbgfont (and similar 2D overlays) store screen positions biased far
         * outside clip space; their vertex program folds them back to clip
         * space using transform constants we don't execute. When a position is
         * well outside NDC, recover the fractional part as a normalized [0,1]
         * screen coord and map it to NDC (screen Y-down -> clip Y-up). In this
         * layout the attribute is [posX, posY, U, V] (size 4), so the last two
         * components are the atlas texcoords, not depth.
         * TODO: execute the real vertex-program / RSX viewport transform so
         * this is not needed. */
        if (out->x > 2.0f || out->x < -2.0f || out->y > 2.0f || out->y < -2.0f) {
            float sx = out->x - floorf(out->x);
            float sy = out->y - floorf(out->y);
            out->x = sx * 2.0f - 1.0f;
            out->y = 1.0f - sy * 2.0f;
            out->z = 0.0f;
            if (pos->size >= 4) {
                out->u = rsx_rd_bef(src + 8);   /* U */
                out->v = rsx_rd_bef(src + 12);  /* V */
            }
        }
    }

    const rsx_vertex_attrib* col = &state->vertex_attribs[3];
    if (col->enabled && col->size >= 3) {
        u8* src = vm_base + cellGcmResolveOffset(col->offset + vindex * col->stride);
        if (col->type == 4 /* ubyte */) {
            out->r = src[0] / 255.0f;
            out->g = src[1] / 255.0f;
            out->b = src[2] / 255.0f;
            out->a = (col->size >= 4) ? src[3] / 255.0f : 1.0f;
        } else if (col->type == 2 /* float */) {
            u32 fr, fg, fb, fa;
            memcpy(&fr, src,     4); fr = ((fr>>24)&0xFF)|((fr>>8)&0xFF00)|((fr<<8)&0xFF0000)|((fr<<24)&0xFF000000);
            memcpy(&fg, src + 4, 4); fg = ((fg>>24)&0xFF)|((fg>>8)&0xFF00)|((fg<<8)&0xFF0000)|((fg<<24)&0xFF000000);
            memcpy(&fb, src + 8, 4); fb = ((fb>>24)&0xFF)|((fb>>8)&0xFF00)|((fb<<8)&0xFF0000)|((fb<<24)&0xFF000000);
            memcpy(&out->r, &fr, 4); memcpy(&out->g, &fg, 4); memcpy(&out->b, &fb, 4);
            if (col->size >= 4) {
                memcpy(&fa, src + 12, 4); fa = ((fa>>24)&0xFF)|((fa>>8)&0xFF00)|((fa<<8)&0xFF0000)|((fa<<24)&0xFF000000);
                memcpy(&out->a, &fa, 4);
            }
        }
    }
}

/* Upload `count` sequential vertices [first, first+count). Returns the count
 * actually written (clamped to the remaining per-frame buffer). */
static u32 upload_vertices_from_rsx(u32 first, u32 count)
{
    BasicVertex* verts = (BasicVertex*)((u8*)s_d3d.vb_mapped + s_d3d.vb_offset);
    u32 max_verts = (MAX_VERTICES * VERTEX_STRIDE - s_d3d.vb_offset) / sizeof(BasicVertex);
    if (count > max_verts) count = max_verts;
    const rsx_state* state = s_d3d.current_rsx_state;
    for (u32 i = 0; i < count; i++)
        read_rsx_vertex(state, first + i, &verts[i]);
    s_d3d.vb_offset += count * sizeof(BasicVertex);
    return count;
}

/* Upload RSX QUADS (prim 8) as a triangle list: each 4-vertex quad v0..v3
 * (perimeter winding) splits into triangles (v0,v1,v2) and (v0,v2,v3).
 * D3D12 has no quad topology, so this expansion is how quads render at all.
 * Returns the number of triangle-list vertices emitted (6 per quad). */
static u32 upload_quads_from_rsx(u32 first, u32 count)
{
    const rsx_state* state = s_d3d.current_rsx_state;
    u32 quads = count / 4;
    u32 max_verts = (MAX_VERTICES * VERTEX_STRIDE - s_d3d.vb_offset) / sizeof(BasicVertex);
    if (quads * 6 > max_verts) quads = max_verts / 6;
    BasicVertex* verts = (BasicVertex*)((u8*)s_d3d.vb_mapped + s_d3d.vb_offset);
    u32 o = 0;
    for (u32 q = 0; q < quads; q++) {
        BasicVertex c[4];
        for (u32 k = 0; k < 4; k++)
            read_rsx_vertex(state, first + q * 4 + k, &c[k]);
        verts[o++] = c[0]; verts[o++] = c[1]; verts[o++] = c[2];
        verts[o++] = c[0]; verts[o++] = c[2]; verts[o++] = c[3];
    }
    s_d3d.vb_offset += o * sizeof(BasicVertex);
    return o;
}

/* Upload RSX QUADS as raw float4 attrib0 (byte-swapped) into vp_vb, expanded
 * to a triangle list (6 verts/quad). The decompiled vertex shader does the
 * transform. Returns emitted vertex count. */
/* Generic VP vertex: all 16 RSX vertex attributes, each converted to a float4
 * slot -- 256 bytes/vertex, input layout ATTRi @ i*16. Apps place attributes at
 * arbitrary indices (tiny3d: pos=a0 colour=a3 tex=a8; SDK gcm samples: pos=a0
 * colour=a1 tex=a2; dbgfont: a0..a2), so a hardcoded pos+colour pair can't
 * cover them: every enabled attrib is fetched from guest memory and converted
 * by its RSX type; disabled slots read (0,0,0,1). */
typedef struct { float v[4]; } VPSlot;
#define VP_VERT_STRIDE (16 * sizeof(VPSlot))   /* 256 */


void rsx_vtx_pos_dbg(const rsx_state* state, const float* v, u32 n);

/* Fill all 16 attribute slots for one vertex. The per-attribute work lives in
 * rsx_vertex_fetch.c so the Metal and null backends read guest vertices the
 * same way this one does, rather than each porting the logic again. */
static void read_vp_vertex(const rsx_state* state, u32 vi, VPSlot* out16)
{
    s_vcnt[s_vpath]++;
    for (int i = 0; i < 16; i++) {
        rsx_fetch_attrib(state, i, vi, out16[i].v);
        if (i == 0) {
            const rsx_vertex_attrib* a = &state->vertex_attribs[0];
            u32 n = a->size ? a->size : 4; if (n > 4) n = 4;
            rsx_vtx_pos_dbg(state, out16[0].v, n);
        }
    }
    /* [#102] VTXALL_VS=<hex vs hash> + VTXALL_AFTER=<s uptime> (+ VTXALL_SECS,
     * default 3; VTXALL_MAX lines, default 20000): EVERY fetched vertex of that
     * VS in the window -- [VTXALL] vi + a0, and once per (fp, a0) the world /
     * view / proj rows, so a draw's screen extent (runaway vertices) can be
     * computed offline. */
    { static u32 want_vs = 0xFFFFFFFFu; static double t_after = 0, t_secs = 3; static int maxl = 20000, nl = 0;
      static ULONGLONG t0 = 0; static u32 last_fp = 0, last_a0 = 0, last_hit = 0;
      if (want_vs == 0xFFFFFFFFu) { const char* e = getenv("VTXALL_VS"); want_vs = e ? (u32)strtoul(e, NULL, 16) : 0u;
          e = getenv("VTXALL_AFTER"); if (e) t_after = atof(e); e = getenv("VTXALL_SECS"); if (e) t_secs = atof(e);
          e = getenv("VTXALL_MAX"); if (e) maxl = atoi(e); t0 = GetTickCount64(); }
      if (want_vs && nl < maxl) {
          const double up = (GetTickCount64() - t0) / 1000.0;
          static double t_every = 0; static int ev_init = 0;
          if (!ev_init) { ev_init = 1; const char* e = getenv("VTXALL_EVERY"); t_every = e ? atof(e) : 0; }
          const double rel = up - t_after;
          /* VTXALL_GATE=<file>: instead of the time window, dump while <file> exists. */
          static const char* gate = (const char*)1; static int gate_open = 0; static ULONGLONG gate_t = 0;
          if (gate == (const char*)1) gate = getenv("VTXALL_GATE");
          if (gate) { const ULONGLONG gt = GetTickCount64();
              if (gt - gate_t > 200) { gate_t = gt; gate_open = GetFileAttributesA(gate) != INVALID_FILE_ATTRIBUTES; } }
          if (gate ? gate_open : (rel >= 0 && (t_every > 0 ? fmod(rel, t_every) < t_secs : rel < t_secs))) {
              if (state->shader_program != last_fp || state->vertex_attribs[0].offset != last_a0) {
                  last_fp = state->shader_program; last_a0 = state->vertex_attribs[0].offset;
                  u32 vs0 = state->transform_program_start * 16u; if (vs0 >= state->vp_ucode_bytes) vs0 = 0;
                  extern u32 rsx_vp_program_size_instrs(const u8*, u32);
                  u32 vl = state->vp_ucode_bytes - vs0, ni = rsx_vp_program_size_instrs(state->vp_ucode + vs0, vl);
                  if (ni) vl = ni * 16u;
                  const u32 hv = vp_hash_ucode(state->vp_ucode + vs0, vl);
                  last_hit = (want_vs == 1u) || (hv == want_vs);   /* VTXALL_VS=1: every VS */
                  if (last_hit && want_vs == 1u) fprintf(stderr, "[VTXALL] vs=0x%08X%c", hv, 10);
                  if (last_hit) { const float (*k)[4] = state->vertex_constants; nl++;
                      fprintf(stderr, "[VTXALL] draw fp=0x%08X a0off=0x%08X t%u n%u st%u", state->shader_program,
                              state->vertex_attribs[0].offset, state->vertex_attribs[0].type, state->vertex_attribs[0].size,
                              state->vertex_attribs[0].stride);
                      for (int r = 0; r < 8; r++) fprintf(stderr, " c%d=(%g,%g,%g,%g)", r, k[r][0], k[r][1], k[r][2], k[r][3]);
                      for (int r = 36; r < 40; r++) fprintf(stderr, " c%d=(%g,%g,%g,%g)", r, k[r][0], k[r][1], k[r][2], k[r][3]);
                      fputc(10, stderr); } }
              /* VTXALL_THRESH=<f>: only vertices with a non-finite or |component| > f position */
              static double thr = -1; if (thr < 0) { const char* e = getenv("VTXALL_THRESH"); thr = e ? atof(e) : 0; }
              /* VTXALL_STRIDE=<n>: with THRESH 0, only every n-th vertex of each draw */
              static int vstride = -1; static u32 vcount = 0, vc_fp = 0, vc_a0 = 0;
              if (vstride < 0) { const char* e = getenv("VTXALL_STRIDE"); vstride = e ? atoi(e) : 1; if (vstride < 1) vstride = 1; }
              if (state->shader_program != vc_fp || state->vertex_attribs[0].offset != vc_a0) {
                  vc_fp = state->shader_program; vc_a0 = state->vertex_attribs[0].offset; vcount = 0; }
              const int take = (vcount++ % (u32)vstride) == 0;
              if (last_hit && (thr > 0 || take)) {
                  const float x = out16[0].v[0], y = out16[0].v[1], z = out16[0].v[2];
                  const int bad = !(x == x && y == y && z == z) || fabsf(x) > thr || fabsf(y) > thr || fabsf(z) > thr;
                  if (thr <= 0 || bad) { nl++;
                      /* a8 (tc0 source) + its raw bytes: is the UV varying per vertex? */
                      const rsx_vertex_attrib* a8 = &state->vertex_attribs[8];
                      u32 raw8 = 0;
                      if (a8->enabled && a8->stride) {
                          extern u32 cellGcmResolveLocated(int, u32);
                          const u32 o8 = (a8->offset & 0x7FFFFFFFu) + vi * a8->stride;
                          const u8* q = vm_base + cellGcmResolveLocated((a8->offset & 0x80000000u) ? 0 : 1, o8);
                          raw8 = ((u32)q[0] << 24) | ((u32)q[1] << 16) | ((u32)q[2] << 8) | q[3];
                      }
                      fprintf(stderr, "[VTXALL] v %u %g %g %g uv %g %g raw8=%08X%c", vi, x, y, z,
                              out16[8].v[0], out16[8].v[1], raw8, 10); } } } } }
    /* [#86] VTXDUMP_RT=<hex surface>: for draws into that colour surface, one
     * vertex per (FP, attr0 array) change, first 40: every attribute's enable,
     * RSX type, size, stride, offset, divider and fetched value. The scene's
     * texcoords came out constant and its vertex colour zero (play93). */
    /* [#89] SCENECAM_RT=<hex surface>: every 2 s, the camera rows c4..c7 and
     * the world row c36 as seen by a draw into that surface -- is the 3D camera
     * moving at all? (c0..c7 at frame end belong to the 2D UI.) */
    { static u32 cam_rt = 0xFFFFFFFFu; static ULONGLONG cam_t = 0;
      if (cam_rt == 0xFFFFFFFFu) { const char* e = getenv("SCENECAM_RT"); cam_rt = e ? (u32)strtoul(e, NULL, 16) : 0u; }
      if (cam_rt && state->surface_color_offset[(state->color_target == 2) ? 1 : 0] == cam_rt) {
          const ULONGLONG t = GetTickCount64();
          if (t - cam_t >= 2000) { cam_t = t;
              const float (*k)[4] = state->vertex_constants;
              fprintf(stderr, "[SCENECAM] t=%.1f c4=(%.4g,%.4g,%.4g,%.4g) c5=(%.4g,%.4g,%.4g,%.4g) c7=(%.4g,%.4g,%.4g,%.4g) c36=(%.4g,%.4g,%.4g,%.4g)\n",
                      t / 1000.0, k[4][0], k[4][1], k[4][2], k[4][3], k[5][0], k[5][1], k[5][2], k[5][3],
                      k[7][0], k[7][1], k[7][2], k[7][3], k[36][0], k[36][1], k[36][2], k[36][3]);
          } } }
    { static u32 want = 0xFFFFFFFFu; static int n = 0; static u32 last_fp = 0, last_a0 = 0;
      /* [#91] VTXDUMP_MAX=<n> (default 40) and VTXDUMP_SKIP=<n> (skip the first n
       * matches, e.g. past the transition into the match). */
      static int vmax = -1, vskip = 0;
      if (want == 0xFFFFFFFFu) { const char* e = getenv("VTXDUMP_RT"); want = e ? (u32)strtoul(e, NULL, 16) : 0u; }
      /* VTXDUMP_EVERY=<sec>: start a fresh batch of vmax draws every <sec> s
       * (snapshots across transition and match; each batch starts [VTXRT] ---). */
      static int vevery = -1; static ULONGLONG vlast = 0;
      if (vmax < 0) { const char* e = getenv("VTXDUMP_MAX"); vmax = e ? atoi(e) : 40;
                      e = getenv("VTXDUMP_SKIP"); vskip = e ? atoi(e) : 0;
                      e = getenv("VTXDUMP_EVERY"); vevery = e ? atoi(e) : 0; }
      if (want && vevery > 0 && state->surface_color_offset[(state->color_target == 2) ? 1 : 0] == want) {
          const ULONGLONG t = GetTickCount64();
          if (t - vlast >= (ULONGLONG)vevery * 1000u) { vlast = t; n = 0; last_fp = 0; last_a0 = 0;
              fprintf(stderr, "[VTXRT] --- batch t=%.1f\n", t / 1000.0); } }
      if (want && n < vmax + vskip && state->surface_color_offset[(state->color_target == 2) ? 1 : 0] == want &&
          (state->shader_program != last_fp || state->vertex_attribs[0].offset != last_a0)) {
          last_fp = state->shader_program; last_a0 = state->vertex_attribs[0].offset; n++;
          if (n > vskip) {
          u32 vsh = 0;   /* same key as vp_get_vs: this program's bytes through D3.end */
          { u32 vs0 = state->transform_program_start * 16u;
            if (vs0 >= state->vp_ucode_bytes) vs0 = 0;
            extern u32 rsx_vp_program_size_instrs(const u8*, u32);
            u32 vl = state->vp_ucode_bytes - vs0, ni = rsx_vp_program_size_instrs(state->vp_ucode + vs0, vl);
            if (ni) vl = ni * 16u;
            vsh = vp_hash_ucode(state->vp_ucode + vs0, vl); }
          char b[3072]; int p = snprintf(b, sizeof b, "[VTXRT] fp=0x%08X vs=0x%08X vi=%u:", state->shader_program, vsh, vi);
          for (int i = 0; i < 16 && p < (int)sizeof b - 160; i++) {
              const rsx_vertex_attrib* a = &state->vertex_attribs[i];
              if (!a->enabled && !a->size) continue;
              p += snprintf(b + p, sizeof b - p, " a%d%s[t%u n%u st%u off=0x%08X]=(%.3f,%.3f,%.3f,%.3f)", i,
                            a->enabled ? "" : "(off)", a->type, a->size, a->stride, a->offset,
                            out16[i].v[0], out16[i].v[1], out16[i].v[2], out16[i].v[3]);
          }
          fprintf(stderr, "%s\n", b);
          { static const int ks[] = { 0, 1, 2, 3, 4, 5, 6, 7, 24, 25, 26, 27, 28, 32, 36, 37, 38, 39, 64, 65, 467 };
            char k[2048]; int q = snprintf(k, sizeof k, "[VTXRT]   consts dirty=%d lo=%u hi=%u load=%u:",
                                           state->vertex_constants_dirty, state->vertex_constants_lo,
                                           state->vertex_constants_hi, state->transform_constant_load);
            for (unsigned j = 0; j < sizeof ks / sizeof ks[0] && q < (int)sizeof k - 80; j++)
                q += snprintf(k + q, sizeof k - q, " c%d=(%.3g,%.3g,%.3g,%.3g)", ks[j],
                              state->vertex_constants[ks[j]][0], state->vertex_constants[ks[j]][1],
                              state->vertex_constants[ks[j]][2], state->vertex_constants[ks[j]][3]);
            fprintf(stderr, "%s\n", k); }
          }
      } }
    /* [NHL local patch] Placeholder marker: an untextured UI fill (u == the
     * -65536 sentinel) whose attr3 bytes are "NHL\0" was tagged by the lifted
     * Apt bitmap-fill writer as an image slot nobody filled. */
    if (out16[8].v[0] == -65536.0f) {
        const rsx_vertex_attrib* a3 = &state->vertex_attribs[3];
        if (a3->enabled && a3->stride) {
            u32 o3 = (a3->offset & 0x7FFFFFFFu) + vi * a3->stride;
            extern u32 cellGcmResolveLocated(int, u32);
            u32 ea = cellGcmResolveLocated((a3->offset & 0x80000000u) ? 0 : 1, o3);
            const u8* p = vm_base + ea;
            if (p[0] == 0x4E && p[1] == 0x48 && p[2] == 0x4C && p[3] == 0x00) s_vp_placeholder = 1;
        }
    }
    /* [NHL local patch] VTXDUMP_FILL=1: vertices of NHL's untextured UI fills
     * (texcoord u == -65536, the c[466].x sentinel) -- their colour comes from
     * attr3 + attr4 alone. Raw bytes of attr3 (UB4) too, for byte order. */
    { static int vf = -1; if (vf < 0) vf = getenv("VTXDUMP_FILL") ? 1 : 0;
      if (vf && out16[8].v[0] == -65536.0f) {
          static int n = 0;
          if (n++ < 60) {
              const rsx_vertex_attrib* a3 = &state->vertex_attribs[3];
              u8 rb[4] = {0, 0, 0, 0};
              if (a3->enabled && a3->stride) {
                  u32 o3 = (a3->offset & 0x7FFFFFFFu) + vi * a3->stride;
                  extern u32 cellGcmResolveLocated(int, u32);
                  u32 ea = cellGcmResolveLocated((a3->offset & 0x80000000u) ? 0 : 1, o3);
                  memcpy(rb, vm_base + ea, 4);
              }
              fprintf(stderr, "[VTXFILL] vi=%u tex0=0x%08X a3=(%.3f,%.3f,%.3f,%.3f) raw=%02X%02X%02X%02X a4=(%.3f,%.3f,%.3f,%.3f) a8=(%.1f,%.3f)"
                              " | ztest=%d zfunc=0x%X zmask=%d stest=%d sfunc=0x%X sref=%u smask=0x%X sops=%X/%X/%X cmask=0x%08X"
                              " | tgt=0x%X sfmt=0x%X off0=0x%X off1=0x%X clip=%ux%u\n",
                      vi, s_d3d.cur_texs[0].off, out16[3].v[0], out16[3].v[1], out16[3].v[2], out16[3].v[3],
                      rb[0], rb[1], rb[2], rb[3],
                      out16[4].v[0], out16[4].v[1], out16[4].v[2], out16[4].v[3], out16[8].v[0], out16[8].v[1],
                      state->depth_test_enable, state->depth_func, state->depth_mask,
                      state->stencil_test_enable, state->stencil_func, state->stencil_ref, state->stencil_mask,
                      state->stencil_op_fail, state->stencil_op_zfail, state->stencil_op_zpass, state->color_mask,
                      state->color_target, state->surface_format, state->surface_color_offset[0],
                      state->surface_color_offset[1], state->surface_clip_w, state->surface_clip_h);
          } } }
    /* [NHL local patch] VTXDUMP=1: the first vertex of the first draw bound to
     * each distinct LINEAR A8R8G8B8 texture (NHL's runtime UI art -- the flat
     * white/blue/green panels), plus three DXT-textured draws to compare. All
     * 16 slots with each array's type/size/stride. */
    { static int vd = -1; if (vd < 0) vd = getenv("VTXDUMP") ? 1 : 0;
      if (vd) {
        const u32 f0 = s_d3d.cur_texs[0].fmt, b0 = f0 & 0x9F;
        static u32 seen[48]; static int ns = 0, ndxt = 0;
        int want = 0;
        if (b0 == 0x85 && (f0 & 0x20)) {
            int f = 0; for (int k = 0; k < ns; k++) if (seen[k] == s_d3d.cur_texs[0].off) f = 1;
            if (!f && ns < 48) { seen[ns++] = s_d3d.cur_texs[0].off; want = 1; }
        } else if (b0 == 0x88 && ndxt < 3) {
            int f = 0; for (int k = 0; k < ns; k++) if (seen[k] == s_d3d.cur_texs[0].off) f = 1;
            if (!f && ns < 48) { seen[ns++] = s_d3d.cur_texs[0].off; ndxt++; want = 1; }
        }
        if (want) {
            char b[2048]; int p = snprintf(b, sizeof b, "[VTXDUMP] tex0 off=0x%08X fmt=0x%02X %ux%u fp=0x%08X vi=%u"
                " c466=(%.3f,%.3f,%.3f,%.3f) c467=(%.3f,%.3f,%.3f,%.3f) dirty=%d lo=%u hi=%u:",
                s_d3d.cur_texs[0].off, f0, s_d3d.cur_texs[0].w, s_d3d.cur_texs[0].h,
                s_d3d.current_rsx_state ? s_d3d.current_rsx_state->shader_program : 0u, vi,
                state->vertex_constants[466][0], state->vertex_constants[466][1],
                state->vertex_constants[466][2], state->vertex_constants[466][3],
                state->vertex_constants[467][0], state->vertex_constants[467][1],
                state->vertex_constants[467][2], state->vertex_constants[467][3],
                state->vertex_constants_dirty, state->vertex_constants_lo, state->vertex_constants_hi);
            for (int i = 0; i < 16 && p < (int)sizeof b - 120; i++) {
                const rsx_vertex_attrib* a = &state->vertex_attribs[i];
                if (!a->enabled && out16[i].v[0] == 0 && out16[i].v[1] == 0 && out16[i].v[2] == 0 && out16[i].v[3] == 0) continue;
                p += snprintf(b + p, sizeof b - p, " a%d%s[t%u s%u st%u]=(%.3f,%.3f,%.3f,%.3f)", i,
                              a->enabled ? "" : "(off)", a->type, a->size, a->stride,
                              out16[i].v[0], out16[i].v[1], out16[i].v[2], out16[i].v[3]);
            }
            fprintf(stderr, "%s\n", b);
        } } }
}

/* VTX_POS=<N>: print the first fetched position of the first N draws. A guest
 * vertex array that resolves to the wrong memory reads as garbage/denormals, and
 * that is indistinguishable from "the shader is wrong" without seeing the input. */
void rsx_vtx_pos_dbg(const rsx_state* state, const float* v, u32 n)
{
    static int cap = -1, seen = 0;
    if (cap < 0) { const char* e = getenv("VTX_POS"); cap = e ? atoi(e) : 0; }
    if (!cap || seen >= cap) return;
    seen++;
    const rsx_vertex_attrib* a = &state->vertex_attribs[0];
    fprintf(stderr, "[VTXPOS] a0 off=0x%X stride=%u size=%u type=%u -> (%g, %g, %g, %g)\n",
            a->offset, a->stride, a->size, a->type,
            v[0], v[1], v[2], v[3]);   /* all four: masking by `size` hid whether w survived */
}

static void vp_attrs_dbg(const rsx_state* state)
{
    if (!getenv("VP_ATTRS")) return;
    /* VP_ATTRS_FP=<hex shader_program>: dump only the draws that use one
     * fragment program. Without it the first six draws are whatever the frame
     * happens to start with, which is never the mesh being investigated. */
    { static const char* e = (const char*)1; static u32 want = 0;
      if (e == (const char*)1) { e = getenv("VP_ATTRS_FP");
          want = e ? (u32)strtoul(e, NULL, 0) : 0; }
      if (want && (!state || state->shader_program != want)) return; }
    /* Dedupe by the set of enabled attributes rather than printing the first
     * few draws: one fragment program can be used by meshes with different
     * vertex layouts, and a mesh missing an attribute takes the constant
     * register instead -- which is exactly the case worth seeing. */
    { u32 mask = 0;
      for (int i = 0; i < 16; i++) if (state->vertex_attribs[i].enabled) mask |= 1u << i;
      static u32 seen[16]; static int ns = 0;
      for (int i = 0; i < ns; i++) if (seen[i] == mask) return;
      if (ns >= 16) return;
      seen[ns++] = mask; }
    fprintf(stderr, "[VPATTR] --- fp=0x%X ---%c", state->shader_program, 10);
    fprintf(stderr, "[VPATTR] divider_op=0x%08X\n", state->frequency_divider_op);
    for (int i = 0; i < 16; i++) {
        const rsx_vertex_attrib* a = &state->vertex_attribs[i];
        if (a->enabled) fprintf(stderr, "[VPATTR] a%d off=0x%X stride=%u size=%u type=%u freq=%u fmt=0x%08X\n",
                                i, a->offset, a->stride, a->size, a->type, a->frequency, a->format);
    }
    /* VP_ATTRS_DUMP=<attr>:<n>: read n entries of that attribute straight out of
     * guest memory. Answers "did the guest write these values, or did our fetch
     * mangle them" without inferring it from the rendered image. */
    { const char* e = getenv("VP_ATTRS_DUMP"); if (!e) return;
      int ai = 0, cnt = 8; sscanf(e, "%d:%d", &ai, &cnt);
      const rsx_vertex_attrib* a = &state->vertex_attribs[ai];
      if (!a->enabled || !a->stride) return;
      extern uint8_t* vm_base;
      extern u32 cellGcmResolveLocated(int, u32);
      u32 nbad = 0, nzero = 0;
      for (int v = 0; v < cnt; v++) {
          u32 aoff = (a->offset & 0x7FFFFFFFu) + (u32)v * a->stride;
          const u8* q = vm_base + ((a->offset & 0x80000000u)
                        ? cellGcmResolveLocated(0, aoff) : cellGcmResolveLocated(1, aoff));
          /* Also read the same offset through the OTHER context-DMA. An array
           * the guest put in main memory but whose offset we resolve as LOCAL
           * reads as untouched VRAM, i.e. all zeros -- indistinguishable from
           * "the producer never ran". */
          if (v == 0) {
              u32 la = cellGcmResolveLocated(1, aoff), ma = cellGcmResolveLocated(0, aoff);
              const u8* lp = vm_base + la; const u8* mp = vm_base + ma;
              const u8* rp = vm_base + (a->offset & 0x7FFFFFFFu);   /* raw, unresolved */
              fprintf(stderr, "[VPDUMP] a%d off=0x%X  LOCAL@0x%X=(%g,%g,%g)  MAIN@0x%X=(%g,%g,%g)"
                              "  RAW@0x%X=(%g,%g,%g)%c",
                      ai, aoff, la, rsx_rd_bef(lp), rsx_rd_bef(lp+4), rsx_rd_bef(lp+8),
                      ma, rsx_rd_bef(mp), rsx_rd_bef(mp+4), rsx_rd_bef(mp+8),
                      (a->offset & 0x7FFFFFFFu), rsx_rd_bef(rp), rsx_rd_bef(rp+4), rsx_rd_bef(rp+8), 10);
          }
          u32 nc = a->size ? a->size : 3; if (nc > 3) nc = 3;
          float f[3] = {0,0,0};
          for (u32 k = 0; k < nc; k++) f[k] = rsx_rd_bef(q + k * 4);
          if (v < 12)
              fprintf(stderr, "[VPDUMP] a%d[%d] = (%g, %g, %g)%c", ai, v, f[0], f[1], f[2], 10);
          int allz = 1; for (u32 k = 0; k < nc; k++) if (f[k] != 0.f) allz = 0;
          if (allz) nzero++;
          else if (f[2] < -0.9f && f[0] > -0.1f && f[0] < 0.1f) nbad++;
      }
      fprintf(stderr, "[VPDUMP] a%d over %d verts: %u zero, %u near (0,0,-1)%c",
              ai, cnt, nzero, nbad, 10);
      /* How many entries differ from the first? An all-identical position array
       * is a degenerate mesh: it rasterizes nothing, which looks the same as
       * "the draw never happened". */
      { float f0[3]; u32 o0 = (a->offset & 0x7FFFFFFFu);
        const u8* q0 = vm_base + ((a->offset & 0x80000000u)
                       ? cellGcmResolveLocated(0, o0) : cellGcmResolveLocated(1, o0));
        for (int k = 0; k < 3; k++) f0[k] = rsx_rd_bef(q0 + k * 4);
        u32 diff = 0;
        for (int v = 1; v < cnt; v++) {
            u32 ao = o0 + (u32)v * a->stride;
            const u8* q = vm_base + ((a->offset & 0x80000000u)
                          ? cellGcmResolveLocated(0, ao) : cellGcmResolveLocated(1, ao));
            for (int k = 0; k < 3; k++)
                if (rsx_rd_bef(q + k * 4) != f0[k]) { diff++; break; }
        }
        fprintf(stderr, "[VPDUMP] a%d: %u/%d entries differ from entry 0 (%g,%g,%g)%c",
                ai, diff, cnt, f0[0], f0[1], f0[2], 10);
        /* Extent: an isosurface with little fluid is a speck, and a speck that
         * rasterizes nothing is correct, not a bug. */
        { float lo[3] = {1e30f,1e30f,1e30f}, hi[3] = {-1e30f,-1e30f,-1e30f};
          for (int v = 0; v < cnt; v++) {
              u32 ao = o0 + (u32)v * a->stride;
              const u8* q = vm_base + ((a->offset & 0x80000000u)
                            ? cellGcmResolveLocated(0, ao) : cellGcmResolveLocated(1, ao));
              for (int k = 0; k < 3; k++) { float f = rsx_rd_bef(q + k * 4);
                  if (f < lo[k]) lo[k] = f; if (f > hi[k]) hi[k] = f; } }
          u32 nnan = 0, nfin = 0;
          for (int v = 0; v < cnt; v++) {
              u32 ao = o0 + (u32)v * a->stride;
              const u8* q = vm_base + ((a->offset & 0x80000000u)
                            ? cellGcmResolveLocated(0, ao) : cellGcmResolveLocated(1, ao));
              int bad = 0;
              for (int k = 0; k < 3; k++) { float f = rsx_rd_bef(q + k * 4);
                  if (!(f == f) || f > 1e30f || f < -1e30f) bad = 1; }
              if (bad) nnan++; else nfin++;
          }
          fprintf(stderr, "[VPDUMP] a%d extent x[%g..%g] y[%g..%g] z[%g..%g]"
                          "  finite=%u nan/inf=%u%c",
                  ai, lo[0], hi[0], lo[1], hi[1], lo[2], hi[2], nfin, nnan, 10); } } }
}

static u32 upload_quads_vp(const rsx_state* state, u32 first, u32 count)
{
    s_vpath = VP_QUADS; s_vcalls[VP_QUADS]++;   /* [NHL] PERFVTX */
    extern uint8_t* vm_base;
    if (!state || !vm_base || !s_d3d.vp_vb_mapped) return 0;
    if (!state->vertex_attribs[0].enabled) return 0;
    vp_attrs_dbg(state);
    u32 quads = count / 4;
    u32 maxv = (MAX_VERTICES * VP_VERT_STRIDE - s_d3d.vp_vb_offset) / VP_VERT_STRIDE;
    if (quads * 6 > maxv) quads = maxv / 6;
    VPSlot* out = (VPSlot*)((u8*)s_d3d.vp_vb_mapped
        + (u64)s_d3d.vp_parity * MAX_VERTICES * VP_VERT_STRIDE + s_d3d.vp_vb_offset);
    u32 o = 0;
    for (u32 q = 0; q < quads; q++) {
        VPSlot c[4][16];
        for (u32 k = 0; k < 4; k++)
            read_vp_vertex(state, first + q*4 + k, c[k]);
        /* quad -> two triangles (perimeter winding) */
        static const int idx[6] = {0,1,2, 0,2,3};
        for (int t = 0; t < 6; t++) { memcpy(&out[o*16], c[idx[t]], sizeof(c[0])); o++; }
        if (getenv("VTX_DUMP")) { static int _n=0; if (_n++ < 4) {
            FILE* f = fopen("vtx_dump.txt", _n==1 ? "w" : "a");
            if (f) { for (u32 k = 0; k < 4; k++)
                fprintf(f, "q%02d v%u a0=(%.3f,%.3f,%.3f,%.3f) a8=(%.3f,%.3f)\n", _n, k,
                    c[k][0].v[0],c[k][0].v[1],c[k][0].v[2],c[k][0].v[3],
                    c[k][8].v[0],c[k][8].v[1]);
              fclose(f); } } }
    }
    s_d3d.vp_vb_offset += o * VP_VERT_STRIDE;
    return o;
}

/* Straight triangle-list upload through the VP path (gcm/cube's cube draws
 * TRIANGLES, prim 5 -- no expansion needed). */
static u32 upload_tris_vp(const rsx_state* state, u32 first, u32 count)
{
    s_vpath = VP_TRIS; s_vcalls[VP_TRIS]++;   /* [NHL] PERFVTX */
    extern uint8_t* vm_base;
    s_req_verts += count; s_req_draws++;
    if (!state || !vm_base || !s_d3d.vp_vb_mapped) return 0;
    if (!state->vertex_attribs[0].enabled) return 0;
    vp_attrs_dbg(state);
    u32 maxv = (MAX_VERTICES * VP_VERT_STRIDE - s_d3d.vp_vb_offset) / VP_VERT_STRIDE;
    if (count > maxv) { s_drop_draws++;
        { static int _n = 0; if (getenv("VBFULL") && _n++ < 12)
            fprintf(stderr, "[VBFULL] fp=0x%X wanted %u verts, room for %u%c",
                    state->shader_program, count, maxv, 10); }
        count = maxv - (maxv % 3); }
    VPSlot* out = (VPSlot*)((u8*)s_d3d.vp_vb_mapped
        + (u64)s_d3d.vp_parity * MAX_VERTICES * VP_VERT_STRIDE + s_d3d.vp_vb_offset);
    { double _tv = perf_on() ? perf_now() : 0.0;
      for (u32 k = 0; k < count; k++)
          read_vp_vertex(state, first + k, &out[k*16]);
      if (perf_on()) { s_perf_vtx += perf_now() - _tv; s_perf_nverts += count; } }
    /* IDXDBG=<hex shader_program>: vertex range for that program's draws. */
    { static const char* ie = (const char*)1; static u32 iw = 0;
      if (ie == (const char*)1) { ie = getenv("IDXDBG");
          iw = ie ? (u32)strtoul(ie, NULL, 16) : 0; }
      if (iw && state->shader_program == iw) { static int _n = 0; if (_n++ < 12)
          fprintf(stderr, "[IDXDBG] tris first=%u count=%u a0off=0x%X stride=%u%c",
                  first, count, state->vertex_attribs[0].offset,
                  state->vertex_attribs[0].stride, 10); } }
    /* RSX_DBG_VTX=<hex tex0 offset>: dump attribute-0 positions for the draws that
     * bind that texture. The duck's texture resolves and its 9960 draws all
     * target the backbuffer, yet none of its texels reach the screen -- so the
     * question is whether its vertices are where they should be. */
    { static const char* dv = (const char*)1; static u32 want = 0; static int n = 0;
      if (dv == (const char*)1) { dv = getenv("RSX_DBG_VTX");
                                  want = dv ? (u32)strtoul(dv, NULL, 16) : 0; }
      if (want && s_d3d.cur_texs[0].raw == want && n < 6) { n++;
        fprintf(stderr, "[DUCKVTX] first=%u count=%u", first, count);
        for (u32 k = 0; k < count && k < 4; k++)
            fprintf(stderr, "  v%u=(%.2f,%.2f,%.2f)", k,
                    out[k*16].v[0], out[k*16].v[1], out[k*16].v[2]);
        { int lo = -1, hi = -1, nz = 0;
          for (int r = 0; r < RSX_MAX_VERTEX_CONSTANTS; r++) {
              const float* m = state->vertex_constants[r];
              if (m[0] || m[1] || m[2] || m[3]) { if (lo < 0) lo = r; hi = r; nz++; }
          }
          fprintf(stderr, "  | constants: %d non-zero, slots %d..%d", nz, lo, hi); }
        fprintf(stderr, "%c", 10);
      } }
    if (getenv("VTX_DUMP") && state->surface_color_offset[0] == 0xCC0000) {
        static int _n=0; if (_n++ < 1) {
        FILE* f = fopen("vtx_dump.txt", "w");
        if (f) { fprintf(f, "G-buffer draw surf0=0x%X count=%u first=%u\n",
                         state->surface_color_offset[0], count, first);
          for (u32 k = 0; k < count && k < 30; k++)
            fprintf(f, "v%02u a0=(%.3f,%.3f,%.3f) a2=(%.3f,%.3f,%.3f) a9=(%.4f,%.4f,%.4f,%.4f)\n", k,
                out[k*16].v[0],out[k*16].v[1],out[k*16].v[2],
                out[k*16+2].v[0],out[k*16+2].v[1],out[k*16+2].v[2],
                out[k*16+9].v[0],out[k*16+9].v[1],out[k*16+9].v[2],out[k*16+9].v[3]);
          fclose(f); } } }
    s_d3d.vp_vb_offset += count * VP_VERT_STRIDE;
    return count;
}

/* Non-indexed TRIANGLE_STRIP (fan=0) / TRIANGLE_FAN (fan=1) through the VP
 * path, expanded to a triangle list. LBP's Bink movie draws its YUV quad as a
 * non-indexed 4-vertex strip (prim 6); without this it fell to the fixed-
 * function fallback with is_vp=0 (no vertex-program transform -> the quad
 * collapsed to a line) and textured=0 (the Y/U/V planes never sampled). */
static u32 upload_strip_vp(const rsx_state* state, u32 first, u32 count, int fan)
{
    s_vpath = VP_STRIP; s_vcalls[VP_STRIP]++;   /* [NHL] PERFVTX */
    extern uint8_t* vm_base;
    if (!state || !vm_base || !s_d3d.vp_vb_mapped) return 0;
    if (!state->vertex_attribs[0].enabled) return 0;
    if (count < 3) return 0;
    u32 tris = count - 2;
    u32 maxv = (MAX_VERTICES * VP_VERT_STRIDE - s_d3d.vp_vb_offset) / VP_VERT_STRIDE;
    if (tris * 3 > maxv) tris = maxv / 3;
    VPSlot* out = (VPSlot*)((u8*)s_d3d.vp_vb_mapped
        + (u64)s_d3d.vp_parity * MAX_VERTICES * VP_VERT_STRIDE + s_d3d.vp_vb_offset);
    u32 o = 0;
    for (u32 t = 0; t < tris; t++) {
        u32 i0 = fan ? 0 : t;
        /* strip winding alternates per triangle to keep facing consistent */
        u32 i1 = t + 1, i2 = t + 2;
        if (!fan && (t & 1)) { u32 tmp = i1; i1 = i2; i2 = tmp; }
        read_vp_vertex(state, first + i0, &out[o*16]); o++;
        read_vp_vertex(state, first + i1, &out[o*16]); o++;
        read_vp_vertex(state, first + i2, &out[o*16]); o++;
    }
    s_d3d.vp_vb_offset += o * VP_VERT_STRIDE;
    return o;
}

/* Fetch index k from the guest index array (SET_INDEX_ARRAY_ADDRESS/_DMA:
 * dma [3:0] = location (0 local, 1 main), [7:4] = type (0 u32, 1 u16)).
 * Indices are big-endian in guest memory. */
static u32 read_guest_index(const rsx_state* st, u32 k)
{
    extern uint8_t* vm_base;
    extern u32 cellGcmResolveLocated(int local, u32 offset);
    int local = ((st->index_array_dma & 0xF) == 0);
    u32 base = cellGcmResolveLocated(local, st->index_array_offset);
    const u8* p = vm_base + base;
    if (((st->index_array_dma >> 4) & 0xF) == 1) {
        p += (u64)k * 2;
        return ((u32)p[0] << 8) | p[1];
    }
    p += (u64)k * 4;
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

/* Indexed variants of the VP-path uploads. */
static u32 upload_quads_vp_indexed(const rsx_state* state, u32 first, u32 count)
{
    s_vpath = VP_QUADS_IDX; s_vcalls[VP_QUADS_IDX]++;   /* [NHL] PERFVTX */
    extern uint8_t* vm_base;
    if (!state || !vm_base || !s_d3d.vp_vb_mapped) return 0;
    if (!state->vertex_attribs[0].enabled) return 0;
    u32 quads = count / 4;
    u32 maxv = (MAX_VERTICES * VP_VERT_STRIDE - s_d3d.vp_vb_offset) / VP_VERT_STRIDE;
    if (quads * 6 > maxv) quads = maxv / 6;
    VPSlot* out = (VPSlot*)((u8*)s_d3d.vp_vb_mapped
        + (u64)s_d3d.vp_parity * MAX_VERTICES * VP_VERT_STRIDE + s_d3d.vp_vb_offset);
    u32 o = 0;
    for (u32 q = 0; q < quads; q++) {
        VPSlot c[4][16];
        for (u32 k = 0; k < 4; k++)
            read_vp_vertex(state, read_guest_index(state, first + q*4 + k), c[k]);
        static const int idx[6] = {0,1,2, 0,2,3};
        for (int t = 0; t < 6; t++) { memcpy(&out[o*16], c[idx[t]], sizeof(c[0])); o++; }
        if (getenv("VTX_DUMP")) { static int _n=0; if (_n++ < 6) {
            FILE* f = fopen("vtx_dump.txt", _n==1 ? "w" : "a");
            if (f) {
                const float* vs_ = state->viewport_scale;
                const float* vo_ = state->viewport_offset;
                fprintf(f, "iq%02d vp_scale=(%.1f,%.1f,%.3f) vp_off=(%.1f,%.1f,%.3f)\n", _n,
                        vs_[0], vs_[1], vs_[2], vo_[0], vo_[1], vo_[2]);
                for (int _c = 464; _c <= 467; _c++)
                    fprintf(f, "  c[%d]=(%.3f,%.3f,%.3f,%.3f)\n", _c,
                            state->vertex_constants[_c][0], state->vertex_constants[_c][1],
                            state->vertex_constants[_c][2], state->vertex_constants[_c][3]);
                for (u32 k = 0; k < 4; k++)
                    fprintf(f, "  v%u i%u a0=(%.3f,%.3f,%.3f,%.3f) a8=(%.3f,%.3f)\n", k,
                        read_guest_index(state, first + q*4 + k),
                        c[k][0].v[0],c[k][0].v[1],c[k][0].v[2],c[k][0].v[3],
                        c[k][8].v[0],c[k][8].v[1]);
                fclose(f);
            } } }
    }
    s_d3d.vp_vb_offset += o * VP_VERT_STRIDE;
    return o;
}

/* [NHL local patch #62] Result of the last upload_tris_vp_indexed when it took
 * the GPU-indexed path (consumed by d3d12_draw_indexed). */
static int s_idx_gpu_last = 0;
static u32 s_idx_gpu_first = 0;

static u32 upload_tris_vp_indexed(const rsx_state* state, u32 first, u32 count)
{
    s_vpath = VP_TRIS_IDX_CPU; s_vcalls[VP_TRIS_IDX_CPU]++;   /* [NHL] PERFVTX */
    extern uint8_t* vm_base;
    s_req_verts += count; s_req_draws++;
    s_idx_gpu_last = 0;
    if (!state || !vm_base || !s_d3d.vp_vb_mapped) return 0;
    if (!state->vertex_attribs[0].enabled) return 0;
    vp_attrs_dbg(state);
    /* [NHL local patch #62] GPU-indexed path: convert each referenced vertex
     * ONCE (the index range lo..hi) and let the GPU expand the indices. The
     * CPU expansion below converts a vertex per INDEX -- ~6x the work on a
     * typical mesh; NHL's 3D intro pushed ~2.2M converted vertices per game
     * frame through it (play40: vtx 59% of frame time, ~1 fps). Whole
     * triangles only (partial batches keep the merge path), compact ranges
     * only. NHL_IDX_GPU=0 disables. */
    { static int on = -1; if (on < 0) { const char* e = getenv("NHL_IDX_GPU"); on = e ? atoi(e) : 1; }
      if (on && (count < 3 || (count % 3) != 0)) s_idx_fb_partial++;
      else if (on && s_d3d.vp_ib_offset + count > VP_MAX_INDICES) s_idx_fb_room++;
      if (on && s_d3d.vp_ib_mapped && count >= 3 && (count % 3) == 0 &&
          s_d3d.vp_ib_offset + count <= VP_MAX_INDICES) {
          u32 lo = 0xFFFFFFFFu, hi = 0;
          for (u32 k = 0; k < count; k++) { u32 ix = read_guest_index(state, first + k);
              if (ix < lo) lo = ix; if (ix > hi) hi = ix; }
          u32 nv = hi - lo + 1;
          u32 room = (MAX_VERTICES * VP_VERT_STRIDE - s_d3d.vp_vb_offset) / VP_VERT_STRIDE;
          if (!(lo <= hi && nv <= count * 2u + 64u)) s_idx_fb_sparse++;
          else if (nv > room) s_idx_fb_room++;
          if (lo <= hi && nv <= count * 2u + 64u && nv <= room) {
              s_vpath = VP_TRIS_IDX_GPU; s_vcalls[VP_TRIS_IDX_GPU]++; s_vcalls[VP_TRIS_IDX_CPU]--;
              VPSlot* out = (VPSlot*)((u8*)s_d3d.vp_vb_mapped
                  + (u64)s_d3d.vp_parity * MAX_VERTICES * VP_VERT_STRIDE + s_d3d.vp_vb_offset);
              double _tv = perf_on() ? perf_now() : 0.0;
              for (u32 v = 0; v < nv; v++) read_vp_vertex(state, lo + v, &out[v * 16]);
              u32* ib = (u32*)s_d3d.vp_ib_mapped + (u64)s_d3d.vp_parity * VP_MAX_INDICES + s_d3d.vp_ib_offset;
              for (u32 k = 0; k < count; k++) ib[k] = read_guest_index(state, first + k) - lo;
              if (perf_on()) { s_perf_vtx += perf_now() - _tv; s_perf_nverts += nv; }
              s_idx_gpu_last = 1; s_idx_gpu_first = s_d3d.vp_ib_offset;
              s_d3d.vp_ib_offset += count;
              s_d3d.vp_vb_offset += nv * VP_VERT_STRIDE;
              return count;
          }
      } }
    u32 maxv = (MAX_VERTICES * VP_VERT_STRIDE - s_d3d.vp_vb_offset) / VP_VERT_STRIDE;
    if (count > maxv) { s_drop_draws++;
        { static int _n = 0; if (getenv("VBFULL") && _n++ < 12)
            fprintf(stderr, "[VBFULL] fp=0x%X wanted %u verts, room for %u%c",
                    state->shader_program, count, maxv, 10); }
        count = maxv - (maxv % 3); }
    VPSlot* out = (VPSlot*)((u8*)s_d3d.vp_vb_mapped
        + (u64)s_d3d.vp_parity * MAX_VERTICES * VP_VERT_STRIDE + s_d3d.vp_vb_offset);
    { double _tv = perf_on() ? perf_now() : 0.0;
      for (u32 k = 0; k < count; k++)
          read_vp_vertex(state, read_guest_index(state, first + k), &out[k*16]);
      if (perf_on()) { s_perf_vtx += perf_now() - _tv; s_perf_nverts += count; } }
    /* IDXDBG=<hex shader_program>: index range for that program's draws. An
     * index past the vertex array reads unmapped guest memory as zero, which
     * shows up as a spike triangle with a zero texcoord, not as a missing draw. */
    { static const char* ie = (const char*)1; static u32 iw = 0;
      if (ie == (const char*)1) { ie = getenv("IDXDBG");
          iw = ie ? (u32)strtoul(ie, NULL, 16) : 0; }
      if (iw && state->shader_program == iw) { static int _n = 0; if (_n++ < 12) {
          u32 lo = 0xFFFFFFFFu, hi = 0;
          for (u32 k = 0; k < count; k++) { u32 ix = read_guest_index(state, first + k);
              if (ix < lo) lo = ix; if (ix > hi) hi = ix; }
          fprintf(stderr, "[IDXDBG] first=%u count=%u idx=[%u..%u] dma=0x%X a0off=0x%X stride=%u%c",
                  first, count, lo, hi, state->index_array_dma,
                  state->vertex_attribs[0].offset, state->vertex_attribs[0].stride, 10); } } }
    /* DBG_LOCK tracking: projected centroid of this mesh, updated every draw. */
    { static const char* dl = (const char*)1; static u32 wantl = 0; static int mvpb = 256;
      if (dl == (const char*)1) { dl = getenv("RSX_DBG_VTX");
                                  wantl = dl ? (u32)strtoul(dl, NULL, 16) : 0;
                                  const char* mb = getenv("MVP_BASE");
                                  if (mb) mvpb = atoi(mb); }
      /* RSX_DBG_TRACK=1 tracks the content-identified duck without needing its
       * offset passed in (VRAM offsets move between runs). Re-read every call:
       * s_duck_raw is filled by the texture upload, which happens AFTER the
       * first draws, so caching it once left the tracker watching offset 0. */
      { static int dtrk = -1;
        if (dtrk < 0) { const char* e = getenv("RSX_DBG_TRACK"); dtrk = e ? atoi(e) : 0; }
        if (dtrk && s_duck_raw) wantl = s_duck_raw; }
      /* RSX_DBG_PICK=<first>: track ONLY the draw with that starting index. The
       * mesh is one big vertex buffer sliced into 256-index chunks, so averaging
       * across all of them converges on the centre of the whole field -- which is
       * where there is nothing in particular. A single slice is a stable target
       * and can be magnified without drifting off it. */
      static int pick = -1;
      if (pick < 0) { const char* e = getenv("RSX_DBG_PICK"); pick = e ? atoi(e) : -2; }
      if (wantl && s_d3d.cur_texs[0].raw == wantl && count
          && (pick == -2 || (int)first == pick)) {
          double sx = 0, sy = 0, sz = 0;
          for (u32 k = 0; k < count; k++) {
              sx += out[k*16].v[0]; sy += out[k*16].v[1]; sz += out[k*16].v[2];
          }
          float v[4] = { (float)(sx/count), (float)(sy/count), (float)(sz/count), 1.0f };
          float clip[4];
          for (int r = 0; r < 4; r++) {
              const float* m = state->vertex_constants[mvpb + r];
              clip[r] = m[0]*v[0] + m[1]*v[1] + m[2]*v[2] + m[3]*v[3];
          }
          if (clip[3] > 1e-6f) {
              float nx = clip[0]/clip[3], ny = clip[1]/clip[3];
              if (nx > -2.0f && nx < 2.0f && ny > -2.0f && ny < 2.0f) {
                  s_lock_sx += nx; s_lock_sy += ny; s_lock_n++;
                  if (!s_lock_valid) {        /* seed so frame 1 is usable */
                      s_lock_x = nx; s_lock_y = ny; s_lock_valid = 1;
                  }
              }
          }
      } }
    /* RSX_DBG_SCALE=<f>: multiply attribute-0 positions for the draws binding
     * RSX_DBG_VTX's texture. The duck's object-space bbox is ~0.2 units and its
     * draws rasterize only a few dozen pixels per frame -- sub-pixel. Growing it
     * about its own origin tests the rest of the chain (index buffer, texture,
     * per-draw transform): if a duck-shaped, duck-coloured object appears, only
     * the scale is wrong. Diagnostic. */
    { static const char* ds = (const char*)1; static float sc = 0.0f;
      if (ds == (const char*)1) { ds = getenv("RSX_DBG_SCALE");
                                  sc = ds ? (float)atof(ds) : 0.0f; }
      static const char* dv2 = (const char*)1; static u32 want2 = 0;
      if (dv2 == (const char*)1) { dv2 = getenv("RSX_DBG_VTX");
                                   want2 = dv2 ? (u32)strtoul(dv2, NULL, 16) : 0; }
      if (sc > 0.0f && want2 && s_d3d.cur_texs[0].raw == want2 && count) {
        /* Magnify about a FIXED point captured from the first tracked draw, not
         * about each chunk's own centre -- recentring per chunk would stack every
         * chunk of the mesh on top of the others. Combined with VP_BYPASS (which
         * maps attribute 0 straight to clip space) this renders the mesh's real
         * geometry and texture at a readable size. */
        /* RSX_DBG_RECENTER=1: recentre EVERY chunk on its own centroid, overlaying
         * them. If each chunk is one instance of the same mesh they align into a
         * single silhouette; if they are arbitrary slices of one big mesh they
         * smear. Either way the answer is visible at a glance. */
        static int recen = -1;
        if (recen < 0) { const char* e = getenv("RSX_DBG_RECENTER"); recen = e ? atoi(e) : 0; }
        static int have_c = 0; static float ox = 0, oy = 0, oz = 0;
        if (!have_c || recen) {
            double sx = 0, sy = 0, sz = 0;
            for (u32 k = 0; k < count; k++) {
                sx += out[k*16].v[0]; sy += out[k*16].v[1]; sz += out[k*16].v[2];
            }
            ox = (float)(sx/count); oy = (float)(sy/count); oz = (float)(sz/count);
            have_c = 1;
        }
        for (u32 k = 0; k < count; k++) {
            /* RSX_DBG_SHIFT="dx,dy": nudge after scaling. VP_BYPASS applies the
             * guest's posoffset, so a recentred mesh does not land at screen
             * centre -- without this the magnified geometry sits clipped against
             * the top edge. */
            static int shf = -1; static float dx = 0.0f, dy = 0.0f;
            if (shf < 0) { const char* e = getenv("RSX_DBG_SHIFT");
                           if (e) { double a=0,b=0; sscanf(e, "%lf,%lf", &a, &b);
                                    dx = (float)a; dy = (float)b; }
                           shf = 1; }
            out[k*16].v[0] = (out[k*16].v[0] - ox) * sc + dx;
            out[k*16].v[1] = (out[k*16].v[1] - oy) * sc + dy;
            /* Leave z alone: under VP_BYPASS the object z maps straight to clip
             * z, so scaling it walks the geometry out of the depth range and
             * everything clips away. */
            (void)oz;
        }
      } }
    /* RSX_DBG_VTX=<hex tex0 offset>: attribute-0 positions for the draws binding
     * that texture (see upload_tris_vp). The duck's meshes are indexed, so this
     * is the copy that actually fires for it. */
    { static const char* dv = (const char*)1; static u32 want = 0; static int n = 0;
      if (dv == (const char*)1) { dv = getenv("RSX_DBG_VTX");
                                  want = dv ? (u32)strtoul(dv, NULL, 16) : 0; }
      if (want && s_d3d.cur_texs[0].raw == want && n < 8) { n++;
        float mnx=1e30f,mny=1e30f,mnz=1e30f,mxx=-1e30f,mxy=-1e30f,mxz=-1e30f;
        for (u32 k = 0; k < count; k++) {
            float x=out[k*16].v[0], y=out[k*16].v[1], z=out[k*16].v[2];
            if (x<mnx)mnx=x; if (y<mny)mny=y; if (z<mnz)mnz=z;
            if (x>mxx)mxx=x; if (y>mxy)mxy=y; if (z>mxz)mxz=z;
        }
        const rsx_vertex_attrib* _a3 = &state->vertex_attribs[3];
        fprintf(stderr, "[DUCKVTX] a3(en=%d type=%u size=%u stride=%u off=0x%08X freq=%u)"
                        " -> col0 v0=(%.3f %.3f %.3f %.3f)%c",
                _a3->enabled, _a3->type, _a3->size, _a3->stride, _a3->offset,
                _a3->frequency,
                out[3].v[0], out[3].v[1], out[3].v[2], out[3].v[3], 10);
        const rsx_vertex_attrib* _a0 = &state->vertex_attribs[0];
        fprintf(stderr, "[DUCKVTX] first=%u count=%u a0(type=%u size=%u stride=%u off=0x%08X)"
                        " obj-bbox x[%.3f,%.3f] y[%.3f,%.3f] z[%.3f,%.3f]%c",
                first, count, _a0->type, _a0->size, _a0->stride, _a0->offset,
                mnx,mxx, mny,mxy, mnz,mxz, 10);
        /* Project the object-space bbox corners through the first four transform
         * constants (the conventional MVP rows) and report NDC. Sub-pixel output
         * with correct vertices means the placement transform is the problem, and
         * the NDC extent says by how much. */
        { float cx=(mnx+mxx)*0.5f, cy=(mny+mxy)*0.5f, cz=(mnz+mxz)*0.5f;
          /* This title's Cg programs keep their MVP at c[256..259], not c[0..3]
           * (see the vp_uses_c03 note); projecting through c0 reported zeros. */
          static int MVP = -1;
          if (MVP < 0) { const char* e = getenv("MVP_BASE"); MVP = e ? atoi(e) : 256; }
          float v[4] = { cx, cy, cz, 1.0f };
          float clip[4];
          for (int r = 0; r < 4; r++) {
              const float* m = state->vertex_constants[MVP + r];
              clip[r] = m[0]*v[0] + m[1]*v[1] + m[2]*v[2] + m[3]*v[3];
          }
          float ex[4];
          { float e[4] = { mxx-cx, mxy-cy, mxz-cz, 0.0f };
            for (int r = 0; r < 4; r++) {
                const float* m = state->vertex_constants[MVP + r];
                ex[r] = m[0]*e[0] + m[1]*e[1] + m[2]*e[2];
            } }
          { int lo = -1, hi = -1, nz = 0;
            for (int r = 0; r < RSX_MAX_VERTEX_CONSTANTS; r++) {
                const float* m = state->vertex_constants[r];
                if (m[0] || m[1] || m[2] || m[3]) { if (lo < 0) lo = r; hi = r; nz++; }
            }
            fprintf(stderr, "[DUCKVTX]   constants: %d non-zero, slots %d..%d;", nz, lo, hi);
            for (int r = (lo < 0 ? 0 : lo); r < (lo < 0 ? 0 : lo) + 4 && r < RSX_MAX_VERTEX_CONSTANTS; r++)
                fprintf(stderr, " c%d=(%.3f %.3f %.3f %.3f)", r,
                        state->vertex_constants[r][0], state->vertex_constants[r][1],
                        state->vertex_constants[r][2], state->vertex_constants[r][3]);
            fprintf(stderr, "%c", 10); }
          fprintf(stderr, "[DUCKVTX]   mvp0=(%.3f %.3f %.3f %.3f) clip=(%.3f %.3f %.3f w=%.3f)",
                  state->vertex_constants[MVP][0], state->vertex_constants[MVP][1],
                  state->vertex_constants[MVP][2], state->vertex_constants[MVP][3],
                  clip[0], clip[1], clip[2], clip[3]);
          if (clip[3] > 1e-6f)
              fprintf(stderr, " ndc=(%.4f %.4f) half-extent-px=(%.2f %.2f)",
                      clip[0]/clip[3], clip[1]/clip[3],
                      ex[0]/clip[3]*640.0f, ex[1]/clip[3]*360.0f);
          fprintf(stderr, "%c", 10); }
      } }
    /* IDX_DBG=<N>: the first indices and the position they resolve to. An index
     * buffer read with the wrong element size or base yields huge indices and
     * degenerate triangles -- an INDEXED mesh vanishes while non-indexed
     * geometry in the same scene renders fine. */
    { static int cap = -1, n = 0;
      if (cap < 0) { const char* e = getenv("IDX_DBG"); cap = e ? atoi(e) : 0; }
      if (cap && n < cap && count >= 3) { n++;
        extern u32 cellGcmResolveOffset(u32);
        const rsx_vertex_attrib* a0 = &state->vertex_attribs[0];
        u32 vbase = cellGcmResolveOffset(a0->offset & 0x7FFFFFFFu);
        u32 nz = 0; for (u32 q = 0; q < 0x1000u; q += 29) if (vm_base[vbase + q]) nz++;
        fprintf(stderr, "[IDX] idxoff=0x%X idx: %u %u %u | a0 off=0x%08X(->0x%08X nz=%u/142)"
                        " stride=%u size=%u type=%u  pos0=(%g %g %g)%c",
                state->index_array_offset,
                read_guest_index(state, first), read_guest_index(state, first+1),
                read_guest_index(state, first+2),
                a0->offset, vbase, nz, a0->stride, a0->size, a0->type,
                out[0].v[0], out[0].v[1], out[0].v[2], 10); } }
    s_d3d.vp_vb_offset += count * VP_VERT_STRIDE;
    return count;
}

/* Indexed TRIANGLE_STRIP / TRIANGLE_FAN, expanded CPU-side into a triangle
 * list (the whole VP path draws lists; CullMode is NONE so strip parity
 * winding doesn't matter). DeferredShading's spline tube meshes -- the
 * octopus tentacles -- are indexed strips. */
static u32 upload_strip_vp_indexed(const rsx_state* state, u32 first, u32 count, int fan)
{
    s_vpath = VP_STRIP_IDX; s_vcalls[VP_STRIP_IDX]++;   /* [NHL] PERFVTX */
    extern uint8_t* vm_base;
    if (!state || !vm_base || !s_d3d.vp_vb_mapped) return 0;
    if (!state->vertex_attribs[0].enabled) return 0;
    if (count < 3) return 0;
    u32 tris = count - 2;
    u32 maxv = (MAX_VERTICES * VP_VERT_STRIDE - s_d3d.vp_vb_offset) / VP_VERT_STRIDE;
    if (tris * 3 > maxv) tris = maxv / 3;
    VPSlot* out = (VPSlot*)((u8*)s_d3d.vp_vb_mapped
        + (u64)s_d3d.vp_parity * MAX_VERTICES * VP_VERT_STRIDE + s_d3d.vp_vb_offset);
    u32 o = 0;
    for (u32 t = 0; t < tris; t++) {
        u32 i0 = fan ? 0 : t;
        read_vp_vertex(state, read_guest_index(state, first + i0),    &out[o*16]); o++;
        read_vp_vertex(state, read_guest_index(state, first + t + 1), &out[o*16]); o++;
        read_vp_vertex(state, read_guest_index(state, first + t + 2), &out[o*16]); o++;
    }
    s_d3d.vp_vb_offset += o * VP_VERT_STRIDE;
    return o;
}

/* [#169] copy one offscreen RT back into guest memory (A8R8G8B8: bytes A,R,G,B, guest pitch) */
static void wb_readback(int slot, const WBEnt* e)
{
    extern uint8_t* vm_base;
    extern u32 cellGcmResolveOffset(u32);
    OffRT* r = &s_d3d.off_rt[slot];
    if (r->dxgi != (u32)DXGI_FORMAT_R8G8B8A8_UNORM || !vm_base) return;
    if (r->rw != r->w || r->rh != r->h) {   /* #253: a scaled surface has no 1x image to write back */
        static int n = 0;
        if (n++ < 8) fprintf(stderr, "[RSCALE] write-back of scaled RT 0x%08X %ux%u skipped%c", r->off, r->w, r->h, 10);
        return;
    }
    const u32 dst = cellGcmResolveOffset(e->off);
    if (dst == 0xFFFFFFFFu) return;
    const u32 w = r->w < e->w ? r->w : e->w, h = r->h < e->h ? r->h : e->h;
    const u32 gpitch = e->pitch ? e->pitch : w * 4u;
    const u32 rp = (r->w * 4u + 255u) & ~255u;
    static ID3D12Resource* rb = NULL; static u64 rb_size = 0;
    if ((u64)rp * r->h > rb_size) {
        if (rb) { rb->lpVtbl->Release(rb); rb = NULL; }
        D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC rd = {0};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; rd.Width = (u64)rp * r->h; rd.Height = 1;
        rd.DepthOrArraySize = 1; rd.MipLevels = 1; rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(s_d3d.device->lpVtbl->CreateCommittedResource(s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &rd,
                D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void**)&rb)) || !rb) { rb_size = 0; return; }
        rb_size = rd.Width;
    }
    wait_for_gpu();
    const u32 fi = s_d3d.frame_index;
    s_d3d.cmd_allocators[fi]->lpVtbl->Reset(s_d3d.cmd_allocators[fi]);
    s_d3d.cmd_list->lpVtbl->Reset(s_d3d.cmd_list, s_d3d.cmd_allocators[fi], NULL);
    off_rt_transition(slot, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION cd = {0}, cs = {0};
    cd.pResource = rb; cd.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    cd.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    cd.PlacedFootprint.Footprint.Width = r->w; cd.PlacedFootprint.Footprint.Height = r->h;
    cd.PlacedFootprint.Footprint.Depth = 1; cd.PlacedFootprint.Footprint.RowPitch = rp;
    cs.pResource = r->res; cs.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    s_d3d.cmd_list->lpVtbl->CopyTextureRegion(s_d3d.cmd_list, &cd, 0, 0, 0, &cs, NULL);
    s_d3d.cmd_list->lpVtbl->Close(s_d3d.cmd_list);
    { ID3D12CommandList* cl[] = { (ID3D12CommandList*)s_d3d.cmd_list };
      s_d3d.cmd_queue->lpVtbl->ExecuteCommandLists(s_d3d.cmd_queue, 1, cl); }
    upr_on_submit();   /* #219 */
    txh_on_submit();   /* #220 */
    wait_for_gpu();
    void* mp = NULL; D3D12_RANGE rr = {0, (SIZE_T)rp * r->h};
    if (SUCCEEDED(rb->lpVtbl->Map(rb, 0, &rr, &mp)) && mp) {
        static int bgra = -1; if (bgra < 0) bgra = getenv("NHL_WB_BGRA") ? 1 : 0;   /* A/B the byte order */
        const u32 cw = (gpitch / 4u) < w ? gpitch / 4u : w;
        for (u32 y = 0; y < h; y++) {
            const u8* s = (const u8*)mp + (u64)y * rp;
            u8* d = vm_base + dst + (u64)y * gpitch;
            for (u32 x = 0; x < cw; x++, s += 4, d += 4) {
                if (bgra) { d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = s[3]; }
                else      { d[0] = s[3]; d[1] = s[0]; d[2] = s[1]; d[3] = s[2]; }   /* A,R,G,B */
            }
        }
        /* NHL_WB_DUMP=<n>: the first n copies as BMPs (wbdump\wb_<n>_<rt>_<w>x<h>.bmp) */
        { static int dn = -1, nd = 0; if (dn < 0) { const char* ev = getenv("NHL_WB_DUMP"); dn = ev ? atoi(ev) : 0; }
          if (nd < dn) { char p[96]; CreateDirectoryA("wbdump", NULL);
              snprintf(p, sizeof p, "wbdump/wb_%03d_%08X_%ux%u.bmp", nd++, e->off, r->w, r->h);
              cap_write_bmp(p, mp, r->w, r->h, rp, (u32)DXGI_FORMAT_R8G8B8A8_UNORM);
              /* [#171c] NHL_WB_DUMP_A=1: the alpha channel as a grey image next to it */
              if (getenv("NHL_WB_DUMP_A")) {
                  u8* ab = (u8*)malloc((size_t)rp * r->h);
                  if (ab) {
                      for (u32 y = 0; y < r->h; y++) {
                          const u8* s = (const u8*)mp + (u64)y * rp; u8* d = ab + (u64)y * rp;
                          for (u32 x = 0; x < r->w; x++) { d[4*x] = d[4*x+1] = d[4*x+2] = s[4*x+3]; d[4*x+3] = 255; }
                      }
                      snprintf(p, sizeof p, "wbdump/wb_%03d_%08X_%ux%u_alpha.bmp", nd - 1, e->off, r->w, r->h);
                      cap_write_bmp(p, ab, r->w, r->h, rp, (u32)DXGI_FORMAT_R8G8B8A8_UNORM);
                      free(ab);
                  }
              } } }
        D3D12_RANGE wr = {0, 0}; rb->lpVtbl->Unmap(rb, 0, &wr);
    }
}

/* [#169] before the FIFO publishes a label/semaphore: render the batch so far and copy every
 * candidate surface drawn since the last sync back into guest memory (ticker thread) */
static void wb_sync(void)
{
    if (!wb_on() || !s_wb_any_pending || !s_d3d.initialized) return;
    s_wb_any_pending = 0;
    int ncand = 0;
    for (int i = 0; i < s_wb_n; i++) if (s_wb[i].pending && !wb_reject(&s_wb[i])) ncand++;
    if (ncand && s_d3d.draw_count > 0) {   /* the candidates' draws are still in the batch */
        s_present_this_frame = 0; render_frame(); s_present_this_frame = 1;
    }
    for (int i = 0; i < s_wb_n; i++) {
        WBEnt* e = &s_wb[i];
        if (!e->pending) continue;
        e->pending = 0;
        const char* why = wb_reject(e);
        const int slot = why ? -1 : off_rt_find(e->off);
        if (!why && slot < 0) why = "no-rt-slot";
        if (!why && s_d3d.off_rt[slot].dxgi != (u32)DXGI_FORMAT_R8G8B8A8_UNORM) why = "rt-not-rgba8";
        if (why) {
            if (why[0] == 'g' && why[1] == 'p') e->deferred = 1;   /* #218 "gpu-sampled(deferred to blit)" */
            if (!e->logged_skip) { e->logged_skip = 1;
                fprintf(stderr, "[RTWB-SKIP] rt=0x%08X %ux%u fmt=0x%X pitch=%u flips=%d/64 sampled=%d: %s%c",
                        e->off, e->w, e->h, e->fmt, e->pitch, wb_popcount64(e->hist), e->sampled, why, 10); }
            continue;
        }
        wb_readback(slot, e);
        e->wb_ever = 1; wb_snap(e);   /* #172: guest now holds the surface's own content */
        { static long n = 0; n++;
          if (n <= 400 || (n % 100) == 0)
              fprintf(stderr, "[RTWB] #%ld rt=0x%08X %ux%u fmt=0x%X pitch=%u sampled=%d flips=%d/64 -> guest%c",
                      n, e->off, e->w, e->h, e->fmt, e->pitch, e->sampled, wb_popcount64(e->hist), 10); }
    }
}
static void wb_sync_hook(void) { wb_sync(); }
/* [#218] before an NV3089 blit reads local [off, off + bytes): write back every deferred
 * (GPU-sampled) surface it overlaps -- render the batch so far first, since that surface's
 * draws may still be in it. Ticker thread, like wb_sync. */
static void wb_blit_src(u32 off, u32 bytes)
{
    if (!wb_on() || !s_d3d.initialized || !bytes) return;
    int hit = 0;
    for (int i = 0; i < s_wb_n; i++) {
        const WBEnt* e = &s_wb[i];
        if (!e->deferred && !(e->pending && e->sampled)) continue;
        const u32 eb = (e->pitch ? e->pitch : e->w * 4u) * e->h;
        if (off < e->off + eb && e->off < off + bytes) { hit = 1; break; }
    }
    if (!hit) return;
    if (s_d3d.draw_count > 0) { s_present_this_frame = 0; render_frame(); s_present_this_frame = 1; }
    for (int i = 0; i < s_wb_n; i++) {
        WBEnt* e = &s_wb[i];
        if (!e->deferred && !(e->pending && e->sampled)) continue;
        const u32 eb = (e->pitch ? e->pitch : e->w * 4u) * e->h;
        if (!(off < e->off + eb && e->off < off + bytes)) continue;
        const int slot = off_rt_find(e->off);
        if (slot < 0 || s_d3d.off_rt[slot].dxgi != (u32)DXGI_FORMAT_R8G8B8A8_UNORM) continue;
        wb_readback(slot, e);
        e->deferred = 0; e->pending = 0;
        e->wb_ever = 1; wb_snap(e);   /* #172 */
        { static long n = 0; n++;
          if (n <= 200 || (n % 100) == 0)
              fprintf(stderr, "[RTWB-DEMAND] #%ld rt=0x%08X %ux%u (blit reads 0x%08X+%u) -> guest%c",
                      n, e->off, e->w, e->h, off, bytes, 10); }
    }
}
/* [NHL local patch #77] A full batch must not lock the display path. Batches
 * present only once they hold a display-buffer draw; NHL's gameplay frames
 * carry more offscreen pass work (shadow/HDR/bloom/reflection passes) than the
 * record array held, so the array filled with offscreen records, every later
 * draw -- the display composite included -- was dropped, the batch could never
 * present, and it never emptied (play68: draws=2048, presented=0, nodisp=~100/s
 * from the end of the EA SPORTS transition on: the "frozen hexagons").
 * Before a draw would overflow, run what is accumulated (offscreen work; a
 * display draw already in it is presented, as the ticker path does) and start
 * an empty batch. Called before the draw uploads anything, so its vertex data
 * lands in the new frame's buffers. */
static void d3d12_flush_if_full(void)
{
    if (s_d3d.draw_count + 16 < MAX_DRAWS || !s_d3d.initialized) return;
    int has_display = 0;
    for (u32 _i = 0; _i < s_d3d.draw_count && _i < MAX_DRAWS; _i++)
        if (!s_d3d.draws[_i].is_clear && s_d3d.draws[_i].rt_off == 0) { has_display = 1; break; }
    { static int n = 0;
      if (n++ < 4) fprintf(stderr, "[batch-full] %u records -- flushing (display draws: %s)\n",
                           s_d3d.draw_count, has_display ? "presented" : "none, offscreen only"); }
    /* [#186] the surfaces the flushed batch ended on: the next batch continues them */
    const D3D12DrawRecord* last = s_d3d.draw_count ? &s_d3d.draws[s_d3d.draw_count - 1] : NULL;
    const int cont = last && !last->is_clear && last->rt_off;
    u32 cont_rt[4] = { 0, 0, 0, 0 };
    if (cont) { cont_rt[0] = last->rt_off; for (int m = 0; m < 3; m++) cont_rt[m + 1] = last->rt_mrt[m]; }
    s_present_this_frame = has_display;
    render_frame();          /* resets draw_count and the vertex/index offsets */
    s_present_this_frame = 1;
    s_depth_cont = cont; memcpy(s_depth_cont_rt, cont_rt, sizeof cont_rt);
}
void d3d12_flush_if_full_fwd(void) { d3d12_flush_if_full(); }   /* for d3d12_clear, defined earlier */

/* ---- [NHL local patch #125] GPU vertex fetch ---------------------------------
 * Instead of converting every vertex of every attribute on the CPU (16 float4 =
 * 256 bytes per vertex, ~4M vertices per 20 match frames: the RSX thread's top
 * cost), copy the guest's raw arrays for the draw's index range into vp_raw and
 * let the vertex shader (rsx_vp_decompile_gpu) decode them. Covers indexed
 * TRIANGLES / STRIP / FAN and non-indexed STRIP / FAN (~98% of NHL's match
 * vertices; whole non-indexed TRIANGLES too since #133); everything else, and any draw this cannot handle (a range that is
 * not contiguous in guest memory, the ring full, a VS that fails), keeps the
 * CPU path. NHL_GPU_VTX=0 = CPU path everywhere. [GPUVTX] reports every 5 s. */
typedef struct { u32 attr[16][4]; float def[16][4]; u32 vtx[4]; } VtxDesc;
static struct { u64 draws, idx, bytes, fb_vs, fb_contig, fb_room, fb_sparse, fb_partial; } s_gv;
/* 0 = off, 1 = on (default). 2 was a test mode for whole non-indexed TRIANGLES,
 * which #133 made the default; it now behaves as 1. */
static int gpu_vtx_on(void)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("NHL_GPU_VTX"); on = e ? atoi(e) : 1; }
    return (s_d3d.vp_raw_mapped && s_d3d.vp_ib_mapped) ? on : 0;
}
static void gpu_vtx_stats(void)
{
    static ULONGLONG t0 = 0; const ULONGLONG t = GetTickCount64();
    if (!t0) t0 = t;
    if (t - t0 < 5000) return;
    if (s_gv.draws || s_gv.fb_vs || s_gv.fb_contig || s_gv.fb_room || s_gv.fb_sparse || s_gv.fb_partial)
        fprintf(stderr, "[GPUVTX] 5 s: %llu draws, %llu indices, %.1f MB raw | CPU fallback: vs %llu, non-contiguous %llu, ring full %llu, sparse %llu, partial/continued tris %llu%c",
                s_gv.draws, s_gv.idx, s_gv.bytes / 1048576.0, s_gv.fb_vs, s_gv.fb_contig, s_gv.fb_room, s_gv.fb_sparse, s_gv.fb_partial, 10);
    memset(&s_gv, 0, sizeof s_gv); t0 = t;
}
static u32 vtx_elem_bytes(u32 type, u32 n)
{
    switch (type) {
    case 3: case 1: case 5: return 2u * n;   /* half, s16 norm, s16 */
    case 4: case 7:         return n;        /* u8 norm / unnorm */
    case 6:                 return 4u;       /* CMP 11:11:10 */
    default:                return 4u * n;   /* float32 (and unknown, as the CPU path) */
    }
}
/* Copy the bytes guest vertices lo..hi read into vp_raw and fill the attribute
 * table. Returns 0 (nothing consumed) when the draw must use the CPU path. */
static int gpu_vtx_setup(const rsx_state* st, u32 lo, u32 hi, VtxDesc* d)
{
    extern uint8_t* vm_base;
    extern u32 cellGcmResolveLocated(int local, u32 offset);
    struct { u32 ea0, ea1; int k; u32 e_lo; } seg[16]; int ns = 0;
    memset(d, 0, sizeof *d);
    d->vtx[0] = st->vertex_base_index; d->vtx[1] = lo;
    for (int k = 0; k < 16; k++) {
        const rsx_vertex_attrib* a = &st->vertex_attribs[k];
        if (!a->enabled || a->stride == 0) {          /* constant register */
            memcpy(d->def[k], st->vertex_data4f[k], 16);
            continue;
        }
        u32 n = a->size ? a->size : 4; if (n > 4) n = 4;
        u32 mode, e_lo, e_hi;
        if (a->frequency <= 1) {
            mode = 1;
            e_lo = (lo + st->vertex_base_index) & 0x000FFFFFu;
            e_hi = (hi + st->vertex_base_index) & 0x000FFFFFu;
            if (e_hi < e_lo) { s_gv.fb_contig++; return 0; }   /* index wrap */
        } else if (st->frequency_divider_op & (1u << k)) {
            const u32 f = a->frequency; mode = 3;
            if (hi - lo + 1 >= f) { e_lo = 0; e_hi = f - 1; }
            else { e_lo = lo % f; e_hi = hi % f; if (e_hi < e_lo) { e_lo = 0; e_hi = f - 1; } }
        } else {
            const u32 f = a->frequency; mode = 2;
            e_lo = lo / f; e_hi = hi / f;
        }
        const u32 esz  = vtx_elem_bytes(a->type, n);
        const u32 base = (a->offset & 0x7FFFFFFFu) + st->vertex_base_offset;
        const u32 off0 = base + e_lo * a->stride;
        const u32 off1 = base + e_hi * a->stride + esz;     /* exclusive */
        const int local = !(a->offset & 0x80000000u);
        const u32 ea0 = cellGcmResolveLocated(local, off0);
        const u32 eaL = cellGcmResolveLocated(local, off1 - 1);
        if (off1 <= off0 || ea0 == 0xFFFFFFFFu || eaL - ea0 != off1 - 1 - off0 ||
            (u64)ea0 + (off1 - off0) > 0x100000000ull) { s_gv.fb_contig++; return 0; }
        seg[ns].ea0 = ea0; seg[ns].ea1 = eaL + 1; seg[ns].k = k; seg[ns].e_lo = e_lo; ns++;
        d->attr[k][1] = a->stride;
        d->attr[k][2] = (a->type & 15u) | (n << 4) | (mode << 8);
        d->attr[k][3] = a->frequency;
    }
    /* Interleaved attributes overlap: merge into as few copies as possible. */
    for (int i = 1; i < ns; i++)
        for (int j = i; j > 0 && seg[j].ea0 < seg[j - 1].ea0; j--) {
            const __typeof__(seg[0]) t = seg[j]; seg[j] = seg[j - 1]; seg[j - 1] = t; }
    u32 g0 = 0, g1 = 0, gdst = 0; int open = 0;
    u32 off = s_d3d.vp_raw_offset;
    u8* const half = s_d3d.vp_raw_mapped + (u64)s_d3d.vp_parity * VP_RAW_BYTES;
    for (int i = 0; i <= ns; i++) {
        if (open && (i == ns || seg[i].ea0 > g1 + 256u)) {   /* close the group: copy it */
            memcpy(half + gdst, vm_base + g0, g1 - g0);
            off = gdst + (g1 - g0) + 16u;          /* pad: vf_be32 may read one dword past */
            open = 0;
        }
        if (i == ns) break;
        if (!open) {
            g0 = seg[i].ea0; g1 = seg[i].ea1; open = 1;
            gdst = ((off + 15u) & ~15u) + (g0 & 15u);
            /* room for this whole group (later segments may extend it, checked below) */
        } else if (seg[i].ea1 > g1) g1 = seg[i].ea1;
        if ((u64)gdst + (g1 - g0) + 32u > VP_RAW_BYTES) { s_gv.fb_room++; return 0; }
        const int k = seg[i].k;
        d->attr[k][0] = (gdst + (seg[i].ea0 - g0)) - seg[i].e_lo * d->attr[k][1];   /* mod 2^32 */
    }
    s_gv.bytes += off - s_d3d.vp_raw_offset;
    s_d3d.vp_raw_offset = off;
    return 1;
}
/* GV_CHECK=<n> (diag): for the first n GPU-fetch draws, decode vertices lo and
 * hi exactly as the shader will (from vp_raw + the table) and compare with
 * rsx_fetch_attrib. Separates "the upload/table is wrong" from "the GPU side is
 * wrong". */
static u32 gv_be32(const u8* half, u32 a)
{ const u8* p = half + a; return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }
static void gv_check(const rsx_state* st, const VtxDesc* d, u32 vi)
{
    const u8* half = s_d3d.vp_raw_mapped + (u64)s_d3d.vp_parity * VP_RAW_BYTES;
    for (int k = 0; k < 16; k++) {
        const u32* a = d->attr[k];
        const u32 mode = (a[2] >> 8) & 3u, type = a[2] & 15u, n = (a[2] >> 4) & 7u;
        float g[4] = { 0, 0, 0, 1 }, c[4];
        if (mode == 0) memcpy(g, d->def[k], 16);
        else {
            const u32 e = mode == 1 ? ((vi + d->vtx[0]) & 0xFFFFFu) : mode == 2 ? vi / (a[3] ? a[3] : 1) : vi % (a[3] ? a[3] : 1);
            const u32 ad = a[0] + e * a[1];
            for (u32 q = 0; q < n && q < 4; q++) {
                u32 w;
                switch (type) {
                case 3: { extern float rsx_rd_half_be(const u8*); g[q] = rsx_rd_half_be(half + ad + q * 2); break; }
                case 4: g[q] = (float)(gv_be32(half, ad + q) >> 24) / 255.0f; break;
                case 7: g[q] = (float)(gv_be32(half, ad + q) >> 24); break;
                case 1: g[q] = (float)((int)gv_be32(half, ad + q * 2) >> 16) / 32767.0f; break;
                case 5: g[q] = (float)((int)gv_be32(half, ad + q * 2) >> 16); break;
                default: w = gv_be32(half, ad + q * 4); memcpy(&g[q], &w, 4); break;
                }
            }
        }
        rsx_fetch_attrib(st, k, vi, c);
        if (memcmp(g, c, 16) && !(type == 6 && mode))
            fprintf(stderr, "[GVCHECK] vi=%u attr%d mode=%u type=%u n=%u stride=%u xoff=0x%X: gpu=(%g,%g,%g,%g) cpu=(%g,%g,%g,%g)%c",
                    vi, k, mode, type, n, a[1], a[0], g[0], g[1], g[2], g[3], c[0], c[1], c[2], c[3], 10);
    }
}
/* After vp_record_cb has filled the draw's constant slot: append the table. */
static void gpu_vtx_write_cb(u32 slot, const VtxDesc* d)
{
    if (!s_d3d.vp_cb_mapped || slot >= MAX_DRAWS) return;
    u8* cb = (u8*)s_d3d.vp_cb_mapped + ((u64)s_d3d.vp_parity * MAX_DRAWS + slot) * VP_CB_STRIDE;
    memcpy(cb + VP_CB_ATTR_VEC4 * 16, d, sizeof *d);
}
/* Guest index reader with the base resolved once per draw. */
typedef struct { const u8* p; int u16; } IdxRd;
static IdxRd idx_reader(const rsx_state* st)
{
    extern uint8_t* vm_base;
    extern u32 cellGcmResolveLocated(int local, u32 offset);
    IdxRd r;
    r.p = vm_base + cellGcmResolveLocated((st->index_array_dma & 0xF) == 0, st->index_array_offset);
    r.u16 = ((st->index_array_dma >> 4) & 0xF) == 1;
    return r;
}
static inline u32 idx_at(const IdxRd* r, u32 k)
{
    if (r->u16) { const u8* q = r->p + (u64)k * 2; return ((u32)q[0] << 8) | q[1]; }
    const u8* q = r->p + (u64)k * 4;
    return ((u32)q[0] << 24) | ((u32)q[1] << 16) | ((u32)q[2] << 8) | q[3];
}
/* Build one GPU-fetch draw: indices (relative to lo) into vp_ib, raw bytes into
 * vp_raw, the VS variant. prim: RSX_PRIMITIVE_TRIANGLES / _STRIP / _FAN;
 * indexed = 0: vertices first..first+count-1. Returns the index count (0 = use
 * the CPU path; nothing has been consumed then). */
static u32 gpu_vtx_draw(const rsx_state* st, u32 prim, int indexed, u32 first, u32 count,
                        int* vs_idx, u32* ib_first, VtxDesc* d)
{
    if (!st || !st->vertex_attribs[0].enabled || count < 3) return 0;
    const int strip = (prim == RSX_PRIMITIVE_TRIANGLE_STRIP), fan = (prim == RSX_PRIMITIVE_TRIANGLE_FAN);
    /* NHL_GPU_VTX_MASK (bisect): 1 indexed tris, 2 indexed strips, 4 indexed fans,
     * 8 strips, 16 fans, 32 plain tris (test mode 2); default all */
    { static int mask = -1;
      if (mask < 0) { const char* e = getenv("NHL_GPU_VTX_MASK"); mask = e ? (int)strtol(e, NULL, 0) : 63; }
      const int bit = indexed ? (strip ? 2 : fan ? 4 : 1) : (strip ? 8 : fan ? 16 : 32);
      if (!(mask & bit)) return 0; }
    const u32 nidx = (strip || fan) ? (count - 2) * 3 : (count / 3) * 3;
    if (!nidx || s_d3d.vp_ib_offset + nidx > VP_MAX_INDICES) { s_gv.fb_room++; return 0; }
    const IdxRd ir = indexed ? idx_reader(st) : (IdxRd){ NULL, 0 };
    u32 lo = first, hi = first + count - 1;
    if (indexed) {
        lo = 0xFFFFFFFFu; hi = 0;
        for (u32 k = 0; k < count; k++) { const u32 ix = idx_at(&ir, first + k);
            if (ix < lo) lo = ix; if (ix > hi) hi = ix; }
        /* a few far-apart indices would copy a huge span: leave those to the CPU
         * (64-bit: a 0xFFFFFFFF restart index made hi - lo + 1 wrap to 0) */
        if ((u64)hi - lo + 1u > (u64)count * 4u + 4096u) { s_gv.fb_sparse++; return 0; }
    }
    /* #225: a GPU-fetch geometry draw may get its VS in the background; display
     * draws and full-screen passes compile now. #225c: a shadow-pass draw (colour
     * target NONE reads as offset 0 = "display") is not a display draw -- its big
     * skinning VSs were the remaining synchronous compiles (up to 1.36 s each);
     * the depth path skips a draw whose VS is not ready. */
    { const u32 rt = current_rt_off(NULL, NULL, NULL);   /* #229: one-off targets compile now too */
      s_async_sync = !async_all() && !s_depth_rec && (rt == 0 || rt_oneoff(rt)); }
    s_rec_small = !async_all() && count <= 6;   /* #225b: decided per VS size in vp_get_vs_slow */
    *vs_idx = vp_get_vs_ex(st, 1);
    s_async_sync = 1; s_rec_small = 0;
    if (*vs_idx < 0) { s_gv.fb_vs++; return 0; }
    if (!gpu_vtx_setup(st, lo, hi, d)) return 0;
    { static int cap = -1, n = 0;
      if (cap < 0) { const char* e = getenv("GV_CHECK"); cap = e ? atoi(e) : 0; }
      if (n < cap) { n++;
          fprintf(stderr, "[GVCHECK] draw %d prim=%u indexed=%d first=%u count=%u lo=%u hi=%u vbi=%u vbo=0x%X raw=%u%c",
                  n, prim, indexed, first, count, lo, hi, st->vertex_base_index, st->vertex_base_offset, s_d3d.vp_raw_offset, 10);
          gv_check(st, d, lo); gv_check(st, d, hi); } }
    {   /* the Apt placeholder marker read_vp_vertex looks for (first vertex only) */
        extern uint8_t* vm_base; extern u32 cellGcmResolveLocated(int, u32);
        float v8[4]; rsx_fetch_attrib(st, 8, lo, v8);
        const rsx_vertex_attrib* a3 = &st->vertex_attribs[3];
        if (v8[0] == -65536.0f && a3->enabled && a3->stride) {
            const u8* p = vm_base + cellGcmResolveLocated((a3->offset & 0x80000000u) ? 0 : 1,
                                                          (a3->offset & 0x7FFFFFFFu) + lo * a3->stride);
            if (p[0] == 0x4E && p[1] == 0x48 && p[2] == 0x4C && p[3] == 0x00) s_vp_placeholder = 1;
        }
    }
    u32* ib = (u32*)s_d3d.vp_ib_mapped + (u64)s_d3d.vp_parity * VP_MAX_INDICES + s_d3d.vp_ib_offset;
    #define GV_IX(k) ((indexed ? idx_at(&ir, first + (k)) : first + (k)) - lo)
    if (strip || fan) {
        for (u32 t = 0, o = 0; t + 2 < count; t++) {
            u32 i1 = t + 1, i2 = t + 2;
            /* strips alternate winding: swap odd triangles so culling keeps them */
            if (strip && (t & 1u)) { const u32 x = i1; i1 = i2; i2 = x; }
            ib[o++] = GV_IX(fan ? 0u : t); ib[o++] = GV_IX(i1); ib[o++] = GV_IX(i2);
        }
    } else {
        for (u32 k = 0; k < nidx; k++) ib[k] = GV_IX(k);
    }
    #undef GV_IX
    *ib_first = s_d3d.vp_ib_offset;
    s_d3d.vp_ib_offset += nidx;
    s_gv.draws++; s_gv.idx += nidx;
    gpu_vtx_stats();
    return nidx;
}

/* [NHL local patch #113] A surface with NO colour target (SET_SURFACE_COLOR_TARGET
 * = NONE: NHL's shadow-map passes, 1024x1024 depth only) has colour offset 0,
 * which current_rt_off() reports as 0 = THE DISPLAY. Its clears then counted as
 * display clears -- presents fired mid-frame, the backbuffer was re-cleared, and
 * batches that ended before the scene composite showed a flat (white) screen:
 * the match's constant blinking. Depth-only work has nothing to show here
 * (depth-as-texture is not emulated), so its draws and clears are dropped.
 * RSX_DEPTHONLY_KEEP=1 restores the old behaviour; [DEPTHONLY] counts them. */
static int rsx_depth_only_pass(int is_clear)
{
    const rsx_state* st = s_d3d.current_rsx_state;
    static int keep = -1; if (keep < 0) keep = getenv("RSX_DEPTHONLY_KEEP") ? 1 : 0;
    if (keep || !st || !st->color_target_set || st->color_target != 0) return 0;
    static u32 nd = 0, nc = 0; static ULONGLONG t0 = 0;
    if (is_clear) nc++; else nd++;
    const ULONGLONG t = GetTickCount64(); if (!t0) t0 = t;
    if (t - t0 >= 5000) { fprintf(stderr, "[DEPTHONLY] %s %u draws + %u clears / 5 s (zeta 0x%X clip %ux%u)%c",
                                  shadows_on() ? "rendered (#190)" : "dropped",
                                  nd, nc, st->surface_zeta_offset, st->surface_clip_w, st->surface_clip_h, 10);
                          nd = nc = 0; t0 = t; }
    return 1;
}

/* [#190] a depth-only (shadow) draw is recorded by the normal path, then marked */
static int s_depth_rec;
static void depth_mark(u32 from)
{
    const rsx_state* st = s_d3d.current_rsx_state;
    if (!st) return;
    for (u32 i = from; i < s_d3d.draw_count && i < MAX_DRAWS; i++) {
        D3D12DrawRecord* dr = &s_d3d.draws[i];
        if (!dr->is_vp || dr->is_clear) continue;
        dr->is_depth = 1; dr->is_clear = 1;   /* is_clear: every colour path skips it */
        dr->zeta_off = st->surface_zeta_offset; dr->z_w = st->surface_clip_w; dr->z_h = st->surface_clip_h;
    }
    /* exists before the pre-pass binds it for this batch's receivers */
    shadowz_get(st->surface_zeta_offset, st->surface_clip_w, st->surface_clip_h);
}
static void d3d12_draw_arrays(void* ud, u32 primitive, u32 first, u32 count)
{
    if (!s_depth_rec && rsx_depth_only_pass(0)) {   /* #113 / #190 */
        if (!shadows_on()) return;
        d3d12_flush_if_full();
        const u32 b = s_d3d.draw_count;
        s_depth_rec = 1; d3d12_draw_arrays(ud, primitive, first, count); s_depth_rec = 0;
        depth_mark(b);
        return;
    }
    d3d12_flush_if_full();
    /* RSX_LIVE_FEED_DBG=1: pair with the [feed] method counts so "the FIFO
     * carries no draw batches" can be checked against "the backend is told
     * to draw" in the same run. */
    { static int dbg = -1; if (dbg < 0) dbg = getenv("RSX_LIVE_FEED_DBG") ? 1 : 0;
      if (dbg) { static long long n = 0;
          if (((++n) % 20000) == 0)
              printf("[backend] arrays callbacks=%lld (prim=%u count=%u)\n", n, primitive, count), fflush(stdout); } }
    (void)ud;
    /* Log the first 20 calls in detail, then every 1000th to show liveness
     * without flooding. */
    static u64 s_total = 0;
    if (s_total < 20 || (s_total % 1000) == 0) {
        printf("[D3D12] draw_arrays #%llu prim=%u first=%u count=%u\n",
               (unsigned long long)s_total, primitive, first, count);
    }
    s_total++;

    if (!s_d3d.pipeline_ready || !s_d3d.vb_mapped) return;
    if (count == 0 || count > MAX_VERTICES) return;

    /* QUADS (prim 8) have no D3D12 topology; expand to a triangle list.
     * dbgfont draws all text as quads. Prefer the real vertex-program path
     * (exact position + texcoord): upload raw float4 attrib0 into vp_vb and
     * mark the draw is_vp; render_frame compiles the VP and draws it. Fall
     * back to the frac-approximation textured path if VP resources are absent. */
    if (primitive == RSX_PRIMITIVE_QUADS) {
        /* Prefer the real vertex-program path whenever VP resources exist --
         * NOT only when a texture is bound. Requiring tex_bound routed vkcube's
         * UNtextured cube to the fixed-function fallback, which never applies the
         * VP's MVP transform, so the cube was drawn in object space and clipped
         * off-screen. Untextured VP draws now render via the colour PSO. */
        if (s_d3d.vp_vb_mapped && s_d3d.vp_root_sig) {
            u32 rec = s_d3d.vp_vb_offset;
            u32 emitted = upload_quads_vp(s_d3d.current_rsx_state, first, count);
            if (emitted && s_d3d.draw_count < MAX_DRAWS) {
                D3D12DrawRecord* dr = &s_d3d.draws[s_d3d.draw_count];
                dr->vb_byte_offset = rec;
                dr->vertex_count   = emitted;
                dr->topology       = RSX_TOPOLOGY_TRIANGLES;
                dr->textured       = s_d3d.tex_bound;
                dr->is_vp          = 1;
                dr->fp_addr = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->shader_program : 0; fp_snapshot(dr);   /* #82 */
                dr->fp_exp32 = s_d3d.current_rsx_state ?
                    rsx_ctrl_key(s_d3d.current_rsx_state) : rsx_ctrl_key(NULL);
                dr->cull = rsx_cull_key(s_d3d.current_rsx_state); dr_note_view(dr);   /* #108 */
        dr->begin_epoch = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->begin_epoch : 0;
            dr->begin_epoch = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->begin_epoch : 0;
                dr->cmask = 0xF;
                if (s_d3d.current_rsx_state) {
                    u32 _cm = s_d3d.current_rsx_state->color_mask;
                    dr->cmask = rsx_cmask_d3d(_cm);   /* #110 */
                }
                dr_set_stencil(dr); dr->alpha_ctl = 0;
                if (s_d3d.current_rsx_state)
                    dr->alpha_ctl = ((s_d3d.current_rsx_state->alpha_test_enable ? 1u : 0u) << 16)
                                  | ((s_d3d.current_rsx_state->alpha_func & 0xFFu) << 8)
                                  | (s_d3d.current_rsx_state->alpha_ref & 0xFFu);
                for (int _u = 0; _u < D3D_TEX_UNITS; _u++) {
                    dr->tex[_u].off = s_d3d.cur_texs[_u].off;
                    dr->tex[_u].raw = s_d3d.cur_texs[_u].raw;
                    dr->tex[_u].w   = s_d3d.cur_texs[_u].w;
                    dr->tex[_u].h   = s_d3d.cur_texs[_u].h;
                    dr->tex[_u].fmt = s_d3d.cur_texs[_u].fmt;
                    dr->tex[_u].ctrl1 = s_d3d.cur_texs[_u].ctrl1; dr->tex[_u].pitch = s_d3d.cur_texs[_u].pitch; dr->tex[_u].filter = s_d3d.cur_texs[_u].filter; dr->tex[_u].snap = NULL; dr->tex[_u].addr = s_d3d.cur_texs[_u].addr; dr->tex[_u].ctl0 = s_d3d.cur_texs[_u].ctl0; dr->tex[_u].dim = s_d3d.cur_texs[_u].dim;   /* #190 */
                    dr->tex[_u].cube  = s_d3d.cur_texs[_u].cube;
                    dr->tex[_u].mips  = s_d3d.cur_texs[_u].mips;
                    dr->tex[_u].set = s_d3d.cur_texs[_u].set;
                    dr->tex_rt[_u]  = -1;
                }
                dr->tex_slot = -1;
                vp6_snap_record(dr);   /* #179 */
                dr->vs_idx = vp_get_vs_rec(count);   /* #225d */
                dr->is_clear = 0;
                dr->blend = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->blend_enable : 1;
                dr->blend_key = rsx_blend_key(s_d3d.current_rsx_state, dr->blend);
                if (getenv("BLENDDBG")) { static u32 seen[32]; static int ns=0;
                    u32 k = (dr->blend ? 0x80000000u : 0u) | dr->blend_key;
                    int f=0; for (int i=0;i<ns;i++) if (seen[i]==k) f=1;
                    if (!f && ns<32) { seen[ns++]=k;
                        fprintf(stderr, "[BLEND] enable=%d key=0x%X%c",
                                dr->blend, dr->blend_key, 10); } }
                dr->rt_off = current_rt_off(&dr->rt_w, &dr->rt_h, dr->rt_mrt);
                dr->rt_fmt = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->surface_format : 0;
                if (s_d3d.current_rsx_state) {
                    dr->vp_x = s_d3d.current_rsx_state->viewport_x;
                    dr->vp_y = s_d3d.current_rsx_state->viewport_y;
                    dr->vp_w = s_d3d.current_rsx_state->viewport_w;
                    dr->vp_h = s_d3d.current_rsx_state->viewport_h;
                } else { dr->vp_x = dr->vp_y = dr->vp_w = dr->vp_h = 0; }
                dr->cb_slot = s_d3d.draw_count;
                vp_record_cb(s_d3d.draw_count, dr->vs_idx, dr);
                s_d3d.draw_count++;
            }
            return;
        }
        u32 record_offset = s_d3d.vb_offset;
        u32 emitted = upload_quads_from_rsx(first, count);
        if (emitted == 0) return;
        if (s_d3d.draw_count < MAX_DRAWS) {
            s_d3d.draws[s_d3d.draw_count].vb_byte_offset = record_offset;
            s_d3d.draws[s_d3d.draw_count].vertex_count   = emitted;
            s_d3d.draws[s_d3d.draw_count].topology       = RSX_TOPOLOGY_TRIANGLES;
            s_d3d.draws[s_d3d.draw_count].textured       = s_d3d.tex_bound;
            s_d3d.draws[s_d3d.draw_count].is_vp          = 0;
            s_d3d.draws[s_d3d.draw_count].is_clear       = 0;
            s_d3d.draws[s_d3d.draw_count].rt_off         = 0;
            s_d3d.draw_count++;
        }
        return;
    }

    /* TRIANGLES (prim 5): same VP-path preference as quads -- the guest's
     * vertex program does the MVP transform (gcm/cube draws its cube this way);
     * the fixed-function fallback below applies no transform, so 3D geometry
     * ends up in object space (invisible/garbage). */
    /* Route TRIANGLES/STRIP/FAN through the VP path (sagemono: LBP draws
     * non-indexed strips/fans), but ONLY when the guest ACTUALLY HAS a vertex
     * program (flOw). The VP path transforms via the guest's VP microcode; with
     * no microcode loaded it transforms nothing and the draw silently produces
     * zero pixels (it also uploads to vp_vb, leaving the fixed-function vb empty).
     * Geometry already in clip space with no VP -- flOw's injected scene -- must
     * go down the fixed-function passthrough below instead. */
    if ((primitive == RSX_PRIMITIVE_TRIANGLES || primitive == RSX_PRIMITIVE_TRIANGLE_STRIP
         || primitive == RSX_PRIMITIVE_TRIANGLE_FAN) &&
        s_d3d.vp_vb_mapped && s_d3d.vp_root_sig &&
        s_d3d.current_rsx_state && s_d3d.current_rsx_state->vp_ucode_bytes >= 16) {
        u32 rec = s_d3d.vp_vb_offset;
        /* [#125] strips/fans: GPU vertex fetch. [#133] TRIANGLES too, now that
         * rsx_commands joins a list's 256-vertex words into one call: whole lists
         * only -- a partial one (or a continuation) keeps the CPU merge below. */
        int gv_vs = -1; u32 gv_ib = 0; static VtxDesc gv_desc;
        const int gv_tri_ok = count % 3 == 0 &&
                              !(s_d3d.merge_prev_draw && s_d3d.merge_first_end == first);
        if (primitive == RSX_PRIMITIVE_TRIANGLES && !gv_tri_ok && gpu_vtx_on()) s_gv.fb_partial++;
        const u32 gv_n = (gpu_vtx_on() && (primitive != RSX_PRIMITIVE_TRIANGLES || gv_tri_ok))
            ? gpu_vtx_draw(s_d3d.current_rsx_state, primitive, 0, first, count, &gv_vs, &gv_ib, &gv_desc) : 0;
        u32 emitted = gv_n ? gv_n
            : (primitive == RSX_PRIMITIVE_TRIANGLES)
            ? upload_tris_vp(s_d3d.current_rsx_state, first, count)
            : upload_strip_vp(s_d3d.current_rsx_state, first, count, primitive == RSX_PRIMITIVE_TRIANGLE_FAN);
        /* RSX splits one primitive stream across several DRAW_ARRAYS entries
         * inside a single BEGIN/END (each carries at most 256 vertices), and the
         * hardware concatenates them before assembling primitives. Recording a
         * draw per batch instead regroups the triangles: this title's tub mesh
         * arrives as 128 then 256,256,..., neither a multiple of 3, so every
         * batch after the first was assembled one vertex out of phase -- which
         * rendered as spike triangles with a zero texcoord along the tub rim.
         * The batches land contiguously in the vertex buffer, so extending the
         * previous record is all that is needed to restore the stream. */
        if (emitted && !gv_n && s_d3d.merge_prev_draw && s_d3d.draw_count > 0 &&
            s_d3d.merge_first_end == first) {
            D3D12DrawRecord* pv = &s_d3d.draws[s_d3d.draw_count - 1];
            u32 fpnow = s_d3d.current_rsx_state
                        ? s_d3d.current_rsx_state->shader_program : 0;
            if (pv->is_vp && !pv->indexed && pv->topology == RSX_TOPOLOGY_TRIANGLES &&
                pv->fp_addr == fpnow && primitive == RSX_PRIMITIVE_TRIANGLES &&
                pv->begin_epoch == (s_d3d.current_rsx_state
                                    ? s_d3d.current_rsx_state->begin_epoch : 0) &&
                pv->vb_byte_offset + pv->vertex_count * VP_VERT_STRIDE == rec) {
                pv->vertex_count += emitted;
                s_d3d.merge_first_end = first + count;
                return;
            }
        }
        if (emitted && s_d3d.draw_count < MAX_DRAWS) {
            D3D12DrawRecord* dr = &s_d3d.draws[s_d3d.draw_count];
            dr->vb_byte_offset = rec;
            dr->vertex_count   = emitted;
            dr->topology       = RSX_TOPOLOGY_TRIANGLES;
            dr->textured       = s_d3d.tex_bound;
            dr->is_vp          = 1;
            dr->fp_addr = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->shader_program : 0; fp_snapshot(dr);   /* #82 */
            dr->fp_exp32 = s_d3d.current_rsx_state ?
                rsx_ctrl_key(s_d3d.current_rsx_state) : rsx_ctrl_key(NULL);
            dr->cull = rsx_cull_key(s_d3d.current_rsx_state); dr_note_view(dr);   /* #108 */
        dr->begin_epoch = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->begin_epoch : 0;
            dr->begin_epoch = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->begin_epoch : 0;
            dr->cmask = 0xF;
            if (s_d3d.current_rsx_state) {
                u32 _cm = s_d3d.current_rsx_state->color_mask;
                dr->cmask = rsx_cmask_d3d(_cm);   /* #110 */
            }
            dr_set_stencil(dr); dr->alpha_ctl = 0;
            if (s_d3d.current_rsx_state)
                dr->alpha_ctl = ((s_d3d.current_rsx_state->alpha_test_enable ? 1u : 0u) << 16)
                              | ((s_d3d.current_rsx_state->alpha_func & 0xFFu) << 8)
                              | (s_d3d.current_rsx_state->alpha_ref & 0xFFu);
            for (int _u = 0; _u < D3D_TEX_UNITS; _u++) {
                dr->tex[_u].off = s_d3d.cur_texs[_u].off;
                dr->tex[_u].raw = s_d3d.cur_texs[_u].raw;
                dr->tex[_u].w   = s_d3d.cur_texs[_u].w;
                dr->tex[_u].h   = s_d3d.cur_texs[_u].h;
                dr->tex[_u].fmt = s_d3d.cur_texs[_u].fmt;
                    dr->tex[_u].ctrl1 = s_d3d.cur_texs[_u].ctrl1; dr->tex[_u].pitch = s_d3d.cur_texs[_u].pitch; dr->tex[_u].filter = s_d3d.cur_texs[_u].filter; dr->tex[_u].snap = NULL; dr->tex[_u].addr = s_d3d.cur_texs[_u].addr; dr->tex[_u].ctl0 = s_d3d.cur_texs[_u].ctl0; dr->tex[_u].dim = s_d3d.cur_texs[_u].dim;   /* #190 */
                    dr->tex[_u].cube  = s_d3d.cur_texs[_u].cube;
                    dr->tex[_u].mips  = s_d3d.cur_texs[_u].mips;
                dr->tex[_u].set = s_d3d.cur_texs[_u].set;
                dr->tex_rt[_u]  = -1;
            }
            dr->tex_slot = -1;
            vp6_snap_record(dr);   /* #179 */
            dr->vs_idx = gv_n ? gv_vs : vp_get_vs_rec(count);   /* #125; #225d */
            dr->is_clear = 0;
            dr->blend = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->blend_enable : 1;
            dr->blend_key = rsx_blend_key(s_d3d.current_rsx_state, dr->blend);
            dr->rt_off = current_rt_off(&dr->rt_w, &dr->rt_h, dr->rt_mrt);
            dr->rt_fmt = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->surface_format : 0;
            if (s_d3d.current_rsx_state) {
                dr->vp_x = s_d3d.current_rsx_state->viewport_x;
                dr->vp_y = s_d3d.current_rsx_state->viewport_y;
                dr->vp_w = s_d3d.current_rsx_state->viewport_w;
                dr->vp_h = s_d3d.current_rsx_state->viewport_h;
            } else { dr->vp_x = dr->vp_y = dr->vp_w = dr->vp_h = 0; }
            dr->cb_slot = s_d3d.draw_count;
                vp_record_cb(s_d3d.draw_count, dr->vs_idx, dr);
            if (gv_n) {   /* [#125] */
                dr->indexed = 1; dr->ib_first = gv_ib;
                dr->gpu_vtx = 1; dr->vb_byte_offset = 0;
                gpu_vtx_write_cb(dr->cb_slot, &gv_desc);
            }
            s_d3d.draw_count++;
            /* Anchor for merging the next DRAW_ARRAYS batch of this stream. */
            s_d3d.merge_prev_draw  = (primitive == RSX_PRIMITIVE_TRIANGLES);
            s_d3d.merge_first_end  = first + count;
        }
        return;
    }
    s_d3d.merge_prev_draw = 0;   /* any other primitive breaks the stream */

    u32 topo = (u32)rsx_primitive_topology(primitive);
    if (topo == RSX_TOPOLOGY_UNSUPPORTED) {
        /* Other primitives still needing index-buffer conversion
         * (line loops, triangle fans) — skip rather than draw wrong. */
        static int s_skipped_nontri = 0;
        if (s_skipped_nontri < 3) {
            printf("[D3D12] draw_arrays: skipping prim=%u (needs index conversion)\n",
                   primitive);
            s_skipped_nontri++;
        }
        return;
    }

    u32 record_offset = s_d3d.vb_offset;
    u32 actual_count  = upload_vertices_from_rsx(first, count);
    if (actual_count == 0) return;

    if (s_d3d.draw_count < MAX_DRAWS) {
        s_d3d.draws[s_d3d.draw_count].vb_byte_offset = record_offset;
        s_d3d.draws[s_d3d.draw_count].vertex_count   = actual_count;
        s_d3d.draws[s_d3d.draw_count].topology       = topo;
        s_d3d.draws[s_d3d.draw_count].textured       = 0;
        s_d3d.draws[s_d3d.draw_count].is_vp          = 0;
        s_d3d.draws[s_d3d.draw_count].is_clear       = 0;
        s_d3d.draws[s_d3d.draw_count].rt_off         = 0;
        s_d3d.draw_count++;
    }
}

static void d3d12_draw_indexed(void* ud, u32 primitive, u32 first, u32 count)
{
    if (!s_depth_rec && rsx_depth_only_pass(0)) {   /* #113 / #190 */
        if (!shadows_on()) return;
        d3d12_flush_if_full();
        const u32 b = s_d3d.draw_count;
        s_depth_rec = 1; d3d12_draw_indexed(ud, primitive, first, count); s_depth_rec = 0;
        depth_mark(b);
        return;
    }
    d3d12_flush_if_full();
    /* RSX_LIVE_FEED_DBG=1: pair with the [feed] method counts so "the FIFO
     * carries no draw batches" can be checked against "the backend is told
     * to draw" in the same run. */
    { static int dbg = -1; if (dbg < 0) dbg = getenv("RSX_LIVE_FEED_DBG") ? 1 : 0;
      if (dbg) { static long long n = 0;
          if (((++n) % 20000) == 0)
              printf("[backend] indexed callbacks=%lld (prim=%u count=%u)\n", n, primitive, count), fflush(stdout); } }
    (void)ud;
    static int log_count = 0;
    if (log_count < 8) {
        printf("[D3D12] draw_indexed(prim=%u, first=%u, count=%u)\n",
               primitive, first, count);
        log_count++;
    }
    if (!s_d3d.pipeline_ready) return;
    if (count == 0 || count > MAX_VERTICES) return;
    if (!s_d3d.vp_vb_mapped || !s_d3d.vp_root_sig) return;

    /* Expand through the VP path (indices resolved CPU-side): QUADS -> two
     * triangles per quad, TRIANGLES straight through, STRIP/FAN -> triangle
     * list. Other primitives are skipped rather than drawn wrong. */
    u32 emitted = 0;
    u32 rec = s_d3d.vp_vb_offset;
    /* [#125] GPU vertex fetch first; any draw it declines takes the CPU path below */
    int gv_vs = -1; u32 gv_ib = 0; static VtxDesc gv_desc;
    /* TRIANGLES only as whole triangles that do not continue the previous
     * record: a batch ending mid-triangle is glued to the next by the CPU merge
     * below, and drawing the continuation on its own assembles it out of phase
     * (play175: geometry flung across the screen). */
    const int gv_tri_ok = (count % 3 == 0) &&
                          !(s_d3d.merge_prev_draw && s_d3d.merge_first_end == first);
    if (primitive == RSX_PRIMITIVE_TRIANGLES && !gv_tri_ok && gpu_vtx_on()) s_gv.fb_partial++;
    const u32 gv_n = (gpu_vtx_on() &&
                      ((primitive == RSX_PRIMITIVE_TRIANGLES && gv_tri_ok) ||
                       primitive == RSX_PRIMITIVE_TRIANGLE_STRIP || primitive == RSX_PRIMITIVE_TRIANGLE_FAN))
        ? gpu_vtx_draw(s_d3d.current_rsx_state, primitive, 1, first, count, &gv_vs, &gv_ib, &gv_desc) : 0;
    if (gv_n) { emitted = gv_n; s_idx_gpu_last = 0; }
    else if (primitive == RSX_PRIMITIVE_QUADS)        emitted = upload_quads_vp_indexed(s_d3d.current_rsx_state, first, count);
    else if (primitive == RSX_PRIMITIVE_TRIANGLES)    emitted = upload_tris_vp_indexed(s_d3d.current_rsx_state, first, count);
    else if (primitive == RSX_PRIMITIVE_TRIANGLE_STRIP) emitted = upload_strip_vp_indexed(s_d3d.current_rsx_state, first, count, 0);
    else if (primitive == RSX_PRIMITIVE_TRIANGLE_FAN)   emitted = upload_strip_vp_indexed(s_d3d.current_rsx_state, first, count, 1);
    else {
        static int _skip = 0;
        if (_skip++ < 3)
            printf("[D3D12] draw_indexed: skipping prim=%u (not wired)\n", primitive);
        return;
    }
    const int idx_gpu = gv_n || ((primitive == RSX_PRIMITIVE_TRIANGLES) && s_idx_gpu_last);  /* [NHL #62] */
    const u32 idx_first = gv_n ? gv_ib : s_idx_gpu_first;
    s_idx_gpu_last = 0;
    /* Same BEGIN/END batch concatenation as the non-indexed path: this title's
     * fluid arrives as ~3000 DRAW_INDEX_ARRAY batches of 256, and 256 is not a
     * multiple of 3, so every batch after the first assembled out of phase. */
    if (emitted && !idx_gpu && s_d3d.merge_prev_draw && s_d3d.draw_count > 0 &&
        s_d3d.merge_first_end == first && primitive == RSX_PRIMITIVE_TRIANGLES) {
        D3D12DrawRecord* pv = &s_d3d.draws[s_d3d.draw_count - 1];
        u32 fpnow = s_d3d.current_rsx_state
                    ? s_d3d.current_rsx_state->shader_program : 0;
        if (pv->is_vp && !pv->indexed && pv->topology == RSX_TOPOLOGY_TRIANGLES &&
            pv->fp_addr == fpnow &&
            pv->begin_epoch == (s_d3d.current_rsx_state
                                ? s_d3d.current_rsx_state->begin_epoch : 0) &&
            pv->vb_byte_offset + pv->vertex_count * VP_VERT_STRIDE == rec) {
            pv->vertex_count += emitted;
            s_d3d.merge_first_end = first + count;
            return;
        }
    }
    if (emitted && s_d3d.draw_count < MAX_DRAWS) {
        D3D12DrawRecord* dr = &s_d3d.draws[s_d3d.draw_count];
        dr->vb_byte_offset = rec;
        dr->vertex_count   = emitted;
        dr->topology       = RSX_TOPOLOGY_TRIANGLES;
        dr->textured       = s_d3d.tex_bound;
        dr->is_vp          = 1;
        dr->fp_addr = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->shader_program : 0; fp_snapshot(dr);   /* #82 */
        dr->fp_exp32 = s_d3d.current_rsx_state ?
            rsx_ctrl_key(s_d3d.current_rsx_state) : rsx_ctrl_key(NULL);
        dr->cull = rsx_cull_key(s_d3d.current_rsx_state); dr_note_view(dr);   /* #108 */
        dr->begin_epoch = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->begin_epoch : 0;
        dr->cmask = 0xF;
        if (s_d3d.current_rsx_state) {
            u32 _cm = s_d3d.current_rsx_state->color_mask;
            dr->cmask = rsx_cmask_d3d(_cm);   /* #110 */
        }
        dr_set_stencil(dr); dr->alpha_ctl = 0;
        if (s_d3d.current_rsx_state)
            dr->alpha_ctl = ((s_d3d.current_rsx_state->alpha_test_enable ? 1u : 0u) << 16)
                          | ((s_d3d.current_rsx_state->alpha_func & 0xFFu) << 8)
                          | (s_d3d.current_rsx_state->alpha_ref & 0xFFu);
        for (int _u = 0; _u < D3D_TEX_UNITS; _u++) {
            dr->tex[_u].off = s_d3d.cur_texs[_u].off;
            dr->tex[_u].raw = s_d3d.cur_texs[_u].raw;
            dr->tex[_u].w   = s_d3d.cur_texs[_u].w;
            dr->tex[_u].h   = s_d3d.cur_texs[_u].h;
            dr->tex[_u].fmt = s_d3d.cur_texs[_u].fmt;
                    dr->tex[_u].ctrl1 = s_d3d.cur_texs[_u].ctrl1; dr->tex[_u].pitch = s_d3d.cur_texs[_u].pitch; dr->tex[_u].filter = s_d3d.cur_texs[_u].filter; dr->tex[_u].snap = NULL; dr->tex[_u].addr = s_d3d.cur_texs[_u].addr; dr->tex[_u].ctl0 = s_d3d.cur_texs[_u].ctl0; dr->tex[_u].dim = s_d3d.cur_texs[_u].dim;   /* #190 */
                    dr->tex[_u].cube  = s_d3d.cur_texs[_u].cube;
                    dr->tex[_u].mips  = s_d3d.cur_texs[_u].mips;
            dr->tex[_u].set = s_d3d.cur_texs[_u].set;
            dr->tex_rt[_u]  = -1;
        }
        dr->tex_slot = -1;
        vp6_snap_record(dr);   /* #179 */
        dr->vs_idx = gv_n ? gv_vs : vp_get_vs_rec(count);   /* #125; #225d */
        dr->is_clear = 0;
        dr->blend = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->blend_enable : 1;
        dr->blend_key = rsx_blend_key(s_d3d.current_rsx_state, dr->blend);
        dr->rt_off = current_rt_off(&dr->rt_w, &dr->rt_h, dr->rt_mrt);
        dr->rt_fmt = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->surface_format : 0;
        if (s_d3d.current_rsx_state) {
            dr->vp_x = s_d3d.current_rsx_state->viewport_x;
            dr->vp_y = s_d3d.current_rsx_state->viewport_y;
            dr->vp_w = s_d3d.current_rsx_state->viewport_w;
            dr->vp_h = s_d3d.current_rsx_state->viewport_h;
        } else { dr->vp_x = dr->vp_y = dr->vp_w = dr->vp_h = 0; }
        dr->cb_slot = s_d3d.draw_count;
                vp_record_cb(s_d3d.draw_count, dr->vs_idx, dr);
        if (idx_gpu) { dr->indexed = 1; dr->ib_first = idx_first; }   /* [NHL #62] */
        if (gv_n) {   /* [#125] no CPU vertices: base vertex 0, attribute table in the CB */
            dr->gpu_vtx = 1; dr->vb_byte_offset = 0;
            gpu_vtx_write_cb(dr->cb_slot, &gv_desc);
        }
        s_d3d.draw_count++;
        /* Anchor for merging the next batch of this indexed stream. */
        s_d3d.merge_prev_draw = (primitive == RSX_PRIMITIVE_TRIANGLES) && !idx_gpu;
        s_d3d.merge_first_end = first + count;
    }
}

static void d3d12_bind_texture(void* ud, u32 unit, const rsx_texture_state* tex)
{
    (void)ud;
    extern uint8_t* vm_base;
    extern u32 cellGcmResolveOffset(u32);

    u32 width  = (tex->image_rect >> 16) & 0xFFFF;
    u32 height = tex->image_rect & 0xFFFF;
    u32 format = (tex->format >> 8) & 0xFF;
    u32 offset = tex->offset;
    /* CUBEDBG=1: NV4097_SET_TEXTURE_FORMAT bit 2 is the cubemap flag, and the
     * line above throws it away with the rest of the low byte. Nothing in this
     * backend handles cube textures, so a cubemap bound for an environment
     * reflection is sampled as a plain 2D image -- which is what the chrome
     * faucet's black patches and banded escutcheons look like. */
    { static int cd = -1;
      if (cd < 0) { const char* e = getenv("CUBEDBG"); cd = e ? atoi(e) : 0; }
      if (cd && (tex->format & 4)) {
          static u32 seen[8]; static int ns = 0; int known = 0;
          for (int k = 0; k < ns; k++) if (seen[k] == offset) known = 1;
          if (!known && ns < 8) { seen[ns++] = offset;
              fprintf(stderr, "[CUBE] unit=%u offset=0x%08X %ux%u fmt=0x%02X"
                              " raw_format=0x%08X (cubemap bit SET)%c",
                      unit, offset, width, height, format, tex->format, 10); }
      } }

    static int log_count = 0;
    if (log_count < 10) {
        printf("[D3D12] bind_texture(unit=%u, offset=0x%X, fmt=0x%02X, %ux%u)\n",
               unit, offset, format, width, height);
        log_count++;
    }
    /* MOVIE_BIND=1: trace movie-plane binds (640x360 Y / 320x180 U/V) with the
     * resolved EA + a content probe -- used to diagnose the Bink frame-buffer
     * ring mismatch (the draw binds a cleared buffer 0x4D80 before the one the
     * decoder actually fills). */
    static int s_movie_bind = -1;   /* #228: was a getenv per texture bind */
    if (s_movie_bind < 0) s_movie_bind = getenv("MOVIE_BIND") ? 1 : 0;
    if (s_movie_bind &&
        ((width == 640 && height == 360) || (width == 320 && height == 180))) {
        extern u32 cellGcmResolveLocated(int local, u32 offset);
        static int _mv = 0; if (_mv++ < 24) {
            /* Resolve the SAME raw offset both ways -- as LOCAL (VRAM) and as MAIN
             * (IO-mapped) -- and count nonzero bytes over the whole plane for each.
             * If the plane the game declares (loc bits fmt&3) is zero but the OTHER
             * pool has content, the decoder wrote to a different memory space than
             * our texture resolve picked (the local-vs-main mismatch). */
            u32 loc = tex->format & 3;               /* 1=LOCAL, 2=MAIN */
            u32 ea_loc  = cellGcmResolveLocated(1, offset);
            u32 ea_main = cellGcmResolveLocated(0, offset);
            u32 plane = width * height;
            u32 nz_loc = 0, nz_main = 0;
            if (vm_base) for (u32 i = 0; i < plane; i += 137) {
                if (vm_base[ea_loc  + i]) nz_loc++;
                if (vm_base[ea_main + i]) nz_main++;
            }
            fprintf(stderr, "[mv-bind] unit=%u fmt=0x%02X loc=%u %ux%u off=0x%X | LOCAL ea=0x%08X nz=%u | MAIN ea=0x%08X nz=%u\n",
                    unit, format, loc, width, height, offset,
                    ea_loc, nz_loc, ea_main, nz_main);
        }
    }

    if (!vm_base || width == 0 || height == 0) return;

    /* Record the currently-bound atlas so subsequent quad draws sample it. The
     * actual GPU upload happens in render_frame (we have no open command list
     * here). Only the 8-bit single-channel font atlas (B8, RSX fmt base 0x81 /
     * as-seen 0xA1 with the LN flag) is wired up so far; other formats fall
     * back to untextured. */
    u32 base_fmt = format & 0x9F;   /* strip LN(0x20)/UN(0x40) flags */
    /* VP path: record the latest bound texture (any supported format) so draws
     * can carry it per-draw. Location bits (format[1:0]): 1 = LOCAL, 2 = MAIN. */
    extern u32 cellGcmResolveLocated(int local, u32 offset);
    /* [NHL local patch #53] Accept every base format rsx_texture_layout decodes
     * (the packed 16-bit colours, X16/Y16X16, depth, float formats...), and
     * CLEAR a unit that is disabled (CONTROL0 bit 31) or has a format we cannot
     * decode. The old 6-format allow-list left such units untouched, so the
     * draw sampled whatever texture was bound there before -- a wrong
     * background and missing text on NHL's copyright slide.
     * NHL_TEX_GATE_OLD=1 restores the old allow-list and no clearing. */
    static int gate_old = -1;
    if (gate_old < 0) gate_old = getenv("NHL_TEX_GATE_OLD") ? 1 : 0;
    int fmt_ok = gate_old
        ? (base_fmt == 0x81 || base_fmt == 0x85 || base_fmt == 0x8B ||
           (base_fmt >= 0x86 && base_fmt <= 0x88) || base_fmt == 0x9A)
        : ((base_fmt >= 0x81 && base_fmt <= 0x88) ||
           (base_fmt >= 0x8B && base_fmt <= 0x8D) ||
           base_fmt == 0x90 || base_fmt == 0x92 || base_fmt == 0x94 ||
           base_fmt == 0x95 || base_fmt == 0x97 ||
           (base_fmt >= 0x9A && base_fmt <= 0x9F));
    int tex_on = gate_old ? 1 : ((tex->control0 >> 31) & 1u);
    if (unit < D3D_TEX_UNITS && !gate_old && (!fmt_ok || !tex_on || !width || !height)) {
        s_d3d.cur_texs[unit].set = 0;
        if (tex_on && !fmt_ok && width) {
            static u32 seen[32]; static int ns = 0; int f = 0;
            for (int i = 0; i < ns; i++) if (seen[i] == base_fmt) f = 1;
            if (!f && ns < 32) { seen[ns++] = base_fmt;
                fprintf(stderr, "[TEXGATE] unit %u: unsupported base format 0x%02X (%ux%u) -> unbound%c",
                        unit, base_fmt, width, height, 10); }
        }
    }
    if (unit < D3D_TEX_UNITS && fmt_ok && tex_on) {
        /* TEX_RESOLVE_AUTO=1: resolve through the page tables (local-page map
         * then IO table) instead of trusting the format's location bits. A
         * texture the guest built in main memory but tagged local resolves to
         * untouched VRAM and samples as all-zero -- geometry renders, flat. */
        { static int _ra = -1; if (_ra < 0) _ra = getenv("TEX_RESOLVE_AUTO") ? 1 : 0;
          extern u32 cellGcmResolveIO(u32);
          u32 _r = 0;
          if (_ra) _r = cellGcmResolveIO(offset);          /* IO table first */
          if (!_r) _r = cellGcmResolveLocated((tex->format & 3) == 1, offset);
          /* TEX_PROBE=1: the location bits are not always right. This title
           * binds its menu textures with a location that resolves into main
           * memory, where the bytes are all zero, while the same raw offset
           * resolved as LOCAL lands on real data -- so 100k menu draws sample
           * black and the menu never appears. When the chosen resolution is
           * empty over a sample and the other one is not, take the other. */
          { static int _tp = -1;
            if (_tp < 0) _tp = getenv("TEX_PROBE") ? 1 : 0;
            if (_tp && vm_base && _r) {
                u32 alt = cellGcmResolveLocated((tex->format & 3) != 1, offset);
                if (alt && alt != _r && alt < 0xE0000000u && _r < 0xE0000000u) {
                    u32 span = width * height / 2;
                    if (span > 4096) span = 4096;
                    u32 nz_r = 0, nz_a = 0;
                    for (u32 i = 0; i < span; i += 7) {
                        if (vm_base[_r  + i]) nz_r++;
                        if (vm_base[alt + i]) nz_a++;
                    }
                    if (nz_r == 0 && nz_a > 0) {
                        static u32 seen[32]; static int ns = 0; int known = 0;
                        for (int k = 0; k < ns; k++) if (seen[k] == offset) known = 1;
                        if (!known && ns < 32) { seen[ns++] = offset;
                            fprintf(stderr, "[TEX_PROBE] 0x%08X: %s empty, using %s"
                                            " (%u non-zero)%c", offset,
                                    ((tex->format & 3) == 1) ? "local" : "main",
                                    ((tex->format & 3) == 1) ? "main" : "local",
                                    nz_a, 10); }
                        _r = alt;
                    }
                }
            } }
          s_d3d.cur_texs[unit].off = _r; }
        s_d3d.cur_texs[unit].raw = offset;
        s_d3d.cur_texs[unit].w = width; s_d3d.cur_texs[unit].h = height;
        s_d3d.cur_texs[unit].fmt = format;   /* full byte: LN(0x20)/UN(0x40) kept */
        s_d3d.cur_texs[unit].ctrl1 = tex->control1;
        s_d3d.cur_texs[unit].pitch = tex->control3 & 0xFFFFFu;   /* #173 */
        s_d3d.cur_texs[unit].filter = tex->filter;                /* #176 */
        s_d3d.cur_texs[unit].addr = tex->address;                 /* #190 */
        s_d3d.cur_texs[unit].ctl0 = tex->control0;
        s_d3d.cur_texs[unit].dim  = (tex->format >> 4) & 0xFu;
        s_d3d.cur_texs[unit].cube  = (tex->format & 4) ? 1 : 0;
        s_d3d.cur_texs[unit].mips  = (tex->format >> 16) & 0xFFFFu;
        if (getenv("TEXFMTDBG")) { static u32 seen[64]; static int ns=0;
            u32 k = (unit<<24) | (((tex->format>>8) & 0xFFu)<<16) | (width & 0xFFFFu);
            int f=0; for (int i=0;i<ns;i++) if (seen[i]==k) f=1;
            if (!f && ns < 64) { seen[ns++]=k;
                fprintf(stderr, "[TEXFMT] unit=%u fmt=0x%02X %ux%u mips=%u cube=%d off=0x%X%c",
                        unit, (tex->format >> 8) & 0xFFu, width, height,
                        (tex->format>>16)&0xFFFFu, (tex->format&4)?1:0, offset, 10); } }
        s_d3d.cur_texs[unit].set = 1;
        /* [NHL local patch] NHL_B8_DUMP=<n>: every n-th bind of a wide B8 plane
         * (the VP6 movie's Y/U/V), dump the guest bytes as a grey BMP into the
         * working directory, with control3 (pitch) logged -- decoder output vs
         * display path, answered by eye. Up to 24 dumps. */
        { static int every = -1; static unsigned binds = 0, dumped = 0;
          /* NHL_B8_DUMP_AFTER=<s>: start dumping only after that many seconds of
           * uptime, so the 24 dumps land on a later screen (the front end)
           * instead of being spent on the intro movie. */
          static ULONGLONG s_t0 = 0, s_after_ms = 0;
          static int s_unit = -1;   /* NHL_B8_DUMP_UNIT=<n>: only that texture unit */
          if (every < 0) { const char* e = getenv("NHL_B8_DUMP"); every = e ? atoi(e) : 0;
                           const char* a = getenv("NHL_B8_DUMP_AFTER"); s_after_ms = a ? (ULONGLONG)atoi(a) * 1000ull : 0;
                           const char* u = getenv("NHL_B8_DUMP_UNIT"); s_unit = u ? atoi(u) : -1;
                           s_t0 = GetTickCount64(); }
          if (every > 0 && base_fmt == 0x81 && width >= 320 && dumped < 24 && vm_base &&
              (s_unit < 0 || (int)unit == s_unit) &&
              GetTickCount64() - s_t0 >= s_after_ms &&
              (binds++ % (unsigned)every) == 0) {
              const u32 ea = s_d3d.cur_texs[unit].off;
              const u8* sp = vm_base + ea;
              u8 mn = 255, mx = 0;
              for (u32 i = 0; i < width * height; i += 97) { u8 v = sp[i]; if (v < mn) mn = v; if (v > mx) mx = v; }
              fprintf(stderr, "[B8DUMP] #%u unit=%u ea=0x%08X %ux%u fmt=0x%02X ctrl3=0x%08X min=%u max=%u fp=0x%08X%c",
                      dumped, unit, ea, width, height, format, tex->control3, mn, mx,
                      s_d3d.current_rsx_state ? s_d3d.current_rsx_state->shader_program : 0u, 10);
              if (mx > mn) {
                  char pn[96];
                  snprintf(pn, sizeof pn, "b8_%02u_u%u_%ux%u.bmp", dumped, unit, width, height);
                  FILE* f = fopen(pn, "wb");
                  if (f) {
                      u32 rowb = width * 3, rowp = (rowb + 3) & ~3u, fsz = 54 + rowp * height;
                      u8 hd[54] = {'B','M'};
                      hd[2]=(u8)fsz; hd[3]=(u8)(fsz>>8); hd[4]=(u8)(fsz>>16); hd[5]=(u8)(fsz>>24);
                      hd[10]=54; hd[14]=40;
                      hd[18]=(u8)width; hd[19]=(u8)(width>>8); hd[22]=(u8)height; hd[23]=(u8)(height>>8);
                      hd[26]=1; hd[28]=24;
                      fwrite(hd, 1, 54, f);
                      for (int y = (int)height - 1; y >= 0; y--) {
                          const u8* row = sp + (u64)y * width;
                          for (u32 x = 0; x < width; x++) { u8 px[3] = {row[x], row[x], row[x]}; fwrite(px, 1, 3, f); }
                          u8 z[3] = {0, 0, 0}; fwrite(z, 1, rowp - rowb, f);
                      }
                      fclose(f);
                  }
              }
              dumped++;
          } }
    }
    if (base_fmt == 0x81 /* B8 */) {
        s_d3d.tex_src_offset = cellGcmResolveLocated((tex->format & 3) == 1, offset);
        if (s_d3d.tex_w != width || s_d3d.tex_h != height) {
            /* dims changed -> resource must be (re)created in render_frame */
            s_d3d.tex_ready = 0;
        }
        s_d3d.tex_w     = width;
        s_d3d.tex_h     = height;
        s_d3d.tex_bound = 1;
        s_d3d.tex_dirty = 1;
    } else {
        s_d3d.tex_bound = 0;
    }
}

static void d3d12_set_vertex_attribs(void* ud, const rsx_state* state)
{
    (void)ud;
    s_d3d.current_rsx_state = state;

    /* Log enabled vertex attributes for debugging. RSX_VTXDBG=<N> raises the
     * cap: the fixed 5 were all consumed by boot-time setup, so the layouts the
     * real draws use were never printed. */
    static int log_count = 0, log_cap = -1;
    if (log_cap < 0) { const char* e = getenv("RSX_VTXDBG"); log_cap = e ? atoi(e) : 5; }
    if (log_count < log_cap) {
        printf("[D3D12] set_vertex_attribs:\n");
        for (int i = 0; i < 16; i++) {
            const rsx_vertex_attrib* a = &state->vertex_attribs[i];
            if (a->enabled) {
                const char* type_name = "?";
                switch (a->type) {
                case 1: type_name = "snorm16"; break;
                case 2: type_name = "float32"; break;
                case 3: type_name = "float16"; break;
                case 4: type_name = "ubyte"; break;
                case 5: type_name = "s16"; break;
                case 7: type_name = "ubyte256"; break;
                }
                printf("  attrib[%d]: %s x%u, stride=%u, offset=0x%X\n",
                       i, type_name, a->size, a->stride, a->offset);
            }
        }
        log_count++;
    }
}

static void d3d12_set_shader(void* ud, const rsx_state* state)
{
    (void)ud;
    s_d3d.current_rsx_state = state;

    static int log_count = 0;
    if (log_count < 5) {
        printf("[D3D12] set_shader: fp_addr=0x%08X, vp_load=%u, output_mask=0x%08X\n",
               state->fragment_program_addr, state->transform_program_load,
               state->vertex_attrib_output_mask);
        log_count++;
    }

    /* TODO: Look up or compile a PSO matching this shader combination.
     * For now we use the basic vertex-colored PSO for everything. */
}

static void d3d12_set_blend(void* ud, const rsx_state* state)
{
    (void)ud;
    /* TODO: modify PSO blend state or use dynamic state.
     * D3D12 requires PSO recreation for blend state changes,
     * so we'd need a PSO cache keyed by blend configuration. */
    static int log_count = 0;
    if (log_count < 5) {
        printf("[D3D12] set_blend(enable=%d, sfactor=0x%X, dfactor=0x%X)\n",
               state->blend_enable, state->blend_sfactor, state->blend_dfactor);
        log_count++;
    }
}

static void d3d12_set_depth_stencil(void* ud, const rsx_state* state)
{
    (void)ud;
    static int log_count = 0;
    if (log_count < 5) {
        printf("[D3D12] set_depth_stencil(depth=%d, stencil=%d, func=0x%X)\n",
               state->depth_test_enable, state->stencil_test_enable,
               state->depth_func);
        log_count++;
    }
}

/* ---------------------------------------------------------------------------
 * Backend registration
 * -----------------------------------------------------------------------*/

static rsx_backend s_d3d12_backend = {0};

/* ---------------------------------------------------------------------------
 * Public API
 * -----------------------------------------------------------------------*/

int rsx_d3d12_backend_init(u32 width, u32 height, const char* title)
{
    memset(&s_d3d, 0, sizeof(s_d3d));
    s_d3d.width = width;
    s_d3d.height = height;
    s_d3d.clear_color[0] = 0.0f;
    s_d3d.clear_color[1] = 0.0f;
    s_d3d.clear_color[2] = 0.1f;
    s_d3d.clear_color[3] = 1.0f;

    /* Debug: dump the first N presented frames to BMP if CELLMARK_DUMP is set
     * (its numeric value when > 1, else 24). */
    {
        const char* dv = getenv("CELLMARK_DUMP");
        int n = dv ? atoi(dv) : 0;
        s_d3d.dump_frames_left = dv ? (n > 1 ? n : 24) : 0;
        /* CELLMARK_DUMP_SKIP=N: ignore the first N presents before dumping.
         * The opening frames are the loading screen; the interesting content
         * only appears once the sim SPUs have advanced the scene. */
        { const char* sv = getenv("CELLMARK_DUMP_SKIP");
          s_d3d.dump_skip_left = sv ? atoi(sv) : 0; }
    }

    /* Create window */
    {
        extern char g_rsx_title_base[128];
        snprintf(g_rsx_title_base, sizeof(g_rsx_title_base), "%s",
                 title ? title : "ps3recomp");
    }
    s_d3d.hwnd = create_window(width, height, title);
    /* [#120] diagnostic: system-wide F9. [#184] also with RSX_CAP_DIR: in fullscreen the
     * focused-window F9 never arrived (play252/253, no capture). */
    if (s_d3d.hwnd && (getenv("CAP_GLOBAL_F9") || getenv("RSX_CAP_DIR"))) {
        BOOL ok = RegisterHotKey(s_d3d.hwnd, 0x4E48, 0x4000 /* MOD_NOREPEAT */, VK_F9);
        printf("[CAPKEY] global F9 hotkey %s\n", ok ? "registered" : "FAILED");
    }
    if (s_d3d.hwnd && getenv("NHL_LIVEKEYS")) {   /* #189b: system-wide F5 = shadow-map units lit on/off */
        BOOL ok = RegisterHotKey(s_d3d.hwnd, 0x4E4A, 0x4000 /* MOD_NOREPEAT */, VK_F5);
        fprintf(stderr, "[LIVEKEY] global F5 hotkey %s\n", ok ? "registered" : "FAILED");
        ok = RegisterHotKey(s_d3d.hwnd, 0x4E4B, 0x4000 /* MOD_NOREPEAT */, VK_F4);   /* #214 */
        fprintf(stderr, "[LIVEKEY] global F4 hotkey %s\n", ok ? "registered" : "FAILED");
        ok = RegisterHotKey(s_d3d.hwnd, 0x4E4C, 0x4000 /* MOD_NOREPEAT */, VK_F3);   /* #216 */
        fprintf(stderr, "[LIVEKEY] global F3 hotkey %s\n", ok ? "registered" : "FAILED");
    }
    if (s_d3d.hwnd && getenv("HITCH")) {   /* #122: system-wide F8 = "felt a slowdown" */
        BOOL ok = RegisterHotKey(s_d3d.hwnd, 0x4E49, 0x4000 /* MOD_NOREPEAT */, VK_F8);
        fprintf(stderr, "[HITCH] global F8 marker hotkey %s\n", ok ? "registered" : "FAILED");
    }
    if (!s_d3d.hwnd) {
        printf("[D3D12] ERROR: Window creation failed\n");
        return -1;
    }

    /* Initialize D3D12 */
    if (init_d3d12(width, height) != 0) {
        printf("[D3D12] ERROR: D3D12 initialization failed\n");
        return -1;
    }

    if (s_sc_on && getenv("NHL_FULLSCREEN") && atoi(getenv("NHL_FULLSCREEN"))) sc_toggle_fullscreen(s_d3d.hwnd);   /* #163 */

    /* Set up backend callbacks */
    s_d3d12_backend.userdata          = &s_d3d;
    { extern int (*g_gcm_gpu_blit_hook)(u32, u32, u32, u32, u32, u32, u32, u32, u32, u32, u32, u32);
      g_gcm_gpu_blit_hook = rsx_d3d12_gpu_blit; }   /* #85 */
    { extern u32 (*g_gcm_batch_draws_fn)(void); g_gcm_batch_draws_fn = d3d12_batch_draws; }   /* #167 */
    { extern void (*g_gcm_label_hook)(void); g_gcm_label_hook = wb_sync_hook; }   /* #169 */
    { extern void (*g_gcm_blit_src_hook)(u32, u32); g_gcm_blit_src_hook = wb_blit_src; }   /* #218 */
    s_d3d12_backend.init              = d3d12_init;
    s_d3d12_backend.shutdown          = d3d12_shutdown;
    s_d3d12_backend.begin_frame       = d3d12_begin_frame;
    s_d3d12_backend.end_frame         = d3d12_end_frame;
    s_d3d12_backend.present           = d3d12_present;
    s_d3d12_backend.clear             = d3d12_clear;
    s_d3d12_backend.set_render_target = d3d12_set_render_target;
    s_d3d12_backend.set_viewport      = d3d12_set_viewport;
    s_d3d12_backend.set_blend         = d3d12_set_blend;
    s_d3d12_backend.set_depth_stencil = d3d12_set_depth_stencil;
    s_d3d12_backend.set_shader        = d3d12_set_shader;
    s_d3d12_backend.set_vertex_attribs = d3d12_set_vertex_attribs;
    s_d3d12_backend.draw_arrays       = d3d12_draw_arrays;
    s_d3d12_backend.draw_indexed      = d3d12_draw_indexed;
    s_d3d12_backend.bind_texture      = d3d12_bind_texture;

    rsx_set_backend(&s_d3d12_backend);

    s_d3d.initialized = 1;
    s_d3d.last_fps_time = GetTickCount64();

    printf("[D3D12] Backend ready: %ux%u\n", width, height);
    return 0;
}

void rsx_d3d12_backend_shutdown(void)
{
    if (!s_d3d.initialized) return;

    pp_issue();   /* #164 */
    wait_for_gpu();

    /* Release D3D12 resources */
    if (s_d3d.vertex_buffer) {
        s_d3d.vertex_buffer->lpVtbl->Unmap(s_d3d.vertex_buffer, 0, NULL);
        s_d3d.vertex_buffer->lpVtbl->Release(s_d3d.vertex_buffer);
    }
    if (s_d3d.pipeline_state)        s_d3d.pipeline_state->lpVtbl->Release(s_d3d.pipeline_state);
    if (s_d3d.pipeline_state_lines)  s_d3d.pipeline_state_lines->lpVtbl->Release(s_d3d.pipeline_state_lines);
    if (s_d3d.pipeline_state_points) s_d3d.pipeline_state_points->lpVtbl->Release(s_d3d.pipeline_state_points);
    if (s_d3d.depth_buffer) s_d3d.depth_buffer->lpVtbl->Release(s_d3d.depth_buffer);
    if (s_d3d.depth_buffer2) s_d3d.depth_buffer2->lpVtbl->Release(s_d3d.depth_buffer2);
    if (s_d3d.dsv_heap)     s_d3d.dsv_heap->lpVtbl->Release(s_d3d.dsv_heap);
    if (s_d3d.root_signature) s_d3d.root_signature->lpVtbl->Release(s_d3d.root_signature);
    if (s_d3d.fence) s_d3d.fence->lpVtbl->Release(s_d3d.fence);
    if (s_d3d.fence_event) CloseHandle(s_d3d.fence_event);
    if (s_d3d.cmd_list) s_d3d.cmd_list->lpVtbl->Release(s_d3d.cmd_list);
    for (u32 i = 0; i < FRAME_COUNT; i++) {
        if (s_d3d.cmd_allocators[i]) s_d3d.cmd_allocators[i]->lpVtbl->Release(s_d3d.cmd_allocators[i]);
        if (s_d3d.render_targets[i]) s_d3d.render_targets[i]->lpVtbl->Release(s_d3d.render_targets[i]);
    }
    if (s_d3d.rtv_heap) s_d3d.rtv_heap->lpVtbl->Release(s_d3d.rtv_heap);
    if (s_d3d.swap_chain) s_d3d.swap_chain->lpVtbl->Release(s_d3d.swap_chain);
    if (s_d3d.cmd_queue) s_d3d.cmd_queue->lpVtbl->Release(s_d3d.cmd_queue);
    if (s_d3d.device) s_d3d.device->lpVtbl->Release(s_d3d.device);

    if (s_d3d.hwnd) DestroyWindow(s_d3d.hwnd);

    rsx_set_backend(NULL);
    s_d3d.initialized = 0;

    printf("[D3D12] Backend shut down\n");
}

int rsx_d3d12_backend_pump_messages(void)
{
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) return -1;
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return s_d3d.window_closed ? -1 : 0;
}

void rsx_d3d12_backend_present(void)
{
    if (blink_dbg())
        printf("[PRESENT] draws=%u clears_since_last=%u clear_presents=%u\n",
               s_d3d.draw_count, s_dbg_clears_since_present, s_clear_presents);
    s_dbg_clears_since_present = 0;

    /* Once frame-boundary presents are active (d3d12_clear presents each
     * completed frame as the drain crosses into the next one), the ticker
     * present would only ever show the partially-accumulated NEXT frame --
     * that partial present right after the FIFO ring recycle was the visible
     * blink. Keep the ticker present solely as the boot-time fallback (before
     * the first framed clear arrives). */
    if (s_clear_presents > 0 && !present_on_flip())   /* #131: the flip IS the frame boundary */
        return;

    /* Same display gate as d3d12_present: a batch of offscreen pass work only
     * (render-to-texture) keeps accumulating until its composite arrives.
     * Empty batches present only until the first real frame -- after that an
     * empty present is a flip/drain race and wipes the screen for a frame
     * (wave: black flashes and layout flicker between frames). */
    static int s_seen_content = 0;
    int has_display = (s_d3d.draw_count == 0 && !s_seen_content);
    for (u32 _i = 0; _i < s_d3d.draw_count && _i < MAX_DRAWS; _i++)
        if (!s_d3d.draws[_i].is_clear && s_d3d.draws[_i].rt_off == 0) {
            has_display = 1;
            break;
        }
    /* The guest-framebuffer present needs the pass to run at all. */
    { static int gfb = -1;
      if (gfb < 0) { const char* e = getenv("GCM_GUEST_FB"); gfb = e ? 1 : 0; }
      if (gfb) has_display = 1; }
    if (s_composite_src) has_display = 1;
    if (has_display && s_d3d.draw_count > 0) s_seen_content = 1;

    /* VP_SUBMIT=<N>: has_display gates render_frame() entirely, so a batch whose
     * records all target an OFFSCREEN rt (rt_off != 0) presents without ever
     * running the draw pass -- the backbuffer then shows only the clear, which
     * reads as "nothing rasterizes" from every downstream check. */
    { static int cap = -1, n = 0;
      if (cap < 0) { const char* e = getenv("VP_SUBMIT"); cap = e ? atoi(e) : 0; }
      if (cap && n < cap && s_d3d.draw_count) { n++;
        u32 onscreen = 0, offscreen = 0, clears = 0;
        for (u32 _i = 0; _i < s_d3d.draw_count && _i < MAX_DRAWS; _i++) {
            if (s_d3d.draws[_i].is_clear) { clears++; continue; }
            if (s_d3d.draws[_i].rt_off) offscreen++; else onscreen++;
        }
        { /* Which surfaces the batch's draws actually target -- "offscreen=57"
           * does not say whether they all go to one RT or scatter. */
          u32 offs[8]; u32 cnt[8]; int nd = 0;
          for (u32 _i = 0; _i < s_d3d.draw_count && _i < MAX_DRAWS; _i++) {
              if (s_d3d.draws[_i].is_clear) continue;
              u32 o = s_d3d.draws[_i].rt_off; int f = -1;
              for (int _j = 0; _j < nd; _j++) if (offs[_j] == o) { f = _j; break; }
              if (f < 0) { if (nd < 8) { offs[nd] = o; cnt[nd] = 1; nd++; } }
              else cnt[f]++;
          }
          { u32 vp = 0, novp = 0;
            for (u32 _i = 0; _i < s_d3d.draw_count && _i < MAX_DRAWS; _i++) {
                if (s_d3d.draws[_i].is_clear) continue;
                if (s_d3d.draws[_i].is_vp) vp++; else novp++;
            }
            /* The execution loop skips anything without is_vp, so a batch full
             * of !is_vp records is counted, gated and then never drawn. */
            fprintf(stderr, "[PRESENTGATE] is_vp=%u not_vp=%u%c", vp, novp, 10); }
          fprintf(stderr, "[PRESENTGATE] targets:");
          for (int _j = 0; _j < nd; _j++)
              fprintf(stderr, " 0x%08X x%u", offs[_j], cnt[_j]);
          fprintf(stderr, "%c", 10); }
        fprintf(stderr, "[PRESENTGATE] records=%u onscreen=%u offscreen=%u clears=%u"
                        " has_display=%d seen_content=%d -> render_frame=%s\n",
                s_d3d.draw_count, onscreen, offscreen, clears, has_display,
                s_seen_content, (s_d3d.initialized && has_display) ? "YES" : "SKIPPED"); } }

    /* Execute the batch whenever it has draws. Gating the whole call on
     * has_display meant a render-to-texture pass -- every draw targeting an
     * offscreen surface -- was DISCARDED rather than deferred, so the texture
     * it produces was never written and whatever sampled it later read an
     * empty resource. Only the Present needs onscreen content. */
    { extern void gcm_ftl(const char*, u32, u32, u32); extern u32 cellGcmGetCurrentDisplayBufferId(void);
      gcm_ftl("bpres", s_d3d.draw_count, (u32)has_display, cellGcmGetCurrentDisplayBufferId()); }   /* #167 */
    /* [NHL local patch #178] movie display meter (set by the VP6 bridge with NHL_VP6_SHOW=1):
     * which guest movie frame index each presented flip shows, per movie size. Per second:
     * steps 0 (repeat), 1 (ok), 2, 3+ (skips), back -- jitter on screen, not just in publishing. */
    { extern int (*g_vp6_frame_of_hook)(u32 ea, u32* wh);
      if (g_vp6_frame_of_hook && has_display) {
          static struct { u32 wh; int last, cur; u32 n0, n1, n2, n3, nb; char seq[64]; u32 sl; } mv[4];
          static ULONGLONG t0; ULONGLONG t = GetTickCount64(); if (!t0) t0 = t;
          /* NHL_VP6_SHOWDBG=<w>: the first 120 presents' list of drawn movie frame indices of
           * that width, with each draw's Y-plane address (which ring slot) */
          { static int sd = -1, sn = 0; if (sd < 0) { const char* e = getenv("NHL_VP6_SHOWDBG"); sd = e ? atoi(e) : 0; }
            if (sd > 0 && sn < 120) { char line[512]; int o = 0, any = 0;
                for (u32 i = 0; i < s_d3d.draw_count && i < MAX_DRAWS && o < 480; i++) {
                    const D3D12DrawRecord* dr = &s_d3d.draws[i];
                    if (dr->is_clear || dr->vp6_idx < 0 || (dr->vp6_wh >> 16) != (u32)sd) continue;
                    u32 y = 0; for (int u = 0; u < D3D_TEX_UNITS; u++) if (dr->tex[u].set && (dr->tex[u].fmt & 0x9Fu) == 0x81u && dr->tex[u].w == (u32)sd) { y = dr->tex[u].off; break; }
                    o += snprintf(line + o, sizeof line - o, " %d@%08X", dr->vp6_idx, y); any = 1; }
                if (any) { sn++; fprintf(stderr, "[vp6showdbg] present:%s%c", line, 10); } } }
          for (u32 i = 0; i < s_d3d.draw_count && i < MAX_DRAWS; i++) {
              const D3D12DrawRecord* dr = &s_d3d.draws[i];
              if (dr->is_clear || dr->vp6_idx < 0) continue;   /* #179: index captured at record time */
              const u32 wh = dr->vp6_wh; const int idx = dr->vp6_idx;
              int k = -1; for (int j = 0; j < 4; j++) if (mv[j].wh == wh) { k = j; break; }
              if (k < 0) for (int j = 0; j < 4; j++) if (!mv[j].wh) { mv[j].wh = wh; mv[j].last = -1; mv[j].cur = -1; k = j; break; }
              if (k >= 0 && idx > mv[k].cur) mv[k].cur = idx;
          }
          for (int j = 0; j < 4; j++) {
              if (!mv[j].wh || mv[j].cur < 0) continue;
              if (mv[j].last >= 0) {
                  const int d = mv[j].cur - mv[j].last;
                  char c = d == 0 ? '0' : d == 1 ? '1' : d == 2 ? '2' : d > 2 ? '3' : 'B';
                  if (d == 0) mv[j].n0++; else if (d == 1) mv[j].n1++; else if (d == 2) mv[j].n2++; else if (d > 2) mv[j].n3++; else mv[j].nb++;
                  if (mv[j].sl < 63) { mv[j].seq[mv[j].sl++] = c; mv[j].seq[mv[j].sl] = 0; }
              }
              mv[j].last = mv[j].cur; mv[j].cur = -1;
          }
          if (t - t0 >= 1000) { t0 = t;
              for (int j = 0; j < 4; j++) if (mv[j].wh && mv[j].sl) {
                  fprintf(stderr, "[vp6show] %ux%u steps: repeat %u, +1 %u, +2 %u, +3+ %u, back %u | %s%c",
                          mv[j].wh >> 16, mv[j].wh & 0xFFFFu, mv[j].n0, mv[j].n1, mv[j].n2, mv[j].n3, mv[j].nb, mv[j].seq, 10);
                  mv[j].n0 = mv[j].n1 = mv[j].n2 = mv[j].n3 = mv[j].nb = 0; mv[j].sl = 0; mv[j].seq[0] = 0; }
          }
      } }
    /* [NHL local patch #165] NHL_PRESENTSTAT=1: what each guest flip brings -- presented
     * (display draws), offscreen-only (executed, not presented) or empty -- with the flipped
     * display buffer id, per second, plus the last 24 flips as id:draws:kind@ms-since-prev. */
    { static int ps = -1; if (ps < 0) { const char* e = getenv("NHL_PRESENTSTAT"); ps = (e && *e != '0') ? 1 : 0; }
      if (ps) {
          extern u32 cellGcmGetCurrentDisplayBufferId(void);
          static u32 n_pres, n_off, n_empty, n_same; static u32 last_id = 0xFFFFFFFFu;
          static char ring[24][24]; static u32 rn; static ULONGLONG t0; static LARGE_INTEGER qf, qprev;
          if (!qf.QuadPart) QueryPerformanceFrequency(&qf);
          LARGE_INTEGER q; QueryPerformanceCounter(&q);
          const double dms = qprev.QuadPart ? (double)(q.QuadPart - qprev.QuadPart) * 1000.0 / (double)qf.QuadPart : 0.0;
          qprev = q;
          const u32 id = cellGcmGetCurrentDisplayBufferId();
          const char kind = (s_d3d.draw_count == 0) ? 'E' : has_display ? 'P' : 'O';
          if (kind == 'P') n_pres++; else if (kind == 'O') n_off++; else n_empty++;
          if (id == last_id) n_same++;
          last_id = id;
          snprintf(ring[rn++ % 24], 24, "%u:%u:%c@%.1f", id, s_d3d.draw_count, kind, dms);
          const ULONGLONG t = GetTickCount64(); if (!t0) t0 = t;
          if (t - t0 >= 1000) {
              char seq[24 * 24 + 1] = ""; size_t o = 0;
              for (u32 k = 0; k < 24 && k < rn; k++) {
                  const char* s = ring[(rn - (rn < 24 ? rn : 24) + k) % 24];
                  o += (size_t)snprintf(seq + o, sizeof seq - o, " %s", s); if (o >= sizeof seq) break; }
              fprintf(stderr, "[PRESENTSTAT] flips %u: presented %u offscreen-only %u empty %u same-buffer-as-prev %u |%s%c",
                      n_pres + n_off + n_empty, n_pres, n_off, n_empty, n_same, seq, 10);
              n_pres = n_off = n_empty = n_same = 0; t0 = t;
          } } }
    if (s_d3d.initialized && (has_display || s_d3d.draw_count > 0)) {
        { extern void rsx_reset_upload_claims(void);
          static int remap = -1;
          if (remap < 0) { const char* e = getenv("TEX_REMAP"); remap = e ? atoi(e) : 0; }
          if (remap) rsx_reset_upload_claims(); }
        s_present_this_frame = has_display;
        render_frame();
        s_present_this_frame = 1;
    }
}

#else /* !_WIN32 */

#include <ps3emu/ps3types.h>   /* u32 (header includes above are inside the _WIN32 guard) */
#include <stdio.h>

/* Stub for non-Windows — D3D12 is Windows-only */
int rsx_d3d12_backend_init(u32 w, u32 h, const char* t)
{
    (void)w; (void)h; (void)t;
    printf("[D3D12] Not available on this platform (use Vulkan backend)\n");
    return -1;
}
void rsx_d3d12_backend_shutdown(void) {}
int rsx_d3d12_backend_pump_messages(void) { return 0; }
void rsx_d3d12_backend_present(void) {}

#endif /* _WIN32 */
