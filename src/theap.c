/*----------------------------------------------------------------------------
Copyright (c) 2018-2026, Microsoft Research, Daan Leijen
This is free software; you can redistribute it and/or modify it under the
terms of the MIT license. A copy of the license can be found in the file
"LICENSE" at the root of this distribution.
-----------------------------------------------------------------------------*/

#include "mimalloc.h"
#include "mimalloc/internal.h"
#include "mimalloc/prim.h"      // _mi_prim_thread_yield
#include "mimalloc/prim-tls.h"  // _mi_theap_default

#if defined(_MSC_VER) && (_MSC_VER < 1920)
#pragma warning(disable:4204)  // non-constant aggregate initializer
#endif

/* -----------------------------------------------------------
  Helpers
----------------------------------------------------------- */

// return `true` if ok, `false` to break
typedef bool (theap_page_visitor_fun)(mi_theap_t* theap, mi_page_queue_t* pq, mi_page_t* page, void* arg1, void* arg2);

// Visit all pages in a theap; returns `false` if break was called.
static bool mi_theap_visit_pages(mi_theap_t* theap, theap_page_visitor_fun* fn, bool include_full, void* arg1, void* arg2)
{
  if (theap==NULL || theap->page_count==0) return true;

  // visit all pages
  #if MI_DEBUG>1
  size_t total = theap->page_count;
  size_t count = 0;
  #endif

  const size_t max_bin = (include_full ? MI_BIN_FULL : MI_BIN_FULL - 1);
  for (size_t i = 0; i <= max_bin; i++) {
    mi_page_queue_t* pq = &theap->pages[i];
    mi_page_t* page = pq->first;
    while(page != NULL) {
      mi_page_t* next = page->next; // save next in case the page gets removed from the queue
      mi_assert_internal(mi_page_theap(page) == theap);
      #if MI_DEBUG>1
      count++;
      #endif
      if (!fn(theap, pq, page, arg1, arg2)) return false;
      page = next; // and continue
    }
  }
  mi_assert_internal(!include_full || count == total);
  return true;
}


#if MI_DEBUG>=3
static bool mi_theap_page_is_valid(mi_theap_t* theap, mi_page_queue_t* pq, mi_page_t* page, void* arg1, void* arg2) {
  MI_UNUSED(arg1);
  MI_UNUSED(arg2);
  MI_UNUSED(pq);
  mi_assert_internal(mi_page_theap(page) == theap);
  mi_theap_t* const page_theap = _mi_heap_theap_peek(page->heap);
  mi_assert_internal(page_theap == NULL || theap == page_theap || theap->tld->thread_id == MI_THREADID_DETACHED);
  mi_assert_expensive(_mi_page_is_valid(page));
  return true;
}

static bool mi_theap_is_valid(mi_theap_t* theap) {
  mi_assert_internal(theap!=NULL);
  mi_heap_t* const heap = _mi_theap_heap_peek(theap);
  mi_assert_internal(heap != NULL);
  mi_theap_t* const heap_theap = _mi_heap_theap_peek(heap);  // don't use mi_heap_theap as that may re-initialize the thread
  // a detached theap (`theap_meta`) is used under a lock from any thread, whose own theap for `heap` it is not
  mi_assert_internal(heap_theap==NULL || heap_theap == theap || theap->tld->thread_id == MI_THREADID_DETACHED);
  mi_theap_visit_pages(theap, &mi_theap_page_is_valid, true, NULL, NULL);
  for (size_t bin = 0; bin < MI_BIN_COUNT; bin++) {
    mi_assert_internal(_mi_page_queue_is_valid(theap, &theap->pages[bin]));
  }
  return true;
}
#endif




/* -----------------------------------------------------------
  "Collect" pages by migrating `local_free` and `thread_free`
  lists and freeing empty pages. This is done when a thread
  stops (and in that case abandons pages if there are still
  blocks alive)
----------------------------------------------------------- */

typedef enum mi_collect_e {
  MI_NORMAL,
  MI_FORCE,
  MI_ABANDON
} mi_collect_t;


static bool mi_theap_page_collect(mi_theap_t* theap, mi_page_queue_t* pq, mi_page_t* page, void* arg_collect, void* arg2 ) {
  MI_UNUSED(arg2);
  MI_UNUSED(theap);
  mi_assert_expensive(mi_theap_page_is_valid(theap, pq, page, NULL, NULL));
  mi_collect_t collect = *((mi_collect_t*)arg_collect);
  // A collect has no allocation to serve, so it must not un-purge: the allocation path
  // (`mi_page_queue_find_free_ex`) hands a hole back when a page is actually needed. Otherwise
  // `mi_on_thread_idle`, which collects before it sweeps, un-purges a run of every page whose free
  // blocks are all discarded and then re-discards it: two syscalls per page per park, forever.
  if (theap->tld != NULL && mi_atomic_load_relaxed(&theap->tld->park_reclaim) != 0) return false;
  _mi_page_free_collect_no_unpurge(page, collect >= MI_FORCE);
  if (mi_page_all_free(page)) {
    // no more used blocks, possibly free the page.
    if (collect >= MI_FORCE || page->retire_expire == 0) {  // either forced/abandon, or not already retired
      // note: this will potentially free retired pages as well.
      _mi_page_free(page, pq);
    }
  }
  else if (collect == MI_ABANDON) {
    // still used blocks but the thread is done; abandon the page
    _mi_page_abandon(page, pq);
  }
  return true; // don't break
}

void _mi_theap_merge_stats(mi_theap_t* theap) {
  mi_assert_internal(mi_theap_is_initialized(theap));
  mi_heap_t* const heap = _mi_theap_heap(theap);
  _mi_stats_merge_into(&heap->stats, &theap->stats);
}

