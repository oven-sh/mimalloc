/* ----------------------------------------------------------------------------
Copyright (c) 2018-2026, Microsoft Research, Daan Leijen
This is free software; you can redistribute it and/or modify it under the
terms of the MIT license. A copy of the license can be found in the file
"LICENSE" at the root of this distribution.
-----------------------------------------------------------------------------*/
#pragma once
#ifndef MI_INTERNAL_PAGE_H
#define MI_INTERNAL_PAGE_H

#include "../types.h"
#include "../prim/os.h"
#include "../heap/heap.h"
#include "../heap/page-map.h"
#include "../track.h"

mi_decl_restrict void* _mi_malloc_generic(mi_theap_t* theap, size_t size, size_t zero_huge_alignment, mi_page_t** ppage) mi_attr_noexcept mi_attr_malloc;
#define MI_MALLOC_GENERIC_ZERO        ((size_t)1)   // bits in `zero_huge_alignment` below the huge alignment (a multiple of MI_SLICE_SIZE)
#define MI_MALLOC_GENERIC_BLOCK_START ((size_t)2)   // the caller needs the start of a block: if this allocation is to be a sample, allocate nothing and return NULL (`mi_theap_should_sample` is true then)
mi_decl_restrict void* _mi_malloc_generic_no_sample(mi_theap_t* theap, size_t size, bool zero, mi_page_t** ppage) mi_attr_noexcept mi_attr_malloc;

void          _mi_page_retire(mi_page_t* page) mi_attr_noexcept;       // free the page if there are no other pages with many free blocks
void          _mi_page_unfull(mi_page_t* page);
bool          _mi_page_free(mi_page_t* page, mi_page_queue_t* pq);     // false if page release is delayed
void          _mi_page_abandon(mi_page_t* page, mi_page_queue_t* pq);  // abandon the page, to be picked up by another thread...
void          _mi_page_destroy(mi_page_t* page, mi_page_queue_t* pq);  // free the page even if it has live blocks
void          _mi_deferred_free(mi_theap_t* theap, bool force);
bool          _mi_page_free_collect(mi_page_t* page, bool force);  // returns `true` if cross-thread free'd blocks were collected
bool          _mi_page_free_collect_no_unpurge(mi_page_t* page, bool force);   // the same, for read-only heap inspection: never un-purges a hole
mi_block_t*   _mi_page_free_collect_partly(mi_page_t* page, mi_block_t* head);
mi_decl_nodiscard bool _mi_page_init(mi_theap_t* theap, mi_page_t* page);
void          _mi_page_update_stats(mi_page_t* page);
void          _mi_page_update_stats_for(mi_page_t* page, mi_theap_t* theapx /* can be NULL */);

void          _mi_theap_collect_retired(mi_theap_t* theap, bool force);
bool          _mi_theap_page_reclaim(mi_theap_t* theap, mi_page_t* page);  // true if an empty page was freed
#if MI_DEBUG>1
bool          _mi_page_is_valid(mi_page_t* page);
#endif

// in page-queue.c
bool          _mi_page_queue_is_valid(mi_theap_t* theap, const mi_page_queue_t* pq);
size_t        _mi_page_stats_bin(const mi_page_t* page); // for stats
size_t        _mi_bin_size(size_t bin);                  // for stats
size_t        _mi_bin(size_t size);                      // for stats


static inline mi_page_queue_t* mi_page_queue(const mi_theap_t* theap, size_t size) {
  mi_page_queue_t* const pq = &((mi_theap_t*)theap)->pages[_mi_bin(size)];
  if (size <= MI_LARGE_MAX_OBJ_SIZE) { mi_assert_internal(pq->block_size <= MI_LARGE_MAX_OBJ_SIZE); }
  return pq;
}

// Get the block size of a page
static inline size_t mi_page_block_size(const mi_page_t* page) {
  mi_assert_internal(page->block_size > 0);
  return page->block_size;
}

// Page start
static inline uint8_t* mi_page_start(const mi_page_t* page) {
  // multiplication must be done in `size_t`
  return (uint8_t*)page + page->page_offset;
}

static inline size_t mi_page_size(const mi_page_t* page) {
  return mi_page_block_size(page) * page->reserved;
}

static inline uint8_t* mi_page_area(const mi_page_t* page, size_t* size) {
  if (size) { *size = mi_page_size(page); }
  return mi_page_start(page);
}

static inline size_t mi_page_info_size(void) {
  return mi_align_up(sizeof(mi_page_t), MI_MAX_ALIGN_SIZE);
}

// Can the first block in a page with blocks of size `bsize` have a cache offset? (see `MI_PAGE_CACHE_OFFSET`)
// Not the power-of-two blocks that are naturally aligned (`mi_natural_alignment_of_size`): an offset would take that away.
static inline bool mi_block_size_has_cache_offset(size_t bsize) {
  return (MI_PAGE_CACHE_OFFSET != 0 && bsize > MI_SMALL_MAX_OBJ_SIZE &&
          !(mi_is_power_of_two(bsize) && bsize <= MI_PAGE_MAX_START_BLOCK_ALIGN2));
}

// Is a (non-small) block of size `bsize` allocated in a medium page?
// Blocks of 1/8th of a medium page (64 KiB) leave no space for a cache offset and use a large page instead.
static inline bool mi_block_size_is_medium(size_t bsize) {
  #if MI_ENABLE_LARGE_PAGES
  if (mi_block_size_has_cache_offset(bsize) && bsize == MI_MEDIUM_PAGE_SIZE/8) return false;
  #endif
  return (bsize <= MI_MEDIUM_MAX_OBJ_SIZE);
}

