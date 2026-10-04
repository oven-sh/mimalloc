/* ----------------------------------------------------------------------------
Copyright (c) 2026, Microsoft Research, Daan Leijen
This is free software; you can redistribute it and/or modify it under the
terms of the MIT license. A copy of the license can be found in the file
"LICENSE" at the root of this distribution.
-----------------------------------------------------------------------------*/

// Every way to allocate, as the first call into mimalloc on a new thread. With a TLS model where the thread local
// heap of a new thread starts out as NULL (pthreads, Windows, macOS), each entry point has to handle that by itself.

#include "mimalloc.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#if defined(_WIN32)
#include <windows.h>
#else
#include <pthread.h>
#endif

#define BLOCK_SIZE  (40)

static void* first_malloc(void)           { return mi_malloc(BLOCK_SIZE); }
static void* first_zalloc(void)           { return mi_zalloc(BLOCK_SIZE); }
static void* first_calloc(void)           { return mi_calloc(2, BLOCK_SIZE); }
static void* first_mallocn(void)          { return mi_mallocn(2, BLOCK_SIZE); }
static void* first_malloc_small(void)     { return mi_malloc_small(BLOCK_SIZE); }
static void* first_zalloc_small(void)     { return mi_zalloc_small(BLOCK_SIZE); }
static void* first_malloc_large(void)     { return mi_malloc(4 * 1024 * 1024); }
static void* first_realloc(void)          { return mi_realloc(NULL, BLOCK_SIZE); }
static void* first_reallocn(void)         { return mi_reallocn(NULL, 2, BLOCK_SIZE); }
static void* first_reallocf(void)         { return mi_reallocf(NULL, BLOCK_SIZE); }
static void* first_rezalloc(void)         { return mi_rezalloc(NULL, BLOCK_SIZE); }
static void* first_recalloc(void)         { return mi_recalloc(NULL, 2, BLOCK_SIZE); }
static void* first_strdup(void)           { return mi_strdup("the first call on this thread, 40 bytes...."); }
static void* first_strndup(void)          { return mi_strndup("the first call on this thread, 40 bytes....", 42); }
static void* first_malloc_aligned(void)   { return mi_malloc_aligned(BLOCK_SIZE, 64); }
static void* first_zalloc_aligned(void)   { return mi_zalloc_aligned(BLOCK_SIZE, 64); }
static void* first_calloc_aligned(void)   { return mi_calloc_aligned(2, BLOCK_SIZE, 64); }
static void* first_malloc_aligned_at(void){ return mi_malloc_aligned_at(BLOCK_SIZE, 64, 8); }
static void* first_malloc_aligned_big(void){ return mi_malloc_aligned(BLOCK_SIZE, 1024 * 1024); }
static void* first_realloc_aligned(void)  { return mi_realloc_aligned(NULL, BLOCK_SIZE, 64); }
static void* first_rezalloc_aligned(void) { return mi_rezalloc_aligned(NULL, BLOCK_SIZE, 64); }
static void* first_aligned_alloc(void)    { return mi_aligned_alloc(64, 64); }
static void* first_memalign(void)         { return mi_memalign(64, BLOCK_SIZE); }
static void* first_posix_memalign(void)   { void* p = NULL; return (mi_posix_memalign(&p, 64, BLOCK_SIZE) == 0 ? p : NULL); }
static void* first_new(void)              { return mi_new(BLOCK_SIZE); }
static void* first_new_nothrow(void)      { return mi_new_nothrow(BLOCK_SIZE); }
static void* first_new_n(void)            { return mi_new_n(2, BLOCK_SIZE); }
static void* first_new_aligned(void)      { return mi_new_aligned(BLOCK_SIZE, 64); }
static void* first_new_realloc(void)      { return mi_new_realloc(NULL, BLOCK_SIZE); }
static void* first_heap_malloc(void)      { return mi_heap_malloc(mi_heap_main(), BLOCK_SIZE); }
static void* first_heap_realloc(void)     { return mi_heap_realloc(mi_heap_main(), NULL, BLOCK_SIZE); }
static void* first_heap_strdup(void)      { return mi_heap_strdup(mi_heap_main(), "the first call on this thread, 40 bytes...."); }
static void* first_free_null(void)        { mi_free(NULL); return mi_malloc(BLOCK_SIZE); }
static void* first_usable_size_null(void) { return (mi_usable_size(NULL) == 0 ? mi_malloc(BLOCK_SIZE) : NULL); }

typedef struct first_call_s {
  const char* name;
  void*     (*call)(void);
  bool        ok;
} first_call_t;

#define FIRST(name)  { #name, &first_##name, false }

static first_call_t first_calls[] = {
  FIRST(malloc), FIRST(zalloc), FIRST(calloc), FIRST(mallocn), FIRST(malloc_small), FIRST(zalloc_small), FIRST(malloc_large),
  FIRST(realloc), FIRST(reallocn), FIRST(reallocf), FIRST(rezalloc), FIRST(recalloc), FIRST(strdup), FIRST(strndup),
  FIRST(malloc_aligned), FIRST(zalloc_aligned), FIRST(calloc_aligned), FIRST(malloc_aligned_at), FIRST(malloc_aligned_big),
  FIRST(realloc_aligned), FIRST(rezalloc_aligned), FIRST(aligned_alloc), FIRST(memalign), FIRST(posix_memalign),
  FIRST(new), FIRST(new_nothrow), FIRST(new_n), FIRST(new_aligned), FIRST(new_realloc),
  FIRST(heap_malloc), FIRST(heap_realloc), FIRST(heap_strdup), FIRST(free_null), FIRST(usable_size_null),
};

static void run(first_call_t* first) {
  void* const p = first->call();
  if (p == NULL) return;
  memset(p, 0x5A, BLOCK_SIZE);
  first->ok = (mi_usable_size(p) >= BLOCK_SIZE);
  mi_free(p);
}

#if defined(_WIN32)
static DWORD WINAPI thread_entry(LPVOID arg) { run((first_call_t*)arg); return 0; }
static bool run_in_new_thread(first_call_t* first) {
  HANDLE thread = CreateThread(NULL, 0, &thread_entry, first, 0, NULL);
  if (thread == NULL) return false;
  WaitForSingleObject(thread, INFINITE);
  CloseHandle(thread);
  return true;
}
#else
static void* thread_entry(void* arg) { run((first_call_t*)arg); return NULL; }
static bool run_in_new_thread(first_call_t* first) {
  pthread_t thread;
  if (pthread_create(&thread, NULL, &thread_entry, first) != 0) return false;
  pthread_join(thread, NULL);
  return true;
}
#endif

int main(void) {
  int failed = 0;
  for (size_t i = 0; i < sizeof(first_calls) / sizeof(first_calls[0]); i++) {
    first_call_t* const first = &first_calls[i];
    // (printed before the call: what goes wrong here is a crash)
    printf("first call on a new thread: %s\n", first->name);
    fflush(stdout);
    if (!run_in_new_thread(first) || !first->ok) {
      printf("  failed\n");
      failed++;
    }
  }
  printf("test-thread-first-call: %s\n", (failed == 0 ? "ok" : "FAILED"));
  return (failed == 0 ? 0 : 1);
}
