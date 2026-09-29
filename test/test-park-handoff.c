/* ----------------------------------------------------------------------------
Copyright (c) 2026, Microsoft Research, Daan Leijen
This is free software; you can redistribute it and/or modify it under the
terms of the MIT license. A copy of the license can be found in the file
"LICENSE" at the root of this distribution.
-----------------------------------------------------------------------------*/

// `mi_on_thread_idle_start`/`mi_on_thread_idle_end`: a thread that is about to block hands its
// theaps to the scavenger, which sweeps them while it is in the kernel.
//
// Holes need scattered survivors pinning a page, so every case here keeps one block every
// KEEP_EVERY: two whole OS pages of blocks between survivors, whatever the OS page size (4KB or
// the 16KB of Apple Silicon), so free runs cover whole OS pages inside a page that is still
// used. Freeing a contiguous run instead would empty whole mimalloc pages, which go back through
// the arena and never exercise hole punching at all.

#if defined(_WIN32)
#include <stdio.h>
int main(void) { printf("test-park-handoff: skipped on Windows (uses pthreads/fork)\n"); return 0; }
#else

#include "mimalloc.h"
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>
#include <stdatomic.h>

static int failures = 0;

static void check(const char* name, bool ok) {
  fprintf(stderr, "test: %s...  %s\n", name, ok ? "ok." : "FAILED");
  if (!ok) failures++;
}

#if (defined(MI_GUARDED) && MI_GUARDED>0)
#define LIVE   (2000)     // every sampled allocation gets a guard page (its own mapping): stay under vm.max_map_count
#else
#define LIVE   (20000)
#endif
#define BSZ    (512)
// two OS pages of BSZ blocks between survivors (+2 for the margin), as in test-purge-holes.c
static size_t keep_every(void) {
  return ((((size_t)2 * (size_t)sysconf(_SC_PAGESIZE)) + BSZ - 1) / BSZ) + 2;
}

// A per-(block,byte) pattern so that a purge, a free-list rewrite, or a hole punched one OS page
// too far in either direction shows up as a byte mismatch and not just as a crash.
static uint8_t pattern_byte(size_t id, size_t off) {
  return (uint8_t)((id * 131u) ^ (off * 7u) ^ (off >> 8));
}
static void pattern_fill(void* p, size_t size, size_t id) {
  uint8_t* b = (uint8_t*)p;
  for (size_t i = 0; i < size; i++) { b[i] = pattern_byte(id, i); }
}
// returns the offset of the first corrupt byte, or `size` when intact
static size_t pattern_check(const void* p, size_t size, size_t id) {
  const uint8_t* b = (const uint8_t*)p;
  for (size_t i = 0; i < size; i++) { if (b[i] != pattern_byte(id, i)) return i; }
  return size;
}

static void churn(void** p) {
  const size_t ke = keep_every();
  for (int i = 0; i < LIVE; i++) { if (p[i] == NULL) { p[i] = mi_malloc(BSZ); memset(p[i], 1, BSZ); } }
  for (int i = 0; i < LIVE; i++) { if (((size_t)i % ke) != 0 && p[i] != NULL) { mi_free(p[i]); p[i] = NULL; } }
}

// churn, but with a checkable pattern in every survivor
static void churn_pattern(void** p) {
  const size_t ke = keep_every();
  for (int i = 0; i < LIVE; i++) {
    if (p[i] == NULL) { p[i] = mi_malloc(BSZ); pattern_fill(p[i], BSZ, (size_t)i); }
  }
  for (int i = 0; i < LIVE; i++) { if (((size_t)i % ke) != 0 && p[i] != NULL) { mi_free(p[i]); p[i] = NULL; } }
}

// index of the first survivor with a corrupt byte, or -1 when every survivor is intact
static long first_corrupt_survivor(void** p) {
  for (int i = 0; i < LIVE; i++) {
    if (p[i] != NULL && pattern_check(p[i], BSZ, (size_t)i) != BSZ) return (long)i;
  }
  return -1;
}

static size_t discards(void) {
  mi_purge_holes_stats_t h; mi_purge_holes_stats_get(&h); return h.discard_calls;
}

// wait (bounded) for the handoff to have actually done a discard, standing in for a syscall
static bool wait_for_discard_after(size_t before) {
  for (int i = 0; i < 20000; i++) { if (discards() > before) return true; usleep(100); }
  return discards() > before;
}

// ---------------------------------------------------------------------------
// The scavenger thread exists only once there is something for it to do (a second thread, or a
// park), and it must not block the signals a fault on it raises, or a crash during a sweep it does
// for a parked thread ends the process without the host's crash report. Linux only: both are read
// from /proc.
// ---------------------------------------------------------------------------
#if defined(__linux__)
#include <dirent.h>
#include <signal.h>
// the blocked-signal mask of the thread named "mi-scavenger" (0 if there is none yet)
static unsigned long long scavenger_sigblk(int* count) {
  unsigned long long mask = 0;
  *count = 0;
  DIR* d = opendir("/proc/self/task");
  if (d == NULL) return 0;
  struct dirent* e;
  while ((e = readdir(d)) != NULL) {
    if (e->d_name[0] == '.') continue;
    char path[300]; char line[256];
    snprintf(path, sizeof(path), "/proc/self/task/%s/comm", e->d_name);
    FILE* f = fopen(path, "r");
    if (f == NULL) continue;
    const bool is_scav = (fgets(line, sizeof(line), f) != NULL && strncmp(line, "mi-scavenger", 12) == 0);
    fclose(f);
    if (!is_scav) continue;
    (*count)++;
    snprintf(path, sizeof(path), "/proc/self/task/%s/status", e->d_name);
    f = fopen(path, "r");
    if (f == NULL) continue;
    while (fgets(line, sizeof(line), f) != NULL) {
      if (strncmp(line, "SigBlk:", 7) == 0) { mask = strtoull(line + 7, NULL, 16); break; }
    }
    fclose(f);
  }
  closedir(d);
  return mask;
}
static void test_lazy_start_and_signals(void) {
  if (!mi_option_is_enabled(mi_option_scavenger)) { fprintf(stderr, "test: scavenger lazy start...  skipped (scavenger disabled)\n"); return; }
  void* q = mi_malloc(100); mi_free(q);
  int n = 0;
  scavenger_sigblk(&n);
  check("no scavenger thread for a process that only ever had one thread", n == 0);
  if (mi_on_thread_idle_start()) { mi_on_thread_idle_end(); }
  // it names itself and sets its mask first thing; wait for that rather than assume it
  unsigned long long blk = 0;
  for (int i = 0; i < 20000 && (blk = scavenger_sigblk(&n)) == 0; i++) { usleep(100); }
  check("the first park starts it", n == 1);
  const unsigned long long segv = 1ULL << (SIGSEGV - 1), bus = 1ULL << (SIGBUS - 1), term = 1ULL << (SIGTERM - 1);
  check("the scavenger blocks process-directed signals", (blk & term) != 0);
  check("but not the ones a fault on it raises", (blk & (segv | bus)) == 0);
}
#else
static void test_lazy_start_and_signals(void) { fprintf(stderr, "test: scavenger lazy start / signal mask...  skipped (needs /proc)\n"); }
#endif