static void mi_theap_collect_ex(mi_theap_t* theap, mi_collect_t collect)
{
  if (theap==NULL || !mi_theap_is_initialized(theap)) return;
  mi_assert_expensive(mi_theap_is_valid(theap));

  const bool force = (collect >= MI_FORCE);
  _mi_deferred_free(theap, force);

  // python/cpython#112532: we may be called from a thread that is not the owner of the theap
  // const bool is_main_thread = (_mi_is_main_thread() && theap->thread_id == _mi_thread_id());

  // collect retired pages (and full pages if theap->allow_page_abandon is false)
  _mi_theap_collect_retired(theap, force); 

  // collect all pages owned by this thread
  mi_theap_visit_pages(theap, &mi_theap_page_collect, (collect!=MI_NORMAL), &collect, NULL);  // dont normally visit full pages, see issue #1220

  // collect arenas (this is program wide so don't force purges on abandonment of threads).
  // Not from a claimed parked sweep though: a woken owner spins in `_mi_park_leave` for the
  // whole of it and nothing in the arena purge reads `park_reclaim`, so the "bounded by one page"
  // wait would become an unbounded subproc-wide madvise pass. The sweep's caller runs the arena
  // purge itself as a reclaim-gated phase (`_mi_arenas_purge_now`).
  //mi_atomic_storei64_release(&theap->tld->subproc->purge_expire, 1);
  if (theap->tld == NULL || mi_atomic_load_relaxed(&theap->tld->park_state) != MI_PARK_SWEEPING) {
    _mi_arenas_collect(collect == MI_FORCE /* force purge? */, collect >= MI_FORCE /* visit all? */, theap->tld);
  }

  // merge statistics
  _mi_theap_merge_stats(theap);
}

void _mi_theap_collect_abandon(mi_theap_t* theap) {
  mi_theap_collect_ex(theap, MI_ABANDON);
}

// Visit every page (INCLUDING the full queue, which a normal collect skips --
// see mi_theap_collect_ex) and discard the memory of free blocks inside pages
// that are still partially used. Meant to be called when the application knows
// it is idle (e.g. from an event loop about to park): it costs a few madvise
// calls and nothing on the alloc/free hot path.
static bool mi_theap_page_purge_holes(mi_theap_t* theap, mi_page_queue_t* pq, mi_page_t* page, void* arg_tld, void* arg2) {
  MI_UNUSED(arg2);
  mi_tld_t* const tld = (mi_tld_t*)arg_tld;   // the tld being swept (== theap->tld)
  // When the scavenger is doing this for a parked thread, the owner may wake at any moment and
  // has to wait for us. Stopping between pages bounds that wait to one page's walk; the pages we
  // skip are simply swept at the next park (`swept_state` makes the re-walk cheap).
  if (theap->tld != NULL && mi_atomic_load_relaxed(&theap->tld->park_reclaim) != 0) return false;
  // force: fold local_free (and thread_free) into `free` first. Never un-purge here: we are about
  // to purge, and a run brought back now would be discarded again right away (see `mi_theap_page_collect`).
  _mi_page_free_collect_no_unpurge(page, true);
  if (mi_page_all_free(page)) {
    // the forced collect emptied the page: hand it back instead of leaving it resident
    _mi_page_holes_count_page_freed();
    _mi_page_free(page, pq);
    return true;
  }
  _mi_page_purge_holes(page, tld);
  mi_assert_expensive(_mi_page_is_valid(page));
  return true; // continue
}

static void mi_theap_purge_holes(mi_theap_t* theap) mi_attr_noexcept {
  if (theap == NULL || !mi_theap_is_initialized(theap)) return;
  if (!mi_option_is_enabled(mi_option_purge_holes)) return;
  // This rewrites the thread-local free list of every page, so it may only run when the owner is
  // not allocating. Two ways to know that: we ARE the owner, or the owner published MI_PARK_PARKED
  // and the scavenger claimed it (MI_PARK_SWEEPING) -- the same "owner is quiesced" precondition
  // `mi_theap_collect` already relies on for its non-owner callers (see python/cpython#112532).
  if (theap->tld == NULL) return;
  if (theap->tld->thread_id != _mi_thread_id() &&
      mi_atomic_load_acquire(&theap->tld->park_state) != MI_PARK_SWEEPING) return;
  mi_tld_t* const tld = theap->tld;
  _mi_page_purge_holes_begin(tld);
  mi_theap_visit_pages(theap, &mi_theap_page_purge_holes, true /* include full pages */, tld, NULL);
  _mi_page_purge_holes_end(tld);
}

// Purge the holes in every page this thread may safely touch:
//  - the pages of every theap of this thread (`page->free`/`used` are plain fields that only the
//    owning thread may write, so we can never do this for a theap of another thread), and
//  - the abandoned pages of the heaps behind those theaps: those have no owning thread and are
//    claimed through the arena ownership protocol (see `_mi_arenas_purge_abandoned_holes`).
// The abandoned pages matter: with the default `allow_page_abandon`, every page that ever became
// full ends up there. Non-default heaps matter too (JSC allocates its structure heap with
// `mi_heap_new_in_arena`), which is why we sweep every theap and not just the default one.
#define MI_PURGE_HOLES_MAX_HEAPS  (8)

static void mi_purge_holes_of(mi_tld_t* tld) mi_attr_noexcept {
  if (!mi_option_is_enabled(mi_option_purge_holes)) return;
  if (tld == NULL) return;
  _mi_page_purge_holes_sweep_begin(tld);  // decides whether this sweep skips unchanged pages
  _mi_page_holes_reset_ineligible();   // the ineligible counters are a gauge over this sweep

  mi_heap_t* heaps[MI_PURGE_HOLES_MAX_HEAPS];
  size_t heap_count = 0;

  // Hold `tld->theaps_lock` for the whole sweep, including the abandoned-page pass below:
  //  - another thread can unlink a theap from this list in `mi_heap_free_theaps`, and
  //  - it keeps every `heaps[i]` alive: a heap is only freed by `mi_heap_delete`/`mi_heap_destroy`
  //    *after* `mi_heap_free_theaps` freed every theap of it, and freeing our theap needs this lock
  //    (`_mi_theap_free` try-acquires it and its caller retries), so it cannot complete while we
  //    hold it. Reading a `heaps[i]` outside the lock is a use-after-free (`heap->subproc`).
  mi_lock(&tld->theaps_lock) {
    for (mi_theap_t* theap = tld->theaps; theap != NULL; theap = theap->tnext) {
      mi_theap_purge_holes(theap);
      mi_heap_t* const heap = _mi_theap_heap(theap);
      if (heap != NULL && heap_count < MI_PURGE_HOLES_MAX_HEAPS) {
        bool seen = false;
        for (size_t i = 0; i < heap_count; i++) { if (heaps[i] == heap) { seen = true; break; } }
        if (!seen) { heaps[heap_count++] = heap; }
      }
    }
    for (size_t i = 0; i < heap_count; i++) {
      if (mi_atomic_load_relaxed(&tld->park_reclaim) != 0) break;
      _mi_arenas_purge_abandoned_holes(heaps[i], tld);
    }
  }
}