// The alignment that every block of size `bsize` is guaranteed to have in any page (or 0 if `bsize==0`).
// This is guaranteed by the block start (see `arena/arena-page.c:mi_page_meta_in_front_block_start` and `mi_page_meta_select`),
// and used by `mi_malloc_aligned` to avoid over-allocation for naturally aligned blocks.
static inline size_t mi_natural_alignment_of_size(size_t bsize) {
  const size_t lowbit = bsize & (~bsize + 1);  // largest power of 2 that divides `bsize`
  if (mi_is_power_of_two(bsize) && bsize <= MI_PAGE_MAX_START_BLOCK_ALIGN2) return bsize;   // power-of-two blocks up to 64KiB
  if (bsize <= MI_SMALL_SIZE_MAX) return lowbit;                                            // small blocks start at a multiple of their size
  if ((bsize % MI_PAGE_OSPAGE_BLOCK_ALIGN2) == 0) {                                         // multiples of 4KiB are 4KiB aligned (or cache offset unit aligned)
    return (mi_block_size_has_cache_offset(bsize) ? MI_PAGE_CACHE_OFFSET_UNIT : MI_PAGE_OSPAGE_BLOCK_ALIGN2);
  }
  return (lowbit < MI_MAX_ALIGN_SIZE ? lowbit : MI_MAX_ALIGN_SIZE);                         // otherwise blocks start at least `MI_MAX_ALIGN_SIZE` aligned
}

static inline bool mi_page_contains_address(const mi_page_t* page, const void* p) {
  size_t psize;
  uint8_t* start = mi_page_area(page, &psize);
  return (start <= (uint8_t*)p && (uint8_t*)p < start + psize);
}

static inline bool mi_page_is_in_arena(const mi_page_t* page) {
  return (page->memid.memkind == MI_MEM_ARENA);
}

static inline bool mi_page_is_singleton(const mi_page_t* page) {
  return (page->reserved == 1);
}

// Get the usable block size of a page without fixed padding.
// This may still include internal padding due to alignment and rounding up size classes.
static inline size_t mi_page_usable_block_size(const mi_page_t* page) {
  return mi_page_block_size(page) - MI_PADDING_SIZE;
}

static inline bool mi_page_meta_is_separated(const mi_page_t* page) {
  #if MI_PAGE_META_IS_ALIGNED
    #if MI_PAGE_META_SMALL_IS_ALIGNED
    return (page != mi_align_down_ptr(mi_page_start(page), MI_ARENA_SLICE_ALIGN));
    #else
    MI_UNUSED_RELEASE(page);
    mi_assert_internal(page != mi_align_down_ptr(mi_page_start(page), MI_ARENA_SLICE_ALIGN));
    return true;
    #endif
  #elif MI_PAGE_META_IS_SEPARATED
  // usually separated but can still be in front for direct OS allocations (due to size or alignment) or due to MI_PAGE_META_SMALL_IS_ALIGNED
  return (page->memid.memkind == MI_MEM_ARENA && page != mi_align_down_ptr(mi_page_start(page), MI_ARENA_SLICE_ALIGN));
  #else
  MI_UNUSED(page);
  return false;
  #endif
}

static inline uint8_t* mi_page_slice_start(const mi_page_t* page) {
  if (mi_page_meta_is_separated(page)) {
    // page meta info is at a separate location (at `arena->pages`)
    return (uint8_t*)mi_align_down_ptr(mi_page_start(page), MI_ARENA_SLICE_ALIGN);
  }
  else {
    // page meta info is at the start of the page slices
    return (uint8_t*)page;
  }
}

// This gives the offset relative to the start slice of a page.
static inline size_t mi_page_slice_offset_of(const mi_page_t* page, size_t offset_relative_to_page_start) {
  return (mi_page_start(page) - mi_page_slice_start(page)) + offset_relative_to_page_start;
}

// How much of the page is committed relative to the slice start? (or 0 if fully committed already)
static inline size_t mi_page_slice_committed(const mi_page_t* page) {
  return ((size_t)page->slice_pcommitted * _mi_os_page_size());
}

// Currently committed part of a page
static inline size_t mi_page_committed(const mi_page_t* page) {
  const size_t slice_committed = mi_page_slice_committed(page);
  return (slice_committed == 0 ? mi_page_size(page) : slice_committed - mi_page_slice_offset_of(page,0));
}

static inline size_t mi_page_used(const mi_page_t* page) {
  mi_assert_internal(page != NULL);
  return mi_xused_used_count(page->xused);
}

static inline void mi_page_used_reset(mi_page_t* page) {
  page->xused = mi_xused_used_reset(page->xused);
}

static inline size_t mi_page_alloc_count(const mi_page_t* page) {
  return mi_xused_alloc_count(page->xused);
}

static inline size_t mi_page_last_used(const mi_page_t* page) {
  #if MI_SIZE_SIZE >= 8
  return (page->xused.used_alloc >> 32) & 0xFFFF;
  #else
  return page->xlast_used;
  #endif
}

static inline size_t mi_page_last_alloc(const mi_page_t* page) {
  #if MI_SIZE_SIZE >= 8
  return (page->xused.used_alloc >> 48) & 0xFFFF;
  #else
  return page->xlast_alloc;
  #endif
}