// ---------------------------------------------------------------------------
// The handoff does the same work as the inline sweep -- and when there is nobody to hand off to
// it says so rather than sweeping inline behind the caller's back.
// ---------------------------------------------------------------------------
static void test_handoff_sweeps(void) {
  void** p = (void**)calloc(LIVE, sizeof(void*));
  if (p == NULL) return;
  churn(p);
  const size_t before = discards();
  const bool parked = mi_on_thread_idle_start();
  if (parked) {
    // Stand in for a blocking syscall: the sweep is asynchronous, so wait for it rather than
    // assuming it happened. Bounded so a broken handoff fails instead of hanging.
    for (int i = 0; i < 20000 && discards() == before; i++) { usleep(100); }
    mi_on_thread_idle_end();
    check("handoff sweeps the parked thread's heaps", discards() > before);
  }
  else {
    // No scavenger: `_start` must be a no-op, NOT an inline sweep. A caller parks far more often
    // than it is idle, so sweeping here is the between-task sweep it is trying to avoid.
    check("_start does not sweep inline when it cannot hand off", discards() == before);
    mi_on_thread_idle();   // what such a caller does instead, when it decides it is idle
    check("the caller can still sweep for itself", discards() > before);
  }
  for (int i = 0; i < LIVE; i++) { if (p[i] != NULL) mi_free(p[i]); }
  free(p);
}

// ---------------------------------------------------------------------------
// _end with no _start, and _start twice, must not corrupt the park state.
// ---------------------------------------------------------------------------
static void test_unbalanced(void) {
  mi_on_thread_idle_end();          // no matching start
  (void)mi_on_thread_idle_start();
  (void)mi_on_thread_idle_start();  // twice
  mi_on_thread_idle_end();
  mi_on_thread_idle_end();          // and one too many
  void* q = mi_malloc(64);     // the thread must still be able to allocate
  check("unbalanced start/end leaves the thread usable", q != NULL);
  mi_free(q);
}

// ---------------------------------------------------------------------------
// A thread that parks and then EXITS without ever calling `mi_on_thread_idle_end`. `epoll_wait` is
// a pthread_cancel cancellation point, and pthread_exit and unwinding leave the same way. Teardown
// then frees the tld -- and destroys `theaps_lock` -- which the scavenger may be walking right now.
// Without `_mi_park_leave` in `_mi_thread_done` this is a use-after-free: it reports races under
// the thread sanitizer and trips the assertion in `mi_tld_unregister`.
// ---------------------------------------------------------------------------
static void* park_then_exit(void* arg) {
  (void)arg;
  void** p = (void**)calloc(LIVE, sizeof(void*));
  if (p == NULL) return NULL;
  churn(p);
  free(p);                     // the mi blocks stay allocated on purpose: the pages must stay live
  (void)mi_on_thread_idle_start();
  usleep(200);                 // let the scavenger claim and start sweeping
  pthread_exit(NULL);          // ...and leave without _end
}

static void* park_then_cancel(void* arg) {
  (void)arg;
  void** p = (void**)calloc(LIVE, sizeof(void*));
  if (p == NULL) return NULL;
  churn(p);
  free(p);
  (void)mi_on_thread_idle_start();
  #if defined(MI_TSAN)
  // ThreadSanitizer stops modelling the synchronization of a thread that is cancelled inside one of its
  // blocking interceptors (`usleep`: the interceptor scope is never closed, and the mutexes and atomics of
  // the thread's destructors are then ignored), which reports everything the teardown does as a race.
  for (;;) { pthread_testcancel(); sched_yield(); }
  #else
  for (;;) { pthread_testcancel(); usleep(50); }   // cancelled mid-park, as at a blocking syscall
  #endif
}

static void test_park_then_exit(void) {
  enum { THREADS = 8, ROUNDS = 10 };
  for (int r = 0; r < ROUNDS; r++) {
    pthread_t t[THREADS];
    for (int i = 0; i < THREADS; i++) {
      if (pthread_create(&t[i], NULL, ((i % 2) != 0 ? &park_then_exit : &park_then_cancel), NULL) != 0) return;
    }
    usleep(500);
    for (int i = 0; i < THREADS; i++) { if ((i % 2) == 0) pthread_cancel(t[i]); }
    for (int i = 0; i < THREADS; i++) { pthread_join(t[i], NULL); }
  }
  check("a thread may exit while still parked", true);   // reaching here without a crash IS the test
}

// ---------------------------------------------------------------------------
// Many threads parking and waking at randomized moments, so the reclaim lands both before and in
// the middle of a sweep.
// ---------------------------------------------------------------------------
static int stress_corrupt = 0;   // set by a worker on the first corrupt survivor (racy write is fine: it only latches)

static void* park_stress(void* arg) {
  unsigned seed = (unsigned)(uintptr_t)arg * 2654435761u;
  void** p = (void**)calloc(LIVE, sizeof(void*));
  if (p == NULL) return NULL;
  for (int r = 0; r < 100; r++) {
    churn_pattern(p);
    (void)mi_on_thread_idle_start();
    if ((rand_r(&seed) % 4) == 0) { usleep(rand_r(&seed) % 200); }
    mi_on_thread_idle_end();
    void* q = mi_malloc(64); mi_free(q);   // allocate immediately on wake: must be safe
    // a wake that raced a sweep (an aborted reclaim) must still leave every survivor intact
    if (first_corrupt_survivor(p) >= 0) { stress_corrupt = 1; break; }
  }
  for (int i = 0; i < LIVE; i++) { if (p[i] != NULL) mi_free(p[i]); }
  free(p);
  return NULL;
}

static void test_park_stress(void) {
  enum { THREADS = 4 };
  pthread_t t[THREADS];
  stress_corrupt = 0;
  for (long i = 0; i < THREADS; i++) {
    if (pthread_create(&t[i], NULL, &park_stress, (void*)i) != 0) return;
  }
  for (int i = 0; i < THREADS; i++) { pthread_join(t[i], NULL); }
  check("concurrent park/wake keeps every survivor intact", stress_corrupt == 0);
}

// ---------------------------------------------------------------------------
// A handoff sweep does the same work as an inline one, so the survivors must come back byte-for-
// byte intact -- a free-list rewrite or a hole punched over a live block corrupts, not crashes.
// This is the check that turns every other test's "did not crash" into "the heap is intact".
// ---------------------------------------------------------------------------
static void test_survivors_intact(void) {
  void** p = (void**)calloc(LIVE, sizeof(void*));
  if (p == NULL) return;
  churn_pattern(p);
  const size_t before = discards();
  const bool parked = mi_on_thread_idle_start();
  if (parked) { wait_for_discard_after(before); }
  mi_on_thread_idle_end();
  if (!parked) { mi_on_thread_idle(); }   // no scavenger: do the sweep ourselves so it is not vacuous
  // a sweep that never ran would leave the survivors trivially intact -- assert it did run
  check("a sweep ran before checking survivors", discards() > before);
  const long bad = first_corrupt_survivor(p);
  if (bad >= 0) { fprintf(stderr, "\n  CORRUPT survivor block=%ld byte=%zu\n", bad, pattern_check(p[bad], BSZ, (size_t)bad)); }
  check("survivor bytes intact after a sweep", bad < 0);
  // and the swept pages must still allocate correctly afterwards
  for (int i = 0; i < LIVE; i++) { if (p[i] == NULL) { p[i] = mi_malloc(BSZ); pattern_fill(p[i], BSZ, (size_t)i); } }
  check("refill after sweep is intact", first_corrupt_survivor(p) < 0);
  for (int i = 0; i < LIVE; i++) { if (p[i] != NULL) mi_free(p[i]); }
  free(p);
}

// ---------------------------------------------------------------------------
// A THIRD thread frees a parked thread's blocks while the scavenger sweeps them: cross-thread
// frees land on the page's xthread list, which the sweep folds. Every block must end up freed
// exactly once and no survivor may be corrupted.
// ---------------------------------------------------------------------------
typedef struct third_free_args_s {
  void** p;
  atomic_int go;
  atomic_int done;
} third_free_args_t;

static void* third_thread_freer(void* varg) {
  third_free_args_t* a = (third_free_args_t*)varg;
  while (!atomic_load(&a->go)) { usleep(50); }
  // free every survivor at an even index; odd survivors stay live for the owner to verify
  for (int i = 0; i < LIVE; i++) {
    if ((i % 2) == 0 && a->p[i] != NULL) { mi_free(a->p[i]); a->p[i] = NULL; }
  }
  atomic_store(&a->done, 1);
  return NULL;
}

