/* ----------------------------------------------------------------------------
Reproduces the `(mi_theap_t*)1` sentinel leak in `mi_heap_init_theap`.

`heap.c:mi_heap_init_theap` does:

    _mi_thread_local_set(heap->theap, (mi_theap_t*)1);          // (a) reserve slot
    theap = _mi_theap_create(heap, _mi_theap_default_safe()->tld);
    _mi_thread_local_set(heap->theap, theap);                   // (b) overwrite

If anything between (a) and (b) re-enters `mi_heap_malloc(heap, ...)` on the
same thread, `_mi_heap_theap_get_or_init` reads `1` back from the slot,
treats it as a real theap (`1 != NULL`), caches it in `__mi_theap_cached`,
returns it, and the caller dereferences it.

The window contains `_mi_theap_default_safe()` (may run `mi_thread_init` →
allocates), `_mi_meta_zalloc` (may grow the meta arena), and `_mi_theap_init`
→ `_mi_random_init` (calls `_mi_prim_random_buf` → `arc4random_buf` /
`CCRandomGenerateBytes` / `getrandom`, which may `malloc` under MI_OVERRIDE).

This file does two things:

  (1) `--prove`  : deterministic logic proof using internal headers.
                   Places `1` in `slot[heap->theap]` directly (the exact state
                   that exists between (a) and (b)) and shows
                   `mi_heap_malloc(heap, n)` faults at `1 + offsetof(...)`.

  (2) `--stress` : a faithful copy of the pattern that produced the crash:
                   N threadpool workers, each `mi_thread_set_in_threadpool()`
                   then loops `{ h = mi_heap_new(); mi_heap_malloc(h, big);
                   mi_heap_destroy(h); }`. The SIGSEGV handler reports whether
                   the fault address is in the `1 + sizeof(mi_theap_t)` band.

Observed at https://github.com/oven-sh/bun build #56330, macOS x86_64,
fault address 0xE89 == `(mi_theap_t*)1 + offsetof(mi_theap_s, memid.memkind)`
under `-DNDEBUG` (release). Chain: heap.c:_mi_heap_theap_get_or_init →
_mi_theap_cached_set(1) → _mi_theap_incref(1) → `1->memid.memkind`.
-----------------------------------------------------------------------------*/

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <pthread.h>

#include "mimalloc.h"
// internal headers — this test pokes the TLS slot directly to model the
// re-entrance state without depending on platform-specific malloc interposition.
#include "mimalloc/types.h"
#include "mimalloc/internal.h"
#include "mimalloc/prim.h"

#ifndef THREADS
#define THREADS  8
#endif
#ifndef ITER
#define ITER     200000
#endif
#ifndef ALLOC_SIZE
// > MI_SMALL_SIZE_MAX so we hit `_mi_malloc_generic` (matches the
// `read_file_with_allocator` source-file read that crashed)
#define ALLOC_SIZE  (64 * 1024)
#endif

// ───────────────────────── fault classifier ─────────────────────────

static const char* g_phase = "?";

static void wr(const char* s) { (void)!write(2, s, strlen(s)); }
static void wrhex(uintptr_t x) {
  char buf[2 + 16 + 1]; char* p = buf + sizeof(buf); *--p = 0;
  if (x == 0) *--p = '0';
  while (x) { *--p = "0123456789abcdef"[x & 15]; x >>= 4; }
  *--p = 'x'; *--p = '0';
  wr(p);
}

static void on_segv(int sig, siginfo_t* info, void* uctx) {
  (void)sig; (void)uctx;
  uintptr_t addr = (uintptr_t)info->si_addr;
  wr("\n["); wr(g_phase); wr("] SIGSEGV at "); wrhex(addr); wr("\n");
  if (addr >= 1 && addr < 1 + sizeof(mi_theap_t)) {
    wr("  => inside (mi_theap_t*)1 .. +sizeof(mi_theap_t): sentinel leaked as real theap\n");
    wr("  => field offset "); wrhex(addr - 1); wr("\n");
  } else {
    wr("  (outside the (mi_theap_t*)1 band)\n");
  }
  _exit(42);
}

static void install_segv_handler(void) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = on_segv;
  sa.sa_flags = SA_SIGINFO;
  sigaction(SIGSEGV, &sa, NULL);
  sigaction(SIGBUS,  &sa, NULL);
}

static void print_offsets(void) {
  fprintf(stderr,
    "  sizeof(mi_theap_t)        = %zu\n"
    "  +heap                     = %zu\n"
    "  +refcount                 = %zu\n"
    "  +memid.memkind            = %zu (0x%zx)  <- _mi_theap_incref reads this\n",
    sizeof(mi_theap_t),
    offsetof(mi_theap_t, heap),
    offsetof(mi_theap_t, refcount),
    offsetof(mi_theap_t, memid) + offsetof(mi_memid_t, memkind),
    offsetof(mi_theap_t, memid) + offsetof(mi_memid_t, memkind));
}

