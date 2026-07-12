/* ----------------------------------------------------------------------------
Copyright (c) 2025, Microsoft Research, Daan Leijen
This is free software; you can redistribute it and/or modify it under the
terms of the MIT license. A copy of the license can be found in the file
"LICENSE" at the root of this distribution.
-----------------------------------------------------------------------------*/

// Demand-driven background scavenger. Waits on subproc->scavenger_wake (set by
// mi_arena_schedule_purge) and runs _mi_arenas_try_purge when due, so freed
// arena memory returns to the OS without waiting for the next allocation.

#include "mimalloc.h"
#include "mimalloc/internal.h"

#if defined(_WIN32) || defined(__wasi__) || (defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__))

// No scavenger thread on these platforms; purging stays allocation-driven.
void _mi_scavenger_start(void) { }
void _mi_scavenger_stop(void)  { }
void _mi_scavenger_wake(mi_subproc_t* subproc) { MI_UNUSED(subproc); }

#else

#include <pthread.h>
#include <time.h>

#if defined(__linux__)
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

static pthread_t          _mi_scavenger_thread;
static _Atomic(uintptr_t) _mi_scavenger_running;  // 0 = not running, 1 = running

#if defined(__linux__)
static void mi_scavenger_futex_wait(_Atomic(uint32_t)* addr, uint32_t expected, mi_msecs_t timeout_ms) {
  if (timeout_ms > 1000) timeout_ms = 1000;
  if (timeout_ms <= 0)   timeout_ms = 1;
  struct timespec ts;
  ts.tv_sec  = (time_t)(timeout_ms / 1000);
  ts.tv_nsec = (long)((timeout_ms % 1000) * 1000000L);
  syscall(SYS_futex, (uint32_t*)addr, FUTEX_WAIT_PRIVATE, expected, &ts, NULL, 0);
}

static void mi_scavenger_futex_wake(_Atomic(uint32_t)* addr) {
  syscall(SYS_futex, (uint32_t*)addr, FUTEX_WAKE_PRIVATE, 1, NULL, NULL, 0);
}
#else
// Fallback: timed polling only (no explicit wake).
static void mi_scavenger_futex_wait(_Atomic(uint32_t)* addr, uint32_t expected, mi_msecs_t timeout_ms) {
  MI_UNUSED(addr); MI_UNUSED(expected);
  if (timeout_ms > 1000) timeout_ms = 1000;
  if (timeout_ms <= 0)   timeout_ms = 1;
  struct timespec ts;
  ts.tv_sec  = (time_t)(timeout_ms / 1000);
  ts.tv_nsec = (long)((timeout_ms % 1000) * 1000000L);
  nanosleep(&ts, NULL);
}

static void mi_scavenger_futex_wake(_Atomic(uint32_t)* addr) {
  MI_UNUSED(addr);
}
#endif

static void* mi_scavenger_thread_main(void* arg) {
  MI_UNUSED(arg);
  mi_subproc_t* const subproc = _mi_subproc();
  while (mi_atomic_load_acquire(&_mi_scavenger_running) != 0) {
    const mi_msecs_t expire = mi_atomic_loadi64_acquire(&subproc->purge_expire);
    if (mi_atomic_load_acquire(&subproc->scavenger_wake) == 0) {
      mi_msecs_t timeout_ms;
      if (expire == 0) {
        timeout_ms = 1000;  // idle: wait for a signal (bounded so stop() takes effect)
      }
      else {
        const mi_msecs_t now = _mi_clock_now();
        timeout_ms = (expire > now ? expire - now : 0);
      }
      if (timeout_ms > 0) {
        mi_scavenger_futex_wait(&subproc->scavenger_wake, 0, timeout_ms);
      }
    }
    if (mi_atomic_load_acquire(&_mi_scavenger_running) == 0) break;
    mi_atomic_store_release(&subproc->scavenger_wake, (uint32_t)0);
    _mi_arenas_try_purge(false /* force */, true /* visit_all */, subproc, 0 /* tseq */);
  }
  return NULL;
}

void _mi_scavenger_wake(mi_subproc_t* subproc) {
  if (mi_atomic_load_relaxed(&_mi_scavenger_running) == 0) return;
  mi_atomic_store_release(&subproc->scavenger_wake, (uint32_t)1);
  mi_scavenger_futex_wake(&subproc->scavenger_wake);
}

void _mi_scavenger_start(void) {
  if (mi_atomic_load_acquire(&_mi_scavenger_running) != 0) return;
  if (!mi_option_is_enabled(mi_option_scavenger)) return;
  if (mi_option_get(mi_option_purge_delay) <= 0) return;
  mi_atomic_store_release(&_mi_scavenger_running, (uintptr_t)1);
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  if (pthread_create(&_mi_scavenger_thread, &attr, &mi_scavenger_thread_main, NULL) != 0) {
    mi_atomic_store_release(&_mi_scavenger_running, (uintptr_t)0);
  }
  pthread_attr_destroy(&attr);
}

void _mi_scavenger_stop(void) {
  if (mi_atomic_load_acquire(&_mi_scavenger_running) == 0) return;
  mi_atomic_store_release(&_mi_scavenger_running, (uintptr_t)0);
  mi_subproc_t* const subproc = _mi_subproc_main();
  mi_atomic_store_release(&subproc->scavenger_wake, (uint32_t)1);
  mi_scavenger_futex_wake(&subproc->scavenger_wake);
}

#endif
