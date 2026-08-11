/* ----------------------------------------------------------------------------
Copyright (c) 2018-2026, Microsoft Research, Daan Leijen
This is free software; you can redistribute it and/or modify it under the
terms of the MIT license. A copy of the license can be found in the file
"LICENSE" at the root of this distribution.
-----------------------------------------------------------------------------*/

#include "mimalloc.h"
#include "mimalloc/internal.h"
#include "mimalloc/prim-tls.h"

// pre-allocate the main subprocess structure.
static mi_decl_cache_align mi_subproc_t mi_process_subproc_main = mi_init_struct_zero;
static mi_subproc_t* mi_subprocs = NULL;
static mi_lock_t     mi_subprocs_lock = MI_LOCK_INITIALIZER;


/* -----------------------------------------------------------
  Meta-data allocation
  We allocate thread local data and theaps through a dedicated
  theap `subproc.theap_meta` which uses a detached tld with
  a detached thread id. The initial theap_meta is statically 
  allocated and can thus be used to allocate on an as yet
  uninitialized thread or process.
  We need to take a lock though to allocate safely on the
  detached `theap_meta`.
----------------------------------------------------------- */

void* _mi_meta_zalloc( mi_subproc_t* subproc, size_t size, mi_memid_t* memid ) {
  mi_assert_internal(subproc->theap_meta != NULL);
  void* p = NULL;
  mi_lock(&subproc->theap_meta_lock) {
    p = mi_theap_zalloc(subproc->theap_meta, size);
    if (memid != NULL) { *memid = (p==NULL ? _mi_memid_none() : _mi_memid_create_malloc(p,size,true) ); }
  }
  return p;
}

void* _mi_meta_zalloc_aligned( mi_subproc_t* subproc, size_t size, size_t aligned, mi_memid_t* memid ) {
  mi_assert_internal(subproc->theap_meta != NULL);
  void* p = NULL;
  mi_lock(&subproc->theap_meta_lock) {
    p = mi_theap_zalloc_aligned(subproc->theap_meta, size, aligned);
    if (memid != NULL) { *memid = (p==NULL ? _mi_memid_none() : _mi_memid_create_malloc(p,size,true) ); }
  }
  return p;
}

void* _mi_meta_rezalloc( mi_subproc_t* subproc, void* oldp, size_t newsize, mi_memid_t* memid ) {
  mi_assert_internal(subproc->theap_meta != NULL);
  // note: since we take a meta lock we cannot use `mi_theap_rezalloc` as that could call `mi_free` which
  // can call `mi_stat_free` which would try to take the meta lock again. See issue #1358.
  void* p = NULL;  
  mi_lock(&subproc->theap_meta_lock) {
    p = mi_theap_zalloc(subproc->theap_meta, newsize);
  }
  if (p!=NULL) {
    if (oldp!=NULL) {
      const size_t oldsize  = mi_usable_size(oldp);
      const size_t copysize = (newsize < oldsize ? newsize : oldsize);
      _mi_memcpy(p,oldp,copysize);
      if (memid!=NULL) { _mi_meta_free(subproc,oldp,*memid); } 
                  else { mi_free(oldp); }
    }
    if (memid!=NULL) { *memid = _mi_memid_create_malloc(p,newsize,true); }
  }
  else {
    if (memid!=NULL) { *memid = _mi_memid_none(); }  
  }
  return p;
}

void _mi_meta_free(mi_subproc_t* subproc, void* p, mi_memid_t memid) {
  if (p==NULL || mi_memid_needs_no_free(memid)) return;
  if (memid.memkind == MI_MEM_MALLOC) {
    mi_free(p);
  }
  else {
    mi_assert_internal(subproc!=NULL);  
    _mi_arenas_free(subproc, p, _mi_memid_size(memid), memid);
  }
}

bool _mi_meta_is_meta_page(const mi_subproc_t* subproc, const mi_page_t* page) {
  if (page==NULL) return false;
  mi_theap_t* theap = page->theap;
  return (theap != NULL && theap == subproc->theap_meta);
}


