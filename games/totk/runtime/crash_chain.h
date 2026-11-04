/* The crash handlers' bookkeeping (loader.c crash and abort_report), as code the host test (tests/test_crash_chain.py)
   runs unchanged. Our handlers replace whatever the process had for SIGSEGV, SIGBUS, SIGILL, SIGTRAP and SIGABRT
   (Android: the debuggerd handler that writes the tombstone and the logcat backtrace; macOS: none, the crash reporter
   sees the signal's default action). That previous action is kept, and after our report the signal goes back to it:
   - crash_chain_install: sigaction keeping the previous action;
   - crash_chain_claim: the first crashing thread reports; a nested crash of that thread skips its report; any other
     crashing thread waits in crash_chain_wait (pause) until the first one has handed the process on;
   - crash_chain_forward: flush stdout, restore the previous action, and for a synchronous fault (SIGSEGV, SIGBUS,
     SIGILL or SIGFPE raised by the kernel for an instruction) return, so the instruction faults again into it with its
     original siginfo; anything else (SIGABRT, SIGTRAP, a signal sent with kill) is raised again, delivered to the
     previous action when our handler returns. An ignored previous action becomes the default one: a crash we
     reported must still end the process. */
#pragma once
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

enum { CRASH_CHAIN_SIGNALS = 5 };
enum { CRASH_CHAIN_FIRST, CRASH_CHAIN_NESTED, CRASH_CHAIN_OTHER };
static const int crash_chain_signal[CRASH_CHAIN_SIGNALS] = { SIGSEGV, SIGBUS, SIGILL, SIGTRAP, SIGABRT };
static struct sigaction crash_chain_prev[CRASH_CHAIN_SIGNALS];
/* The first crashing thread (pthread_self, never 0), 0 until then. Not a __thread variable: in a dlopen'ed library
   (Android's libtotk.so) the first touch of TLS on a thread may allocate, which a crash inside malloc cannot afford. */
static uintptr_t crash_chain_owner;

static inline int crash_chain_slot(int num) {
    for (int i = 0; i < CRASH_CHAIN_SIGNALS; i++) if (crash_chain_signal[i] == num) return i;
    return -1;
}
static inline int crash_chain_install(int num, const struct sigaction *ours) {
    int i = crash_chain_slot(num);
    return i < 0 ? -1 : sigaction(num, ours, &crash_chain_prev[i]);
}
static inline int crash_chain_claim(void) {
    uintptr_t self = (uintptr_t)pthread_self(), expected = 0;
    if (__atomic_compare_exchange_n(&crash_chain_owner, &expected, self, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return CRASH_CHAIN_FIRST;
    return expected == self ? CRASH_CHAIN_NESTED : CRASH_CHAIN_OTHER;
}
static inline void crash_chain_wait(void) { for (;;) pause(); }
static inline int crash_chain_synchronous(int num, const siginfo_t *info) {
    return (num == SIGSEGV || num == SIGBUS || num == SIGILL || num == SIGFPE) && info && info->si_code > 0;
}
static inline void crash_chain_forward(int num, const siginfo_t *info) {
    /* stderr is unbuffered; stdout may hold a line. Never wait for a stdio lock another (stopped) thread holds. */
    if (!ftrylockfile(stdout)) { fflush(stdout); funlockfile(stdout); }
    int i = crash_chain_slot(num);
    struct sigaction prev;
    if (i >= 0) prev = crash_chain_prev[i];
    else { prev = (struct sigaction){0}; prev.sa_handler = SIG_DFL; sigemptyset(&prev.sa_mask); }
    if (!(prev.sa_flags & SA_SIGINFO) && prev.sa_handler == SIG_IGN) prev.sa_handler = SIG_DFL;
    sigaction(num, &prev, NULL);
    if (!crash_chain_synchronous(num, info)) raise(num);   /* blocked while our handler runs: delivered on its return */
}
