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
// The fields up to and including `theap` are read by `mi_free` and `mi_page_alloc`;
// they must all sit in the first cache line (this is why `purged` is at the end).
typedef char mi_page_hot_fields_first_cacheline[(offsetof(mi_page_t,theap) + sizeof(mi_theap_t*) <= 64) ? 1 : -1];
#endif

void _mi_page_purged_reset(mi_page_t* page) {
  for (size_t i = 0; i < MI_PAGE_PURGE_WORDS; i++) { page->purged[i] = 0; }
}

static inline size_t mi_page_block_index(const mi_page_t* page, const mi_block_t* block) {
  mi_assert_internal((uint8_t*)block >= page->page_start);
  return ((size_t)((uint8_t*)block - page->page_start)) / page->block_size;
}

static inline mi_block_t* mi_page_block_index_at(const mi_page_t* page, size_t idx) {
  mi_assert_internal(idx < page->capacity);
  return (mi_block_t*)(page->page_start + (idx * page->block_size));
}

static inline void mi_page_purged_clear(mi_page_t* page, size_t k) {
  mi_assert_internal(k < MI_PAGE_PURGE_BITS);
  page->purged[k / 64] &= ~((uint64_t)1 << (k % 64));
}

// the OS pages that the block at index `idx` overlaps (relative to `mi_page_purge_base`)
static inline void mi_page_block_os_pages(const mi_page_t* page, size_t idx, size_t* kfirst, size_t* klast) {
  const size_t os_size = _mi_os_page_size();
  const uintptr_t base = mi_page_purge_base(page);
  const uintptr_t lo = (uintptr_t)page->page_start + (idx * page->block_size);
  *kfirst = (size_t)(lo - base) / os_size;
  *klast = (size_t)((lo + page->block_size - 1) - base) / os_size;
}

// the blocks that overlap OS page `k`, or `false` if that OS page is not entirely inside
// the *committed* block area (see `_mi_page_purge_os_page_blocks`)
static bool mi_page_os_page_blocks(const mi_page_t* page, size_t k, size_t* first, size_t* last) {
  return _mi_page_purge_os_page_blocks(_mi_os_page_size(), page->block_size, (uintptr_t)page->page_start,
                                       page->capacity, k, first, last);
}

// The number of blocks that are purged: the blocks overlapping a discarded OS page.
// (the blocks of a maximal run of discarded OS pages are contiguous)
size_t _mi_page_purged_count(const mi_page_t* page) {
  if (!mi_page_has_purged(page)) return 0;
  size_t total = 0;
  size_t k = 0;
  while (k < MI_PAGE_PURGE_BITS) {
    if (!mi_page_os_page_purged(page, k)) { k++; continue; }
    const size_t k0 = k;
    while (k < MI_PAGE_PURGE_BITS && mi_page_os_page_purged(page, k)) { k++; }
    size_t first, last, first1, last1;
    if (!mi_page_os_page_blocks(page, k0, &first, &last)) { mi_assert_internal(false); continue; }
    if (!mi_page_os_page_blocks(page, k - 1, &first1, &last1)) { mi_assert_internal(false); continue; }
    total += (last1 - first) + 1;
  }
  return total;
}


