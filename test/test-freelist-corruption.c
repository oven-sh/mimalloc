/* ----------------------------------------------------------------------------
Copyright (c) 2018-2025, Microsoft Research, Daan Leijen
This is free software; you can redistribute it and/or modify it under the
terms of the MIT license. A copy of the license can be found in the file
"LICENSE" at the root of this distribution.
-----------------------------------------------------------------------------*/

// The walkers that read a whole free list (the idle sweep, the forced collect and the
// thread-free collect; see the "Free list corruption" section in `src/page.c`) must survive a
// corrupted list: report it once through the error handler, cut the list, leave the page's
// accounting consistent, and never hand out the block that held the bad link.
//
// Each test corrupts a list the way the crashes in the field look (oven-sh/bun BUN-40BH and
// its siblings): the first words of a block were overwritten after it was freed. The bad link
// points 8 bytes into the block itself, so that a build that encodes its free lists (whose
// `mi_block_next` already rejects a link into another page) and a release build take the same
// path; the word at that address is 0xA0D, the value seen most often in the field. On a build
// without the checks these tests dereference 0xA0D and fault (and `free-twice` loops forever).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>

#include "mimalloc.h"
#include "mimalloc/internal.h"   // _mi_ptr_page, mi_block_next, mi_block_set_next, mi_page_thread_free, _mi_page_purged_count

#include "testhelper.h"

// ---------------------------------------------------------------------------
// The error handler and the output hook. A corruption must be reported as an
// EFAULT, and the walker under test must report it exactly once: that is
// counted on the message text, because a debug build also walks the lists in
// its assertions (`_mi_page_is_valid`) and reports a bad link from there first.
// ---------------------------------------------------------------------------

static int         error_count = 0;
static int         error_last  = 0;
static const char* report_needle = NULL;    // the message the walker under test is expected to print ...
static int         report_count  = 0;       // ... and how often it was printed

static void on_error(int err, void* arg) {
  MI_UNUSED(arg);
  error_count++;
  error_last = err;
}

static void on_output(const char* msg, void* arg) {
  MI_UNUSED(arg);
  fputs(msg, stderr);
  if (report_needle != NULL && strstr(msg, report_needle) != NULL) { report_count++; }
}

static void expect_report(const char* needle) {
  report_needle = needle;
  report_count = 0;
  error_count = 0;
  error_last = 0;
}

static bool reported_once(const char* what) {
  if (report_count != 1 || error_count < 1 || error_last != EFAULT) {
    fprintf(stderr, "\n  %s: expected one \"%s\" report and an EFAULT, got %d report(s) and %d error(s), the last one %d\n",
            what, report_needle, report_count, error_count, error_last);
    return false;
  }
  return true;
}

#define REPORT_FREE         "corrupted free list in page"
#define REPORT_LOCAL_FREE   "corrupted local free list in page"
#define REPORT_THREAD_FREE  "corrupted thread-free list in page"

// ---------------------------------------------------------------------------
// a page we control: NBLOCKS blocks in one page of a fresh heap; the odd ones
// are freed and collected onto `page->free`, the even ones stay live and hold
// a pattern. With every other block live no OS page is ever discardable, so
// the freed blocks stay on the list.
// ---------------------------------------------------------------------------

#define NBLOCKS   (24)
#define BSIZE     (1000)

typedef struct victim_s {
  mi_heap_t*  heap;
  mi_page_t*  page;
  void*       block[NBLOCKS];
  bool        live[NBLOCKS];
} victim_t;

static uint8_t pattern_byte(size_t id, size_t off) {
  return (uint8_t)((id * 131u) ^ (off * 7u) ^ (off >> 8));
}

static bool pattern_intact(const victim_t* v) {
  for (size_t i = 0; i < NBLOCKS; i++) {
    if (!v->live[i]) continue;
    const uint8_t* const b = (const uint8_t*)v->block[i];
    for (size_t off = 0; off < BSIZE; off++) {
      if (b[off] != pattern_byte(i, off)) {
        fprintf(stderr, "\n  live block %zu was damaged at offset %zu\n", i, off);
        return false;
      }
    }
  }
  return true;
}

