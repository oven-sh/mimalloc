/* ----------------------------------------------------------------------------
Copyright (c) 2018-2025, Microsoft Research, Daan Leijen
This is free software; you can redistribute it and/or modify it under the
terms of the MIT license. A copy of the license can be found in the file
"LICENSE" at the root of this distribution.
-----------------------------------------------------------------------------*/

// Tests for "hole punching": discarding the memory of free blocks that sit
// inside a still-used page (`mi_purge_holes`, see the hole purging section in
// `src/page.c`).
//
// Run with MIMALLOC_PURGE_HOLES=0 to check the feature is a no-op when off:
// every data-integrity check still runs, and we additionally assert that no
// block is ever discarded.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "mimalloc.h"
#include "mimalloc-stats.h"
#include "mimalloc/internal.h"   // _mi_ptr_page, _mi_page_purged_count, _mi_page_purge_run_range

#include "testhelper.h"

static bool purging_enabled = true;   // MIMALLOC_PURGE_HOLES

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

// the process-wide hole-purging counters
typedef struct hole_stats_s {
  int64_t bytes_now;      // bytes discarded right now
  int64_t bytes_total;    // bytes ever discarded
  int64_t blocks_now;     // blocks held off the free lists right now
  int64_t discards;       // discard syscalls
  int64_t reuses;         // reuse syscalls
  int64_t pages_freed;    // pages the sweep found all-free and gave back to the arena
} hole_stats_t;

static hole_stats_t hole_stats(void) {
  mi_purge_holes_stats_t s;
  mi_purge_holes_stats_get(&s);
  hole_stats_t h;
  h.bytes_now   = (int64_t)s.purged_bytes;
  h.bytes_total = (int64_t)s.purged_bytes_total;
  h.blocks_now  = (int64_t)s.purged_blocks;
  h.discards    = (int64_t)s.discard_calls;
  h.reuses      = (int64_t)s.reuse_calls;
  h.pages_freed = (int64_t)s.pages_freed;
  return h;
}

// a byte pattern that depends on both the block and the offset, so a hole
// punched one OS page too far in either direction shows up as a mismatch.
static uint8_t pattern_byte(size_t id, size_t off) {
  return (uint8_t)((id * 131u) ^ (off * 7u) ^ (off >> 8));
}

static void pattern_fill(void* p, size_t size, size_t id) {
  uint8_t* b = (uint8_t*)p;
  for (size_t i = 0; i < size; i++) { b[i] = pattern_byte(id, i); }
}

// returns the offset of the first mismatching byte, or `size` when intact.
static size_t pattern_check(const void* p, size_t size, size_t id) {
  const uint8_t* b = (const uint8_t*)p;
  for (size_t i = 0; i < size; i++) {
    if (b[i] != pattern_byte(id, i)) return i;
  }
  return size;
}

// total number of discarded blocks over the pages holding the given pointers
static size_t purged_blocks(void** ptrs, size_t n) {
  size_t total = 0;
  mi_page_t* last = NULL;
  for (size_t i = 0; i < n; i++) {
    if (ptrs[i] == NULL) continue;
    mi_page_t* const page = _mi_ptr_page(ptrs[i]);
    if (page == last) continue;   // pointers are handed out per page, so this dedups most of it
    last = page;
    total += _mi_page_purged_count(page);
  }
  return total;
}

// every test that depends on a purge asserts one actually happened (or, with the feature
// off, that none did) -- otherwise the test passes vacuously when the pages turn out to be
// ineligible or the sweep no-ops.
static bool expect_purged(hole_stats_t before, const char* what) {
  const hole_stats_t after = hole_stats();
  const long long dbytes = (long long)(after.bytes_total - before.bytes_total);
  const long long ddisc  = (long long)(after.discards - before.discards);
  if (purging_enabled) {
    if (dbytes <= 0 || ddisc <= 0) {
      fprintf(stderr, "\n  %s: NOTHING was purged (bytes=%lld, discards=%lld)\n", what, dbytes, ddisc);
      return false;
    }
  }
  else if (dbytes != 0 || ddisc != 0) {
    fprintf(stderr, "\n  %s: purging is off but %lld bytes were discarded in %lld calls\n", what, dbytes, ddisc);
    return false;
  }
  return true;
}

