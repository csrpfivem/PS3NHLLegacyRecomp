/* #137: every x86 helper == its scalar _ref, bit for bit. Build once with /arch:AVX2
 * (Steam Deck class) and once with /arch:AVX512 (the real build). */
#include "spu_helpers.h"
#include <stdio.h>
#ifndef SPU_X86_INT
#error "SPU_X86_INT not enabled for this target"
#endif

static uint64_t s = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void) { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return (uint32_t)(s >> 16); }
/* lanes biased toward the edge cases of each family */
static uint32_t lane(void) {
    static const uint32_t sp[] = {0, 1, 2, 0x7FFFFFFFu, 0x80000000u, 0x80000001u, 0xFFFFFFFFu, 0x7F800000u,
        0xFF800000u, 0x7F7FFFFFu, 0x00800000u, 0x007FFFFFu, 0x80800000u, 0x4F000000u, 0xCF000000u,
        0x4F7FFFFFu, 0x4EFFFFFFu, 0x3F800000u, 0xBF800000u, 0x00FFFFFFu, 0x01000000u, 0xFFFFFF00u};
    const uint32_t k = rnd() % 8;
    if (k == 0) return sp[rnd() % (sizeof sp / sizeof sp[0])];
    if (k == 1) return rnd() & 0xFFFF;                                      /* small ints */
    if (k == 2) return (uint32_t)-(int32_t)(rnd() & 0xFFFFF);               /* small negatives */
    if (k == 3) return (rnd() & 0x807FFFFFu) | ((100 + rnd() % 60) << 23);  /* floats near int range */
    if (k == 4) return rnd() >> (rnd() % 32);                               /* every magnitude */
    return rnd();
}
static u128 vec(void) { u128 r; for (int i = 0; i < 4; i++) r._u32[i] = lane(); return r; }
static long fails = 0;
static void chk(const char* op, int arg, u128 a, u128 got, u128 want) {
    if (!memcmp(&got, &want, 16)) return;
    if (fails++ < 20)
        printf("FAIL %s arg=%d a=%08X %08X %08X %08X got=%08X %08X %08X %08X want=%08X %08X %08X %08X\n", op, arg,
               a._u32[0], a._u32[1], a._u32[2], a._u32[3], got._u32[0], got._u32[1], got._u32[2], got._u32[3],
               want._u32[0], want._u32[1], want._u32[2], want._u32[3]);
}
int main(void) {
    long n = 0;
    for (int it = 0; it < 200000; it++) {
        const u128 a = vec();
        for (int sh = -70; sh <= 70; sh++) {
            chk("shlqbyi", sh, a, spu_shlqbyi(a, sh), spu_shlqbyi_ref(a, sh));
            chk("rotqbyi", sh, a, spu_rotqbyi(a, sh), spu_rotqbyi_ref(a, sh));
            chk("rotqmbyi", sh, a, spu_rotqmbyi(a, sh), spu_rotqmbyi_ref(a, sh));
            chk("cwd", sh, a, spu_cwd(a, sh), spu_cwd_ref(a, sh));
            chk("shlqbii", sh, a, spu_shlqbii(a, sh), spu_shlqbii_ref(a, sh));     /* #138 */
            chk("rotqbii", sh, a, spu_rotqbii(a, sh), spu_rotqbii_ref(a, sh));
            chk("rotqmbii", sh, a, spu_rotqmbii(a, sh), spu_rotqmbii_ref(a, sh));
            n += 7;
        }
        for (int j = 0; j < 32; j++) {
            u128 b = vec(); b._u32[0] = (j < 16) ? (rnd() & ~0xFFu) | (uint32_t)(it * 7 + j) % 256 : lane();
            chk("shlqby", j, a, spu_shlqby(a, b), spu_shlqby_ref(a, b));
            chk("rotqby", j, a, spu_rotqby(a, b), spu_rotqby_ref(a, b));
            chk("shlqbybi", j, a, spu_shlqbybi(a, b), spu_shlqbybi_ref(a, b));
            chk("rotqbybi", j, a, spu_rotqbybi(a, b), spu_rotqbybi_ref(a, b));
            chk("rotqmby", j, a, spu_rotqmby(a, b), spu_rotqmby_ref(a, b));
            chk("rotqmbybi", j, a, spu_rotqmbybi(a, b), spu_rotqmbybi_ref(a, b));
            chk("shlqbi", j, a, spu_shlqbi(a, b), spu_shlqbi_ref(a, b));          /* #138 */
            chk("rotqbi", j, a, spu_rotqbi(a, b), spu_rotqbi_ref(a, b));
            chk("rotqmbi", j, a, spu_rotqmbi(a, b), spu_rotqmbi_ref(a, b));
            chk("sumb", j, a, spu_sumb(a, b), spu_sumb_ref(a, b));
            n += 10;
        }
        for (int i8 = 0; i8 < 256; i8 += (it < 2000 ? 1 : 37)) {
            const int arg = i8 | ((rnd() & 1) << 8);   /* bits above 8 must be ignored */
            chk("cflts", arg, a, spu_cflts(a, arg), spu_cflts_ref(a, arg));
            chk("cfltu", arg, a, spu_cfltu(a, arg), spu_cfltu_ref(a, arg));
            chk("csflt", arg, a, spu_csflt(a, arg), spu_csflt_ref(a, arg));
            chk("cuflt", arg, a, spu_cuflt(a, arg), spu_cuflt_ref(a, arg));
            n += 4;
        }
        {   /* #157: shufb, random controls with every selector class */
            const u128 b = vec(); u128 c;
            for (int j = 0; j < 16; j++) c._u8[j] = (uint8_t)rnd();
            chk("shufb", 0, a, spu_shufb(a, b, c), spu_shufb_ref(a, b, c));
            /* #159 logic ops + selb, #160 fi */
            chk("selb", 0, a, spu_selb(a, b, c), spu_selb_ref(a, b, c));
            chk("and", 0, a, spu_and(a, b), spu_and_ref(a, b));
            chk("or", 0, a, spu_or(a, b), spu_or_ref(a, b));
            chk("xor", 0, a, spu_xor(a, b), spu_xor_ref(a, b));
            chk("nand", 0, a, spu_nand(a, b), spu_nand_ref(a, b));
            chk("nor", 0, a, spu_nor(a, b), spu_nor_ref(a, b));
            chk("andc", 0, a, spu_andc(a, b), spu_andc_ref(a, b));
            chk("orc", 0, a, spu_orc(a, b), spu_orc_ref(a, b));
            chk("eqv", 0, a, spu_eqv(a, b), spu_eqv_ref(a, b));
            chk("fi", 0, a, spu_fi(a, b), spu_fi_ref(a, b));
            chk("fi-c", 0, c, spu_fi(c, a), spu_fi_ref(c, a));
            n += 12;
        }
        chk("clz", 0, a, spu_clz(a), spu_clz_ref(a));
        chk("gbb", 0, a, spu_gbb(a), spu_gbb_ref(a));
        chk("fsm", 0, a, spu_fsm(a), spu_fsm_ref(a));
        chk("fsmh", 0, a, spu_fsmh(a), spu_fsmh_ref(a));
        chk("fsmb", 0, a, spu_fsmb(a), spu_fsmb_ref(a));
        chk("gb", 0, a, spu_gb(a), spu_gb_ref(a));                               /* #138 */
        chk("gbh", 0, a, spu_gbh(a), spu_gbh_ref(a));
        n += 7;
    }
    /* every low-bit pattern for gb (4 words) and gbh (8 halfwords), random high bits */
    for (uint32_t v = 0; v < 256; v++) for (int rep = 0; rep < 64; rep++) {
        u128 a = vec();
        for (int h = 0; h < 8; h++) a._u16[h] = (uint16_t)((a._u16[h] & ~1u) | ((v >> h) & 1));
        chk("gbh", (int)v, a, spu_gbh(a), spu_gbh_ref(a));
        chk("gb", (int)v, a, spu_gb(a), spu_gb_ref(a));
        n += 2;
    }
    /* exhaustive masks and every single-bit clz input */
    for (uint32_t v = 0; v < 0x20000u; v++) {
        u128 a = vec(); a._u32[0] = v | (rnd() & 0xFFFE0000u);
        chk("fsmb", (int)v, a, spu_fsmb(a), spu_fsmb_ref(a));
        chk("fsmh", (int)v, a, spu_fsmh(a), spu_fsmh_ref(a));
        chk("fsm", (int)v, a, spu_fsm(a), spu_fsm_ref(a));
        a._u32[1] = v & 1 ? ~0u : 0; a._u32[2] = v & 2 ? 1u : 0;
        chk("gbb", (int)v, a, spu_gbb(a), spu_gbb_ref(a));
        n += 4;
    }
    /* #160 fi: every exponent (incl. 0, 1, 255) and the d ~ 0 boundary (step * y vs base << 19) */
    for (uint32_t e = 0; e < 256; e++) for (int rep = 0; rep < 20000; rep++) {
        u128 a, b;
        for (int i = 0; i < 4; i++) {
            const uint32_t base = rnd() & 0x1FFF, step = rnd() & 0x3FF;
            uint32_t y = rnd() & 0x7FFFF;
            if ((rep & 3) == 0 && step) { y = ((base << 19) / step) + (rnd() % 5) - 2; y &= 0x7FFFF; }
            b._u32[i] = (rnd() & 0x80000000u) | (e << 23) | (base << 10) | step;
            a._u32[i] = (rnd() & 0xFFF80000u) | y;
        }
        chk("fi-e", (int)e, a, spu_fi(a, b), spu_fi_ref(a, b));
        n++;
    }
    /* #157: every control byte value at every byte position */
    for (uint32_t v = 0; v < 256; v++) for (int rep = 0; rep < 256; rep++) {
        const u128 a = vec(), b = vec(); u128 c;
        for (int j = 0; j < 16; j++) c._u8[j] = (uint8_t)(v + (uint32_t)j * (2u * (uint32_t)rep + 1u));
        chk("shufb-all", (int)v, a, spu_shufb(a, b, c), spu_shufb_ref(a, b, c));
        n++;
    }
    for (int b = 0; b < 32; b++) for (uint32_t lo = 0; lo < 4096; lo++) {
        u128 a; for (int i = 0; i < 4; i++) a._u32[i] = (1u << b) | ((lo * 2654435761u + i) & ((1u << b) - 1));
        chk("clz", b, a, spu_clz(a), spu_clz_ref(a));
        for (int i8 = 0; i8 < 256; i8 += 51) {
            chk("csflt", i8, a, spu_csflt(a, i8), spu_csflt_ref(a, i8));
            chk("cuflt", i8, a, spu_cuflt(a, i8), spu_cuflt_ref(a, i8));
            u128 ng = a; for (int i = 0; i < 4; i++) ng._u32[i] = 0u - a._u32[i];
            chk("csflt-neg", i8, ng, spu_csflt(ng, i8), spu_csflt_ref(ng, i8));
            chk("cuflt-hi", i8, ng, spu_cuflt(ng, i8), spu_cuflt_ref(ng, i8));
            n += 4;
        }
        n++;
    }
    printf("%s: %ld checks, %ld failures\n",
#if defined(__AVX512F__)
           "AVX-512 build",
#else
           "AVX2 build",
#endif
           n, fails);
    return fails != 0;
}
