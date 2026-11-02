/* Check edge_ref.c against the NHL_EDGE_DUMP corpus: stage tables + final output bytes.
 * usage: ref_test <seg.bin>...   REF_V=1 prints the first bad vertex of each segment */
#include "edge_ref.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <intrin.h>

typedef struct { char tag[5]; uint32_t st, len; uint8_t* d; } chunk_t;
static chunk_t* load(const char* path, unsigned* n)
{
    FILE* f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t* b = malloc(sz); fread(b, 1, sz, f); fclose(f);
    chunk_t* c = calloc(256, sizeof *c); unsigned k = 0; long o = 0;
    while (o + 12 <= sz && k < 256) {
        memcpy(c[k].tag, b + o, 4); c[k].tag[4] = 0;
        memcpy(&c[k].st, b + o + 4, 4); memcpy(&c[k].len, b + o + 8, 4);
        c[k].d = b + o + 12; o += 12 + c[k].len; k++;
    }
    *n = k; return c;
}
static const uint8_t* snap(chunk_t* c, unsigned n, unsigned st)
{
    for (unsigned i = 0; i < n; i++) if (!strcmp(c[i].tag, "SNAP") && c[i].st == st) return c[i].d;
    return NULL;
}
#define REG(d, r) (*(const uint32_t*)((d) + (r) * 16))
static int64_t ulps(float a, float b)
{
    union { float f; int32_t i; } x, y; x.f = a; y.f = b;
    int64_t p = x.i < 0 ? (int64_t)(int32_t)0x80000000 - x.i : x.i, q = y.i < 0 ? (int64_t)(int32_t)0x80000000 - y.i : y.i;
    return p > q ? p - q : q - p;
}
int main(int argc, char** argv)
{
    static float pos[3 * 65536], nrm[3 * 65536], tan[3 * 65536];
    static uint8_t out[20 * 65536];
    int verbose = getenv("REF_V") != NULL;
    uint64_t verts = 0, big_pos = 0, big_n = 0, big_t = 0, out_exact = 0, out_total = 0, pack_hist[4] = { 0 }, pos_bytes_bad = 0;
    int64_t max_pos = 0; double max_n = 0, max_t = 0, max_abs = 0; unsigned segs = 0, skipped = 0;
    unsigned long long tsc = 0;
    for (int a = 1; a < argc; a++) {
        unsigned n; chunk_t* c = load(argv[a], &n); if (!c) continue;
        const uint8_t* rec = c[0].d, * s3 = snap(c, n, 3), * s4 = snap(c, n, 4), * s5 = snap(c, n, 5), * s10 = snap(c, n, 10);
        if (!s3 || !s4 || !s5 || !s10) { skipped++; continue; }
        const uint8_t* ls3 = s3 + 2048, * ls4 = s4 + 2048, * ls5 = s5 + 2048, * ls10 = s10 + 2048;
        uint32_t ctx = REG(s3, 3);
        edge_seg s;
        s.vtx = ls3 + REG(s3, 4); s.fixed = ls3 + REG(s3, 5);
        s.palette = ls4 + REG(s4, 4); s.weights = ls4 + REG(s4, 6);
        s.nv = (unsigned)rec[8] << 8 | rec[9];
        s.flavor = rec[6] & 15; s.outfmt = rec[4];
        if (memcmp(ls3 + edge_rd32(ls3 + ctx + 0x6C), k_edge_desc, 0x30)) { skipped++; continue; }
        unsigned bytes = 0;
        unsigned long long best = ~0ull;
        for (int r = 0; r < 5; r++) {
            unsigned long long t0 = __rdtsc();
            bytes = edge_ref_vertices(&s, out, pos, nrm, tan);
            t0 = __rdtsc() - t0; if (t0 < best) best = t0;
        }
        tsc += best;
        segs++; verts += s.nv;
        const uint8_t* tp = ls5 + edge_rd32(ls5 + ctx + 0x5C), * tn = ls5 + edge_rd32(ls5 + ctx + 0x60), * tt = ls5 + edge_rd32(ls5 + ctx + 0x64);
        int printed = 0;
        for (unsigned v = 0; v < s.nv; v++)
            for (int k = 0; k < 3; k++) {
                float gp = edge_rdf(tp + v * 16 + k * 4), gn = edge_rdf(tn + v * 16 + k * 4), gt = edge_rdf(tt + v * 16 + k * 4);
                int64_t u = ulps(pos[v * 3 + k], gp); if (u > max_pos) max_pos = u; if (u > 16) big_pos++;
                double dp = fabs(pos[v * 3 + k] - gp); if (dp > max_abs) max_abs = dp;
                double dn = fabs(nrm[v * 3 + k] - gn), dt = fabs(tan[v * 3 + k] - gt);
                if (dn > max_n) max_n = dn;
                if (dt > max_t) max_t = dt;
                if (dn > 1e-3) big_n++;
                if (dt > 1e-3) big_t++;
                if (verbose && !printed && (u > 16 || dn > 1e-3 || dt > 1e-3)) {
                    printed = 1;
                    printf("%s v%u.%d flavor %u: pos %f/%f nrm %f/%f tan %f/%f\n", argv[a], v, k, s.flavor, pos[v * 3 + k], gp, nrm[v * 3 + k], gn, tan[v * 3 + k], gt);
                }
            }
        const uint8_t* go = ls10 + edge_rd32(ls10 + ctx + 0x54);
        unsigned stride = s.outfmt ? 20 : 12;
        out_total += bytes;
        for (unsigned v = 0; v < s.nv; v++) {
            const uint8_t* A = out + v * stride, * B = go + v * stride;
            for (int k = 0; k < 3; k++) { int64_t u = ulps(edge_rdf(A + k * 4), edge_rdf(B + k * 4)); if (u > 16) pos_bytes_bad++; }
            if (!s.outfmt) continue;
            for (int w = 0; w < 2; w++) {
                uint32_t p = edge_rd32(A + 12 + w * 4), q = edge_rd32(B + 12 + w * 4);
                if (p == q) { out_exact += 4; pack_hist[0]++; continue; }
                int dx = (int)((p ^ 0x400) & 0x7FF) - (int)((q ^ 0x400) & 0x7FF);
                int dy = (int)(((p >> 11) ^ 0x400) & 0x7FF) - (int)(((q >> 11) ^ 0x400) & 0x7FF);
                int dz = (int)(((p >> 22) ^ 0x200) & 0x3FF) - (int)(((q >> 22) ^ 0x200) & 0x3FF);
                int m = abs(dx); if (abs(dy) > m) m = abs(dy); if (abs(dz) > m) m = abs(dz);
                pack_hist[m >= 3 ? 3 : m]++;
            }
        }
    }
    printf("%u segments (%u skipped), %llu verts, %.0f kcyc/seg, %.1f cyc/vert\n", segs, skipped, (unsigned long long)verts, tsc / 1000.0 / (segs ? segs : 1), (double)tsc / (verts ? verts : 1));
    printf("pos: max %lld ulp, max abs err %.7f, %llu comps > 16 ulp | output pos comps > 16 ulp: %llu\n", (long long)max_pos, max_abs, (unsigned long long)big_pos, (unsigned long long)pos_bytes_bad);
    printf("normal: max err %.6f, %llu comps > 1e-3 | tangent: max err %.6f, %llu comps > 1e-3\n", max_n, (unsigned long long)big_n, max_t, (unsigned long long)big_t);
    printf("packed words: exact %llu, off by 1 LSB %llu, 2 LSB %llu, >=3 LSB %llu\n", (unsigned long long)pack_hist[0], (unsigned long long)pack_hist[1], (unsigned long long)pack_hist[2], (unsigned long long)pack_hist[3]);
    return 0;
}
