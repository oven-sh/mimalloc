/* ----------------------------------------------------------------------------
Copyright (c) 2026, Microsoft Research, Daan Leijen
This is free software; you can redistribute it and/or modify it under the
terms of the MIT license. A copy of the license can be found in the file
"LICENSE" at the root of this distribution.
-----------------------------------------------------------------------------*/

// The wake of the scavenger on Windows: issued while the process lives, and not issued once the process exits.
//
// `ExitProcess` terminates every other thread first. Then the exiting thread runs the FLS callbacks, the DLL detach
// routines and the TLS callbacks, and code that frees memory runs in those: the thread local destructors of the
// host (Rust runs them from an FLS callback) and `mi_process_done`. Such a free can schedule a purge, and that wakes
// the scavenger. But the scavenger was blocked in `WaitOnAddress` when it was terminated. ntdll keeps the wait block
// of a waiter on the stack of that waiter and leaves it queued, so `WakeByAddressSingle` reads the stack of a thread
// that no longer exists. Natively that stack is still mapped and nothing shows. Where the stack of a thread goes
// away with the thread (under Intel SDE / Pin) the read is an access violation: oven-sh/bun crashed at exit that
// way in most runs of its baseline check.
//
// The test puts itself between mimalloc and its two imports (it patches the import address table of this
// executable), so it knows when the scavenger is in its wait, and it counts the wakes. One process for each case,
// as the first wake after the exit is the one that matters:
//
//   free, park, stop   An FLS callback at process exit frees a large block (a purge is scheduled), parks, or stops
//                      the scavenger. None of them may issue a wake.
//   gone               The callback first releases the stack of the terminated scavenger, as SDE does, and then
//                      does all three. The process has to survive that.
//   live               While the process lives a free wakes the scavenger and is purged long before the safety
//                      timeout of the scavenger. So is a free from the FLS callback of a thread that ends. And
//                      `mi_scavenger_stop` wakes the scavenger once, joins it, and does nothing the second time.
//
// An exit case returns 1 from `main`. Only the end of its FLS callback ends the process with exit code 0.

#if !defined(_WIN32)
#include <stdio.h>
int main(void) { printf("test-exit-wake: skipped (Windows only)\n"); return 0; }
#else

#include "mimalloc.h"
#include "mimalloc-stats.h"
#include <windows.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define BLOCK_SIZE      ((size_t)8 * 1024 * 1024)   // has a page for itself: a free gives that page back to the arena
#define SAFETY_TIMEOUT  (30000)                     // what the scavenger waits for with nothing scheduled (`mi_scavenger_run`)
#define BOUND_SECS      (10)                        // far below that

static int failures = 0;

static void check(const char* name, bool ok) {
  fprintf(stderr, "test: %s...  %s\n", name, ok ? "ok." : "FAILED");
  if (!ok) failures++;
}

// -----------------------------------------------------------------------------
// Between mimalloc and `WaitOnAddress` / `WakeByAddressSingle`
// -----------------------------------------------------------------------------

typedef BOOL (WINAPI *wait_on_address_fun)(volatile VOID*, PVOID, SIZE_T, DWORD);
typedef VOID (WINAPI *wake_by_address_single_fun)(PVOID);
typedef BOOLEAN (NTAPI *shutdown_in_progress_fun)(void);

static ULONG_PTR     real_wait;       // what mimalloc imported
static ULONG_PTR     real_wake;
static volatile LONG waiting_for;     // the timeout of the wait that the scavenger is in (0: it is not in one)
static volatile LONG wakes;           // calls of `WakeByAddressSingle`
static HANDLE        scavenger;       // the thread that waits (only the scavenger does)
static void*         scavenger_stack; // the reservation that its stack is in

static BOOL WINAPI counted_wait(volatile VOID* address, PVOID compare, SIZE_T size, DWORD timeout) {
  if (scavenger == NULL) {
    MEMORY_BASIC_INFORMATION here;
    char on_stack = 0;
    if (VirtualQuery(&on_stack, &here, sizeof(here)) != 0) { scavenger_stack = here.AllocationBase; }
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &scavenger, 0, FALSE, DUPLICATE_SAME_ACCESS);
  }
  InterlockedExchange(&waiting_for, (LONG)timeout);
  const BOOL woken = ((wait_on_address_fun)real_wait)(address, compare, size, timeout);
  InterlockedExchange(&waiting_for, 0);
  return woken;
}

static VOID WINAPI counted_wake(PVOID address) {
  InterlockedIncrement(&wakes);
  ((wake_by_address_single_fun)real_wake)(address);
}

