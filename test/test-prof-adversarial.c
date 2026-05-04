// Adversarial tests for the sampling heap profiler.
// Each case targets a specific race or edge condition; failures abort.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdatomic.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include "mimalloc.h"

#if defined(__GNUC__)
#define NOINLINE __attribute__((noinline))
#else
#define NOINLINE
#endif

static int g_fail = 0;
#define CHECK(cond, msg) do { if (!(cond)) { fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); g_fail = 1; } } while(0)
#define OK(name) fprintf(stderr, "  ok: %s\n", name)

// ---------------------------------------------------------------------------
// Minimal profile.proto validator: checks header structure + counts samples.
// Not a full parser; enough to catch malformed output and verify inuse totals.
// ---------------------------------------------------------------------------

static size_t pb_varint(const uint8_t** p, const uint8_t* end) {
  size_t v = 0, s = 0;
  while (*p < end) { uint8_t b = *(*p)++; v |= (size_t)(b & 0x7f) << s; if (!(b & 0x80)) break; s += 7; }
  return v;
}

typedef struct { size_t nsamples, nlocs, nmaps, nstrings; int64_t inuse_bytes, alloc_bytes; } pb_stats_t;

static int pb_validate(const char* path, pb_stats_t* st) {
  memset(st, 0, sizeof(*st));
  int fd = open(path, O_RDONLY); if (fd < 0) return -1;
  off_t sz = lseek(fd, 0, SEEK_END); lseek(fd, 0, SEEK_SET);
  uint8_t* buf = malloc((size_t)sz); read(fd, buf, (size_t)sz); close(fd);
  const uint8_t* p = buf; const uint8_t* end = buf + sz;
  while (p < end) {
    size_t tag = pb_varint(&p, end);
    uint32_t field = (uint32_t)(tag >> 3), wt = (uint32_t)(tag & 7);
    if (wt == 0) { pb_varint(&p, end); }
    else if (wt == 2) {
      size_t len = pb_varint(&p, end);
      const uint8_t* sub_end = p + len;
      if (sub_end > end) { free(buf); return -1; }  // malformed
      if (field == 2) {  // Sample: extract value[1]=alloc_space, value[3]=inuse_space
        st->nsamples++;
        const uint8_t* sp = p;
        while (sp < sub_end) {
          size_t st2 = pb_varint(&sp, sub_end); uint32_t sf = (uint32_t)(st2>>3), swt=(uint32_t)(st2&7);
          if (swt == 2) {
            size_t slen = pb_varint(&sp, sub_end);
            if (sf == 2) {  // value[] packed
              const uint8_t* vp = sp; const uint8_t* ve = sp + slen;
              int64_t v[4] = {0}; int vi = 0;
              while (vp < ve && vi < 4) v[vi++] = (int64_t)pb_varint(&vp, ve);
              st->alloc_bytes += v[1]; st->inuse_bytes += v[3];
            }
            sp += slen;
          } else if (swt == 0) { pb_varint(&sp, sub_end); }
          else { free(buf); return -1; }
        }
      }
      else if (field == 3) st->nmaps++;
      else if (field == 4) st->nlocs++;
      else if (field == 6) st->nstrings++;
      p = sub_end;
    }
    else { free(buf); return -1; }  // unknown wire type
  }
  free(buf);
  return 0;
}

// ---------------------------------------------------------------------------
// Case: cross-thread free — alloc on T1, free on T2; inuse must drop.
// Exercises mi_free_generic_mt -> mi_free_generic_mt_prof path.
// ---------------------------------------------------------------------------

#define XFREE_N 4096
static void* g_xfree_blocks[XFREE_N];
static atomic_int g_xfree_ready;

static NOINLINE void* xfree_alloc_thread(void* _) {
  for (int i = 0; i < XFREE_N; i++) g_xfree_blocks[i] = mi_malloc(512);
  atomic_store(&g_xfree_ready, 1);
  return NULL;
}
static NOINLINE void* xfree_free_thread(void* _) {
  while (!atomic_load(&g_xfree_ready)) sched_yield();
  for (int i = 0; i < XFREE_N; i++) mi_free(g_xfree_blocks[i]);
  return NULL;
}

