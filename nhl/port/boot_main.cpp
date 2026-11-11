/*
 * ps3recomp - integrated PPU boot harness.
 *
 * Links the whole PPU runtime half into one executable and starts executing a
 * recompiled program's entry point:
 *
 *   lifted code (ppu_recomp.c) + loader (ppu_loader.cpp) + HLE bridge
 *   (ppu_hle.cpp + generated NID table) + HLE libs (cellGcmSys, rsx_commands)
 *
 * It loads the PPU ELF named on the command line, registers the lifted
 * functions and the HLE NID handlers, starts the frame clock, then dispatches
 * the entry OPD. Execution runs real guest code until it reaches a function
 * outside the lifted subset (logged by the unlifted stub), an unimplemented
 * firmware import (logged by ps3_hle_call), or an lv2 syscall (logged by
 * lv2_syscall) -- telling us exactly what to implement next.
 *
 * Nothing here is title-specific: the ELF path, the VFS root derived from it
 * and the window title all come from the command line or the environment. Two
 * things drive it today -- a game port, which supplies its own lifted
 * ppu_recomp.c and its own HLE overrides, and the smoke title in
 * tests/smoke/, which is the same harness with a synthetic program in it and
 * is what proves this path runs at all on POSIX.
 */
#include <stdarg.h>
#include "ppu_recomp.h"
#include "ps3emu/milestone.h"   /* ps3_ms / ps3_ms_kv -- boot milestone log */
/* PPU_THREAD_LOCAL only -- NOT ppu_context.h, which would redefine the struct
 * a generated ppu_recomp.h already declares. Ports generated before the
 * qualifier existed do not define it, so the scaffold must carry its own
 * reachable definition. */
#include "ppu_tls.h"
#include "win32_compat.h"      /* Sleep, GetTickCount64, threads on POSIX */
#include "win32_backtrace.h"   /* RtlCaptureStackBackTrace / GetModuleHandleA on POSIX */
/* The lifted ppu_recomp.h already defines `struct ppu_context` (identical to
 * runtime/ppu/ppu_context.h by design -- "keep the two in sync"). A syscall
 * header included later (sys_ppu_thread.h -> lv2_syscall_table.h -> ppu_context.h)
 * would redefine it, so claim ppu_context.h's guard now: this TU has the struct,
 * and boot_main uses none of that header's PPU_CR or XER bit macros. */
#ifndef PPU_CONTEXT_H
#define PPU_CONTEXT_H
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

extern "C" {
uint32_t ppu_load_elf(const char* path);
void     ppu_recomp_register(void);
void     ppu_hle_init(void);
uint32_t ps3_hle_count(void);   /* handlers actually registered, for the milestone log */
void     ppu_sysprx_register(void);
void     ppu_fs_register(void);
int      ppu_run(uint32_t entry_opd, uint32_t stack_top);
extern const char* ppu_vfs_root;   /* host dir that PS3 mount points map into */
/* The status a guest handed to sys_process_exit, published by
 * libs/system/sysPrxForUser.c before it calls the host exit(). A title that
 * exits that way never returns through ppu_run, so this is only read on the
 * path where the guest fell out of its entry function instead. */
extern int     g_sys_process_exit_called;
extern int32_t g_sys_process_exit_code;
void     cellGame_init_from_paramsfo(const char* sfo_path);  /* libs/system/cellGame.c */
void     cellfs_add_path_mapping(const char* ps3_prefix, const char* host_path);  /* NHL: libs/filesystem/cellFs.c */
/* Optional hook: load real system PRX modules (libsre = cellSpurs/cellSync) into
 * guest RAM and register their exports. Weak default is a no-op; a title that
 * links a lifted PRX defines a strong version. Called after the lifted function
 * table is registered and vm_base is live, before the game runs. */
void     ps3_load_prx_modules(void) __attribute__((weak));
void     ps3_load_prx_modules(void) {}
}

#include <string.h>
#include <stdlib.h>
#include <signal.h>

#ifdef _WIN32
#include <windows.h>
#include <intrin.h>   /* _ReturnAddress */
/* timeBeginPeriod: windows.h is included with WIN32_LEAN_AND_MEAN, which
 * excludes the multimedia timer API, so it must be asked for by name -- and
 * AFTER windows.h, since timeapi.h uses UINT and friends. */
#include <timeapi.h>

/* Process-wide getenv cache. The runtime has ~700 `if (getenv("X"))` diagnostic
 * gates, many on per-draw / per-syscall paths, and the UCRT getenv takes the
 * environment lock and scans every variable each time: profiled at ~30% of
 * GH3's main PPU thread and ~35% of the RSX thread in gameplay. Defining the
 * import slot here (an object in the link beats the import library's lazy
 * definition) routes every dllimport'ed getenv call through this cache.
 * Nothing in the runtime mutates the environment after startup, so a value is
 * looked up once and the pointer stays valid forever. Reads are lock-free:
 * a slot's key is published last, after its value.
 * ponytail: 1024 open-addressed slots, fixed; names past that fall back to the OS lookup. */
namespace {
struct EnvSlot { const char* volatile key; const char* val; };
EnvSlot  s_env[1024];
SRWLOCK  s_env_lock = SRWLOCK_INIT;

unsigned env_hash(const char* s)
{
    unsigned h = 2166136261u;
    for (; *s; s++) h = (h ^ (unsigned char)(*s >= 'a' && *s <= 'z' ? *s - 32 : *s)) * 16777619u;
    return h;
}

const char* env_lookup_os(const char* name)
{
    DWORD n = GetEnvironmentVariableA(name, NULL, 0);
    if (!n) return NULL;
    char* v = (char*)malloc(n);
    if (!v || GetEnvironmentVariableA(name, v, n) >= n) { free(v); return NULL; }
    return v;
}

char* __cdecl cached_getenv(const char* name)
{
    if (!name || !*name) return NULL;
    const unsigned mask = 1023, h = env_hash(name);
    for (unsigned i = h & mask, n = 0; n <= mask; n++, i = (i + 1) & mask) {
        const char* k = s_env[i].key;
        _ReadWriteBarrier();   /* #228: x64 keeps load order; the writer's MemoryBarrier publishes val first */
        if (!k) break;
        if (!_stricmp(k, name)) {
            /* GETENV_STATS=1: which gate is being polled on a hot path. */
            static int st = -1;
            if (st < 0) st = env_lookup_os("GETENV_STATS") ? 1 : 0;
            if (st) {
                static unsigned cnt[1024]; static void* ra[1024]; static unsigned long long tot;
                cnt[i]++; ra[i] = _ReturnAddress();
                if ((++tot & ((1u << 22) - 1)) == 0) {
                    const uintptr_t exe = (uintptr_t)GetModuleHandleA(NULL);
                    for (int r = 0; r < 5; r++) {
                        unsigned b = 0;
                        for (unsigned j = 1; j <= mask; j++) if (cnt[j] > cnt[b]) b = j;
                        if (!cnt[b]) break;
                        fprintf(stderr, "[getenv-stats] %-28s %u calls  caller rva=0x%llX%c",
                                s_env[b].key, cnt[b], (unsigned long long)((uintptr_t)ra[b] - exe), 10);
                        cnt[b] = 0;
                    }
                    memset(cnt, 0, sizeof cnt);
                }
            }
            return (char*)s_env[i].val;
        }
    }
    AcquireSRWLockExclusive(&s_env_lock);
    const char* val = NULL; int found = 0;
    unsigned i = h & mask, n = 0;
    for (; n <= mask; n++, i = (i + 1) & mask) {
        if (!s_env[i].key) break;
        if (!_stricmp(s_env[i].key, name)) { val = s_env[i].val; found = 1; break; }
    }
    if (!found) {
        val = env_lookup_os(name);
        if (n <= mask) {
            s_env[i].val = val;
            MemoryBarrier();
            s_env[i].key = _strdup(name);
        }
    }
    ReleaseSRWLockExclusive(&s_env_lock);
    return (char*)val;
}
}
#ifndef NHL_PGO_GEN   /* #156: the clang profile runtime imports getenv itself -> duplicate __imp_getenv */
extern "C" char* (__cdecl* __imp_getenv)(const char*) = cached_getenv;
#endif
/* #221: nhl_config_load sets the built-in defaults at the top of main(); drop
 * anything cached before that (static initialisers). Single-threaded then; the
 * old keys/values leak, a few bytes once. */
extern "C" void nhl_env_cache_reset(void)
{
    AcquireSRWLockExclusive(&s_env_lock);
    for (EnvSlot& e : s_env) { e.key = NULL; e.val = NULL; }
    ReleaseSRWLockExclusive(&s_env_lock);
}
void        nhl_config_load(const char* config);   /* nhl_config.cpp */
const char* nhl_config_eboot(void);

/* Last-chance crash reporter: vm_base accesses are bounds-guarded, so a real
 * access violation means a HOST pointer deref (e.g. a bad function pointer or a
 * runtime-struct walk). Print the faulting address and the RIP as a module
 * offset (RVA) so it can be symbolized with llvm-symbolizer against the PDB. */
extern "C" uint32_t    g_last_hle_nid;    /* ppu_hle.cpp breadcrumb */
extern "C" const char* g_last_hle_name;

extern "C" PPU_THREAD_LOCAL ppu_context* g_active_ctx;
static LONG WINAPI ydkj_crash_filter(EXCEPTION_POINTERS* ep)
{
    EXCEPTION_RECORD* er = ep->ExceptionRecord;
    fprintf(stderr, "\n[CRASH] code=0x%08lX rip=%p\n",
            (unsigned long)er->ExceptionCode, er->ExceptionAddress);
    fprintf(stderr, "[CRASH] last HLE NID 0x%08X (%s)\n",
            g_last_hle_nid, g_last_hle_name ? g_last_hle_name : "");
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2)
        fprintf(stderr, "[CRASH] %s fault address 0x%llX\n",
                er->ExceptionInformation[0] ? "write" : "read",
                (unsigned long long)er->ExceptionInformation[1]);
    if (g_active_ctx) fprintf(stderr, "[CRASH] guest ctr=0x%08X lr=0x%08X r3=0x%08X\n",
          (uint32_t)g_active_ctx->ctr, (uint32_t)g_active_ctx->lr, (uint32_t)g_active_ctx->gpr[3]);
    HMODULE mod = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)er->ExceptionAddress, &mod);
    fprintf(stderr, "[CRASH] module=%p rva=0x%llX  (llvm-symbolizer --obj=ydkj_boot.exe 0x%llX)\n",
            (void*)mod, (unsigned long long)((char*)er->ExceptionAddress - (char*)mod),
            (unsigned long long)((char*)er->ExceptionAddress - (char*)mod));
    /* Host call stack (RVAs) so the lifted caller can be symbolized. */
    void* frames[24];
    USHORT n = RtlCaptureStackBackTrace(0, 24, frames, NULL);
    for (USHORT i = 0; i < n; i++) {
        HMODULE m = NULL;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)frames[i], &m);
        if (m == mod)
            fprintf(stderr, "[CRASH]   #%-2u rva=0x%llX\n", i,
                    (unsigned long long)((char*)frames[i] - (char*)m));
    }
    fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

#ifdef _WIN32
/* abort()/exit(3) reporter: the recompiled CRT (or a failed invariant) can call
 * abort() â€” Windows turns that into exit code 3 with no message. Capture a host
 * backtrace (RVAs) + the last HLE NID so the aborting caller can be symbolized. */
static void ydkj_abort_handler(int)
{
    fprintf(stderr, "\n[ABORT] SIGABRT raised; last HLE NID 0x%08X (%s)\n",
            g_last_hle_nid, g_last_hle_name ? g_last_hle_name : "");
    void* frames[32];
    USHORT n = RtlCaptureStackBackTrace(0, 32, frames, NULL);
    HMODULE self = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&ydkj_abort_handler, &self);
    for (USHORT i = 0; i < n; i++) {
        HMODULE m = NULL;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)frames[i], &m);
        if (m == self)
            fprintf(stderr, "[ABORT]   #%-2u rva=0x%llX\n", i,
                    (unsigned long long)((char*)frames[i] - (char*)m));
    }
    fflush(stderr);
    _exit(3);
}
#endif

/* Derive the VFS root (the dir containing PS3_GAME) from the EBOOT path
 * <root>/PS3_GAME/USRDIR/EBOOT.elf  -> <root>. $PS3_VFS_ROOT overrides. */
static char s_vfs_root[1024];
static void derive_vfs_root(const char* eboot)
{
    const char* env = getenv("PS3_VFS_ROOT");
    if (env && *env) { ppu_vfs_root = env; return; }
    strncpy(s_vfs_root, eboot, sizeof s_vfs_root - 1);
    for (char* p = s_vfs_root; *p; p++) if (*p == '\\') *p = '/';
    /* strip three trailing components: EBOOT.elf / USRDIR / PS3_GAME */
    for (int i = 0; i < 3; i++) { char* s = strrchr(s_vfs_root, '/'); if (s) *s = 0; }
    if (!s_vfs_root[0]) strcpy(s_vfs_root, ".");
    ppu_vfs_root = s_vfs_root;
}

/* Host-provided symbols the runtime + HLE libs need. */
extern "C" uint8_t* vm_base = nullptr;
extern "C" uint32_t ppu_vm_size;   /* defined in ppu_loader.cpp (OOB guard) */
extern "C" void vm_gate_init(void);   /* ppu_loader.cpp, #271 */
extern "C" void lv2_init_syscalls(void);   /* runtime/syscalls/lv2_register.c */
extern "C" void np_psnr_setup(const char* username, const char* server);  /* libs/network/np_psnr.c */

/* Guest-callback dispatch + RSX vblank/flip driver.
 *
 * g_ps3_guest_caller (defined NULL by libs/system/cellSysutil.c) is the hook the
 * HLE runtime uses to call back into recompiled code -- cellSysutil events and
 * the GCM vblank/flip handlers. ppu_guest_call (ppu_loader.cpp) does the OPD ->
 * dispatch. On real hardware the RSX fires a vblank interrupt ~60x/s that drives
 * the game's frame loop; with no RSX we synthesize it from a host timer thread
 * calling cellGcmTickVBlank()/TickFlip(), which invoke the registered handlers.
 * Without this the game inits, registers its handlers, and then waits forever
 * for a vblank that never comes. */
typedef void (*ps3_guest_caller_fn)(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t,
                                    uint64_t, uint64_t, uint64_t, uint64_t);
extern "C" ps3_guest_caller_fn g_ps3_guest_caller;        /* libs/system/cellSysutil.c */
extern "C" uint64_t ppu_guest_call(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t,
                                   uint64_t, uint64_t, uint64_t, uint64_t);
extern "C" void cellGcmTickVBlank(void);
extern "C" void cellGcmTickFlip(void);
extern "C" int  cellGcm_take_flip_pending(void);  /* hoisted to file scope: clang-cl rejects block-scope extern "C" */
extern "C" int  ps3_scsum_format(int slot, char* out, int cap);   /* ppu_loader.cpp, #145 PS3_SCSUM */
extern "C" void spu_putmap_tick(double t);                          /* spu_channels.c, #148 SPU_PUTMAP */
#ifdef NHL_PGO_GEN
extern "C" int  __llvm_profile_write_file(void);                     /* clang profile runtime, #156 */
#endif

static void harness_guest_caller(uint32_t opd, uint64_t a0, uint64_t a1,
                                 uint64_t a2, uint64_t a3, uint64_t a4,
                                 uint64_t a5, uint64_t a6, uint64_t a7)
{ ppu_guest_call(opd, a0, a1, a2, a3, a4, a5, a6, a7); }

/* RSX present backend. D3D12 on Windows, Metal on Apple, and the null
 * backend's headless software path anywhere else -- the same selection
 * runtime/host/host_posix.c makes, so the ticker below is backend-agnostic
 * and a POSIX host gets a frame clock instead of no clock at all. */