// The slot of `name` in the import address table of this executable, or NULL.
static ULONG_PTR* import_slot(const char* name) {
  uint8_t* const base = (uint8_t*)GetModuleHandleW(NULL);
  const IMAGE_DOS_HEADER* const dos = (const IMAGE_DOS_HEADER*)base;
  const IMAGE_NT_HEADERS* const nt = (const IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
  const DWORD imports = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
  if (imports == 0) return NULL;
  const IMAGE_IMPORT_DESCRIPTOR* dll = (const IMAGE_IMPORT_DESCRIPTOR*)(base + imports);
  for (; dll->Name != 0; dll++) {
    if (dll->OriginalFirstThunk == 0) continue;   // no names to go by
    const IMAGE_THUNK_DATA* names = (const IMAGE_THUNK_DATA*)(base + dll->OriginalFirstThunk);
    IMAGE_THUNK_DATA* slots = (IMAGE_THUNK_DATA*)(base + dll->FirstThunk);
    for (; names->u1.AddressOfData != 0; names++, slots++) {
      if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
      const IMAGE_IMPORT_BY_NAME* const entry = (const IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData);
      if (strcmp((const char*)entry->Name, name) == 0) return (ULONG_PTR*)&slots->u1.Function;
    }
  }
  return NULL;
}

static bool intercept(const char* name, ULONG_PTR replacement, ULONG_PTR* original) {
  ULONG_PTR* const slot = import_slot(name);
  DWORD protect;
  if (slot == NULL || !VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &protect)) return false;
  *original = *slot;
  *slot = replacement;
  VirtualProtect(slot, sizeof(*slot), protect, &protect);
  return true;
}

// Is the scavenger in its wait with nothing scheduled? Then the next free, park or stop has to wake it.
static bool scavenger_is_idle(void) {
  const time_t deadline = time(NULL) + BOUND_SECS;
  while (waiting_for != SAFETY_TIMEOUT) {
    if (time(NULL) >= deadline) return false;
    Sleep(1);
  }
  return true;
}

static size_t purged(void) {
  mi_stats_t_decl(stats);
  mi_subproc_stats_get_exclusive(mi_subproc_main(), &stats);
  return (size_t)stats.purged.total;
}

static bool purged_since(size_t purged0, size_t size) {
  const time_t deadline = time(NULL) + BOUND_SECS;
  while (purged() - purged0 < size) {
    if (time(NULL) >= deadline) return false;
    Sleep(1);
  }
  return true;
}

static void* alloc_block(void) {
  void* const p = mi_malloc(BLOCK_SIZE);
  if (p != NULL) { memset(p, 1, BLOCK_SIZE); }   // resident, so there is something to purge
  return p;
}

// -----------------------------------------------------------------------------
// At process exit
// -----------------------------------------------------------------------------

typedef enum exit_case_e { EXIT_FREE, EXIT_PARK, EXIT_STOP, EXIT_GONE } exit_case_t;

static exit_case_t exit_case;
static void*       exit_block;
static shutdown_in_progress_fun shutdown_in_progress;

// (not stdio: the C runtime is past `exit` when the FLS callbacks run)
static void say(const char* msg) {
  DWORD written;
  WriteFile(GetStdHandle(STD_ERROR_HANDLE), msg, (DWORD)strlen(msg), &written, NULL);
}

static bool release_scavenger_stack(void) {
  MEMORY_BASIC_INFORMATION stack;
  if (VirtualQuery(scavenger_stack, &stack, sizeof(stack)) == 0 || stack.State == MEM_FREE) {
    say("test-exit-wake: the stack of the terminated scavenger is gone already\n");
    return true;
  }
  if (stack.AllocationBase != scavenger_stack || !VirtualFree(scavenger_stack, 0, MEM_RELEASE)) return false;
  say("test-exit-wake: released the stack of the terminated scavenger\n");
  return true;
}

// Runs on the exiting thread, from `LdrShutdownProcess`: the other threads are terminated, and the DLL detach
// routines and the TLS callbacks (`mi_process_done`) are still to come.
static void WINAPI at_process_exit(void* arg) {
  (void)arg;
  if (failures != 0) return;
  // (`ExitProcess` does not wait for the threads that it terminates: most of the time the scavenger is still on its
  // way out here. It does not run again, but give it the time to be gone before its stack is released.)
  if (WaitForSingleObject(scavenger, BOUND_SECS * 1000) != WAIT_OBJECT_0) {
    say("test-exit-wake: FAILED (the exit did not terminate the scavenger)\n");
    return;
  }
  if (waiting_for != SAFETY_TIMEOUT) {
    say("test-exit-wake: FAILED (the scavenger was not in its wait when it was terminated)\n");
    return;
  }
  if (shutdown_in_progress == NULL || !shutdown_in_progress()) {
    say("test-exit-wake: FAILED (RtlDllShutdownInProgress is not true in the FLS callback)\n");
    return;
  }
  if (exit_case == EXIT_GONE && !release_scavenger_stack()) {
    say("test-exit-wake: FAILED (could not release the stack of the terminated scavenger)\n");
    return;
  }
  const LONG wakes0 = wakes;
  if (exit_case == EXIT_FREE || exit_case == EXIT_GONE) {
    say("test-exit-wake: a free that schedules a purge\n");
    mi_free(exit_block);
  }
  if (exit_case == EXIT_PARK || exit_case == EXIT_GONE) {
    say("test-exit-wake: a park\n");
    if (mi_on_thread_idle_start()) { mi_on_thread_idle_end(); }
  }
  if (exit_case == EXIT_STOP || exit_case == EXIT_GONE) {
    say("test-exit-wake: a stop of the scavenger\n");
    mi_scavenger_stop();
  }
  if (wakes != wakes0) {
    say("test-exit-wake: FAILED (a wake of the scavenger after the exit terminated it)\n");
    return;
  }
  say("test-exit-wake: ok\n");
  TerminateProcess(GetCurrentProcess(), 0);
}