static bool victim_setup(victim_t* v) {
  memset(v, 0, sizeof(*v));
  v->heap = mi_heap_new();
  if (v->heap == NULL) { fprintf(stderr, "\n  mi_heap_new failed\n"); return false; }
  for (size_t i = 0; i < NBLOCKS; i++) {
    uint8_t* const b = (uint8_t*)mi_heap_malloc(v->heap, BSIZE);
    if (b == NULL) { fprintf(stderr, "\n  mi_heap_malloc failed\n"); return false; }
    for (size_t off = 0; off < BSIZE; off++) { b[off] = pattern_byte(i, off); }
    v->block[i] = b;
    v->live[i] = true;
  }
  v->page = _mi_ptr_page(v->block[0]);
  for (size_t i = 1; i < NBLOCKS; i++) {
    if (_mi_ptr_page(v->block[i]) != v->page) { fprintf(stderr, "\n  the blocks do not share one page\n"); return false; }
  }
  for (size_t i = 1; i < NBLOCKS; i += 2) {
    mi_free(v->block[i]);
    v->live[i] = false;
  }
  error_count = 0;
  mi_on_thread_idle();      // collects the frees onto `page->free`
  if (error_count != 0) { fprintf(stderr, "\n  the sweep of an intact page reported an error\n"); return false; }
  if (v->page->free == NULL || mi_block_next(v->page, v->page->free) == NULL) {
    fprintf(stderr, "\n  expected at least two blocks on page->free\n");
    return false;
  }
  return true;
}

static void victim_done(victim_t* v) {
  if (v->heap != NULL) { mi_heap_destroy(v->heap); }
  v->heap = NULL;
}

// Overwrite the first words of a freed block the way the field crashes look: its link points
// 8 bytes into the block itself, and the word there is 0xA0D. Returns the bad link.
static mi_block_t* scribble(const mi_page_t* page, void* block) {
  uintptr_t* const inside = (uintptr_t*)((uint8_t*)block + sizeof(mi_block_t));
  *inside = (uintptr_t)0xA0D;
  mi_block_set_next(page, (mi_block_t*)block, (mi_block_t*)inside);
  return (mi_block_t*)inside;
}

// Walks are bounded so that a failing test cannot hang on a list that is still broken.
static size_t list_count(const mi_page_t* page, mi_block_t* head) {
  size_t n = 0;
  for (mi_block_t* b = head; b != NULL && n <= (size_t)page->capacity + 1; b = mi_block_next(page, b)) { n++; }
  return n;
}

static size_t list_occurrences(const mi_page_t* page, mi_block_t* head, const void* block) {
  size_t n = 0, found = 0;
  for (mi_block_t* b = head; b != NULL && n <= (size_t)page->capacity + 1; b = mi_block_next(page, b)) {
    n++;
    if ((const void*)b == block) { found++; }
  }
  return found;
}

static bool on_no_list(const victim_t* v, const void* block) {
  if (list_occurrences(v->page, v->page->free, block) != 0 ||
      list_occurrences(v->page, v->page->local_free, block) != 0 ||
      list_occurrences(v->page, mi_page_thread_free(v->page), block) != 0) {
    fprintf(stderr, "\n  the block that held the bad link is still on a list\n");
    return false;
  }
  return true;
}

// The conservation of blocks that a cut must leave intact: `used` (which includes the blocks
// on the thread-free list) + the blocks on `free` and `local_free` + the discarded blocks.
static bool page_accounts(const victim_t* v, const char* when) {
  const mi_page_t* const page = v->page;
  const size_t listed = list_count(page, page->free) + list_count(page, page->local_free);
  const size_t discarded = _mi_page_purged_count(page);
  if ((size_t)page->used + listed + discarded != (size_t)page->capacity) {
    fprintf(stderr, "\n  %s: used %u + listed %zu + discarded %zu != capacity %u\n",
            when, (unsigned)page->used, listed, discarded, (unsigned)page->capacity);
    return false;
  }
  return true;
}

typedef void (pass_fun_t)(victim_t* v);
static void pass_sweep(victim_t* v)   { MI_UNUSED(v); mi_on_thread_idle(); }
static void pass_collect(victim_t* v) { mi_heap_collect(v->heap, true); }

