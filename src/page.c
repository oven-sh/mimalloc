/*----------------------------------------------------------------------------
Copyright (c) 2018-2024, Microsoft Research, Daan Leijen
This is free software; you can redistribute it and/or modify it under the
terms of the MIT license. A copy of the license can be found in the file
"LICENSE" at the root of this distribution.
-----------------------------------------------------------------------------*/

/* -----------------------------------------------------------
  The core of the allocator. Every segment contains
  pages of a certain block size. The main function
  exported is `mi_malloc_generic`.
----------------------------------------------------------- */

#include "mimalloc.h"
#include "mimalloc/internal.h"
#include "mimalloc/atomic.h"
#include "mimalloc/prim.h"
#include "mimalloc/prim-tls.h"
#include <stdio.h>

/* -----------------------------------------------------------
  Definition of page queues for each block size
----------------------------------------------------------- */

#define MI_IN_PAGE_C
#include "page-queue.c"
#undef MI_IN_PAGE_C


/* -----------------------------------------------------------
  Page helpers
----------------------------------------------------------- */

// Index a block in a page
static inline mi_block_t* mi_page_block_at(const mi_page_t* page, void* page_start, size_t block_size, size_t i) {
  MI_UNUSED(page);
  mi_assert_internal(page != NULL);
  mi_assert_internal(i <= page->reserved);
  return (mi_block_t*)((uint8_t*)page_start + (i * block_size));
}

static bool mi_page_extend_free(mi_theap_t* theap, mi_page_t* page);


/* -----------------------------------------------------------
  Hole purging: the `page->purged` bitmap of discarded OS pages.
  (see the "Page hole purging" section below for the algorithm)
----------------------------------------------------------- */

#if !MI_PADDING && !MI_ENCODE_FREELIST
// The fields before `xthread_free` are read by `mi_free` and `mi_page_alloc`;
// they must all sit in the first cache line (this is why `purged` is at the end).
typedef char mi_page_hot_fields_first_cacheline[(offsetof(mi_page_t,xthread_free) <= 64 && offsetof(mi_page_t,purged) > offsetof(mi_page_t,memid)) ? 1 : -1];
#endif

void _mi_page_purged_reset(mi_page_t* page) {
  for (size_t i = 0; i < MI_PAGE_PURGE_WORDS; i++) { page->purged[i] = 0; }
  page->unformed_purged_lo = 0;
  page->unformed_purged_hi = 0;
  page->swept_state = MI_PAGE_SWEPT_NONE;   // a fresh (or recycled) page was never swept
}

// The block state of the page, as the sweep sees it (see `_mi_page_purge_holes`): `(capacity << 32) | used`. Lossless
// while `capacity` fits 16 bits and `used` 32, and then `MI_PAGE_SWEPT_NONE` (all ones) is impossible:
// (`mi_page_used` is a 16-bit count)
typedef char mi_page_sweep_state_fits[(sizeof(((mi_page_t*)0)->capacity) <= 2) ? 1 : -1];

static inline uint64_t mi_page_sweep_state(const mi_page_t* page) {
  mi_assert_internal(mi_page_used(page) <= page->capacity);
  return (((uint64_t)page->capacity) << 32) | (uint64_t)mi_page_used(page);
}

// The hole sweep's epoch: a counter that only a sweep moves (`_mi_page_purge_holes_epoch_advance`), and at most once in
// `purge_holes_min_interval`. It is the only notion of time that the allocating side has for the rule that the free
// blocks of a large page stay while the page is in use (`_mi_page_purge_holes`): an allocation from a large page leaves
// the epoch it sees in `swept_state` (`mi_malloc_generic_fallback`), under a bit that no `(capacity,used)` has and with the
// top bit clear, which `MI_PAGE_SWEPT_NONE` has set. No clock is read there.
// Process-wide, as the clock and the option are (a page of any subprocess can be compared with it).
// (a cache line of their own: both are written once per epoch, and the counters of the sweep further down on every discard)
typedef struct mi_holes_time_s {
  _Atomic(int64_t)   start;   // `_mi_clock_now()` when the current epoch began; `MI_HOLES_EPOCH_MOVING` while a sweep moves it
  _Atomic(uintptr_t) epoch;   // read (relaxed) by every allocation from a large page
  uint8_t            padding[64 - sizeof(uintptr_t) - sizeof(int64_t)];
} mi_holes_time_t;
static mi_decl_cache_align mi_holes_time_t mi_holes_time;
#define mi_holes_epoch            (mi_holes_time.epoch)
#define mi_holes_epoch_start      (mi_holes_time.start)
#define MI_HOLES_EPOCH_MOVING     (INT64_MIN)

#define MI_PAGE_SWEPT_ALLOC       (((uint64_t)1) << 62)
#define MI_PAGE_SWEPT_ALLOC_MASK  (((uint64_t)3) << 62)
static inline void mi_page_sweep_state_set_alloc(mi_page_t* page) {
  // The low 32 bits of the epoch. Written only when it differs: a page that is allocated from all the time is written
  // to once per epoch, and not at all when nothing sweeps (the epoch stands still then).
  const uint64_t stamp = (MI_PAGE_SWEPT_ALLOC | (uint64_t)((uint32_t)mi_atomic_load_relaxed(&mi_holes_epoch)));
  if (page->swept_state != stamp) { page->swept_state = stamp; }
}
static inline bool mi_page_sweep_state_is_alloc(const mi_page_t* page, uint32_t* epoch) {
  if ((page->swept_state & MI_PAGE_SWEPT_ALLOC_MASK) != MI_PAGE_SWEPT_ALLOC) return false;
  *epoch = (uint32_t)page->swept_state;
  return true;
}

// How many epochs ago was the page allocated from, for a page that `mi_page_sweep_state_is_alloc`? In 32 bits: a page
// that was left for a multiple of 2^32 epochs (13 years of sweeping with the default interval) reads as young once
// more, and is taken two epochs later.
// The epoch is read now and not once per sweep: whoever hands us a page (a thread that parks, one that abandons it)
// does so with a release that we acquired, so what we read here is not older than what is in the page. (If it were,
// the page would read as very old and its blocks would go now, which is what happened to every page before this rule.)
static inline uint32_t mi_page_sweep_state_alloc_age(uint32_t epoch) {
  return (_mi_page_purge_holes_epoch() - epoch);
}

// Move the epoch on if the current one has lasted for `purge_holes_min_interval`. Called by every idle sweep as the first
// thing it does (`_mi_thread_idle_work`: a caller that sweeps every interval by its own clock is to find the epoch that its
// last call began as old as that); the clock is read here and nowhere on the allocating side.
// What the sweep relies on is that the epoch after the one of an allocation lasted for the whole interval by the time
// a third one is there to be seen (`_mi_page_purge_holes`): an epoch begins for everybody else at the increment below,
// so the time it began is a reading of the clock from AFTER that, and until that time is known nobody else moves it
// (a sweeper that is preempted in between delays the next epoch; it can never shorten this one). Any number of threads
// sweep at the same time (the scavenger for the parked ones, and each thread that calls `mi_on_thread_idle` itself).
void _mi_page_purge_holes_epoch_advance(void) {
  if (!mi_option_is_enabled(mi_option_purge_holes)) return;
  const mi_msecs_t interval = (mi_msecs_t)mi_option_get_clamp(mi_option_purge_holes_min_interval, 0, 3600000);
  if (interval <= 0) return;   // nothing is held then
  int64_t start = mi_atomic_loadi64_acquire(&mi_holes_epoch_start);
  if (start != MI_HOLES_EPOCH_MOVING) {
    const mi_msecs_t now = _mi_clock_now();
    if (now >= start && (now - start) < interval) return;   // to the millisecond, as the clock is. (A clock that went back: move it, the new start heals that.)
    if (mi_atomic_casi64_strong_acq_rel(&mi_holes_epoch_start, &start, MI_HOLES_EPOCH_MOVING)) {
      mi_atomic_add_acq_rel(&mi_holes_epoch, (uintptr_t)1);
      mi_atomic_storei64_release(&mi_holes_epoch_start, (int64_t)_mi_clock_now());
      return;
    }
  }
  // Another sweep is moving it right now, which is an increment and a reading of the clock: let it, so that this sweep
  // sees the new epoch (bounded: if that thread is not running we go on with the old one, which only keeps pages longer).
  for (int i = 0; i < 1000 && mi_atomic_loadi64_acquire(&mi_holes_epoch_start) == MI_HOLES_EPOCH_MOVING; i++) { mi_atomic_pause(); }
}

// (for `test-purge-holes-large.c`)
uint32_t _mi_page_purge_holes_epoch(void) {
  return (uint32_t)mi_atomic_load_acquire(&mi_holes_epoch);
}

// Bytes that the last sweeps of all threads left under `purge_holes_large_floor` (`_mi_page_purge_holes`). A tld gives
// its share (`holes_floor_kept`) back when its next sweep begins or when it goes away; stale only on the high side.
static _Atomic(size_t) mi_holes_floor_kept;

void _mi_page_purge_holes_floor_release(mi_tld_t* tld) {
  if (tld == NULL || tld->holes_floor_kept == 0) return;
  mi_atomic_sub_relaxed(&mi_holes_floor_kept, tld->holes_floor_kept);
  tld->holes_floor_kept = 0;
}

size_t _mi_page_purge_holes_floor_kept(void) {
  return mi_atomic_load_relaxed(&mi_holes_floor_kept);
}

static bool mi_holes_floor_take(mi_tld_t* tld, size_t bytes) {
  const size_t floor = mi_option_get_size(mi_option_purge_holes_large_floor);
  if (bytes == 0 || bytes > floor) return false;
  const size_t before = mi_atomic_add_relaxed(&mi_holes_floor_kept, bytes);
  if (before > floor - bytes) {
    mi_atomic_sub_relaxed(&mi_holes_floor_kept, bytes);
    return false;
  }
  tld->holes_floor_kept += bytes;
  return true;
}

// A `fork` can land between the two stores above, in a thread that is not there in the child.
void _mi_page_purge_holes_forked_child(void) {
  mi_atomic_store_relaxed(&mi_holes_floor_kept, (size_t)0);   // the shares are zeroed with the tlds (`subproc.c`)
  if (mi_atomic_loadi64_relaxed(&mi_holes_epoch_start) == MI_HOLES_EPOCH_MOVING) {
    mi_atomic_storei64_release(&mi_holes_epoch_start, (int64_t)_mi_clock_now());
  }
}

// Anything that changes which blocks are free *without* changing `(capacity,used)` must say so,
// or the next sweep would wrongly skip the page. That is exactly `mi_page_unpurge_range`: it puts
// discarded blocks back on the free list, and a purged block was already free.
static inline void mi_page_sweep_state_invalidate(mi_page_t* page) {
  page->swept_state = MI_PAGE_SWEPT_NONE;
}

static inline size_t mi_page_block_index(const mi_page_t* page, const mi_block_t* block) {
  mi_assert_internal((uint8_t*)block >= mi_page_start(page));
  return ((size_t)((uint8_t*)block - mi_page_start(page))) / page->block_size;
}

static inline mi_block_t* mi_page_block_index_at(const mi_page_t* page, size_t idx) {
  mi_assert_internal(idx < page->capacity);
  return (mi_block_t*)(mi_page_start(page) + (idx * page->block_size));
}

static inline void mi_page_purged_clear(mi_page_t* page, size_t k) {
  mi_assert_internal(k < MI_PAGE_PURGE_BITS);
  page->purged[k / 64] &= ~((uint64_t)1 << (k % 64));
}

// the units that the block at index `idx` overlaps (relative to `mi_page_purge_base`); `os_size` is `mi_page_purge_unit(page)`
static inline void mi_page_block_os_pages(const mi_page_t* page, size_t os_size, size_t idx, size_t* kfirst, size_t* klast) {
  const uintptr_t base = mi_page_purge_base_of(page, os_size);
  const uintptr_t lo = (uintptr_t)mi_page_start(page) + (idx * page->block_size);
  *kfirst = (size_t)(lo - base) / os_size;
  *klast = (size_t)((lo + page->block_size - 1) - base) / os_size;
}

// the blocks that overlap unit `k`, or `false` if that unit is not entirely inside
// the *committed* block area (see `_mi_page_purge_os_page_blocks`)
static bool mi_page_os_page_blocks(const mi_page_t* page, size_t os_size, size_t k, size_t* first, size_t* last) {
  mi_assert_internal(os_size == mi_page_purge_unit(page));
  return _mi_page_purge_os_page_blocks(os_size, page->block_size, (uintptr_t)mi_page_start(page),
                                       page->capacity, k, first, last);
}

// The number of blocks that are purged: the blocks overlapping a discarded OS page.
// (the blocks of a maximal run of discarded OS pages are contiguous)
size_t _mi_page_purged_count(const mi_page_t* page) {
  if (!mi_page_has_purged(page)) return 0;
  const size_t os_size = mi_page_purge_unit(page);
  size_t total = 0;
  size_t k = 0;
  while (k < MI_PAGE_PURGE_BITS) {
    if (!mi_page_os_page_purged(page, k)) { k++; continue; }
    const size_t k0 = k;
    while (k < MI_PAGE_PURGE_BITS && mi_page_os_page_purged(page, k)) { k++; }
    size_t first, last, first1, last1;
    if (!mi_page_os_page_blocks(page, os_size, k0, &first, &last)) { mi_assert_internal(false); continue; }
    if (!mi_page_os_page_blocks(page, os_size, k - 1, &first1, &last1)) { mi_assert_internal(false); continue; }
    total += (last1 - first) + 1;
  }
  return total;
}


#if (MI_DEBUG>=3)
static size_t mi_page_list_count(mi_page_t* page, mi_block_t* head) {
  mi_assert_internal(_mi_ptr_page(mi_page_start(page)) == page);
  const uint8_t* slice_start = mi_page_slice_start(page);
  mi_assert_internal(_mi_is_aligned(slice_start,MI_PAGE_ALIGN));
  size_t count = 0;
  while (head != NULL) {
    mi_assert_internal((uint8_t*)head - slice_start > (ptrdiff_t)MI_LARGE_PAGE_SIZE || page == _mi_ptr_page(head));
    count++;
    head = mi_block_next(page, head);
  }
  return count;
}

/*
// Start of the page available memory
static inline uint8_t* mi_page_area(const mi_page_t* page) {
  return _mi_page_start(_mi_page_segment(page), page, NULL);
}
*/

static bool mi_page_list_is_valid(mi_page_t* page, mi_block_t* p) {
  size_t psize;
  uint8_t* page_area = mi_page_area(page, &psize);
  mi_block_t* start = (mi_block_t*)page_area;
  mi_block_t* end   = (mi_block_t*)(page_area + psize);
  while(p != NULL) {
    if (p < start || p >= end) return false;
    p = mi_block_next(page, p);
  }
#if MI_DEBUG>3 // generally too expensive to check this
  if (page->free_is_zero) {
    const size_t ubsize = mi_page_usable_block_size(page);
    for (mi_block_t* block = page->free; block != NULL; block = mi_block_next(page, block)) {
      mi_assert_expensive(mi_mem_is_zero(block + 1, ubsize - sizeof(mi_block_t)));
    }
  }
#endif
  return true;
}

static bool mi_page_is_valid_init(mi_page_t* page) {
  mi_assert_internal(mi_page_block_size(page) > 0);
  mi_assert_internal(mi_page_used(page) <= page->capacity);
  mi_assert_internal(page->capacity <= page->reserved);
  
  mi_assert_internal(page->heap!=NULL);
  mi_theap_t* const page_theap = _mi_heap_theap_peek(page->heap);
  mi_assert_internal(page_theap == NULL || mi_page_theap(page)==page_theap || mi_page_theap(page)->tld->thread_id != _mi_thread_id());

  // const size_t bsize = mi_page_block_size(page);
  // uint8_t* start = mi_page_start(page);
  //mi_assert_internal(start + page->capacity*page->block_size == page->top);

  mi_assert_internal(mi_page_list_is_valid(page,page->free));
  mi_assert_internal(mi_page_list_is_valid(page,page->local_free));

  #if MI_DEBUG>3 // generally too expensive to check this
  if (page->free_is_zero) {
    const size_t ubsize = mi_page_usable_block_size(page);
    for(mi_block_t* block = page->free; block != NULL; block = mi_block_next(page,block)) {
      mi_assert_expensive(mi_mem_is_zero(block + 1, ubsize - sizeof(mi_block_t)));
    }
  }
  #endif 

  #if !MI_TRACK_ENABLED && !MI_TSAN
  mi_block_t* tfree = mi_page_thread_free(page);
  mi_assert_internal(mi_page_list_is_valid(page, tfree));
  //size_t tfree_count = mi_page_list_count(page, tfree);
  //mi_assert_internal(tfree_count <= page->thread_freed + 1);
  #endif

  // blocks are conserved: used (incl. not-yet-collected thread frees) + free-listed + purged == capacity.
  // purged blocks are free but held OFF every free list (their memory is discarded).
  size_t free_count = mi_page_list_count(page, page->free) + mi_page_list_count(page, page->local_free);
  mi_assert_internal(mi_page_used(page) + free_count + _mi_page_purged_count(page) == page->capacity);

  mi_assert_internal(mi_page_alloc_count(page) + mi_page_last_used(page) >= mi_page_used(page));
  return true;
}

extern mi_decl_hidden bool _mi_process_is_initialized;             // has mi_process_init been called?

