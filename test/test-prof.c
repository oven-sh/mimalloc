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

int main(int argc, char** argv) {
  const char* out = (argc > 1 ? argv[1] : "heap-prof.pb");
  mi_prof_enable(64*1024);  // 64 KiB sample rate for a small test
  workload();
  if (mi_prof_dump_to_file(out) != 0) { fprintf(stderr, "dump failed\n"); return 1; }
  printf("wrote %s\n", out);
  return 0;
}
