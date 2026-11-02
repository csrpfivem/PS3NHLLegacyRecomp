/* Relocatable lift: one position-independent job (gen_test_reloc.py), lifted
 * once with spu_lifter --relocatable, run at two local-store bases.
 *
 * Entry goes through the same resident-slot resolution the runtime dispatcher
 * uses (spu_reloc.h: slot -> SPU_RELOC_KEY|offset -> ctx->code_base), so what
 * this checks is the lifter output and that glue together:
 *   - the base trick yields the real load base (r79), not 0,
 *   - brsl links are base + offset (r0, r6),
 *   - lqr/stqr reach the job's own data at base + offset,
 *   - fall-through and `br` tails land on base + offset (ctx->pc),
 *   - and nothing written by the first run leaks into the second.
 * A stale code_base before entry must not matter: the dispatcher sets it. */
#include "spu_recomp.h"
#include "spu_helpers.h"
#include "spu_reloc.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>

uint8_t* vm_base = 0;
int g_cri_video_dma = 0;
int g_spu_ls_watch_n = 0;   /* SPU_LSWATCH hook (spu_context.h): off */
void spu_ls_watch_slow(uint32_t lsa, int is_write, const uint8_t* p, uint32_t pc, uint32_t lr)
{ (void)lsa; (void)is_write; (void)p; (void)pc; (void)lr; }
SPU_THREAD_LOCAL void (*g_spu_trampoline_fn)(spu_context*) = 0;

static uint32_t g_mbox[4]; static int g_nmbox = 0;

u128 spu_rdch(spu_context* ctx, uint32_t ch) { (void)ctx; (void)ch; return spu_zero(); }
uint32_t spu_rchcnt(spu_context* ctx, uint32_t ch) { (void)ctx; (void)ch; return 1; }
void spu_wrch(spu_context* ctx, uint32_t channel, u128 value) {
    (void)ctx;
    if (channel == SPU_WrOutMbox && g_nmbox < 4) g_mbox[g_nmbox++] = value._u32[0];
}
void spu_stop(spu_context* ctx) { (void)ctx; }
void spu_halt(spu_context* ctx) { (void)ctx; }
void spu_img_restore(spu_context* ctx, int32_t saved) { ctx->image_id = saved; }

/* Minimal registry: key -> fn (the lifted table registers SPU_RELOC_KEY|offset). */
static struct { uint32_t key; void (*fn)(spu_context*); } g_reg[64]; static int g_nreg = 0;
void spu_register_function(uint32_t addr, void (*fn)(spu_context*)) {
    if (g_nreg < 64) { g_reg[g_nreg].key = addr; g_reg[g_nreg].fn = fn; g_nreg++; }
}
static void (*lookup(uint32_t key))(spu_context*) {
    for (int i = 0; i < g_nreg; i++) if (g_reg[i].key == key) return g_reg[i].fn;
    return 0;
}

static int g_bad_dispatch = 0;
void spu_indirect_branch(spu_context* ctx) {
    int s = spu_resident_slot_of(ctx, ctx->pc);
    void (*fn)(spu_context*) = s >= 0 ? lookup(spu_resident_key(ctx, s, ctx->pc)) : 0;
    if (!fn) { printf("FAIL: no lifted code for pc=0x%05X\n", ctx->pc); g_bad_dispatch = 1; return; }
    spu_resident_enter(ctx, s);
    fn(ctx);
}
static void drain(spu_context* ctx) {
    while (g_spu_trampoline_fn) { void (*f)(spu_context*) = g_spu_trampoline_fn; g_spu_trampoline_fn = 0; f(ctx); }
}
void spu_drain_call(spu_context* ctx, uint32_t return_pc) {
    drain(ctx);
    if (ctx->pc != return_pc) { printf("FAIL: call returned to 0x%05X, expected 0x%05X\n", ctx->pc, return_pc); g_bad_dispatch = 1; }
}

#include "test_reloc_job.h"   /* g_job: the job's bytes (gen_test_reloc.py) */

static int run_at(spu_context* ctx, uint32_t base) {
    int fails = 0;
    memset(ctx->ls, 0xCD, SPU_LS_SIZE);              /* poison: a wrong base reads junk */
    memcpy(ctx->ls + base, g_job, sizeof g_job);
    memset(ctx->resident_code, 0, sizeof ctx->resident_code);
    ctx->resident_code[3].lsa = base; ctx->resident_code[3].size = sizeof g_job;
    ctx->resident_code[3].source_ea = 0x01DF7500u; ctx->resident_code[3].image_id = 150;
    ctx->resident_code[3].reloc = 1;
    ctx->code_base = 0xDEAD0;                         /* stale: the dispatcher must set it */
    g_nmbox = 0; g_bad_dispatch = 0;

    ctx->pc = base; spu_indirect_branch(ctx); drain(ctx);

#define CHECK(c, ...) do { if (!(c)) { printf("FAIL @0x%05X: ", base); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)
    uint32_t stored = ((uint32_t)ctx->ls[base + 0x70] << 24) | ((uint32_t)ctx->ls[base + 0x71] << 16) |
                      ((uint32_t)ctx->ls[base + 0x72] << 8) | ctx->ls[base + 0x73];
    CHECK(!g_bad_dispatch, "dispatch failed");
    CHECK(ctx->gpr[79]._u32[0] == base, "base trick r79=0x%X", ctx->gpr[79]._u32[0]);
    CHECK(ctx->gpr[6]._u32[0] == base + 0x08, "PC-getter link r6=0x%X", ctx->gpr[6]._u32[0]);
    CHECK(ctx->gpr[0]._u32[0] == base + 0x14, "call link r0=0x%X", ctx->gpr[0]._u32[0]);
    CHECK(g_nmbox == 2, "%d mailbox writes, expected 2", g_nmbox);
    CHECK(g_nmbox < 1 || g_mbox[0] == 1000 + 1 + base, "lqr+call+base out=%u expected %u", g_mbox[0], 1000 + 1 + base);
    CHECK(g_nmbox < 2 || g_mbox[1] == 7, "tail out=%u", g_mbox[1]);
    CHECK(stored == 1001, "stqr stored %u at base+0x70", stored);
    CHECK(ctx->pc == base + 0x40, "tail pc=0x%X", ctx->pc);
    CHECK(ctx->code_base == base, "code_base=0x%X", ctx->code_base);
    return fails;
}

int main(void) {
    spu_recomp_register();

    static spu_context ctx;
    spu_context_init(&ctx, 0);
    int fails = run_at(&ctx, 0x3800) + run_at(&ctx, 0x9800) + run_at(&ctx, 0x0BA80);
    if (fails) { printf("FAIL: %d check(s)\n", fails); return 1; }
    printf("OK: one relocatable lift ran correctly at LS 0x3800, 0x9800 and 0xBA80\n");
    return 0;
}
