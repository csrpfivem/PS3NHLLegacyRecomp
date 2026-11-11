/* NHL: native table scan for eacgfx_base_command_dispatch_job (SPU program 0x01DB3300). #307.
 *
 * cmdprof1: 27% of this program's time is one loop (body 0xB2EC-0xB484, LS = body + 0x80):
 * a linear scan over 8-byte entries { u32 item, u8 mask, u8 level, .. } fetched from main
 * memory, looking for one this SPU may claim:
 *     item != 0  &&  level < r96 (byte)  &&  (mask & LS[0x3FFF1]) != 0
 * Entries that fail only step the cursors (r81 LS pointer, r82 64-bit EA, r85, r88 index)
 * and recompute r3/r4/r7/r8/r95. This hook, at the loop head (lifted func 0xB36C), does
 * those steps natively for every failing entry except the last of the table, then lets
 * the lifted code carry on from the first entry it has to look at itself. No LS writes,
 * no channel traffic: registers only.
 *
 *   NHL_CMDSCAN=1   native skip (default since #307 once verified; =0 lifted only)
 *   NHL_CMDSCAN=2   check mode: nothing is changed; each lifted iteration is compared with
 *                   what the native step would have produced ([CMDSCAN] ... mismatches)
 *   [CMDSCAN] every 10 s: scans, entries seen at the head, entries skipped natively */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define LS_MASK 0x3FFFFu
enum { R3 = 3, R4 = 4, R7 = 7, R8 = 8, R75 = 75, R76 = 76, R77 = 77, R78 = 78, R79 = 79,
       R81 = 81, R82 = 82, R83 = 83, R85 = 85, R88 = 88, R95 = 95, R96 = 96 };
static const int k_regs[] = { R3, R4, R7, R8, R81, R82, R85, R88, R95 };
#define NREGS ((int)(sizeof k_regs / sizeof k_regs[0]))

volatile int g_cmdscan_live = -1;      /* boot_main.cpp place_thread: -1 = no override, 0 = lifted, 1 = native */
static int s_mode = -1;
static long long s_scans, s_heads, s_skipped, s_checked, s_bad;
static _Thread_local uint32_t t_pred[NREGS][4];
static _Thread_local int t_have_pred;

static inline uint32_t* rw(void* regs, int n) { return (uint32_t*)((uint8_t*)regs + n * 16); }

static int mode(void)
{
    if (s_mode < 0) {
        const char* e = getenv("NHL_CMDSCAN");
        s_mode = e && *e ? atoi(e) : 1;      /* default native since #308 (cmdscan2 / cmdscan3: 2 x 500k iterations, 0 mismatches) */
        if (s_mode) fprintf(stderr, "[CMDSCAN] %s\n", s_mode == 2 ? "check mode" : s_mode == 3 ? "check, then native" : "native skip on");
    }
    return s_mode;
}

static void report(void)
{
    static clock_t s_last;
    const clock_t now = clock();
    if (now - s_last < 10 * CLOCKS_PER_SEC) return;
    s_last = now;
    fprintf(stderr, "[CMDSCAN] t=%ld scans %lld, head entries %lld, skipped natively %lld | checked %lld, mismatches %lld\n",
            (long)(now / CLOCKS_PER_SEC), s_scans, s_heads, s_skipped, s_checked, s_bad);
}

/* Would the lifted loop take the "not for me" path for the entry at r81? */
static inline int entry_skippable(const uint8_t* ls, const void* regs)
{
    const uint32_t a = *rw((void*)regs, R81) & LS_MASK;
    const uint8_t* e = ls + a;
    uint32_t item; memcpy(&item, e, 4);
    if (item == 0) return 1;
    const uint8_t limit = (uint8_t)*rw((void*)regs, R96);       /* SPU byte 3 of r96 */
    if (!(limit > e[5])) return 1;
    return (ls[0x3FFF1] & e[4]) == 0;
}