static uint32_t rng_state = 0x853c49e6;
static uint32_t rng_next(void) {
  rng_state = (rng_state * 1103515245u) + 12345u;
  return (rng_state >> 8);
}

// ---------------------------------------------------------------------------
// 1. survivors: scattered live blocks must be byte-for-byte intact after purging
// ---------------------------------------------------------------------------

static bool test_survivors(size_t bsize, size_t count, size_t keep_every, bool* out_purged) {
  void** ptrs = (void**)calloc(count, sizeof(void*));
  if (ptrs == NULL) return false;
  bool ok_all = true;
  size_t usable = 0;

  for (size_t i = 0; i < count; i++) {
    ptrs[i] = mi_malloc(bsize);
    if (ptrs[i] == NULL) { ok_all = false; goto done; }
    usable = mi_usable_size(ptrs[i]);
    pattern_fill(ptrs[i], usable, i);
  }
  // free all but every `keep_every`-th block
  for (size_t i = 0; i < count; i++) {
    if ((i % keep_every) != 0) { mi_free(ptrs[i]); ptrs[i] = NULL; }
  }

  mi_purge_holes();

  const size_t npurged = purged_blocks(ptrs, count);
  if (out_purged != NULL) { *out_purged = (npurged > 0); }
  if (!purging_enabled && npurged != 0) {
    fprintf(stderr, "\n  purging is off but %zu blocks were discarded!\n", npurged);
    ok_all = false;
  }

  // every survivor must be intact
  for (size_t i = 0; i < count; i++) {
    if (ptrs[i] == NULL) continue;
    const size_t bad = pattern_check(ptrs[i], usable, i);
    if (bad != usable) {
      fprintf(stderr, "\n  CORRUPT survivor: bsize=%zu block=%zu offset=%zu (of %zu)\n", bsize, i, bad, usable);
      ok_all = false;
      break;
    }
  }
  // and the memory must still be writable (a decommit would fault here)
  for (size_t i = 0; i < count; i++) {
    if (ptrs[i] != NULL) { memset(ptrs[i], 0xA5, usable); }
  }

done:
  for (size_t i = 0; i < count; i++) { if (ptrs[i] != NULL) mi_free(ptrs[i]); }
  free(ptrs);
  return ok_all;
}

// ---------------------------------------------------------------------------
// 2. randomized churn: no aliasing, no lost blocks, contents conserved
// ---------------------------------------------------------------------------

#define CHURN_LIVE_MAX   400
#define CHURN_ITERS      20000

typedef struct live_s {
  void*  p;
  size_t usable;
  size_t id;
} live_t;

