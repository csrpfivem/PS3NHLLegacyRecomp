/* #142: the lean SPU float path (MXCSR DAZ|FTZ + one result check) == the pure integer
 * reference (spu__*_lane), bit for bit. Build with /arch:AVX512 (#142) or /arch:AVX2 (#260). */
#include "spu_helpers.h"
#include <stdio.h>
#include <xmmintrin.h>
#if !defined(__AVX2__)
#error "needs the AVX2 or AVX-512 SPU float path"
#endif

static uint64_t s = 0x243F6A8885A308D3ull;
static uint32_t rnd(void) { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return (uint32_t)(s >> 16); }
static uint32_t fl(void) {
    const uint32_t sign = rnd() & 0x80000000u, frac = rnd() & 0x7FFFFFu;
    switch (rnd() % 12) {
    case 0:  return sign;                                         /* +-0 */
    case 1:  return sign | (frac ? frac : 1);                     /* denormal */
    case 2:  return sign | 0x7F800000u | frac;                    /* exponent 255 (x86 Inf/NaN) */
    case 3:  return (rnd() & 1) ? 0x7FFFFFFFu : 0xFFFFFFFFu;      /* +-Smax sentinels */
    case 4:  return sign | ((250 + rnd() % 5) << 23) | frac;      /* near overflow */
    case 5:  return sign | ((1 + rnd() % 6) << 23) | frac;        /* near underflow */
    case 6:  return sign | (63u << 23) | frac;                    /* tiny (products underflow) */
    case 7:  return sign | (191u << 23) | frac;                   /* huge (products overflow) */
    case 8:  return 0x7F7FFFFFu | sign;                           /* +-FLT_MAX */
    default: return sign | ((100 + rnd() % 56) << 23) | frac;     /* ordinary */
    }
}
static u128 vec(void) { u128 r; for (int i = 0; i < 4; i++) r._u32[i] = fl(); return r; }
/* the pure integer reference, lane by lane */
static u128 ref2(int op, u128 a, u128 b) {
    u128 r; for (int i = 0; i < 4; i++) {
        const uint32_t x = a._u32[i], y = b._u32[i];
        r._u32[i] = op == 0 ? spu__fa_lane(x, y) : op == 1 ? spu__fa_lane(x, y ^ 0x80000000u) : spu__fm_lane(x, y);
    } return r;
}
static u128 ref3(int op, u128 a, u128 b, u128 c) {
    u128 r; for (int i = 0; i < 4; i++) {
        const uint32_t x = a._u32[i], y = b._u32[i], z = c._u32[i];
        r._u32[i] = op == 0 ? spu__fma_lane(x, y, z, 0, 0) : op == 1 ? spu__fma_lane(x, y, z, 0, 0x80000000u)
                                                                    : spu__fma_lane(x, y, z, 0x80000000u, 0);
    } return r;
}
static long fails = 0;
static void chk(const char* op, u128 a, u128 b, u128 c, u128 got, u128 want) {
    if (!memcmp(&got, &want, 16)) return;
    if (fails++ < 25)
        printf("FAIL %-4s a=%08X %08X %08X %08X b=%08X %08X %08X %08X c=%08X  got=%08X %08X %08X %08X want=%08X %08X %08X %08X\n",
               op, a._u32[0], a._u32[1], a._u32[2], a._u32[3], b._u32[0], b._u32[1], b._u32[2], b._u32[3], c._u32[0],
               got._u32[0], got._u32[1], got._u32[2], got._u32[3], want._u32[0], want._u32[1], want._u32[2], want._u32[3]);
}
/* #280: the SPU double ops on an RZ thread (g_spu_float_rz, out-of-line switch to nearest) ==
 * the same ops on a round-to-nearest thread. */
static uint64_t dbl(void) {
    const uint64_t sign = (uint64_t)(rnd() & 1) << 63, frac = ((uint64_t)rnd() << 32 | rnd()) & 0xFFFFFFFFFFFFFull;
    switch (rnd() % 8) {
    case 0:  return sign;                                                   /* +-0 */
    case 1:  return sign | (frac ? frac : 1);                               /* denormal */
    case 2:  return sign | ((uint64_t)(1023 - 140 + rnd() % 280) << 52) | frac;  /* single range +- (frds) */
    default: return sign | ((uint64_t)(900 + rnd() % 250) << 52) | frac;
    }
}
static u128 dvec(void) { u128 r; for (int i = 0; i < 2; i++) { const uint64_t u = dbl();
    r._u32[2*i] = (uint32_t)(u >> 32); r._u32[2*i+1] = (uint32_t)u; } return r; }