// Fold in pending frees, discard the holes in still-used pages, drain the arena purge queue.
// Runs on the owner (`mi_on_thread_idle`) or on the scavenger for a parked thread; both require
// that the owner of `tld` is not allocating while we rewrite its free lists.
void _mi_thread_idle_work(mi_tld_t* tld, mi_theap_t* theap0) mi_attr_noexcept {
  if (tld == NULL) return;
  // each phase is a full walk: an owner waiting in `_mi_park_leave` cannot allocate until we stop
  if (mi_atomic_load_relaxed(&tld->park_reclaim) != 0) return;
  if (theap0 != NULL && mi_theap_is_initialized(theap0)) {
    mi_theap_collect(theap0, false /* not forced */);
  }
  if (mi_atomic_load_relaxed(&tld->park_reclaim) != 0) return;
  mi_purge_holes_of(tld);   // every theap of this thread + the abandoned pages
  if (mi_atomic_load_relaxed(&tld->park_reclaim) != 0) return;
  _mi_arenas_purge_now(tld->subproc);
}

// Take the theaps of `tld` back from the scavenger. Also called from teardown: a thread can leave
// a park without reaching `mi_on_thread_idle_end` (`epoll_wait` is a cancellation point), and
// freeing the tld while the sweeper walks it is a use-after-free.
void _mi_park_leave(mi_tld_t* tld) {
  if (tld == NULL) return;
  for (;;) {
    uint32_t expected = MI_PARK_PARKED;
    if (mi_atomic_cas_strong_acq_rel(&tld->park_state, &expected, MI_PARK_RUNNING)) break;
    if (expected == MI_PARK_RUNNING) return;   // not parked: nothing to take back
    // it may re-claim the moment it releases, so re-race rather than store: only a CAS from
    // PARKED may reach RUNNING
    mi_assert_internal(expected == MI_PARK_SWEEPING);
    mi_atomic_store_release(&tld->park_reclaim, 1);
    // it stops at its next page or phase: spin briefly (no syscall), and only if it is still
    // sweeping after that -- likely descheduled -- yield the CPU to it
    size_t spin = 0;
    while (mi_atomic_load_acquire(&tld->park_state) == MI_PARK_SWEEPING) {
      if (spin < 256) { mi_atomic_pause(); spin++; }
      else { _mi_prim_thread_yield(); }
    }
  }
  mi_atomic_store_release(&tld->park_reclaim, 0);
  mi_atomic_decrement_relaxed(&tld->subproc->parked_count);
}

// The original entry point: do the work inline, on the calling thread. Kept for callers that
// have no wake-up side to pair with (and as the fallback when no scavenger is running).
void mi_on_thread_idle(void) mi_attr_noexcept {
  mi_theap_t* const theap0 = _mi_theap_default();
  if (theap0 == NULL || !mi_theap_is_initialized(theap0) || theap0->tld == NULL) return;
  if (theap0->tld->thread_id != _mi_thread_id()) return;
  _mi_thread_idle_work(theap0->tld, theap0);
}

// Declare that this thread will not allocate or free until `mi_on_thread_idle_end` -- the sweep's
// precondition -- so the scavenger can do it while we block.
//
// Returns false when nothing was handed off, and then `mi_on_thread_idle_end` is not required.
// It deliberately does NOT sweep inline in that case: a caller parks far more often than it is
// idle, and sweeping on every park is what it is trying to avoid. Only the caller knows whether
// this park is idle enough to afford `mi_on_thread_idle()` instead.
bool mi_on_thread_idle_start(void) mi_attr_noexcept {
  mi_theap_t* const theap0 = _mi_theap_default();
  if (theap0 == NULL || !mi_theap_is_initialized(theap0) || theap0->tld == NULL) return false;
  mi_tld_t* const tld = theap0->tld;
  if (tld->thread_id != _mi_thread_id()) return false;
  // the scavenger only sweeps the main subproc, so a thread elsewhere would never be swept
  if (!_mi_scavenger_is_running() || tld->subproc != _mi_subproc_main()) return false;

  // The scavenger has no TLS of ours to find the default theap with, so leave it here.
  tld->park_theap0 = theap0;
  mi_atomic_store_release(&tld->park_reclaim, 0);
  mi_atomic_store_release(&tld->park_swept, 0);
  uint32_t expected = MI_PARK_RUNNING;
  if (!mi_atomic_cas_strong_acq_rel(&tld->park_state, &expected, MI_PARK_PARKED)) return false;
  mi_atomic_increment_relaxed(&tld->subproc->parked_count);
  _mi_scavenger_wake(tld->subproc);
  return true;
}

// The other half: we are awake and about to allocate again, so take the theaps back. Usually an
// uncontended CAS. If the scavenger is mid-sweep we ask it to stop (it checks between pages) and
// spin until it does -- bounded by one page's walk, so normally a syscall-free wait.
void mi_on_thread_idle_end(void) mi_attr_noexcept {
  mi_theap_t* const theap0 = _mi_theap_default();
  if (theap0 == NULL || !mi_theap_is_initialized(theap0) || theap0->tld == NULL) return;
  mi_tld_t* const tld = theap0->tld;
  if (tld->thread_id != _mi_thread_id()) return;
  _mi_park_leave(tld);
}

