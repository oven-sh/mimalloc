/* ----------------------------------------------------------------------------
Copyright (c) Microsoft Research, Daan Leijen
This is free software; you can redistribute it and/or modify it under the
terms of the MIT license.
-----------------------------------------------------------------------------*/

/* A block above `mi_arena_max_object_size()` does not live in an arena: it is mapped from the OS
   and unmapped again when it is freed. `_mi_os_get_aligned_hint` (src/os.c) gave each of those
   mappings an address from a counter that only moves forward, so every one landed in a part of
   the address space that the page map had not seen before. The 2-level page map commits a
   sub-map for each such part (64 KiB per 512 MiB of address space) and keeps it until the process
   ends. A program that allocates and frees huge blocks one after the other (a runtime that
   reserves 4 GiB of address space per WebAssembly memory, oven-sh/bun#41459) lost 64 KiB of
   committed memory per block, 4 KiB of it resident.

   Without a hint the OS gives the range that was just unmapped to the next mapping of that size.
   The test allocates and frees huge blocks, two at a time, and counts the 512 MiB parts of the
   address space in which a block started: each one stands for a sub-map that stays behind.

   Expected with the bug present: one part per block.
   Skipped where such a block cannot be allocated at all (a 32-bit address space).
   Only a few blocks: a debug build reads every fresh block back to check that it is zero. */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <mimalloc.h>

#define BLOCKS       6
#define LIVE         2
#define MAX_PARTS    4             // two blocks are live at a time: two parts when every range is used again
#define PART_SHIFT   29            // 512 MiB, what one sub-map of the page map covers

static uintptr_t parts[BLOCKS];
static int       part_count;

static void note_part(const void* p) {
  const uintptr_t part = ((uintptr_t)p >> PART_SHIFT);
  for (int i = 0; i < part_count; i++) {
    if (parts[i] == part) return;
  }
  parts[part_count++] = part;
}

int main(void) {
  if (sizeof(void*) < 8) {
    printf("test-huge-cycle: skipped, needs a 64-bit address space\n");
    return 0;
  }
  // above what an arena holds, and above 1 GiB also where an arena holds less than that
  const size_t GiB = ((size_t)1 << 30);
  const size_t size = (mi_arena_max_object_size() > GiB ? mi_arena_max_object_size() : GiB) + ((size_t)1 << 20);

  void* live[LIVE];
  for (int done = 0; done < BLOCKS; done += LIVE) {
    for (int i = 0; i < LIVE; i++) {
      live[i] = mi_malloc_aligned(size, 4096);
      if (live[i] == NULL) {
        printf("test-huge-cycle: skipped, cannot allocate a block of %zu MiB\n", size >> 20);
        return 0;
      }
      memset(live[i], 1, 4096);
      note_part(live[i]);
    }
    for (int i = 0; i < LIVE; i++) { mi_free(live[i]); }
  }

  const int ok = (part_count <= MAX_PARTS);
  printf("%s %d blocks of %zu MiB started in %d part(s) of the address space, expected at most %d\n",
         (ok ? "ok  " : "FAIL"), BLOCKS, size >> 20, part_count, MAX_PARTS);
  return (ok ? 0 : 1);
}
