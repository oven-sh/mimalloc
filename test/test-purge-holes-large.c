/* ----------------------------------------------------------------------------
Copyright (c) 2018-2026, Microsoft Research, Daan Leijen
This is free software; you can redistribute it and/or modify it under the
terms of the MIT license. A copy of the license can be found in the file
"LICENSE" at the root of this distribution.
-----------------------------------------------------------------------------*/

// Hole purging in large (4 MiB) pages, which hold the blocks of 96 KiB up to 512 KiB.
//
// The bitmap of a page has 256 bits. A large page has 1024 OS pages of 4 KiB, so its bitmap is in units of 16 KiB
// (`mi_page_purge_unit`, see the "Page hole purging" section in `src/page.c`); everything else is what
// `test-purge-holes.c` tests for small and medium pages. Here: the unit itself, that the free blocks of a large page
// are discarded at an idle sweep (on Linux: that they are not resident any more) while their live neighbours keep
// every byte, what comes back when the blocks are used again (`mi_zalloc` included), frees from another thread into
// a page with holes, `mi_realloc` next to holes, a page that is abandoned with holes, `mi_heap_destroy` /
// `mi_heap_delete` of a heap with such pages.
//
// Run with MIMALLOC_PURGE_HOLES=0 to check that nothing is discarded then and everything else still holds.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "mimalloc.h"
#include "mimalloc-stats.h"
#include "mimalloc/internal.h"   // _mi_ptr_page, mi_page_purge_unit, _mi_page_purged_count, _mi_page_purge_os_page_blocks

#include "testhelper.h"

#if defined(__linux__)
#include <sys/mman.h>
#include <unistd.h>
#define HAVE_MINCORE 1
#else
#define HAVE_MINCORE 0
#endif

static bool purging_enabled = true;   // MIMALLOC_PURGE_HOLES

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

typedef struct hole_stats_s {
  int64_t bytes_now;      // bytes discarded right now
  int64_t bytes_total;    // bytes ever discarded
  int64_t blocks_now;     // blocks held off the free lists right now
  int64_t discards;
  int64_t reuses;
  int64_t inelig_pages;
} hole_stats_t;

static hole_stats_t hole_stats(void) {
  mi_purge_holes_stats_t s;
  mi_purge_holes_stats_get(&s);
  hole_stats_t h;
  h.bytes_now    = (int64_t)s.purged_bytes;
  h.bytes_total  = (int64_t)s.purged_bytes_total;
  h.blocks_now   = (int64_t)s.purged_blocks;
  h.discards     = (int64_t)s.discard_calls;
  h.reuses       = (int64_t)s.reuse_calls;
  h.inelig_pages = (int64_t)s.ineligible_pages;
  return h;
}

static uint8_t pattern_byte(size_t id, size_t off) {
  return (uint8_t)((id * 131u) ^ (off * 7u) ^ (off >> 8) ^ 0x5Bu);
}

static void pattern_fill(void* p, size_t size, size_t id) {
  uint8_t* b = (uint8_t*)p;
  for (size_t i = 0; i < size; i++) { b[i] = pattern_byte(id, i); }
}

static size_t pattern_check(const void* p, size_t size, size_t id) {
  const uint8_t* b = (const uint8_t*)p;
  for (size_t i = 0; i < size; i++) {
    if (b[i] != pattern_byte(id, i)) return i;
  }
  return size;
}

// The resident bytes of the OS pages that lie wholly inside `[p, p+size)`; `SIZE_MAX` where we cannot tell.
static size_t resident_bytes(const void* p, size_t size) {
  #if HAVE_MINCORE
  const size_t os = _mi_os_page_size();
  const uintptr_t lo = _mi_align_up((uintptr_t)p, os);
  const uintptr_t hi = _mi_align_down((uintptr_t)p + size, os);
  if (hi <= lo) return 0;
  const size_t n = (size_t)(hi - lo) / os;
  unsigned char* vec = (unsigned char*)calloc(n, 1);
  if (vec == NULL) return SIZE_MAX;
  size_t resident = SIZE_MAX;
  if (mincore((void*)lo, n * os, vec) == 0) {
    resident = 0;
    for (size_t i = 0; i < n; i++) { if ((vec[i] & 1) != 0) { resident += os; } }
  }
  free(vec);
  return resident;
  #else
  (void)p; (void)size;
  return SIZE_MAX;
  #endif
}

// An idle sweep.
static void sweep(void) {
  mi_on_thread_idle();
}