static bool test_churn(void) {
  // sizes straddle the small/medium boundary (10240) and the 16KB OS page of arm64
  static const size_t sizes[] = { 64, 512, 1024, 4096, 8192, 10239, 10240, 10241, 16384, 16385, 24576 };
  const size_t nsizes = sizeof(sizes) / sizeof(sizes[0]);

  live_t live[CHURN_LIVE_MAX];
  size_t nlive = 0;
  size_t next_id = 1;
  size_t total_allocs = 0, total_frees = 0, total_purges = 0;
  memset(live, 0, sizeof(live));

  for (size_t iter = 0; iter < CHURN_ITERS; iter++) {
    const uint32_t r = rng_next();
    const bool do_alloc = (nlive == 0) || (nlive < CHURN_LIVE_MAX && (r % 100) < 55);

    if (do_alloc) {
      const size_t sz = sizes[rng_next() % nsizes];
      void* const p = mi_malloc(sz);
      if (p == NULL) { fprintf(stderr, "\n  out of memory\n"); return false; }
      const size_t usable = mi_usable_size(p);
      if (usable < sz) { fprintf(stderr, "\n  usable %zu < requested %zu\n", usable, sz); return false; }
      // the fresh block may not alias any live block
      for (size_t i = 0; i < nlive; i++) {
        uint8_t* const a = (uint8_t*)p;
        uint8_t* const b = (uint8_t*)live[i].p;
        if (a < b + live[i].usable && b < a + usable) {
          fprintf(stderr, "\n  ALIAS: new %p+%zu overlaps live %p+%zu\n", p, usable, live[i].p, live[i].usable);
          return false;
        }
      }
      if (((uintptr_t)p % MI_MAX_ALIGN_SIZE) != 0) {
        fprintf(stderr, "\n  misaligned block %p\n", p);
        return false;
      }
      pattern_fill(p, usable, next_id);
      live[nlive].p = p;
      live[nlive].usable = usable;
      live[nlive].id = next_id;
      nlive++; next_id++; total_allocs++;
    }
    else {
      const size_t idx = rng_next() % nlive;
      const size_t bad = pattern_check(live[idx].p, live[idx].usable, live[idx].id);
      if (bad != live[idx].usable) {
        fprintf(stderr, "\n  CORRUPT live block %p (size %zu) at offset %zu\n", live[idx].p, live[idx].usable, bad);
        return false;
      }
      mi_free(live[idx].p);
      live[idx] = live[nlive - 1];
      nlive--; total_frees++;
    }

    if ((iter % 97) == 0) { mi_purge_holes(); total_purges++; }

    // spot-check a couple of survivors right after a purge
    if ((iter % 97) == 1 && nlive > 0) {
      for (size_t k = 0; k < 8 && k < nlive; k++) {
        const size_t idx = rng_next() % nlive;
        const size_t bad = pattern_check(live[idx].p, live[idx].usable, live[idx].id);
        if (bad != live[idx].usable) {
          fprintf(stderr, "\n  CORRUPT after purge: %p (size %zu) at offset %zu\n", live[idx].p, live[idx].usable, bad);
          return false;
        }
      }
    }
  }

  // final: every live block is intact and writable
  for (size_t i = 0; i < nlive; i++) {
    const size_t bad = pattern_check(live[i].p, live[i].usable, live[i].id);
    if (bad != live[i].usable) {
      fprintf(stderr, "\n  CORRUPT at end: %p (size %zu) at offset %zu\n", live[i].p, live[i].usable, bad);
      return false;
    }
    memset(live[i].p, 0x5A, live[i].usable);
    mi_free(live[i].p);
  }
  fprintf(stderr, "(%zu allocs, %zu frees, %zu purges) ", total_allocs, total_frees, total_purges);
  return true;
}

// ---------------------------------------------------------------------------
// 3. aligned allocation (JSC's MarkedBlock is 16KB aligned to 16KB)
// ---------------------------------------------------------------------------

static bool test_aligned(void) {
  enum { N = 128, ALIGN = 16384 };
  void* ptrs[N];
  const hole_stats_t before = hole_stats();
  for (size_t i = 0; i < N; i++) {
    ptrs[i] = mi_malloc_aligned(ALIGN, ALIGN);
    if (ptrs[i] == NULL) return false;
    if (((uintptr_t)ptrs[i] % ALIGN) != 0) {
      fprintf(stderr, "\n  not aligned: %p\n", ptrs[i]);
      return false;
    }
    pattern_fill(ptrs[i], ALIGN, i);
  }
  // free every other block, punch holes, then take the blocks back out of the bitmap
  for (size_t i = 1; i < N; i += 2) { mi_free(ptrs[i]); ptrs[i] = NULL; }
  mi_purge_holes();

  // these 16KB blocks must actually have been discarded (this is the JSC MarkedBlock case)
  if (!expect_purged(before, "aligned-16k")) return false;
  const size_t npurged = purged_blocks(ptrs, N);
  if (purging_enabled && npurged == 0) {
    fprintf(stderr, "\n  aligned-16k: no block of these pages is discarded\n");
    return false;
  }
  if (!purging_enabled && npurged != 0) return false;

  for (size_t i = 1; i < N; i += 2) {
    ptrs[i] = mi_malloc_aligned(ALIGN, ALIGN);
    if (ptrs[i] == NULL) return false;
    if (((uintptr_t)ptrs[i] % ALIGN) != 0) {
      fprintf(stderr, "\n  alignment lost after unpurge: %p\n", ptrs[i]);
      return false;
    }
    memset(ptrs[i], 0xC3, ALIGN);   // must be writable
  }
  // the survivors are untouched
  for (size_t i = 0; i < N; i += 2) {
    const size_t bad = pattern_check(ptrs[i], ALIGN, i);
    if (bad != ALIGN) {
      fprintf(stderr, "\n  CORRUPT aligned survivor %zu at offset %zu\n", i, bad);
      return false;
    }
  }
  for (size_t i = 0; i < N; i++) { mi_free(ptrs[i]); }
  return true;
}