// are all blocks in a page freed?
// note: needs up-to-date used count, (as the `xthread_free` list may not be empty). see `_mi_page_collect_free`.
static inline bool mi_page_all_free(const mi_page_t* page) {
  mi_assert_internal(page != NULL);
  return (mi_page_used(page)==0);
}

// ------------------------------------------------------
// Page hole purging  (see the "Page hole purging" section in `alloc/page.c`)
// ------------------------------------------------------

void          _mi_page_purge_holes(mi_page_t* page, mi_tld_t* tld);   // `tld`: the thread whose sweep this is (see `_mi_page_purge_holes_begin`)
void          _mi_page_purged_reset(mi_page_t* page);
bool          _mi_page_unpurge_run(mi_page_t* page, size_t max_size);
void          _mi_page_unpurge_all(mi_page_t* page);
size_t        _mi_page_purged_count(const mi_page_t* page);
void          _mi_page_unpurge_unformed_upto(mi_page_t* page, uintptr_t end);   // hand the discarded unformed tail back below `end` (an absolute address)
size_t        _mi_page_unformed_purged_bytes(const mi_page_t* page);            // the bytes of this page's unformed tail that are discarded right now
bool          _mi_page_purge_os_page_blocks(size_t os_page_size, size_t block_size, uintptr_t page_start,
                                            size_t capacity, size_t k, size_t* first, size_t* last);
bool          _mi_page_purge_holes_in_progress(void);            // is the calling thread inside a sweep of its own heaps?
void          _mi_page_holes_count_page_freed(void);
void          _mi_page_holes_count_ineligible(const mi_page_t* page);
void          _mi_page_holes_reset_ineligible(void);
void          _mi_page_purge_holes_begin(mi_tld_t* tld);         // around each pass of a sweep; `tld` is the thread being swept
void          _mi_page_purge_holes_end(mi_tld_t* tld);
void          _mi_page_purge_holes_sweep_begin(mi_tld_t* tld);   // once per idle sweep, before its passes
void          _mi_page_purge_holes_forked_child(void);
void          _mi_page_purge_holes_floor_release(mi_tld_t* tld);  // a tld that goes away gives its share of `purge_holes_large_floor` back
bool          _mi_page_purge_holes_floor_is_due(const mi_tld_t* tld);   // what its last sweep left under the floor is old enough to go by now
void          _mi_page_purge_holes_floor_resolve(mi_tld_t* tld);  // once per idle sweep, after the pages of the thread's own theaps: which of them stay under the floor
bool          _mi_page_purge_holes_free_page_stays(mi_page_t* page, mi_tld_t* tld);   // in a sweep: is this large page with no block in use left as it is for now?
bool          _mi_page_purge_holes_large_page_waits(const mi_page_t* page, const mi_tld_t* tld);   // ..and outside of one: is it for the sweeps to decide on?
bool          _mi_page_purge_holes_large_page_is_kept(const mi_page_t* page, const mi_tld_t* tld); // ..and did the last sweep of this thread leave it under the floor?
#define MI_HOLES_FLOOR_LIST_MAX   (64)
typedef struct mi_holes_floor_item_s {
  mi_page_t* page;
  mi_page_queue_t* pq;  // the queue it was in (it is looked up there before it is touched again)
  uint32_t   age;       // epochs since the page was last allocated from: smaller is more recent
  size_t     bytes;
} mi_holes_floor_item_t;
typedef struct mi_holes_floor_list_s {
  size_t count;
  mi_holes_floor_item_t items[MI_HOLES_FLOOR_LIST_MAX];
} mi_holes_floor_list_t;
size_t        _mi_page_purge_holes_floor_kept(void);             // (for `test-purge-holes-large.c`)
void          _mi_page_purge_holes_epoch_advance(void);          // ..by this, which every idle sweep begins with
uint32_t      _mi_page_purge_holes_epoch(void);                  // the epoch of the sweep: moved on by an idle sweep, once in `purge_holes_min_interval` at the most (`alloc/page.c`)

// ------------------------------------------------------
// Hole report  (`mi_purge_holes_report`, see the "Hole report" section in `alloc/page.c`)
//
// What is left behind after a sweep, per size class: the free bytes that share an OS page
// with a live block cannot be discarded, and this says how much of that there is and how
// few live blocks are holding it down.
// ------------------------------------------------------

#define MI_HOLES_HIST_BUCKETS  (5)    // live blocks per pinned OS page: 1, 2, 3-4, 5-8, 9+
#define MI_HOLES_GRAN_COUNT    (5)    // the hypothetical OS page sizes of the granularity curve

typedef struct mi_holes_bin_s {
  size_t block_size;           // the largest block size seen in this bin
  size_t pages;
  size_t ineligible_pages;     // pages `mi_page_can_purge_holes` rejects (nothing in them is discardable)
  size_t live_bytes;           // bytes of allocated blocks
  size_t free_bytes;           // bytes of free blocks (free-listed *and* already discarded)
  size_t undiscardable_bytes;  // free bytes in an OS page that a live block pins (or that is not entirely inside the block area)
  size_t discarded_bytes;      // bytes of the OS pages that are discarded right now
  size_t edge_bytes;           // of `undiscardable_bytes`: those in a partial OS page (holds the page header, or memory past `capacity`)
  size_t pending_bytes;        // free bytes in a fully free OS page that is not discarded (no sweep yet, or the discard failed)
  size_t pinned_ospages;       // OS pages holding >= 1 live block
  size_t pinned_live_blocks;   // live blocks over those (a block straddling two pinned OS pages counts in both)
  size_t pinned_free_bytes;    // free bytes trapped inside those pinned OS pages
  size_t pinned_live_bytes;    // live bytes inside those pinned OS pages
  size_t hist[MI_HOLES_HIST_BUCKETS];
} mi_holes_bin_t;