static void case_cross_thread_free(void) {
  mi_prof_reset();
  mi_prof_enable(256);  // sample ~every other 512B alloc
  atomic_store(&g_xfree_ready, 0);
  pthread_t t1, t2;
  pthread_create(&t1, NULL, xfree_alloc_thread, NULL);
  pthread_create(&t2, NULL, xfree_free_thread, NULL);
  pthread_join(t1, NULL); pthread_join(t2, NULL);
  mi_prof_dump_to_file("/tmp/prof-xfree.pb");
  pb_stats_t st; CHECK(pb_validate("/tmp/prof-xfree.pb", &st) == 0, "xfree: profile parses");
  CHECK(st.nsamples > 100, "xfree: samples captured");
  // all allocs were freed cross-thread; inuse should be ~0 (small noise from other allocs ok)
  CHECK(st.inuse_bytes < st.alloc_bytes / 4, "xfree: cross-thread frees tracked (inuse << alloc)");
  mi_prof_enable(0);
  OK("cross-thread free");
}

// ---------------------------------------------------------------------------
// Case: page reuse — fill a page (sampled), free everything, page returns to
// arena, allocate again from a fresh page; new page must NOT have stale
// MI_PAGE_HAS_PROF_SAMPLES flag (frees on it would route to slow path forever).
// We can't observe the flag directly, but we can check: with prof DISABLED,
// after page reuse, free should not call _mi_prof_free (would crash on uninit
// lock), and inuse accounting from a re-enabled prof should be clean.
// ---------------------------------------------------------------------------

static void case_page_flag_reuse(void) {
  mi_prof_enable(64);
  void* a[2048];
  for (int i = 0; i < 2048; i++) a[i] = mi_malloc(128);
  for (int i = 0; i < 2048; i++) mi_free(a[i]);
  mi_collect(true);  // force page return to arena
  mi_prof_enable(0);
  // re-allocate same size; pages may be reused. with prof off, frees must be fast-path.
  for (int i = 0; i < 2048; i++) a[i] = mi_malloc(128);
  for (int i = 0; i < 2048; i++) mi_free(a[i]);  // would corrupt if stale flag + uninit prof state
  OK("page-flag reuse after collect");
}

// ---------------------------------------------------------------------------
// Case: realloc — sampled block realloc'd to larger; old must be marked freed,
// new may be sampled independently; no double-count.
// ---------------------------------------------------------------------------

static void case_realloc(void) {
  mi_prof_reset();
  mi_prof_enable(1);  // sample every alloc
  void* keep[500];
  for (int i = 0; i < 500; i++) {
    void* p = mi_malloc(200);
    p = mi_realloc(p, 800);  // free 200B, alloc 800B
    keep[i] = p;
  }
  mi_prof_dump_to_file("/tmp/prof-realloc.pb");
  pb_stats_t st; CHECK(pb_validate("/tmp/prof-realloc.pb", &st) == 0, "realloc: parses");
  // inuse should reflect only the 800B blocks (old 200B freed)
  // alloc_bytes counts both. ratio ~ 800/(200+800) = 0.8; allow slack for rounding
  double ratio = (double)st.inuse_bytes / (double)st.alloc_bytes;
  CHECK(ratio > 0.5 && ratio < 0.95, "realloc: old block freed, not double-counted");
  for (int i = 0; i < 500; i++) mi_free(keep[i]);
  mi_prof_enable(0);
  OK("realloc");
}

// ---------------------------------------------------------------------------
// Case: aligned alloc — interior pointer; free must resolve to block start
// and remove the sample.
// ---------------------------------------------------------------------------

static void case_aligned(void) {
  mi_prof_reset();
  mi_prof_enable(1);
  void* a[200];
  for (int i = 0; i < 200; i++) a[i] = mi_malloc_aligned(300, 256);
  for (int i = 0; i < 200; i++) mi_free(a[i]);
  mi_prof_dump_to_file("/tmp/prof-aligned.pb");
  pb_stats_t st; CHECK(pb_validate("/tmp/prof-aligned.pb", &st) == 0, "aligned: parses");
  CHECK(st.inuse_bytes < st.alloc_bytes / 4, "aligned: interior-ptr frees tracked");
  mi_prof_enable(0);
  OK("aligned alloc");
}

// ---------------------------------------------------------------------------
// Case: MT stress — N threads allocating/freeing random sizes concurrently
// while another thread dumps the profile mid-stream. Mutex must serialize.
// ---------------------------------------------------------------------------

#define MT_THREADS 8
#define MT_ITERS   50000
static atomic_int g_mt_stop;

