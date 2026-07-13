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

#if defined(__wasi__) || (defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__))

// No scavenger thread on these platforms; purging stays allocation-driven.
void _mi_scavenger_start(void) { }
void _mi_scavenger_stop(void)  { }
void _mi_scavenger_wake(mi_subproc_t* subproc) { MI_UNUSED(subproc); }
bool _mi_scavenger_is_running(void) { return false; }

#else

#include <errno.h>

static _Atomic(uintptr_t) _mi_scavenger_running;  // 0 = not running, 1 = running

// -----------------------------------------------------------------------------
// Wait/wake on subproc->scavenger_wake (a uint32_t futex word).
//
//   mi_scav_wait(addr, timeout_ms) : block while *addr == 0, up to timeout_ms.
//   mi_scav_wake_one(addr)         : wake one waiter on addr.
//
// The thread loop re-reads scavenger_wake and purge_expire after every return,
// so spurious wakeups are fine; EINTR is retried in-place so signals do not
// turn the wait into a busy spin.
// -----------------------------------------------------------------------------

#if defined(__linux__)

#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>

static void mi_scav_wait(_Atomic(uint32_t)* addr, mi_msecs_t timeout_ms) {
  if (timeout_ms <= 0) timeout_ms = 1;
  struct timespec ts;
  ts.tv_sec  = (time_t)(timeout_ms / 1000);
  ts.tv_nsec = (long)((timeout_ms % 1000) * 1000000L);
  while (mi_atomic_load_acquire(addr) == 0) {
    const long rc = syscall(SYS_futex, (uint32_t*)addr, FUTEX_WAIT_PRIVATE, (uint32_t)0, &ts, NULL, 0);
    if (rc == 0) return;                 // woken by FUTEX_WAKE
    if (errno == ETIMEDOUT) return;
    if (errno == EAGAIN) return;         // *addr != 0 at kernel check; caller re-reads
    // EINTR (or anything else unexpected): retry
  }
}

static void mi_scav_wake_one(_Atomic(uint32_t)* addr) {
  syscall(SYS_futex, (uint32_t*)addr, FUTEX_WAKE_PRIVATE, 1, NULL, NULL, 0);
}

#elif defined(__APPLE__)

// Darwin's private wait-on-address syscall. The public os_sync_wait_on_address
// is macOS 14.4+; __ulock_* has been stable since 10.12 and is what libc++ and
// Rust std park on.
#if defined(__cplusplus)
extern "C" {
#endif
extern int __ulock_wait(uint32_t operation, void* addr, uint64_t value, uint32_t timeout_us);
extern int __ulock_wake(uint32_t operation, void* addr, uint64_t wake_value);
#if defined(__cplusplus)
}
#endif
#define MI_UL_COMPARE_AND_WAIT  1
#define MI_ULF_NO_ERRNO         0x01000000

static void mi_scav_wait(_Atomic(uint32_t)* addr, mi_msecs_t timeout_ms) {
  if (timeout_ms <= 0) timeout_ms = 1;
  const uint32_t timeout_us = (uint32_t)timeout_ms * 1000u;
  while (mi_atomic_load_acquire(addr) == 0) {
    const int rc = __ulock_wait(MI_UL_COMPARE_AND_WAIT | MI_ULF_NO_ERRNO, (void*)addr, 0, timeout_us);
    if (rc >= 0) return;                 // woken or value already changed
    if (rc == -ETIMEDOUT) return;
    // -EINTR / -EFAULT: retry
  }
}

static void mi_scav_wake_one(_Atomic(uint32_t)* addr) {
  __ulock_wake(MI_UL_COMPARE_AND_WAIT | MI_ULF_NO_ERRNO, (void*)addr, 0);
}

#elif defined(_WIN32)

// WaitOnAddress/WakeByAddressSingle require Windows 8+. windows.h is already
// included via mimalloc/atomic.h; declare here as well (matching the SDK
// signature) so older/MinGW headers that gate on _WIN32_WINNT still resolve.
#if defined(__cplusplus)
extern "C" {
#endif
BOOL WINAPI WaitOnAddress(volatile VOID* Address, PVOID CompareAddress, SIZE_T AddressSize, DWORD dwMilliseconds);
VOID WINAPI WakeByAddressSingle(PVOID Address);
#if defined(__cplusplus)
}
#endif
#if defined(_MSC_VER)
#pragma comment(lib, "synchronization")
#endif

