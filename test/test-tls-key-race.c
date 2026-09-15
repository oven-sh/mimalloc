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

   There is one first time in a process, so every round is a child process: its threads make their heaps, wait
   for each other, and then allocate from them at the same moment.

   > mimalloc-test-tls-key-race [ROUNDS]
*/

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

#include "mimalloc.h"

#if defined(_WIN32) || defined(__EMSCRIPTEN__)
int main(void) { printf("skipped (no fork)\n"); return 0; }
#else

#include <pthread.h>
#include <stdatomic.h>
#include <sys/wait.h>
#include <unistd.h>

#define THREADS 8

static atomic_int ready, go, bad;

static void* worker(void* arg) {
  (void)arg;
  mi_heap_t* const heap = mi_heap_new();   // (takes a lock: the threads do not get here together)
  atomic_fetch_add(&ready, 1);
  while (!atomic_load(&go)) { }
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

static int round_in_child(void) {
  pthread_t threads[THREADS];
  for (int i = 0; i < THREADS; i++) {
    if (pthread_create(&threads[i], NULL, &worker, NULL) != 0) return 2;
  }
  while (atomic_load(&ready) < THREADS) { }
  atomic_store(&go, 1);
  for (int i = 0; i < THREADS; i++) { pthread_join(threads[i], NULL); }
  return (atomic_load(&bad) == 0 ? 0 : 1);
}

int main(int argc, char** argv) {
  int rounds = 200;
  if (argc > 1) { rounds = atoi(argv[1]); }
  mi_free(mi_malloc(8));   // the main heap only: no round has happened in this process yet
  int failed = 0;
  for (int r = 0; r < rounds; r++) {
    const pid_t pid = fork();
    if (pid == 0) { _exit(round_in_child()); }
    int status = 0;
    if (pid < 0 || waitpid(pid, &status, 0) < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
      failed++;
      if (failed == 1) { fprintf(stderr, "round %d: status 0x%x\n", r, (unsigned)status); }
    }
  }
  printf("%d of %d rounds failed\n", failed, rounds);
  return (failed == 0 ? 0 : 1);
}

#endif
