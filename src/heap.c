/*----------------------------------------------------------------------------
Copyright (c) 2018-2026, Microsoft Research, Daan Leijen
This is free software; you can redistribute it and/or modify it under the
terms of the MIT license. A copy of the license can be found in the file
"LICENSE" at the root of this distribution.
-----------------------------------------------------------------------------*/

#include "mimalloc.h"
#include "mimalloc/internal.h"
#include "mimalloc/prim.h"      // _mi_prim_thread_yield
#include "mimalloc/prim-tls.h"  // _mi_heap_theap


/* -----------------------------------------------------------
  Heap's
----------------------------------------------------------- */

mi_theap_t* mi_heap_theap(mi_heap_t* heap) {
  return _mi_heap_theap(heap);  // in prim.h
}

void mi_heap_set_numa_affinity(mi_heap_t* heap, int numa_node) {
  if (heap==NULL) { heap = mi_heap_main(); }
  heap->numa_node = (numa_node < 0 ? -1 : numa_node % _mi_os_numa_node_count());
}

void mi_heap_stats_merge_to_subproc(mi_heap_t* heap) {
  if (heap==NULL) { heap = mi_heap_main(); }
  mi_stats_t* const stats = mi_atomic_load_ptr_acquire(mi_stats_t,&heap->stats);
  if (stats!=NULL) { _mi_stats_merge_into(&heap->subproc->stats, stats); }
}

void mi_heap_stats_merge_to_main(mi_heap_t* heap) {
  if (heap==NULL) return;
  mi_stats_t* const stats = mi_atomic_load_ptr_acquire(mi_stats_t,&heap->stats);
  if (stats!=NULL) { _mi_stats_merge_into(_mi_heap_stats(mi_heap_get_heap_main(heap)), stats); }
}

// Uses the meta-data allocator since this can be called during thread teardown and from `mi_free`.
mi_decl_noinline mi_stats_t* _mi_heap_stats_ensure(mi_heap_t* heap) {
  mi_assert_internal(!_mi_is_heap_main(heap));
  if (mi_atomic_load_acquire(&heap->releasing) != 0) {
    return _mi_heap_stats(mi_heap_get_heap_main(heap));  // merged into the main heap on release anyway
  }
  mi_stats_t* const fresh = (mi_stats_t*)_mi_meta_zalloc(heap->subproc, sizeof(mi_stats_t), NULL);
  if (fresh==NULL) { return &heap->subproc->stats; }
  mi_stats_header_init(fresh);
  mi_stats_t* expected = NULL;
  if (mi_atomic_cas_ptr_strong_acq_rel(mi_stats_t, &heap->stats, &expected, fresh)) { return fresh; }
  _mi_free_subproc_safe(fresh);  // lost the race
  return expected;
}

bool _mi_heap_theap_set(mi_heap_t* heap, mi_theap_t* theap) {
  mi_assert_internal((uintptr_t)theap == 1 || _mi_theap_heap(theap)==heap);
  mi_assert_internal(!_mi_is_empty_theap(theap));
  mi_assert_internal(heap->theap != 0);
  return _mi_thread_local_set(heap->theap,theap);
}

// mi_theap_t* _mi_heap_theap_get_peek(const mi_heap_t* heap) {
//   mi_theap_t* theap;
//   mi_assert_internal(heap->theap != 0);
//   if mi_likely(heap->theap!=0) {  // paranoia
//     theap = (mi_theap_t*)_mi_thread_local_get(heap->theap);
//   }
//   else {
//     _mi_error_message(EFAULT, "no thread-local reserved for heap (%p)\n", heap);
//     return NULL;
//   }
//   mi_assert_internal(!_mi_is_empty_theap(theap));
//   mi_assert_internal(theap->heap == heap);  // this goes wrong if using main heaps across subprocesses (as all share the same key)
//   return theap;
// }