/* -----------------------------------------------------------
  Sub process helpers
----------------------------------------------------------- */

mi_subproc_t* _mi_subproc_main(void) {
  return &mi_process_subproc_main;
}

bool _mi_subproc_is_main(mi_subproc_t* subproc) {
  return (subproc == &mi_process_subproc_main);
}

mi_subproc_t* _mi_subproc(void) {
  mi_theap_t* theap = _mi_theap_default();
  if (theap == NULL || theap->tld == NULL) {  // see issue #1289
    return _mi_subproc_main();
  }
  else {
    return theap->tld->subproc;
  }
}

mi_heap_t* mi_heap_main(void) {
  return _mi_subproc_heap_main(_mi_subproc()); // don't use mi_theap_main_init_get() so this call works during process_init
}


mi_subproc_t* _mi_subproc_from_id(mi_subproc_id_t subproc_id) {
  return (mi_subproc_t*)(subproc_id._mi_subproc_id);
}

mi_subproc_id_t _mi_subproc_to_id(mi_subproc_t* subproc) {
  mi_subproc_id_t id = { subproc };
  return id;
}

mi_subproc_id_t mi_subproc_main(void) {
  return _mi_subproc_to_id(_mi_subproc_main());
}

mi_subproc_id_t mi_subproc_current(void) {
  return _mi_subproc_to_id(_mi_subproc());
}


/* -----------------------------------------------------------
  Sub process creation
----------------------------------------------------------- */


static mi_subproc_t* mi_subproc_init(mi_subproc_t* subproc, mi_subproc_t* parent) {
  static _Atomic(size_t) subproc_total_count;
  subproc->parent = parent;
  subproc->subproc_seq = mi_atomic_increment_relaxed(&subproc_total_count);
  mi_stats_header_init(&subproc->stats);
  mi_lock_init(&subproc->arena_reserve_lock);
  mi_lock_init(&subproc->heaps_lock);
  mi_lock_init(&subproc->theap_meta_lock);
  mi_lock_init(&subproc->tlds_lock);   // fork: tld registry
  mi_lock(&mi_subprocs_lock) {
    // push on subproc list
    subproc->next = mi_subprocs;
    if (mi_subprocs!=NULL) { mi_subprocs->prev = subproc; }
    mi_subprocs = subproc;
  }
  return subproc;
}

mi_subproc_id_t mi_subproc_new(void) {
  mi_thread_init();
  mi_subproc_t* const parent = _mi_subproc();
  mi_memid_t memid;
  mi_subproc_t* const subproc = (mi_subproc_t*)_mi_meta_zalloc(parent, sizeof(mi_subproc_t), &memid);
  if (subproc == NULL) { return _mi_subproc_to_id(NULL); }
  subproc->memid  = memid;  
  
  mi_memid_t theap_memid;
  mi_theap_t* const theap_meta = (mi_theap_t*)_mi_meta_zalloc(parent, sizeof(mi_theap_t), &theap_memid);
  if (theap_meta==NULL) { 
    _mi_meta_free(parent, subproc, memid); 
    return _mi_subproc_to_id(NULL); 
  }
  theap_meta->memid = memid;
  
  // init subproc
  mi_subproc_init(subproc,parent);
  
  // init main heap
  mi_heap_t* heap_main = _mi_heap_new_for_subproc(subproc,0,true);
  if (heap_main==NULL) {
    _mi_meta_free(parent, theap_meta, theap_meta->memid);
    mi_subproc_destroy(_mi_subproc_to_id(subproc));
    return _mi_subproc_to_id(NULL);
  }
  mi_assert_internal(subproc->heap_main == heap_main);

  // init meta theap
  mi_assert_internal(parent->theap_meta!=NULL);
  mi_assert_internal(parent->theap_meta->tld!=NULL);
  mi_assert_internal(parent->theap_meta->tld->thread_id == MI_THREADID_DETACHED);
  _mi_theap_init(theap_meta,heap_main,parent->theap_meta->tld /* detached tld */);
  subproc->theap_meta = theap_meta;

  return _mi_subproc_to_id(subproc);
}