#if defined(_WIN32)
extern "C" int  rsx_d3d12_backend_init(uint32_t w, uint32_t h, const char* title);
extern "C" void rsx_d3d12_backend_present(void);
extern "C" int  rsx_d3d12_backend_pump_messages(void);
extern "C" int  rsx_d3d12_backend_vblank(void);         /* #164: issue a VRR present held for the tick */
extern "C" int  rsx_d3d12_backend_present_held(void);   /* #164 */
#  define rsx_backend_init    rsx_d3d12_backend_init
#  define rsx_backend_present rsx_d3d12_backend_present
#  define rsx_backend_pump    rsx_d3d12_backend_pump_messages
#elif defined(__APPLE__)
extern "C" int  rsx_metal_backend_init(uint32_t w, uint32_t h, const char* title);
extern "C" void rsx_metal_backend_present(void);
extern "C" int  rsx_metal_backend_pump_messages(void);
#  define rsx_backend_init    rsx_metal_backend_init
#  define rsx_backend_present rsx_metal_backend_present
#  define rsx_backend_pump    rsx_metal_backend_pump_messages
#else
extern "C" int  rsx_null_backend_init(uint32_t w, uint32_t h, const char* title);
extern "C" void rsx_null_backend_present(void);
extern "C" int  rsx_null_backend_pump_messages(void);
#  define rsx_backend_init    rsx_null_backend_init
#  define rsx_backend_present rsx_null_backend_present
#  define rsx_backend_pump    rsx_null_backend_pump_messages
#endif
extern "C" void cellGcm_rsx_process_fifo(void);   /* cellGcmSys.c: drain get->put */
extern "C" void cellGcm_fifo_kick_wait(unsigned ms); /* idle wait, woken by a kick */
extern "C" unsigned cellGcm_flip_request_count(void);
extern "C" unsigned cellGcmGetCurrentDisplayBufferId(void);
extern "C" int sys_event_queue_inject(unsigned int, unsigned long long, unsigned long long, unsigned long long, unsigned long long);

/* Live NV4097->D3D12 engine (libs/video/rsx_live_draw.c, from caner /
 * canersaka's Yakuza: Dead Souls port). Opt-in with RSX_LIVE_DRAW=1: the null
 * backend opens the window, the engine binds its swap chain to that HWND and
 * owns presentation, so rsx_d3d12_backend is left out entirely rather than run
 * alongside it. Unset, every path below is exactly what it was. */
extern "C" int   rsx_live_draw_enabled(void);
extern "C" int   rsx_live_draw_init(void* hwnd, uint32_t w, uint32_t h,
                                    const uint8_t* (*guest_ptr)(void*, uint32_t,
                                                                uint32_t, uint32_t),
                                    void* user);
extern "C" void  rsx_live_draw_present(uint32_t buffer_id);
extern "C" uint32_t rsx_live_draw_get_frames(void);
extern "C" uint32_t rsx_live_draw_get_last_draws(void);
extern "C" int   rsx_null_backend_init(uint32_t w, uint32_t h, const char* title);
extern "C" int   rsx_null_backend_pump_messages(void);
extern "C" void* rsx_null_backend_get_hwnd(void);
extern "C" void  rsx_null_backend_suppress_present(int on);
extern "C" uint32_t cellGcmResolveLocated(int local, uint32_t offset);
extern "C" uint32_t cellGcmResolveIO(uint32_t offset);

static int s_rsx_live = 0;   /* live engine selected AND up */

/* Resolve (location, offset) to host memory for the engine. location 0 is RSX
 * local VRAM, 1 is main/IO memory; the engine promises its callers the whole
 * min_bytes span is readable, so validate the interval, not just its start.
 * The location comes straight from the RSX DMA context selector and is
 * authoritative -- cellGcmResolveOffset()'s heuristic prefers VRAM for any page
 * the guest ever derived from a local EA, which maps a title's MAIN-memory
 * textures into local memory where they read as garbage (caner hit exactly
 * that in the Yakuza port; the geometry came out flat white). */
static const uint8_t* rsx_live_guest_ptr(void* user, uint32_t location,
                                         uint32_t offset, uint32_t min_bytes)
{
    (void)user;
    if (!vm_base) return nullptr;
    uint32_t ea;
    if (location == 0) {
        ea = cellGcmResolveLocated(1, offset);          /* RSX local VRAM */
    } else {
        ea = cellGcmResolveIO(offset);                  /* main, via the IO table */
        if (!ea) ea = cellGcmResolveLocated(0, offset); /* unmapped: old behaviour */
    }
    if (!ea || ea == 0xFFFFFFFFu ||
        (uint64_t)ea + min_bytes > 0x100000000ull) return nullptr;
    return (const uint8_t*)vm_base + ea;
}

/* Present / pump through whichever backend is live. */
static void rsx_present_frame(void)
{
    /* Present the buffer the guest actually flipped to, not buffer 0.
     *
     * rsx_live_draw_present() looks up the surface registered for the display
     * buffer it is given, so hardcoding 0 presented buffer 0's surface for
     * EVERY flip. A double-buffered title flips 0,1,0,1..., so half its frames
     * showed the previous image instead of the one just drawn -- content
     * appearing and vanishing at ~30 Hz, and black wherever buffer 0 had not
     * been drawn yet. The Simpsons Arcade Game flips 0,1 alternately and that
     * is exactly what it looked like.
     *
     * cellGcmGetCurrentDisplayBufferId() is set by cellGcmSetFlipCommand,
     * which the FIFO walker calls on this same thread before setting the
     * pending flag we are responding to, so it is current here. */
    if (s_rsx_live) rsx_live_draw_present(cellGcmGetCurrentDisplayBufferId());
    else            rsx_backend_present();   /* platform backend: D3D12 / Metal / null */
}
static int rsx_pump_messages(void)
{ return s_rsx_live ? rsx_null_backend_pump_messages() : rsx_backend_pump(); }

/* #222: the window's X (or Alt+F4) only destroyed the window -- the guest kept
 * running headless, audio and all, and the launcher waited on it forever. Now a
 * close asks the game to quit the way the PS button menu does
 * (CELL_SYSUTIL_REQUEST_EXITGAME on NHL's callback slot 0); if it has not called
 * sys_process_exit within NHL_CLOSE_GRACE_MS (default 3000), the process ends. */
extern "C" void cellSysutilQueueEvent(int slot, uint32_t status, uint32_t param);
static void nhl_window_closed(void)
{
    static LONG once = 0;
    if (InterlockedExchange(&once, 1)) return;
    const char* g = getenv("NHL_CLOSE_GRACE_MS");
    static DWORD grace; grace = g ? (DWORD)strtoul(g, 0, 10) : 3000u;
    fprintf(stderr, "[close] window closed: requesting game exit, ending in %lu ms\n", (unsigned long)grace);
    fflush(stderr);
    cellSysutilQueueEvent(0, 0x0101 /* CELL_SYSUTIL_REQUEST_EXITGAME */, 0);
    CreateThread(NULL, 64u * 1024, [](LPVOID) -> DWORD {
        Sleep(grace);
        fprintf(stderr, "[close] grace over, ending the process\n");
        fflush(stderr);
        TerminateProcess(GetCurrentProcess(), 0);
        return 0;
    }, NULL, 0, NULL);
}

/* Frames handed to the backend at a guest FLIP boundary -- one per frame the
 * guest actually finished, which is the number a port means by "is anything
 * reaching the screen". The presents the ticker makes before the guest's first
 * flip (so a fresh window is not left white through a long boot) are
 * deliberately not counted: they carry no guest frame. */
static volatile LONG g_frames_presented = 0;
extern "C" unsigned ppu_boot_frames_presented(void) { return (unsigned)g_frames_presented; }
extern "C" int  cellGcm_tl_on(void);
extern "C" void cellGcm_tl(const char* what, uint32_t a, uint32_t b, uint32_t c, uint32_t d);
extern "C" unsigned long long ps3_qpc_us(void);
/* #122 hitch monitor state the ticker feeds (the monitor itself is further down) */
static volatile long long s_hz_fifo_us = 0, s_hz_present_us = 0;
static DWORD s_hz_main_tid = 0, s_hz_rsx_tid = 0;
static unsigned long long s_hz_t0 = 0;   /* uptime origin */
static int hz_on(void);
static void hz_on_present(void);
static void present_guest_frame(void)
{
    const unsigned long long t0 = cellGcm_tl_on() ? ps3_qpc_us() : 0;
    const unsigned long long hz0 = ps3_qpc_us();
    rsx_present_frame();
    if (t0) cellGcm_tl("present", cellGcm_flip_request_count(), 0, 0, (uint32_t)(ps3_qpc_us() - t0));
    s_hz_present_us += (long long)(ps3_qpc_us() - hz0);   /* ticker thread only */
#if defined(_WIN32)
    if (hz_on() && (s_rsx_live || !rsx_d3d12_backend_present_held())) hz_on_present();   /* #164: held -> counted at the tick */
#else
    if (hz_on()) hz_on_present();
#endif
    /* The ticker increments, a guest thread reads: interlocked rather than a
     * volatile ++, which is neither atomic nor a fence on arm64. */
    InterlockedIncrement(&g_frames_presented);
}
/* cellGcm_rsx_process_fifo, timed for the hitch monitor */
static void hz_process_fifo(void)
{
    const unsigned long long t = ps3_qpc_us();
    cellGcm_rsx_process_fifo();
    s_hz_fifo_us += (long long)(ps3_qpc_us() - t);
}

/* ---- Guest-PC sampling profiler (PS3_GUEST_PROF=1) -----------------------
 * Samples every guest thread's ctx.cia every ~5ms and dumps the top sites
 * every 5s. cia is refreshed at every syscall, and guest spin loops issue a
 * syscall per iteration (yield/usleep), so a thread stuck in a wait loop
 * shows its loop's PC dominating the histogram. This is the tool that
 * answers "where does thread X spend its wall time" without a debugger. */
#include "sys_ppu_thread.h"
static DWORD WINAPI guest_prof_thread(LPVOID)
{
    struct Site { uint32_t tid, cia; uint32_t hits; };
    static Site sites[256];
    int nsites = 0;
    ULONGLONG t0 = GetTickCount64();
    unsigned total = 0;
    for (;;) {
        Sleep(5);
        for (int i = 0; i < PPU_THREAD_MAX; i++) {
            ppu_thread_info* t = &g_ppu_threads[i];
            if (t->state != 1 /*RUNNING-ish; harmless if enum differs*/ && t->state != 2) continue;
            uint32_t cia = (uint32_t)t->ctx.cia;
            if (!cia) continue;
            uint32_t tid = (uint32_t)(i + 1);
            total++;
            int f = -1;
            for (int k = 0; k < nsites; k++)
                if (sites[k].tid == tid && sites[k].cia == cia) { f = k; break; }
            if (f < 0 && nsites < 256) { f = nsites++; sites[f].tid = tid; sites[f].cia = cia; sites[f].hits = 0; }
            if (f >= 0) sites[f].hits++;
        }
        ULONGLONG now = GetTickCount64();
        if (now - t0 >= 5000) {
            /* top 10 by hits */
            fprintf(stderr, "[gprof] %u samples/5s; top sites:\n", total);
            for (int rank = 0; rank < 10; rank++) {
                int best = -1; uint32_t bh = 0;
                for (int k = 0; k < nsites; k++)
                    if (sites[k].hits > bh) { bh = sites[k].hits; best = k; }
                if (best < 0 || bh == 0) break;
                fprintf(stderr, "[gprof]   tid=%-3u cia=0x%08X  %5.1f%%  (%s)\n",
                        sites[best].tid, sites[best].cia,
                        100.0 * sites[best].hits / (total ? total : 1),
                        g_ppu_threads[sites[best].tid - 1].name);
                sites[best].hits = 0;   /* consume */
            }
            fflush(stderr);
            nsites = 0; total = 0; t0 = now;
        }
    }
}

extern "C" const char* cellGame_get_title(void);   /* PARAM.SFO TITLE, for the caption */

static DWORD WINAPI vblank_ticker(LPVOID)
{
    s_hz_rsx_tid = GetCurrentThreadId();   /* #122 */
    /* Window caption: $PS3_TITLE wins, else the TITLE field PARAM.SFO already
     * gave cellGame, else a neutral name. It used to fall back to a specific
     * other title, so every port announced itself as that game. */
    const char* _title = getenv("PS3_TITLE");
    if (!_title || !*_title) {
        const char* sfo = cellGame_get_title();
        if (sfo && *sfo && strcmp(sfo, "Unknown Title") != 0) _title = sfo;
    }
    if (!_title || !*_title) _title = "ps3recomp";
    /* The live engine's RT-as-backbuffer rescue only treats a surface as the
     * backbuffer when its clip EQUALS the backend size, so a title that renders
     * into something other than 1280x720 must say so: RSX_W / RSX_H. */
    uint32_t rsx_w = 1280, rsx_h = 720;
    if (const char* e = getenv("RSX_W")) rsx_w = (uint32_t)strtoul(e, 0, 0);
    if (const char* e = getenv("RSX_H")) rsx_h = (uint32_t)strtoul(e, 0, 0);
    int rsx_ok;
    if (rsx_live_draw_enabled()) {
        rsx_ok = (rsx_null_backend_init(rsx_w, rsx_h, _title) == 0);
        if (rsx_ok && rsx_live_draw_init(rsx_null_backend_get_hwnd(), rsx_w, rsx_h,
                                         rsx_live_guest_ptr, nullptr) == 0) {
            s_rsx_live = 1;
            rsx_null_backend_suppress_present(1);
            fprintf(stderr, "[rsx] live-draw engine up (D3D12); GDI present suppressed\n");
        } else {
            fprintf(stderr, "[rsx] live-draw init FAILED -- falling back to the D3D12 backend\n");
            rsx_ok = (rsx_backend_init(rsx_w, rsx_h, _title) == 0);
        }
    } else {
        rsx_ok = (rsx_backend_init(rsx_w, rsx_h, _title) == 0);
    }
    fprintf(stderr, "[rsx] backend init %s\n", rsx_ok ? "OK -- window open" : "FAILED");
    unsigned last_flip = 0;
    /* The game's frame pacing (vblank/flip handlers -> display frame counter) must
     * advance at ~60Hz regardless of how long present() blocks. On a hidden/occluded
     * window DXGI Present throttles hard, which previously stalled these ticks and
     * paced the game's main loop to ~0.5fps. Drive the ticks off REAL elapsed time
     * and catch up in a bounded burst so present latency never slows the game. */
    /* [NHL local patch] The vblank clock runs on QPC microseconds at the display
     * rate (RSX_VBLANK_HZ, default 59.94). It ran on GetTickCount64 + 16 ms:
     * 62.5 Hz, and quantised to the ~15.6 ms system tick, so vblanks landed
     * 15/16/31 ms apart. NHL paces itself on a vblank countdown (label 0x4B0,
     * two vblanks a frame), so that jitter became 27/35/47 ms frames. */
    static unsigned long long s_vbl_us = 0;
    if (!s_vbl_us) { double hz = 59.94; if (const char* e = getenv("RSX_VBLANK_HZ")) hz = atof(e);
                     if (hz < 1.0) hz = 59.94; s_vbl_us = (unsigned long long)(1000000.0 / hz + 0.5); }
    unsigned long long next_tick = ps3_qpc_us();
    for (;;) {
        /* Was Sleep(4). The FIFO kick event existed, was signalled on a dry
         * fence queue, and had no waiter -- so a guest starved for `get` or a
         * fence still paid the whole tick. Waiting on it keeps the same 4 ms
         * ceiling when nothing kicks, and drains on demand when something
         * does. */
        { const unsigned long long t = ps3_qpc_us();
          unsigned ms = 4;
          if ((long long)(next_tick - t) < 4000) ms = (long long)(next_tick - t) <= 0 ? 0 : (unsigned)((next_tick - t) / 1000);
          if (ms) cellGcm_fifo_kick_wait(ms);
          else if ((long long)(next_tick - t) > 0) SwitchToThread(); }
        unsigned long long now = ps3_qpc_us();
        int fired = 0;
        while ((long long)(now - next_tick) >= 0 && fired < 240) {
#if defined(_WIN32)
            /* [NHL local patch #164] VRR: the frame rendered since the last tick is presented
             * ON the tick, so presents sit on the 59.94 Hz grid and the display follows it */
            if (rsx_ok && !s_rsx_live && rsx_d3d12_backend_vblank() && hz_on()) hz_on_present();
#endif
            cellGcmTickVBlank();
            cellGcmTickFlip();
            /* Present a pending flip BEFORE draining further: the flip fires
             * at a get==put frame boundary (guest thread), so the batch held
             * right now is exactly the completed frame. Presenting on a raw
             * flip-count change after the drain raced the guest's next-frame
             * writes and showed empty or mixed batches. */
            {
                if (rsx_ok && cellGcm_take_flip_pending()) {
                    present_guest_frame();
                    last_flip = cellGcm_flip_request_count();
                }
            }
            /* Drain the game's GCM FIFO every tick -- this writes the RSX sync-fence
             * labels (e.g. @0x03000410) that the game's per-frame logic blocks on.
             * Doing it here (not after present) keeps those fences advancing at 60Hz
             * even when present() throttles on a hidden/occluded window. */
            if (rsx_ok) hz_process_fifo();
            next_tick += s_vbl_us;           /* display rate */
            fired++;
        }
        if (fired >= 240) next_tick = now;   /* fell too far behind -> resync */
        /* Drain at the outer ~4ms cadence too (not just the 16ms tick):
         * titles fence EVERY render pass on an RSX label the drain writes;
         * at 16ms per fence wave's six passes paced the guest to ~7 fps.
         * The real RSX writes those fences in microseconds. */
        if (rsx_ok) {
            if (cellGcm_take_flip_pending()) {
                present_guest_frame();
                last_flip = cellGcm_flip_request_count();
            }
            hz_process_fifo();
        }
        /* YDKJ_INJECT_Q3: the main thread polls q3 (load/state-complete) each frame but
         * it's never posted (producer signals a condvar, never enqueues). Inject a q3
         * event periodically to test whether delivering the completion advances the
         * game's state machine to instantiate + display the (force-parsed) menu movie. */
        static const char* s_injq3 = (const char*)1; static int s_pacetrace = -1;   /* #228: read once, not per ticker loop */
        if (s_injq3 == (const char*)1) { s_injq3 = getenv("YDKJ_INJECT_Q3"); s_pacetrace = getenv("YDKJ_PACETRACE") ? 1 : 0; }
        if (s_injq3) {
            static int s_q3=0; const char* qe=s_injq3; uint32_t qid=(uint32_t)strtoul(qe,0,0); if(qid==0||qid==1) qid=3;
            if(++s_q3 % 8 == 0){ int r=sys_event_queue_inject(qid, 0x1234, 0, 0, 0);
              static int _n=0; if(_n++<12) fprintf(stderr,"[INJQ3] injected q%u event rc=%d\n",qid,r); } }
        if (rsx_ok) {
            if (rsx_pump_messages() != 0) { rsx_ok = 0; nhl_window_closed(); }
            if (s_pacetrace) {
                static ULONGLONG s_win=0; static int s_pf=0, s_pres=0; static ULONGLONG s_presms=0;
                s_pf += fired; s_pres++;
                ULONGLONG t0=GetTickCount64(); rsx_present_frame(); ULONGLONG t1=GetTickCount64();
                s_presms += (t1-t0);
                if (s_win==0) s_win=now;
                if (now - s_win >= 1000000) {   /* now is QPC us */
                    fprintf(stderr,"[PACE] process_fifo=%d/s  present=%d/s  present_total=%llums/s (avg %llums)\n",
                            s_pf, s_pres, s_presms, s_pres? s_presms/s_pres:0);
                    s_pf=0; s_pres=0; s_presms=0; s_win=now;
                }
            } else {
                /* Present only on a guest flip (frame boundary). A fixed-clock
                 * present can catch the drain mid-frame -- notably while the
                 * guest is blocked in the FIFO-wrap recycle callback -- and
                 * flash a partial frame (clear + a few draws). Before the
                 * first flip present freely so the window isn't stuck white
                 * during boot. */
                unsigned fc = cellGcm_flip_request_count();
                if (fc != last_flip || fc == 0) {
                    /* [NHL local patch #141] This present IS that flip's present:
                     * consume the pending flag too. Left set, the next pass's
                     * take_flip_pending() presented the same (empty) batch again
                     * ~4 ms later -- two vsync'd Presents per guest frame on the
                     * thread that drains the FIFO (gaps 29.6/3.8 ms, x140c).
                     * PRESENT_DUP=1 = old behaviour. */
                    static int s_dup = -1; if (s_dup < 0) s_dup = getenv("PRESENT_DUP") ? 1 : 0;
                    if (fc && !s_dup) (void)cellGcm_take_flip_pending();
                    if (fc) present_guest_frame(); else rsx_present_frame();
                    last_flip = fc;
                } else if (fc == 0) {
                    rsx_backend_present();
                }
            }
        }
    }
    return 0;
}