// ---------------------------------------------------------------------------
// 4. page lifecycle: hole-punched pages go back to the arena and are re-used
//    (a MEM_DECOMMIT regression would fault on the first write below, on Windows)
// ---------------------------------------------------------------------------

static bool test_page_lifecycle(void) {
  enum { N = 512, SZ = 8192 };
  void** ptrs = (void**)calloc(N, sizeof(void*));
  if (ptrs == NULL) return false;
  const hole_stats_t before = hole_stats();

  for (size_t i = 0; i < N; i++) {
    ptrs[i] = mi_malloc(SZ);
    if (ptrs[i] == NULL) { free(ptrs); return false; }
    memset(ptrs[i], (int)(i & 0xFF), SZ);
  }
  // keep one block in every page: free all but every 8th
  for (size_t i = 0; i < N; i++) {
    if ((i % 8) != 0) { mi_free(ptrs[i]); ptrs[i] = NULL; }
  }
  mi_purge_holes();

  // the pages we are about to recycle must really carry holes, or the test below
  // (which is what would catch a MEM_DECOMMIT on Windows) proves nothing
  if (!expect_purged(before, "page-lifecycle")) { free(ptrs); return false; }
  const size_t npurged = purged_blocks(ptrs, N);
  if (purging_enabled && npurged == 0) {
    fprintf(stderr, "\n  page-lifecycle: no block of these pages is discarded\n");
    free(ptrs);
    return false;
  }

  // now free the survivors: the pages (holes and all) return to the arena
  for (size_t i = 0; i < N; i++) {
    if (ptrs[i] != NULL) { mi_free(ptrs[i]); ptrs[i] = NULL; }
  }
  mi_collect(true);

  // force the arena to hand the same slices back out, and write every byte
  for (size_t i = 0; i < N; i++) {
    ptrs[i] = mi_malloc(SZ);
    if (ptrs[i] == NULL) { free(ptrs); return false; }
    memset(ptrs[i], 0x77, SZ);
  }
  for (size_t i = 0; i < N; i++) {
    const uint8_t* b = (const uint8_t*)ptrs[i];
    for (size_t j = 0; j < SZ; j++) {
      if (b[j] != 0x77) { fprintf(stderr, "\n  re-used page byte %zu of block %zu is %u\n", j, i, b[j]); free(ptrs); return false; }
    }
    mi_free(ptrs[i]);
  }
  free(ptrs);
  return true;
}

// ---------------------------------------------------------------------------
// 5. abandoned pages: a thread exits while blocks in its pages are still live, so
//    the pages end up in the arena's abandoned list with no owning thread. This is
//    where most of the holes are (every page that ever became full is abandoned),
//    so `mi_purge_holes` must reach them.
// ---------------------------------------------------------------------------

#define ABANDON_N   (256)
#define ABANDON_SZ  (8192)
static void* abandoned_ptrs[ABANDON_N];

static void run_one_thread(void (*fun)(void));   // joins; defined at the bottom