#if (MI_DEBUG>=3)
static size_t mi_page_list_count(mi_page_t* page, mi_block_t* head) {
  mi_assert_internal(_mi_ptr_page(page->page_start) == page);
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
  mi_assert_internal(page->used <= page->capacity);
  mi_assert_internal(page->capacity <= page->reserved);

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
  mi_assert_internal(page->used + free_count + _mi_page_purged_count(page) == page->capacity);

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
    {
      mi_page_queue_t* pq = mi_page_queue_of(page);
      mi_assert_internal(mi_page_queue_contains(pq, page));
      mi_assert_internal(pq->block_size==mi_page_block_size(page) || mi_page_is_huge(page) || mi_page_is_in_full(page));
      // mi_assert_internal(mi_theap_contains_queue(mi_page_theap(page),pq));
    }
  }
  return true;
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
    _mi_error_message(EFAULT, "corrupted thread-free list\n");
    return; // the thread-free items cannot be freed
  }
  // if `count > page->used` there was another kind memory corruption (either in the page meta-data or in the linked list)
  else if mi_unlikely(count > page->used) {
    _mi_error_message(EFAULT, "corrupted meta-data in thread-free list\n");
    return; // the thread-free items cannot be freed
  }

  // and append the current local free list
  mi_block_set_next(page, last, page->local_free);
  page->local_free = head;

  // update counts now
  mi_assert_internal(count <= UINT16_MAX);
  mi_assert_internal(page->used >= (uint16_t)count);
  page->used = page->used - (uint16_t)count;
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
static bool mi_page_free_quick_collect(mi_page_t* page) {
  if (page->free != NULL) return true;
  if (page->local_free == NULL) return false;
  // move local_free to free
  page->free = page->local_free;
  page->local_free = NULL;
  page->free_is_zero = false;
  return true;
}

