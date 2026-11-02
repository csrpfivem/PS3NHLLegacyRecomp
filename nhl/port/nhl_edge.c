/* NHL: recon for EA's Sony Edge Geometry SPU job (program 0x01DBF980, LOCAL_PATCHES #268).
 *
 * The program takes a table of 0x180-byte draw records (batches of 5) and runs the Edge
 * pipeline on each one from func 0x1D0: decompress, skin, blend, cull, compress, output
 * to the per-SPU ring, fill the RSX command-buffer hole. Hooks in the lifted code
 * (tools/nhl_hooks.py, EDGE*) call these with NHL_EDGE_LOG=1:
 *   [EDGE]      per 10 s: segments, vertices, indices, cycles per stage
 *   [EDGE-KEY]  format mix: the first 8 bytes of EdgeGeomSpuConfigInfo + rec+0x11C
 *   [EDGE-REC]  raw record, the first few of each key
 *   [EDGE-OUT]  output PUTs by kind and 1 MB bucket, with RSX vertex-array draws there
 * Off by default: the hooks cost one predicted branch per stage. */
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

enum { ST_NONE, ST_FETCH, ST_INIT, ST_DECOMP, ST_A0F8, ST_7010, ST_CULL, ST_ALLOC, ST_IDX,
       ST_COMPRESS, ST_VTX, ST_HOLE, ST_HOLE_END, ST_N };
static const char* const k_stage[ST_N] = { "other", "fetch", "init", "decomp", "A0F8", "7010", "cull",
                                           "alloc", "idxout", "compress", "vtxout", "hole", "holeend" };

#define KEYS 128
#define REC_SIZE 0x180
#define RECS_PER_KEY 3
typedef struct { uint8_t cfg[8]; uint32_t skin; } Key;
static Key       s_key[KEYS];
static uint8_t   s_used[KEYS];
static long long s_segs[KEYS], s_verts[KEYS], s_idx[KEYS];
static int       s_dumped[KEYS];
static long long s_cycles[ST_N], s_total_segs, s_total_verts, s_total_idx;
static long long s_out[3][4096];
static volatile long s_lock;
static int s_on = -1;
static _Thread_local unsigned t_stage;
static _Thread_local unsigned long long t_since;

extern long rsx_vamap_total(int bucket);   /* runtime/spu/spu_channels.c, needs SPU_PUTMAP=1 */

static inline int edge_on(void)
{
    if (s_on < 0) { const char* e = getenv("NHL_EDGE_LOG"); s_on = (e && *e == '1') ? 1 : 0; }
    return s_on;
}

static inline void edge_lock(void) { while (__atomic_exchange_n(&s_lock, 1, __ATOMIC_ACQUIRE)) { _mm_pause(); } }
static inline void edge_unlock(void) { __atomic_store_n(&s_lock, 0, __ATOMIC_RELEASE); }

static inline uint32_t be32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static inline uint32_t be16(const uint8_t* p) { return ((uint32_t)p[0] << 8) | p[1]; }

static void edge_report(void)
{
    static long long last_cycles[ST_N], last_segs, last_verts, last_idx;
    long long total = 0;
    char line[512];
    int p;
    for (int i = 0; i < ST_N; i++) total += s_cycles[i] - last_cycles[i];
    p = snprintf(line, sizeof line, "[EDGE] t=%ld segs %lld verts %lld idx %lld | Gcycles %.2f:",
                 (long)(clock() / CLOCKS_PER_SEC), s_total_segs - last_segs, s_total_verts - last_verts,
                 s_total_idx - last_idx, total / 1e9);
    for (int i = 0; i < ST_N; i++) {
        p += snprintf(line + p, sizeof line - p, " %s %.1f%%", k_stage[i],
                      total ? 100.0 * (s_cycles[i] - last_cycles[i]) / total : 0.0);
        last_cycles[i] = s_cycles[i];
    }
    last_segs = s_total_segs; last_verts = s_total_verts; last_idx = s_total_idx;
    fprintf(stderr, "%s\n", line);
    for (int k = 0; k < KEYS; k++) if (s_used[k]) {
        const Key* e = &s_key[k];
        fprintf(stderr, "[EDGE-KEY] flags %02X hole %02X in %02X/%02X out %02X delta %02X idx+skin %02X mtx %02X skin11C %08X | segs %lld verts %lld idx %lld\n",
                e->cfg[0], e->cfg[1], e->cfg[2], e->cfg[3], e->cfg[4], e->cfg[5], e->cfg[6], e->cfg[7],
                e->skin, s_segs[k], s_verts[k], s_idx[k]);
    }
    for (int kind = 0; kind < 3; kind++) for (int b = 0; b < 4096; b++) if (s_out[kind][b]) {
        fprintf(stderr, "[EDGE-OUT] %s %03X00000 %.1f MB total, RSX va draws %ld\n",
                kind == 0 ? "vtx" : kind == 1 ? "idx" : "other", b, s_out[kind][b] / 1048576.0, rsx_vamap_total(b));
    }
}