bool _mi_page_is_valid(mi_page_t* page) {
  mi_assert_internal(mi_page_is_valid_init(page));
  #if MI_SECURE
  mi_assert_internal(page->keys[0] != 0);
  #endif

  // Hole-purging invariants (the conservation invariant is in mi_page_is_valid_init).
  // These are what make every existing test in the suite a test of the purge machinery.
  if (mi_page_has_purged(page)) {
    mi_assert_internal(mi_page_can_purge_holes(page));   // only eligible pages may carry holes
    // A purged block is OFF every free list: if it were on one, its `next` pointer would
    // live in discarded memory. (checked from the lists, which is O(list) instead of the
    // O(capacity * list) of the other direction -- pages can hold thousands of blocks)
    for (mi_block_t* b = page->free; b != NULL; b = mi_block_next(page, b)) {
      mi_assert_internal(!mi_page_block_is_purged(page, b));
    }
    for (mi_block_t* b = page->local_free; b != NULL; b = mi_block_next(page, b)) {
      mi_assert_internal(!mi_page_block_is_purged(page, b));
    }
    #if !MI_TRACK_ENABLED && !MI_TSAN
    for (mi_block_t* b = mi_page_thread_free(page); b != NULL; b = mi_block_next(page, b)) {
      mi_assert_internal(!mi_page_block_is_purged(page, b));
    }
    #endif
  }
  if (!mi_page_is_abandoned(page)) {
    //mi_assert_internal(!_mi_process_is_initialized);
    mi_assert_internal(page->heap!=NULL);
    mi_theap_t* const page_theap = _mi_heap_theap_peek(page->heap);
    mi_assert_internal(page_theap == NULL || mi_page_theap(page)==page_theap || mi_page_theap(page)->tld->thread_id != _mi_thread_id());
    {
      mi_page_queue_t* pq = mi_page_queue_of(page);
      mi_assert_internal(mi_page_queue_contains(pq, page));
      mi_assert_internal(pq->block_size==mi_page_block_size(page) || mi_page_is_huge(page) || mi_page_is_in_full(page));
      // mi_assert_internal(mi_theap_contains_queue(mi_page_theap(page),pq));
    }
  }
  mi_assert_internal(mi_page_alloc_count(page) + mi_page_last_used(page) >= mi_page_used(page));
  mi_assert_internal(mi_page_alloc_count(page) >= mi_page_last_alloc(page));
  return true;
}
#endif

#if MI_STATS
// Gets the theap belonging to a page.
static mi_theap_t* mi_theap_of_page(mi_page_t* page) {
  mi_theap_t* theap = page->theap;
  if mi_unlikely(mi_page_thread_id(page) != _mi_prim_thread_id()) { 
    // An abandoned page belongs to no theap: account on its heap (atomically). In particular not on the theap this
    // thread may have for that heap: we get here from a `mi_free` of a block of another thread, and a concurrent
    // `mi_heap_delete` may be detaching that theap and merging its (non-atomic) statistics (see `test-heap-mt.c`).
    // Where the abandoning thread is known, it is passed explicitly (`_mi_page_update_stats_for`).
    if (mi_page_is_abandoned(page)) return NULL;
    theap = _mi_page_associated_theap_peek(page);
  }
  return theap;
}
#endif

#if MI_SAMPLE==1 /* for ==2, the countdown is already done at every `alloc.c:mi_page_alloc_zero` */
static void mi_theap_adjust_sample_countdown(mi_theap_t* theap, mi_page_t* page, size_t alloc_count) 
{
  mi_assert_internal(mi_page_alloc_count(page) + mi_page_last_used(page) >= mi_page_used(page));    
  mi_assert_internal(theap!=NULL);
  if (theap->sample_rate==0) return;

  const size_t last_alloc = mi_page_last_alloc(page);
  if (alloc_count <= last_alloc) return;
  
  // update countdown
  const size_t bsize = mi_page_usable_block_size(page);  mi_assert_internal(bsize >= MI_PADDING_SIZE);
  const size_t alloc_diff = alloc_count - last_alloc;
  const uint64_t requested = (uint64_t)alloc_diff * (uint64_t)(bsize - MI_PADDING_SIZE);
  if (requested <= SIZE_MAX && theap->sample_countdown >= (size_t)requested) {
    theap->sample_countdown -= (size_t)requested;
  }
  else {
    theap->sample_requested   += (requested - theap->sample_countdown);
    theap->sample_countdown = 0;    
  }
}
#endif

#if MI_STATS
// Merge stats from the page into the corresponding theap or heap.
static void mi_theap_page_merge_stats(mi_theap_t* theap, const mi_page_t* page, size_t alloc_count, size_t free_count ) {
  // get heap (as the theap might be NULL)
  mi_heap_t* const heap = mi_page_heap(page);
  mi_assert_internal(_mi_theap_can_touch(theap));   // (fork: also a theap that `mi_heap_delete` detached, or that a parked thread handed to the scavenger)
  mi_theapx_stat_counter_increase(heap,theap,pages_stat_updates,1);
  mi_theapx_stat_counter_increase(heap,theap,pages_stat_update_count, alloc_count + free_count);

  // allocation sizes
  const size_t bsize = mi_page_usable_block_size(page);
  const uint64_t allocated = (uint64_t)alloc_count * (uint64_t)bsize;
  const uint64_t freed     = (uint64_t)free_count * (uint64_t)bsize;
  #if MI_STATS==1
  const uint64_t requested = allocated - ((uint64_t)alloc_count * MI_PADDING_SIZE);
  #endif
  mi_assert_internal(allocated <= INT64_MAX);  // safe to cast to int64_t for stats
  mi_assert_internal(freed <= INT64_MAX);
  
  // adjust stats
  if (bsize <= MI_LARGE_MAX_OBJ_SIZE) {
    const size_t bin = _mi_bin(bsize);      
    // allocations
    if (alloc_count > 0) {
      mi_theapx_stat_counter_increase(heap, theap, malloc_normal_count, alloc_count);
      mi_theapx_stat_increase(heap, theap, malloc_normal, allocated);
      mi_theapx_stat_increase(heap, theap, malloc_bins[bin], alloc_count);
      #if MI_STATS==1
      // use coarse total requested bytes
      mi_theapx_stat_counter_increase(heap, theap, malloc_requested, requested);      
      #endif
    }
    // frees
    if (free_count > 0) {
      mi_theapx_stat_decrease(heap, theap, malloc_normal, freed);
      mi_theapx_stat_decrease(heap, theap, malloc_bins[bin], free_count);      
    }
  }
  else {
    // allocations
    mi_assert_internal(alloc_count<=1);
    mi_assert_internal(free_count<=1);    
    if (alloc_count > 0) {      
      mi_theapx_stat_counter_increase(heap, theap, malloc_huge_count, alloc_count);
      mi_theapx_stat_increase(heap, theap, malloc_huge, allocated);
      #if MI_STATS==1
      // use coarse total requested bytes      
      mi_theapx_stat_counter_increase(heap, theap, malloc_requested, requested);      
      #endif
    }
    // frees
    if (free_count > 0) {
      mi_theapx_stat_decrease(heap, theap, malloc_huge, freed);
    }
  }
}

// Update stats for a page
static void mi_theap_page_update_stats(mi_theap_t* theap, mi_page_t* page) {
  mi_assert_internal(mi_page_alloc_count(page) + mi_page_last_used(page) >= mi_page_used(page));
  mi_assert_internal(mi_page_alloc_count(page) >= mi_page_last_alloc(page));
  
  // get stat counts
  const size_t used = mi_page_used(page);
  const size_t alloc_count = mi_page_alloc_count(page);
  const size_t last_used = mi_page_last_used(page);    
  mi_assert_internal(last_used + alloc_count >= used);
  mi_assert_internal(used <= UINT16_MAX);
  const size_t free_count = last_used + alloc_count - used;  

  #if MI_SAMPLE==1  // for ==2 it is already counted in every `alloc.c:mi_page_alloc_zero`
  if (theap!=NULL) { mi_theap_adjust_sample_countdown(theap,page,alloc_count); }
  #endif

  // update stats?
  if (alloc_count + free_count == 0) {
    mi_assert_internal(last_used == used);
    return;
  }

  // reset the `alloc_count` (and `last_alloc`), and set `last_used` to `used`
  #if MI_SIZE_SIZE >= 8
  page->xused.used_alloc = (used << 32) | used;
  #else
  page->xused.used_alloc = used;
  page->xlast_used = (uint16_t)used;
  page->xlast_alloc = 0;
  #endif
  mi_assert_internal(mi_page_alloc_count(page) + mi_page_last_used(page) >= mi_page_used(page));
  mi_assert_internal(mi_page_alloc_count(page) >= mi_page_last_alloc(page));

  mi_theap_page_merge_stats(theap, page, alloc_count, free_count);
}

void _mi_page_update_stats(mi_page_t* page) {         // called on abandoned pages etc.
  mi_theap_page_update_stats(mi_theap_of_page(page),page);
}

// `theapx`: a theap the calling thread may touch (`_mi_theap_can_touch`), or NULL to account on the heap of the page
void _mi_page_update_stats_for(mi_page_t* page, mi_theap_t* theapx) {
  mi_theap_page_update_stats(theapx,page);
}

#else
void _mi_page_update_stats(mi_page_t* page) {
  MI_UNUSED(page);
}
void _mi_page_update_stats_for(mi_page_t* page, mi_theap_t* theapx) {
  MI_UNUSED(page); MI_UNUSED(theapx);
}
#endif

// Called when the free list of a page is refilled.
static void mi_page_update_sample_countdown(mi_page_t* page) 
{  
  const size_t alloc_count = mi_page_alloc_count(page);  
  if mi_unlikely(alloc_count>=0x8000) {  // if the count could overflow, update stats so the counter is reset
    _mi_page_update_stats(page);
  }
  // (with `MI_SAMPLE==1` the blocks that were handed out are counted against the sample countdown by
  //  `mi_theap_count_page_allocs`, with `MI_SAMPLE==2` in every `alloc.c:mi_page_alloc_zero_ex`)
}

#if MI_SAMPLE==1
static void mi_page_set_last_alloc(mi_page_t* page, size_t alloc_count) {
  mi_assert_internal(alloc_count <= UINT16_MAX);
  #if MI_SIZE_SIZE >= 8
    page->xused.used_alloc = (alloc_count << 48) | (page->xused.used_alloc & (~MI_ZU(0) >> 16));
  #else
    page->xlast_alloc = (uint16_t)alloc_count;
  #endif
  mi_assert_internal(mi_page_alloc_count(page) + mi_page_last_used(page) >= mi_page_used(page));
  mi_assert_internal(mi_page_alloc_count(page) >= mi_page_last_alloc(page));
}

// Count the blocks that the fast path took from a page of `theap` since the last time against its sample countdown.
// A theap that samples does so in the generic path: before it refills the free list of the first page of a size class
// (`mi_theap_refill_is_sample`), and after an allocation in `mi_malloc_generic_fallback`. A theap that does not
// sample counts nothing, and `_mi_malloc_generic` has only a test of `sample_rate` for it.
static void mi_theap_count_page_allocs(mi_theap_t* theap, mi_page_t* page) {
  mi_assert_internal(page->theap == theap);
  const size_t alloc_count = mi_page_alloc_count(page);
  if (alloc_count==0) return;
  if mi_unlikely(alloc_count>=0x8000) {  // if the count could overflow, update stats so the counter is reset (this counts as well)
    _mi_page_update_stats_for(page,theap);
    return;
  }
  mi_theap_adjust_sample_countdown(theap,page,alloc_count);
  mi_page_set_last_alloc(page,alloc_count);
}

// When a theap starts to sample, what its pages handed out before is not to be counted.
void _mi_theap_sync_sample_counts(mi_theap_t* theap) {
  for (size_t bin = 0; bin <= MI_BIN_FULL; bin++) {
    for (mi_page_t* page = theap->pages[bin].first; page != NULL; page = page->next) {
      mi_page_set_last_alloc(page, mi_page_alloc_count(page));
    }
  }
}
#endif


/* -----------------------------------------------------------
  Page collect the `local_free` and `thread_free` lists
----------------------------------------------------------- */

static void mi_page_thread_collect_to_local(mi_page_t* page, mi_block_t* head)
{
  if (head == NULL) return;

  // find the last block in the list -- also to get a proper use count (without data races)
  size_t max_count = page->capacity; // cannot collect more than capacity
  size_t count = 1;
  mi_block_t* last = head;
  mi_block_t* next;
  while ((next = mi_block_next(page, last)) != NULL && count <= max_count) {
    count++;
    last = next;
  }

  // if `count > max_count` there was a memory corruption (possibly infinite list due to double multi-threaded free)
  if mi_unlikely(count > max_count) {
    _mi_error_message(EFAULT, "corrupted thread-free list (possibly due to a cross-thread double free)\n");
    return; // the thread-free items cannot be freed
  }
  // if `count > page->used` there was another kind memory corruption (either in the page meta-data or in the linked list)
  else if mi_unlikely(count > mi_page_used(page)) {
    _mi_error_message(EFAULT, "corrupted meta-data in thread-free list\n");
    return; // the thread-free items cannot be freed
  }

  // and append the current local free list
  mi_block_set_next(page, last, page->local_free);
  page->local_free = head;

  // update counts now
  mi_assert_internal(count <= UINT16_MAX);
  mi_assert_internal(mi_page_used(page) >= count);
  page->xused.used_alloc -= count; // page->used = page->used - (uint16_t)count;
}

// Collect the local `thread_free` list using an atomic exchange.
static void mi_page_thread_free_collect(mi_page_t* page)
{
  // atomically capture the thread free list
  mi_block_t* head;
  mi_thread_free_t tfreex;
  mi_thread_free_t tfree = mi_atomic_load_relaxed(&page->xthread_free);
  do {
    head = mi_tf_block(tfree);
    if mi_likely(head == NULL) return; // return if the list is empty
    tfreex = mi_tf_create(NULL,mi_tf_is_owned(tfree));  // set the thread free list to NULL
  } while (!mi_atomic_cas_weak_acq_rel(&page->xthread_free, &tfree, tfreex));  // release is enough?
  mi_assert_internal(head != NULL);

  // and move it to the local list
  mi_page_thread_collect_to_local(page, head);
}


// returns `true` if after collection `mi_page_immediate_available` is true.
static inline bool mi_page_free_quick_collect(mi_page_t* page) {
  if mi_likely(page->free != NULL) return true;
  if (page->local_free == NULL) return false;
  // move local_free to free
  page->free = page->local_free;
  page->local_free = NULL;
  page->free_is_zero = false;  
  mi_page_update_sample_countdown(page);
  return true;
}

/* -----------------------------------------------------------
  Page hole purging.

  A page returns to the arena only once *every* block is free, so one live
  block pins the whole page (64KB, 512KB or 4MB). Here we discard the memory of the
  free blocks inside a still-used page.

  The unit of discarding is an OS page, so that is what we track: `page->purged`
  is a bitmap over the OS pages of this mimalloc page (bit `k` = the OS page at
  `mi_page_purge_base(page) + k*os_page_size`). Its size does not depend on the
  size class, so *every* size class is eligible -- a run of adjacent free blocks
  covering one whole OS page is enough, no matter how small the blocks are.

  The bitmap has `MI_PAGE_PURGE_BITS` bits, which is an OS page per bit for a small
  and a medium page. A large (4 MiB) page has more OS pages than that unless they are
  16 KiB or more, so there a bit stands for the smallest power-of-two multiple of the
  OS page that makes the page fit: 16 KiB on a 4 KiB OS page (`mi_page_purge_unit`).
  That is the only difference: read "OS page" below as "unit" (and `os_size` in the
  code is the unit of the page at hand). The blocks of a large page are 96 KiB and up
  in steps of at least 16 KiB and start at the slice (where the page meta data is kept
  apart from the page), so each free block in it is discarded as a whole; nothing
  relies on that, the rule below decides unit by unit.

  We discard an OS page only when EVERY block overlapping it is free. That makes
  the purged state of a block *derived*:

      block i is purged  <=>  block i overlaps a discarded OS page

  which is what `mi_page_block_index_is_purged` computes. A purged block lost
  bytes, so it is taken OFF every free list: mimalloc threads its free list
  through the free blocks themselves and a discarded block cannot carry a `next`
  pointer. `_mi_page_unpurge_run` hands a whole run of discarded OS pages back at
  once: it calls `_mi_os_reuse` on exactly that byte range before any byte of it
  is written again (on macOS a discarded page stays MADV_FREE_REUSABLE --
  reclaimable by the kernel -- until it is REUSE'd), and pushes every block that
  is whole again back onto the free list.

  An OS page that is not entirely inside the block area is never discarded: it
  holds bytes we do not own (the page header, which lives *before* `page_start`,
  or blocks beyond `capacity` that are not formed yet). See
  `_mi_page_purge_os_page_blocks`, which is the only place this arithmetic lives.

  The discard goes through `_mi_os_discard`, which NEVER changes commit state
  (MADV_FREE_REUSABLE on macOS, MADV_DONTNEED on Linux -- both keep the mapping
  and demand-fault zeroes -- MEM_RESET on Windows). The arena tracks commit per
  64KB slice and cannot represent a sub-slice hole, so commit state MUST stay
  untouched: otherwise a page returned to the arena would be re-handed-out as
  "committed" and the first write into a hole would fault (on Windows only --
  silently fine on macOS/Linux). Note that `_mi_os_purge` is NOT usable here:
  with the default `MIMALLOC_PURGE_DECOMMITS=1` it decommits (and in debug
  builds it also mprotects the range PROT_NONE).
----------------------------------------------------------- */

// The blocks that overlap OS page `k` of a page whose block area starts at `page_start` and
// holds `capacity` blocks of `block_size` bytes. OS pages are counted from
// `align_down(page_start, os_page_size)`, so OS page `k` is an OS-page aligned range in
// absolute terms -- exactly what `_mi_os_discard` and `_mi_os_reuse` need.
// Returns `false` when OS page `k` is not *entirely* inside the block area: such an OS page
// holds bytes of the page header or of blocks that are not formed yet, and may never be
// discarded. Exposed for `test-purge-holes.c`.
bool _mi_page_purge_os_page_blocks(size_t os_page_size, size_t block_size, uintptr_t page_start,
                                   size_t capacity, size_t k, size_t* first, size_t* last)
{
  *first = 0;
  *last = 0;
  const uintptr_t base = _mi_align_down(page_start, os_page_size);
  const uintptr_t lo = base + (k * os_page_size);
  const uintptr_t hi = lo + os_page_size;
  const uintptr_t pend = page_start + (capacity * block_size);
  if (lo < page_start || hi > pend) return false;   // not entirely inside the block area
  *first = (size_t)(lo - page_start) / block_size;
  *last = (size_t)((hi - 1) - page_start) / block_size;
  mi_assert_internal(*first <= *last && *last < capacity);
  return true;
}