static void test_third_thread_frees_during_sweep(void) {
  enum { ROUNDS = 20 };
  bool intact = true;
  for (int r = 0; r < ROUNDS && intact; r++) {
    void** p = (void**)calloc(LIVE, sizeof(void*));
    if (p == NULL) return;
    churn_pattern(p);
    third_free_args_t args = { .p = p };
    atomic_init(&args.go, 0); atomic_init(&args.done, 0);
    pthread_t t;
    if (pthread_create(&t, NULL, &third_thread_freer, &args) != 0) { free(p); return; }
    const bool parked = mi_on_thread_idle_start();
    atomic_store(&args.go, 1);                  // the frees race the (possibly running) sweep
    while (!atomic_load(&args.done)) { usleep(50); }
    mi_on_thread_idle_end();
    if (!parked) { mi_on_thread_idle(); }
    pthread_join(t, NULL);
    // the odd survivors are still live and must be intact
    for (int i = 1; i < LIVE; i += 2) {
      if (p[i] != NULL && pattern_check(p[i], BSZ, (size_t)i) != BSZ) { intact = false; break; }
    }
    for (int i = 0; i < LIVE; i++) { if (p[i] != NULL) mi_free(p[i]); }
    free(p);
  }
  check("third-thread frees during a sweep keep survivors intact", intact);
}

// ---------------------------------------------------------------------------
// A single thread parks repeatedly: each park with fresh holes must be swept within a deadline far
// under the scavenger's 30s safety timeout. Sweeps of one thread are rate-limited to
// `purge_holes_min_interval` (100ms by default), so space the parks past it (the in-window case
// is `test_park_inside_window_gets_swept`); the `-eager` ctest variant sets the interval to 0.
// ---------------------------------------------------------------------------
static void test_parks_get_swept(void) {
  enum { ROUNDS = 20 };
  const long interval_ms = mi_option_get(mi_option_purge_holes_min_interval);
  void** p = (void**)calloc(LIVE, sizeof(void*));
  if (p == NULL) return;
  int missed = 0;
  int handed_off = 0;
  for (int r = 0; r < ROUNDS; r++) {
    churn(p);   // fresh holes to punch, so a swept park is observable
    if (interval_ms > 0) { usleep((useconds_t)(interval_ms * 1000 + 5000)); }   // clear the rate window
    const size_t before = discards();
    const bool parked = mi_on_thread_idle_start();
    if (parked) {
      handed_off++;
      bool swept = false;
      for (int i = 0; i < 3000 && !swept; i++) { swept = (discards() > before); if (!swept) usleep(1000); }
      if (!swept) missed++;
    }
    mi_on_thread_idle_end();
    for (int i = 0; i < LIVE; i++) { if (p[i] == NULL) { p[i] = mi_malloc(BSZ); memset(p[i], 3, BSZ); } }
  }
  for (int i = 0; i < LIVE; i++) { if (p[i] != NULL) mi_free(p[i]); }
  free(p);
  fprintf(stderr, "  parks handed off: %d, unswept: %d (min_interval=%ldms)\n", handed_off, missed, interval_ms);
  check("every spaced park gets swept promptly", missed == 0);
}

// ---------------------------------------------------------------------------
// The common idle shape: swept, woken briefly by a timer, parked again inside `min_interval` --
// this time for long. That second park is passed over when it starts, and must be swept once the
// window ends rather than at the scavenger's next unrelated wake (up to its 30s safety timeout).
// ---------------------------------------------------------------------------
static void test_park_inside_window_gets_swept(void) {
  const long interval_ms = mi_option_get(mi_option_purge_holes_min_interval);
  if (interval_ms <= 0) return;   // no window (the `-eager` variant)
  void** p = (void**)calloc(LIVE, sizeof(void*));
  if (p == NULL) return;
  // first park: spaced, so it is swept and stamps `holes_sweep_last`
  churn(p);
  usleep((useconds_t)(interval_ms * 1000 + 5000));
  size_t before = discards();
  bool parked = mi_on_thread_idle_start();
  bool first_swept = parked && wait_for_discard_after(before);
  mi_on_thread_idle_end();
  if (!parked) { for (int i = 0; i < LIVE; i++) { mi_free(p[i]); } free(p); return; }   // nobody to hand off to (no scavenger)
  if (!first_swept) { free(p); check("park inside the rate window is swept when the window ends", false); return; }
  // second park: straight away, inside the window, with fresh holes
  for (int i = 0; i < LIVE; i++) { if (p[i] == NULL) { p[i] = mi_malloc(BSZ); memset(p[i], 3, BSZ); } }
  churn(p);
  before = discards();
  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  parked = mi_on_thread_idle_start();
  long waited_ms = -1;
  if (parked) {
    const long deadline_ms = interval_ms * 10 + 1000;   // far under 30s, generous over `interval_ms`
    for (;;) {
      clock_gettime(CLOCK_MONOTONIC, &t1);
      const long elapsed = (long)(t1.tv_sec - t0.tv_sec) * 1000 + (long)(t1.tv_nsec - t0.tv_nsec) / 1000000;
      if (discards() > before) { waited_ms = elapsed; break; }
      if (elapsed > deadline_ms) break;
      usleep(1000);
    }
  }
  mi_on_thread_idle_end();
  for (int i = 0; i < LIVE; i++) { if (p[i] != NULL) mi_free(p[i]); }
  free(p);
  fprintf(stderr, "  in-window park swept after %ldms (min_interval=%ldms)\n", waited_ms, interval_ms);
  check("park inside the rate window is swept when the window ends", parked && waited_ms >= 0);
}

// ---------------------------------------------------------------------------
// What the handoff costs and what it gets done is counted (`mi_purge_holes_stats_t`): the wake
// syscalls that parks issued, the turns of the scavenger's loop, and its sweeps of parked threads.
// ---------------------------------------------------------------------------
typedef struct handoff_counts_s { size_t wakes, turns, sweeps; } handoff_counts_t;

static handoff_counts_t handoff_counts(void) {
  mi_purge_holes_stats_t h; mi_purge_holes_stats_get(&h);
  handoff_counts_t c = { h.park_wakes, h.scavenger_turns, h.parked_sweeps };
  return c;
}

// wait (bounded) for the scavenger to have swept a parked thread since `before`
static bool wait_for_sweep_after(size_t before, long deadline_ms) {
  for (long i = 0; i < deadline_ms * 10; i++) { if (handoff_counts().sweeps > before) return true; usleep(100); }
  return handoff_counts().sweeps > before;
}

static void test_handoff_counters(void) {
  const long interval_ms = mi_option_get(mi_option_purge_holes_min_interval);
  const handoff_counts_t start = handoff_counts();
  if (!mi_on_thread_idle_start()) {
    const handoff_counts_t end = handoff_counts();
    check("nothing is counted where nothing is handed off", end.wakes == start.wakes && end.turns == start.turns && end.sweeps == start.sweeps);
    return;
  }
  mi_on_thread_idle_end();
  // A park that finds the scavenger asleep wakes it, and a park that is due is swept. The wake of a park is
  // not counted if another wake was pending (a free that scheduled a purge): go again then.
  bool woke = false, turned = false, swept = false;
  for (int round = 0; round < 5 && !(woke && turned && swept); round++) {
    usleep((useconds_t)((interval_ms > 0 ? interval_ms : 1) * 1000 + 20000));   // past the rate window, and the scavenger is back in its wait
    const handoff_counts_t before = handoff_counts();
    if (!mi_on_thread_idle_start()) break;
    const bool swept_now = wait_for_sweep_after(before.sweeps, 3000);
    mi_on_thread_idle_end();
    const handoff_counts_t after = handoff_counts();
    woke   = woke   || (after.wakes > before.wakes);
    turned = turned || (after.turns > before.turns);
    swept  = swept  || swept_now;
  }
  check("a park that wakes the scavenger counts a wake", woke);
  check("the scavenger counts its turns", turned);
  check("a sweep of a parked thread is counted", swept);
}

