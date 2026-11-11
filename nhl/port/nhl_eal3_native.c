/* NHL: native IMDCT + polyphase synthesis for EAAudioCore's EALayer3 decoder SPU job
 * (spudecealayer3.elf, body 0x01DE9800). #305, NEXT_STEPS "DE-EMULATION".
 *
 * The job is an mpg123-style MPEG Layer III decoder. Its per-granule driver (job+0x9E18)
 * runs, per channel: alias reduction, a transpose, the IMDCT (job+0x90A8) and the
 * synthesis filter (job+0xA998). The two last ones are ~92% of the driver's cycles; the
 * hooks at their entries (tools/nhl_hooks.py, EAL3) call nhl_eal3_native(), which does
 * the same work on the same LS buffers, so the lifted and native paths share all state
 * and can be mixed call by call.
 *
 * Layouts (all floats big-endian in the LS):
 *   IMDCT  r3 = granule/channel record (+8 block type, +9 mixed flag), r4 = state block
 *          (+0 overlap, 576 floats), r5 = samples, both as 8 groups x 18 vectors of 4
 *          subbands ([g][i][lane], subband = 4g + lane). In place: samples = windowed
 *          IMDCT first half + old overlap, overlap = windowed second half.
 *   SYNTH  r3 = history: two 0x110-float buffers at +0 and +0x480 (mpg123 "buffs"),
 *          r4 = PCM out (18 x 32 floats), r5 = samples (same layout), r6 = scratch
 *          ([i][32], frequency-inverted copy), r7 = u32 rolling offset "bo".
 *          Window table (mpg123 decwin, 0x230 floats) = job + 0xD050.
 * Anything not covered by the capture corpus (mixed blocks) returns 0 = run the lifted code.
 * Checked offline against nhl\runs\eal3dump1 by _port_work\x360\ealayer3\ref_test.c. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Audio: sums may be reordered so the dot products vectorize (error stays ~1e-6). */
#pragma clang fp reassociate(on) contract(fast)

#define DECWIN_OFF 0xD050u
#define DECWIN_N   0x230
#define LS_MASK    0x3FFFFu

static float s_imdct[4][18][36];     /* [block type][input k][output i], window folded in */
static float s_matrix[32][40];       /* [input k][j]: j 0..16 = V[j], j 17..33 = -V[32 + j - 17] */
static float s_decwin[DECWIN_N];
static int s_ready, s_enabled = -1;
static volatile long s_init_lock;

static inline uint32_t bswap32(uint32_t v) { return __builtin_bswap32(v); }
static inline float ldf(const uint8_t* p) { uint32_t u; memcpy(&u, p, 4); u = bswap32(u); float f; memcpy(&f, &u, 4); return f; }
static inline void stf(uint8_t* p, float f) { uint32_t u; memcpy(&u, &f, 4); u = bswap32(u); memcpy(p, &u, 4); }
static inline uint32_t reg32(const void* regs, int n) { uint32_t v; memcpy(&v, (const uint8_t*)regs + n * 16, 4); return v; }

static void build_tables(void)
{
    const double pi = 3.14159265358979323846;
    double win[4][36];
    for (int i = 0; i < 36; i++) {
        const double lw = sin(pi / 36 * (i + 0.5));
        win[0][i] = lw;
        win[1][i] = i < 18 ? lw : i < 24 ? 1.0 : i < 30 ? sin(pi / 12 * (i - 18 + 0.5)) : 0.0;   /* start */
        win[3][i] = i < 6 ? 0.0 : i < 12 ? sin(pi / 12 * (i - 6 + 0.5)) : i < 18 ? 1.0 : lw;     /* stop */
    }
    for (int bt = 0; bt < 4; bt++) {
        if (bt == 2) continue;
        for (int k = 0; k < 18; k++)
            for (int i = 0; i < 36; i++)
                s_imdct[bt][k][i] = (float)(cos(pi / 72 * (2 * i + 1 + 18) * (2 * k + 1)) * win[bt][i]);
    }
    /* short blocks: three 12-point IMDCTs on k = 3m + w, overlapped at 6w + 6 */
    memset(s_imdct[2], 0, sizeof s_imdct[2]);
    for (int w = 0; w < 3; w++)
        for (int m = 0; m < 6; m++)
            for (int p = 0; p < 12; p++)
                s_imdct[2][3 * m + w][6 * w + 6 + p] +=
                    (float)(cos(pi / 24 * (2 * p + 7) * (2 * m + 1)) * sin(pi / 12 * (p + 0.5)));
    memset(s_matrix, 0, sizeof s_matrix);
    for (int k = 0; k < 32; k++)
        for (int j = 0; j <= 16; j++) {
            s_matrix[k][j]      = (float)cos(pi / 64 * (16 + j) * (2 * k + 1));
            s_matrix[k][17 + j] = (float)-cos(pi / 64 * (48 + j) * (2 * k + 1));
        }
}

