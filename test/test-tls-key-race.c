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
  pthread_t threads[THREADS];
  int started = 0;
  for (; started < THREADS; started++) {
    if (pthread_create(&threads[started], NULL, &worker, NULL) != 0) break;
  }
  while (atomic_load(&ready) < started) { sched_yield(); }
  atomic_store(&go, 1);
  for (int i = 0; i < started; i++) { pthread_join(threads[i], NULL); }
  #if MI_DEBUG > 0
  const uintptr_t arrived = atomic_exchange(&mi_debug_stall_in_pthread_key_create, (uintptr_t)0) - 1;
  printf("%d threads, %d of them lost their thread local slots; %d created the key at the same time\n", started, atomic_load(&bad), (int)arrived);
  #else
  printf("%d threads, %d of them lost their thread local slots\n", started, atomic_load(&bad));
  #endif
  return (started >= 2 && atomic_load(&bad) == 0 ? 0 : 1);
}

#endif