static void abandoned_worker(void) {
  for (size_t i = 0; i < ABANDON_N; i++) {
    abandoned_ptrs[i] = mi_malloc(ABANDON_SZ);
    if (abandoned_ptrs[i] != NULL) { pattern_fill(abandoned_ptrs[i], ABANDON_SZ, i); }
  }
  // keep one live block in every 64KB page (8 blocks of 8KB); the rest become holes
  for (size_t i = 0; i < ABANDON_N; i++) {
    if ((i % 8) != 0) { mi_free(abandoned_ptrs[i]); abandoned_ptrs[i] = NULL; }
  }
  // the thread now exits: the pages are abandoned with a live block in each
}

static bool test_abandoned(void) {
  const hole_stats_t before = hole_stats();
  memset(abandoned_ptrs, 0, sizeof(abandoned_ptrs));
  run_one_thread(&abandoned_worker);

  mi_purge_holes();

  const size_t npurged = purged_blocks(abandoned_ptrs, ABANDON_N);
  if (purging_enabled) {
    if (!expect_purged(before, "abandoned-pages")) return false;
    if (npurged == 0) {
      fprintf(stderr, "\n  the abandoned pages of the exited thread were not purged\n");
      return false;
    }
  }
  else if (npurged != 0) {
    fprintf(stderr, "\n  purging is off but %zu abandoned blocks were discarded\n", npurged);
    return false;
  }

  // the survivors in those pages must be intact and writable
  for (size_t i = 0; i < ABANDON_N; i++) {
    if (abandoned_ptrs[i] == NULL) continue;
    const size_t bad = pattern_check(abandoned_ptrs[i], ABANDON_SZ, i);
    if (bad != ABANDON_SZ) {
      fprintf(stderr, "\n  CORRUPT survivor in abandoned page: block %zu at offset %zu\n", i, bad);
      return false;
    }
    memset(abandoned_ptrs[i], 0x3C, ABANDON_SZ);
  }
  // re-use the holes from this thread (they are reclaimed on allocation)
  void* re[ABANDON_N];
  for (size_t i = 0; i < ABANDON_N; i++) {
    re[i] = mi_malloc(ABANDON_SZ);
    if (re[i] == NULL) return false;
    memset(re[i], 0x5E, ABANDON_SZ);
  }
  for (size_t i = 0; i < ABANDON_N; i++) { mi_free(re[i]); }
  for (size_t i = 0; i < ABANDON_N; i++) { if (abandoned_ptrs[i] != NULL) { mi_free(abandoned_ptrs[i]); abandoned_ptrs[i] = NULL; } }
  return true;
}

// ---------------------------------------------------------------------------
// 6. option off == no-op
// ---------------------------------------------------------------------------

static bool test_option_off(void) {
  enum { N = 256, SZ = 4096 };
  void** ptrs = (void**)calloc(N, sizeof(void*));
  if (ptrs == NULL) return false;
  bool ok_all = true;

  const long saved = mi_option_get(mi_option_purge_holes);
  mi_option_set(mi_option_purge_holes, 0);

  for (size_t i = 0; i < N; i++) {
    ptrs[i] = mi_malloc(SZ);
    if (ptrs[i] == NULL) { ok_all = false; goto done; }
    pattern_fill(ptrs[i], SZ, i);
  }
  for (size_t i = 0; i < N; i++) {
    if ((i % 4) != 0) { mi_free(ptrs[i]); ptrs[i] = NULL; }
  }
  mi_purge_holes();

  if (purged_blocks(ptrs, N) != 0) {
    fprintf(stderr, "\n  purge_holes=0 but blocks were discarded\n");
    ok_all = false;
  }
  for (size_t i = 0; i < N; i++) {
    if (ptrs[i] == NULL) continue;
    if (pattern_check(ptrs[i], SZ, i) != SZ) { ok_all = false; break; }
  }

done:
  for (size_t i = 0; i < N; i++) { if (ptrs[i] != NULL) mi_free(ptrs[i]); }
  free(ptrs);
  mi_option_set(mi_option_purge_holes, saved);
  return ok_all;
}

// ---------------------------------------------------------------------------
// 6. unit test of the run -> OS-page-aligned-interior arithmetic
//    (this is what catches the arm64/16KB bug class on a 4KB CI box)
// ---------------------------------------------------------------------------

