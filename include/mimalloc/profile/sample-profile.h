/* ----------------------------------------------------------------------------
Copyright (c) 2018-2026, Microsoft Research, Daan Leijen
This is free software; you can redistribute it and/or modify it under the
terms of the MIT license. A copy of the license can be found in the file
"LICENSE" at the root of this distribution.
-----------------------------------------------------------------------------*/
#pragma once
#ifndef MI_INTERNAL_SAMPLE_PROFILE_H
#define MI_INTERNAL_SAMPLE_PROFILE_H

#include "../types.h"

mi_decl_restrict void* _mi_theap_malloc_sampled(mi_theap_t* theap, size_t req_size, bool zero, mi_page_t** ppage) mi_attr_noexcept;
size_t        _mi_theap_update_sample_rate(mi_theap_t* theap);
#if MI_SAMPLE==1
void          _mi_theap_sync_sample_counts(mi_theap_t* theap);
#endif
void          _mi_theap_start_profile_period(mi_theap_t* theap, const mi_profiler_t* prof, size_t prof_state);
void          _mi_theap_update_profiling(mi_theap_t* theap);
bool          _mi_theap_profiling_is_stale(const mi_theap_t* theap, size_t prof_state);

mi_decl_restrict void* _mi_theap_malloc_profiled(mi_theap_t* theap, size_t size, uint64_t requested_since_last_sample, bool zero, mi_page_t** ppage) mi_attr_noexcept;
void          _mi_page_profile_on_free(mi_page_t* page, mi_block_t* block, void* p);
void          _mi_page_profile_free_all(const mi_heap_area_t* area, mi_page_t* page);
size_t        _mi_theap_set_profile_sample_rate(mi_theap_t* theap, size_t sample_rate);

// permanently exclude a theap from profiling (mirrors `mi_heap_profile_disable` but at the theap level);
// used for detached/meta theaps used to bootstrap thread/theap metadata, since sampling those can call
// back into the profiler while allocating on a not yet (re-)initialized thread, causing deadlock.
static inline void mi_theap_profile_never(mi_theap_t* theap) {
  theap->profile_never = true;
  theap->profile_sample_rate = 0;
  theap->profile_sample_countdown = 0;
}

/* -------------------------------------------------------------------
  Guarded and profiled objects
------------------------------------------------------------------- */

#define MI_SAMPLE_RATE_MAX        (SIZE_MAX/4)
#define MI_SAMPLE_COUNTDOWN_MAX   (MI_MAX_ALLOC_SIZE)

// `_mi_malloc_generic` takes its fast path for this many calls in a row, and then `mi_malloc_generic_admin` runs.
#define MI_GENERIC_FAST_LIMIT   (1000)

static inline bool mi_theap_should_sample(mi_theap_t* theap, size_t req_size) {
  // note: this should return `true` on an empty theap so we initialize it's countdown to `-1`.
  mi_assert_internal(req_size <= SIZE_MAX/2);
  // const size_t sample_countdown = theap->sample_countdown - req_size;
  // return ((mi_ssize_t)sample_countdown < 0);
  return ((mi_ssize_t)theap->sample_countdown < (mi_ssize_t)req_size);
}

// we always align guarded pointers in a block at an offset
// the block `next` field is then used as a tag to distinguish regular offset aligned blocks from guarded ones
#define MI_BLOCK_TAG_ALIGNED   ((mi_encoded_t)(0))
#define MI_BLOCK_TAG_PROFILED  ((mi_encoded_t)(1))


static inline bool mi_block_ptr_is_sampled(const mi_block_t* block, const void* p) {
#if MI_GUARDED || MI_PROFILE
  const ptrdiff_t offset = (uint8_t*)p - (uint8_t*)block;
  return (offset >= (ptrdiff_t)(sizeof(mi_block_t)) && block->next != MI_BLOCK_TAG_ALIGNED);
#else
  MI_UNUSED(block); MI_UNUSED(p);
  return false;
#endif
}

// `mi_profiler_t.reserved` is `(epoch << 1) | enabled`. Every start of a profiler that is not running takes a new
// epoch from a counter of the process (so two profilers never have the same one): a theap keeps the epoch that its
// profile rate and countdowns belong to, and state from before a stop is not taken into the next start
// (see `_mi_theap_update_profiling`). One word, so enabled and epoch are read together.
extern mi_decl_hidden _Atomic(size_t) _mi_profiler_epoch;   // sample-profile.c

static inline size_t mi_profiler_state(const mi_profiler_t* prof) {
  _Atomic(size_t)* pstate = (_Atomic(size_t)*)&prof->reserved;
  return mi_atomic_load_acquire(pstate);
}

static inline bool mi_profiler_state_is_enabled(size_t state) {
  return ((state & 1) != 0);
}

static inline size_t mi_profiler_state_epoch(size_t state) {
  return (state >> 1);
}

static inline bool mi_profiler_is_enabled(const mi_profiler_t* prof) {
  return mi_profiler_state_is_enabled(mi_profiler_state(prof));
}

// Returns whether it was enabled before.
static inline bool mi_profiler_set_enabled(mi_profiler_t* prof, bool enable) {
  _Atomic(size_t)* pstate = (_Atomic(size_t)*)&prof->reserved;
  size_t state = mi_atomic_load_relaxed(pstate);
  size_t epoch = 0;
  size_t desired;
  do {
    if (mi_profiler_state_is_enabled(state) == enable) return enable;
    if (enable && epoch==0) { epoch = mi_atomic_increment_relaxed(&_mi_profiler_epoch) + 1; }
    desired = (enable ? ((epoch << 1) | 1) : (state & ~(size_t)1));
  } while (!mi_atomic_cas_weak_acq_rel(pstate, &state, desired));
  return !enable;
}

#endif
