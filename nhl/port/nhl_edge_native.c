/* NHL: native AVX2 decompress + skin + cull + compress for the Sony Edge job (SPU program 0x01DBF980,
 * LOCAL_PATCHES #292). NHL_EDGE_NATIVE=1 turns it on.
 *
 * Replaces funcs 0xA048 (decompress), 0xA0F8 (skin), 0x5C08 (cull) and 0x4C70 (compress), about 95%
 * of the job, through hooks in the lifted code (tools/nhl_hooks.py, EDGE*). Index decode, ring alloc
 * and the command hole stay lifted. Rebuilt from the job's code and checked against the NHL_EDGE_DUMP
 * corpus (_port_work/edge: edge_ref.c = plain C version, bench\harness.c, cullsim.py).
 *
 * Handles the one layout the game uses (k_desc, flavors 2/3, output formats 0/1, no custom cull
 * transform); anything else stays on the lifted code and logs an [EDGE-NATIVE] line. Stage 3 only
 * checks the segment and does the bookkeeping; all the work happens at stage 4, because skinned
 * positions are what the cull needs: it reads them from the first uniform table in LS, which is
 * also where a lifted cull would find them. Normals and tangents are only read by compress, so they
 * stay in a per-thread buffer, already packed.
 *
 * Results differ from the SPU's by rounding only: its square roots are table estimates, so about
 * 5% of the packed normals are 1 LSB off, and its 1/w is an estimate too, so now and then a triangle
 * on the edge of a test goes the other way (5 of 216 captured segments, one triangle each). */
#include <immintrin.h>
#include <malloc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LS_MASK 0x3FFFFu

/* ctx (EdgeGeom context in LS, r3) fields used here */
#define C_TAB      0x10    /* uniform table addresses */
#define C_VTX_IN   0x54    /* compress: output start */
#define C_FREE     0x58    /* lowest free LS address */
#define C_POS      0x5C    /* attribute id 1 -> table */
#define C_NRM      0x60
#define C_TAN      0x64
#define C_DESC     0x6C
#define C_CALLBACK 0x7C
#define C_INDEXES  0x50    /* index list */
#define C_WORLD    0xA0    /* local to world, 3x4 */
#define C_VIEWPORT 0xD0    /* u16 x, y, w, h, then two depth limits */
#define C_VIEWPROJ 0xE0
#define C_SCALE    0x120   /* viewport transform */
#define C_OFFSET   0x130
#define C_AA       0x140   /* RSX antialias mode */
#define C_CUSTOM   0x184   /* custom vertex transform */
#define C_SLOTS    0x160   /* table slot -> attribute id */
#define C_NI       0x190   /* index count, u16 */

#define CULL_MAX_VERTS 65536   /* indexes are u16 */

static const uint8_t k_desc[0x30] = {
    0x03,0x0C,0x04,0x00, 0,0,0,0,
    0x00,0x08,0x03,0x01, 0x06,0x00,0x18,0x00,
    0x06,0x09,0x03,0x02, 0x03,0x02,0x00,0x00,
    0x09,0x09,0x03,0x03, 0x03,0x0E,0x00,0x00,
    0x02,0x0E,0x02,0x0E, 0x02,0x0E,0,0, 0,0,0,0, 0,0,0,0 };

typedef struct { __m256 a, b; } Pal;   /* columns: a = c0,c1  b = c2,c3 */

typedef struct {
    Pal palette[256];
    uint32_t* packed;      /* normal, tangent per vertex */
    uint32_t capacity;
    uint32_t fixed_lsa;
    int active;
    float* sx, * sy;       /* cull: screen position per vertex */
    int32_t* ix, * iy;     /* rounded to the sample grid */
    int32_t* flags;        /* outside x+ x- y+ y- z+ z- <<7..2, w > 0 <<1 */
} State;

static _Thread_local State* t_state;
static float s_weight[256];   /* byte -> float */
static float s_unit[1024];    /* format 9 component -> float */
static int s_on = -1;
static long s_fallbacks;