/* -----------------------------------------------------------
  Page hole purging.

  A page returns to the arena only once *every* block is free, so one live
  block pins the whole page (up to 512KB). Here we discard the memory of the
  free blocks inside a still-used page.

  The unit of discarding is an OS page, so that is what we track: `page->purged`
  is a bitmap over the OS pages of this mimalloc page (bit `k` = the OS page at
  `mi_page_purge_base(page) + k*os_page_size`). Its size does not depend on the
  size class, so *every* size class is eligible -- a run of adjacent free blocks
  covering one whole OS page is enough, no matter how small the blocks are.

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
  size_t nfree = 0;
  for (mi_block_t* b = page->free; b != NULL; b = mi_block_next(page, b)) { nfree++; }
  for (mi_block_t* b = page->local_free; b != NULL; b = mi_block_next(page, b)) { nfree++; }
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

// Re-entrancy guard: while the idle sweep is rewriting a page's free list and
// bitmap, a nested `mi_malloc` (only reachable through a user output function
// from a warning message) must not un-purge a hole from under it.
static mi_decl_thread bool mi_purging_holes;

bool _mi_page_purge_holes_in_progress(void) { return mi_purging_holes; }
void _mi_page_purge_holes_begin(void) { mi_assert_internal(!mi_purging_holes); mi_purging_holes = true; }
void _mi_page_purge_holes_end(void)   { mi_assert_internal(mi_purging_holes);  mi_purging_holes = false; }

static inline bool mi_page_bits_at(const uint64_t* bits, size_t k) {
  mi_assert_internal(k < MI_PAGE_PURGE_BITS);
  return ((bits[k / 64] >> (k % 64)) & 1) != 0;
}

// does the block at index `idx` overlap any OS page in `bits`?
static bool mi_page_block_overlaps(const mi_page_t* page, size_t idx, const uint64_t* bits) {
  size_t kfirst, klast;
  mi_page_block_os_pages(page, idx, &kfirst, &klast);
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
  const size_t os_size = _mi_os_page_size();
  const uintptr_t dstart = mi_page_purge_base(page) + (k0 * os_size);
  const size_t dsize = ((k1 - k0) + 1) * os_size;
  if (discarded) { _mi_os_reuse((void*)dstart, dsize); }

  // clear the bits first: `mi_page_block_index_is_purged` then tells us exactly which
  // blocks are whole again
  for (size_t k = k0; k <= k1; k++) { mi_page_purged_clear(page, k); }

  size_t first, last, first1, last1;
  if (!mi_page_os_page_blocks(page, k0, &first, &last) ||
      !mi_page_os_page_blocks(page, k1, &first1, &last1)) {
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
  mi_holes_count_reuse(dsize, nblocks, discarded);
}

// Discard the memory of the free blocks in a still-used page.
void _mi_page_purge_holes(mi_page_t* page) {
  mi_assert_internal(page != NULL);
  if (!mi_option_is_enabled(mi_option_purge_holes)) return;
  if (mi_page_all_free(page)) return;                     // the page itself is about to be freed
  if (mi_option_get(mi_option_purge_delay) < 0) return;   // purging disabled
  if (!mi_page_can_purge_holes(page)) { _mi_page_holes_count_ineligible(page); return; }
  if (page->free == NULL) return;                         // nothing new to take off the free list

  const size_t os_size = _mi_os_page_size();
  const size_t nbits = mi_page_purge_bits(page);
  mi_assert_internal(nbits <= MI_PAGE_PURGE_BITS);
  if (nbits > MI_PAGE_PURGE_BITS) return;

  // 1. count, per OS page, the blocks on the free list that overlap it
  uint16_t nfree[MI_PAGE_PURGE_BITS];
  _mi_memzero(nfree, nbits * sizeof(uint16_t));
  for (mi_block_t* b = page->free; b != NULL; b = mi_block_next(page, b)) {
    const size_t idx = mi_page_block_index(page, b);
    mi_assert_internal(idx < page->capacity);
    mi_assert_internal(!mi_page_block_index_is_purged(page, idx));   // it is on the free list, so not purged
    size_t kfirst, klast;
    mi_page_block_os_pages(page, idx, &kfirst, &klast);
    for (size_t k = kfirst; k <= klast && k < nbits; k++) {
      mi_assert_internal(nfree[k] < UINT16_MAX);
      nfree[k]++;
    }
  }

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
    if (!mi_page_os_page_blocks(page, k, &first, &last)) continue; // not entirely inside the block area
    size_t nfreek = nfree[k];
    if (mi_page_block_index_is_purged(page, first)) { nfreek++; }
    if (last != first && mi_page_block_index_is_purged(page, last)) { nfreek++; }
    mi_assert_internal(nfreek <= (last - first) + 1);
    if (nfreek != (last - first) + 1) continue;                    // some block overlapping it is still live
    todo[k / 64] |= ((uint64_t)1 << (k % 64));
    ntodo++;
  }
  if (ntodo == 0) return;

  // 3. rebuild the free list without the blocks that are about to lose memory. This must
  //    happen *before* the discard: it walks `next` pointers that live in the very memory
  //    we are about to discard.
  mi_block_t* keep = NULL;
  size_t ndropped = 0;
  mi_block_t* b = page->free;
  while (b != NULL) {
    mi_block_t* const next = mi_block_next(page, b);
    if (mi_page_block_overlaps(page, mi_page_block_index(page, b), todo)) {
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
    if (_mi_os_discard((void*)(mi_page_purge_base(page) + (k0 * os_size)), dsize)) {
      mi_holes_count_discard(dsize);
    }
    else {
      // the discard failed: the memory is intact, so hand these blocks straight back
      mi_page_unpurge_range(page, k0, k - 1, false /* nothing was discarded, so no reuse */);
    }
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
  if (!mi_page_has_purged(page)) return;
  const size_t os_size = _mi_os_page_size();
  const uintptr_t base = mi_page_purge_base(page);
  size_t k = 0;
  while (k < MI_PAGE_PURGE_BITS) {
    if (!mi_page_os_page_purged(page, k)) { k++; continue; }
    const size_t k0 = k;
    while (k < MI_PAGE_PURGE_BITS && mi_page_os_page_purged(page, k)) { k++; }
    const size_t dsize = (k - k0) * os_size;
    _mi_os_reuse((void*)(base + (k0 * os_size)), dsize);
    size_t first, last, first1, last1;
    if (mi_page_os_page_blocks(page, k0, &first, &last) &&
        mi_page_os_page_blocks(page, k - 1, &first1, &last1)) {
      mi_holes_count_reuse(dsize, (last1 - first) + 1, true);
    }
    else {
      mi_assert_internal(false);   // a discarded OS page is always entirely inside the block area
    }
  }
  _mi_page_purged_reset(page);
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
//
// `head` must be in the `xthread_free` list. It will not collect `head` itself
// so the `used` count is not fully updated in general. However, if the `head` is
// the last remaining element, it will be collected and the used count will become `0` (so `mi_page_all_free` becomes true).
void _mi_page_free_collect_partly(mi_page_t* page, mi_block_t* head) {
  if (head == NULL) return;
  mi_block_t* next = mi_block_next(page,head);  // we cannot collect the head element itself as `page->thread_free` may point to it (and we want to avoid atomic ops)
  if (next != NULL) {
    mi_block_set_next(page, head, NULL);
    mi_page_thread_collect_to_local(page, next);
    if (page->local_free != NULL && page->free == NULL) {
      page->free = page->local_free;
      page->local_free = NULL;
      page->free_is_zero = false;
    }
  }
  if (page->used == 1) {
    // all elements are free'd since we skipped the `head` element itself
    mi_assert_internal(mi_tf_block(mi_atomic_load_relaxed(&page->xthread_free)) == head);
    mi_assert_internal(mi_block_next(page,head) == NULL);
    _mi_page_free_collect(page, false);  // collect the final element
  }
}