// ---------------------------------------------------------------------------
// The scavenger clears its wake word BEFORE it walks the parked threads (`mi_scavenger_run`), so a park that
// comes after the walk and before the wait is not lost: its wake is still pending when the wait begins.
// Hold the scavenger at exactly that point (a hook that only a debug build has) and park.
// A wake that is lost only shows if nothing else ends the wait: the scavenger says for how long it is going
// to wait, and while that is less than the deadline (a purge is scheduled) the case says nothing: go again.
// ---------------------------------------------------------------------------
#if MI_DEBUG > 0
extern _Atomic(uintptr_t) mi_debug_stall_in_scavenger_wait;
extern _Atomic(uintptr_t) mi_debug_scavenger_wait_msecs;

// wait (bounded) for a thread to be held at a stall point that was set to 1
static bool wait_for_stall(_Atomic(uintptr_t)* stall, long deadline_ms) {
  for (long i = 0; i < deadline_ms * 10; i++) { if (atomic_load(stall) == 2) return true; usleep(100); }
  return atomic_load(stall) == 2;
}

static void test_park_between_walk_and_wait(void) {
  const long interval_ms = mi_option_get(mi_option_purge_holes_min_interval);
  const long deadline_ms = interval_ms * 10 + 1000;
  if (!mi_on_thread_idle_start()) { fprintf(stderr, "test: park between the walk and the wait...  skipped (nobody to hand off to)\n"); return; }
  mi_on_thread_idle_end();
  bool held = false, told = false, swept = false;
  for (int round = 0; round < 20 && !told; round++) {
    usleep(20000);   // the scavenger is back in its wait
    atomic_store(&mi_debug_stall_in_scavenger_wait, (uintptr_t)1);
    if (mi_on_thread_idle_start()) { mi_on_thread_idle_end(); }   // a park to have it take a turn
    held = wait_for_stall(&mi_debug_stall_in_scavenger_wait, 5000);
    if (!held) break;
    const long wait_ms = (long)atomic_load(&mi_debug_scavenger_wait_msecs);
    if (wait_ms < 2 * deadline_ms) {
      atomic_store(&mi_debug_stall_in_scavenger_wait, (uintptr_t)0);
      usleep((useconds_t)((wait_ms + 20) * 1000));   // let what is scheduled pass
      continue;
    }
    told = true;
    const size_t before = handoff_counts().sweeps;
    const bool parked = mi_on_thread_idle_start();   // the scavenger is past its walk: it cannot have seen this park
    atomic_store(&mi_debug_stall_in_scavenger_wait, (uintptr_t)0);
    // (inside the rate window if a park above was swept: then the sweep comes when the window ends)
    swept = parked && wait_for_sweep_after(before, deadline_ms);
    mi_on_thread_idle_end();
  }
  atomic_store(&mi_debug_stall_in_scavenger_wait, (uintptr_t)0);
  check("the scavenger was held between its walk and its wait", held);
  if (held && !told) { fprintf(stderr, "test: a park between the walk and the wait of the scavenger is swept...  skipped (the scavenger had something scheduled each time)\n"); return; }
  check("a park between the walk and the wait of the scavenger is swept", swept);
}
#else
static void test_park_between_walk_and_wait(void) { fprintf(stderr, "test: park between the walk and the wait...  skipped (needs MI_DEBUG>0)\n"); }
#endif

// ---------------------------------------------------------------------------
// The schedule of the scavenger. A park inside the rate window of its thread is passed over, and from then on
// the scavenger comes for that thread by itself when the window ends (`park_paced`): until then the parks of the
// thread wake nobody, and none of them is lost to that.
// ---------------------------------------------------------------------------
static long now_ms(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (long)t.tv_sec * 1000 + (long)(t.tv_nsec / 1000000);
}

// A thread that parks all the time (an event loop that is busy) wakes the scavenger a few times for each
// window, not once for each park, and is swept as often as it was when each park woke it.
// The scavenger has to see a park of 1 ms to put the thread on its schedule. On a machine that is busy with
// something else for a moment it comes too late for that, and the next park wakes it again: go again then.
static void test_frequent_parks_wake_seldom(void) {
  enum { PARKS = 1000, ATTEMPTS = 3 };
  const long interval_ms = mi_option_get(mi_option_purge_holes_min_interval);
  if (!mi_on_thread_idle_start()) { fprintf(stderr, "test: parks inside the rate window wake nobody...  skipped (nobody to hand off to)\n"); return; }
  mi_on_thread_idle_end();
  bool seldom = false, swept = false, slow = false;
  for (int attempt = 0; attempt < ATTEMPTS && !(seldom && swept); attempt++) {
    const handoff_counts_t before = handoff_counts();
    const long t0 = now_ms();
    for (int i = 0; i < PARKS; i++) {
      if (mi_on_thread_idle_start()) { usleep(1000); mi_on_thread_idle_end(); }
    }
    const long elapsed_ms = now_ms() - t0;
    const handoff_counts_t after = handoff_counts();
    const size_t wakes = after.wakes - before.wakes, turns = after.turns - before.turns, sweeps = after.sweeps - before.sweeps;
    fprintf(stderr, "  %d parks of 1 ms in %ld ms: %zu wakes, %zu turns of the scavenger, %zu sweeps (min_interval=%ldms)\n", PARKS, elapsed_ms, wakes, turns, sweeps, interval_ms);
    if (interval_ms <= 0) {   // no window (the `-eager` variant): nothing is ever on the schedule
      seldom = true;
      swept = (sweeps >= (size_t)PARKS / 2);
      continue;
    }
    // A window takes a wake for its sweep and one to get on the schedule for the next: twice as many for each window
    // are still far fewer than one for each park, unless the machine is so slow that a window has but a few parks.
    const size_t windows = (size_t)(elapsed_ms / interval_ms) + 1;
    slow = (8 * windows > (size_t)PARKS);
    if (slow) continue;
    seldom = (wakes <= 4 * windows + 4);
    swept = (2 * sweeps + 2 >= windows);
  }
  if (interval_ms <= 0) { check("with no rate window every park is swept", swept); return; }
  if (slow && !(seldom && swept)) { fprintf(stderr, "test: parks inside the rate window wake nobody...  skipped (too slow: a window has but a few parks of 1 ms)\n"); return; }
  check("a thread that parks all the time wakes the scavenger for its windows, not for its parks", seldom);
  check("and is swept once for each window", swept);
}

// a thread that parks when it is told to, so that the test can look on while it is in `mi_on_thread_idle_start`
enum { PARKER_RUNS = 0, PARKER_PARKS = 1, PARKER_REFUSED = 2, PARKER_EXITS = 3 };
typedef struct parker_s {
  pthread_t   thread;
  atomic_int  want;    // what it is asked for
  atomic_int  is;      // what it did
} parker_t;

static void* parker_main(void* arg) {
  parker_t* const pk = (parker_t*)arg;
  void* q = mi_malloc(64); mi_free(q);   // heaps of its own to hand off
  for (;;) {
    const int want = atomic_load(&pk->want);
    const int is = atomic_load(&pk->is);
    if (want == PARKER_EXITS) break;   // (parked or not)
    if (want == PARKER_PARKS && is == PARKER_RUNS) {
      atomic_store(&pk->is, mi_on_thread_idle_start() ? PARKER_PARKS : PARKER_REFUSED);
    }
    else if (want == PARKER_RUNS && is != PARKER_RUNS) {
      mi_on_thread_idle_end();
      atomic_store(&pk->is, PARKER_RUNS);
    }
    else {
      #if defined(MI_TSAN)
      sched_yield();   // (see `park_then_cancel`)
      #else
      usleep(50);
      #endif
    }
  }
  return NULL;
}

