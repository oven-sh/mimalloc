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
// `mi_heap_delete` of a heap with such pages, and when they are discarded: not while the page is in use, which is while the
// last allocation from it was in the current epoch of the sweep or the one before (an epoch is `purge_holes_min_interval`
// long at least and only a sweep ends it), neither by an inline sweep nor in the park handoff.
//
// Run with MIMALLOC_PURGE_HOLES=0 to check that nothing is discarded then and everything else still holds.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "mimalloc.h"
#include "mimalloc-stats.h"
#include "mimalloc/internal.h"   // _mi_ptr_page, mi_page_purge_unit, _mi_page_purged_count, _mi_page_purge_os_page_blocks
#include "mimalloc/prim-tls.h"   // _mi_theap_default

#include "testhelper.h"
#if !defined(_WIN32)
#include <time.h>
#endif

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

// (a parked thread may not allocate, so it waits with this)
static void sleep_ms(unsigned ms) {
  #if defined(_WIN32)
  Sleep(ms);
  #else
  struct timespec ts;
  ts.tv_sec = (time_t)(ms / 1000);
  ts.tv_nsec = (long)(ms % 1000) * 1000000L;
  nanosleep(&ts, NULL);
  #endif
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

// An idle sweep. (The free blocks of a large page stay until a whole epoch of `purge_holes_min_interval` passed without an
// allocation from the page; `main` sets that to 0, which is no waiting, for the cases that are about something else.)
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
// 9. the park handoff: the sweep of a park leaves the free blocks of a large page that is in use (a thread parks far more
//    often than it is idle, and a busy one takes those buffers again at once): that was allocated from in the current epoch
//    of the sweep or the one before. With one sweeper, as in here, the first sweep after an allocation finds that whenever
//    it comes (it ends one epoch at the most). The park is swept again `purge_holes_min_interval` later, and once more
//    after that if the first sweep did not end an epoch; that takes them, and then the park is done and not swept again.
//    (What is checked about time are lower bounds, which hold on a machine of any load.)
// ---------------------------------------------------------------------------

static size_t count_purged_in(void** freed, size_t n, size_t bsize, const mi_page_t* only);

static bool test_park_defers_large(void) {
  const size_t size = large_size(1);
  const long interval_ms = 300;
  void* ptrs[MAXB];
  void* freed[MAXB];
  size_t usable = 0;
  bool ok_all = true;
  const size_t n = 24;
  memset(ptrs, 0, sizeof(ptrs)); memset(freed, 0, sizeof(freed));
  const long old_interval = mi_option_get(mi_option_purge_holes_min_interval);
  mi_option_set(mi_option_purge_holes_min_interval, interval_ms);
  if (!alloc_filled(ptrs, n, size, &usable)) { free_all(ptrs, n); return false; }
  const mi_page_t* const page0 = _mi_ptr_page(ptrs[0]);
  const bool large = is_large_page(page0);
  const size_t bsize = page0->block_size;
  size_t nfreed = 0;
  for (size_t i = 0; i < n; i++) { if ((i % 2) == 1) { freed[i] = ptrs[i]; mi_free(ptrs[i]); ptrs[i] = NULL; nfreed++; } }

  const hole_stats_t before = hole_stats();
  mi_tld_t* const tld = _mi_theap_default()->tld;
  const size_t seq_start = tld->holes_sweep_seq;
  const mi_msecs_t t_park = _mi_clock_now();   // before the park, and so before its first sweep
  if (!mi_on_thread_idle_start()) {
    fprintf(stderr, "(no scavenger to hand off to: not tested) ");
  }
  else {
    // (nothing in here may allocate or free, and with malloc overridden that includes printing)
    // the first sweep of the park: it leaves the large pages, so the park is not done
    while (mi_atomic_load_acquire(&tld->park_swept) == MI_PARK_SWEPT_NONE && _mi_clock_now() - t_park < 10000) { sleep_ms(2); }
    const uint32_t swept1 = mi_atomic_load_acquire(&tld->park_swept);
    const hole_stats_t s1 = hole_stats();
    const bool s1_is_first = (tld->holes_sweep_seq == seq_start + 1);   // no second sweep began before we looked (the count moves when a sweep begins)
    // the thread stays parked: the park is swept again, which takes them, and then it is done
    while (mi_atomic_load_acquire(&tld->park_swept) != MI_PARK_SWEPT_DONE && _mi_clock_now() - t_park < 30000) { sleep_ms(2); }
    const bool park_done = (mi_atomic_load_acquire(&tld->park_swept) == MI_PARK_SWEPT_DONE);
    const mi_msecs_t t_done = _mi_clock_now();   // after the last sweep
    const size_t seq_done = tld->holes_sweep_seq;
    // ..and nothing comes back for it
    sleep_ms((unsigned)(3 * interval_ms));
    const size_t seq_end = tld->holes_sweep_seq;
    mi_on_thread_idle_end();

    if (purging_enabled && large) {
      const size_t npurged = count_purged_in(freed, n, bsize, NULL);
      if (swept1 == MI_PARK_SWEPT_NONE) { fprintf(stderr, "\n  the park was not swept\n"); ok_all = false; }
      else {
        // (if we looked too late to see the state after the first sweep, it is that of a later one: not done either, or done with everything taken)
        if (swept1 == MI_PARK_SWEPT_DONE && (seq_done - seq_start) < 2) { fprintf(stderr, "\n  the first sweep of the park, right after the allocations, marked it as done\n"); ok_all = false; }
        if (swept1 == MI_PARK_SWEPT_SMALL && s1_is_first && (s1.bytes_now - before.bytes_now) >= (int64_t)bsize) {
          fprintf(stderr, "\n  the first sweep of the park, right after the allocations, discarded %lld bytes\n", (long long)(s1.bytes_now - before.bytes_now));
          ok_all = false;
        }
      }
      if (npurged != nfreed) { fprintf(stderr, "\n  %zu of the %zu free blocks of the large pages were discarded while the thread stayed parked\n", npurged, nfreed); ok_all = false; }
      if (!park_done || seq_end != seq_done) {
        fprintf(stderr, "\n  the park was %s, and swept %zu more times in the %ld ms after that\n", (park_done ? "done" : "NOT marked as done"), seq_end - seq_done, 3 * interval_ms);
        ok_all = false;
      }
      if ((seq_done - seq_start) < 2 || (seq_done - seq_start) > 3) { fprintf(stderr, "\n  the park was swept %zu times\n", seq_done - seq_start); ok_all = false; }
      // each sweep is an interval after the one before
      if (park_done && (t_done - t_park) < (mi_msecs_t)((seq_done - seq_start) - 1) * interval_ms) {
        fprintf(stderr, "\n  the park was swept %zu times in %lld ms\n", seq_done - seq_start, (long long)(t_done - t_park));
        ok_all = false;
      }
      fprintf(stderr, "(%zu sweeps, done after %lld ms) ", seq_done - seq_start, (long long)(t_done - t_park));
    }
  }
  if (!survivors_intact(ptrs, n, usable, "park")) { ok_all = false; }
  mi_option_set(mi_option_purge_holes_min_interval, old_interval);
  free_all(ptrs, n);
  return ok_all;
}

// ---------------------------------------------------------------------------
// 10. ..and the same for the sweep that a thread does itself: nothing of a large page goes at the first sweep after an
//     allocation from it, whatever was freed, nor at a second one right after that, and `mi_on_thread_idle_pending` says so;
//     the pages that were left alone for a whole epoch go; a page that is allocated from in every epoch keeps its blocks
//     for good, however many sweeps pass, and they go when that stops
// ---------------------------------------------------------------------------

static size_t count_purged_in(void** freed, size_t n, size_t bsize, const mi_page_t* only) {
  size_t npurged = 0;
  for (size_t i = 0; i < n; i++) {
    const mi_page_t* const page = (freed[i] != NULL ? freed_block_page(freed[i], bsize) : NULL);
    if (page != NULL && (only == NULL || page == only) && mi_page_block_is_purged(page, freed[i])) { npurged++; }
  }
  return npurged;
}

// Take blocks of `size` until one comes out of the blocks in `freed` (other pages of the class may have free blocks as
// well), and put them all back: its page was allocated from now. NULL if none did.
static const mi_page_t* touch_one_page(void** freed, size_t n, size_t size) {
  void* extra[64];
  size_t nextra = 0;
  const mi_page_t* hit = NULL;
  while (hit == NULL && nextra < 64) {
    void* const p = mi_malloc(size);
    if (p == NULL) break;
    memset(p, 0x3E, size);
    extra[nextra++] = p;
    for (size_t i = 0; i < n; i++) { if (freed[i] == p) { hit = _mi_ptr_page(p); } }
  }
  free_all(extra, nextra);
  return hit;
}

static bool test_recent_allocation(void) {
  const size_t size = large_size(1);
  const long interval_ms = 300;
  void* ptrs[MAXB];
  void* freed[MAXB];
  size_t usable = 0;
  bool ok_all = true;
  const size_t n = 16;
  memset(ptrs, 0, sizeof(ptrs)); memset(freed, 0, sizeof(freed));
  const long old_interval = mi_option_get(mi_option_purge_holes_min_interval);
  mi_option_set(mi_option_purge_holes_min_interval, interval_ms);
  // so that the first sweep below ends an epoch whatever ran before this: the current one is older than the interval then
  mi_on_thread_idle();
  sleep_ms((unsigned)interval_ms + 20);
  const mi_msecs_t t_alloc = _mi_clock_now();   // before the allocations: no page is older than this
  if (!alloc_filled(ptrs, n, size, &usable)) { free_all(ptrs, n); return false; }
  const bool large = is_large_page(_mi_ptr_page(ptrs[0]));
  const size_t bsize = _mi_ptr_page(ptrs[0])->block_size;
  size_t nfreed = 0;
  for (size_t i = 0; i < n; i++) { if ((i % 4) != 0) { freed[i] = ptrs[i]; mi_free(ptrs[i]); ptrs[i] = NULL; nfreed++; } }
  if (purging_enabled && large) {
    // the first sweep after the allocations, whenever it comes
    mi_on_thread_idle();
    const bool all1 = !mi_on_thread_idle_pending();
    const size_t n1 = count_purged_in(freed, n, bsize, NULL);
    if (n1 != 0) { fprintf(stderr, "\n  the first sweep after the allocations discarded %zu blocks of the large pages\n", n1); ok_all = false; }
    if (all1) { fprintf(stderr, "\n  a sweep that left the free blocks of large pages did not say so\n"); ok_all = false; }
    // ..and a second one right after it: two epochs cannot end within one interval
    mi_on_thread_idle();
    const bool all1b = !mi_on_thread_idle_pending();
    const size_t n1b = count_purged_in(freed, n, bsize, NULL);
    if (_mi_clock_now() - t_alloc < interval_ms) {
      if (n1b != 0) { fprintf(stderr, "\n  two sweeps within the interval after the allocations discarded %zu blocks of the large pages\n", n1b); ok_all = false; }
      if (all1b) { fprintf(stderr, "\n  a second sweep that left the free blocks of large pages did not say so\n"); ok_all = false; }
    }
    // an interval later, all but one page were left alone for a whole epoch: take a block out of that one and put it back
    sleep_ms((unsigned)interval_ms + 20);
    const mi_page_t* const hit = touch_one_page(freed, n, size);
    mi_on_thread_idle();
    const bool all2 = !mi_on_thread_idle_pending();
    if (hit == NULL) { fprintf(stderr, "(no block came out of the pages under test) "); }
    else {
      const size_t nhit = count_purged_in(freed, n, bsize, hit);
      if (nhit != 0) { fprintf(stderr, "\n  a sweep discarded %zu blocks of a large page that was allocated from right before it\n", nhit); ok_all = false; }
      if (all2) { fprintf(stderr, "\n  a sweep that left the free blocks of a large page did not say so\n"); ok_all = false; }
    }
    size_t nother = 0, nother_purged = 0;
    for (size_t i = 0; i < n; i++) {
      const mi_page_t* const page = (freed[i] != NULL ? freed_block_page(freed[i], bsize) : NULL);
      if (page != NULL && page != hit) { nother++; if (mi_page_block_is_purged(page, freed[i])) { nother_purged++; } }
    }
    if (nother_purged != nother) { fprintf(stderr, "\n  %zu of the %zu free blocks in the pages that were left alone for an epoch were discarded\n", nother_purged, nother); ok_all = false; }
    // a page that is allocated from in every epoch keeps its blocks, sweep after sweep (the same page every time, as the
    // queue of the size class has it first; the check is per sweep, so it holds for whichever page it is)
    size_t nbusy = 0, nbusy_same = 0, nbusy_bad = 0, nbusy_quiet = 0;
    const mi_page_t* busy_prev = NULL;
    for (int round = 0; round < 4; round++) {
      sleep_ms((unsigned)interval_ms + 20);
      const mi_page_t* const busy = touch_one_page(freed, n, size);
      if (busy == NULL) continue;
      nbusy++;
      if (busy == busy_prev) { nbusy_same++; }
      busy_prev = busy;
      const size_t nb0 = count_purged_in(freed, n, bsize, busy);
      mi_on_thread_idle();
      if (count_purged_in(freed, n, bsize, busy) != nb0) { nbusy_bad++; }
      if (!mi_on_thread_idle_pending()) { nbusy_quiet++; }
    }
    if (nbusy_bad != 0) { fprintf(stderr, "\n  %zu of %zu sweeps, an interval apart, discarded blocks of a large page that was allocated from before each of them\n", nbusy_bad, nbusy); ok_all = false; }
    if (nbusy_quiet != 0) { fprintf(stderr, "\n  %zu of %zu sweeps that left the free blocks of a large page did not say so\n", nbusy_quiet, nbusy); ok_all = false; }
    // and they go once it has been left alone for an epoch: the last sweep above ended the epoch of the last allocation,
    // the next one ends the one after it
    sleep_ms((unsigned)interval_ms + 20);
    mi_on_thread_idle();
    const bool all3 = !mi_on_thread_idle_pending();
    const size_t n3 = count_purged_in(freed, n, bsize, NULL);
    if (n3 != nfreed) { fprintf(stderr, "\n  %zu of the %zu free blocks are discarded after the pages were left alone\n", n3, nfreed); ok_all = false; }
    if (!all3) { fprintf(stderr, "\n  a sweep that left nothing says that it did\n"); ok_all = false; }
    // a sweep after that has nothing to do in them
    const hole_stats_t s3 = hole_stats();
    mi_on_thread_idle();
    const hole_stats_t s4 = hole_stats();
    if (s4.discards != s3.discards) { fprintf(stderr, "\n  a sweep of pages that were swept and not touched since made %lld discards\n", (long long)(s4.discards - s3.discards)); ok_all = false; }
    fprintf(stderr, "(%zu + %zu of %zu free blocks; %zu sweeps of a page in use, %zu of the same page as before) ", nother_purged, n3 - nother_purged, nfreed, nbusy, nbusy_same);
  }
  if (!survivors_intact(ptrs, n, usable, "recent allocation")) { ok_all = false; }
  mi_option_set(mi_option_purge_holes_min_interval, old_interval);
  free_all(ptrs, n);
  return ok_all;
}

// ---------------------------------------------------------------------------
// 11. the same for an abandoned page: the sweep of another thread leaves its free blocks while the page is young, and
//     takes them when it is two epochs old
// ---------------------------------------------------------------------------

static bool test_abandoned_young(void) {
  const long interval_ms = 300;
  bool ok_all = true;
  ab_n = 20;
  memset(ab_ptrs, 0, sizeof(ab_ptrs)); memset(ab_freed, 0, sizeof(ab_freed));
  const long old_interval = mi_option_get(mi_option_purge_holes_min_interval);
  mi_option_set(mi_option_purge_holes_min_interval, interval_ms);
  mi_on_thread_idle();                     // (so that the first sweep below ends an epoch, see `test_recent_allocation`)
  sleep_ms((unsigned)interval_ms + 20);
  if (!mi_run_on_thread(&ab_worker)) { mi_option_set(mi_option_purge_holes_min_interval, old_interval); return false; }
  const size_t bsize = (ab_ptrs[0] != NULL ? _mi_ptr_page(ab_ptrs[0])->block_size : 0);
  const bool large = (ab_ptrs[0] != NULL && is_large_page(_mi_ptr_page(ab_ptrs[0])));
  size_t nfreed = 0;
  for (size_t i = 0; i < ab_n; i++) { if (ab_freed[i] != NULL) { nfreed++; } }
  if (purging_enabled && large) {
    mi_on_thread_idle();   // goes over the abandoned pages of the heaps of this thread: allocated from in the epoch that this sweep ended
    const size_t n1 = count_purged_in(ab_freed, ab_n, bsize, NULL);
    const bool pending1 = mi_on_thread_idle_pending();
    sleep_ms((unsigned)interval_ms + 20);
    mi_on_thread_idle();   // two epochs old now
    const size_t n2 = count_purged_in(ab_freed, ab_n, bsize, NULL);
    if (n1 != 0) { fprintf(stderr, "\n  the first sweep after a thread abandoned its large pages discarded %zu of their free blocks\n", n1); ok_all = false; }
    if (n2 != 0 && !pending1) { fprintf(stderr, "\n  a sweep that left the free blocks of abandoned large pages did not say so\n"); ok_all = false; }
    if (n2 == 0) { fprintf(stderr, "\n  none of the %zu free blocks in the abandoned pages was discarded two epochs later\n", nfreed); ok_all = false; }
    fprintf(stderr, "(%zu, then %zu of %zu free blocks in abandoned pages purged) ", n1, n2, nfreed);
  }
  if (!survivors_intact(ab_ptrs, ab_n, ab_usable, "abandoned, young")) { ok_all = false; }
  mi_option_set(mi_option_purge_holes_min_interval, old_interval);
  free_all(ab_ptrs, ab_n);
  return ok_all;
}

// ---------------------------------------------------------------------------
// 12. several threads sweep at the same time, all the time: no epoch is shorter than the interval for that (it is what
//     the rule rests on), and a page that is allocated from before each sweep keeps its blocks
//     (Every check is a lower bound on a time between two readings of the clock that lie around what is measured, or is
//      guarded by one: a thread that is held up anywhere makes them weaker, not wrong.)
// ---------------------------------------------------------------------------

#if defined(_WIN32)
typedef HANDLE test_thread_t;
static DWORD WINAPI test_thread_entry(LPVOID arg) { ((void (*)(void))arg)(); return 0; }
static bool test_thread_start(test_thread_t* t, void (*fun)(void)) { *t = CreateThread(NULL, 0, &test_thread_entry, (LPVOID)fun, 0, NULL); return (*t != NULL); }
static void test_thread_join(test_thread_t t) { WaitForSingleObject(t, INFINITE); CloseHandle(t); }
#else
typedef pthread_t test_thread_t;
static void* test_thread_entry(void* arg) { ((void (*)(void))arg)(); return NULL; }
static bool test_thread_start(test_thread_t* t, void (*fun)(void)) { return (pthread_create(t, NULL, &test_thread_entry, (void*)fun) == 0); }
static void test_thread_join(test_thread_t t) { pthread_join(t, NULL); }
#endif

static _Atomic(uintptr_t) cs_stop;

// sweeps without a pause, with a large page of its own that it allocates from before each sweep
static void cs_sweeper(void) {
  const size_t size = large_size(0);
  void* own[4];
  for (size_t i = 0; i < 4; i++) { own[i] = mi_malloc(size); }
  mi_free(own[1]); own[1] = NULL;
  mi_free(own[3]); own[3] = NULL;
  while (mi_atomic_load_acquire(&cs_stop) == 0) {
    void* const p = mi_malloc(size);
    if (p != NULL) { *(volatile uint8_t*)p = 1; mi_free(p); }
    mi_on_thread_idle();
  }
  mi_free(own[0]); mi_free(own[2]);
}

#define CS_SWEEPERS  (4)

static bool test_concurrent_sweepers(void) {
  const size_t size = large_size(1);
  const long interval_ms = 50;
  void* ptrs[MAXB];
  void* freed[MAXB];
  size_t usable = 0;
  bool ok_all = true;
  const size_t n = 12;
  memset(ptrs, 0, sizeof(ptrs)); memset(freed, 0, sizeof(freed));
  const long old_interval = mi_option_get(mi_option_purge_holes_min_interval);
  mi_option_set(mi_option_purge_holes_min_interval, interval_ms);
  if (!alloc_filled(ptrs, n, size, &usable)) { free_all(ptrs, n); return false; }
  const bool large = is_large_page(_mi_ptr_page(ptrs[0]));
  const size_t bsize = _mi_ptr_page(ptrs[0])->block_size;
  for (size_t i = 0; i < n; i++) { if ((i % 2) == 1) { freed[i] = ptrs[i]; mi_free(ptrs[i]); ptrs[i] = NULL; } }
  if (purging_enabled && large) {
    test_thread_t threads[CS_SWEEPERS];
    size_t nthreads = 0;
    mi_atomic_store_release(&cs_stop, (uintptr_t)0);
    for (size_t i = 0; i < CS_SWEEPERS; i++) { if (test_thread_start(&threads[nthreads], &cs_sweeper)) { nthreads++; } }
    // The latest time at which each of the last epochs was still seen (a reading of the clock from before the reading of the epoch).
    mi_msecs_t seen_at[8];
    uint32_t seen_epoch[8];
    for (size_t i = 0; i < 8; i++) { seen_at[i] = 0; seen_epoch[i] = 0; }
    uint32_t last = _mi_page_purge_holes_epoch() - 8;   // (nothing is on record for the ones before the first we see)
    size_t nepochs = 0, nshort = 0, nchecked = 0, ntaken = 0;
    mi_msecs_t shortest = INT64_MAX;
    const mi_msecs_t t_start = _mi_clock_now();
    for (;;) {
      const mi_msecs_t t0 = _mi_clock_now();
      // for 12 intervals, and then until this thread has seen the epoch move a few times (it does within 10 s, however little of the machine we get)
      if ((t0 - t_start) >= (12 * interval_ms) && (nepochs >= 4 || (t0 - t_start) >= 10000)) break;
      // a page of ours is allocated from, after t0, and swept at once..
      const mi_page_t* const busy = touch_one_page(freed, n, size);
      const size_t nb0 = (busy != NULL ? count_purged_in(freed, n, bsize, busy) : 0);
      mi_on_thread_idle();
      const size_t nb1 = (busy != NULL ? count_purged_in(freed, n, bsize, busy) : 0);
      const uint32_t epoch = _mi_page_purge_holes_epoch();
      const mi_msecs_t t1 = _mi_clock_now();
      // ..so its blocks stay, unless all of that took as long as an epoch (to the millisecond, as the clock is)
      if (busy != NULL && (t1 - t0) < (interval_ms - 2)) { nchecked++; if (nb1 != nb0) { ntaken++; } }
      // Two epochs on from one that we saw at a time T: all of the one in between was after T and before now.
      if (epoch != last) {
        nepochs++;
        for (uint32_t back = 2; back <= 7; back++) {
          const uint32_t e = epoch - back;
          if (seen_epoch[e % 8] == e && seen_at[e % 8] != 0) {
            const mi_msecs_t len = (t1 - seen_at[e % 8]) / (mi_msecs_t)(back - 1);   // the mean of the `back-1` epochs in between
            if (len < shortest) { shortest = len; }
            if (len < interval_ms - 1) { nshort++; }
            break;
          }
        }
        last = epoch;
      }
      seen_at[epoch % 8] = t0; seen_epoch[epoch % 8] = epoch;   // (`epoch` was read after t0)
    }
    mi_atomic_store_release(&cs_stop, (uintptr_t)1);
    for (size_t i = 0; i < nthreads; i++) { test_thread_join(threads[i]); }
    if (nshort != 0) { fprintf(stderr, "\n  %zu epochs were shorter than the interval of %ld ms (the shortest: %lld ms)\n", nshort, interval_ms, (long long)shortest); ok_all = false; }
    if (ntaken != 0) { fprintf(stderr, "\n  %zu of %zu sweeps discarded blocks of a large page that was allocated from less than %ld ms before\n", ntaken, nchecked, interval_ms - 2); ok_all = false; }
    if (nepochs < 4) { fprintf(stderr, "\n  the epoch moved %zu times in 10 s of sweeping\n", nepochs - 1); ok_all = false; }
    fprintf(stderr, "(%zu sweepers and this thread: %zu epochs, the shortest %lld ms; %zu sweeps of a page in use) ", nthreads, nepochs, (long long)(shortest == INT64_MAX ? 0 : shortest), nchecked);
  }
  if (!survivors_intact(ptrs, n, usable, "sweepers at the same time")) { ok_all = false; }
  mi_option_set(mi_option_purge_holes_min_interval, old_interval);
  free_all(ptrs, n);
  return ok_all;
}

// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// the floor: the first `purge_holes_large_floor` bytes of free blocks in large pages stay, the rest goes
// ---------------------------------------------------------------------------

static void*  fl_ptrs[MAXB];
static void*  fl_freed[MAXB];
static size_t fl_usable;

static size_t fl_count_purged(size_t n, size_t bsize) {
  size_t npurged = 0;
  for (size_t i = 0; i < n; i++) {
    if (fl_freed[i] == NULL) continue;
    const mi_page_t* const page = freed_block_page(fl_freed[i], bsize);
    if (page != NULL && mi_page_block_is_purged(page, fl_freed[i])) { npurged++; }
  }
  return npurged;
}

// sweeps an interval apart until the pages that were allocated from before the first of them are out of the hold
static void fd_sweeps_past_the_hold(long interval_ms) {
  for (int i = 0; i < 4; i++) { sweep(); sleep_ms((unsigned)interval_ms + 3); }
  sweep();
}

#define FL_INTERVAL_MS  (2)

static bool fl_worker(void) {
  // takes all of the floor on a thread that goes away: its share is to come back
  void* p[24];
  const size_t size = large_size(1);
  for (size_t i = 0; i < 24; i++) { p[i] = mi_malloc(size); if (p[i] == NULL) return false; memset(p[i], 1, size); }
  for (size_t i = 0; i < 24; i++) { if ((i % 3) != 0) { mi_free(p[i]); p[i] = NULL; } }
  fd_sweeps_past_the_hold(FL_INTERVAL_MS);
  const bool took = (_mi_page_purge_holes_floor_kept() > 0);
  for (size_t i = 0; i < 24; i++) { mi_free(p[i]); }
  return took;
}

static size_t fl_kept_expected;

static bool fl_second_sweeper(void) {
  // another thread sweeps the same heap: the abandoned pages that the first one counts are not counted again
  void* q = mi_malloc(64);
  fd_sweeps_past_the_hold(FL_INTERVAL_MS);
  const bool same = (_mi_page_purge_holes_floor_kept() == fl_kept_expected);
  if (!same) { fprintf(stderr, "\n  %zu bytes are counted under the floor after another thread swept the heap, %zu before\n", _mi_page_purge_holes_floor_kept(), fl_kept_expected); }
  mi_free(q);
  return same;
}

static bool test_floor(void) {
  if (!purging_enabled) return true;
  bool ok_all = true;
  const size_t size = large_size(1);
  const size_t n = 60;
  memset(fl_ptrs, 0, sizeof(fl_ptrs)); memset(fl_freed, 0, sizeof(fl_freed));
  if (!alloc_filled(fl_ptrs, n, size, &fl_usable)) { free_all(fl_ptrs, n); return false; }
  if (!is_large_page(_mi_ptr_page(fl_ptrs[0]))) { fprintf(stderr, "(not in a large page here) "); free_all(fl_ptrs, n); return true; }
  const size_t bsize = _mi_ptr_page(fl_ptrs[0])->block_size;
  size_t nfreed = 0;
  for (size_t i = 0; i < n; i++) {
    if ((i % 3) != 0) { fl_freed[i] = fl_ptrs[i]; mi_free(fl_ptrs[i]); fl_ptrs[i] = NULL; nfreed++; }
  }
  // a quarter of what is freed, whatever a large page and its blocks are here (4 MiB and 320 KiB, or 1 MiB and 80 KiB):
  // more than the free blocks of one page and less than all of them
  const size_t floor = _mi_align_up((nfreed * bsize) / 4, MI_KiB);
  if (nfreed * bsize <= 2 * floor) { fprintf(stderr, "\n  the case frees too little: %zu bytes\n", nfreed * bsize); ok_all = false; }

  // (there is a floor only where there is an epoch to count in: an interval, and then the hold comes first)
  const long old_interval = mi_option_get(mi_option_purge_holes_min_interval);
  mi_option_set(mi_option_purge_holes_min_interval, FL_INTERVAL_MS);
  mi_option_set(mi_option_purge_holes_large_floor, (long)(floor / MI_KiB));
  fd_sweeps_past_the_hold(FL_INTERVAL_MS);
  const size_t kept1 = _mi_page_purge_holes_floor_kept();
  const size_t purged1 = fl_count_purged(n, bsize);
  if (kept1 == 0 || kept1 > floor) {
    fprintf(stderr, "\n  %zu bytes are kept under a floor of %zu\n", kept1, floor);
    ok_all = false;
  }
  if (purged1 == 0 || purged1 == nfreed || (nfreed - purged1) * bsize > floor) {
    fprintf(stderr, "\n  %zu of %zu free blocks of %zu bytes are purged with a floor of %zu\n", purged1, nfreed, bsize, floor);
    ok_all = false;
  }
  // the same again: what stayed is decided anew and comes out the same, nothing adds up
  sweep();
  const size_t kept2 = _mi_page_purge_holes_floor_kept();
  const size_t purged2 = fl_count_purged(n, bsize);
  if (kept2 != kept1 || purged2 != purged1) {
    fprintf(stderr, "\n  a second sweep: %zu bytes kept and %zu blocks purged, %zu and %zu after the first\n", kept2, purged2, kept1, purged1);
    ok_all = false;
  }
  if (!survivors_intact(fl_ptrs, n, fl_usable, "floor")) { ok_all = false; }
  fl_kept_expected = kept2;
  if (!mi_run_on_thread(&fl_second_sweeper)) { ok_all = false; }
  if (fl_count_purged(n, bsize) != purged1) { fprintf(stderr, "\n  %zu blocks are purged after another thread swept the heap, %zu before\n", fl_count_purged(n, bsize), purged1); ok_all = false; }

  // without the floor the rest goes as well
  mi_option_set(mi_option_purge_holes_large_floor, 0);
  sweep();
  const size_t kept3 = _mi_page_purge_holes_floor_kept();
  const size_t purged3 = fl_count_purged(n, bsize);
  if (kept3 != 0 || purged3 <= purged1) {
    fprintf(stderr, "\n  no floor: %zu bytes kept, %zu blocks purged (%zu with the floor)\n", kept3, purged3, purged1);
    ok_all = false;
  }
  if (!survivors_intact(fl_ptrs, n, fl_usable, "floor, none")) { ok_all = false; }

  // a thread that ends gives its share back
  mi_option_set(mi_option_purge_holes_large_floor, (long)(floor / MI_KiB));
  if (!mi_run_on_thread(&fl_worker)) { fprintf(stderr, "\n  the other thread kept nothing under the floor\n"); ok_all = false; }
  if (_mi_page_purge_holes_floor_kept() != 0) {
    fprintf(stderr, "\n  %zu bytes are still counted under the floor after the thread that kept them ended\n", _mi_page_purge_holes_floor_kept());
    ok_all = false;
  }
  mi_option_set(mi_option_purge_holes_large_floor, 0);
  mi_option_set(mi_option_purge_holes_min_interval, old_interval);
  fprintf(stderr, "(%zu of %zu free blocks of %zu bytes purged under a floor of %zu KiB, %zu KiB kept; %zu without) ",
          purged1, nfreed, bsize, floor / MI_KiB, kept1 / MI_KiB, purged3);
  free_all(fl_ptrs, n);
  mi_collect(true);
  return ok_all;
}

// ---------------------------------------------------------------------------
// what stays under the floor goes when its page was not allocated from for a while, and it is the pages that were
// used last that stay
// ---------------------------------------------------------------------------

static void*  fd_ptrs[2][MAXB];
static void*  fd_freed[2][MAXB];
static size_t fd_usable[2];

static size_t fd_count_purged(int g, size_t n, size_t bsize) {
  size_t npurged = 0;
  for (size_t i = 0; i < n; i++) {
    if (fd_freed[g][i] == NULL) continue;
    const mi_page_t* const page = freed_block_page(fd_freed[g][i], bsize);
    if (page != NULL && mi_page_block_is_purged(page, fd_freed[g][i])) { npurged++; }
  }
  return npurged;
}

static size_t fd_fill(int g, size_t n, size_t size, size_t* bsize) {   // two of three blocks freed; returns how many
  if (!alloc_filled(fd_ptrs[g], n, size, &fd_usable[g])) return 0;
  *bsize = _mi_ptr_page(fd_ptrs[g][0])->block_size;
  size_t nfreed = 0;
  for (size_t i = 0; i < n; i++) {
    if ((i % 3) != 0) { fd_freed[g][i] = fd_ptrs[g][i]; mi_free(fd_ptrs[g][i]); fd_ptrs[g][i] = NULL; nfreed++; }
  }
  return nfreed;
}

static bool test_floor_decay(void) {
  if (!purging_enabled) return true;
  bool ok_all = true;
  const long interval_ms = 2;
  const long old_interval = mi_option_get(mi_option_purge_holes_min_interval);
  memset(fd_ptrs, 0, sizeof(fd_ptrs)); memset(fd_freed, 0, sizeof(fd_freed));
  mi_option_set(mi_option_purge_holes_min_interval, interval_ms);
  const long old_epochs = mi_option_get(mi_option_purge_holes_large_floor_epochs);
  const long epochs = 64;
  mi_option_set(mi_option_purge_holes_large_floor_epochs, epochs);
  const size_t n = 24;
  size_t bsize0 = 0, bsize1 = 0;
  const size_t nfreed0 = fd_fill(0, n, large_size(1), &bsize0);
  if (nfreed0 == 0) { mi_option_set(mi_option_purge_holes_min_interval, old_interval); mi_option_set(mi_option_purge_holes_large_floor_epochs, old_epochs); return false; }
  if (!is_large_page(_mi_ptr_page(fd_ptrs[0][0]))) { fprintf(stderr, "(not in a large page here) "); free_all(fd_ptrs[0], n); mi_option_set(mi_option_purge_holes_min_interval, old_interval); mi_option_set(mi_option_purge_holes_large_floor_epochs, old_epochs); return true; }
  // room for all of the first group, and not for both
  const size_t floor = _mi_align_up(nfreed0 * bsize0 + bsize0, MI_KiB);
  mi_option_set(mi_option_purge_holes_large_floor, (long)(floor / MI_KiB));

  // 1. out of the hold, the free blocks stay under the floor, sweep after sweep
  fd_sweeps_past_the_hold(interval_ms);
  const size_t kept1 = _mi_page_purge_holes_floor_kept();
  const size_t purged1 = fd_count_purged(0, n, bsize0);
  sweep();
  const size_t kept1b = _mi_page_purge_holes_floor_kept();
  if (kept1 != nfreed0 * bsize0 || purged1 != 0 || kept1b != kept1) {
    fprintf(stderr, "\n  after the hold %zu bytes are kept (%zu at the next sweep) and %zu blocks purged: %zu free blocks of %zu bytes under a floor of %zu\n", kept1, kept1b, purged1, nfreed0, bsize0, floor);
    ok_all = false;
  }

  // 2. pages that are used later take the floor from the ones that were used before
  const size_t nfreed1 = fd_fill(1, n, large_size(0), &bsize1);
  if (nfreed1 == 0 || nfreed1 * bsize1 > floor || (nfreed0 * bsize0) + (nfreed1 * bsize1) <= floor) {
    fprintf(stderr, "\n  the second group does not fit the case: %zu blocks of %zu bytes\n", nfreed1, bsize1);
    ok_all = false;
  }
  fd_sweeps_past_the_hold(interval_ms);
  const size_t purged2_old = fd_count_purged(0, n, bsize0);
  const size_t purged2_new = fd_count_purged(1, n, bsize1);
  if (purged2_new != 0 || purged2_old == 0) {
    fprintf(stderr, "\n  with both groups out of the hold: %zu of %zu blocks of the pages that were used last are purged, %zu of %zu of the ones before\n", purged2_new, nfreed1, purged2_old, nfreed0);
    ok_all = false;
  }

  // 3. time alone takes it: with nothing swept for the hold, the epoch stands still, and the next sweep takes all of it
  //    by the clock (the scavenger comes for a thread that stays parked: `test_floor_idle_park`)
  const uint32_t epoch3 = _mi_page_purge_holes_epoch();
  const size_t kept2 = _mi_page_purge_holes_floor_kept();
  sleep_ms((unsigned)((epochs + 2) * interval_ms) + 100);
  const size_t kept3a = _mi_page_purge_holes_floor_kept();
  if (kept2 < nfreed1 * bsize1 || kept3a != kept2 || _mi_page_purge_holes_epoch() != epoch3 || fd_count_purged(1, n, bsize1) != 0 || fd_count_purged(0, n, bsize0) != purged2_old) {
    fprintf(stderr, "\n  with nothing swept for %ld ms: %zu bytes kept (%zu before), epoch %u (%u before)\n", epochs * interval_ms + 100, kept3a, kept2, _mi_page_purge_holes_epoch(), epoch3);
    ok_all = false;
  }
  sweep();
  const size_t kept3 = _mi_page_purge_holes_floor_kept();
  const size_t purged3 = fd_count_purged(0, n, bsize0) + fd_count_purged(1, n, bsize1);
  if (kept3 != 0 || purged3 != nfreed0 + nfreed1 || (uint32_t)(_mi_page_purge_holes_epoch() - epoch3) > 1) {
    fprintf(stderr, "\n  one sweep (%u epochs) after the hold by the clock %zu bytes are still kept, %zu of %zu free blocks purged\n", _mi_page_purge_holes_epoch() - epoch3, kept3, purged3, nfreed0 + nfreed1);
    ok_all = false;
  }
  if (!survivors_intact(fd_ptrs[0], n, fd_usable[0], "floor decay, first group")) { ok_all = false; }
  if (!survivors_intact(fd_ptrs[1], n, fd_usable[1], "floor decay, second group")) { ok_all = false; }
  fprintf(stderr, "(%zu KiB kept after the hold; then %zu of %zu blocks of the older pages purged and %zu of %zu of the newer; nothing kept %ld intervals later) ",
          kept1 / MI_KiB, purged2_old, nfreed0, purged2_new, nfreed1, epochs);
  mi_option_set(mi_option_purge_holes_large_floor_epochs, old_epochs);
  mi_option_set(mi_option_purge_holes_large_floor, 0);
  mi_option_set(mi_option_purge_holes_min_interval, old_interval);
  free_all(fd_ptrs[0], n); free_all(fd_ptrs[1], n);
  mi_collect(true);
  return ok_all;
}

// ---------------------------------------------------------------------------
// ..and a thread that stays parked in a process where nothing else sweeps: the scavenger comes back by the clock and
// takes what the park left under the floor
// ---------------------------------------------------------------------------
static bool test_floor_idle_park(void) {
  if (!purging_enabled) return true;
  bool ok_all = true;
  const long interval_ms = 20;
  const long epochs = 16;
  const long old_interval = mi_option_get(mi_option_purge_holes_min_interval);
  const long old_epochs = mi_option_get(mi_option_purge_holes_large_floor_epochs);
  memset(fd_ptrs, 0, sizeof(fd_ptrs)); memset(fd_freed, 0, sizeof(fd_freed));
  mi_option_set(mi_option_purge_holes_min_interval, interval_ms);
  mi_option_set(mi_option_purge_holes_large_floor_epochs, epochs);
  mi_option_set(mi_option_purge_holes_large_floor, 64 * 1024);
  const size_t n = 24;
  size_t bsize = 0;
  const size_t nfreed = fd_fill(0, n, large_size(1), &bsize);
  if (nfreed == 0 || !is_large_page(_mi_ptr_page(fd_ptrs[0][0]))) { fprintf(stderr, "(not in a large page here) "); }
  else if (!mi_on_thread_idle_start()) { fprintf(stderr, "(no scavenger to hand off to: not tested) "); }
  else {
    // (nothing in here may allocate or free)
    mi_tld_t* const tld = _mi_theap_default()->tld;
    const mi_msecs_t t_park = _mi_clock_now();
    while (mi_atomic_load_acquire(&tld->park_swept) != MI_PARK_SWEPT_DONE && _mi_clock_now() - t_park < 10000) { sleep_ms(2); }
    const size_t kept_done = _mi_page_purge_holes_floor_kept();
    const size_t purged_done = fd_count_purged(0, n, bsize);
    while (_mi_page_purge_holes_floor_kept() != 0 && _mi_clock_now() - t_park < 10000) { sleep_ms(2); }
    const mi_msecs_t t_gone = _mi_clock_now() - t_park;
    const size_t kept_end = _mi_page_purge_holes_floor_kept();
    const size_t purged_end = fd_count_purged(0, n, bsize);
    mi_on_thread_idle_end();
    if (kept_done != nfreed * bsize || purged_done != 0) { fprintf(stderr, "\n  the park was done with %zu bytes kept and %zu blocks purged (%zu free blocks of %zu bytes)\n", kept_done, purged_done, nfreed, bsize); ok_all = false; }
    if (kept_end != 0 || purged_end != nfreed || t_gone < epochs * interval_ms) { fprintf(stderr, "\n  %lld ms into the park %zu bytes are kept and %zu of %zu blocks purged\n", (long long)t_gone, kept_end, purged_end, nfreed); ok_all = false; }
    fprintf(stderr, "(%zu KiB kept by the park, given back after %lld ms) ", kept_done / MI_KiB, (long long)t_gone);
  }
  if (!survivors_intact(fd_ptrs[0], n, fd_usable[0], "floor, idle park")) { ok_all = false; }
  mi_option_set(mi_option_purge_holes_large_floor_epochs, old_epochs);
  mi_option_set(mi_option_purge_holes_large_floor, 0);
  mi_option_set(mi_option_purge_holes_min_interval, old_interval);
  free_all(fd_ptrs[0], n);
  mi_collect(true);
  return ok_all;
}

// ---------------------------------------------------------------------------
// A working set of more than one large page: the full page was abandoned and its blocks freed there. The next burst
// takes that page back before it extends the thread's own page into memory that was never touched.
// ---------------------------------------------------------------------------
static bool test_reclaim_before_extend_with_floor(bool all_of_it) {
  bool ok_all = true;
  void* ptrs[MAXB];
  size_t usable = 0;
  memset(ptrs, 0, sizeof(ptrs));
  const size_t size = large_size(1);
  if (!alloc_filled(ptrs, 1, size, &usable)) return false;
  const mi_page_t* const first = _mi_ptr_page(ptrs[0]);
  if (!is_large_page(first)) { fprintf(stderr, "(not in a large page here) "); free_all(ptrs, 1); return true; }
  const size_t per_page = first->reserved;
  const size_t n = per_page + 1;   // one full page, and one block of the next
  if (n > MAXB || !alloc_filled(ptrs + 1, n - 1, size, &usable)) { free_all(ptrs, n); return false; }
  const mi_page_t* const second = _mi_ptr_page(ptrs[n - 1]);
  const size_t capacity_before = second->capacity;
  if (second == first || !mi_page_is_abandoned(first)) { fprintf(stderr, "(the full page is not abandoned here) "); free_all(ptrs, n); return true; }
  // all of the full page (it comes back to this thread with its last block), or all but one (it stays abandoned); the one block of the own page stays in use
  const size_t i0 = (all_of_it ? 0 : 1);
  for (size_t i = i0; i < n - 1; i++) { mi_free(ptrs[i]); ptrs[i] = NULL; }
  if (!alloc_filled(ptrs + i0, n - 1 - i0, size, &usable)) { free_all(ptrs, n); return false; }
  for (size_t i = 0; i < n; i++) { pattern_fill(ptrs[i], usable, i); }
  size_t in_first = 0;
  for (size_t i = 0; i < n - 1; i++) { if (_mi_ptr_page(ptrs[i]) == first) { in_first++; } }
  if (in_first != per_page || second->capacity != capacity_before) {
    fprintf(stderr, "\n  (%s) %zu of %zu blocks came from the page that was abandoned; the own page grew from %zu to %u blocks\n", (all_of_it ? "all free" : "one in use"), in_first, per_page, capacity_before, (unsigned)second->capacity);
    ok_all = false;
  }
  if (!survivors_intact(ptrs, n, usable, "reclaim before extend")) { ok_all = false; }
  free_all(ptrs, n);
  mi_collect(true);
  return ok_all;
}

static bool test_reclaim_before_extend(void) {
  if (!purging_enabled) return true;
  const long old_interval = mi_option_get(mi_option_purge_holes_min_interval);
  mi_option_set(mi_option_purge_holes_min_interval, 2);
  mi_option_set(mi_option_purge_holes_large_floor, 64 * 1024);
  sweep();   // (a thread that is swept)
  const bool ok_all_free = test_reclaim_before_extend_with_floor(true);
  const bool ok_some = test_reclaim_before_extend_with_floor(false);
  mi_option_set(mi_option_purge_holes_large_floor, 0);
  mi_option_set(mi_option_purge_holes_min_interval, old_interval);
  return (ok_all_free && ok_some);
}

// ---------------------------------------------------------------------------
// a large page with no block in use stays by the same rule; and there is no floor without an interval to count in
// ---------------------------------------------------------------------------
static size_t pages_freed_by_sweeps(void) {
  mi_purge_holes_stats_t s; mi_purge_holes_stats_get(&s); return s.pages_freed;
}

static bool test_floor_free_page(void) {
  if (!purging_enabled) return true;
  bool ok_all = true;
  const long interval_ms = 2;
  const long epochs = 32;
  const long old_interval = mi_option_get(mi_option_purge_holes_min_interval);
  const long old_epochs = mi_option_get(mi_option_purge_holes_large_floor_epochs);
  mi_option_set(mi_option_purge_holes_min_interval, interval_ms);
  mi_option_set(mi_option_purge_holes_large_floor_epochs, epochs);
  mi_option_set(mi_option_purge_holes_large_floor, 8 * 1024);
  sweep();   // (this thread is one that is swept)
  const size_t size = (MI_LARGE_MAX_OBJ_SIZE / 2) + (MI_LARGE_MAX_OBJ_SIZE / 8);   // a size class of its own in this test
  void* p[3];
  size_t usable = 0;
  if (!alloc_filled(p, 3, size, &usable)) { free_all(p, 3); mi_option_set(mi_option_purge_holes_large_floor, 0); mi_option_set(mi_option_purge_holes_large_floor_epochs, old_epochs); mi_option_set(mi_option_purge_holes_min_interval, old_interval); return false; }
  const mi_page_t* const page = _mi_ptr_page(p[0]);
  if (!is_large_page(page) || _mi_ptr_page(p[1]) != page || _mi_ptr_page(p[2]) != page) {
    fprintf(stderr, "(the blocks are not in one large page here) ");
    free_all(p, 3);
  }
  else {
    const size_t freed0 = pages_freed_by_sweeps();
    const size_t bsize = page->block_size;
    void* const p0 = p[0];
    free_all(p, 3);   // the last block of the page is freed: it waits for the sweep
    fd_sweeps_past_the_hold(interval_ms);
    const size_t kept1 = _mi_page_purge_holes_floor_kept();
    // A collect that is not forced is not a sweep, and leaves what the floor keeps: every `generic_collect` allocations
    // do one, and a runtime does one at the end of a garbage collection, which is more often than a server gets a request.
    for (int i = 0; i < 40; i++) { mi_collect(false); }
    const bool there1 = (freed_block_page(p0, bsize) == page);
    sweep();
    const size_t kept1b = _mi_page_purge_holes_floor_kept();
    if (!there1 || kept1b != kept1) {
      fprintf(stderr, "\n  after collects that were not forced the page that the floor kept %s there: %zu bytes kept at the next sweep, %zu before\n", (there1 ? "is" : "is not"), kept1b, kept1);
      ok_all = false;
    }
    void* const q = mi_malloc(size);   // and the next allocation of that size takes it
    const bool same = (q != NULL && _mi_ptr_page(q) == page);
    if (kept1 == 0 || pages_freed_by_sweeps() != freed0 || !same) {
      fprintf(stderr, "\n  out of the hold: %zu bytes kept, %zu pages freed by the sweeps, the next block %s from that page\n", kept1, pages_freed_by_sweeps() - freed0, (same ? "is" : "is not"));
      ok_all = false;
    }
    mi_free(q);
    // Nor does such a collect free the page in the hold, where the first sweep has passed it and a later one decides..
    sweep();
    mi_collect(false);
    const bool there2 = (freed_block_page(p0, bsize) == page);
    fd_sweeps_past_the_hold(interval_ms);
    const size_t kept1c = _mi_page_purge_holes_floor_kept();
    if (!there2 || kept1c != kept1) {
      fprintf(stderr, "\n  after a collect that was not forced the page in the hold %s there: %zu bytes kept after the hold, %zu the time before\n", (there2 ? "is" : "is not"), kept1c, kept1);
      ok_all = false;
    }
    // ..but it does not wait for a sweep that never comes: no sweep has counted that page, so it is retired, as where
    // its last block is freed, and gone some collects later.
    void* const q2 = mi_malloc(size);
    const bool same2 = (q2 != NULL && _mi_ptr_page(q2) == page);
    mi_free(q2);
    sweep();
    for (int i = 0; i < 40; i++) { mi_collect(false); }
    if (!same2 || freed_block_page(p0, bsize) != NULL) {
      fprintf(stderr, "\n  a page in the hold is still there after 40 collects and no sweep\n");
      ok_all = false;
    }
    // A forced collect takes the one under the floor as well.
    void* const q3 = mi_malloc(size);
    const mi_page_t* const page3 = (q3 != NULL ? _mi_ptr_page(q3) : NULL);
    mi_free(q3);
    fd_sweeps_past_the_hold(interval_ms);
    const size_t kept1d = _mi_page_purge_holes_floor_kept();
    mi_collect(true);
    if (kept1d == 0 || page3 == NULL || freed_block_page(q3, bsize) != NULL) {
      fprintf(stderr, "\n  a forced collect left the page that the floor kept (%zu bytes)\n", kept1d);
      ok_all = false;
    }
    // (the rest of the case is about the page of before: one more, out of the hold and under the floor)
    void* const q4 = mi_malloc(size);
    const mi_page_t* const page4 = (q4 != NULL ? _mi_ptr_page(q4) : NULL);
    mi_free(q4);
    fd_sweeps_past_the_hold(interval_ms);
    if (page4 == NULL || _mi_page_purge_holes_floor_kept() == 0 || pages_freed_by_sweeps() != freed0) {
      fprintf(stderr, "\n  a new page with no block in use is not kept: %zu bytes, %zu pages freed by the sweeps\n", _mi_page_purge_holes_floor_kept(), pages_freed_by_sweeps() - freed0);
      ok_all = false;
    }
    // ..until it was not allocated from for that many epochs
    for (long i = 0; i < epochs + 8; i++) { sweep(); sleep_ms((unsigned)interval_ms + 1); }
    sweep();
    const size_t kept2 = _mi_page_purge_holes_floor_kept();
    if (kept2 != 0 || pages_freed_by_sweeps() == freed0) {
      fprintf(stderr, "\n  %ld epochs later: %zu bytes kept, %zu pages freed by the sweeps\n", epochs + 8, kept2, pages_freed_by_sweeps() - freed0);
      ok_all = false;
    }
    fprintf(stderr, "(%zu KiB of a page with no block in use kept, and taken again; freed %ld epochs later) ", kept1 / MI_KiB, epochs + 8);
  }

  // no interval, no epochs: no floor. Free blocks go at the next sweep, as `purge_holes_min_interval` 0 says.
  mi_option_set(mi_option_purge_holes_min_interval, 0);
  void* r[6];
  if (!alloc_filled(r, 6, size, &usable)) { free_all(r, 6); mi_option_set(mi_option_purge_holes_large_floor, 0); mi_option_set(mi_option_purge_holes_large_floor_epochs, old_epochs); mi_option_set(mi_option_purge_holes_min_interval, old_interval); return false; }
  {
  const size_t bsize = _mi_ptr_page(r[0])->block_size;
  const int64_t before = hole_stats().bytes_now;
  for (size_t i = 1; i < 5; i++) { mi_free(r[i]); r[i] = NULL; }
  sweep();
  const int64_t gone = hole_stats().bytes_now - before;
  if (_mi_page_purge_holes_floor_kept() != 0 || gone < (int64_t)(4 * (bsize - 2 * _mi_os_page_size()))) {
    fprintf(stderr, "\n  without an interval: %zu bytes kept, %lld bytes of 4 free blocks of %zu discarded by one sweep\n", _mi_page_purge_holes_floor_kept(), (long long)gone, bsize);
    ok_all = false;
  }
  if (!survivors_intact(r, 6, usable, "floor, no interval")) { ok_all = false; }
  free_all(r, 6);
  }
  mi_option_set(mi_option_purge_holes_large_floor, 0);
  mi_option_set(mi_option_purge_holes_large_floor_epochs, old_epochs);
  mi_option_set(mi_option_purge_holes_min_interval, old_interval);
  mi_collect(true);
  return ok_all;
}

int main(void) {
  mi_version();
  purging_enabled = mi_option_is_enabled(mi_option_purge_holes);
  // zero every hole before it is discarded, so that a discard that covers a live block shows on every OS (see `test-purge-holes.c`)
  mi_option_set(mi_option_purge_holes_eager_zero, 1);
  // no waiting for the free blocks of a large page (the cases that are about that set their own interval)
  mi_option_set(mi_option_purge_holes_min_interval, 0);
  // and nothing stays under the floor (`test_floor` sets its own)
  mi_option_set(mi_option_purge_holes_large_floor, 0);
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
  CHECK("park-defers-large-pages", test_park_defers_large());
  CHECK("recent-allocation", test_recent_allocation());
  CHECK("abandoned-young", test_abandoned_young());
  CHECK("sweepers-at-the-same-time", test_concurrent_sweepers());
  CHECK("floor", test_floor());
  CHECK("floor-decay", test_floor_decay());
  CHECK("floor-idle-park", test_floor_idle_park());
  CHECK("reclaim-before-extend", test_reclaim_before_extend());
  CHECK("floor-free-page", test_floor_free_page());

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