extern "C" uint32_t    g_last_hle_nid;
extern "C" const char* g_last_hle_name;

/* Windows-only from here to the end of hang_watchdog. Snapshotting every
 * OTHER thread and reading its instruction pointer needs tlhelp32 plus
 * SuspendThread/GetThreadContext; POSIX has no equivalent that does not
 * amount to writing a debugger. The vblank ticker above, which is what the
 * frame loop actually depends on, is deliberately outside this. */
#ifdef _WIN32
#include <tlhelp32.h>
/* When the boot wedges, snapshot every other thread's instruction pointer as a
 * module RVA (symbolize with llvm-symbolizer) so a guest spin/wait is pinned to
 * an exact lifted function -- the HLE breadcrumb only covers HLE calls. */
/* Snapshot every other thread's RIP. For threads in the boot module (lifted
 * guest code) print the RVA (symbolizable) + a couple of stack-return RVAs;
 * for threads parked in a DLL (OS waits / FMOD) print the module name so they
 * are not mistaken for guest spins. Called twice so the caller can diff which
 * guest thread is genuinely parked (same RIP) vs. still progressing. */
/* Defined with the debug console below; writes to stderr and, when the
 * console is servicing a command, also to its response file. */
static void dbg_printf(const char* fmt, ...);
/* While dump_threads holds a thread suspended, dbg_printf formats into this
 * buffer instead of a stream: the suspended thread may own stderr's CRT lock,
 * and writing then deadlocks the watchdog -- and with it every thread that
 * logs. GH3 froze solid (0% CPU, log silent mid-boot) exactly that way. */
static __declspec(thread) char   s_dbg_capbuf[16384];
static __declspec(thread) char*  s_dbg_cap = NULL;
static __declspec(thread) size_t s_dbg_capn = 0;

extern "C" const char* g_hle_inflight[];   /* ppu_hle.cpp; 64 entries */
extern "C" void ppu_report_guest_lrs(void); /* ppu_loader.cpp */

static void dump_threads(const char* label, HMODULE self)
{
    dbg_printf( "[WATCHDOG] %s; last HLE call = 0x%08X (%s)\n",
            label, g_last_hle_nid, g_last_hle_name ? g_last_hle_name : "");

    /* Which HLE each guest thread is INSIDE right now.
     *
     * ppu_hle.cpp has maintained this array for a while and nothing ever read
     * it. It is the only way to see a thread parked in an HLE wait:
     * sys_lwmutex_lock and sys_lwcond_wait are sysPrxForUser handlers, not lv2
     * syscalls, so PS3_SCBLOCK_PROF cannot see them block, the usleep
     * histogram cannot see them (they do not usleep), and the sampler just
     * reports ntdll. A thread stuck and invisible to all three is exactly the
     * case this answers. */
    { int any = 0;
      for (unsigned t = 0; t < 64u; t++) {
          const char* n = g_hle_inflight[t];
          if (!n) continue;
          if (!any) { dbg_printf("[WATCHDOG] guest threads inside an HLE:\n"); any = 1; }
          dbg_printf("[WATCHDOG]   guest-tid %-3u in %s\n", t, n);
      }
      if (!any) dbg_printf("[WATCHDOG] no guest thread is inside an HLE\n"); }
    dbg_printf("[WATCHDOG] guest threads, by guest lr:\n");
    ppu_report_guest_lrs();
    DWORD me = GetCurrentThreadId(), pid = GetCurrentProcessId();
    /* [NHL local patch #143] Nothing that takes a lock may run while a thread is suspended.
     * GetModuleHandleExA / GetModuleFileNameA take the loader lock; a thread suspended while
     * it holds it (exiting: DLL_THREAD_DETACH; starting: DLL_THREAD_ATTACH) deadlocked this
     * dump, and then every _beginthreadex (it pins its module through the loader lock) --
     * the early-boot hangs "main inside sys_ppu_thread_create" (play193, play199) and
     * cellPadInit (play183), all at the 8 s automatic sample. Now: suspend, read the context,
     * copy the stack into a buffer allocated beforehand, resume -- and only then resolve. */
    static uint64_t* s_stk = (uint64_t*)VirtualAlloc(NULL, 0x200000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te; te.dwSize = sizeof te;
    if (snap != INVALID_HANDLE_VALUE && Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID != pid || te.th32ThreadID == me) continue;
            HANDLE th = OpenThread(THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME,
                                   FALSE, te.th32ThreadID);
            if (!th) continue;
            s_dbg_capn = 0; s_dbg_capbuf[0] = 0; s_dbg_cap = s_dbg_capbuf;
            CONTEXT ctx; ctx.ContextFlags = CONTEXT_CONTROL;
            int have_ctx = 0, nstk = 0;
            SuspendThread(th);
            if (GetThreadContext(th, &ctx)) {
                have_ctx = 1;
                /* VirtualQuery is a kernel call (no user-mode lock); the copy is a plain read */
                MEMORY_BASIC_INFORMATION mbi;
                if (s_stk && VirtualQuery((LPCVOID)ctx.Rsp, &mbi, sizeof mbi) &&
                    mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
                    (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE))) {
                    uint64_t end = (uint64_t)mbi.BaseAddress + mbi.RegionSize;
                    uint64_t n = (end - (uint64_t)ctx.Rsp) / 8;
                    if (n > 0x200000 / 8) n = 0x200000 / 8;   /* lifted SPU frames are big */
                    memcpy(s_stk, (const void*)ctx.Rsp, (size_t)n * 8);
                    nstk = (int)n;
                }
            }
            ResumeThread(th);
            if (have_ctx) {
                HMODULE m = NULL;
                GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   (LPCSTR)ctx.Rip, &m);
                if (m == self) {
                    dbg_printf( "[WATCHDOG]   tid %5lu BOOT rip rva=0x%llX\n",
                            (unsigned long)te.th32ThreadID,
                            (unsigned long long)((char*)ctx.Rip - (char*)self));
                } else {
                    char path[MAX_PATH] = "?";
                    if (m) GetModuleFileNameA(m, path, sizeof path);
                    const char* base = strrchr(path, '\\');
                    dbg_printf( "[WATCHDOG]   tid %5lu in %s\n",
                            (unsigned long)te.th32ThreadID, base ? base + 1 : path);
                }
                /* Scan the suspended thread's stack for boot-module return
                 * addresses to reconstruct the lifted call chain â€” done for ALL
                 * threads (even when RIP is parked in ntdll inside a CriticalSection
                 * call), since that's exactly where the busy-spin's lwmutex churn
                 * lands the main thread. Map RVAs -> func_ names via flow.map.
                 * (some false positives expected â€” these are stack-scan hits.) */
                {
                    /* #143: scan the copy taken while the thread was suspended (bounded
                     * to its committed stack region, so the guard page was never read) */
                    const uint64_t* sp = s_stk;
                    const int maxk = nstk;
                    int found = 0;
                    for (int k = 0; k < maxk && found < 40; k++) {
                        uint64_t v = sp[k];
                        if (v < (uint64_t)self) continue;
                        HMODULE mm = NULL;
                        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                           (LPCSTR)v, &mm);
                        if (mm == self) {
                            /* Resolve to the guest function containing this return
                             * address: the function_table entry with the largest host
                             * pointer <= it. A raw RVA needs symbols a release build
                             * does not have, and "ret rva=0x519F66" says nothing about
                             * where a hung title actually is. Same mapping the
                             * unresolved-call dump uses. */
                            uint32_t bestGuest = 0;
                            uintptr_t bestHost = 0, nextHost = 0, maxHost = 0;
                            for (uint64_t q = 0; q < function_table_count; q++) {
                                uintptr_t h = (uintptr_t)function_table[q].func;
                                if (h > maxHost) maxHost = h;
                                if (h <= (uintptr_t)v && h > bestHost) {
                                    bestHost = h; bestGuest = function_table[q].addr;
                                }
                            }
                            for (uint64_t q = 0; q < function_table_count; q++) {
                                uintptr_t h = (uintptr_t)function_table[q].func;
                                if (h > bestHost && (nextHost == 0 || h < nextHost)) nextHost = h;
                            }
                            /* The highest-addressed lifted function has no next, so its
                             * extent is unknown -- and EVERY runtime/CRT address above it
                             * was attributed to it. Refuse to guess there. A confident
                             * wrong frame is worse than no frame. */
                            if (bestHost == maxHost ||
                                (uintptr_t)v - bestHost > 0x4000u) bestGuest = 0;
                            dbg_printf( bestGuest
                                    ? "[WATCHDOG]       tid %5lu ret rva=0x%llX  func_%08X+0x%llX\n"
                                    : "[WATCHDOG]       tid %5lu ret rva=0x%llX\n",
                                    (unsigned long)te.th32ThreadID,
                                    (unsigned long long)(v - (uint64_t)self),
                                    bestGuest, (unsigned long long)((uintptr_t)v - bestHost));
                            found++;
                        }
                    }
                }
            }
            s_dbg_cap = NULL;
            if (s_dbg_capn) dbg_printf("%s", s_dbg_capbuf);
            CloseHandle(th);
        } while (Thread32Next(snap, &te));
    }
    if (snap != INVALID_HANDLE_VALUE) CloseHandle(snap);
    fflush(stderr);
}

/* â”€â”€ Debug console â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
 * PS3_DEBUG=<path> opens a file-based command channel, so a title that is
 * already running can be asked what it is doing without a rebuild.
 *
 * Write one command into <path>; the console executes it, appends the answer
 * to <path>.out, and truncates <path> ready for the next one.
 *
 *     echo threads    > dbg.txt     # symbolised stack of every guest thread
 *     echo "mem 10200 64" > dbg.txt # hexdump of guest memory
 *     echo stat       > dbg.txt     # flips, HLE breadcrumb, uptime
 *
 * A file and not a socket on purpose: no listening port inside a game
 * process, nothing for a firewall to prompt about, and it drives from a shell
 * script exactly the way PAD_FILE already does.
 *
 * Read-only by design apart from poke32. Changing a diagnostic knob while the
 * title runs is NOT possible here: the large majority are read once at first
 * use and cached in a function-local static (see docs/DIAGNOSTICS.md), so they
 * are launch-time settings. `knobs` reports which ones this run was started
 * with, which is the useful half of that.
 */
static FILE* s_dbg_out = NULL;

