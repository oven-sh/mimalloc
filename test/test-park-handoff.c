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

#include "mimalloc.h"
#include <pthread.h>
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

#define LIVE   (20000)
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
  for (;;) { pthread_testcancel(); usleep(50); }   // cancelled mid-park, as at a blocking syscall
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
// `purge_holes_min_interval` (100ms by default) and a park inside that window is skipped on
// purpose, so space the parks past it; the `-eager` ctest variant sets the interval to 0.
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
// fork() by a thread that is between _start and _end -- fork does not allocate, so the contract
// permits it, and the scavenger may be part-way through rewriting this thread's page free lists.
// The damage is in those *freed* holes, not the survivors: the child must be able to allocate
// straight out of the churned pages' free lists without meeting a corrupt entry (which aborts) and
// without two allocations aliasing. Refilling exactly the freed slots forces exactly that path.
// ---------------------------------------------------------------------------
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
      if (waitpid(pid, &status, 0) < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) { all_ok = false; }
      if (first_corrupt_survivor(p) >= 0) { all_ok = false; }   // and the parent stays intact
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
    for (int i = 0; i < 500; i++) { dblocks[i] = mi_malloc(96); }
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
  test_handoff_sweeps();
  test_survivors_intact();
  test_unbalanced();
  test_third_thread_frees_during_sweep();
  test_parks_get_swept();
  test_fork_while_parked();
  test_park_then_exit();
  test_exit_while_swept_with_dyn_tls();
  test_park_stress();
  test_scavenger_stop();
  fprintf(stderr, "\n%s\n", failures == 0 ? "all tests passed." : "SOME TESTS FAILED.");
  return failures == 0 ? 0 : 1;
}
