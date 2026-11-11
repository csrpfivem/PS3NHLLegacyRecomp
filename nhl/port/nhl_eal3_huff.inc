/* NHL #314: native fast path for the EALayer3 decoder's Huffman pair loop
 * (job 0x01DE9800, body 0x4E3C-0x50D0: decode one (x, y) pair of "big values" and
 * requantize both in place). Included into the lifted job's spu_recomp.c by
 * tools/nhl_hooks.py (EAL3H), so it uses the job's own helper for the one float
 * multiply (bit-identical to the lifted code) and is built in both SPU variants.
 *
 * deck20real: the two chunks of this loop are 54% of what is left of the job (~1.4% of
 * all Deck cycles). The lifted loop keeps its bit reader in three LS quadwords and
 * stores them back several times per pair.
 *
 * Per pair (state: P = byte pointer LS[r80+0xF0], BB = bit buffer +0x120, BC = bits in
 * buffer +0x130; START = +0x100; r91 = bit limit; r90 = table of s16 (len << 8 | x << 4 | y,
 * negative = escape); r85 = linbits; r93 / r94 = band scale for negative / positive;
 * r100 = |v|^(4/3) table; r102 = zero; r82 = output pointer; r89 = sample index,
 * r95 = region end):
 *   refill, look up 8 bits, x (+ linbits if 15), sign + scale * pow[x];
 *   refill, y (+ linbits), sign + scale * pow[y]; r89 += 2, r82 / r87 += 8.
 * Anything else (escape entry, value > 31, odd reader state, the last pair of a
 * region) is left to the lifted code: the fast path only ever stops BETWEEN pairs.
 *
 *   NHL_EAL3_HUFF=1 native, =2 check (one dry pair per lifted pair), =3 check then native. */
int nhl_eal3_huff_mode(void);
void nhl_eal3_huff_check(void* ctx, void* gpr, uint8_t* ls, int (*one)(void*, uint32_t*));

typedef struct { uint32_t P, BB; int32_t BC; uint32_t w0, w1; } nhl_huff_pair;

static inline uint32_t nhl_h_ld32(const uint8_t* ls, uint32_t a) { uint32_t v; memcpy(&v, ls + (a & 0x3FFFFu), 4); return __builtin_bswap32(v); }
static inline void nhl_h_st32(uint8_t* ls, uint32_t a, uint32_t v) { v = __builtin_bswap32(v); memcpy(ls + (a & 0x3FFFFu), &v, 4); }
static inline uint32_t nhl_h_fm(uint32_t a, uint32_t b)
{
    u128 x, y; memset(&x, 0, sizeof x); memset(&y, 0, sizeof y);
    x._u32[0] = a; y._u32[0] = b;
    return spu_fm(x, y)._u32[0];
}

/* One pair on a private copy of the reader state. 0 = this pair is for the lifted code. */
static inline int nhl_eal3_pair(const spu_context* ctx, const uint8_t* ls, nhl_huff_pair* s)
{
    const uint32_t r80 = ctx->gpr[80]._u32[0];
    const uint32_t start = nhl_h_ld32(ls, r80 + 0x100);
    const int32_t limit = (int32_t)ctx->gpr[91]._u32[0];
    const uint32_t linbits = ctx->gpr[85]._u32[0];
    const uint32_t pow_tab = ctx->gpr[100]._u32[0];
    uint32_t P = s->P, BB = s->BB;
    int32_t BC = s->BC;
    if (BC < 0 || BC > 32 || linbits > 16) return 0;

    /* refill before x */
    const int32_t consumed = (int32_t)((P - start) << 3);
    const int32_t pos = consumed - BC;                        /* r92 */
    int32_t n = 32 - BC;
    if (n > limit - consumed + 7) n = limit - consumed + 7;
    if (n < 0) return 0;
    n &= ~7;
    if (BC < 32) BB |= nhl_h_ld32(ls, P) >> BC;
    BC += n; P += (uint32_t)n >> 3;

    uint16_t e16; memcpy(&e16, ls + ((ctx->gpr[90]._u32[0] + ((BB >> 24) << 1)) & 0x3FFFFu), 2);
    const int32_t e = (int16_t)__builtin_bswap16(e16);
    if (e < 0) return 0;                                      /* escape to a sub-table */
    const int32_t len = e >> 8;
    uint32_t x = ((uint32_t)e & 255u) >> 4, y = (uint32_t)e & 15u;
    BB = len >= 32 ? 0 : BB << len; BC -= len;

    if (x == 15 && linbits) { x += BB >> (32 - linbits); BB <<= linbits; BC -= (int32_t)linbits; }
    if (x == 0) s->w0 = ctx->gpr[102]._u32[0];
    else {
        if (x > 31) return 0;                                 /* big value: other path */
        const uint32_t sc = (int32_t)BB > -1 ? ctx->gpr[94]._u32[0] : ctx->gpr[93]._u32[0];
        s->w0 = nhl_h_fm(sc, nhl_h_ld32(ls, pow_tab + 4 * x));
        BC -= 1; BB += BB;
    }

    /* refill before y (the job measures the limit from the pair's start position here) */
    if (BC < 0 || BC > 32) return 0;
    int32_t n2 = 32 - BC;
    if (n2 > limit - (pos + BC) + 7) n2 = limit - (pos + BC) + 7;
    if (n2 < 0) return 0;
    n2 &= ~7;
    if (BC < 32) BB |= nhl_h_ld32(ls, P) >> BC;
    BC += n2; P += (uint32_t)n2 >> 3;

    if (y == 15 && linbits) { y += BB >> (32 - linbits); BB <<= linbits; BC -= (int32_t)linbits; }
    if (y == 0) s->w1 = ctx->gpr[102]._u32[0];
    else {
        if (y > 31) return 0;
        const uint32_t sc = (int32_t)BB > -1 ? ctx->gpr[94]._u32[0] : ctx->gpr[93]._u32[0];
        s->w1 = nhl_h_fm(sc, nhl_h_ld32(ls, pow_tab + 4 * y));
        BC -= 1; BB += BB;
    }
    s->P = P; s->BB = BB; s->BC = BC;
    return 1;
}

