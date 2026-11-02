#include "edge_ref.h"
#include <math.h>
#include <string.h>

static inline float asf(uint32_t u) { union { uint32_t u; float f; } c; c.u = u; return c.f; }

/* format 9: 10+10 bit components of the two smallest axes, 4 bits = axis of the dropped one +
 * its sign + w. The SPU builds 1.0 + k*2^-23 and does a FUSED mul-add with 0x46352E7C / 0xC6353150
 * (without the fuse that rounds to 2^-10): k * c1 + c0 is the same value, stable on any FPU. */
#define EDGE_UV_C1 ((float)(11595.62109375 / 8388608.0))
#define EDGE_UV_C0 ((float)(11595.62109375 - 11596.328125))
static void unit_vector(const uint8_t* b, float* v)
{
    uint32_t X = ((uint32_t)b[0] << 8 | b[1]) >> 6 & 0x3FF;
    uint32_t Y = ((uint32_t)b[1] << 8 | b[2]) >> 4 & 0x3FF;
    uint32_t idx = b[2] & 15;
    float x = (float)X * EDGE_UV_C1 + EDGE_UV_C0;
    float y = (float)Y * EDGE_UV_C1 + EDGE_UV_C0;
    float r = 1.0f - x * x - y * y;
    float z = r > 0.0f ? sqrtf(r) : 0.0f;
    if (!(idx & 1)) z = -z;
    switch (idx >> 2) {
    case 0: v[0] = z; v[1] = x; v[2] = y; break;
    case 1: v[0] = x; v[1] = z; v[2] = y; break;
    default: v[0] = x; v[1] = y; v[2] = z; break;
    }
}

static inline void normalize3(float* v)
{
    float l = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
    if (l > 0.0f) { l = 1.0f / sqrtf(l); v[0] *= l; v[1] *= l; v[2] *= l; }
}

/* X11Y11Z10N, floor of the scaled value (SPU: cflts truncates at 2^31, then logical shifts) */
static uint32_t pack_x11y11z10(const float* v)
{
    int32_t x = (int32_t)floorf((v[0] * asf(0x3F7FE000) + asf(0xBA000000)) * 1024.0f);
    int32_t y = (int32_t)floorf((v[1] * asf(0x3F7FE000) + asf(0xBA000000)) * 1024.0f);
    int32_t z = (int32_t)floorf((v[2] * asf(0x3F7FC000) + asf(0xBA800000)) * 512.0f);
    return ((uint32_t)x & 0x7FF) | ((uint32_t)y & 0x7FF) << 11 | ((uint32_t)z & 0x3FF) << 22;
}

unsigned edge_ref_vertices(const edge_seg* s, uint8_t* out, float* pos, float* nrm, float* tan)
{
    const float w255 = asf(0x3B808080);
    int32_t off[3];
    for (int c = 0; c < 3; c++) off[c] = (int32_t)edge_rd32(s->fixed + c * 4);
    unsigned stride = s->outfmt ? 20 : 12;
    for (unsigned v = 0; v < s->nv; v++) {
        const uint8_t* in = s->vtx + v * 12;
        const uint8_t* w = s->weights + v * 8;
        float p[3], n[3], t[3], m[12];
        for (int c = 0; c < 3; c++)
            p[c] = (float)((int32_t)((uint32_t)in[c * 2] << 8 | in[c * 2 + 1]) + off[c]) * (1.0f / 16384.0f);
        unit_vector(in + 6, n);
        unit_vector(in + 9, t);
        memset(m, 0, sizeof m);
        for (int b = 0; b < 4; b++) {
            float f = (float)w[b * 2] * w255;
            const uint8_t* mp = s->palette + (unsigned)w[b * 2 + 1] * 0x30;
            for (int i = 0; i < 3; i++)
                for (int j = 0; j < 4; j++)
                    m[i * 4 + j] += f * edge_rdf(mp + i * 16 + j * 4);
        }
        float q[3], nn[3], tt[3];
        for (int i = 0; i < 3; i++) {
            const float* r = m + i * 4;
            q[i] = r[0] * p[0] + r[1] * p[1] + r[2] * p[2] + r[3];
            tt[i] = r[0] * t[0] + r[1] * t[1] + r[2] * t[2];
        }
        if (s->flavor == 3) {
            const float* r0 = m, * r1 = m + 4, * r2 = m + 8;
            float c[9] = {
                r1[1] * r2[2] - r1[2] * r2[1], r1[2] * r2[0] - r1[0] * r2[2], r1[0] * r2[1] - r1[1] * r2[0],
                r2[1] * r0[2] - r2[2] * r0[1], r2[2] * r0[0] - r2[0] * r0[2], r2[0] * r0[1] - r2[1] * r0[0],
                r0[1] * r1[2] - r0[2] * r1[1], r0[2] * r1[0] - r0[0] * r1[2], r0[0] * r1[1] - r0[1] * r1[0] };
            for (int i = 0; i < 3; i++) nn[i] = c[i * 3] * n[0] + c[i * 3 + 1] * n[1] + c[i * 3 + 2] * n[2];
        } else {
            for (int i = 0; i < 3; i++) nn[i] = m[i * 4] * n[0] + m[i * 4 + 1] * n[1] + m[i * 4 + 2] * n[2];
        }
        normalize3(nn);
        normalize3(tt);
        if (pos) memcpy(pos + v * 3, q, 12);
        if (nrm) memcpy(nrm + v * 3, nn, 12);
        if (tan) memcpy(tan + v * 3, tt, 12);
        if (out) {
            uint8_t* o = out + v * stride;
            for (int c = 0; c < 3; c++) { union { float f; uint32_t u; } u; u.f = q[c]; edge_wr32(o + c * 4, u.u); }
            if (s->outfmt) { edge_wr32(o + 12, pack_x11y11z10(nn)); edge_wr32(o + 16, pack_x11y11z10(tt)); }
        }
    }
    return s->nv * stride;
}