static void dbg_printf(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    if (s_dbg_cap) {             /* a thread is suspended: format only, no stream lock */
        int n = vsnprintf(s_dbg_cap + s_dbg_capn, sizeof s_dbg_capbuf - s_dbg_capn, fmt, ap);
        if (n > 0) s_dbg_capn += (size_t)n;
        if (s_dbg_capn >= sizeof s_dbg_capbuf) s_dbg_capn = sizeof s_dbg_capbuf - 1;
        va_end(ap);
        return;
    }
    if (s_dbg_out) { va_list c; va_copy(c, ap); vfprintf(s_dbg_out, fmt, c); va_end(c); }
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

static void dbg_mem(uint32_t ea, uint32_t len)
{
    if (!vm_base) { dbg_printf("  vm_base not live yet%c", 10); return; }
    if (len == 0 || len > 4096) len = 64;
    for (uint32_t off = 0; off < len; off += 16) {
        char asc[17];
        dbg_printf("  0x%08X ", ea + off);
        for (uint32_t i = 0; i < 16; i++) {
            if (off + i < len) {
                uint8_t b = vm_base[(size_t)(ea + off + i)];
                dbg_printf("%02X ", b);
                asc[i] = (b >= 32 && b < 127) ? (char)b : '.';
            } else { dbg_printf("   "); asc[i] = ' '; }
        }
        asc[16] = 0;
        dbg_printf(" |%s|%c", asc, 10);
    }
}

static void dbg_knobs(const char* prefix)
{
    /* Which diagnostics this run was actually started with. */
    LPCH env = GetEnvironmentStringsA();
    if (!env) return;
    int n = 0;
    for (LPCH p = env; *p; p += strlen(p) + 1) {
        const char* eq = strchr(p, '=');
        if (!eq || eq == p) continue;
        /* Only the runtime's own namespaces, not the whole shell environment. */
        static const char* known[] = {
            "PS3_", "SPU_", "SPURS_", "GCM_", "RSX_", "LD_", "YZ_", "YDKJ_",
            "LBP_", "FLOW_", "RD_", "TEX_", "FP_", "VP_", "PPU_", "PS1_",
            "CELLMARK_", "RTT_", "PAD_", "FS_", "WATCHDOG_", "SYNC_", NULL };
        int match = 0;
        for (int k = 0; known[k]; k++)
            if (strncmp(p, known[k], strlen(known[k])) == 0) { match = 1; break; }
        if (!match) continue;
        if (prefix && *prefix && strncmp(p, prefix, strlen(prefix)) != 0) continue;
        dbg_printf("  %s%c", p, 10);
        n++;
    }
    FreeEnvironmentStringsA(env);
    dbg_printf("  (%d set; docs/DIAGNOSTICS.md lists all of them)%c", n, 10);
}

/* "prof <host-tid> [sec]": sample ONE thread at ~1 kHz and unwind it through
 * .pdata (exact, unlike dump_threads' stack scan), then print the hottest
 * functions exclusive (leaf) and inclusive (anywhere on the stack), keyed by
 * function start. exe entries print as rva=; resolve against the link map.
 * Only lock-free work happens while the thread is suspended.
 * ponytail: fixed 4096-slot tables; a thread touching more functions drops the rest. */
#define PROF_SLOTS 4096
static uint64_t s_prof_key[3][PROF_SLOTS];
static uint32_t s_prof_ct[3][PROF_SLOTS];
static void prof_hit(int tab, uint64_t key)
{
    for (uint32_t i = (uint32_t)((key >> 4) * 2654435761u) % PROF_SLOTS, n = 0; n < PROF_SLOTS;
         n++, i = (i + 1) % PROF_SLOTS) {
        if (s_prof_key[tab][i] == key) { s_prof_ct[tab][i]++; return; }
        if (!s_prof_key[tab][i]) { s_prof_key[tab][i] = key; s_prof_ct[tab][i] = 1; return; }
    }
}
static void hz_mods_init(void);                                  /* #152, below */
static PRUNTIME_FUNCTION hz_lookup(DWORD64 pc, DWORD64* base);
static int s_hz_mods_ready;
/* [NHL #312] One sample's unwind, under SEH like hz_unwind: lifted code's tail-call chains
 * can hand RtlVirtualUnwind a bad frame, and an unguarded fault here killed the game
 * (cmdprof1, 2026-11-09). A faulting unwind keeps the frames counted so far. */
static void prof_sample(CONTEXT* c, uintptr_t exe)
{
    uint64_t seen[24]; int ns = 0, got = 0;
    __try {
        for (int d = 0; d < 24 && c->Rip; d++) {
            DWORD64 base = 0;
            PRUNTIME_FUNCTION rf = hz_lookup(c->Rip, &base);   /* #152: lock-free */
            uint64_t key = rf ? base + rf->BeginAddress : c->Rip;
            if (d == 0) prof_hit(0, key);
            /* first exe frame of a sample whose leaf is in a DLL: who called out */
            if (!got && (uintptr_t)(key - exe) < 0x40000000u) { got = 1; if (d > 0) prof_hit(2, key); }
            int dup = 0; for (int k = 0; k < ns; k++) dup |= seen[k] == key;
            if (!dup) { seen[ns++] = key; prof_hit(1, key); }
            if (!rf) break;                       /* leaf without unwind data: stop */
            void* hd; DWORD64 ef;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c->Rip, rf, c, &hd, &ef, NULL);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        static volatile LONG n = 0; InterlockedIncrement(&n);   /* counted, not logged: the target is still suspended */
    }
}
static void dbg_prof(DWORD tid, unsigned sec)
{
    /* Suspending ourselves never resumes: the console would hang for good. */
    if (tid == GetCurrentThreadId()) { dbg_printf("  tid %lu is the console%c", (unsigned long)tid, 10); return; }
    if (!s_hz_mods_ready) { hz_mods_init(); s_hz_mods_ready = 1; }   /* #152: not while suspended */
    HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
    if (!h) { dbg_printf("  cannot open tid %lu%c", (unsigned long)tid, 10); return; }
    memset(s_prof_key, 0, sizeof s_prof_key); memset(s_prof_ct, 0, sizeof s_prof_ct);
    const uintptr_t exe = (uintptr_t)GetModuleHandleA(NULL);
    unsigned samples = 0;
    const ULONGLONG end = GetTickCount64() + sec * 1000ull;
    while (GetTickCount64() < end) {
        if (SuspendThread(h) == (DWORD)-1) break;
        CONTEXT c; c.ContextFlags = CONTEXT_FULL;
        if (GetThreadContext(h, &c)) {
            samples++;
            prof_sample(&c, exe);
        }
        ResumeThread(h);
        Sleep(1);
    }
    CloseHandle(h);
    dbg_printf("  prof tid %lu: %u samples over %us%c", (unsigned long)tid, samples, sec, 10);
    for (int tab = 0; tab < 3; tab++) {
        static const char* tn[3] = { "exclusive", "inclusive", "exe caller of DLL leaf" };
        dbg_printf("  --- %s ---%c", tn[tab], 10);
        for (int rank = 0; rank < 40; rank++) {
            uint32_t best = 0; int bi = -1;
            for (int k = 0; k < PROF_SLOTS; k++) if (s_prof_ct[tab][k] > best) { best = s_prof_ct[tab][k]; bi = k; }
            if (bi < 0) break;
            uint64_t key = s_prof_key[tab][bi];
            HMODULE m = NULL; char path[MAX_PATH] = "?";
            GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)key, &m);
            if (m) GetModuleFileNameA(m, path, sizeof path);
            const char* b = strrchr(path, 92); b = b ? b + 1 : path;
            /* NHL: name lifted guest functions -- the key is a host function
             * start, which is exactly a function_table entry's func. */
            uint32_t guest = 0;
            if ((uintptr_t)m == exe)
                for (uint64_t q = 0; q < function_table_count; q++)
                    if ((uint64_t)(uintptr_t)function_table[q].func == key) { guest = (uint32_t)function_table[q].addr; break; }
            if (guest) dbg_printf("  %5.1f%%  func_%08X (rva=0x%llX)%c", 100.0 * best / (samples ? samples : 1),
                                  guest, (unsigned long long)(key - exe), 10);
            else if ((uintptr_t)m == exe) dbg_printf("  %5.1f%%  rva=0x%llX%c", 100.0 * best / (samples ? samples : 1),
                                                (unsigned long long)(key - exe), 10);
            else dbg_printf("  %5.1f%%  %s+0x%llX%c", 100.0 * best / (samples ? samples : 1), b,
                            (unsigned long long)(key - (uintptr_t)m), 10);
            s_prof_ct[tab][bi] = 0;
        }
    }
}

/* ---- [NHL local patch #122] HITCH=1: hitch monitor ------------------------
 * Finds what the host was doing when the game slowed down.
 *  - [HZ] one line per second: presents, longest frame, and host ms spent in
 *    FXC (VS/PS), PSO creation, texture binds/uploads, render_frame, FIFO
 *    processing, guest file reads.
 *  - [HITCH] for every frame (present-to-present) longer than HITCH_MS
 *    (default 250; adjacent long frames merge into one report): the same
 *    counters for that window plus a sampled profile of the main guest
 *    thread, the RSX ticker thread and HITCH_TIDS guest tids (default 7 =
 *    render), unwound through .pdata like `prof`.
 *  - [MARK] F8 (global hotkey): the user felt a slowdown -- profile of the
 *    3 s before the key press.
 * rva= entries resolve against nhl\build\NHLLegacy.map. */
extern "C" volatile long long g_hitch_vs_us, g_hitch_vs_n, g_hitch_ps_us, g_hitch_ps_n,
                              g_hitch_pso_us, g_hitch_pso_n, g_hitch_tex_us, g_hitch_tex_n,
                              g_hitch_rf_us, g_hitch_fs_us, g_hitch_fs_bytes, g_hitch_fs_n,
                              g_hitch_texup_new, g_hitch_texup_chg, g_hitch_texup_evict,
                              g_hitch_texup_create,   /* #136 */
                              g_hitch_texup_alloc_us, g_hitch_texup_decode_us,   /* #219 */
                              g_game_ticks,           /* #140 (ppu_hle.cpp, PS3_TICK_LR) */
                              g_game_steps;           /* #269: sim steps, 30/s = real time; one tick runs N of them */
extern "C" volatile long g_hitch_mark_req, g_hitch_mark_tick;
enum { HZ_VS_US, HZ_VS_N, HZ_PS_US, HZ_PS_N, HZ_PSO_US, HZ_PSO_N, HZ_TEX_US, HZ_TEX_N, HZ_RF_US,
       HZ_FS_US, HZ_FS_B, HZ_FS_N, HZ_FIFO_US, HZ_PRES_US,
       HZ_UP_NEW, HZ_UP_CHG, HZ_UP_EVICT, HZ_UP_CREATE, HZ_UP_ALLOC_US, HZ_UP_DECODE_US, HZ_GAME, HZ_STEPS, HZ_NV };
struct HzSnap { long long v[HZ_NV]; };
static void hz_read(HzSnap* s)
{
    s->v[HZ_VS_US] = g_hitch_vs_us;   s->v[HZ_VS_N] = g_hitch_vs_n;
    s->v[HZ_PS_US] = g_hitch_ps_us;   s->v[HZ_PS_N] = g_hitch_ps_n;
    s->v[HZ_PSO_US] = g_hitch_pso_us; s->v[HZ_PSO_N] = g_hitch_pso_n;
    s->v[HZ_TEX_US] = g_hitch_tex_us; s->v[HZ_TEX_N] = g_hitch_tex_n;
    s->v[HZ_RF_US] = g_hitch_rf_us;
    s->v[HZ_FS_US] = g_hitch_fs_us;   s->v[HZ_FS_B] = g_hitch_fs_bytes; s->v[HZ_FS_N] = g_hitch_fs_n;
    s->v[HZ_FIFO_US] = s_hz_fifo_us;  s->v[HZ_PRES_US] = s_hz_present_us;
    s->v[HZ_UP_NEW] = g_hitch_texup_new;     s->v[HZ_UP_CHG] = g_hitch_texup_chg;
    s->v[HZ_UP_EVICT] = g_hitch_texup_evict; s->v[HZ_UP_CREATE] = g_hitch_texup_create;
    s->v[HZ_UP_ALLOC_US] = g_hitch_texup_alloc_us; s->v[HZ_UP_DECODE_US] = g_hitch_texup_decode_us;
    s->v[HZ_GAME] = g_game_ticks;     s->v[HZ_STEPS] = g_game_steps;
}
static void hz_sub(HzSnap* d, const HzSnap* a, const HzSnap* b)
{ for (int i = 0; i < HZ_NV; i++) d->v[i] = a->v[i] - b->v[i]; }
static void hz_fmt(char* o, size_t n, const HzSnap* d)
{
    snprintf(o, n, "fifo %lldms | render_frame %lldms | present %lldms | VS fxc %lld/%lldms | PS fxc %lld/%lldms"
                   " | PSO %lld/%lldms | tex %lld/%lldms | fs %lld reads %.1fMB %lldms"
                   " | texup new %lld chg %lld evict %lld create %lld alloc %lldms decode %lldms | game %lld | steps %lld",
             d->v[HZ_FIFO_US] / 1000, d->v[HZ_RF_US] / 1000, d->v[HZ_PRES_US] / 1000,
             d->v[HZ_VS_N], d->v[HZ_VS_US] / 1000, d->v[HZ_PS_N], d->v[HZ_PS_US] / 1000,
             d->v[HZ_PSO_N], d->v[HZ_PSO_US] / 1000, d->v[HZ_TEX_N], d->v[HZ_TEX_US] / 1000,
             d->v[HZ_FS_N], d->v[HZ_FS_B] / 1048576.0, d->v[HZ_FS_US] / 1000,
             d->v[HZ_UP_NEW], d->v[HZ_UP_CHG], d->v[HZ_UP_EVICT], d->v[HZ_UP_CREATE],
             d->v[HZ_UP_ALLOC_US] / 1000, d->v[HZ_UP_DECODE_US] / 1000, d->v[HZ_GAME], d->v[HZ_STEPS]);
}
static int hz_on(void) { static int v = -1; if (v < 0) v = getenv("HITCH") ? 1 : 0; return v; }
/* long-frame events: ticker (present_guest_frame) produces, the monitor consumes */
struct HzEvent { unsigned long long t0, t1; HzSnap d; };
static HzEvent s_hz_ev[64];
static volatile LONG s_hz_ev_w = 0, s_hz_ev_r = 0;
static volatile long long s_hz_maxgap_us = 0;   /* this second's longest frame */
/* [#140b] frame pacing, per second: present gaps by size (~1, ~2, 3+ vblanks) and how
 * many presents each game update stayed on screen (a game at 30 on a 60 Hz present
 * should be 2,2,2...; a 1/3 mix is judder). Printed as [PACE] next to [HZ]. */
static volatile long s_pace_gap[4];    /* <20 ms, 20-40, 40-60, >=60 */
static volatile long s_pace_run[5];    /* game update shown on 1,2,3,4,5+ presents */
static volatile long s_pace_skip;      /* game updates never shown (2+ between presents) */
static long long s_pace_raw[32]; static volatile long s_pace_raw_n;   /* last 32 gaps, us */
static void hz_on_present(void)   /* ticker thread, after each guest-frame present */
{
    static unsigned long long last = 0; static HzSnap at_last;
    static long long thr = -1;
    if (thr < 0) { const char* e = getenv("HITCH_MS"); thr = (e ? atoll(e) : 250) * 1000; }
    const unsigned long long now = ps3_qpc_us();
    HzSnap cur; hz_read(&cur);
    { static long long tick_prev = -1; static int run = 0;
      const long long tk = cur.v[HZ_GAME];
      if (tk != tick_prev) {
          if (tick_prev >= 0 && run > 0) s_pace_run[run >= 5 ? 4 : run - 1]++;
          if (tick_prev >= 0 && tk - tick_prev > 1) s_pace_skip += (long)(tk - tick_prev - 1);
          tick_prev = tk; run = 1; }
      else run++; }
    if (last) {
        const long long gap = (long long)(now - last);
        if (gap > s_hz_maxgap_us) s_hz_maxgap_us = gap;
        s_pace_gap[gap < 20000 ? 0 : gap < 40000 ? 1 : gap < 60000 ? 2 : 3]++;
        s_pace_raw[s_pace_raw_n++ & 31] = gap;
        if (gap > thr && s_hz_ev_w - s_hz_ev_r < 64) {
            HzEvent* e = &s_hz_ev[s_hz_ev_w & 63];
            e->t0 = last; e->t1 = now; hz_sub(&e->d, &cur, &at_last);
            InterlockedIncrement(&s_hz_ev_w);
        }
    }
    last = now; at_last = cur;
}
/* sample ring: leaf + first 7 distinct exe-module frames */
struct HzSample { unsigned long long t; uint32_t slot; uint64_t key[12]; };
enum { HZ_RING = 65536, HZ_SLOTS = 16 };
extern "C" volatile long g_spu_host_n;           /* lv2_register.c, SPU spawn order */
extern "C" unsigned long g_spu_host_tid[16];
static HzSample* s_hz_ring = NULL;
static volatile LONG s_hz_ring_n = 0;   /* total written */
static const char* s_hz_slot_name[HZ_SLOTS];
static int s_hz_nslots = 0;
static uint64_t s_hz_cnt_key[2][2048]; static uint32_t s_hz_cnt_ct[2][2048];
static void hz_hit(int tab, uint64_t key)
{
    for (uint32_t i = (uint32_t)((key >> 4) * 2654435761u) % 2048, n = 0; n < 2048; n++, i = (i + 1) % 2048) {
        if (s_hz_cnt_key[tab][i] == key) { s_hz_cnt_ct[tab][i]++; return; }
        if (!s_hz_cnt_key[tab][i]) { s_hz_cnt_key[tab][i] = key; s_hz_cnt_ct[tab][i] = 1; return; }
    }
}
static void hz_name(uint64_t key, char* o, size_t n)
{
    const uintptr_t exe = (uintptr_t)GetModuleHandleA(NULL);
    HMODULE m = NULL; char path[MAX_PATH] = "?";
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)key, &m);
    if ((uintptr_t)m == exe) {
        for (uint64_t q = 0; q < function_table_count; q++)
            if ((uint64_t)(uintptr_t)function_table[q].func == key) {
                snprintf(o, n, "func_%08X", (uint32_t)function_table[q].addr); return; }
        snprintf(o, n, "rva=0x%llX", (unsigned long long)(key - exe)); return;
    }
    if (m) GetModuleFileNameA(m, path, sizeof path);
    const char* b = strrchr(path, 92); b = b ? b + 1 : path;
    snprintf(o, n, "%s+0x%llX", b, (unsigned long long)(key - (uintptr_t)m));
}
/* Profile of every sampled thread over [t0, t1] (QPC us). */
static void hz_dump_window(unsigned long long t0, unsigned long long t1)
{
    if (!s_hz_ring) return;
    const LONG w = s_hz_ring_n;
    const LONG lo = w > HZ_RING ? w - HZ_RING : 0;
    for (int sl = 0; sl < s_hz_nslots; sl++) {
        memset(s_hz_cnt_key, 0, sizeof s_hz_cnt_key); memset(s_hz_cnt_ct, 0, sizeof s_hz_cnt_ct);
        unsigned ns = 0;
        for (LONG i = lo; i < w; i++) {
            const HzSample* s = &s_hz_ring[i % HZ_RING];
            if ((int)s->slot != sl || s->t < t0 || s->t > t1) continue;
            ns++;
            hz_hit(0, s->key[0]);
            for (int k = 1; k < 12 && s->key[k]; k++) hz_hit(1, s->key[k]);
        }
        if (!ns) continue;
        fprintf(stderr, "[HITCH]   thread %s: %u samples%c", s_hz_slot_name[sl], ns, 10);
        /* #263 HITCH_TOP=<n>: list n leaves / n stack entries (default 8 / 16; max 120) */
        static int top[2] = { 0, 0 };
        if (!top[0]) { const char* e = getenv("HITCH_TOP"); const int n = e ? atoi(e) : 0;
                       top[0] = n > 0 ? (n > 120 ? 120 : n) : 8; top[1] = n > 0 ? top[0] : 16; }
        static const char* tn[2] = { "leaf", "on stack (exe)" };
        for (int tab = 0; tab < 2; tab++) {
            char line[12000]; int ln = snprintf(line, sizeof line, "[HITCH]     %s:", tn[tab]);
            for (int r = 0; r < top[tab]; r++) {
                uint32_t best = 0; int bi = -1;
                for (int k = 0; k < 2048; k++) if (s_hz_cnt_ct[tab][k] > best) { best = s_hz_cnt_ct[tab][k]; bi = k; }
                if (bi < 0) break;
                char nm[160]; hz_name(s_hz_cnt_key[tab][bi], nm, sizeof nm);
                if (ln < (int)sizeof line - 200)
                    ln += snprintf(line + ln, sizeof line - ln, top[0] > 8 ? " %s %.1f%%;" : " %s %.0f%%;", nm, 100.0 * best / ns);
                s_hz_cnt_ct[tab][bi] = 0;
            }
            fprintf(stderr, "%s%c", line, 10);
        }
    }
}
/* Unwind a suspended thread's context into key[0] = leaf, key[1..11] = distinct
 * exe frames. A thread stopped at an arbitrary instruction (a prologue, code
 * without unwind data, a driver's JIT) can hand RtlVirtualUnwind a garbage
 * stack pointer; it then faults inside ntdll -- play174 died exactly so
 * ("read fault 0x109" in ntdll, 14 threads sampled at 1 kHz). Catch it and keep
 * the frames found so far; the sample is merely shorter. */