static bool parker_start(parker_t* pk) {
  atomic_store(&pk->want, PARKER_RUNS);
  atomic_store(&pk->is, PARKER_RUNS);
  return (pthread_create(&pk->thread, NULL, &parker_main, pk) == 0);
}

static void parker_stop(parker_t* pk) {
  atomic_store(&pk->want, PARKER_EXITS);
  pthread_join(pk->thread, NULL);
}

static bool parker_wait(parker_t* pk, int is, long deadline_ms) {
  for (long i = 0; i < deadline_ms * 10; i++) { if (atomic_load(&pk->is) == is) return true; usleep(100); }
  return atomic_load(&pk->is) == is;
}

static void parker_park_begin(parker_t* pk) { atomic_store(&pk->want, PARKER_PARKS); }
static bool parker_park(parker_t* pk)       { parker_park_begin(pk); return parker_wait(pk, PARKER_PARKS, 5000); }
static void parker_unpark(parker_t* pk)     { atomic_store(&pk->want, PARKER_RUNS); parker_wait(pk, PARKER_RUNS, 5000); }

// Have a park of `pk` swept, and the next one, inside the window that begins with that sweep, passed over: the
// thread is on the schedule then, and running. The window ends between `*ends_after` and `*ends_before`.
// False if the machine was too slow for that (the window was over before the second park was looked at).
static bool parker_get_on_schedule(parker_t* pk, long interval_ms, long* ends_after, long* ends_before) {
  usleep((useconds_t)(interval_ms * 1000 + 5000));   // due
  size_t before = handoff_counts().sweeps;
  *ends_after = now_ms() + interval_ms;
  if (!parker_park(pk)) { parker_unpark(pk); return false; }
  const bool swept = wait_for_sweep_after(before, 3000);
  parker_unpark(pk);   // (waits for the sweep to end: it is stamped now)
  *ends_before = now_ms() + interval_ms + 1;
  if (!swept) return false;
  before = handoff_counts().turns;
  if (!parker_park(pk)) { parker_unpark(pk); return false; }
  for (int i = 0; i < 10000 && handoff_counts().turns == before; i++) { usleep(100); }   // this park woke the scavenger: it walks
  size_t turns = handoff_counts().turns;
  for (int i = 0; i < 20; i++) {   // ..and is back in its wait
    usleep(1000);
    const size_t now = handoff_counts().turns;
    if (now == turns && i > 0) break;
    turns = now;
  }
  const bool looked = (turns > before && now_ms() < *ends_after - 10);
  parker_unpark(pk);
  return looked;
}

// A park of a thread that is on the schedule wakes nobody, and is swept when the window ends.
static void test_park_on_schedule_is_swept(void) {
  const long interval_ms = mi_option_get(mi_option_purge_holes_min_interval);
  if (interval_ms <= 0) return;   // no window (the `-eager` variant)
  if (!mi_on_thread_idle_start()) { fprintf(stderr, "test: a park of a thread on the schedule...  skipped (nobody to hand off to)\n"); return; }
  mi_on_thread_idle_end();
  parker_t pk;
  if (!parker_start(&pk)) return;
  bool tried = false, quiet = false, swept = false;
  long waited_ms = -1;
  for (int round = 0; round < 8 && !quiet; round++) {   // (again if the scavenger was too slow to see the park that gets the thread on the schedule)
    long ends_after, ends_before;
    if (!parker_get_on_schedule(&pk, interval_ms, &ends_after, &ends_before)) continue;
    const handoff_counts_t before = handoff_counts();
    const long t0 = now_ms();
    const bool parked = parker_park(&pk);
    if (parked && now_ms() < ends_after - 5) {   // parked inside the window
      tried = true;
      quiet = (handoff_counts().wakes == before.wakes);
      swept = wait_for_sweep_after(before.sweeps, interval_ms * 10 + 1000);
      waited_ms = now_ms() - t0;
    }
    parker_unpark(&pk);
  }
  parker_stop(&pk);
  if (!tried) { fprintf(stderr, "test: a park of a thread on the schedule...  skipped (too slow to park inside a window of %ld ms)\n", interval_ms); return; }
  fprintf(stderr, "  park of a thread on the schedule swept after %ldms (min_interval=%ldms)\n", waited_ms, interval_ms);
  check("a park of a thread that is on the schedule of the scavenger wakes nobody", quiet);
  check("and is swept when the window of the thread ends", swept);
}

// A thread that is on the schedule and then exits is taken off it, and a forked child starts with nobody on it:
// a park there has to wake the scavenger of the child, which has no schedule to go by.
static void* park_on_schedule_then_exit(void* arg) {
  (void)arg;
  void* q = mi_malloc(64); mi_free(q);
  for (int i = 0; i < 3; i++) {   // swept, passed over, and parked once more on the schedule
    if (!mi_on_thread_idle_start()) break;
    usleep(3000);
    if (i < 2) { mi_on_thread_idle_end(); }
  }
  return NULL;   // (parked)
}