/* -----------------------------------------------------------
  Sub process destruction
----------------------------------------------------------- */

// destroy all subproc resources including arena's, heap's etc.
static void mi_subproc_unsafe_destroy(mi_subproc_t* subproc, bool acquire_subprocs_lock)
{
  if (subproc==NULL) return;

  // remove from the subproc list
  mi_lock_maybe(&mi_subprocs_lock, acquire_subprocs_lock) {
    if (subproc->next!=NULL) { subproc->next->prev = subproc->prev;  }
    if (subproc->prev!=NULL) { subproc->prev->next = subproc->next;  }
                        else { mi_assert_internal(mi_subprocs==subproc);  mi_subprocs = subproc->next; }
  }

  // destroy all subproc heaps
  mi_lock(&subproc->heaps_lock) {
    mi_heap_t* heap = subproc->heaps;
    while (heap != NULL) {
      mi_heap_t* next = heap->next;
      if (heap!=subproc->heap_main) { _mi_heap_force_destroy(heap, false /* don't re-acquire the heaps_lock */); }
      heap = next;
    }
    mi_assert_internal(subproc->heap_main==NULL || subproc->heaps == subproc->heap_main);
    if (subproc->heap_main!=NULL) {
      _mi_thread_locals_thread_done(); // release thread locals that may have been allocated (safe as the main heap uses the fast key)
      if (_mi_subproc_is_main(subproc)) {
        _mi_thread_locals_done();      
      }
      _mi_heap_force_destroy(subproc->heap_main, false /* don't re-acquire the heaps_lock */);  // no warning if destroying the main heap
    }
  }

  subproc->theap_meta = NULL; // theap meta stats are merged during heap_destroy of the main heap

  if (!_mi_subproc_is_main(subproc)) {
    // merge stats back into the main subproc  
    _mi_stats_merge_into(&mi_process_subproc_main.stats, &subproc->stats);
  }

  // remove associated arenas
  _mi_arenas_unsafe_destroy_all(subproc);

  // show stats of the main process (at process end) before releasing the heaps lock
  if (_mi_subproc_is_main(subproc)) {
    if (mi_option_is_enabled(mi_option_show_stats) || mi_option_is_enabled(mi_option_verbose)) {
      mi_subproc_stats_print_out(mi_subproc_main(), NULL, NULL);
    }
  }

  // todo: should we refcount subprocesses?
  mi_lock_done(&subproc->arena_reserve_lock);
  mi_lock_done(&subproc->heaps_lock);
  mi_lock_done(&subproc->theap_meta_lock);  
  _mi_meta_free( subproc->parent, subproc, subproc->memid);  
  if (_mi_subproc_is_main(subproc)) {
    // for the main subproc, also release the global page map
    _mi_page_map_unsafe_destroy();
  }
}

void mi_subproc_destroy(mi_subproc_id_t subproc_id) {
  mi_subproc_t* subproc = _mi_subproc_from_id(subproc_id);
  if (subproc==NULL || subproc==&mi_process_subproc_main) return;
  mi_subproc_unsafe_destroy(subproc, true /* take lock */);
}

void _mi_subprocs_unsafe_destroy_all(void) {
  mi_lock(&mi_subprocs_lock) {
    mi_subproc_t* subproc = mi_subprocs;
    while (subproc!=NULL) {
      mi_subproc_t* next = subproc->next;
      if (subproc!=&mi_process_subproc_main) {
        mi_subproc_unsafe_destroy(subproc, false /* take mi_subprocs lock */);
      }
      subproc = next;
    }
  }
  mi_subproc_unsafe_destroy(&mi_process_subproc_main, true /* take mi_subprocs lock */);
}