static mi_decl_noinline mi_theap_t* mi_heap_init_theap(const mi_heap_t* const_heap)
{
  mi_heap_t* heap = (mi_heap_t*)const_heap;
  mi_assert_internal(heap!=NULL);

  // initialize thread first in case this is the main heap
  // (which may allocate the default theap already for the main heap)
  if (!_mi_thread_is_initialized()) {
    mi_thread_init();
  }

  // get the thread local theap
  mi_theap_t* theap = (mi_theap_t*)_mi_thread_local_get(heap->theap);
  mi_assert(theap==NULL || _mi_theap_heap_peek(theap)==heap); // or else `heap` was deleted (and we, a thread that used it before, use it still)

  // create a fresh theap?
  if (theap==NULL) {
    // allocate a fresh theap
    theap = _mi_theap_create(heap, mi_theap_get_default()->tld); // sets the theap thread local
    if (theap==NULL) {
      _mi_error_message(EFAULT, "unable to allocate memory for a thread local heap\n");
      return NULL;
    }
    _mi_heap_theap_set(heap, theap);
    mi_assert_internal(theap == (mi_theap_t*)_mi_thread_local_get(heap->theap));
  }
  return theap;
}


// get (and possibly create) the theap belonging to a heap
mi_decl_cold mi_theap_t* _mi_heap_theap_get_or_init(const mi_heap_t* heap)
{
  mi_assert_internal(heap->theap != 0);
  mi_theap_t* theap = (mi_theap_t*)_mi_thread_local_get(heap->theap);
  if mi_unlikely(theap==NULL) {
    theap = mi_heap_init_theap(heap);
    if (theap==NULL) { return (mi_theap_t*)&_mi_theap_empty_wrong; }  // this will return NULL from page.c:_mi_malloc_generic
  }
  _mi_theap_cached_set(theap);
  return theap;
}

void _mi_heap_init(mi_heap_t* heap, mi_thread_local_t theap_slot, mi_subproc_t* subproc, mi_arena_id_t exclusive_arena_id, mi_stats_t* stats)
{
  // init fields
  heap->theap = theap_slot;
  heap->subproc = subproc;
  heap->exclusive_arena = _mi_arena_from_id(exclusive_arena_id);
  heap->numa_node = -1; // no initial affinity
  heap->profiler = mi_atomic_load_ptr_acquire(mi_profiler_t,&subproc->profiler);
  const bool is_main = (stats!=NULL);
  if (is_main) { mi_stats_header_init(stats); }
  mi_atomic_store_ptr_release(mi_stats_t,&heap->stats,stats);
  mi_lock_init(&heap->theaps_lock);
  mi_lock_init(&heap->os_abandoned_pages_lock);

  // avoid thread-local access for a main heap: the process may still be initializing
  mi_heap_t* const  heap_main  = (is_main ? heap : mi_heap_get_heap_main(heap));
  mi_theap_t* const theap_main = (is_main ? NULL : _mi_heap_theap_peek(heap_main));

  // push onto the subproc heaps
  const size_t shard_idx = (theap_main==NULL ? 0 : theap_main->tld->thread_seq % MI_HEAPS_SHARD_COUNT);
  mi_heaps_shard_t* const shard = &subproc->heaps[shard_idx];
  mi_lock(&shard->lock) {
    mi_heap_t* head = shard->first;
    heap->prev = NULL;
    heap->next = head;
    if (head!=NULL) { head->prev = heap;  }
    shard->first = heap;
    heap->heap_seq = (shard->total_count * MI_HEAPS_SHARD_COUNT) + shard_idx;
    shard->total_count++;
  }
  mi_assert_internal((heap->heap_seq == 0) == is_main);
  if (is_main) { mi_subproc_stat_increase(subproc, heaps, 1); }
          else { mi_theapx_stat_increase(heap_main, theap_main, heaps, 1); }
  mi_assert_internal(_mi_is_heap_main(heap) ? heap->theap == mi_thread_local_key_fast : heap->theap != 0);
}

