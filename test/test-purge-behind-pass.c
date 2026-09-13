/* ----------------------------------------------------------------------------
Copyright (c) 2026, Microsoft Research, Daan Leijen
This is free software; you can redistribute it and/or modify it under the
terms of the MIT license. A copy of the license can be found in the file
"LICENSE" at the root of this distribution.
-----------------------------------------------------------------------------*/

// What a purge pass leaves behind is purged by a later pass, without a thread that parks or forces a collect.
//
// An arena has an expire and so has the subprocess. A free arms the arena if it is not armed yet, and only then
// arms the subprocess, which is what the scavenger sleeps on (and what an allocating thread polls without one).
// A pass resets the expire of an arena when it goes into it. So an arena can be armed when a pass ends: a free
// came in behind the pass, or the arena was not yet due. The subprocess has to be armed as well then. If it is
// not, the arena stays as it is: every later free into it sees an armed arena and does not look further.
//
// 1. A free behind a pass. The pass used to keep the old expire of the subprocess until its end (and the scavenger
//    cleared it after that), so a free during the pass found it set and armed the arena alone. Each round frees a
//    first batch, waits for the scavenger to go into an arena for it (`arena_purges` counts those), and frees a
//    second batch while the scavenger is in there. All of both has to be counted in `purged` within the bound.
// 2. A fork in the middle of a pass. The child has no thread that ends the pass, so an arena that the pass had yet
//    to come to stays armed there with the expire of the subprocess reset. An arena of its own is freed into a
//    little later than the main one, so that the pass over the main one finds it not yet due.
//
// Counters, not RSS: they read the same on every platform and in every build.

#include "mimalloc.h"
#include "mimalloc-stats.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#if !defined(_WIN32)
#include <unistd.h>
#include <sys/wait.h>
#endif

static int failures = 0;

static void check(const char* name, bool ok) {
  fprintf(stderr, "test: %s...  %s\n", name, ok ? "ok." : "FAILED");
  if (!ok) failures++;
}

#define MAX_FIRST   (32)
#define MAX_SECOND  (8)
#define BLOCK_SIZE  ((size_t)8 * 1024 * 1024)
#define ROUNDS      (3)
#define BOUND_SECS  (10)    // a pass comes `purge_delay` (100ms) after a free

// the counters of the subprocess itself: read without a lock, and without touching our theap
static void counters(size_t* purged, size_t* arena_purges) {
  mi_stats_t_decl(stats);
  mi_subproc_stats_get_exclusive(mi_subproc_main(), &stats);
  *purged = (size_t)stats.purged.total;
  *arena_purges = (size_t)stats.arena_purges.total;
}

static bool alloc_blocks(void** blocks, int count) {
  for (int i = 0; i < count; i++) {
    blocks[i] = mi_malloc(BLOCK_SIZE);
    if (blocks[i] == NULL) return false;
    memset(blocks[i], 1, BLOCK_SIZE);   // resident, so purging it takes the scavenger a while
  }
  return true;
}

// Spin until the scavenger goes into an arena. Only spin: a park or a collect would purge as well.
static bool wait_for_pass(size_t passes0) {
  size_t purged, passes;
  const time_t deadline = time(NULL) + BOUND_SECS;
  do { counters(&purged, &passes); } while (passes == passes0 && time(NULL) < deadline);
  return (passes != passes0);
}

static void test_free_behind_pass(int first_count, int second_count) {
  static void* first[MAX_FIRST];
  static void* second[MAX_SECOND];
  const size_t total = (size_t)(first_count + second_count) * BLOCK_SIZE;
  for (int round = 0; round < ROUNDS && failures == 0; round++) {
    if (!alloc_blocks(first, first_count) || !alloc_blocks(second, second_count)) {
      check("out of memory", false);
      return;
    }
    size_t purged0, passes0;
    counters(&purged0, &passes0);
    for (int i = 0; i < first_count; i++) { mi_free(first[i]); }     // back to the arena, purge scheduled

    char name[160];
    snprintf(name, sizeof(name), "round %d: the scavenger starts on what was freed", round);
    check(name, wait_for_pass(passes0));

    for (int i = 0; i < second_count; i++) { mi_free(second[i]); }   // behind the pass, or in front of it

    size_t purged1, passes1;
    const time_t deadline = time(NULL) + BOUND_SECS;
    do { counters(&purged1, &passes1); } while (purged1 - purged0 < total && time(NULL) < deadline);
    snprintf(name, sizeof(name), "round %d: what was freed during a pass is purged as well (%zu of %zu MiB)",
             round, (purged1 - purged0) / (1024 * 1024), total / (1024 * 1024));
    check(name, purged1 - purged0 >= total);
  }
}