/* [NHL local patch #152] Lock-free function-table lookup for the sampler. hz_unwind
 * runs while its target thread is SUSPENDED, and RtlLookupFunctionEntry can take an
 * ntdll lock: a thread suspended while holding it (main inside thread creation)
 * deadlocked the sampler, and main with it -- the flaky early-boot hang (play183/193/
 * 199, x151p: main parked in sys_ppu_thread_create of "Shutdown", no [HZ] line ever).
 * The exception directories of the exe and the usual system DLLs are read once here
 * and binary-searched; RtlVirtualUnwind itself only reads memory. An address in any
 * other module ends the walk. */
struct HzMod { uintptr_t base, end; const RUNTIME_FUNCTION* rf; DWORD n; };
static HzMod s_hz_mods[8]; static int s_hz_nmods;
static void hz_mods_init(void)
{
    if (s_hz_nmods) return;
    static const char* const names[] = { 0, "ntdll.dll", "kernelbase.dll", "kernel32.dll",
                                         "ucrtbase.dll", "vcruntime140.dll", "win32u.dll", "user32.dll" };
    for (int i = 0; i < 8 && s_hz_nmods < 8; i++) {
        const uintptr_t b = (uintptr_t)GetModuleHandleA(names[i]);
        if (!b) continue;
        const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)(b + ((const IMAGE_DOS_HEADER*)b)->e_lfanew);
        const IMAGE_DATA_DIRECTORY& d = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
        if (!d.VirtualAddress || !d.Size) continue;
        HzMod& m = s_hz_mods[s_hz_nmods++];
        m.base = b; m.end = b + nt->OptionalHeader.SizeOfImage;
        m.rf = (const RUNTIME_FUNCTION*)(b + d.VirtualAddress); m.n = d.Size / sizeof(RUNTIME_FUNCTION);
    }
}
static PRUNTIME_FUNCTION hz_lookup(DWORD64 pc, DWORD64* base)
{
    for (int i = 0; i < s_hz_nmods; i++) {
        const HzMod& m = s_hz_mods[i];
        if (pc < m.base || pc >= m.end) continue;
        const DWORD rva = (DWORD)(pc - m.base);
        DWORD lo = 0, hi = m.n;
        while (lo < hi) {                       /* last entry with BeginAddress <= rva */
            const DWORD mid = (lo + hi) / 2;
            if (m.rf[mid].BeginAddress <= rva) lo = mid + 1; else hi = mid;
        }
        if (!lo || rva >= m.rf[lo - 1].EndAddress) return NULL;
        const RUNTIME_FUNCTION* rf = &m.rf[lo - 1];
        if (rf->UnwindData & 1) rf = (const RUNTIME_FUNCTION*)(m.base + (rf->UnwindData & ~1u));   /* indirect entry */
        *base = m.base;
        return (PRUNTIME_FUNCTION)rf;
    }
    return NULL;
}
static void hz_unwind(CONTEXT* c, uint64_t* key, uintptr_t exe)
{
    int nk = 1;
    __try {
        for (int d = 0; d < 40 && c->Rip && nk < 12; d++) {
            DWORD64 base = 0;
            PRUNTIME_FUNCTION rf = hz_lookup(c->Rip, &base);
            const uint64_t k = rf ? base + rf->BeginAddress : c->Rip;
            if (d == 0) key[0] = k;
            if ((uintptr_t)(k - exe) < 0x40000000u) {
                int dup = 0; for (int q = 1; q < nk; q++) dup |= key[q] == k;
                if (!dup) key[nk++] = k; }
            if (!rf) break;
            void* hd; DWORD64 ef;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c->Rip, rf, c, &hd, &ef, NULL);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        static volatile LONG n = 0; InterlockedIncrement(&n);   /* counted, not logged: the target is still suspended */
    }
}
/* [NHL #270] NHL_PLACE_FILE=<path>: the file holds one line "role=<hex mask> ... ;" (roles
 * below; token usleep=1 = g_usleep_block), re-read twice a second and applied with
 * SetThreadAffinityMask, so one match can compare thread placements. [CPU] every 5 s = % of
 * one logical CPU per role, from thread cycle counters, so it needs no sampling. */
extern "C" volatile int g_usleep_block;
extern "C" volatile unsigned g_spu_poll_n, g_spu_poll_us;
extern "C" volatile long long g_lockline_acquires, g_lockline_contended, g_lockline_wait_tsc;   /* spu_coherency.c */
extern "C" int g_lock_hist;
extern "C" int g_dma_stat;
extern "C" long long g_chan_n[5], g_chan_tsc[5], g_chan_bytes[5];
extern "C" volatile int g_coh_fast;
extern "C" volatile int g_cmdscan_live, g_dma_live;
extern "C" void spu_lock_sites_report(uintptr_t exe_base, int secs);
extern "C" void spu_atom_report(int secs);
extern "C" void spu_coh_lines_report(int secs);
static DWORD WINAPI place_thread(LPVOID)
{
    enum { ROLES = 11 };
    static const char* const name[ROLES] = { "main", "rsx", "t7", "t4", "t5", "s0", "s1", "s2", "s3", "s4", "s5" };
    static const int gtid[ROLES] = { 0, 0, 7, 4, 5 };
    HANDLE h[ROLES] = { 0 };
    ULONG64 prev[ROLES] = { 0 };
    char line[512], seen[512] = "";
    unsigned long long tsc_prev = __rdtsc();
    for (int tick = 1;; tick++) {
        Sleep(500);
        bool fresh = false;
        for (int r = 0; r < ROLES; r++) {
            if (h[r]) continue;
            const DWORD tid = r == 0 ? s_hz_main_tid : r == 1 ? s_hz_rsx_tid
                            : r < 5 ? (DWORD)g_ppu_threads[gtid[r] - 1].host_tid
                            : r - 5 < g_spu_host_n ? (DWORD)g_spu_host_tid[r - 5] : 0;
            if (tid && (h[r] = OpenThread(THREAD_SET_INFORMATION | THREAD_QUERY_INFORMATION, FALSE, tid))) fresh = true;
        }
        FILE* f = fopen(getenv("NHL_PLACE_FILE"), "r");
        const bool got = f && fgets(line, sizeof line, f) && strchr(line, ';');
        if (f) fclose(f);
        if (got && (fresh || strcmp(line, seen))) {
            for (int r = 0; r < ROLES; r++) {
                char key[8]; snprintf(key, sizeof key, "%s=", name[r]);
                const char* p = strstr(line, key);
                if (h[r] && p) SetThreadAffinityMask(h[r], (DWORD_PTR)strtoull(p + strlen(key), NULL, 16));
            }
            if (strstr(line, "usleep=")) g_usleep_block = strstr(line, "usleep=1") != NULL;   /* sys_timer.c */
            g_coh_fast = strstr(line, "cohfast=1") != NULL;   /* spu_coherency.c, #274 */
            { const char* p = strstr(line, "cmdscan="); g_cmdscan_live = p ? atoi(p + 8) : -1;     /* nhl_cmdscan.c, #307 */
              p = strstr(line, "dmaloop="); g_dma_live = p ? atoi(p + 8) : -1; }                   /* nhl_cmdscan.c, #309 */
            { const char* p = strstr(line, "polln=");  g_spu_poll_n  = p ? (unsigned)atoi(p + 6) : 64;      /* spu_channels.c */
              p = strstr(line, "pollus=");             g_spu_poll_us = p ? (unsigned)atoi(p + 7) : 100; }
            if (strcmp(line, seen)) fprintf(stderr, "[PLACE] t=%.0fs %s", (ps3_qpc_us() - s_hz_t0) / 1e6, line);
            strcpy(seen, line);
        }
        if (tick % 10 == 0) {
            const unsigned long long tsc = __rdtsc();
            char o[512]; int p = snprintf(o, sizeof o, "[CPU] t=%.0fs", (ps3_qpc_us() - s_hz_t0) / 1e6);
            for (int r = 0; r < ROLES; r++) {
                ULONG64 c = 0;
                if (h[r]) QueryThreadCycleTime(h[r], &c);
                p += snprintf(o + p, sizeof o - p, " %s %.0f", name[r], 100.0 * (double)(c - prev[r]) / (double)(tsc - tsc_prev));
                prev[r] = c;
            }
            { static long long a0, c0, w0;
              const long long a = g_lockline_acquires, c = g_lockline_contended, w = g_lockline_wait_tsc;
              p += snprintf(o + p, sizeof o - p, " | lockline %lld/s contended %.2f%% wait %.1f%% of 1 CPU",
                            (a - a0) / 5, a > a0 ? 100.0 * (double)(c - c0) / (double)(a - a0) : 0.0,
                            100.0 * (double)(w - w0) / (double)(tsc - tsc_prev));
              a0 = a; c0 = c; w0 = w; }
            if (g_dma_stat > 0) {   /* #279: per second, and % of ONE CPU */
                static long long n0[5], t0[5], b0[5];
                static const char* const k[5] = { "GET", "PUT", "atomic", "chan", "poll" };
                for (int i = 0; i < 5; i++) {
                    const long long n = g_chan_n[i], t = g_chan_tsc[i], b = g_chan_bytes[i];
                    p += snprintf(o + p, sizeof o - p, " | %s %lld/s %lldMB/s %.1f%%", k[i], (n - n0[i]) / 5, (b - b0[i]) / 5 / 1048576,
                                  100.0 * (double)(t - t0[i]) / (double)(tsc - tsc_prev));
                    n0[i] = n; t0[i] = t; b0[i] = b;
                }
            }
            tsc_prev = tsc;
            fprintf(stderr, "%s\n", o);
            if (g_lock_hist > 0) { spu_lock_sites_report((uintptr_t)GetModuleHandleA(NULL), 5); spu_atom_report(5); spu_coh_lines_report(5); }   /* #273 */
        }
    }
}
/* [NHL #278] NHL_TL_FILE=<path>: a row every ms of what each thread is doing, for critical-path
 * analysis: time us, sim steps, then syscall + lr of main, then syscall of tids 4, 5, 7, then
 * program, job, job LSA, pc of each SPU. Reads shared state, suspends nothing. */