/* -----------------------------------------------------------
  Page fresh and retire
----------------------------------------------------------- */

/*
// called from segments when reclaiming abandoned pages
void _mi_page_reclaim(mi_theap_t* theap, mi_page_t* page) {
  // mi_page_set_theap(page, theap);
  // _mi_page_use_delayed_free(page, MI_USE_DELAYED_FREE, true); // override never (after theap is set)
  _mi_page_free_collect(page, false); // ensure used count is up to date

  mi_assert_expensive(mi_page_is_valid_init(page));
  // mi_assert_internal(mi_page_theap(page) == theap);
  // mi_assert_internal(mi_page_thread_free_flag(page) != MI_NEVER_DELAYED_FREE);

  // TODO: push on full queue immediately if it is full?
  mi_page_queue_t* pq = mi_theap_page_queue_of(theap, page);
  mi_page_queue_push(theap, pq, page);
  mi_assert_expensive(_mi_page_is_valid(page));
}
*/

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
  _mi_page_free_collect(page, false); // ensure used count is up to date
  if (mi_page_all_free(page)) {
    _mi_page_free(page, pq);
  }
  else {
    mi_page_queue_remove(pq, page);
    mi_theap_t* theap = page->theap;
    mi_page_set_theap(page, NULL);
    page->theap = theap; // don't actually set theap to NULL so we can reclaim_on_free within the same theap
    _mi_arenas_page_abandon(page, theap);
    _mi_arenas_collect(false, false, theap->tld); // allow purging
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
          return NULL; // cannot commit
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
  _mi_arenas_collect(false, false, theap->tld);  // allow purging
}