mi_heap_t* _mi_heap_new_for_subproc(mi_subproc_t* subproc, mi_arena_id_t exclusive_arena_id, bool is_main_heap) {
  mi_assert_internal(is_main_heap ? (subproc->heap_main == NULL && subproc->parent != NULL) : subproc->heap_main != NULL);
  // heap data is allocated in the current subproc
  mi_heap_t* const heap_main = (is_main_heap ? subproc->parent->heap_main : subproc->heap_main);
  // todo: allocate heap data in the exclusive arena ?
  // a main heap has its statistics allocated inline
  mi_heap_t* const heap = (mi_heap_t*)mi_heap_zalloc( heap_main, sizeof(mi_heap_t) + (is_main_heap ? sizeof(mi_stats_t) : 0) );
  if (heap==NULL) return NULL;
  // reserve a thread local slot for this heap (see also issue #1230)
  mi_thread_local_t theap_slot = (is_main_heap ? mi_thread_local_key_fast : _mi_thread_local_create());
  if (theap_slot == 0) {
    _mi_error_message(EFAULT, "unable to dynamically create a thread local for a heap\n");
    mi_free(heap);
    return NULL;
  }
  if (is_main_heap) {
    mi_assert_internal(subproc->heap_main == NULL);
    subproc->heap_main = heap;
  }
  _mi_heap_init(heap, theap_slot, subproc, exclusive_arena_id, (is_main_heap ? (mi_stats_t*)(heap + 1) : NULL));
  return heap;
}

mi_heap_t* mi_heap_new_in_arena(mi_arena_id_t exclusive_arena_id) {
  // `mi_heap_new` may be the very first mimalloc call in a process, in which case the
  // main heap does not exist yet and `_mi_heap_new_for_subproc` would allocate from a NULL `subproc->heap_main`.
  mi_thread_init();
  return _mi_heap_new_for_subproc(_mi_subproc(), exclusive_arena_id, false);
}

mi_heap_t* mi_heap_new(void) {
  return mi_heap_new_in_arena(0);
}

/* -----------------------------------------------------------
  Heap delete and destroy.

  These can run concurrently with threads that terminate, and (delete) with `mi_free` calls from threads
  that never allocated from this heap. A thread that did allocate from the heap must not allocate from it
  or free into it while it is deleted (it may free afterwards, the blocks then belong to the main heap).

  1. `_mi_heap_detach_theaps`: unlink every theap of the heap from its thread and clear `theap->heap`.
     From here on no thread finds a theap for this heap anymore, so nothing allocates into it, reclaims an
     abandoned page of it, or abandons pages into it on thread termination. The list of theaps is now private
     to us and needs no lock.
  2. `_mi_theap_abandon` each detached theap: its pages become abandoned pages of the heap, exactly as if
     its thread had terminated. Now every page of the heap is abandoned, and the only other party that can
     hold one is a concurrent `mi_free` collecting it.
     (`mi_heap_destroy` frees these pages directly instead; step 3 then only sees pages that were abandoned earlier.)
  3. `_mi_heap_move_pages`/`_mi_heap_destroy_pages`: claim each abandoned page (waiting out such a free,
     see `arena.c:mi_heap_visit_page_claim`) and move it to the main heap or free it.
  4. free the theap structs (a free in step 3 may still have been using one it found just before step 1),
     and then the heap.
----------------------------------------------------------- */

// Returns the detached theaps; free them with `mi_heap_free_theaps`.
static mi_theap_t* mi_heap_release_pages(mi_heap_t* heap, mi_heap_t* heap_target) {
  mi_theap_t* const theaps = _mi_heap_detach_theaps(heap);
  if (_mi_is_heap_main(heap)) return theaps;  // (`_mi_heap_force_destroy` of a main heap at sub-process teardown: the arenas go as a whole)
  // Step 3 claims every page through the `arena_pages->pages` bitmap, so the pages abandoned in step 2
  // do not need to be findable by size class: `_mi_arenas_page_abandon` leaves them out of the
  // per-bin abandoned maps, which are then never allocated for a heap that only lives to be released.
  mi_atomic_store_release(&heap->releasing, (uintptr_t)1);
  // Merge the theap stats straight into the main heap, preferably into our own (thread-local) theap of it.
  // This must happen before step 3 decrements the counts or the peaks would miss this heap.
  mi_heap_t* const  heap_main  = mi_heap_get_heap_main(heap);
  mi_theap_t* const theap_main = _mi_heap_theap_peek(heap_main);
  for (mi_theap_t* theap = theaps; theap != NULL; theap = theap->hnext) {
    mi_assert_internal(_mi_theap_heap_peek(theap)==NULL);
    // after a fork() the theap of a thread that is gone may be torn: don't walk it, step 3 finds its pages
    const bool is_torn = (_mi_process_is_forked_child && theap->tld->thread_id != _mi_thread_id());
    if mi_likely(!is_torn) {
      if (heap_target!=NULL) { _mi_theap_abandon(theap); }
                        else { _mi_theap_destroy_pages(theap); }
    }
    if (theap_main!=NULL) { _mi_stats_add_into_local(&theap_main->stats, &theap->stats); }
                     else { _mi_stats_add_into(_mi_heap_stats(heap_main), &theap->stats); }
  }
  if (heap_target != NULL) {
    _mi_heap_move_pages(heap, heap_target);
  }
  else {
    _mi_heap_destroy_pages(heap);
  }
  return theaps;
}

