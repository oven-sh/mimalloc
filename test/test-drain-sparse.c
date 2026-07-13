// Does page_drain_sparse actually redirect allocations away from sparse pages?
#include <mimalloc.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#define N 200000
static void* blocks[N];
int main(void) {
  mi_option_set(mi_option_page_drain_sparse, 8);
  mi_option_set(mi_option_purge_holes, 1);
  // wave 1: fill pages with 100B allocs (class 128)
  for (int i = 0; i < N; i++) blocks[i] = mi_malloc(100);
  // record the set of 64KB page bases used by wave 1
  // free ~97% (keep every 40th) -> all pages sparse (12/512 used = 2.3% < 12.5%)
  for (int i = 0; i < N; i++) if (i % 40 != 0) { mi_free(blocks[i]); blocks[i] = NULL; }
  // wave 2: allocate again; with drain-sparse these must NOT land in wave-1 pages
  int in_old = 0, fresh = 0;
  for (int i = 0; i < 30000; i++) {
    void* p = mi_malloc(100);
    uintptr_t base = (uintptr_t)p & ~((uintptr_t)0xFFFF);
    int found = 0;
    for (int j = 0; j < N; j += 40) { // survivors mark old pages
      if (blocks[j] && (((uintptr_t)blocks[j] & ~((uintptr_t)0xFFFF)) == base)) { found = 1; break; }
    }
    if (found) in_old++; else fresh++;
  }
  int ok = (in_old < fresh / 10);
  printf("wave2: %d in old sparse pages, %d in fresh pages -> drain-sparse %s\n",
         in_old, fresh, ok ? "FIRES" : "DOES NOT FIRE");
  return ok ? 0 : 1;
}
