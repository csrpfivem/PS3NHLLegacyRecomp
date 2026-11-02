/*
 * ps3recomp - PPU <-> SPU lock-line coherence
 *
 * See spu_coherency.h for what this is for. The state here is deliberately
 * plain: a bit per 128-byte line, a small table of the SPU contexts that have
 * ever reserved one, and the lock-line spinlock. Everything that WRITES it runs
 * with the lock-line lock held (the SPU's GETLLAR, the PPU's coherent store),
 * so the read-modify-write on a shared bitmap byte is serialized. Only
 * spu_coh_is_reserved reads without the lock, which is the entire point: it is
 * on every PPU store.
 */

#include <stdlib.h>
#include <string.h>
#include "spu_coherency.h"

#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===========================================================================
 * The lock-line lock
 *
 * Moved here from spu_channels.c, where it was a file-static and so could
 * serialize SPU against SPU but not SPU against PPU -- which left the PPU free
 * to write a line in the middle of a PUTLLC's compare-and-commit. Same
 * spinlock, same semantics, now shared. _InterlockedExchange is a clang-cl/MSVC
 * intrinsic (no runtime library symbol needed); elsewhere use the C11
 * equivalent, which lowers to the same LL/SC or lock-xchg on every target.
 * ===========================================================================*/
#if defined(_MSC_VER)
#include <intrin.h>
#include <windows.h>   /* SwitchToThread */
static volatile long g_lockline = 0;
/* Test-and-test-and-set: spin on a plain read (no cache-line ping-pong) with a
 * pause, and yield after a long wait. The bare exchange loop let GH3's four
 * idle job-policy pollers saturate the line and delay every other SPU atomic
 * -- FMOD's mixer task among them, 25-40 ms at a time. */
/* [NHL local patch #272] Contention counters, read by [CPU] (boot_main.cpp). The clock only
 * runs on the contended path, so an uncontended lock costs one increment under the lock. */
volatile long long g_lockline_acquires, g_lockline_contended, g_lockline_wait_tsc;
/* NHL_LOCK_HIST=1: acquisitions per call site, printed with the atomics by spu_atom_report. */
int g_lock_hist = -1;
#define LOCK_SITES 32
static void* s_site[LOCK_SITES];
static long long s_site_n[LOCK_SITES];
static void lock_site_note(void* ra)
{
    unsigned k = (unsigned)(((uintptr_t)ra >> 2) * 2654435761u) % LOCK_SITES;
    for (int i = 0; i < LOCK_SITES; i++, k = (k + 1) % LOCK_SITES) {
        if (!s_site[k]) s_site[k] = ra;
        if (s_site[k] == ra) { s_site_n[k]++; return; }
    }
}
void spu_lock_sites_report(uintptr_t exe_base, int secs)
{
    for (int i = 0; i < LOCK_SITES; i++) if (s_site[i]) {
        const long long n = s_site_n[i];
        s_site_n[i] = 0;   /* a lost count in a diagnostic does not matter */
        if (n / secs >= 1000)
            fprintf(stderr, "[LOCKSITE] rva=0x%llX %lld/s\n", (unsigned long long)((uintptr_t)s_site[i] - exe_base), n / secs);
    }
}
void spu_lockline_lock(void)
{
    if (__builtin_expect(g_lock_hist != 0, 0)) {
        if (g_lock_hist < 0) g_lock_hist = getenv("NHL_LOCK_HIST") ? 1 : 0;
    }
    if (_InterlockedExchange(&g_lockline, 1)) {
        const unsigned long long t0 = __rdtsc();
        unsigned spins = 0;
        do {
            do {
                _mm_pause();
                if (++spins >= 4096) { spins = 0; SwitchToThread(); }
            } while (g_lockline);
        } while (_InterlockedExchange(&g_lockline, 1));
        g_lockline_wait_tsc += (long long)(__rdtsc() - t0);   /* under the lock */
        g_lockline_contended++;
    }
    g_lockline_acquires++;
    if (__builtin_expect(g_lock_hist > 0, 0)) lock_site_note(_ReturnAddress());
}
void spu_lockline_unlock(void) { _InterlockedExchange(&g_lockline, 0); }
#else
#include <stdatomic.h>
static atomic_flag g_lockline = ATOMIC_FLAG_INIT;
void spu_lockline_lock(void)   { while (atomic_flag_test_and_set_explicit(&g_lockline, memory_order_acquire)) { } }
void spu_lockline_unlock(void) { atomic_flag_clear_explicit(&g_lockline, memory_order_release); }
#endif

