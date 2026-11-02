/*
 * NHL Legacy Edition -- port-specific HLE handlers.
 *
 * Imports the toolkit leaves unregistered fall into ps3_hle_unresolved, which
 * fakes CELL_OK. For an asynchronous initialiser that is the worst answer: the
 * real call returns CELL_OK and later delivers an "initialize result" callback,
 * so a faked OK with no callback leaves the title waiting for an event that
 * never arrives. These two are the custom-soundtrack (XMB music) services; a
 * clean failure makes the game run without user music, which is what an
 * offline PS3 with no music would also give it.
 *
 * Handlers follow ps3_hle_call's convention: r3..r10 in, r3 out.
 * NHL_FAKE_USERMUSIC=1 skips the registration (back to the faked CELL_OK).
 */

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <intrin.h>   /* #234 _Interlocked* */

extern "C" void ps3_hle_register(uint32_t nid, const char* name, void* handler);

namespace {

constexpr uint64_t CELL_SEARCH_ERROR_GENERIC       = 0x8002C8FFu;
constexpr uint64_t CELL_MUSIC_DECODE_ERROR_GENERIC = 0x8002C1FFu;

uint64_t nhl_cellSearchInitialize(uint64_t, uint64_t, uint64_t, uint64_t,
                                  uint64_t, uint64_t, uint64_t, uint64_t)
{
    std::fprintf(stderr, "[nhl-hle] cellSearchInitialize -> CELL_SEARCH_ERROR_GENERIC (no user music)\n");
    return CELL_SEARCH_ERROR_GENERIC;
}

uint64_t nhl_cellMusicDecodeInitialize2(uint64_t, uint64_t, uint64_t, uint64_t,
                                        uint64_t, uint64_t, uint64_t, uint64_t)
{
    std::fprintf(stderr, "[nhl-hle] cellMusicDecodeInitialize2 -> CELL_MUSIC_DECODE_ERROR_GENERIC (no user music)\n");
    return CELL_MUSIC_DECODE_ERROR_GENERIC;
}

} // namespace

/* Heap-corruption reporter, called from a hand-added check at the top of the
 * lifted func_00C9AF88 (the game allocator's free-chunk unlink: +4 size|flags,
 * +8/+C free-list fd/bk, +10/+14 size-bin links). A chunk whose fd/bk do not
 * point back at it is not on any free list although its header says free --
 * the unlink then writes through NULL. Dumps the chunk, its neighbour and the
 * guest call chain for the first NHL_HEAPCHK_MAX (default 4) bad chunks. */
#include "ppu_context.h"
extern "C" uint8_t* vm_base;
extern "C" void ppu_dump_guest_stack(ppu_context*, const char*);

/* Apt mask-definition depth on the render thread: the render-list walker
 * (func_00989138) sees type-0x0D entries with +0x78 = +1 / -1 around the
 * shapes that define a mask. The unclaimed-image-fill skip in func_0160C4B0
 * must not drop those shapes -- they are drawn colour-masked into stencil, and
 * without them the masked content (e.g. the main menu's scrolling description
 * box) is clipped away entirely. */
extern "C" { int g_nhl_apt_mask_depth = 0; }

