/* Production dispatch, compared with the old linear gate selection. */
#include <assert.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "guest_clock.h"
typedef struct { uint64_t ret, original; unsigned timer, arg; } Gate;
typedef struct { const char *name; uint8_t *base; const Gate *gates; unsigned ngate; } Module;
#define NM 4
static Module modules[NM];
static uint64_t timer_calls, svc_calls, addresses;
static jmp_buf failed;
static const char *called_name;
static uint64_t called_original;
static unsigned called_arg;
uint64_t runtime_now_ns(void) { return 987654321012345ULL; }
static void runtime_fail(const char *s) { (void)s; longjmp(failed, 1); }
static void need(int ok, const char *s) { if (!ok) runtime_fail(s); }
static void runtime_svc(unsigned arg, uint64_t *regs, const char *name, uint64_t original) {
    (void)regs; called_arg = arg; called_name = name; called_original = original;
}
#include "gate_dispatch.inc"
static void check(uint64_t ret) {
    const Gate *expected = NULL; const char *name = NULL;
    /* The previous implementation, with module precedence unchanged. */
    for (unsigned i = 0; i < NM && !expected; ++i)
        for (unsigned j = 0; j < modules[i].ngate; ++j)
            if ((uintptr_t)modules[i].base + modules[i].gates[j].ret == ret) {
                expected = &modules[i].gates[j]; name = modules[i].name; break;
            }
    uint64_t regs[31], before[31];
    for (unsigned i = 0; i < 31; ++i) regs[i] = 0xfeed0000 + i;
    memcpy(before, regs, sizeof regs);
    timer_calls = svc_calls = 0; called_name = NULL;
    if (setjmp(failed)) { assert(!expected); ++addresses; return; }
    runtime_dispatch(ret, regs); assert(expected);
    if (expected->timer) {
        before[expected->arg] = guest_clock_ns_to_ticks(runtime_now_ns());
        assert(timer_calls == 1 && !svc_calls && !called_name);
    } else {
        assert(!timer_calls && svc_calls == 1 && called_name == name);
        assert(called_arg == expected->arg && called_original == expected->original);
    }
    assert(!memcmp(regs, before, sizeof regs)); ++addresses;
}
static void check_input(const Gate *gates, unsigned n) {
    memset(modules, 0, sizeof modules);
    modules[0] = (Module){"actual", (uint8_t *)(uintptr_t)0x100000000ULL, gates, n};
    check(0); check(UINT64_MAX); check(0x100000000ULL - 1); check(0x100000000ULL);
    for (unsigned i = 0; i < n; ++i) {
        uint64_t ret = 0x100000000ULL + gates[i].ret;
        check(ret); check(ret - 1); check(ret + 1);
        /* Between adjacent gate addresses lookup is uniformly unknown. Both
         * endpoints and an interior representative cover each partition. */
        if (i && gates[i].ret - gates[i-1].ret > 2)
            check(0x100000000ULL + gates[i-1].ret + (gates[i].ret - gates[i-1].ret) / 2);
    }
}
#include "gate_inputs.inc"
int main(void) {
    static const Gate a[] = {{0, 11, 0, 0}, {16, 22, 1, 30}, {32, 33, 0, 99}};
    static const Gate b[] = {{0, 44, 1, 1}, {8, 55, 0, 2}, {32, 66, 1, 0}};
    modules[0] = (Module){"first", (uint8_t *)(uintptr_t)100, a, 3};
    modules[1] = (Module){"overlap", (uint8_t *)(uintptr_t)100, b, 3};
    modules[2] = (Module){"empty", (uint8_t *)(uintptr_t)10, NULL, 0};
    modules[3] = (Module){"unloaded", NULL, NULL, 0};
    for (uint64_t ret = 0; ret < 65536; ++ret) check(ret);
    check(UINT64_MAX);
    check_input(NULL, 0); check_input(a, 3); inputs();
    printf("PASS: %llu gate/unknown addresses; timer registers, SVC identity and module precedence\n", (unsigned long long)addresses);
}