/* The window table comes from the job's own data (identical for every load). */
static int init(const uint8_t* ls, uint32_t cb)
{
    while (__atomic_exchange_n(&s_init_lock, 1, __ATOMIC_ACQUIRE)) { }
    if (!s_ready) {
        const uint8_t* w = ls + ((cb + DECWIN_OFF) & LS_MASK);
        const float peak = ldf(w + 8 * 4);
        if (fabsf(peak + 1.144989f) < 1e-4f) {
            for (int i = 0; i < DECWIN_N; i++) s_decwin[i] = ldf(w + i * 4);
            build_tables();
            __atomic_store_n(&s_ready, 1, __ATOMIC_RELEASE);
        } else {
            fprintf(stderr, "[EAL3-NATIVE] window table not found at job+%X (%g); staying on the lifted code\n", DECWIN_OFF, peak);
            __atomic_store_n(&s_ready, -1, __ATOMIC_RELEASE);
        }
    }
    __atomic_store_n(&s_init_lock, 0, __ATOMIC_RELEASE);
    return s_ready > 0;
}

static int imdct_stage(const void* regs, uint8_t* ls)
{
    const uint8_t* rec = ls + (reg32(regs, 3) & LS_MASK);
    uint8_t* ov = ls + (reg32(regs, 4) & LS_MASK);
    uint8_t* sm = ls + (reg32(regs, 5) & LS_MASK);
    const unsigned bt = rec[8];
    if (bt > 3 || rec[9]) return 0;                       /* mixed blocks: not in the corpus */
    const float (*M)[36] = s_imdct[bt];
    for (int sb = 0; sb < 32; sb++) {
        const int g = sb >> 2, l = sb & 3;
        uint8_t* ps = sm + g * 288 + l * 4;               /* + i * 16 */
        uint8_t* po = ov + g * 288 + l * 4;
        float x[18];
        uint32_t any = 0;
        for (int k = 0; k < 18; k++) { uint32_t u; memcpy(&u, ps + k * 16, 4); any |= u; x[k] = ldf(ps + k * 16); }
        if (!(any & 0xFFFFFF7Fu)) {                       /* silent subband (BE word, sign bit ignored): out = overlap, overlap = 0 */
            for (int i = 0; i < 18; i++) { memcpy(ps + i * 16, po + i * 16, 4); memset(po + i * 16, 0, 4); }
            continue;
        }
        float acc[36] = { 0 };
        for (int k = 0; k < 18; k++) {
            const float xv = x[k];
            const float* restrict row = M[k];
            for (int i = 0; i < 36; i++) acc[i] += xv * row[i];
        }
        for (int i = 0; i < 18; i++) {
            stf(ps + i * 16, acc[i] + ldf(po + i * 16));
            stf(po + i * 16, acc[18 + i]);
        }
    }
    return 1;
}

