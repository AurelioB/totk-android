/* Test-only linkage for CPU synchronization probes on non-ARM64 Linux.
 * C test entries run on the pthread stack; ARM64 guest entry/stack switching and
 * asynchronous preemption are deliberately outside these host tests' scope. */
#include "../../runtime/runtime.h"
#include <stdlib.h>
uint64_t runtime_hle_tsd_off;
int runtime_game_code_pc(uint64_t pc) { (void)pc; return 0; }
int runtime_guest_code_pc(uint64_t pc) { (void)pc; return 0; }
/* The real loader allocates gate/TLS keys before the scheduler; g_keythr uses
 * zero as its not-initialized sentinel. Preserve that initialization order. */
__attribute__((constructor)) static void reserve_loader_key(void) {
    pthread_key_t key; if (pthread_key_create(&key, NULL)) abort();
}
void dm_thread_enter(uint64_t entry, uint64_t arg, uint64_t sp, uint64_t *context) {
    (void)sp; (void)context;
    ((void (*)(uint64_t))(uintptr_t)entry)(arg);
    runtime_fail("test thread entry returned");
}
_Noreturn void runtime_thread_leave(const uint64_t *context, void *arg, void (*fn)(void *)) {
    (void)context; fn(arg); abort();
}
int host_safe_read(uint64_t address, void *out, size_t size) {
    /* Diagnostics must not dereference arbitrary guest addresses in a CPU probe. */
    (void)address; (void)out; (void)size; return 0;
}