static void mi_scav_wait(_Atomic(uint32_t)* addr, mi_msecs_t timeout_ms) {
  if (timeout_ms <= 0) timeout_ms = 1;
  uint32_t expected = 0;
  while (mi_atomic_load_acquire(addr) == 0) {
    if (!WaitOnAddress((volatile VOID*)addr, &expected, sizeof(uint32_t), (DWORD)timeout_ms)) {
      return;  // timeout (GetLastError() == ERROR_TIMEOUT)
    }
    // woken (possibly spuriously): loop re-checks *addr
  }
}

static void mi_scav_wake_one(_Atomic(uint32_t)* addr) {
  WakeByAddressSingle((PVOID)addr);
}

#else  // generic POSIX (FreeBSD, OpenBSD, etc.)

#include <pthread.h>
#include <time.h>

// One scavenger per process, so a file-static mutex/cond is sufficient and
// avoids bloating mi_subproc_s with platform-conditional fields.
static pthread_mutex_t _mi_scav_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  _mi_scav_cond  = PTHREAD_COND_INITIALIZER;

static void mi_scav_wait(_Atomic(uint32_t)* addr, mi_msecs_t timeout_ms) {
  if (timeout_ms <= 0) timeout_ms = 1;
  struct timespec ts;
  #if defined(CLOCK_REALTIME)
  clock_gettime(CLOCK_REALTIME, &ts);
  #else
  struct timeval tv; gettimeofday(&tv, NULL);
  ts.tv_sec = tv.tv_sec; ts.tv_nsec = tv.tv_usec * 1000L;
  #endif
  ts.tv_sec  += (time_t)(timeout_ms / 1000);
  ts.tv_nsec += (long)((timeout_ms % 1000) * 1000000L);
  if (ts.tv_nsec >= 1000000000L) { ts.tv_sec += 1; ts.tv_nsec -= 1000000000L; }
  pthread_mutex_lock(&_mi_scav_mutex);
  while (mi_atomic_load_acquire(addr) == 0) {
    if (pthread_cond_timedwait(&_mi_scav_cond, &_mi_scav_mutex, &ts) == ETIMEDOUT) break;
  }
  pthread_mutex_unlock(&_mi_scav_mutex);
}

static void mi_scav_wake_one(_Atomic(uint32_t)* addr) {
  MI_UNUSED(addr);
  pthread_mutex_lock(&_mi_scav_mutex);
  pthread_cond_signal(&_mi_scav_cond);
  pthread_mutex_unlock(&_mi_scav_mutex);
}

#endif

// -----------------------------------------------------------------------------
// Scavenger thread body (shared across platforms)
// -----------------------------------------------------------------------------

static void mi_scavenger_run(void) {
  // Use the main subproc directly: this thread never allocates, so don't
  // initialise a theap/tld via _mi_subproc()'s TLS path.
  mi_subproc_t* const subproc = _mi_subproc_main();
  while (mi_atomic_load_acquire(&_mi_scavenger_running) != 0) {
    mi_atomic_store_release(&subproc->scavenger_wake, (uint32_t)0);
    mi_msecs_t expire = mi_atomic_loadi64_acquire(&subproc->purge_expire);
    mi_msecs_t timeout_ms;
    if (expire == 0) {
      // Nothing scheduled: park until woken. The 30s bound is a pure safety
      // net so stop() is guaranteed to take effect and any per-arena expiry
      // that did not propagate to subproc is still eventually purged.
      timeout_ms = 30000;
    }
    else {
      const mi_msecs_t now = _mi_clock_now();
      if (expire > now) {
        timeout_ms = expire - now;
        if (timeout_ms > 30000) timeout_ms = 30000;
      }
      else {
        _mi_arenas_try_purge(false /* force */, true /* visit_all */, subproc, 0 /* tseq */);
        // _mi_arenas_try_purge clears subproc->purge_expire to 0 once every
        // arena is done. If it left the stale past value (some arena's own
        // expire is still in the future), clear it so the next iteration parks
        // on the 30s safety net instead of spinning. CAS so a concurrently
        // scheduled future expire is never clobbered.
        mi_atomic_casi64_strong_acq_rel(&subproc->purge_expire, &expire, (mi_msecs_t)0);
        continue;
      }
    }
    if (mi_atomic_load_acquire(&_mi_scavenger_running) == 0) break;
    mi_scav_wait(&subproc->scavenger_wake, timeout_ms);
  }
}