// Process-wide counters: this is the only way to see how much hole punching actually
// reclaims (`mi_stats_t` cannot grow, see `mi_purge_holes_stats_t`).
// (plain `int64_t` updated through the atomic i64 helpers, exactly as `mi_stat_counter_t` is)
static int64_t mi_holes_bytes;          // currently discarded
static int64_t mi_holes_blocks;         // currently held off the free lists
static int64_t mi_holes_bytes_total;
static int64_t mi_holes_discard_calls;
static int64_t mi_holes_reuse_calls;
static int64_t mi_holes_pages_freed;
static int64_t mi_holes_inelig_pages;   // pages the sweep cannot purge at all (see `mi_page_can_purge_holes`)
static int64_t mi_holes_inelig_bytes;
static int64_t mi_holes_inelig_free_bytes;
static int64_t mi_holes_unformed_bytes;         // unformed tail discarded right now
static int64_t mi_holes_unformed_bytes_total;
static int64_t mi_holes_unformed_discard_calls;
static int64_t mi_holes_unformed_reuse_calls;
static int64_t mi_holes_pages_skipped;          // pages the sweep skipped: unchanged since it last swept them
static int64_t mi_holes_blocks_visited;         // free-list blocks the sweep walked (the cost the skip avoids)
static int64_t mi_holes_full_sweeps;            // sweeps that walked every page regardless (`purge_holes_full_every`)

static inline size_t mi_holes_load(volatile int64_t* c) {
  const int64_t v = mi_atomic_addi64_relaxed(c, 0);
  return (v < 0 ? 0 : (size_t)v);
}

void mi_purge_holes_stats_get(mi_purge_holes_stats_t* stats) mi_attr_noexcept {
  if (stats == NULL) return;
  stats->purged_bytes       = mi_holes_load(&mi_holes_bytes);
  stats->purged_blocks      = mi_holes_load(&mi_holes_blocks);
  stats->purged_bytes_total = mi_holes_load(&mi_holes_bytes_total);
  stats->discard_calls      = mi_holes_load(&mi_holes_discard_calls);
  stats->reuse_calls        = mi_holes_load(&mi_holes_reuse_calls);
  stats->pages_freed        = mi_holes_load(&mi_holes_pages_freed);
  stats->ineligible_pages      = mi_holes_load(&mi_holes_inelig_pages);
  stats->ineligible_bytes      = mi_holes_load(&mi_holes_inelig_bytes);
  stats->ineligible_free_bytes = mi_holes_load(&mi_holes_inelig_free_bytes);
  stats->unformed_bytes         = mi_holes_load(&mi_holes_unformed_bytes);
  stats->unformed_bytes_total   = mi_holes_load(&mi_holes_unformed_bytes_total);
  stats->unformed_discard_calls = mi_holes_load(&mi_holes_unformed_discard_calls);
  stats->unformed_reuse_calls   = mi_holes_load(&mi_holes_unformed_reuse_calls);
  stats->pages_skipped          = mi_holes_load(&mi_holes_pages_skipped);
  stats->blocks_visited         = mi_holes_load(&mi_holes_blocks_visited);
  stats->full_sweeps            = mi_holes_load(&mi_holes_full_sweeps);
}

void _mi_page_holes_count_page_freed(void) {
  mi_atomic_addi64_relaxed(&mi_holes_pages_freed, 1);
}

// The pages the sweep could not touch at all, so it is visible what hole punching does
// *not* reach. A gauge over the last sweep: `mi_purge_holes` zeroes it before it starts.
void _mi_page_holes_reset_ineligible(void) {
  mi_atomic_addi64_relaxed(&mi_holes_inelig_pages, -(int64_t)mi_holes_load(&mi_holes_inelig_pages));
  mi_atomic_addi64_relaxed(&mi_holes_inelig_bytes, -(int64_t)mi_holes_load(&mi_holes_inelig_bytes));
  mi_atomic_addi64_relaxed(&mi_holes_inelig_free_bytes, -(int64_t)mi_holes_load(&mi_holes_inelig_free_bytes));
}

void _mi_page_holes_count_ineligible(const mi_page_t* page) {
  // Blocks are conserved (see `mi_page_is_valid_init`): free-listed == capacity - used - purged.
  // Eligibility is fixed for a page's lifetime (page size, block size, memid), so an ineligible
  // page never carries a purged block and the last term is 0 -- O(1), on every page of every sweep.
  mi_assert_internal(!mi_page_has_purged(page));
  const size_t nfree = (size_t)(page->capacity - mi_page_used(page));
  mi_atomic_addi64_relaxed(&mi_holes_inelig_pages, 1);
  mi_atomic_addi64_relaxed(&mi_holes_inelig_bytes, (int64_t)mi_page_size(page));
  mi_atomic_addi64_relaxed(&mi_holes_inelig_free_bytes, (int64_t)(nfree * page->block_size));
}

static void mi_holes_count_discard(size_t bytes) {
  mi_atomic_addi64_relaxed(&mi_holes_discard_calls, 1);
  mi_atomic_addi64_relaxed(&mi_holes_bytes_total, (int64_t)bytes);
  mi_atomic_addi64_relaxed(&mi_holes_bytes, (int64_t)bytes);
}

static void mi_holes_count_blocks_off(size_t blocks) {
  mi_atomic_addi64_relaxed(&mi_holes_blocks, (int64_t)blocks);
}

static void mi_holes_count_reuse(size_t bytes, size_t blocks, bool reused) {
  if (reused) {
    mi_atomic_addi64_relaxed(&mi_holes_reuse_calls, 1);
    mi_atomic_addi64_relaxed(&mi_holes_bytes, -(int64_t)bytes);
  }
  mi_atomic_addi64_relaxed(&mi_holes_blocks, -(int64_t)blocks);
}

// The state of a running sweep lives on the tld being swept (`tld->holes_sweep*`, see `types.h`),
// never in thread-locals of the sweeping thread. Besides the scavenger sweeping many tlds from one
// thread, this must not touch a `__thread` variable at all: `_mi_page_purge_holes_in_progress` is
// read from `mi_page_free_collect_ex`, inside the allocator, and on targets where `__thread` is
// emulated (Android before API 29) the first access on a thread allocates -- re-entering the
// collect that is reading it, without bound (oven-sh/bun#38051). The tld of the calling thread is
// reached through the default theap, which every TLS model can read without allocating.

// Re-entrancy guard: while a sweep is rewriting a page's free list and bitmap, a nested `mi_malloc`
// on the sweeping thread (only reachable through a user output function from a warning message)
// must not un-purge a hole from under it. That nested allocation comes out of the calling thread's
// own theaps, so it is its own tld that matters here: on the owner that is the tld being swept;
// the scavenger has no theaps of its own being swept (it sweeps a parked thread's theaps, and the
// abandoned pages it touches are claimed), so un-purging there is harmless.
bool _mi_page_purge_holes_in_progress(void) {
  mi_theap_t* const theap = _mi_theap_default();
  if (theap == NULL || theap->tld == NULL) return false;
  return theap->tld->holes_sweeping;
}

void _mi_page_purge_holes_begin(mi_tld_t* tld) {
  mi_assert_internal(tld != NULL && !tld->holes_sweeping);
  tld->holes_sweeping = true;
}

// Also folds the per-sweep counters into the process-wide ones. The sweep runs over every page of
// the thread, so a process-wide atomic per page would be a real cost on the very path we are making
// cheap: they accumulate on the tld and are folded in once per pass.
void _mi_page_purge_holes_end(mi_tld_t* tld) {
  mi_assert_internal(tld != NULL && tld->holes_sweeping);
  tld->holes_sweeping = false;
  if (tld->holes_sweep_skipped > 0) {
    mi_atomic_addi64_relaxed(&mi_holes_pages_skipped, (int64_t)tld->holes_sweep_skipped);
    tld->holes_sweep_skipped = 0;
  }
  if (tld->holes_sweep_visited > 0) {
    mi_atomic_addi64_relaxed(&mi_holes_blocks_visited, (int64_t)tld->holes_sweep_visited);
    tld->holes_sweep_visited = 0;
  }
}

// Called once per idle sweep of `tld`'s heaps, before its passes (`mi_purge_holes_of`): decides
// whether this sweep ignores `page->swept_state` (see `_mi_page_purge_holes`), and notes its epoch.
void _mi_page_purge_holes_sweep_begin(mi_tld_t* tld) {
  const long every = mi_option_get(mi_option_purge_holes_full_every);
  const size_t seq = ++tld->holes_sweep_seq;
  tld->holes_sweep_full = (every > 0 && (seq % (size_t)every) == 0);
  if (tld->holes_sweep_full) { mi_atomic_addi64_relaxed(&mi_holes_full_sweeps, 1); }
  _mi_page_purge_holes_floor_release(tld);   // this sweep decides again what stays under the floor
  tld->holes_free_held = 0;                  // ..also for the pages that were left to it when their last block was freed
  tld->holes_sweep_epoch = _mi_page_purge_holes_epoch();   // the sweep owns the time that large pages are held by (see `_mi_page_purge_holes`): no page of this sweep is compared with an older epoch than this
}

static inline bool mi_page_bits_at(const uint64_t* bits, size_t k) {
  mi_assert_internal(k < MI_PAGE_PURGE_BITS);
  return ((bits[k / 64] >> (k % 64)) & 1) != 0;
}

// does the block at index `idx` overlap any unit in `bits`?
static bool mi_page_block_overlaps(const mi_page_t* page, size_t os_size, size_t idx, const uint64_t* bits) {
  size_t kfirst, klast;
  mi_page_block_os_pages(page, os_size, idx, &kfirst, &klast);
  for (size_t k = kfirst; k <= klast && k < MI_PAGE_PURGE_BITS; k++) {
    if (mi_page_bits_at(bits, k)) return true;
  }
  return false;
}

// Bring the discarded OS pages [k0,k1] back. `discarded` is false when the discard itself
// failed, in which case the memory was never released and needs no `reuse`.
// Tells the OS we are using the memory again *before* any block in it is written to, then
// pushes every block that is whole again back onto the free list. A block at either end of
// the range can still overlap a hole we are not touching: those stay purged.
static void mi_page_unpurge_range(mi_page_t* page, size_t k0, size_t k1, bool discarded) {
  mi_assert_internal(k0 <= k1 && k1 < MI_PAGE_PURGE_BITS);
  const size_t os_size = mi_page_purge_unit(page);
  const uintptr_t dstart = mi_page_purge_base_of(page, os_size) + (k0 * os_size);
  const size_t dsize = ((k1 - k0) + 1) * os_size;
  if (discarded) { _mi_os_reuse(mi_page_subproc(page), (void*)dstart, dsize); }

  // clear the bits first: `mi_page_block_index_is_purged` then tells us exactly which
  // blocks are whole again
  for (size_t k = k0; k <= k1; k++) { mi_page_purged_clear(page, k); }
  mi_page_sweep_state_invalidate(page);   // the free list is about to grow, but `used`/`capacity` will not
                                          // (before any early return below: the bits are already cleared)

  size_t first, last, first1, last1;
  if (!mi_page_os_page_blocks(page, os_size, k0, &first, &last) ||
      !mi_page_os_page_blocks(page, os_size, k1, &first1, &last1)) {
    mi_assert_internal(false);   // a discarded OS page is always entirely inside the block area
    return;
  }
  // every block in [first,last1] was free when we discarded, and a purged block cannot be
  // allocated or freed, so they are all still free
  size_t nblocks = 0;
  for (size_t i = last1 + 1; i > first; i--) {   // descending: the free list stays in address order
    const size_t idx = i - 1;
    if (mi_page_block_index_is_purged(page, idx)) continue;   // still overlaps another hole
    mi_block_t* const block = mi_page_block_index_at(page, idx);
    mi_block_set_next(page, block, page->free);
    page->free = block;
    nblocks++;
  }
  page->free_is_zero = false;
  mi_page_sweep_state_invalidate(page);   // the free list grew, but `used`/`capacity` did not
  mi_holes_count_reuse(dsize, nblocks, discarded);
}


/* -----------------------------------------------------------
  The unformed tail.

  The blocks in `[capacity, reserved)` were never handed out: `mi_page_extend_free`
  formats them lazily, a few OS pages worth at a time. They still cost memory though --
  a page is carved from an arena slice that had a previous life, so its tail is already
  resident, dirtying memory for blocks that may never exist.

  So we discard it too, but NOT through the `purged` bitmap: a bit there means "this block
  is free and off every free list" (`_mi_page_is_valid`, `mi_check_is_double_freex`), and an
  unformed block is on no list and has no identity yet. The region is contiguous and only
  shrinks from the left as `capacity` grows, so two offsets say everything there is to say.

  `mi_page_extend_free` calls `_mi_page_unpurge_unformed_upto` on exactly the range it is
  about to format, *before* it writes the first free-list pointer into it.
----------------------------------------------------------- */

// Can this page's memory be discarded at ALL? Pinned (large/huge OS pages) memory cannot be
// madvise'd away, and an arena with a custom commit function owns its own decommit. Note this
// is deliberately weaker than `mi_page_can_purge_holes`, which also rejects pages whose OS
// pages do not fit the `purged` bitmap (none since the unit of the bitmap follows the size of the page) and a page of a
// single block -- the unformed tail needs no bitmap.
static bool mi_page_holes_madvisable(const mi_page_t* page) {
  if (page->memid.is_pinned) return false;
  const mi_arena_t* const arena = mi_memid_arena(page->memid);
  return (arena == NULL || arena->commit_fun == NULL);
}

// The OS pages that lie wholly inside the unformed tail *and* inside the committed part of
// the page: `[align_up(page_start + capacity*bs), align_down(min(page_start + reserved*bs, committed_end)))`.
// Empty (lo == hi) when there is no tail, when it is smaller than an OS page, or when the
// page has no committed memory there (`slice_committed`).
static void mi_page_unformed_tail_range(const mi_page_t* page, uintptr_t* lo, uintptr_t* hi) {
  *lo = 0; *hi = 0;
  if (page->capacity >= page->reserved) return;    // no tail
  const size_t os_size = _mi_os_page_size();
  const uintptr_t pstart = (uintptr_t)mi_page_start(page);
  const uintptr_t tlo = pstart + ((size_t)page->capacity * page->block_size);
  uintptr_t thi = pstart + ((size_t)page->reserved * page->block_size);
  const uintptr_t climit = pstart + mi_page_committed(page);   // never discard memory that is not committed
  if (thi > climit) { thi = climit; }
  const uintptr_t alo = _mi_align_up(tlo, os_size);
  const uintptr_t ahi = _mi_align_down(thi, os_size);
  if (alo >= ahi) return;
  *lo = alo;
  *hi = ahi;
}

size_t _mi_page_unformed_purged_bytes(const mi_page_t* page) {
  return (page->unformed_purged_hi > page->unformed_purged_lo
            ? (size_t)(page->unformed_purged_hi - page->unformed_purged_lo) : 0);
}

// Discard the OS pages of the unformed tail that are not discarded already.
// The part of the unformed tail that is not discarded yet, `[*dlo,*hi)`, and where the discarded part starts once that is
// discarded too (`*lo`). Returns false if there is none.
static bool mi_page_unformed_tail_todo(const mi_page_t* page, uintptr_t* lo, uintptr_t* dlo, uintptr_t* hi) {
  *lo = 0; *dlo = 0; *hi = 0;
  if (!mi_page_holes_madvisable(page)) return false;
  mi_page_unformed_tail_range(page, lo, hi);
  if (*lo >= *hi) return false;
  const uintptr_t pstart = (uintptr_t)mi_page_start(page);
  mi_assert_internal(*hi - pstart <= UINT32_MAX);   // only a huge page can be that big, and it has no tail
  if (*hi - pstart > UINT32_MAX) return false;

  // the part of the tail that is not discarded yet (the tail can only grow to the right,
  // when `mi_page_extend_free` commits more of the page)
  *dlo = *lo;
  const size_t already = _mi_page_unformed_purged_bytes(page);
  if (already > 0) {
    mi_assert_internal(pstart + page->unformed_purged_lo >= *lo);   // extend un-discards what it formats
    const uintptr_t uhi = pstart + page->unformed_purged_hi;
    if (uhi > *dlo) { *dlo = uhi; }
    *lo = pstart + page->unformed_purged_lo;
  }
  return (*dlo < *hi);   // (else nothing new)
}

static void mi_page_purge_unformed_tail(mi_page_t* page) {
  uintptr_t lo, dlo, hi;
  if (!mi_page_unformed_tail_todo(page, &lo, &dlo, &hi)) return;
  const uintptr_t pstart = (uintptr_t)mi_page_start(page);

  if (!_mi_os_discard(mi_page_subproc(page), (void*)dlo, (size_t)(hi - dlo))) return;   // the discard failed: leave the page as it was
  page->unformed_purged_lo = (uint32_t)(lo - pstart);
  page->unformed_purged_hi = (uint32_t)(hi - pstart);
  mi_atomic_addi64_relaxed(&mi_holes_unformed_discard_calls, 1);
  mi_atomic_addi64_relaxed(&mi_holes_unformed_bytes_total, (int64_t)(hi - dlo));
  mi_atomic_addi64_relaxed(&mi_holes_unformed_bytes, (int64_t)(hi - dlo));
}

// Has the unformed tail OS pages that `mi_page_purge_unformed_tail` would discard now?
static bool mi_page_unformed_tail_pending(const mi_page_t* page) {
  uintptr_t lo, dlo, hi;
  return mi_page_unformed_tail_todo(page, &lo, &dlo, &hi);
}

// Tell the OS we are using the discarded unformed tail below `end` again, *before* anything
// in it is written to. `end` is an absolute address (`UINTPTR_MAX` for the whole tail); it is
// rounded up to an OS page, as the discard covers whole OS pages.
void _mi_page_unpurge_unformed_upto(mi_page_t* page, uintptr_t end) {
  if (_mi_page_unformed_purged_bytes(page) == 0) return;
  const size_t os_size = _mi_os_page_size();
  const uintptr_t pstart = (uintptr_t)mi_page_start(page);
  const uintptr_t rlo = pstart + page->unformed_purged_lo;
  const uintptr_t rhi = pstart + page->unformed_purged_hi;
  uintptr_t rend;
  if (end >= rhi) { rend = rhi; }   // (also the `UINTPTR_MAX` case: `_mi_align_up` would overflow)
  else {
    rend = _mi_align_up(end, os_size);
    if (rend > rhi) { rend = rhi; }
  }
  if (rend <= rlo) return;   // nothing of the discarded tail is needed yet

  _mi_os_reuse(mi_page_subproc(page), (void*)rlo, (size_t)(rend - rlo));
  if (rend >= rhi) { page->unformed_purged_lo = 0; page->unformed_purged_hi = 0; }
  else { page->unformed_purged_lo = (uint32_t)(rend - pstart); }
  mi_atomic_addi64_relaxed(&mi_holes_unformed_reuse_calls, 1);
  mi_atomic_addi64_relaxed(&mi_holes_unformed_bytes, -(int64_t)(rend - rlo));
}