static long dfails = 0;
static void dchk(const char* op, u128 got, u128 want) {
    if (!memcmp(&got, &want, 16)) return;
    if (dfails++ < 10) printf("FAIL %-5s got=%08X%08X %08X%08X want=%08X%08X %08X%08X\n", op,
                              got._u32[0], got._u32[1], got._u32[2], got._u32[3], want._u32[0], want._u32[1], want._u32[2], want._u32[3]);
}
static long test_double(long iters, unsigned rzcsr) {
    long n = 0;
    for (long it = 0; it < iters; it++) {
        const u128 a = dvec(), b = dvec(), c = dvec();
        u128 w[8], g[8];
        _mm_setcsr(rzcsr & ~0x6000u); g_spu_float_rz = 0;
        w[0] = spu_dfa(a, b); w[1] = spu_dfs(a, b); w[2] = spu_dfm(a, b); w[3] = spu_dfma(a, b, c);
        w[4] = spu_dfms(a, b, c); w[5] = spu_dfnms(a, b, c); w[6] = spu_dfnma(a, b, c); w[7] = spu_frds(a);
        _mm_setcsr(rzcsr); g_spu_float_rz = getenv("NO_DGUARD") ? 0 : 1;   /* NO_DGUARD=1: control, must FAIL */
        g[0] = spu_dfa(a, b); g[1] = spu_dfs(a, b); g[2] = spu_dfm(a, b); g[3] = spu_dfma(a, b, c);
        g[4] = spu_dfms(a, b, c); g[5] = spu_dfnms(a, b, c); g[6] = spu_dfnma(a, b, c); g[7] = spu_frds(a);
        static const char* nm[8] = {"dfa", "dfs", "dfm", "dfma", "dfms", "dfnms", "dfnma", "frds"};
        for (int k = 0; k < 8; k++) dchk(nm[k], g[k], w[k]);
        n += 8 * 2;
    }
    return n;
}
int main(int argc, char** argv) {
    const long iters = argc > 1 ? atol(argv[1]) : 4000000;
    /* FTZ | DAZ, as spu_float_thread_init sets them. NO_DAZ=1 = negative control: the lean
     * path without them must FAIL on denormal operands/results.
     * RZ=1 (#280): + MXCSR round toward zero and g_spu_float_rz (the AVX2 native-op path; on the
     * AVX-512 build it checks that the {rz-sae} path and the exact fallbacks don't mind RZ).
     * RZ=1 NO_RZCSR=1 = negative control: g_spu_float_rz without the MXCSR mode must FAIL. */
    const int rz = getenv("RZ") != NULL;
    if (!getenv("NO_DAZ")) _mm_setcsr(_mm_getcsr() | 0x8040);
    if (rz) {
        const unsigned base = _mm_getcsr() | 0x6000u;
        const long dn = test_double(iters / 4, base);
        printf("SPU double ops on an RZ thread: %ld lane checks, %ld failures\n", dn, dfails);
        _mm_setcsr(getenv("NO_RZCSR") ? base & ~0x6000u : base);
        g_spu_float_rz = 1;
    }
    g_spu_float_lean = 1;
    long n = 0, slow = 0;
    for (long it = 0; it < iters; it++) {
        u128 a = vec(), b = vec(), c = vec();
        if ((it & 7) == 0) for (int i = 0; i < 4; i++) b._u32[i] = a._u32[i] ^ 0x80000000u;   /* exact cancellation */
        if ((it & 15) == 1) for (int i = 0; i < 4; i++) c._u32[i] = 0;
        /* #260: near cancellation, and an addend just off the product (rounded sums near 24-bit edges) */
        if ((it & 7) == 3) for (int i = 0; i < 4; i++) b._u32[i] = (a._u32[i] ^ 0x80000000u) + (rnd() & 0xFF) - 0x80;
        if ((it & 7) == 5) { const u128 p = ref2(2, a, b);
                             for (int i = 0; i < 4; i++) c._u32[i] = (p._u32[i] ^ ((rnd() & 1) ? 0x80000000u : 0)) + (rnd() & 0x3F) - 0x20; }
        if ((it & 7) == 6) for (int i = 0; i < 4; i++) b._u32[i] = (rnd() & 0x80000000u) | (((a._u32[i] >> 23 & 0xFF) - 20 - rnd() % 20) & 0xFF) << 23 | (rnd() & 0x7FFFFF);
        chk("fa", a, b, c, spu_fa(a, b), ref2(0, a, b));
        chk("fs", a, b, c, spu_fs(a, b), ref2(1, a, b));
        chk("fm", a, b, c, spu_fm(a, b), ref2(2, a, b));
        chk("fma", a, b, c, spu_fma(a, b, c), ref3(0, a, b, c));
        chk("fms", a, b, c, spu_fms(a, b, c), ref3(1, a, b, c));
        chk("fnms", a, b, c, spu_fnms(a, b, c), ref3(2, a, b, c));
        n += 6 * 4;
        (void)slow;
    }
    printf("lean SPU float (DAZ|FTZ%s): %ld lane checks, %ld failures\n", rz ? "|RZ" : "", n, fails);
    return fails != 0 || dfails != 0;
}