// Sweep the theaps of every parked thread of `subproc`; scavenger only.
//
// MI_PARK_SWEEPING is what keeps `tld` alive across the sweep without holding `tlds_lock`: every
// path out of a park (`mi_on_thread_idle_end`, and teardown via `_mi_park_leave`) waits for it to
// clear before freeing anything. So the lock covers the walk, not the work.
//
// Returns in how many msecs a park that was passed over for `purge_holes_min_interval` becomes
// due (0: none was), so the scavenger can wake for it instead of leaving it to its safety timeout.
mi_msecs_t _mi_theap_sweep_parked(mi_subproc_t* subproc) {
  if (subproc == NULL) return 0;
  if (mi_atomic_load_relaxed(&subproc->parked_count) == 0) return 0;
  for (;;) {
    mi_tld_t* claimed = NULL;
    mi_theap_t* theap0 = NULL;
    mi_msecs_t due_in = 0;
    mi_lock(&subproc->tlds_lock) {
      const mi_msecs_t now = _mi_clock_now();
      const mi_msecs_t interval = (mi_msecs_t)mi_option_get_clamp(mi_option_purge_holes_min_interval, 0, 3600000);
      for (mi_tld_t* tld = subproc->tlds; tld != NULL; tld = tld->subproc_next) {
        if (mi_atomic_load_acquire(&tld->park_swept) != 0) continue;   // already done for this park
        if (interval > 0 && tld->holes_sweep_last != 0 && now - tld->holes_sweep_last < interval) {
          if (mi_atomic_load_relaxed(&tld->park_state) == MI_PARK_PARKED) {
            const mi_msecs_t due = interval - (now - tld->holes_sweep_last);
            if (due_in == 0 || due < due_in) { due_in = due; }
          }
          continue;
        }
        uint32_t expected = MI_PARK_PARKED;
        if (mi_atomic_cas_strong_acq_rel(&tld->park_state, &expected, MI_PARK_SWEEPING)) {
          claimed = tld; theap0 = tld->park_theap0; break;
        }
      }
    }
    if (claimed == NULL) return due_in;   // nothing parked (any more) that is due yet
    claimed->holes_sweep_last = _mi_clock_now();
    _mi_thread_idle_work(claimed, theap0);
    // Mark BEFORE releasing: a `park_swept` set after the store could land on the thread's *next*
    // park and silently skip that sweep. Cleared by `mi_on_thread_idle_start`. If we bailed out
    // early on `park_reclaim`, the owner is leaving the park anyway, so the rest is its next park's.
    mi_atomic_store_release(&claimed->park_swept, 1);
    // Back to PARKED, not RUNNING: the owner is still blocked and still owns the transition out.
    mi_atomic_store_release(&claimed->park_state, MI_PARK_PARKED);
  }
}

// Report what hole punching leaves behind (see the "Hole report" section in `page.c`).
// Same traversal and same ownership rules as `mi_purge_holes` -- every theap of this thread,
// plus the abandoned pages of the heaps behind them -- but read-only: it collects nothing,
// purges nothing, un-purges nothing, and never touches a free list.
static bool mi_theap_page_holes_report(mi_theap_t* theap, mi_page_queue_t* pq, mi_page_t* page, void* arg1, void* arg2) {
  MI_UNUSED(theap); MI_UNUSED(pq); MI_UNUSED(arg2);
  _mi_page_holes_report_page(page, (mi_holes_report_t*)arg1);
  return true; // continue
}

void _mi_purge_holes_report_collect(mi_holes_report_t* rep) {
  if (rep == NULL) return;
  _mi_memzero(rep, sizeof(*rep));
  mi_theap_t* const theap0 = _mi_theap_default();
  if (theap0 == NULL || !mi_theap_is_initialized(theap0) || theap0->tld == NULL) return;
  mi_tld_t* const tld = theap0->tld;
  if (tld->thread_id != _mi_thread_id()) return;   // owner thread only, exactly as for the sweep

  mi_heap_t* heaps[MI_PURGE_HOLES_MAX_HEAPS];
  size_t heap_count = 0;

  // hold the lock for the whole walk -- it also keeps the heaps alive, see `mi_purge_holes`
  mi_lock(&tld->theaps_lock) {
    for (mi_theap_t* theap = tld->theaps; theap != NULL; theap = theap->tnext) {
      if (!mi_theap_is_initialized(theap)) continue;
      mi_theap_visit_pages(theap, &mi_theap_page_holes_report, true /* include full pages */, rep, NULL);
      mi_heap_t* const heap = _mi_theap_heap(theap);
      if (heap != NULL && heap_count < MI_PURGE_HOLES_MAX_HEAPS) {
        bool seen = false;
        for (size_t i = 0; i < heap_count; i++) { if (heaps[i] == heap) { seen = true; break; } }
        if (!seen) { heaps[heap_count++] = heap; }
      }
    }
    for (size_t i = 0; i < heap_count; i++) {
      _mi_arenas_holes_report(heaps[i], rep);
    }
    // The committed partition is a property of the subprocess's arenas, not of a heap, so count it
    // once: every heap of this thread reaches the same arenas.
    if (heap_count > 0) { _mi_arenas_holes_committed(heaps[0], rep); }
  }
}

void mi_purge_holes_report(void) mi_attr_noexcept {
  mi_holes_report_t rep;
  _mi_purge_holes_report_collect(&rep);
  _mi_page_holes_report_print(&rep);
}

void mi_theap_collect(mi_theap_t* theap, bool force) mi_attr_noexcept {
  mi_theap_collect_ex(theap, (force ? MI_FORCE : MI_NORMAL));
}

void mi_collect(bool force) mi_attr_noexcept {
  // cannot really collect process wide, just a theap..
  mi_theap_collect(_mi_theap_default(), force);
}

void mi_heap_collect(mi_heap_t* heap, bool force) {
  // cannot really collect a heap, just a theap..
  mi_theap_collect(mi_heap_theap(heap), force);
}

