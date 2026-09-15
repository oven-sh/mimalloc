/* ----------------------------------------------------------------------------
Copyright (c) 2018-2026 Microsoft Research, Daan Leijen
This is free software; you can redistribute it and/or modify it under the
terms of the MIT license.
-----------------------------------------------------------------------------*/

/* The thread local slots of a thread (`threadlocal.c`), which hold the theap of every heap besides the main
   one, hang off a thread local of their own. Where that is a pthread key (macOS, MI_TLS_MODEL_PTHREADS) the
   key is created when it is first set, which is when a heap besides the main one is first used: and a program
   can do that on many threads at once. Every thread has to end up with the same key, or the one whose key was
   replaced does not find its slots again (and makes a second theap for its heap; a debug build asserts in
   `heap.c:mi_heap_init_theap`).

   There is one first time in a process. A debug build has a hook that holds the first thread that creates the
   key until a second one is there too (`libc.c:mi_debug_stall_in_pthread_key_create`), which makes that one
   time the race; without it the threads make their heaps, wait for each other, and allocate from them at once.
   Where thread locals are not pthread keys this only checks that it works.

   > mimalloc-test-tls-key-race
*/

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

#include "mimalloc.h"
#include "mimalloc-stats.h"

#if defined(_WIN32) || defined(__EMSCRIPTEN__)
int main(void) { printf("skipped (no pthreads)\n"); return 0; }
#else

#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>

#if MI_DEBUG > 0
extern _Atomic(uintptr_t) mi_debug_stall_in_pthread_key_create;
#endif

#define THREADS 8

static atomic_int ready, go, bad;

// A thread that lost its slots does not notice: it makes new ones, and a second theap for its heap, and goes on
// with those. The count of the theaps that were made tells.
static size_t theaps_made(void) {
  static mi_stats_t stats;
  stats.size = sizeof(stats);
  stats.version = MI_STAT_VERSION;
  if (!mi_subproc_stats_get_exclusive(mi_subproc_main(), &stats)) return 0;
  return (size_t)stats.theaps.total;
}

static void* worker(void* arg) {
  (void)arg;
  mi_heap_t* const heap = mi_heap_new();   // (takes a lock: the threads do not get here together)
  atomic_fetch_add(&ready, 1);
  while (!atomic_load(&go)) { sched_yield(); }
  void* const p = mi_heap_malloc(heap, 64);   // the first use of the thread local slots
  mi_theap_t* const theap = mi_heap_theap(heap);
  void* const q = mi_heap_malloc(heap, 64);
  if (p == NULL || q == NULL || theap == NULL || mi_heap_theap(heap) != theap || !mi_heap_contains(heap, p) || !mi_heap_contains(heap, q)) {
    atomic_fetch_add(&bad, 1);
  }
  mi_free(p);
  mi_free(q);
  mi_heap_delete(heap);
  return NULL;
}

int main(void) {
  mi_free(mi_malloc(8));   // the main heap only
  #if MI_DEBUG > 0
  atomic_store(&mi_debug_stall_in_pthread_key_create, (uintptr_t)1);
  #endif
  const size_t theaps_before = theaps_made();
  pthread_t threads[THREADS];
  int started = 0;
  for (; started < THREADS; started++) {
    if (pthread_create(&threads[started], NULL, &worker, NULL) != 0) break;
  }
  while (atomic_load(&ready) < started) { sched_yield(); }
  atomic_store(&go, 1);
  for (int i = 0; i < started; i++) { pthread_join(threads[i], NULL); }
  // every thread makes a theap for the main heap (its heap is allocated there) and one for its own
  const size_t made = theaps_made() - theaps_before;
  const bool counted = (theaps_made() != 0);   // (not in a build without statistics)
  bool ok = (started >= 2 && atomic_load(&bad) == 0 && (!counted || made == 2*(size_t)started));
  printf("%d threads made %zu theaps, %d of them saw its heap change", started, made, atomic_load(&bad));
  #if MI_DEBUG > 0
  const uintptr_t arrived = atomic_exchange(&mi_debug_stall_in_pthread_key_create, (uintptr_t)0) - 1;
  printf("; %d created the key at the same time", (int)arrived);
  #if MI_TLS_MODEL_PTHREADS || defined(__APPLE__)
  ok = ok && (arrived >= 2);   // or this was not the race
  #endif
  #endif
  printf("\n");
  return (ok ? 0 : 1);
}

#endif
