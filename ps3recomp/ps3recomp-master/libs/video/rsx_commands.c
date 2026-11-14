/*
 * ps3recomp - RSX Command Buffer Processor
 *
 * Parses NV47xx GPU methods from the command buffer and updates RSX state.
 * Dispatches to the registered graphics backend for actual rendering.
 *
 * Command buffer format (NV47xx FIFO):
 *   Each command is a 32-bit header followed by N data words.
 *   Header format: [31:29] type | [28:18] count | [17:13] subchannel | [12:2] method | [1:0] flags
 *
 *   Type 0 (increasing): method, method+4, method+8, ... for each data word
 *   Type 2 (non-increasing): same method repeated for each data word
 *   Type 1 (jump): jump to address in data
 *   Type 3 (call/return): call/return from subroutine
 */

#include "rsx_commands.h"
#include "ps3emu/milestone.h"   /* ps3_ms -- boot milestone log */
#include <stdio.h>
#include <stdlib.h>   /* getenv -- an implicit decl returns int, truncating the pointer */
#include <string.h>
#include "../../runtime/memory/vm.h"    /* VM_HLE_INJECT_BASE */

/* ---------------------------------------------------------------------------
 * Global backend
 * -----------------------------------------------------------------------*/

static rsx_backend* s_backend = NULL;

/* Last NV406E_SET_REFERENCE value seen in the FIFO. The title writes a
 * reference then spins on the GCM control register's `ref` field until the RSX
 * reports it as reached; cellGcmSys mirrors this into the guest control
 * register after each drain so those waits (cellGcmFinish / wait-label) unblock. */
u32 g_rsx_last_reference = 0;

/* [NHL local patch #74] RSX_FPRINT: per-frame hash of the vertex-constant
 * uploads; the D3D12 backend reads the env, prints and resets it each frame. */
int g_rsx_fprint_on = -1;
u64 g_rsx_fprint_hash = 1469598103934665603ull;
u32 g_rsx_fprint_n = 0;

void rsx_set_backend(rsx_backend* backend)
{
    s_backend = backend;
}

rsx_backend* rsx_get_backend(void)
{
    return s_backend;
}

/* ---------------------------------------------------------------------------
 * State initialization
 * -----------------------------------------------------------------------*/

void rsx_state_init(rsx_state* state)
{
    memset(state, 0, sizeof(rsx_state));
    /* RSX reset value for the constant vertex attributes is (0,0,0,1); a zeroed
     * struct would leave w at 0. */
    for (int _i = 0; _i < RSX_MAX_VERTEX_ATTRIBS; _i++)
        state->vertex_data4f[_i][3] = 1.0f;

    /* Default viewport */
    state->viewport_w = 1280;
    state->viewport_h = 720;
    state->clip_min = 0.0f;
    state->clip_max = 1.0f;

    /* Default scissor */
    state->scissor_w = 4096;
    state->scissor_h = 4096;

    /* Default depth */
    state->depth_func = 1; /* LESS */
    state->depth_mask = 1;

    /* Default cull */
    state->cull_face = 0x0405; /* BACK */
    /* [NHL local patch #109] The RSX resets FRONT_FACE to CCW (0x0901), the GL
     * default (RPCS3 initialises the register the same way). It was 0 here,
     * which the backend reads as CW: a title that never writes FRONT_FACE (NHL)
     * then had every culled mesh's FRONT faces culled -- the ice and the players
     * vanished. RSX_FRONT_DEFAULT_CW=1 restores the old default. */
    state->front_face = getenv("RSX_FRONT_DEFAULT_CW") ? 0x0900u : 0x0901u;

    /* Default color mask: all channels writable (A|R|G|B) */
    state->color_mask = 0x01010101;

    /* Default stencil: sensible initial values */
    state->stencil_func = 0x0207; /* ALWAYS */
    state->stencil_ref = 0;
    state->stencil_mask = 0xFF;
    state->stencil_op_fail = 0x1E00;  /* KEEP */
    state->stencil_op_zfail = 0x1E00; /* KEEP */
    state->stencil_op_zpass = 0x1E00; /* KEEP */

    /* Default alpha test */
    state->alpha_func = 0x0207; /* ALWAYS */
    state->alpha_ref = 0;

    /* Default shader control: 32-bit colour exports (r0..) -- matches every
     * title that never programs the register. */
    state->shader_control = CELL_GCM_SHADER_CONTROL_32_BITS_EXPORTS;

    /* Mark everything dirty */
    state->surface_dirty = 1;
    state->viewport_dirty = 1;
    state->blend_dirty = 1;
    state->depth_dirty = 1;
    state->stencil_dirty = 1;
    state->texture_dirty = 1;
    state->vertex_dirty = 1;
    state->color_mask_dirty = 1;
    state->alpha_dirty = 1;
    state->shader_dirty = 1;
}

/* ---------------------------------------------------------------------------
 * Method processing
 * -----------------------------------------------------------------------*/

static int process_surface_method(rsx_state* state, u32 method, u32 data)
{
    switch (method) {
    case NV4097_SET_SURFACE_FORMAT:
        state->surface_format = data;
        state->surface_dirty = 1;
        return 0;
    case NV4097_SET_SURFACE_CLIP_HORIZONTAL:
        state->surface_clip_x = data & 0xFFFF;
        state->surface_clip_w = (data >> 16) & 0xFFFF;
        state->surface_dirty = 1;
        return 0;
    case NV4097_SET_SURFACE_CLIP_VERTICAL:
        state->surface_clip_y = data & 0xFFFF;
        state->surface_clip_h = (data >> 16) & 0xFFFF;
        state->surface_dirty = 1;
        return 0;
    case NV4097_SET_SURFACE_COLOR_AOFFSET:
        state->surface_color_offset[0] = data;
        { static int _s = -1; if (_s < 0) _s = getenv("SURFDBG") ? 1 : 0;
          static unsigned _seen[32]; static int _n = 0;
          if (_s) { int f = 0; for (int k = 0; k < _n; k++) if (_seen[k] == data) f = 1;
              if (!f && _n < 32) { _seen[_n++] = data;
                  fprintf(stderr, "[SURF] color offset -> 0x%08X%c", data, 10); } } }
        state->surface_dirty = 1;
        return 0;
    case NV4097_SET_SURFACE_COLOR_BOFFSET:
        state->surface_color_offset[1] = data;
        state->surface_dirty = 1;
        return 0;
    case NV4097_SET_SURFACE_COLOR_COFFSET:
        state->surface_color_offset[2] = data;
        state->surface_dirty = 1;
        return 0;
    case NV4097_SET_SURFACE_COLOR_DOFFSET:
        state->surface_color_offset[3] = data;
        state->surface_dirty = 1;
        return 0;
    case NV4097_SET_SURFACE_ZETA_OFFSET:
        state->surface_zeta_offset = data;
        state->surface_dirty = 1;
        return 0;
    case NV4097_SET_SURFACE_COLOR_TARGET:
        { static int _ct=0; if (_ct++ < 24 && getenv("RTT_DUMP"))
            fprintf(stderr, "[RSXCT] color_target=0x%X offA=0x%X offB=0x%X offC=0x%X offD=0x%X%s",
                    data, state->surface_color_offset[0], state->surface_color_offset[1],
                    state->surface_color_offset[2], state->surface_color_offset[3], "\n"); }
        state->color_target = data;
        state->color_target_set = 1;
        state->surface_dirty = 1;
        return 0;
    case NV4097_SET_SURFACE_PITCH_A:
        state->surface_color_pitch[0] = data;
        return 0;
    case NV4097_SET_SURFACE_PITCH_B:
        state->surface_color_pitch[1] = data;
        return 0;
    case NV4097_SET_SURFACE_PITCH_C:
        state->surface_color_pitch[2] = data;
        return 0;
    case NV4097_SET_SURFACE_PITCH_D:
        state->surface_color_pitch[3] = data;
        return 0;
    case NV4097_SET_SURFACE_PITCH_Z:
        state->surface_zeta_pitch = data;
        return 0;
    default:
        return -1;
    }
}