static int synth_stage(const void* regs, uint8_t* ls)
{
    uint8_t* hist = ls + (reg32(regs, 3) & LS_MASK);
    uint8_t* pcm  = ls + (reg32(regs, 4) & LS_MASK);
    uint8_t* sm   = ls + (reg32(regs, 5) & LS_MASK);
    uint8_t* tmp  = ls + (reg32(regs, 6) & LS_MASK);
    uint8_t* pbo  = ls + (reg32(regs, 7) & LS_MASK);
    float buf[2][0x110];
    for (int i = 0; i < 0x110; i++) { buf[0][i] = ldf(hist + i * 4); buf[1][i] = ldf(hist + 0x480 + i * 4); }
    uint32_t bo; memcpy(&bo, pbo, 4); bo = bswap32(bo);
    for (int t = 0; t < 18; t++) {
        float s[32];
        for (int sb = 0; sb < 32; sb++) {
            uint8_t* p = sm + (sb >> 2) * 288 + t * 16 + (sb & 3) * 4;
            float v = ldf(p);
            if (t & sb & 1) { v = -v; stf(p, v); }        /* frequency inversion, kept in place like the job */
            s[sb] = v;
            stf(tmp + (t * 32 + sb) * 4, v);
        }
        float v[40] = { 0 };
        for (int k = 0; k < 32; k++) {
            const float sv = s[k];
            const float* restrict row = s_matrix[k];
            for (int j = 0; j < 40; j++) v[j] += sv * row[j];
        }
        bo = (bo - 1) & 15;
        float *b0, *out0, *out1;
        unsigned bo1;
        if (bo & 1) { b0 = buf[0]; bo1 = bo;     out0 = buf[1] + ((bo + 1) & 15); out1 = buf[0] + bo; }
        else        { b0 = buf[1]; bo1 = bo + 1; out0 = buf[0] + bo;              out1 = buf[1] + bo + 1; }
        for (int j = 0; j <= 16; j++) { out1[16 * j] = v[j]; out0[16 * j] = v[17 + j]; }
        uint8_t* o = pcm + t * 32 * 4;
        const float* w = s_decwin + 16 - bo1;
        for (int j = 0; j < 16; j++) {
            const float* restrict wj = w + 32 * j;
            const float* restrict bj = b0 + 16 * j;
            float sum = 0;
            for (int k = 0; k < 16; k += 2) sum += wj[k] * bj[k] - wj[k + 1] * bj[k + 1];
            stf(o + j * 4, sum);
        }
        {
            const float* wj = w + 0x200; const float* bj = b0 + 0x100;
            float sum = 0;
            for (int k = 0; k < 16; k += 2) sum += wj[k] * bj[k];
            stf(o + 16 * 4, sum);
        }
        w = s_decwin + 16 + bo1;
        for (int j = 15; j >= 1; j--) {
            const float* wj = w + 32 * j; const float* bj = b0 + 16 * j;
            float sum = 0;
            for (int k = 0; k < 16; k++) sum -= wj[-1 - k] * bj[k];
            stf(o + (32 - j) * 4, sum);
        }
    }
    for (int i = 0; i < 0x110; i++) { stf(hist + i * 4, buf[0][i]); stf(hist + 0x480 + i * 4, buf[1][i]); }
    bo = bswap32(bo); memcpy(pbo, &bo, 4);
    return 1;
}

/* stage 4 = IMDCT entry, 6 = synthesis entry (ES_* in nhl_eal3.c). Returns 1 when the work is done. */
int nhl_eal3_native(const void* regs, uint8_t* ls, uint32_t code_base, unsigned stage)
{
    if (s_enabled < 0) {
        const char* e = getenv("NHL_EAL3_NATIVE");
        s_enabled = (e && *e == '0') ? 0 : 1;             /* default on since #306; NHL_EAL3_NATIVE=0 = lifted */
        fprintf(stderr, "[EAL3-NATIVE] %s\n", s_enabled ? "on" : "off (NHL_EAL3_NATIVE=0)");
    }
    if (!s_enabled) return 0;
    if (s_ready <= 0 && (s_ready < 0 || !init(ls, code_base))) return 0;
    return stage == 4 ? imdct_stage(regs, ls) : stage == 6 ? synth_stage(regs, ls) : 0;
}