typedef struct mi_holes_report_s {
  mi_holes_bin_t bin[MI_BIN_COUNT];
  size_t total_pages;
  size_t ineligible_pages;
  size_t unformed_bytes;       // memory of blocks not formed yet (`capacity < reserved`)
  size_t unformed_discarded_bytes;  // of those: the OS pages the sweep discarded (`page->unformed_purged_*`)
  // The granularity curve: how many bytes WOULD be discardable if the OS page size were
  // `mi_holes_granularity(g)` -- the total size of the G-aligned, G-sized spans that lie wholly
  // inside a page's block area and hold not one live block. Nothing is discarded to measure it;
  // it is pure counting over the same free/live classification the sweep uses.
  size_t discardable_at[MI_HOLES_GRAN_COUNT];
  size_t unmadvisable_pages;   // excluded from the curve: their memory cannot be discarded at ANY granularity

  // Where the memory actually IS. If the curve turns out flat, the free memory is not
  // contaminating the pages -- and then this says where it went instead.
  //
  // CAVEAT, and it is why these fields are named the way they are: `slices_committed` is set for
  // the WHOLE arena at reserve time whenever the OS memory is `initially_committed` (which every
  // POSIX mmap is), and a reset-style purge (MADV_FREE_REUSABLE on darwin) does NOT clear it. So
  // there that bitmap is address space, not residency, and `arena_committed_bytes` must not be
  // read as "memory we are paying for". `slices_dirty` (ever touched) and `slices_purge`
  // (scheduled but not yet purged) are the bitmaps that carry residency information.
  size_t page_committed_bytes;       // committed bytes of the pages this walk reached
  size_t arena_reserved_bytes;       // total arena address space
  size_t arena_committed_bytes;      // popcount(slices_committed) -- see the caveat: on POSIX this is ~= reserved
  size_t arena_free_dirty_bytes;     // slices in NO page that were touched at least once: the UPPER bound on arena slack still resident
  size_t arena_purge_pending_bytes;  // slices in NO page, scheduled for purge but not purged yet: definitely still resident (the purge delay)
  size_t arena_meta_bytes;           // the arenas' own bitmaps (`info_slices`) -- ROUGH: excludes the `mi_meta` heaps
} mi_holes_report_t;

size_t        mi_holes_granularity(size_t g);
void          _mi_page_holes_report_page(const mi_page_t* page, mi_holes_report_t* rep);
void          _mi_page_holes_report_print(const mi_holes_report_t* rep);
void          _mi_arenas_holes_report(mi_heap_t* heap, mi_holes_report_t* rep);
void          _mi_arenas_holes_committed(mi_heap_t* heap, mi_holes_report_t* rep);
void          _mi_purge_holes_report_collect(mi_holes_report_t* rep);

// The unit of the purge bitmap of this page: what one bit of `page->purged` stands for, and the
// granularity of a discard. It is the OS page where the block area then needs no more than
// `MI_PAGE_PURGE_BITS` bits, and else the smallest power-of-two multiple of the OS page for which it
// does: 16 KiB for a large (4 MiB) page on a 4 KiB OS page. Fixed for the lifetime of a page (it
// depends on `page_start`, `block_size * reserved` and the OS page size only).
// The rule for a discard does not depend on how the block size relates to the unit: a unit is
// discarded only when it lies wholly inside the block area and EVERY block that overlaps it is
// free (`mi_page_purge_holes_walk`). Where the block size is a multiple of the unit and the block
// area starts on a unit boundary, as in a large page (block sizes of 96 KiB and up in steps of
// 16 KiB or more; the blocks start at the slice where the page meta data is kept apart from the
// page, which is every configuration but a flat page map, and the page has no cache offset, see
// `MI_PAGE_CACHE_OFFSET`), that is each free block as a whole;
// a block that shares a unit with a live neighbour keeps that unit.
#define MI_PAGE_PURGE_MAX_UNIT  MI_ARENA_SLICE_SIZE
static inline size_t mi_page_purge_unit(const mi_page_t* page) {
  size_t unit = _mi_os_page_size();
  const uintptr_t start = (uintptr_t)mi_page_start(page);
  const uintptr_t end = start + mi_page_size(page);
  while ((end - mi_align_down(start, unit)) > (MI_PAGE_PURGE_BITS * unit) && unit < MI_PAGE_PURGE_MAX_UNIT) { unit <<= 1; }
  return unit;
}

// The base of the purge bitmap: the start of the first unit that the block area of this page
// overlaps. It is unit aligned by construction (and so OS-page aligned), so bit `k` always names
// the aligned range `[base + k*unit, base + (k+1)*unit)`.
static inline uintptr_t mi_page_purge_base_of(const mi_page_t* page, size_t unit) {
  return mi_align_down((uintptr_t)mi_page_start(page), unit);
}

static inline uintptr_t mi_page_purge_base(const mi_page_t* page) {
  return mi_page_purge_base_of(page, mi_page_purge_unit(page));
}