extern "C" void spu_coh_timeline(uint32_t* out, int slots);
extern "C" int g_tl_on;
extern "C" uint32_t g_sc_inflight[64], g_sc_inflight_lr[64];
static DWORD WINAPI timeline_thread(LPVOID)
{
    FILE* f = fopen(getenv("NHL_TL_FILE"), "w");
    if (!f) return 0;
    g_tl_on = 1;
    static char buf[1 << 20];
    setvbuf(f, buf, _IOFBF, sizeof buf);
    HANDLE tm = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!tm) { fclose(f); return 0; }
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    LARGE_INTEGER due; due.QuadPart = -10000;
    for (unsigned n = 0;; n++) {
        SetWaitableTimer(tm, &due, 0, NULL, NULL, FALSE);
        WaitForSingleObject(tm, INFINITE);
        uint32_t s[24];
        spu_coh_timeline(s, 6);
        fprintf(f, "%llu %lld %u %08X %u %u %u", ps3_qpc_us() - s_hz_t0, (long long)g_game_steps,
                g_sc_inflight[1], g_sc_inflight_lr[1], g_sc_inflight[4], g_sc_inflight[5], g_sc_inflight[7]);
        for (int i = 0; i < 6; i++) fprintf(f, " %X %X %X %X", s[i * 4], s[i * 4 + 1], s[i * 4 + 2], s[i * 4 + 3]);
        fputc('\n', f);
        if (n % 1000 == 999) fflush(f);
    }
}
static DWORD WINAPI hitch_monitor(LPVOID)
{
    s_hz_ring = (HzSample*)calloc(HZ_RING, sizeof(HzSample));
    if (!s_hz_ring) return 0;
    if (!s_hz_mods_ready) { hz_mods_init(); s_hz_mods_ready = 1; }   /* #152: before any thread is suspended */
    /* slots: 0 = main guest thread, 1 = RSX ticker, then HITCH_TIDS guest tids */
    DWORD tids[HZ_SLOTS] = { 0 }; int gtid[HZ_SLOTS] = { 0 }; HANDLE hs[HZ_SLOTS] = { 0 };
    s_hz_slot_name[0] = "main(tid1)"; s_hz_slot_name[1] = "rsx-ticker"; s_hz_nslots = 2;
    { const char* e = getenv("HITCH_TIDS"); if (!e) e = "7";
      for (const char* p = e; *p && s_hz_nslots < HZ_SLOTS; ) {
          const int g = atoi(p);
          if (g > 1 && g <= PPU_THREAD_MAX) {
              static char names[HZ_SLOTS][24];
              snprintf(names[s_hz_nslots], 24, "tid%d", g);
              s_hz_slot_name[s_hz_nslots] = names[s_hz_nslots]; gtid[s_hz_nslots++] = g; }
          const char* c = strchr(p, ','); if (!c) break; p = c + 1; } }
    const uintptr_t exe = (uintptr_t)GetModuleHandleA(NULL);
    unsigned long long next_sec = ps3_qpc_us() + 1000000;
    HzSnap sec_prev; hz_read(&sec_prev);
    LONG pres_prev = 0;
    long mark_seen = 0;
    int nrep = 0;
    bool pend = false; unsigned long long p_t0 = 0, p_t1 = 0; HzSnap p_d; int p_frames = 0;
    for (;;) {
        Sleep(1);
        /* resolve host threads as they appear */
        if (!tids[0] && s_hz_main_tid) tids[0] = s_hz_main_tid;
        if (!tids[1] && s_hz_rsx_tid) tids[1] = s_hz_rsx_tid;
        for (int sl = 2; sl < s_hz_nslots; sl++)
            if (!tids[sl] && gtid[sl] > 0 && g_ppu_threads[gtid[sl] - 1].host_tid) {
                tids[sl] = g_ppu_threads[gtid[sl] - 1].host_tid;
                static char names[HZ_SLOTS][48];
                snprintf(names[sl], 48, "tid%d %s", gtid[sl], g_ppu_threads[gtid[sl] - 1].name);
                s_hz_slot_name[sl] = names[sl]; }
        /* HITCH_SPU=1: also the SPU threads (lifted SPU code shows by name) */
        { static int spu = -1; static long seen = 0;
          if (spu < 0) spu = getenv("HITCH_SPU") ? 1 : 0;
          while (spu && seen < g_spu_host_n && s_hz_nslots < HZ_SLOTS) {
              static char names[16][16];
              snprintf(names[seen], 16, "spu%ld", seen);
              tids[s_hz_nslots] = (DWORD)g_spu_host_tid[seen];
              s_hz_slot_name[s_hz_nslots++] = names[seen]; seen++; } }
        /* #182 HITCH_NOSAMPLE=1: counters only ([HZ]/[PACE]), no 1 ms thread suspends */
        static int nosample = -1;
        if (nosample < 0) nosample = getenv("HITCH_NOSAMPLE") ? 1 : 0;
        /* #182b HITCH_NOMAIN=1: do not suspend the main thread (slot 0) */
        static int nomain = -1;
        if (nomain < 0) nomain = getenv("HITCH_NOMAIN") ? 1 : 0;
        for (int sl = 0; sl < (nosample ? 0 : s_hz_nslots); sl++) {
            if (!tids[sl] || (sl == 0 && nomain)) continue;
            if (!hs[sl]) hs[sl] = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tids[sl]);
            if (!hs[sl]) { tids[sl] = 0; continue; }
            if (SuspendThread(hs[sl]) == (DWORD)-1) { CloseHandle(hs[sl]); hs[sl] = 0; continue; }
            CONTEXT c; c.ContextFlags = CONTEXT_FULL;
            HzSample s; memset(&s, 0, sizeof s);
            if (GetThreadContext(hs[sl], &c)) hz_unwind(&c, s.key, exe);
            ResumeThread(hs[sl]);
            if (s.key[0]) { s.t = ps3_qpc_us(); s.slot = (uint32_t)sl;
                            s_hz_ring[s_hz_ring_n % HZ_RING] = s; InterlockedIncrement(&s_hz_ring_n); }
        }
        const unsigned long long now = ps3_qpc_us();
        /* per-second line */
        if (now >= next_sec) {
            next_sec += 1000000; if (now > next_sec) next_sec = now + 1000000;
            HzSnap cur, d; hz_read(&cur); hz_sub(&d, &cur, &sec_prev); sec_prev = cur;
            const LONG pres = g_frames_presented; char b[512]; hz_fmt(b, sizeof b, &d);
            const long long mg = s_hz_maxgap_us; s_hz_maxgap_us = 0;
            fprintf(stderr, "[HZ] t=%.0fs pres=%ld maxframe=%lldms | %s%c",
                    (now - s_hz_t0) / 1e6, (long)(pres - pres_prev), mg / 1000, b, 10);
            /* #241 NHL_FREEZE_DUMP=N (default 6, 0 = off): once frames have been shown, N seconds
             * with no present = a freeze (Create-a-Play, play-test 2026-10-28: main stopped making
             * syscalls, no error logged). Dump every thread twice, 3 s apart, so a guest spin is
             * pinned to its lifted function; re-armed when presents resume. */
            { static int fz_after = -1; static int fz_secs = 0, fz_done = 0;
              if (fz_after < 0) { const char* e = getenv("NHL_FREEZE_DUMP"); fz_after = e ? atoi(e) : 6; }
              if (pres != pres_prev || pres == 0) { fz_secs = 0; fz_done = 0; }
              else if (fz_after > 0 && fz_done < 2 && ++fz_secs >= fz_after + 3 * fz_done) {
                  char lbl[64]; snprintf(lbl, sizeof lbl, "#241 freeze: no present for %ds", fz_secs);
                  dump_threads(lbl, (HMODULE)exe); fz_done++; } }
            pres_prev = pres;
            /* #281 NHL_FSCAN=<sec>: at that second (and 10 s later) list guest words holding the
             * timestep 1/60 (f32 3C888889 +-1 ulp, f64 3F91111111111111) -- half-rate sim RE. */
            { static int fs_at = -2, fs_n = 0;
              if (fs_at == -2) { const char* e = getenv("NHL_FSCAN"); fs_at = e ? atoi(e) : -1; }
              const int tsec = (int)((now - s_hz_t0) / 1000000);
              if (fs_at >= 0 && fs_n < 2 && tsec >= fs_at + 10 * fs_n) {
                  int hits = 0; fs_n++;
                  for (uint8_t* p = vm_base; p < vm_base + 0x100000000ull; ) {
                      MEMORY_BASIC_INFORMATION mi;
                      if (!VirtualQuery(p, &mi, sizeof mi)) break;
                      uint8_t* end = (uint8_t*)mi.BaseAddress + mi.RegionSize;
                      if (mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
                          for (uint8_t* q = (uint8_t*)mi.BaseAddress; q + 8 <= end; q += 4) {
                              const uint32_t w = _byteswap_ulong(*(const uint32_t*)q);
                              const int f32 = w >= 0x3C888888u && w <= 0x3C88888Au;
                              const int f64 = w == 0x3F911111u && _byteswap_ulong(*(const uint32_t*)(q + 4)) == 0x11111111u;
                              if ((f32 || f64) && hits++ < 400)
                                  fprintf(stderr, "[FSCAN] pass %d ea %08llX %s %08X\n", fs_n,
                                          (unsigned long long)(q - vm_base), f32 ? "f32" : "f64", w);
                          }
                      p = end;
                  }
                  fprintf(stderr, "[FSCAN] pass %d done, %d hits\n", fs_n, hits);
                  /* NHL_FCLOCK=1: snapshot all guest memory at pass 1; at pass 2 list f32/f64 words
                   * that grew by ~(steps between the passes) / 60 = sim-time accumulators. */
                  /* NHL_DT2=1: at pass 1 double the dt fields (+0..+C, +0x28) of the 16 records at
                   * 024F3000 (stride 0x3C0), to see which clocks follow them. */
                  if (fs_n == 1 && getenv("NHL_DT2")) {
                      const uint32_t two = _byteswap_ulong(0x3D08888Au);   /* 2/60 */
                      for (int r = 0; r < 16; r++) {
                          uint8_t* b = vm_base + 0x024F3000u + r * 0x3C0u;
                          static const int offs[5] = { 0, 4, 8, 12, 0x28 };
                          for (int o : offs) *(uint32_t*)(b + o) = two; }
                      fprintf(stderr, "[FCLOCK] NHL_DT2: 16 records set to 2/60\n");
                  }
                  if (getenv("NHL_FCLOCK")) {
                      struct Snap { uint64_t ea; size_t n; uint8_t* d; };
                      static Snap* sn = 0; static int nsn = 0; static long long st0 = 0;
                      if (fs_n == 1) {
                          sn = (Snap*)calloc(4096, sizeof(Snap)); st0 = g_game_steps;
                          for (uint8_t* p = vm_base; p < vm_base + 0x100000000ull && nsn < 4096; ) {
                              MEMORY_BASIC_INFORMATION mi; if (!VirtualQuery(p, &mi, sizeof mi)) break;
                              uint8_t* end = (uint8_t*)mi.BaseAddress + mi.RegionSize;
                              if (mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
                                  (mi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY))) {
                                  Snap& s = sn[nsn]; s.ea = (uint64_t)((uint8_t*)mi.BaseAddress - vm_base); s.n = mi.RegionSize;
                                  s.d = (uint8_t*)malloc(s.n); if (s.d) { memcpy(s.d, mi.BaseAddress, s.n); nsn++; } }
                              p = end; }
                          fprintf(stderr, "[FCLOCK] snapshot %d regions at step %lld\n", nsn, st0);
                      } else if (sn) {
                          const char* mul = getenv("NHL_FCLOCK_MUL");   /* look for clocks at N x sim rate */
                          const double want = (double)(g_game_steps - st0) / 60.0 * (mul ? atof(mul) : 1.0); int nh = 0;
                          fprintf(stderr, "[FCLOCK] %lld steps -> want +%.3f\n", g_game_steps - st0, want);
                          for (int r = 0; r < nsn; r++) {
                              const uint8_t* now_p = vm_base + sn[r].ea;
                              for (size_t o = 0; o + 8 <= sn[r].n; o += 4) {
                                  uint32_t a = _byteswap_ulong(*(const uint32_t*)(sn[r].d + o)), b = _byteswap_ulong(*(const uint32_t*)(now_p + o));
                                  if (a == b) continue;
                                  float fa, fb; memcpy(&fa, &a, 4); memcpy(&fb, &b, 4);
                                  const double df = (double)fb - fa;
                                  if (fa == fa && fb == fb && df > want * 0.9 && df < want * 1.1 && nh++ < 300)
                                      fprintf(stderr, "[FCLOCK] f32 ea %08llX %.4f -> %.4f (+%.4f)\n", (unsigned long long)(sn[r].ea + o), fa, fb, df);
                                  if (!(o & 7)) {
                                      uint64_t a8 = _byteswap_uint64(*(const uint64_t*)(sn[r].d + o)), b8 = _byteswap_uint64(*(const uint64_t*)(now_p + o));
                                      double da, db; memcpy(&da, &a8, 8); memcpy(&db, &b8, 8);
                                      const double dd = db - da;
                                      if (da == da && db == db && dd > want * 0.9 && dd < want * 1.1 && nh++ < 300)
                                          fprintf(stderr, "[FCLOCK] f64 ea %08llX %.4f -> %.4f (+%.4f)\n", (unsigned long long)(sn[r].ea + o), da, db, dd);
                                  }
                              }
                          }
                          fprintf(stderr, "[FCLOCK] %d candidates\n", nh);
                      }
                  }
                  /* NHL_FDUMP=<ea>,<bytes>: hex words (BE) of one candidate record */
                  if (const char* e = getenv("NHL_FDUMP")) {
                      unsigned ea = (unsigned)strtoul(e, 0, 16), n = 0x100;
                      if (const char* c = strchr(e, ',')) n = (unsigned)strtoul(c + 1, 0, 16);
                      for (unsigned o = 0; o < n; o += 32) {
                          char ln[160]; int k = snprintf(ln, sizeof ln, "[FDUMP] %08X:", ea + o);
                          for (unsigned j = 0; j < 32 && o + j < n; j += 4)
                              k += snprintf(ln + k, sizeof ln - k, " %08X", _byteswap_ulong(*(const uint32_t*)(vm_base + ea + o + j)));
                          fprintf(stderr, "%s\n", ln);
                      }
                  }
              } }
            { long g[4], r[5], sk;   /* #140b */
              for (int i = 0; i < 4; i++) g[i] = InterlockedExchange(&s_pace_gap[i], 0);
              for (int i = 0; i < 5; i++) r[i] = InterlockedExchange(&s_pace_run[i], 0);
              sk = InterlockedExchange(&s_pace_skip, 0);
              fprintf(stderr, "[PACE] t=%.0fs gaps <20ms %ld, 20-40 %ld, 40-60 %ld, 60+ %ld | update shown x1 %ld, x2 %ld, x3 %ld, x4 %ld, x5+ %ld | never shown %ld%c",
                      (now - s_hz_t0) / 1e6, g[0], g[1], g[2], g[3], r[0], r[1], r[2], r[3], r[4], sk, 10);
              static unsigned sec_n = 0;
              if ((++sec_n % 10) == 0) {   /* every 10 s: the last 32 present gaps in ms */
                  char o[400]; int p = 0; const long n0 = s_pace_raw_n;
                  for (int i = 0; i < 32 && p < 380; i++)
                      p += snprintf(o + p, sizeof o - p, " %.1f", s_pace_raw[(n0 + i) & 31] / 1000.0);
                  fprintf(stderr, "[PACE] last gaps ms:%s%c", o, 10); } }
            spu_putmap_tick((now - s_hz_t0) / 1e6);   /* #148 SPU_PUTMAP, every 10 s */
            for (int sl = 0; sl < 4; sl++) {   /* #145 PS3_SCSUM */
                char o[700];
                if (ps3_scsum_format(sl, o, sizeof o))
                    fprintf(stderr, "[SCSUM] t=%.0fs %s%c", (now - s_hz_t0) / 1e6, o, 10); }
        }
        /* long frames: merge adjacent ones, report once the run has ended */
        while (s_hz_ev_r != s_hz_ev_w) {
            const HzEvent* e = &s_hz_ev[s_hz_ev_r & 63];
            if (pend && e->t0 <= p_t1 + 300000 && e->t1 - p_t0 < 4000000) {
                p_t1 = e->t1; for (int i = 0; i < HZ_NV; i++) p_d.v[i] += e->d.v[i]; p_frames++;
            } else {
                if (pend && nrep < 400) { /* flush the previous run first */
                    char b[512]; hz_fmt(b, sizeof b, &p_d); nrep++;
                    fprintf(stderr, "[HITCH] t=%.2fs..%.2fs (%lldms, %d long frames) | %s%c",
                            (p_t0 - s_hz_t0) / 1e6, (p_t1 - s_hz_t0) / 1e6, (long long)(p_t1 - p_t0) / 1000, p_frames, b, 10);
                    hz_dump_window(p_t0, p_t1); }
                pend = true; p_t0 = e->t0; p_t1 = e->t1; p_d = e->d; p_frames = 1;
            }
            InterlockedIncrement(&s_hz_ev_r);
        }
        if (pend && now > p_t1 + 500000) {
            if (nrep++ < 400) {
                char b[512]; hz_fmt(b, sizeof b, &p_d);
                fprintf(stderr, "[HITCH] t=%.2fs..%.2fs (%lldms, %d long frames) | %s%c",
                        (p_t0 - s_hz_t0) / 1e6, (p_t1 - s_hz_t0) / 1e6, (long long)(p_t1 - p_t0) / 1000, p_frames, b, 10);
                hz_dump_window(p_t0, p_t1); }
            pend = false;
        }
        /* HITCH_PERIOD=<s>: profile of the last <s> seconds every <s> seconds --
         * where the threads spend steady-state time, not only during hitches. */
        { static long long per = -1; static unsigned long long next_per = 0;
          if (per < 0) { const char* e = getenv("HITCH_PERIOD"); per = e ? atoll(e) * 1000000ll : 0; }
          if (per > 0) {
              if (!next_per) next_per = now + (unsigned long long)per;
              if (now >= next_per) {
                  fprintf(stderr, "[HZPROF] t=%.0fs: profile of the last %llds%c",
                          (now - s_hz_t0) / 1e6, per / 1000000ll, 10);
                  hz_dump_window(now - (unsigned long long)per, now);
                  next_per = now + (unsigned long long)per;
              } } }
        /* F8: the user felt a slowdown */
        if (g_hitch_mark_req != mark_seen) {
            mark_seen = g_hitch_mark_req;
            const long lag_ms = (long)(GetTickCount() - (DWORD)g_hitch_mark_tick);
            const unsigned long long tm = now - (unsigned long long)(lag_ms > 0 ? lag_ms : 0) * 1000ull;
            fprintf(stderr, "[MARK] F8 #%ld at t=%.2fs (handled %ldms later) -- profile of the 3 s before:%c",
                    mark_seen, (tm - s_hz_t0) / 1e6, lag_ms, 10);
            hz_dump_window(tm - 3000000ull, tm);
        }
    }
}

#include "sys_mutex.h"   /* NHL: g_sys_mutexes, for the waits command's owner lookup */
extern "C" void ps3_host_apply_ppu_sched(int32_t guest_prio);   /* NHL: runtime/syscalls/sys_ppu_thread.c */
extern "C" void nhl_hle_register(void);                          /* NHL: nhl_hle.cpp */
/* NHL: in-flight syscall/HLE per guest thread (runtime/ppu/ppu_loader.cpp, ppu_hle.cpp). */
extern "C" uint32_t    g_sc_inflight[64], g_sc_inflight_r3[64], g_sc_inflight_lr[64];
extern "C" const char* g_hle_inflight[64];
extern "C" int spu_live_dump(char* out, int cap);   /* spu_channels.c: `spus` */

