/* nhl_spu_isa.c -- NHL: picks the AVX-512 or AVX2 build of the lifted SPU code (#257).
 *
 * The exe carries the lifted SPU code twice (CMake NHL_SPU_DUAL, wrappers from
 * tools/nhl_spu_dual.py). The registration constructors in spu_workloads.c call
 * nhl_spu_isa_avx512() and only the matching set registers. This file is built
 * for the baseline target: it runs before anything knows the CPU can do more.
 *
 * AVX-512 = what -march=x86-64-v4 assumes: F, DQ, CD, BW, VL, with the OS saving
 * the opmask and ZMM state (XCR0). Everything else (Steam Deck / Zen 2, most Intel
 * desktop chips) gets AVX2. The rest of the exe needs x86-64-v3 anyway (the lifted
 * PPU code uses FMA), so a CPU without it gets a message instead of a crash.
 * NHL_SPU_ISA=avx2 forces the AVX2 set (testing the Deck path here); =avx512 is
 * honoured only if the CPU has it. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <intrin.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

__attribute__((target("xsave"))) static unsigned long long nhl_xcr0(void)
{
    return _xgetbv(0);
}

static int nhl_cpu_check(int* has_v3)
{
    int r[4], r7[4] = {0}, rx[4] = {0};
    __cpuid(r, 0);
    int max_leaf = r[0];
    __cpuid(r, 1);
    if (max_leaf >= 7) __cpuidex(r7, 7, 0);
    __cpuid(rx, 0x80000000);
    if ((unsigned)rx[0] >= 0x80000001u) __cpuid(rx, 0x80000001); else memset(rx, 0, sizeof rx);

    const unsigned ecx1 = (unsigned)r[2], ebx7 = (unsigned)r7[1], ecxx = (unsigned)rx[2];
    const int osxsave = (ecx1 >> 27) & 1;
    const unsigned long long xcr0 = osxsave ? nhl_xcr0() : 0;
    const int os_avx = (xcr0 & 0x6) == 0x6;           /* XMM + YMM state */
    const int os_avx512 = (xcr0 & 0xE6) == 0xE6;      /* + opmask, ZMM_Hi256, Hi16_ZMM */

    const unsigned v3_ecx1 = (1u << 12) | (1u << 19) | (1u << 20) | (1u << 22) | (1u << 23) | (1u << 28) | (1u << 29);
    const unsigned v3_ebx7 = (1u << 3) | (1u << 5) | (1u << 8);   /* BMI1, AVX2, BMI2 */
    *has_v3 = os_avx && (ecx1 & v3_ecx1) == v3_ecx1 && (ebx7 & v3_ebx7) == v3_ebx7 && ((ecxx >> 5) & 1);

    const unsigned v4_ebx7 = (1u << 16) | (1u << 17) | (1u << 28) | (1u << 30) | (1u << 31);  /* F DQ CD BW VL */
    return *has_v3 && os_avx512 && (ebx7 & v4_ebx7) == v4_ebx7;
}

int nhl_spu_isa_avx512(void)
{
    static int s_pick = -1;
    if (s_pick >= 0) return s_pick;

    int has_v3 = 0;
    const int has_v4 = nhl_cpu_check(&has_v3);
    if (!has_v3) {
        MessageBoxA(NULL,
            "NHL Legacy needs a CPU with AVX2 and FMA (Intel Haswell / AMD Zen or newer).",
            "NHL Legacy", MB_OK | MB_ICONERROR);
        ExitProcess(1);
    }
    const char* e = getenv("NHL_SPU_ISA");
    const char* why = has_v4 ? "cpu has AVX-512" : "cpu has no AVX-512";
    s_pick = has_v4;
    if (e && _stricmp(e, "avx2") == 0) {
        s_pick = 0;
        why = "NHL_SPU_ISA=avx2";
    } else if (e && _stricmp(e, "avx512") == 0 && !has_v4) {
        why = "NHL_SPU_ISA=avx512 ignored, cpu has no AVX-512";
    }
    fprintf(stderr, "[SPU] code: %s (%s)\n", s_pick ? "AVX-512" : "AVX2", why);
    /* #280: the AVX2 float path runs with MXCSR round-toward-zero (spu_float_thread_init);
     * #282: always -- the AVX2 copy (NHL_SPU_VARIANT 2) is built with RZ as a constant */
    extern int g_spu_float_rz_ok;
    g_spu_float_rz_ok = s_pick ? 0 : 2;
    return s_pick;
}