// Walk the free list of a page and discard every OS page in it that holds no live block.
// Returns false if any discard failed: those blocks went straight back on the free list and the
// page must be swept again, so the caller must not record it as swept.
static bool mi_page_purge_holes_walk(mi_page_t* page, mi_tld_t* tld) {
  if (page->free == NULL) return true;                    // nothing to take off the free list

  const size_t os_size = mi_page_purge_unit(page);        // the unit of the bitmap: the OS page in all but large pages
  const size_t nbits = mi_page_purge_bits_of(page, os_size);
  mi_assert_internal(nbits <= MI_PAGE_PURGE_BITS);
  if (nbits > MI_PAGE_PURGE_BITS) return true;
  bool complete = true;

  // 1. count, per OS page, the blocks on the free list that overlap it
  uint16_t nfree[MI_PAGE_PURGE_BITS];
  _mi_memzero(nfree, nbits * sizeof(uint16_t));
  size_t nvisited = 0;
  for (mi_block_t* b = page->free; b != NULL; b = mi_block_next(page, b)) {
    const size_t idx = mi_page_block_index(page, b);
    mi_assert_internal(idx < page->capacity);
    mi_assert_internal(!mi_page_block_index_is_purged(page, idx));   // it is on the free list, so not purged
    nvisited++;
    size_t kfirst, klast;
    mi_page_block_os_pages(page, os_size, idx, &kfirst, &klast);
    for (size_t k = kfirst; k <= klast && k < nbits; k++) {
      mi_assert_internal(nfree[k] < UINT16_MAX);
      nfree[k]++;
    }
  }
  tld->holes_sweep_visited += nvisited;   // folded into the process-wide counter at the end of the pass

  // 2. an OS page can be discarded when *every* block overlapping it is free -- either on the
  //    free list, or purged already. Of the blocks overlapping an OS page, only the first and
  //    the last can stick out into another OS page, so only those two can be purged already
  //    (any other block lies entirely inside this OS page, whose bit is clear here).
  uint64_t todo[MI_PAGE_PURGE_WORDS];
  for (size_t i = 0; i < MI_PAGE_PURGE_WORDS; i++) { todo[i] = 0; }
  size_t ntodo = 0;
  for (size_t k = 0; k < nbits; k++) {
    if (mi_page_os_page_purged(page, k)) continue;                 // discarded already
    size_t first, last;
    if (!mi_page_os_page_blocks(page, os_size, k, &first, &last)) continue; // not entirely inside the block area
    size_t nfreek = nfree[k];
    if (mi_page_block_index_is_purged(page, first)) { nfreek++; }
    if (last != first && mi_page_block_index_is_purged(page, last)) { nfreek++; }
    mi_assert_internal(nfreek <= (last - first) + 1);
    if (nfreek != (last - first) + 1) continue;                    // some block overlapping it is still live
    todo[k / 64] |= ((uint64_t)1 << (k % 64));
    ntodo++;
  }
  if (ntodo == 0) return true;   // nothing discardable: the page IS fully swept

  // 3. rebuild the free list without the blocks that are about to lose memory. This must
  //    happen *before* the discard: it walks `next` pointers that live in the very memory
  //    we are about to discard.
  mi_block_t* keep = NULL;
  size_t ndropped = 0;
  mi_block_t* b = page->free;
  while (b != NULL) {
    mi_block_t* const next = mi_block_next(page, b);
    if (mi_page_block_overlaps(page, os_size, mi_page_block_index(page, b), todo)) {
      ndropped++;   // it becomes purged in step 4
    }
    else {
      mi_block_set_next(page, b, keep);
      keep = b;
    }
    b = next;
  }
  page->free = keep;
  page->free_is_zero = false;   // discarded memory reads back zero or stale; never assume

  // 4. mark the OS pages as discarded (this is what makes the blocks we just dropped
  //    "purged"), then discard them a maximal run at a time.
  for (size_t i = 0; i < MI_PAGE_PURGE_WORDS; i++) { page->purged[i] |= todo[i]; }
  mi_holes_count_blocks_off(ndropped);
  size_t k = 0;
  while (k < nbits) {
    if (!mi_page_bits_at(todo, k)) { k++; continue; }
    const size_t k0 = k;
    while (k < nbits && mi_page_bits_at(todo, k)) { k++; }
    const size_t dsize = (k - k0) * os_size;
    if (_mi_os_discard(mi_page_subproc(page), (void*)(mi_page_purge_base_of(page, os_size) + (k0 * os_size)), dsize)) {
      mi_holes_count_discard(dsize);
    }
    else {
      // the discard failed: the memory is intact, so hand these blocks straight back
      mi_page_unpurge_range(page, k0, k - 1, false /* nothing was discarded, so no reuse */);
      complete = false;
    }
  }
  return complete;
}

// What the floor is about in a large page: the blocks on its free list (some forty at the most), which were in use and
// are what the next allocations from the page take. Not the blocks that were never formed: nobody wrote to those, so
// they are in memory only if whoever had that part of the arena before left them there, and a page with two blocks of
// 256 KiB would count for 4 MiB of the floor with them. They go as they always did (`mi_page_purge_unformed_tail`).
static size_t mi_page_large_resident_free(const mi_page_t* page) {
  size_t nfree = 0;
  for (mi_block_t* b = page->free; b != NULL; b = mi_block_next(page, b)) { nfree++; }
  return (nfree * mi_page_block_size(page));
}

static void mi_page_purge_holes_now(mi_page_t* page, mi_tld_t* tld);

// The pages of a thread that are up for the floor in one sweep: decided together once all of them are known, so that
// what is there of the floor goes to the ones that were used last and not to the ones that the sweep came to first.
// (The abandoned pages of its heaps come after that, one at a time as they are claimed, for what is left.)
// (`mi_holes_floor_list_t` is in `internal.h`: it lives on the stack of `theap.c:mi_purge_holes_of`)
static void mi_page_holes_floor_stays(mi_page_t* page) {
  mi_page_purge_unformed_tail(page);   // (the blocks that were never formed are not what stays)
  // `swept_state` stays the epoch of the page's last allocation: that is all the age there is
}

// Is this a large page that may stay under the floor: one that was allocated from in the last
// `purge_holes_large_floor_epochs` epochs? Then either it is noted for the end of the pass (true), or decided now:
// true if it stays.
static bool mi_page_holes_floor_keep(mi_page_t* page, mi_tld_t* tld) {
  if (mi_page_block_size(page) <= MI_MEDIUM_MAX_OBJ_SIZE || page->reserved <= 1 || !mi_page_holes_madvisable(page)) return false;
  if (mi_option_get_size(mi_option_purge_holes_large_floor) == 0) return false;
  uint32_t stamp;
  if (!mi_page_sweep_state_is_alloc(page, &stamp)) return false;   // swept before, and not allocated from since
  const uint32_t age = mi_page_sweep_state_alloc_age(stamp);
  if ((long)age >= mi_option_get_clamp(mi_option_purge_holes_large_floor_epochs, 0, INT32_MAX)) return false;   // not used for that long: it goes
  const size_t bytes = mi_page_large_resident_free(page);   // (what is discarded already is not on the free list)
  if (bytes == 0) return false;
  mi_holes_floor_list_t* const list = tld->holes_floor_list;
  if (list != NULL && !mi_page_is_abandoned(page) && list->count < MI_HOLES_FLOOR_LIST_MAX) {
    mi_holes_floor_item_t* const item = &list->items[list->count++];
    item->page = page; item->age = age; item->bytes = bytes;
    return true;
  }
  if (!mi_holes_floor_take(tld, bytes)) return false;
  mi_page_holes_floor_stays(page);
  return true;
}

// A large page all of whose blocks are free (the sweep found it so after it collected what other threads freed) is
// what its free blocks are: the buffers of a thread that takes them again at its next request, and where most of the
// page faults of such a thread came from. It stays by the same rule as the free blocks of a page with a block in use
// (`_mi_page_purge_holes`). Else the caller frees it, as it did every such page before.
// Also asked when the last block of a page is freed (`MI_HOLES_ASKED_AT_FREE`) and by a collect that is not forced
// (`MI_HOLES_ASKED_BY_COLLECT`): those leave a page that was allocated from since it was last swept to the next sweep,
// for a thread that is swept at all. Between two sweeps a thread leaves `purge_holes_large_floor` bytes that way at the
// most (`holes_free_held`): one that frees and does not park for a long time is not to sit on more than that.
bool _mi_page_purge_holes_free_page_stays(mi_page_t* page, mi_tld_t* tld, int asked) {
  if (mi_page_block_size(page) <= MI_MEDIUM_MAX_OBJ_SIZE || page->reserved <= 1) return false;
  if (!mi_option_is_enabled(mi_option_purge_holes) || mi_option_get(mi_option_purge_delay) < 0) return false;
  if (tld == NULL || !mi_page_holes_madvisable(page)) return false;
  const size_t floor = mi_option_get_size(mi_option_purge_holes_large_floor);
  if (floor == 0) return false;   // (such a page is freed where it is found then, as it always was)
  uint32_t stamp;
  if (asked != MI_HOLES_ASKED_IN_SWEEP) {
    if (tld->holes_sweep_seq == 0) return false;   // nothing sweeps this thread
    if (!mi_page_sweep_state_is_alloc(page, &stamp)) return false;
    if (asked == MI_HOLES_ASKED_AT_FREE) {
      const size_t bytes = (size_t)page->capacity * mi_page_block_size(page);   // (its blocks are on more than one list here; on the high side where some are discarded)
      if (bytes > floor || tld->holes_free_held > floor - bytes) return false;
      tld->holes_free_held += bytes;
    }
    return true;
  }
  if (mi_page_sweep_state_is_alloc(page, &stamp) && mi_page_sweep_state_alloc_age(stamp) < 2 && mi_option_get(mi_option_purge_holes_min_interval) > 0) {
    tld->holes_sweep_deferred = true;
    return true;
  }
  return mi_page_holes_floor_keep(page, tld);
}

// The end of the pass over the thread's own pages: the most recently used first, while they fit.
void _mi_page_purge_holes_floor_resolve(mi_tld_t* tld) {
  mi_holes_floor_list_t* const list = tld->holes_floor_list;
  if (list == NULL) return;
  tld->holes_floor_list = NULL;
  if (mi_atomic_load_relaxed(&tld->park_reclaim) != 0) return;   // the owner wants its pages back: they are as they were, for the next sweep
  for (size_t i = 1; i < list->count; i++) {   // (insertion sort: a few pages)
    const mi_holes_floor_item_t item = list->items[i];
    size_t j = i;
    while (j > 0 && list->items[j-1].age > item.age) { list->items[j] = list->items[j-1]; j--; }
    list->items[j] = item;
  }
  for (size_t i = 0; i < list->count; i++) {
    mi_holes_floor_item_t* const item = &list->items[i];
    if (mi_holes_floor_take(tld, item->bytes)) { mi_page_holes_floor_stays(item->page); }
    else if (mi_page_all_free(item->page)) { _mi_page_holes_count_page_freed(); _mi_page_free(item->page, mi_page_queue_of(item->page)); }   // (see `_mi_page_purge_holes_free_page_stays`)
    else { mi_page_purge_holes_now(item->page, tld); }
  }
}

// Discard the memory of the free blocks in a still-used page.
//
// The free blocks a sweep leaves behind are the ones it could NOT discard (their OS page still
// holds a live block), and they stay on `page->free`. Walking them again finds exactly the same
// thing, so a sweep that follows one with nothing in between must not walk them: on a thread that
// parks often that re-walk is the dominant cost, and it grows with uptime. `page->swept_state` --
// the `(capacity,used)` we left the page in -- is the O(1) guard, and it is sound in one
// direction:
//
//   an OS page that is discardable now but was not at the end of the last sweep must have gained
//   a free block since (that sweep discarded EVERY OS page all of whose blocks were free), and a
//   block can only become free by being freed (`used` down) or by being formed (`capacity` up).
//
// So an unchanged `(capacity,used)` means at most that the page CHURNED: as many frees as allocs,
// leaving `used` where it was but a different set of blocks free -- which can hide a discardable
// OS page from the check. That is a missed discard, never a correctness bug (the block bookkeeping
// is exact either way), but a steady-state server can sit at the same `used` at every park, so it
// would not heal on its own. `purge_holes_full_every` bounds it: every N'th sweep walks every page
// regardless, which caps the delay of a missed discard at N parks for 1/N of the old cost.
// (An exact "was anything freed in this page" bit is the alternative, and it costs a store in
// `mi_free` itself -- the hot path this whole feature stays off.)
void _mi_page_purge_holes(mi_page_t* page, mi_tld_t* tld) {
  mi_assert_internal(page != NULL && tld != NULL);
  mi_assert_internal(tld->holes_sweeping);
  if (!mi_option_is_enabled(mi_option_purge_holes)) return;
  if (mi_page_all_free(page)) return;                     // the page itself is about to be freed
  if (mi_option_get(mi_option_purge_delay) < 0) return;   // purging disabled
  // The free blocks of a large page are whole buffers (96 KiB and up). A thread parks, and is swept, far more often than
  // it is idle, and a busy one takes those buffers again at once: giving them back in between costs it a discard and a
  // page fault for every OS page of every buffer (a server with 256 KiB request bodies lost a tenth of its throughput
  // that way). So they stay while the page is in use (`used` does not tell, it is the same at every park of a busy
  // server): an allocation from the page leaves the sweep's epoch in `swept_state`, and the blocks stay while that is
  // the current epoch or the one before. An epoch lasts for `purge_holes_min_interval` at least and only a sweep ends
  // it (`_mi_page_purge_holes_epoch_advance`), so with the third epoch there to be seen, the whole of the second one lies between
  // the allocation and now: the blocks of a page that is two epochs old were left alone for the interval at least. A
  // busy thread puts the new epoch in its pages within the interval and keeps them for good; after the last allocation
  // the blocks go one to two intervals later if something sweeps that often. A sweep ends one epoch at the most, so
  // where one thread sweeps, the first sweep after an allocation finds the page young whenever it comes, and it takes
  // a second or a third one, an interval apart each, to take the blocks. The same for the blocks of such a page that
  // are not formed yet: it is about to form them. Whoever sweeps a thread that stays parked comes back for them (`_mi_theap_sweep_parked`);
  // `mi_on_thread_idle_pending` tells its caller.
  // (Not for memory that cannot be given back at all, which is counted below as it always was.)
  uint32_t allocated_in;
  if (mi_page_holes_madvisable(page) && mi_page_sweep_state_is_alloc(page, &allocated_in)) {
    if (mi_page_sweep_state_alloc_age(allocated_in) < 2 && mi_option_get(mi_option_purge_holes_min_interval) > 0) {
      // Say that something was left. Also for an abandoned page that this sweep passes: a large page is abandoned when it
      // is full, so that is where most of a thread's own buffers are (and those of others; hence a fixed number of
      // sweeps, not "until nothing is left", in `_mi_theap_sweep_parked`). With nothing to take now, the epoch stays for
      // what is freed until the next sweep.
      if (page->free != NULL || mi_page_unformed_tail_pending(page)) { tld->holes_sweep_deferred = true; }
      return;
    }
  }
  // The floor. The whole rule for a large page, its free blocks or all of it (`_mi_page_purge_holes_free_page_stays`):
  //   it is left alone until it has not been allocated from for `purge_holes_large_floor_epochs` epochs, for up to
  //   `purge_holes_large_floor` bytes process-wide (the pages that were used last first); past either, the two epochs
  //   above are all it gets.
  // A server with a request now and then would fault its buffers in again on every request otherwise. The only time
  // there is is the epoch, which a sweep moves and nothing else: no clock is kept in the page and nothing is woken for
  // this. A process in which no thread parks any more keeps those bytes until one does.
  if (mi_page_holes_floor_keep(page, tld)) return;
  mi_page_purge_holes_now(page, tld);
}

// The part of `_mi_page_purge_holes` that takes what can be taken.
static void mi_page_purge_holes_now(mi_page_t* page, mi_tld_t* tld) {
  mi_page_purge_unformed_tail(page);                      // the blocks that are not formed yet: resident, but never handed out
  if (!mi_page_can_purge_holes(page)) { _mi_page_holes_count_ineligible(page); return; }

  if (!tld->holes_sweep_full && page->swept_state == mi_page_sweep_state(page)) {
    tld->holes_sweep_skipped++;   // nothing was allocated or freed in this page since we swept it
    return;
  }
  // Record the state we LEAVE the page in, read back from the page: a nested `mi_malloc` (see
  // `_mi_page_purge_holes_in_progress`) may have taken a block out of it while we walked.
  //
  // Only if the walk got everything. A failed `_mi_os_discard` (ENOMEM under pressure) puts its
  // blocks straight back, and changes neither `capacity` nor `used` -- so recording here would
  // say "already swept" for a page that still has holes, and the skip check would then park them
  // until the next full sweep, or forever with `purge_holes_full_every=0`.
  if (mi_page_purge_holes_walk(page, tld)) {
    page->swept_state = mi_page_sweep_state(page);
  }
}

// Bring the first run of discarded OS pages back onto the free list. Only that run is
// touched, so the other holes in the page stay discarded. A whole run at a time (and not
// one OS page at a time) so that the `_mi_os_reuse` is one call and the following
// allocations from this page hit the fast path instead of a syscall per block.
bool _mi_page_unpurge_run(mi_page_t* page) {
  if (!mi_page_has_purged(page)) return false;
  size_t k0 = 0;
  while (k0 < MI_PAGE_PURGE_BITS && !mi_page_os_page_purged(page, k0)) { k0++; }
  mi_assert_internal(k0 < MI_PAGE_PURGE_BITS);
  if (k0 >= MI_PAGE_PURGE_BITS) return false;
  size_t k1 = k0;
  while (k1 + 1 < MI_PAGE_PURGE_BITS && mi_page_os_page_purged(page, k1 + 1)) { k1++; }
  mi_page_unpurge_range(page, k0, k1, true);
  return true;
}