// The size classes of a large page: a request just over what a medium page takes, the largest one, and one in between.
static size_t large_size(int which) {
  switch (which) {
    case 0:  return MI_MEDIUM_MAX_OBJ_SIZE + 1024;
    case 1:  return (MI_LARGE_MAX_OBJ_SIZE / 2) + 1024;
    default: return MI_LARGE_MAX_OBJ_SIZE - 4096;   // (room for the padding of a debug build)
  }
}

// is this a page of many blocks that is larger than a medium page?
static bool is_large_page(const mi_page_t* page) {
  return (page != NULL && page->reserved > 1 && mi_page_size(page) > MI_MEDIUM_PAGE_SIZE);
}

// the page of a block that was freed: it may be gone by now (every block in it free), so look it up the careful way
static const mi_page_t* freed_block_page(const void* p, size_t bsize) {
  const mi_page_t* const page = _mi_safe_ptr_page(p);
  return (page != NULL && page->block_size == bsize ? page : NULL);
}

#define MAXB  (256)

// Allocate `n` blocks of `size` and fill each with its pattern over its usable size.
static bool alloc_filled(void** ptrs, size_t n, size_t size, size_t* usable) {
  for (size_t i = 0; i < n; i++) {
    ptrs[i] = mi_malloc(size);
    if (ptrs[i] == NULL) return false;
    *usable = mi_usable_size(ptrs[i]);
    pattern_fill(ptrs[i], *usable, i);
  }
  return true;
}

static void free_all(void** ptrs, size_t n) {
  for (size_t i = 0; i < n; i++) { if (ptrs[i] != NULL) { mi_free(ptrs[i]); ptrs[i] = NULL; } }
}

