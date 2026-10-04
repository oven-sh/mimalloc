/* ----------------------------------------------------------------------------
Copyright (c) 2026, Microsoft Research, Daan Leijen
This is free software; you can redistribute it and/or modify it under the
terms of the MIT license. A copy of the license can be found in the file
"LICENSE" at the root of this distribution.
-----------------------------------------------------------------------------*/

// `purge_delay` is a minimum age: a purge pass leaves the slices that were freed since the pass before it.
// These tests run the passes themselves (`mi_collect(false)` in a process without a scavenger), and
// use counters instead of RSS.

#include "mimalloc.h"
#include "mimalloc-stats.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
static void msleep(unsigned ms) { Sleep(ms); }
#else
#include <time.h>
static void msleep(unsigned ms) {
  struct timespec ts;
  ts.tv_sec = ms / 1000;
  ts.tv_nsec = (long)(ms % 1000) * 1000000;
  nanosleep(&ts, NULL);
}
#endif

static int failures = 0;

static void check(const char* name, bool ok) {
  fprintf(stderr, "test: %s...  %s\n", name, ok ? "ok." : "FAILED");
  if (!ok) failures++;
}

static size_t purged(void) {
  mi_stats_t_decl(stats);
  mi_subproc_stats_get_exclusive(mi_subproc_main(), &stats);
  return (size_t)stats.purged.total;
}

#define MiB  ((size_t)1024 * 1024)

static void alloc_blocks(void** blocks, int count, size_t size) {
  for (int i = 0; i < count; i++) {
    blocks[i] = mi_malloc(size);
    if (blocks[i] != NULL) { memset(blocks[i], 1, size); }
  }
}

// The delay is long so a slow machine does not turn the first pass into a late one.
static void test_minimum_age(void) {
  enum { COUNT = 16 };
  void* blocks[COUNT];
  mi_option_set(mi_option_purge_delay, 4000);   // a pass every 2s
  alloc_blocks(blocks, COUNT, MiB);
  const size_t purged0 = purged();
  mi_free(blocks[0]);                           // schedules the first pass
  msleep(2100);
  for (int i = 1; i < COUNT; i++) { mi_free(blocks[i]); }
  mi_collect(false);
  check("a pass leaves what was just freed", purged() - purged0 < MiB);
  msleep(2100);
  mi_collect(false);
  check("and the next pass purges it", purged() - purged0 >= (COUNT - 1) * MiB);
}

// Without a scavenger there may be only one pass, long after the free.
static void test_late_pass(void) {
  enum { COUNT = 16 };
  void* blocks[COUNT];
  mi_option_set(mi_option_purge_delay, 100);
  alloc_blocks(blocks, COUNT, MiB);
  const size_t purged0 = purged();
  for (int i = 0; i < COUNT; i++) { mi_free(blocks[i]); }
  msleep(300);
  mi_collect(false);
  check("a late pass purges everything", purged() - purged0 >= (COUNT - 1) * MiB);
}

// With a minimal purge size a range is purged as a whole, also if its slices were freed in between different passes.
static void test_minimal_purge_size(void) {
  enum { COUNT = 64 };
  static void* blocks[COUNT];
  mi_option_set(mi_option_purge_delay, 100);
  mi_option_set(mi_option_minimal_purge_size, 2048);  // KiB
  alloc_blocks(blocks, COUNT, MiB);
  const size_t purged0 = purged();
  for (int i = 0; i < COUNT; i += 2) { mi_free(blocks[i]); }
  msleep(60);
  mi_collect(false);
  for (int i = 1; i < COUNT; i += 2) { mi_free(blocks[i]); }
  for (int i = 0; i < 4; i++) {
    msleep(60);
    mi_collect(false);
  }
  // (the first and the last range can be partial)
  check("ranges that were freed in two parts are purged", purged() - purged0 >= (COUNT - 8) * MiB);
  mi_option_set(mi_option_minimal_purge_size, 64);
}

int main(void) {
  mi_option_set(mi_option_arena_purge_mult, 1);
  mi_option_set(mi_option_minimal_purge_size, 64);  // KiB: one slice
  test_minimum_age();
  test_late_pass();
  test_minimal_purge_size();
  return (failures > 0 ? 1 : 0);
}