/* ===========================================================================
 * The reserved-line bitmap
 *
 * One bit per 128-byte line over the whole 32-bit guest address space: 4 MiB of
 * zero-initialized BSS, of which only the pages around addresses an SPU has
 * actually reserved are ever touched. The reference implementation scopes its
 * bitmap to the one window a single title's SPURS structures live in; this
 * runtime hands out guest memory from several disjoint regions (the ELF image,
 * the sys_memory window at 0x40000000, the raw-SPU local-store windows), and a
 * line outside a hardcoded window would be silently non-coherent, which is the
 * failure this whole file exists to remove.
 * ===========================================================================*/
#define SPU_COH_LINE_SHIFT  7                                  /* 128-byte lines */
#define SPU_COH_LINES       (1u << (32 - SPU_COH_LINE_SHIFT))   /* 2^25 lines */
#define SPU_COH_BITMAP_SZ   (SPU_COH_LINES / 8)                 /* 4 MiB */

/* [NHL local patch #147] exported: ppu_recomp.h's inline stores read it directly. */
unsigned char g_spu_coh_bitmap[SPU_COH_BITMAP_SZ];
#define s_coh_bitmap g_spu_coh_bitmap
/* [NHL local patch #271] Lifted stores read this: non-zero = out of line, which does the exact
 * line check. One byte per 8 lines (1 KB), set with the bitmap, plus vm_gate_init's regions. */
unsigned char g_vm_st_gate[SPU_COH_BITMAP_SZ];

/* Zero until the first GETLLAR anywhere. Keeps the PPU fast path off the
 * bitmap entirely for a title that runs no SPU code, and keeps every existing
 * test that stores through vm_write* paying one compare against a hot global.
 * Written once, under the lock; read unlocked, where a stale zero costs at most
 * one missed event on the very first reservation of the run. */
int g_spu_coh_armed;
#define s_coh_armed g_spu_coh_armed

/* Every live virtual context may retain a reservation between scheduling
 * quanta; physical SPU count is not a bound on this set. All access is under
 * the lock-line lock. Nodes are removed before their context's lifetime ends. */
typedef struct spu_coh_entry {
    spu_context* ctx;
    struct spu_coh_entry* next;
    int slot;
    uint32_t prog, job, job_lsa;   /* #278 */
} spu_coh_entry;
static spu_coh_entry* s_coh_ctxs;

unsigned long g_spu_lr_raise = 0;

/* [NHL local patch #274] A store to a marked line took the lock and walked every context, and
 * under 1% found a holder: marks are sticky, so most marked lines are held by nobody (lock2:
 * 3.3M locked PPU stores/s, the lock word bouncing between cores). Clearing the marks froze a
 * match (play212), so they stay. Instead each context publishes the line of its last GETLLAR,
 * and a store (ppu_loader.cpp VM_WRITE_COH) may skip the lock when it can show nobody holds it:
 *
 *   store path:  s = seq; if odd -> lock path; store; fence; if seq != s or line held -> lock path
 *   GETLLAR:     publish line; fence; read the line          (spu_channels.c, under the lock)
 *   PUTLLC/UC:   seq++ (odd); compare + commit; seq++ (even)  (spu_channels.c, under the lock)
 *
 * The fences mean a store that lands after a GETLLAR's read sees the published line, and one
 * that lands inside a commit, which would be written straight back over, sees seq move. Both
 * take the old lock path, which redoes the store and notifies. A published line is never
 * cleared: stale entries only send a store down the lock path. NHL_COH_FAST / the place file. */
#define COH_SLOTS 32
volatile int g_coh_fast;
volatile long g_coh_commit_seq;
static volatile uint32_t s_resv_line[COH_SLOTS];   /* line | 1, 0 = free */
static volatile int s_coh_nslots;
static volatile int s_coh_no_slot;                 /* a holder without a slot: always lock */
static uint32_t s_slot_used;

/* [NHL local patch #278] Timeline recorder (boot_main.cpp, NHL_TL_FILE): per registered SPU, by
 * slot, the last program (LS < 0x400) and job loaded and the current pc; ctx->image_id does not
 * follow programs. Read without the lock. Entries only leave at thread exit, and a context that
 * has never reserved a line is not listed, which is fine for the job kernels. */