// Undo every hole in the page (the page is going back to the arena, which may hand the
// memory out as committed without any further `reuse` call). The page is dead here (every
// block is free), so we do NOT rebuild its free list: writing `next` pointers into the
// holes would fault every discarded OS page right back in.
void _mi_page_unpurge_all(mi_page_t* page) {
  _mi_page_unpurge_unformed_upto(page, UINTPTR_MAX);   // the unformed tail goes back as well
  if (!mi_page_has_purged(page)) return;
  const size_t os_size = mi_page_purge_unit(page);
  const uintptr_t base = mi_page_purge_base_of(page, os_size);
  size_t k = 0;
  while (k < MI_PAGE_PURGE_BITS) {
    if (!mi_page_os_page_purged(page, k)) { k++; continue; }
    const size_t k0 = k;
    while (k < MI_PAGE_PURGE_BITS && mi_page_os_page_purged(page, k)) { k++; }
    const size_t dsize = (k - k0) * os_size;
    _mi_os_reuse(mi_page_subproc(page), (void*)(base + (k0 * os_size)), dsize);
    size_t first, last, first1, last1;
    if (mi_page_os_page_blocks(page, os_size, k0, &first, &last) &&
        mi_page_os_page_blocks(page, os_size, k - 1, &first1, &last1)) {
      mi_holes_count_reuse(dsize, (last1 - first) + 1, true);
    }
    else {
      mi_assert_internal(false);   // a discarded OS page is always entirely inside the block area
    }
  }
  _mi_page_purged_reset(page);
}


/* -----------------------------------------------------------
  Hole report

  After a sweep, the memory that hole punching did *not* get back is the free blocks that
  share an OS page with a live block: an OS page is discarded only when every block
  overlapping it is free, so a single live block pins the whole OS page. This accounts for
  that, per size class, and says how many live blocks each pinned OS page is holding.

  Read-only: it does not purge, un-purge, collect, or touch a free list. It walks the three
  free lists in place instead of collecting them.

  A block is exactly one of:
   - free-listed: on `free`, `local_free`, or `xthread_free`;
   - purged: free but held off every list because its memory is discarded. This is derived
     from the OS-page bitmap (`mi_page_block_index_is_purged`: a block is purged iff it
     overlaps a discarded OS page), which is how the rest of the code derives it too;
   - live: everything else. Note `page->used` counts the not-yet-collected `xthread_free`
     blocks as used, so live is *not* `page->used` -- we take it as the complement of the
     other two (the conservation invariant is asserted in `mi_page_is_valid_init`).

  Bytes are attributed per OS page, by overlap: every byte of every block lies in exactly
  one OS page, so nothing is double counted even for a block straddling a boundary.
----------------------------------------------------------- */

#define MI_HOLES_MAX_CAP  (1 << 16)   // `page->capacity` is a uint16_t

static void mi_holes_mark_free_list(const mi_page_t* page, mi_block_t* b, uint64_t* set) {
  const size_t cap = page->capacity;
  for (size_t n = 0; b != NULL && n <= cap; n++) {   // `n` bounds a corrupt or cyclic list
    const size_t idx = mi_page_block_index(page, b);
    if (idx >= cap) break;
    set[idx / 64] |= ((uint64_t)1 << (idx % 64));
    b = mi_block_next((mi_page_t*)page, b);
  }
}

static size_t mi_holes_hist_bucket(size_t nlive) {
  if (nlive <= 1) return 0;
  if (nlive == 2) return 1;
  if (nlive <= 4) return 2;
  if (nlive <= 8) return 3;
  return 4;
}

// the hypothetical OS page sizes of the granularity curve
size_t mi_holes_granularity(size_t g) {
  static const size_t grans[MI_HOLES_GRAN_COUNT] = { 4*MI_KiB, 8*MI_KiB, 16*MI_KiB, 32*MI_KiB, 64*MI_KiB };
  return (g < MI_HOLES_GRAN_COUNT ? grans[g] : 0);
}

// is the block at `idx` free? Either on a free list, or purged (free, but held off every list
// because its memory is already discarded). Anything else is live.
static bool mi_holes_block_is_free(const mi_page_t* page, const uint64_t* freelisted, size_t idx) {
  if (((freelisted[idx / 64] >> (idx % 64)) & 1) != 0) return true;
  return mi_page_block_index_is_purged(page, idx);
}

// How many bytes of this page would be discardable if the OS page size were `G`? A G-aligned,
// G-sized span can be discarded exactly when it lies wholly inside the block area and every
// block overlapping it is free -- the same rule the real sweep applies at the unit of the page (`mi_page_purge_unit`).
static void mi_page_holes_granularity_curve(const mi_page_t* page, const uint64_t* freelisted, mi_holes_report_t* rep) {
  const size_t bs = page->block_size;
  const size_t cap = page->capacity;
  const uintptr_t pstart = (uintptr_t)mi_page_start(page);
  const uintptr_t pend = pstart + (cap * bs);
  for (size_t g = 0; g < MI_HOLES_GRAN_COUNT; g++) {
    const size_t gran = mi_holes_granularity(g);
    for (uintptr_t lo = _mi_align_down(pstart, gran); lo + gran <= pend; lo += gran) {
      if (lo < pstart) continue;   // not entirely inside the block area
      const size_t first = (size_t)(lo - pstart) / bs;
      const size_t last = (size_t)((lo + gran - 1) - pstart) / bs;
      bool all_free = true;
      for (size_t idx = first; idx <= last && idx < cap; idx++) {
        if (!mi_holes_block_is_free(page, freelisted, idx)) { all_free = false; break; }
      }
      if (all_free) { rep->discardable_at[g] += gran; }
    }
  }
}

void _mi_page_holes_report_page(const mi_page_t* page, mi_holes_report_t* rep) {
  if (page == NULL || rep == NULL) return;
  const size_t bs = page->block_size;
  const size_t cap = page->capacity;
  if (bs == 0 || cap > MI_HOLES_MAX_CAP) return;
  mi_holes_bin_t* const r = &rep->bin[_mi_bin(bs)];
  r->pages++;
  if (bs > r->block_size) { r->block_size = bs; }
  rep->total_pages++;
  rep->page_committed_bytes += mi_page_committed(page);
  if (page->reserved > cap) { rep->unformed_bytes += ((size_t)page->reserved - cap) * bs; }
  rep->unformed_discarded_bytes += _mi_page_unformed_purged_bytes(page);
  if (cap == 0) return;

  uint64_t freelisted[MI_HOLES_MAX_CAP / 64];
  const size_t nwords = _mi_divide_up(cap, 64);
  _mi_memzero(freelisted, nwords * sizeof(uint64_t));
  mi_holes_mark_free_list(page, page->free, freelisted);
  mi_holes_mark_free_list(page, page->local_free, freelisted);
  mi_holes_mark_free_list(page, mi_page_thread_free(page), freelisted);   // a concurrent free can push after this read: that block reads as live (a diagnostic, so this is fine)
  #define mi_holes_is_free(idx)  mi_holes_block_is_free(page, freelisted, idx)

  if (mi_page_holes_madvisable(page)) { mi_page_holes_granularity_curve(page, freelisted, rep); }
  else { rep->unmadvisable_pages++; }

  // An ineligible page has no discardable OS page at all (and carries no holes, see
  // `_mi_page_is_valid`), so every free block in it is undiscardable by definition.
  if (!mi_page_can_purge_holes(page)) {
    size_t nlive = 0;
    for (size_t idx = 0; idx < cap; idx++) {
      if (!mi_holes_is_free(idx)) { nlive++; }
    }
    r->live_bytes += nlive * bs;
    r->free_bytes += (cap - nlive) * bs;
    r->undiscardable_bytes += (cap - nlive) * bs;
    r->ineligible_pages++;
    rep->ineligible_pages++;
    return;
  }

  const size_t os_size = mi_page_purge_unit(page);   // the unit of the bitmap: the OS page in all but large pages
  const uintptr_t pstart = (uintptr_t)mi_page_start(page);
  const uintptr_t pend = pstart + (cap * bs);
  const uintptr_t base = mi_page_purge_base_of(page, os_size);
  const size_t nbits = mi_page_purge_bits_of(page, os_size);
  mi_assert_internal(nbits <= MI_PAGE_PURGE_BITS);

  for (size_t k = 0; k < nbits; k++) {
    const uintptr_t lo = base + (k * os_size);
    const uintptr_t hi = lo + os_size;
    const uintptr_t clo = (lo < pstart ? pstart : lo);
    const uintptr_t chi = (hi > pend ? pend : hi);
    if (clo >= chi) continue;                            // holds no block byte at all (the page header, or memory past `capacity`)
    const bool whole = (lo >= pstart && hi <= pend);     // entirely inside the block area -- only such an OS page can ever be discarded
    const size_t first = (size_t)(clo - pstart) / bs;
    const size_t last = (size_t)((chi - 1) - pstart) / bs;
    size_t live_ov = 0, free_ov = 0, nlive = 0;
    for (size_t idx = first; idx <= last && idx < cap; idx++) {
      const uintptr_t blo = pstart + (idx * bs);
      const uintptr_t bhi = blo + bs;
      const uintptr_t olo = (blo < lo ? lo : blo);
      const uintptr_t ohi = (bhi > hi ? hi : bhi);
      const size_t ov = (size_t)(ohi - olo);
      if (mi_holes_is_free(idx)) { free_ov += ov; }
      else { live_ov += ov; nlive++; }
    }
    r->live_bytes += live_ov;
    r->free_bytes += free_ov;
    if (mi_page_os_page_purged(page, k)) {
      mi_assert_internal(whole && live_ov == 0 && free_ov == os_size);
      r->discarded_bytes += os_size;
    }
    else if (!whole) {
      // Not entirely inside the block area (it holds the page header, or memory past `capacity`),
      // so it is never discardable whatever lives in it. Checked BEFORE liveness on purpose:
      // counting it as pinned would blame a live block for an OS page that freeing that block
      // cannot release anyway, and `pinned_ospages` / the histogram are the whole point here.
      r->undiscardable_bytes += free_ov;
      r->edge_bytes += free_ov;
    }
    else if (nlive > 0) {
      r->undiscardable_bytes += free_ov;                 // pinned: a live block in this OS page keeps it resident
      r->pinned_ospages++;
      r->pinned_live_blocks += nlive;
      r->pinned_free_bytes += free_ov;
      r->pinned_live_bytes += live_ov;
      r->hist[mi_holes_hist_bucket(nlive)]++;
    }
    else {
      r->pending_bytes += free_ov;                       // fully free and discardable, but not discarded (no sweep yet, or the discard failed)
    }
  }
  #undef mi_holes_is_free
}

// bytes as "MB.hh", since the mimalloc printf has no %f
static void mi_holes_mb(size_t bytes, char* buf, size_t bufsize) {
  const size_t mb = bytes / MI_MiB;
  const size_t hundredths = ((bytes % MI_MiB) * 100) / MI_MiB;
  _mi_snprintf(buf, bufsize, "%zu.%02zu", mb, hundredths);
}

static void mi_holes_print_row(const char* name, const mi_holes_bin_t* r) {
  char slive[32], sfree[32], sundisc[32], sdisc[32];
  mi_holes_mb(r->live_bytes, slive, sizeof(slive));
  mi_holes_mb(r->free_bytes, sfree, sizeof(sfree));
  mi_holes_mb(r->undiscardable_bytes, sundisc, sizeof(sundisc));
  mi_holes_mb(r->discarded_bytes, sdisc, sizeof(sdisc));
  // live blocks per pinned OS page, to two decimals
  const size_t avg100 = (r->pinned_ospages == 0 ? 0 : (r->pinned_live_blocks * 100) / r->pinned_ospages);
  _mi_fprintf(NULL, NULL, "%10s %8zu %10s %10s %18s %13s %10zu.%02zu\n",
              name, r->pages, slive, sfree, sundisc, sdisc, avg100 / 100, avg100 % 100);
}

void _mi_page_holes_report_print(const mi_holes_report_t* rep) {
  if (rep == NULL) return;
  static const char* hist_name[MI_HOLES_HIST_BUCKETS] = { "1", "2", "3-4", "5-8", "9+" };

  _mi_fprintf(NULL, NULL, "\nholes report: os page = %zu bytes, %zu pages (%zu ineligible, %zu never madvise-able)\n",
              _mi_os_page_size(), rep->total_pages, rep->ineligible_pages, rep->unmadvisable_pages);
  size_t large_unit = _mi_os_page_size();
  while ((large_unit * MI_PAGE_PURGE_BITS) < MI_LARGE_PAGE_SIZE && large_unit < MI_PAGE_PURGE_MAX_UNIT) { large_unit <<= 1; }
  if (large_unit > _mi_os_page_size()) {
    _mi_fprintf(NULL, NULL, "  (in a large page an \"OS page\" below is its unit of %zu bytes: the sweep works at that size there)\n", large_unit);
  }

  mi_holes_bin_t total;
  _mi_memzero(&total, sizeof(total));
  for (size_t bin = 0; bin < MI_BIN_COUNT; bin++) {
    const mi_holes_bin_t* const r = &rep->bin[bin];
    total.live_bytes += r->live_bytes;
    total.free_bytes += r->free_bytes;
    total.pinned_ospages += r->pinned_ospages;
    total.pinned_free_bytes += r->pinned_free_bytes;
    total.pinned_live_bytes += r->pinned_live_bytes;
  }

  // THE measurement: what a smaller OS page would buy us. `discardable@4K - discardable@16K` is
  // the memory the 16KB darwin page costs us, measured directly here -- no cross-platform
  // subtraction, no RSS arithmetic. Nothing is discarded to produce these numbers.
  char sgran[32];
  _mi_fprintf(NULL, NULL, "  discardable bytes vs hypothetical OS page size (nothing is discarded to measure this):\n");
  for (size_t g = 0; g < MI_HOLES_GRAN_COUNT; g++) {
    const size_t gran = mi_holes_granularity(g);
    mi_holes_mb(rep->discardable_at[g], sgran, sizeof(sgran));
    _mi_fprintf(NULL, NULL, "    @%6zu : %10s MB%s\n", gran, sgran,
                (gran == _mi_os_page_size() ? "   <-- this machine's OS page size" : ""));
  }
  char slive[32], sfree[32], spfree[32], splive[32];
  mi_holes_mb(total.live_bytes, slive, sizeof(slive));
  mi_holes_mb(total.free_bytes, sfree, sizeof(sfree));
  mi_holes_mb(total.pinned_free_bytes, spfree, sizeof(spfree));
  mi_holes_mb(total.pinned_live_bytes, splive, sizeof(splive));
  _mi_fprintf(NULL, NULL, "  live %s MB, free %s MB\n", slive, sfree);
  _mi_fprintf(NULL, NULL, "  %zu pinned OS pages (>= 1 live block): %s MB live + %s MB free trapped in them\n",
              total.pinned_ospages, splive, spfree);

  // Where the memory IS. If the curve is flat, the free memory is not sitting inside the pages --
  // and then it is sitting here. "in-page free" is memory the PAGES are holding (contamination);
  // "arena slack" is memory held in NO page at all (a purge/arena problem). Those want completely
  // different fixes, so the split has to be explicit -- and so does what each number can't tell us.
  {
    char spage[32], soverhead[32], sslack[32], spend[32], smeta[32], scommit[32], sresv[32], sother[32];
    const size_t page_overhead = (rep->page_committed_bytes > total.live_bytes + total.free_bytes
                                   ? rep->page_committed_bytes - total.live_bytes - total.free_bytes : 0);
    const size_t touched = rep->page_committed_bytes + rep->arena_free_dirty_bytes + rep->arena_meta_bytes;
    mi_holes_mb(rep->page_committed_bytes, spage, sizeof(spage));
    mi_holes_mb(page_overhead, soverhead, sizeof(soverhead));
    mi_holes_mb(rep->arena_free_dirty_bytes, sslack, sizeof(sslack));
    mi_holes_mb(rep->arena_purge_pending_bytes, spend, sizeof(spend));
    mi_holes_mb(rep->arena_meta_bytes, smeta, sizeof(smeta));
    mi_holes_mb(touched, sother, sizeof(sother));
    mi_holes_mb(rep->arena_committed_bytes, scommit, sizeof(scommit));
    mi_holes_mb(rep->arena_reserved_bytes, sresv, sizeof(sresv));
    _mi_fprintf(NULL, NULL, "  memory partition (walking the arena bitmaps):\n");
    _mi_fprintf(NULL, NULL, "    in pages           : %10s MB  = live %s + in-page free %s + page overhead %s (header/unformed)\n",
                spage, slive, sfree, soverhead);
    _mi_fprintf(NULL, NULL, "    arena slack        : %10s MB  (in NO page, touched at least once; %s MB of it is queued for purge and so certainly still resident)\n",
                sslack, spend);
    _mi_fprintf(NULL, NULL, "    arena meta (ROUGH) : %10s MB  (arena bitmaps only; excludes the mi_meta heaps)\n", smeta);
    _mi_fprintf(NULL, NULL, "    ---- ever-touched  : %10s MB  (in-pages + slack + meta; the ceiling on what we can be paying for)\n", sother);
    _mi_fprintf(NULL, NULL, "    reserved %s MB, slices_committed %s MB -- NOTE: on POSIX every slice is marked committed at reserve\n", sresv, scommit);
    _mi_fprintf(NULL, NULL, "      time and a reset-purge never clears it, so slices_committed is address space, NOT residency.\n");
    _mi_fprintf(NULL, NULL, "      'arena slack' is an UPPER bound: a slice purged earlier still reads as dirty here.\n");
    _mi_fprintf(NULL, NULL, "      'in pages' misses pages owned by OTHER threads' theaps -- this walk cannot read them.\n");
  }

  _mi_fprintf(NULL, NULL, "%10s %8s %10s %10s %18s %13s %13s\n",
              "size_class", "pages", "live_MB", "free_MB", "undiscardable_MB", "discarded_MB", "avg_live_blocks_per_pinned_ospage");
  _mi_memzero(&total, sizeof(total));
  char name[32];
  for (size_t bin = 0; bin < MI_BIN_COUNT; bin++) {
    const mi_holes_bin_t* const r = &rep->bin[bin];
    if (r->pages == 0) continue;
    _mi_snprintf(name, sizeof(name), "%zu", r->block_size);
    mi_holes_print_row(name, r);
    total.pages += r->pages;
    total.live_bytes += r->live_bytes;
    total.free_bytes += r->free_bytes;
    total.undiscardable_bytes += r->undiscardable_bytes;
    total.discarded_bytes += r->discarded_bytes;
    total.edge_bytes += r->edge_bytes;
    total.pending_bytes += r->pending_bytes;
    total.pinned_ospages += r->pinned_ospages;
    total.pinned_live_blocks += r->pinned_live_blocks;
  }
  mi_holes_print_row("TOTAL", &total);

  char edge[32], pending[32], unformed[32], unformed_disc[32];
  mi_holes_mb(total.edge_bytes, edge, sizeof(edge));
  mi_holes_mb(total.pending_bytes, pending, sizeof(pending));
  mi_holes_mb(rep->unformed_bytes, unformed, sizeof(unformed));
  mi_holes_mb(rep->unformed_discarded_bytes, unformed_disc, sizeof(unformed_disc));
  _mi_fprintf(NULL, NULL, "  of undiscardable: %s MB lies in a partial OS page (page header / past capacity)\n", edge);
  _mi_fprintf(NULL, NULL, "  free and discardable but not discarded: %s MB;  blocks not formed yet: %s MB (of which discarded: %s MB)\n", pending, unformed, unformed_disc);

  // the 3 worst size classes by undiscardable bytes: how many live blocks pin each pinned OS page?
  size_t taken[3] = { MI_BIN_COUNT, MI_BIN_COUNT, MI_BIN_COUNT };
  for (size_t n = 0; n < 3; n++) {
    size_t worst = MI_BIN_COUNT;
    for (size_t bin = 0; bin < MI_BIN_COUNT; bin++) {
      const mi_holes_bin_t* const r = &rep->bin[bin];
      if (r->pinned_ospages == 0 || r->undiscardable_bytes == 0) continue;
      bool already = false;
      for (size_t i = 0; i < n; i++) { if (taken[i] == bin) { already = true; break; } }
      if (already) continue;
      if (worst == MI_BIN_COUNT || r->undiscardable_bytes > rep->bin[worst].undiscardable_bytes) { worst = bin; }
    }
    if (worst == MI_BIN_COUNT) break;
    taken[n] = worst;
    const mi_holes_bin_t* const r = &rep->bin[worst];
    char worst_undisc[32];
    mi_holes_mb(r->undiscardable_bytes, worst_undisc, sizeof(worst_undisc));
    _mi_fprintf(NULL, NULL, "  block_size %zu: %s MB undiscardable over %zu pinned OS pages; live blocks per pinned OS page:",
                r->block_size, worst_undisc, r->pinned_ospages);
    for (size_t h = 0; h < MI_HOLES_HIST_BUCKETS; h++) {
      _mi_fprintf(NULL, NULL, "  %s: %zu", hist_name[h], r->hist[h]);
    }
    _mi_fprintf(NULL, NULL, "\n");
  }
  _mi_fprintf(NULL, NULL, "  (a live block straddling two pinned OS pages counts in both; abandoned pages are only reached when they are in the arena's abandoned map)\n");
}


