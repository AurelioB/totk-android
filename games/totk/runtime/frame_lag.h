/* Startup policy shared by guest waits, protected capture and diagnostic routing.
 * Call after totk.env is loaded; callers cache immutable startup configuration. */
#pragma once
#include <stdlib.h>
#include <string.h>

static inline int runtime_frame_lag_requested(void) {
    const char *lag = getenv("TOTK_FRAME_LAG");
    return lag && !strcmp(lag, "1");
}
