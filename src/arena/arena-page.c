/* ----------------------------------------------------------------------------
Copyright (c) 2019-2026, Microsoft Research, Daan Leijen
This is free software; you can redistribute it and/or modify it under the
terms of the MIT license. A copy of the license can be found in the file
"LICENSE" at the root of this distribution.
-----------------------------------------------------------------------------*/

#include "mimalloc.h"
#include "mimalloc/prim.h"                    // _mi_prim_thread_yield
#include "mimalloc/prim/prim-tls.h"
#include "mimalloc/util/stats.h"
#include "mimalloc/alloc/free.h"              // _mi_free_subproc_safe
#include "mimalloc/alloc/page.h"
#include "mimalloc/arena/arena-alloc.h"
#include "mimalloc/profile/sample-guarded.h"  // _mi_page_unguard_all
#include "mimalloc/arena/arena-page.h"
#include "arena.h"

/* -----------------------------------------------------------
  Util
----------------------------------------------------------- */

#if MI_DEBUG > 1
static bool mi_heap_has_page(mi_heap_t* heap, mi_arena_t* arena, mi_page_t* page) {
  mi_assert(arena->arena_idx < MI_MAX_ARENAS);
  mi_arena_pages_t* arena_pages = heap->arena_pages[arena->arena_idx];
  return (page->memid.memkind == MI_MEM_ARENA &&
          page->memid.mem.arena.arena == arena &&
          arena_pages != NULL &&
          mi_bitmap_is_setN(arena_pages->pages, page->memid.mem.arena.slice_index, 1));
}
#endif

mi_page_t* mi_arena_page_at_slice(mi_arena_t* arena, size_t slice_index) {
  mi_assert_internal(slice_index < arena->slice_count);
  #if MI_PAGE_META_IS_ALIGNED
  mi_page_t* const page = mi_aligned_ptr_page(mi_arena_slice_start(arena,slice_index)); // todo: optimize?
  return page;
  #else
  if (arena->pages_meta != NULL) {
    mi_page_t* const page = &arena->pages_meta[slice_index];
    #if MI_PAGE_META_SMALL_IS_ALIGNED
    // pages with small blocks still have the page at the start of the slice (and set the `block_size` in pages_meta to 0)
    if (page->block_size>0) return page;
    #else
    return page;
    #endif
  }
  // fall through (for MI_PAGE_META_SMALL_IS_ALIGNED)
  return (mi_page_t*)mi_arena_slice_start(arena,slice_index);
  #endif
}

static size_t mi_page_full_size(mi_page_t* page) {
  if (page->memid.memkind == MI_MEM_ARENA) {
    return page->memid.mem.arena.slice_count * MI_ARENA_SLICE_SIZE;
  }
  else if (mi_memid_is_os(page->memid) || page->memid.memkind == MI_MEM_EXTERNAL) {
    mi_assert_internal((uint8_t*)page->memid.mem.os.base <= (uint8_t*)page);
    const ptrdiff_t presize = (uint8_t*)page - (uint8_t*)page->memid.mem.os.base;
    mi_assert_internal((ptrdiff_t)page->memid.mem.os.size >= presize);
    return (presize > (ptrdiff_t)page->memid.mem.os.size ? 0 : page->memid.mem.os.size - presize);
  }
  else {
    return 0;
  }
}

/* -----------------------------------------------------------
  Arena pages: these bitmaps track abandoned pages per size
  bin, and the overall pages belonging to a particular heap.
----------------------------------------------------------- */

static mi_arena_pages_t* mi_heap_arena_pages(mi_heap_t* heap, mi_arena_t* arena) {
  mi_assert_internal(arena!=NULL);
  mi_assert_internal(heap!=NULL);
  mi_assert(arena->arena_idx < MI_MAX_ARENAS);
  return mi_atomic_load_ptr_acquire(mi_arena_pages_t, &heap->arena_pages[arena->arena_idx]);
}


// allocate initial arena_pages from the main heap
static mi_arena_pages_t* mi_arena_pages_alloc(mi_arena_t* arena) {
  const size_t slice_count = arena->slice_count;
  // only the bitmap needs alignment: align it inside the block instead of using the (slower) aligned allocation
  const size_t size = sizeof(mi_arena_pages_t) + (MI_BCHUNK_SIZE - 1) + mi_bitmap_size(slice_count, NULL);
  mi_arena_pages_t* arena_pages = (mi_arena_pages_t*)mi_heap_zalloc(arena->subproc->heap_main, size);
  if (arena_pages==NULL) return NULL;
  uint8_t* base = (uint8_t*)mi_align_up_ptr(arena_pages + 1, MI_BCHUNK_SIZE);
  mi_assert_internal(mi_is_aligned(base, MI_BCHUNK_SIZE));
  arena_pages->pages = mi_arena_bitmap_init(slice_count, &base);
  // `pages_abandoned[]` stays NULL (the allocation is zeroed) until a page of that bin is abandoned.
  return arena_pages;
}

// The abandoned-pages bitmap of `bin`, or NULL if no page of that bin was ever abandoned in
// this (heap, arena) pair. A NULL bitmap reads as all-clear.
static mi_bitmap_t* mi_arena_pages_abandoned(mi_arena_pages_t* arena_pages, size_t bin) {
  mi_assert_internal(bin < MI_ARENA_BIN_COUNT);
  return mi_atomic_load_ptr_acquire(mi_bitmap_t, &arena_pages->pages_abandoned[bin]);
}

// The abandoned-pages bitmap of `bin`, allocated on first use. Allocated from the subproc
// meta-data heap so this is safe on the abandon paths (a thread tearing down its theaps, a
// foreign free re-abandoning a page), where allocating from a regular heap is not. Publishing
// is a CAS so no lock is needed: a loser frees its copy and uses the winner's. Returns NULL only
// if the allocation failed.
#if MI_DEBUG > 0
mi_decl_export _Atomic(uintptr_t) mi_debug_abandoned_maps_allocated;   // test hook (test-abandoned-lazy): per-bin abandoned maps published so far
#endif

static mi_bitmap_t* mi_arena_pages_abandoned_ensure(mi_arena_t* arena, mi_arena_pages_t* arena_pages, size_t bin) {
  mi_bitmap_t* bitmap = mi_arena_pages_abandoned(arena_pages, bin);
  if mi_likely(bitmap != NULL) return bitmap;
  const size_t slice_count = arena->slice_count;
  const size_t size = mi_bitmap_size(slice_count, NULL);
  mi_bitmap_t* fresh = (mi_bitmap_t*)_mi_meta_zalloc_aligned(arena->subproc, size, MI_BCHUNK_SIZE, NULL);
  if (fresh == NULL) return NULL;
  mi_bitmap_init(fresh, slice_count, true /* already zero */);
  mi_bitmap_t* expected = NULL;
  if (mi_atomic_cas_ptr_strong_acq_rel(mi_bitmap_t, &arena_pages->pages_abandoned[bin], &expected, fresh)) {
    #if MI_DEBUG > 0
    mi_atomic_increment_relaxed(&mi_debug_abandoned_maps_allocated);
    #endif
    return fresh;
  }
  // another thread published one first
  _mi_free_subproc_safe(fresh);
  mi_assert_internal(expected != NULL);
  return expected;
}

// Release the on-demand abandoned bitmaps of a heap's arena pages (the `pages` bitmap is part of
// the `arena_pages` allocation itself).
static void mi_arena_pages_free_abandoned(mi_arena_pages_t* arena_pages) {
  for (size_t bin = 0; bin < MI_ARENA_BIN_COUNT; bin++) {
    if (mi_atomic_load_ptr_relaxed(mi_bitmap_t, &arena_pages->pages_abandoned[bin]) == NULL) continue;  // the common case
    mi_bitmap_t* bitmap = mi_atomic_exchange_ptr_acq_rel(mi_bitmap_t, &arena_pages->pages_abandoned[bin], NULL);
    if (bitmap != NULL) { _mi_free_subproc_safe(bitmap); }
  }
}

void _mi_arena_pages_free(mi_arena_pages_t* arena_pages) {
  if (arena_pages == NULL) return;
  mi_arena_pages_free_abandoned(arena_pages);
  _mi_free_subproc_safe(arena_pages);
}

static mi_arena_t* mi_page_arena_pages(mi_page_t* page, size_t* slice_index, size_t* slice_count, mi_arena_pages_t** parena_pages) {
  // todo: maybe store the arena* directly in the page?
  mi_assert_internal(mi_page_is_owned(page));
  mi_arena_t* const arena = mi_arena_from_memid(page->memid, slice_index, slice_count);
  mi_assert_internal(arena != NULL);
  if (parena_pages != NULL) {
    mi_heap_t* heap = mi_page_heap(page);
    mi_arena_pages_t* const arena_pages = mi_heap_arena_pages(heap, arena);
    mi_assert_internal(arena_pages != NULL);
    *parena_pages = arena_pages;
  }
  return arena;
}

static mi_arena_pages_t* mi_heap_ensure_arena_pages(mi_heap_t* heap, mi_arena_t* arena) {
  mi_assert_internal(arena!=NULL);
  mi_assert_internal(heap!=NULL);
  mi_assert(arena->arena_idx < MI_MAX_ARENAS);
  mi_arena_pages_t* arena_pages = mi_heap_arena_pages(heap, arena);
  if mi_likely(arena_pages!=NULL) return arena_pages;
  const bool is_main = mi_is_heap_main(heap);
  if (is_main) {
    // the page info for the main heap is always allocated as part of an arena
    arena_pages = &arena->pages_main;  // can never fail
  }
  else {
    // always allocate the arena pages info from the main heap
    // todo: allocate into the current arena?
    arena_pages = mi_arena_pages_alloc(arena);
    if (arena_pages==NULL) return NULL;
  }
  mi_arena_pages_t* expected = NULL;
  if (mi_atomic_cas_ptr_strong_acq_rel(mi_arena_pages_t, &heap->arena_pages[arena->arena_idx], &expected, arena_pages)) return arena_pages;
  if (!is_main) { _mi_arena_pages_free(arena_pages); }  // lost the race
  mi_assert_internal(expected != NULL);
  return expected;
}


/* -----------------------------------------------------------
  Claim abandoned pages
----------------------------------------------------------- */

static bool mi_arena_try_claim_abandoned(size_t slice_index, mi_arena_t* arena, bool* keep_abandoned) {
  // found an abandoned page of the right size
  mi_page_t* const page  = mi_arena_page_at_slice(arena, slice_index);
  // can we claim ownership?
  if (!mi_page_claim_ownership(page)) {
    // there was a concurrent free that reclaims this page ..
    // we need to keep it in the abandoned map as the free will call `mi_arena_page_unabandon`,
    // and wait for readers (us!) to finish. This is why it is very important to set the abandoned
    // bit again (or otherwise the unabandon will never stop waiting).
    *keep_abandoned = true;
    return false;
  }
  else {
    // yes, we can reclaim it, keep the abandoned map entry clear
    *keep_abandoned = false;
    return true;
  }
}


typedef struct mi_arena_abandoned_visit_info_s {
  mi_theap_t* theap;
  size_t      slice_count;
  size_t      block_size;
  size_t      bin;
} mi_arena_abandoned_visit_info_t;

static bool mi_arena_try_find_abandoned_visit(mi_arena_t* arena, const void* arg, void** result) {
  const mi_arena_abandoned_visit_info_t* info = (const mi_arena_abandoned_visit_info_t*)arg;
  mi_assert_internal(result != NULL);
  mi_theap_t* const theap = info->theap;
  mi_heap_t* const heap = mi_theap_heap(theap);
  const size_t tseq = theap->tld->thread_seq;
  mi_arena_pages_t* const arena_pages = mi_heap_arena_pages(heap, arena);
  mi_bitmap_t* const bitmap = (arena_pages != NULL ? mi_arena_pages_abandoned(arena_pages, info->bin) : NULL);
  if (bitmap != NULL) {
    size_t slice_index;

    if (mi_bitmap_try_find_and_claim(bitmap, tseq, &slice_index, &mi_arena_try_claim_abandoned, arena)) {
      // found an abandoned page of the right size
      // and claimed ownership.
      mi_page_t* page = mi_arena_page_at_slice(arena, slice_index);
      mi_assert_internal(mi_page_is_owned(page));
      mi_assert_internal(mi_page_is_abandoned(page));
      mi_assert_internal(mi_heap_has_page(heap, arena, page));
      mi_atomic_decrement_relaxed(&heap->abandoned_count[info->bin]);
      mi_theap_stat_decrease(theap, pages_abandoned, 1);
      mi_theap_stat_counter_increase(theap, pages_reclaim_on_alloc, 1);

      _mi_page_free_collect(page, false);  // update `used` count
      mi_assert_internal(mi_bbitmap_is_clearN(arena->slices_free, slice_index, info->slice_count));
      mi_assert_internal(mi_page_slice_committed(page) > 0 || mi_bitmap_is_setN(arena->slices_committed, slice_index, info->slice_count));
      mi_assert_internal(mi_bitmap_is_setN(arena->slices_dirty, slice_index, info->slice_count));
      mi_assert_internal(mi_is_aligned(mi_page_slice_start(page), MI_PAGE_ALIGN));
      mi_assert_internal(mi_ptr_page(mi_page_start(page))==page);
      mi_assert_internal(mi_page_block_size(page) == info->block_size);
      mi_assert_internal(!mi_page_is_full(page));
      *result = page;
      return false; // break
    }
  }
  return true; // continue
}

