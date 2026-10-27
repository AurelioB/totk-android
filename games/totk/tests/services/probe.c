/* Exercise the actual service implementation without loading a game or reserving its address space. */
#include "../../runtime/services.c"

_Noreturn void runtime_fail(const char *message) {
    fprintf(stderr, "probe: %s\n", message);
    exit(2);
}

static uint32_t allocation_result;
static uint32_t guest_region(uint64_t *out, uint64_t size) {
    (void)size; *out = 0x12340000;
    return allocation_result;
}
static uint32_t guest_pages(uint64_t address, uint64_t size) {
    (void)address; (void)size;
    return allocation_result;
}
void *runtime_symbol(const char *name) {
    if (!strcmp(name, "_ZN2nn2os21AllocateAddressRegionEPmm")) return guest_region;
    if (!strcmp(name, "_ZN2nn2os19AllocateMemoryPagesEmm")) return guest_pages;
    runtime_fail("unexpected symbol");
}

int main(int argc, char **argv) {
    if (argc > 1) {
        allocation_result = (uint32_t)strtoul(argv[1], NULL, 0);
        uint64_t out = 0;
        if (observed_region(&out, 4096) != allocation_result ||
            observed_pages(0x12340000, 4096) != allocation_result || out != 0x12340000)
            return 1;
        return 0;
    }
    uint64_t language = desired_language();
    char code[9] = {0};
    memcpy(code, &language, 8);
    puts(code);
    return 0;
}
