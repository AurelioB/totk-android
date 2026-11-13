/* MIT. Link nvn_vk.c with section GC: real pool registry/alias resolver,
 * teardown and Site2 selector, without creating a Vulkan device. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#define TOTK_VK_TESTS 1
#include "../../android/gfx/vk_int.h"
#include "../../third_party/d2r_gfx/gfx/nvn/nvn_dirty.c"
#include "backing.h"
#include <signal.h>
static int (*fixture_fault)(void *, int, void *);
static void (*fixture_prep)(void *, size_t);
void dm_fault_hook_register(int (*fn)(void *, int, void *)) { fixture_fault = fn; }
void dm_guest_write_prep_register(void (*fn)(void *, size_t)) { fixture_prep = fn; }
static void handle_fault(int number, siginfo_t *info, void *context) {
    if (number != SIGSEGV || !fixture_fault || !fixture_fault(info->si_addr, 1, context)) _exit(91);
}
static void wait_for(_Atomic int *flag, const char *label) {
    uint64_t deadline = mach_absolute_time() + 5000000000ull;
    while (!atomic_load_explicit(flag, memory_order_acquire)) {
        if (mach_absolute_time() > deadline) { fprintf(stderr, "timeout: %s\n", label); exit(1); }
        sched_yield();
    }
}
static uint8_t *reserve(size_t bytes) {
    void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    required(p != MAP_FAILED, "reserve fixture guest"); return p;
}
static uint8_t *span(uint64_t identity, void *address, size_t requested, size_t expected, int supported) {
    required(vk_wb_pool_lock(identity, address, requested), "pin real pool registration");
    size_t available = SIZE_MAX; uint8_t *result = vk_wb_alias_span_locked((uintptr_t)address, requested, &available);
    CHECK((result != NULL) == supported, "alias span support matches exact owner");
    CHECK(available == expected, "alias span clips at precise owner/view/virtual transition");
    vk_wb_pool_unlock(); return result;
}
static void span_write(uint64_t identity, void *address, size_t size, int enabled, uint8_t value) {
    required(vk_wb_pool_lock(identity, address, size), "pin owner through alias store");
    size_t available = SIZE_MAX; uint8_t *alias = vk_wb_alias_span_locked((uintptr_t)address, size, &available);
    CHECK(vk_wb_alias_range_locked((uintptr_t)address, size) == enabled, "fully supported range uses production span walker");
    if (enabled) { CHECK(alias && available == size, "alias store has full exact storage span"); memset(alias, value, size); }
    vk_wb_pool_unlock();
    if (enabled) { int equal = 1; for (size_t i = 0; i < size; ++i) equal &= ((uint8_t *)address)[i] == value; CHECK(equal, "alias bytes appear at real guest offset"); }
}
static void owner_view_virtual(backing *a, backing *b, uint64_t aid, uint64_t bid, int enabled) {
    size_t page = (size_t)getpagesize();
    uint8_t *view = reserve(3 * page), *virt = reserve(12 * page);
    uint64_t view_id = vk_wb_test_alias_view(view, 3 * page, a->guest, 5 * page);
    uint64_t virtual_id = vk_wb_test_alias_virtual(virt, 12 * page);
    CHECK(view_id && virtual_id, "real synthetic KVIEW and KVIRT registrations");
    span_write(view_id, view + 13, page - 23, enabled, 0x69);
    if (enabled) CHECK(!memcmp(view + 13, a->guest + 5 * page + 13, page - 23), "KVIEW resolves owner base plus nonzero offset and byte offset");
    CHECK(vk_wb_test_alias_map(virt, 0, a->guest, 7 * page, 2 * page), "virtual maps first owner nonzero offset");
    CHECK(vk_wb_test_alias_map(virt, 2 * page, b->guest, 2 * page, 3 * page), "virtual maps adjacent distinct owner");
    span(virtual_id, virt + 13, 5 * page - 13, enabled ? 2 * page - 13 : 0, enabled);
    required(vk_wb_pool_lock(virtual_id, virt, 12 * page), "pin virtual mixed supported range");
    CHECK(vk_wb_alias_range_locked((uintptr_t)virt, 5 * page) == enabled, "range validates across multiple private owners");
    CHECK(!vk_wb_alias_range_locked((uintptr_t)virt, 6 * page), "range rejects unsupported virtual hole");
    size_t available = SIZE_MAX; void *gap = vk_wb_alias_span_locked((uintptr_t)virt + 5 * page, 7 * page, &available);
    CHECK(!gap && available == (enabled ? 7 * page : 0), "unmapped virtual tail is an unsupported span");
    CHECK(!vk_wb_alias_range_locked(UINTPTR_MAX - 3, 8), "overflow is unsupported");
    CHECK(!vk_wb_alias_range_locked((uintptr_t)virt, 0), "empty range is unsupported");
    vk_wb_pool_unlock();
    span_write(virtual_id, virt + 31, page - 42, enabled, 0x51);
    span_write(virtual_id, virt + 2 * page + 11, page - 29, enabled, 0x83);
    if (enabled) CHECK(virt[31] == a->guest[7 * page + 31] && virt[2 * page + 11] == b->guest[2 * page + 11], "virtual byte offsets select exact physical owners");
    CHECK(vk_wb_test_alias_map(virt, page, b->guest, 8 * page, 3 * page), "last-writer virtual overlap replaces old aliases");
    span(virtual_id, virt, 5 * page, enabled ? page : 0, enabled);
    span(virtual_id, virt + page + 17, 4 * page - 17, enabled ? 3 * page - 17 : 0, enabled);
    span_write(virtual_id, virt + page + 17, page - 31, enabled, 0xc5);
    if (enabled) CHECK(!memcmp(virt + page + 17, b->guest + 8 * page + 17, page - 31), "overlap resolver matches actual last MAP_FIXED owner offset");
    uint8_t old = b->guest[9 * page];
    CHECK(vk_wb_test_alias_unmap(virt, 2 * page, page), "partial virtual unmap installs real fresh anonymous backing");
    span(virtual_id, virt + 2 * page + 5, 3 * page - 5, enabled ? page - 5 : 0, 0);
    CHECK(virt[2 * page] == 0 && b->guest[9 * page] == old, "unmap zeroes only fresh virtual page, never physical donor");
    CHECK(vk_wb_test_alias_map(virt, 2 * page, a->guest, 12 * page, page), "partial remap restores different owner without stale alias");
    span_write(virtual_id, virt + 2 * page + 9, page - 18, enabled, 0x37);
    if (enabled) CHECK(!memcmp(virt + 2 * page + 9, a->guest + 12 * page + 9, page - 18), "remapped virtual page follows newest owner");
    CHECK(!vk_wb_test_alias_map(virt, 0, a->guest, a->bytes, page), "out-of-bounds donor offset rejected");
    CHECK(!vk_wb_test_alias_view(view + page, page, b->guest, b->bytes), "out-of-bounds view rejected");
    (void)aid; (void)bid;
    vk_wb_test_alias_remove(view); vk_wb_test_alias_remove(virt);
    required(!munmap(view, 3 * page) && !munmap(virt, 12 * page), "release virtual/view guest mappings");
    puts("BACKEND_OFFSETS PASS KOWN KVIEW KVIRT byte-offset cross-owner last-writer unmap remap");
}
typedef struct { uint8_t *address; uint32_t value; _Atomic int done; int result; } completion;
static void *completion_run(void *argument) {
    completion *c = argument;
    c->result = vk_wb_test_alias_store(c->address, &c->value, sizeof c->value, 1, 0, 1);
    atomic_store_explicit(&c->done, 1, memory_order_release); return NULL;
}
static void site2(backing *b, uint64_t identity, int enabled) {
    nvn_dirty_add_range(b->guest, b->bytes);
    void *watch = nvn_dirty_wb_watch(b->guest, b->bytes, nvn_dirty_now()); CHECK(watch != NULL, "Site2 watch arms");
    uint64_t stamp = nvn_dirty_now(), calls = atomic_load(&g_mp_calls); uint32_t value = 0x194ac325, got = 0;
    CHECK(vk_wb_test_alias_store(b->guest + 128, &value, sizeof value, 1, 0, 1), "production Site2 selector stores without lock contention");
    memcpy(&got, b->guest + 128, sizeof got); CHECK(got == value, "Site2 exact report value");
    CHECK(enabled ? atomic_load(&g_mp_calls) == calls : atomic_load(&g_mp_calls) > calls, "uncontended alias has no guest mprotect; legacy exercises production permission calls");
    required(vk_wb_pool_lock(identity, b->guest, b->bytes), "hold registry for same-thread Site2 contention");
    calls = atomic_load(&g_mp_calls); value++;
    CHECK(vk_wb_test_alias_store(b->guest + 128, &value, sizeof value, 1, 1, 1), "same-thread held registry uses nonblocking legacy add32 fallback");
    CHECK(atomic_load(&g_mp_calls) > calls, "same-thread contention reaches actual legacy dirty store");
    completion c = {.address = b->guest + 256, .value = 0x97a45621}; pthread_t worker;
    calls = atomic_load(&g_mp_calls); required(!pthread_create(&worker, NULL, completion_run, &c), "create contended Site2 worker");
    wait_for(&c.done, "Site2 must finish while another thread retains registry");
    CHECK(c.result && atomic_load(&g_mp_calls) > calls, "second-thread fallback completes via real legacy mprotect before registry unlock");
    vk_wb_pool_unlock(); required(!pthread_join(worker, NULL), "join Site2 worker");
    memcpy(&got, b->guest + 256, sizeof got); CHECK(got == c.value, "contended Site2 exact contents");
    CHECK(!nvn_dirty_since(b->guest, b->bytes, stamp) && !nvn_dirty_wb_written(watch), "all Site2 variants retain GPU ownership and unchanged CPU generation");
    nvn_dirty_wb_unwatch(watch); nvn_dirty_remove_range(b->guest);
    puts("SITE2 PASS uncontended same-thread-trylock second-thread-trylock no-deadlock exact-report generations");
}
typedef struct { backing *b; _Atomic int checked, done; int unexpectedly_acquired; } removal;
static void *remove_run(void *argument) {
    removal *r = argument; r->unexpectedly_acquired = vk_wb_test_alias_registry_trylock();
    atomic_store_explicit(&r->checked, 1, memory_order_release);
    vk_wb_test_alias_remove(r->b->guest); atomic_store_explicit(&r->done, 1, memory_order_release); return NULL;
}
static void pinned_lifetime(backing *b, uint64_t identity, int enabled) {
    nvn_dirty_add_range(b->guest, b->bytes); nvn_dirty_protect_exact(b->guest, b->bytes);
    required(vk_wb_pool_lock(identity, b->guest, b->bytes), "pin production registry for landing lifetime");
    size_t available; uint8_t *alias = vk_wb_alias_span_locked((uintptr_t)b->guest, b->bytes, &available);
    void *token = enabled ? nvn_dirty_gpu_write_alias_begin(b->guest, b->bytes) : nvn_dirty_gpu_write_begin(b->guest, b->bytes);
    CHECK(token != NULL, "pinned landing token owns guest pages");
    removal r = {.b = b}; pthread_t thread; required(!pthread_create(&thread, NULL, remove_run, &r), "create real remove worker");
    wait_for(&r.checked, "remove worker actually attempts production registry lock");
    CHECK(!r.unexpectedly_acquired && !atomic_load(&r.done), "pool lock excludes actual concurrent registry admission and teardown");
    CHECK(!enabled || (alias && available == b->bytes), "private backing remains usable while remove is blocked");
    memset(enabled ? alias : b->guest, 0x8e, b->bytes);
    nvn_dirty_gpu_write_end(token); vk_wb_pool_unlock();
    required(!pthread_join(thread, NULL), "join remove after landing unlock");
    CHECK(atomic_load(&r.done), "real pool removal completes after pinned landing releases lifetime");
    int equal = 1; for (size_t i = 0; i < b->bytes; ++i) equal &= b->guest[i] == 0x8e;
    CHECK(equal, "real owner teardown preserves every landed byte in restored guest backing");
    CHECK(!vk_wb_pool_lock(identity, b->guest, b->bytes), "retired registration identity cannot authorize stale alias");
    nvn_dirty_remove_range(b->guest); puts("LIFETIME PASS pinned-token removal-excluded release-remove preserved-contents stale-identity-rejected");
}
int main(int argc, char **argv) {
    int gem = argc > 1 && !strcmp(argv[1], "i915"); const char *option = getenv("TOTK_WB_ALIAS"); int enabled = option && !strcmp(option, "1");
    CHECK(!vk_wb_alias_enabled(), "backend getter does not lazily initialize switch"); vk_wb_test_alias_init();
    CHECK(vk_wb_alias_enabled() == enabled, "backend exact startup environment switch");
    required(!setenv("TOTK_WB_ALIAS", enabled ? "0" : "1", 1), "change post-init environment");
    CHECK(vk_wb_alias_enabled() == enabled, "backend switch frozen after initialization");
    size_t page = (size_t)getpagesize(); backing a, b; int result = backing_open(&a, 32 * page, gem, 0);
    if (result) { backing_close(&a); return result; }
    result = backing_open(&b, 32 * page, gem, 0); if (result) { backing_close(&a); backing_close(&b); return result; }
    memset(a.guest, 0x21, a.bytes); memset(b.guest, 0x43, b.bytes);
    uint64_t initial_rows = guest_rows(&a);
    uint64_t aid = vk_wb_test_alias_owner(a.guest, a.bytes, a.fd, a.offset), bid = vk_wb_test_alias_owner(b.guest, b.bytes, b.fd, b.offset);
    CHECK(aid && bid && aid != bid, "real-fd synthetic owners have distinct registration identities");
    CHECK(guest_rows(&a) == initial_rows, "retaining backend-private alias never creates a guest mapping row");
    uint64_t memory[2]; vk_wb_test_alias_memory(memory); CHECK(memory[0] == (enabled ? 2u : 0u) && memory[1] == (enabled ? a.bytes + b.bytes : 0), "private alias mapping and byte accounting is exact");
    owner_view_virtual(&a, &b, aid, bid, enabled);
    void *alt = reserve(128 * 1024); stack_t stack = {.ss_sp = alt, .ss_size = 128 * 1024}; required(!sigaltstack(&stack, NULL), "backend alternate stack");
    struct sigaction action = {.sa_sigaction = handle_fault, .sa_flags = SA_SIGINFO | SA_ONSTACK}; sigemptyset(&action.sa_mask); required(!sigaction(SIGSEGV, &action, NULL), "backend fault hook");
    nvn_dirty_lag_enable(); site2(&a, aid, enabled); pinned_lifetime(&a, aid, enabled);
    vk_wb_test_alias_memory(memory); CHECK(memory[0] == (enabled ? 1u : 0u), "one owner's remove retires precisely its private mapping");
    vk_wb_test_alias_remove(b.guest); vk_wb_test_alias_memory(memory); CHECK(!memory[0] && !memory[1], "all private mappings and bytes retire with owners");
    backing_close(&a); backing_close(&b); stack.ss_flags = SS_DISABLE; required(!sigaltstack(&stack, NULL), "disable backend altstack"); required(!munmap(alt, 128 * 1024), "release backend altstack");
    printf("WB_ALIAS_BACKEND %s alias=%d checks=%u failures=%u\n", failures ? "FAIL" : "PASS", enabled, checks, failures); return failures != 0;
}