static mi_page_t* mi_arenas_page_try_find_abandoned(mi_theap_t* theap, size_t slice_count, size_t block_size)
{
  mi_heap_t* const heap = mi_theap_heap(theap);
  const size_t tseq = theap->tld->thread_seq;
  mi_arena_t* const req_arena = heap->exclusive_arena;

  const size_t bin = _mi_bin(block_size);
  if (bin >= MI_ARENA_BIN_COUNT) {
    return NULL; // singleton page size
  }

  // any abandoned in our size class?
  mi_assert_internal(heap != NULL);
  if (mi_atomic_load_relaxed(&heap->abandoned_count[bin]) == 0) {
    return NULL;
  }

  // search arena's
  const bool allow_large = true;
  const int  any_numa = -1;
  const bool match_numa = true;
  const mi_arena_abandoned_visit_info_t info = { theap, slice_count, block_size, bin };
  void* page = NULL;
  mi_forall_suitable_arenas(heap, req_arena, tseq, match_numa, any_numa, allow_large, &mi_arena_try_find_abandoned_visit, &info, &page);
  return (mi_page_t*)page;
}


/* -----------------------------------------------------------
  Allocate the memory area and page meta data for a fresh page
  (`mi_arenas_page_alloc_fresh_area`)
----------------------------------------------------------- */

// Offset of the first block when the page meta data is in front of the slices.
static size_t mi_page_meta_in_front_block_start(size_t block_size, bool os_align) {
  size_t offset;
  #if MI_GUARDED
  // in a guarded build, we align pages with blocks a multiple of an OS page size, to the OS page size
  // this ensures that all blocks in such pages are OS page size aligned (which is needed for the guard pages)
  const size_t os_page_size = _mi_os_page_size();
  mi_assert_internal(MI_PAGE_ALIGN >= os_page_size);
  if (!os_align && block_size % os_page_size == 0 && block_size > os_page_size /* at least 2 or more */ ) {
    const bool natural = (mi_is_power_of_two(block_size) && block_size <= MI_PAGE_MAX_START_BLOCK_ALIGN2);
    offset = mi_align_up(mi_page_info_size(), (natural ? block_size : os_page_size));
  }
  else
  #endif
  if (os_align) {
    offset = MI_PAGE_ALIGN;
  }
  else if (block_size != 0 && mi_is_power_of_two(block_size) && block_size <= MI_PAGE_MAX_START_BLOCK_ALIGN2) {
    // naturally align power-of-2 blocks up to MI_PAGE_MAX_START_BLOCK_ALIGN2 size
    offset = mi_align_up(mi_page_info_size(), block_size);
  }
  else if (block_size != 0 && block_size <= MI_SMALL_SIZE_MAX) {
    // align small blocks to their size
    offset = mi_align_up(mi_page_info_size(), block_size);
  }
  else if (block_size != 0 && (block_size % MI_PAGE_OSPAGE_BLOCK_ALIGN2) == 0) {
    // also align large pages that are a multiple of MI_PAGE_OSPAGE_BLOCK_ALIGN2 (4KiB)
    offset = mi_align_up(mi_page_info_size(), MI_PAGE_OSPAGE_BLOCK_ALIGN2);
  }
  else {
    // otherwise start after the info
    offset = mi_page_info_size();
  }
  return mi_align_up(offset,MI_MAX_ALIGN_SIZE);
}

// The usable size of a page area (excluding the guard page at the end in secure mode).
static size_t mi_arenas_page_noguard_size(size_t alloc_size) {
  #if MI_SECURE>=5
  mi_assert(alloc_size > _mi_os_secure_guard_page_size());
  return alloc_size - _mi_os_secure_guard_page_size();
  #else
  return alloc_size;
  #endif
}

// Can we offset the first block to spread cache sets? (see `MI_PAGE_CACHE_OFFSET`)
static bool mi_page_block_can_cache_offset(size_t block_size, bool os_align) {
  if (os_align || !mi_block_size_has_cache_offset(block_size)) return false;
  if (mi_natural_alignment_of_size(block_size) > MI_PAGE_CACHE_OFFSET_UNIT) return false;  // power-of-2 blocks up to MI_PAGE_MAX_START_BLOCK_ALIGN2 stay naturally aligned
  #if MI_GUARDED
  if (block_size % _mi_os_page_size() == 0) return false;  // guarded blocks must stay OS page aligned
  #endif
  return true;
}

static size_t mi_page_cache_offset_index(uint8_t* slice_start, size_t slice_count, size_t noffsets) {
  // Fibonacci hash of the page index: the top bits spread consecutive pages evenly over the offsets
  const size_t page_index = (size_t)((uintptr_t)slice_start / MI_ARENA_SLICE_SIZE) / slice_count;
  #if MI_SIZE_BITS >= 64
  const size_t hash = page_index * MI_ZU(0x9E3779B97F4A7C15);
  #else
  const size_t hash = page_index * MI_ZU(0x9E3779B9);
  #endif
  return (((hash >> (MI_SIZE_BITS - 16)) * noffsets) >> 16);
}

static size_t mi_page_small_block_cache_offset(uint8_t* slice_start, size_t slice_count, size_t block_size) {
  const size_t unit = mi_max(MI_PAGE_SMALL_CACHE_OFFSET_UNIT, mi_natural_alignment_of_size(block_size));
  if (unit > MI_PAGE_SMALL_CACHE_OFFSET_MAX) return 0;
  return mi_page_cache_offset_index(slice_start, slice_count, 1 + MI_PAGE_SMALL_CACHE_OFFSET_MAX/unit) * unit;
}

// Cache offset for the first block to spread the blocks of different pages over the cache sets (issue #1121).
// The offset fits in the unused space at the end of the page; if there is none, we skip the last block
// if that wastes less than 12.5% of the page (e.g. 64 KiB blocks in a 4 MiB page, but not 512 KiB blocks).
static size_t mi_page_block_cache_offset(uint8_t* slice_start, size_t slice_count, size_t block_size, size_t block_start, bool os_align) {
  if (MI_PAGE_CACHE_OFFSET == 0 || os_align) return 0;
  if (!mi_block_size_has_cache_offset(block_size)) return mi_page_small_block_cache_offset(slice_start, slice_count, block_size);
  if (!mi_page_block_can_cache_offset(block_size, os_align)) return 0;
  const size_t page_noguard_size = mi_arenas_page_noguard_size(_mi_size_of_slices(slice_count));
  mi_assert_internal(block_start + block_size <= page_noguard_size);
  size_t slack = (page_noguard_size - block_start) % block_size;
  if (slack < MI_PAGE_CACHE_OFFSET_UNIT && block_size < page_noguard_size/8) { slack += block_size; }
  const size_t noffsets = 1 + (slack < MI_PAGE_CACHE_OFFSET_MAX ? slack : MI_PAGE_CACHE_OFFSET_MAX) / MI_PAGE_CACHE_OFFSET_UNIT;
  return mi_page_cache_offset_index(slice_start, slice_count, noffsets) * MI_PAGE_CACHE_OFFSET_UNIT;
}

// Use the separate (zero initialized) `page_meta` if available and not preferring in front of the slices,
// and otherwise put the page meta data in front of the slices. Also determines the offset of the first block.
static mi_page_t* mi_page_meta_select(uint8_t* slice_start, size_t slice_count, mi_page_t* page_meta, size_t block_size, bool os_align, size_t* block_start) {
  #if MI_PAGE_META_IS_ALIGNED || MI_PAGE_META_SMALL_IS_ALIGNED
  // Only pre-zeroed in these configurations; see the `pages_meta` note in
  // `mi_arena_initialize`. Otherwise an unclaimed entry may hold anything
  // (external memory we were not told was zero) until the memzero below.
  mi_assert_internal(page_meta == NULL || page_meta->block_size == 0);
  #endif
  mi_page_t* page;
  size_t offset;
  if (page_meta == NULL
      #if MI_PAGE_META_SMALL_IS_ALIGNED
      // small pages keep the page meta data in front of the slices for a fast `mi_free_small`
      // (note: it is important that the separate `page_meta->block_size == 0` for `mi_arena_page_at_slice`)
      || (!os_align && block_size <= MI_SMALL_MAX_OBJ_SIZE)
      #endif
  ) {
    // put the actual page meta info in front of the slices
    // (note: it is not initialized yet since the slices may not be committed yet)
    offset = mi_page_meta_in_front_block_start(block_size, os_align);
    page = (mi_page_t*)slice_start;
  }
  else {
    // use the separate page meta data
    mi_memzero_aligned(page_meta, sizeof(*page_meta));
    offset = 0;
    page = page_meta;
  }
  offset += mi_page_block_cache_offset(slice_start, slice_count, block_size, offset, os_align);
  mi_assert_internal(os_align || (uint8_t*)page == slice_start || offset < MI_ARENA_SLICE_SIZE);  // so `mi_page_slice_start` can align down the page start
  *block_start = offset;
  return page;
}

#if MI_PAGE_META_IS_ALIGNED
// Ensure the aligned page meta data for the slices at `slice_start` is committed.
static bool mi_arena_page_meta_ensure_committed(mi_arena_t* arena, uint8_t* slice_start) {
  uint8_t* const meta_slices = (uint8_t*)mi_align_down_ptr(slice_start,MI_PAGE_META_ALIGNMENT);
  mi_assert_internal(meta_slices >= mi_arena_start(arena));
  const size_t meta_slice_index  = (meta_slices - mi_arena_start(arena)) / MI_ARENA_SLICE_SIZE;
  if mi_unlikely(mi_bitmap_is_clear(arena->slices_committed, meta_slice_index)) {
    // try to commit all page meta slices now
    const size_t meta_slice_count = mi_arena_page_meta_aligned_slice_count();
    // the following assertion does not hold in a concurrent setting..
    // mi_assert_internal(mi_bitmap_is_clearN(arena->slices_committed, meta_slice_index, meta_slice_count));
    const size_t commit_size = meta_slice_count * MI_ARENA_SLICE_SIZE;
    if (!_mi_arena_commit(arena->subproc, arena, meta_slices, commit_size, NULL, 0)) {
      return false;
    }
    mi_bitmap_setN(arena->slices_committed, meta_slice_index, meta_slice_count, NULL);
  }
  return true;
}
#endif

// Try to allocate slices from free space in the arena's, and determine the page meta data (`*ppage`) and the offset of the first block (`*pblock_start`).
static uint8_t* mi_arenas_page_alloc_from_slices(mi_theap_t* theap, size_t slice_count, size_t block_size, bool commit, bool allow_large,
                                                 mi_memid_t* memid, mi_arena_pages_t** parena_pages, mi_page_t** ppage, size_t* pblock_start) {
  mi_heap_t* const heap = mi_theap_heap(theap);
  mi_tld_t*  const tld  = theap->tld;
  const int numa_node = (heap->numa_node >= 0 ? heap->numa_node : tld->numa_node);

  uint8_t* const start = (uint8_t*)_mi_arenas_try_alloc(heap, slice_count, MI_ARENA_SLICE_ALIGN, commit, allow_large, heap->exclusive_arena, tld->thread_seq, numa_node, memid);
  if (start == NULL) return NULL;
  mi_arena_t* const arena = memid->mem.arena.arena;

  // ensure the arena pages (and the aligned page meta data)
  mi_arena_pages_t* const arena_pages = mi_heap_ensure_arena_pages(heap, arena);
  if (arena_pages==NULL
      #if MI_PAGE_META_IS_ALIGNED
      || !mi_arena_page_meta_ensure_committed(arena, start)
      #endif
     ) {
    _mi_arenas_free(heap->subproc, start, _mi_size_of_slices(slice_count), *memid); // roll back
    return NULL;
  }
  // note: the following assert should hold if we could check it atomically, but in a concurrent setting we may already allocate in slice_count
  // mi_assert_internal(mi_bitmap_is_clearN(arena_pages->pages, memid->mem.arena.slice_index, memid->mem.arena.slice_count));
  mi_assert_internal(mi_bitmap_is_clear(arena_pages->pages, memid->mem.arena.slice_index));
  // don't set yet: mi_bitmap_set(arena_pages->pages, memid->mem.arena.slice_index);
  *parena_pages = arena_pages;

  // the separate page meta data (if available)
  #if MI_PAGE_META_IS_ALIGNED
  mi_page_t* const page_meta = mi_aligned_ptr_page0(start);
  #else
  mi_assert_internal(MI_PAGE_META_IS_SEPARATED!=0 || arena->pages_meta == NULL);
  mi_page_t* const page_meta = (arena->pages_meta == NULL ? NULL : &arena->pages_meta[memid->mem.arena.slice_index]);
  #endif
  *ppage = mi_page_meta_select(start, slice_count, page_meta, block_size, false, pblock_start);
  return start;
}