// After a cut the page must be an ordinary page again: another pass reports nothing, the
// accounting holds, the live blocks are intact, and `culprit` (the block that held the bad
// link; NULL when the head itself was bad) is never handed out again.
static bool page_recovered(victim_t* v, const void* culprit, pass_fun_t* pass) {
  error_count = 0;
  pass(v);
  if (error_count != 0) { fprintf(stderr, "\n  the second pass reported %d error(s)\n", error_count); return false; }
  if (!page_accounts(v, "after the second pass")) return false;
  void* got[4 * NBLOCKS];
  bool handed_out = false;
  for (size_t i = 0; i < 4 * NBLOCKS; i++) {
    got[i] = mi_heap_malloc(v->heap, BSIZE);
    if (culprit != NULL && got[i] == culprit) { handed_out = true; }
  }
  for (size_t i = 0; i < 4 * NBLOCKS; i++) { mi_free(got[i]); }
  if (handed_out) { fprintf(stderr, "\n  the block that held the bad link was handed out again\n"); return false; }
  return pattern_intact(v);
}

// ---------------------------------------------------------------------------
// 1. a bad link in the middle of `page->free` (the idle sweep's walk): the
//    list is cut in front of the block holding it
// ---------------------------------------------------------------------------

static bool test_free_link(void) {
  victim_t v;
  bool ok_ = victim_setup(&v);
  if (ok_) {
    mi_block_t* const first = v.page->free;
    mi_block_t* const culprit = mi_block_next(v.page, first);    // the second block: something stays in front of the cut
    scribble(v.page, culprit);
    expect_report(REPORT_FREE);
    mi_on_thread_idle();
    ok_ = reported_once("free-link")
       && (v.page->free == first && mi_block_next(v.page, first) == NULL)
       && on_no_list(&v, culprit)
       && page_accounts(&v, "after the cut")
       && page_recovered(&v, culprit, &pass_sweep);
  }
  victim_done(&v);
  return ok_;
}

// ---------------------------------------------------------------------------
// 2. the head of `page->free` itself is bad: the whole list is dropped
// ---------------------------------------------------------------------------

static bool test_free_head(void) {
  victim_t v;
  bool ok_ = victim_setup(&v);
  if (ok_) {
    v.page->free = scribble(v.page, v.page->free);
    expect_report(REPORT_FREE);
    mi_on_thread_idle();
    ok_ = reported_once("free-head")
       && (v.page->free == NULL)
       && page_accounts(&v, "after the cut")
       && page_recovered(&v, NULL, &pass_sweep);
  }
  victim_done(&v);
  return ok_;
}

// ---------------------------------------------------------------------------
// 3. a block that links to itself, as after a double free: the sweep must
//    neither loop nor count the block twice (an OS page is discarded when every
//    block in it is free, and a block counted twice can make it look that way).
//    The block is an ordinary free block afterwards, so it may be handed out.
// ---------------------------------------------------------------------------

static bool test_free_twice(void) {
  victim_t v;
  bool ok_ = victim_setup(&v);
  if (ok_) {
    mi_block_t* const twice = mi_block_next(v.page, v.page->free);
    mi_block_set_next(v.page, twice, twice);
    expect_report(REPORT_FREE);
    mi_on_thread_idle();
    ok_ = reported_once("free-twice")
       && (list_occurrences(v.page, v.page->free, twice) == 1)    // its first occurrence stays ...
       && (mi_block_next(v.page, twice) == NULL)                   // ... as the end of the list
       && page_accounts(&v, "after the cut");
    if (ok_) {
      error_count = 0;
      mi_on_thread_idle();
      ok_ = (error_count == 0) && page_accounts(&v, "after the second sweep") && pattern_intact(&v);
      if (error_count != 0) { fprintf(stderr, "\n  the second sweep reported %d error(s)\n", error_count); }
    }
  }
  victim_done(&v);
  return ok_;
}

// ---------------------------------------------------------------------------
// 4. a bad link on `page->local_free`, which the sweep's forced collect
//    appends to `free`: the part in front of the cut is still appended
// ---------------------------------------------------------------------------

