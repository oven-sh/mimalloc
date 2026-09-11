// Exercise the sampling heap profiler.
//   ./mimalloc-test-prof /tmp/prof.pb
// then: go tool pprof -top /tmp/prof.pb

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mimalloc.h"

#if defined(__GNUC__)
#define NOINLINE __attribute__((noinline))
#elif defined(_MSC_VER)
#define NOINLINE __declspec(noinline)
#else
#define NOINLINE
#endif

static NOINLINE void* leaky_alloc(size_t n) { return mi_malloc(n); }
static NOINLINE void* tidy_alloc(size_t n)  { return mi_malloc(n); }

static NOINLINE void workload(void) {
  // ~50 MB through leaky_alloc (kept), ~50 MB through tidy_alloc (freed)
  for (int i = 0; i < 50000; i++) {
    void* p = leaky_alloc(1000);   // leaks
    memset(p, 1, 8);
  }
  for (int i = 0; i < 50000; i++) {
    void* p = tidy_alloc(1000);
    memset(p, 2, 8);
    mi_free(p);                    // freed
  }
}

// More distinct call sites than the dump's initial location table holds (4096): every expansion of SITE is its own
// return address, and with a sample rate of 1 every allocation is sampled, so the dump sees >5000 unique frames.
#define SITE      sites[n++] = leaky_alloc(16);
#define SITE10    SITE SITE SITE SITE SITE SITE SITE SITE SITE SITE
#define SITE100   SITE10 SITE10 SITE10 SITE10 SITE10 SITE10 SITE10 SITE10 SITE10 SITE10
#define SITE1000  SITE100 SITE100 SITE100 SITE100 SITE100 SITE100 SITE100 SITE100 SITE100 SITE100
static void* sites[6000];
static NOINLINE size_t many_call_sites(void) {
  size_t n = 0;
  SITE1000 SITE1000 SITE1000 SITE1000 SITE1000
  return n;
}

// Blocks that go away with `mi_heap_destroy` are never freed one by one; their samples must not stay "in use".
static NOINLINE int heap_destroy_drops_samples(void) {
  mi_prof_reset();
  mi_prof_enable(1);
  mi_heap_t* heap = mi_heap_new();
  mi_heap_malloc(heap, 48);   // the heap's own meta-data (its theap, the thread-local slot) is allocated by now
  size_t live0 = 0; mi_prof_get_counts(NULL, &live0);
  for (int i = 0; i < 2000; i++) { memset(mi_heap_malloc(heap, 48 + (size_t)(i % 7) * 100), 3, 8); }
  size_t live1 = 0; mi_prof_get_counts(NULL, &live1);
  if (live1 < live0 + 2000) { fprintf(stderr, "expected 2000 live samples, got %zu\n", live1 - live0); return 1; }
  mi_heap_destroy(heap);
  size_t live2 = 0; mi_prof_get_counts(NULL, &live2);
  if (live2 > live0) { fprintf(stderr, "%zu samples still live after mi_heap_destroy\n", live2 - live0); return 1; }
  return 0;
}

// Profiling for a long time must not grow the profiler's own tables: one call site, allocate and free.
static NOINLINE int churn_is_bounded(void) {
  mi_prof_reset();
  mi_prof_enable(1);
  for (int i = 0; i < 2000000; i++) { mi_free(tidy_alloc(64)); }
  size_t stacks = 0, live = 0; mi_prof_get_counts(&stacks, &live);
  if (stacks > 64 || live > 64) { fprintf(stderr, "churn left %zu stacks, %zu live samples\n", stacks, live); return 1; }
  return 0;
}

int main(int argc, char** argv) {
  const char* out = (argc > 1 ? argv[1] : "heap-prof.pb");
  mi_prof_enable(64*1024);  // 64 KiB sample rate for a small test
  workload();
  if (mi_prof_dump_to_file(out) != 0) { fprintf(stderr, "dump failed\n"); return 1; }
  printf("wrote %s\n", out);

  mi_prof_reset();
  mi_prof_enable(1);
  size_t n = many_call_sites();
  size_t size = mi_prof_dump_buf(NULL, 0);   // used to spin forever once the location table filled up
  if (size == 0) { fprintf(stderr, "dump of %zu call sites produced nothing\n", n); return 1; }
  for (size_t i = 0; i < n; i++) mi_free(sites[i]);
  printf("dumped %zu call sites in %zu bytes\n", n, size);

  if (heap_destroy_drops_samples() != 0) return 1;
  if (churn_is_bounded() != 0) return 1;
  mi_prof_enable(0);
  return 0;
}
