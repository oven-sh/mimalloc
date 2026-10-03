/* ----------------------------------------------------------------------------
Copyright (c) 2026, Microsoft Research, Daan Leijen
This is free software; you can redistribute it and/or modify it under the
terms of the MIT license.
-----------------------------------------------------------------------------*/

/* test-stats-merge.c

   A block of statistics (`mi_stats_t`) has about 180 counts: one per statistic
   and one per size bin for blocks, for pages and for chunks. Every theap and
   every heap has such a block, and a merge (`_mi_stats_merge_into`) adds one
   block into another with atomic operations. A thread that exits merges, and so
   does a collect, a `mi_stats_get`, and a `mi_heap_delete` or `mi_heap_destroy`
   (theap into heap twice, heap into the main heap: three merges).

   Almost all counts of a short-lived heap are zero. A merge added each of them
   anyway, with a locked instruction per count. The three merges were more than
   half of the time of a `mi_heap_new` + `mi_heap_destroy` pair. A merge now
   skips a count that the source never touched.

   The test checks that:

   1. a merge adds up: totals and current values are summed, the peak is the
      destination's current value plus the source's peak (if that is more), and
      the source is empty afterwards;
   2. a merge of a block that counted nothing does not write to the destination.
      The destination is mapped read-only, so a write faults. Without the skip
      the first count faults;
   3. the counts of heaps that are created, used and destroyed arrive in
      `mi_stats_get`: the merges of the destroy path still carry everything.

   > mimalloc-test-stats-merge
*/

#if defined(_WIN32)
#include <stdio.h>
int main(void) { printf("test-stats-merge: skipped on Windows (uses mmap/mprotect and sigaction)\n"); return 0; }
#else

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <signal.h>
#include <setjmp.h>
#include <unistd.h>
#include <sys/mman.h>

#include "mimalloc.h"
#include "mimalloc-stats.h"
// internal headers: the test calls the merge itself, on blocks of statistics that it owns
#include "mimalloc/types.h"
#include "mimalloc/internal.h"

static int failed = 0;
#define EXPECT(what, cond)  do { if (!(cond)) { fprintf(stderr, "\n  FAILED %s: %s (%s:%d)\n", what, #cond, __FILE__, __LINE__); failed++; } } while(0)

// ---------------------------------------------------------------------------
// 1. a merge adds up
// ---------------------------------------------------------------------------

static void test_merge_adds_up(void) {
  mi_stats_t_decl(to);
  mi_stats_t_decl(from);

  // destination: 3 pages now (at most 5, 9 in total), blocks in one bin, and a counter
  to.pages.total = 9;  to.pages.peak = 5;  to.pages.current = 3;
  to.malloc_bins[4].total = 100;  to.malloc_bins[4].peak = 60;  to.malloc_bins[4].current = 40;
  to.mmap_calls.total = 7;

  // source: a statistic, two counters, and a bin of each kind
  from.pages.total = 4;  from.pages.peak = 2;  from.pages.current = 1;
  from.malloc_bins[4].total = 10;  from.malloc_bins[4].peak = 10;  from.malloc_bins[4].current = -5;   // it freed more than it allocated
  from.page_bins[7].total = 2;     from.page_bins[7].peak = 2;     from.page_bins[7].current = 2;
  from.chunk_bins[1].total = 1;    from.chunk_bins[1].peak = 1;    from.chunk_bins[1].current = 0;     // it came and went
  from.pages_abandoned.current = -1;                                                                   // only a decrease: total and peak are zero
  from.mmap_calls.total = 1;
  from.pages_retire.total = 3;

  _mi_stats_merge_into(&to, &from);

  EXPECT("pages.total", to.pages.total == 13);
  EXPECT("pages.current", to.pages.current == 4);
  EXPECT("pages.peak", to.pages.peak == 5);                        // max(5, 3 + 2)
  EXPECT("malloc_bins.total", to.malloc_bins[4].total == 110);
  EXPECT("malloc_bins.current", to.malloc_bins[4].current == 35);
  EXPECT("malloc_bins.peak", to.malloc_bins[4].peak == 60);        // max(60, 40 + 10)
  EXPECT("page_bins.total", to.page_bins[7].total == 2);
  EXPECT("page_bins.current", to.page_bins[7].current == 2);
  EXPECT("page_bins.peak", to.page_bins[7].peak == 2);
  EXPECT("chunk_bins.total", to.chunk_bins[1].total == 1);
  EXPECT("chunk_bins.current", to.chunk_bins[1].current == 0);
  EXPECT("chunk_bins.peak", to.chunk_bins[1].peak == 1);
  EXPECT("pages_abandoned.total", to.pages_abandoned.total == 0);
  EXPECT("pages_abandoned.current", to.pages_abandoned.current == -1);
  EXPECT("pages_abandoned.peak", to.pages_abandoned.peak == 0);
  EXPECT("mmap_calls", to.mmap_calls.total == 8);
  EXPECT("pages_retire", to.pages_retire.total == 3);

  // a count that neither side touched is still zero
  EXPECT("untouched count", to.malloc_bins[5].total == 0 && to.malloc_bins[5].peak == 0 && to.malloc_bins[5].current == 0);
  EXPECT("untouched counter", to.commit_calls.total == 0);

  // the source is empty again, with its header intact
  mi_stats_t_decl(empty);
  EXPECT("source is reset", memcmp(&from, &empty, sizeof(mi_stats_t)) == 0);
}

