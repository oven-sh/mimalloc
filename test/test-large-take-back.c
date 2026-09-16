/* ----------------------------------------------------------------------------
Copyright (c) 2018-2026 Microsoft Research, Daan Leijen
This is free software; you can redistribute it and/or modify it under the
terms of the MIT license.
-----------------------------------------------------------------------------*/

/* A large page (blocks of 96 KiB and up) is abandoned when it is full, and a thread that goes on allocating takes
   such a page back, its own or another thread's, before it extends the page it is in (`page.c:mi_page_queue_find_free_ex`).
   That search claims pages in the abandoned map of the arenas while other threads free into them, abandon more,
   are swept (they park) and end. Here producers allocate buffers in several large size classes and keep a few of
   their own, a consumer frees what they hand over, and producers come and go; every buffer carries a pattern that is
   checked before it is freed. It is for the thread sanitizer and the debug assertions to find what is wrong.

   > mimalloc-test-large-take-back
*/

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "mimalloc.h"

#if defined(_WIN32) || defined(__EMSCRIPTEN__)
int main(void) { printf("skipped (no pthreads)\n"); return 0; }
#else

#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <unistd.h>

#define PRODUCERS   (4)
#define ROUNDS      (6)      // each producer thread ends and is started again this often
#define PER_ROUND   (400)    // buffers a producer makes in a round
#define QUEUE       (256)
#define OWN         (24)     // buffers a producer keeps for itself at a time

typedef struct buffer_s { size_t size; uint8_t tag; } buffer_t;   // at the start of every block

static _Atomic(buffer_t*) queue[QUEUE];
static _Atomic(int)       producers_left;
static _Atomic(size_t)    bad;
static _Atomic(size_t)    freed_by_consumer;

static const size_t sizes[] = { 100 * 1024, 160 * 1024, 256 * 1024, 300 * 1024, 384 * 1024, 512 * 1024 };

static buffer_t* make(unsigned* rng, uint8_t tag) {
  *rng = *rng * 1664525u + 1013904223u;
  const size_t size = sizes[(*rng >> 16) % (sizeof(sizes) / sizeof(sizes[0]))];
  buffer_t* const b = (buffer_t*)mi_malloc(size);
  if (b == NULL) return NULL;
  b->size = size; b->tag = tag;
  uint8_t* const p = (uint8_t*)b;
  for (size_t o = sizeof(buffer_t); o < size; o += 4096) { p[o] = tag; }   // a byte on every page of it
  p[size - 1] = tag;
  return b;
}

static void check_and_free(buffer_t* b) {
  const uint8_t* const p = (const uint8_t*)b;
  bool ok = (b->size >= 96 * 1024 && b->size <= 512 * 1024 && p[b->size - 1] == b->tag);
  for (size_t o = sizeof(buffer_t); ok && o < b->size; o += 4096) { ok = (p[o] == b->tag); }
  if (!ok) { atomic_fetch_add(&bad, 1); }
  mi_free(b);
}

static void* producer(void* arg) {
  unsigned rng = (unsigned)(uintptr_t)arg * 2654435761u + 1;
  buffer_t* own[OWN] = { NULL };
  for (int i = 0; i < PER_ROUND; i++) {
    buffer_t* const b = make(&rng, (uint8_t)(i + (int)(uintptr_t)arg));
    if (b == NULL) continue;
    if ((i % 3) == 0) {          // keep it for a while: this thread frees it itself, into a page that may be abandoned by then
      const int k = (int)((rng >> 8) % OWN);
      if (own[k] != NULL) check_and_free(own[k]);
      own[k] = b;
    }
    else {                       // hand it over
      const int slot = (int)((rng >> 12) % QUEUE);
      buffer_t* const old = atomic_exchange(&queue[slot], b);
      if (old != NULL) check_and_free(old);
    }
    if ((i % 64) == 63) {        // a thread that parks is swept
      if (mi_on_thread_idle_start()) { usleep(2000); }
      mi_on_thread_idle_end();
    }
  }
  for (int k = 0; k < OWN; k++) { if (own[k] != NULL) check_and_free(own[k]); }
  atomic_fetch_sub(&producers_left, 1);
  return NULL;
}

static void* consumer(void* arg) {
  (void)arg;
  unsigned rng = 12345;
  while (atomic_load(&producers_left) > 0) {
    rng = rng * 1664525u + 1013904223u;
    buffer_t* const b = atomic_exchange(&queue[(rng >> 12) % QUEUE], NULL);
    if (b != NULL) { check_and_free(b); atomic_fetch_add(&freed_by_consumer, 1); }
    else { sched_yield(); }
  }
  for (int slot = 0; slot < QUEUE; slot++) {
    buffer_t* const b = atomic_exchange(&queue[slot], NULL);
    if (b != NULL) { check_and_free(b); atomic_fetch_add(&freed_by_consumer, 1); }
  }
  return NULL;
}

int main(void) {
  mi_option_set(mi_option_purge_holes_min_interval, 1);   // epochs of a millisecond: the sweeps of the parks above get to take and to keep
  atomic_store(&producers_left, PRODUCERS * ROUNDS);
  pthread_t c;
  if (pthread_create(&c, NULL, &consumer, NULL) != 0) { fprintf(stderr, "cannot start a thread\n"); return 1; }
  for (int round = 0; round < ROUNDS; round++) {
    pthread_t p[PRODUCERS];
    bool started[PRODUCERS];
    for (int i = 0; i < PRODUCERS; i++) { started[i] = (pthread_create(&p[i], NULL, &producer, (void*)(uintptr_t)(round * PRODUCERS + i + 1)) == 0); if (!started[i]) atomic_fetch_sub(&producers_left, 1); }
    for (int i = 0; i < PRODUCERS; i++) { if (started[i]) pthread_join(p[i], NULL); }
  }
  pthread_join(c, NULL);
  mi_collect(true);
  const size_t nbad = atomic_load(&bad);
  fprintf(stderr, "%d producers in %d rounds made %d buffers each; the consumer freed %zu; %zu had a wrong pattern\n", PRODUCERS, ROUNDS, PER_ROUND, atomic_load(&freed_by_consumer), nbad);
  if (nbad != 0) { fprintf(stderr, "FAILED\n"); return 1; }
  fprintf(stderr, "ok\n");
  return 0;
}

#endif
