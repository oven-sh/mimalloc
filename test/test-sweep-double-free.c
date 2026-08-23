/* A block freed twice puts a cycle in its page's free lists. That is the program's bug and the
   allocator cannot undo it, but the idle sweep must not make it worse: it has to terminate, and
   it must not discard an OS page based on counts from such a list (the duplicate would make an
   OS page that still holds a live block look entirely free). On the sweep as first written this
   test hangs in `mi_page_purge_holes_walk`. A debug build refuses the second free up front
   (`mi_check_is_double_free`), so there the test only shows that nothing else breaks. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <mimalloc.h>
#ifndef _WIN32
#include <unistd.h>
#include <signal.h>
static void on_alarm(int sig) { (void)sig; fprintf(stderr, "\n  the sweep did not terminate\n"); _exit(2); }
#endif

#define BSIZE  1024
#define COUNT  64

int main(void) {
  mi_option_set(mi_option_purge_holes_eager_zero, 1);   // a wrong discard shows as zeroed data even where the OS discards lazily
  mi_option_set(mi_option_purge_holes_min_interval, 0);
#ifndef _WIN32
  signal(SIGALRM, on_alarm);
  alarm(30);
#endif
  const size_t os_page = 4096;   // lower bound; the grouping below only needs blocks that share *some* OS page
  uint8_t* p[COUNT];
  for (int i = 0; i < COUNT; i++) { p[i] = (uint8_t*)mi_malloc(BSIZE); memset(p[i], 0xC0 | i, BSIZE); }

  // a live block and, right behind it, two freed ones of which one is freed again; the live block
  // and the double-freed one overlap the same OS page (the distance between blocks is the padded
  // block size in a debug build, so measure it)
  int k = -1;
  for (int i = 0; i + 2 < COUNT; i++) {
    const ptrdiff_t stride = p[i+1] - p[i];
    if (stride >= BSIZE && p[i+2] - p[i+1] == stride &&
        ((uintptr_t)(p[i] + stride - 1) / os_page) == ((uintptr_t)p[i+1] / os_page)) { k = i; break; }
  }
  if (k < 0) { fprintf(stderr, "no two neighbouring blocks share an OS page (block size %d): nothing to test\n", BSIZE); return 0; }
  mi_free(p[k+1]); mi_free(p[k+2]);
  fprintf(stderr, "test: double-free-sweep...  ");
  mi_free(p[k+1]);   // the double free

  mi_on_thread_idle();   // must return
  mi_on_thread_idle();

  for (size_t j = 0; j < BSIZE; j++) {
    if (p[k][j] != (uint8_t)(0xC0 | k)) { fprintf(stderr, "\n  the live block next to the double-freed one lost its contents at byte %zu\n", j); return 1; }
  }
  fprintf(stderr, "ok.\n");
  return 0;   // the page with the broken list is deliberately left alone
}