/* The loop tail (body 0xB444-0xB484) on a register file. */
static inline void step(const uint8_t* ls, void* regs)
{
    uint32_t* r81 = rw(regs, R81); uint32_t* r85 = rw(regs, R85); uint32_t* r82 = rw(regs, R82);
    uint32_t* r88 = rw(regs, R88); const uint32_t* r83 = rw(regs, R83);
    for (int i = 0; i < 4; i++) { r85[i] += 8; r81[i] += 8; }
    uint32_t* r79 = rw(regs, R79); r79[0] = 0; r79[1] = 0xFF; r79[2] = 0; r79[3] = 0xFF;
    uint32_t* r76 = rw(regs, R76); r76[0] = 0; r76[1] = 8; r76[2] = 0; r76[3] = 8;
    uint32_t* r78 = rw(regs, R78);
    for (int i = 0; i < 4; i++) { uint32_t w; memcpy(&w, ls + 0xC4F0 + i * 4, 4); r78[i] = __builtin_bswap32(w); }
    uint32_t* r77 = rw(regs, R77);
    r77[0] = 0; r77[2] = 0;
    r77[1] = r82[1] > 0xFFFFFFF7u; r77[3] = r82[3] > 0xFFFFFFF7u;      /* cg: carry out of each word */
    r82[0] += r77[1]; r82[1] += 8; r82[2] += r77[3]; r82[3] += 8;      /* two 64-bit adds of 8 */
    memcpy(rw(regs, R75), r82, 16);
    uint32_t* r95 = rw(regs, R95);
    for (int i = 0; i < 4; i++) r95[i] = (int32_t)r83[i] > (int32_t)r88[i] ? 0xFFFFFFFFu : 0;
    uint32_t* r3 = rw(regs, R3); uint32_t* r4 = rw(regs, R4); uint32_t* r7 = rw(regs, R7); uint32_t* r8 = rw(regs, R8);
    for (int i = 0; i < 4; i++) { r3[i] = r81[i] + 5; r4[i] = r81[i] + 4; r7[i] = r81[i] + 18; r8[i] = r81[i] + 17; r88[i] += 1; }
}

/* #313: n loop tails at once. spuprof1 showed the per-entry step() above (it rewrites ~12
 * registers and a shared counter per entry) cost more than the lifted loop it replaced;
 * only the last tail's values survive, so they are computed once. apply_selftest() checks
 * this against n calls of step() before it is first used. */
static void apply(const uint8_t* ls, void* regs, uint32_t n)
{
    uint32_t* r81 = rw(regs, R81); uint32_t* r85 = rw(regs, R85); uint32_t* r82 = rw(regs, R82);
    uint32_t* r88 = rw(regs, R88); const uint32_t* r83 = rw(regs, R83);
    for (int i = 0; i < 4; i++) { r85[i] += 8 * n; r81[i] += 8 * n; }
    uint32_t* r79 = rw(regs, R79); r79[0] = 0; r79[1] = 0xFF; r79[2] = 0; r79[3] = 0xFF;
    uint32_t* r76 = rw(regs, R76); r76[0] = 0; r76[1] = 8; r76[2] = 0; r76[3] = 8;
    uint32_t* r78 = rw(regs, R78);
    for (int i = 0; i < 4; i++) { uint32_t w; memcpy(&w, ls + 0xC4F0 + i * 4, 4); r78[i] = __builtin_bswap32(w); }
    uint32_t* r77 = rw(regs, R77);
    for (int h = 0; h < 4; h += 2) {                       /* two 64-bit values (hi word, lo word) */
        uint64_t v = ((uint64_t)r82[h] << 32) | r82[h + 1];
        v += 8ull * (n - 1);                               /* value before the last tail */
        r77[h] = 0; r77[h + 1] = (uint32_t)v > 0xFFFFFFF7u;
        v += 8;
        r82[h] = (uint32_t)(v >> 32); r82[h + 1] = (uint32_t)v;
    }
    memcpy(rw(regs, R75), r82, 16);
    uint32_t* r95 = rw(regs, R95);
    for (int i = 0; i < 4; i++) r95[i] = (int32_t)r83[i] > (int32_t)(r88[i] + n - 1) ? 0xFFFFFFFFu : 0;
    uint32_t* r3 = rw(regs, R3); uint32_t* r4 = rw(regs, R4); uint32_t* r7 = rw(regs, R7); uint32_t* r8 = rw(regs, R8);
    for (int i = 0; i < 4; i++) { r3[i] = r81[i] + 5; r4[i] = r81[i] + 4; r7[i] = r81[i] + 18; r8[i] = r81[i] + 17; r88[i] += n; }
}

static int s_apply_ok = -1;
static int apply_selftest(const uint8_t* ls, const void* regs)
{
    uint8_t a[128 * 16], b[128 * 16];
    uint32_t seed = 0x1234567u;
    for (int trial = 0; trial < 400; trial++) {
        memcpy(a, regs, sizeof a);
        for (int k = 0; k < NREGS; k++) for (int i = 0; i < 4; i++) {
            seed = seed * 1664525u + 1013904223u;
            uint32_t v = seed;
            if ((trial & 3) == 1) v |= 0xFFFFFF00u;                /* near a carry */
            if ((trial & 3) == 2) v &= 0x000000FFu;
            rw(a, k_regs[k])[i] = v;
        }
        seed = seed * 1664525u + 1013904223u; rw(a, R83)[0] = seed; rw(a, R83)[2] = seed >> 3;
        memcpy(b, a, sizeof b);
        const uint32_t n = 1 + (seed >> 20) % 50;
        for (uint32_t i = 0; i < n; i++) step(ls, a);
        apply(ls, b, n);
        if (memcmp(a, b, sizeof a)) {
            fprintf(stderr, "[CMDSCAN] apply() self-test FAILED at trial %d (n=%u): using the per-entry path\n", trial, n);
            return 0;
        }
    }
    return 1;
}