#if MI_PAGE_META_IS_ALIGNED
// Allocate slices from the OS with the (committed and zero'd) page meta data at the start of the `MI_PAGE_META_ALIGNMENT` aligned area,
// and determine the page meta data (`*ppage`) and the offset of the first block (`*pblock_start`).
static uint8_t* mi_arenas_page_alloc_from_os(mi_heap_t* heap, size_t slice_count, size_t max_page_meta_count, size_t block_size, size_t block_alignment, bool os_align, bool commit, bool allow_large,
                                             mi_memid_t* memid, mi_page_t** ppage, size_t* pblock_start) {
  MI_UNUSED(allow_large);
  mi_arena_t* const req_arena = heap->exclusive_arena;
  const size_t alloc_size = _mi_size_of_slices(slice_count);
  if (block_alignment >= MI_PAGE_META_ALIGNMENT) {
    // if we allow alignment >= MI_PAGE_META_ALIGNMENT we need to substract 1 from a pointer
    // in mi_aligned_ptr_page (and test for (intptr_t)p < 0 instead of NULL). We avoid this by limiting the max alignment.
    _mi_warning_message("requested alignment is too large (%zu KiB)\n", block_alignment / MI_KiB);
    errno = EINVAL;
    return NULL;
    // page_offset = MI_PAGE_META_ALIGNMENT;
    // os_start = (uint8_t*)_mi_arena_os_alloc_aligned(heap->subproc, alloc_size + page_offset, block_alignment, MI_PAGE_META_ALIGNMENT /* align offset */, false /* commit */, false /* allow large */, req_arena, memid);
  }

  // offset in the block for the page area
  const size_t page_offset = (block_alignment < MI_PAGE_ALIGN ? MI_PAGE_ALIGN : block_alignment);
  uint8_t* const os_start = (uint8_t*)_mi_arena_os_alloc_aligned(heap->subproc, alloc_size + page_offset, MI_PAGE_META_ALIGNMENT, 0 /* align offset */, false /* commit */, false /* allow large */, req_arena, memid);
  if (os_start==NULL) return NULL;

  // commit page info and page
  const size_t min_page_count = mi_divide_up(page_offset + MI_ARENA_SLICE_SIZE,MI_ARENA_SLICE_SIZE) + max_page_meta_count;
  const size_t min_page_meta  = min_page_count * sizeof(mi_page_t);
  mi_assert_internal(min_page_meta < page_offset);
  bool is_zero;
  bool ok = _mi_arena_commit(heap->subproc,req_arena,os_start,min_page_meta,&is_zero,min_page_meta /* don't count in stats? */);
  if (ok && commit) {
    ok = _mi_arena_commit(heap->subproc,req_arena,os_start + page_offset,alloc_size,NULL,0);
  }
  if (!ok) { _mi_os_free(heap->subproc,os_start,alloc_size+page_offset,*memid); return NULL; }
  if (!is_zero && !memid->initially_zero) {
    mi_memzero_aligned(os_start,min_page_meta);
  }
  uint8_t* const start = os_start + page_offset;
  mi_assert_internal(mi_is_aligned(start,block_alignment));
  mi_assert_internal(mi_is_aligned(os_start,MI_PAGE_META_ALIGNMENT));
  mi_assert_internal(mi_align_down_ptr(start-1,MI_PAGE_META_ALIGNMENT) == os_start);
  mi_assert_internal((uint8_t*)mi_aligned_ptr_page0(start) < os_start + min_page_meta);
  memid->initially_committed = true; // so we don't commit again
  *ppage = mi_page_meta_select(start, slice_count, mi_aligned_ptr_page0(start), block_size, os_align, pblock_start);
  return start;
}
#else
// Allocate slices from the OS with the page meta data in front of the slices,
// and determine the page meta data (`*ppage`) and the offset of the first block (`*pblock_start`).
static uint8_t* mi_arenas_page_alloc_from_os(mi_heap_t* heap, size_t slice_count, size_t max_page_meta_count, size_t block_size, size_t block_alignment, bool os_align, bool commit, bool allow_large,
                                             mi_memid_t* memid, mi_page_t** ppage, size_t* pblock_start) {
  MI_UNUSED(max_page_meta_count);
  const size_t page_alignment = MI_ARENA_SLICE_ALIGN;
  const size_t alloc_size = _mi_size_of_slices(slice_count);
  uint8_t* start;
  if (os_align) {
    // note: slice_count already includes the page meta data (of MI_PAGE_ALIGN size) in front of the aligned block
    start = (uint8_t*)_mi_arena_os_alloc_aligned(heap->subproc, alloc_size, block_alignment, page_alignment /* align offset */, commit, allow_large, heap->exclusive_arena, memid);
    mi_assert_internal(start==NULL || mi_is_aligned(start + page_alignment, block_alignment));
  }
  else {
    start = (uint8_t*)_mi_arena_os_alloc_aligned(heap->subproc, alloc_size, page_alignment, 0 /* align offset */, commit, allow_large, heap->exclusive_arena, memid);
  }
  if (start == NULL) return NULL;
  // page meta data is always placed in front of the slices
  *ppage = mi_page_meta_select(start, slice_count, NULL, block_size, os_align, pblock_start);
  return start;
}
#endif

// Allocate the slices for a fresh page and determine its page meta data (`*ppage`) and the offset of the first block (`*pblock_start`).
static uint8_t* mi_arenas_page_alloc_fresh_area(mi_theap_t* theap, size_t slice_count, size_t max_page_meta_count, size_t block_size, size_t block_alignment, bool os_align, bool commit,
                                                mi_memid_t* memid, mi_arena_pages_t** parena_pages, mi_page_t** ppage, size_t* pblock_start ) {
  mi_assert_internal(parena_pages!=NULL && ppage!=NULL && pblock_start!=NULL);
  *parena_pages = NULL;
  *memid = _mi_memid_none();
  const bool allow_large = (MI_SECURE < 5); // 5 = guard page at end of each arena page
  mi_heap_t* const heap = mi_theap_heap(theap);

  // try to allocate from free space in arena's
  uint8_t* slice_start = NULL;
  if (!mi_option_is_enabled(mi_option_disallow_arena_alloc) &&       // allowed to allocate from arena's?
      !os_align &&                                                   // not large alignment
      slice_count <= mi_arena_max_object_size()/MI_ARENA_SLICE_SIZE) // and not too large
  {
    slice_start = mi_arenas_page_alloc_from_slices(theap, slice_count, block_size, commit, allow_large, memid, parena_pages, ppage, pblock_start);
  }

  // otherwise fall back to the OS
  if (slice_start == NULL) {
    slice_start = mi_arenas_page_alloc_from_os(heap, slice_count, max_page_meta_count, block_size, block_alignment, os_align, commit, allow_large, memid, ppage, pblock_start);
    if (slice_start == NULL) return NULL;
    mi_atomic_store_release(&heap->has_os_pages, (uintptr_t)1);
    mi_heap_stat_increase(heap,pages_os_allocated,1);
  }
  mi_assert_internal(mi_is_aligned(slice_start, MI_PAGE_ALIGN));
  return slice_start;
}


/* -----------------------------------------------------------
  Allocate and initialize a fresh page (`mi_arenas_page_alloc_fresh`)
----------------------------------------------------------- */

// Free a page without modifying page_bin stats
static void mi_arenas_page_free_prim(mi_page_t* page, mi_subproc_t* subproc, mi_arena_pages_t* arena_pages);

// Commit the start of the page up to and including the first block if the slices are not yet committed.
// Sets `*commit_size` to the committed size (or 0 if the slices were already committed).
static bool mi_arenas_page_commit_first_block(mi_theap_t* theap, mi_memid_t memid, uint8_t* slice_start, size_t block_start, size_t block_size, size_t page_noguard_size, size_t* commit_size) {
  *commit_size = 0;
  if (memid.initially_committed) return true;
  size_t csize = mi_align_up(block_start + block_size, mi_page_min_commit_size());
  if (csize > page_noguard_size) { csize = page_noguard_size; }
  bool is_zero = false;
  if mi_unlikely(!_mi_arena_commit(mi_theap_subproc(theap), mi_memid_arena(memid), slice_start, csize, &is_zero, 0)) {
    return false;
  }
  *commit_size = csize;
  return true;
}

// Prepare the page memory: zero the page meta data if it is in front of the slices, set the guard page, etc.
static void mi_arenas_page_init_memory(mi_theap_t* theap, mi_page_t* page, uint8_t* slice_start, size_t slice_count, size_t block_start, size_t page_noguard_size, mi_memid_t* memid)
{
  MI_UNUSED(theap); MI_UNUSED(slice_count); MI_UNUSED(block_start); MI_UNUSED(page_noguard_size);

  // zero initialize the page meta data
  if (!memid->initially_zero && (uint8_t*)page == slice_start) {
    mi_memzero_aligned(page, sizeof(*page));
  }

  // set the guard page
  #if MI_SECURE>=5
  if (memid->initially_committed) {
    _mi_os_secure_guard_page_set_at(mi_theap_subproc(theap), slice_start + page_noguard_size, *memid);
  }
  #endif

  // claimed free slices: initialize the page partly
  if (!memid->initially_zero && memid->initially_committed) {
    mi_track_mem_undefined(slice_start, slice_count * MI_ARENA_SLICE_SIZE);
  }
  else if (memid->initially_committed) {
    mi_track_mem_defined(slice_start, slice_count * MI_ARENA_SLICE_SIZE);
  }
  #if MI_DEBUG > 1
  if (memid->initially_zero && memid->initially_committed) {
    if (!mi_mem_is_zero(slice_start, page_noguard_size)) {
      _mi_error_message(EFAULT, "internal error: page memory was not zero initialized.\n");
      memid->initially_zero = false;
      if (block_start > 0) { mi_memzero_aligned(page, sizeof(*page)); }
    }
  }
  #endif
}

// Initialize the fields of the page meta data.
static void mi_arenas_page_init_meta(mi_theap_t* theap, mi_page_t* page, uint8_t* slice_start, size_t block_start, size_t block_size, bool os_align,
                                     size_t page_noguard_size, size_t commit_size, mi_memid_t memid)
{
  const size_t reserved = (os_align ? 1 : (page_noguard_size - block_start) / block_size);
  mi_assert_internal(reserved > 0 && reserved <= UINT16_MAX);

  // initialize the page start
  uint8_t* const start = slice_start + block_start;
  mi_assert_internal(start > (uint8_t*)page);
  page->page_offset = start - (uint8_t*)page;

  // initialize page meta-data
  page->reserved = (uint16_t)reserved;
  page->block_size = block_size;
  page->memid = memid;
  page->free_is_zero = memid.initially_zero;
  page->slice_pcommitted = (uint16_t)(commit_size / _mi_os_page_size());

  page->heap = mi_theap_heap(theap);
  mi_page_set_theap(page,theap);
  // mi_assert_internal(mi_page_theap(page) == _mi_heap_theap_peek(page->heap))
}

