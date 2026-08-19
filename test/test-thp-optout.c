/* ----------------------------------------------------------------------------
Copyright (c) Microsoft Research, Daan Leijen
This is free software; you can redistribute it and/or modify it under the
terms of the MIT license.
-----------------------------------------------------------------------------*/

/* With `allow_thp` off, `unix_mmap` (src/prim/unix/prim.c) opts the mappings it creates out of
   transparent huge pages with madvise(MADV_NOHUGEPAGE). The kernel only gives huge pages to a
   mapping that did not ask for them when /sys/kernel/mm/transparent_hugepage/enabled is
   `[always]`, so that is the only setting under which the allocator may make the call. Under
   `[madvise]` and `[never]` it must not: there the call is one wasted syscall per mmap. When the
   setting cannot be read, the allocator must keep opting out.

   The test interposes `madvise` (the executable's definition wins over libc's, also for the calls
   made from the static library) and counts the MADV_NOHUGEPAGE calls the allocator makes:

   - during process initialization, when `allow_thp` was already off at that point (the ctest entry
     runs with MIMALLOC_ALLOW_THP=0; a build with MI_NO_THP gets the same),
   - for a fresh OS reservation made with `allow_thp` off,
   - and, as a control, for a reservation made with `allow_thp` on: never.

   Expected with the bug present, on a system whose setting is `[madvise]` or `[never]`: the first
   two counts are not zero.  Linux only: elsewhere the test passes without doing anything. */

#if !defined(__linux__)
#include <stdio.h>
int main(void) { printf("test-thp-optout: skipped, transparent huge pages are Linux only\n"); return 0; }
#else

#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE   // madvise, MADV_NOHUGEPAGE
#endif
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <mimalloc.h>

static long nohugepage_calls;   // counted from process start: the allocator initializes before main

int madvise(void* addr, size_t len, int advice) {
  if (advice == MADV_NOHUGEPAGE) { nohugepage_calls++; }
  return (int)syscall(SYS_madvise, addr, len, advice);
}

// Returns the current value between the brackets, or "" when the setting cannot be read.
static const char* thp_setting(char* buf, size_t bufsize) {
  int fd = open("/sys/kernel/mm/transparent_hugepage/enabled", O_RDONLY);
  if (fd < 0) return "";
  ssize_t n = read(fd, buf, bufsize - 1);
  close(fd);
  if (n <= 0) return "";
  buf[n] = 0;
  char* start = strchr(buf, '[');
  char* end = (start == NULL ? NULL : strchr(start, ']'));
  if (end == NULL) return "";
  *end = 0;
  return start + 1;
}

static int failures;

static void check(const char* what, long calls, bool expect_calls) {
  const bool ok = (expect_calls ? calls > 0 : calls == 0);
  printf("%s %s: %ld MADV_NOHUGEPAGE call(s), expected %s\n", (ok ? "ok  " : "FAIL"), what, calls, (expect_calls ? "at least one" : "none"));
  if (!ok) failures++;
}

// A fresh arena reservation always creates at least one new mapping. Uncommitted: only address space.
static long nohugepage_calls_for_one_reservation(void) {
  const long before = nohugepage_calls;
  if (mi_reserve_os_memory(mi_arena_min_size(), false /* commit */, false /* allow_large */) != 0) {
    printf("FAIL mi_reserve_os_memory failed\n");
    failures++;
  }
  return nohugepage_calls - before;
}

int main(void) {
  char buf[128];
  const char* setting = thp_setting(buf, sizeof(buf));
  const bool readable = (setting[0] != 0);
  const bool expect_optout = (!readable || strcmp(setting, "always") == 0);
  printf("transparent_hugepage/enabled: %s\n", (readable ? setting : "(not readable)"));

  mi_free(mi_malloc(1));   // make sure the allocator is initialized
  const long at_startup = nohugepage_calls;
  if (!mi_option_is_enabled(mi_option_allow_thp)) {
    check("process initialization with allow_thp off", at_startup, expect_optout);
  }
  else {
    printf("skip process initialization: allow_thp was on (run with MIMALLOC_ALLOW_THP=0 to cover it)\n");
  }

  mi_option_disable(mi_option_allow_thp);
  check("reservation with allow_thp off", nohugepage_calls_for_one_reservation(), expect_optout);

  mi_option_enable(mi_option_allow_thp);
  check("reservation with allow_thp on", nohugepage_calls_for_one_reservation(), false);

  return (failures == 0 ? 0 : 1);
}

#endif
