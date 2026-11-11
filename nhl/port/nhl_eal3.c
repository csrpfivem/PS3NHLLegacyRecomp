/* NHL: recon + corpus capture for EAAudioCore's EALayer3 decoder SPU job
 * (spudecealayer3.elf, body 0x01DE9800, relocatable). Step 0 of replacing it natively
 * (NEXT_STEPS "DE-EMULATION"). Hooks in the lifted code come from tools/nhl_hooks.py (EAL3).
 *
 * Job layout (offsets from the job's load base): 0x9E18 = per-granule hybrid + synthesis
 * driver, per channel: 0x8C40, 0x90A8 (IMDCT), a transpose loop, 0xA998 (polyphase
 * synthesis, calls 0xA3A8 = DCT).
 *
 *   NHL_EAL3_LOG=1        [EAL3] every 10 s: granules, channel passes, cycles per stage
 *   NHL_EAL3_DUMP=<dir>   <dir>/gran_NNNN.bin for a spread of granules; chunks
 *                         { u32 tag 'SNAP', u32 stage, u32 len, u32 code_base, 128 GPRs (2 KB),
 *                         256 KB LS } at every stage boundary (stage numbers = ES_* below).
 * Off by default: one predicted branch per hook. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <x86intrin.h>
#endif

enum { ES_NONE, ES_GRAN, ES_8C40, ES_8C40_END, ES_IMDCT, ES_IMDCT_END, ES_SYNTH, ES_SYNTH_END,
       ES_GRAN_END, ES_DCT, ES_N };
static const char* const k_name[ES_N] = { "-", "granule", "8C40", "-", "imdct", "-", "synth", "-", "-", "dct", };

#define DUMP_FIRST 6
#define DUMP_EVERY 61
#define DUMP_MAX   48

static int s_mode = -1;                 /* bit 0 log, bit 1 dump */
static char s_dir[260];
static long long s_cycles[ES_N], s_calls[ES_N];
static volatile long s_lock;
static long long s_gran, s_dumped;
static _Thread_local unsigned long long t_t0[ES_N];
static _Thread_local FILE* t_dump;
static _Thread_local int t_snaps;

static int eal3_mode(void)
{
    if (s_mode < 0) {
        const char* l = getenv("NHL_EAL3_LOG");
        const char* d = getenv("NHL_EAL3_DUMP");
        int m = (l && *l == '1') ? 1 : 0;
        if (d && *d) { m |= 2; snprintf(s_dir, sizeof s_dir, "%s", d); }
        s_mode = m;
    }
    return s_mode;
}

static void eal3_report(void)
{
    static long long lc[ES_N], ln[ES_N];
    char line[400];
    int p = snprintf(line, sizeof line, "[EAL3] t=%ld", (long)(clock() / CLOCKS_PER_SEC));
    for (int i = 1; i < ES_N; i++) {
        if (k_name[i][0] == '-') continue;
        const long long n = s_calls[i] - ln[i], c = s_cycles[i] - lc[i];
        p += snprintf(line + p, sizeof line - p, " | %s %lld calls %.1f Mcyc %.1f kcyc/call",
                      k_name[i], n, c / 1e6, n ? c / 1e3 / n : 0.0);
        ln[i] = s_calls[i]; lc[i] = s_cycles[i];
    }
    fprintf(stderr, "%s\n", line);
}

static void eal3_snap(const void* regs, const uint8_t* ls, uint32_t code_base, unsigned stage)
{
    if (!t_dump || t_snaps >= 40) return;
    uint32_t h[4]; memcpy(&h[0], "SNAP", 4); h[1] = stage; h[2] = 4 + 128 * 16 + 0x40000; h[3] = code_base;
    fwrite(h, 4, 4, t_dump);
    fwrite(regs, 1, 128 * 16, t_dump);
    fwrite(ls, 1, 0x40000, t_dump);
    fflush(t_dump);
    t_snaps++;
}

void nhl_eal3_stage(const void* regs, const uint8_t* ls, uint32_t code_base, unsigned stage)
{
    const int mode = eal3_mode();
    if (!mode) return;
    const unsigned long long now = __rdtsc();
    /* an "end" stage closes the interval its partner opened */
    const unsigned open = stage == ES_8C40_END ? ES_8C40 : stage == ES_IMDCT_END ? ES_IMDCT :
                          stage == ES_SYNTH_END ? ES_SYNTH : stage == ES_GRAN_END ? ES_GRAN : 0;
    if (open) {
        if (t_t0[open]) { __atomic_fetch_add(&s_cycles[open], (long long)(now - t_t0[open]), __ATOMIC_RELAXED); t_t0[open] = 0; }
    } else if (stage == ES_DCT) {
        __atomic_fetch_add(&s_calls[ES_DCT], 1, __ATOMIC_RELAXED);
    } else {
        __atomic_fetch_add(&s_calls[stage], 1, __ATOMIC_RELAXED);
    }
    if (stage == ES_GRAN) {
        int report = 0; long long n, d = -1;
        static clock_t s_last;
        while (__atomic_exchange_n(&s_lock, 1, __ATOMIC_ACQUIRE)) _mm_pause();
        n = s_gran++;
        if ((mode & 2) && s_dumped < DUMP_MAX && (n < DUMP_FIRST || n % DUMP_EVERY == 0)) d = s_dumped++;
        if ((mode & 1) && (n & 63) == 0) { const clock_t c = clock(); if (c - s_last >= 10 * CLOCKS_PER_SEC) { s_last = c; report = 1; } }
        if (report) eal3_report();
        __atomic_store_n(&s_lock, 0, __ATOMIC_RELEASE);
        if (t_dump) { fclose(t_dump); t_dump = NULL; }
        if (d >= 0) {
            char path[300];
            snprintf(path, sizeof path, "%s/gran_%04lld.bin", s_dir, d);
            t_dump = fopen(path, "wb");
            t_snaps = 0;
            if (d == 0 || d + 1 == DUMP_MAX) fprintf(stderr, "[EAL3-DUMP] %s granule file %lld (granule %lld)\n", d ? "last" : "first", d, n);
        }
    }
    if (t_dump && stage != ES_DCT) {
        eal3_snap(regs, ls, code_base, stage);
        if (stage == ES_GRAN_END) { fclose(t_dump); t_dump = NULL; }
    }
    if (!open && stage != ES_DCT) t_t0[stage] = __rdtsc();   /* exclude the snapshot's own cost */
}