/* site 0 = loop head (called before the lifted function loads its registers),
 * site 1 = scan start (body 0xB2C8, right after the table GET; once per scan). */
void nhl_cmdscan(void* regs, const uint8_t* ls, unsigned site)
{
    const int m = mode();
    if (!m) return;
    if (m == 1) {                                          /* native: no shared counters on this path */
        const uint32_t* q81 = rw(regs, R81);
        if (site != 0 || (q81[0] & 7) || g_cmdscan_live == 0) return;   /* live A/B: NHL_PLACE_FILE token cmdscan=0 */
        if (s_apply_ok < 0) s_apply_ok = apply_selftest(ls, regs);
        const int32_t cnt = (int32_t)*rw(regs, R83), idx = (int32_t)*rw(regs, R88);
        const uint8_t limit = (uint8_t)*rw(regs, R96), me = ls[0x3FFF1];
        const uint32_t a0 = q81[0];
        uint32_t n = 0;
        while (cnt > (int32_t)((uint32_t)idx + n)) {
            const uint8_t* e = ls + ((a0 + 8 * n) & LS_MASK);
            uint32_t item; memcpy(&item, e, 4);
            if (item != 0 && limit > e[5] && (me & e[4]) != 0) break;   /* one this SPU may claim: lifted code takes it */
            n++;
        }
        if (!n) return;
        if (s_apply_ok) apply(ls, regs, n);
        else for (uint32_t i = 0; i < n; i++) step(ls, regs);
        return;
    }
    if (site == 1) {
        t_have_pred = 0;
        __atomic_fetch_add(&s_scans, 1, __ATOMIC_RELAXED);
        if ((s_scans & 255) == 0) report();
        return;
    }
    __atomic_fetch_add(&s_heads, 1, __ATOMIC_RELAXED);
    const uint32_t* r81 = rw(regs, R81);
    if (m >= 2) {
        /* mode 3: check the first AUTO_CHECKS iterations, then go native if all agreed */
        if (m == 3 && s_checked >= 500000) {
            const int next = s_bad ? 2 : 1;
            if (__atomic_exchange_n(&s_mode, next, __ATOMIC_RELAXED) == 3)
                fprintf(stderr, "[CMDSCAN] verified %lld iterations, %lld mismatches -> %s\n", s_checked, s_bad,
                        next == 1 ? "native skip ON" : "staying in check mode (lifted)");
            t_have_pred = 0;
            return;
        }
        if (t_have_pred) {
            int bad = 0;
            for (int i = 0; i < NREGS; i++) if (memcmp(t_pred[i], rw(regs, k_regs[i]), 16)) bad = 1;
            __atomic_fetch_add(&s_checked, 1, __ATOMIC_RELAXED);
            if (bad) {
                if (__atomic_fetch_add(&s_bad, 1, __ATOMIC_RELAXED) < 8)
                    for (int i = 0; i < NREGS; i++) {
                        const uint32_t* a = rw(regs, k_regs[i]);
                        if (memcmp(t_pred[i], a, 16))
                            fprintf(stderr, "[CMDSCAN] MISMATCH r%d native %08X %08X %08X %08X lifted %08X %08X %08X %08X\n", k_regs[i],
                                    t_pred[i][0], t_pred[i][1], t_pred[i][2], t_pred[i][3], a[0], a[1], a[2], a[3]);
                    }
            }
            t_have_pred = 0;
        }
        if ((r81[0] & 7) == 0 && entry_skippable(ls, regs) && (int32_t)*rw(regs, R83) > (int32_t)*rw(regs, R88)) {
            uint8_t copy[128 * 16];
            memcpy(copy, regs, sizeof copy);
            step(ls, copy);
            for (int i = 0; i < NREGS; i++) memcpy(t_pred[i], rw(copy, k_regs[i]), 16);
            t_have_pred = 1;
        }
        return;
    }
    if ((r81[0] & 7) || g_cmdscan_live == 0) return;     /* live A/B: NHL_PLACE_FILE token cmdscan=0 */
    long long n = 0;
    while (entry_skippable(ls, regs) && (int32_t)*rw(regs, R83) > (int32_t)*rw(regs, R88)) { step(ls, regs); n++; }
    if (n) __atomic_fetch_add(&s_skipped, n, __ATOMIC_RELAXED);
}