// the number of units the block area spans = the number of bits this page needs
static inline size_t mi_page_purge_bits_of(const mi_page_t* page, size_t unit) {
  const uintptr_t base = mi_page_purge_base_of(page, unit);
  const uintptr_t end = (uintptr_t)mi_page_start(page) + mi_page_size(page);
  return mi_divide_up((size_t)(end - base), unit);
}

static inline size_t mi_page_purge_bits(const mi_page_t* page) {
  return mi_page_purge_bits_of(page, mi_page_purge_unit(page));
}

// Eligible when the page's units fit the bitmap, which `mi_page_purge_unit` sees to for every page
// of up to `MI_PAGE_PURGE_BITS` slices (small, medium and large pages are 1, 8 and 64). This does not
// depend on the block size at all: a discard covers a whole unit, so any number of small free blocks
// can together cover one (and a page whose free runs never cover a whole unit simply discards
// nothing). A huge page is a singleton (one block) so there is nothing to purge in it.
// Pinned memory (large/huge OS pages) cannot be madvise'd away, and an arena with a custom
// commit function owns its own commit/decommit -- like every other purge site
// (`mi_arena_schedule_purge`, `_mi_os_purge_ex`), we stay away from both.
static inline bool mi_page_can_purge_holes(const mi_page_t* page) {
  if (page->reserved <= 1) return false;             // a singleton page has no free block while it is in use
  if (page->memid.is_pinned) return false;
  const mi_arena_t* const arena = mi_memid_arena(page->memid);
  if (arena != NULL && arena->commit_fun != NULL) return false;
  return (mi_page_purge_bits(page) <= MI_PAGE_PURGE_BITS);
}

static inline bool mi_page_has_purged(const mi_page_t* page) {
  for (size_t i = 0; i < MI_PAGE_PURGE_WORDS; i++) {
    if (page->purged[i] != 0) return true;
  }
  return false;
}

// is the memory of unit `k` (counted from `mi_page_purge_base`; an OS page in all but large pages) discarded?
static inline bool mi_page_os_page_purged(const mi_page_t* page, size_t k) {
  if (k >= MI_PAGE_PURGE_BITS) return false;
  return ((page->purged[k / 64] >> (k % 64)) & 1) != 0;
}

// Is the block at index `idx` free-but-discarded (and thus not on any free list)?
// This is the derived purge predicate: a unit is discarded only when *every* block
// overlapping it is free, so a block lost memory exactly when it overlaps a discarded unit.
static inline bool mi_page_block_index_is_purged(const mi_page_t* page, size_t idx) {
  if (!mi_page_has_purged(page)) return false;
  const size_t os_size = mi_page_purge_unit(page);
  const uintptr_t base = mi_page_purge_base_of(page, os_size);
  const uintptr_t lo = (uintptr_t)mi_page_start(page) + (idx * page->block_size);
  const size_t kfirst = (size_t)(lo - base) / os_size;
  const size_t klast = (size_t)((lo + page->block_size - 1) - base) / os_size;
  for (size_t k = kfirst; k <= klast && k < MI_PAGE_PURGE_BITS; k++) {
    if (mi_page_os_page_purged(page, k)) return true;
  }
  return false;
}

// is `block` free-but-discarded? A purged block is on no free list, so a free-list walk
// (as `alloc/free.c:mi_check_double_freex` does) cannot see that it is already free.
static inline bool mi_page_block_is_purged(const mi_page_t* page, const void* block) {
  if (!mi_page_has_purged(page)) return false;
  mi_assert_internal((const uint8_t*)block >= mi_page_start(page));
  const size_t idx = ((size_t)((const uint8_t*)block - mi_page_start(page))) / page->block_size;
  if (idx >= page->capacity) return false;
  return mi_page_block_index_is_purged(page, idx);
}

// are there immediately available blocks, i.e. blocks available on the free list.
static inline bool mi_page_immediate_available(const mi_page_t* page) {
  mi_assert_internal(page != NULL);
  return (page->free != NULL);
}


// is the page not yet used up to its reserved space?
static inline bool mi_page_is_expandable(const mi_page_t* page) {
  mi_assert_internal(page != NULL);
  mi_assert_internal(page->capacity <= page->reserved);
  return (page->capacity < page->reserved);
}


static inline bool mi_page_is_full(const mi_page_t* page) {
  const bool full = (page->reserved == mi_page_used(page));
  mi_assert_internal(!full || page->free == NULL);
  return full;
}

// is more than (1 - 1/n)'th of a page in use?
static inline bool mi_page_is_used_at_frac(const mi_page_t* page, size_t n) {
  if (page==NULL) return true;
  const size_t frac = page->reserved / n;
  return (page->reserved - mi_page_used(page) <= frac);
}

// is more than 7/8th of a page in use?
static inline bool mi_page_is_mostly_used(const mi_page_t* page) {
  return mi_page_is_used_at_frac(page, 8);
}

static inline bool mi_page_is_small(const mi_page_t* page) {
  return (page->block_size <= MI_SMALL_MAX_OBJ_SIZE && !mi_page_is_singleton(page));
}

static inline bool mi_page_is_huge(const mi_page_t* page) {
  return (mi_page_is_singleton(page) &&
          (page->block_size > MI_LARGE_MAX_OBJ_SIZE ||
           (mi_memkind_is_os(page->memid.memkind) && page->memid.mem.os.base < (void*)page)));
}

