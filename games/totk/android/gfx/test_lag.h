/* A writer must run independently of the GPU gate: wait-mode protection intentionally blocks its first store. */
#pragma once
#include "nvn_dirty.h"
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>

typedef struct { void *dst; size_t n; int byte, host; _Atomic int done; } lag_writer;
extern void dm_guest_write_prepare(void *addr, size_t n);
/* A libc/TSan memset interceptor describes its write BEFORE the first machine store faults. Make the ownership
   transition with a real guest-style native store first; the following fully instrumented memset then observes the
   actual post-fault ordering. This is not a sanitizer suppression: an unprotected probe still performs the bad write. */
static void lag_guest_store(void *dst, uint8_t byte) {
#if defined(__x86_64__)
    __asm__ volatile("movb %b1, (%0)" : : "r"(dst), "q"(byte) : "memory");
#elif defined(__aarch64__)
    __asm__ volatile("strb %w1, [%0]" : : "r"(dst), "r"((unsigned)byte) : "memory");
#else
    *(volatile uint8_t *)dst = byte;
#endif
}
static void *lag_writer_run(void *arg) {
    lag_writer *w = arg;
    if (w->host) dm_guest_write_prepare(w->dst, w->n);
    if (w->n) lag_guest_store(w->dst, (uint8_t)w->byte);
    memset(w->dst, w->byte, w->n);
    atomic_store(&w->done, 1);
    return NULL;
}
static int lag_test_rewrite(void *dst, size_t n, int byte, int host, VkEvent gate) {
    uint64_t faults0, host0, ns, faults, hosts;
    nvn_dirty_lag_stats(&faults0, &host0, &ns);
    lag_writer w = {.dst = dst, .n = n, .byte = byte, .host = host};
    pthread_t thread;
    if (pthread_create(&thread, NULL, lag_writer_run, &w)) abort();
    /* Either an unprotected/copy-mode write finishes, or the protection reports its actual wait before we open the
       gate. This proves the race without sleeps or a scheduler-dependent 'probably already blocked' assertion. */
    do {
        nvn_dirty_lag_stats(&faults, &hosts, &ns);
        if (faults != faults0 || hosts != host0) break;
        sched_yield();
    } while (!atomic_load(&w.done));
    int waited = !atomic_load(&w.done);
    VKCHECK(vkSetEvent(V.dev, gate));
    pthread_join(thread, NULL);
    return waited;
}

