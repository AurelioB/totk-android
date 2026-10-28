/* Host-only tests for the production Adreno 7xx sysmem policy; no GPU or game required. */
#include "../../runtime/adreno_gmem.h"

#include <stdio.h>
#include <string.h>

#define A722_CHIP_ID UINT32_C(0x43020100)
#define OUTPUT_SENTINEL UINT32_C(0xa5a5a5a5)

static unsigned checks;
static unsigned failures;

#define CHECK(condition, message) do { \
    checks++; \
    if (!(condition)) { \
        failures++; \
        fprintf(stderr, "FAIL: %s\n", message); \
    } \
} while (0)

struct mock_probe {
    unsigned calls;
    bool succeeds;
    uint32_t chip_id;
};

static bool probe_chip(void *context, uint32_t *chip_id) {
    struct mock_probe *mock = context;
    mock->calls++;
    if (!mock->succeeds)
        return false;
    *chip_id = mock->chip_id;
    return true;
}

static void matching_chip_without_override(void) {
    struct mock_probe mock = {0, true, A722_CHIP_ID};
    uint32_t chip_id = OUTPUT_SENTINEL;

    CHECK(totk_a722_sysmem_default(true, NULL, probe_chip, &mock, &chip_id),
          "exact Adreno 722 match enables the default without TU_DEBUG");
    CHECK(mock.calls == 1, "eligible default probes exactly once");
    CHECK(chip_id == A722_CHIP_ID, "matching GPU id is returned to the caller");
}

static void failed_probe(void) {
    struct mock_probe mock = {0, false, A722_CHIP_ID};
    uint32_t chip_id = OUTPUT_SENTINEL;

    CHECK(!totk_a722_sysmem_default(true, NULL, probe_chip, &mock, &chip_id),
          "failed chip probe does not enable the default");
    CHECK(mock.calls == 1, "failed eligible probe is called once");
    CHECK(chip_id == OUTPUT_SENTINEL, "failed probe leaves output unchanged");
}

static void nonmatching_chips(void) {
    static const uint32_t chip_ids[] = {
        UINT32_C(0x430200ff), UINT32_C(0x43020101), UINT32_C(0x43020110), UINT32_C(0x00000000)
    };

    for (size_t i = 0; i < sizeof chip_ids / sizeof chip_ids[0]; i++) {
        struct mock_probe mock = {0, true, chip_ids[i]};
        uint32_t chip_id = OUTPUT_SENTINEL;
        CHECK(!totk_a722_sysmem_default(true, NULL, probe_chip, &mock, &chip_id),
              "nonmatching GPU id does not enable the default");
        CHECK(mock.calls == 1, "nonmatching eligible probe is called once");
        CHECK(chip_id == OUTPUT_SENTINEL, "nonmatching GPU leaves output unchanged");
    }
}

static void explicit_overrides(void) {
    static const char *const overrides[] = {"", "0", "other", "sysmem"};

    for (size_t i = 0; i < sizeof overrides / sizeof overrides[0]; i++) {
        struct mock_probe mock = {0, true, A722_CHIP_ID};
        uint32_t chip_id = OUTPUT_SENTINEL;
        CHECK(!totk_a722_sysmem_default(true, overrides[i], probe_chip, &mock, &chip_id),
              "any existing TU_DEBUG value suppresses the default");
        CHECK(mock.calls == 0, "explicit TU_DEBUG avoids the chip probe");
        CHECK(chip_id == OUTPUT_SENTINEL, "explicit override leaves output unchanged");
    }
}

static void system_driver(void) {
    struct mock_probe mock = {0, true, A722_CHIP_ID};
    uint32_t chip_id = OUTPUT_SENTINEL;

    CHECK(!totk_a722_sysmem_default(false, NULL, probe_chip, &mock, &chip_id),
          "system driver does not enable the custom-driver default");
    CHECK(mock.calls == 0, "system driver path avoids the chip probe");
    CHECK(chip_id == OUTPUT_SENTINEL, "system driver path leaves output unchanged");
}

static void explicit_sysmem_is_not_redefaulted(void) {
    struct mock_probe mock = {0, true, A722_CHIP_ID};
    uint32_t chip_id = OUTPUT_SENTINEL;

    CHECK(totk_a722_sysmem_default(true, NULL, probe_chip, &mock, &chip_id),
          "first eligible decision enables the default");
    CHECK(mock.calls == 1, "first decision probes once");
    CHECK(chip_id == A722_CHIP_ID, "first decision reports the matched GPU");

    chip_id = OUTPUT_SENTINEL;
    CHECK(!totk_a722_sysmem_default(true, "sysmem", probe_chip, &mock, &chip_id),
          "subsequent explicit sysmem setting wins without a new default");
    CHECK(mock.calls == 1, "explicit sysmem decision does not probe again");
    CHECK(chip_id == OUTPUT_SENTINEL, "explicit sysmem decision leaves output unchanged");
}

int main(void) {
    matching_chip_without_override();
    failed_probe();
    nonmatching_chips();
    explicit_overrides();
    system_driver();
    explicit_sysmem_is_not_redefaulted();

    if (failures) {
        fprintf(stderr, "%u/%u GMEM policy checks failed\n", failures, checks);
        return 1;
    }
    printf("%u GMEM policy checks passed\n", checks);
    return 0;
}
