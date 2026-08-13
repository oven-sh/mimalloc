// The allocator must not touch a `__thread` variable of its own from inside an allocation or a
// page collect. On targets where `__thread` is emulated (Android before API 29, which is what
// bun's Android build targets), every access goes through `__emutls_get_address`, and the first
// access on a thread materializes the variable -- and the thread's per-variable address array --
// with malloc(). With mimalloc being malloc, that re-enters the allocator from wherever the access
// sits. The idle sweep's guard used to be such a variable, read from `mi_page_free_collect_ex`:
// the collect called into emulated TLS, emulated TLS called malloc, malloc had to collect a page
// of exactly the size class it was asked for, that collect read the (still unmaterialized) guard
// again, and so on until the stack was gone (oven-sh/bun#38051).
//
// This binary is linked against a build of the allocator compiled with `-femulated-tls` and
// malloc overriding (see CMakeLists.txt), so emulated TLS allocates from the allocator under test,
// exactly as on Android. It then arranges for a thread's first page collect to happen while the
// page of whichever size class emulated TLS is going to ask for is exhausted. Every small size
// class gets a turn, so the test does not depend on the sizes a particular emutls runtime asks for
// (compiler-rt and libgcc differ), and a bug of this kind shows up as a stack overflow instead of
// a failed check. With a correct allocator nothing here ever reaches emulated TLS at all.
#include <mimalloc.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

// Request sizes covering every size bin up to 1 KiB (several of these share a bin; part 2 below
// only uses one request size per bin).
static const size_t sizes[] = { 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256,
                                320, 384, 448, 512, 640, 768, 896, 1024 };
#define NSIZES          (sizeof(sizes) / sizeof(sizes[0]))
// Per class: stays inside one (64 KiB) small page, yet spans several OS pages even with 16 KiB ones.
#define BYTES_PER_CLASS (48 * 1024)
#define MAX_BLOCKS      (BYTES_PER_CLASS / 8)
#define EXTEND_BYTES    4096          // a page formats this many bytes of blocks at a time (MI_MAX_EXTEND_SIZE)

static void* blocks[MAX_BLOCKS];
static uintptr_t os_page;   // the OS page size: the granularity at which the sweep discards

static void run_thread(void* (*fun)(void*), void* arg) {
  pthread_t th;
  if (pthread_create(&th, NULL, fun, arg) != 0) { printf("  pthread_create failed\n"); exit(2); }
  pthread_join(th, NULL);
}

// ---------------------------------------------------------------------------------------------
// 1. A fresh thread allocates one size class until its page runs out of formatted blocks. The
//    allocation that runs out collects the page (`mi_page_queue_find_free_ex`), which is the first
//    collect on this thread. An optimizing compiler hoists a thread-local read in that collect
//    ahead of the conditions guarding it, so with the bug this is where emulated TLS allocates,
//    and the thread whose size class matches the emutls request recurses into the same exhausted
//    page without bound.
// ---------------------------------------------------------------------------------------------
static void* exhaust_one_class(void* arg) {
  const size_t size = *(const size_t*)arg;
  const size_t n = BYTES_PER_CLASS / size;
  for (size_t i = 0; i < n; i++) {
    blocks[i] = malloc(size);
    if (blocks[i] == NULL) { printf("  out of memory\n"); exit(2); }
    memset(blocks[i], (int)i, size);
  }
  for (size_t i = 0; i < n; i++) free(blocks[i]);
  return NULL;
}

// ---------------------------------------------------------------------------------------------
// 2. The unoptimized variant of the same read only happens for a page with purged holes, so also
//    produce those: a fresh thread leaves, in every size class, a page whose free blocks all sit in
//    OS pages without a live block, parks, and lets the scavenger sweep it. The sweep discards
//    those OS pages and takes every one of those blocks off the free lists. The next allocation in
//    any class has to collect a page with purged holes and an empty free list; with the bug that
//    reads the guard, emulated TLS allocates, and that allocation lands on another such page.
// ---------------------------------------------------------------------------------------------
static int cmp_ptr(const void* a, const void* b) {
  const uintptr_t x = *(const uintptr_t*)a, y = *(const uintptr_t*)b;
  return (x < y ? -1 : (x > y ? 1 : 0));
}

typedef struct { void** live; size_t count; } kept_t;

