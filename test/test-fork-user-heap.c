/* ----------------------------------------------------------------------------
Copyright (c) Microsoft Research, Daan Leijen
This is free software; you can redistribute it and/or modify it under the
terms of the MIT license.
-----------------------------------------------------------------------------*/

/* Reproducers for two fork() issues in the Bun fork-safety patch (init.c
   _mi_process_fork_prepare/_mi_process_fork_child):

   Case A: fork_prepare only acquires heap_main locks, not user-heap
   theaps_lock. A concurrent _mi_theap_init mutating a user heap's theaps
   list races the fork; the child can inherit a half-linked list.

   Case B: fork_child re-inits heap-level locks for all heaps but only
   tld_main.theaps_lock and the surviving thread's tld->theaps_lock. If a
   thread that vanished at fork time held its tld->theaps_lock (e.g. inside
   _mi_theap_init), the child deadlocks the first time it touches that tld
   via mi_heap_delete -> _mi_theap_free -> theap.c:322.

   Case A is probabilistic. Case B is made deterministic via the
   MI_DEBUG-gated mi_debug_stall_in_theap_init hook (see src/theap.c).

   Expected with bugs present: case_b child times out (SIGALRM -> exit 2).
*/

#ifdef _WIN32
#include <stdio.h>
int main(void) { fprintf(stderr, "test-fork-user-heap: skipped on Windows\n"); return 0; }
#else

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <stdatomic.h>
#include <mimalloc.h>

#if MI_DEBUG > 0
extern volatile int mi_debug_stall_in_thread_theaps_done;
#endif

static mi_heap_t* g_heap;
static atomic_int g_stop;

static void* thrash_thread(void* arg) {
  (void)arg;
  while (!atomic_load(&g_stop)) {
    void* p = mi_heap_malloc(g_heap, 32);
    mi_free(p);
  }
  return NULL;
}

static void on_alarm(int sig) {
  (void)sig;
  _exit(2); // child deadlocked
}

// ---------------------------------------------------------------------------
// Case A: probabilistic race between fork() and _mi_theap_init on user heap
// ---------------------------------------------------------------------------
static int case_a(void) {
  fprintf(stderr, "case_a: fork while another thread mutates user-heap theaps list\n");
  g_heap = mi_heap_new();
  atomic_store(&g_stop, 0);
  pthread_t t;
  pthread_create(&t, NULL, thrash_thread, NULL);
  int child_failures = 0;
  for (int i = 0; i < 200; i++) {
    pid_t pid = fork();
    if (pid == 0) {
      // child: walk and delete the user heap; if the theaps list is
      // half-linked we crash or hang here.
      signal(SIGALRM, on_alarm);
      alarm(5);
      mi_heap_collect(g_heap, true);
      mi_heap_delete(g_heap);
      _exit(0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
      fprintf(stderr, "  case_a iter %d: child status=0x%x (signal=%d exit=%d)\n",
              i, status, WIFSIGNALED(status) ? WTERMSIG(status) : 0,
              WIFEXITED(status) ? WEXITSTATUS(status) : -1);
      child_failures++;
    }
  }
  atomic_store(&g_stop, 1);
  pthread_join(t, NULL);
  mi_heap_delete(g_heap);
  fprintf(stderr, "case_a: %d/200 child failures\n", child_failures);
  return child_failures;
}

// ---------------------------------------------------------------------------
// Case B: deterministic — fork while another thread holds tld->theaps_lock
// inside _mi_theap_init; child should be able to mi_heap_delete without
// blocking on that vanished thread's tld lock.
// ---------------------------------------------------------------------------
#if MI_DEBUG > 0
static void* stall_thread(void* arg) {
  (void)arg;
  // allocate so a theap for this thread is linked onto g_heap->theaps,
  // then on return mi_thread_done -> mi_thread_theaps_done parks while
  // holding this thread's tld->theaps_lock.
  void* p = mi_heap_malloc(g_heap, 32);
  mi_free(p);
  return NULL;
}

static int case_b(void) {
  fprintf(stderr, "case_b: fork while sibling thread holds tld->theaps_lock in mi_thread_theaps_done\n");
  g_heap = mi_heap_new();
  pthread_t t;
  pthread_create(&t, NULL, stall_thread, NULL);
  // let the thread allocate first, then arm the stall and wait for it to
  // park inside mi_thread_theaps_done (signals 2 once the lock is held).
  mi_debug_stall_in_thread_theaps_done = 1;
  while (mi_debug_stall_in_thread_theaps_done != 2) { sched_yield(); }

  pid_t pid = fork();
  if (pid == 0) {
    signal(SIGALRM, on_alarm);
    alarm(5);
    // child: stalled thread is gone; its tld->theaps_lock is still held.
    // fork_child re-inits heap-level locks but not dead-thread tld locks.
    // mi_heap_delete -> mi_heap_free_theaps -> _mi_theap_free -> acquires
    // theap->tld->theaps_lock for the vanished thread's theap -> deadlock.
    mi_heap_delete(g_heap);
    _exit(0);
  }
  int status = 0;
  waitpid(pid, &status, 0);

  // parent: release the stall and clean up
  mi_debug_stall_in_thread_theaps_done = 0;
  pthread_join(t, NULL);
  mi_heap_delete(g_heap);

  int rc = (!WIFEXITED(status) || WEXITSTATUS(status) != 0) ? 1 : 0;
  fprintf(stderr, "case_b: child status=0x%x (%s)\n", status, rc ? "FAIL (deadlock/crash)" : "ok");
  return rc;
}
#endif

int main(void) {
  int rc = 0;
  rc |= (case_a() > 0 ? 1 : 0);
  #if MI_DEBUG > 0
  rc |= case_b();
  #else
  fprintf(stderr, "case_b: skipped (requires MI_DEBUG>0)\n");
  #endif
  return rc;
}

#endif // !_WIN32