/* ---------------------------------------------------------------------------
 * Texture method processing
 *
 * Texture registers are laid out in 16 units, each spanning 0x20 bytes:
 *   Unit 0: 0x1A00..0x1A1C
 *   Unit 1: 0x1A20..0x1A3C
 *   ...
 *   Unit N: 0x1A00 + N*0x20 .. 0x1A1C + N*0x20
 * -----------------------------------------------------------------------*/

static int process_texture_method(rsx_state* state, u32 method, u32 data)
{
    /* Compute texture unit index and register offset within the unit */
    u32 base = method - NV4097_SET_TEXTURE_OFFSET; /* 0x1A00 */
    u32 unit = base / 0x20;
    u32 reg  = base % 0x20;

    if (unit >= RSX_MAX_TEXTURES)
        return -1;

    rsx_texture_state* tex = &state->textures[unit];

    switch (reg) {
    case 0x00: /* TEXTURE_OFFSET */
        tex->offset = data;
        break;
    case 0x04: /* TEXTURE_FORMAT */
        tex->format = data;
        break;
    case 0x08: /* TEXTURE_ADDRESS (wrap S/T/R) */
        tex->address = data;
        break;
    case 0x0C: /* TEXTURE_CONTROL0 (enable, min/max LOD, max aniso) */
        tex->control0 = data;
        break;
    case 0x10: /* TEXTURE_CONTROL1 (remap) */
        tex->control1 = data;
        break;
    case 0x14: /* TEXTURE_FILTER (bias, min/mag filter) */
        tex->filter = data;
        break;
    case 0x18: /* TEXTURE_IMAGE_RECT (width << 16 | height) */
        tex->image_rect = data;
        break;
    case 0x1C: /* TEXTURE_BORDER_COLOR */
        tex->border_color = data;
        break;
    default:
        return -1;
    }

    tex->dirty = 1;
    state->texture_dirty = 1;
    return 0;
}

/* Vertex texture unit block: 0x0900 + unit*0x20, eight words. Same order as a
 * fragment unit's except at +0x10, which is CONTROL3 (the row pitch) rather
 * than CONTROL1 (the component crossbar) -- a vertex unit has no crossbar. */
static int process_vertex_texture_method(rsx_state* state, u32 method, u32 data)
{
    u32 base = method - NV4097_SET_VERTEX_TEXTURE_OFFSET;
    u32 unit = base / 0x20;
    u32 reg  = base % 0x20;

    if (unit >= RSX_MAX_VERTEX_TEXTURES)
        return -1;

    rsx_texture_state* tex = &state->vertex_textures[unit];

    switch (reg) {
    case 0x00: tex->offset       = data; break;
    case 0x04: tex->format       = data; break;
    case 0x08: tex->address      = data; break;
    case 0x0C: tex->control0     = data; break;
    case 0x10: tex->control3     = data; break;   /* CONTROL1's slot */
    case 0x14: tex->filter       = data; break;
    case 0x18: tex->image_rect   = data; break;
    case 0x1C: tex->border_color = data; break;
    default:
        return -1;
    }

    tex->dirty = 1;
    state->texture_dirty = 1;
    return 0;
}

/* SET_TEXTURE_CONTROL3: 0x1840 + unit*4, one word per unit rather than a slot
 * in the 0x20-byte unit block. */
static int process_texture_control3(rsx_state* state, u32 method, u32 data)
{
    u32 unit = (method - NV4097_SET_TEXTURE_CONTROL3) / 4;
    if (unit >= RSX_MAX_TEXTURES)
        return -1;
    state->textures[unit].control3 = data;
    state->textures[unit].dirty = 1;
    state->texture_dirty = 1;
    return 0;
}

/* ---------------------------------------------------------------------------
 * Vertex attribute method processing
 *
 * FORMAT registers: 0x1740 + attrib*4  (16 attributes)
 * OFFSET registers: 0x1680 + attrib*4  (16 attributes)
 * -----------------------------------------------------------------------*/