static void mi_heap_free_theaps(mi_heap_t* heap, mi_theap_t* theaps) {
  mi_theap_t* const cached = _mi_theap_cached();
  mi_theap_t* theap = theaps;
  while(theap != NULL) {
    mi_theap_t* next = theap->hnext;
    theap->hnext = NULL;
    theap->hprev = NULL;
    mi_assert_internal(_mi_theap_heap_peek(theap)==NULL && (theap->page_count==0 || _mi_is_heap_main(heap) || _mi_process_is_forked_child));
    if (_mi_is_heap_main(heap)) { _mi_stats_merge_into(_mi_heap_stats(heap), &theap->stats); }  // else already merged in `mi_heap_release_pages`
    // drop our cached reference so the theap is freed now
    if (theap == cached) { _mi_theap_cached_set(_mi_theap_empty_get()); }
    _mi_theap_decref(theap);  // another thread's `_mi_theap_cached` can still reference (but no longer use) it
    theap = next;
  }

  // set the theap thread local to NULL (so _mi_page_associated_theap does not read from a freed theap (through delete pages -> page_update_stats))
  // (in this fork the theaps are detached before, and freed after, the pages leave the heap, so this only releases the slot)
  if (!_mi_is_process_heap_main(heap)) { 
    _mi_thread_local_free(heap->theap);
    heap->theap = 0;
  }
}

// free the heap resources (assuming the pages are already moved/destroyed, and all theaps have been freed)
static void mi_heap_free(mi_heap_t* heap, bool acquire_heaps_lock) {
  mi_assert_internal(heap!=NULL); // && !_mi_is_process_heap_main(heap));

  const bool is_main = _mi_is_heap_main(heap);

  // remove the heap from the subproc
  if (!is_main) { 
    mi_stats_t* const stats = mi_atomic_load_ptr_acquire(mi_stats_t,&heap->stats);
    if (stats!=NULL) {
      _mi_stats_add_into(_mi_heap_stats(mi_heap_get_heap_main(heap)), stats);
      _mi_free_subproc_safe(stats);
    }
    mi_heap_t* const  heap_main  = mi_heap_get_heap_main(heap);
    mi_theap_t* const theap_main = _mi_heap_theap_peek(heap_main);
    mi_theapx_stat_decrease(heap_main, theap_main, heaps, 1);
  }
  else {
    mi_heap_stats_merge_to_subproc(heap);
    mi_subproc_stat_decrease(heap->subproc, heaps, 1);
  }
  mi_heaps_shard_t* const shard = &heap->subproc->heaps[heap->heap_seq % MI_HEAPS_SHARD_COUNT];
  mi_lock_maybe(&shard->lock, acquire_heaps_lock) {
    if (heap->next!=NULL) { heap->next->prev = heap->prev; }
    if (heap->prev!=NULL) { heap->prev->next = heap->next; }
                     else { mi_assert_internal(shard->first==heap); shard->first = heap->next; }
  }

  // free all arena pages infos (after unlinking, so no heap walker can see them half freed)
  if (!is_main) {  // pages for the main heap are pre-allocated in the arenas
    const size_t arena_count = mi_atomic_load_acquire(&heap->subproc->arena_count);  // never shrinks while there are heaps
    for (size_t i = 0; i < arena_count; i++) {
      mi_arena_pages_t* arena_pages = mi_atomic_load_ptr_acquire(mi_arena_pages_t, &heap->arena_pages[i]);
      if (arena_pages!=NULL) {
        mi_atomic_store_ptr_relaxed(mi_arena_pages_t, &heap->arena_pages[i], NULL);
        _mi_arena_pages_free(arena_pages);
      }
    }
  }

  mi_lock_done(&heap->theaps_lock);
  mi_lock_done(&heap->os_abandoned_pages_lock);
  if (!_mi_is_process_heap_main(heap)) { 
    // _mi_thread_local_free(heap->theap);
    _mi_free_subproc_safe(heap); 
  }
}

