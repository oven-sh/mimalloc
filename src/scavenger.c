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
#include <signal.h>
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
  // Use the main subproc directly: this thread never allocates, so don't
  // initialise a theap/tld via _mi_subproc()'s TLS path.
  mi_subproc_t* const subproc = _mi_subproc_main();
  // Minimum spacing between purge attempts. _mi_arenas_try_purge can leave
  // subproc->purge_expire at a stale past value while per-arena expiries are
  // still in the future; without a floor this loop would spin until they pass.
  const long arena_delay = mi_option_get(mi_option_purge_delay) * mi_option_get(mi_option_arena_purge_mult);
  const mi_msecs_t min_wait_ms = (arena_delay >= 10 ? (mi_msecs_t)(arena_delay / 10) : 1);
  while (mi_atomic_load_acquire(&_mi_scavenger_running) != 0) {
    mi_atomic_store_release(&subproc->scavenger_wake, (uint32_t)0);
    mi_msecs_t expire = mi_atomic_loadi64_acquire(&subproc->purge_expire);
    mi_msecs_t timeout_ms;
    if (expire == 0) {
      // Nothing scheduled: park until woken (bounded so stop() takes effect).
      timeout_ms = 1000;
    }
    else {
      const mi_msecs_t now = _mi_clock_now();
      if (expire <= now) {
        _mi_arenas_try_purge(false /* force */, true /* visit_all */, subproc, 0 /* tseq */);
        expire = mi_atomic_loadi64_acquire(&subproc->purge_expire);
        timeout_ms = (expire == 0 ? 1000 : (expire > now ? expire - now : min_wait_ms));
      }
      else {
        timeout_ms = expire - now;
      }
    }
    if (mi_atomic_load_acquire(&_mi_scavenger_running) == 0) break;
    // Always wait: futex_wait(expected=0) returns immediately if a wake was
    // already posted, and the timeout floor in the helper prevents a hot spin.
    mi_scavenger_futex_wait(&subproc->scavenger_wake, 0, timeout_ms);
  }
  return NULL;
}

void _mi_scavenger_wake(mi_subproc_t* subproc) {
  if (mi_atomic_load_relaxed(&_mi_scavenger_running) == 0) return;
  // Coalesce: only issue the futex syscall on the 0->1 edge. Callers sit on
  // the page-free path and would otherwise turn every arena page free into a
  // syscall on the freeing thread.
  if (mi_atomic_exchange_acq_rel(&subproc->scavenger_wake, (uint32_t)1) == 0) {
    mi_scavenger_futex_wake(&subproc->scavenger_wake);
  }
}

void _mi_scavenger_start(void) {
  if (mi_atomic_load_acquire(&_mi_scavenger_running) != 0) return;
  if (!mi_option_is_enabled(mi_option_scavenger)) return;
  if (mi_option_get(mi_option_purge_delay) <= 0) return;
  mi_atomic_store_release(&_mi_scavenger_running, (uintptr_t)1);
  // Block all signals on the scavenger thread. It runs before the host has set
  // up its own signal masking, and a thread that leaves (e.g.) SIGCHLD
  // unblocked will have process-directed signals dispatched to it and silently
  // discarded, starving signalfd/kqueue consumers. sigfillset on glibc/musl
  // already excludes the libc-internal realtime signals used for setxid/cancel.
  sigset_t all, old;
  sigfillset(&all);
  pthread_sigmask(SIG_SETMASK, &all, &old);
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  if (pthread_create(&_mi_scavenger_thread, &attr, &mi_scavenger_thread_main, NULL) != 0) {
    mi_atomic_store_release(&_mi_scavenger_running, (uintptr_t)0);
  }
  pthread_attr_destroy(&attr);
  pthread_sigmask(SIG_SETMASK, &old, NULL);
}

void _mi_scavenger_stop(void) {
  if (mi_atomic_load_acquire(&_mi_scavenger_running) == 0) return;
  mi_atomic_store_release(&_mi_scavenger_running, (uintptr_t)0);
  mi_subproc_t* const subproc = _mi_subproc_main();
  mi_atomic_store_release(&subproc->scavenger_wake, (uint32_t)1);
  mi_scavenger_futex_wake(&subproc->scavenger_wake);
}

#endif
