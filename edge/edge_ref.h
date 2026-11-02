/* C reference for NHL's Sony Edge vertex path (SPU program 01DBF980), rebuilt from its code:
 * decompress A048/7088 -> skin A0F8 -> compress 4C70. Only the one custom input layout the game
 * uses: pos 3x u16 fixed 2.14 + per-segment s32 offsets, normal + tangent 3-byte unit vectors,
 * stride 12. Input data is big-endian guest memory; floats go out as host floats. */
#pragma once
#include <stdint.h>

/* the stream descriptor every captured segment carries (0x30 bytes) */
static const uint8_t k_edge_desc[0x30] = {
    0x03,0x0C,0x04,0x00, 0,0,0,0,
    0x00,0x08,0x03,0x01, 0x06,0x00,0x18,0x00,
    0x06,0x09,0x03,0x02, 0x03,0x02,0x00,0x00,
    0x09,0x09,0x03,0x03, 0x03,0x0E,0x00,0x00,
    0x02,0x0E,0x02,0x0E, 0x02,0x0E,0,0, 0,0,0,0, 0,0,0,0 };

typedef struct {
    const uint8_t* vtx;     /* nv * 12 bytes */
    const uint8_t* fixed;   /* 3 x s32 */
    const uint8_t* weights; /* nv * 8: w0 i0 w1 i1 w2 i2 w3 i3 */
    const uint8_t* palette; /* 3x4 row-major BE floats, 0x30 each */
    unsigned nv;
    unsigned flavor;        /* 2 = uniform scale, 3 = non-uniform (cofactor normals) */
    unsigned outfmt;        /* 0 = pos, 1 = pos + normal + tangent */
} edge_seg;

static inline uint32_t edge_rd32(const uint8_t* p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static inline float edge_rdf(const uint8_t* p) { union { uint32_t u; float f; } c; c.u = edge_rd32(p); return c.f; }
static inline void edge_wr32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

/* out: (12 or 20) * nv bytes, big-endian as the RSX reads it. pos/nrm/tan: optional float3 * nv
 * (skinned, normalized), for checking. Returns bytes written. */
unsigned edge_ref_vertices(const edge_seg* s, uint8_t* out, float* pos, float* nrm, float* tan);
