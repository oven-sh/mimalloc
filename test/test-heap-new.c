/* ----------------------------------------------------------------------------
Copyright (c) 2026, Microsoft Research, Daan Leijen
This is free software; you can redistribute it and/or modify it under the
terms of the MIT license.
-----------------------------------------------------------------------------*/

// Tests for heap creation and destruction:
// lazy heap statistics, the sharded heap list, thread-local key reuse,
// release of meta-data pages, and a multi-threaded stress test with mixed sizes.

#if defined(_WIN32)
#include <stdio.h>
int main(void) {
  printf("test-heap-new: skipped on Windows (uses pthreads)\n");
  return 0;
}
#else

#include <mimalloc.h>
#include <mimalloc-stats.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failed = 0;

#define CHECK(cond) \
  do { \
    if (!(cond)) { \
      fprintf(stderr, "\n  FAILED: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
      failed++; \
    } \
  } while (0)

// Get the statistics of a heap, or of the whole process if `heap` is NULL.
static mi_stats_t get_stats(mi_heap_t* heap) {
  mi_stats_t stats;
  mi_stats_init(&stats);
  CHECK(heap == NULL ? mi_stats_get(&stats) : mi_heap_stats_get(heap, &stats));
  return stats;
}

static void heap_alloc(mi_heap_t* heap, size_t size) {
  void* p = mi_heap_malloc(heap, size);
  CHECK(p != NULL);
}


// --- statistics of a live heap

static void* alloc_three_pages(void* arg) {
  mi_heap_t* heap = (mi_heap_t*)arg;
  heap_alloc(heap, 40);
  heap_alloc(heap, 400);
  heap_alloc(heap, 4000);
  return NULL;
}

static void test_live_stats(void) {
  mi_heap_t* unused = mi_heap_new();
  mi_heap_t* used = mi_heap_new();

  mi_stats_t stats = get_stats(unused);
  CHECK(stats.pages.total == 0);
  CHECK(stats.theaps.total == 0);

  heap_alloc(used, 64);
  heap_alloc(used, 2000);
  // (no exact page counts: guarded sampling can move a block to another size class)
  stats = get_stats(used);
  const int64_t own_pages = stats.pages.current;
  CHECK(own_pages >= 1 && own_pages <= 2);
  CHECK(stats.theaps.current == 1);

  stats = get_stats(unused);
  CHECK(stats.pages.total == 0);

  // a thread merges its statistics into the heap when it exits
  pthread_t thread;
  pthread_create(&thread, NULL, &alloc_three_pages, used);
  pthread_join(thread, NULL);
  stats = get_stats(used);
  CHECK(stats.pages.current > own_pages);
  CHECK(stats.pages_abandoned.current >= 1);

  mi_heap_destroy(used);
  mi_heap_destroy(unused);
}


// --- statistics of destroyed heaps end up in the process statistics

static void test_destroyed_stats(void) {
  static const size_t sizes[] = { 16, 100, 700, 3000, 12000, 100000 };
  const int size_count = (int)(sizeof(sizes) / sizeof(sizes[0]));
  const int heap_count = 100;

  const mi_stats_t before = get_stats(NULL);
  for (int i = 0; i < heap_count; i++) {
    mi_heap_t* heap = mi_heap_new();
    for (int j = 0; j < size_count; j++) {
      heap_alloc(heap, sizes[j]);
    }
    if (i % 2 == 0) {
      // reading the statistics allocates them: cover heaps with and without
      const int64_t pages = get_stats(heap).pages.current;
      CHECK(pages >= 1 && pages <= size_count);
    }
    mi_heap_destroy(heap);
  }
  const mi_stats_t after = get_stats(NULL);

  CHECK(after.heaps.total - before.heaps.total == heap_count);
  CHECK(after.heaps.current == before.heaps.current);
  CHECK(after.theaps.total - before.theaps.total == heap_count);
  CHECK(after.theaps.current == before.theaps.current);
  CHECK(after.pages.total - before.pages.total >= heap_count);
  CHECK(after.pages.peak > before.pages.current);
  CHECK(after.malloc_normal_count.total - before.malloc_normal_count.total >= heap_count);
  CHECK(after.malloc_normal.current <= before.malloc_normal.current + 64 * 1024);
}


// --- every heap is visited exactly once, whichever thread created it

#define MAKERS 40  // more threads than shards
#define HEAPS_PER_MAKER 5
#define MADE_COUNT (MAKERS * HEAPS_PER_MAKER)

static mi_heap_t* made[MADE_COUNT];

static void* make_heaps(void* arg) {
  mi_heap_t** heaps = (mi_heap_t**)arg;
  for (int i = 0; i < HEAPS_PER_MAKER; i++) {
    heaps[i] = mi_heap_new();
    if (i % 2 == 0) {
      heap_alloc(heaps[i], 24);
    }
  }
  return NULL;
}

typedef struct visit_count_s {
  size_t total;
  size_t made;
  size_t main;
} visit_count_t;

static bool count_heap(mi_heap_t* heap, void* arg) {
  visit_count_t* count = (visit_count_t*)arg;
  count->total++;
  if (heap == mi_heap_main()) {
    count->main++;
  }
  for (int i = 0; i < MADE_COUNT; i++) {
    if (made[i] == heap) {
      count->made++;
    }
  }
  return true;
}

static visit_count_t visit_heaps(void) {
  visit_count_t count = { 0, 0, 0 };
  mi_subproc_visit_heaps(mi_subproc_current(), &count_heap, &count);
  return count;
}

static int compare_size(const void* a, const void* b) {
  const size_t x = *(const size_t*)a;
  const size_t y = *(const size_t*)b;
  return (x > y) - (x < y);
}

static void test_visit_heaps(void) {
  const visit_count_t before = visit_heaps();
  CHECK(before.main == 1);

  pthread_t threads[MAKERS];
  for (int i = 0; i < MAKERS; i++) {
    pthread_create(&threads[i], NULL, &make_heaps, &made[i * HEAPS_PER_MAKER]);
  }
  for (int i = 0; i < MAKERS; i++) {
    pthread_join(threads[i], NULL);
  }

  const visit_count_t during = visit_heaps();
  CHECK(during.made == MADE_COUNT);
  CHECK(during.total == before.total + MADE_COUNT);
  CHECK(during.main == 1);

  // sequence numbers are unique and only the main heap has 0
  CHECK(mi_heap_get_seq(mi_heap_main()) == 0);
  size_t seqs[MADE_COUNT];
  for (int i = 0; i < MADE_COUNT; i++) {
    seqs[i] = mi_heap_get_seq(made[i]);
  }
  qsort(seqs, MADE_COUNT, sizeof(seqs[0]), &compare_size);
  CHECK(seqs[0] != 0);
  for (int i = 1; i < MADE_COUNT; i++) {
    CHECK(seqs[i - 1] != seqs[i]);
  }

  // destroy from another thread than the creator, unlinking from both ends of the lists
  for (int i = 0; i < MADE_COUNT / 2; i++) {
    mi_heap_destroy(made[i]);
    mi_heap_destroy(made[MADE_COUNT - 1 - i]);
  }
  memset(made, 0, sizeof(made));

  const visit_count_t after = visit_heaps();
  CHECK(after.total == before.total);
  CHECK(after.main == 1);
}


// --- a reused thread-local key must not find the theap of the previous heap

#define USERS 4
#if defined(MI_TEST_LIGHT)
#define ROUNDS 200
#else
#define ROUNDS 2000
#endif

static _Atomic(mi_heap_t*) shared_heap;
static _Atomic(int) shared_round;
static _Atomic(int) users_done;
static _Atomic(int) wrong_heap;

static void alloc_and_check_heap(mi_heap_t* heap) {
  void* p = mi_heap_malloc(heap, 32);
  if (p == NULL || mi_heap_of(p) != heap) {
    atomic_fetch_add(&wrong_heap, 1);
  }
}

static void* use_shared_heap(void* arg) {
  (void)arg;
  for (int round = 1; round <= ROUNDS; round++) {
    while (atomic_load(&shared_round) != round) { sched_yield(); }
    alloc_and_check_heap(atomic_load(&shared_heap));
    atomic_fetch_add(&users_done, 1);
  }
  return NULL;
}

static void test_key_reuse(void) {
  pthread_t threads[USERS];
  for (int i = 0; i < USERS; i++) {
    pthread_create(&threads[i], NULL, &use_shared_heap, NULL);
  }
  for (int round = 1; round <= ROUNDS; round++) {
    // reuses the key index of the heap destroyed in the previous round
    mi_heap_t* heap = mi_heap_new();
    alloc_and_check_heap(heap);
    atomic_store(&users_done, 0);
    atomic_store(&shared_heap, heap);
    atomic_store(&shared_round, round);
    while (atomic_load(&users_done) != USERS) { sched_yield(); }
    mi_heap_destroy(heap);
  }
  for (int i = 0; i < USERS; i++) {
    pthread_join(threads[i], NULL);
  }
  CHECK(atomic_load(&wrong_heap) == 0);
}


// --- meta-data pages are released when threads exit

#define HOLDERS 32
#define HEAPS_PER_HOLDER 6

static _Atomic(int) holders_ready;

static bool count_area(const mi_heap_t* heap, const mi_heap_area_t* area, void* block, size_t block_size, void* arg) {
  (void)heap; (void)area; (void)block_size;
  if (block == NULL) {
    (*(size_t*)arg)++;
  }
  return true;
}

// meta-data pages belong to the main heap
static size_t main_heap_page_count(void) {
  size_t count = 0;
  mi_collect(true);
  mi_heap_visit_blocks(mi_heap_main(), false, &count_area, &count);
  return count;
}

static void* hold_heaps(void* arg) {
  (void)arg;
  mi_heap_t* heaps[HEAPS_PER_HOLDER];
  for (int i = 0; i < HEAPS_PER_HOLDER; i++) {
    heaps[i] = mi_heap_new();
    heap_alloc(heaps[i], 48);
  }
  // make sure the theaps of all threads are live at the same time
  atomic_fetch_add(&holders_ready, 1);
  while (atomic_load(&holders_ready) != HOLDERS) { sched_yield(); }
  for (int i = 0; i < HEAPS_PER_HOLDER; i++) {
    mi_heap_destroy(heaps[i]);
  }
  return NULL;
}

static void test_meta_pages(void) {
  const size_t before = main_heap_page_count();

  pthread_t threads[HOLDERS];
  for (int i = 0; i < HOLDERS; i++) {
    pthread_create(&threads[i], NULL, &hold_heaps, NULL);
  }
  for (int i = 0; i < HOLDERS; i++) {
    pthread_join(threads[i], NULL);
  }

  // the theaps took ~35 pages; allow for a few retired pages
  const size_t after = main_heap_page_count();
  CHECK(after <= before + 8);
}


// --- stress: many threads, many blocks of all sizes

#define WORKERS 8
#define KEPT_HEAPS 3
#if defined(MI_TEST_LIGHT)
#define HEAPS_PER_WORKER 60
#define MAX_BLOCKS 150
#else
#define HEAPS_PER_WORKER 400
#define MAX_BLOCKS 250
#endif

static _Atomic(int) bad_blocks;

typedef struct block_s {
  uint8_t* p;
  size_t   size;
  uint8_t  tag;
} block_t;

static uint64_t next_random(uint64_t* state) {
  uint64_t x = *state;
  x ^= x << 13;
  x ^= x >> 7;
  x ^= x << 17;
  *state = x;
  return x;
}

static size_t random_between(uint64_t* state, size_t lo, size_t hi) {
  return lo + (size_t)(next_random(state) % (hi - lo));
}

// mostly small blocks with a tail up to 1.5 MB
static size_t random_size(uint64_t* state) {
  const unsigned pct = (unsigned)(next_random(state) % 100);
  if (pct < 55) return random_between(state, 1, 128);
  if (pct < 80) return random_between(state, 128, 1024);
  if (pct < 93) return random_between(state, 1024, 8192);
  if (pct < 98) return random_between(state, 8192, 65536);
  if (pct < 99) return random_between(state, 65536, 262144);
  return random_between(state, 262144, 1500000);
}

// tag the first and last byte, and one byte in every OS page
static void block_fill(const block_t* b) {
  b->p[0] = b->tag;
  b->p[b->size - 1] = b->tag;
  for (size_t i = 4096; i < b->size; i += 4096) {
    b->p[i] = b->tag;
  }
}

static void block_check(const block_t* b) {
  bool ok = (b->p[0] == b->tag && b->p[b->size - 1] == b->tag);
  for (size_t i = 4096; i < b->size; i += 4096) {
    ok = ok && (b->p[i] == b->tag);
  }
  if (!ok) {
    atomic_fetch_add(&bad_blocks, 1);
  }
}

static uint8_t* block_alloc(mi_heap_t* heap, size_t size, uint64_t* state) {
  const unsigned pct = (unsigned)(next_random(state) % 100);
  uint8_t* p;
  bool ok = true;
  if (pct < 75) {
    p = (uint8_t*)mi_heap_malloc(heap, size);
  }
  else if (pct < 88) {
    p = (uint8_t*)mi_heap_zalloc(heap, size);
    ok = (p != NULL && p[0] == 0 && p[size - 1] == 0);
  }
  else {
    const size_t alignment = (size_t)16 << (next_random(state) % 9);
    p = (uint8_t*)mi_heap_malloc_aligned(heap, size, alignment);
    ok = (((uintptr_t)p % alignment) == 0);
  }
  if (!ok || p == NULL || mi_heap_of(p) != heap) {
    atomic_fetch_add(&bad_blocks, 1);
  }
  return p;
}

// Fill a heap with up to MAX_BLOCKS blocks, with some frees and reallocs in between.
// Returns the number of live blocks.
static size_t fill_heap(mi_heap_t* heap, block_t* blocks, uint64_t* state) {
  size_t n = 0;
  const size_t ops = (size_t)(next_random(state) % MAX_BLOCKS);
  for (size_t i = 0; i < ops; i++) {
    const unsigned pct = (unsigned)(next_random(state) % 100);
    const size_t size = random_size(state);
    if (pct < 6 && n > 0) {
      const size_t k = (size_t)(next_random(state) % n);
      block_check(&blocks[k]);
      mi_free(blocks[k].p);
      blocks[k] = blocks[--n];
    }
    else if (pct < 12 && n > 0) {
      block_t* b = &blocks[next_random(state) % n];
      block_check(b);
      b->p = (uint8_t*)mi_heap_realloc(heap, b->p, size);
      b->size = size;
      if (b->p[0] != b->tag) {
        atomic_fetch_add(&bad_blocks, 1);
      }
      block_fill(b);
    }
    else {
      block_t b;
      b.p = block_alloc(heap, size, state);
      b.size = size;
      b.tag = (uint8_t)(next_random(state) | 1);
      if (b.p == NULL) continue;
      block_fill(&b);
      blocks[n++] = b;
    }
  }
  return n;
}

static void* stress_worker(void* arg) {
  uint64_t state = UINT64_C(0x9E3779B97F4A7C15) * ((uintptr_t)arg + 1);
  block_t* blocks = (block_t*)mi_malloc(sizeof(block_t) * MAX_BLOCKS);
  mi_heap_t* kept[KEPT_HEAPS] = { NULL };

  for (int i = 0; i < HEAPS_PER_WORKER; i++) {
    mi_heap_t* heap = mi_heap_new();
    const size_t n = fill_heap(heap, blocks, &state);
    for (size_t j = 0; j < n; j++) {
      block_check(&blocks[j]);
    }

    const unsigned pct = (unsigned)(next_random(&state) % 100);
    if (pct < 80) {
      mi_heap_destroy(heap);
    }
    else if (pct < 90) {
      // keep it alive for a while
      const size_t k = (size_t)(next_random(&state) % KEPT_HEAPS);
      mi_heap_destroy(kept[k]);
      kept[k] = heap;
    }
    else {
      // the blocks survive a delete
      mi_heap_delete(heap);
      for (size_t j = 0; j < n; j++) {
        block_check(&blocks[j]);
        mi_free(blocks[j].p);
      }
    }
  }

  for (int k = 0; k < KEPT_HEAPS; k++) {
    mi_heap_destroy(kept[k]);
  }
  mi_free(blocks);
  return NULL;
}

// Reading the statistics walks all heaps, also the ones that are being destroyed.
// (this needs a sanitizer to fail)
#if defined(MI_TEST_LIGHT)
#define WALK_ROUNDS 100
#else
#define WALK_ROUNDS 1000
#endif

static atomic_int walk_churners;

static void* walk_helper(void* arg) {
  mi_heap_malloc((mi_heap_t*)arg, 100);
  return NULL;
}

static void* walk_churn(void* arg) {
  (void)arg;
  for (int round = 0; round < WALK_ROUNDS; round++) {
    mi_heap_t* heap = mi_heap_new();
    mi_heap_malloc(heap, 64);
    // the exit of a thread that used the heap allocates the statistics of the heap
    pthread_t thread;
    pthread_create(&thread, NULL, &walk_helper, heap);
    pthread_join(thread, NULL);
    mi_heap_destroy(heap);
  }
  atomic_fetch_sub(&walk_churners, 1);
  return NULL;
}

static void* walk_reader(void* arg) {
  (void)arg;
  while (atomic_load(&walk_churners) > 0) {
    get_stats(NULL);
  }
  return NULL;
}

static void test_stats_walk(void) {
  pthread_t churners[2];
  pthread_t readers[2];
  atomic_store(&walk_churners, 2);
  for (int i = 0; i < 2; i++) {
    pthread_create(&churners[i], NULL, &walk_churn, NULL);
    pthread_create(&readers[i], NULL, &walk_reader, NULL);
  }
  for (int i = 0; i < 2; i++) {
    pthread_join(churners[i], NULL);
    pthread_join(readers[i], NULL);
  }
}

// A sub-process main heap uses the fast key, which has no index that can be reused.
// (this runs first, when no index is in use)
static void test_subproc_key(void) {
  enum { HEAPS = 4 };
  mi_subproc_destroy(mi_subproc_new());

  const mi_stats_t before = get_stats(NULL);
  mi_heap_t* heaps[HEAPS];
  for (int i = 0; i < HEAPS; i++) {
    heaps[i] = mi_heap_new();
  }
  for (int round = 0; round < 100; round++) {
    for (int i = 0; i < HEAPS; i++) {
      mi_free(mi_heap_malloc(heaps[i], 32));
    }
  }
  for (int i = 0; i < HEAPS; i++) {
    mi_heap_destroy(heaps[i]);
  }
  // one theap per heap: heaps that share an index evict each other's theap on every use
  const mi_stats_t after = get_stats(NULL);
  CHECK(after.theaps.total - before.theaps.total == HEAPS);
}

static void test_stress(void) {
  const mi_stats_t before = get_stats(NULL);

  pthread_t threads[WORKERS];
  for (int i = 0; i < WORKERS; i++) {
    pthread_create(&threads[i], NULL, &stress_worker, (void*)(uintptr_t)i);
  }
  for (int i = 0; i < WORKERS; i++) {
    pthread_join(threads[i], NULL);
  }

  const mi_stats_t after = get_stats(NULL);
  CHECK(atomic_load(&bad_blocks) == 0);
  CHECK(after.heaps.total - before.heaps.total == WORKERS * HEAPS_PER_WORKER);
  CHECK(after.heaps.current == before.heaps.current);
  CHECK(after.theaps.current == before.theaps.current);
}


static void run(const char* name, void (*test)(void)) {
  const int failed_before = failed;
  fprintf(stderr, "test: %s...  ", name);
  test();
  fprintf(stderr, "%s\n", failed == failed_before ? "ok." : "");
}

int main(void) {
  run("subproc-key", &test_subproc_key);
  run("live-stats", &test_live_stats);
  run("destroyed-stats", &test_destroyed_stats);
  run("visit-heaps", &test_visit_heaps);
  run("key-reuse", &test_key_reuse);
  run("meta-pages", &test_meta_pages);
  run("stats-walk", &test_stats_walk);
  run("stress", &test_stress);
  if (failed > 0) {
    fprintf(stderr, "%d check(s) failed\n", failed);
    return 1;
  }
  return 0;
}

#endif
