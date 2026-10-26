#include "framegen_slots.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
    const uint64_t r = 16666667, s = 1000000000;
    assert(framegen_midpoint(s,r,2,s-1000)==s+r);
    assert(framegen_midpoint(s,r,2,s)==s+r);
    assert(!framegen_midpoint(s,r,2,s+1));
    assert(!framegen_midpoint(s,r,1,s-1));
    assert(!framegen_midpoint(s,8333333,4,s-1));
    assert(!framegen_midpoint(s,11111111,2,s-1));
    assert(!framegen_midpoint(0,r,2,0));
    assert(!framegen_midpoint(INT64_MAX-r+1,r,2,1));
    assert(framegen_child_ready(s-1,s,s+r,s+1));
    assert(framegen_child_ready(s,s,s+r,s+r-1));
    assert(!framegen_child_ready(s+1,s,s+r,s+2));
    assert(!framegen_child_ready(0,s,s+r,s+2));
    assert(!framegen_child_ready(s,s,s+r,s+r));
    puts("framegen slot/deadline fixtures: 13 passed");
    return 0;
}