static bool survivors_intact(void** ptrs, size_t n, size_t usable, const char* what) {
  for (size_t i = 0; i < n; i++) {
    if (ptrs[i] == NULL) continue;
    const size_t bad = pattern_check(ptrs[i], usable, i);
    if (bad != usable) {
      fprintf(stderr, "\n  %s: CORRUPT survivor %zu at offset %zu of %zu\n", what, i, bad, usable);
      return false;
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// 1. the unit of the bitmap
// ---------------------------------------------------------------------------

static bool test_unit(void) {
  const size_t os = _mi_os_page_size();
  bool ok_all = true;
  // small and medium pages: one bit is one OS page, as it always was
  const size_t small_sizes[4] = { 16, 1024, 8192, 65536 };
  for (size_t i = 0; i < 4; i++) {
    void* p = mi_malloc(small_sizes[i]);
    if (p == NULL) return false;
    const mi_page_t* page = _mi_ptr_page(p);
    if (mi_page_size(page) <= MI_MEDIUM_PAGE_SIZE && mi_page_purge_unit(page) != os) {
      fprintf(stderr, "\n  a page of %zu byte blocks has a unit of %zu bytes (the OS page is %zu)\n", small_sizes[i], mi_page_purge_unit(page), os);
      ok_all = false;
    }
    mi_free(p);
  }
  // large pages: the smallest power-of-two multiple of the OS page for which the block area fits the bitmap
  for (int which = 0; which < 3; which++) {
    void* p = mi_malloc(large_size(which));
    if (p == NULL) return false;
    const mi_page_t* page = _mi_ptr_page(p);
    if (is_large_page(page)) {
      const size_t unit = mi_page_purge_unit(page);
      const size_t bits = mi_page_purge_bits(page);
      const uintptr_t start = (uintptr_t)mi_page_start(page);
      const uintptr_t end = start + mi_page_size(page);
      bool good = (unit >= os && _mi_is_power_of_two(unit) && bits <= MI_PAGE_PURGE_BITS && mi_page_can_purge_holes(page));
      // no smaller unit fits
      if (good && unit > os) {
        const size_t half = unit / 2;
        good = (_mi_divide_up((size_t)(end - _mi_align_down(start, half)), half) > MI_PAGE_PURGE_BITS);
      }
      // the base is aligned to the unit and covers the start of the blocks
      good = good && ((mi_page_purge_base(page) % unit) == 0) && (mi_page_purge_base(page) <= start) && (start - mi_page_purge_base(page) < unit);
      if (!good) {
        fprintf(stderr, "\n  large page of %zu byte blocks: unit %zu, %zu bits, os page %zu, block area %zu bytes at offset %zu of its unit\n",
                page->block_size, unit, bits, os, mi_page_size(page), (size_t)(start - mi_page_purge_base(page)));
        ok_all = false;
      }
      fprintf(stderr, "(%zu: unit %zu, %zu bits, %u blocks) ", page->block_size, unit, bits, (unsigned)page->reserved);
    }
    else {
      fprintf(stderr, "(%zu: not in a large page here) ", large_size(which));
    }
    mi_free(p);
  }
  return ok_all;
}

// The rule for a unit does not depend on how the block size relates to it: a unit is discardable when it lies
// wholly inside the block area and every block that overlaps it is free. Check `_mi_page_purge_os_page_blocks`, which
// holds that arithmetic, against a direct enumeration for units above the OS page and block sizes that are and that
// are not a multiple of the unit (a 64 KiB unit under blocks of 80, 96 and 112 KiB), with and without an offset of
// the first block.
static bool test_unit_arithmetic(void) {
  const size_t units[3] = { 8*MI_KiB, 16*MI_KiB, 64*MI_KiB };
  const size_t bsizes[7] = { 80*MI_KiB, 96*MI_KiB, 112*MI_KiB, 128*MI_KiB, 160*MI_KiB, 320*MI_KiB, 512*MI_KiB };
  const size_t offsets[3] = { 0, 4*MI_KiB, 192 };
  size_t cases = 0;
  for (size_t u = 0; u < 3; u++) {
    for (size_t b = 0; b < 7; b++) {
      for (size_t o = 0; o < 3; o++) {
        const size_t unit = units[u];
        const size_t bs = bsizes[b];
        const uintptr_t slice = (uintptr_t)1 << 30;
        const uintptr_t pstart = slice + offsets[o];
        const size_t capacity = (4*MI_MiB - offsets[o]) / bs;
        const uintptr_t base = _mi_align_down(pstart, unit);
        const uintptr_t pend = pstart + capacity*bs;
        const size_t nbits = _mi_divide_up((size_t)((pstart + capacity*bs) - base), unit);
        for (size_t k = 0; k < nbits; k++) {
          const uintptr_t lo = base + k*unit;
          const uintptr_t hi = lo + unit;
          size_t first = 0, last = 0;
          const bool inside = _mi_page_purge_os_page_blocks(unit, bs, pstart, capacity, k, &first, &last);
          const bool expect_inside = (lo >= pstart && hi <= pend);
          if (inside != expect_inside) {
            fprintf(stderr, "\n  unit %zu, block %zu, offset %zu: unit %zu is %s the block area, expected %s\n", unit, bs, offsets[o], k,
                    (inside ? "inside" : "outside"), (expect_inside ? "inside" : "outside"));
            return false;
          }
          if (!inside) continue;
          // the blocks that overlap [lo,hi), by enumeration
          size_t efirst = SIZE_MAX, elast = 0;
          for (size_t idx = 0; idx < capacity; idx++) {
            const uintptr_t blo = pstart + idx*bs;
            const uintptr_t bhi = blo + bs;
            if (blo < hi && bhi > lo) { if (efirst == SIZE_MAX) { efirst = idx; } elast = idx; }
          }
          if (first != efirst || last != elast) {
            fprintf(stderr, "\n  unit %zu, block %zu, offset %zu, unit index %zu: blocks [%zu,%zu], expected [%zu,%zu]\n", unit, bs, offsets[o], k, first, last, efirst, elast);
            return false;
          }
          cases++;
        }
      }
    }
  }
  fprintf(stderr, "(%zu cases) ", cases);
  return true;
}

// ---------------------------------------------------------------------------
// 2. the free blocks of a large page are discarded at an idle sweep; the survivors keep every byte
// ---------------------------------------------------------------------------

static bool test_discard(int which) {
  const size_t size = large_size(which);
  void* ptrs[MAXB];
  void* freed[MAXB];
  size_t usable = 0;
  bool ok_all = true;
  const size_t n = 40;
  memset(ptrs, 0, sizeof(ptrs)); memset(freed, 0, sizeof(freed));
  if (!alloc_filled(ptrs, n, size, &usable)) { free_all(ptrs, n); return false; }
  const bool large = is_large_page(_mi_ptr_page(ptrs[0]));
  const size_t bsize = _mi_ptr_page(ptrs[0])->block_size;

  // keep every third block: the others are free blocks with a live neighbour
  size_t nfreed = 0;
  for (size_t i = 0; i < n; i++) {
    if ((i % 3) != 0) { freed[i] = ptrs[i]; mi_free(ptrs[i]); ptrs[i] = NULL; nfreed++; }
  }
  size_t resident_before = 0;
  for (size_t i = 0; large && i < n; i++) { if (freed[i] != NULL) { const size_t r = resident_bytes(freed[i], bsize); if (r != SIZE_MAX) { resident_before += r; } } }

  const hole_stats_t before = hole_stats();
  sweep();
  const hole_stats_t after = hole_stats();

  size_t npurged = 0;
  size_t resident_after = 0;
  bool can_tell = true;
  for (size_t i = 0; i < n; i++) {
    if (freed[i] == NULL) continue;
    const mi_page_t* page = (large ? freed_block_page(freed[i], bsize) : NULL);
    if (page != NULL && mi_page_block_is_purged(page, freed[i])) { npurged++; }
    const size_t r = (large ? resident_bytes(freed[i], bsize) : SIZE_MAX);
    if (r == SIZE_MAX) { can_tell = false; } else { resident_after += r; }
  }
  if (!survivors_intact(ptrs, n, usable, "discard")) { ok_all = false; }
  for (size_t i = 0; i < n; i++) { if (ptrs[i] != NULL) { memset(ptrs[i], 0xA5, usable); } }   // and still writable

  if (!large) {
    fprintf(stderr, "(%zu: not in a large page here) ", size);
  }
  else if (purging_enabled) {
    // every freed block shares no unit with a live one when the block size is a multiple of the unit; else it keeps its edges
    const mi_page_t* page = _mi_ptr_page(ptrs[0]);
    const size_t unit = mi_page_purge_unit(page);
    const bool whole_blocks = ((bsize % unit) == 0 && (((uintptr_t)mi_page_start(page)) % unit) == 0);
    if (whole_blocks && npurged != nfreed) {
      fprintf(stderr, "\n  %zu of the %zu free blocks of %zu bytes are purged (unit %zu)\n", npurged, nfreed, bsize, unit);
      ok_all = false;
    }
    if (npurged == 0 || after.bytes_total <= before.bytes_total || after.discards <= before.discards) {
      fprintf(stderr, "\n  nothing was discarded in the large pages of %zu byte blocks\n", bsize);
      ok_all = false;
    }
    // (at least: a page can hold free blocks that were formed but never handed out, and those go as well)
    if (whole_blocks && (after.bytes_now - before.bytes_now) < (int64_t)(nfreed * bsize)) {
      fprintf(stderr, "\n  %lld bytes are discarded now, expected %zu or more\n", (long long)(after.bytes_now - before.bytes_now), nfreed * bsize);
      ok_all = false;
    }
    if (can_tell && HAVE_MINCORE && whole_blocks && resident_after != 0) {
      fprintf(stderr, "\n  %zu bytes of the freed blocks are still resident after the sweep (%zu before)\n", resident_after, resident_before);
      ok_all = false;
    }
    fprintf(stderr, "(%zu: %zu of %zu free blocks purged; resident %zu -> %zu KiB) ", bsize, npurged, nfreed,
            resident_before / MI_KiB, (can_tell && HAVE_MINCORE ? resident_after / MI_KiB : (size_t)0));
  }
  else if (npurged != 0 || after.bytes_total != before.bytes_total) {
    fprintf(stderr, "\n  purging is off but %zu blocks were discarded\n", npurged);
    ok_all = false;
  }
  free_all(ptrs, n);
  return ok_all;
}

// ---------------------------------------------------------------------------
// 3. use after a purge: fresh memory that is writable all over, zero from `mi_zalloc`, no block handed out twice
// ---------------------------------------------------------------------------

static bool test_reuse(void) {
  const size_t size = large_size(1);
  void* ptrs[MAXB];
  void* freed[MAXB];
  void* again[MAXB];
  size_t usable = 0;
  bool ok_all = true;
  const size_t n = 24;
  memset(ptrs, 0, sizeof(ptrs)); memset(freed, 0, sizeof(freed)); memset(again, 0, sizeof(again));
  if (!alloc_filled(ptrs, n, size, &usable)) { free_all(ptrs, n); return false; }
  size_t nfreed = 0;
  for (size_t i = 0; i < n; i++) {
    if ((i % 4) != 0) { memset(ptrs[i], 0xAB, usable); freed[nfreed++] = ptrs[i]; mi_free(ptrs[i]); ptrs[i] = NULL; }   // freed dirty
  }
  const hole_stats_t before = hole_stats();
  sweep();
  const hole_stats_t swept = hole_stats();

  // take them all again, and one more round than there were: alternately zeroed and not
  size_t reused = 0;
  for (size_t i = 0; i < nfreed; i++) {
    const bool zero = ((i % 2) == 0);
    again[i] = (zero ? mi_zalloc(size) : mi_malloc(size));
    if (again[i] == NULL) { ok_all = false; break; }
    for (size_t j = 0; j < nfreed; j++) { if (freed[j] == again[i]) { reused++; break; } }
    for (size_t j = 0; j < n; j++) {
      if (ptrs[j] == again[i]) { fprintf(stderr, "\n  a live block was handed out again\n"); ok_all = false; }
    }
    for (size_t j = 0; j < i; j++) {
      if (again[j] == again[i]) { fprintf(stderr, "\n  a block was handed out twice\n"); ok_all = false; }
    }
    const size_t u = mi_usable_size(again[i]);
    if (zero) {
      const uint8_t* b = (const uint8_t*)again[i];
      for (size_t k = 0; k < size; k++) {
        if (b[k] != 0) { fprintf(stderr, "\n  mi_zalloc returned a block with byte 0x%02x at offset %zu\n", b[k], k); ok_all = false; break; }
      }
    }
    pattern_fill(again[i], u, 1000 + i);   // writable all over
  }
  const hole_stats_t after = hole_stats();
  for (size_t i = 0; i < nfreed && again[i] != NULL; i++) {
    const size_t u = mi_usable_size(again[i]);
    if (pattern_check(again[i], u, 1000 + i) != u) { fprintf(stderr, "\n  a reused block lost its contents\n"); ok_all = false; break; }
  }
  if (!survivors_intact(ptrs, n, usable, "reuse")) { ok_all = false; }
  if (purging_enabled && is_large_page(_mi_ptr_page(ptrs[0]))) {
    if (swept.bytes_now <= before.bytes_now) { fprintf(stderr, "\n  nothing was discarded before the reuse\n"); ok_all = false; }
    if (after.reuses <= swept.reuses || after.bytes_now >= swept.bytes_now) {
      fprintf(stderr, "\n  the holes were not taken back: %lld reuses, %lld bytes still discarded (of %lld)\n",
              (long long)(after.reuses - swept.reuses), (long long)after.bytes_now, (long long)swept.bytes_now);
      ok_all = false;
    }
    if (reused == 0) { fprintf(stderr, "\n  none of the purged blocks was handed out again\n"); ok_all = false; }
  }
  fprintf(stderr, "(%zu of %zu purged blocks handed out again) ", reused, nfreed);
  free_all(again, nfreed);
  free_all(ptrs, n);
  return ok_all;
}

// ---------------------------------------------------------------------------
// 4. frees from another thread into pages with holes
// ---------------------------------------------------------------------------

static void*  xt_ptrs[MAXB];
static size_t xt_n;
static size_t xt_usable;

static bool xt_free_some(void) {    // free every block at an index of 3k+1 (their neighbours at 3k+2 are purged, 3k live)
  for (size_t i = 0; i < xt_n; i++) {
    if ((i % 3) == 1 && xt_ptrs[i] != NULL) { mi_free(xt_ptrs[i]); xt_ptrs[i] = NULL; }
  }
  return true;
}

static bool xt_free_rest(void) {
  for (size_t i = 0; i < xt_n; i++) {
    if (xt_ptrs[i] != NULL) {
      if (pattern_check(xt_ptrs[i], xt_usable, i) != xt_usable) return false;
      mi_free(xt_ptrs[i]); xt_ptrs[i] = NULL;
    }
  }
  return true;
}

static bool test_cross_thread_free(void) {
  const size_t size = large_size(0);
  bool ok_all = true;
  xt_n = 60;
  memset(xt_ptrs, 0, sizeof(xt_ptrs));
  if (!alloc_filled(xt_ptrs, xt_n, size, &xt_usable)) { free_all(xt_ptrs, xt_n); return false; }
  for (size_t i = 0; i < xt_n; i++) { if ((i % 3) == 2) { mi_free(xt_ptrs[i]); xt_ptrs[i] = NULL; } }
  const hole_stats_t start = hole_stats();
  sweep();                                  // the blocks at 3k+2 are holes now
  const hole_stats_t first = hole_stats();
  if (!mi_run_on_thread(&xt_free_some)) return false;   // another thread frees the blocks at 3k+1, into pages with holes
  if (!survivors_intact(xt_ptrs, xt_n, xt_usable, "cross-thread free")) { ok_all = false; }
  sweep();                                  // collects those frees; they are holes as well now
  const hole_stats_t second = hole_stats();
  if (!survivors_intact(xt_ptrs, xt_n, xt_usable, "cross-thread free, second sweep")) { ok_all = false; }
  if (purging_enabled && is_large_page(_mi_ptr_page(xt_ptrs[0]))) {
    if (first.bytes_now <= start.bytes_now || second.bytes_now <= first.bytes_now) {
      fprintf(stderr, "\n  discarded bytes: %lld at the start, %lld after the first sweep, %lld after the frees from the other thread and a second sweep\n",
              (long long)start.bytes_now, (long long)first.bytes_now, (long long)second.bytes_now);
      ok_all = false;
    }
  }
  // and the other thread frees what is left: the pages become free with holes in them, by way of their thread-free lists
  if (!mi_run_on_thread(&xt_free_rest)) { fprintf(stderr, "\n  a survivor was corrupt when the other thread freed it\n"); ok_all = false; }
  mi_collect(true);
  return ok_all;
}

// ---------------------------------------------------------------------------
// 5. realloc next to holes: shrinking in place and moving to another size class
// ---------------------------------------------------------------------------

static bool test_realloc(void) {
  const size_t size = large_size(1);
  void* ptrs[MAXB];
  size_t usable = 0;
  bool ok_all = true;
  const size_t n = 16;
  memset(ptrs, 0, sizeof(ptrs));
  if (!alloc_filled(ptrs, n, size, &usable)) { free_all(ptrs, n); return false; }
  const bool large = is_large_page(_mi_ptr_page(ptrs[0]));
  const hole_stats_t before = hole_stats();
  for (size_t i = 0; i < n; i++) { if ((i % 2) == 1) { mi_free(ptrs[i]); ptrs[i] = NULL; } }
  sweep();
  const hole_stats_t swept = hole_stats();
  if (purging_enabled && large && swept.bytes_now <= before.bytes_now) { fprintf(stderr, "\n  no holes next to the blocks that are about to be reallocated\n"); ok_all = false; }
  for (size_t i = 0; i < n; i += 2) {
    // shrink a little: stays where it is (more than half of the block is still in use)
    void* const p1 = mi_realloc(ptrs[i], size - 4096);
    if (p1 == NULL) return false;
    if (p1 != ptrs[i]) { fprintf(stderr, "(a shrink by one OS page moved the block) "); }
    ptrs[i] = p1;
    if (pattern_check(ptrs[i], size - 4096, i) != size - 4096) { fprintf(stderr, "\n  contents lost in a shrinking realloc\n"); ok_all = false; break; }
    // grow into the next size classes: moves, possibly into the hole next door of another page
    void* const p2 = mi_realloc(ptrs[i], 2 * size);
    if (p2 == NULL) return false;
    ptrs[i] = p2;
    if (pattern_check(ptrs[i], size - 4096, i) != size - 4096) { fprintf(stderr, "\n  contents lost in a growing realloc\n"); ok_all = false; break; }
    // and back down, into the class it came from: takes a purged block of the first page again
    void* const p3 = mi_realloc(ptrs[i], size / 2 + 8192);
    if (p3 == NULL) return false;
    ptrs[i] = p3;
    if (pattern_check(ptrs[i], size / 2 + 8192, i) != size / 2 + 8192) { fprintf(stderr, "\n  contents lost in a realloc to half the size\n"); ok_all = false; break; }
    sweep();   // purge what the moves left behind before the next one
  }
  const hole_stats_t after = hole_stats();
  if (purging_enabled && large && after.reuses <= swept.reuses) { fprintf(stderr, "\n  the moves took no hole back\n"); ok_all = false; }
  free_all(ptrs, n);
  return ok_all;
}

// ---------------------------------------------------------------------------
// 6. a large page that is abandoned with free blocks in it: the sweep of another thread reaches what is in the
//    abandoned map (a page that was abandoned while it was full is not, see `test-purge-holes.c`), and its blocks
//    can be used and freed from there
// ---------------------------------------------------------------------------

static void*  ab_ptrs[MAXB];
static void*  ab_freed[MAXB];
static size_t ab_n;
static size_t ab_usable;

static bool ab_worker(void) {
  const size_t size = large_size(2);
  if (!alloc_filled(ab_ptrs, ab_n, size, &ab_usable)) return false;
  for (size_t i = 0; i < ab_n; i++) { if ((i % 2) == 1) { ab_freed[i] = ab_ptrs[i]; mi_free(ab_ptrs[i]); ab_ptrs[i] = NULL; } }
  return true;   // the thread exits: its pages are abandoned, with free blocks that nobody swept yet
}

static bool test_abandoned(void) {
  bool ok_all = true;
  ab_n = 20;
  memset(ab_ptrs, 0, sizeof(ab_ptrs)); memset(ab_freed, 0, sizeof(ab_freed));
  if (!mi_run_on_thread(&ab_worker)) return false;
  const size_t bsize = (ab_ptrs[0] != NULL ? _mi_ptr_page(ab_ptrs[0])->block_size : 0);
  const bool large = (ab_ptrs[0] != NULL && is_large_page(_mi_ptr_page(ab_ptrs[0])));
  sweep();   // the sweep of this thread goes over the abandoned pages of its heaps that are in the abandoned map
  size_t npurged = 0, nfreed = 0;
  for (size_t i = 0; i < ab_n; i++) {
    if (ab_freed[i] == NULL) continue;
    nfreed++;
    const mi_page_t* const page = freed_block_page(ab_freed[i], bsize);
    if (page != NULL && mi_page_block_is_purged(page, ab_freed[i])) { npurged++; }
  }
  if (purging_enabled && large && npurged == 0) { fprintf(stderr, "\n  none of the %zu free blocks in the abandoned pages was discarded by the sweep of another thread\n", nfreed); ok_all = false; }
  if (!purging_enabled && npurged != 0) { fprintf(stderr, "\n  purging is off but %zu blocks in abandoned pages were discarded\n", npurged); ok_all = false; }
  fprintf(stderr, "(%zu of %zu free blocks in abandoned pages purged) ", npurged, nfreed);
  if (!survivors_intact(ab_ptrs, ab_n, ab_usable, "abandoned")) { ok_all = false; }
  // use the holes from here, then free everything from here
  void* re[MAXB];
  memset(re, 0, sizeof(re));
  for (size_t i = 0; i < ab_n && ok_all; i++) {
    re[i] = mi_zalloc(large_size(2));
    if (re[i] == NULL) { ok_all = false; break; }
    const uint8_t* b = (const uint8_t*)re[i];
    for (size_t k = 0; k < large_size(2); k += 509) { if (b[k] != 0) { fprintf(stderr, "\n  mi_zalloc from an abandoned page with holes is not zero\n"); ok_all = false; break; } }
    memset(re[i], 0x5E, large_size(2));
  }
  if (!survivors_intact(ab_ptrs, ab_n, ab_usable, "abandoned, after reuse")) { ok_all = false; }
  free_all(re, ab_n);
  free_all(ab_ptrs, ab_n);
  return ok_all;
}

// ---------------------------------------------------------------------------
// 7. heap destroy and heap delete with holes in large pages
// ---------------------------------------------------------------------------

static bool test_heap_release(bool destroy) {
  const size_t size = large_size(1);
  bool ok_all = true;
  mi_heap_t* const heap = mi_heap_new();
  if (heap == NULL) return false;
  void* ptrs[MAXB];
  const size_t n = 30;
  size_t usable = 0;
  memset(ptrs, 0, sizeof(ptrs));
  for (size_t i = 0; i < n; i++) {
    ptrs[i] = mi_heap_malloc(heap, size);
    if (ptrs[i] == NULL) { mi_heap_destroy(heap); return false; }
    usable = mi_usable_size(ptrs[i]);
    pattern_fill(ptrs[i], usable, i);
  }
  for (size_t i = 0; i < n; i++) { if ((i % 3) != 0) { mi_free(ptrs[i]); ptrs[i] = NULL; } }
  const hole_stats_t before = hole_stats();
  sweep();
  const hole_stats_t swept = hole_stats();
  if (purging_enabled && is_large_page(_mi_ptr_page(ptrs[0])) && swept.bytes_now <= before.bytes_now) {
    fprintf(stderr, "\n  nothing was discarded in the pages of the heap\n");
    ok_all = false;
  }
  if (!survivors_intact(ptrs, n, usable, "heap release")) { ok_all = false; }
  if (destroy) {
    mi_heap_destroy(heap);   // frees the survivors too
  }
  else {
    mi_heap_delete(heap);    // the survivors move to the main heap
    if (!survivors_intact(ptrs, n, usable, "heap delete")) { ok_all = false; }
    free_all(ptrs, n);
  }
  mi_collect(true);
  const hole_stats_t after = hole_stats();
  if (after.bytes_now != before.bytes_now || after.blocks_now != before.blocks_now) {
    fprintf(stderr, "\n  after the heap is gone %lld bytes in %lld blocks are still counted as discarded (%lld / %lld before)\n",
            (long long)after.bytes_now, (long long)after.blocks_now, (long long)before.bytes_now, (long long)before.blocks_now);
    ok_all = false;
  }
  return ok_all;
}

static bool test_heap_destroy(void) { return test_heap_release(true); }
static bool test_heap_delete(void)  { return test_heap_release(false); }

// ---------------------------------------------------------------------------
// 8. a block that the sweep discarded is freed once more: reported where double frees are checked
// ---------------------------------------------------------------------------

static bool test_double_free(void) {
  #if (MI_PADDING || MI_SECURE>=3)
  const size_t size = large_size(1);
  void* ptrs[8];
  size_t usable = 0;
  if (!alloc_filled(ptrs, 8, size, &usable)) return false;
  void* const victim = ptrs[3];
  mi_free(ptrs[3]); ptrs[3] = NULL;
  sweep();
  const mi_page_t* const page = freed_block_page(victim, _mi_ptr_page(ptrs[0])->block_size);
  bool ok_all = true;
  if (purging_enabled && is_large_page(page) && mi_page_block_is_purged(page, victim)) {
    const size_t used = mi_page_used(page);
    const hole_stats_t s1 = hole_stats();
    mi_free(victim);   // reported (to stderr) and ignored
    const hole_stats_t s2 = hole_stats();
    if (mi_page_used(page) != used || !mi_page_block_is_purged(page, victim) || s2.bytes_now != s1.bytes_now) {
      fprintf(stderr, "\n  a double free of a purged block changed the page\n");
      ok_all = false;
    }
  }
  if (!survivors_intact(ptrs, 8, usable, "double free")) { ok_all = false; }
  free_all(ptrs, 8);
  return ok_all;
  #else
  fprintf(stderr, "(double frees are not checked in this build) ");
  return true;
  #endif
}

// ---------------------------------------------------------------------------

int main(void) {
  mi_version();
  purging_enabled = mi_option_is_enabled(mi_option_purge_holes);
  // zero every hole before it is discarded, so that a discard that covers a live block shows on every OS (see `test-purge-holes.c`)
  mi_option_set(mi_option_purge_holes_eager_zero, 1);
  fprintf(stderr, "purge_holes is %s, os page size is %zu, large pages are %zu KiB for blocks over %zu up to %zu bytes\n",
          (purging_enabled ? "ON" : "OFF"), (size_t)_mi_os_page_size(), (size_t)(MI_LARGE_PAGE_SIZE / MI_KiB),
          (size_t)MI_MEDIUM_MAX_OBJ_SIZE, (size_t)MI_LARGE_MAX_OBJ_SIZE);
  const hole_stats_t start = hole_stats();

  CHECK("unit", test_unit());
  CHECK("unit-arithmetic", test_unit_arithmetic());
  CHECK("discard-first-class", test_discard(0));
  CHECK("discard-middle-class", test_discard(1));
  CHECK("discard-largest-class", test_discard(2));
  CHECK("reuse-after-purge", test_reuse());
  CHECK("cross-thread-free", test_cross_thread_free());
  CHECK("realloc", test_realloc());
  CHECK("abandoned", test_abandoned());
  CHECK("heap-destroy", test_heap_destroy());
  CHECK("heap-delete", test_heap_delete());
  CHECK("double-free-of-a-purged-block", test_double_free());

  // everything above is freed by now, so every hole must have been handed back
  mi_collect(true);
  const hole_stats_t end = hole_stats();
  fprintf(stderr, "holes: %lld bytes discarded in total over %lld discards / %lld reuses; %lld bytes still discarded; %lld ineligible pages in the last sweep\n",
          (long long)(end.bytes_total - start.bytes_total), (long long)(end.discards - start.discards), (long long)(end.reuses - start.reuses),
          (long long)end.bytes_now, (long long)end.inelig_pages);
  void* const probe = malloc(24);
  const bool overridden = mi_is_in_heap_region(probe);
  free(probe);
  if (overridden) {
    // (as in `test-purge-holes.c`: the C runtime has live pages of its own then. This is every Linux build of the static
    //  library; the cases above check that the accounting returns to where it was one by one)
    fprintf(stderr, "(malloc is overridden: not checking that no hole is outstanding at exit)\n");
  }
  else {
    CHECK("no-holes-outstanding-at-exit", (end.bytes_now == 0 && end.blocks_now == 0));
  }
  return print_test_summary();
}