static void mi_page_free_collect_ex(mi_page_t* page, bool force, bool allow_unpurge) {
  mi_assert_internal(page!=NULL);

  // collect the thread free list
  mi_page_thread_free_collect(page);

  // and the local free list
  if (page->local_free != NULL) {
    if mi_likely(page->free == NULL) {
      // usual case
      page->free = page->local_free;
      page->local_free = NULL;
      page->free_is_zero = false;
    }
    else if (force) {
      // append -- only on shutdown (force) as this is a linear operation
      mi_block_t* tail = page->local_free;
      mi_block_t* next;
      while ((next = mi_block_next(page, tail)) != NULL) {
        tail = next;
      }
      mi_block_set_next(page, tail, page->free);
      page->free = page->local_free;
      page->local_free = NULL;
      page->free_is_zero = false;
    }
    mi_page_update_sample_countdown(page);
  }  
  mi_assert_internal(!force || page->local_free == NULL);

  // Free list empty but this page has discarded holes: bring a whole run of them
  // back. Every caller re-checks `mi_page_immediate_available` after collect, so the
  // page becomes usable again without touching the other holes.
  if (allow_unpurge && page->free == NULL && mi_page_has_purged(page) && !_mi_page_purge_holes_in_progress()) {
    _mi_page_unpurge_run(page);
  }
}

void _mi_page_free_collect(mi_page_t* page, bool force) {
  mi_page_free_collect_ex(page, force, true);
}

// Heap inspection must not mutate the heap: leave the holes discarded (they are still
// reported as free, see `_mi_theap_area_visit_blocks`).
void _mi_page_free_collect_no_unpurge(mi_page_t* page, bool force) {
  mi_page_free_collect_ex(page, force, false);
}

// Collect elements in the thread-free list starting at `head`. This is an optimized
// version of `_mi_page_free_collect` to be used from `free.c:_mi_free_collect_mt` that avoids atomic access to `xthread_free`.
// returns a possibly updated expected value for the thread_free pointer.
//
// `head` must be in the `xthread_free` list. It will not collect `head` itself
// so the `used` count is not fully updated in general. However, if the `head` is
// the last remaining element, it will be collected and the used count will become `0` (so `mi_page_all_free` becomes true).
mi_block_t* _mi_page_free_collect_partly(mi_page_t* page, mi_block_t* head) {
  if (head == NULL) return NULL;
  mi_block_t* next = mi_block_next(page,head);  // we cannot collect the head element itself as `page->thread_free` may point to it (and we want to avoid atomic ops)
  if (next != NULL) {
    mi_block_set_next(page, head, NULL);
    mi_page_thread_collect_to_local(page, next);
    if (page->local_free != NULL && page->free == NULL) {
      page->free = page->local_free;
      page->local_free = NULL;
      page->free_is_zero = false;
      mi_page_update_sample_countdown(page);
    }    
  }
  if (mi_page_used(page) == 1) {
    // all elements are free'd since we skipped the `head` element itself
    mi_assert_internal(mi_tf_block(mi_atomic_load_relaxed(&page->xthread_free)) == head);
    mi_assert_internal(mi_block_next(page,head) == NULL);
    _mi_page_free_collect(page, false);  // collect the final element
    return NULL;
  }
  else {
    return head;
  }
}


/* -----------------------------------------------------------
  Page fresh and retire
----------------------------------------------------------- */

// called from `mi_free` on a reclaim, and fresh_alloc if we get an abandoned page
void _mi_theap_page_reclaim(mi_theap_t* theap, mi_page_t* page)
{
  mi_assert_internal(_mi_is_aligned(mi_page_slice_start(page), MI_PAGE_ALIGN));
  mi_assert_internal(_mi_ptr_page(mi_page_start(page))==page);
  mi_assert_internal(mi_page_is_owned(page));
  mi_assert_internal(mi_page_is_abandoned(page));

  mi_page_set_theap(page,theap);
  _mi_page_free_collect(page, false); // ensure used count is up to date
  
  mi_page_queue_t* pq = mi_theap_page_queue_of(theap, page);
  mi_page_queue_push_at_end(theap, pq, page);
  mi_assert_expensive(_mi_page_is_valid(page));
}

void _mi_page_abandon(mi_page_t* page, mi_page_queue_t* pq) {
  // no allocation to serve here either (see `mi_theap_page_collect`): un-purging would fault a run
  // of this page's holes back in on the way out, for a page nobody is about to allocate from.
  _mi_page_free_collect_no_unpurge(page, false); // ensure used count is up to date
  if (mi_page_all_free(page)) {
    _mi_page_free(page, pq);
  }
  else {
    mi_page_queue_remove(pq, page);
    mi_theap_t* theap = page->theap;
    mi_page_set_theap(page, NULL);
    page->theap = theap; // don't actually set theap to NULL so we can reclaim_on_free within the same theap
    _mi_arenas_page_abandon(page, theap);
    // _mi_arenas_collect(false, false, theap->tld); // allow purging
  }
}


// allocate a fresh page from an arena
static mi_page_t* mi_page_fresh_alloc(mi_theap_t* theap, mi_page_queue_t* pq, size_t block_size, size_t page_alignment) {
  #if !MI_HUGE_PAGE_ABANDON
  mi_assert_internal(pq != NULL);
  mi_assert_internal(mi_theap_contains_queue(theap, pq));
  mi_assert_internal(page_alignment > 0 || block_size > MI_LARGE_MAX_OBJ_SIZE || block_size == pq->block_size);
  #endif
  mi_page_t* page = _mi_arenas_page_alloc(theap, block_size, page_alignment);
  if (page == NULL) {
    // out-of-memory
    return NULL;
  }
  if (mi_page_is_abandoned(page)) {
    _mi_theap_page_reclaim(theap, page);
    if (!mi_page_immediate_available(page)) {
      if (mi_page_is_expandable(page)) {
        if (!mi_page_extend_free(theap, page)) {
          // cannot commit
          _mi_page_abandon(page,pq);
          return NULL;
        };
      }
      else {
        mi_assert(false); // should not happen?
        return NULL;
      }
    }
  }
  else if (pq != NULL) {
    mi_page_queue_push(theap, pq, page);
  }
  mi_assert_internal(pq!=NULL || mi_page_block_size(page) >= block_size);
  mi_assert_expensive(_mi_page_is_valid(page));
  return page;
}

// Get a fresh page to use
static mi_page_t* mi_page_fresh(mi_theap_t* theap, mi_page_queue_t* pq) {
  mi_assert_internal(mi_theap_contains_queue(theap, pq));
  mi_page_t* page = mi_page_fresh_alloc(theap, pq, pq->block_size, 0);
  if (page==NULL) return NULL;
  mi_assert_internal(pq->block_size==mi_page_block_size(page));
  mi_assert_internal(pq==mi_theap_page_queue_of(theap, page));
  return page;
}


/* -----------------------------------------------------------
  Unfull, abandon, free and retire
----------------------------------------------------------- */

// Move a page from the full list back to a regular list (called from thread-local mi_free)
void _mi_page_unfull(mi_page_t* page) {
  mi_assert_internal(page != NULL);
  mi_assert_expensive(_mi_page_is_valid(page));
  mi_assert_internal(mi_page_is_in_full(page));
  mi_assert_internal(!mi_page_theap(page)->allow_page_abandon);
  if (!mi_page_is_in_full(page)) return;

  mi_theap_t* theap = mi_page_theap(page);
  mi_page_queue_t* pqfull = &theap->pages[MI_BIN_FULL];
  mi_page_set_in_full(page, false); // to get the right queue
  mi_page_queue_t* pq = mi_theap_page_queue_of(theap, page);
  mi_page_set_in_full(page, true);
  mi_page_queue_enqueue_from_full(pq, pqfull, page);
}

static void mi_page_to_full(mi_page_t* page, mi_page_queue_t* pq) {
  mi_assert_internal(pq == mi_page_queue_of(page));
  mi_assert_internal(!mi_page_immediate_available(page));
  mi_assert_internal(!mi_page_is_in_full(page));

  mi_theap_t* theap = mi_page_theap(page);
  if (theap->allow_page_abandon) {
    // abandon full pages (this is the usual case in order to allow for sharing of memory between theaps)
    _mi_page_abandon(page, pq);
  }
  else if (!mi_page_is_in_full(page)) {
    // put full pages in a theap local queue (this is for theaps that cannot abandon, for example, if the theap can be destroyed)
    mi_page_queue_enqueue_from(&mi_page_theap(page)->pages[MI_BIN_FULL], pq, page);
    _mi_page_free_collect(page, false);  // try to collect right away in case another thread freed just before MI_USE_DELAYED_FREE was set
    _mi_page_update_stats(page);         // we must update stats here as mi_theap_collect does not normally visit full pages 
  }
}


// Free a page with no more free blocks
void _mi_page_free(mi_page_t* page, mi_page_queue_t* pq) {
  mi_assert_internal(page != NULL);
  mi_assert_expensive(_mi_page_is_valid(page));
  mi_assert_internal(pq == mi_page_queue_of(page));
  mi_assert_internal(mi_page_all_free(page));
  // mi_assert_internal(mi_page_thread_free_flag(page)!=MI_DELAYED_FREEING);

  // no more aligned blocks in here
  mi_page_set_has_interior_pointers(page, false);

  // note: the hole bookkeeping is undone in `_mi_arenas_page_free` (the single
  // choke point for a page going back to the arena -- `free.c` frees abandoned
  // pages without coming through here).

  // remove from the page list
  // (no need to do _mi_theap_delayed_free first as all blocks are already free)
  mi_page_queue_remove(pq, page);

  // and free it
  mi_theap_t* theap = mi_page_theap(page); mi_assert_internal(theap!=NULL);
  mi_page_set_theap(page,NULL);
  _mi_arenas_page_free(page, theap);
  // _mi_arenas_collect(false, false, theap->tld);  // allow purging
}

#define MI_RETIRE_CYCLES      (16)      /* keep a retired page around for about 16 "admin cycles" before free'ing it */
#define MI_RETIRE_MAX_PAGES   (3)       /* keep at most N pages per size bin as retired */

// Retire a page with no more used blocks
// Important to not retire too quickly though as new
// allocations might coming.
//
// Note: called from `mi_free` and benchmarks often
// trigger this due to freeing everything and then
// allocating again so careful when changing this.
void _mi_page_retire(mi_page_t* page) mi_attr_noexcept {
  mi_assert_internal(page != NULL);
  mi_assert_expensive(_mi_page_is_valid(page));
  mi_assert_internal(mi_page_all_free(page));

  if (page->retire_expire!=0) return;  // already retired, just keep it retired
  mi_page_set_has_interior_pointers(page, false);

  // don't retire too often..
  // (or we end up retiring and re-allocating most of the time)
  // NOTE: refine this more: we should not retire if this
  // is the only page left with free blocks. It is not clear
  // how to check this efficiently though...
  // for now, we don't retire if it is the only page left of this size class.
  mi_page_queue_t* pq = mi_page_queue_of(page);
  // a large page that was just allocated from is for the idle sweep to free (`_mi_page_purge_holes_free_page_stays`)
  if (mi_page_block_size(page) > MI_MEDIUM_MAX_OBJ_SIZE && !mi_page_queue_is_special(pq)) {
    mi_theap_t* const rtheap = mi_page_theap(page);
    if (rtheap != NULL && _mi_page_purge_holes_free_page_stays(page, rtheap->tld, MI_HOLES_ASKED_AT_FREE)) return;
  }
  #if MI_RETIRE_CYCLES > 0
  const size_t bsize = mi_page_block_size(page);
  if mi_likely( pq->count <= MI_RETIRE_MAX_PAGES && !mi_page_queue_is_special(pq)) {  // not full or huge queue?
    if (pq->count==1 || bsize < MI_SMALL_SIZE_MAX) {
      mi_theap_t* theap = mi_page_theap(page);
      mi_theap_stat_counter_increase(theap, pages_retire, 1);
      page->retire_expire = (bsize <= MI_SMALL_MAX_OBJ_SIZE ? MI_RETIRE_CYCLES : MI_RETIRE_CYCLES/4);
      mi_assert_internal(pq >= theap->pages);
      const size_t index = pq - theap->pages;
      mi_assert_internal(index < MI_BIN_FULL && index < MI_BIN_HUGE);
      if (index < theap->page_retired_min) theap->page_retired_min = index;
      if (index > theap->page_retired_max) theap->page_retired_max = index;
      mi_assert_internal(mi_page_all_free(page));
      return; // don't free after all
    }  
  }
  #endif
  _mi_page_free(page, pq);
}


static void mi_theap_collect_full_pages(mi_theap_t* theap) {
  // note: normally full pages get immediately abandoned and the full queue is always empty
  // this path is only used if abandoning is disabled due to a destroy-able theap or options
  // set by the user.
  mi_page_queue_t* pq = &theap->pages[MI_BIN_FULL];
  for (mi_page_t* page = pq->first; page != NULL; ) {
    mi_page_t* next = page->next;         // get next in case we free the page
    _mi_page_free_collect(page, false);   // register concurrent free's
    // no longer full?
    if (!mi_page_is_full(page)) {
      if (mi_page_all_free(page)) {
        _mi_page_free(page, pq);
      }
      else {
        _mi_page_unfull(page);
      }
    }
    page = next;
  }
}

static void mi_page_try_retire(mi_page_queue_t* pq, mi_page_t* page, size_t bin, bool force, size_t* min, size_t* max) {    
  mi_assert_internal(page!=NULL && page->retire_expire!=0);
  if (mi_page_all_free(page)) {
    page->retire_expire--;
    if (page->retire_expire == 0 || force) {
      _mi_page_free(page, pq);
    }
    else {
      // keep retired, update min/max
      if (bin < *min) *min = bin;
      if (bin > *max) *max = bin;
    }
  }
  else {
    page->retire_expire = 0;
  }  
}

// free retired pages: we don't need to look at the entire queues
// since we only retire pages that are at the head position in a queue.
void _mi_theap_collect_retired(mi_theap_t* theap, bool force) {
  size_t min = MI_BIN_FULL;
  size_t max = 0;
  for(size_t bin = theap->page_retired_min; bin <= theap->page_retired_max; bin++) {
    mi_page_queue_t* pq  = &theap->pages[bin];
    mi_page_t* page      = pq->first;
    for (int i = 0; i<MI_RETIRE_MAX_PAGES && page!=NULL && page->retire_expire!=0; i++) {
      mi_page_t* next = page->next;
      mi_page_try_retire(pq,page,bin,force,&min,&max);
      page = next;
    }
  }
  theap->page_retired_min = min;
  theap->page_retired_max = max;
  if (!theap->allow_page_abandon) {
    mi_theap_collect_full_pages(theap);
  }
}




/* -----------------------------------------------------------
  Initialize the initial free list in a page.
  In secure mode we initialize a randomized list by
  alternating between slices.
----------------------------------------------------------- */

#define MI_MAX_SLICE_SHIFT  (6)   // at most 64 slices
#define MI_MAX_SLICES       (1UL << MI_MAX_SLICE_SHIFT)
#define MI_MIN_SLICES       (2)