static bool test_local_free_link(void) {
  victim_t v;
  bool ok_ = victim_setup(&v);
  if (ok_) {
    // frees by the owning thread go onto `local_free`, most recent first: block 2 -> block 0
    mi_free(v.block[0]); v.live[0] = false;
    mi_free(v.block[2]); v.live[2] = false;
    ok_ = (v.page->local_free == (mi_block_t*)v.block[2] && mi_block_next(v.page, v.page->local_free) == (mi_block_t*)v.block[0]);
    if (!ok_) { fprintf(stderr, "\n  local_free is not [block 2, block 0]\n"); }
  }
  if (ok_) {
    scribble(v.page, v.block[0]);     // the last block of the list: the cut drops exactly this one
    expect_report(REPORT_LOCAL_FREE);
    mi_on_thread_idle();
    ok_ = reported_once("local-free-link")
       && on_no_list(&v, v.block[0])
       && (list_occurrences(v.page, v.page->free, v.block[2]) == 1)
       && (v.page->local_free == NULL)
       && page_accounts(&v, "after the cut")
       && page_recovered(&v, v.block[0], &pass_sweep);
  }
  victim_done(&v);
  return ok_;
}

// ---------------------------------------------------------------------------
// 5. a bad link on the thread-free list (a block freed by another thread):
//    the collect drops the list it took off the page, and the blocks on it
//    stay accounted as used
// ---------------------------------------------------------------------------

static void* freed_by_other_thread = NULL;

static bool free_on_other_thread(void) {
  mi_free(freed_by_other_thread);
  return true;
}

static bool test_thread_free_link(void) {
  victim_t v;
  bool ok_ = victim_setup(&v);
  if (ok_) {
    freed_by_other_thread = v.block[0];
    v.live[0] = false;
    ok_ = mi_run_on_thread(&free_on_other_thread) && (mi_page_thread_free(v.page) == (mi_block_t*)v.block[0]);
    if (!ok_) { fprintf(stderr, "\n  the block freed on the other thread is not the head of the thread-free list\n"); }
  }
  if (ok_) {
    const uint32_t used_before = v.page->used;     // a block on the thread-free list still counts as used
    scribble(v.page, v.block[0]);
    expect_report(REPORT_THREAD_FREE);
    mi_heap_collect(v.heap, true);
    ok_ = reported_once("thread-free-link")
       && (mi_page_thread_free(v.page) == NULL)
       && on_no_list(&v, v.block[0])
       && (v.page->used == used_before)
       && page_accounts(&v, "after the drop")
       && page_recovered(&v, v.block[0], &pass_collect);
    if (ok_ == false && v.page->used != used_before) { fprintf(stderr, "\n  used changed from %u to %u\n", (unsigned)used_before, (unsigned)v.page->used); }
  }
  victim_done(&v);
  return ok_;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

// An optional argument selects a single test, which is how each one is checked to fault (or
// loop) on a build without the checks: the first one to run would otherwise take the rest with it.
static bool selected(int argc, char** argv, const char* name) {
  return (argc < 2 || strcmp(argv[1], name) == 0);
}

int main(int argc, char** argv) {
  mi_version();
  mi_register_error(&on_error, NULL);
  mi_register_output(&on_output, NULL);
  mi_option_set(mi_option_show_errors, 1);            // the messages are part of what is tested: they have to format without faulting
  mi_option_set(mi_option_purge_holes_full_every, 1); // walk every page on every sweep: a scribbled link changes nothing the skip check looks at

  if (mi_option_is_enabled(mi_option_purge_holes)) {
    if (selected(argc, argv, "free-link"))       { CHECK("free-link",       test_free_link()); }
    if (selected(argc, argv, "free-head"))       { CHECK("free-head",       test_free_head()); }
    if (selected(argc, argv, "free-twice"))      { CHECK("free-twice",      test_free_twice()); }
    if (selected(argc, argv, "local-free-link")) { CHECK("local-free-link", test_local_free_link()); }
  }
  else {
    fprintf(stderr, "purge_holes is off: the sweep tests are skipped\n");
  }
  if (selected(argc, argv, "thread-free-link")) { CHECK("thread-free-link", test_thread_free_link()); }

  return print_test_summary();
}