/* The SPU builds 1.0 + k*2^-23 and does a fused mul-add with 0x46352E7C / 0xC6353150;
 * k * c1 + c0 is the same value. */
static void init_tables(void)
{
    for (int i = 0; i < 256; i++) s_weight[i] = (float)i * 0.0039215689f;
    for (int i = 0; i < 1024; i++) s_unit[i] = (float)((double)i * (11595.62109375 / 8388608.0) + (11595.62109375 - 11596.328125));
}

static int enabled(void)
{
    if (s_on < 0) {
        const char* e = getenv("NHL_EDGE_NATIVE");
        if (e && *e == '1') init_tables();
        s_on = (e && *e == '1') ? 1 : 0;
        if (s_on) fprintf(stderr, "[EDGE-NATIVE] on\n");
    }
    return s_on;
}

static State* state(void)
{
    if (!t_state) {
        t_state = (State*)_aligned_malloc(sizeof(State), 64);
        memset(t_state, 0, sizeof(State));
    }
    return t_state;
}

static inline uint32_t rd32(const uint8_t* p) { return __builtin_bswap32(*(const uint32_t*)p); }
static inline void wr32(uint8_t* p, uint32_t v) { *(uint32_t*)p = __builtin_bswap32(v); }
static inline uint32_t reg(const void* c, unsigned n) { return ((const uint32_t*)c)[n * 4]; }

static int fall_back(const uint8_t* cfg, const char* why)
{
    if (__atomic_fetch_add(&s_fallbacks, 1, __ATOMIC_RELAXED) < 16)
        fprintf(stderr, "[EDGE-NATIVE] lifted: %s, cfg %02X%02X%02X%02X %02X%02X%02X%02X\n", why,
                cfg[0], cfg[1], cfg[2], cfg[3], cfg[4], cfg[5], cfg[6], cfg[7]);
    return 0;
}

/* func 0xA048: r3 ctx, r4 vertexes, r5 fixed offsets, r6/r7 secondary stream. r82 = record (caller). */
static int stage_decompress(const void* c, uint8_t* ls)
{
    State* s = state();
    const uint32_t ctx = reg(c, 3) & LS_MASK, rec = reg(c, 82) & LS_MASK;
    const uint8_t* cfg = ls + ctx;
    s->active = 0;
    if (cfg[0] != 0x83 || cfg[2] != 0xFF || cfg[3] != 0 || cfg[4] > 1 || cfg[5] != 0xFF || cfg[7] != 0)
        return fall_back(cfg, "config");
    if ((cfg[6] & 15) != 2 && (cfg[6] & 15) != 3) return fall_back(cfg, "skin flavor");
    if (reg(c, 6) || reg(c, 7) || rd32(ls + ctx + C_CALLBACK)) return fall_back(cfg, "secondary stream");
    if (memcmp(ls + (rd32(ls + ctx + C_DESC) & LS_MASK), k_desc, sizeof k_desc)) return fall_back(cfg, "stream layout");
    if (!rd32(ls + rec + 0x11C)) return fall_back(cfg, "no skinning");
    if (rd32(ls + ctx + C_SLOTS)) return fall_back(cfg, "tables in use");
    const uint32_t vtx = reg(c, 4) & LS_MASK;
    s->fixed_lsa = reg(c, 5) & LS_MASK;
    s->active = 1;
    /* what the lifted decode leaves behind for the later stages */
    for (int i = 0; i < 3; i++) wr32(ls + ctx + C_POS + i * 4, rd32(ls + ctx + C_TAB + i * 4));
    wr32(ls + ctx + C_SLOTS, 0x01020300u);
    wr32(ls + ctx + C_VTX_IN, vtx);
    wr32(ls + ctx + C_FREE, vtx);
    return 1;
}

static inline __m128i bswap32x4(__m128i v)
{
    return _mm_shuffle_epi8(v, _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12));
}

