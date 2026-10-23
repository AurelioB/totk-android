/* Pure production-policy regression. No clocks, Vulkan, Android or game files.
   cc -std=c11 -Wall -Wextra -Werror tests/test_pipeline_scan_budget.c -o /tmp/test_pipeline_scan_budget */
#include "../games/totk/third_party/d2r_gfx/gfx/nvn/nvn_scan_budget.h"
#include <assert.h>

int main(void) {
    nvn_scan_budget b = {0};
    const uint64_t fixed = 4000000;
    assert(nvn_scan_budget_left(&b, 1, fixed, 1));
    nvn_scan_budget_spend(&b, 1, fixed);
    assert(!nvn_scan_budget_left(&b, 1, fixed, 1));
    assert(!b.sync_ns);   /* a scan cannot pay for the next scan */
    nvn_scan_budget_build(&b, 1, 23000000);
    assert(nvn_scan_budget_left(&b, 1, fixed, 1));
    assert(!nvn_scan_budget_left(&b, 1, fixed, 0));
    nvn_scan_budget_spend(&b, 1, 19000000);
    assert(!nvn_scan_budget_left(&b, 1, fixed, 1));   /* max(fixed, sync), not their sum */
    nvn_scan_budget_spend(&b, 1, 100000000);
    assert(b.sync_ns == 23000000 && !nvn_scan_budget_left(&b, 1, fixed, 1));
    nvn_scan_budget_build(&b, 1, 101000000);
    assert(nvn_scan_budget_left(&b, 1, fixed, 1));   /* only a later real build replenishes credit */

    assert(nvn_scan_budget_left(&b, 2, fixed, 1));
    assert(!b.used_ns && !b.sync_ns);
    assert(!nvn_scan_budget_left(&b, 3, 0, 1));
    nvn_scan_budget_build(&b, 3, 5);
    assert(nvn_scan_budget_left(&b, 3, 0, 1));
    nvn_scan_budget_spend(&b, 3, 5);
    assert(!nvn_scan_budget_left(&b, 3, 0, 1));
    nvn_scan_budget_build(&b, 4, 7);   /* build-first frame reset */
    assert(!b.used_ns && b.sync_ns == 7 && b.frame == 4);
    nvn_scan_budget_spend(&b, 5, 9);   /* scan-first frame reset */
    assert(b.used_ns == 9 && !b.sync_ns && b.frame == 5);

    b.sync_ns = UINT64_MAX - 1;
    nvn_scan_budget_build(&b, 5, 2);
    assert(b.sync_ns == UINT64_MAX);
    b.used_ns = UINT64_MAX - 1;
    nvn_scan_budget_spend(&b, 5, UINT64_MAX);
    assert(b.used_ns == UINT64_MAX && !nvn_scan_budget_left(&b, 5, UINT64_MAX, 1));
    nvn_scan_budget_build(&b, 5, UINT64_MAX);
    assert(b.sync_ns == UINT64_MAX && !nvn_scan_budget_left(&b, 5, 0, 1));
    assert(nvn_scan_budget_left(&b, 0, 1, 0));   /* wrap/reset in frame identity also clears credit */
    assert(!b.used_ns && !b.sync_ns);
    return 0;
}