/* NHL_EDGE_SKIP=1: the hooks skip decompress/skin/0x7010 and report zero triangles from
 * cull, so every segment takes Edge's own empty-hole path. Skinned geometry disappears,
 * the rest of the frame runs: the upper bound of what removing this job can give. */
int nhl_edge_skip(void)
{
    static int s_skip = -1;
    if (s_skip < 0) { const char* e = getenv("NHL_EDGE_SKIP"); s_skip = (e && *e == '1') ? 1 : 0; }
    return s_skip;
}

/* Entered at each stage: everything since the last mark belongs to the previous stage. */
void nhl_edge_mark(unsigned stage)
{
    if (!edge_on()) return;
    const unsigned long long now = __rdtsc();
    if (t_since) __atomic_fetch_add(&s_cycles[t_stage], (long long)(now - t_since), __ATOMIC_RELAXED);
    t_since = now;
    t_stage = stage;
}

/* func 0x1D0: r3 = LS address of the record. */
static int dump_on(void);
static void dump_seg(const uint8_t* r);
void nhl_edge_seg(const uint8_t* ls, uint32_t lsa)
{
    if (dump_on()) dump_seg(ls + (lsa & 0x3FFFF));
    if (!edge_on()) return;
    nhl_edge_mark(ST_NONE);
    const uint8_t* r = ls + (lsa & 0x3FFFF);
    Key key;
    memcpy(key.cfg, r, 8);
    key.skin = be32(r + 0x11C);
    const uint32_t verts = be16(r + 8), idx = be16(r + 10);
    uint32_t h = 2166136261u;
    for (int i = 0; i < 8; i++) h = (h ^ key.cfg[i]) * 16777619u;
    h = (h ^ (key.skin != 0)) * 16777619u;
    int dump = 0, report = 0, k = (int)(h % KEYS);
    static long long s_next;
    edge_lock();
    for (int n = 0; n < KEYS; n++, k = (k + 1) % KEYS) {
        if (!s_used[k]) { s_used[k] = 1; s_key[k] = key; break; }
        if (!memcmp(s_key[k].cfg, key.cfg, 8) && (s_key[k].skin != 0) == (key.skin != 0)) break;
    }
    s_segs[k]++; s_verts[k] += verts; s_idx[k] += idx;
    s_total_segs++; s_total_verts += verts; s_total_idx += idx;
    if (s_dumped[k] < RECS_PER_KEY) { s_dumped[k]++; dump = 1; }
    if (s_total_segs >= s_next) {
        static clock_t s_last;
        const clock_t now = clock();
        s_next = s_total_segs + 512;
        if (now - s_last >= 10 * CLOCKS_PER_SEC) { s_last = now; report = 1; }
    }
    if (dump) {
        char hex[REC_SIZE * 3 + 64];
        int p = 0;
        for (int i = 0; i < REC_SIZE; i += 4) p += snprintf(hex + p, sizeof hex - p, "%08X%s", be32(r + i), (i & 31) == 28 ? " | " : " ");
        fprintf(stderr, "[EDGE-REC] %s\n", hex);
    }
    if (report) edge_report();
    edge_unlock();
}