static void load_palette(State* s, const uint8_t* src, uint32_t count)
{
    if (!count)   /* the lifted skin returns without touching anything; identity gives the same */
        for (int i = 0; i < 256; i++) {
            s->palette[i].a = _mm256_setr_ps(1, 0, 0, 0, 0, 1, 0, 0);
            s->palette[i].b = _mm256_setr_ps(0, 0, 1, 0, 0, 0, 0, 0);
        }
    for (uint32_t i = 0; i < count && i < 256; i++, src += 0x30) {
        __m128 r0 = _mm_castsi128_ps(bswap32x4(_mm_loadu_si128((const __m128i*)src)));
        __m128 r1 = _mm_castsi128_ps(bswap32x4(_mm_loadu_si128((const __m128i*)(src + 16))));
        __m128 r2 = _mm_castsi128_ps(bswap32x4(_mm_loadu_si128((const __m128i*)(src + 32))));
        __m128 r3 = _mm_setzero_ps();
        _MM_TRANSPOSE4_PS(r0, r1, r2, r3);
        s->palette[i].a = _mm256_set_m128(r1, r0);
        s->palette[i].b = _mm256_set_m128(r3, r2);
    }
}

/* Format 9: 10 + 10 bits for two axes, then the axis the third goes on, its sign. */
static inline __m128 unit_vector(const uint8_t* b)
{
    const uint32_t a = (uint32_t)b[0] << 16 | (uint32_t)b[1] << 8 | b[2];
    const float x = s_unit[a >> 14 & 0x3FF], y = s_unit[a >> 4 & 0x3FF];
    const unsigned idx = a & 15;
    static const int32_t order[4][4] = { { 0, 1, 2, 3 }, { 1, 0, 2, 3 }, { 1, 2, 0, 3 }, { 1, 2, 0, 3 } };
    __m128 r = _mm_max_ss(_mm_set_ss(1.0f - x * x - y * y), _mm_set_ss(1e-20f));
    __m128 e = _mm_rsqrt_ss(r);
    e = _mm_mul_ss(e, _mm_sub_ss(_mm_set_ss(1.5f), _mm_mul_ss(_mm_mul_ss(_mm_set_ss(0.5f), r), _mm_mul_ss(e, e))));
    float z = _mm_cvtss_f32(_mm_mul_ss(r, e));
    if (!(idx & 1)) z = -z;
    return _mm_permutevar_ps(_mm_setr_ps(z, x, y, 0.0f), _mm_loadu_si128((const __m128i*)order[idx >> 2]));
}

static inline __m128 normalize3(__m128 v)
{
    const __m128 m = _mm_mul_ps(v, v);
    __m128 l = _mm_add_ss(_mm_add_ss(m, _mm_shuffle_ps(m, m, 1)), _mm_movehl_ps(m, m));
    l = _mm_max_ss(l, _mm_set_ss(1e-20f));
    __m128 e = _mm_rsqrt_ss(l);
    e = _mm_mul_ss(e, _mm_sub_ss(_mm_set_ss(1.5f), _mm_mul_ss(_mm_mul_ss(_mm_set_ss(0.5f), l), _mm_mul_ss(e, e))));
    return _mm_mul_ps(v, _mm_shuffle_ps(e, e, 0));
}

/* X11Y11Z10N: floor of the scaled value */
static inline uint32_t pack(__m128 v)
{
    const __m128 scale = _mm_setr_ps(0.99951172f, 0.99951172f, 0.99902344f, 0.0f);
    const __m128 bias = _mm_setr_ps(-0.00048828125f, -0.00048828125f, -0.0009765625f, 0.0f);
    const __m128 steps = _mm_setr_ps(1024.0f, 1024.0f, 512.0f, 0.0f);
    __m128i i = _mm_cvttps_epi32(_mm_floor_ps(_mm_mul_ps(_mm_fmadd_ps(v, scale, bias), steps)));
    i = _mm_sllv_epi32(_mm_and_si128(i, _mm_setr_epi32(0x7FF, 0x7FF, 0x3FF, 0)), _mm_setr_epi32(0, 11, 22, 0));
    i = _mm_or_si128(_mm_or_si128(i, _mm_shuffle_epi32(i, 1)), _mm_shuffle_epi32(i, 2));
    return (uint32_t)_mm_cvtsi128_si32(i);
}