int g_tl_on;
void spu_coh_note_code(spu_context* ctx, uint32_t ea, uint32_t lsa)
{
    for (spu_coh_entry* e = s_coh_ctxs; e; e = e->next)
        if (e->ctx == ctx) {
            if (lsa < 0x400u) e->prog = ea;
            else { e->job = ea; e->job_lsa = lsa; }
            return;
        }
}
void spu_coh_timeline(uint32_t* out, int slots)
{
    memset(out, 0, (size_t)slots * 4 * sizeof *out);
    for (spu_coh_entry* e = s_coh_ctxs; e; e = e->next)
        if (e->slot >= 0 && e->slot < slots) {
            uint32_t* o = out + e->slot * 4;
            const uint32_t pc = (uint32_t)e->ctx->pc;
            o[0] = e->prog; o[1] = 0; o[2] = 0; o[3] = pc;
            /* the resident table names the job at pc; several stay resident at once */
            for (int i = 0; i < SPU_RESIDENT_SLOTS; i++) {
                const uint32_t lsa = e->ctx->resident_code[i].lsa;
                if (lsa >= 0x400u && pc >= lsa && pc - lsa < e->ctx->resident_code[i].size) {
                    o[1] = e->ctx->resident_code[i].source_ea; o[2] = lsa;
                    break;
                }
            }
        }
}

int spu_coh_line_held(uint32_t addr)
{
    if (s_coh_no_slot) return 1;
    const uint32_t want = (addr & ~127u) | 1u;
    const int n = s_coh_nslots;
    for (int i = 0; i < n; i++)
        if (s_resv_line[i] == want) return 1;
    return 0;
}
static void coh_publish(int slot, uint32_t ea)
{
    const uint32_t v = (ea & ~127u) | 1u;
    if (slot < 0 || s_resv_line[slot] == v) return;
    s_resv_line[slot] = v;
    MemoryBarrier();
}
static int coh_slot_alloc(void)
{
    for (int i = 0; i < COH_SLOTS; i++)
        if (!(s_slot_used & (1u << i))) {
            s_slot_used |= 1u << i;
            if (i >= s_coh_nslots) s_coh_nslots = i + 1;
            return i;
        }
    s_coh_no_slot = 1;
    MemoryBarrier();
    return -1;
}
static void coh_slot_free(int slot)
{
    if (slot < 0) return;
    s_resv_line[slot] = 0;
    s_slot_used &= ~(1u << slot);
}

void spu_coh_reserve(spu_context* ctx, uint32_t ea)
{
    uint32_t line = ea >> SPU_COH_LINE_SHIFT;
    /* [NHL local patch #271] Written only on a change: every lifted store reads these tables,
     * so rewriting the same byte on each GETLLAR would bounce its cache line between cores. */
    const unsigned char bit = (unsigned char)(1u << (line & 7));
    if (!(s_coh_bitmap[line >> 3] & bit)) s_coh_bitmap[line >> 3] |= bit;
    if (!g_vm_st_gate[line >> 3]) g_vm_st_gate[line >> 3] = 1;
    if (!s_coh_armed) s_coh_armed = 1;

    if (!ctx) {                     /* a holder that cannot publish */
        s_coh_no_slot = 1;
        MemoryBarrier();
        return;
    }
    for (spu_coh_entry* entry = s_coh_ctxs; entry; entry = entry->next)
        if (entry->ctx == ctx) { coh_publish(entry->slot, ea); return; }
    spu_coh_entry* entry = (spu_coh_entry*)malloc(sizeof(*entry));
    if (!entry) {
        fprintf(stderr, "[spu-coh] cannot register reservation holder %p\n", (void*)ctx);
        fflush(stderr);
        abort(); /* A missed holder loses invalidation events, including ABA. */
    }
    entry->ctx = ctx;
    entry->next = s_coh_ctxs;
    entry->prog = entry->job = entry->job_lsa = 0;
    entry->slot = coh_slot_alloc();
    s_coh_ctxs = entry;
    coh_publish(entry->slot, ea);
}

/* Drop a context from the reserving set.
 *
 * There was no way to do this, and the contexts that most need it are
 * TRANSIENT: spu_run_lifted_job_abi runs each job on a STACK-LOCAL
 * spu_context, which registered itself here on its first reservation and then
 * returned. The entry outlived the object, so spu_coh_notify_write walked a
 * dangling pointer -- reading resv_valid, writing event_status and calling
 * spu_ch_wake on reclaimed stack. Undefined behaviour, and observable: Guitar
 * Hero III showed THREE live-looking contexts for one dispatch, stealing each
 * other`s reservations, with every PUTLLC failing "no reservation". */
