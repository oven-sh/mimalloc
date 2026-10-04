/* ----------------------------------------------------------------------------
Copyright (c) 2026, Microsoft Research, Daan Leijen
This is free software; you can redistribute it and/or modify it under the
terms of the MIT license. A copy of the license can be found in the file
"LICENSE" at the root of this distribution.
-----------------------------------------------------------------------------*/

// An allocation takes a resident free block before one that an idle sweep purged.
//
// The heap is fragmented and swept. One block in 8 is live, so half of the OS pages are purged and the other half
// has resident free blocks, in every page. Then each "request" allocates some blocks and frees them again: there
// are more than enough resident blocks for that.

#include "mimalloc.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define BLOCK_SIZE  (1024)
#define COUNT       (32 * 1024)   // 32 MiB
#define REQUEST     (500)
#define REQUESTS    (20)

static void* blocks[COUNT];
static void* request[REQUEST];
static int failures = 0;

static void check(const char* name, bool ok) {
  fprintf(stderr, "test: %s...  %s\n", name, ok ? "ok." : "FAILED");
  if (!ok) failures++;
}

static size_t reused(void) {
  mi_purge_holes_stats_t stats;
  mi_purge_holes_stats_get(&stats);
  return stats.reuse_calls;
}

static void do_request(void) {
  for (int i = 0; i < REQUEST; i++) {
    request[i] = mi_malloc(BLOCK_SIZE);
    memset(request[i], 2, BLOCK_SIZE);
  }
  for (int i = 0; i < REQUEST; i++) {
    mi_free(request[i]);
  }
}

int main(void) {
  mi_option_set(mi_option_purge_holes_min_interval, 0);
  for (int i = 0; i < COUNT; i++) {
    blocks[i] = mi_malloc(BLOCK_SIZE);
    memset(blocks[i], 1, BLOCK_SIZE);
  }
  for (int i = 0; i < COUNT; i++) {
    if (i % 8 != 0) {
      mi_free(blocks[i]);
      blocks[i] = NULL;
    }
  }
  mi_on_thread_idle();
  mi_purge_holes_stats_t stats;
  mi_purge_holes_stats_get(&stats);
  if (stats.discard_calls == 0) {
    fprintf(stderr, "test-purge-resident: skipped (nothing is discarded here)\n");
    return 0;
  }

  const size_t reused0 = reused();
  for (int i = 0; i < REQUESTS; i++) {
    do_request();
  }
  fprintf(stderr, "  purged runs that were taken back: %zu\n", reused() - reused0);
  check("the requests take resident blocks", reused() - reused0 <= REQUESTS / 4);

  // the live blocks are as they were
  bool intact = true;
  for (int i = 0; i < COUNT; i += 8) {
    const unsigned char* p = (const unsigned char*)blocks[i];
    for (int j = 0; j < BLOCK_SIZE; j++) {
      if (p[j] != 1) { intact = false; }
    }
    mi_free(blocks[i]);
  }
  check("live blocks are intact", intact);
  return (failures > 0 ? 1 : 0);
}