static void mi_page_free_list_extend_secure(mi_theap_t* const theap, mi_page_t* const page, const size_t bsize, const size_t extend) {
  #if (MI_SECURE < 2)
  mi_assert_internal(page->free == NULL);
  mi_assert_internal(page->local_free == NULL);
  #endif
  mi_assert_internal(page->capacity + extend <= page->reserved);
  mi_assert_internal(bsize == mi_page_block_size(page));
  void* const page_area = mi_page_start(page);

  // initialize a randomized free list
  // set up `slice_count` slices to alternate between
  size_t shift = MI_MAX_SLICE_SHIFT;
  while ((extend >> shift) == 0) {
    shift--;
  }
  const size_t slice_count = (size_t)1U << shift;
  const size_t slice_extend = extend / slice_count;
  mi_assert_internal(slice_extend >= 1);
  mi_block_t* blocks[MI_MAX_SLICES];   // current start of the slice
  size_t      counts[MI_MAX_SLICES];   // available objects in the slice
  for (size_t i = 0; i < slice_count; i++) {
    blocks[i] = mi_page_block_at(page, page_area, bsize, page->capacity + i*slice_extend);
    counts[i] = slice_extend;
  }
  counts[slice_count-1] += (extend % slice_count);  // final slice holds the modulus too (todo: distribute evenly?)

  // and initialize the free list by randomly threading through them
  // set up first element
  const size_t r = _mi_theap_random_next(theap);
  size_t current = r % slice_count;
  counts[current]--;
  mi_block_t* const free_start = blocks[current];
  // and iterate through the rest; use `random_shuffle` for performance
  size_t rnd = _mi_random_shuffle(r|1); // ensure not 0
  for (size_t i = 1; i < extend; i++) {
    // call random_shuffle only every SIZE_SIZE rounds
    const size_t round = i%MI_SIZE_SIZE;
    if (round == 0) rnd = _mi_random_shuffle(rnd);
    // select a random next slice index
    size_t next = ((rnd >> 8*round) & (slice_count-1));
    while (counts[next]==0) {                            // ensure it still has space
      next++;
      if (next==slice_count) next = 0;
    }
    // and link the current block to it
    counts[next]--;
    mi_block_t* const block = blocks[current];
    blocks[current] = (mi_block_t*)((uint8_t*)block + bsize);  // bump to the following block
    mi_block_set_next(page, block, blocks[next]);   // and set next; note: we may have `current == next`
    current = next;
  }
  // prepend to the free list (usually NULL)
  mi_block_set_next(page, blocks[current], page->free);  // end of the list
  page->free = free_start;
}

static mi_decl_noinline void mi_page_free_list_extend( mi_page_t* const page, const size_t bsize, const size_t extend)
{
  #if (MI_SECURE < 2)
  mi_assert_internal(page->free == NULL);
  mi_assert_internal(page->local_free == NULL);
  #endif
  mi_assert_internal(page->capacity + extend <= page->reserved);
  mi_assert_internal(bsize == mi_page_block_size(page));
  void* const page_area = mi_page_start(page);

  mi_block_t* const start = mi_page_block_at(page, page_area, bsize, page->capacity);

  // initialize a sequential free list
  mi_block_t* const last = mi_page_block_at(page, page_area, bsize, page->capacity + extend - 1);
  mi_block_t* block = start;
  while(block <= last) {
    mi_block_t* next = (mi_block_t*)((uint8_t*)block + bsize);
    mi_block_set_next(page,block,next);
    block = next;
  }
  // prepend to free list (usually `NULL`)
  mi_block_set_next(page, last, page->free);
  page->free = start;
}

/* -----------------------------------------------------------
  Page initialize and extend the capacity
----------------------------------------------------------- */

#define MI_MAX_EXTEND_SIZE    (8*1024)      // heuristic, one or two OS pages seems to work well.
#if (MI_SECURE>=2)
#define MI_MIN_EXTEND         (8*MI_SECURE) // extend at least by this many
#else
#define MI_MIN_EXTEND         (1)
#endif

// Extend the capacity (up to reserved) by initializing a free list
// We do at most `MI_MAX_EXTEND` to avoid touching too much memory
// Note: we also experimented with "bump" allocation on the first
// allocations but this did not speed up any benchmark (due to an
// extra test in malloc? or cache effects?)
static bool mi_page_extend_free(mi_theap_t* theap, mi_page_t* page) {
  mi_assert_expensive(mi_page_is_valid_init(page));
  #if (MI_SECURE < 2)
  mi_assert(page->free == NULL);
  mi_assert(page->local_free == NULL);
  if (page->free != NULL) return true;
  #endif
  if (page->capacity >= page->reserved) return true;

  size_t page_size;
  //uint8_t* page_start =
  mi_page_area(page, &page_size);
  mi_theap_stat_counter_increase(theap, pages_extended, 1);
  mi_page_update_sample_countdown(page);  // the blocks of the previous extension are handed out: count them
  
  // calculate the extend count
  const size_t bsize = mi_page_block_size(page);
  size_t extend = (size_t)page->reserved - page->capacity;
  mi_assert_internal(extend > 0);

  size_t max_extend = (bsize >= MI_MAX_EXTEND_SIZE ? MI_MIN_EXTEND : MI_MAX_EXTEND_SIZE/bsize);
  if (max_extend < MI_MIN_EXTEND) { max_extend = MI_MIN_EXTEND; }
  mi_assert_internal(max_extend > 0);

  if (extend > max_extend) {
    // ensure we don't touch memory beyond the page to reduce page commit.
    // the `lean` benchmark tests this. Going from 1 to 8 increases rss by 50%.
    extend = max_extend;
  }

  mi_assert_internal(extend > 0 && extend + page->capacity <= page->reserved);
  mi_assert_internal(extend < (1UL<<16));

  // commit on demand?
  const size_t slice_committed = mi_page_slice_committed(page);
  if (slice_committed > 0) {
    // reduce extend if it commits more than an arena slice
    if ((extend * bsize) > MI_ARENA_SLICE_SIZE) {
      extend = _mi_divide_up(MI_ARENA_SLICE_SIZE, bsize);
    }
    // commit required size
    const size_t needed_size = (page->capacity + extend)*bsize;
    mi_assert_internal(needed_size <= page_size);
    size_t needed_commit = _mi_align_up( mi_page_slice_offset_of(page, needed_size), mi_page_min_commit_size());
    #if MI_SECURE>=5
    // the previous alignup could extend the commit into the guard page; re-adjust if needed
    const size_t page_size_commit = _mi_align_up( mi_page_slice_offset_of(page, page_size), _mi_os_page_size() );    
    if (needed_commit > page_size_commit) { 
      needed_commit = page_size_commit;
    }
    #endif
    if (needed_commit > slice_committed) {
      mi_assert_internal(((needed_commit - slice_committed) % _mi_os_page_size()) == 0);
      if (!_mi_os_commit(_mi_theap_subproc(theap), mi_page_slice_start(page) + slice_committed, needed_commit - slice_committed, NULL)) {
        return false;
      }
      mi_assert_internal(needed_commit <= UINT16_MAX * _mi_os_page_size());
      page->slice_pcommitted = (uint16_t)(needed_commit / _mi_os_page_size());
    }
  }

  // The blocks we are about to format may sit in the discarded unformed tail: hand that memory
  // back to the OS *before* the first free-list pointer is written into it (on macOS a discarded
  // page stays reclaimable by the kernel, and stays charged to the process, until it is REUSE'd).
  _mi_page_unpurge_unformed_upto(page, (uintptr_t)mi_page_start(page) + ((size_t)page->capacity + extend) * bsize);

  // and append the extend the free list
  if (extend < MI_MIN_SLICES || MI_SECURE<2) { //!mi_option_is_enabled(mi_option_secure)) {
    mi_page_free_list_extend(page, bsize, extend );
  }
  else {
    mi_page_free_list_extend_secure(theap, page, bsize, extend);
  }
  // enable the new free list
  page->capacity += (uint16_t)extend;
  mi_theap_stat_increase(theap, page_committed, extend * bsize);
  mi_assert_expensive(mi_page_is_valid_init(page));
  return true;
}

// Initialize a fresh page (that is already partially initialized)
mi_decl_nodiscard bool _mi_page_init(mi_theap_t* theap, mi_page_t* page) {
  mi_assert(page != NULL);
  mi_assert(theap!=NULL);
  // page->heap = (_mi_is_heap_main(_mi_theap_heap(theap)) ? NULL : _mi_theap_heap(theap)); // faster for `mi_page_associated_theap`
  // mi_page_set_theap(page, theap);

  _mi_page_purged_reset(page);   // fresh page: no holes

  size_t page_size;
  uint8_t* page_start = mi_page_area(page, &page_size); MI_UNUSED(page_start);
  mi_track_mem_noaccess(page_start,page_size);
  mi_assert_internal(page_size / mi_page_block_size(page) < (1L<<16));
  mi_assert_internal(page->reserved > 0);
  #if (MI_PADDING || MI_ENCODE_FREELIST)
  page->keys[0] = _mi_theap_random_next(theap);
  #if MI_PAGE_KEY_COUNT==2
  page->keys[1] = _mi_theap_random_next(theap);
  #endif
  #endif
  #if MI_DEBUG>2
  if (page->memid.initially_zero) {
    mi_track_mem_defined(mi_page_start(page), mi_page_committed(page));
    mi_assert_expensive(mi_mem_is_zero(page_start, mi_page_committed(page)));
  }
  #endif

  mi_assert_internal(page->heap != NULL);
  mi_assert_internal(page->heap == _mi_theap_heap(theap));
  mi_assert_internal(page->theap!=NULL);
  mi_assert_internal(page->theap == mi_page_theap(page));
  mi_assert_internal(page->capacity == 0);
  mi_assert_internal(page->free == NULL);
  mi_assert_internal(mi_page_used(page) == 0);
  mi_assert_internal(page->xused.used_alloc == 0);
  mi_assert_internal(mi_page_is_owned(page));
  mi_assert_internal(page->xthread_free == 1);
  mi_assert_internal(page->next == NULL);
  mi_assert_internal(page->prev == NULL);
  mi_assert_internal(page->retire_expire == 0);
  mi_assert_internal(!mi_page_has_interior_pointers(page));
  #if (MI_PADDING || MI_ENCODE_FREELIST)
  mi_assert_internal(page->keys[0] != 0);
  #if MI_PAGE_KEY_COUNT==2
  mi_assert_internal(page->keys[1] != 0);
  #endif
  #endif
  mi_assert_expensive(mi_page_is_valid_init(page));

  // initialize an initial free list
  if (!mi_page_extend_free(theap,page)) return false;
  mi_assert(mi_page_immediate_available(page));
  return true;
}


/* -----------------------------------------------------------
  Find pages with free blocks
-------------------------------------------------------------*/

// Find a page with free blocks of `page->block_size`.
static mi_decl_noinline mi_page_t* mi_page_queue_find_free_ex(mi_theap_t* theap, mi_page_queue_t* pq, bool first_try)
{
  // search through the pages in "next fit" order
  size_t count = 0;
  long candidate_limit = 0;          // we reset this on the first candidate to limit the search
  long page_full_retain = (pq->block_size > MI_SMALL_MAX_OBJ_SIZE ? 0 : theap->page_full_retain); // only retain small pages
  mi_page_t* page_candidate = NULL;  // a page with free space
  mi_page_t* page = pq->first;
  mi_page_t* const last = pq->last;

  while (page!=NULL)
  {
    mi_page_t* next = page->next; // remember next (as this page can move to another queue)
    count++;
    candidate_limit--;

    // search up to N pages for a best candidate

    // is the local free list non-empty?
    bool immediate_available = mi_page_immediate_available(page);
    if (!immediate_available) {
      // collect freed blocks by us and other threads to we get a proper use count
      _mi_page_free_collect(page, false);
      immediate_available = mi_page_immediate_available(page);
    }

    // if the page is completely full, move it to the `mi_pages_full`
    // queue so we don't visit long-lived pages too often.
    if (!immediate_available && !mi_page_is_expandable(page)) {
      page_full_retain--;
      if (page_full_retain < 0) {
        mi_assert_internal(!mi_page_is_in_full(page) && !mi_page_immediate_available(page));
        mi_page_to_full(page, pq);
      }
      else if (page!=last && pq->last!=page) {
        // avoid revisiting this page for a while
        mi_page_queue_move_to_back(theap, pq, page);
      }
    }
    else {
      // the page has free space, make it a candidate
      // we prefer non-expandable pages with high usage as candidates (to reduce commit, and increase chances of free-ing up pages)
      if (page_candidate == NULL) {
        page_candidate = page;
        candidate_limit = _mi_option_get_fast(mi_option_page_max_candidates);
      }
      else if (mi_page_all_free(page_candidate)) {
        _mi_page_free(page_candidate, pq);
        page_candidate = page;
      }
      // prefer to reuse fuller pages (in the hope the less used page gets freed)
      else if (mi_page_used(page) >= mi_page_used(page_candidate) && !mi_page_is_mostly_used(page)) { // && !mi_page_is_expandable(page)) {
        page_candidate = page;
      }
      // if we find a non-expandable candidate, or searched for N pages, return with the best candidate
      if (immediate_available || candidate_limit <= 0) {
        mi_assert_internal(page_candidate!=NULL);
        break;
      }
    }

  #if 0
    // first-fit algorithm without candidates
    // If the page contains free blocks, we are done
    if (mi_page_immediate_available(page) || mi_page_is_expandable(page)) {
      break;  // pick this one
    }

    // If the page is completely full, move it to the `mi_pages_full`
    // queue so we don't visit long-lived pages too often.
    mi_assert_internal(!mi_page_is_in_full(page) && !mi_page_immediate_available(page));
    mi_page_to_full(page, pq);
  #endif
    if (page==last) { page=NULL; }  // don't revisit earlier pages that moved to the back
              else  { page = next; }
  } // for each page

  mi_theap_stat_counter_increase(theap, page_searches, count);
  mi_theap_stat_counter_increase(theap, page_searches_count, 1);

  // set the page to the best candidate
  if (page_candidate != NULL) {
    page = page_candidate;
  }
  if (page != NULL) {
    if (!mi_page_immediate_available(page)) {
      mi_assert_internal(mi_page_is_expandable(page));
      if (!mi_page_extend_free(theap, page)) {
        page = NULL; // failed to extend
      }
    }
    mi_assert_internal(page == NULL || mi_page_immediate_available(page));
  }

  if (page == NULL) {
    _mi_theap_collect_retired(theap, false); // perhaps make a page available
    page = mi_page_fresh(theap, pq);         
    mi_assert_internal(page == NULL || mi_page_immediate_available(page));
    if (page == NULL && first_try) {
      // out-of-memory _or_ an abandoned page with free blocks was reclaimed, try once again
      page = mi_page_queue_find_free_ex(theap, pq, false);
      mi_assert_internal(page == NULL || mi_page_immediate_available(page));
    }
  }
  else {
    mi_assert_internal(page == NULL || mi_page_immediate_available(page));
    // move the page to the front of the queue
    mi_page_queue_move_to_front(theap, pq, page);
    page->retire_expire = 0;
    // _mi_theap_collect_retired(theap, false); // update retire counts; note: increases rss on MemoryLoad bench so don't do this
  }
  mi_assert_internal(page == NULL || mi_page_immediate_available(page));


  return page;
}

// Look for a page with free blocks of `size` but don't try to search or allocate
static inline mi_page_t* mi_page_queue_lookup_free_first(mi_theap_t* theap, mi_page_queue_t* pq) {
  mi_assert_internal(!mi_page_queue_is_huge(pq));
  mi_page_t* page = pq->first;
  if mi_likely(page!=NULL && mi_page_free_quick_collect(page)) {
    // fast path
    #if (MI_SECURE>=2) // in secure mode, we extend half the time to increase randomness
    if (page->capacity < page->reserved && ((_mi_theap_random_next(theap) & 1) == 1)) {
      (void)mi_page_extend_free(theap, page);  // ok if this fails
      mi_assert_internal(mi_page_immediate_available(page));
    }
    #else
    MI_UNUSED(theap);
    #endif
    page->retire_expire = 0;
    mi_assert_internal(mi_page_immediate_available(page));
    mi_assert_internal(_mi_is_aligned(mi_page_slice_start(page), MI_PAGE_ALIGN));
    mi_assert_internal(_mi_ptr_page(mi_page_start(page))==page);
    return page;
  }
  else {
    return NULL;
  }
}

// Find a page with free blocks of `size`.
static inline mi_page_t* mi_page_queue_find_free(mi_theap_t* theap, mi_page_queue_t* pq) {
  // mi_page_queue_t* pq = mi_page_queue(theap, size);
  mi_assert_internal(!mi_page_queue_is_huge(pq));

  // check the first page: we even do this with candidate search or otherwise we re-search every time
  mi_page_t* page = mi_page_queue_lookup_free_first(theap,pq);
  if (page==NULL) {
    page = mi_page_queue_find_free_ex(theap, pq, true);
    if (page==NULL) return NULL;
  }  
  mi_assert_internal(mi_page_immediate_available(page));
  mi_assert_internal(_mi_is_aligned(mi_page_slice_start(page), MI_PAGE_ALIGN));
  mi_assert_internal(_mi_ptr_page(mi_page_start(page))==page);
  return page;
}

// Huge pages contain just one block, and the segment contains just that page.
// Huge pages are also use if the requested alignment is very large (> MI_BLOCK_ALIGNMENT_MAX)
// so their size is not always `> MI_LARGE_OBJ_SIZE_MAX`.
static mi_page_t* mi_huge_page_alloc(mi_theap_t* theap, size_t size, size_t page_alignment, mi_page_queue_t* pq) {
  const size_t block_size = _mi_os_good_alloc_size(size);
  // mi_assert_internal(mi_bin(block_size) == MI_BIN_HUGE || page_alignment > 0);
  #if MI_HUGE_PAGE_ABANDON
  #error todo.
  #else
  // mi_page_queue_t* pq = mi_page_queue(theap, MI_LARGE_MAX_OBJ_SIZE+1);  // always in the huge queue regardless of the block size
  mi_assert_internal(mi_page_queue_is_huge(pq));
  #endif
  mi_page_t* page = mi_page_fresh_alloc(theap, pq, block_size, page_alignment);
  if (page != NULL) {
    mi_assert_internal(mi_page_block_size(page) >= size);
    mi_assert_internal(mi_page_immediate_available(page));
    mi_assert_internal(mi_page_is_huge(page));
    mi_assert_internal(mi_page_is_singleton(page));
    #if MI_HUGE_PAGE_ABANDON
    mi_assert_internal(mi_page_is_abandoned(page));
    mi_page_set_theap(page, NULL);
    #endif
    // mi_theap_stat_increase(theap, malloc_huge, mi_page_block_size(page));
    // mi_theap_stat_counter_increase(theap, malloc_huge_count, 1);
  }
  return page;
}

