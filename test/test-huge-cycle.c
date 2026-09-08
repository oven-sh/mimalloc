/* ----------------------------------------------------------------------------
Copyright (c) Microsoft Research, Daan Leijen
This is free software; you can redistribute it and/or modify it under the
terms of the MIT license.
-----------------------------------------------------------------------------*/

/* An object above `arena_max_object_size` (2 GiB by default on 64-bit) does not live in an arena:
   its `malloc` maps it from the OS and its `free` unmaps it. Registering the mapping in the 2-level
   page map allocates a 64 KiB submap for every 512 MiB of address space that did not have one yet
   (`mi_page_map_alloc_submap_at`), and submaps are kept for the life of the process.

   `_mi_os_get_aligned_hint` used to place every such mapping at a fresh, ever increasing address, so
   each malloc/free cycle of a huge object started in address space the page map had never seen and
   left one more submap behind: 64 KiB of reserved (and partly touched) memory per cycle that is never
   returned. A program that repeatedly creates and drops a >2 GiB buffer (or, through JavaScriptCore,
   a WebAssembly memory, which reserves 4 GiB up front) grew without bound.

   Without a hint the OS hands back the range that was just unmapped, so the submap that already
   covers it is used again. The test lowers `arena_max_object_size` so that the objects it cycles can
   stay small (a debug build checks that every fresh OS page reads back zero, byte by byte), keeps each
   object larger than the 512 MiB a submap covers so that with the bug every cycle needs a new one, and
   checks through the allocator's own `reserved` statistic that the cycles leave nothing behind.
   With the bug present it reports +64 KiB per cycle.  64-bit only. */

#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <mimalloc.h>
#include <mimalloc-stats.h>

#if (SIZE_MAX <= UINT32_MAX)
int main(void) { printf("test-huge-cycle: skipped, needs a 64-bit address space\n"); return 0; }
#else

#define CYCLES          10
#define MAX_OBJ_KIB     ((long)256 << 10)                       // arena_max_object_size for this test: 256 MiB (the option is in KiB)
#define HUGE_SIZE       ((size_t)520 << 20)                     // > arena_max_object_size, and > 512 MiB so every hinted cycle lands in a new submap
#define SUBMAP_SIZE     ((int64_t)64 << 10)

static int64_t reserved_now(int64_t* mmap_calls) {
  mi_stats_t st; mi_stats_init(&st);
  mi_stats_get(&st);
  if (mmap_calls != NULL) { *mmap_calls = st.mmap_calls.total; }
  return st.reserved.current;
}

static bool cycle(int i) {
  void* p = mi_malloc_aligned(HUGE_SIZE, 4096);
  if (p == NULL) {
    printf("test-huge-cycle: skipped, cannot allocate %zu MiB (cycle %d)\n", HUGE_SIZE >> 20, i);
    return false;
  }
  ((volatile char*)p)[0] = (char)i;
  mi_free(p);
  return true;
}

int main(void) {
  mi_option_set(mi_option_arena_max_object_size, MAX_OBJ_KIB);

  // warm up: the first cycles may legitimately add a submap or two for address space not seen before
  for (int i = 0; i < 2; i++) {
    if (!cycle(i)) return 0;
  }
  int64_t mmaps0 = 0;
  const int64_t reserved0 = reserved_now(&mmaps0);

  for (int i = 0; i < CYCLES; i++) {
    if (!cycle(i)) return 0;
  }

  int64_t mmaps1 = 0;
  const int64_t reserved1 = reserved_now(&mmaps1);
  const int64_t growth = reserved1 - reserved0;
  printf("test-huge-cycle: %d malloc/free cycles of a %zu MiB object: reserved %+lld KiB (%+.1f KiB per cycle), %lld OS allocations\n",
         CYCLES, HUGE_SIZE >> 20, (long long)(growth / 1024), (double)growth / 1024.0 / CYCLES, (long long)(mmaps1 - mmaps0));

  // Allow a couple of new submaps in case the OS moves the mapping around, but nothing close to one per cycle.
  const int64_t allowed = 3 * SUBMAP_SIZE;
  if (growth > allowed) {
    printf("  FAIL: reserved memory grew by %lld KiB over %d cycles (allowed %lld KiB): each cycle left a page-map submap behind\n",
           (long long)(growth / 1024), CYCLES, (long long)(allowed / 1024));
    return 1;
  }
  printf("test-huge-cycle: ok\n");
  return 0;
}

#endif