#if MI_PAGE_META_IS_ALIGNED
// Point the aligned page meta entries of the slices to the actual page meta data.
static void mi_arenas_page_link_aligned_meta(mi_page_t* page, uint8_t* slice_start, size_t slice_count, size_t max_page_meta_count)
{
  mi_page_t* const page_meta = mi_aligned_ptr_page0(slice_start);
  mi_atomic_store_ptr_release(mi_page_t,&page_meta->self,page);
  if (slice_count > 1) {
    // at least two for large singleton blocks as guard pages can have a large offset beyond a single slice
    for(size_t i = 1; i < max_page_meta_count; i++) {
      mi_assert_internal(page_meta[i].block_size == 0);
      mi_atomic_store_ptr_release(mi_page_t,&page_meta[i].self,page);
    }
  }
  #if MI_DEBUG>1
  mi_page_t* pstart = mi_aligned_ptr_page0(slice_start);
  mi_assert_internal(mi_atomic_load_ptr_acquire(mi_page_t,&pstart->self)==page);
  if (page->reserved>1) {
    mi_page_t* pend = mi_aligned_ptr_page0(slice_start + (slice_count*MI_ARENA_SLICE_SIZE) - 1);
    mi_assert_internal(mi_atomic_load_ptr_acquire(mi_page_t,&pend->self)==page);
  }
  #endif
}
#endif

// Register a fresh page in its arena pages and the page map, and claim ownership.
// On failure the page is freed.
static bool mi_arenas_page_register(mi_theap_t* theap, mi_page_t* page, mi_arena_pages_t* arena_pages)
{
  // own it
  mi_page_claim_ownership(page);

  // and publish it in the heap's arena_pages
  if (arena_pages!=NULL) {
    mi_assert_internal(page->memid.memkind == MI_MEM_ARENA);
    mi_bitmap_set(arena_pages->pages, page->memid.mem.arena.slice_index);
  }

  // register in the page map
  if mi_unlikely(!_mi_page_map_register(page)) {
    if (arena_pages==NULL) { mi_heap_stat_decrease(page->heap, pages_os_allocated, 1); }
    mi_arenas_page_free_prim(page, mi_theap_subproc(theap), arena_pages);
    return false;
  }

  // stats
  mi_theap_stat_increase(theap, pages, 1);
  mi_theap_stat_increase(theap, page_bins[_mi_page_stats_bin(page)], 1);
  return true;
}

// Allocate a fresh page
static mi_page_t* mi_arenas_page_alloc_fresh(mi_theap_t* theap, size_t slice_count, size_t block_size, size_t block_alignment, bool commit)
{
  const bool os_align           = (block_alignment > MI_PAGE_MAX_OVERALLOC_ALIGN);
  const bool singleton          = (os_align || block_size > MI_LARGE_MAX_OBJ_SIZE);
  const size_t max_page_meta_count = (singleton && slice_count > 2 ? 2 : slice_count);

  // allocate the slices and determine the page meta data
  mi_memid_t memid              = _mi_memid_none();
  mi_arena_pages_t* arena_pages = NULL;
  mi_page_t* page               = NULL;
  size_t block_start            = 0;
  uint8_t* const slice_start    = mi_arenas_page_alloc_fresh_area(theap,slice_count,max_page_meta_count,block_size,block_alignment,os_align,commit,&memid,&arena_pages,&page,&block_start);
  if (!slice_start) return NULL;
  mi_assert_internal(block_size < MI_MAX_ALIGN_SIZE || block_start % MI_MAX_ALIGN_SIZE == 0);
  // all blocks must be naturally aligned (as relied upon by `mi_malloc_aligned`)
  mi_assert_internal(block_size % mi_natural_alignment_of_size(block_size) == 0);
  mi_assert_internal(mi_is_aligned(slice_start + block_start, mi_natural_alignment_of_size(block_size)));

  // commit the first block (if needed)
  const size_t alloc_size        = _mi_size_of_slices(slice_count);
  const size_t page_noguard_size = mi_arenas_page_noguard_size(alloc_size);
  size_t commit_size = 0;
  if (!mi_arenas_page_commit_first_block(theap, memid, slice_start, block_start, block_size, page_noguard_size, &commit_size)) {
    _mi_arenas_free(mi_theap_subproc(theap), slice_start, alloc_size, memid);
    return NULL;
  }
  mi_assert_internal((commit && commit_size==0) || (!commit && (commit_size <= UINT16_MAX * _mi_os_page_size())));

  // initialize the page memory and meta data
  mi_arenas_page_init_memory(theap, page, slice_start, slice_count, block_start, page_noguard_size, &memid);
  mi_arenas_page_init_meta(theap, page, slice_start, block_start, block_size, os_align, page_noguard_size, commit_size, memid);
  #if MI_PAGE_META_IS_ALIGNED
  mi_arenas_page_link_aligned_meta(page, slice_start, slice_count, max_page_meta_count);
  #endif
  mi_assert_internal(page->free==NULL);
  mi_assert_internal(((uint8_t*)page != slice_start) == mi_page_meta_is_separated(page));
  mi_assert_internal(mi_page_slice_start(page) == slice_start);
  mi_assert_internal(mi_page_size(page) <= page_noguard_size);

  // now we can register the page (and use `mi_arenas_page_free_prim` on error)
  if (!mi_arenas_page_register(theap, page, arena_pages)) return NULL;

  mi_assert_internal(mi_is_aligned(mi_page_slice_start(page),MI_PAGE_ALIGN));
  mi_assert_internal(mi_ptr_page(mi_page_start(page))==page);
  mi_assert_internal(mi_page_block_size(page) == block_size);
  #if MI_PAGE_META_SMALL_IS_ALIGNED
  // `mi_free_small` finds the page meta data by aligning a block pointer down to `MI_SMALL_PAGE_SIZE`
  if (mi_page_is_small(page)) {
    mi_assert_internal((uint8_t*)page == slice_start && mi_is_aligned(page, MI_SMALL_PAGE_SIZE));
    mi_assert_internal(mi_page_start(page) + mi_page_size(page) <= (uint8_t*)page + MI_SMALL_PAGE_SIZE);
  }
  #endif
  // mi_assert_internal(mi_page_is_abandoned(page));
  mi_assert_internal(mi_page_is_owned(page));
  return page;
}



/* -----------------------------------------------------------
  Allocate page (`_mi_arenas_page_alloc`)
----------------------------------------------------------- */

// Allocate a regular small/medium/large page.
static mi_page_t* mi_arenas_page_regular_alloc(mi_theap_t* theap, size_t slice_count, size_t block_size)
{
  // 1. look for an abandoned page
  mi_page_t* page = mi_arenas_page_try_find_abandoned(theap, slice_count, block_size);
  if (page != NULL) {
    return page;  // return as abandoned
  }

  // 2. find a free block, potentially allocating a new arena
  const long commit_on_demand = mi_option_get(mi_option_page_commit_on_demand);
  const bool commit = (mi_page_min_commit_size() >= slice_count * MI_ARENA_SLICE_SIZE ||           // always commit small pages
                       (slice_count >= _mi_slice_count_of_size(UINT16_MAX * _mi_os_page_size())) ||   // always commit pages too large to hold a 32-bit slice_committed
                        (commit_on_demand == 2 && _mi_os_has_overcommit()) || (commit_on_demand == 0));
  page = mi_arenas_page_alloc_fresh(theap, slice_count, block_size, 1, commit);
  if (page == NULL) return NULL;

  mi_assert_internal(page->memid.memkind != MI_MEM_ARENA || page->memid.mem.arena.slice_count == slice_count);
  if (!_mi_page_init(theap, page)) {
    _mi_arenas_page_free(page,theap);
    return NULL;
  }

  return page;
}

// Allocate a page containing one block (very large, or with large alignment)
static mi_page_t* mi_arenas_page_singleton_alloc(mi_theap_t* theap, size_t block_size, size_t block_alignment)
{
  const bool os_align = (block_alignment > MI_PAGE_MAX_OVERALLOC_ALIGN);
  #if MI_PAGE_META_IS_ALIGNED
  const size_t info_size = 0;
  #else
  const size_t info_size = (os_align ? MI_PAGE_ALIGN : mi_page_info_size());
  #endif
  // room for the cache offset of the block (committed but not touched)
  const size_t cache_offset_size = (mi_page_block_can_cache_offset(block_size, os_align) ? MI_PAGE_CACHE_OFFSET_MAX : 0);
  #if MI_SECURE < 2
  const size_t slice_count = _mi_slice_count_of_size(info_size + cache_offset_size + block_size);
  #else
  const size_t slice_count = _mi_slice_count_of_size(mi_align_up(info_size + cache_offset_size + block_size, _mi_os_secure_guard_page_size()) + _mi_os_secure_guard_page_size());
  #endif

  mi_page_t* page = mi_arenas_page_alloc_fresh(theap, slice_count, block_size, block_alignment, true /* commit singletons always */);
  if (page == NULL) return NULL;

  mi_assert(page->reserved == 1);
  if (!_mi_page_init(theap, page)) {
    _mi_arenas_page_free(page,theap);
    return NULL;
  }

  return page;
}


mi_page_t* _mi_arenas_page_alloc(mi_theap_t* theap, size_t block_size, size_t block_alignment) {
  mi_page_t* page;
  // semi static assert: ensure that all non-singleton block size bins are covered.
  mi_assert(_mi_bin(MI_LARGE_MAX_OBJ_SIZE)  < MI_ARENA_BIN_COUNT);
  if mi_unlikely(block_alignment > MI_PAGE_MAX_OVERALLOC_ALIGN) {
    mi_assert_internal(mi_is_power_of_two(block_alignment));
    page = mi_arenas_page_singleton_alloc(theap, block_size, block_alignment);
  }
  else if (block_size <= MI_SMALL_MAX_OBJ_SIZE) {
    page = mi_arenas_page_regular_alloc(theap, _mi_slice_count_of_size(MI_SMALL_PAGE_SIZE), block_size);
  }
  else if (mi_block_size_is_medium(block_size)) {
    page = mi_arenas_page_regular_alloc(theap, _mi_slice_count_of_size(MI_MEDIUM_PAGE_SIZE), block_size);
  }
  #if MI_ENABLE_LARGE_PAGES
  else if (block_size <= MI_LARGE_MAX_OBJ_SIZE) {
    page = mi_arenas_page_regular_alloc(theap, _mi_slice_count_of_size(MI_LARGE_PAGE_SIZE), block_size);
  }
  #endif
  else {
    page = mi_arenas_page_singleton_alloc(theap, block_size, block_alignment);
  }
  if mi_unlikely(page == NULL) {
    return NULL;
  }
  // mi_assert_internal(page == NULL || _mi_page_segment(page)->subproc == tld->subproc);
  mi_assert_internal(mi_is_aligned(mi_page_slice_start(page), MI_PAGE_ALIGN));
  mi_assert_internal(mi_ptr_page(mi_page_start(page))==page);
  mi_assert_internal(block_alignment <= MI_PAGE_MAX_OVERALLOC_ALIGN || mi_is_aligned(mi_page_start(page), block_alignment));

  return page;
}


/* -----------------------------------------------------------
  Free a page
----------------------------------------------------------- */