// ---------------------------------------------------------------------------
// 2. a merge of an empty block does not write to the destination
// ---------------------------------------------------------------------------

static sigjmp_buf fault_jmp;
static void on_fault(int sig) { (void)sig; siglongjmp(fault_jmp, 1); }

static void test_merge_of_empty_does_not_write(void) {
  const size_t page_size = (size_t)sysconf(_SC_PAGESIZE);
  const size_t size = ((sizeof(mi_stats_t) + page_size - 1) / page_size) * page_size;
  void* const mem = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (mem == MAP_FAILED) { perror("mmap"); abort(); }
  mi_stats_t* const to = (mi_stats_t*)mem;
  mi_stats_init(to);
  to->pages.total = 9;  to->pages.peak = 5;  to->pages.current = 3;
  to->malloc_bins[4].total = 100;  to->malloc_bins[4].peak = 60;  to->malloc_bins[4].current = 40;
  mi_stats_t before;
  memcpy(&before, to, sizeof(mi_stats_t));
  if (mprotect(mem, size, PROT_READ) != 0) { perror("mprotect"); abort(); }

  struct sigaction sa, old_segv, old_bus;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = &on_fault;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGSEGV, &sa, &old_segv);
  sigaction(SIGBUS, &sa, &old_bus);    // (macOS reports a write to a read-only page as SIGBUS)

  mi_stats_t_decl(from);
  volatile bool wrote = false;
  if (sigsetjmp(fault_jmp, 1) == 0) {
    _mi_stats_merge_into(to, &from);
  }
  else {
    wrote = true;
  }
  sigaction(SIGSEGV, &old_segv, NULL);
  sigaction(SIGBUS, &old_bus, NULL);

  EXPECT("a merge of an empty block of statistics wrote to the destination", !wrote);
  EXPECT("destination is unchanged", memcmp(&before, to, sizeof(mi_stats_t)) == 0);
  munmap(mem, size);
}

// ---------------------------------------------------------------------------
// 3. the counts of destroyed heaps arrive in `mi_stats_get`
// ---------------------------------------------------------------------------

#define HEAPS   64
#define BLOCKS  10
#define BSIZE   200

static void test_destroyed_heaps_are_counted(void) {
  // no guarded blocks in the heaps below: they are counted in another place
  const long guarded_rate = mi_option_get(mi_option_guarded_sample_rate);
  mi_option_set(mi_option_guarded_sample_rate, 0);   // picked up by the theap of each new heap

  mi_stats_t_decl(before);
  mi_stats_t_decl(after);
  size_t bin = 0;
  EXPECT("mi_stats_get", mi_stats_get(&before));
  for (int i = 0; i < HEAPS; i++) {
    mi_heap_t* heap = mi_heap_new();
    if (heap == NULL) { fprintf(stderr, "mi_heap_new failed\n"); abort(); }
    for (int j = 0; j < BLOCKS; j++) {
      void* p = mi_heap_malloc(heap, BSIZE);
      if (p == NULL) { fprintf(stderr, "allocation failed\n"); abort(); }
      memset(p, i, BSIZE);
      bin = _mi_page_stats_bin(_mi_ptr_page(p));
    }
    mi_heap_destroy(heap);
  }
  EXPECT("mi_stats_get", mi_stats_get(&after));
  mi_option_set(mi_option_guarded_sample_rate, guarded_rate);

  EXPECT("heaps.total", after.heaps.total - before.heaps.total == HEAPS);
  EXPECT("heaps.current", after.heaps.current == before.heaps.current);
  EXPECT("page_bins.total", after.page_bins[bin].total - before.page_bins[bin].total == HEAPS);   // a page per heap
  EXPECT("page_bins.current", after.page_bins[bin].current == before.page_bins[bin].current);
  #if MI_STATS
  EXPECT("malloc_normal_count", after.malloc_normal_count.total - before.malloc_normal_count.total == HEAPS*BLOCKS);
  EXPECT("malloc_bins.total", after.malloc_bins[bin].total - before.malloc_bins[bin].total == HEAPS*BLOCKS);
  EXPECT("malloc_bins.current", after.malloc_bins[bin].current == before.malloc_bins[bin].current);
  #endif
}

int main(void) {
  test_merge_adds_up();
  test_merge_of_empty_does_not_write();
  test_destroyed_heaps_are_counted();
  if (failed > 0) {
    fprintf(stderr, "test-stats-merge: %d check(s) failed\n", failed);
    return 1;
  }
  printf("test-stats-merge: ok\n");
  return 0;
}

#endif