static bool check_run_range(size_t ospage, size_t bsize, uintptr_t pstart, size_t start, size_t len, size_t capacity) {
  uintptr_t ds = 0; size_t dsz = 0;
  const bool discarded = _mi_page_purge_run_range(ospage, bsize, pstart, start, len, &ds, &dsz);

  const uintptr_t lo = pstart + (start * bsize);         // first byte of the run
  const uintptr_t hi = lo + (len * bsize);               // one past the last byte of the run
  const uintptr_t pend = pstart + (capacity * bsize);    // one past the last block of the page

  if (!discarded) {
    if (ds != 0 || dsz != 0) { fprintf(stderr, "\n  no discard but range is %p+%zu\n", (void*)ds, dsz); return false; }
    // nothing discarded is only correct when the run holds no whole OS page
    const uintptr_t alo = (lo + ospage - 1) & ~(uintptr_t)(ospage - 1);
    const uintptr_t ahi = hi & ~(uintptr_t)(ospage - 1);
    if (ahi > alo) {
      fprintf(stderr, "\n  MISSED a whole OS page: ospage=%zu bsize=%zu run=[%zu,%zu)\n", ospage, bsize, start, start + len);
      return false;
    }
    return true;
  }

  if ((ds % ospage) != 0 || (dsz % ospage) != 0) {
    fprintf(stderr, "\n  not OS-page aligned: %p+%zu (ospage=%zu)\n", (void*)ds, dsz, ospage);
    return false;
  }
  if (ds < lo || ds + dsz > hi) {
    fprintf(stderr, "\n  OUTSIDE the run: discard [%p,%p) run [%p,%p)\n", (void*)ds, (void*)(ds + dsz), (void*)lo, (void*)hi);
    return false;
  }
  if (ds < pstart || ds + dsz > pend) {
    fprintf(stderr, "\n  OUTSIDE the page: discard [%p,%p) page [%p,%p)\n", (void*)ds, (void*)(ds + dsz), (void*)pstart, (void*)pend);
    return false;
  }
  // maximal: exactly the OS-page-aligned interior of the run
  const uintptr_t alo = (lo + ospage - 1) & ~(uintptr_t)(ospage - 1);
  const uintptr_t ahi = hi & ~(uintptr_t)(ospage - 1);
  if (ds != alo || dsz != (size_t)(ahi - alo)) {
    fprintf(stderr, "\n  not the aligned interior: got [%p,%p) want [%p,%p)\n",
            (void*)ds, (void*)(ds + dsz), (void*)alo, (void*)ahi);
    return false;
  }
  return true;
}

static bool test_run_arithmetic(void) {
  static const size_t ospages[] = { 4096, 16384 };
  static const size_t bsizes[]  = { 64, 512, 1024, 4096, 8192, 10240, 10256, 16384, 21856, 65536 };
  // page_start offsets: the block area does not have to be OS-page aligned
  static const size_t offsets[] = { 0, 8, 64, 128, 4096, 8192, 12288 };

  size_t cases = 0;
  for (size_t oi = 0; oi < sizeof(ospages)/sizeof(ospages[0]); oi++) {
    const size_t ospage = ospages[oi];
    for (size_t bi = 0; bi < sizeof(bsizes)/sizeof(bsizes[0]); bi++) {
      const size_t bsize = bsizes[bi];
      for (size_t fi = 0; fi < sizeof(offsets)/sizeof(offsets[0]); fi++) {
        const uintptr_t pstart = (uintptr_t)0x40000000u + offsets[fi];
        const size_t capacity = 128;
        for (size_t start = 0; start < capacity; start++) {
          for (size_t len = 0; len + start <= capacity; len++) {
            if (!check_run_range(ospage, bsize, pstart, start, len, capacity)) {
              fprintf(stderr, "  (case: ospage=%zu bsize=%zu offset=%zu start=%zu len=%zu)\n",
                      ospage, bsize, offsets[fi], start, len);
              return false;
            }
            cases++;
          }
        }
      }
    }
  }
  fprintf(stderr, "(%zu cases) ", cases);
  return true;
}