// Free a page to the arena or OS. `subproc` and `arena_pages` (NULL for an OS page) were resolved from the
// page's heap by the caller: this does not read `page->heap` anymore, as the clear of the `arena_pages->pages`
// bit below (or, for an OS page, the unlink from `heap->os_abandoned_pages` the caller did already) is what a
// concurrent `mi_heap_delete` waits for before it frees the heap (see `mi_heap_visit_page_claim`).
static void mi_arenas_page_free_prim(mi_page_t* page, mi_subproc_t* subproc, mi_arena_pages_t* arena_pages) {
  mi_assert_internal(_mi_page_qsbr_is_clear(page));
  mi_assert_internal(mi_is_aligned(mi_page_slice_start(page), MI_PAGE_ALIGN));
  mi_assert_internal(_mi_safe_ptr_page(mi_page_start(page))==page || _mi_safe_ptr_page(mi_page_start(page))==NULL);  // (NULL: `_mi_page_map_register` failed in `mi_arenas_page_register`)
  mi_assert_internal(mi_page_is_owned(page));
  mi_assert_internal(mi_page_all_free(page));
  mi_assert_internal(page->next==NULL && page->prev==NULL);
  mi_assert_internal((page->memid.memkind == MI_MEM_ARENA) == (arena_pages != NULL));

  // recommit guard page at the end?
  // we must do this since we may later allocate large spans over this page and cannot have a guard page in between
  #if MI_SECURE >= 5
  if (!page->memid.is_pinned) {
    _mi_os_secure_guard_page_reset_before(subproc, mi_page_slice_start(page) + mi_page_full_size(page), page->memid);
  }
  #endif

  // unpublish from the heap's arena_pages bitmap before unregistering the page-map: visitors
  // (mi_heap_visit_page_at) walk this bitmap and call mi_ptr_page on the result, and a fork()
  // between page-map-unregister and bitmap-clear would freeze that inconsistency in the child.
  if (page->memid.memkind == MI_MEM_ARENA) {
    size_t slice_index;
    size_t slice_count;
    mi_arena_t* const arena = mi_arena_from_memid(page->memid, &slice_index, &slice_count);
    mi_assert_internal(arena->subproc == subproc);
    #if MI_DEBUG>1
    if (!mi_page_is_full(page)) {
      const size_t bin = _mi_bin(mi_page_block_size(page));
      mi_assert_internal(mi_bbitmap_is_clearN(arena->slices_free, slice_index, slice_count));
      mi_assert_internal(mi_page_slice_committed(page) > 0 || mi_bitmap_is_setN(arena->slices_committed, slice_index, slice_count));
      mi_assert_internal(bin >= MI_ARENA_BIN_COUNT || mi_arena_pages_abandoned(arena_pages, bin) == NULL || mi_bitmap_is_clearN(mi_arena_pages_abandoned(arena_pages, bin), slice_index, 1));
      // note: we cannot check for `!mi_page_is_abandoned_and_mapped` since that may
      // be (temporarily) not true if the free happens while trying to reclaim
      // see `mi_arena_try_claim_abandoned`
    }
    #endif
    // A heap walker may hold the bit clear for a moment to pin the page while it tries to claim it
    // (`mi_heap_visit_page_claim`); wait for it to give the bit back, then clear it. Last access to the heap.
    mi_bitmap_clear_once_set(subproc, arena_pages->pages, slice_index);
    const size_t slice_committed = mi_page_slice_committed(page);
    if (slice_committed > 0) {
      // if committed on-demand, set the commit bits to account commit properly
      mi_assert_internal(mi_page_full_size(page) >= slice_committed);
      const size_t total_slices = slice_committed / MI_ARENA_SLICE_SIZE;  // conservative
      //mi_assert_internal(mi_bitmap_is_clearN(arena->slices_committed, slice_index, total_slices));
      mi_assert_internal(slice_count >= total_slices);
      if (total_slices > 0) {
        mi_bitmap_setN(arena->slices_committed, slice_index, total_slices, NULL);
      }
      // any left over?
      const size_t extra = slice_committed % MI_ARENA_SLICE_SIZE;
      if (extra > 0) {
        // pretend it was decommitted already
        mi_subproc_stat_decrease(arena->subproc, committed, extra);
      }
    }
    else {
      mi_assert_internal(mi_bitmap_is_setN(arena->slices_committed, slice_index, slice_count));
    }
  }
  // unregister page (after the arena_pages bitmap clear above)
  _mi_page_map_unregister(page);
  if (mi_page_meta_is_separated(page)) { page->block_size = 0; }  // for assertion checking
  _mi_arenas_free( subproc, mi_page_slice_start(page), mi_page_full_size(page), page->memid);
}

// Free a page that is not in the abandoned map or list (anymore): held by a theap until now, or claimed by a heap delete.
static void mi_arenas_page_free_ex(mi_page_t* page, mi_theap_t* current_theapx, bool unabandon) {
  mi_assert_internal(_mi_page_qsbr_is_clear(page));
  mi_assert_internal(mi_is_aligned(mi_page_slice_start(page), MI_PAGE_ALIGN));
  mi_assert_internal(mi_ptr_page(mi_page_start(page))==page);
  mi_assert_internal(mi_page_is_owned(page));
  mi_assert_internal(mi_page_all_free(page));
  mi_assert_internal(mi_page_is_abandoned(page));
  mi_assert_internal(unabandon || (page->next==NULL && page->prev==NULL));
  mi_assert_internal(_mi_theap_can_touch(current_theapx));

  // Undo any hole in the page: the arena can hand this memory out again as committed without
  // any further `reuse` call, and on macOS a discarded page stays reclaimable by the kernel
  // until it is MADV_FREE_REUSE'd.
  _mi_page_unpurge_all(page);

  // all we need from the heap, before the page is unpublished from it (see `mi_arenas_page_free_prim`)
  mi_heap_t* const heap = mi_page_heap(page);
  mi_subproc_t* const subproc = heap->subproc;
  mi_arena_pages_t* arena_pages = NULL;
  if (page->memid.memkind == MI_MEM_ARENA) { mi_page_arena_pages(page, NULL, NULL, &arena_pages); }
                                       else { mi_heap_stat_decrease(heap, pages_os_allocated, 1); }  // (here and not in `mi_arenas_page_free_prim`, which cannot read the heap)
  mi_theapx_stat_decrease(heap, current_theapx, page_bins[_mi_page_stats_bin(page)], 1);
  mi_theapx_stat_decrease(heap, current_theapx, pages, 1);
  _mi_page_free_collect(page,false);  // update used count for cross-thread free's (nothing is purged anymore, so this cannot un-purge)
  _mi_page_update_stats_for(page,current_theapx);  // and update the stats
  if (unabandon) {
    _mi_arenas_page_unabandon(page, current_theapx);   // for an OS page this is where it is unpublished
  }
  mi_arenas_page_free_prim(page, subproc, arena_pages);
}

void _mi_arenas_page_free(mi_page_t* page, mi_theap_t* current_theapx) {
  mi_arenas_page_free_ex(page, current_theapx, false);
}

// Free an abandoned page (that the caller owns and found all free): take it out of the abandoned page map
// or list of its heap and free it.
void _mi_arenas_abandoned_page_free(mi_page_t* page, mi_theap_t* current_theapx) {
  mi_arenas_page_free_ex(page, current_theapx, true);
}


/* -----------------------------------------------------------
  Abandon a page
----------------------------------------------------------- */


// release ownership of a page. This may free the page if all blocks were concurrently
// freed in the meantime. Returns true if the page was freed.
static bool mi_abandoned_page_unown(mi_page_t* page, mi_theap_t* current_theapx /* can be NULL */) {
  mi_assert_internal(mi_page_is_owned(page));
  mi_assert_internal(mi_page_is_abandoned(page));
  mi_assert_internal(_mi_theap_can_touch(current_theapx));
  mi_thread_free_t tf_new;
  mi_thread_free_t tf_old = mi_atomic_load_relaxed(&page->xthread_free);
  do {
    mi_assert_internal(mi_tf_is_owned(tf_old));
    while mi_unlikely(mi_tf_block(tf_old) != NULL) {
      _mi_page_free_collect(page, false);  // update used
      if (mi_page_all_free(page)) {
        if (!_mi_page_qsbr_should_delay(page)) {
          _mi_page_qsbr_clear(page);
          _mi_arenas_abandoned_page_free(page, current_theapx);
          return true;
        }
        #if MI_QSBR
        else if (page->memid.memkind == MI_MEM_ARENA &&
                 !mi_page_is_singleton(page) && !mi_page_is_abandoned_mapped(page))
        {
          // if empty and qsbr delayed, we re-abandon to mapped so it can be reused
          _mi_arenas_page_unabandon(page, current_theapx);
          _mi_arenas_page_abandon(page, current_theapx);   // cannot recurse due to `!mi_page_is_abandoned_mapped(page)`
          return false;
        }
        #endif
      }
      tf_old = mi_atomic_load_relaxed(&page->xthread_free);
    }
    mi_assert_internal(mi_tf_block(tf_old)==NULL);
    tf_new = mi_tf_create(NULL, false);
  } while (!mi_atomic_cas_weak_acq_rel(&page->xthread_free, &tf_old, tf_new));
  return false;
}


void _mi_arenas_page_abandon(mi_page_t* page, mi_theap_t* current_theapx) {
  mi_assert_internal(mi_is_aligned(mi_page_slice_start(page), MI_PAGE_ALIGN));
  mi_assert_internal(mi_ptr_page(mi_page_start(page))==page);
  mi_assert_internal(mi_page_is_owned(page));
  mi_assert_internal(mi_page_is_abandoned(page));
  mi_assert_internal(MI_QSBR || !mi_page_all_free(page));
  mi_assert_internal(page->next==NULL && page->prev == NULL);
  mi_assert_internal(_mi_theap_can_touch(current_theapx));
  // mi_assert_internal(current_theap == _mi_page_associated_theap(page));
  #if MI_QSBR
  mi_assert_internal(!page->qsbr_in_queue && page->qsbr_next == NULL && page->qsbr_prev == NULL);
  #endif

  // note: somewhat expensive to update here, but might be good as then we attribute
  // the current allocations/frees to the current thread/theap. Otherwise it might be
  // reclaimed later in another thread/theap and those allocations/frees get attributed there...
  _mi_page_update_stats_for(page, current_theapx);

  // add to abandoned?
  // (not for a heap that is being released: its teardown claims every page through `pages`, see `mi_heap_release_pages`)
  mi_heap_t* heap = mi_page_heap(page);
  if (page->memid.memkind==MI_MEM_ARENA && !mi_page_is_full(page) && mi_atomic_load_relaxed(&heap->releasing) == 0
      #if MI_QSBR
      && !mi_page_is_singleton(page) // an arena singleton has size > MI_LARGE_MAX_OBJ_SIZE and bin==MI_BIN_HUGE
      #endif
     ) {
    // make available for allocations
    size_t bin = _mi_bin(mi_page_block_size(page));
    mi_assert_internal(bin < MI_ARENA_BIN_COUNT);
    if (bin < MI_ARENA_BIN_COUNT) { // paranoia
      size_t slice_index;
      size_t slice_count;
      mi_arena_pages_t* arena_pages = NULL;
      mi_arena_t* const arena = mi_page_arena_pages(page, &slice_index, &slice_count, &arena_pages); MI_UNUSED(arena);

      mi_assert_internal(!mi_page_is_singleton(page));
      mi_assert_internal(mi_bbitmap_is_clearN(arena->slices_free, slice_index, slice_count));
      mi_assert_internal(mi_page_slice_committed(page) > 0 || mi_bitmap_is_setN(arena->slices_committed, slice_index, slice_count));
      mi_assert_internal(mi_bitmap_is_setN(arena->slices_dirty, slice_index, slice_count));

      // If the bin's bitmap cannot be allocated the page is abandoned unmapped, like a full
      // page: it is still reachable through `pages` and is reclaimed once a block in it is freed.
      mi_bitmap_t* const bitmap = mi_arena_pages_abandoned_ensure(arena, arena_pages, bin);
      if mi_likely(bitmap != NULL) {
        mi_page_set_abandoned_mapped(page);
        const bool was_clear = mi_bitmap_set(bitmap, slice_index);
        MI_UNUSED(was_clear); mi_assert_internal(was_clear);
        mi_atomic_increment_relaxed(&heap->abandoned_count[bin]);
        mi_theapx_stat_increase(heap, current_theapx, pages_abandoned, 1);
        mi_abandoned_page_unown(page, current_theapx);
        return;
      }
    }
  }
  // otherwise,
  // page is full (or a singleton), or the page is OS/externally allocated
  // leave as is; it will be reclaimed when an object is free'd in the page
  // but for non-arena pages, add to the subproc list so these can be visited
  if (page->memid.memkind != MI_MEM_ARENA) {
    mi_lock(&heap->os_abandoned_pages_lock) {
      // push in front
      page->prev = NULL;
      page->next = heap->os_abandoned_pages;
      if (page->next != NULL) { page->next->prev = page; }
      heap->os_abandoned_pages = page;
    }
    mi_theapx_stat_increase(heap, current_theapx, pages_os_abandoned, 1);
  }
  mi_theapx_stat_increase(heap, current_theapx, pages_abandoned, 1);
  mi_abandoned_page_unown(page, current_theapx);
}