/* -----------------------------------------------------------
  Heap new
----------------------------------------------------------- */

mi_theap_t* mi_theap_get_default(void) {
  mi_theap_t* theap = _mi_theap_default();
  if mi_unlikely(!mi_theap_is_initialized(theap)) {
    mi_thread_init();
    theap = _mi_theap_default();
    mi_assert_internal(mi_theap_is_initialized(theap));
  }
  return theap;
}

mi_theap_t* mi_theap_set_default(mi_theap_t* theap) {
  mi_theap_t* const previous = mi_theap_get_default();
  if (mi_theap_is_initialized(theap)) {
    _mi_theap_default_set(theap);
  }
  return previous;
}

#if MI_GUARDED
mi_decl_export void mi_theap_guarded_set_sample_rate(mi_theap_t* theap, size_t sample_rate, size_t seed) {
  theap->guarded_sample_rate  = sample_rate;
  theap->guarded_sample_count = sample_rate;  // count down samples
  if (theap->guarded_sample_rate > 1) {
    if (seed == 0) {
      seed = _mi_theap_random_next(theap);
    }
    theap->guarded_sample_count = (seed % theap->guarded_sample_rate) + 1;  // start at random count between 1 and `sample_rate`
  }
}

mi_decl_export void mi_theap_guarded_set_size_bound(mi_theap_t* theap, size_t min, size_t max) {
  theap->guarded_size_min = min;
  theap->guarded_size_max = (min > max ? min : max);
}

static void mi_theap_guarded_init(mi_theap_t* theap) {
  mi_theap_guarded_set_sample_rate(theap,
    (size_t)mi_option_get_clamp(mi_option_guarded_sample_rate, 0, LONG_MAX),
    (size_t)mi_option_get(mi_option_guarded_sample_seed));
  mi_theap_guarded_set_size_bound(theap,
    (size_t)mi_option_get_clamp(mi_option_guarded_min, 0, LONG_MAX),
    (size_t)mi_option_get_clamp(mi_option_guarded_max, 0, LONG_MAX) );
}
#else
mi_decl_export void mi_theap_guarded_set_sample_rate(mi_theap_t* theap, size_t sample_rate, size_t seed) {
  MI_UNUSED(theap); MI_UNUSED(sample_rate); MI_UNUSED(seed);
}

mi_decl_export void mi_theap_guarded_set_size_bound(mi_theap_t* theap, size_t min, size_t max) {
  MI_UNUSED(theap); MI_UNUSED(min); MI_UNUSED(max);
}
static void mi_theap_guarded_init(mi_theap_t* theap) {
  MI_UNUSED(theap);
}
#endif

static void mi_theap_options_init(mi_theap_t* theap) {
  theap->allow_page_reclaim = (mi_option_get(mi_option_page_reclaim_on_free) >= 0);
  theap->allow_page_abandon = (mi_option_get(mi_option_page_full_retain) >= 0);
  theap->page_full_retain = mi_option_get_clamp(mi_option_page_full_retain, -1, 32);
  _mi_prof_theap_init(theap);   // fork: sampling state
  theap->is_detached = (theap->tld->thread_id == MI_THREADID_DETACHED);
}

// todo: make order of parameters consistent (but would that break compat with CPython?)
void _mi_theap_init(mi_theap_t* theap, mi_heap_t* heap, mi_tld_t* tld)
{
  mi_assert_internal(theap!=NULL);
  mi_assert_internal(heap!=NULL);
  mi_assert_internal(tld!=NULL);
  mi_memid_t memid = theap->memid;
  _mi_memcpy_aligned(theap, &_mi_theap_empty, sizeof(mi_theap_t));
  theap->memid = memid;
  theap->tld   = tld;  // avoid reading the thread-local tld during initialization
  mi_atomic_store_release(&theap->refcount,1);  
  mi_atomic_store_ptr_release(mi_subproc_t,&theap->subproc,heap->subproc);
  mi_assert_internal(theap->stats.size == sizeof(mi_stats_t));
  mi_theap_options_init(theap);

  if (theap->tld->is_in_threadpool) {
    // if we run as part of a thread pool it is better to not arbitrarily reclaim abandoned pages into our theap.
    // this is checked in `free.c:mi_free_try_collect_mt`
    // .. but abandoning is good in this case: quarter the full page retain (possibly to 0)
    // (so blocked threads do not hold on to too much memory)
    if (theap->page_full_retain > 0) {
      theap->page_full_retain = theap->page_full_retain / 4;
    }
  }

  // push on the thread local theaps list
  mi_theap_t* head = NULL;
  mi_random_ctx_t head_random;
  mi_lock(&theap->tld->theaps_lock) {
    head = theap->tld->theaps;
    theap->tprev = NULL;
    theap->tnext = head;
    theap->tld->theaps = theap;
    if (head!=NULL) { 
      head->tprev = theap; 
      head_random = head->random;
    }    
  }

  // initialize random if heap==NULL
  if (head==NULL) {  // first theap of the first thread?
    #if defined(_WIN32) && !defined(MI_SHARED_LIB)
    if (tld->thread_seq==0) {
      _mi_random_init_weak(&theap->random);    // prevent allocation failure during bcrypt dll initialization with static linking (issue #1185)
    }
    else
    #endif
    {
      _mi_random_init(&theap->random);
    }
  }
  else {
    _mi_random_split(&head_random, &theap->random); // &theap->random is used as nonce so it is ok if threads capture the same head->random
  }
  theap->cookie = _mi_theap_random_next(theap) | 1;
  mi_theap_guarded_init(theap); // needs theap->random
  if (!theap->is_detached) {
    mi_subproc_stat_increase(_mi_theap_subproc(theap),theaps,1);  // on subproc to match theap_free_mem
  }

  // only now set the heap member as it is used to determine if a theap is initialized
  mi_atomic_store_ptr_release(mi_heap_t,&theap->heap,heap);
  
  // push on the heap's theap list
  mi_lock(&heap->theaps_lock) {
    head = heap->theaps;
    theap->hprev = NULL;
    theap->hnext = head;
    if (head!=NULL) { head->hprev = theap; }
    heap->theaps = theap;
  }
}