// Allocate a page
// Note: in debug mode the size includes MI_PADDING_SIZE and might have overflowed.
static mi_page_t* mi_find_page(mi_theap_t* theap, size_t size, size_t huge_alignment) mi_attr_noexcept {
  const size_t req_size = size - MI_PADDING_SIZE;  // correct for padding_size in case of an overflow on `size`
  if mi_unlikely(req_size > MI_MAX_ALLOC_SIZE) {   // TODO: remove as we check in generic_fallback ?
    _mi_error_message(EOVERFLOW, "allocation request is too large (%zu bytes)\n", req_size);
    return NULL;
  }
  mi_page_queue_t* pq = mi_page_queue(theap, (huge_alignment > 0 ? MI_LARGE_MAX_OBJ_SIZE+1 : size));
  mi_page_t* page;
  // huge allocation?
  if mi_unlikely(mi_page_queue_is_huge(pq) || req_size > MI_MAX_ALLOC_SIZE) {
    page = mi_huge_page_alloc(theap,size,huge_alignment,pq);
  }
  else {
    // otherwise find a page with free blocks in our size segregated queues
    #if MI_PADDING
    mi_assert_internal(size >= MI_PADDING_SIZE);
    #endif
    page = mi_page_queue_find_free(theap,pq);
  }
  if (page==NULL) return NULL;
  mi_assert_internal(mi_page_block_size(page) >= size);
  mi_assert_internal(mi_page_immediate_available(page));
  mi_assert_internal(_mi_is_aligned(mi_page_slice_start(page), MI_PAGE_ALIGN));
  mi_assert_internal(_mi_ptr_page(mi_page_start(page))==page);
  return page;
}



/* -----------------------------------------------------------
  Users can register a deferred free function called
  when the `free` list is empty. Since the `local_free`
  is separate this is deterministically called after
  a certain number of allocations.
----------------------------------------------------------- */

// The program should only install a single deferred free handler before doing allocation.
static _Atomic(void*) deferred_free; // is `mi_deferred_free_fun*` (but some platforms don't support atomic function pointers)
static _Atomic(void*) deferred_arg;   

void _mi_deferred_free(mi_theap_t* theap, bool force) {
  // owner only: `heartbeat` and `recurse` are plain fields, and the handler is thread-affine
  if (theap->tld != NULL && theap->tld->thread_id != _mi_thread_id()) return;
  theap->heartbeat++;
  mi_deferred_free_fun* const fun = (mi_deferred_free_fun*)mi_atomic_load_ptr_acquire(void,&deferred_free);
  if (fun != NULL && !theap->tld->recurse) {
    theap->tld->recurse = true;
    void* const arg = mi_atomic_load_ptr_acquire(void,&deferred_arg);  
    fun(force, theap->heartbeat, arg);
    theap->tld->recurse = false;
  }
}

void mi_register_deferred_free(mi_deferred_free_fun* fn, void* arg) mi_attr_noexcept {
  mi_atomic_store_ptr_release(void,&deferred_arg, arg);
  mi_atomic_store_ptr_release(void,&deferred_free, (void*)fn);  
}


/* -----------------------------------------------------------
  Admin
----------------------------------------------------------- */
static mi_theap_t* mi_theap_init(mi_theap_t* theap) {
  if mi_likely(mi_theap_is_initialized(theap)) return theap;
  if (theap==&_mi_theap_empty_wrong) {
    // we were unable to allocate a theap for a first-class heap
    return NULL;
  }
  // otherwise we initialize the thread and its default theap
  theap = _mi_thread_init();
  if mi_unlikely(!mi_theap_is_initialized(theap)) { return NULL; }
  mi_assert_internal(mi_theap_is_initialized(theap));
  return theap;
}

// Start or stop profiling for a theap if its profiler was started or stopped.
void _mi_theap_update_profiling(mi_theap_t* theap) {
  if (theap->is_detached) return;  // (meta data is not sampled, see `theap.c:_mi_theap_init`)
  #if MI_SAMPLE
  // we look now: a request that comes in from here on is for a next look (0: no fast path in the meantime)
  mi_atomic_exchange_acq_rel(&theap->generic_fast_limit, (intptr_t)0);
  #endif
  mi_heap_t* const heap = _mi_theap_heap(theap);
  mi_profiler_t* prof = mi_atomic_load_ptr_acquire(mi_profiler_t, &heap->profiler);
  const bool prof_enabled = (prof!=NULL && mi_profiler_is_enabled(prof));
  if (prof_enabled != (theap->profile_sample_rate!=0)) {
    if (theap->sample_rate==0 && (theap->profile_sample_rate!=0 || theap->guarded_sample_rate!=0)) {
      // not inside `_mi_malloc_generic_no_sample`, which has set the sample rate aside and puts it back: at the next
      // generic allocation. (If every one is a sample, `mi_malloc_generic_admin` can get here inside of it every time.)
      #if MI_SAMPLE
      mi_atomic_store_release(&theap->generic_fast_limit, (intptr_t)(-1));
      #endif
      return;
    }
    if (prof_enabled) {
      _mi_theap_set_profile_sample_rate(theap,mi_max(1,prof->initial_sample_rate)); // start profiling
      // with a full period, as in `theap.c:_mi_theap_init` (with guarded sampling on, the first sample point would be a
      // profile sample, with what was requested since the theap was made)
      theap->profile_sample_countdown = theap->profile_sample_rate;
      theap->sample_countdown = theap->sample_rate;
      theap->sample_requested = 0;
    }
    else {
      _mi_theap_set_profile_sample_rate(theap,0); // stop profiling
    }
  }
  #if MI_SAMPLE
  // and back to the limit, unless a new request has come in
  intptr_t expected = 0;
  mi_atomic_cas_strong_acq_rel(&theap->generic_fast_limit, &expected, (intptr_t)MI_GENERIC_FAST_LIMIT);
  #endif
}

static mi_theap_t* mi_malloc_generic_admin(mi_theap_t* theap) 
{
  theap = mi_theap_init(theap);
  if (theap==NULL) return NULL;
  mi_assert_internal(mi_theap_is_initialized(theap));

  // do administrative tasks every N generic mallocs
  if mi_unlikely(theap->generic_count >= MI_GENERIC_FAST_LIMIT) {
    theap->generic_collect_count += theap->generic_count;
    theap->generic_count = 0;

    // check if the profiler is enabled
    _mi_theap_update_profiling(theap);

    // do a full theap collect every once in a while (10000 by default)
    const long generic_collect = mi_option_get_clamp(mi_option_generic_collect, 1, 1000000L);
    if (theap->generic_collect_count >= generic_collect) {
      theap->generic_collect_count = 0;
      mi_theap_collect(theap, false /* force? */);
    }
    // else if (theap->sample_rate != 0) {             // update stats more aggressively if we are sampling
    //   mi_theap_collect(theap, false /* force? */);  // TODO: make specialized mi_theap_collect_update_stats ?
    // }
    else {
      // otherwise we do a mini-collect
      _mi_deferred_free(theap, false);         // call potential deferred free routines      
      _mi_theap_collect_retired(theap, false); // free retired pages            
    }
  }
  return theap;
}

/* -----------------------------------------------------------
  Generic allocation
----------------------------------------------------------- */

static mi_decl_noinline void* mi_malloc_generic_fallback(mi_theap_t* theap, size_t size, size_t zero_huge_alignment, mi_page_t** ppage)
{  
  const bool zero = ((zero_huge_alignment & MI_MALLOC_GENERIC_ZERO) != 0);
  const size_t huge_alignment = (zero_huge_alignment & ~(MI_MALLOC_GENERIC_ZERO | MI_MALLOC_GENERIC_BLOCK_START));

  #if MI_SAMPLE
  // `mi_profiler_start` asks every theap to look at its profiler at its next generic allocation (and not only when
  // `mi_malloc_generic_admin` does, which can be many megabytes of small blocks later)
  if mi_unlikely(mi_theap_is_initialized(theap) && (intptr_t)mi_atomic_load_relaxed(&theap->generic_fast_limit) < 0) {
    _mi_theap_update_profiling(theap);
  }
  #endif

  // initialize if necessary
  theap = mi_malloc_generic_admin(theap);
  if (theap==NULL) return NULL;
  
  // check allocation size
  const size_t req_size = size - MI_PADDING_SIZE;
  if mi_unlikely(req_size > MI_MAX_ALLOC_SIZE - MI_PADDING_SIZE) {
    _mi_error_message(EOVERFLOW, "allocation request is too large (%zu bytes)\n", req_size);
    return NULL;
  }

  // take a sample?
  bool sample_countdown_is_adjusted = false;
  if mi_unlikely(mi_theap_should_sample(theap,req_size)) {
    if (huge_alignment==0 && theap->sample_rate!=0) {
      if ((zero_huge_alignment & MI_MALLOC_GENERIC_BLOCK_START)!=0) return NULL;
      return _mi_theap_malloc_sampled(theap,req_size,zero,ppage);    
    }    
    mi_assert_internal(!_mi_is_empty_theap(theap));       // cannot write to the empty theap
    mi_assert_internal(req_size <= MI_SAMPLE_COUNTDOWN_MAX);
    if (theap->sample_rate==0) {
      theap->sample_countdown = MI_SAMPLE_COUNTDOWN_MAX;  // reset the countdown counter    
    }
    #if MI_SAMPLE==2
    else {
      // huge_alignment!=0 so we need to adjust the countdown 
      mi_assert_internal(huge_alignment!=0);
      mi_assert_internal(theap->sample_countdown <= SIZE_MAX - req_size);
      sample_countdown_is_adjusted = true;
      theap->sample_countdown += req_size;                // adjust such that after `_mi_page_malloc_zero` the countdown is correct again
    }
    #endif    
  }

  // find (or allocate) a page of the right size
  mi_page_t* page = mi_find_page(theap, size, huge_alignment);
  if mi_unlikely(page == NULL) { // first time out of memory, try to collect and retry the allocation once more
    mi_theap_collect(theap, true /* force? */);
    page = mi_find_page(theap, size, huge_alignment);
  }

  if mi_unlikely(page == NULL) { // out of memory
    if (sample_countdown_is_adjusted) { theap->sample_countdown -= req_size; }  // unadjust again if we failed to find a page
    _mi_error_message(ENOMEM, "unable to allocate memory (%zu bytes)\n", req_size);
    return NULL;  
  }

  mi_assert_internal(mi_page_immediate_available(page));
  mi_assert_internal(mi_page_block_size(page) >= size);
  mi_assert_internal(_mi_is_aligned(mi_page_slice_start(page), MI_PAGE_ALIGN));
  mi_assert_internal(_mi_ptr_page(mi_page_start(page))==page);

  // and try again, this time succeeding! (i.e. this should never recurse through _mi_page_malloc_zero)
  if (ppage!=NULL) { *ppage = page; }
  void* const p = _mi_page_malloc_zero(theap,page,size,zero);
  mi_assert_internal(p != NULL);
  #if MI_SAMPLE==1
  // (also for a theap that does not sample, which counts nothing but moves `last_alloc` up: we may be in
  //  `_mi_malloc_generic_no_sample` for the block of a sample, which `_mi_theap_malloc_sampled` has counted)
  mi_theap_count_page_allocs(theap,page);
  #else
  mi_page_update_sample_countdown(page);
  #endif
  
  // move full pages to the full queue
  // this will also call _mi_page_update_stats for huge pages  
  if (mi_page_block_size(page) > MI_SMALL_MAX_OBJ_SIZE) {
    if (mi_page_block_size(page) > MI_MEDIUM_MAX_OBJ_SIZE && page->reserved > 1) {   // (not a huge page, which is one block: the sweep never looks at it)
      mi_page_sweep_state_set_alloc(page);   // a large page is in use: the idle sweep leaves its free blocks for now (see `_mi_page_purge_holes`). A load, and a store once per epoch.
    }
    if (mi_page_is_full(page)) {
      mi_page_to_full(page, mi_page_queue_of(page));
    }
  }  
  return p;
}


// Generic allocation routine if the fast path (`alloc.c:mi_page_malloc`) does not succeed.
// Note: in debug mode the size includes MI_PADDING_SIZE and might have overflowed.
// The `huge_alignment` is normally 0 but is set to a multiple of MI_SLICE_SIZE for
// very large requested alignments in which case we use a huge singleton page.
// Note: we put `bool zero, size_t huge_alignment` into one parameter (with zero in the low bit)
// to use 4 parameters which compiles better on msvc for the malloc fast path.
#if MI_SAMPLE==1
// Called by a theap that samples before it refills the free list of `page` (the first one of its size class, or NULL)
// for an allocation of `req_size`: is that allocation to be a sample? (`mi_malloc_generic_fallback` takes it then)
// The blocks that the fast path took from the page are counted against the sample countdown now. If that uses up the
// countdown, the allocation in progress (of the size class of those blocks) is the sample. If we leave it to the next
// allocation that comes through the generic path, that is one above `MI_SMALL_SIZE_MAX` (these always do) far more
// often than a small one: with one such allocation for every 40 blocks of 64 bytes, less than 2% of the sampled
// bytes went to the blocks of 64 bytes (`test-profile.c:test_profiler_small_attribution`).
// (if `mi_page_queue_find_free` comes up with another page than the first, that one is counted when it is the first)
static mi_decl_noinline bool mi_theap_refill_is_sample(mi_theap_t* theap, mi_page_t* page, size_t req_size) {
  if (mi_theap_should_sample(theap,req_size)) return true;
  if (page==NULL) return false;
  mi_theap_count_page_allocs(theap,page);
  return mi_theap_should_sample(theap,req_size);
}
#endif

void* _mi_malloc_generic(mi_theap_t* theap, size_t size, size_t zero_huge_alignment, mi_page_t** ppage) mi_attr_noexcept
{
  #if !MI_THEAP_INITASNULL
  mi_assert_internal(theap != NULL);
  #endif
  // (the flags and the alignment are taken from `zero_huge_alignment` where they are used, so only that stays live)
  #define mi_generic_zero()            ((zero_huge_alignment & MI_MALLOC_GENERIC_ZERO) != 0)
  #define mi_generic_huge_alignment()  (zero_huge_alignment & ~(MI_MALLOC_GENERIC_ZERO | MI_MALLOC_GENERIC_BLOCK_START))
  // (the limit comes from the theap so that `mi_profiler_start` can send it to `mi_malloc_generic_fallback`)
  #if MI_SAMPLE
  #define mi_generic_fast_limit()      ((intptr_t)mi_atomic_load_relaxed(&theap->generic_fast_limit))
  #else
  #define mi_generic_fast_limit()      MI_GENERIC_FAST_LIMIT
  #endif
  mi_page_t* page = NULL;

  // fast path objects that fit in a small page
  if mi_likely(mi_theap_is_initialized(theap) && ++theap->generic_count < mi_generic_fast_limit() && mi_generic_huge_alignment()==0) {
    const size_t req_size = size - MI_PADDING_SIZE;  // correct for padding_size in case of an overflow on `size`         
    if (req_size < MI_SMALL_MAX_OBJ_SIZE)
    { 
      #if MI_SAMPLE==2
      if mi_likely(!mi_theap_should_sample(theap,req_size)) // ensure we don't need to take a sample
      #endif
      {
        mi_page_queue_t* pq = mi_page_queue(theap, size);
        mi_assert_internal(pq!=NULL && !mi_page_queue_is_huge(pq));
        #if MI_SAMPLE==1
        // With coarse sampling this test is all there is in the generic path for a theap that does not sample.
        if mi_unlikely(theap->sample_rate!=0 && mi_theap_refill_is_sample(theap,pq->first,req_size)) goto fallback;
        #endif
        page = mi_page_queue_find_free(theap,pq);
        // mi_assert_internal(mi_page_block_size(page) <= MI_SMALL_MAX_OBJ_SIZE);
        if (page!=NULL) {        
          if (ppage!=NULL) { *ppage = page; }
          mi_assert_internal(mi_page_immediate_available(page)); // we should never recurse in _mi_page_malloc_zero
          return _mi_page_malloc_zero(theap,page,size,mi_generic_zero());
        }
      }
    }
  }
  // otherwise fallback
  #if MI_SAMPLE==1
  fallback:
  #endif
  return mi_malloc_generic_fallback(theap,size,zero_huge_alignment,ppage);
  #undef mi_generic_zero
  #undef mi_generic_huge_alignment
  #undef mi_generic_fast_limit
}

void* _mi_malloc_generic_no_sample(mi_theap_t* theap, size_t size, bool zero, mi_page_t** ppage) mi_attr_noexcept {
  theap = mi_theap_init(theap);
  if (theap==NULL) return NULL;
  const size_t sample_rate = theap->sample_rate;
  if (sample_rate==0) {
    // nothing to set aside (with `MI_SAMPLE==2` the first allocation of a thread comes here through `_mi_theap_empty`,
    // and this call may well be where the new theap starts to sample: `_mi_theap_update_profiling`)
    return _mi_malloc_generic(theap, size, (zero ? MI_MALLOC_GENERIC_ZERO : 0), ppage);
  }
  const size_t sample_countdown = theap->sample_countdown;
  theap->sample_rate = 0;  // prevent a recursive call to mi_theap_malloc_sampled from _mi_malloc_generic
  theap->sample_countdown = MI_SAMPLE_COUNTDOWN_MAX;
  void* p = _mi_malloc_generic(theap, size, (zero ? MI_MALLOC_GENERIC_ZERO : 0), ppage);
  theap->sample_rate = sample_rate;
  theap->sample_countdown = sample_countdown;
  return p;
}
