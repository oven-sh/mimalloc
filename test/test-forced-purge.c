/* ----------------------------------------------------------------------------
Copyright (c) 2026, Microsoft Research, Daan Leijen
This is free software; you can redistribute it and/or modify it under the
terms of the MIT license. A copy of the license can be found in the file
"LICENSE" at the root of this distribution.
-----------------------------------------------------------------------------*/

// `mi_collect(true)` purges every free arena range before it returns.
//
// Only one thread runs a purge pass at a time, and with a scavenger thread there often is one in a
// pass: it takes the freed ranges `purge_delay` after they were freed. Its pass is not forced, so it
// is no substitute for the caller's, and a caller of `mi_collect(true)` that found the scavenger in
// there used to return with nothing purged at all. Whoever forces a collect reads the footprint next.
//
// Each round frees a few hundred MiB, waits for the scavenger to start on them (`arena_purges` counts
// the arenas a pass went into), and forces a collect while the scavenger is in the middle of that.
// Everything the round freed has to be counted in `purged` when the collect returns. Counters, not
// RSS: they read the same on every platform and in every build.

#include "mimalloc.h"
#include "mimalloc-stats.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static int failures = 0;

static void check(const char* name, bool ok) {
  fprintf(stderr, "test: %s...  %s\n", name, ok ? "ok." : "FAILED");
  if (!ok) failures++;
}

#define MAX_BLOCKS  (32)
#define BLOCK_SIZE  ((size_t)8 * 1024 * 1024)
#define ROUNDS      (4)

// the counters of the subprocess itself: read without a lock, and without touching our theap
static void counters(size_t* purged, size_t* arena_purges) {
  mi_stats_t_decl(stats);
  mi_subproc_stats_get_exclusive(mi_subproc_main(), &stats);
  *purged = (size_t)stats.purged.total;
  *arena_purges = (size_t)stats.arena_purges.total;
}

int main(void) {
  if (!mi_option_is_enabled(mi_option_scavenger) || mi_option_get(mi_option_purge_delay) <= 0) {
    fprintf(stderr, "test-forced-purge: skipped (no scavenger, or no purge delay)\n");
    return 0;
  }
  // the scavenger starts when a second thread initializes or a thread first parks
  void* warm = mi_malloc(64); mi_free(warm);
  if (mi_on_thread_idle_start()) { mi_on_thread_idle_end(); }

  // 256 MiB a round, so that a pass over it holds the purge guard for milliseconds (64 MiB where address space is scarce)
  const int BLOCKS = (sizeof(void*) >= 8 ? MAX_BLOCKS : MAX_BLOCKS / 4);
  static void* blocks[MAX_BLOCKS];
  int in_a_pass = 0;
  for (int round = 0; round < ROUNDS; round++) {
    for (int i = 0; i < BLOCKS; i++) {
      blocks[i] = mi_malloc(BLOCK_SIZE);
      if (blocks[i] == NULL) { fprintf(stderr, "test-forced-purge: out of memory\n"); return 1; }
      memset(blocks[i], 1, BLOCK_SIZE);   // resident, so purging it takes the scavenger a while
    }
    size_t purged0, passes0;
    counters(&purged0, &passes0);
    for (int i = 0; i < BLOCKS; i++) { mi_free(blocks[i]); }   // back to the arena, purge scheduled

    // Wait for the scavenger to go into an arena. If it never does (bounded), the forced collect
    // below has all of it to purge by itself, which has to work just as well.
    size_t purged1, passes1;
    const time_t deadline = time(NULL) + 10;
    do { counters(&purged1, &passes1); } while (passes1 == passes0 && time(NULL) < deadline);
    if (passes1 != passes0) { in_a_pass++; }

    mi_collect(true);

    counters(&purged1, &passes1);
    char name[128];
    snprintf(name, sizeof(name), "round %d: a forced collect leaves nothing that was freed unpurged (%zu of %zu MiB)",
             round, (purged1 - purged0) / (1024 * 1024), ((size_t)BLOCKS * BLOCK_SIZE) / (1024 * 1024));
    check(name, purged1 - purged0 >= (size_t)BLOCKS * BLOCK_SIZE);
  }
  fprintf(stderr, "test-forced-purge: %d of %d forced collects came while the scavenger was purging\n", in_a_pass, ROUNDS);
  return failures;
}
