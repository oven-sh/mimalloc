// `free(NULL)` must be a no-op even before mimalloc is initialized (issue #1341).
//
// This is not hypothetical: glibc >= 2.44 calls `free(NULL)` unconditionally at the end of
// `newlocale()`, and the C++ runtime calls `newlocale()` from a static initializer while
// constructing `std::locale::classic()`. With a global malloc override (MI_OVERRIDE) that
// `free` lands in mimalloc before any allocation has happened, so before the page map exists.
//
// The 2-level page map resolves a pointer as `submaps[idx][sub_idx]`; for NULL both indices
// are 0. The default (non-secure, non-debug) build uses `_mi_unchecked_ptr_page`, which does
// not test the sub-map for NULL -- so the static "empty" page map must carry a real (zeroed)
// sub-map at entry 0 rather than a NULL one, or this dereferences address 0.
#include <mimalloc.h>
#include <stdio.h>
#include <stdlib.h>

static int early_ran = 0;

#if defined(__GNUC__) || defined(__clang__)
// priority 101 runs before mimalloc's own (default priority) constructor
__attribute__((constructor(101)))
static void free_null_before_init(void) {
  mi_free(NULL);   // goes to mimalloc whether or not the malloc override is enabled
  free(NULL);      // and via the override, when it is
  early_ran = 1;
}
#endif

int main(void) {
  int failures = 0;

  // and once more now that mimalloc is certainly initialized
  mi_free(NULL);
  free(NULL);

#if defined(__GNUC__) || defined(__clang__)
  if (!early_ran) {
    printf("  FAIL: the early constructor did not run\n");
    failures++;
  }
#endif

  printf(failures == 0 ? "ok\n" : "failed\n");
  return (failures == 0 ? 0 : 1);
}