void spu_coh_unregister(spu_context* ctx)
{
    if (!ctx) return;
    /* Under the lock-line lock, which every walker of s_coh_ctxs holds: a
     * notifier that loaded this pointer just before removal would
     * otherwise go on dereferencing it after the job returned and its stack
     * (or its whole async thread) was gone. GH3 at -O2 crashed that way in
     * spu_coh_notify_write, reading a freed thread stack. */
    spu_lockline_lock();
    for (spu_coh_entry** link = &s_coh_ctxs; *link; link = &(*link)->next) {
        if ((*link)->ctx != ctx) continue;
        spu_coh_entry* entry = *link;
        *link = entry->next;
        coh_slot_free(entry->slot);
        free(entry);
        break;
    }
    spu_lockline_unlock();
}

/* Drop every registered context that lies in [lo, hi) -- a host thread's
 * stack, called as that thread exits. A stack-local context still registered
 * then has leaked past its unregister; the next walker would read an unmapped
 * stack (GH3 at the end of a Havok task: spu_coh_notify_write faulting).
 * ponytail: safety net that also names the leak; find the path that skips
 * spu_coh_unregister if it keeps firing. */
void spu_coh_forget_range(uintptr_t lo, uintptr_t hi)
{
    spu_lockline_lock();
    for (spu_coh_entry** link = &s_coh_ctxs; *link;) {
        spu_coh_entry* entry = *link;
        uintptr_t c = (uintptr_t)entry->ctx;
        if (c >= lo && c < hi) {
            static int _n = 0;
            if (_n++ < 16)
                fprintf(stderr, "[spu-coh] leaked ctx %p (img=%d) dropped at thread exit\n",
                        (void*)c, entry->ctx->image_id);
            *link = entry->next;
            coh_slot_free(entry->slot);
            free(entry);
        } else {
            link = &entry->next;
        }
    }
    spu_lockline_unlock();
}

int spu_coh_is_reserved(uint32_t addr)
{
    if (!s_coh_armed) return 0;
    uint32_t line = addr >> SPU_COH_LINE_SHIFT;
    return (s_coh_bitmap[line >> 3] >> (line & 7)) & 1u;
}

/* A DMA PUT is issued BY an SPU, and hardware does not take that SPU's own
 * reservation away for its own MFC write -- the MFC is part of the same SPE.
 * Our notify had no way to say "everyone but me", so an SPU that reserved a
 * line and then DMA'd a buffer overlapping it killed its own reservation and
 * its next PUTLLC could never succeed. Guitar Hero III's job policy module
 * livelocks exactly there: 16 million atomic ops on one line, every PUTLLC
 * failing for "no reservation", 600k of them self-inflicted.
 *
 * The PUTLLC path already guards this case by dropping its own reservation
 * before notifying; this gives the DMA path the same ability. */
/* [NHL local patch #153] Marks used to be sticky: once any SPU had reserved a line,
 * every later PPU store to it took the lock-line lock and walked every context, for
 * the rest of the run (NHL's render thread: spu_lockline_lock + spu_coh_notify_write
 * ~5-6%, play207). After a notify no context holds a reservation on the line (they
 * were all dropped, or `self` kept its own), so the mark is cleared there -- under
 * the lock, as spu_coh_reserve sets it -- and the next GETLLAR sets it again. A
 * store racing a new reservation is no worse than on a never-marked line: PUTLLC
 * still compares the line against its snapshot. SPU_COH_STICKY=1 = the old marks. */
static void spu_coh_notify_write_one(uint32_t line);
static void spu_coh_unmark(uint32_t line)
{
    /* OPT-IN since play212: with unmarking on, the match froze at t=59 (main in the
     * Job Manager wait, all SPUs idle in their queue poll) -- a lost update or
     * wake-up through the window between a PPU store's unlocked mark check and a new
     * GETLLAR is the suspect (#154's priority knobs were also on). The sticky marks
     * close that window; SPU_COH_UNMARK=1 = clear them (UNSAFE, experiments only). */
    static int s_sticky = -1;
    if (s_sticky < 0) { const char* e = getenv("SPU_COH_UNMARK"); s_sticky = !(e && *e == '1'); }
    if (s_sticky) return;
    const uint32_t l = line >> SPU_COH_LINE_SHIFT;
    s_coh_bitmap[l >> 3] &= (unsigned char)~(1u << (l & 7));
}