static NOINLINE void* mt_worker(void* arg) {
  uintptr_t seed = (uintptr_t)arg;
  void* slot[32] = {0};
  for (int i = 0; i < MT_ITERS; i++) {
    seed = seed * 6364136223846793005ull + 1;
    int j = (int)(seed % 32);
    if (slot[j]) { mi_free(slot[j]); slot[j] = NULL; }
    else slot[j] = mi_malloc(16 + (seed % 4096));
  }
  for (int j = 0; j < 32; j++) if (slot[j]) mi_free(slot[j]);
  return NULL;
}
static void* mt_dumper(void* _) {
  while (!atomic_load(&g_mt_stop)) {
    mi_prof_dump_to_file("/tmp/prof-mt.pb");
    usleep(1000);
  }
  return NULL;
}

static void case_mt_stress(void) {
  mi_prof_enable(4096);
  atomic_store(&g_mt_stop, 0);
  pthread_t th[MT_THREADS], dumper;
  pthread_create(&dumper, NULL, mt_dumper, NULL);
  for (int i = 0; i < MT_THREADS; i++) pthread_create(&th[i], NULL, mt_worker, (void*)(uintptr_t)(i+1));
  for (int i = 0; i < MT_THREADS; i++) pthread_join(th[i], NULL);
  atomic_store(&g_mt_stop, 1);
  pthread_join(dumper, NULL);
  mi_prof_dump_to_file("/tmp/prof-mt.pb");
  pb_stats_t st; CHECK(pb_validate("/tmp/prof-mt.pb", &st) == 0, "mt: final profile parses");
  CHECK(st.nsamples > 100, "mt: samples captured");
  mi_prof_enable(0);
  OK("MT stress + concurrent dump");
}

// ---------------------------------------------------------------------------
// Case: rate=1 hammer — sample every single alloc; table grows; no crash/leak
// in the sample-table grow path.
// ---------------------------------------------------------------------------

static void case_rate1_hammer(void) {
  mi_prof_enable(1);
  void** a = mi_malloc(20000 * sizeof(void*));
  for (int i = 0; i < 20000; i++) a[i] = mi_malloc(32);
  mi_prof_dump_to_file("/tmp/prof-rate1.pb");
  pb_stats_t st; CHECK(pb_validate("/tmp/prof-rate1.pb", &st) == 0, "rate1: parses");
  CHECK(st.nsamples >= 10000, "rate1: many samples captured");
  for (int i = 0; i < 20000; i++) mi_free(a[i]);
  mi_free(a);
  mi_prof_enable(0);
  OK("rate=1 hammer (table growth)");
}

// ---------------------------------------------------------------------------
// Case: enable/disable cycling — toggle profiling repeatedly while allocating.
// ---------------------------------------------------------------------------

static void case_enable_cycle(void) {
  for (int c = 0; c < 20; c++) {
    mi_prof_enable(c % 2 == 0 ? 256 : 0);
    for (int i = 0; i < 1000; i++) { void* p = mi_malloc(100); mi_free(p); }
  }
  mi_prof_enable(256);
  mi_prof_dump_to_file("/tmp/prof-cycle.pb");
  pb_stats_t st; CHECK(pb_validate("/tmp/prof-cycle.pb", &st) == 0, "cycle: parses");
  mi_prof_enable(0);
  OK("enable/disable cycling");
}

// ---------------------------------------------------------------------------
// Case: empty profile — dump before any samples; file must be valid pprof.
// ---------------------------------------------------------------------------

static void case_empty(void) {
  // fresh state: enable with huge rate so nothing is sampled
  mi_prof_enable(1ull << 40);
  void* p = mi_malloc(16); mi_free(p);
  CHECK(mi_prof_dump_to_file("/tmp/prof-empty.pb") == 0, "empty: dump succeeds");
  pb_stats_t st; CHECK(pb_validate("/tmp/prof-empty.pb", &st) == 0, "empty: parses");
  CHECK(st.nstrings >= 8, "empty: string table present");
  mi_prof_enable(0);
  OK("empty profile");
}

// ---------------------------------------------------------------------------
// Case: huge allocation — singleton page; flag + free path
// ---------------------------------------------------------------------------

static void case_huge(void) {
  mi_prof_reset();
  mi_prof_enable(1);
  void* a[10];
  for (int i = 0; i < 10; i++) a[i] = mi_malloc(2 * 1024 * 1024);
  for (int i = 0; i < 10; i++) mi_free(a[i]);
  mi_prof_dump_to_file("/tmp/prof-huge.pb");
  pb_stats_t st; CHECK(pb_validate("/tmp/prof-huge.pb", &st) == 0, "huge: parses");
  CHECK(st.inuse_bytes < st.alloc_bytes / 4, "huge: singleton-page frees tracked");
  mi_prof_enable(0);
  OK("huge alloc");
}

