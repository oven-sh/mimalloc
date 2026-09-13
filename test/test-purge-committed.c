/* ----------------------------------------------------------------------------
Copyright (c) 2026, Microsoft Research, Daan Leijen
This is free software; you can redistribute it and/or modify it under the
terms of the MIT license. A copy of the license can be found in the file
"LICENSE" at the root of this distribution.
-----------------------------------------------------------------------------*/

// The `committed` stat comes back down when a purge returns a range to the OS, and reads the same
// after every purge/reuse cycle.
//
// On an OS with overcommit (Linux, macOS) an arena is committed when it is reserved and the stat
// does not count that: a slice is counted the first time it is handed out. A purge on those
// systems (`MADV_DONTNEED`) leaves the range committed and the arena clears its dirty bits, so the
// next allocation of the range counts its slices as handed out for the first time, again. The purge
// has to debit what that allocation credits, or `committed` grows by the size of every cycle and
// never comes down: bun saw 28 GB "committed" against 0.6 GB resident after 8 days.
//
// `MIMALLOC_PAGE_COMMIT_ON_DEMAND=1` covers the other direction: an allocation that does not ask
// for commit, on a range that is committed already, has to credit the same slices, or the debit at
// the purge walks the stat below zero.

#include "mimalloc.h"
#include "mimalloc-stats.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static int failures = 0;

static void check(const char* name, bool ok) {
  fprintf(stderr, "test: %s...  %s\n", name, ok ? "ok." : "FAILED");
  if (!ok) failures++;
}

#define MiB         ((size_t)1024 * 1024)
#define ROUNDS      (8)
#define BLOCKS      (16)
#define SLACK       (2 * MiB)   // page meta data and the blocks the allocator keeps for itself

static int64_t committed(void) {
  mi_stats_t_decl(stats);
  mi_subproc_stats_get_exclusive(mi_subproc_main(), &stats);
  return stats.committed.current;
}

// allocate `BLOCKS` blocks of `size`, write them, free them, and purge: `committed` has to be back
// where it started, and the same in every round
static void cycle(const char* name, size_t size) {
  void* blocks[BLOCKS];
  // let the first round take its pages and meta data: the rounds after it reuse them
  for (int i = 0; i < BLOCKS; i++) { blocks[i] = mi_malloc(size); memset(blocks[i], 1, size); }
  for (int i = 0; i < BLOCKS; i++) { mi_free(blocks[i]); }
  mi_collect(true);
  const int64_t base = committed();

  bool flat = true;
  bool down = true;
  bool nonneg = (base >= 0);
  int64_t held0 = 0;
  for (int round = 0; round < ROUNDS; round++) {
    for (int i = 0; i < BLOCKS; i++) { blocks[i] = mi_malloc(size); memset(blocks[i], 1, size); }
    const int64_t held = committed();
    if (round == 0) { held0 = held; }
    for (int i = 0; i < BLOCKS; i++) { mi_free(blocks[i]); }
    mi_collect(true);
    const int64_t after = committed();
    fprintf(stderr, "  %s round %d: held %lld KiB, after purge %lld KiB (base %lld KiB)\n",
            name, round, (long long)(held / 1024), (long long)(after / 1024), (long long)(base / 1024));
    if (held > held0 + (int64_t)SLACK) { flat = false; }   // grows with every cycle
    if (after > base + (int64_t)SLACK) { down = false; }   // the purge debited nothing
    if (held < 0 || after < 0) { nonneg = false; }         // debited more than was credited
  }
  #if !defined(__linux__)
  // Only a purge that zero-fills on the next access (Linux `MADV_DONTNEED`; `MADV_ZERO` on a recent
  // macOS) un-dirties the range. Elsewhere the stat is a high-water mark by design: it does not
  // come down after the purge, but it does not grow either.
  down = true;
  #endif
  check(name, flat && down && nonneg);
}

int main(void) {
  if (mi_option_get(mi_option_purge_delay) < 0) {
    fprintf(stderr, "test-purge-committed: skipped (purging is off)\n");
    return 0;
  }
  mi_option_set(mi_option_purge_delay, 0);                    // a free purges at once
  mi_option_set(mi_option_minimal_purge_size, 4 /* KiB */);   // and purges the edges of a run too, not 2 MiB units only

  cycle("committed is flat across 16 x 1 MiB purge/reuse cycles", 1 * MiB);
  cycle("committed is flat across 16 x 64 KiB purge/reuse cycles", 64 * 1024);
  cycle("committed is flat across 16 x 4 MiB purge/reuse cycles", 4 * MiB);

  if (failures == 0) { fprintf(stderr, "test-purge-committed: all ok.\n"); }
  else { fprintf(stderr, "test-purge-committed: %d FAILED\n", failures); }
  return (failures == 0 ? 0 : 1);
}