// ---------------------------------------------------------------------------
// portable "run one thread and join" (mirrors test-stress.c)
// ---------------------------------------------------------------------------

#ifdef _WIN32
#include <windows.h>
static void (*thread_fun)(void);
static DWORD WINAPI thread_entry(LPVOID param) {
  (void)param;
  thread_fun();
  return 0;
}
static void run_one_thread(void (*fun)(void)) {
  thread_fun = fun;
  DWORD tid = 0;
  HANDLE h = CreateThread(0, 8*1024L, &thread_entry, NULL, 0, &tid);
  if (h == NULL) return;
  WaitForSingleObject(h, INFINITE);
  CloseHandle(h);
}
#else
#include <pthread.h>
static void* thread_entry(void* param) {
  ((void (*)(void))param)();
  return NULL;
}
static void run_one_thread(void (*fun)(void)) {
  pthread_t t;
  if (pthread_create(&t, NULL, &thread_entry, (void*)(uintptr_t)fun) != 0) return;
  pthread_join(t, NULL);
}
#endif

// ---------------------------------------------------------------------------

int main(void) {
  mi_version();
  purging_enabled = mi_option_is_enabled(mi_option_purge_holes);
  // Zero every hole before it is discarded. Without this the survivor checks below are
  // VACUOUS in a release build on macOS: MADV_FREE_REUSABLE is lazy, so a discard that
  // wrongly covers a live block leaves its data intact until the kernel reclaims the page.
  mi_option_set(mi_option_purge_holes_eager_zero, 1);
  fprintf(stderr, "purge_holes is %s, os page size is %zu\n",
          (purging_enabled ? "ON" : "OFF"), (size_t)_mi_os_page_size());

  CHECK("run-arithmetic", test_run_arithmetic());

  // an eligible page holds at most MI_PAGE_PURGE_MAX_BLOCKS blocks; 1024-byte
  // blocks in a 64KB page (64 blocks) is the case the old `block_size >= os_page`
  // gate excluded, and the one that needs whole *runs* to cover an OS page.
  bool purged_any = false;
  bool p1 = false, p2 = false, p3 = false, p4 = false;
  CHECK("survivors-1024",  test_survivors(1024, 2048, 32, &p1));
  CHECK("survivors-4096",  test_survivors(4096, 1024, 16, &p2));
  CHECK("survivors-8192",  test_survivors(8192, 512, 8, &p3));
  CHECK("survivors-16384", test_survivors(16384, 256, 4, &p4));
  purged_any = (p1 || p2 || p3 || p4);
  // 256-byte blocks give 256 blocks per 64KB page: too many for the bitmap, so
  // the page is not eligible -- but it must of course still be uncorrupted.
  CHECK("survivors-256-ineligible", test_survivors(256, 4096, 32, NULL));

  if (purging_enabled) {
    CHECK("purging-actually-happened", purged_any);
  }
  else {
    CHECK("nothing-purged-when-off", !purged_any);
  }

  CHECK("churn-no-aliasing", test_churn());
  CHECK("aligned-16k", test_aligned());
  CHECK("page-lifecycle", test_page_lifecycle());
  CHECK("abandoned-pages", test_abandoned());
  CHECK("option-off-is-noop", test_option_off());

  // everything above is freed by now, so every hole must have been handed back
  mi_collect(true);
  const hole_stats_t end = hole_stats();
  fprintf(stderr, "holes: %lld bytes discarded in total over %lld discards / %lld reuses; %lld pages freed by the sweep; %lld bytes still discarded\n",
          (long long)end.bytes_total, (long long)end.discards, (long long)end.reuses,
          (long long)end.pages_freed, (long long)end.bytes_now);
  CHECK("no-holes-outstanding-at-exit", (end.bytes_now == 0 && end.blocks_now == 0));

  mi_stats_print(NULL);
  return print_test_summary();
}