/* ---- #309: native loop for EA's chunked DMA transfer helper ---------------------------
 * 68 lifted SPU images carry the same helper (r3 LSA, r4 EA, r5 size, r6 tag, r7 cmd):
 * a loop that issues the transfer in pieces (1/2/4/8 bytes, a multiple of 16, at most
 * 16 KB), six channel writes per piece. Lifted, every channel write first stores ~25
 * cached registers back; it is 45% of ANT_kernel and 17% of the command dispatch job.
 * tools/nhl_hooks.py (DMALOOP) injects nhl_dma_step() into each of those files = the same
 * helper calls as the lifted chunks, but on ctx->gpr in place, and hooks the loop's
 * condition chunk:
 *   NHL_DMA_NATIVE=1  run the whole loop natively      =0  lifted
 *   NHL_DMA_NATIVE=2  check only: every lifted iteration vs one dry native step
 *   NHL_DMA_NATIVE=3  check 300k iterations, then native if all agreed
 *   NHL_PLACE_FILE token dmaloop=0/1 switches a verified native loop live. */
volatile int g_dma_live = -1;
static int s_dma_mode = -1;
static long long s_dma_checked, s_dma_bad, s_dma_iters;
static _Thread_local uint8_t t_dma_pred[32 * 16];
static _Thread_local int t_dma_have;

int nhl_dma_mode(void)
{
    if (s_dma_mode < 0) {
        const char* e = getenv("NHL_DMA_NATIVE");
        s_dma_mode = e && *e ? atoi(e) : 1;      /* default native since #310 (dmaloop1: 300k iterations, 0 mismatches) */
        if (s_dma_mode) fprintf(stderr, "[DMALOOP] %s\n", s_dma_mode == 2 ? "check mode" : s_dma_mode == 3 ? "check, then native" : "native on");
    }
    if (s_dma_mode == 1 && g_dma_live == 0) return 0;
    return s_dma_mode;
}

void nhl_dma_check(void* ctx, void* g, int (*step)(void*, void*, int))
{
    static long long s_need = -1;
    if (s_need < 0) { const char* e = getenv("NHL_DMA_CHECKS"); s_need = e && *e ? atoll(e) : 300000; }
    if (s_dma_mode == 3 && s_dma_checked >= s_need) {
        const int next = s_dma_bad ? 2 : 1;
        if (__atomic_exchange_n(&s_dma_mode, next, __ATOMIC_RELAXED) == 3)
            fprintf(stderr, "[DMALOOP] verified %lld iterations, %lld mismatches -> %s\n", s_dma_checked, s_dma_bad,
                    next == 1 ? "native ON" : "staying lifted (check mode)");
        t_dma_have = 0;
        return;
    }
    __atomic_fetch_add(&s_dma_iters, 1, __ATOMIC_RELAXED);
    if (t_dma_have) {
        t_dma_have = 0;
        const long long n = __atomic_add_fetch(&s_dma_checked, 1, __ATOMIC_RELAXED);
        if ((n & (n - 1)) == 0 && n >= 1024) fprintf(stderr, "[DMALOOP] checked %lld, mismatches %lld\n", n, s_dma_bad);
        if (memcmp(t_dma_pred + 2 * 16, (const uint8_t*)g + 2 * 16, 30 * 16)) {
            if (__atomic_fetch_add(&s_dma_bad, 1, __ATOMIC_RELAXED) < 6)
                for (int r = 2; r < 32; r++) {
                    const uint32_t* a = (const uint32_t*)(t_dma_pred + r * 16);
                    const uint32_t* b = (const uint32_t*)((const uint8_t*)g + r * 16);
                    if (memcmp(a, b, 16))
                        fprintf(stderr, "[DMALOOP] MISMATCH r%d native %08X %08X %08X %08X lifted %08X %08X %08X %08X\n",
                                r, a[0], a[1], a[2], a[3], b[0], b[1], b[2], b[3]);
                }
        }
    }
    memcpy(t_dma_pred, g, sizeof t_dma_pred);
    t_dma_have = step(ctx, t_dma_pred, 0);
    if ((s_dma_iters & 0xFFFFF) == 0)
        fprintf(stderr, "[DMALOOP] iterations %lld, checked %lld, mismatches %lld\n", s_dma_iters, s_dma_checked, s_dma_bad);
}
