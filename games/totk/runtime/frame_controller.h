#pragma once
#include <stdint.h>

/* v143 main+0x2daa48 passes x0=sys+0x200, x1=sys to +0x2db174.
 * The controller's saved x20 at guest FP+0x30 is its CALLER's x20,
 * not its live controller pointer; in the normal caller it is sys itself. */
static inline uint64_t frame_controller_v143_address(uint64_t sys) {
    return sys ? sys + 0x200 : 0;
}