static inline __m128 cross3(__m128 a, __m128 b)
{
    const __m128 ay = _mm_shuffle_ps(a, a, _MM_SHUFFLE(3, 0, 2, 1)), by = _mm_shuffle_ps(b, b, _MM_SHUFFLE(3, 0, 2, 1));
    const __m128 r = _mm_fmsub_ps(a, by, _mm_mul_ps(ay, b));
    return _mm_shuffle_ps(r, r, _MM_SHUFFLE(3, 0, 2, 1));
}

static inline __m128 mul3x3(__m128 v, __m128 c0, __m128 c1, __m128 c2)
{
    const __m128 x = _mm_shuffle_ps(v, v, 0x00), y = _mm_shuffle_ps(v, v, 0x55), z = _mm_shuffle_ps(v, v, 0xAA);
    return _mm_fmadd_ps(x, c0, _mm_fmadd_ps(y, c1, _mm_mul_ps(z, c2)));
}

/* func 0xA0F8: r3 ctx, r4 palette, r5 palette count, r6 weights (w0 i0 w1 i1 w2 i2 w3 i3). */
static int stage_skin(const void* c, uint8_t* ls)
{
    State* s = state();
    if (!s->active) return 0;
    const uint32_t ctx = reg(c, 3) & LS_MASK, pal = reg(c, 4) & LS_MASK, wgt = reg(c, 6) & LS_MASK;
    const uint8_t* cfg = ls + ctx;
    const unsigned nv = (unsigned)cfg[8] << 8 | cfg[9], flavor = cfg[6] & 15, outfmt = cfg[4];
    const uint32_t count = reg(c, 5);
    const uint8_t* in = ls + (rd32(ls + ctx + C_VTX_IN) & LS_MASK);
    const uint8_t* w = ls + wgt;
    uint8_t* tab = ls + (rd32(ls + ctx + C_POS) & LS_MASK);
    const __m128i off = bswap32x4(_mm_blend_epi32(_mm_loadu_si128((const __m128i*)(ls + s->fixed_lsa)), _mm_setzero_si128(), 8));
    const __m128i expand = _mm_setr_epi8(1, 0, -1, -1, 3, 2, -1, -1, 5, 4, -1, -1, -1, -1, -1, -1);
    const __m128 one = _mm_set1_ps(1.0f), fix = _mm_set1_ps(1.0f / 16384.0f);
    const Pal* P = s->palette;
    uint32_t* packed = s->packed;
    if (outfmt && s->capacity < nv) {
        free(s->packed);
        s->capacity = (nv + 1023) & ~1023u;
        s->packed = packed = (uint32_t*)malloc(s->capacity * 8);
    }
    load_palette(s, ls + pal, count);
    for (unsigned v = 0; v < nv; v++, in += 12, w += 8) {
        __m256 f = _mm256_broadcast_ss(&s_weight[w[0]]);
        __m256 a = _mm256_mul_ps(f, P[w[1]].a), b = _mm256_mul_ps(f, P[w[1]].b);
        for (int k = 1; k < 4; k++) {
            f = _mm256_broadcast_ss(&s_weight[w[k * 2]]);
            a = _mm256_fmadd_ps(f, P[w[k * 2 + 1]].a, a);
            b = _mm256_fmadd_ps(f, P[w[k * 2 + 1]].b, b);
        }
        const __m128 c0 = _mm256_castps256_ps128(a), c1 = _mm256_extractf128_ps(a, 1);
        const __m128 c2 = _mm256_castps256_ps128(b), c3 = _mm256_extractf128_ps(b, 1);
        const __m128 p = _mm_mul_ps(_mm_cvtepi32_ps(_mm_add_epi32(_mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)in), expand), off)), fix);
        const __m128 q = _mm_add_ps(mul3x3(p, c0, c1, c2), c3);
        _mm_storeu_si128((__m128i*)(tab + (size_t)v * 16), bswap32x4(_mm_castps_si128(_mm_blend_ps(q, one, 8))));
        if (!outfmt) continue;
        const __m128 n = unit_vector(in + 6), t = unit_vector(in + 9);
        const __m128 nn = flavor == 3 ? mul3x3(n, cross3(c1, c2), cross3(c2, c0), cross3(c0, c1)) : mul3x3(n, c0, c1, c2);
        packed[v * 2] = pack(normalize3(nn));
        packed[v * 2 + 1] = pack(normalize3(mul3x3(t, c0, c1, c2)));
    }
    const uint32_t r4 = reg(c, 4), r6 = reg(c, 6);
    wr32(ls + ctx + C_FREE, r4 > r6 ? r6 : r4);
    return 1;
}

