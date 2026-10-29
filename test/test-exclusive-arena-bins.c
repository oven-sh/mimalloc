/* ----------------------------------------------------------------------------
Copyright (c) 2026, Microsoft Research, Daan Leijen
This is free software; you can redistribute it and/or modify it under the
terms of the MIT license.
-----------------------------------------------------------------------------*/

// A heap with an exclusive arena has no other arena to go to. A chunk of an arena gets the size bin of the
// first page that is placed in it, and takes no small pages once that is a page of several slices.
// In an arena of two chunks, small blocks were refused with half of the arena free.

#include <mimalloc.h>
#include <stdint.h>
#include <stdio.h>

int main(void) {
  const size_t KiB = 1024;
  const size_t MiB = 1024*KiB;
  // The sizes of a 64-bit build. With other sizes the state below is not reached, and all blocks are served anyway.
  const size_t slice = 64*KiB;
  const size_t chunk = 32*MiB;
  const size_t size  = 2*chunk;
  const size_t block = 1*KiB;

  mi_arena_id_t arena;
  if (mi_reserve_os_memory_ex(size, false /* commit */, false /* allow large */, true /* exclusive */, &arena) != 0) {
    printf("test-exclusive-arena-bins: skipped (cannot reserve %zu MiB)\n", size/MiB);
    return 0;
  }
  size_t area_size = 0;
  const uint8_t* const start = (const uint8_t*)mi_arena_area(arena, &area_size);
  mi_heap_t* const heap = mi_heap_new_in_arena(arena);
  if (start == NULL || heap == NULL) {
    fprintf(stderr, "FAILED: no heap in the arena\n");
    return 1;
  }

  // small pages, until the first chunk has no room for a page of 8 slices
  size_t served = 0;
  for (;;) {
    const uint8_t* const p = (const uint8_t*)mi_heap_malloc(heap, block);
    if (p == NULL) break;
    served += mi_usable_size(p);
    if ((size_t)(p - start) >= chunk - 4*slice) break;
  }
  // which so becomes the first page of the second chunk
  if (mi_heap_malloc(heap, 48*KiB) == NULL) {
    fprintf(stderr, "FAILED: no room for a medium block\n");
    return 1;
  }
  // small blocks again, until they are refused
  for (void* p; (p = mi_heap_malloc(heap, block)) != NULL; ) {
    served += mi_usable_size(p);
  }

  printf("test-exclusive-arena-bins: small blocks got %zu of %zu MiB\n", served/MiB, area_size/MiB);
  // half of it at most when they are refused the second chunk; a debug build spends a fifth of each block on padding
  if (served < (area_size/8)*5) {
    fprintf(stderr, "FAILED: small blocks were refused with room left in the arena\n");
    return 1;
  }
  return 0;
}