// this is called from `alloc/free.c:mi_free_try_collect_mt` only.
bool _mi_arenas_page_try_reabandon_to_mapped(mi_page_t* page) {
  mi_assert_internal(mi_is_aligned(mi_page_slice_start(page), MI_PAGE_ALIGN));
  mi_assert_internal(mi_ptr_page(mi_page_start(page))==page);
  mi_assert_internal(mi_page_is_owned(page));
  mi_assert_internal(mi_page_is_abandoned(page));
  mi_assert_internal(!mi_page_is_abandoned_mapped(page));
  mi_assert_internal(!mi_page_is_full(page));
  mi_assert_internal(!mi_page_all_free(page));
  mi_assert_internal(!mi_page_is_singleton(page));
  if (mi_page_is_full(page) || mi_page_is_abandoned_mapped(page) || page->memid.memkind != MI_MEM_ARENA) {
    return false;
  }
  else if (mi_atomic_load_relaxed(&mi_page_heap(page)->releasing) != 0) {
    return false;  // the heap is being released and claims the page through `pages` (see `mi_heap_release_pages`)
  }
  else {
    // Account on the heap and not on this thread's theap for it (`_mi_page_associated_theap_peek`): the theap
    // was only used for its statistics here, and a concurrent `mi_heap_delete` may be detaching and merging it.
    mi_heap_t* const heap = mi_page_heap(page);
    mi_heap_stat_counter_increase(heap, pages_reabandon_full, 1);
    mi_heap_stat_adjust_decrease(heap, pages_abandoned, 1);  // adjust as we are not abandoning fresh
    _mi_arenas_page_abandon(page, NULL);
    return true;
  }
}

// called from `mi_free` if trying to unabandon an abandoned page
void _mi_arenas_page_unabandon(mi_page_t* page, mi_theap_t* current_theapx) {
  mi_assert_internal(mi_is_aligned(mi_page_slice_start(page), MI_PAGE_ALIGN));
  mi_assert_internal(mi_ptr_page(mi_page_start(page))==page);
  mi_assert_internal(mi_page_is_owned(page));
  mi_assert_internal(mi_page_is_abandoned(page));
  mi_assert_internal(_mi_theap_can_touch(current_theapx));

  mi_heap_t* const heap = mi_page_heap(page);
  if (mi_page_is_abandoned_mapped(page)) {
    mi_assert_internal(page->memid.memkind==MI_MEM_ARENA);
    // remove from the abandoned map
    const size_t bin = _mi_bin(mi_page_block_size(page));
    mi_assert_internal(bin < MI_ARENA_BIN_COUNT);
    size_t slice_index;
    size_t slice_count;
    mi_arena_pages_t* arena_pages;
    mi_arena_t* arena = mi_page_arena_pages(page, &slice_index, &slice_count, &arena_pages);  MI_UNUSED(arena);

    mi_assert_internal(mi_bbitmap_is_clearN(arena->slices_free, slice_index, slice_count));
    mi_assert_internal(mi_page_slice_committed(page) > 0 || mi_bitmap_is_setN(arena->slices_committed, slice_index, slice_count));

    // this busy waits until a concurrent reader (from alloc_abandoned) is done
    mi_bitmap_t* const bitmap = mi_arena_pages_abandoned(arena_pages, bin);
    mi_assert_internal(bitmap != NULL);  // a mapped page was set in it
    mi_bitmap_clear_once_set(arena->subproc, bitmap, slice_index);
    mi_page_clear_abandoned_mapped(page);
    mi_atomic_decrement_relaxed(&heap->abandoned_count[bin]);
  }
  else {
    mi_theapx_stat_decrease(heap, current_theapx, pages_abandoned, 1);
    // page is full (or a singleton), or the page is OS allocated
    // if not an arena page, remove it from the heap os pages list. That unpublishes it from the heap: when we
    // are about to free the page this is our last access to the heap (see `mi_arenas_page_free_prim`).
    if (page->memid.memkind != MI_MEM_ARENA) {
      mi_theapx_stat_decrease(heap, current_theapx, pages_os_abandoned, 1);  // (before the unlink: that is the last access to the heap)
      mi_lock(&heap->os_abandoned_pages_lock) {
        if (page->prev != NULL) { page->prev->next = page->next; }
        if (page->next != NULL) { page->next->prev = page->prev; }
        if (heap->os_abandoned_pages == page) { heap->os_abandoned_pages = page->next; }
        page->next = NULL;
        page->prev = NULL;
      }
    }
    return;
  }
  mi_theapx_stat_decrease(heap, current_theapx, pages_abandoned, 1);
}


/* -----------------------------------------------------------
  Purge the holes in abandoned pages

  Abandoned pages have no owning thread, so the idle sweep of a theap never sees them --
  yet with the default `allow_page_abandon` every page that ever became full ends up here.
  We claim each page with the arena's ownership protocol before touching its free list.
----------------------------------------------------------- */

typedef struct mi_purge_holes_arg_s {
  mi_bitmap_t* bitmap;
  mi_tld_t*    tld;      // the thread whose sweep this is (its own, or the parked one the scavenger is sweeping for)
} mi_purge_holes_arg_t;

static bool mi_arena_page_purge_holes_at(size_t slice_index, size_t slice_count, mi_arena_t* arena, void* arg) {
  MI_UNUSED(slice_count);
  mi_purge_holes_arg_t* const parg = (mi_purge_holes_arg_t*)arg;
  // this pass holds every page that ever became full, so it is most of a cold sweep: an owner
  // waiting in `_mi_park_leave` cannot allocate until we stop
  if (mi_atomic_load_relaxed(&parg->tld->park_reclaim) != 0) return false;
  mi_bitmap_t* const bitmap = parg->bitmap;

  // Take the page out of the abandoned map first: this is the reader side of the protocol in
  // `mi_arena_try_claim_abandoned`. A concurrent `_mi_arenas_page_unabandon` busy-waits for the
  // bit to reappear (`mi_bitmap_clear_once_set`), so while we hold it cleared the page cannot be
  // freed under us. If the bit is already clear, someone else has the page: skip it.
  if (!mi_bitmap_clear(bitmap, slice_index)) return true;

  mi_page_t* const page = mi_arena_page_at_slice(arena, slice_index);
  if (!mi_page_claim_ownership(page)) {
    mi_bitmap_set(bitmap, slice_index);   // a concurrent free owns it: keep it abandoned (as `mi_arena_try_claim_abandoned` does)
    return true;
  }

  // We own the page: no other thread can reclaim, unabandon, or free it now, and only the
  // atomic `xthread_free` can still change under us. (No un-purging: we are about to purge.)
  _mi_page_free_collect_no_unpurge(page, true);
  if (mi_page_all_free(page) && !_mi_page_qsbr_should_delay(page)) {
    mi_bitmap_set(bitmap, slice_index);   // `_mi_arenas_page_unabandon` expects it in the map
    _mi_page_qsbr_clear(page);
    _mi_arenas_abandoned_page_free(page, NULL);
    _mi_page_holes_count_page_freed();
    return true;
  }
  _mi_page_purge_holes(page, parg->tld);
  mi_bitmap_set(bitmap, slice_index);     // back in the map *before* unowning: unown may free the page
  mi_abandoned_page_unown(page, NULL);
  return true;
}

typedef struct mi_purge_holes_visit_info_s {
  mi_heap_t* heap;
  mi_tld_t*  tld;
} mi_purge_holes_visit_info_t;

static bool mi_arena_purge_abandoned_holes_visit(mi_arena_t* arena, const void* arg, void** result) {
  MI_UNUSED(result);
  const mi_purge_holes_visit_info_t* const info = (const mi_purge_holes_visit_info_t*)arg;
  mi_heap_t* const heap = info->heap;
  mi_arena_pages_t* const arena_pages = mi_heap_arena_pages(heap, arena);
  if (arena_pages == NULL) return true; // continue
  // pages_abandoned[] is MI_ARENA_BIN_COUNT wide, not MI_BIN_COUNT: bins above the
  // singleton bins have no abandoned bitmap (upstream ad1bcdbf, to shrink arena meta).
  for (size_t bin = 0; bin < MI_ARENA_BIN_COUNT; bin++) {
    if (mi_atomic_load_relaxed(&heap->abandoned_count[bin]) == 0) continue;
    mi_bitmap_t* const bitmap = mi_arena_pages_abandoned(arena_pages, bin);
    if (bitmap == NULL) continue;
    mi_purge_holes_arg_t parg = { bitmap, info->tld };
    (void)_mi_bitmap_forall_set(bitmap, &mi_arena_page_purge_holes_at, arena, &parg);
  }
  return true; // continue
}

// note: this only reaches the *mapped* abandoned pages (the ones in `pages_abandoned`).
// A page abandoned while full is not mapped; it has no free blocks at that point, and once
// enough blocks are freed in it, `_mi_arenas_page_try_reabandon_to_mapped` puts it in the map.
void _mi_arenas_purge_abandoned_holes(mi_heap_t* heap, mi_tld_t* tld) {
  if (heap == NULL || tld == NULL) return;
  if (!mi_option_is_enabled(mi_option_purge_holes)) return;
  _mi_page_purge_holes_begin(tld);
  const mi_purge_holes_visit_info_t info = { heap, tld };
  mi_forall_arenas(heap, NULL, 0, &mi_arena_purge_abandoned_holes_visit, &info, NULL);
  _mi_page_purge_holes_end(tld);
}

// The read-only counterpart of the sweep above: account for the holes in the abandoned pages
// instead of purging them. Same claim protocol -- the page must not be reclaimed or freed
// while we read its free lists -- but nothing inside the page is modified.
typedef struct mi_arena_holes_report_arg_s {
  mi_bitmap_t*       bitmap;
  mi_holes_report_t* rep;
} mi_arena_holes_report_arg_t;

static bool mi_arena_page_holes_report_at(size_t slice_index, size_t slice_count, mi_arena_t* arena, void* arg) {
  MI_UNUSED(slice_count);
  mi_arena_holes_report_arg_t* const ra = (mi_arena_holes_report_arg_t*)arg;
  if (!mi_bitmap_clear(ra->bitmap, slice_index)) return true;   // someone else has the page

  mi_page_t* const page = mi_arena_page_at_slice(arena, slice_index);
  if (!mi_page_claim_ownership(page)) {
    mi_bitmap_set(ra->bitmap, slice_index);   // a concurrent free owns it: keep it abandoned
    return true;
  }
  _mi_page_holes_report_page(page, ra->rep);
  mi_bitmap_set(ra->bitmap, slice_index);     // back in the map *before* unowning: unown may free the page
  // Unown collects a pending `xthread_free`, but that collect always lands a block on `page->free`,
  // so it can never take the `free == NULL` branch that would un-purge a run of holes.
  mi_abandoned_page_unown(page, NULL);
  return true;
}

typedef struct mi_holes_report_visit_info_s {
  mi_heap_t*         heap;
  mi_holes_report_t* rep;
} mi_holes_report_visit_info_t;

static bool mi_arena_holes_report_visit(mi_arena_t* arena, const void* arg, void** result) {
  MI_UNUSED(result);
  const mi_holes_report_visit_info_t* const info = (const mi_holes_report_visit_info_t*)arg;
  mi_heap_t* const heap = info->heap;
  mi_arena_pages_t* const arena_pages = mi_heap_arena_pages(heap, arena);
  if (arena_pages == NULL) return true; // continue
  for (size_t bin = 0; bin < MI_ARENA_BIN_COUNT; bin++) {   // see above: not MI_BIN_COUNT
    if (mi_atomic_load_relaxed(&heap->abandoned_count[bin]) == 0) continue;
    mi_arena_holes_report_arg_t ra = { mi_arena_pages_abandoned(arena_pages, bin), info->rep };
    if (ra.bitmap == NULL) continue;
    (void)_mi_bitmap_forall_set(ra.bitmap, &mi_arena_page_holes_report_at, arena, &ra);
  }
  return true; // continue
}

void _mi_arenas_holes_report(mi_heap_t* heap, mi_holes_report_t* rep) {
  if (heap == NULL || rep == NULL) return;
  const mi_holes_report_visit_info_t info = { heap, rep };
  mi_forall_arenas(heap, NULL, 0, &mi_arena_holes_report_visit, &info, NULL);
}

static bool mi_arena_holes_committed_visit(mi_arena_t* arena, const void* arg, void** result) {
  MI_UNUSED(result);
  mi_holes_report_t* const rep = ((const mi_holes_report_visit_info_t*)arg)->rep;
  rep->arena_meta_bytes += _mi_size_of_slices(mi_arena_info_slices(arena));
  rep->arena_reserved_bytes += _mi_size_of_slices(arena->slice_count);
  const size_t slice_count = arena->slice_count;
  for (size_t i = 0; i < slice_count; i++) {
    if (mi_bitmap_is_set(arena->slices_committed, i)) { rep->arena_committed_bytes += MI_ARENA_SLICE_SIZE; }
    if (!mi_bbitmap_is_setN(arena->slices_free, i, 1)) continue;   // in a page (or reserved meta): not slack
    if (mi_bitmap_is_set(arena->slices_dirty, i)) { rep->arena_free_dirty_bytes += MI_ARENA_SLICE_SIZE; }
    if (mi_arena_slice_is_purgeable(arena, i)) { rep->arena_purge_pending_bytes += MI_ARENA_SLICE_SIZE; }
  }
  return true; // continue
}