// ---------------------------------------------------------------------------
// Case: invalid fd — dump to bad fd must return -1, not crash.
// ---------------------------------------------------------------------------

static void case_bad_fd(void) {
  mi_prof_enable(256);
  void* p = mi_malloc(1000); (void)p;
  CHECK(mi_prof_dump(-1) == -1, "bad-fd: returns error");
  CHECK(mi_prof_dump(12345) == -1, "closed-fd: returns error");
  mi_prof_enable(0);
  OK("bad fd");
}

// ---------------------------------------------------------------------------
// Case: process-wide enable — main calls start(), a worker thread that was
// already running (and had already allocated) must start sampling.
// ---------------------------------------------------------------------------

static atomic_int g_pw_phase; // 0=warmup, 1=profile, 2=stop
static NOINLINE void* pw_worker(void* _) {
  // warmup: allocate before profiling is on so this thread has a populated theap
  for (int i = 0; i < 1000; i++) { void* p = mi_malloc(256); mi_free(p); }
  atomic_store(&g_pw_phase, 1);
  while (atomic_load(&g_pw_phase) == 1) sched_yield();
  // now main has called mi_prof_enable; allocate — these must be sampled
  for (int i = 0; i < 5000; i++) { void* p = mi_malloc(256); mi_free(p); }
  return NULL;
}

static void case_process_wide(void) {
  mi_prof_reset();
  atomic_store(&g_pw_phase, 0);
  pthread_t t; pthread_create(&t, NULL, pw_worker, NULL);
  while (atomic_load(&g_pw_phase) == 0) sched_yield();  // wait for worker warmup
  mi_prof_enable(128);  // ON: must reach the worker's already-existing theap
  atomic_store(&g_pw_phase, 2);
  pthread_join(t, NULL);
  mi_prof_enable(0);
  mi_prof_dump_to_file("/tmp/prof-pw.pb");
  pb_stats_t st; CHECK(pb_validate("/tmp/prof-pw.pb", &st) == 0, "process-wide: parses");
  // worker allocated 5000*256 = 1.28MB at rate=128 -> ~10000 samples expected;
  // even if a few fast-path allocs slip through before lazy-enable, should be >>100
  CHECK(st.nsamples > 1000, "process-wide: worker thread sampled after main enabled");
  OK("process-wide enable");
}

// ---------------------------------------------------------------------------
// Case: dump_buf — two-call size query, then fill; bytes match dump_to_file.
// ---------------------------------------------------------------------------

static void case_dump_buf(void) {
  mi_prof_reset();
  mi_prof_enable(64);
  for (int i = 0; i < 1000; i++) { void* p = mi_malloc(500); if (i&1) mi_free(p); }
  mi_prof_enable(0);  // stop before querying size so output is stable across calls
  size_t need = mi_prof_dump_buf(NULL, 0);
  CHECK(need > 100, "dump_buf: size query");
  uint8_t* b = mi_malloc(need);
  size_t got = mi_prof_dump_buf(b, need);
  CHECK(got == need, "dump_buf: stable size between calls");
  // also dump to file and compare bytes
  mi_prof_dump_to_file("/tmp/prof-buf.pb");
  int fd = open("/tmp/prof-buf.pb", O_RDONLY); off_t fsz = lseek(fd,0,SEEK_END); lseek(fd,0,SEEK_SET);
  uint8_t* fbuf = mi_malloc((size_t)fsz); read(fd, fbuf, (size_t)fsz); close(fd);
  CHECK((size_t)fsz == need && memcmp(b, fbuf, need) == 0, "dump_buf: identical to dump_to_file");
  mi_free(b); mi_free(fbuf);
  OK("dump_buf");
}

// ---------------------------------------------------------------------------

int main(void) {
  fprintf(stderr, "test-prof-adversarial:\n");
  case_empty();
  case_bad_fd();
  case_page_flag_reuse();
  case_realloc();
  case_aligned();
  case_huge();
  case_cross_thread_free();
  case_rate1_hammer();
  case_enable_cycle();
  case_dump_buf();
  case_process_wide();
  case_mt_stress();
  if (g_fail) { fprintf(stderr, "FAILED\n"); return 1; }
  fprintf(stderr, "all cases passed\n");
  return 0;
}