#define MI_MAX_RETIRE_SIZE    MI_LARGE_OBJ_SIZE_MAX   // should be less than size for MI_BIN_HUGE
#define MI_RETIRE_CYCLES      (16)

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
  #if MI_RETIRE_CYCLES > 0
  const size_t bsize = mi_page_block_size(page);
  if mi_likely( /* bsize < MI_MAX_RETIRE_SIZE && */ !mi_page_queue_is_special(pq)) {  // not full or huge queue?
    if (pq->last==page && pq->first==page) { // the only page in the queue?
      mi_theap_t* theap = mi_page_theap(page);
      #if MI_STAT>0
      mi_theap_stat_counter_increase(theap, pages_retire, 1);
      #endif
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

// free retired pages: we don't need to look at the entire queues
// since we only retire pages that are at the head position in a queue.
void _mi_theap_collect_retired(mi_theap_t* theap, bool force) {
  size_t min = MI_BIN_FULL;
  size_t max = 0;
  for(size_t bin = theap->page_retired_min; bin <= theap->page_retired_max; bin++) {
    mi_page_queue_t* pq   = &theap->pages[bin];
    mi_page_t*       page = pq->first;
    if (page != NULL && page->retire_expire != 0) {
      if (mi_page_all_free(page)) {
        page->retire_expire--;
        if (page->retire_expire == 0 || force) {
          _mi_page_free(page, pq);
        }
        else {
          // keep retired, update min/max
          if (bin < min) min = bin;
          if (bin > max) max = bin;
        }
      }
      else {
        page->retire_expire = 0;
      }
    }
  }
  theap->page_retired_min = min;
  theap->page_retired_max = max;
}

/*
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
*/


/* -----------------------------------------------------------
  Initialize the initial free list in a page.
  In secure mode we initialize a randomized list by
  alternating between slices.
----------------------------------------------------------- */

#define MI_MAX_SLICE_SHIFT  (6)   // at most 64 slices
#define MI_MAX_SLICES       (1UL << MI_MAX_SLICE_SHIFT)
#define MI_MIN_SLICES       (2)

static void mi_page_free_list_extend_secure(mi_theap_t* const theap, mi_page_t* const page, const size_t bsize, const size_t extend) {
  #if (MI_SECURE<3)
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
  const uintptr_t r = _mi_theap_random_next(theap);
  size_t current = r % slice_count;
  counts[current]--;
  mi_block_t* const free_start = blocks[current];
  // and iterate through the rest; use `random_shuffle` for performance
  uintptr_t rnd = _mi_random_shuffle(r|1); // ensure not 0
  for (size_t i = 1; i < extend; i++) {
    // call random_shuffle only every INTPTR_SIZE rounds
    const size_t round = i%MI_INTPTR_SIZE;
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
  #if (MI_SECURE<3)
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

#define MI_MAX_EXTEND_SIZE    (4*1024)      // heuristic, one OS page seems to work well.
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
  #if (MI_SECURE<3)
  mi_assert(page->free == NULL);
  mi_assert(page->local_free == NULL);
  if (page->free != NULL) return true;
  #endif
  if (page->capacity >= page->reserved) return true;

  size_t page_size;
  //uint8_t* page_start =
  mi_page_area(page, &page_size);
  #if MI_STAT>0
  mi_theap_stat_counter_increase(theap, pages_extended, 1);
  #endif

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
  if (page->slice_committed > 0) {
    const size_t needed_size = (page->capacity + extend)*bsize;
    const size_t needed_commit = _mi_align_up( mi_page_slice_offset_of(page, needed_size), MI_PAGE_MIN_COMMIT_SIZE );
    if (needed_commit > page->slice_committed) {
      mi_assert_internal(((needed_commit - page->slice_committed) % _mi_os_page_size()) == 0);
      if (!_mi_os_commit(mi_page_slice_start(page) + page->slice_committed, needed_commit - page->slice_committed, NULL)) {
        return false;
      }
      page->slice_committed = needed_commit;
    }
  }

  // and append the extend the free list
  if (extend < MI_MIN_SLICES || MI_SECURE<2) { //!mi_option_is_enabled(mi_option_secure)) {
    mi_page_free_list_extend(page, bsize, extend );
  }
  else {
    mi_page_free_list_extend_secure(theap, page, bsize, extend);
  }
  // enable the new free list
  page->capacity += (uint16_t)extend;
  #if MI_STAT>0
  mi_theap_stat_increase(theap, page_committed, extend * bsize);
  #endif
  mi_assert_expensive(mi_page_is_valid_init(page));
  return true;
}

// Initialize a fresh page (that is already partially initialized)
mi_decl_nodiscard bool _mi_page_init(mi_theap_t* theap, mi_page_t* page) {
  mi_assert(page != NULL);
  mi_assert(theap!=NULL);
  page->heap = (_mi_is_heap_main(_mi_theap_heap(theap)) ? NULL : _mi_theap_heap(theap)); // faster for `mi_page_associated_theap`
  mi_page_set_theap(page, theap);

  _mi_page_purged_reset(page);   // fresh page: no holes

  size_t page_size;
  uint8_t* page_start = mi_page_area(page, &page_size); MI_UNUSED(page_start);
  mi_track_mem_noaccess(page_start,page_size);
  mi_assert_internal(page_size / mi_page_block_size(page) < (1L<<16));
  mi_assert_internal(page->reserved > 0);
  #if (MI_PADDING || MI_ENCODE_FREELIST)
  page->keys[0] = _mi_theap_random_next(theap);
  page->keys[1] = _mi_theap_random_next(theap);
  #endif
  #if MI_DEBUG>2
  if (page->memid.initially_zero) {
    mi_track_mem_defined(page->page_start, mi_page_committed(page));
    mi_assert_expensive(mi_mem_is_zero(page_start, mi_page_committed(page)));
  }
  #endif

  mi_assert_internal(page->theap!=NULL);
  mi_assert_internal(page->theap == mi_page_theap(page));
  mi_assert_internal(page->capacity == 0);
  mi_assert_internal(page->free == NULL);
  mi_assert_internal(page->used == 0);
  mi_assert_internal(mi_page_is_owned(page));
  mi_assert_internal(page->xthread_free == 1);
  mi_assert_internal(page->next == NULL);
  mi_assert_internal(page->prev == NULL);
  mi_assert_internal(page->retire_expire == 0);
  mi_assert_internal(!mi_page_has_interior_pointers(page));
  #if (MI_PADDING || MI_ENCODE_FREELIST)
  mi_assert_internal(page->keys[0] != 0);
  mi_assert_internal(page->keys[1] != 0);
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

  while (page != NULL)
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
      else if (page->used >= page_candidate->used && !mi_page_is_mostly_used(page)) { // && !mi_page_is_expandable(page)) {
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

    page = next;
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



// Find a page with free blocks of `size`.
static mi_page_t* mi_find_free_page(mi_theap_t* theap, mi_page_queue_t* pq) {
  // mi_page_queue_t* pq = mi_page_queue(theap, size);
  mi_assert_internal(!mi_page_queue_is_huge(pq));

  // check the first page: we even do this with candidate search or otherwise we re-search every time
  mi_page_t* page = pq->first;
  if mi_likely(page != NULL && mi_page_free_quick_collect(page)) {
    #if (MI_SECURE>=2) // in secure mode, we extend half the time to increase randomness
    if (page->capacity < page->reserved && ((_mi_theap_random_next(theap) & 1) == 1)) {
      (void)mi_page_extend_free(theap, page);  // ok if this fails
      mi_assert_internal(mi_page_immediate_available(page));
    }
    #endif
    page->retire_expire = 0;
    return page; // fast path
  }
  else {
    return mi_page_queue_find_free_ex(theap, pq, true);
  }
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
  General allocation
----------------------------------------------------------- */

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
    mi_theap_stat_increase(theap, malloc_huge, mi_page_block_size(page));
    mi_theap_stat_counter_increase(theap, malloc_huge_count, 1);
  }
  return page;
}


// Allocate a page
// Note: in debug mode the size includes MI_PADDING_SIZE and might have overflowed.
static mi_page_t* mi_find_page(mi_theap_t* theap, size_t size, size_t huge_alignment) mi_attr_noexcept {
  const size_t req_size = size - MI_PADDING_SIZE;  // correct for padding_size in case of an overflow on `size`
  if mi_unlikely(req_size > MI_MAX_ALLOC_SIZE) {
    _mi_error_message(EOVERFLOW, "allocation request is too large (%zu bytes)\n", req_size);
    return NULL;
  }
  mi_page_queue_t* pq = mi_page_queue(theap, (huge_alignment > 0 ? MI_LARGE_MAX_OBJ_SIZE+1 : size));
  // huge allocation?
  if mi_unlikely(mi_page_queue_is_huge(pq) || req_size > MI_MAX_ALLOC_SIZE) {
    return mi_huge_page_alloc(theap,size,huge_alignment,pq);
  }
  else {
    // otherwise find a page with free blocks in our size segregated queues
    #if MI_PADDING
    mi_assert_internal(size >= MI_PADDING_SIZE);
    #endif
    return mi_find_free_page(theap, pq);
  }
}


// Generic allocation routine if the fast path (`alloc.c:mi_page_malloc`) does not succeed.
// Note: in debug mode the size includes MI_PADDING_SIZE and might have overflowed.
// The `huge_alignment` is normally 0 but is set to a multiple of MI_SLICE_SIZE for
// very large requested alignments in which case we use a huge singleton page.
// Note: we put `bool zero, size_t huge_alignment` into one parameter (with zero in the low bit)
// to use 4 parameters which compiles better on msvc for the malloc fast path.
void* _mi_malloc_generic(mi_theap_t* theap, size_t size, size_t zero_huge_alignment, size_t* usable) mi_attr_noexcept
{
  const bool zero = ((zero_huge_alignment & 1) != 0);
  const size_t huge_alignment = (zero_huge_alignment & ~1);

  #if !MI_THEAP_INITASNULL
  mi_assert_internal(theap != NULL);
  #endif

  // initialize if necessary
  if mi_unlikely(!mi_theap_is_initialized(theap)) {
    if (theap==&_mi_theap_empty_wrong) {
      // we were unable to allocate a theap for a first-class heap
      return NULL;
    }
    // otherwise we initialize the thread and its default theap
    mi_thread_init();
    theap = _mi_theap_default();
    if mi_unlikely(!mi_theap_is_initialized(theap)) { return NULL; }
    mi_assert_internal(_mi_theap_default()==theap);
  }
  mi_assert_internal(mi_theap_is_initialized(theap));

  // do administrative tasks every N generic mallocs
  if mi_unlikely(++theap->generic_count >= 1000) {
    theap->generic_collect_count += theap->generic_count;
    theap->generic_count = 0;
    // call potential deferred free routines
    _mi_deferred_free(theap, false);
    // free retired pages
    _mi_theap_collect_retired(theap, false);

    // collect every once in a while (10000 by default)
    const long generic_collect = mi_option_get_clamp(mi_option_generic_collect, 1, 1000000L);
    if (theap->generic_collect_count >= generic_collect) {
      theap->generic_collect_count = 0;
      mi_theap_collect(theap, false /* force? */);
    }
  }

  // find (or allocate) a page of the right size
  mi_page_t* page = mi_find_page(theap, size, huge_alignment);
  if mi_unlikely(page == NULL) { // first time out of memory, try to collect and retry the allocation once more
    mi_theap_collect(theap, true /* force? */);
    page = mi_find_page(theap, size, huge_alignment);
  }

  if mi_unlikely(page == NULL) { // out of memory
    const size_t req_size = size - MI_PADDING_SIZE;  // correct for padding_size in case of an overflow on `size`
    _mi_error_message(ENOMEM, "unable to allocate memory (%zu bytes)\n", req_size);
    return NULL;
  }

  mi_assert_internal(mi_page_immediate_available(page));
  mi_assert_internal(mi_page_block_size(page) >= size);
  mi_assert_internal(_mi_is_aligned(mi_page_slice_start(page), MI_PAGE_ALIGN));
  mi_assert_internal(_mi_ptr_page(mi_page_start(page))==page);

  // and try again, this time succeeding! (i.e. this should never recurse through _mi_page_malloc)
  if (usable!=NULL) { *usable = mi_page_usable_block_size(page); }
  void* const p = _mi_page_malloc_zero(theap,page,size,zero);
  mi_assert_internal(p != NULL);

  // heap profiling: only checked here (slow path); zero overhead on the fast path.
  // gate on the global rate so any thread picks up profiling enabled elsewhere.
  if mi_unlikely(_mi_prof_rate() != 0) {
    if (theap->prof_countdown == 0) { _mi_prof_theap_lazy_enable(theap); }
    const size_t bsize = mi_page_block_size(page);
    if ((theap->prof_countdown -= (intptr_t)bsize) <= 0) {
      _mi_prof_sample(theap, page, p, bsize);
    }
  }

  // move full pages to the full queue
  if (mi_page_block_size(page) > MI_SMALL_MAX_OBJ_SIZE && mi_page_is_full(page)) {
    mi_page_to_full(page, mi_page_queue_of(page));
  }
  return p;
}