mi_theap_t* _mi_theap_alloc(mi_heap_t* heap, mi_tld_t* tld) {
  mi_assert_internal(tld!=NULL);
  mi_assert_internal(heap!=NULL);
  mi_assert_internal(tld->thread_id == MI_THREADID_DETACHED || _mi_thread_id() == tld->thread_id);
  // mi_assert_internal(_mi_heap_theap_peek(heap)==NULL);  // don't access thread locals as this is called on thread init

  // allocate and initialize a theap
  mi_memid_t memid;
  mi_theap_t* theap;
  
  if (heap->exclusive_arena == NULL) {
    theap = (mi_theap_t*)_mi_meta_zalloc(heap->subproc, sizeof(mi_theap_t), &memid);
  }
  else {
    // theaps associated with a specific arena are allocated in that arena
    // note: takes up at least one slice which is quite wasteful...
    const size_t size = _mi_align_up(sizeof(mi_theap_t),MI_ARENA_MIN_OBJ_SIZE);
    theap = (mi_theap_t*)_mi_arenas_alloc(heap, size, true, true, heap->exclusive_arena, tld->thread_seq, tld->numa_node, &memid);    
  }
  if (theap==NULL) {
    _mi_error_message(ENOMEM, "unable to allocate theap meta-data\n");
    return NULL;
  }

  theap->memid = memid;
  return theap;
}

mi_theap_t* _mi_theap_create(mi_heap_t* heap, mi_tld_t* tld) {
  mi_theap_t* theap = _mi_theap_alloc(heap,tld);
  if (theap == NULL) return NULL;
  _mi_theap_init(theap, heap, tld);
  return theap;
}

uintptr_t _mi_theap_random_next(mi_theap_t* theap) {
  return _mi_random_next(&theap->random);
}

static void mi_theap_free_mem(mi_theap_t* theap) {
  if (theap!=NULL) {
    mi_subproc_t* const subproc = mi_atomic_load_ptr_relaxed(mi_subproc_t,&theap->subproc);      
    if (!theap->is_detached) {
      mi_subproc_stat_decrease(subproc,theaps,1);  
    }
    _mi_meta_free(subproc, theap, theap->memid);
  }
}

// we need to reference count theaps due to the _mi_theap_cached thread locals
void _mi_theap_incref(mi_theap_t* theap) {
  if (theap!=NULL && !mi_memid_needs_no_free(theap->memid)) {
    mi_atomic_increment_acq_rel(&theap->refcount);
  }
}

void _mi_theap_decref(mi_theap_t* theap) {
  if (theap!=NULL && !mi_memid_needs_no_free(theap->memid)) {
    if (mi_atomic_decrement_acq_rel(&theap->refcount) == 1) {
      mi_theap_free_mem(theap);
    }
  }
}

// Thread termination and heap delete/destroy might run concurrently 
// and we need to ensure we free the memory correctly. A heap or tld
// will first "detach" its theaps so it has a list with theaps that are
// no longer shared, and only then free's the theaps in that list.
// To detach we need to hold both the `heap->theaps_lock` and the `tld->theaps_lock`.
// Due to lock-inversion we need to use `mi_lock_try_acquire` and if that fails
// we back-off, release the outer lock, and try again until we succeed.

// Remove the theaps in this heap from any thread local tld lists.
void _mi_heap_detach_theaps( mi_heap_t* heap ) {
  bool all_detached;
  do {
    all_detached = true;
    mi_lock(&heap->theaps_lock) {
      mi_theap_t* theap = heap->theaps;
      while (theap != NULL) {
        mi_theap_t* next = theap->hnext;
        mi_tld_t* tld = theap->tld; 
        if (tld != NULL) {
          if (mi_lock_try_acquire(&tld->theaps_lock)) {
            // remove the theap from the tld theaps list
            if (theap->tnext != NULL) { theap->tnext->tprev = theap->tprev;  }
            if (theap->tprev != NULL) { theap->tprev->tnext = theap->tnext;  }
                                else { mi_assert_internal(theap->tld->theaps == theap); theap->tld->theaps = theap->tnext; }
            theap->tnext = theap->tprev = NULL;       
            theap->tld = NULL;
            mi_lock_release(&tld->theaps_lock);
          }
          else {
            all_detached = false;
          }
        }
        theap = next;
      }
    }
    if (!all_detached) {
      mi_subproc_stat_counter_increase(heap->subproc,heaps_delete_wait,1);
      _mi_prim_thread_yield();
    }
  } while (!all_detached);
}

// Remove the theaps in this thread from the heaps that own them.
void _mi_tld_detach_theaps( mi_tld_t* tld ) {
  bool all_detached;
  do {
    all_detached = true;
    mi_lock(&tld->theaps_lock) {
      mi_theap_t* theap = tld->theaps;
      while (theap != NULL) {
        mi_theap_t* next = theap->tnext;
        mi_assert_internal(theap->page_count==0);
        mi_heap_t* heap = _mi_theap_heap_peek(theap); // now the heap might be NULL from an earlier iteration
        if (heap != NULL) {
          if (mi_lock_try_acquire(&heap->theaps_lock)) {
            // merge stats into the owning heap stats
            _mi_stats_merge_into(&heap->stats, &theap->stats);
            // remove the theap from the heap list
            if (theap->hnext != NULL) { theap->hnext->hprev = theap->hprev; }
            if (theap->hprev != NULL) { theap->hprev->hnext = theap->hnext; }
                                else { mi_assert_internal(heap->theaps == theap); heap->theaps = theap->hnext; }
            theap->hnext = theap->hprev = NULL;
            // and set `heap` to NULL
            mi_atomic_store_ptr_release(mi_heap_t, &theap->heap, NULL);
            mi_lock_release(&heap->theaps_lock);
          }
          else {
            all_detached = false;
          }
        }
        theap = next;
      }
    }
    if (!all_detached) {
      mi_subproc_stat_counter_increase(tld->subproc,heaps_delete_wait,1);
      _mi_prim_thread_yield();
    }
  } while (!all_detached);
}