/* func 0x4C70: r3 ctx. Output goes at the free pointer; pos, then packed normal + tangent. */
static int stage_compress(const void* c, uint8_t* ls)
{
    State* s = state();
    if (!s->active) return 0;
    const uint32_t ctx = reg(c, 3) & LS_MASK;
    const uint8_t* cfg = ls + ctx;
    const unsigned nv = (unsigned)cfg[8] << 8 | cfg[9], outfmt = cfg[4];
    const unsigned stride = outfmt ? 20 : 12;
    const uint32_t out = rd32(ls + ctx + C_FREE);
    const uint8_t* tab = ls + (rd32(ls + ctx + C_POS) & LS_MASK);
    uint8_t* o = ls + (out & LS_MASK);
    if (outfmt) {
        const uint32_t* packed = s->packed;
        for (unsigned v = 0; v < nv; v++, o += 20, tab += 16, packed += 2) {
            memcpy(o, tab, 12);
            wr32(o + 12, packed[0]);
            wr32(o + 16, packed[1]);
        }
    } else {
        for (unsigned v = 0; v < nv; v++, o += 12, tab += 16) memcpy(o, tab, 12);
    }
    wr32(ls + ctx + C_VTX_IN, out);
    wr32(ls + ctx + C_FREE, out + ((nv * stride + 15) & ~15u));
    return 1;
}

/* First half of the cull, func 0x5850: world * view-proj, viewport, outside flags for all vertexes. */
static void cull_vertices(State* s, const uint8_t* ls, uint32_t ctx, unsigned nv, float step)
{
    const uint8_t* c = ls + ctx;
    float m[4][4];
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            float v = 0.0f;
            for (int k = 0; k < 3; k++) {
                const uint32_t pu = rd32(c + C_VIEWPROJ + i * 16 + k * 4), wu = rd32(c + C_WORLD + k * 16 + j * 4);
                v += *(const float*)&pu * *(const float*)&wu;
            }
            if (j == 3) { const uint32_t pu = rd32(c + C_VIEWPROJ + i * 16 + 12); v += *(const float*)&pu; }
            m[i][j] = v;
        }
    }
    float scale[3], offset[3];
    for (int i = 0; i < 3; i++) {
        const uint32_t a = rd32(c + C_SCALE + i * 4), b = rd32(c + C_OFFSET + i * 4);
        scale[i] = *(const float*)&a; offset[i] = *(const float*)&b;
    }
    const float x0 = (float)(c[C_VIEWPORT] << 8 | c[C_VIEWPORT + 1]), y0 = (float)(c[C_VIEWPORT + 2] << 8 | c[C_VIEWPORT + 3]);
    const float x1 = x0 + (float)(c[C_VIEWPORT + 4] << 8 | c[C_VIEWPORT + 5]), y1 = y0 + (float)(c[C_VIEWPORT + 6] << 8 | c[C_VIEWPORT + 7]);
    const uint32_t za = rd32(c + C_VIEWPORT + 8), zb = rd32(c + C_VIEWPORT + 12);
    const float z0 = *(const float*)&za, z1 = *(const float*)&zb;