static int process_vertex_attrib_method(rsx_state* state, u32 method, u32 data)
{
    if (method >= NV4097_SET_VERTEX_DATA_ARRAY_FORMAT &&
        method < NV4097_SET_VERTEX_DATA_ARRAY_FORMAT + RSX_MAX_VERTEX_ATTRIBS * 4) {
        u32 index = (method - NV4097_SET_VERTEX_DATA_ARRAY_FORMAT) / 4;
        rsx_vertex_attrib* attr = &state->vertex_attribs[index];

        /*
         * Format register layout:
         *   [3:0]   type   (float, half, ubyte, short, etc.)
         *   [7:4]   size   (number of components: 1-4)
         *   [15:8]  stride (bytes between consecutive elements)
         *   [16]    enable
         */
        attr->type      = data & 0xF;
        attr->size      = (data >> 4) & 0xF;
        attr->stride    = (data >> 8) & 0xFF;
        attr->frequency = (data >> 16) & 0xFFFF;   /* instancing divisor */
        /* SIZE (the component count) is what enables the attribute: NV4097 uses
         * size == 0 to mean "this array is off". Keying off `type` instead marked
         * every unused slot enabled-with-zero-components, because PSGL writes a
         * bare type (e.g. 0x00000002 = float32, size 0, stride 0) into the slots
         * it is NOT using -- so the input layout was built from 16 attributes of
         * which most were degenerate, and the draws rasterized nothing. */
        attr->enabled   = (attr->size != 0);
        attr->format    = data;
        state->vertex_dirty = 1;
        return 0;
    }

    if (method >= NV4097_SET_VERTEX_DATA_ARRAY_OFFSET &&
        method < NV4097_SET_VERTEX_DATA_ARRAY_OFFSET + RSX_MAX_VERTEX_ATTRIBS * 4) {
        u32 index = (method - NV4097_SET_VERTEX_DATA_ARRAY_OFFSET) / 4;
        state->vertex_attribs[index].offset = data;
        state->vertex_dirty = 1;
        return 0;
    }

    /* [NHL local patch #103] anti-aliasing control (alpha-to-coverage); was ignored. */
    if (method == 0x1D7C) {
        state->aa_control = data;
        { static u32 seen[16]; static int ns = 0; int f = 0;
          for (int i = 0; i < ns; i++) if (seen[i] == data) f = 1;
          if (!f && ns < 16) { seen[ns++] = data;
              fprintf(stderr, "[AACTL] 0x%08X (msaa=%u a2c=%u a2one=%u mask=0x%04X)%c", data,
                      data & 1u, (data >> 4) & 1u, (data >> 8) & 1u, data >> 16, 10); } }
        return 0;
    }

    /* [NHL local patch #208] NV4097_SET_SHADER_PACKER: framebuffer sRGB output (was ignored). */
    if (method == 0x1FEC) {
        state->shader_packer = data;
        { static u32 seen[8]; static int ns = 0; int f = 0;
          for (int i = 0; i < ns; i++) if (seen[i] == data) f = 1;
          if (!f && ns < 8) { seen[ns++] = data; fprintf(stderr, "[PACKER] 0x%08X%c", data, 10); } }
        return 0;
    }

    /* [NHL local patch #101] vertex base offset / base index (were ignored). */
    if (method == 0x1738 || method == 0x173C) {
        if (method == 0x1738) state->vertex_base_offset = data;
        else                  state->vertex_base_index  = data;
        state->vertex_dirty = 1;
        { static int n = 0; if (data && n < 12) { n++;
            fprintf(stderr, "[VBBASE] %s = 0x%X%c", method == 0x1738 ? "BASE_OFFSET" : "BASE_INDEX", data, 10); } }
        return 0;
    }

    return -1;
}

/* ---------------------------------------------------------------------------
 * Main method dispatch
 * -----------------------------------------------------------------------*/

/* [NHL local patch #63] Coalesce DRAW_INDEX_ARRAY words. Each word carries at
 * most 256 indices, so a mesh arrives as a run of words with contiguous index
 * ranges -- and 256 is not a multiple of 3, so a backend that handles whole
 * triangle lists saw every piece as a partial batch (play42: 22,900 calls,
 * 5.2M per-index vertex conversions per 20 frames). Contiguous words with the
 * same primitive are joined and handed to the backend as one draw; the pending
 * draw is flushed before any other method (state changes, BEGIN_END end).
 * RSX_IDX_COALESCE=0 disables. */
static int s_idx_pend = 0;
static u32 s_idx_pend_prim, s_idx_pend_first, s_idx_pend_count;
static rsx_state* s_idx_pend_state;
static void rsx_idx_flush(void)
{
    if (!s_idx_pend) return;
    s_idx_pend = 0;
    ps3_ms("rsx:draw_indexed");
    if (s_backend && s_backend->draw_indexed)
        s_backend->draw_indexed(s_backend->userdata, s_idx_pend_prim,
                                s_idx_pend_first, s_idx_pend_count);
    (void)s_idx_pend_state;
}
/* [NHL local patch #133] The same for DRAW_ARRAYS: a plain triangle list
 * arrives as 256-vertex words, and only whole lists can take the backend's GPU
 * vertex fetch (a word ending mid-triangle had to be glued to the next on the
 * CPU). RSX_ARR_COALESCE=0 disables. */
static int s_arr_pend = 0;
static u32 s_arr_pend_prim, s_arr_pend_first, s_arr_pend_count;
static rsx_state* s_arr_pend_state;
static void rsx_arr_flush(void)
{
    if (!s_arr_pend) return;
    s_arr_pend = 0;
    ps3_ms("rsx:draw_arrays");
    if (s_backend && s_backend->draw_arrays)
        s_backend->draw_arrays(s_backend->userdata, s_arr_pend_prim,
                               s_arr_pend_first, s_arr_pend_count);
}