/* -----------------------------------------------------------
  Sub process various
----------------------------------------------------------- */

void mi_subproc_add_current_thread(mi_subproc_id_t subproc_id) {
  mi_subproc_t* subproc = _mi_subproc_from_id(subproc_id);
  mi_assert_internal(subproc!=NULL);
  if (subproc==NULL) return;
  mi_assert_internal(subproc->heap_main!=NULL);
  if (subproc->heap_main==NULL) return;
  mi_theap_t* theap = _mi_theap_default();
  if (mi_theap_is_initialized(theap)) {
    if (theap->tld!=NULL && theap->tld->subproc != subproc) {
      _mi_warning_message("unable to add thread to the subprocess as it was already in another subprocess (at %p)\n", theap->tld->subproc);
    }
    return;
  }

  // initialize this thread tld & theap
  _mi_thread_init_with_heap(subproc->heap_main);
}


bool mi_subproc_visit_heaps(mi_subproc_id_t subproc_id, mi_heap_visit_fun* visitor, void* arg) {
  mi_subproc_t* subproc = _mi_subproc_from_id(subproc_id);
  if (subproc==NULL) return false;
  bool ok = true;
  mi_lock(&subproc->heaps_lock) {
    for (mi_heap_t* heap = subproc->heaps; heap!=NULL && ok; heap = heap->next) {
      ok = (*visitor)(heap, arg);
    }
  }
  return ok;
}


mi_subproc_t* _mi_subproc_main_init(void) {
  mi_lock_init(&mi_subprocs_lock);
  mi_memid_t memid = _mi_memid_create_static(&mi_process_subproc_main,sizeof(mi_subproc_t));
  mi_process_subproc_main.memid = memid;
  mi_subproc_init(&mi_process_subproc_main,NULL);
  return &mi_process_subproc_main;
}

void _mi_subproc_main_done(void) {
  mi_lock_done(&mi_subprocs_lock);
}

/* -----------------------------------------------------------
  fork() support (fork-only): quiesce every sub-process around a fork so the child inherits consistent allocator state.
----------------------------------------------------------- */

static _Atomic(uintptr_t) mi_fork_depth;  // allow nested prepare/parent calls (e.g., zone hooks + pthread_atfork)
bool _mi_process_is_forked_child;          // set once in fork_child, never cleared; lets visitors avoid waiting on dead threads

// Lock order (prepare acquires in this order; parent releases in reverse; child re-initializes everything):
//   subprocs registry -> thread locals -> per subproc: main heap's arena_pages (it may allocate while held) -> heaps list ->
//   every other heap's arena_pages/theaps/os_abandoned -> main heap's theaps/os_abandoned -> tlds registry (leaf) ->
//   arena reserve -> meta-data theap.
static void mi_subproc_fork_prepare(mi_subproc_t* sp) {
  mi_heap_t* const heap_main = _mi_subproc_heap_main(sp);
  if (heap_main != NULL) { mi_lock_acquire(&heap_main->arena_pages_lock); }
  mi_lock_acquire(&sp->heaps_lock);
  for (mi_heap_t* h = sp->heaps; h != NULL; h = h->next) {
    if (h == heap_main) continue;
    mi_lock_acquire(&h->arena_pages_lock);
    mi_lock_acquire(&h->theaps_lock);
    mi_lock_acquire(&h->os_abandoned_pages_lock);
  }
  if (heap_main != NULL) {
    mi_lock_acquire(&heap_main->theaps_lock);
    mi_lock_acquire(&heap_main->os_abandoned_pages_lock);
  }
  mi_lock_acquire(&sp->tlds_lock);
  mi_lock_acquire(&sp->arena_reserve_lock);
  mi_lock_acquire(&sp->theap_meta_lock);
}