bool _mi_scavenger_is_running(void) {
  return (mi_atomic_load_relaxed(&_mi_scavenger_running) != 0);
}

void _mi_scavenger_wake(mi_subproc_t* subproc) {
  if (mi_atomic_load_relaxed(&_mi_scavenger_running) == 0) return;
  // Coalesce: only issue the wake syscall on the 0->1 edge. Callers sit on
  // the page-free path and would otherwise turn every arena page free into a
  // syscall on the freeing thread.
  if (mi_atomic_exchange_acq_rel(&subproc->scavenger_wake, (uint32_t)1) == 0) {
    mi_scav_wake_one(&subproc->scavenger_wake);
  }
}

// -----------------------------------------------------------------------------
// Thread lifecycle
// -----------------------------------------------------------------------------

#if defined(_WIN32)

static HANDLE _mi_scavenger_thread;

static DWORD WINAPI mi_scavenger_thread_main(LPVOID arg) {
  MI_UNUSED(arg);
  mi_scavenger_run();
  return 0;
}

void _mi_scavenger_start(void) {
  if (mi_atomic_load_acquire(&_mi_scavenger_running) != 0) return;
  if (!mi_option_is_enabled(mi_option_scavenger)) return;
  if (mi_option_get(mi_option_purge_delay) <= 0) return;
  mi_atomic_store_release(&_mi_scavenger_running, (uintptr_t)1);
  _mi_scavenger_thread = CreateThread(NULL, 0, &mi_scavenger_thread_main, NULL, 0, NULL);
  if (_mi_scavenger_thread == NULL) {
    mi_atomic_store_release(&_mi_scavenger_running, (uintptr_t)0);
  }
}

void _mi_scavenger_stop(void) {
  if (mi_atomic_exchange_acq_rel(&_mi_scavenger_running, (uintptr_t)0) == 0) return;
  mi_subproc_t* const subproc = _mi_subproc_main();
  mi_atomic_store_release(&subproc->scavenger_wake, (uint32_t)1);
  mi_scav_wake_one(&subproc->scavenger_wake);
  if (_mi_scavenger_thread != NULL) {
    WaitForSingleObject(_mi_scavenger_thread, INFINITE);
    CloseHandle(_mi_scavenger_thread);
    _mi_scavenger_thread = NULL;
  }
}

#else  // POSIX

#include <pthread.h>
#include <signal.h>

static pthread_t          _mi_scavenger_thread;
static _Atomic(uintptr_t) _mi_scavenger_joinable;

static void* mi_scavenger_thread_main(void* arg) {
  MI_UNUSED(arg);
  mi_scavenger_run();
  return NULL;
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
  if (pthread_create(&_mi_scavenger_thread, NULL, &mi_scavenger_thread_main, NULL) != 0) {
    mi_atomic_store_release(&_mi_scavenger_running, (uintptr_t)0);
  }
  else {
    mi_atomic_store_release(&_mi_scavenger_joinable, (uintptr_t)1);
  }
  pthread_sigmask(SIG_SETMASK, &old, NULL);
}

void _mi_scavenger_stop(void) {
  if (mi_atomic_exchange_acq_rel(&_mi_scavenger_running, (uintptr_t)0) == 0) return;
  mi_subproc_t* const subproc = _mi_subproc_main();
  mi_atomic_store_release(&subproc->scavenger_wake, (uint32_t)1);
  mi_scav_wake_one(&subproc->scavenger_wake);
  if (mi_atomic_exchange_acq_rel(&_mi_scavenger_joinable, (uintptr_t)0) != 0) {
    pthread_join(_mi_scavenger_thread, NULL);
  }
}

#endif

#endif