/* ---- NHL_EDGE_DUMP=<dir> (Edge GPU project, step 0): a corpus for rebuilding the job ----
 * For up to DUMP_PER_KEY segments of each format key (DUMP_MAX in all), one file per
 * segment, <dir>/seg_NNNN.bin, of chunks { u32 tag, u32 stage, u32 len, bytes }:
 *   'REC ' stage 0     the 0x180-byte record (LS copy at func 0x1D0)
 *   'SNAP' stage N     128 GPRs (2 KB, host order as in spu_context) + the whole 256 KB LS,
 *                      at the entry of each pipeline stage (N = the ST_* number)
 *   'PUT ' stage kind  u32 lsa, ea, size, lr + the bytes Edge DMAs out (0 vtx, 1 idx, 2 other)
 * A segment's file closes when the same SPU thread starts the next segment. */
#define DUMP_PER_KEY 12
#define DUMP_MAX 240
static int s_dump_on = -1;
static char s_dump_dir[260];
static int s_dump_key[KEYS], s_dump_n;
static _Thread_local FILE* t_dump;
static _Thread_local int t_snaps;   /* SNAP chunks in this segment (capped: stages can repeat) */

static int dump_on(void)
{
    if (s_dump_on < 0) {
        const char* e = getenv("NHL_EDGE_DUMP");
        s_dump_on = (e && *e) ? 1 : 0;
        if (s_dump_on) snprintf(s_dump_dir, sizeof s_dump_dir, "%s", e);
    }
    return s_dump_on;
}

static void dump_chunk(const char* tag, uint32_t stage, const void* a, uint32_t na, const void* b, uint32_t nb)
{
    uint32_t h[3]; memcpy(&h[0], tag, 4); h[1] = stage; h[2] = na + nb;
    fwrite(h, 4, 3, t_dump);
    if (na) fwrite(a, 1, na, t_dump);
    if (nb) fwrite(b, 1, nb, t_dump);
}

static void dump_seg(const uint8_t* r)
{
    if (t_dump) { fclose(t_dump); t_dump = NULL; }
    uint32_t h = 2166136261u;
    for (int i = 0; i < 8; i++) h = (h ^ r[i]) * 16777619u;
    h = (h ^ (be32(r + 0x11C) != 0)) * 16777619u;
    const int k = (int)(h % KEYS);
    int n = -1;
    edge_lock();
    if (s_dump_n < DUMP_MAX && s_dump_key[k] < DUMP_PER_KEY) { s_dump_key[k]++; n = s_dump_n++; }
    edge_unlock();
    if (n < 0) return;
    char path[300];
    snprintf(path, sizeof path, "%s/seg_%04d.bin", s_dump_dir, n);
    t_dump = fopen(path, "wb");
    if (!t_dump) return;
    t_snaps = 0;
    dump_chunk("REC ", 0, r, REC_SIZE, NULL, 0);
    if (n == 0 || n + 1 == DUMP_MAX) fprintf(stderr, "[EDGE-DUMP] %s segment %d\n", n ? "last" : "first", n);
}

/* Stage entry with the SPU context: snapshot regs + LS while this segment is being dumped. */
void nhl_edge_snap(const void* regs, const uint8_t* ls, unsigned stage)
{
    if (!t_dump || t_snaps >= 32) return;
    t_snaps++;
    dump_chunk("SNAP", stage, regs, 128 * 16, ls, 0x40000);
    fflush(t_dump);
}

/* func 0xB0F8: r3 = LS source, r4 slot 1 = EA, r5 = size, r0 = return address. */
void nhl_edge_put(const uint8_t* ls, uint32_t lsa, uint32_t ea, uint32_t size, uint32_t lr)
{
    const int kind = lr == 0x2D28 ? 0 : lr == 0x2B38 ? 1 : 2;
    if (t_dump && size <= 0x4000) {
        uint32_t m[4] = { lsa, ea, size, lr };
        uint8_t buf[0x4000];
        for (uint32_t i = 0; i < size; i++) buf[i] = ls[(lsa + i) & 0x3FFFF];
        dump_chunk("PUT ", (uint32_t)kind, m, sizeof m, buf, size);
        fflush(t_dump);
    }
    if (!edge_on()) return;
    __atomic_fetch_add(&s_out[kind][ea >> 20], (long long)size, __ATOMIC_RELAXED);
}