// Where the memory IS: the arena slack, i.e. the memory mimalloc holds OUTSIDE any page. If what
// we are chasing lands here instead of inside the pages, the problem is the arena / purge delay
// and not free blocks contaminating pages -- a completely different fix.
//
// A slice in `slices_free` belongs to no page. Of those, a slice that was never touched costs
// nothing, so residency is carried by `slices_dirty` (ever handed out, hence written) and
// `slices_purge` (queued for purge, so still resident right now). `slices_committed` is NOT a
// residency signal: it is set for the whole arena at reserve time for any `initially_committed`
// memory (every POSIX mmap) and a reset-style purge does not clear it -- we report it only so the
// caveat is visible in the output rather than silently misleading.
void _mi_arenas_holes_committed(mi_heap_t* heap, mi_holes_report_t* rep) {
  if (heap == NULL || rep == NULL) return;
  const mi_holes_report_visit_info_t info = { heap, rep };
  mi_forall_arenas(heap, NULL, 0, &mi_arena_holes_committed_visit, &info, NULL);
}


/* -----------------------------------------------------------
  Visit all pages and blocks in a heap
----------------------------------------------------------- */

typedef struct mi_heap_visit_info_s {
  mi_heap_t*          heap;
  mi_block_visit_fun* visitor;
  void*               arg;
  bool                visit_blocks;
  bool                abandoned_only;
  bool                claim_pages;      // claim ownership before visiting (for delete/destroy)
  mi_arena_pages_t*   arena_pages;      // current arena's pages bitmap (for re-check during claim)
} mi_heap_visit_info_t;

static bool mi_heap_visit_page(mi_page_t* page, const mi_heap_visit_info_t* vinfo) {
  mi_heap_area_t area;
  #if MI_QSBR
  const bool claimed = (mi_page_is_abandoned(page) && mi_page_claim_ownership(page));
  #else
  const bool claimed = false;
  #endif
  // Collect cross-thread free'd blocks so `area.used` is precise, but only in a page that is ours to write: a walk that
  // visits no blocks only reads the other pages, which is harmless next to a thread that still allocates from the heap.
  if (claimed || vinfo->claim_pages || mi_page_thread_id(page) == _mi_thread_id()) {
    _mi_page_free_collect_no_unpurge(page, true);  // (visiting must not un-purge a hole)
  }
  #if MI_QSBR
  if (claimed && mi_abandoned_page_unown(page, NULL)) return true;
  #endif
  _mi_heap_area_init(&area, page);
  mi_assert_internal(vinfo->heap == mi_page_heap(page));
  if (!vinfo->visitor(vinfo->heap, &area, NULL, area.block_size, vinfo->arg)) {
    return false; // break
  }
  if (vinfo->visit_blocks) {
    return _mi_theap_area_visit_blocks(&area, page, vinfo->visitor, vinfo->arg); // continue or break
  }
  else {
    return true; // continue
  }
}

// Claim a page of `vinfo->heap` for `mi_heap_delete`/`mi_heap_destroy` (`claim_pages`). All theaps of the heap
// were detached and their pages abandoned before the walk (`heap.c:mi_heap_release_pages`), so every page
// still published in `arena_pages->pages` is an abandoned page of this heap, and the only other party that
// can own one is a concurrent `mi_free` that collects it (`free.c:mi_free_try_collect_mt`) -- which may
// free the page, after which the slice can be reused for anything at once. So we must not write to the
// page (claiming is a write) unless it is pinned: we pin it by clearing its `pages` bit while it is set.
// The owner of a page unpublishes it with `mi_bitmap_clear_once_set` (`mi_arenas_page_free_prim`), which
// waits while we hold the bit, so a pinned page stays a page of this heap. If the page turns out to be
// owned we give the bit back and retry; once we own it nobody else can free it and the bit goes back too.
// This is the same protocol the abandoned-page map uses (`mi_arena_try_claim_abandoned`,
// `mi_arena_page_purge_holes_at` against `_mi_arenas_page_unabandon`).
// The page struct is only located (`mi_arena_page_at_slice`, which reads `block_size` with separated page
// meta) once the slice is pinned: before that a concurrent `mi_free` may be freeing the page and the slice
// may already be a fresh page of another heap.
// Returns the page if we own it, or NULL if it is gone.
#if MI_DEBUG > 0
mi_decl_export _Atomic(uintptr_t) mi_debug_stall_in_heap_delete_claim;   // test hook (test-heap-teardown): stall while a page is pinned but not yet claimed
#endif

static void mi_heap_visit_page_seize(mi_page_t* page) {
  // the page sits in the queue of a theap whose thread is gone (fork) or misbehaving; leave that queue be
  page->next = page->prev = NULL;
  mi_page_set_theap(page, NULL);
}

static mi_page_t* mi_heap_visit_page_claim(const mi_heap_visit_info_t* vinfo, mi_arena_t* arena, size_t slice_index) {
  mi_bitmap_t* const pages = vinfo->arena_pages->pages;
  mi_page_t* page = NULL;
  for (;;) {
    if (!mi_bitmap_clear(pages, slice_index)) return NULL;   // freed by a concurrent `mi_free`
    // pinned: now it is a page of this heap and stays one while we hold the bit
    #if MI_PAGE_META_IS_ALIGNED
    if mi_unlikely(_mi_process_is_forked_child) {
      // not through `mi_arena_page_at_slice`: in a debug build that checks the page map, whose entry may be the very thing that is missing (see below)
      page = mi_atomic_load_ptr_acquire(mi_page_t, &mi_aligned_ptr_page0(mi_arena_slice_start(arena, slice_index))->self);
    }
    else
    #endif
    {
      page = mi_arena_page_at_slice(arena, slice_index);
    }
    #if MI_DEBUG > 0
    if (mi_atomic_load_acquire(&mi_debug_stall_in_heap_delete_claim) == 1) {
      mi_atomic_store_release(&mi_debug_stall_in_heap_delete_claim, (uintptr_t)2);  // signal: pinned, not yet claimed
      while (mi_atomic_load_acquire(&mi_debug_stall_in_heap_delete_claim) == 2) { _mi_prim_thread_yield(); }
    }
    #endif
    if mi_unlikely(_mi_process_is_forked_child) {
      // After a multi-threaded fork() the child may inherit a torn snapshot of a page that another
      // thread was allocating or freeing: the bit propagated but the page-map entry or the owned bit
      // did not, and that thread is gone. Re-derive what we can and take the page.
      if (page == NULL || page->block_size == 0 || mi_page_start(page) == NULL) return NULL;  // the page struct never made it across: leave it unpublished
      if (_mi_safe_ptr_page(mi_page_start(page)) != page && !_mi_page_map_register(page)) return NULL;
      mi_page_claim_ownership(page);   // ours now, whether or not the dead thread held it
      mi_bitmap_set(pages, slice_index);
      if (!mi_page_is_abandoned(page)) { mi_heap_visit_page_seize(page); }
      break;
    }
    if mi_unlikely(!mi_page_is_abandoned(page)) {
      // A thread that allocated from this heap before is using it during the delete (it allocated this
      // page, or reclaimed it on a free just now): that is outside the contract of `mi_heap_delete`.
      mi_assert(false && "a heap is deleted while a thread that allocated from it still uses it");
      mi_bitmap_set(pages, slice_index);
      mi_heap_visit_page_seize(page);
      break;
    }
    if (mi_page_claim_ownership(page)) {
      mi_bitmap_set(pages, slice_index);
      break;
    }
    // owned by a concurrent `mi_free`: let it finish (it may free the page, re-abandon it, or just release it)
    mi_bitmap_set(pages, slice_index);
    mi_subproc_stat_counter_increase(vinfo->heap->subproc, heaps_delete_wait, 1);
    _mi_prim_thread_yield();
  }
  mi_assert_internal(mi_page_is_owned(page) && mi_page_is_abandoned(page));
  mi_assert_internal(mi_bitmap_is_set(pages, slice_index));
  mi_assert_internal(mi_ptr_page(mi_page_start(page)) == page && mi_page_heap(page) == vinfo->heap);
  return page;
}

static bool mi_heap_visit_page_at(size_t slice_index, size_t slice_count, mi_arena_t* arena, void* arg) {
  MI_UNUSED(slice_count);
  const mi_heap_visit_info_t* vinfo = (const mi_heap_visit_info_t*)arg;
  mi_page_t* page;
  if (vinfo->claim_pages) {
    page = mi_heap_visit_page_claim(vinfo, arena, slice_index);
    if (page == NULL) return true;  // gone: freed by a concurrent `mi_free`
  }
  else {
    page = mi_arena_page_at_slice(arena, slice_index);
  }
  return mi_heap_visit_page(page, vinfo); // continue or break
}

static bool mi_heap_visit_arena(mi_arena_t* arena, const void* arg, void** result) {
  MI_UNUSED(result);
  const mi_heap_visit_info_t* vinfo = (const mi_heap_visit_info_t*)arg;
  mi_heap_visit_info_t visit_info = *vinfo;
  mi_heap_t* const heap = vinfo->heap;
  mi_arena_pages_t* arena_pages = mi_heap_arena_pages(heap, arena);
  if (arena_pages == NULL) return true; // continue
  visit_info.arena_pages = arena_pages;
  if (vinfo->abandoned_only) {
    for (size_t bin = 0; bin < MI_ARENA_BIN_COUNT; bin++) {
      // TODO: if we had a single abandoned page map as well, this can be faster. Maybe for CPython?
      if (mi_atomic_load_relaxed(&heap->abandoned_count[bin]) > 0) {
        mi_bitmap_t* const bitmap = mi_arena_pages_abandoned(arena_pages, bin);
        if (bitmap == NULL) continue;
        if (!_mi_bitmap_forall_set(bitmap, &mi_heap_visit_page_at, arena, &visit_info)) return false; // break
      }
    }
    return true; // continue
  }
  else {
    return _mi_bitmap_forall_set(arena_pages->pages, &mi_heap_visit_page_at, arena, &visit_info); // continue or break
  }
}

// The caller holds heap->theaps_lock to prevent thread-exit abandonment and theap removal.
static bool mi_heap_visit_owned_os_blocks(mi_heap_t* heap, bool visit_blocks, mi_block_visit_fun* visitor, void* arg) {
  const mi_heap_visit_info_t visit_info = { heap, visitor, arg, visit_blocks, false, false, NULL };
  for (mi_theap_t* theap = heap->theaps; theap != NULL; theap = theap->hnext) {
    if (theap->page_count_os == 0) continue;
    for (size_t bin = 0; bin <= MI_BIN_FULL; bin++) {
      mi_page_t* page = theap->pages[bin].first;
      while (page != NULL) {
        mi_page_t* const next = page->next;
        if (page->memid.memkind != MI_MEM_ARENA && !mi_heap_visit_page(page, &visit_info)) return false;
        page = next;
      }
    }
  }
  return true;
}

// Visit the abandoned OS allocated pages of a heap. When `claim_pages` is set (delete/destroy), a page on
// the list is pinned by holding the list lock: a concurrent `mi_free` that owns it has to take the lock to
// unlink it before it can free it (`_mi_arenas_page_unabandon`). So we claim under the lock, and if the page
// is owned we drop the lock to let that free finish and start over, until the list is empty.
static bool mi_heap_visit_os_pages(mi_heap_t* heap, const mi_heap_visit_info_t* vinfo) {
  if mi_likely(mi_atomic_load_acquire(&heap->has_os_pages) == 0) return true;
  if (!vinfo->claim_pages) {
    // (we assume we are the only thread running in this heap)
    mi_page_t* page = NULL;
    mi_lock(&heap->os_abandoned_pages_lock) { page = heap->os_abandoned_pages; }
    while (page != NULL) {
      mi_page_t* const next = page->next;  // read upfront in case the visitor frees the page
      if (!mi_heap_visit_page(page, vinfo)) return false;
      page = next;
    }
    return true;
  }
  for (;;) {
    mi_page_t* page = NULL;
    bool owned = false;
    mi_lock(&heap->os_abandoned_pages_lock) {
      page = heap->os_abandoned_pages;
      if (page != NULL) { owned = mi_page_claim_ownership(page); }
    }
    if (page == NULL) return true;
    if (owned) {
      mi_assert_internal(mi_page_is_abandoned(page) && mi_page_heap(page) == heap);
      if (!mi_heap_visit_page(page, vinfo)) return false;  // unlinks it from the list
    }
    else {
      mi_subproc_stat_counter_increase(heap->subproc, heaps_delete_wait, 1);
      _mi_prim_thread_yield();
    }
  }
}

