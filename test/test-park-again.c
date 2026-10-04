/* ----------------------------------------------------------------------------
Copyright (c) 2026, Microsoft Research, Daan Leijen
This is free software; you can redistribute it and/or modify it under the
terms of the MIT license. A copy of the license can be found in the file
"LICENSE" at the root of this distribution.
-----------------------------------------------------------------------------*/

// A thread parks and is swept, and stays parked. Then another thread frees its blocks. Only the owner (or the
// scavenger in its place) can collect those, so the scavenger has to come back for a thread that stays parked.

#if defined(_WIN32)
#include <stdio.h>
int main(void) {
  printf("test-park-again: skipped on Windows (uses pthreads)\n");
  return 0;
}
#else

#include "mimalloc.h"
#include "mimalloc-stats.h"
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define COUNT       (128 * 1024)
#define BLOCK_SIZE  (256)
#define TOTAL       ((size_t)COUNT * BLOCK_SIZE)
#define BOUND_SECS  (75)     // it is swept again after 30s, at a safety timeout of the scavenger (also 30s)

static void* blocks[COUNT];

static void msleep(unsigned ms) {
  struct timespec ts;
  ts.tv_sec = ms / 1000;
  ts.tv_nsec = (long)(ms % 1000) * 1000000;
  nanosleep(&ts, NULL);
}

static size_t purged(void) {
  mi_stats_t_decl(stats);
  mi_subproc_stats_get_exclusive(mi_subproc_main(), &stats);
  return (size_t)stats.purged.total;
}

static void* free_blocks(void* arg) {
  (void)arg;
  for (int i = 0; i < COUNT; i++) {
    mi_free(blocks[i]);
  }
  return NULL;
}

int main(void) {
  for (int i = 0; i < COUNT; i++) {
    blocks[i] = mi_malloc(BLOCK_SIZE);
    memset(blocks[i], 1, BLOCK_SIZE);
  }
  // no page is full, as a full page is abandoned by a free from another thread and needs no owner
  for (int i = 0; i < COUNT; i += 8) {
    mi_free(blocks[i]);
    blocks[i] = NULL;
  }
  if (!mi_on_thread_idle_start()) {
    fprintf(stderr, "test-park-again: skipped (no scavenger)\n");
    return 0;
  }
  msleep(1000);  // swept, with the sweeps that follow up on it

  const size_t purged0 = purged();
  pthread_t thread;
  pthread_create(&thread, NULL, &free_blocks, NULL);
  pthread_join(thread, NULL);

  bool ok = false;
  for (int i = 0; i < BOUND_SECS * 10 && !ok; i++) {
    msleep(100);
    ok = (purged() - purged0 >= TOTAL / 2);
  }
  mi_on_thread_idle_end();
  fprintf(stderr, "test: what another thread frees is returned while the owner stays parked...  %s\n", ok ? "ok." : "FAILED");
  return (ok ? 0 : 1);
}

#endif