#define SPLAT(v) _mm256_set1_ps(v)
#define FLAG(cmp, bit) _mm256_and_ps(cmp, _mm256_castsi256_ps(_mm256_set1_epi32(bit)))
    const __m256i swap = _mm256_broadcastsi128_si256(_mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12));
    const uint8_t* p = ls + (rd32(c + C_POS) & LS_MASK);
    for (unsigned v = 0; v < nv; v += 8, p += 128) {
#define ROW(n) _mm256_castsi256_ps(_mm256_shuffle_epi8(_mm256_loadu2_m128i((const __m128i*)(p + 64 + (n) * 16), (const __m128i*)(p + (n) * 16)), swap))
        const __m256 t0 = _mm256_unpacklo_ps(ROW(0), ROW(1)), t1 = _mm256_unpackhi_ps(ROW(0), ROW(1));
        const __m256 t2 = _mm256_unpacklo_ps(ROW(2), ROW(3)), t3 = _mm256_unpackhi_ps(ROW(2), ROW(3));
#undef ROW
        const __m256 x = _mm256_shuffle_ps(t0, t2, 0x44), y = _mm256_shuffle_ps(t0, t2, 0xEE), z = _mm256_shuffle_ps(t1, t3, 0x44);
#define XFORM(i) _mm256_fmadd_ps(x, SPLAT(m[i][0]), _mm256_fmadd_ps(y, SPLAT(m[i][1]), _mm256_fmadd_ps(z, SPLAT(m[i][2]), SPLAT(m[i][3]))))
        const __m256 w = XFORM(3);
        /* x * |1/w|; the clamp stands in for the SPU saturating on w = 0 */
        const __m256 inv = _mm256_div_ps(SPLAT(1.0f), _mm256_max_ps(_mm256_andnot_ps(SPLAT(-0.0f), w), SPLAT(1.17549435e-38f)));
        const __m256 sx = _mm256_fmadd_ps(_mm256_mul_ps(XFORM(0), inv), SPLAT(scale[0]), SPLAT(offset[0]));
        const __m256 sy = _mm256_fmadd_ps(_mm256_mul_ps(XFORM(1), inv), SPLAT(scale[1]), SPLAT(offset[1]));
        const __m256 sz = _mm256_fmadd_ps(_mm256_mul_ps(XFORM(2), inv), SPLAT(scale[2]), SPLAT(offset[2]));
#undef XFORM
        const __m256 f = _mm256_or_ps(_mm256_or_ps(_mm256_or_ps(
            FLAG(_mm256_cmp_ps(sx, SPLAT(x1), _CMP_GT_OQ), 0x80) , FLAG(_mm256_cmp_ps(sx, SPLAT(x0), _CMP_LT_OQ), 0x40)),
            _mm256_or_ps(FLAG(_mm256_cmp_ps(sy, SPLAT(y1), _CMP_GT_OQ), 0x20), FLAG(_mm256_cmp_ps(sy, SPLAT(y0), _CMP_LT_OQ), 0x10))),
            _mm256_or_ps(_mm256_or_ps(FLAG(_mm256_cmp_ps(sz, SPLAT(z1 > z0 ? z1 : z0), _CMP_GT_OQ), 0x08), FLAG(_mm256_cmp_ps(sz, SPLAT(z1 > z0 ? z0 : z1), _CMP_LT_OQ), 0x04)),
                         FLAG(_mm256_cmp_ps(w, _mm256_setzero_ps(), _CMP_GT_OQ), 0x02)));
        _mm256_storeu_ps(s->sx + v, sx);
        _mm256_storeu_ps(s->sy + v, sy);
        _mm256_storeu_si256((__m256i*)(s->flags + v), _mm256_castps_si256(f));
        /* the SPU saturates here, x86 gives INT_MIN for too big */
        const __m256 top = SPLAT(2147483520.0f);
        _mm256_storeu_si256((__m256i*)(s->ix + v), _mm256_cvttps_epi32(_mm256_min_ps(_mm256_fmadd_ps(sx, SPLAT(step), SPLAT(0.5f)), top)));
        _mm256_storeu_si256((__m256i*)(s->iy + v), _mm256_cvttps_epi32(_mm256_min_ps(_mm256_fmadd_ps(sy, SPLAT(step), SPLAT(0.5f)), top)));
    }
#undef FLAG
#undef SPLAT
}

/* func 0x5C08: r3 ctx, r4 mode (1 outside only, 2/3 + one winding), r5 index bias. The index list is
 * compacted in place. Returns the index count in r3. A triangle goes if all its vertexes are outside
 * one plane; if all of them are safely in front of the camera it also goes if it faces away, or if
 * it fits between two sample rows or columns. */