static bool mi_heap_visit_blocks_locked(mi_heap_t* heap, bool abandoned_only, bool visit_blocks, bool claim_pages, mi_block_visit_fun* visitor, void* arg) {
  // visit all pages in a heap
  // Other threads must not access this heap; thread-exit abandonment is blocked by the heap lock.
  const mi_heap_visit_info_t visit_info = { heap, visitor, arg, visit_blocks, abandoned_only, claim_pages, NULL };
  bool ok = mi_forall_arenas(heap, NULL, 0, &mi_heap_visit_arena, &visit_info, NULL);
  if (!ok) return false;

  // visit abandoned pages in OS allocated memory
  ok = mi_heap_visit_os_pages(heap, &visit_info);

  // visit owned OS allocated blocks in the theaps
  // (not for a delete/destroy: its theaps are detached and hold no pages anymore, see `heap.c:mi_heap_release_pages`)
  if (ok && !abandoned_only && !claim_pages) {
    ok = mi_heap_visit_owned_os_blocks(heap, visit_blocks, visitor, arg);
  }
  return ok;
}

bool _mi_heap_visit_blocks(mi_heap_t* heap, bool abandoned_only, bool visit_blocks, bool claim_pages, mi_block_visit_fun* visitor, void* arg) {
  mi_assert(visitor!=NULL);
  if (visitor==NULL) return false;
  if (heap==NULL) { heap = mi_heap_main(); }
  bool ok = false;
  // (a delete/destroy needs no lock: with its theaps detached no thread abandons pages into the heap on exit)
  mi_lock_maybe(&heap->theaps_lock, !claim_pages) {
    ok = mi_heap_visit_blocks_locked(heap, abandoned_only, visit_blocks, claim_pages, visitor, arg);
  }
  return ok;
}

bool mi_heap_visit_blocks(mi_heap_t* heap, bool visit_blocks, mi_block_visit_fun* visitor, void* arg) {
  return _mi_heap_visit_blocks(heap, false, visit_blocks, false, visitor, arg);
}

bool mi_heap_visit_abandoned_blocks(mi_heap_t* heap, bool visit_blocks, mi_block_visit_fun* visitor, void* arg) {
  return _mi_heap_visit_blocks(heap, true, visit_blocks, false, visitor, arg);
}

#if MI_QSBR
static bool mi_heap_collect_abandoned_page(mi_page_t* page) {
  if (!mi_page_is_abandoned(page) || !mi_page_claim_ownership(page)) return true;
  _mi_page_free_collect(page, true);
  if (mi_page_all_free(page) && !_mi_page_qsbr_should_delay(page)) {
    _mi_page_qsbr_clear(page);
    _mi_arenas_abandoned_page_free(page, NULL);
  }
  else {
    mi_abandoned_page_unown(page, NULL);
  }
  return true;
}

static bool mi_heap_collect_abandoned_page_at(size_t slice_index, size_t slice_count, mi_arena_t* arena, void* arg) {
  MI_UNUSED(slice_count); MI_UNUSED(arg);
  return mi_heap_collect_abandoned_page(mi_arena_page_at_slice(arena, slice_index));
}

static bool mi_heap_collect_abandoned_arena(mi_arena_t* arena, const void* arg, void** result) {
  MI_UNUSED(result);
  mi_heap_t* heap = (mi_heap_t*)arg;
  mi_arena_pages_t* arena_pages = mi_heap_arena_pages(heap, arena);
  if (arena_pages == NULL) return true;
  return _mi_bitmap_forall_set(arena_pages->pages, &mi_heap_collect_abandoned_page_at, arena, NULL);
}

void _mi_heap_collect_abandoned(mi_heap_t* heap) {
  mi_assert_internal(heap != NULL);
  mi_lock(&heap->theaps_lock) {
    // Use the all-pages map: abandoned size-class maps exclude singletons and full pages.
    mi_forall_arenas(heap, NULL, 0, &mi_heap_collect_abandoned_arena, heap, NULL);
    mi_page_t* page = NULL;
    mi_lock(&heap->os_abandoned_pages_lock) { page = heap->os_abandoned_pages; }
    while (page != NULL) {
      mi_page_t* next = page->next;
      mi_heap_collect_abandoned_page(page);
      page = next;
    }
  }
}
#endif


// Free a page even if it has live blocks (for `mi_heap_destroy`).
// The page must be owned and not be in a page queue or in the abandoned map/list.
void _mi_arenas_page_destroy(mi_page_t* page, mi_theap_t* current_theapx) {
  // Fold in the blocks that other threads freed: an abandoned page keeps at least the last of them on its
  // thread-free list (`_mi_page_free_collect_partly` never collects the head), and they still count as used.
  // (no un-purging: nothing is allocated from this page here)
  _mi_page_free_collect_no_unpurge(page, false);
  if (mi_page_used(page)!=0) {
    #if MI_GUARDED
    _mi_page_unguard_all(page);          // remove potential interior guard pages
    #endif
    #if MI_PROFILE
    if mi_unlikely(mi_page_has_interior_pointers(page)) {
      mi_heap_area_t area;
      _mi_heap_area_init(&area, page);
      _mi_page_profile_free_all(&area,page); // the sampled blocks that are still in use get their `on_free`
    }
    #endif
    // Drop what is (still) on the thread-free list first: `_mi_arenas_page_free` collects it, and with the used
    // count reset that reads as a corrupted list (more blocks freed than were in use).
    mi_atomic_store_release(&page->xthread_free, mi_tf_create(NULL, true /* owned */));
    mi_page_used_reset(page);           // note: invariant `|local_free| + |free| == reserved - used`  does not hold in this case
  }
  _mi_arenas_page_free(page, current_theapx);
}

typedef struct mi_heap_delete_visit_info_s {
  mi_heap_t*  heap_target;
  mi_theap_t* theap_target;
  mi_theap_t* theap;
} mi_heap_delete_visit_info_t;

static bool mi_heap_delete_page(const mi_heap_t* heap, const mi_heap_area_t* area, void* block, size_t block_size, void* arg) {
  MI_UNUSED(block); MI_UNUSED(block_size); MI_UNUSED(heap);
  const mi_heap_delete_visit_info_t* info = (const mi_heap_delete_visit_info_t*)arg;
  mi_heap_t*  heap_target           = info->heap_target;
  mi_theap_t* const theap           = info->theap;  // only used for statistics (can be NULL)
  mi_page_t*  const page            = (mi_page_t*)area->reserved1;

  // claimed by `mi_heap_visit_page_claim` (or `mi_heap_visit_os_pages`)
  mi_assert_internal(mi_page_is_owned(page));
  mi_assert_internal(mi_page_is_abandoned(page));
  _mi_page_qsbr_clear(page); // deletion requires an unprotected heap; destruction requires no readers
  _mi_arenas_page_unabandon(page,theap);

  if (heap_target==NULL) {
    _mi_arenas_page_destroy(page, theap);
    return true;
  }

  // collect blocks freed by other threads (see `_mi_arenas_page_destroy`)
  _mi_page_free_collect_no_unpurge(page, false);

  if (mi_page_used(page)==0) {
    // free the page
    _mi_arenas_page_free(page, theap);
  }
  else if (page->memid.memkind != MI_MEM_ARENA) {
    // OS/external page: no arena bitmaps to update. Re-home and re-abandon it
    // into the target heap's `os_abandoned_pages` list via the normal path.
    const size_t sbin = _mi_page_stats_bin(page);
    if (theap != NULL) { mi_theap_stat_decrease(theap, page_bins[sbin], 1); mi_theap_stat_decrease(theap, pages, 1); }
    else               { mi_heap_stat_decrease((mi_heap_t*)heap, page_bins[sbin], 1); mi_heap_stat_decrease((mi_heap_t*)heap, pages, 1); }
    mi_theap_t* theap_target = info->theap_target;
    mi_atomic_store_release(&heap_target->has_os_pages, (uintptr_t)1);
    page->heap = heap_target;
    mi_theap_stat_increase(theap_target, page_bins[sbin], 1);
    mi_theap_stat_increase(theap_target, pages, 1);
    _mi_arenas_page_abandon(page, theap_target);
  }
  else {
    // move the page to `heap_target` as an abandoned page
    // first remove it from the current heap
    const size_t sbin = _mi_page_stats_bin(page);
    mi_arena_t* arena = NULL;
    size_t slice_index = 0;
    if (page->memid.memkind == MI_MEM_ARENA) {
      size_t slice_count;
      mi_arena_pages_t* arena_pages = NULL;
      arena = mi_page_arena_pages(page, &slice_index, &slice_count, &arena_pages);
      mi_assert_internal(mi_bitmap_is_set(arena_pages->pages, slice_index));
      mi_bitmap_clear(arena_pages->pages, slice_index);
    }
    else {
      // os allocated
      mi_assert_internal(mi_memid_is_os(page->memid) && page->next == NULL);
    }
    if (theap != NULL) {
      mi_theap_stat_decrease(theap, page_bins[sbin], 1);
      mi_theap_stat_decrease(theap, pages, 1);
    }
    else {
      mi_heap_stat_decrease((mi_heap_t*)heap, page_bins[_mi_page_stats_bin(page)], 1);
      mi_heap_stat_decrease((mi_heap_t*)heap, pages, 1);
    }
    mi_theap_t* theap_target = info->theap_target;

    // and then add it to the new target heap
    if (arena != NULL) {
      mi_arena_pages_t* arena_pages_target = mi_heap_ensure_arena_pages(heap_target, arena);
      if mi_unlikely(arena_pages_target==NULL) {
        // if we cannot allocate this, we move it to the main heap instead (which does not require allocation)
        heap_target = _mi_arena_heap_main(arena);
        theap_target = mi_heap_theap(heap_target);  // todo: find through theap_target tld?
        arena_pages_target = mi_heap_ensure_arena_pages(heap_target, arena);
        mi_assert_internal(arena_pages_target!=NULL);
      }
      mi_assert_internal(mi_bitmap_is_clear(arena_pages_target->pages, slice_index));
      mi_bitmap_set(arena_pages_target->pages, slice_index);
    }
    page->heap = heap_target;
    mi_theap_stat_increase(theap_target, page_bins[sbin], 1);
    mi_theap_stat_increase(theap_target, pages, 1);

    // and abandon in the new heap
    _mi_arenas_page_abandon(page,theap_target);
  }
  return true; // continue
}

static void mi_heap_delete_pages(mi_heap_t* heap, mi_heap_t* heap_target) {
  mi_theap_t* const theap_target = (heap_target != NULL ? _mi_heap_theap(heap_target) : NULL);
  // count on our theap of the main heap, where `mi_heap_release_pages` merged the stats of `heap`
  mi_theap_t* const theap = _mi_heap_theap_peek(mi_heap_get_heap_main(heap));
  mi_heap_delete_visit_info_t info = { heap_target, theap_target, theap };
  _mi_heap_visit_blocks(heap, false, false, true /* claim each page */, &mi_heap_delete_page, &info);
  #if MI_DEBUG>1
  // no more arena pages?
  for (size_t i = 0; i < MI_MAX_ARENAS; i++) {
    mi_arena_pages_t* const arena_pages = mi_atomic_load_ptr_relaxed(mi_arena_pages_t, &heap->arena_pages[i]);
    if (arena_pages!=NULL) {
      mi_assert_internal(mi_bitmap_is_all_clear(arena_pages->pages));
    }
  }
  // nor os abandoned pages?
  mi_lock(&heap->os_abandoned_pages_lock) {

    mi_assert_internal(heap->os_abandoned_pages == NULL);
  }
  // nor arena abandoned pages?
  for (size_t i = 0; i < MI_ARENA_BIN_COUNT; i++) {
    mi_assert_internal(mi_atomic_load_relaxed(&heap->abandoned_count[i])==0);
  }
  #endif
}

void _mi_heap_move_pages(mi_heap_t* heap_from, mi_heap_t* heap_to) {
  if (mi_is_heap_main(heap_from)) return;
  if (heap_to==NULL) { heap_to = heap_from->subproc->heap_main; }
  mi_heap_delete_pages(heap_from, heap_to);
}

void _mi_heap_destroy_pages(mi_heap_t* heap_from) {
  if (mi_is_heap_main(heap_from)) return;
  mi_heap_delete_pages(heap_from, NULL);
}