static DWORD WINAPI debug_console(LPVOID param)
{
    const char* path = (const char*)param;
    char outpath[1024];
    snprintf(outpath, sizeof outpath, "%s.out", path);

    HMODULE self = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&debug_console, &self);
    const DWORD t0 = GetTickCount();
    fprintf(stderr, "[dbg] console on '%s' -- write a command, read '%s'%c",
            path, outpath, 10);

    for (;;) {
        Sleep(100);
        FILE* f = fopen(path, "rb");
        if (!f) continue;
        char cmd[512] = {0};
        if (!fgets(cmd, sizeof cmd, f)) { fclose(f); continue; }
        fclose(f);
        char* nl = strpbrk(cmd, "\r\n"); if (nl) *nl = 0;
        if (!cmd[0]) continue;
        FILE* t = fopen(path, "wb"); if (t) fclose(t);   /* consume it */

        s_dbg_out = fopen(outpath, "ab");
        dbg_printf("%c[dbg] > %s%c", 10, cmd, 10);

        char verb[64] = {0};
        unsigned a = 0, b = 0;
        sscanf(cmd, "%63s", verb);

        if (!strcmp(verb, "help")) {
            dbg_printf("  threads          stacks of every guest thread, symbolised%c", 10);
            dbg_printf("  waits            guest threads inside a syscall/HLE: number, r3, lr%c", 10);
            dbg_printf("  prof <tid> [s]   sample one host thread ~1kHz, hottest functions%c", 10);
            dbg_printf("  hle              last HLE call the runtime dispatched%c", 10);
            dbg_printf("  stat             flips, HLE breadcrumb, uptime%c", 10);
            dbg_printf("  mem <hex> [len]  hexdump guest memory%c", 10);
            dbg_printf("  poke32 <hex> <v> write a guest u32%c", 10);
            dbg_printf("  knobs [prefix]   diagnostics this run was started with%c", 10);
        } else if (!strcmp(verb, "threads")) {
            dump_threads("console", self);
        } else if (!strcmp(verb, "find32") && sscanf(cmd, "%*s %x", &a) == 1) {
            /* find32 <hex>: every aligned big-endian u32 equal to <hex> in committed
             * guest memory below 0xC0000000 (first 4096 hits). */
            const uint32_t be = ((a & 0xFF) << 24) | ((a & 0xFF00) << 8) | ((a >> 8) & 0xFF00) | (a >> 24);
            int hits = 0;
            for (uintptr_t p = 0; p < 0xC0000000u && hits < 4096 && vm_base; ) {
                MEMORY_BASIC_INFORMATION mi;
                if (!VirtualQuery(vm_base + p, &mi, sizeof mi)) break;
                const uintptr_t end = (uintptr_t)((uint8_t*)mi.BaseAddress + mi.RegionSize) - (uintptr_t)vm_base;
                if (mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
                    for (uintptr_t q = p & ~3u; q + 4 <= end && q < 0xC0000000u && hits < 4096; q += 4)
                        if (*(const uint32_t*)(vm_base + q) == be) { dbg_printf("  0x%08X%c", (unsigned)q, 10); hits++; }
                p = end;
            }
            dbg_printf("  %d hit(s)%c", hits, 10);
        } else if (!strcmp(verb, "qbset") && sscanf(cmd, "%*s %x %x", &a, &b) == 2) {
            /* qbset <name-crc> <value>: set every integer QB struct member with
             * that name. A member is 16 bytes: xx 01|81 00 00 | name | value | next,
             * big-endian (GH3: bot_play, unlocked). Prints how many it set. */
            const uint32_t be_name = ((a & 0xFF) << 24) | ((a & 0xFF00) << 8) | ((a >> 8) & 0xFF00) | (a >> 24);
            const uint32_t be_val  = ((b & 0xFF) << 24) | ((b & 0xFF00) << 8) | ((b >> 8) & 0xFF00) | (b >> 24);
            unsigned n = 0;
            for (uintptr_t p = 0; p < 0xC0000000u && vm_base; ) {
                MEMORY_BASIC_INFORMATION mi;
                if (!VirtualQuery(vm_base + p, &mi, sizeof mi)) break;
                const uintptr_t end = (uintptr_t)((uint8_t*)mi.BaseAddress + mi.RegionSize) - (uintptr_t)vm_base;
                if (mi.State == MEM_COMMIT && (mi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)))
                    for (uintptr_t q = (p + 4) & ~3u; q + 8 <= end; q += 4)
                        if (*(const uint32_t*)(vm_base + q) == be_name) {
                            /* type word: any flags byte, then 0x01 or 0x81 (integer), 00 00;
                             * only 0/1 values change -- an unlocked tag is a flag. */
                            const uint8_t* t = vm_base + q - 4;
                            const uint32_t v = *(const uint32_t*)(vm_base + q + 4);
                            if ((t[1] == 0x01 || t[1] == 0x81) && !t[2] && !t[3] &&
                                (v == 0 || v == 0x01000000u)) {
                                *(uint32_t*)(vm_base + q + 4) = be_val; n++;
                            }
                        }
                p = end;
            }
            dbg_printf("  qbset 0x%08X = %u: %u member(s)%c", a, b, n, 10);
        } else if (!strcmp(verb, "prof") && sscanf(cmd, "%*s %u %u", &a, &b) >= 1) {
            dbg_prof((DWORD)a, b ? b : 5);
        } else if (!strcmp(verb, "dumpb8")) {
            /* NHL: dumpb8 <local 1|io 0> <hexoff> <w> <h> <pitch> <path.bmp> -- write a
             * guest single-channel (B8) plane as a grey BMP: the movie's Y/U/V
             * planes, to tell decoder output apart from the display path. */
            unsigned loc = 0, off = 0, w = 0, h = 0, pitch = 0; char path[512] = {0};
            if (sscanf(cmd, "%*s %u %x %u %u %u %511s", &loc, &off, &w, &h, &pitch, path) == 6 &&
                w && h && w <= 4096 && h <= 4096 && pitch >= w && vm_base) {
                const uint32_t ea = cellGcmResolveLocated((int)loc, off);
                FILE* f = fopen(path, "wb");
                if (f && ea) {
                    const uint32_t rowb = w * 3, rowp = (rowb + 3) & ~3u, fsz = 54 + rowp * h;
                    unsigned char hd[54] = {'B','M'};
                    hd[2]=(unsigned char)fsz; hd[3]=(unsigned char)(fsz>>8); hd[4]=(unsigned char)(fsz>>16); hd[5]=(unsigned char)(fsz>>24);
                    hd[10]=54; hd[14]=40;
                    hd[18]=(unsigned char)w; hd[19]=(unsigned char)(w>>8); hd[22]=(unsigned char)h; hd[23]=(unsigned char)(h>>8);
                    hd[26]=1; hd[28]=24;
                    fwrite(hd, 1, 54, f);
                    for (int y = (int)h - 1; y >= 0; y--) {
                        const unsigned char* row = vm_base + ea + (uint64_t)y * pitch;
                        for (unsigned x = 0; x < w; x++) { unsigned char px[3] = {row[x], row[x], row[x]}; fwrite(px, 1, 3, f); }
                        unsigned char z[3] = {0, 0, 0}; fwrite(z, 1, rowp - rowb, f);
                    }
                    dbg_printf("  wrote %s from ea 0x%08X%c", path, ea, 10);
                } else dbg_printf("  dumpb8: cannot open %s or resolve offset%c", path, 10);
                if (f) fclose(f);
            } else dbg_printf("  usage: dumpb8 <local 1|io 0> <hexoff> <w> <h> <pitch> <path.bmp>%c", 10);
        } else if (!strcmp(verb, "spus")) {
            /* NHL: every live interpreter SPU context -- pc, lr, channel wait, image. */
            static char buf[4096];
            spu_live_dump(buf, (int)sizeof buf);
            dbg_printf("%s", buf);
        } else if (!strcmp(verb, "waits")) {
            /* NHL: every guest thread that is inside an lv2 syscall or an HLE call
             * right now, with the syscall's r3 (the sync object id for every
             * blocking primitive) and the guest lr it was called from. */
            for (unsigned t = 0; t < 64; t++) {
                const uint32_t sc = g_sc_inflight[t];
                const char* hle = g_hle_inflight[t];
                if (!sc && !hle) continue;
                const char* nm = "";
                switch (sc) {
                case 85:  nm = "event_flag_wait";     break;
                case 92:  nm = "semaphore_wait";      break;
                case 97:  nm = "lwmutex_lock";        break;
                case 113: nm = "lwcond_queue_wait";   break;
                case 102: nm = "mutex_lock";          break;
                case 107: nm = "cond_wait";           break;
                case 130: nm = "event_queue_receive"; break;
                case 141: nm = "timer_usleep";        break;
                case 142: nm = "timer_sleep";         break;
                case 44:  nm = "ppu_thread_join";     break;
                }
                dbg_printf("  tid %2u  sc %3u %-20s r3=0x%08X lr=0x%08X  hle=%s%c", t, sc, nm,
                           g_sc_inflight_r3[t], g_sc_inflight_lr[t], hle ? hle : "-", 10);
                const uint32_t mid = g_sc_inflight_r3[t];
                if (sc == 102 && mid >= 1 && mid <= SYS_MUTEX_MAX) {
                    const sys_mutex_info* m = &g_sys_mutexes[mid - 1];
                    dbg_printf("          mutex %u '%.8s' owner tid %llu lock_count %u recursive %d%c",
                               mid, m->name, (unsigned long long)m->owner_tid,
                               (unsigned)m->lock_count, m->recursive, 10);
                }
            }
        } else if (!strcmp(verb, "hle")) {
            dbg_printf("  last HLE = 0x%08X (%s)%c", g_last_hle_nid,
                       g_last_hle_name ? g_last_hle_name : "", 10);
        } else if (!strcmp(verb, "stat")) {
            dbg_printf("  uptime   %.1f s%c", (GetTickCount() - t0) / 1000.0, 10);
            dbg_printf("  flips    %u%c", cellGcm_flip_request_count(), 10);
            dbg_printf("  last HLE 0x%08X (%s)%c", g_last_hle_nid,
                       g_last_hle_name ? g_last_hle_name : "", 10);
            dbg_printf("  vm_base  %s%c", vm_base ? "live" : "not mapped", 10);
        } else if (!strcmp(verb, "mem") && sscanf(cmd, "%*s %x %u", &a, &b) >= 1) {
            dbg_mem(a, b);
        } else if (!strcmp(verb, "poke32") && sscanf(cmd, "%*s %x %x", &a, &b) == 2) {
            if (vm_base) {
                uint32_t be = ((b & 0xFF) << 24) | ((b & 0xFF00) << 8) |
                              ((b >> 8) & 0xFF00) | ((b >> 24) & 0xFF);
                memcpy(vm_base + a, &be, 4);
                dbg_printf("  [0x%08X] = 0x%08X%c", a, b, 10);
            } else dbg_printf("  vm_base not live yet%c", 10);
        } else if (!strcmp(verb, "knobs")) {
            char pfx[64] = {0};
            sscanf(cmd, "%*s %63s", pfx);
            dbg_knobs(pfx);
        } else {
            dbg_printf("  ? '%s' -- try 'help'%c", verb, 10);
        }

        if (s_dbg_out) { fclose(s_dbg_out); s_dbg_out = NULL; }
    }
    return 0;
}

static DWORD WINAPI hang_watchdog(LPVOID)
{
    HMODULE self = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&hang_watchdog, &self);
    /* ponytail: two fixed samples cover a boot wedge, but a hang that happens
     * minutes in (e.g. after a level load) needs them moved. WATCHDOG_AT="a,b"
     * gives the two sample times in seconds. */
    /* [#143] The automatic 8 s / 15 s samples are opt-in now (WATCHDOG_AT=8,15): mid-boot,
     * suspending every thread was the trigger of the boot hangs, and the console's
     * "threads" command gives the same dump on demand. */
    int t1 = 8, t2 = 15;
    { const char* e = getenv("WATCHDOG_AT"); int a = 0, b = 0;
      if (!e || !*e) return 0;
      if (sscanf(e, "%d,%d", &a, &b) == 2 && a > 0 && b > a) { t1 = a; t2 = b; } }
    char lbl[64];
    Sleep((DWORD)t1 * 1000);
    snprintf(lbl, sizeof lbl, "%ds sample", t1); dump_threads(lbl, self);
    Sleep((DWORD)(t2 - t1) * 1000);
    snprintf(lbl, sizeof lbl, "%ds sample", t2); dump_threads(lbl, self);
    return 0;
}
#endif

/* The flat VM treats every address as valid RAM, so it must span every region
 * the PS3 memory map uses. The game's heap maps at 0x20000000+ and reaches
 * ~0x50000000, but sys_ppu_thread_create allocates thread stacks in the PS3
 * stack region at 0xD0000000-0xDFFFFFFF (vm.h: VM_STACK_BASE). Without covering
 * that, every spawned thread's stack access is OOB (reads 0 / writes dropped)
 * and the thread crashes. Size to include the stack region: ~3.75 GB, lazily
 * committed by the OS (only touched pages are backed). */
#define VM_SIZE    0x100010000ull /* full 32-bit guest space + 64K guard (top-edge reads), demand-committed */
#define STACK_TOP  0x0FF00000u   /* main-thread stack, below the 0x10000000 segment */

#ifdef _WIN32
/* Demand-paging for the flat VM: reserve the full 4 GB guest space up front (no
 * commit cost) and commit each 64 KB page on first access. This makes EVERY
 * 32-bit guest offset valid -- a garbage guest pointer reads as zero instead of
 * crashing the process (essential now that the recompiled engine runs deep and
 * worker threads touch incomplete state). Out-of-arena faults fall through to
 * the crash reporter. */
/* Guard-page-violation reporter: STATUS_GUARD_PAGE_VIOLATION (0x80000001)
 * bypasses every ACCESS_VIOLATION-only handler and kills the process SILENTLY
 * (observed the moment the Bink SPU decoder first really ran). Log full context
 * before letting the chain continue. */
