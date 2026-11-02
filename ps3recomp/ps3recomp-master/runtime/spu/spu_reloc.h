/*
 * [NHL local patch] Relocatable SPU images (spu_lifter.py --relocatable).
 *
 * NHL's Job Manager jobs are position-independent: linked at 0, DMA'd to
 * whatever local-store address the job runner picks (one body lands at five
 * different addresses), and they find their own base with
 * `ila $r16,X; brsl $r6,X; sf $r79,$r16,$r6`. A fixed-base lift cannot serve
 * them, and one lift per (job, base) pair does not scale. So a job is lifted
 * once at base 0: its functions register under SPU_RELOC_KEY | offset, and
 * every value the lifted code derives from its own position is
 * ctx->code_base + offset.
 *
 * Residency is tracked by the MFC GET hook as for any overlay region
 * (spu_overlay_note_get); a slot registered with
 * spu_overlay_register_reloc_region() is marked reloc. This header is the one
 * place that turns "pc inside a resident slot" into "registry key + code base",
 * so the dispatcher and the unit test (runtime/spu/tests/test_reloc_main.c)
 * run the same code.
 *
 * SPU_RELOC_KEY must equal RELOC_KEY in tools/spu_lifter.py. It is above the
 * 256 KB LS range, so no real LS address can equal a relocatable key: an
 * image-0 wildcard lookup can never serve a job's function at low LS, and a
 * job's lookup can never land on fixed-base code.
 */
#ifndef SPU_RELOC_H
#define SPU_RELOC_H

#include "spu_context.h"

#define SPU_RELOC_KEY 0x100000u

/* The resident slot whose span contains `pc`, or -1. */
static inline int spu_resident_slot_of(const spu_context* ctx, uint32_t pc)
{
    for (int s = 0; s < SPU_RESIDENT_SLOTS; ++s)
        if (ctx->resident_code[s].image_id && pc >= ctx->resident_code[s].lsa &&
            pc - ctx->resident_code[s].lsa < ctx->resident_code[s].size)
            return s;
    return -1;
}

/* Registry key for `pc` inside slot `s`: the LS address itself for a
 * fixed-base image, SPU_RELOC_KEY | offset for a relocatable one. */
static inline uint32_t spu_resident_key(const spu_context* ctx, int s, uint32_t pc)
{
    return ctx->resident_code[s].reloc ? (SPU_RELOC_KEY | (pc - ctx->resident_code[s].lsa)) : pc;
}

/* About to run slot `s`'s code: a relocatable image needs its load base. */
static inline void spu_resident_enter(spu_context* ctx, int s)
{
    if (ctx->resident_code[s].reloc) ctx->code_base = ctx->resident_code[s].lsa;
}

#endif /* SPU_RELOC_H */