static int test_exit(exit_case_t which) {
  const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  if (ntdll != NULL) {
    shutdown_in_progress = (shutdown_in_progress_fun)(ULONG_PTR)GetProcAddress(ntdll, "RtlDllShutdownInProgress");
  }
  exit_case = which;
  exit_block = alloc_block();
  check("a block to free at exit", exit_block != NULL);
  check("the scavenger is in its wait with nothing scheduled", scavenger_is_idle());
  check("the thread and the stack of the scavenger", scavenger != NULL && scavenger_stack != NULL);
  const DWORD slot = FlsAlloc(&at_process_exit);
  check("an FLS slot", slot != FLS_OUT_OF_INDEXES && FlsSetValue(slot, exit_block));
  fflush(stderr);
  return 1;   // `at_process_exit` makes it 0
}

// -----------------------------------------------------------------------------
// While the process lives
// -----------------------------------------------------------------------------

static DWORD free_slot;

static void WINAPI free_at_thread_exit(void* p) {
  mi_free(p);
}

static DWORD WINAPI thread_that_ends(LPVOID arg) {
  (void)arg;
  void* const p = alloc_block();
  if (p != NULL && !FlsSetValue(free_slot, p)) { mi_free(p); }
  return 0;
}

static int test_live(void) {
  void* const p = alloc_block();
  check("a block to free", p != NULL);
  check("the scavenger is in its wait with nothing scheduled", scavenger_is_idle());
  size_t purged0 = purged();
  LONG wakes0 = wakes;
  mi_free(p);
  check("a free that schedules a purge wakes the scavenger", wakes == wakes0 + 1);
  check("and is purged long before the safety timeout", purged_since(purged0, BLOCK_SIZE));

  free_slot = FlsAlloc(&free_at_thread_exit);
  check("an FLS slot", free_slot != FLS_OUT_OF_INDEXES);
  check("the scavenger is in its wait with nothing scheduled", scavenger_is_idle());
  purged0 = purged();
  wakes0 = wakes;
  const HANDLE thread = CreateThread(NULL, 0, &thread_that_ends, NULL, 0, NULL);
  check("a thread", thread != NULL);
  if (thread != NULL) {
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
  }
  check("a free from the FLS callback of a thread that ends wakes the scavenger", wakes > wakes0);
  check("and is purged long before the safety timeout", purged_since(purged0, BLOCK_SIZE));

  check("the scavenger is in its wait with nothing scheduled", scavenger_is_idle());
  wakes0 = wakes;
  mi_scavenger_stop();
  check("a stop wakes the scavenger once", wakes == wakes0 + 1);
  check("and joins it", WaitForSingleObject(scavenger, 0) == WAIT_OBJECT_0);
  mi_scavenger_stop();
  check("a second stop does nothing", wakes == wakes0 + 1);
  check("a park hands nothing off without a scavenger", !mi_on_thread_idle_start());
  void* const q = alloc_block();
  check("an allocation without a scavenger", q != NULL);
  mi_free(q);
  check("a free without a scavenger wakes nobody", wakes == wakes0 + 1);
  return failures;
}

int main(int argc, char** argv) {
  const char* const which = (argc > 1 ? argv[1] : "free");
  if (!mi_option_is_enabled(mi_option_scavenger) || mi_option_get(mi_option_purge_delay) <= 0) {
    fprintf(stderr, "test-exit-wake: skipped (no scavenger, or no purge delay)\n");
    return 0;
  }
  if (!intercept("WaitOnAddress", (ULONG_PTR)&counted_wait, &real_wait) ||
      !intercept("WakeByAddressSingle", (ULONG_PTR)&counted_wake, &real_wake)) {
    fprintf(stderr, "test-exit-wake: skipped (mimalloc does not import WaitOnAddress and WakeByAddressSingle into this executable)\n");
    return 0;
  }
  mi_option_set(mi_option_minimal_purge_size, 4 /* KiB */);   // purge by the slice: the cases count bytes
  // the scavenger starts when a second thread initializes or a thread first parks
  void* const warm = mi_malloc(64); mi_free(warm);
  if (mi_on_thread_idle_start()) { mi_on_thread_idle_end(); }

  if (strcmp(which, "live") == 0) return test_live();
  if (strcmp(which, "free") == 0) return test_exit(EXIT_FREE);
  if (strcmp(which, "park") == 0) return test_exit(EXIT_PARK);
  if (strcmp(which, "stop") == 0) return test_exit(EXIT_STOP);
  if (strcmp(which, "gone") == 0) return test_exit(EXIT_GONE);
  fprintf(stderr, "test-exit-wake: unknown case %s\n", which);
  return 1;
}

#endif