static LONG WINAPI guard_report_veh(EXCEPTION_POINTERS* ep)
{
    if (ep->ExceptionRecord->ExceptionCode == 0x80000001u /*STATUS_GUARD_PAGE_VIOLATION*/) {
        static LONG s_n = 0;
        if (InterlockedIncrement(&s_n) <= 8) {
            char* mbase = (char*)GetModuleHandleA(NULL);
            fprintf(stderr, "\n[GUARDVIOLATION] tid=%lu %s fault=0x%llX rip-rva=0x%llX\n",
                    GetCurrentThreadId(),
                    ep->ExceptionRecord->ExceptionInformation[0] ? "write" : "read",
                    (unsigned long long)ep->ExceptionRecord->ExceptionInformation[1],
                    (unsigned long long)((char*)ep->ExceptionRecord->ExceptionAddress - mbase));
            void* fr[24]; USHORT n = RtlCaptureStackBackTrace(0, 24, fr, NULL);
            fprintf(stderr, "[GUARDVIOLATION] bt-rva:");
            for (USHORT i = 0; i < n; i++)
                fprintf(stderr, " %llX", (unsigned long long)((char*)fr[i] - mbase));
            fprintf(stderr, "\n"); fflush(stderr);
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
static LONG WINAPI vm_commit_veh(EXCEPTION_POINTERS* ep)
{
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        ULONG_PTR fault = ep->ExceptionRecord->ExceptionInformation[1];
        uintptr_t base  = (uintptr_t)vm_base;
        if (vm_base && fault >= base && fault < base + VM_SIZE) {
            void* page = (void*)(fault & ~(uintptr_t)0xFFFF);
            if (VirtualAlloc(page, 0x10000, MEM_COMMIT, PAGE_READWRITE))
                return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

extern "C" uint32_t ppu_hle_inject_base;   /* vm.h; set in main() below */
extern "C" int nhl_skip_intro(void);       /* nhl_hle.cpp, #297 */
extern "C" int32_t cellSaveData_set_scratch_region(uint32_t base, uint32_t size);   /* cellSaveData.c */
extern "C" int32_t cellGame_set_scratch_region(uint32_t base, uint32_t size);       /* cellGame.c */

int main(int argc, char** argv)
{
    /* [PPU ELF] [--config FILE] [--username NAME] [--psnr host[:port]], flags in
     * any order. The ELF defaults to nhl_settings.ini's [paths] eboot (#221). */
    const char* elf = NULL;
    const char* username = NULL;
    const char* psnr = NULL;
    const char* config = NULL;
    for (int i = 1; i < argc; i++) {
        const char* a = argv[i];
        const char** flag = !strncmp(a, "--username", 10) ? &username
                          : !strncmp(a, "--psnr", 6)      ? &psnr
                          : !strncmp(a, "--config", 8)    ? &config : NULL;
        if (!flag) { if (!elf) elf = a; continue; }
        const char* eq = strchr(a, '=');
        if (eq) *flag = eq + 1;
        else if (i + 1 < argc) *flag = argv[++i];
    }
    nhl_config_load(config);   /* before anything reads the environment */
    if (!elf) elf = nhl_config_eboot();
    if (!elf) {
        printf("usage: %s [PPU ELF] [--config FILE] [--username NAME] [--psnr host[:port]]\n", argv[0]);
        return 2;
    }
    np_psnr_setup(username, psnr);

#ifdef _WIN32
#pragma comment(lib, "winmm.lib")
    timeBeginPeriod(1);   /* 1ms timer resolution: the default ~15.6ms granularity
                           * inflates every sub-15ms wait (the game's event polls,
                           * usleeps) and throttled the whole title. */
    SetUnhandledExceptionFilter(ydkj_crash_filter);
    AddVectoredExceptionHandler(0 /*last*/, [](EXCEPTION_POINTERS* ep)->LONG{
        if (ep->ExceptionRecord->ExceptionCode == 0xC00000FDu /*STACK_OVERFLOW*/) {
            fprintf(stderr,"\n[STACKOVERFLOW] infinite recursion detected; backtrace (RVAs):\n");
            HMODULE mod=GetModuleHandleA(0); void* fr[62]; USHORT n=RtlCaptureStackBackTrace(0,62,fr,0);
            for(USHORT i=0;i<n;i++) fprintf(stderr," %llX",(unsigned long long)((char*)fr[i]-(char*)mod));
            fprintf(stderr,"\n"); fflush(stderr); ExitProcess(7);
        }
        return EXCEPTION_CONTINUE_SEARCH; });
    { ULONG g=256*1024; SetThreadStackGuarantee(&g); }  /* reserve stack so the SO handler can run */
    signal(SIGABRT, ydkj_abort_handler);
    setvbuf(stdout, NULL, _IONBF, 0);   /* unbuffered: don't lose prints on kill */
    /* [NHL #259] NHL_CPU_AFFINITY=<hex mask>: run on those logical CPUs only, before any
     * thread starts (FF on an SMT 2k/2k+1 layout = 4 cores / 8 threads, a Steam Deck stand-in).
     * #258's SPU 'auto' reads the same mask, so it picks what that CPU would get. */
    if (const char* e = getenv("NHL_CPU_AFFINITY")) {
        const DWORD_PTR m = (DWORD_PTR)_strtoui64(e, 0, 16);
        const BOOL ok = m && SetProcessAffinityMask(GetCurrentProcess(), m);
        fprintf(stderr, "[boot] cpu affinity 0x%llX %s\n", (unsigned long long)m, ok ? "set" : "FAILED");
    }
#endif

    /* Flat VM: one host buffer, guest addr -> vm_base + addr. This maps the
     * FULL 32-bit guest space uniformly (page 0, the 0x60000000..0xD0000000
     * range, everything) -- which native-VA mapping can't on Windows, because
     * the OS reserves the low 64 KB and DLLs occupy parts of the mid range.
     * On real PS3 those addresses are RAM, and the game writes to them (its
     * null-object inits land on page 0); calloc backs them so the game runs.
     * HLE functions that take guest pointers must translate via vm_base /
     * vm_write* (which also byte-swap) -- a raw *guest_ptr would deref the host
     * buffer's offset incorrectly. */
#ifdef _WIN32
    /* Reserve the full 4 GB guest space; pages commit on first touch via the VEH. */
    AddVectoredExceptionHandler(1, guard_report_veh);  /* diagnose 0x80000001 silent deaths */
    AddVectoredExceptionHandler(1, vm_commit_veh);
    vm_base = (uint8_t*)VirtualAlloc(NULL, VM_SIZE, MEM_RESERVE, PAGE_READWRITE);
    ppu_vm_size = 0;   /* full 32-bit space backed -> OOB guard unnecessary */
#else
    vm_base = (uint8_t*)calloc(1, 0xE0000000u);
    ppu_vm_size = 0xE0000000u;
#endif
    if (!vm_base) { printf("vm alloc failed\n"); return 1; }

    /* Home of the runtime-injected guest structures (GCM labels, the control
     * register put/get/ref, offset tables, ADEC/VDEC scratch, RSX device info;
     * vm.h). The SDK default 0x20000000 is the first byte of NHL's own heap: the
     * game reserves 0x20000000..0x40000000 with sys_mmapper_allocate_address and
     * its movie player and job workers write all over the start of it. The worst
     * casualty was ctrl->ref, which EA's command-ring allocator (func_00DFA55C)
     * reads as the GPU position -- a stray heap value there made it compute a
     * segment that cannot hold SET_REFERENCE, recurse until the depth cap skipped
     * the reserve, and run the command buffer straight out of the IO region (the
     * "derail after the intro movie"). 0xBF000000 sits above every allocator the
     * title uses (sys_memory 0x40000000..0x50000000) and below RSX local memory
     * (0xC0000000). NHL_INJECT_BASE=<hex> overrides it for experiments. */
    { const char* e = getenv("NHL_INJECT_BASE");
      ppu_hle_inject_base = e ? (uint32_t)strtoul(e, 0, 16) : 0xBF000000u;
      printf("[boot] HLE inject base 0x%08X\n", ppu_hle_inject_base); }
#ifdef _WIN32
    printf("[boot] ANSI code page %u%s\n", GetACP(), GetACP() == 65001 ? " (UTF-8, #317)" : "");
#endif
    vm_gate_init();   /* #271 */
    /* The save-data and game-data HLEs marshal their callback structs through
     * fixed guest scratch windows, and the SDK defaults (0x024E0000 / 0x02520000)
     * are inside NHL's own BSS (0x020E0000..0x0252DE00). cellSaveDataListAutoLoad
     * zeroed and rewrote 0x024E0000+ under the game -- including the "AI" entry of
     * its named-handler list at 0x024E074C, so the match load could never find its
     * AI and froze before the starting lineup (play11/play12: [GUARD] CLEARED from
     * savedata_execute). Both move next to the GCM window. */
    { const uint32_t sd = ppu_hle_inject_base + 0x00100000u, gd = ppu_hle_inject_base + 0x00120000u;
      if (cellSaveData_set_scratch_region(sd, 0x20000u) != 0 || cellGame_set_scratch_region(gd, 0x1000u) != 0)
          printf("[boot] WARNING: could not move the save/game-data scratch windows\n");
      else printf("[boot] save-data scratch 0x%08X, game-data scratch 0x%08X\n", sd, gd); }

    uint32_t entry = ppu_load_elf(elf);
    if (!entry) { printf("load failed\n"); return 1; }

    /* [NHL local patch #297] skip the intro: the game's own quick boot (full-boot byte off),
     * only when the loaded program has the 1.01 word there. */
    { if (nhl_skip_intro()) {
          const uint32_t w = vm_read32(0x0220B9DCu);
          if (w == 0x01000000u) vm_write32(0x0220B9DCu, 0);
          printf("[boot] skip intro: %s\n", w == 0x01000000u ? "quick boot" : "unexpected word, left as is");
      } }

    derive_vfs_root(elf);
    printf("[boot] VFS root: %s\n", ppu_vfs_root);

    /* Real title id, from the game's own PARAM.SFO. cellGame has been able to
     * read this since 2026-06-21, but only a title's own main() ever called it,
     * so every port that moved to this harness silently kept the BLES00000
     * placeholder -- and that id is what cellGame / cellSaveData / trophy build
     * their /dev_hdd0/game/<id> paths from, so saves landed in a directory
     * belonging to no title. Same shape as the live-draw engine before 1a050be:
     * a working facility with no caller in the shared harness. */
    {
        /* PARAM.SFO sits in a different place depending on how the VFS is rooted.
         * A disc-style root has it under PS3_GAME/; a title whose content is
         * opened by RELATIVE path needs the root AT USRDIR, and then PARAM.SFO is
         * one level up instead. Only the first layout was tried, so rooting at
         * USRDIR silently lost the title id and name -- which is how the window
         * caption ended up on its hardcoded fallback. Try each and take the
         * first that exists. */
        static const char* const kSfoLayouts[] = {
            "%s/PS3_GAME/PARAM.SFO",   /* vfs root = disc root      */
            "%s/../PARAM.SFO",         /* vfs root = .../USRDIR     */
            "%s/PARAM.SFO",            /* vfs root = the game dir   */
        };
        char sfo[1100];
        int found = 0;
        for (size_t i = 0; i < sizeof kSfoLayouts / sizeof kSfoLayouts[0]; i++) {
            snprintf(sfo, sizeof sfo, kSfoLayouts[i], ppu_vfs_root);
            FILE* f = fopen(sfo, "rb");
            if (f) { fclose(f); found = 1; break; }
        }
        if (!found)
            snprintf(sfo, sizeof sfo, "%s/PS3_GAME/PARAM.SFO", ppu_vfs_root);
        cellGame_init_from_paramsfo(sfo);
    }

    /* NHL: cellFs keeps its own mount table (default ./gamedata/...). Point it
     * where ppu_fs points: disc and hdd1 under the VFS root, hdd0 under
     * PS3_HDD0_ROOT -- the port's sandbox, never RPCS3's dev_hdd0. */
    {
        static char root_slash[1100], hdd0_slash[1100], hdd1_slash[1100];
        snprintf(root_slash, sizeof root_slash, "%s/", ppu_vfs_root);
        cellfs_add_path_mapping("/dev_bdvd/", root_slash);
        /* #221: PS3_HDD1_ROOT keeps the system cache out of the game folder. */
        const char* hdd1 = getenv("PS3_HDD1_ROOT");
        snprintf(hdd1_slash, sizeof hdd1_slash, "%s/", hdd1 && *hdd1 ? hdd1 : ppu_vfs_root);
        cellfs_add_path_mapping("/dev_hdd1/", hdd1_slash);
        const char* hdd0 = getenv("PS3_HDD0_ROOT");
        if (hdd0 && *hdd0) {
            snprintf(hdd0_slash, sizeof hdd0_slash, "%s/", hdd0);
            cellfs_add_path_mapping("/dev_hdd0/", hdd0_slash);
        }
    }

    fprintf(stderr,"[boot-dbg] before ppu_recomp_register\n"); fflush(stderr);
    ppu_recomp_register();   /* lifted function table -> address map */
    fprintf(stderr,"[boot-dbg] after ppu_recomp_register; before ps3_load_prx_modules\n"); fflush(stderr);
    ps3_load_prx_modules();  /* real system PRX (libsre) -> guest RAM + exports */
    fprintf(stderr,"[boot-dbg] after prx; before ppu_hle_init\n"); fflush(stderr);
    ppu_hle_init();          /* firmware import NID -> HLE handlers */
    nhl_hle_register();      /* NHL: port-specific handlers (nhl_hle.cpp) */
    ppu_sysprx_register();   /* boot-critical CRT (sys_initialize_tls, ...) */
    ppu_fs_register();       /* cellFs VFS over the real game directory */
    fprintf(stderr,"[boot-dbg] before lv2_init_syscalls\n"); fflush(stderr);
    lv2_init_syscalls();     /* real lv2 syscall table (semaphore/memory/fs/...) */

    /* Two scalars that make a milestone diff interpretable: a lift that found
     * fewer functions, or a build that registered fewer HLE handlers, explains
     * a whole run going missing without anyone having to bisect the stream. */
    ps3_ms_kv("load:functions", (long long)function_table_count);
    ps3_ms_kv("load:hle_registered", (long long)ps3_hle_count());
    ps3_ms("boot:init_done");
    fprintf(stderr,"[boot-dbg] after lv2_init_syscalls\n"); fflush(stderr);

    /* Install the guest-callback hook and start the synthetic RSX vblank driver
     * so the game's frame loop advances (it no-ops until the game registers its
     * vblank/flip handlers during init). */
    g_ps3_guest_caller = harness_guest_caller;
    /* CreateThread is a pthread wrapper off Windows (win32_compat.h), so the
     * frame clock now runs everywhere. Without it a POSIX host never ticks
     * vblank or drains the FIFO, and the guest waits on fences forever. */
    CreateThread(NULL, 4u * 1024 * 1024, vblank_ticker, NULL, 0, NULL);
    /* NHL_WORDWATCH=<hex ea>: a 1 ms sampler on one guest word, for a field that
     * something other than a PPU store or an SPU PUT (both have watches) clears --
     * a host memcpy, a file read. Logs each change with every guest thread's
     * in-flight syscall/HLE at that moment. */
    if (const char* ww = getenv("NHL_WORDWATCH")) {
        static uint32_t s_ea; s_ea = (uint32_t)strtoul(ww, 0, 16);
        CreateThread(NULL, 64u * 1024, [](LPVOID) -> DWORD {
            uint32_t last = 0; int have = 0, n = 0;
            for (;;) {
                Sleep(1);
                if (!vm_base) continue;
                const uint32_t v = *(volatile const uint32_t*)(vm_base + s_ea);
                if (have && v == last) continue;
                if (have && n++ < 64) {
                    fprintf(stderr, "[wordwatch] t=%llums 0x%08X: 0x%08X -> 0x%08X  last HLE %s; in flight:",
                            (unsigned long long)GetTickCount64(), s_ea, _byteswap_ulong(last), _byteswap_ulong(v),
                            g_last_hle_name ? g_last_hle_name : "-");
                    for (unsigned t = 0; t < 64; t++)
                        if (g_sc_inflight[t] || g_hle_inflight[t])
                            fprintf(stderr, " t%u:sc%u/%s", t, g_sc_inflight[t], g_hle_inflight[t] ? g_hle_inflight[t] : "-");
                    fprintf(stderr, "\n"); fflush(stderr);
                }
                last = v; have = 1;
            }
        }, NULL, 0, NULL);
    }
    if (getenv("PS3_GUEST_PROF"))
        CreateThread(NULL, 0, guest_prof_thread, NULL, 0, NULL);
#ifdef _WIN32
#ifdef NHL_PGO_GEN
    /* #156: an instrumented (NHL_PGO_GEN) build writes its counters to LLVM_PROFILE_FILE
     * every 30 s -- runs end by process kill, which skips the exit-time write. Each
     * write replaces the file with the cumulative counts so far. */
    { struct PgoDump { static DWORD WINAPI run(LPVOID) {
          for (unsigned n = 1;; n++) {
              Sleep(30000);
              const int rc = __llvm_profile_write_file();
              fprintf(stderr, "[PGO] profile write #%u -> %s (rc %d)%c", n,
                      getenv("LLVM_PROFILE_FILE") ? getenv("LLVM_PROFILE_FILE") : "default.profraw", rc, 10);
          } } };
      CreateThread(NULL, 0, PgoDump::run, NULL, 0, NULL); }
#endif
    s_hz_t0 = ps3_qpc_us();
    s_hz_main_tid = GetCurrentThreadId();   /* ppu_run below runs the main guest thread here */
    if (getenv("NHL_USLEEP_SPIN")) g_usleep_block = 0;   /* #270 */
    if (getenv("NHL_PLACE_FILE")) CreateThread(NULL, 0, place_thread, NULL, 0, NULL);   /* #270 */
    if (getenv("NHL_TL_FILE")) CreateThread(NULL, 0, timeline_thread, NULL, 0, NULL);   /* #278 */
    if (hz_on()) {
        HANDLE hm = CreateThread(NULL, 0, hitch_monitor, NULL, 0, NULL);
        if (hm) SetThreadPriority(hm, THREAD_PRIORITY_HIGHEST);
        fprintf(stderr, "[HITCH] monitor on (HITCH_MS=%s, HITCH_TIDS=%s)\n",
                getenv("HITCH_MS") ? getenv("HITCH_MS") : "250", getenv("HITCH_TIDS") ? getenv("HITCH_TIDS") : "7");
    }
    CreateThread(NULL, 0, hang_watchdog, NULL, 0, NULL);
    /* PS3_DEBUG=<file>: ask a running title what it is doing. */
    { static char dbgpath[1024];
      const char* dp = getenv("PS3_DEBUG");
      if (dp && *dp) { snprintf(dbgpath, sizeof dbgpath, "%s", dp);
                       CreateThread(NULL, 0, debug_console, dbgpath, 0, NULL); } }   /* tlhelp32-based */
#endif

    ps3_ms("boot:entry");
#ifdef _WIN32
    { extern void ps3_sampler_start(void); ps3_sampler_start(); } /* PS3_SAMPLE=<ms> */
#endif
    /* PS3_MAIN_STACK_LV2=1: give the main thread its stack where lv2 does, in
     * the 0xD0000000 stack region (the first allocation there, which is also
     * what sys_ppu_thread_get_stack_information already reports for it).
     * Guest code can tell: Tornado Outbreak's SPU memcpy (func_000D60E0) copies
     * on the PPU when the destination shares the stack's top nibble, then waits
     * for an SPU reply only the SPU path sends -- with the stack at 0x0FF00000,
     * every low address "is on the stack" and the boot hangs. Opt-in until the
     * regression gate has run with it on. */
    uint32_t stack_top = STACK_TOP;
    { const char* ms = getenv("PS3_MAIN_STACK_LV2");
      if (ms && *ms && *ms != '0') {
          uint32_t base = vm_stack_allocate(&g_vm_stack_alloc, VM_PPU_STACK_SIZE);
          if (base) stack_top = base + VM_PPU_STACK_SIZE;
      } }
    printf("\n[boot] dispatching entry OPD 0x%08X (stack top 0x%08X)\n\n", entry, stack_top);
#ifdef _WIN32
    fprintf(stderr, "[boot] MAIN guest thread tid=%lu\n", (unsigned long)GetCurrentThreadId());
#endif
    /* NHL: primary_prio from the EBOOT's sys_process_param is 1001. */
    ps3_host_apply_ppu_sched(1001);
    int rc = ppu_run(entry, stack_top);
    printf("\n[boot] ppu_run returned %d (entry function unwound)\n", rc);
    /* A guest that called sys_process_exit never reaches this line: that path
     * ends in the host exit(). Getting here means the entry function returned
     * instead, so hand back the status the guest published if it published one
     * -- returning a hardcoded 0 reported success for a run that never got
     * anywhere, which is exactly the kind of green a boot harness must not
     * produce. */
    return g_sys_process_exit_called ? (int)g_sys_process_exit_code : 0;
}
