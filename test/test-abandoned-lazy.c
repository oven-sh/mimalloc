/* ----------------------------------------------------------------------------
Copyright (c) 2018-2025 Microsoft Research, Daan Leijen
This is free software; you can redistribute it and/or modify it under the
terms of the MIT license.
-----------------------------------------------------------------------------*/

/* The per-bin abandoned-page bitmaps of a (heap, arena) pair are allocated the
   first time a page of that bin is abandoned, not when the heap first touches
   the arena. This drives every path that reads or writes them:

   - worker threads allocate blocks of several size classes from a shared heap
     and exit while the blocks are still live, so their pages are abandoned
     (the bitmaps are allocated on that path, by several threads at once);
   - the main thread then allocates the same sizes from the heap, which reclaims
     the abandoned pages, frees every block (un-abandon, page free), collects
     and deletes the heap (the bitmaps are freed with it);
   - the same with the main heap, whose bitmaps are never freed, and with blocks
     freed by a thread that never allocated (re-abandon of a mostly free page).

   A debug build checks the bitmap invariants on each of these paths.

   > mimalloc-test-abandoned-lazy [ITER]
*/

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "mimalloc.h"

#if defined(MI_TSAN) || defined(MI_UBSAN) || defined(MI_GUARDED)
static int ITER = 20;
#else
static int ITER = 100;
#endif

#define NTHREADS   8
#define NSIZES     6
#define NBLOCKS    64      // per thread per size: enough to span a few pages of each size class

static const size_t sizes[NSIZES] = { 16, 96, 512, 2048, 8192, 40000 };

typedef void (thread_entry_fun_t)(intptr_t tid);
static void run_os_threads(size_t nthreads, thread_entry_fun_t* entry);

static mi_heap_t* shared_heap;           // NULL: use the main heap
static void*      blocks[NTHREADS][NSIZES][NBLOCKS];

static void alloc_and_exit(intptr_t tid) {
  for (int s = 0; s < NSIZES; s++) {
    for (int b = 0; b < NBLOCKS; b++) {
      void* p = (shared_heap != NULL ? mi_heap_malloc(shared_heap, sizes[s]) : mi_malloc(sizes[s]));
      if (p == NULL) { fprintf(stderr, "allocation failed\n"); abort(); }
      memset(p, (int)(tid + s), sizes[s]);
      blocks[tid][s][b] = p;
    }
  }
  // exit with everything live: every page this thread used is abandoned
}

static void free_some(intptr_t tid) {
  // free most blocks of a page from a thread that never allocated from the heap:
  // a multi-threaded free that can re-abandon the page as mapped
  for (int s = 0; s < NSIZES; s++) {
    for (int b = 0; b < NBLOCKS; b++) {
      if ((b % 8) != 0) {
        mi_free(blocks[tid][s][b]);
        blocks[tid][s][b] = NULL;
      }
    }
  }
}

static void check_block(intptr_t tid, int s, void* p) {
  const unsigned char* q = (const unsigned char*)p;
  for (size_t i = 0; i < sizes[s]; i += 64) {
    if (q[i] != (unsigned char)(tid + s)) { fprintf(stderr, "block corrupted\n"); abort(); }
  }
}

static void run_round(bool use_main_heap) {
  shared_heap = (use_main_heap ? NULL : mi_heap_new());
  memset(blocks, 0, sizeof(blocks));

  // 1. abandon pages of several size classes from several threads at once
  run_os_threads(NTHREADS, &alloc_and_exit);

  // 2. reclaim: allocating the same sizes must find the abandoned pages again
  void* mine[NSIZES][NBLOCKS];
  for (int s = 0; s < NSIZES; s++) {
    for (int b = 0; b < NBLOCKS; b++) {
      mine[s][b] = (shared_heap != NULL ? mi_heap_malloc(shared_heap, sizes[s]) : mi_malloc(sizes[s]));
      if (mine[s][b] == NULL) { fprintf(stderr, "allocation failed\n"); abort(); }
    }
  }
  for (intptr_t t = 0; t < NTHREADS; t++) {
    for (int s = 0; s < NSIZES; s++) {
      for (int b = 0; b < NBLOCKS; b++) { check_block(t, s, blocks[t][s][b]); }
    }
  }

  // 3. frees from threads that never touched the heap, then from this thread
  run_os_threads(NTHREADS, &free_some);
  for (intptr_t t = 0; t < NTHREADS; t++) {
    for (int s = 0; s < NSIZES; s++) {
      for (int b = 0; b < NBLOCKS; b++) {
        if (blocks[t][s][b] != NULL) { check_block(t, s, blocks[t][s][b]); mi_free(blocks[t][s][b]); }
      }
    }
  }
  for (int s = 0; s < NSIZES; s++) {
    for (int b = 0; b < NBLOCKS; b++) { mi_free(mine[s][b]); }
  }

  // 4. collect and delete
  if (shared_heap != NULL) {
    mi_heap_collect(shared_heap, true);
    mi_heap_delete(shared_heap);
    shared_heap = NULL;
  }
  else {
    mi_collect(true);
  }
}

int main(int argc, char** argv) {
  if (argc >= 2) {
    char* end;
    long n = strtol(argv[1], &end, 10);
    if (n > 0) ITER = (int)n;
  }
  for (int i = 0; i < ITER; i++) {
    run_round(false);
    run_round(true);
  }
  printf("test-abandoned-lazy: %d rounds, ok\n", ITER);
  mi_stats_print(NULL);
  return 0;
}


/* -----------------------------------------------------------
   OS threads
----------------------------------------------------------- */

#ifdef _WIN32

#include <windows.h>

static thread_entry_fun_t* thread_entry_fun;

static DWORD WINAPI thread_entry(LPVOID param) {
  thread_entry_fun((intptr_t)param);
  return 0;
}

static void run_os_threads(size_t nthreads, thread_entry_fun_t* fun) {
  thread_entry_fun = fun;
  DWORD*  tids     = (DWORD*) calloc(nthreads, sizeof(DWORD));
  HANDLE* thandles = (HANDLE*)calloc(nthreads, sizeof(HANDLE));
  for (size_t i = 0; i < nthreads; i++) {
    thandles[i] = CreateThread(0, 8*1024L, &thread_entry, (void*)(i), 0, &tids[i]);
  }
  for (size_t i = 0; i < nthreads; i++) {
    WaitForSingleObject(thandles[i], INFINITE);
  }
  for (size_t i = 0; i < nthreads; i++) {
    CloseHandle(thandles[i]);
  }
  free(tids);
  free(thandles);
}

#else

#include <pthread.h>

static thread_entry_fun_t* thread_entry_fun;

static void* thread_entry(void* param) {
  thread_entry_fun((intptr_t)param);
  return NULL;
}

static void run_os_threads(size_t nthreads, thread_entry_fun_t* fun) {
  thread_entry_fun = fun;
  pthread_t* threads = (pthread_t*)calloc(nthreads, sizeof(pthread_t));
  memset(threads, 0, sizeof(pthread_t) * nthreads);
  for (size_t i = 0; i < nthreads; i++) {
    pthread_create(&threads[i], NULL, &thread_entry, (void*)i);
  }
  for (size_t i = 0; i < nthreads; i++) {
    pthread_join(threads[i], NULL);
  }
  free(threads);
}

#endif