/* -----------------------------------------------------------
  Safe theap delete
----------------------------------------------------------- */

// Safe delete a theap without freeing any still allocated blocks in that theap.
// void _mi_theap_delete(mi_theap_t* theap, bool acquire_tld_theaps_lock)
// {
//   mi_assert(theap != NULL);
//   mi_assert(mi_theap_is_initialized(theap));
//   mi_assert_expensive(mi_theap_is_valid(theap));
//   if (theap==NULL || !mi_theap_is_initialized(theap)) return;

//   // abandon all pages
//   _mi_theap_collect_abandon(theap);

//   mi_assert_internal(theap->page_count==0);
//   _mi_theap_free(theap, true /* acquire heap->theaps_lock */, acquire_tld_theaps_lock);
// }



/* -----------------------------------------------------------
  Load/unload theaps
----------------------------------------------------------- */
/*
void mi_theap_unload(mi_theap_t* theap) {
  mi_assert(mi_theap_is_initialized(theap));
  mi_assert_expensive(mi_theap_is_valid(theap));
  if (theap==NULL || !mi_theap_is_initialized(theap)) return;
  if (_mi_theap_heap(theap)->exclusive_arena == NULL) {
    _mi_warning_message("cannot unload theaps that are not associated with an exclusive arena\n");
    return;
  }

  // abandon all pages so all thread'id in the pages are cleared
  _mi_theap_collect_abandon(theap);
  mi_assert_internal(theap->page_count==0);

  // remove from theap list
  mi_theap_free(theap, false); // but don't actually free the memory

  // disassociate from the current thread-local and static state
  theap->tld = NULL;
  return;
}

bool mi_theap_reload(mi_theap_t* theap, mi_arena_id_t arena_id) {
  mi_assert(mi_theap_is_initialized(theap));
  if (theap==NULL || !mi_theap_is_initialized(theap)) return false;
  if (_mi_theap_heap(theap)->exclusive_arena == NULL) {
    _mi_warning_message("cannot reload theaps that were not associated with an exclusive arena\n");
    return false;
  }
  if (theap->tld != NULL) {
    _mi_warning_message("cannot reload theaps that were not unloaded first\n");
    return false;
  }
  mi_arena_t* arena = _mi_arena_from_id(arena_id);
  if (_mi_theap_heap(theap)->exclusive_arena != arena) {
    _mi_warning_message("trying to reload a theap at a different arena address: %p vs %p\n", _mi_theap_heap(theap)->exclusive_arena, arena);
    return false;
  }

  mi_assert_internal(theap->page_count==0);

  // re-associate with the current thread-local and static state
  theap->tld = mi_theap_get_default()->tld;

  // reinit direct pages (as we may be in a different process)
  mi_assert_internal(theap->page_count == 0);
  for (size_t i = 0; i < MI_PAGES_DIRECT; i++) {
    theap->pages_free_direct[i] = _mi_page_empty_get();
  }

  // push on the thread local theaps list
  theap->tnext = theap->tld->theaps;
  theap->tld->theaps = theap;
  return true;
}
*/


/* -----------------------------------------------------------
  Visit all theap blocks and areas
  Todo: enable visiting abandoned pages, and
        enable visiting all blocks of all theaps across threads
----------------------------------------------------------- */

void _mi_heap_area_init(mi_heap_area_t* area, mi_page_t* page) {
  const size_t bsize = mi_page_block_size(page);
  const size_t ubsize = mi_page_usable_block_size(page);
  area->reserved = page->reserved * bsize;
  area->committed = page->capacity * bsize;
  area->blocks = mi_page_start(page);
  area->used = page->used;   // number of blocks in use (#553)
  area->block_size = ubsize;
  area->full_block_size = bsize;
  area->reserved1 = page;
}

static void mi_get_fast_divisor(size_t divisor, uint64_t* magic, size_t* shift) {
  mi_assert_internal(divisor > 0 && divisor <= UINT32_MAX);
  *shift = MI_SIZE_BITS - mi_clz(divisor - 1);
  *magic = ((((uint64_t)1 << 32) * (((uint64_t)1 << *shift) - divisor)) / divisor + 1);
}

static size_t mi_fast_divide(size_t n, uint64_t magic, size_t shift) {
  mi_assert_internal(n <= UINT32_MAX);
  const uint64_t hi = ((uint64_t)n * magic) >> 32;
  return (size_t)((hi + n) >> shift);
}

