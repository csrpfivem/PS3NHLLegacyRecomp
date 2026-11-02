/*
 * ps3recomp - Semaphore syscalls
 */

#ifndef SYS_SEMAPHORE_H
#define SYS_SEMAPHORE_H

#include "lv2_syscall_table.h"
#include "../ppu/ppu_context.h"
#include "../../include/ps3emu/ps3types.h"
#include "../../include/ps3emu/error_codes.h"

#include <stdint.h>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
#else
  #include <pthread.h>
  #include <time.h>
  #include <errno.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* [NHL local patch] was 256. EA's EA::Thread::Futex creates one semaphore per
 * lock (every allocator, every container); NHL exhausted 256, the overflow
 * creates failed with EAGAIN, the futexes kept semaphore id 0 and their
 * contended Lock() fell straight through (wait -> ESRCH) -- the heap race.
 * Same ceiling as sys_mutex/sys_cond. */
#define SYS_SEMAPHORE_MAX  8192

typedef struct sys_semaphore_info {
    int      active;
    uint32_t id;         /* [#232] the id handed out for this slot's current object */
    char     name[8];
    uint32_t protocol;
    int32_t  value;      /* current count; the single source of truth */
    int32_t  max_value;
    int32_t  waiters;    /* threads currently parked in wait() */

#ifdef _WIN32
    HANDLE   sem_handle;
    CRITICAL_SECTION value_lock;  /* protects value for get_value/post */
#else
    pthread_mutex_t mtx;
    pthread_cond_t  cv;
#endif

} sys_semaphore_info;

extern sys_semaphore_info g_sys_semaphores[SYS_SEMAPHORE_MAX];

/* Syscall handlers */
int64_t sys_semaphore_create(ppu_context* ctx);
int64_t sys_semaphore_destroy(ppu_context* ctx);
int64_t sys_semaphore_wait(ppu_context* ctx);
int64_t sys_semaphore_trywait(ppu_context* ctx);
int64_t sys_semaphore_post(ppu_context* ctx);
int64_t sys_semaphore_get_value(ppu_context* ctx);

/* Registration */
void sys_semaphore_init(lv2_syscall_table* tbl);

#ifdef __cplusplus
}
#endif

#endif /* SYS_SEMAPHORE_H */