static inline size_t mi_page_min_commit_size(void) {
  const size_t psize = _mi_os_page_size();
  return (MI_PAGE_MIN_COMMIT_SIZE >= psize ? MI_PAGE_MIN_COMMIT_SIZE : psize);
}

//-----------------------------------------------------------
// Page thread id and flags
//-----------------------------------------------------------

// Thread id of thread that owns this page (with flags in the bottom 2 bits)
static inline mi_threadid_t mi_page_xthread_id(const mi_page_t* page) {
  return mi_atomic_load_relaxed(&((mi_page_t*)page)->xthread_id);
}

// Plain thread id of the thread that owns this page
static inline mi_threadid_t mi_page_thread_id(const mi_page_t* page) {
  return (mi_page_xthread_id(page) & ~MI_PAGE_FLAG_MASK);
}

static inline mi_page_flags_t mi_page_flags(const mi_page_t* page) {
  return (mi_page_xthread_id(page) & MI_PAGE_FLAG_MASK);
}

static inline bool mi_page_flags_set(mi_page_t* page, bool set, mi_page_flags_t newflag) {
  mi_page_flags_t old;
  if (set) { old = mi_atomic_or_relaxed(&page->xthread_id, newflag); }
      else { old = mi_atomic_and_relaxed(&page->xthread_id, ~newflag); }
  return ((old & newflag) == newflag);
}

static inline bool mi_page_is_in_full(const mi_page_t* page) {
  return ((mi_page_flags(page) & MI_PAGE_IN_FULL_QUEUE) != 0);
}

static inline void mi_page_set_in_full(mi_page_t* page, bool in_full) {
  const bool was_in_full = mi_page_flags_set(page, in_full, MI_PAGE_IN_FULL_QUEUE);
  if (was_in_full != in_full) {
    // optimize: maintain pages_full_size to avoid visiting the full queue (issue #1220)
    mi_theap_t* const theap = page->theap;
    mi_assert_internal(theap!=NULL);
    if (theap != NULL) {
      mi_assert_internal(page->capacity==page->reserved);
      const size_t size = page->reserved * mi_page_block_size(page);
      if (in_full) { theap->pages_full_size += size; }
              else { mi_assert_internal(size <= theap->pages_full_size); theap->pages_full_size -= size; }
    }
  }
}

static inline bool mi_page_has_interior_pointers(const mi_page_t* page) {
  return ((mi_page_flags(page) & MI_PAGE_HAS_INTERIOR_POINTERS) != 0);
}

static inline void mi_page_set_has_interior_pointers(mi_page_t* page, bool has_aligned) {
  mi_page_flags_set(page, has_aligned, MI_PAGE_HAS_INTERIOR_POINTERS);
}

static inline void mi_page_set_theap(mi_page_t* page, mi_theap_t* theap) {
  // mi_assert_internal(!mi_page_is_in_full(page));  // can happen when destroying pages on theap_destroy
  page->theap = theap;
  const mi_threadid_t tid = (theap == NULL ? MI_THREADID_ABANDONED : theap->tld->thread_id);
  mi_assert_internal((tid & MI_PAGE_FLAG_MASK) == 0);

  // we need to use an atomic cas since a concurrent thread may still set the MI_PAGE_HAS_INTERIOR_POINTERS flag (see `alloc_aligned.c`).
  mi_threadid_t xtid_old = mi_page_xthread_id(page);
  mi_threadid_t xtid;
  do {
    xtid = tid | (xtid_old & MI_PAGE_FLAG_MASK);
  } while (!mi_atomic_cas_weak_release(&page->xthread_id, &xtid_old, xtid));
}

static inline bool mi_page_is_abandoned(const mi_page_t* page) {
  // note: the xtheap field of an abandoned theap is set to the subproc (for fast reclaim-on-free)
  return (mi_page_thread_id(page) <= MI_THREADID_ABANDONED_MAPPED);
}

static inline bool mi_page_is_abandoned_mapped(const mi_page_t* page) {
  return (mi_page_thread_id(page) == MI_THREADID_ABANDONED_MAPPED);
}

static inline void mi_page_set_abandoned_mapped(mi_page_t* page) {
  mi_assert_internal(mi_page_is_abandoned(page));
  mi_atomic_or_relaxed(&page->xthread_id, (mi_threadid_t)MI_THREADID_ABANDONED_MAPPED);
}

static inline void mi_page_clear_abandoned_mapped(mi_page_t* page) {
  mi_assert_internal(mi_page_is_abandoned_mapped(page));
  mi_atomic_and_relaxed(&page->xthread_id, (mi_threadid_t)MI_PAGE_FLAG_MASK);
}


static inline mi_theap_t* mi_page_theap(const mi_page_t* page) {
  mi_assert_internal(!mi_page_is_abandoned(page));
  mi_assert_internal(page->theap != NULL && !_mi_is_empty_theap(page->theap));
  return page->theap;
}

static inline mi_tld_t* mi_page_tld(const mi_page_t* page) {
  mi_assert_internal(!mi_page_is_abandoned(page));
  mi_assert_internal(page->theap != NULL);
  return page->theap->tld;
}


