/* ----------------------------------------------------------------------------
Copyright (c) 2026, Microsoft Research, Daan Leijen
This is free software; you can redistribute it and/or modify it under the
terms of the MIT license. A copy of the license can be found in the file
"LICENSE" at the root of this distribution.
-----------------------------------------------------------------------------*/

// The free slices of the first chunk of an exclusive arena are used again once a later chunk is in use.
//
// Small pages fill the first chunk of the arena and go on into the second. All of them are freed, but for one page
// that keeps the second chunk in its size bin. What is allocated next is to land where the first blocks did, at
// the start of the arena, and not in the second chunk.

#include "mimalloc.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define BLOCK_SIZE  (1024)
#define COUNT       (48 * 1024)   // 48 MiB, more than a chunk (32 MiB)
#define AGAIN       (8 * 1024)    // 8 MiB
#define SLACK       (4 * 1024 * 1024)

static void* blocks[COUNT];
static int failures = 0;

static void check(const char* name, bool ok) {
  fprintf(stderr, "test: %s...  %s\n", name, ok ? "ok." : "FAILED");
  if (!ok) failures++;
}

static bool allocate(mi_heap_t* heap, int count) {
  for (int i = 0; i < count; i++) {
    blocks[i] = mi_heap_malloc(heap, BLOCK_SIZE);
    if (blocks[i] == NULL) return false;
    memset(blocks[i], 1, BLOCK_SIZE);
  }
  return true;
}

int main(void) {
  mi_arena_id_t arena;
  if (mi_reserve_os_memory_ex(256 * 1024 * 1024, false /* commit */, false /* allow large */, true /* exclusive */, &arena) != 0) {
    fprintf(stderr, "test-arena-first-chunk: skipped (cannot reserve an arena)\n");
    return 0;
  }
  mi_heap_t* heap = mi_heap_new_in_arena(arena);

  check("the arena has room", allocate(heap, COUNT));
  uintptr_t end_of_first = 0;
  for (int i = 0; i < AGAIN; i++) {
    if ((uintptr_t)blocks[i] > end_of_first) { end_of_first = (uintptr_t)blocks[i]; }
  }
  void* keeps_bin = mi_heap_malloc(heap, 2 * BLOCK_SIZE);   // a page of another size class, behind all the others
  for (int i = 0; i < COUNT; i++) {
    mi_free(blocks[i]);
  }
  mi_heap_collect(heap, true);   // also the page that is retired

  check("the arena has room again", allocate(heap, AGAIN));
  int elsewhere = 0;
  for (int i = 0; i < AGAIN; i++) {
    if ((uintptr_t)blocks[i] > end_of_first + SLACK) { elsewhere++; }
  }
  fprintf(stderr, "  %d of %d blocks are not where the first %d were\n", elsewhere, AGAIN, AGAIN);
  check("blocks land at the start of the arena again", elsewhere == 0);

  for (int i = 0; i < AGAIN; i++) {
    mi_free(blocks[i]);
  }
  mi_free(keeps_bin);
  return (failures > 0 ? 1 : 0);
}