static uint32_t nhl_rd32(uint32_t ea)
{
    const uint8_t* p = vm_base + ea;
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/* [NHL local patch #88] NHL_VPC_PROBE=1: who uploads the vertex constants that
 * hold the light/shadow projection (c24..c32 -- all zero in the match, which
 * blacks out the scene's lighting lookups). Called at the entry of the game's
 * two constant emitters (hand hooks in recompiled/ppu_recomp_126.cpp):
 *   which 1: func_00E0127C(ctx r3, first reg r4, vec4 count r5, data r6)
 *   which 2: func_00E01E00(ctx r3, CgBinaryParameter* r4, data r5) --
 *            param +0 type, +4 resource, +0xC register index.
 * Logs (capped) the register, the first row of data, the caller and, for the
 * first few, the guest stack. */
extern "C" void nhl_vpc_probe(ppu_context* ctx, int which)
{
    static int on = -1;
    if (on < 0) { const char* e = std::getenv("NHL_VPC_PROBE"); on = e ? std::atoi(e) : 0; }
    if (!on) return;
    uint32_t first, count, data, type = 0, res = 0;
    if (which == 1) { first = (uint32_t)ctx->gpr[4]; count = (uint32_t)ctx->gpr[5]; data = (uint32_t)ctx->gpr[6]; }
    else {
        const uint32_t prm = (uint32_t)ctx->gpr[4];
        if (!prm) return;
        type = nhl_rd32(prm + 0); res = nhl_rd32(prm + 4); first = nhl_rd32(prm + 0xC);
        count = (type >= 0x419 && type <= 0x428) ? 4u : 1u;   /* CG_FLOATaxb matrices: up to 4 regs */
        data = (uint32_t)ctx->gpr[5];
    }
    if (first > 32 || first + count <= 24 || !data) return;
    static volatile long n = 0, nstack = 0;
    if (n >= 200) return;
    n++;
    float f[4] = {0, 0, 0, 0};
    for (int i = 0; i < 4; i++) { uint32_t w = nhl_rd32(data + 4u * i); std::memcpy(&f[i], &w, 4); }
    std::fprintf(stderr, "[VPC] via %s first=%u count=%u type=0x%X res=0x%X data=0x%08X row0=(%g,%g,%g,%g) lr=0x%08X tid=%u\n",
                 which == 1 ? "E0127C" : "E01E00", first, count, type, res, data, f[0], f[1], f[2], f[3],
                 (uint32_t)ctx->lr, (unsigned)ctx->thread_id);
    if (nstack < 6) { nstack++; ppu_dump_guest_stack(ctx, which == 1 ? "vpc-block" : "vpc-param"); }
}

/* [NHL local patch #194] NHL_LIGHTCOPY=1: the player light job (SPU pc 0x052AC) reads its
 * light matrices from a 336-byte GPU-visible block (0x4D04D020 in play273/274) that is all
 * zero in the warmup. Only the PPU writes it: upload helper func_0074B474 allocates it
 * (func_0073C04C) and copies the caller's buffer in (func_016C88D8). Hand hook after that
 * copy ([NHL-LIGHTCOPY], recompiled/ppu_recomp_065.cpp): for copies whose destination
 * overlaps NHL_LIGHTCOPY_DST (default 4D04D020, 336 bytes), log source, size, caller, the
 * source's non-zero word count and first floats -- first 40 + one line per second. */
extern "C" unsigned long long ps3_ms_now(void);
extern "C" void nhl_lightcopy_probe(ppu_context* ctx, uint32_t dst, uint32_t src, uint32_t n, uint32_t caller)
{
    static int on = -1; static uint32_t lo = 0;
    if (on < 0) { const char* e = std::getenv("NHL_LIGHTCOPY"); on = e ? 1 : 0;
                  const char* d = std::getenv("NHL_LIGHTCOPY_DST"); lo = d ? (uint32_t)std::strtoul(d, 0, 16) : 0x4D04D020u; }
    if (!on || !n || dst >= lo + 336u || dst + n <= lo || !src) return;
    static volatile long cnt = 0; static unsigned long long t_last = 0;
    const unsigned long long now = ps3_ms_now();
    if (cnt >= 40 && now - t_last < 1000) return;
    cnt++; t_last = now;
    uint32_t nz = 0; float f[4] = {0, 0, 0, 0};
    for (uint32_t i = 0; i + 4 <= n && i < 1024; i += 4) { const uint32_t w = nhl_rd32(src + i); if (w) nz++; }
    for (int i = 0; i < 4; i++) { uint32_t w = nhl_rd32(src + 0x10 + 4u * i); std::memcpy(&f[i], &w, 4); }
    std::fprintf(stderr, "[LIGHTCOPY] dst=0x%08X src=0x%08X n=%u src_nz=%u/%u row(+0x10)=(%g,%g,%g,%g) caller=0x%08X tid=%u\n",
                 dst, src, n, nz, n / 4, f[0], f[1], f[2], f[3], caller, (unsigned)ctx->thread_id);
    static volatile long nstack = 0;
    if (nstack < 4) { nstack++; ppu_dump_guest_stack(ctx, "lightcopy"); }
}

/* [NHL local patch #195] NHL_LIGHTRIG=1: func_014CC268(out r3, rig r4, .., r6) builds the
 * player light matrices into its out buffer (then uploaded by func_0074B474, #194). Path:
 * rig byte +0xBE4 != 0 -> loop over 4 light slots (rig + 0x3A0 + i*96, bit 7 = active)
 * calling func_014CB9D0; else rig byte +0x120 != 0 -> a default light; else nothing (the
 * empty buffer the warmup gets, play275). Hand hook at its entry ([NHL-LIGHTRIG],
 * recompiled/ppu_recomp_204.cpp). Once per second: rig, the two bytes, each slot's flag
 * byte and the 8 words before it. */
extern "C" void ppu_ww_retarget(uint32_t ww);
extern "C" void nhl_lightrig_probe(ppu_context* ctx)
{
    static int on = -1;
    if (on < 0) { const char* e = std::getenv("NHL_LIGHTRIG"); on = e ? 1 : 0; }
    if (!on) return;
    static unsigned long long t_last = 0;
    const unsigned long long now = ps3_ms_now();
    if (now - t_last < 1000) return;
    t_last = now;
    const uint32_t rig = (uint32_t)ctx->gpr[4];
    if (!rig) { std::fprintf(stderr, "[LIGHTRIG] rig=0\n"); return; }
    /* #196 NHL_LIGHTRIG_WATCH=<slot 0-3>: aim PPU_WWATCH at that slot's flag byte (the
     * rig is a heap object that moves run to run), re-aimed if the rig moves. */
    {
        static int ws = -2;
        if (ws == -2) { const char* e = std::getenv("NHL_LIGHTRIG_WATCH"); ws = e ? std::atoi(e) & 3 : -1; }
        static uint32_t armed_rig = 0;
        if (ws >= 0 && rig != armed_rig) {
            armed_rig = rig;
            ppu_ww_retarget(rig + 0x3A0u + (uint32_t)ws * 96u);
        }
    }
    const uint32_t b_be4 = vm_base[rig + 0xBE4], b_120 = vm_base[rig + 0x120];
    char s[900]; int p = 0;
    for (int i = 0; i < 4; i++) {
        const uint32_t ent = rig + 0x3A0u + (uint32_t)i * 96u;
        p += std::snprintf(s + p, sizeof s - p, " | L%d flag=%02X", i, vm_base[ent]);
        for (int k = -8; k < 0; k++) p += std::snprintf(s + p, sizeof s - p, " %08X", nhl_rd32(ent + 4u * k));
    }
    std::fprintf(stderr, "[LIGHTRIG] out=0x%08X rig=0x%08X be4=%02X b120=%02X%s\n",
                 (uint32_t)ctx->gpr[3], rig, b_be4, b_120, s);
}

/* [NHL local patch #197] NHL_LIGHTLIST=1: the two rig light updaters. func_0075B270(rig r3,
 * list r4) and func_0075DA34(rig r3, .., list r8) set be4 from the light set, then walk a
 * circular list (node+0 next, node+8 light object) and make one virtual call per light
 * (obj->vt[0xC]) -- that call sets the slot flag (play279: 0x80 via lr 0x0075B300). Once a
 * second per updater: calls since last print, rig, list length and the first lights' fns. */
extern "C" void nhl_lightlist_probe(ppu_context* ctx, int which)
{
    static int on = -1;
    if (on < 0) { const char* e = std::getenv("NHL_LIGHTLIST"); on = e ? 1 : 0; }
    if (!on) return;
    static volatile long calls[3];
    static unsigned long long t_last[3];
    calls[which]++;
    const unsigned long long now = ps3_ms_now();
    if (now - t_last[which] < 1000) return;
    t_last[which] = now;
    const uint32_t rig = (uint32_t)ctx->gpr[3];
    const uint32_t head = (uint32_t)(which == 1 ? ctx->gpr[4] : ctx->gpr[8]);
    char s[256]; int p = 0, n = 0;
    uint32_t node = head ? nhl_rd32(head) : 0;
    while (node && node != head && n < 32) {
        if (n < 4) {
            const uint32_t obj = nhl_rd32(node + 8);
            const uint32_t vt = obj ? nhl_rd32(obj) : 0;
            const uint32_t fd = vt ? nhl_rd32(vt + 0xC) : 0;
            p += std::snprintf(s + p, sizeof s - p, " obj=%08X fn=%08X", obj, fd ? nhl_rd32(fd) : 0);
        }
        node = nhl_rd32(node);
        n++;
    }
    std::fprintf(stderr, "[LIGHTLIST] %s calls=%ld rig=0x%08X head=0x%08X n=%d%s lr=0x%08X\n",
                 which == 1 ? "B270" : "DA34", calls[which], rig, head, n, s, (uint32_t)ctx->lr);
    calls[which] = 0;
    /* #197b: the slots are copied from light set rig[0x390] + rig[0xBF0]*0x850 by
     * func_00761C00 (4 lights, stride 0x60, flag byte first; +0x840 time, +0x844 -> be4).
     * Print the index, and the whole table's slot flags when the index changes. */
    if (which == 1 && rig) {
        const uint32_t base = nhl_rd32(rig + 0x390), idx = nhl_rd32(rig + 0xBF0);
        static uint32_t last_idx = 0xFFFFFFFFu, last_base = 0;
        /* #197c NHL_LIGHTSET_WATCH=<entry 0-2>: aim PPU_WWATCH at that light-set entry's
         * slot 0 flag -- the producer that fills the table every frame. */
        {
            static int we = -2;
            if (we == -2) { const char* e = std::getenv("NHL_LIGHTSET_WATCH"); we = e ? std::atoi(e) % 3 : -1; }
            static uint32_t armed = 0;
            if (we >= 0 && base && base != armed) { armed = base; ppu_ww_retarget(base + (uint32_t)we * 0x850u); }
        }
        const uint32_t ent = base + idx * 0x850u;
        std::fprintf(stderr, "[LIGHTSET] base=0x%08X idx=%u (+0x394=%08X +0xBF4=%08X +0xBF8=%08X) ent=0x%08X"
                             " flags=%02X %02X %02X %02X t=%08X b844=%02X col0=%08X %08X %08X\n",
                     base, idx, nhl_rd32(rig + 0x394), nhl_rd32(rig + 0xBF4), nhl_rd32(rig + 0xBF8), ent,
                     vm_base[ent], vm_base[ent + 0x60], vm_base[ent + 0xC0], vm_base[ent + 0x120],
                     nhl_rd32(ent + 0x840), vm_base[ent + 0x844],
                     nhl_rd32(ent + 0x10), nhl_rd32(ent + 0x14), nhl_rd32(ent + 0x18));
        if (idx != last_idx || base != last_base) {
            last_idx = idx; last_base = base;
            char t[1024]; int q = 0;
            for (uint32_t k = 0; k < 24 && base; k++) {
                const uint32_t e = base + k * 0x850u;
                q += std::snprintf(t + q, sizeof t - q, " [%u]%02X%02X%02X%02X/%02X", k, vm_base[e],
                                   vm_base[e + 0x60], vm_base[e + 0xC0], vm_base[e + 0x120], vm_base[e + 0x844]);
            }
            std::fprintf(stderr, "[LIGHTSET] table:%s\n", t);
        }
    }
}

/* [NHL local patch #198] NHL_LIGHTPARAM=1: func_007617F0 (main thread) fills the light-set
 * entry from a parameter record: gate = [0x0224683C]->vt[0x34]() != 0, record =
 * func_00C1EDF4(name @0x0237F3AC) != 0, then func_00C1C400(record, 0x10A + i*0x64 ...) per light
 * (bit 7 of the first = slot flag 0x80). Hooks after each call ([NHL-LIGHTPARAM],
 * ppu_recomp_066.cpp). Once a second: how far it got and the values. */
extern "C" void rsx_d3d12_cap_request(void);
extern "C" void nhl_lightparam_probe(ppu_context* ctx, int stage)
{
    static int on = -1;
    if (on < 0) { const char* e = std::getenv("NHL_LIGHTPARAM"); on = e ? 1 : 0; }
    if (!on) return;
    static volatile long cnt[4];
    static uint32_t last[4];
    static unsigned long long t_last = 0;
    if (stage == 3 && ctx->gpr[26] != 0) return;   /* light 0 only */
    cnt[stage]++;
    last[stage] = (uint32_t)ctx->gpr[3];
    /* #198b NHL_LIGHTPARAM_WATCH=1: func_00C1C400 reads param id at rec + 0x10 + id*4 (int, or a
     * float rounded when descriptor [0x0237EEB0 + id*4]+0x18 is 3/4). Aim PPU_WWATCH at light 0's
     * 0x10A to find the writer; re-aim if the record moves. */
    if (stage == 2 && ctx->gpr[3]) {
        static int w = -1;
        if (w < 0) w = std::getenv("NHL_LIGHTPARAM_WATCH") ? 1 : 0;
        static uint32_t armed = 0;
        const uint32_t rec = (uint32_t)ctx->gpr[3];
        if (w && rec != armed) {
            armed = rec;
            const uint32_t ti = nhl_rd32(0x0237EEB0u + 0x10Au * 4u);
            std::fprintf(stderr, "[LIGHTPARAM] watch rec=0x%08X param 0x10A @0x%08X = %08X desc=0x%08X type=%u hdr %08X %08X %08X %08X\n",
                         rec, rec + 0x10u + 0x428u, nhl_rd32(rec + 0x10u + 0x428u), ti, ti ? nhl_rd32(ti + 0x18u) : 0,
                         nhl_rd32(rec), nhl_rd32(rec + 4), nhl_rd32(rec + 8), nhl_rd32(rec + 0xC));
            ppu_ww_retarget(rec + 0x10u + 0x428u);
        }
    }
    /* #199 NHL_LIGHTPARAM_CAP=<max>: request an RSX capture burst (as F9) 1.5 s after light 0's
     * enable changes, and every 5 s while it holds -- frames from each NIS scene, lit or not,
     * without the user timing F9. Needs RSX_CAP_DIR. */
    if (stage == 3) {
        static int cmax = -1, ncap = 0;
        static uint32_t prev = 0xFFFFFFFFu;
        static unsigned long long due = 0;
        if (cmax < 0) { const char* e = std::getenv("NHL_LIGHTPARAM_CAP"); cmax = e ? std::atoi(e) : 0; }
        if (cmax > 0 && ncap < cmax) {
            const unsigned long long now = ps3_ms_now();
            if (last[3] != prev) { prev = last[3]; due = now + 1500; }
            if (due && now >= due) {
                due = now + 5000; ncap++;
                std::fprintf(stderr, "[LIGHTPARAM] auto capture %d (light0=%u)\n", ncap, prev);
                rsx_d3d12_cap_request();
            }
        }
    }
    if (stage != 1) return;
    const unsigned long long now = ps3_ms_now();
    if (now - t_last < 1000) return;
    t_last = now;
    static int named = 0;
    if (!named) {
        named = 1;
        char nm[64]; int k = 0;
        for (; k < 63; k++) { const uint8_t c = vm_base[0x0237F3ACu + k]; if (!c) break; nm[k] = (c >= 32 && c < 127) ? (char)c : '.'; }
        nm[k] = 0;
        const uint32_t g = nhl_rd32(0x0224683Cu), vt = g ? nhl_rd32(g) : 0, fd = vt ? nhl_rd32(vt + 0x34) : 0;
        std::fprintf(stderr, "[LIGHTPARAM] name@0237F3AC=\"%s\" gate obj=0x%08X fn=0x%08X\n", nm, g, fd ? nhl_rd32(fd) : 0);
    }
    std::fprintf(stderr, "[LIGHTPARAM] calls gate=%ld (last %08X) lookup=%ld (rec %08X) light0=%ld (val %08X)\n",
                 cnt[1], last[1], cnt[2], last[2], cnt[3], last[3]);
    cnt[1] = cnt[2] = cnt[3] = 0;
}

extern "C" void nhl_heap_bad(ppu_context* ctx, uint32_t ck)
{
    static volatile long s_n = 0;
    static int s_max = -1;
    if (s_max < 0) { const char* e = std::getenv("NHL_HEAPCHK_MAX"); s_max = e ? std::atoi(e) : 4; }
    if (s_n++ >= s_max) return;
    uint32_t hdr = nhl_rd32(ck + 4), fd = nhl_rd32(ck + 8), bk = nhl_rd32(ck + 0xC);
    uint32_t nx = ck + (hdr & 0x3FFFFFF8u);
    char tag[96];
    std::snprintf(tag, sizeof tag, "heap-bad#%ld tid=%llu ck=0x%08X", (long)s_n,
                  (unsigned long long)ctx->thread_id, ck);
    std::fprintf(stderr, "[nhl-heap] %s heap=0x%08X lr=0x%08X hdr=0x%08X fd=0x%08X bk=0x%08X"
                         " fd->bk=0x%08X bk->fd=0x%08X next=0x%08X next.hdr=0x%08X\n",
                 tag, (uint32_t)ctx->gpr[3], (uint32_t)ctx->lr, hdr, fd, bk,
                 (fd && fd < 0xF0000000u) ? nhl_rd32(fd + 0xC) : 0,
                 (bk && bk < 0xF0000000u) ? nhl_rd32(bk + 8) : 0, nx,
                 (nx < 0xF0000000u) ? nhl_rd32(nx + 4) : 0);
    /* The heap's EA::Thread::Mutex*: lwmutex at +0 (owner +0, attr +8, recursion +0xC), lock count +0x18. */
    {
        uint32_t heap = (uint32_t)ctx->gpr[29], mtx = nhl_rd32(heap + 0x8FC);
        std::fprintf(stderr, "[nhl-heap]   heap+0x87F=%02X mutex=0x%08X", vm_base[heap + 0x87F], mtx);
        if (mtx && mtx < 0xF0000000u)
            std::fprintf(stderr, " owner=%u attr=0x%X recur=%u count=%u self=%llu",
                         nhl_rd32(mtx), nhl_rd32(mtx + 8), nhl_rd32(mtx + 0xC), nhl_rd32(mtx + 0x18),
                         (unsigned long long)ctx->thread_id);
        std::fprintf(stderr, "\n");
    }
    for (uint32_t o = 0; o < 0x80; o += 0x20) {
        uint32_t a = ck - 0x20 + o;
        std::fprintf(stderr, "[nhl-heap]   %08X: %08X %08X %08X %08X %08X %08X %08X %08X\n", a,
                     nhl_rd32(a), nhl_rd32(a + 4), nhl_rd32(a + 8), nhl_rd32(a + 12),
                     nhl_rd32(a + 16), nhl_rd32(a + 20), nhl_rd32(a + 24), nhl_rd32(a + 28));
    }
    std::fflush(stderr);
    ppu_dump_guest_stack(ctx, tag);
}

/* Called at the top of the lifted allocator (func_00C9DEC8, r3 = heap). Logs
 * each heap (first 16) once with its mutex pointer: a heap whose +0x8FC is 0
 * takes no lock at all, so concurrent callers race on its free lists. */
extern "C" void nhl_heap_lockcheck(ppu_context* ctx, uint32_t heap)
{
    static uint32_t s_seen[16];
    static volatile long s_n = 0;
    for (long i = 0; i < s_n && i < 16; i++) if (s_seen[i] == heap) return;
    long k = s_n++;
    if (k >= 16) return;
    s_seen[k] = heap;
    uint32_t mtx = nhl_rd32(heap + 0x8FC);
    std::fprintf(stderr, "[nhl-heap] heap#%ld 0x%08X mutex=0x%08X flag87F=%02X first-tid=%llu lr=0x%08X%s\n",
                 k, heap, mtx, vm_base[heap + 0x87F], (unsigned long long)ctx->thread_id,
                 (uint32_t)ctx->lr, mtx ? "" : "  <-- UNLOCKED");
    /* Called from the alloc wrapper func_017E77F0 (lr 0x017E78BC), r25 is still
     * its allocator object: EA::Thread::Futex at +8 (count), semaphore id +0x20,
     * vtable/tag at +0. A zero semaphore id means the futex was never built and
     * every contended Lock() falls straight through (wait -> ESRCH). */
    if ((uint32_t)ctx->lr == 0x017E78BCu) {
        uint32_t obj = (uint32_t)ctx->gpr[25];
        std::fprintf(stderr, "[nhl-heap]   allocator obj=0x%08X +0=0x%08X futex.count=%u sem=%u owner+0x18=0x%08X heap+0x38=0x%08X\n",
                     obj, nhl_rd32(obj), nhl_rd32(obj + 0xC), nhl_rd32(obj + 0x20), nhl_rd32(obj + 0x1C),
                     nhl_rd32(obj + 0x38));
    }
}

/* [NHL local patch #237] Heap slack for large blocks. Game fn func_016040E0 (texture
 * processing of a team logo) writes 24-28 bytes past the end of the buffer it unpacks
 * an uncompressed A8R8G8B8 logo with mips into (the user's mod logos, 0x555D4 bytes);
 * that zeroes the next free chunk's header and links, and a later free unlinks through
 * NULL -> heap corruption -> hang (game_20261028_011650, #236 write watch). Stock logos
 * are DXT and take another path. Hand hook (tools/nhl_hooks.py HEAPSLACK) at the entry
 * of the core allocator func_00C9DEC8(heap r3, size r4, align r5, offset r6, flags r7)
 * adds NHL_HEAP_SLACK bytes (hex, default 100; 0 = off) to every request of at least
 * NHL_HEAP_SLACK_MIN (hex, default 10000), so the overrun lands in unused padding. */
extern "C" void nhl_heap_slack(ppu_context* ctx)
{
    static uint32_t slack = 0xFFFFFFFFu, min = 0x10000;
    if (slack == 0xFFFFFFFFu) {
        const char* e = std::getenv("NHL_HEAP_SLACK");
        const uint32_t s = e && *e ? (uint32_t)std::strtoul(e, nullptr, 16) : 0x100;
        if ((e = std::getenv("NHL_HEAP_SLACK_MIN")) && *e) min = (uint32_t)std::strtoul(e, nullptr, 16);
        slack = s;
    }
    if (slack && (uint32_t)ctx->gpr[4] >= min) ctx->gpr[4] = (uint32_t)ctx->gpr[4] + slack;
}
/* [NHL local patch #294] NHL_NO_CROWD: cuts the crowd where the game builds it, not in the
 * backend (#230 showed a backend skip leaves the sim as slow). Hand hooks tools/nhl_hooks.py
 * NOCROWD at the function entries.
 *   1: func_0156905C ("startcrowdculling") does not kick the cull job. Same path as an arena
 *      with no crowd loaded: zero visible instances, so the fan/chair/prop draw loops find
 *      empty pools and queue no nhl_crowd_instanced_dispatch jobs.
 *   2: also func_0145AD30, the UpdateCrowdJob body (crowd rigs stop animating). */
extern "C" int nhl_no_crowd(int level)
{
    static int s_level = -1;
    if (s_level < 0) { const char* e = std::getenv("NHL_NO_CROWD"); s_level = e && *e ? std::atoi(e) : 0; }
    static volatile long s_seen[3];
    if (s_level < level) return 0;
    if (level < 3 && !s_seen[level]) {
        s_seen[level] = 1;
        std::fprintf(stderr, "[NO-CROWD] level %d: cut %d active\n", s_level, level);
    }
    return 1;
}
/* [NHL local patch #297] NHL_SKIP_INTRO=1: Puck Peak's "NHL Legacy skip the intro" (RPCS3
 * patch, MIT, github.com/MatusRiedl/puckpeak-nhl-legacy-updater, gamepatch.INTRO at the
 * update 1.01 addresses of patch101.py). boot_main clears CBootflow's full-boot byte
 * 0x0220B9DC (the game's own quick boot: no language pick, intro video or legal screen) and
 * the SKIPINTRO hook makes FlowBoot::Start's beq at 0x00FB4834 always taken (step 2: no
 * autosave notice; autosave stays on). */
extern "C" int nhl_skip_intro(void)
{
    static int s_on = -1;
    if (s_on < 0) { const char* e = std::getenv("NHL_SKIP_INTRO"); s_on = e && *e == '1' ? 1 : 0; }
    return s_on;
}
/* [NHL local patch #238] Host RefPack decode for in-place streams. func_016040E0 is EA's
 * RefPack decoder (r3 dst, r4 src, r5 src length; returns the unpacked size). When the
 * compressed data sits at the end of the output buffer (dst_end inside the source) it
 * decodes IN PLACE: the write pointer chases the read pointer, and only the last 4 KB of
 * the source are saved to the stack; when the source ends even a few bytes short of the
 * output end it takes the plain decode with no protection at all. The user's mod logos
 * (uncompressed ARGB + mips; BIGF entry "88" followed by padding + a 32-byte entry) end
 * short, so near the end the writer overtakes unread input (offline replay of the game's
 * decode, scratchpad inplace_sim.py: 35/35 mod streams wrong from ~20 bytes before the end) -> the decoder reads its own output / stale stack as
 * commands -> a data-dependent overrun past the buffer (24-28 bytes in #236; 0x100 slack
 * #237 was not enough, game_20261028_170208) -> heap corruption -> hang. Here the whole
 * source is copied aside first and decoded with the output capped at the declared size,
 * which is exactly what an in-place-safe stream decodes to. Streams the game would have
 * misdecoded are counted in [REFPACK] lines. Hand hook tools/nhl_hooks.py REFPACK at the
 * function entry; NHL_REFPACK_HOST=0 = game decoder. */
extern "C" int nhl_refpack_inplace(ppu_context* ctx)
{
    static int on = -1;
    if (on < 0) { const char* e = std::getenv("NHL_REFPACK_HOST"); on = !(e && *e == '0'); }
    if (!on) return 0;
    const uint32_t dst = (uint32_t)ctx->gpr[3], src = (uint32_t)ctx->gpr[4], len = (uint32_t)ctx->gpr[5];
    if (len < 5 || len > 0x4000000u) return 0;
    const uint8_t* s = vm_base + src;
    const uint32_t flags = ((uint32_t)s[0] << 8) | s[1];
    const uint32_t nb = (flags & 0x8000) ? 4 : 3;
    uint32_t i = 2 + ((flags & 0x100) ? nb : 0);
    if (i + nb > len) return 0;
    uint32_t size = 0;
    for (uint32_t k = 0; k < nb; k++) size = (size << 8) | s[i + k];
    i += nb;
    const uint32_t dend = dst + size;
    /* [NHL local patch #297b] With skip-intro the game still waits on Press Start. Puck Peak's
     * art/pressstart.py changes the end of transitionDone in the Press Start screen's Apt
     * script (fe/ion/game/screens/bootflow/03_pressstart.big part 0, the same on every disc:
     * 19,141 bytes unpacked) from
     *     if (_global.QUICK_BOOT_FLOW == true) showPressStart();
     * to  if (_global.QUICK_BOOT_FLOW) { showPressStart(); startPressed(0); }
     * (18 bytes, same length; in a full boot QUICK_BOOT_FLOW is false). Same bytes, applied
     * here as the game unpacks it, so no file is written. */
    static const uint8_t kPressBefore[18] = { 0xB9,0x01,0xAF,0xCA,0x73,0x49,0x12,0x9D,0x00,0x00,0x00,0x00,0x00,0x00,0x03,0x59,0xB0,0xCB };
    static const uint8_t kPressAfter[18]  = { 0xB9,0x01,0xAF,0xCA,0x12,0x9D,0x00,0x00,0x00,0x00,0x07,0x59,0xB0,0xCB,0x59,0x5A,0xB0,0x4E };
    const uint32_t kPressAt = 0x40FD;
    const int press = size == 0x4AC5u && nhl_skip_intro();
    if (!press && (dst >= src + len || src >= dend)) return 0;   /* no overlap: the game's own decode is safe */
    const int inplace = dend >= src + i && dend <= src + len;   /* the game's D8 branch; else its plain decode */

    thread_local std::vector<uint8_t> in;
    in.assign(s, s + len);
    uint8_t* out = vm_base + dst;
    const uint32_t tail = size < 0x1000 ? size : 0x1000, tail_at = src + len - tail;
    uint32_t o = 0, clamped = 0, bad = 0;
    long overtake = 0; int misread = 0, tail_checked = 0;
    auto lit = [&](uint32_t n) {
        for (; n; n--) { if (i >= len) { bad = 1; return; } if (o < size) out[o++] = in[i++]; else { i++; clamped++; } }
    };
    auto copy = [&](uint32_t n, uint32_t off) {
        if (off > o) { bad = 1; return; }
        for (; n; n--) { if (o < size) { out[o] = out[o - off]; o++; } else clamped++; }
    };
    while (i < len && !bad) {
        /* what the game's in-place decode would see at this command boundary */
        const long rd = (long)(src + i) - (long)dst;
        if ((long)o > rd && (long)o - rd > overtake) overtake = (long)o - rd;
        if (!tail_checked && dst + o >= tail_at) { tail_checked = 1; if (src + i < tail_at) misread = 1; }
        const uint32_t b0 = in[i];
        if (!(b0 & 0x80)) {
            if (i + 2 > len) break;
            const uint32_t b1 = in[i + 1]; i += 2;
            lit(b0 & 3); copy(((b0 >> 2) & 7) + 3, ((b0 & 0x60) << 3) + b1 + 1);
        } else if (!(b0 & 0x40)) {
            if (i + 3 > len) break;
            const uint32_t b1 = in[i + 1], b2 = in[i + 2]; i += 3;
            lit(b1 >> 6); copy((b0 & 0x3F) + 4, ((b1 & 0x3F) << 8) + b2 + 1);
        } else if (!(b0 & 0x20)) {
            if (i + 4 > len) break;
            const uint32_t b1 = in[i + 1], b2 = in[i + 2], b3 = in[i + 3]; i += 4;
            lit(b0 & 3); copy(((b0 & 0x0C) << 6) + b3 + 5, ((b0 & 0x10) << 12) + (b1 << 8) + b2 + 1);
        } else {
            const uint32_t n = ((b0 & 0x1F) << 2) + 4; i++;
            if (n <= 0x70) lit(n);
            else { lit(b0 & 3); break; }
        }
    }
    if (press) {
        const int hit = !bad && o == size && std::memcmp(out + kPressAt, kPressBefore, 18) == 0;
        if (hit) std::memcpy(out + kPressAt, kPressAfter, 18);
        std::fprintf(stderr, "[SKIP-INTRO] Press Start screen %s (dst=0x%08X len=0x%X)\n",
                     hit ? "presses Start by itself" : "not the expected script, left as is", dst, len);
        ctx->gpr[3] = size;
        return 1;
    }
    static volatile long s_n = 0, s_unsafe = 0;
    const long k = _InterlockedIncrement(&s_n);
    const int unsafe = (inplace ? misread : overtake > 0) || clamped || bad || o != size;
    const long u = unsafe ? _InterlockedIncrement(&s_unsafe) : s_unsafe;
    if (unsafe ? u <= 40 : k <= 3)
        std::fprintf(stderr, "[REFPACK] %s #%ld dst=0x%08X size=0x%X src=0x%08X len=0x%X out=0x%X overtake=%ld misread=%d clamped=%u bad=%u tid=%llu lr=0x%08X%s\n",
                     inplace ? "in-place" : "overlap", k, dst, size, src, len, o, overtake, misread, clamped, bad,
                     (unsigned long long)ctx->thread_id, (uint32_t)ctx->lr, unsafe ? " (game decoder would corrupt)" : "");
    ctx->gpr[3] = size;
    return 1;
}
extern "C" void nhl_vp6_bridge_init(void);   /* nhl_vp6_bridge.cpp */

extern "C" void nhl_hle_register(void)
{
    nhl_vp6_bridge_init();
    const char* e = std::getenv("NHL_FAKE_USERMUSIC");
    if (e && *e && *e != '0') return;
    ps3_hle_register(0xC81CCF8Au, "cellSearchInitialize",       (void*)&nhl_cellSearchInitialize);
    ps3_hle_register(0xF80E31E5u, "cellMusicDecodeInitialize2", (void*)&nhl_cellMusicDecodeInitialize2);
}