// The heap of a page. `mi_heap_delete` re-points this to the main heap and then frees the heap (and its
// `arena_pages`) while other threads may still be freeing blocks of the heap. It waits for such a thread
// only while that thread owns an abandoned arena page whose bit in the heap's `arena_pages` is still set
// (`arena/arena-page.c:mi_heap_visit_page_at`; a page that a theap still holds is moved without waiting, as only its
// own thread touches it). So on a free of a block of another thread, dereference this (or `mi_page_subproc`)
// only after owning the page (not in `alloc/free.c:mi_stat_free`), and not anymore after clearing that bit
// (`arena/arena-page.c:mi_arenas_page_free_prim`).
static inline mi_heap_t* mi_page_heap(const mi_page_t* page) {
  mi_heap_t* heap = page->heap;
  mi_assert_internal(heap != NULL);
  return heap;
}

static inline mi_subproc_t* mi_page_subproc(const mi_page_t* page) {
  mi_heap_t* const heap = mi_page_heap(page);
  return heap->subproc;
}

//-----------------------------------------------------------
// Thread free list and ownership
//-----------------------------------------------------------

// Thread free flag helpers
static inline mi_block_t* mi_tf_block(mi_thread_free_t tf) {
  return (mi_block_t*)(tf & ~1);
}

static inline bool mi_tf_is_owned(mi_thread_free_t tf) {
  return ((tf & 1) == 1);
}

static inline mi_thread_free_t mi_tf_create(mi_block_t* block, bool owned) {
  const uintptr_t base = (uintptr_t)block | (owned ? 1 : 0);
  return (mi_thread_free_t)base;
}

static inline mi_thread_free_t mi_tf_set_owned(mi_thread_free_t tf, bool owned) {
  return mi_tf_create(mi_tf_block(tf), owned);
}

// Thread free access
static inline mi_block_t* mi_page_thread_free(const mi_page_t* page) {
  return mi_tf_block(mi_atomic_load_relaxed(&((mi_page_t*)page)->xthread_free));
}

// are there any available blocks?
static inline bool mi_page_has_any_available(const mi_page_t* page) {
  mi_assert_internal(page != NULL && page->reserved > 0);
  return (mi_page_used(page) < page->reserved || (mi_page_thread_free(page) != NULL));
}

// Owned?
static inline bool mi_page_is_owned(const mi_page_t* page) {
  return mi_tf_is_owned(mi_atomic_load_relaxed(&((mi_page_t*)page)->xthread_free));
}

// get ownership; returns true if the page was not owned before.
static inline bool mi_page_claim_ownership(mi_page_t* page) {
  const uintptr_t old = mi_atomic_or_acq_rel(&page->xthread_free, (uintptr_t)1);
  return ((old&1)==0);
}

/* -----------------------------------------------------------
  Delayed page release
----------------------------------------------------------- */

static inline void mi_page_qsbr_unlink(mi_page_t* page) {
#if MI_QSBR
  if (!page->qsbr_in_queue) {
    mi_assert_internal(page->qsbr_next == NULL && page->qsbr_prev == NULL);
    return;
  }
  mi_theap_t* theap = mi_page_theap(page);
  if (page->qsbr_prev != NULL) { page->qsbr_prev->qsbr_next = page->qsbr_next; }
  else {
    mi_assert_internal(theap->qsbr_first == page);
    theap->qsbr_first = page->qsbr_next;
  }
  if (page->qsbr_next != NULL) { page->qsbr_next->qsbr_prev = page->qsbr_prev; }
  else {
    mi_assert_internal(theap->qsbr_last == page);
    theap->qsbr_last = page->qsbr_prev;
  }
  page->qsbr_next = page->qsbr_prev = NULL;
  page->qsbr_in_queue = false;
#else
  MI_UNUSED(page);
#endif
}

static inline void mi_page_qsbr_enqueue(mi_page_t* page, mi_theap_t* theap) {
#if MI_QSBR
  mi_assert_internal(mi_page_theap(page) == theap);
  mi_assert_internal(mi_page_all_free(page));
  mi_assert_internal(mi_heap_qsbr_is_enabled(mi_page_heap(page)));
  mi_assert_internal(!mi_page_qsbr_is_clear(&page->qsbr));
  if (page->qsbr_in_queue) return;
  mi_assert_internal(page->qsbr_next == NULL && page->qsbr_prev == NULL);
  page->qsbr_prev = theap->qsbr_last;
  if (theap->qsbr_last != NULL) { theap->qsbr_last->qsbr_next = page; }
  else { theap->qsbr_first = page; }
  theap->qsbr_last = page;
  page->qsbr_in_queue = true;
#else
  MI_UNUSED(page);
  MI_UNUSED(theap);
#endif
}

static inline void _mi_page_qsbr_clear(mi_page_t* page) {
#if MI_QSBR
  mi_page_qsbr_unlink(page);
  mi_page_qsbr_clear(&page->qsbr);
#else
  MI_UNUSED(page);
#endif
}

static inline bool _mi_page_qsbr_is_clear(const mi_page_t* page) {
#if MI_QSBR
  return (!page->qsbr_in_queue && page->qsbr_next == NULL && page->qsbr_prev == NULL &&
          mi_page_qsbr_is_clear(&page->qsbr));
#else
  MI_UNUSED(page);
  return true;
#endif
}

static inline bool _mi_page_qsbr_should_delay(mi_page_t* page) {
#if MI_QSBR
  mi_assert_internal(mi_page_is_owned(page) && mi_page_all_free(page));
  mi_heap_t* heap = mi_page_heap(page);
  if (!mi_heap_qsbr_is_enabled(heap)) return false;
  const bool delayed = mi_page_qsbr_should_delay(&page->qsbr, &heap->qsbr, page->capacity * mi_page_block_size(page));
  mi_assert_internal(!delayed || !mi_page_qsbr_is_clear(&page->qsbr));
  return delayed;
#else
  MI_UNUSED(page);
  return false;
#endif
}