int rsx_process_method(rsx_state* state, u32 method, u32 data)
{
    if (s_idx_pend && method != NV4097_DRAW_INDEX_ARRAY) rsx_idx_flush();
    if (s_arr_pend && method != NV4097_DRAW_ARRAYS) rsx_arr_flush();
    /* RSX_TRACE=<N>: trace the first N methods (bare "1" keeps the old 250).
     * The fixed 250 was spent entirely on boot-time setup, so the methods around
     * the first real draw -- exactly the ones worth seeing -- were never traced. */
    { static int _rt=-1; if(_rt<0){ const char* e=getenv("RSX_TRACE");
        _rt = e ? (atoi(e) > 1 ? atoi(e) : 250) : 0; }
      if(_rt){ static int _m=0; if(_m++<_rt) fprintf(stderr,"[rsxm] method=0x%04X data=0x%08X\n", method, data); } }
    /* Back-end write label / semaphore (cellGcmSetWriteBackEndLabel): the RSX
     * writes a value to a report/label the CPU polls for CPU<->RSX sync (double
     * buffering). Real hardware DOES this; without it the game's frame-fence
     * loop (func_0006E6E0 polling label 0x41 @ 0x03000410) spins forever and
     * never reaches render. NV4097_SET_SEMAPHORE_OFFSET(0x1D6C)=offset (index*0x10
     * into the GCM label window @0x03000000); NV4097_BACK_END_WRITE_SEMAPHORE_
     * RELEASE(0x1D70)=value. Also NV406E semaphore release (0x0010 offset/0x0014
     * value) for the sub-channel path. */
    /* NV4097_SET_FREQUENCY_DIVIDER_OPERATION: per-attribute modulo/divide mask
     * for the frequency divisor (instancing). Captured for read_vp_vertex. */
    if (method == 0x00001FC0) { state->frequency_divider_op = data; return 0; }

    /* [NHL local patch #322 diag] Does NHL use state the D3D12 backend ignores? Polygon offset
     * (0xA60/A64/A68 enables, 0xA78 scale, 0xA7C bias), user clip planes (0x1478), depth clamp
     * (0x1D78; upstream #562/#537/#693). Logs each method's first 6 value changes. Not consumed. */
    if ((method >= 0x0A60 && method <= 0x0A68) || method == 0x0A78 || method == 0x0A7C ||
        method == 0x1478 || method == 0x1D78) {
        if (method == 0x0A68) state->poly_offset_fill = data;    /* #323: consumed by depth-only PSOs */
        if (method == 0x0A78) state->poly_offset_scale = data;
        if (method == 0x0A7C) state->poly_offset_bias = data;
        static u32 s_last[8] = { 0xDEADBEEFu, 0xDEADBEEFu, 0xDEADBEEFu, 0xDEADBEEFu,
                                 0xDEADBEEFu, 0xDEADBEEFu, 0xDEADBEEFu, 0xDEADBEEFu };
        static int s_n[8];
        const int i = method == 0x0A60 ? 0 : method == 0x0A64 ? 1 : method == 0x0A68 ? 2 :
                      method == 0x0A78 ? 3 : method == 0x0A7C ? 4 : method == 0x1478 ? 5 : 6;
        if (data != s_last[i] && s_n[i] < 6) {
            static const char* nm[7] = { "POLY_OFFSET_POINT_ENABLE", "POLY_OFFSET_LINE_ENABLE",
                "POLY_OFFSET_FILL_ENABLE", "POLYGON_OFFSET_SCALE", "POLYGON_OFFSET_BIAS",
                "USER_CLIP_PLANE_CONTROL", "ZMIN_MAX_CONTROL" };
            float f; memcpy(&f, &data, 4);
            s_n[i]++;
            fprintf(stderr, "[NHL-RSXSTATE] %s (0x%04X) = 0x%08X (%g)%s\n", nm[i], method, data, (double)f,
                    s_n[i] == 6 ? " [last log for this method]" : "");
        }
        s_last[i] = data;
    }

    { static u32 s_sem_off = 0;
      if (method == 0x1D6C) { s_sem_off = data; return 0; }
      if (method == 0x1D70) {
        extern void vm_write32(uint32_t a, uint32_t v);
        /* The RSX back-end semaphore write swaps bytes 0<->2 of the value (hw
         * quirk); libgcm's cellGcmSetWriteBackEndLabel PRE-swaps to compensate
         * (SDK gcm_implementation_sub.h: "// swap byte 0 and 2"). Writing the
         * FIFO value verbatim left the label byte-swapped, so gcmutil's
         * cellGcmUtilFinish poll (label 255 == sLabelVal, starts at 1) compared
         * 0x00010000 != 1 and spun forever (gcm/cube sample hang after init).
         * Apply the same swap the hardware does so the pre-swap cancels out. */
        u32 val = (data & 0xff00ff00u) | ((data >> 16) & 0xffu) | ((data & 0xffu) << 16);
        if ((s_sem_off & 0x00FFFFFFu) >= 0x2000u && (s_sem_off & 0x00FFFFFFu) < 0x2010u) {
            static int _rw = 0; if (_rw++ < 20)   /* [NHL local patch] REFWATCH */
                fprintf(stderr, "[REFWATCH] NV4097 backend label onto ctrl: off=0x%X val=0x%08X%c", s_sem_off, val, 10); }
        { extern void gcm_label_pre_write(void); gcm_label_pre_write(); }   /* #169 */
        vm_write32(VM_HLE_INJECT_BASE + (s_sem_off & 0x00FFFFFFu), val);
        { extern void gcm_note_label_write(u32, u32, u32);   /* [NHL local patch] */
          gcm_note_label_write(s_sem_off & 0xFFFFu, val, 0x1D70); }
        { static int _l=0; if(_l++<8) fprintf(stderr,"[RSX] label write @0x%08X = 0x%08X (sync fence, raw 0x%08X)\n", VM_HLE_INJECT_BASE+(s_sem_off&0xFFFFFF), val, data); }
        return 0;
      }
      if (method == 0x1D74) {
        extern void vm_write32(uint32_t a, uint32_t v);
        if ((s_sem_off & 0x00FFFFFFu) >= 0x2000u && (s_sem_off & 0x00FFFFFFu) < 0x2010u) {
            static int _rw = 0; if (_rw++ < 20)   /* [NHL local patch] REFWATCH */
                fprintf(stderr, "[REFWATCH] NV4097 texture label onto ctrl: off=0x%X val=0x%08X%c", s_sem_off, data, 10); }
        { extern void gcm_label_pre_write(void); gcm_label_pre_write(); }   /* #169 */
        vm_write32(VM_HLE_INJECT_BASE + (s_sem_off & 0x00FFFFFFu), data);
        { extern void gcm_note_label_write(u32, u32, u32);   /* [NHL local patch] */
          gcm_note_label_write(s_sem_off & 0xFFFFu, data, 0x1D74); }
        return 0;
      } }
    if ((method >= 0x200 && method <= 0x23C) ||
        (method >= 0x280 && method <= 0x28C))
        return process_surface_method(state, method, data);

    /* Texture methods: 0x1A00..0x1A00 + 16*0x20 - 1 */
    if (method >= 0x1A00 && method < 0x1A00 + RSX_MAX_TEXTURES * 0x20)
        return process_texture_method(state, method, data);

    /* Texture CONTROL3: 0x1840..0x187C */
    if (method >= NV4097_SET_TEXTURE_CONTROL3 &&
        method < NV4097_SET_TEXTURE_CONTROL3 + RSX_MAX_TEXTURES * 4)
        return process_texture_control3(state, method, data);

    /* Vertex texture units: 0x0900..0x097C */
    if (method >= NV4097_SET_VERTEX_TEXTURE_OFFSET &&
        method < NV4097_SET_VERTEX_TEXTURE_OFFSET + RSX_MAX_VERTEX_TEXTURES * 0x20)
        return process_vertex_texture_method(state, method, data);

    /* [NHL local patch #101/#103] vertex base offset/index and AA control live in
     * process_vertex_attrib_method too; route them there (they were unreachable). */
    if (method == 0x1738 || method == 0x173C || method == 0x1D7C || method == 0x1FEC)   /* #208b: + shader packer */
        return process_vertex_attrib_method(state, method, data);
    /* [NHL local patch #108] shader window (origin / pixel centre), was ignored. */
    if (method == 0x1D88) { state->shader_window = data; return 0; }

    /* Vertex attribute FORMAT: 0x1740..0x177C */
    if (method >= 0x1740 && method < 0x1740 + RSX_MAX_VERTEX_ATTRIBS * 4)
        return process_vertex_attrib_method(state, method, data);

    /* Vertex attribute OFFSET: 0x1680..0x16BC */
    if (method >= 0x1680 && method < 0x1680 + RSX_MAX_VERTEX_ATTRIBS * 4)
        return process_vertex_attrib_method(state, method, data);

    /* Viewport */
    /* Viewport transform: window = ndc*scale + offset. The z lane is the
     * GL->[0,1] depth remap (offset.z=0.5/scale.z=0.5); without honoring it,
     * GL-convention projections (SDK gcm samples) get their near-camera
     * geometry clipped by D3D's 0<=z<=w rule (gcm/cube: missing polygons). */
    if (method >= 0x0A20 && method < 0x0A30) {
        u32 f = data; float v; memcpy(&v, &f, 4);
        state->viewport_offset[(method - 0x0A20) >> 2] = v;
        return 0;
    }
    if (method >= 0x0A30 && method < 0x0A40) {
        u32 f = data; float v; memcpy(&v, &f, 4);
        state->viewport_scale[(method - 0x0A30) >> 2] = v;
        return 0;
    }

    if (method == NV4097_SET_VIEWPORT_HORIZONTAL) {
        state->viewport_x = data & 0xFFFF;
        state->viewport_w = (data >> 16) & 0xFFFF;
        state->viewport_dirty = 1;
        return 0;
    }
    if (method == NV4097_SET_VIEWPORT_VERTICAL) {
        state->viewport_y = data & 0xFFFF;
        state->viewport_h = (data >> 16) & 0xFFFF;
        state->viewport_dirty = 1;
        return 0;
    }

    /* Color mask */
    if (method == NV4097_SET_COLOR_MASK) {
        state->color_mask = data;
        state->color_mask_dirty = 1;
        return 0;
    }

    /* Alpha test */
    if (method == NV4097_SET_ALPHA_TEST_ENABLE) {
        state->alpha_test_enable = data ? 1 : 0;
        state->alpha_dirty = 1;
        return 0;
    }
    if (method == NV4097_SET_ALPHA_FUNC) {
        state->alpha_func = data;
        state->alpha_dirty = 1;
        return 0;
    }
    if (method == NV4097_SET_ALPHA_REF) {
        state->alpha_ref = data;
        state->alpha_dirty = 1;
        return 0;
    }

    /* Clear */
    if (method == NV4097_SET_COLOR_CLEAR_VALUE) {
        state->color_clear_value = data;
        return 0;
    }
    if (method == NV4097_SET_ZSTENCIL_CLEAR_VALUE) {
        state->zstencil_clear_value = data;
        return 0;
    }
    if (method == NV4097_CLEAR_SURFACE) {
        { static int _c=0; if (_c++ < 12) fprintf(stderr, "[RSX] CLEAR_SURFACE mask=0x%X color=0x%08X\n", data, state->color_clear_value); }
        if (s_backend && s_backend->clear) {
            float depth = (float)(state->zstencil_clear_value >> 8) / (float)0xFFFFFF;
            u8 stencil = state->zstencil_clear_value & 0xFF;
            s_backend->clear(s_backend->userdata, data,
                           state->color_clear_value, depth, stencil);
        }
        return 0;
    }

    /* Blend */
    if (method == NV4097_SET_BLEND_ENABLE) {
        state->blend_enable = data ? 1 : 0;
        state->blend_dirty = 1;
        return 0;
    }
    if (method == NV4097_SET_BLEND_FUNC_SFACTOR) {
        state->blend_sfactor = data;
        state->blend_dirty = 1;
        return 0;
    }
    if (method == NV4097_SET_BLEND_FUNC_DFACTOR) {
        state->blend_dfactor = data;
        state->blend_dirty = 1;
        return 0;
    }
    if (method == NV4097_SET_BLEND_EQUATION) {
        state->blend_equation = data;
        state->blend_dirty = 1;
        return 0;
    }

    /* Depth */
    if (method == NV4097_SET_DEPTH_TEST_ENABLE) {
        state->depth_test_enable = data ? 1 : 0;
        state->depth_dirty = 1;
        return 0;
    }
    if (method == NV4097_SET_DEPTH_FUNC) {
        state->depth_func = data;
        state->depth_dirty = 1;
        return 0;
    }
    if (method == NV4097_SET_DEPTH_MASK) {
        state->depth_mask = data ? 1 : 0;
        state->depth_dirty = 1;
        return 0;
    }

    /* Stencil */
    if (method == NV4097_SET_STENCIL_TEST_ENABLE) {
        state->stencil_test_enable = data ? 1 : 0;
        state->stencil_dirty = 1;
        return 0;
    }
    if (method == NV4097_SET_STENCIL_MASK) {   /* [NHL local patch] write mask */
        state->stencil_write_mask = data & 0xFF;
        state->stencil_write_mask_set = 1;
        state->stencil_dirty = 1;
        return 0;
    }
    if (method == NV4097_SET_STENCIL_FUNC) {
        state->stencil_func = data;
        state->stencil_dirty = 1;
        return 0;
    }
    if (method == NV4097_SET_STENCIL_FUNC_REF) {
        state->stencil_ref = data;
        state->stencil_dirty = 1;
        return 0;
    }
    if (method == NV4097_SET_STENCIL_FUNC_MASK) {
        state->stencil_mask = data;
        state->stencil_dirty = 1;
        return 0;
    }
    if (method == NV4097_SET_STENCIL_OP_FAIL) {
        state->stencil_op_fail = data;
        state->stencil_dirty = 1;
        return 0;
    }
    if (method == NV4097_SET_STENCIL_OP_ZFAIL) {
        state->stencil_op_zfail = data;
        state->stencil_dirty = 1;
        return 0;
    }
    if (method == NV4097_SET_STENCIL_OP_ZPASS) {
        state->stencil_op_zpass = data;
        state->stencil_dirty = 1;
        return 0;
    }

    /* Culling */
    if (method == NV4097_SET_CULL_FACE_ENABLE) {
        state->cull_face_enable = data ? 1 : 0;
        return 0;
    }
    if (method == NV4097_SET_CULL_FACE) {
        state->cull_face = data;
        return 0;
    }
    if (method == NV4097_SET_FRONT_FACE) {
        state->front_face = data;
        { static u32 seen[8]; static int ns = 0; int f = 0;   /* [#109] */
          for (int i = 0; i < ns; i++) if (seen[i] == data) f = 1;
          if (!f && ns < 8) { seen[ns++] = data;
              fprintf(stderr, "[FRONTFACE] guest wrote 0x%04X%c", data, 10); } }
        return 0;
    }

    /* Shader programs */
    if (method == NV4097_SET_SHADER_PROGRAM) {
        /*
         * Fragment program address register:
         *   [1:0]  location (0 = local, 1 = main)
         *   [31:2] offset (4-byte aligned)
         */
        state->shader_program = data;
        state->fragment_program_addr = data & ~0x3u;
        { extern void gcm_evring_add(char, uint32_t, uint32_t, uint32_t); gcm_evring_add('P', data, 0, 0); }   /* #170 */
        state->shader_dirty = 1;
        return 0;
    }
    if (method == NV4097_SET_TRANSFORM_PROGRAM_LOAD) {
        /* Vertex program load slot — instruction index (each = 16 bytes).
         * Following NV4097_SET_TRANSFORM_PROGRAM words fill from here. */
        state->transform_program_load = data;
        state->vp_ucode_write = data * 16;
        state->shader_dirty = 1;
        return 0;
    }

    if (method == NV4097_SET_TRANSFORM_PROGRAM_START) {
        if (state->transform_program_start != data) { state->vp_dirty = 1; state->vp_gen++; }
        state->transform_program_start = data;
        state->shader_dirty = 1;
        return 0;
    }

    /* NV4097_SET_TRANSFORM_PROGRAM[0..31] — a run of 32-bit vertex-program
     * microcode words appended at the current write cursor. Capture them so the
     * backend can decompile the real VP (4 words = one NV40 instruction). */
    if (method >= NV4097_SET_TRANSFORM_PROGRAM &&
        method <  NV4097_SET_TRANSFORM_PROGRAM + 32 * 4) {
        u32 w = state->vp_ucode_write;
        if (w + 4 <= sizeof(state->vp_ucode)) {
            /* data is host-endian already; store as little-endian bytes. */
            state->vp_ucode[w+0] = (u8)(data);
            state->vp_ucode[w+1] = (u8)(data >> 8);
            state->vp_ucode[w+2] = (u8)(data >> 16);
            state->vp_ucode[w+3] = (u8)(data >> 24);
            state->vp_ucode_write = w + 4;
            if (w + 4 > state->vp_ucode_bytes) state->vp_ucode_bytes = w + 4;
            state->vp_dirty = 1; state->vp_gen++;
        }
        return 0;
    }
    if (method == NV4097_SET_VERTEX_ATTRIB_OUTPUT_MASK) {
        state->vertex_attrib_output_mask = data;
        return 0;
    }
    /* NV4097_SET_VERTEX_DATA4F_M (0x1C00): the constant/"current" value for each
     * vertex attribute, 16 attributes x 4 floats. The hardware feeds this to
     * every vertex when the attribute's ARRAY is disabled -- it is not zero.
     *
     * Rubber Ducky leaves attribute 3 (diffuse colour) disabled and sets it here
     * instead; its duck shader computes (lighting * col0) * texture + spec, so a
     * zero col0 multiplies the texture away and the ducks rendered as a dim grey
     * specular term only -- present in the framebuffer, invisible on screen.
     * SET_VERTEX_DATA2F/4UB/2S/4S are the other encodings of the same register
     * file; add them if a title needs them. */
    if (method >= 0x1C00u && method < 0x1C00u + RSX_MAX_VERTEX_ATTRIBS * 16u) {
        u32 idx  = (method - 0x1C00u) >> 4;         /* attribute */
        u32 lane = ((method - 0x1C00u) >> 2) & 3;   /* x/y/z/w   */
        float f; memcpy(&f, &data, 4);
        state->vertex_data4f[idx][lane] = f;
        { static int _d = -1; if (_d < 0) _d = getenv("VDATA_DBG") ? 1 : 0;
          static int _n = 0;
          if (_d && _n < 400) { _n++;
            fprintf(stderr, "[VDATA4F] attr=%u lane=%u = %.4f%c", idx, lane, f, 10); } }
        return 0;
    }

    if (method == NV4097_SET_TRANSFORM_CONSTANT_LOAD) {
        { static int _n=0, _e=-1; if (_e < 0) _e = getenv("LOAD_DBG") ? 1 : 0; if (_e && _n++ < 200)
            fprintf(stderr, "[LOAD] transform_constant_load = %u\n", data); }
        state->transform_constant_load = data;
        return 0;
    }

    /* Registers that sit right after the 32-dword constant window. Taken as
     * constants c(load+8..15) they overwrite whatever was uploaded there. */
    if (method == NV4097_SET_VERTEX_ATTRIB_INPUT_MASK) {
        state->vertex_attrib_input_mask = data;
        return 0;
    }
    if (method == NV4097_SET_TRANSFORM_BRANCH_BITS) {
        state->transform_branch_bits = data;
        return 0;
    }
    if (method == NV4097_INVALIDATE_L2) {
        /* Post-transform/L2 cache invalidate: the host keeps no such cache,
         * nothing to drop. */
        return 0;
    }

    /* NV4097_SET_TRANSFORM_CONSTANT[0..31] — up to 32 dwords (8 vec4s) per
     * command, 0x1F00..0x1F7C. Each register slot writes to a lane of one
     * vertex constant vec4: vec_index = LOAD + (reg_offset/4), lane = reg_offset%4.
     * The data arrives as a host-endian u32; reinterpret the bits as float
     * because the game's intent is "these 32 bits are a float". The hardware
     * does NOT auto-advance LOAD between commands — games re-issue
     * SET_TRANSFORM_CONSTANT_LOAD before each block. */
    if (method >= NV4097_SET_TRANSFORM_CONSTANT &&
        method <  NV4097_SET_TRANSFORM_CONSTANT + NV4097_SET_TRANSFORM_CONSTANT_DWORDS * 4) {
        u32 reg_offset = (method - NV4097_SET_TRANSFORM_CONSTANT) / 4;
        u32 slot = state->transform_constant_load + (reg_offset >> 2);
        u32 lane = reg_offset & 3;
        if (slot < RSX_MAX_VERTEX_CONSTANTS) {
            float f;
            memcpy(&f, &data, 4);
            { static int _en=-1; if(_en<0){const char*e=getenv("TCONST_DBG");_en=e?1:0;}
              static int _n=0;
              static int _max=-1; if(_max<0){const char*m=getenv("TCONST_MAX");_max=m?atoi(m):64;}
              int _hit = _en && (getenv("TCONST_ALL") ? (_n<_max) : (slot>=12 && slot<=30 && _n<400));
              if(_hit){ _n++; fprintf(stderr,"[TCONST] load=%u slot=%u lane=%u = %.4f\n", state->transform_constant_load, slot, lane, f); } }
            state->vertex_constants[slot][lane] = f;
            /* [NHL local patch #74] RSX_FPRINT=1: order-dependent hash of every
             * constant upload in a frame (camera/animation matrices); the backend
             * prints how many distinct frames it saw per second. */
            /* [#86] TCONST_HIST=1: every 2 s, which constant slots were written
             * (ranges) and the last value written to slots 24 and 32. */
            { static int th = -1; static u8 hit[RSX_MAX_VERTEX_CONSTANTS]; static u64 t0 = 0;
              if (th < 0) th = getenv("TCONST_HIST") ? 1 : 0;
              if (th) {
                  hit[slot] = 1;
                  extern unsigned long long ps3_qpc_us(void);
                  const u64 now = ps3_qpc_us();
                  if (!t0) t0 = now;
                  if (now - t0 >= 2000000ull) {
                      char b[1200]; int p = snprintf(b, sizeof b, "[TCONST_HIST] written:");
                      for (int s = 0; s < RSX_MAX_VERTEX_CONSTANTS && p < (int)sizeof b - 24; s++) {
                          if (!hit[s]) continue;
                          int e = s; while (e + 1 < RSX_MAX_VERTEX_CONSTANTS && hit[e + 1]) e++;
                          p += (e > s) ? snprintf(b + p, sizeof b - p, " %d-%d", s, e) : snprintf(b + p, sizeof b - p, " %d", s);
                          s = e;
                      }
                      fprintf(stderr, "[TCONST_HIST] cam c0=(%.4g,%.4g,%.4g,%.4g) c1=(%.4g,%.4g,%.4g,%.4g) c3=(%.4g,%.4g,%.4g,%.4g) c7=(%.4g,%.4g,%.4g,%.4g)\n",
                              state->vertex_constants[0][0], state->vertex_constants[0][1], state->vertex_constants[0][2], state->vertex_constants[0][3],
                              state->vertex_constants[1][0], state->vertex_constants[1][1], state->vertex_constants[1][2], state->vertex_constants[1][3],
                              state->vertex_constants[3][0], state->vertex_constants[3][1], state->vertex_constants[3][2], state->vertex_constants[3][3],
                              state->vertex_constants[7][0], state->vertex_constants[7][1], state->vertex_constants[7][2], state->vertex_constants[7][3]);
                      fprintf(stderr, "%s | c24=(%.3g,%.3g,%.3g,%.3g) c32=(%.3g,%.3g,%.3g,%.3g)\n", b,
                              state->vertex_constants[24][0], state->vertex_constants[24][1],
                              state->vertex_constants[24][2], state->vertex_constants[24][3],
                              state->vertex_constants[32][0], state->vertex_constants[32][1],
                              state->vertex_constants[32][2], state->vertex_constants[32][3]);
                      memset(hit, 0, sizeof hit); t0 = now;
                  }
              } }
            { extern int g_rsx_fprint_on; extern u64 g_rsx_fprint_hash; extern u32 g_rsx_fprint_n;
              if (g_rsx_fprint_on) {
                  g_rsx_fprint_hash = (g_rsx_fprint_hash ^ (((u64)slot << 34) ^ ((u64)lane << 32) ^ data))
                                      * 1099511628211ull;
                  g_rsx_fprint_n++;
              } }
            { static int _sq=0, _e=-1; if (_e < 0) _e = getenv("SEQ_DBG") ? 1 : 0; if (_e && slot==256 && lane==0 && _sq++ < 500)
                fprintf(stderr, "[SEQ] upload c256.x=%.4f (load=%u)\n", f, state->transform_constant_load); }
            if (!state->vertex_constants_dirty) {
                state->vertex_constants_lo = slot;
                state->vertex_constants_hi = slot;
                state->vertex_constants_dirty = 1;
            } else {
                if (slot < state->vertex_constants_lo) state->vertex_constants_lo = slot;
                if (slot > state->vertex_constants_hi) state->vertex_constants_hi = slot;
            }
        }
        return 0;
    }

    /* Draw */
    if (method == NV4097_SET_BEGIN_END) {
        if (data != 0) {
            state->primitive_type = data;
            state->in_begin_end = 1;
            state->begin_epoch++;
            /* [NHL local patch #148] SPU_PUTMAP: which 1 MB regions this draw's
             * vertex arrays come from (offset bit 31 = location MAIN). */
            { extern int g_spu_putmap_on; extern void rsx_vamap_note(uint32_t);
              extern u32 cellGcmResolveLocated(int local, u32 offset);
              if (g_spu_putmap_on)
                  for (int i = 0; i < RSX_MAX_VERTEX_ATTRIBS; i++) {
                      const u32 off = state->vertex_attribs[i].offset;
                      if (state->vertex_attribs[i].enabled)
                          rsx_vamap_note(cellGcmResolveLocated(!(off >> 31), off & 0x7FFFFFFFu));
                  } }

            /* Flush dirty state to backend before drawing */
            if (s_backend) {
                if (state->surface_dirty && s_backend->set_render_target)
                    s_backend->set_render_target(s_backend->userdata, state);
                if (state->viewport_dirty && s_backend->set_viewport)
                    s_backend->set_viewport(s_backend->userdata, state);
                if (state->blend_dirty && s_backend->set_blend)
                    s_backend->set_blend(s_backend->userdata, state);
                if ((state->depth_dirty || state->stencil_dirty) && s_backend->set_depth_stencil)
                    s_backend->set_depth_stencil(s_backend->userdata, state);
                if (state->color_mask_dirty && s_backend->set_color_mask)
                    s_backend->set_color_mask(s_backend->userdata, state);
                if (state->alpha_dirty && s_backend->set_alpha_test)
                    s_backend->set_alpha_test(s_backend->userdata, state);
                if (state->shader_dirty && s_backend->set_shader)
                    s_backend->set_shader(s_backend->userdata, state);
                if (state->vertex_dirty && s_backend->set_vertex_attribs)
                    s_backend->set_vertex_attribs(s_backend->userdata, state);

                /* Bind dirty textures */
                if (state->texture_dirty && s_backend->bind_texture) {
                    for (u32 i = 0; i < RSX_MAX_TEXTURES; i++) {
                        if (state->textures[i].dirty) {
                            s_backend->bind_texture(s_backend->userdata, i, &state->textures[i]);
                            state->textures[i].dirty = 0;
                        }
                    }
                }

                state->surface_dirty = 0;
                state->viewport_dirty = 0;
                state->blend_dirty = 0;
                state->depth_dirty = 0;
                state->stencil_dirty = 0;
                state->color_mask_dirty = 0;
                state->alpha_dirty = 0;
                state->shader_dirty = 0;
                state->vertex_dirty = 0;
                state->texture_dirty = 0;
            }
        } else {
            state->in_begin_end = 0;
        }
        return 0;
    }

    if (method == NV4097_DRAW_ARRAYS) {
        u32 first = data & 0xFFFFFF;
        u32 count = ((data >> 24) & 0xFF) + 1;
        { static int _d=0; if (_d++ < 32) fprintf(stderr, "[RSX] DRAW_ARRAYS prim=%u first=%u count=%u\n", state->primitive_type, first, count); }
        { static int _sq=0, _e=-1; if (_e < 0) _e = getenv("SEQ_DBG") ? 1 : 0; if (_e && _sq++ < 500)
            fprintf(stderr, "[SEQ] DRAW surf0=0x%X c256.x=%.4f c257.y=%.4f\n",
                    state->surface_color_offset[0], state->vertex_constants[256][0],
                    state->vertex_constants[257][1]); }
        /* MVPDBG: dump every non-zero vertex-constant slot the FIRST time a
         * G-buffer draw (surf0=0xCC0000) is dispatched -- the true MVP the GPU
         * will use, read live (no snapshot/parity indirection). */
        static int s_mvp = -1; if (s_mvp < 0) s_mvp = getenv("MVPDBG") ? 1 : 0;
        if (s_mvp && state->surface_color_offset[0] == 0xCC0000) {
            static int _once = 0;
            if (!_once) { _once = 1;
                for (int s = 0; s < RSX_MAX_VERTEX_CONSTANTS; s++) {
                    float* v = state->vertex_constants[s];
                    if (v[0]||v[1]||v[2]||v[3])
                        fprintf(stderr, "[MVP] c[%3d] = %10.4f %10.4f %10.4f %10.4f\n",
                                s, v[0], v[1], v[2], v[3]);
                }
            }
        }
        { static int co = -1; if (co < 0) { const char* e = getenv("RSX_ARR_COALESCE"); co = e ? atoi(e) : 1; }
          if (co) {   /* [NHL local patch #133] */
              if (s_arr_pend && (s_arr_pend_prim != state->primitive_type || s_arr_pend_state != state ||
                                 s_arr_pend_first + s_arr_pend_count != first))
                  rsx_arr_flush();
              if (s_arr_pend) s_arr_pend_count += count;
              else { s_arr_pend = 1; s_arr_pend_prim = state->primitive_type; s_arr_pend_state = state;
                     s_arr_pend_first = first; s_arr_pend_count = count; }
              return 0;
          } }
        ps3_ms("rsx:draw_arrays");
        if (s_backend && s_backend->draw_arrays)
            s_backend->draw_arrays(s_backend->userdata, state->primitive_type, first, count);
        return 0;
    }

    if (method == NV4097_SET_SHADER_CONTROL) {
        { static int _sc=0; if (_sc++ < 12 && getenv("RTT_DUMP"))
            fprintf(stderr, "[RSXSC] shader_control=0x%X\n", data); }
        state->shader_control = data;
        state->shader_dirty = 1;
        return 0;
    }
    if (method == NV4097_SET_INDEX_ARRAY_ADDRESS) {
        state->index_array_offset = data;
        return 0;
    }
    if (method == NV4097_SET_INDEX_ARRAY_DMA) {
        state->index_array_dma = data;
        return 0;
    }
    if (method == NV4097_DRAW_INDEX_ARRAY) {
        /* [23:0] first index, [31:24] count-1 (same packing as DRAW_ARRAYS). */
        u32 first = data & 0xFFFFFF;
        u32 count = ((data >> 24) & 0xFF) + 1;
        { static int _d=0; if (_d++ < 8) fprintf(stderr, "[RSX] DRAW_INDEX_ARRAY prim=%u first=%u count=%u idxoff=0x%X dma=0x%X\n", state->primitive_type, first, count, state->index_array_offset, state->index_array_dma); }
        { static int co = -1; if (co < 0) { const char* e = getenv("RSX_IDX_COALESCE"); co = e ? atoi(e) : 1; }
          if (co) {   /* [NHL local patch #63] */
              if (s_idx_pend && (s_idx_pend_prim != state->primitive_type || s_idx_pend_state != state ||
                                 s_idx_pend_first + s_idx_pend_count != first))
                  rsx_idx_flush();
              if (s_idx_pend) s_idx_pend_count += count;
              else { s_idx_pend = 1; s_idx_pend_prim = state->primitive_type; s_idx_pend_state = state;
                     s_idx_pend_first = first; s_idx_pend_count = count; }
              return 0;
          } }
        ps3_ms("rsx:draw_indexed");
        if (s_backend && s_backend->draw_indexed)
            s_backend->draw_indexed(s_backend->userdata, state->primitive_type,
                                    first, count);
        return 0;
    }

    /* Scissor */
    if (method == NV4097_SET_SCISSOR_HORIZONTAL) {
        state->scissor_x = data & 0xFFFF;
        state->scissor_w = (data >> 16) & 0xFFFF;
        return 0;
    }
    if (method == NV4097_SET_SCISSOR_VERTICAL) {
        state->scissor_y = data & 0xFFFF;
        state->scissor_h = (data >> 16) & 0xFFFF;
        return 0;
    }

    /* NV406E_SET_REFERENCE (0x0050): the title writes a reference value it then
     * spins on (cellGcmFinish / cellGcmSetWaitLabel). Record the latest so the
     * GCM control register can report completion back to the guest and unblock
     * the spin. Class 0 (software) method, no subchannel state needed here. */
    if (method == 0x0050) {
        g_rsx_last_reference = data;
        return 0;
    }

    /* GCM_FLIP_HEAD (0xE920/0xE924): immediate display flip from the FIFO.
     * The draw engine handles the actual present; suppress the unknown log. */
    if (method == 0xE920 || method == 0xE924) return 0;

    /* Unrecognized method -- log in debug builds */
#ifndef NDEBUG
    static int s_unknown_count = 0;
    if (s_unknown_count < 50) {
        printf("[RSX] unknown method 0x%04X = 0x%08X\n", method, data);
        s_unknown_count++;
    }
#endif

    return -1;
}