void spu_coh_notify_write_except(uint32_t ea, const void* self)
{
    uint32_t line = ea & ~127u;
    int self_holds = 0;
    for (spu_coh_entry* entry = s_coh_ctxs; entry; entry = entry->next) {
        spu_context* c = entry->ctx;
        if (!c) continue;
        if ((const void*)c == self) {
            self_holds = c->resv_valid && (c->resv_ea & ~127u) == line;
            continue;
        }
        if (c->resv_valid && (c->resv_ea & ~127u) == line) {
            c->event_status |= SPU_EVENT_LR;
            c->resv_valid = 0;
            g_spu_lr_raise++;
            spu_ch_wake(c);
        }
    }
    if (!self_holds) spu_coh_unmark(line);
}

/* [NHL local patch #273] NHL_LOCK_HIST=1: which lines take these notifies, and whether any
 * of them finds a holder. Called under the lock-line lock, so plain counters. */
#define COH_LINES 256
extern int g_lock_hist;
static uint32_t s_cl_line[COH_LINES];
static long long s_cl_n[COH_LINES], s_cl_hit[COH_LINES];
static void coh_line_note(uint32_t line, int hit)
{
    unsigned k = ((line >> 7) * 2654435761u) % COH_LINES;
    for (int i = 0; i < 16; i++, k = (k + 1) % COH_LINES) {
        if (!s_cl_line[k]) s_cl_line[k] = line;
        if (s_cl_line[k] == line) { s_cl_n[k]++; s_cl_hit[k] += hit; return; }
    }
}
void spu_coh_lines_report(int secs)
{
    long long tot = 0, hits = 0;
    for (int i = 0; i < COH_LINES; i++) { tot += s_cl_n[i]; hits += s_cl_hit[i]; }
    fprintf(stderr, "[COHLINE] notifies %lld/s, found a holder %lld/s\n", tot / secs, hits / secs);
    for (int t = 0; t < 12; t++) {
        int b = -1; long long bn = 0;
        for (int i = 0; i < COH_LINES; i++) if (s_cl_n[i] > bn) { bn = s_cl_n[i]; b = i; }
        if (b < 0 || bn / secs < 1000) break;
        fprintf(stderr, "[COHLINE] line %08X %lld/s holder %lld/s\n", s_cl_line[b], bn / secs, s_cl_hit[b] / secs);
        s_cl_n[b] = 0; s_cl_hit[b] = 0;
    }
    memset(s_cl_n, 0, sizeof s_cl_n);
    memset(s_cl_hit, 0, sizeof s_cl_hit);
}

void spu_coh_notify_write(uint32_t ea)
{
    uint32_t line = ea & ~127u;
    if (__builtin_expect(g_lock_hist > 0, 0)) {
        const unsigned long before = g_spu_lr_raise;
        spu_coh_notify_write_one(line);
        coh_line_note(line, g_spu_lr_raise != before);
        spu_coh_unmark(line);
        return;
    }
    spu_coh_notify_write_one(line);
    spu_coh_unmark(line);
}

static void spu_coh_notify_write_one(uint32_t line)
{
    for (spu_coh_entry* entry = s_coh_ctxs; entry; entry = entry->next) {
        spu_context* c = entry->ctx;
        if (!c) continue;
        if (c->resv_valid && (c->resv_ea & ~127u) == line) {
            c->event_status |= SPU_EVENT_LR;
            c->resv_valid = 0;          /* reservation lost, PUTLLC must fail */
            /* SPU_PUTLLC_WHY=1: name the agent that killed it. A PUTLLC that
             * always fails for "no reservation" is useless without knowing who
             * took it away -- a peer SPU, or the PPU committing to the line. */
            { static int s_w = -1;
              if (s_w < 0) s_w = getenv("SPU_PUTLLC_WHY") ? 1 : 0;
              if (s_w) { static unsigned long long n;
                  if ((++n % 200000) == 1)
                      fprintf(stderr, "[resv-lost] %llu: line 0x%08X taken from img=%d ctx=%p\n",
                              n, line, c->image_id, (void*)c); } }
            g_spu_lr_raise++;
            /* The event_status store has to be visible before the wake. The
             * waiter re-polls its predicate on a timeout as well, so a
             * straggling store costs latency and not the wakeup itself. */
            spu_ch_wake(c);
        }
    }
}

#ifdef __cplusplus
}
#endif