/* -------------------------------------------------------------------
Encoding/Decoding the free list next pointers

This is to protect against buffer overflow exploits where the
free list is mutated. Many hardened allocators xor the next pointer `p`
with a secret key `k1`, as `p^k1`. This prevents overwriting with known
values but might be still too weak: if the attacker can guess
the pointer `p` this  can reveal `k1` (since `p^k1^p == k1`).
Moreover, if multiple blocks can be read as well, the attacker can
xor both as `(p1^k1) ^ (p2^k1) == p1^p2` which may reveal a lot
about the pointers (and subsequently `k1`).

Instead mimalloc uses an extra key `k2` and encodes as `((p^k2)<<<k1)+k1`.
Since these operations are not associative, the above approaches do not
work so well any more even if the `p` can be guesstimated. For example,
for the read case we can subtract two entries to discard the `+k1` term,
but that leads to `((p1^k2)<<<k1) - ((p2^k2)<<<k1)` at best.
We include the left-rotation since xor and addition are otherwise linear
in the lowest bit. Finally, both keys are unique per page which reduces
the re-use of keys by a large factor.

We also pass a separate `null` value to be used as `NULL` or otherwise
`(k2<<<k1)+k1` would appear (too) often as a sentinel value.
------------------------------------------------------------------- */

static inline bool mi_is_in_same_page(const void* p, const void* q) {
  mi_page_t* page = mi_ptr_page(p);
  return mi_page_contains_address(page,q);
  // return (mi_ptr_page(p) == mi_ptr_page(q));
}

static inline void* mi_ptr_decode(const void* null, const mi_encoded_t x, const uintptr_t* keys) {
  const uintptr_t k1 = keys[0];
  #if MI_PAGE_KEY_COUNT==2
  const uintptr_t k2 = keys[1];
  #else
  const uintptr_t k2 = mi_rotr(k1,13);
  #endif
  void* p = (void*)(mi_rotr(x - k1, k1) ^ k2);
  return (p==null ? NULL : p);
}

static inline mi_encoded_t mi_ptr_encode(const void* null, const void* p, const uintptr_t* keys) {
  const uintptr_t k1 = keys[0];
  #if MI_PAGE_KEY_COUNT==2
  const uintptr_t k2 = keys[1];
  #else
  const uintptr_t k2 = mi_rotr(k1,13);
  #endif
  const uintptr_t x = (uintptr_t)(p==NULL ? null : p);
  return mi_rotl(x ^ k2, k1) + k1;
}

static inline uint32_t mi_ptr_encode_canary(const void* null, const void* p, const uintptr_t* keys) {
  const uint32_t x = (uint32_t)(mi_ptr_encode(null,p,keys));
  // make the lowest byte 0 to prevent spurious read overflows which could be a security issue (issue #951)
  // also clear bit 9 which we set only when a block is freed.
  #if MI_BIG_ENDIAN
  return (x & 0x00FFFEFF);
  #else
  return (x & 0xFFFFFE00);
  #endif
}

static inline uint32_t mi_ptr_encode_canary_freed(void) {
  return (0x00DEAD00);  // set bit 9 so it is different from any valid canary
}

static inline bool mi_ptr_decode_canary_is_freed(uint32_t canary) {
  return (canary == mi_ptr_encode_canary_freed());
}

static inline mi_block_t* mi_block_nextx( const void* null, const mi_block_t* block, const uintptr_t* keys ) {
  mi_track_mem_defined(block,sizeof(mi_block_t));
  mi_block_t* next;
  #if MI_ENCODE_FREELIST
  next = (mi_block_t*)mi_ptr_decode(null, block->next, keys);
  #else
  MI_UNUSED(keys); MI_UNUSED(null);
  next = (mi_block_t*)block->next;
  #endif
  mi_track_mem_noaccess(block,sizeof(mi_block_t));
  return next;
}

static inline void mi_block_set_nextx(const void* null, mi_block_t* block, const mi_block_t* next, const uintptr_t* keys) {
  mi_track_mem_undefined(block,sizeof(mi_block_t));
  #if MI_ENCODE_FREELIST
  block->next = mi_ptr_encode(null, next, keys);
  #else
  MI_UNUSED(keys); MI_UNUSED(null);
  block->next = (mi_encoded_t)next;
  #endif
  mi_track_mem_noaccess(block,sizeof(mi_block_t));
}

static inline mi_block_t* mi_block_next(const mi_page_t* page, const mi_block_t* block) {
  #if MI_ENCODE_FREELIST
  mi_block_t* next = mi_block_nextx(page,block,page->keys);
  // check for free list corruption: is `next` at least in the same page?
  // todo: check if `next` is `page->block_size` aligned?
  if mi_unlikely(next!=NULL && !mi_page_contains_address(page,next)) {
    return _mi_block_next_is_corrupted(page,block,next); // returns NULL
  }
  return next;
  #else
  MI_UNUSED(page);
  return mi_block_nextx(page,block,NULL);
  #endif
}

static inline void mi_block_set_next(const mi_page_t* page, mi_block_t* block, const mi_block_t* next) {
  #if MI_ENCODE_FREELIST
  mi_block_set_nextx(page,block,next, page->keys);
  #else
  MI_UNUSED(page);
  mi_block_set_nextx(page,block,next,NULL);
  #endif
}

#endif