/* ---- #314: mode switch + check for the native Huffman pair loop (nhl_eal3_huff.inc) ----
 * NHL_EAL3_HUFF: 0 lifted (default until verified), 1 native, 2 check, 3 check
 * NHL_EAL3_HUFF_CHECKS (default 300000) pairs, then native if every one agreed. */
static int s_huff_mode = -1;
static long long s_huff_checked, s_huff_bad, s_huff_seen;
static _Thread_local uint32_t t_huff_pred[6];
static _Thread_local uint32_t t_huff_r89, t_huff_r82;
static _Thread_local int t_huff_have;

int nhl_eal3_huff_mode(void)
{
    if (s_huff_mode < 0) {
        const char* e = getenv("NHL_EAL3_HUFF");
        s_huff_mode = e && *e ? atoi(e) : 1;      /* default native since #315 (huff1 + huff2: 365k pairs, 0 mismatches) */
        if (s_huff_mode) fprintf(stderr, "[EAL3-HUFF] %s\n", s_huff_mode == 2 ? "check mode" : s_huff_mode == 3 ? "check, then native" : "native on");
    }
    return s_huff_mode;
}

void nhl_eal3_huff_check(void* ctx, void* gpr, uint8_t* ls, int (*one)(void*, uint32_t*))
{
    static long long s_need = -1;
    if (s_need < 0) { const char* e = getenv("NHL_EAL3_HUFF_CHECKS"); s_need = e && *e ? atoll(e) : 300000; }
    if (s_huff_mode == 3 && s_huff_checked >= s_need) {
        const int next = s_huff_bad ? 2 : 1;
        if (__atomic_exchange_n(&s_huff_mode, next, __ATOMIC_RELAXED) == 3)
            fprintf(stderr, "[EAL3-HUFF] verified %lld pairs, %lld mismatches (%lld loop heads seen) -> %s\n", s_huff_checked, s_huff_bad,
                    s_huff_seen, next == 1 ? "native ON" : "staying lifted (check mode)");
        t_huff_have = 0;
        return;
    }
    __atomic_fetch_add(&s_huff_seen, 1, __ATOMIC_RELAXED);
    const uint32_t r80 = reg32(gpr, 80), r89 = reg32(gpr, 89), r82 = reg32(gpr, 82);
    if (t_huff_have) {
        t_huff_have = 0;
        uint32_t got[7];
        for (int i = 0; i < 3; i++) { uint32_t v; memcpy(&v, ls + ((r80 + (i == 0 ? 0xF0u : i == 1 ? 0x120u : 0x130u)) & LS_MASK), 4); got[i] = bswap32(v); }
        for (int i = 0; i < 2; i++) { uint32_t v; memcpy(&v, ls + ((t_huff_pred[5] + 4u * (unsigned)i) & LS_MASK), 4); got[3 + i] = bswap32(v); }
        const long long n = __atomic_add_fetch(&s_huff_checked, 1, __ATOMIC_RELAXED);
        const int bad = memcmp(got, t_huff_pred, 5 * sizeof(uint32_t)) != 0 || r89 != t_huff_r89 + 2 || r82 != t_huff_r82 + 8;
        if (bad && __atomic_fetch_add(&s_huff_bad, 1, __ATOMIC_RELAXED) < 8)
            fprintf(stderr, "[EAL3-HUFF] MISMATCH native P %05X BB %08X BC %d w %08X %08X | lifted P %05X BB %08X BC %d w %08X %08X | r89 %u->%u r82 %05X->%05X\n",
                    t_huff_pred[0], t_huff_pred[1], (int)t_huff_pred[2], t_huff_pred[3], t_huff_pred[4],
                    got[0], got[1], (int)got[2], got[3], got[4], t_huff_r89, r89, t_huff_r82, r82);
        if ((n & (n - 1)) == 0 && n >= 4096) fprintf(stderr, "[EAL3-HUFF] checked %lld pairs, mismatches %lld\n", n, s_huff_bad);
    }
    if (one(ctx, t_huff_pred)) { t_huff_have = 1; t_huff_r89 = r89; t_huff_r82 = r82; }
}