/* ---------------------------------------------------------------------------
 * Command buffer parsing
 * -----------------------------------------------------------------------*/

int rsx_process_command_buffer(rsx_state* state, const u32* buf, u32 size)
{
    int methods_processed = 0;
    u32 pos = 0;
    u32 count = size / 4; /* size in dwords */

    { static int _c=0; if (count && _c++ < 16) fprintf(stderr, "[RSX] process_cmd_buffer words=%u first_hdr=0x%08X\n", count, buf[0]); }
    while (pos < count) {
        u32 header = buf[pos++];
        u32 type = (header >> 29) & 0x7;

        if (type == 0 || type == 2) {
            /* Increasing or non-increasing method */
            u32 method = (header >> 2) & 0x7FF;
            method <<= 2; /* method addresses are dword-aligned */
            u32 num_data = (header >> 18) & 0x7FF;
            int increasing = (type == 0);

            for (u32 i = 0; i < num_data && pos < count; i++) {
                u32 data = buf[pos++];
                u32 m = increasing ? (method + i * 4) : method;
                rsx_process_method(state, m, data);
                methods_processed++;
            }
        } else if (type == 1) {
            /* Jump — change command buffer read position */
            /* In recomp context, this is handled by the caller */
            break;
        } else {
            /* Unknown type, skip */
            break;
        }
    }

    return methods_processed;
}
