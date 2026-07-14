/* ----------------------------------------------------------------------------
Copyright (c) 2026, Microsoft Research, Daan Leijen
This is free software; you can redistribute it and/or modify it under the
terms of the MIT license. A copy of the license can be found in the file
"LICENSE" at the root of this distribution.
-----------------------------------------------------------------------------*/

// `mi_collect_deferred` must return emptied pages to the arena and schedule their
// purge WITHOUT madvising on the calling thread; `mi_collect` may purge inline.
//
// Run with MIMALLOC_SCAVENGER=0 so no other thread can purge: RSS is then a direct,
// timing-free probe of who made the syscall. With the scavenger enabled this test
// is vacuous -- the scavenger would purge behind our back -- so it asserts the
// scavenger is off and fails loudly if the env did not take.

#include "mimalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures = 0;

static void check(const char* name, bool ok) {
  fprintf(stderr, "test: %s...  %s\n", name, ok ? "ok." : "FAILED");
  if (!ok) failures++;
}

#if defined(__linux__)

static size_t rss_bytes(void) {
  FILE* f = fopen("/proc/self/statm", "r");
  if (f == NULL) return 0;
  long sz = 0, res = 0;
  if (fscanf(f, "%ld %ld", &sz, &res) != 2) res = 0;
  fclose(f);
  return (size_t)res * (size_t)sysconf(_SC_PAGESIZE);
}

#define MI_TEST_MB(x)  ((x) / (1024 * 1024))

int main(void) {
  // The whole test rests on nothing else being able to purge.
  if (mi_option_is_enabled(mi_option_scavenger)) {
    fprintf(stderr, "FAILED: run this with MIMALLOC_SCAVENGER=0; with the scavenger on the RSS probe is vacuous\n");
    return 1;
  }
  const long delay = mi_option_get(mi_option_purge_delay) * mi_option_get(mi_option_arena_purge_mult);
  if (delay <= 0) {
    fprintf(stderr, "FAILED: purge_delay*arena_purge_mult must be > 0 for this test (got %ld)\n", delay);
    return 1;
  }

  enum { N = 512, BLOCK = 1024 * 1024 };   // 512MB, big enough that a purge is unmissable in RSS
  void** p = (void**)malloc(N * sizeof(void*));
  if (p == NULL) return 1;
  for (int i = 0; i < N; i++) {
    p[i] = mi_malloc(BLOCK);
    if (p[i] == NULL) return 1;
    memset(p[i], 1, BLOCK);              // fault it in
  }
  const size_t rss_live = rss_bytes();
  check("allocation is resident", MI_TEST_MB(rss_live) >= (N / 2));

  for (int i = 0; i < N; i++) mi_free(p[i]);

  // Hand the emptied pages back to the arena and schedule their purge. No madvise here.
  mi_collect_deferred();
  const size_t rss_after_deferred = rss_bytes();

  // Wait past the purge delay so the purge is unambiguously DUE. mi_collect_deferred
  // must STILL not make the syscall -- that is the property under test.
  usleep((useconds_t)(delay * 1000) + 250 * 1000);
  mi_collect_deferred();
  const size_t rss_deferred_when_due = rss_bytes();

  check("mi_collect_deferred does not purge (not yet due)",
        MI_TEST_MB(rss_after_deferred) + 64 >= MI_TEST_MB(rss_live));
  check("mi_collect_deferred does not purge (purge IS due)",
        MI_TEST_MB(rss_deferred_when_due) + 64 >= MI_TEST_MB(rss_live));

  // mi_collect does the arena collect, so it purges inline -- proving the pages really
  // were freed and scheduled by the deferred collects above, and that the ONLY thing
  // the deferred variant withholds is the syscall.
  mi_collect(false);
  const size_t rss_after_collect = rss_bytes();
  check("mi_collect purges what mi_collect_deferred scheduled",
        MI_TEST_MB(rss_after_collect) + (N / 2) <= MI_TEST_MB(rss_deferred_when_due));

  fprintf(stderr,
          "\nRSS: live %zuMB -> deferred %zuMB -> deferred(due) %zuMB -> mi_collect %zuMB\n",
          MI_TEST_MB(rss_live), MI_TEST_MB(rss_after_deferred),
          MI_TEST_MB(rss_deferred_when_due), MI_TEST_MB(rss_after_collect));
  free(p);
  fprintf(stderr, "%s\n", failures == 0 ? "all tests passed." : "SOME TESTS FAILED.");
  return failures == 0 ? 0 : 1;
}

#else

// The RSS probe is /proc-based; the behaviour is not platform specific, so just
// exercise the entry point elsewhere rather than skipping silently.
int main(void) {
  void* p = mi_malloc(1024 * 1024);
  memset(p, 1, 1024 * 1024);
  mi_free(p);
  mi_collect_deferred();
  check("mi_collect_deferred runs", true);
  return failures == 0 ? 0 : 1;
}

#endif
