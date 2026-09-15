/* ----------------------------------------------------------------------------
Copyright (c) 2026-2026, Microsoft Research, Daniel Schwartz-Narbonne, Daan Leijen
This is free software; you can redistribute it and/or modify it under the
terms of the MIT license. A copy of the license can be found in the file
"LICENSE" at the root of this distribution.
-----------------------------------------------------------------------------*/

// Tests for the mimalloc heap profiler (src/profile.c).

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stddef.h>

#include "mimalloc.h"
#include "mimalloc-profile.h"
#include "mimalloc-stats.h"
#include "mimalloc/internal.h"
#include "testhelper.h"

typedef void (thread_entry_fun_t)(intptr_t tid);
static void run_os_threads(size_t nthreads, thread_entry_fun_t* entry);

// ---------------------------------------------------------------------------
// Shared callback state (not thread safe!)
// ---------------------------------------------------------------------------

typedef struct {
  mi_profiler_t profiler;
  uint64_t  alloc_count;
  uint64_t  free_count;
  size_t    last_size;
  uint64_t  last_upscaled;
  void*     last_ptr;
  // for the attribution, thread and heap-destroy tests
  size_t    threshold;            // what `on_alloc` returns as the next sample rate
  uint64_t  jitter;               // if not zero: return a random rate around `threshold` (and the state of the generator)
  uint64_t  upscaled_small;       // sum of `bytes_since_last_sample` of the samples with a size <= SMALL_MAX
  uint64_t  upscaled_large;       // .. and above
  const mi_heap_t* watch_heap;    // count the samples and frees of this heap
  uint64_t  watch_alloc_count;
  uint64_t  watch_free_count;
  bool      watch_bad_free;       // `on_free` got something that `on_alloc` did not set up
  bool      in_alloc;             // set around an allocation call: no sampled block is freed inside one
  uint64_t  free_in_alloc_count;
} my_profiler_t;

#define SMALL_MAX  1024

static inline my_profiler_t* downcast( mi_profiler_t* prof ) { 
  return (my_profiler_t*)prof; 
} 

// We store ptr in user_data so on_free can verify the round-trip.

#define TEST_THRESHOLD (16 * 1024)

static size_t mi_cdecl on_alloc(mi_profiler_t* profiler, mi_profiler_sample_data_t* data, void* ptr, size_t requested_size, size_t threshold, uint64_t bytes_since_last_sample, const mi_heap_t* heap) {
  MI_UNUSED(threshold); MI_UNUSED(heap); MI_UNUSED(requested_size);
  my_profiler_t* prof = downcast(profiler);
  assert(bytes_since_last_sample >= requested_size);  
  prof->alloc_count++;
  prof->last_ptr      = ptr;
  prof->last_size     = requested_size;
  prof->last_upscaled = bytes_since_last_sample;   
  // store ptr to verify round-trip 
  assert(data->user_data_size >= sizeof(void*));
  assert(data->user_data_size >= prof->profiler.sample_data_size);
  data->user_data[0] = ptr; 
  if (requested_size <= SMALL_MAX) { prof->upscaled_small += bytes_since_last_sample; }
                              else { prof->upscaled_large += bytes_since_last_sample; }
  if (heap == prof->watch_heap) { prof->watch_alloc_count++; }
  if (prof->jitter != 0) {
    // A fixed rate with a periodic allocation pattern always samples the same allocation of the period.
    prof->jitter = prof->jitter * 6364136223846793005ULL + 1442695040888963407ULL;
    return prof->threshold/2 + (size_t)((prof->jitter >> 33) % prof->threshold);
  }
  return prof->threshold;
}

static void mi_cdecl on_free(mi_profiler_t* profiler, mi_profiler_sample_data_t* data, void* ptr, const mi_heap_t* heap) {
  MI_UNUSED_RELEASE(data); MI_UNUSED_RELEASE(ptr);
  my_profiler_t* prof = downcast(profiler);
  prof->free_count++;
  if (prof->in_alloc) { prof->free_in_alloc_count++; }
  if (heap == prof->watch_heap) { 
    prof->watch_free_count++; 
    if (data->user_data[0] != ptr) { prof->watch_bad_free = true; }
  }
  // verify the user_data round-trip
  assert(data->user_data[0] == ptr);  
}

static my_profiler_t my_profiler = {
  { // profiler_t
    NULL,            // reserved
    sizeof(void*),   // needed sample data size
    0,               // initial sample rate (default)        
    &on_alloc,       
    &on_free,
    NULL
  },
  0, 0, 0, 0, NULL,
  TEST_THRESHOLD, 0, 0, 0, NULL, 0, 0, false, false, 0
};




// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Force at least one sample by allocating well over the threshold.
static void allocate_past_threshold(void) {
  size_t total = 0;
  while (total < TEST_THRESHOLD * 10) {
    void* p = mi_malloc(4096);
    mi_free(p);
    total += 4096;
    // mi_collect()
  }
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

bool test_profiler_samples(void) {
  CHECK_BODY("profiler: on_alloc called after threshold") {
    uint64_t before = my_profiler.alloc_count;
    allocate_past_threshold();
    result = (my_profiler.alloc_count > before);
  }
  return true;
}

#define MAXLOOP 100000

bool test_profiler_record_fields(void) {
  CHECK_BODY("profiler: record ptr and size are non-zero") {
    uint64_t before = my_profiler.alloc_count;
    int count;
    for (count = 0; my_profiler.alloc_count == before && count < MAXLOOP; count++) {
      void* p = mi_malloc(1024);
      mi_free(p);
    }
    assert(count!=MAXLOOP);    
    result = (my_profiler.last_ptr != NULL && my_profiler.last_size > 0 && my_profiler.last_upscaled > 0 && count!=MAXLOOP);
  }
  return true;
}

bool test_profiler_on_free_called(void) {
  CHECK_BODY("profiler: on_free called for sampled allocation") {
    uint64_t alloc_before = my_profiler.alloc_count;
    uint64_t free_before  = my_profiler.free_count;

    // Keep the pointer live until we confirm a sample was taken, then free it.
    void* sampled = NULL;
    int count;
    for (count = 0; my_profiler.alloc_count == alloc_before && count < MAXLOOP; count++) {
      if (sampled) { mi_free(sampled); }
      sampled = mi_malloc(1024);
    }
    // At this point my_profiler.last_ptr is the sampled pointer.
    // Free it and check on_free fires.
    void* expected = my_profiler.last_ptr;
    mi_free(expected);
    sampled = NULL;
    assert(count!=MAXLOOP);
    result = (my_profiler.free_count > free_before && count!=MAXLOOP);
  }
  return true;
}

bool test_profiler_upscaled_at_least_size(void) {
  CHECK_BODY("profiler: upscaled_size >= size") {
    uint64_t before = my_profiler.alloc_count;
    int count;
    for (count = 0; my_profiler.alloc_count == before && count < MAXLOOP; count++) {
      void* p = mi_malloc(256);
      mi_free(p);
    }
    assert(count!=MAXLOOP);
    result = (my_profiler.last_upscaled >= my_profiler.last_size && count!=MAXLOOP);
  }
  return true;
}

bool test_profiler_free_count_le_alloc_count(void) {
  CHECK_BODY("profiler: on_free never called more times than on_alloc") {
    // Free can only fire for sampled allocations, so free_count <= alloc_count
    // must hold at all times.
    allocate_past_threshold();
    result = (my_profiler.free_count <= my_profiler.alloc_count);
  }
  return true;
}


// Small allocations take the fast path and are only counted against the sample countdown when
// their page comes through the generic path. The sample must then go to a small allocation, and 
// not to the next allocation above `MI_SMALL_SIZE_MAX` (which always takes the generic path).
bool test_profiler_small_attribution(void) {
  CHECK_BODY("profiler: bytes of small allocations are attributed to small allocations") {
    const size_t small_size = 64;
    const size_t large_size = SMALL_MAX + 76;   // just over `MI_SMALL_SIZE_MAX`: generic path, a small page
    const size_t rounds     = 20000;
    const size_t small_per_round = 40;
    void** keep = (void**)mi_malloc(rounds * (small_per_round + 1) * sizeof(void*));
    size_t n = 0;
    my_profiler.threshold = 64*1024;
    my_profiler.jitter = 42;
    // let the theap pick up the new rate
    for (int i = 0; i < 2000; i++) { mi_free(mi_malloc(large_size)); }
    my_profiler.upscaled_small = 0;
    my_profiler.upscaled_large = 0;
    for (size_t r = 0; r < rounds; r++) {
      for (size_t i = 0; i < small_per_round; i++) { keep[n++] = mi_malloc(small_size); }
      keep[n++] = mi_malloc(large_size);
    }
    const double small_bytes = (double)(rounds * small_per_round * small_size);
    const double large_bytes = (double)(rounds * large_size);
    const double expected    = small_bytes / (small_bytes + large_bytes);    // 0.70
    const double total       = (double)(my_profiler.upscaled_small + my_profiler.upscaled_large);
    const double share       = (total > 0 ? (double)my_profiler.upscaled_small / total : 0.0);
    for (size_t i = 0; i < n; i++) { mi_free(keep[i]); }
    mi_free(keep);
    my_profiler.threshold = TEST_THRESHOLD;
    my_profiler.jitter = 0;
    fprintf(stderr, "  (small share %.3f, expected %.3f, sampled total %.1f MiB of %.1f MiB)\n", share, expected, total / (1024.0*1024.0), (small_bytes + large_bytes) / (1024.0*1024.0));
    // coarse sampling is not exact (the counts of a page lag behind), but without the re-check in the generic path the share is below 0.02
    result = (share > expected / 2 && share < 0.95);
    // and the samples add up to what was requested (a small block counts for its size class, which rounds up)
    result = result && (total > 0.95 * (small_bytes + large_bytes) && total < 1.15 * (small_bytes + large_bytes));
  }
  return true;
}

// A thread that starts while the profiler runs is sampled from its first allocations on.
static void new_thread_allocs(intptr_t tid) {
  MI_UNUSED(tid);
  for (int i = 0; i < 8; i++) {
    void* p = mi_malloc(1024*1024);   // far above the sample rate: each one is a sample
    mi_free(p);
  }
}

bool test_profiler_new_thread(void) {
  CHECK_BODY("profiler: a new thread is sampled from its first allocation") {
    const uint64_t before = my_profiler.alloc_count;
    run_os_threads(1, &new_thread_allocs);
    fprintf(stderr, "  (%llu samples on the new thread)\n", (unsigned long long)(my_profiler.alloc_count - before));
    result = (my_profiler.alloc_count - before >= 8);
  }
  return true;
}

// ... and its first sample does not report a period of bytes that it never requested.
static void new_thread_small_alloc(intptr_t tid) {
  MI_UNUSED(tid);
  mi_free(mi_malloc(32));
}

bool test_profiler_new_thread_no_phantom_period(void) {
  CHECK_BODY("profiler: a new thread does not start with a sample for bytes it did not request") {
    my_profiler.threshold = 512*1024;
    my_profiler.profiler.initial_sample_rate = 512*1024;   // (no other thread runs now)
    for (int i = 0; i < 2000; i++) { mi_free(mi_malloc(2000)); }   // let this theap pick up the rate
    const uint64_t before = my_profiler.upscaled_small + my_profiler.upscaled_large;
    run_os_threads(4, &new_thread_small_alloc);
    const uint64_t reported = (my_profiler.upscaled_small + my_profiler.upscaled_large) - before;
    my_profiler.profiler.initial_sample_rate = 0;
    my_profiler.threshold = TEST_THRESHOLD;
    fprintf(stderr, "  (%llu bytes reported for 4 x 32 bytes)\n", (unsigned long long)reported);
    result = (reported < 64*1024);
  }
  return true;
}

// `mi_heap_destroy` frees the blocks of a heap without a `mi_free`: the sampled ones still get `on_free`.
bool test_profiler_heap_destroy(void) {
  CHECK_BODY("profiler: mi_heap_destroy calls on_free for the sampled blocks in use") {
    mi_heap_t* heap = mi_heap_new();
    my_profiler.watch_heap = heap;
    my_profiler.watch_alloc_count = 0;
    my_profiler.watch_free_count = 0;
    my_profiler.watch_bad_free = false;
    const size_t sizes[3] = { 48, 2000, 40000 };
    for (int i = 0; i < 30000; i++) {
      // a regular block that starts with the value of the profiled tag is not a profiled block
      uintptr_t* p = (uintptr_t*)mi_heap_malloc(heap, sizes[i % 3]);
      p[0] = 1; p[1] = (uintptr_t)p;
      if (i % 7 == 0) { mi_free(p); }   // freed the normal way: reported once, not again by the destroy
    }
    const uint64_t sampled = my_profiler.watch_alloc_count;
    const uint64_t freed_before = my_profiler.watch_free_count;
    mi_heap_destroy(heap);
    my_profiler.watch_heap = NULL;
    fprintf(stderr, "  (%llu sampled, %llu freed before the destroy, %llu after)\n", (unsigned long long)sampled, (unsigned long long)freed_before, (unsigned long long)my_profiler.watch_free_count);
    result = (sampled > 100 && freed_before < sampled && my_profiler.watch_free_count == sampled && !my_profiler.watch_bad_free);
  }
  return true;
}

// A sampled block that was freed and is handed out again as a regular block is not reported by the destroy,
// also not if the program puts the value of the profiled tag in its first word and leaves the rest as it was.
bool test_profiler_heap_destroy_recycled(void) {
  CHECK_BODY("profiler: mi_heap_destroy does not report a recycled block") {
    mi_heap_t* heap = mi_heap_new();
    my_profiler.watch_heap = heap;
    my_profiler.watch_alloc_count = 0;
    my_profiler.watch_free_count = 0;
    my_profiler.watch_bad_free = false;
    void* blocks[400];
    for (int i = 0; i < 400; i++) { blocks[i] = mi_heap_malloc(heap, 40000); }   // above the sample rate: all sampled
    const uint64_t sampled = my_profiler.watch_alloc_count;
    for (int i = 0; i < 400; i++) { mi_free(blocks[i]); }
    const uint64_t freed = my_profiler.watch_free_count;
    // the same size class as the (larger) blocks that carried the sample data, written to as little as possible
    my_profiler.threshold = 64*1024*1024;
    for (int i = 0; i < 3000; i++) { mi_free(mi_malloc(2000)); }   // let this theap pick up the rate
    const uint64_t resampled_before = my_profiler.watch_alloc_count;
    for (int i = 0; i < 400; i++) {
      uintptr_t* q = (uintptr_t*)mi_heap_malloc(heap, 40000 + 2*sizeof(void*) + sizeof(mi_profiler_sample_data_t));
      q[0] = 1;
    }
    const uint64_t resampled = my_profiler.watch_alloc_count - resampled_before;
    mi_heap_destroy(heap);
    my_profiler.watch_heap = NULL;
    my_profiler.threshold = TEST_THRESHOLD;
    fprintf(stderr, "  (%llu sampled and freed, %llu sampled again, %llu reported by the destroy)\n", (unsigned long long)sampled, (unsigned long long)resampled, (unsigned long long)(my_profiler.watch_free_count - freed));
    result = (sampled >= 400 && freed == sampled && my_profiler.watch_free_count - freed == resampled && !my_profiler.watch_bad_free);
  }
  return true;
}


// Sampled blocks next to guarded ones (in a build with MI_GUARDED; both kinds make `mi_free` take its path for 
// interior pointers, and a sample is profiled or guarded, never both), with and without an alignment, leave through
// `mi_free` on this thread, `mi_free` on another thread, a `mi_heap_realloc` that moves them, and `mi_heap_destroy`. 
// Each sampled block is reported once, with the pointer `on_alloc` got (`on_free` asserts that).
#define MIXED_COUNT 6000
static void* mixed_blocks[MIXED_COUNT];

static void mixed_free_on_other_thread(intptr_t tid) {
  MI_UNUSED(tid);
  for (int i = 1; i < MIXED_COUNT; i += 4) { mi_free(mixed_blocks[i]); mixed_blocks[i] = NULL; }
}

bool test_profiler_guarded_mixed(void) {
  CHECK_BODY("profiler: sampled, guarded and aligned blocks through free, realloc, another thread and mi_heap_destroy") {
    const long guarded_rate = mi_option_get(mi_option_guarded_sample_rate);
    mi_option_set(mi_option_guarded_sample_rate, 1024);   // picked up by the theaps of the new heap and thread
    my_profiler.threshold = 16*1024;
    mi_heap_t* heap = mi_heap_new();
    my_profiler.watch_heap = heap;
    my_profiler.watch_alloc_count = 0;
    my_profiler.watch_free_count = 0;
    my_profiler.watch_bad_free = false;
    const size_t sizes[5] = { 40, 700, 3000, 20000, 70000 };
    const size_t alignments[3] = { 64, 1024, 16384 };
    size_t guarded = 0;
    for (int i = 0; i < MIXED_COUNT; i++) {
      const size_t size = sizes[i % 5];
      mixed_blocks[i] = (i % 3 == 0 ? mi_heap_malloc_aligned(heap, size, alignments[(i / 3) % 3]) : mi_heap_malloc(heap, size));
      memset(mixed_blocks[i], 1, (size < 64 ? size : 64));
    }
    const uint64_t sampled = my_profiler.watch_alloc_count;
    #if MI_GUARDED
    mi_stats_t stats; mi_stats_init(&stats);
    if (mi_heap_stats_get(heap, &stats)) { guarded = (size_t)stats.malloc_guarded_count.total; }
    #endif
    for (int i = 0; i < MIXED_COUNT; i += 4) { mi_free(mixed_blocks[i]); mixed_blocks[i] = NULL; }
    run_os_threads(1, &mixed_free_on_other_thread);
    for (int i = 2; i < MIXED_COUNT; i += 4) {
      mixed_blocks[i] = mi_heap_realloc(heap, mixed_blocks[i], 2*sizes[i % 5] + 100000);   // moves: a new block (that may be sampled) and a free
    }
    const uint64_t freed_before = my_profiler.watch_free_count;
    const uint64_t sampled_all = my_profiler.watch_alloc_count;
    mi_heap_destroy(heap);   // the reallocated ones and every fourth of the first ones
    my_profiler.watch_heap = NULL;
    my_profiler.threshold = TEST_THRESHOLD;
    mi_option_set(mi_option_guarded_sample_rate, guarded_rate);
    fprintf(stderr, "  (%llu of %d sampled, %zu guarded, %llu sampled with the moved ones; %llu freed before the destroy, %llu after)\n", (unsigned long long)sampled, MIXED_COUNT, guarded, (unsigned long long)sampled_all, (unsigned long long)freed_before, (unsigned long long)my_profiler.watch_free_count);
    #if MI_GUARDED
    if (guarded < 100) { result = false; }
    else
    #endif
    result = (sampled > 500 && freed_before > sampled/2 && freed_before < sampled_all && my_profiler.watch_free_count == sampled_all && !my_profiler.watch_bad_free);
  }
  return true;
}


// An aligned allocation that its size class aligns by itself is first tried as a plain allocation, of which the
// start of a block is expected. When the countdown is used up inside it (a page refill, a collection) it must not be
// made a sample there: the sampled block would be freed again inside the allocator, and the allocation that is
// returned would never be one. Samples of aligned allocations stay until the program frees them, and there are as
// many as the bytes call for, also with other allocations in between.
#define ALIGNED_COUNT 120000
static void* aligned_blocks[ALIGNED_COUNT];

bool test_profiler_aligned(void) {
  CHECK_BODY("profiler: samples of aligned allocations live until they are freed") {
    const size_t rate = 64*1024;
    // the size class aligns the first five by itself (three of them also with the padding of a debug build);
    // the last one is above the small object sizes
    const size_t sizes[6]      = { 64, 256, 120, 1016, 48, 12288 };
    const size_t alignments[6] = { 64, 256,  64,  512, 16,  4096 };
    my_profiler.threshold = rate;
    mi_heap_t* heap = mi_heap_new();
    for (int i = 0; i < 3000; i++) { mi_free(mi_heap_malloc(heap, 2000)); }   // let its theap pick up the rate
    my_profiler.watch_heap = heap;
    my_profiler.watch_alloc_count = 0;
    my_profiler.watch_free_count = 0;
    my_profiler.watch_bad_free = false;
    my_profiler.free_in_alloc_count = 0;
    size_t misaligned = 0;
    uint64_t aligned_samples = 0;
    uint64_t aligned_bytes = 0;
    uint64_t freed_early = 0;
    for (int i = 0; i < ALIGNED_COUNT; i++) {
      const int k = (i / 500) % 6;   // runs of one size, so that pages fill up and are refilled
      const size_t size = (k == 5 && (i % 50) != 0 ? sizes[0] : sizes[k]);   // (a large one now and then)
      const size_t alignment = (size == sizes[k] ? alignments[k] : alignments[0]);
      const uint64_t before = my_profiler.watch_alloc_count;
      my_profiler.in_alloc = true;
      aligned_blocks[i] = mi_heap_malloc_aligned(heap, size, alignment);
      my_profiler.in_alloc = false;
      aligned_samples += (my_profiler.watch_alloc_count - before);
      if (((uintptr_t)aligned_blocks[i] % alignment) != 0) { misaligned++; }
      aligned_bytes += size;
      // other allocations in between (every tenth round many, to come by the periodic collection as well)
      const uint64_t freed_before = my_profiler.watch_free_count;
      const uint64_t sampled_before = my_profiler.watch_alloc_count;
      for (int j = 0; j < ((i % 10) == 0 ? 12 : 2); j++) { mi_free(mi_heap_malloc(heap, 32 + 8*(size_t)j)); }
      freed_early += (my_profiler.watch_free_count - freed_before) - (my_profiler.watch_alloc_count - sampled_before);  // an aligned block's sample freed here: never
    }
    const uint64_t sampled = my_profiler.watch_alloc_count;
    const uint64_t freed_mid = my_profiler.watch_free_count;
    for (int i = 0; i < ALIGNED_COUNT; i++) { mi_free(aligned_blocks[i]); }
    const uint64_t freed = my_profiler.watch_free_count;
    mi_heap_destroy(heap);
    my_profiler.watch_heap = NULL;
    my_profiler.threshold = TEST_THRESHOLD;
    const uint64_t expected = aligned_bytes / rate;
    fprintf(stderr, "  (%llu samples of aligned allocations for %llu expected, %llu samples in all, %llu freed inside an allocation, %llu of an aligned one freed early, %llu freed when the program freed them, %zu misaligned)\n",
      (unsigned long long)aligned_samples, (unsigned long long)expected, (unsigned long long)sampled, (unsigned long long)my_profiler.free_in_alloc_count,
      (unsigned long long)freed_early, (unsigned long long)(freed - freed_mid), misaligned);
    result = (misaligned == 0 && my_profiler.free_in_alloc_count == 0 && freed_early == 0 && freed == sampled && !my_profiler.watch_bad_free
              && (freed - freed_mid) == aligned_samples    // each is there until the program frees it
              && aligned_samples > expected / 2 && aligned_samples < expected * 3);  // (block bytes count, not requested ones)
  }
  return true;
}


// `mi_profiler_stop` and `mi_profiler_start` on one thread while another one is being sampled.
static _Atomic(size_t) toggle_done;
static _Atomic(size_t) toggle_samples;
static size_t mi_cdecl toggle_on_alloc(mi_profiler_t* profiler, mi_profiler_sample_data_t* data, void* ptr, size_t requested_size, size_t threshold, uint64_t bytes_since_last_sample, const mi_heap_t* heap) {
  MI_UNUSED(profiler); MI_UNUSED(data); MI_UNUSED(ptr); MI_UNUSED(requested_size); MI_UNUSED(threshold); MI_UNUSED(bytes_since_last_sample); MI_UNUSED(heap);
  mi_atomic_increment_relaxed(&toggle_samples);
  return 1024;
}
static mi_profiler_t toggle_profiler = { NULL, 0, 1024, &toggle_on_alloc, NULL, NULL };

static void toggle_thread(intptr_t tid) {
  if (tid == 0) {
    for (int i = 0; i < 300000; i++) { mi_free(mi_malloc(2048)); }   // each one is above the rate: a sample when the profiler runs
    mi_atomic_store_release(&toggle_done, (size_t)1);
  }
  else {
    while (mi_atomic_load_acquire(&toggle_done) == 0) {
      mi_profiler_stop(&toggle_profiler);
      mi_profiler_start(&toggle_profiler);
    }
  }
}

bool test_profiler_stop_while_sampling(void) {
  CHECK_BODY("profiler: stop and start while another thread is sampled") {
    mi_profiler_stop(&my_profiler.profiler);
    mi_profile(NULL);
    mi_profile(&toggle_profiler);
    mi_profiler_start(&toggle_profiler);
    run_os_threads(2, &toggle_thread);
    mi_profiler_stop(&toggle_profiler);
    mi_profile(NULL);
    mi_profile(&my_profiler.profiler);
    mi_profiler_start(&my_profiler.profiler);
    result = (mi_atomic_load_relaxed(&toggle_samples) > 0);   // (an assertion failed in a debug build)
  }
  return true;
}


// The same for a sample that comes due when the theap picks up the profiler inside the allocation (a theap that was
// there before the profiler started does so once in a while in the generic path, which sets the countdown to at most
// the rate): with a request above the rate that is a sample straight away, also with fine-grained sampling.
bool test_profiler_aligned_late_start(void) {
  CHECK_BODY("profiler: aligned allocations when their theap picks up the profiler") {
    mi_profiler_stop(&my_profiler.profiler);
    mi_heap_t* heap = mi_heap_new();
    mi_free(mi_heap_malloc(heap, 2000));   // its theap is there now, without a profiler
    my_profiler.threshold = 64*1024;
    my_profiler.watch_heap = heap;
    my_profiler.watch_alloc_count = 0;
    my_profiler.watch_free_count = 0;
    my_profiler.watch_bad_free = false;
    my_profiler.free_in_alloc_count = 0;
    mi_profiler_start(&my_profiler.profiler);
    size_t misaligned = 0;
    for (int i = 0; i < 3000; i++) {
      my_profiler.in_alloc = true;
      void* p = mi_heap_malloc_aligned(heap, 128*1024, 4096);
      my_profiler.in_alloc = false;
      if (((uintptr_t)p % 4096) != 0) { misaligned++; }
      mi_free(p);
    }
    const uint64_t sampled = my_profiler.watch_alloc_count;
    const uint64_t freed = my_profiler.watch_free_count;
    mi_heap_destroy(heap);
    my_profiler.watch_heap = NULL;
    my_profiler.threshold = TEST_THRESHOLD;
    fprintf(stderr, "  (%llu samples, %llu freed, %llu freed inside an allocation, %zu misaligned)\n",
      (unsigned long long)sampled, (unsigned long long)freed, (unsigned long long)my_profiler.free_in_alloc_count, misaligned);
    result = (misaligned == 0 && my_profiler.free_in_alloc_count == 0 && freed == sampled && !my_profiler.watch_bad_free
              && sampled >= 1000);   // each one after the theap picked it up
  }
  return true;
}


// A profiler that is started while threads are allocating: `mi_profiler_start` asks their theaps to look at the
// profiler at their next generic allocation. Every thread gets its first sample after about one period of what it
// requests from then on: not many megabytes later (when its theap looks by itself), and not at once for what its
// pages handed out before (a theap that does not sample does not count that).
#define LATE_THREADS  4
#define LATE_RATE     (256*1024)
static _Atomic(size_t) late_warm;                  // threads that are done with their allocations before the start
static _Atomic(size_t) late_started;
static _Atomic(size_t) late_first[LATE_THREADS];   // what a thread requested after the start, up to its first sample
static mi_decl_thread size_t   late_requested;
static mi_decl_thread intptr_t late_tid = -1;

static size_t mi_cdecl late_on_alloc(mi_profiler_t* profiler, mi_profiler_sample_data_t* data, void* ptr, size_t requested_size, size_t threshold, uint64_t bytes_since_last_sample, const mi_heap_t* heap) {
  MI_UNUSED(profiler); MI_UNUSED(data); MI_UNUSED(ptr); MI_UNUSED(threshold); MI_UNUSED(bytes_since_last_sample); MI_UNUSED(heap);
  if (late_tid >= 0 && mi_atomic_load_relaxed(&late_first[late_tid]) == 0) {
    mi_atomic_store_release(&late_first[late_tid], late_requested + requested_size);
  }
  return LATE_RATE;
}

static mi_profiler_t late_profiler = { NULL, 0, LATE_RATE, &late_on_alloc, NULL, NULL };

static size_t late_step(void** slots, uint64_t* r) {   // free a slot that is in use, or fill it; returns the size it allocated
  *r ^= *r << 13; *r ^= *r >> 7; *r ^= *r << 17;
  const size_t slot = (size_t)(*r >> 32) % 256;
  if (slots[slot] != NULL) { mi_free(slots[slot]); slots[slot] = NULL; return 0; }
  const size_t size = 16 + (size_t)(*r % 1024);
  slots[slot] = mi_malloc(size);
  return size;
}

static void late_thread(intptr_t tid) {
  if (tid == LATE_THREADS) {
    // the one that starts the profiler, once the others have allocated for a while
    while (mi_atomic_load_acquire(&late_warm) < LATE_THREADS) { mi_atomic_pause(); }
    mi_profiler_start(&late_profiler);
    mi_atomic_store_release(&late_started, (size_t)1);
    return;
  }
  void* slots[256];
  memset(slots, 0, sizeof(slots));
  uint64_t r = 0x9E3779B97F4A7C15ull + (uint64_t)tid;
  for (size_t allocs = 0; allocs < 250000; ) { if (late_step(slots,&r) != 0) { allocs++; } }
  mi_atomic_increment_acq_rel(&late_warm);
  while (mi_atomic_load_acquire(&late_started) == 0) { mi_atomic_pause(); }
  late_tid = tid;
  late_requested = 0;
  while (mi_atomic_load_relaxed(&late_first[tid]) == 0 && late_requested < 16*LATE_RATE) {
    late_requested += late_step(slots,&r);
  }
  late_tid = -1;
  for (size_t i = 0; i < 256; i++) { mi_free(slots[i]); }
}

bool test_profiler_start_with_running_threads(void) {
  CHECK_BODY("profiler: threads that are allocating when the profiler starts are sampled from then on") {
    mi_profiler_stop(&my_profiler.profiler);
    mi_profile(NULL);
    mi_profile(&late_profiler);
    run_os_threads(LATE_THREADS + 1, &late_thread);
    mi_profiler_stop(&late_profiler);
    mi_profile(NULL);
    mi_profile(&my_profiler.profiler);
    result = true;
    fprintf(stderr, "  (first sample after");
    for (size_t i = 0; i < LATE_THREADS; i++) {
      const size_t first = mi_atomic_load_acquire(&late_first[i]);
      fprintf(stderr, " %zu", first);
      result = result && (first >= LATE_RATE/2 && first <= 2*LATE_RATE);
    }
    fprintf(stderr, " bytes, at a rate of %d)\n", LATE_RATE);
  }
  return true;
}


// A thread that was sampled before a stop, and that allocates nothing between the stop and the next start, starts
// over with that start: it does not run out the period it was in (at the rate it had then), and its first sample does
// not report what it requested before the start.
#define RESTART_BLOCK       (64*1024)
#define RESTART_LONG_RATE   ((size_t)256*1024*1024)
#define RESTART_SHORT_RATE  ((size_t)64*1024)
#define RESTART_EVEN_RATE   ((size_t)1024*1024)
static _Atomic(size_t) restart_phase;       // the worker does phase 1, 3, 5, 7 when it sees that number; the other thread does the even ones
static _Atomic(size_t) restart_rate;        // what `on_alloc` returns
static _Atomic(size_t) restart_samples;     // of the worker, since the last start
static _Atomic(size_t) restart_bytes;       // sum of their `bytes_since_last_sample`
static _Atomic(size_t) restart_first;       // `bytes_since_last_sample` of the first one
static _Atomic(size_t) restart_first_after; // what the worker had requested since the start when it came
static mi_decl_thread size_t restart_requested;
static mi_decl_thread bool   restart_is_worker;

static size_t mi_cdecl restart_on_alloc(mi_profiler_t* profiler, mi_profiler_sample_data_t* data, void* ptr, size_t requested_size, size_t threshold, uint64_t bytes_since_last_sample, const mi_heap_t* heap) {
  MI_UNUSED(profiler); MI_UNUSED(data); MI_UNUSED(ptr); MI_UNUSED(threshold); MI_UNUSED(heap);
  if (restart_is_worker) {
    if (mi_atomic_increment_relaxed(&restart_samples) == 0) {
      mi_atomic_store_relaxed(&restart_first, (size_t)bytes_since_last_sample);
      mi_atomic_store_relaxed(&restart_first_after, restart_requested + requested_size);
    }
    mi_atomic_add_relaxed(&restart_bytes, (size_t)bytes_since_last_sample);
  }
  return mi_atomic_load_relaxed(&restart_rate);
}

static mi_profiler_t restart_profiler = { NULL, 0, RESTART_EVEN_RATE, &restart_on_alloc, NULL, NULL };

static void restart_wait(size_t phase) {
  while (mi_atomic_load_acquire(&restart_phase) != phase) { mi_atomic_pause(); }
}

static void restart_allocate(size_t total) {
  restart_requested = 0;
  while (restart_requested < total) {
    mi_free(mi_malloc(RESTART_BLOCK));   // (through the generic path each time)
    restart_requested += RESTART_BLOCK;
  }
}

static void restart_begin(size_t rate) {
  mi_atomic_store_relaxed(&restart_rate, rate);
  mi_atomic_store_relaxed(&restart_samples, (size_t)0);
  mi_atomic_store_relaxed(&restart_bytes, (size_t)0);
  mi_atomic_store_relaxed(&restart_first, (size_t)0);
  mi_atomic_store_relaxed(&restart_first_after, (size_t)0);
  mi_profiler_start(&restart_profiler);
}

static size_t restart_short_bytes, restart_even_first, restart_even_first_after;

static void restart_thread(intptr_t tid) {
  if (tid == 0) {
    restart_is_worker = true;
    restart_wait(1); restart_allocate(RESTART_EVEN_RATE*3/2);      // ends half way into a period
    mi_atomic_store_release(&restart_phase, (size_t)2);
    restart_wait(3); restart_allocate(RESTART_EVEN_RATE*4);
    mi_atomic_store_release(&restart_phase, (size_t)4);
    restart_wait(5); restart_allocate(32*1024*1024);               // one sample, which sets the long rate
    mi_atomic_store_release(&restart_phase, (size_t)6);
    restart_wait(7); restart_allocate(32*1024*1024);               // at the short rate: all of it is sampled
    mi_atomic_store_release(&restart_phase, (size_t)8);
    restart_is_worker = false;
  }
  else {
    restart_begin(RESTART_EVEN_RATE);
    mi_atomic_store_release(&restart_phase, (size_t)1);
    restart_wait(2);
    mi_profiler_stop(&restart_profiler);
    restart_begin(RESTART_EVEN_RATE);
    mi_atomic_store_release(&restart_phase, (size_t)3);
    restart_wait(4);
    mi_profiler_stop(&restart_profiler);
    restart_even_first = mi_atomic_load_relaxed(&restart_first);
    restart_even_first_after = mi_atomic_load_relaxed(&restart_first_after);
    restart_begin(RESTART_LONG_RATE);
    mi_atomic_store_release(&restart_phase, (size_t)5);
    restart_wait(6);
    mi_profiler_stop(&restart_profiler);
    restart_begin(RESTART_SHORT_RATE);
    mi_atomic_store_release(&restart_phase, (size_t)7);
    restart_wait(8);
    mi_profiler_stop(&restart_profiler);
    restart_short_bytes = mi_atomic_load_relaxed(&restart_bytes);
  }
}

bool test_profiler_restart(void) {
  CHECK_BODY("profiler: a thread starts over when the profiler is started again") {
    mi_profiler_stop(&my_profiler.profiler);
    mi_profile(NULL);
    mi_profile(&restart_profiler);
    run_os_threads(2, &restart_thread);
    mi_profile(NULL);
    mi_profile(&my_profiler.profiler);
    mi_profiler_start(&my_profiler.profiler);
    fprintf(stderr, "  (after a stop half way into a period: the first sample reports %zu bytes, %zu requested since the start; after a period at a rate of %zu, at a rate of %zu: %zu of %d bytes sampled)\n",
      restart_even_first, restart_even_first_after, RESTART_LONG_RATE, RESTART_SHORT_RATE, restart_short_bytes, 32*1024*1024);
    result = (restart_short_bytes >= 31*1024*1024 && restart_short_bytes <= 33*1024*1024 &&
              restart_even_first != 0 && restart_even_first <= restart_even_first_after);
  }
  return true;
}


// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(void) {
  mi_profile(&my_profiler.profiler);
  mi_profiler_start(&my_profiler.profiler);

  test_profiler_upscaled_at_least_size();
  test_profiler_samples();
  test_profiler_record_fields();
  test_profiler_on_free_called();
  test_profiler_free_count_le_alloc_count();
  test_profiler_small_attribution();
  test_profiler_new_thread();
  test_profiler_new_thread_no_phantom_period();
  test_profiler_heap_destroy();
  test_profiler_heap_destroy_recycled();
  test_profiler_guarded_mixed();
  test_profiler_aligned();
  test_profiler_stop_while_sampling();
  test_profiler_aligned_late_start();
  test_profiler_start_with_running_threads();
  test_profiler_restart();

  mi_profiler_stop(&my_profiler.profiler);

  return print_test_summary();
}