static void test_exit_and_fork_on_schedule(void) {
  enum { THREADS = 4 };
  const long interval_ms = mi_option_get(mi_option_purge_holes_min_interval);
  if (!mi_on_thread_idle_start()) { fprintf(stderr, "test: exit and fork on the schedule...  skipped (nobody to hand off to)\n"); return; }
  mi_on_thread_idle_end();
  pthread_t t[THREADS];
  for (int i = 0; i < THREADS; i++) { if (pthread_create(&t[i], NULL, &park_on_schedule_then_exit, NULL) != 0) return; }
  for (int i = 0; i < THREADS; i++) { pthread_join(t[i], NULL); }
  usleep((useconds_t)(interval_ms * 1000 + 20000));   // the windows of the threads that are gone end
  size_t before = handoff_counts().sweeps;
  bool parked = mi_on_thread_idle_start();
  const bool swept = parked && wait_for_sweep_after(before, 3000);
  mi_on_thread_idle_end();
  check("a thread may exit while it is on the schedule of the scavenger", swept);
  if (interval_ms <= 0) return;
  #if defined(MI_TSAN)
  // (the child starts a scavenger thread, which the thread sanitizer does not let a forked child do)
  fprintf(stderr, "test: a park in a forked child...  skipped (thread sanitizer)\n");
  #else
  // get on the schedule ourselves (swept just now: the next park is passed over), and fork
  before = handoff_counts().turns;
  parked = mi_on_thread_idle_start();
  for (int i = 0; parked && i < 10000 && handoff_counts().turns == before; i++) { usleep(100); }
  usleep(2000);
  mi_on_thread_idle_end();
  const pid_t pid = fork();
  if (pid == 0) {
    // The child has a scavenger of its own from its first park on, which knows of no schedule. (A park that is
    // over before that thread runs: it is not to come upon this thread by the walk it starts with.)
    int bad = 0;
    if (mi_on_thread_idle_start()) { mi_on_thread_idle_end(); } else { bad = 1; }
    usleep((useconds_t)(interval_ms * 1000 + 5000));   // the window of the last sweep, which came along, ends
    const size_t sweeps = handoff_counts().sweeps;
    if (bad == 0) {
      if (!mi_on_thread_idle_start()) { bad = 1; }
      else if (!wait_for_sweep_after(sweeps, 3000)) { bad = 2; }
      mi_on_thread_idle_end();
    }
    _exit(bad);
  }
  int status = 0;
  const bool child_ok = (pid > 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
  if (!child_ok && pid > 0 && WIFEXITED(status)) { fprintf(stderr, "\n  the child failed with %d (1: nobody to hand off to, 2: its park was not swept)\n", WEXITSTATUS(status)); }
  check("a park in a forked child wakes the scavenger of the child", child_ok);
  #endif
}

#if MI_DEBUG > 0
extern _Atomic(uintptr_t) mi_debug_stall_in_park_start;
extern _Atomic(uintptr_t) mi_debug_stall_in_scavenger_visit;

// Wait until the scavenger is held before its wait and that wait is to be a long one: it has nobody on its
// schedule and nothing to purge, so nothing but a wake ends that wait before `deadline_ms`.
// (`mi_debug_stall_in_scavenger_wait` is set on return if that is true: the scavenger is held.)
static bool hold_scavenger_before_long_wait(long deadline_ms, long give_up_ms) {
  const long t0 = now_ms();
  atomic_store(&mi_debug_stall_in_scavenger_wait, (uintptr_t)1);
  for (;;) {
    if (atomic_load(&mi_debug_stall_in_scavenger_wait) == 2) {
      if ((long)atomic_load(&mi_debug_scavenger_wait_msecs) >= 2 * deadline_ms) return true;
      atomic_store(&mi_debug_stall_in_scavenger_wait, (uintptr_t)1);   // on to its next turn, and hold it there
    }
    if (now_ms() - t0 > give_up_ms) { atomic_store(&mi_debug_stall_in_scavenger_wait, (uintptr_t)0); return false; }
    usleep(100);
  }
}

// The thread publishes its park and THEN reads whether it is on the schedule. Hold it before it publishes, until
// the scavenger has come for it (its window ended), found it running, taken it off the schedule and is about to
// wait for good: the park that is published then has to find the mark gone, and wake the scavenger.
static void test_park_after_visit_wakes(void) {
  const long interval_ms = mi_option_get(mi_option_purge_holes_min_interval);
  const long deadline_ms = interval_ms * 10 + 1000;
  if (interval_ms <= 0) return;
  if (!mi_on_thread_idle_start()) { fprintf(stderr, "test: a park after the visit of the scavenger...  skipped (nobody to hand off to)\n"); return; }
  mi_on_thread_idle_end();
  parker_t pk;
  if (!parker_start(&pk)) return;
  bool tried = false, swept = false;
  for (int round = 0; round < 5 && !tried; round++) {
    long ends_after, ends_before;
    if (!parker_get_on_schedule(&pk, interval_ms, &ends_after, &ends_before)) continue;
    atomic_store(&mi_debug_stall_in_park_start, (uintptr_t)1);
    parker_park_begin(&pk);
    const bool held = wait_for_stall(&mi_debug_stall_in_park_start, 1000) && (now_ms() < ends_after - 5);   // (inside the window: on the schedule)
    const bool visited = held && hold_scavenger_before_long_wait(deadline_ms, (ends_before - now_ms()) + 2000);
    const size_t before = handoff_counts().sweeps;
    atomic_store(&mi_debug_stall_in_park_start, (uintptr_t)0);
    const bool parked = parker_wait(&pk, PARKER_PARKS, 5000);
    atomic_store(&mi_debug_stall_in_scavenger_wait, (uintptr_t)0);
    if (visited && parked) {
      tried = true;
      swept = wait_for_sweep_after(before, deadline_ms);
    }
    parker_unpark(&pk);
  }
  parker_stop(&pk);
  if (!tried) { fprintf(stderr, "test: a park after the visit of the scavenger...  skipped (too slow, or the scavenger had something scheduled each time)\n"); return; }
  check("a park that is published after the scavenger took the thread off its schedule wakes it", swept);
}

// The scavenger takes the thread off its schedule and THEN looks at its park. Hold it between the two, until
// the thread has published a park: that park found the mark gone, so it woke the scavenger, and is swept.
static void test_park_during_visit_is_seen(void) {
  const long interval_ms = mi_option_get(mi_option_purge_holes_min_interval);
  const long deadline_ms = interval_ms * 10 + 1000;
  if (interval_ms <= 0) return;
  if (!mi_on_thread_idle_start()) { fprintf(stderr, "test: a park during the visit of the scavenger...  skipped (nobody to hand off to)\n"); return; }
  mi_on_thread_idle_end();
  parker_t pk;
  if (!parker_start(&pk)) return;
  bool tried = false, swept = false;
  for (int round = 0; round < 5 && !tried; round++) {
    long ends_after, ends_before;
    if (!parker_get_on_schedule(&pk, interval_ms, &ends_after, &ends_before)) continue;
    atomic_store(&mi_debug_stall_in_scavenger_visit, (uintptr_t)1);
    const bool visiting = wait_for_stall(&mi_debug_stall_in_scavenger_visit, (ends_before - now_ms()) + 2000);
    const size_t before = handoff_counts().sweeps;
    const bool parked = visiting && parker_park(&pk);
    atomic_store(&mi_debug_stall_in_scavenger_visit, (uintptr_t)0);
    if (parked) {
      tried = true;
      swept = wait_for_sweep_after(before, deadline_ms);
    }
    parker_unpark(&pk);
  }
  parker_stop(&pk);
  if (!tried) { fprintf(stderr, "test: a park during the visit of the scavenger...  skipped (too slow to get a thread on the schedule)\n"); return; }
  check("a park that is published while the scavenger takes the thread off its schedule is swept", swept);
}

// A thread on the schedule bounds the wait of the scavenger until its window ends, parked or not: its parks
// wake nobody. Have the scavenger walk (for the park of another thread) while the thread runs, and park after.
static void test_schedule_bounds_the_wait(void) {
  const long interval_ms = mi_option_get(mi_option_purge_holes_min_interval);
  const long deadline_ms = interval_ms * 10 + 1000;
  if (interval_ms <= 0) return;
  if (!mi_on_thread_idle_start()) { fprintf(stderr, "test: a thread on the schedule bounds the wait...  skipped (nobody to hand off to)\n"); return; }
  mi_on_thread_idle_end();
  parker_t pk;
  if (!parker_start(&pk)) return;
  bool tried = false, bounded = false, swept = false;
  for (int round = 0; round < 5 && !tried; round++) {
    parker_t other;   // a new thread: never swept, so its park is due, and not on the schedule, so it wakes
    long ends_after, ends_before;
    if (!parker_get_on_schedule(&pk, interval_ms, &ends_after, &ends_before)) continue;
    if (!parker_start(&other)) break;
    atomic_store(&mi_debug_stall_in_scavenger_wait, (uintptr_t)1);
    const bool parked_other = parker_park(&other);
    const bool held = parked_other && wait_for_stall(&mi_debug_stall_in_scavenger_wait, 1000) && (now_ms() < ends_after - 5);
    const long wait_ms = (long)atomic_load(&mi_debug_scavenger_wait_msecs);
    const handoff_counts_t before = handoff_counts();
    const bool parked = held && parker_park(&pk);
    const bool quiet = parked && (handoff_counts().wakes == before.wakes) && (now_ms() < ends_after - 5);
    atomic_store(&mi_debug_stall_in_scavenger_wait, (uintptr_t)0);
    if (quiet) {
      tried = true;
      bounded = (wait_ms <= interval_ms);
      swept = wait_for_sweep_after(before.sweeps, deadline_ms);
    }
    parker_unpark(&pk);
    parker_unpark(&other);
    parker_stop(&other);
  }
  parker_stop(&pk);
  if (!tried) { fprintf(stderr, "test: a thread on the schedule bounds the wait...  skipped (too slow to park inside a window of %ld ms)\n", interval_ms); return; }
  check("a thread on the schedule bounds the wait of the scavenger while it runs", bounded);
  check("and its next park is swept when its window ends", swept);
}

// A thread comes off the schedule when its window ends. A park that it is in then, and that was not swept yet, is
// not swept there and then: the thread did not say when that park began, and one that parks all the time is about
// to take its heaps back. The scavenger looks again in a moment, and sweeps the thread if it is still parked.
// Hold it where it took the thread off its schedule, and again before its next wait: it swept nothing in between,
// and that wait is the short one. (A turn that something else asks for in between sweeps the park: go again then.)
static void test_park_at_visit_is_looked_at_again(void) {
  const long interval_ms = mi_option_get(mi_option_purge_holes_min_interval);
  const long deadline_ms = interval_ms * 10 + 1000;
  if (interval_ms <= 0) return;
  const long moment_ms = (interval_ms >= 32 ? interval_ms / 16 : 1);   // (`MI_PARK_VISIT_GRACE_DIV`)
  if (!mi_on_thread_idle_start()) { fprintf(stderr, "test: a park at the visit of the scavenger...  skipped (nobody to hand off to)\n"); return; }
  mi_on_thread_idle_end();
  parker_t pk;
  if (!parker_start(&pk)) return;
  bool tried = false, left = false, soon = false, swept = false;
  for (int round = 0; round < 5 && !(left && soon); round++) {
    long ends_after, ends_before;
    if (!parker_get_on_schedule(&pk, interval_ms, &ends_after, &ends_before)) continue;
    const handoff_counts_t before = handoff_counts();
    atomic_store(&mi_debug_stall_in_scavenger_visit, (uintptr_t)1);
    // a park inside the window, which wakes nobody: the scavenger does not know of it
    const bool parked = parker_park(&pk) && (now_ms() < ends_after - 5) && (handoff_counts().wakes == before.wakes);
    const bool visiting = parked && wait_for_stall(&mi_debug_stall_in_scavenger_visit, (ends_before - now_ms()) + 2000);
    atomic_store(&mi_debug_stall_in_scavenger_wait, (uintptr_t)1);
    atomic_store(&mi_debug_stall_in_scavenger_visit, (uintptr_t)0);
    const bool waiting = visiting && wait_for_stall(&mi_debug_stall_in_scavenger_wait, 5000);
    if (waiting) {
      tried = true;
      left = (handoff_counts().sweeps == before.sweeps);
      soon = ((long)atomic_load(&mi_debug_scavenger_wait_msecs) <= moment_ms);
    }
    atomic_store(&mi_debug_stall_in_scavenger_wait, (uintptr_t)0);
    if (left && soon) { swept = wait_for_sweep_after(before.sweeps, deadline_ms); }
    parker_unpark(&pk);
  }
  parker_stop(&pk);
  if (!tried) { fprintf(stderr, "test: a park at the visit of the scavenger...  skipped (too slow to park inside a window of %ld ms)\n", interval_ms); return; }
  check("a park that was not swept yet is not swept where its thread comes off the schedule", left);
  check("the scavenger looks at it again in a moment", soon);
  check("and sweeps the thread if it is still parked", swept);
}

static void test_schedule_handshake(void) {
  usleep(300000);   // what the tests before this one freed is purged: nothing is scheduled
  test_park_after_visit_wakes();
  test_park_during_visit_is_seen();
  test_park_at_visit_is_looked_at_again();
  test_schedule_bounds_the_wait();
}
#else
static void test_schedule_handshake(void) { fprintf(stderr, "test: the order of a park and the visit of the scavenger...  skipped (needs MI_DEBUG>0)\n"); }
#endif

// ---------------------------------------------------------------------------
// fork() by a thread that is between _start and _end -- fork does not allocate, so the contract
// permits it, and the scavenger may be part-way through rewriting this thread's page free lists.
// The damage is in those *freed* holes, not the survivors: the child must be able to allocate
// straight out of the churned pages' free lists without meeting a corrupt entry (which aborts) and
// without two allocations aliasing. Refilling exactly the freed slots forces exactly that path.
// ---------------------------------------------------------------------------
#if defined(__APPLE__) || defined(__GLIBC__)
#include <execinfo.h>
#define FORK_CHILD_HAS_BACKTRACE 1
#endif

// in the forked child: say where a fault happened (there is no core dump to look at on a CI machine)
static void fork_child_fault(int sig, siginfo_t* info, void* ctx) {
  (void)ctx;
  char buf[128];
  const int n = snprintf(buf, sizeof(buf), "\n  forked child: signal %d at address %p (in a mimalloc heap: %d)\n", sig, info->si_addr, (int)mi_is_in_heap_region(info->si_addr));
  if (n > 0) { (void)!write(2, buf, (size_t)n); }
  #if FORK_CHILD_HAS_BACKTRACE
  void* frames[48];
  const int count = backtrace(frames, 48);
  backtrace_symbols_fd(frames, count, 2);
  #endif
  _exit(64 + sig);
}

static void test_fork_while_parked(void) {
  enum { ROUNDS = 16 };
  bool all_ok = true;
  for (int r = 0; r < ROUNDS && all_ok; r++) {
    void** p = (void**)calloc(LIVE, sizeof(void*));
    if (p == NULL) return;
    churn_pattern(p);
    (void)mi_on_thread_idle_start();
    usleep(150 + (unsigned)((r * 37) % 400));   // land the clone before and inside a sweep
    const pid_t pid = fork();
    if (pid == 0) {
      // child: allocate out of the inherited free lists and check for aliasing
      struct sigaction sa;
      memset(&sa, 0, sizeof(sa));
      sa.sa_sigaction = &fork_child_fault;
      sa.sa_flags = SA_SIGINFO;
      sigaction(SIGSEGV, &sa, NULL);
      sigaction(SIGBUS, &sa, NULL);
      const size_t ke = keep_every();
      int bad = 0;
      for (int i = 0; i < LIVE; i++) {
        if (p[i] == NULL) { p[i] = mi_malloc(BSZ); if (p[i] == NULL) { bad = 1; break; } memset(p[i], (int)(i & 0xFF), BSZ); }
      }
      for (int i = 0; !bad && i < LIVE; i++) {
        if (((size_t)i % ke) == 0) {
          // a survivor, still holding churn's pattern -- must be untouched by the interrupted sweep
          if (pattern_check(p[i], BSZ, (size_t)i) != BSZ) { bad = 2; }
        }
        else {
          // a slot the child just refilled: if two mallocs aliased, an earlier fill got overwritten
          const uint8_t* b = (const uint8_t*)p[i];
          if (b[0] != (uint8_t)(i & 0xFF) || b[BSZ - 1] != (uint8_t)(i & 0xFF)) { bad = 3; }
        }
      }
      _exit(bad);
    }
    mi_on_thread_idle_end();
    if (pid < 0) { all_ok = false; }
    else {
      int status = 0;
      // a corrupt free-list entry aborts the child (a signal), which is a failure too
      if (waitpid(pid, &status, 0) < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        all_ok = false;
        if (WIFSIGNALED(status)) { fprintf(stderr, "\n  round %d: the child was killed by signal %d\n", r, WTERMSIG(status)); }
        else if (WIFEXITED(status)) { fprintf(stderr, "\n  round %d: the child failed with %d (1: out of memory, 2: a survivor changed, 3: two allocations alias, 64+n: signal n)\n", r, WEXITSTATUS(status)); }
        else { fprintf(stderr, "\n  round %d: waitpid failed or the child stopped (status 0x%x)\n", r, (unsigned)status); }
      }
      if (first_corrupt_survivor(p) >= 0) {   // and the parent stays intact
        all_ok = false;
        fprintf(stderr, "\n  round %d: survivor %ld changed in the parent\n", r, first_corrupt_survivor(p));
      }
    }
    for (int i = 0; i < LIVE; i++) { if (p[i] != NULL) mi_free(p[i]); }
    free(p);
  }
  check("fork while parked leaves parent and child heaps consistent", all_ok);
}

// ---------------------------------------------------------------------------
// A thread that used a NON-main heap (so it has dynamic thread-locals) parks and then exits while
// a sweep is in flight. Its teardown frees the thread-locals array with mi_free: that free must not
// race the scavenger's rewrite of the same pages -- the park has to be left before any teardown
// free. Also registers a pthread key destructor that frees, as an application would.
// ---------------------------------------------------------------------------
static pthread_key_t dtor_key;
static void dtor_frees(void* blocks_v) {
  void** blocks = (void**)blocks_v;
  if (blocks == NULL) return;
  for (int i = 0; i < 500; i++) { if (blocks[i] != NULL) mi_free(blocks[i]); }
  free(blocks);
}

static void* park_then_exit_with_dyn_tls(void* arg) {
  (void)arg;
  // touch a non-main heap so this thread gets dynamic thread-locals (tls->count > 0)
  mi_heap_t* h = mi_heap_new();
  if (h != NULL) {
    void* q = mi_heap_malloc(h, 64);
    if (q != NULL) mi_free(q);
    // deliberately keep `h`: mi_heap_delete at teardown is part of the exit path under test
  }
  // an app-level destructor that frees on this thread as it exits
  void** dblocks = (void**)calloc(500, sizeof(void*));
  if (dblocks != NULL) {
    for (int i = 0; i < 500; i++) { dblocks[i] = mi_malloc(96); if (dblocks[i] == NULL) break; }
    pthread_setspecific(dtor_key, dblocks);
  }
  void** p = (void**)calloc(LIVE, sizeof(void*));
  if (p != NULL) { churn(p); free(p); }   // holes to punch; the mi blocks stay live on purpose
  (void)mi_on_thread_idle_start();
  usleep(200);        // scavenger claims us and starts sweeping
  pthread_exit(NULL); // leave while parked/swept: teardown frees run against the sweep
}

static void test_exit_while_swept_with_dyn_tls(void) {
  enum { THREADS = 8, ROUNDS = 12 };
  if (pthread_key_create(&dtor_key, &dtor_frees) != 0) return;
  for (int r = 0; r < ROUNDS; r++) {
    pthread_t t[THREADS];
    for (int i = 0; i < THREADS; i++) {
      if (pthread_create(&t[i], NULL, &park_then_exit_with_dyn_tls, NULL) != 0) return;
    }
    for (int i = 0; i < THREADS; i++) { pthread_join(t[i], NULL); }
  }
  pthread_key_delete(dtor_key);
  // reaching here without a crash is the assertion; under TSAN the teardown-free race reports
  check("exit while swept, with dtor frees and dynamic thread-locals, is race-free", true);
}

// ---------------------------------------------------------------------------
// What a sweep leaves under `purge_holes_large_floor` goes when its page was not allocated from for so many epochs of
// the sweep, and an epoch ends only when a thread is swept: nobody is woken for it. A thread that stays parked keeps
// it, and it goes with the parks that come after.
// ---------------------------------------------------------------------------
static size_t purged_now(void) {
  mi_purge_holes_stats_t h; mi_purge_holes_stats_get(&h); return h.purged_bytes;
}

static _Atomic(size_t) park_floor_target;
static _Atomic(int)    park_floor_parks;

static void* park_floor_other(void* arg) {
  (void)arg;
  void* q = mi_malloc(64);   // (a thread with a heap of its own to be swept)
  int parks = 0;
  for (; parks < 400 && purged_now() < atomic_load(&park_floor_target); parks++) {
    if (mi_on_thread_idle_start()) { usleep(3000); }
    mi_on_thread_idle_end();
  }
  atomic_store(&park_floor_parks, parks);
  mi_free(q);
  return NULL;
}

static void test_park_floor(void) {
  enum { BLOCKS = 18, BLOCK = 300 * 1024 };   // a page that fills up (and is abandoned for that), and half of one that stays this thread's own
  const long old_interval = mi_option_get(mi_option_purge_holes_min_interval);
  const long old_floor = mi_option_get(mi_option_purge_holes_large_floor);
  const long old_epochs = mi_option_get(mi_option_purge_holes_large_floor_epochs);
  mi_option_set(mi_option_purge_holes_min_interval, 2);
  mi_option_set(mi_option_purge_holes_large_floor, 8 * 1024);    // KiB
  mi_option_set(mi_option_purge_holes_large_floor_epochs, 32);
  void* p[BLOCKS];
  for (int i = 0; i < BLOCKS; i++) { p[i] = mi_malloc(BLOCK); if (p[i] != NULL) memset(p[i], 5, BLOCK); }
  size_t freed = 0;
  for (int i = 0; i < BLOCKS; i++) { if ((i % 3) != 0 && p[i] != NULL) { mi_free(p[i]); p[i] = NULL; freed++; } }
  const size_t before = purged_now();
  const bool parked = mi_on_thread_idle_start();
  if (parked) {
    usleep(300 * 1000);   // the sweeps of the hold are over: the blocks are out of it and under the floor
    const size_t held = purged_now();
    usleep(700 * 1000);   // far longer than 32 intervals
    const size_t still = purged_now();
    // the parks of another thread move the epoch on, and then this one is looked at again though it never left its park:
    // it is not to hold the floor against the threads that run
    atomic_store(&park_floor_target, before + freed * (BLOCK - 16 * 1024));
    pthread_t other;
    const bool started = (pthread_create(&other, NULL, &park_floor_other, NULL) == 0);
    if (started) { pthread_join(other, NULL); }
    const size_t gone = purged_now();
    mi_on_thread_idle_end();
    fprintf(stderr, "  %zu blocks of %d KiB freed in large pages: %zu KiB of them purged 300 ms into the park, %zu KiB after a second of it, %zu KiB after %d parks of another thread\n", freed, BLOCK / 1024, (held - before) / 1024, (still - before) / 1024, (gone - before) / 1024, atomic_load(&park_floor_parks));
    check("free blocks of large pages stay under the floor at first", held - before < 2 * (size_t)BLOCK);
    check("a thread that stays parked keeps them: nothing comes back for the floor", still == held);
    check("they go once the page was not allocated from for that many epochs, which the parks of another thread move", started && gone >= atomic_load(&park_floor_target));
  }
  else {
    mi_on_thread_idle_end();
  }
  for (int i = 0; i < BLOCKS; i++) { if (p[i] != NULL) mi_free(p[i]); }
  mi_option_set(mi_option_purge_holes_min_interval, old_interval);
  mi_option_set(mi_option_purge_holes_large_floor, old_floor);
  mi_option_set(mi_option_purge_holes_large_floor_epochs, old_epochs);
}

// ---------------------------------------------------------------------------
// Stopping the scavenger joins the thread: a park after it has nobody to hand off to and reports
// false, and the process stays fully usable. Runs last, since it takes the scavenger away.
// ---------------------------------------------------------------------------
static void test_scavenger_stop(void) {
  mi_scavenger_stop();
  check("no handoff once the scavenger is stopped", !mi_on_thread_idle_start());
  mi_scavenger_stop();   // a second stop is a no-op
  void* q = mi_malloc(64);
  check("the thread still allocates after the stop", q != NULL);
  mi_free(q);
  mi_on_thread_idle();   // and can still sweep for itself
}

int main(void) {
  test_lazy_start_and_signals();   // first: nothing may have started the scavenger yet
  test_park_floor();        // (before the others leave free blocks of their own to be purged)
  test_handoff_sweeps();
  test_survivors_intact();
  test_unbalanced();
  test_third_thread_frees_during_sweep();
  test_parks_get_swept();
  test_park_inside_window_gets_swept();
  test_handoff_counters();
  test_park_between_walk_and_wait();
  test_frequent_parks_wake_seldom();
  test_park_on_schedule_is_swept();
  test_schedule_handshake();
  test_exit_and_fork_on_schedule();
  test_fork_while_parked();
  test_park_then_exit();
  test_exit_while_swept_with_dyn_tls();
  test_park_stress();
  test_scavenger_stop();
  fprintf(stderr, "\n%s\n", failures == 0 ? "all tests passed." : "SOME TESTS FAILED.");
  return failures == 0 ? 0 : 1;
}

#endif  // !_WIN32