void mi_heap_delete(mi_heap_t* heap) {
  if (heap==NULL) return;
  mi_heap_t* heap_main = mi_heap_get_heap_main(heap);
  if (heap == heap_main) {
    _mi_warning_message("cannot delete the main heap\n");
    return;
  }
  mi_theap_t* const theaps = mi_heap_release_pages(heap, heap_main);
  mi_heap_free_theaps(heap, theaps);
  mi_heap_free(heap,true /* acquire the shard lock */);
}

void _mi_heap_force_destroy(mi_heap_t* heap, bool acquire_heaps_lock) {
  if (heap==NULL) return;
  mi_theap_t* const theaps = mi_heap_release_pages(heap, NULL);
  mi_heap_free_theaps(heap, theaps);
  // if (_mi_subproc_main()->heap_main == heap) {
  //   _mi_stats_merge_into(&heap->subproc->stats,&heap->stats);
  // }
  // else 
  {
    mi_heap_free(heap, acquire_heaps_lock);   // todo: release locks of the main heap?  
  }
}

void mi_heap_destroy(mi_heap_t* heap) {
  if (heap==NULL) return;
  if (_mi_is_heap_main(heap)) {
    _mi_warning_message("cannot destroy the main heap\n");
    return;
  }
  _mi_heap_force_destroy(heap,true /* acquire the shard lock */);
}

mi_heap_t* mi_heap_of(const void* p) {
  mi_page_t* const page = _mi_safe_ptr_page(p);
  if (page==NULL) return NULL;
  return mi_page_heap(page);
}

bool mi_any_heap_contains(const void* p) {
  mi_page_t* const page = _mi_safe_ptr_page(p);
  return (page!=NULL);
}

bool mi_heap_contains(const mi_heap_t* heap, const void* p) {
  if (heap==NULL) { heap = mi_heap_main(); }
  return (heap==mi_heap_of(p));
}

// deprecated
bool mi_check_owned(const void* p) {
  return mi_any_heap_contains(p);
}

// unsafe heap utilization function for DragonFly (see issue #1258)
// If the page of pointer `p` belongs to `heap` (or `heap==NULL`) and has less than `perc_threshold` used blocks in its used area return `true`.
// This function is unsafe in general as it assumes we are the only thread accessing the page of `p`.
bool mi_unsafe_heap_page_is_under_utilized(mi_heap_t* heap, void* p, size_t perc_threshold) mi_attr_noexcept {
  if (p==NULL) return false;
  const mi_page_t* const page = _mi_safe_ptr_page(p);   // Get the page containing this pointer
  if (page==NULL || mi_page_used(page)==page->capacity || page->capacity < page->reserved) return false;
  // If the page is the head of the queue, it is currently being used for
  // allocations; we skip it to avoid immediate thrashing.
  if (page->prev == NULL)  return false;

  // match heap?
  const mi_heap_t* const page_heap = mi_page_heap(page);
  if (page_heap==NULL) return false;
  if (heap!=NULL && page_heap!=heap) return false;

  // check utilization
  if (page->capacity==0)   return false;
  if (perc_threshold>=100) return true;
  return (perc_threshold >= ((100UL*mi_page_used(page)) / page->capacity));
}