/* Will the lifted loop come back to its head after the pair that starts now? */
static inline int nhl_eal3_more(const spu_context* ctx)
{
    return (int32_t)ctx->gpr[95]._u32[0] > (int32_t)(ctx->gpr[89]._u32[0] + 2u);
}

/* Check mode: out = { P, BB, BC, w0, w1, output address } after one pair, state untouched. */
static int nhl_eal3_pair_dry(void* cv, uint32_t* out)
{
    const spu_context* ctx = (const spu_context*)cv;
    const uint8_t* ls = ctx->ls;
    const uint32_t r80 = ctx->gpr[80]._u32[0];
    if (!nhl_eal3_more(ctx)) return 0;
    nhl_huff_pair s;
    s.P = nhl_h_ld32(ls, r80 + 0xF0); s.BB = nhl_h_ld32(ls, r80 + 0x120); s.BC = (int32_t)nhl_h_ld32(ls, r80 + 0x130);
    if (!nhl_eal3_pair(ctx, ls, &s)) return 0;
    out[0] = s.P; out[1] = s.BB; out[2] = (uint32_t)s.BC; out[3] = s.w0; out[4] = s.w1; out[5] = ctx->gpr[82]._u32[0];
    return 1;
}

/* Native mode: every pair up to (not including) the region's last or the first odd one. */
static void nhl_eal3_pairs(spu_context* ctx)
{
    uint8_t* ls = ctx->ls;
    const uint32_t r80 = ctx->gpr[80]._u32[0];
    nhl_huff_pair s;
    s.P = nhl_h_ld32(ls, r80 + 0xF0); s.BB = nhl_h_ld32(ls, r80 + 0x120); s.BC = (int32_t)nhl_h_ld32(ls, r80 + 0x130);
    uint32_t out = ctx->gpr[82]._u32[0], done = 0;
    const int32_t end = (int32_t)ctx->gpr[95]._u32[0];
    int32_t idx = (int32_t)ctx->gpr[89]._u32[0];
    while (end > idx + 2) {
        nhl_huff_pair t = s;
        if (!nhl_eal3_pair(ctx, ls, &t)) break;
        nhl_h_st32(ls, out, t.w0); nhl_h_st32(ls, out + 4, t.w1);
        s = t; out += 8; idx += 2; done++;
    }
    if (!done) return;
    nhl_h_st32(ls, r80 + 0xF0, s.P); nhl_h_st32(ls, r80 + 0x120, s.BB); nhl_h_st32(ls, r80 + 0x130, (uint32_t)s.BC);
    for (int i = 0; i < 4; i++) {
        ctx->gpr[89]._u32[i] += 2 * done;
        ctx->gpr[87]._u32[i] += 8 * done;
        ctx->gpr[82]._u32[i] += 8 * done;
        ctx->gpr[84]._u32[i] = ctx->gpr[87]._u32[i] + ctx->gpr[83]._u32[i];
    }
}
