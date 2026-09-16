/* ----------------------------------------------------------------------------
Copyright (c) 2018-2026 Microsoft Research, Daan Leijen
This is free software; you can redistribute it and/or modify it under the
terms of the MIT license.
-----------------------------------------------------------------------------*/

/* What the idle sweeps of a thread leave under `purge_holes_large_floor` is for a thread that is swept now and then.
   One that stays parked gives it back `purge_holes_large_floor_epochs` intervals after the sweep that left it: the
   scavenger comes back for it once, by the clock, and that sweep leaves nothing. A thread that parks again within that
   time keeps it (its pages age by the epochs of the sweep, as before).

   The options are set first thing in `main`, before anything of the allocator runs on another thread, and never
   again: the scavenger reads them. (This case was in `test-park-handoff.c`, where it set and restored them while the
   scavenger ran, which the thread sanitizer reported now and then.)

   > mimalloc-test-floor-idle
*/

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "mimalloc.h"

#if defined(_WIN32) || defined(__EMSCRIPTEN__)
int main(void) { printf("skipped (uses usleep and /proc)\n"); return 0; }
#else

#include <unistd.h>
#include <dirent.h>
#include <time.h>

#define INTERVAL_MS  (10)
#define EPOCHS       (100)     // so many intervals: a second
enum { BLOCKS = 18, BLOCK = 300 * 1024 };   // a page that fills up (and is abandoned for that), and half of one that stays the thread's own

static int failures = 0;
static void check(const char* name, bool ok) {
  fprintf(stderr, "test: %s...  %s\n", name, ok ? "ok." : "FAILED");
  if (!ok) failures++;
}

static long msecs_now(void) {
  struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (long)t.tv_sec * 1000 + (long)(t.tv_nsec / 1000000);
}

static size_t purged_now(void) {
  mi_purge_holes_stats_t h; mi_purge_holes_stats_get(&h); return h.purged_bytes;
}

// how often the scavenger thread was woken so far; -1 where that cannot be told (not Linux, or it has not started)
static long scavenger_wakeups(void) {
  long total = -1;
  #if defined(__linux__)
  DIR* const dir = opendir("/proc/self/task");
  if (dir == NULL) return -1;
  for (struct dirent* e = readdir(dir); e != NULL; e = readdir(dir)) {
    const long tid = atol(e->d_name);
    if (tid <= 0) continue;
    char path[64], line[128];
    snprintf(path, sizeof(path), "/proc/self/task/%ld/comm", tid);
    FILE* f = fopen(path, "r");
    if (f == NULL) continue;
    const bool is_scavenger = (fgets(line, sizeof(line), f) != NULL && strncmp(line, "mi-scavenger", 12) == 0);
    fclose(f);
    if (!is_scavenger) continue;
    snprintf(path, sizeof(path), "/proc/self/task/%ld/status", tid);
    f = fopen(path, "r");
    if (f == NULL) continue;
    while (fgets(line, sizeof(line), f) != NULL) { long n; if (sscanf(line, "voluntary_ctxt_switches: %ld", &n) == 1) total = n; }
    fclose(f);
  }
  closedir(dir);
  #endif
  return total;
}

static size_t make_free_blocks(void* p[BLOCKS]) {
  for (int i = 0; i < BLOCKS; i++) { p[i] = mi_malloc(BLOCK); if (p[i] != NULL) memset(p[i], 5, BLOCK); }
  size_t freed = 0;
  for (int i = 0; i < BLOCKS; i++) { if ((i % 3) != 0 && p[i] != NULL) { mi_free(p[i]); p[i] = NULL; freed++; } }
  return freed;
}

int main(void) {
  mi_option_set(mi_option_purge_holes_min_interval, INTERVAL_MS);
  mi_option_set(mi_option_purge_holes_large_floor, 8 * 1024);    // KiB
  mi_option_set(mi_option_purge_holes_large_floor_epochs, EPOCHS);
  if (!mi_option_is_enabled(mi_option_purge_holes)) { fprintf(stderr, "purge_holes is off: nothing to test\n"); return 0; }

  // 1. a thread that parks again and again keeps what is under the floor, also past a second
  void* p[BLOCKS];
  size_t freed = make_free_blocks(p);
  size_t before = purged_now();
  bool parked_all = true;
  for (int i = 0; i < 12; i++) {   // 1.2 s of parks of 100 ms: every one is within a second of the one before, and the epochs that their sweeps move are fewer than EPOCHS
    if (mi_on_thread_idle_start()) { usleep(100 * 1000); } else { parked_all = false; }
    mi_on_thread_idle_end();
    void* const q = mi_malloc(BLOCK); if (q != NULL) { memset(q, 6, 64); mi_free(q); }   // and it uses its pages in between
  }
  const size_t busy = purged_now();
  fprintf(stderr, "  %zu blocks of %d KiB freed in large pages: %zu KiB of them purged after 12 parks in 1.2 s\n", freed, BLOCK / 1024, (busy - before) / 1024);
  if (parked_all) check("a thread that parks now and then keeps its free blocks under the floor", busy - before < 2 * (size_t)BLOCK);

  // 2. one that stays parked keeps them for a second, and then the scavenger comes back for them, once
  const size_t target = before + freed * (BLOCK - 16 * 1024);
  const long park_at = msecs_now();
  if (mi_on_thread_idle_start()) {
    usleep(300 * 1000);   // the sweeps of the hold are over
    const size_t held = purged_now();
    usleep(400 * 1000);   // not yet a second since the last of them
    const size_t still = purged_now();
    const long still_at = msecs_now() - park_at;   // (unless this machine was stalled: then there is nothing to say about it)
    size_t gone = still;
    int waited = 0;
    for (; waited < 4000 && gone < target; waited += 20) { usleep(20 * 1000); gone = purged_now(); }
    const long wakeups = scavenger_wakeups();
    usleep(2500 * 1000);  // and nothing more after that: two and a half times as long again
    const long wakeups_later = scavenger_wakeups();
    mi_on_thread_idle_end();
    fprintf(stderr, "  parked for good: %zu KiB purged at 300 ms, %zu KiB at 700 ms, %zu KiB %d ms later; the scavenger was woken %ld times in the 2.5 s after that\n",
            (held - before) / 1024, (still - before) / 1024, (gone - before) / 1024, waited, wakeups_later - wakeups);
    check("a thread that stays parked keeps them at first", held - before < 2 * (size_t)BLOCK);
    if (still_at < 900) check("and for purge_holes_large_floor_epochs intervals", still == held);
    check("and gives them back then, with nobody else that parks", gone >= target);
    if (wakeups >= 0 && wakeups_later >= 0) check("after which nothing comes back for it", wakeups_later - wakeups <= 1);
  }
  else {
    mi_on_thread_idle_end();
    fprintf(stderr, "  (no scavenger to hand the thread to: skipped)\n");
  }
  for (int i = 0; i < BLOCKS; i++) { if (p[i] != NULL) mi_free(p[i]); }
  fprintf(stderr, "\n%s\n", failures == 0 ? "all tests passed." : "SOME TESTS FAILED.");
  return failures == 0 ? 0 : 1;
}

#endif