#if defined(_WIN32)
static void test_fork_in_pass(int first_count) { (void)first_count; }   // no fork
#else
static void test_fork_in_pass(int first_count) {
  // an arena of our own (the second one, so a pass comes to it after the main one), and a heap that allocates in it only
  mi_arena_id_t arena_id;
  if (mi_reserve_os_memory_ex(4 * BLOCK_SIZE, false /* commit */, false /* allow large */, true /* exclusive */, &arena_id) != 0) {
    fprintf(stderr, "test-purge-behind-pass: no fork test (could not reserve an arena)\n");
    return;
  }
  mi_heap_t* const heap = mi_heap_new_in_arena(arena_id);
  void* const late = (heap == NULL ? NULL : mi_heap_malloc(heap, BLOCK_SIZE));
  static void* first[MAX_FIRST];
  if (late == NULL || !alloc_blocks(first, first_count)) {
    check("out of memory", false);
    return;
  }
  memset(late, 1, BLOCK_SIZE);
  mi_collect(false);   // nothing is left for the child to free into the main arena: that would arm it, and the subprocess with it

  size_t purged0, passes0;
  counters(&purged0, &passes0);
  for (int i = 0; i < first_count; i++) { mi_free(first[i]); }   // arms the main arena and the subprocess
  // ..and our arena about half a purge delay later: a pass that is due for the main arena finds ours not yet due
  const long half_delay_ms = mi_option_get(mi_option_purge_delay) / 2;
  struct timespec ts = { (time_t)(half_delay_ms / 1000), (long)((half_delay_ms % 1000) * 1000000L) };
  nanosleep(&ts, NULL);
  mi_free(late);
  const bool in_pass = wait_for_pass(passes0);   // the scavenger is in the main arena; it has yet to come to ours
  size_t purged_fork, passes_fork;
  counters(&purged_fork, &passes_fork);

  const pid_t pid = fork();
  if (pid == 0) {
    // Poll as an allocating thread does (there is no scavenger here): a pass goes into our arena once it is due.
    // The main arena had its expire reset by the pass that the fork cut off, so it is not counted again.
    size_t purged, passes;
    const time_t deadline = time(NULL) + BOUND_SECS;
    do { mi_heap_collect(heap, false); counters(&purged, &passes); } while (passes == passes_fork && time(NULL) < deadline);
    _exit(passes != passes_fork ? 0 : 1);
  }
  int status = -1;
  if (pid > 0) { waitpid(pid, &status, 0); }
  char name[160];
  snprintf(name, sizeof(name), "a fork %s a pass: the child purges the arena that the pass had yet to come to", in_pass ? "in" : "outside of");
  check(name, pid > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0);
  mi_heap_delete(heap);
}
#endif

int main(void) {
  if (!mi_option_is_enabled(mi_option_scavenger) || mi_option_get(mi_option_purge_delay) <= 0) {
    fprintf(stderr, "test-purge-behind-pass: skipped (no scavenger, or no purge delay)\n");
    return 0;
  }
  // The rounds count bytes. With transparent huge pages left whole (`MI_ALLOW_THP=FULL`) a pass purges aligned 2 MiB
  // units only and leaves the edges of each freed run for later, so purge by the slice here (the size is rounded up to an
  // OS page, which is never more than a slice).
  mi_option_set(mi_option_minimal_purge_size, 4 /* KiB */);
  // the scavenger starts when a second thread initializes or a thread first parks
  void* warm = mi_malloc(64); mi_free(warm);
  if (mi_on_thread_idle_start()) { mi_on_thread_idle_end(); }

  // 256 MiB first, so that the pass over it takes milliseconds (64 MiB where address space is scarce)
  const int first_count  = (sizeof(void*) >= 8 ? MAX_FIRST  : MAX_FIRST / 4);
  const int second_count = (sizeof(void*) >= 8 ? MAX_SECOND : MAX_SECOND / 4);
  test_free_behind_pass(first_count, second_count);
  test_fork_in_pass(first_count);
  return failures;
}