// ---------------------------------------------------------------------------
// Portable threads (as in test-heap-mt.c)
// ---------------------------------------------------------------------------

static thread_entry_fun_t* thread_entry_fun;

#ifdef _WIN32
#include <windows.h>

static DWORD WINAPI thread_entry(LPVOID param) {
  thread_entry_fun((intptr_t)param);
  return 0;
}

static void run_os_threads(size_t nthreads, thread_entry_fun_t* fun) {
  thread_entry_fun = fun;
  HANDLE thandles[16];
  if (nthreads > 16) { nthreads = 16; }
  for (size_t i = 0; i < nthreads; i++) {
    DWORD tid;
    thandles[i] = CreateThread(0, 64*1024L, &thread_entry, (void*)(i), 0, &tid);
  }
  for (size_t i = 0; i < nthreads; i++) {
    WaitForSingleObject(thandles[i], INFINITE);
    CloseHandle(thandles[i]);
  }
}

#else
#include <pthread.h>

static void* thread_entry(void* param) {
  thread_entry_fun((intptr_t)param);
  return NULL;
}

static void run_os_threads(size_t nthreads, thread_entry_fun_t* fun) {
  thread_entry_fun = fun;
  pthread_t threads[16];
  if (nthreads > 16) { nthreads = 16; }
  for (size_t i = 0; i < nthreads; i++) {
    pthread_create(&threads[i], NULL, &thread_entry, (void*)i);
  }
  for (size_t i = 0; i < nthreads; i++) {
    pthread_join(threads[i], NULL);
  }
}
#endif