static int stage_cull(void* c, uint8_t* ls)
{
    State* s = state();
    if (!s->active) return 0;
    const uint32_t ctx = reg(c, 3) & LS_MASK, mode = reg(c, 4);
    const uint8_t* cfg = ls + ctx;
    if (mode < 1 || mode > 3) { if (mode) return fall_back(cfg, "cull mode"); return 0; }   /* 0: lifted, no work */
    if (reg(c, 5)) return fall_back(cfg, "index bias");
    if (rd32(ls + ctx + C_CUSTOM)) return fall_back(cfg, "custom transform");
    const unsigned aa = cfg[C_AA];
    if (aa == 5) return fall_back(cfg, "rotated grid");
    if (!s->sx) {
        const size_t n = CULL_MAX_VERTS + 8;
        float* b = (float*)_aligned_malloc(n * 4 * 5, 64);
        s->sx = b; s->sy = b + n; s->ix = (int32_t*)(b + n * 2); s->iy = (int32_t*)(b + n * 3); s->flags = (int32_t*)(b + n * 4);
    }
    cull_vertices(s, ls, ctx, (unsigned)cfg[8] << 8 | cfg[9], aa == 3 || aa == 4 ? 2.0f : 1.0f);

    const unsigned format = cfg[6] >> 4;
    const uint32_t list = rd32(ls + ctx + C_INDEXES);
    const unsigned tris = ((unsigned)cfg[C_NI] << 8 | cfg[C_NI + 1]) / 3;
    const int backface = mode != 1;
    const uint32_t flip = (mode == 2 ? 0u : 0x80000000u) ^ (format == 0 || format == 2 ? 0x80000000u : 0u);
    const float* sx = s->sx, * sy = s->sy;
    const int32_t* ix = s->ix, * iy = s->iy, * fl = s->flags;
    const uint8_t* in = ls + (list & LS_MASK);
    uint8_t* out = ls + (list & LS_MASK);
    for (unsigned t = 0; t < tris; t++, in += 6) {
        const uint32_t i0 = (uint32_t)in[0] << 8 | in[1], i1 = (uint32_t)in[2] << 8 | in[3], i2 = (uint32_t)in[4] << 8 | in[5];
        const int32_t all = fl[i0] & fl[i1] & fl[i2], any = fl[i0] | fl[i1] | fl[i2];
        int drop = (all & 0xFC) != 0;
        if ((all & 2) && !(any & 4)) {
            const float ax = sx[i0], ay = sy[i0];
            union { float f; uint32_t u; } area;
            area.f = (sx[i1] - ax) * (sy[i2] - ay) - (sy[i1] - ay) * (sx[i2] - ax);
            area.u ^= flip;
            drop |= (ix[i0] == ix[i1]) & (ix[i1] == ix[i2]);
            drop |= (iy[i0] == iy[i1]) & (iy[i1] == iy[i2]);
            drop |= backface & (area.f > 0.0f);
        }
        memcpy(out, in, 4);
        memcpy(out + 4, in + 4, 2);
        out += 6 * !drop;
    }
    const uint32_t count = (uint32_t)((out - (ls + (list & LS_MASK))) / 2);
    ls[ctx + C_NI] = (uint8_t)(count >> 8);
    ls[ctx + C_NI + 1] = (uint8_t)count;
    wr32(ls + ctx + C_FREE, (list + count * 2 + 15) & ~15u);
    ((uint32_t*)c)[3 * 4] = count;
    return 1;
}

/* From the lifted stage functions. c = spu_context (GPRs first). Nonzero: stage done, return. */
int nhl_edge_native(void* c, uint8_t* ls, unsigned stage)
{
    if (!enabled()) return 0;
    switch (stage) {
    case 3: return stage_decompress(c, ls);
    case 4: return stage_skin(c, ls);
    case 6: return stage_cull(c, ls);
    case 9: return stage_compress(c, ls);
    default: return 0;
    }
}