// ───────────────────────── (1) deterministic proof ─────────────────────────

static int run_prove(void) {
  g_phase = "prove";
  fprintf(stderr, "[prove] reference offsets in mi_theap_s:\n");
  print_offsets();

  mi_heap_t* h = mi_heap_new();
  if (h == NULL) { fprintf(stderr, "mi_heap_new failed\n"); return 1; }

  // Model the state that exists between heap.c:(a) and (b): the per-heap TLS
  // slot holds the reservation placeholder. Any same-thread re-entrant
  // `mi_heap_malloc(h, ..)` during that window observes exactly this.
  // Historically the placeholder was `(mi_theap_t*)1`, which faulted in
  // `_mi_theap_incref` at `1 + offsetof(memid.memkind)`; the fix uses
  // `&_mi_theap_empty_wrong`, which every consumer already handles.
  install_segv_handler();

  // (i) the fixed placeholder must be handled gracefully
  if (!_mi_thread_local_set(h->theap, (mi_theap_t*)&_mi_theap_empty_wrong)) {
    fprintf(stderr, "_mi_thread_local_set failed\n");
    return 1;
  }
  // Clear the per-thread theap cache so `_mi_heap_theap(h)` takes the slow
  // path and reads the slot (otherwise a cached real theap from a prior
  // allocation would mask the placeholder).
  _mi_theap_cached_set((mi_theap_t*)&_mi_theap_empty);
  fprintf(stderr,
    "[prove] slot[h->theap]=&_mi_theap_empty_wrong, cache cleared; "
    "calling mi_heap_malloc(h, %d)\n", ALLOC_SIZE);
  void* p = mi_heap_malloc(h, ALLOC_SIZE);
  if (p != NULL) {
    fprintf(stderr,
      "[prove] FAIL: re-entrant placeholder allocated %p — should be NULL "
      "(via _mi_theap_empty_wrong → _mi_malloc_generic early-out)\n", p);
    return 1;
  }
  fprintf(stderr, "[prove] OK: re-entrant placeholder → mi_heap_malloc returned NULL\n");

  // (ii) and once the real theap is installed, allocation works
  _mi_thread_local_set(h->theap, NULL);            // back to "uninit"
  _mi_theap_cached_set((mi_theap_t*)&_mi_theap_empty);
  p = mi_heap_malloc(h, ALLOC_SIZE);               // walks mi_heap_init_theap fully
  if (p == NULL) {
    fprintf(stderr, "[prove] FAIL: normal mi_heap_malloc returned NULL\n");
    return 1;
  }
  fprintf(stderr, "[prove] OK: normal mi_heap_malloc returned %p\n", p);
  mi_heap_destroy(h);
  return 0;
}

// ───────────────────────── (2) stress (Bun's pattern) ─────────────────────────
//
// Per worker thread (mirrors `bun_threading::ThreadPool::Thread::run` →
// `RuntimeTranspilerStore::TranspilerJob::run`):
//   - `mi_thread_set_in_threadpool()` once
//   - loop: `mi_heap_new()` → first `mi_heap_malloc(h, big)` → `mi_heap_destroy(h)`
// The first malloc on each fresh `h` walks `mi_heap_init_theap(h)` and opens
// the (a)..(b) window on every iteration.

static volatile int g_stop;

static void* worker(void* arg) {
  (void)arg;
  mi_thread_set_in_threadpool();
  for (int i = 0; i < ITER && !g_stop; i++) {
    mi_heap_t* h = mi_heap_new();
    if (h == NULL) { g_stop = 1; return NULL; }
    void* p = mi_heap_malloc(h, ALLOC_SIZE);
    if (p != NULL) { ((volatile char*)p)[0] = 1; }
    mi_heap_destroy(h);
  }
  return NULL;
}

static int run_stress(void) {
  g_phase = "stress";
  fprintf(stderr, "[stress] %d threads x %d iters x mi_heap_malloc(%d)\n",
          THREADS, ITER, ALLOC_SIZE);
  install_segv_handler();
  pthread_t tids[THREADS];
  for (int i = 0; i < THREADS; i++) {
    pthread_create(&tids[i], NULL, worker, NULL);
  }
  for (int i = 0; i < THREADS; i++) {
    pthread_join(tids[i], NULL);
  }
  fprintf(stderr, "[stress] done, no fault\n");
  return 0;
}

// ─────────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
  bool prove = false, stress = false;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--prove")  == 0) prove  = true;
    if (strcmp(argv[i], "--stress") == 0) stress = true;
  }
  if (!prove && !stress) prove = true;

  if (prove)  { int r = run_prove();  if (r && !stress) return r; }
  if (stress) { return run_stress(); }
  return 0;
}