// Allocate a fresh page's worth of one bin, then keep exactly the blocks lying entirely inside the
// first and the last OS page the blocks touch (those two OS pages can never be discarded, so their
// blocks must not be free either) and free everything else: every OS page in between then holds
// only free blocks. The count is a whole number of extensions so that no formatted block is left
// over next to the kept ones, which would stay on the free list and serve the allocation later.
static void prepare_class(size_t size, kept_t* kept) {
  static uintptr_t addrs[MAX_BLOCKS];
  addrs[0] = (uintptr_t)malloc(size);
  addrs[1] = (uintptr_t)malloc(size);
  if (addrs[0] == 0 || addrs[1] == 0) { printf("  out of memory\n"); exit(2); }
  const size_t stride = (addrs[1] > addrs[0] ? addrs[1] - addrs[0] : addrs[0] - addrs[1]);   // the block size, padding included
  if (stride < size || stride > 2 * size + 64) { printf("  unexpected block layout for size %zu (stride %zu)\n", size, stride); exit(2); }
  const size_t per_extension = EXTEND_BYTES / stride;
  const size_t n = (BYTES_PER_CLASS / EXTEND_BYTES) * per_extension;
  for (size_t i = 2; i < n; i++) {
    addrs[i] = (uintptr_t)malloc(size);
    if (addrs[i] == 0) { printf("  out of memory\n"); exit(2); }
  }
  qsort(addrs, n, sizeof(addrs[0]), &cmp_ptr);
  const uintptr_t first_page = addrs[0] & ~(os_page - 1);
  const uintptr_t last_page  = (addrs[n - 1] + stride - 1) & ~(os_page - 1);
  kept->count = 0;
  for (size_t i = 0; i < n; i++) {
    const uintptr_t lo = addrs[i], hi = lo + stride;
    const int in_first = (lo >= first_page && hi <= first_page + os_page);
    const int in_last  = (lo >= last_page  && hi <= last_page  + os_page);
    if (in_first || in_last) { kept->live[kept->count++] = (void*)lo; }
    else { free((void*)lo); }
  }
}

static void* purged_pages_in_every_class(void* arg) {
  (void)arg;
  static void*  live[NSIZES][MAX_BLOCKS];   // what prepare_class keeps alive, per class
  static size_t class_size[NSIZES];         // one request size per bin
  kept_t kept[NSIZES];
  size_t nclasses = 0;
  for (size_t s = 0; s < NSIZES; s++) {
    if (s > 0 && mi_good_size(sizes[s]) == mi_good_size(sizes[s - 1])) continue;   // same bin as the previous size
    class_size[nclasses] = sizes[s];
    kept[nclasses].live = live[nclasses];
    prepare_class(sizes[s], &kept[nclasses]);
    nclasses++;
  }

  mi_purge_holes_stats_t before, now;
  mi_purge_holes_stats_get(&before);
  if (!mi_on_thread_idle_start()) {
    printf("  (no scavenger to hand the heaps to: skipping the purged-pages part)\n");
  }
  else {
    // Wait for the scavenger to sweep us: until the discard count has risen and then stayed put
    // for a while (taking the heaps back stops a sweep that is still running). Nothing in this
    // loop may allocate.
    const struct timespec ms = { 0, 1000 * 1000 };
    size_t last = before.purged_blocks;
    int stable = 0, waited = 0;
    do {
      nanosleep(&ms, NULL);
      mi_purge_holes_stats_get(&now);
      if (now.purged_blocks == last) { if (last > before.purged_blocks) stable++; }
      else { last = now.purged_blocks; stable = 0; }
    } while (stable < 100 && ++waited < 20000);
    mi_on_thread_idle_end();
    if (now.purged_blocks <= before.purged_blocks) {
      printf("  (the scavenger did not sweep this thread within 20s: skipping the purged-pages part)\n");
    }
    else {
      printf("  swept: %zu blocks held off the free lists in %zu classes\n", (size_t)(now.purged_blocks - before.purged_blocks), nclasses);
      for (size_t c = 0; c < nclasses; c++) {
        void* p = malloc(class_size[c]);   // collects a page with purged holes and an empty free list
        if (p == NULL) { printf("  out of memory\n"); exit(2); }
        memset(p, 1, class_size[c]);
        free(p);
      }
      mi_purge_holes_stats_t after;
      mi_purge_holes_stats_get(&after);
      printf("  handed back: %zu hole runs\n", (size_t)(after.reuse_calls - now.reuse_calls));
      if (after.reuse_calls == now.reuse_calls) {
        // then the allocations above never collected a purged page and this part tested nothing
        printf("  FAILED: the allocations after the sweep did not reach a page with purged holes\n");
        exit(1);
      }
    }
  }
  for (size_t c = 0; c < nclasses; c++) {
    for (size_t i = 0; i < kept[c].count; i++) free(kept[c].live[i]);
  }
  return NULL;
}

int main(void) {
  const long ps = sysconf(_SC_PAGESIZE);
  os_page = (ps > 0 ? (uintptr_t)ps : 4096);
  printf("exhausting one size class per fresh thread...\n");
  for (size_t s = 0; s < NSIZES; s++) {
    size_t size = sizes[s];
    run_thread(&exhaust_one_class, &size);
  }
  printf("collecting pages with purged holes on a fresh thread...\n");
  run_thread(&purged_pages_in_every_class, NULL);
  printf("ok\n");
  return 0;
}