bool _mi_theap_area_visit_blocks(const mi_heap_area_t* area, mi_page_t* page, mi_block_visit_fun* visitor, void* arg) {
  mi_assert(area != NULL);
  if (area==NULL) return true;
  mi_assert(page != NULL);
  if (page == NULL) return true;

  _mi_page_free_collect_no_unpurge(page,true);   // collect both thread_delayed and local_free; visiting must not un-purge a hole
  mi_assert_internal(page->local_free == NULL);
  if (page->used == 0) return true;

  size_t psize;
  uint8_t* const pstart = mi_page_area(page, &psize);
  mi_heap_t* const heap = mi_page_heap(page);
  const size_t bsize    = mi_page_block_size(page);
  const size_t ubsize   = mi_page_usable_block_size(page); // without padding

  // optimize page with one block
  if (page->capacity == 1) {
    mi_assert_internal(page->used == 1 && page->free == NULL);
    return visitor(heap, area, pstart, ubsize, arg);
  }
  mi_assert(bsize <= UINT32_MAX);

  // optimize full pages
  if (page->used == page->capacity) {
    uint8_t* block = pstart;
    for (size_t i = 0; i < page->capacity; i++) {
      if (!visitor(heap, area, block, ubsize, arg)) return false;
      block += bsize;
    }
    return true;
  }

  // create a bitmap of free blocks.
  #define MI_MAX_BLOCKS   (MI_SMALL_PAGE_SIZE / sizeof(void*))
  uintptr_t free_map[MI_MAX_BLOCKS / MI_INTPTR_BITS];
  const uintptr_t bmapsize = _mi_divide_up(page->capacity, MI_INTPTR_BITS);
  memset(free_map, 0, bmapsize * sizeof(intptr_t));
  if (page->capacity % MI_INTPTR_BITS != 0) {
    // mark left-over bits at the end as free
    size_t shift   = (page->capacity % MI_INTPTR_BITS);
    uintptr_t mask = (UINTPTR_MAX << shift);
    free_map[bmapsize - 1] = mask;
  }

  // fast repeated division by the block size
  uint64_t magic;
  size_t   shift;
  mi_get_fast_divisor(bsize, &magic, &shift);

  #if MI_DEBUG>1
  size_t free_count = 0;
  #endif
  for (mi_block_t* block = page->free; block != NULL; block = mi_block_next(page, block)) {
    #if MI_DEBUG>1
    free_count++;
    #endif
    mi_assert_internal((uint8_t*)block >= pstart && (uint8_t*)block < (pstart + psize));
    size_t offset = (uint8_t*)block - pstart;
    mi_assert_internal(offset % bsize == 0);
    mi_assert_internal(offset <= UINT32_MAX);
    size_t blockidx = mi_fast_divide(offset, magic, shift);
    mi_assert_internal(blockidx == offset / bsize);
    mi_assert_internal(blockidx < MI_MAX_BLOCKS);
    size_t bitidx = (blockidx / MI_INTPTR_BITS);
    size_t bit = blockidx - (bitidx * MI_INTPTR_BITS);
    free_map[bitidx] |= ((uintptr_t)1 << bit);
  }
  // purged blocks are free too, but held off the free list (see the hole purging section in
  // `page.c`): a block is purged exactly when it overlaps a discarded OS page of the page.
  #if MI_DEBUG>1
  size_t purged_count = 0;
  #endif
  if (mi_page_has_purged(page)) {
    for (size_t blockidx = 0; blockidx < page->capacity; blockidx++) {
      if (!mi_page_block_index_is_purged(page, blockidx)) continue;
      #if MI_DEBUG>1
      purged_count++;
      #endif
      const size_t bitidx = (blockidx / MI_INTPTR_BITS);
      const size_t bit = blockidx - (bitidx * MI_INTPTR_BITS);
      free_map[bitidx] |= ((uintptr_t)1 << bit);
    }
  }
  mi_assert_internal(page->capacity == (free_count + purged_count + page->used));

  // walk through all blocks skipping the free ones
  #if MI_DEBUG>1
  size_t used_count = 0;
  #endif
  uint8_t* block = pstart;
  for (size_t i = 0; i < bmapsize; i++) {
    if (free_map[i] == 0) {
      // every block is in use
      for (size_t j = 0; j < MI_INTPTR_BITS; j++) {
        #if MI_DEBUG>1
        used_count++;
        #endif
        if (!visitor(heap, area, block, ubsize, arg)) return false;
        block += bsize;
      }
    }
    else {
      // visit the used blocks in the mask
      uintptr_t m = ~free_map[i];
      while (m != 0) {
        #if MI_DEBUG>1
        used_count++;
        #endif
        size_t bitidx = mi_ctz(m);
        if (!visitor(heap, area, block + (bitidx * bsize), ubsize, arg)) return false;
        m &= m - 1;  // clear least significant bit
      }
      block += bsize * MI_INTPTR_BITS;
    }
  }
  mi_assert_internal(page->used == used_count);
  return true;
}

// bool _mi_page_visit_blocks( mi_page_t* page, mi_block_visit_fun* visitor, void* arg ) {
//   mi_heap_area_t area;
//   _mi_heap_area_init(&area, page);
//   return _mi_theap_area_visit_blocks(&area, page, visitor, arg);
// }


// Separate struct to keep `mi_page_t` out of the public interface
typedef struct mi_theap_area_ex_s {
  mi_heap_area_t area;
  mi_page_t* page;
} mi_theap_area_ex_t;

typedef bool (mi_theap_area_visit_fun)(const mi_theap_t* theap, const mi_theap_area_ex_t* area, void* arg);

static bool mi_theap_visit_areas_page(mi_theap_t* theap, mi_page_queue_t* pq, mi_page_t* page, void* vfun, void* arg) {
  MI_UNUSED(theap);
  MI_UNUSED(pq);
  mi_theap_area_visit_fun* fun = (mi_theap_area_visit_fun*)vfun;
  mi_theap_area_ex_t xarea;
  xarea.page = page;
  _mi_heap_area_init(&xarea.area, page);
  return fun(theap, &xarea, arg);
}

// Visit all theap pages as areas
static bool mi_theap_visit_areas(const mi_theap_t* theap, mi_theap_area_visit_fun* visitor, void* arg) {
  if (visitor == NULL) return false;
  return mi_theap_visit_pages((mi_theap_t*)theap, &mi_theap_visit_areas_page, true, (void*)(visitor), arg); // note: function pointer to void* :-{
}

// Just to pass arguments
typedef struct mi_visit_blocks_args_s {
  bool  visit_blocks;
  mi_block_visit_fun* visitor;
  void* arg;
} mi_visit_blocks_args_t;

static bool mi_theap_area_visitor(const mi_theap_t* theap, const mi_theap_area_ex_t* xarea, void* arg) {
  mi_visit_blocks_args_t* args = (mi_visit_blocks_args_t*)arg;
  if (!args->visitor(_mi_theap_heap(theap), &xarea->area, NULL, xarea->area.block_size, args->arg)) return false;
  if (args->visit_blocks) {
    return _mi_theap_area_visit_blocks(&xarea->area, xarea->page, args->visitor, args->arg);
  }
  else {
    return true;
  }
}

// Visit all blocks in a theap
bool mi_theap_visit_blocks(const mi_theap_t* theap, bool visit_blocks, mi_block_visit_fun* visitor, void* arg) {
  mi_visit_blocks_args_t args = { visit_blocks, visitor, arg };
  return mi_theap_visit_areas(theap, &mi_theap_area_visitor, &args);
}