static void mi_subproc_fork_parent(mi_subproc_t* sp) {
  mi_heap_t* const heap_main = _mi_subproc_heap_main(sp);
  mi_lock_release(&sp->theap_meta_lock);
  mi_lock_release(&sp->arena_reserve_lock);
  mi_lock_release(&sp->tlds_lock);
  if (heap_main != NULL) {
    mi_lock_release(&heap_main->os_abandoned_pages_lock);
    mi_lock_release(&heap_main->theaps_lock);
  }
  for (mi_heap_t* h = sp->heaps; h != NULL; h = h->next) {
    if (h == heap_main) continue;
    mi_lock_release(&h->os_abandoned_pages_lock);
    mi_lock_release(&h->theaps_lock);
    mi_lock_release(&h->arena_pages_lock);
  }
  mi_lock_release(&sp->heaps_lock);
  if (heap_main != NULL) { mi_lock_release(&heap_main->arena_pages_lock); }
}

void _mi_process_fork_prepare(void) {
  if (!_mi_process_is_initialized) return;
  if (mi_atomic_increment_acq_rel(&mi_fork_depth) != 0) return;
  {
    // a parked thread's theaps may be mid-sweep by the scavenger; leave the parked state first so nothing is claimed across the fork
    mi_theap_t* const theap = _mi_theap_default();
    if (theap != NULL && mi_theap_is_initialized(theap) && theap->tld != NULL) { _mi_park_leave(theap->tld); }
  }
  mi_lock_acquire(&mi_subprocs_lock);
  _mi_thread_locals_fork_prepare();
  for (mi_subproc_t* sp = mi_subprocs; sp != NULL; sp = sp->next) { mi_subproc_fork_prepare(sp); }
}

void _mi_process_fork_parent(void) {
  if (!_mi_process_is_initialized) return;
  if (mi_atomic_decrement_acq_rel(&mi_fork_depth) != 1) return;
  // release in reverse: last sub-process first (the registry is a stack: newest first, so walk it into a reversed order)
  size_t n = 0;
  for (mi_subproc_t* sp = mi_subprocs; sp != NULL; sp = sp->next) { n++; }
  while (n > 0) {
    mi_subproc_t* sp = mi_subprocs;
    for (size_t k = 1; k < n; k++) { sp = sp->next; }
    mi_subproc_fork_parent(sp);
    n--;
  }
  _mi_thread_locals_fork_parent();
  mi_lock_release(&mi_subprocs_lock);
}

void _mi_process_fork_child(void) {
  if (!_mi_process_is_initialized) return;
  if (mi_atomic_exchange_acq_rel(&mi_fork_depth, 0) == 0) return;
  _mi_process_is_forked_child = true;
  _mi_scavenger_forked_child();   // the scavenger thread did not survive the fork; clear the state that says it did
  mi_lock_init(&mi_subprocs_lock);
  for (mi_subproc_t* sp = mi_subprocs; sp != NULL; sp = sp->next) {
    mi_lock_init(&sp->arena_reserve_lock);
    mi_lock_init(&sp->heaps_lock);
    mi_lock_init(&sp->tlds_lock);
    mi_lock_init(&sp->theap_meta_lock);
    mi_atomic_store_relaxed(&sp->scavenger_wake, (uint32_t)0);
    mi_atomic_store_relaxed(&sp->parked_count, (size_t)0);
    for (mi_tld_t* t = sp->tlds; t != NULL; t = t->subproc_next) {   // every tld, the forking thread's included, is registered here
      mi_lock_init(&t->theaps_lock);
      mi_atomic_store_relaxed(&t->park_state, (uint32_t)MI_PARK_RUNNING);
      mi_atomic_store_relaxed(&t->park_reclaim, (uint32_t)0);
      mi_atomic_store_relaxed(&t->park_swept, (uint32_t)0);
    }
    for (mi_heap_t* h = sp->heaps; h != NULL; h = h->next) {
      mi_lock_init(&h->theaps_lock);
      mi_lock_init(&h->arena_pages_lock);
      mi_lock_init(&h->os_abandoned_pages_lock);
    }
  }
  _mi_thread_locals_fork_child();
}
