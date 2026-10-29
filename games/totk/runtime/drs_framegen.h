#pragma once
#include <stdint.h>

/* Backend-only host fixtures and older game versions have no guest patch/data
 * page. The pure policy remains exercised without inventing a guest module. */
#if defined(TOTK_GAME_V143) && !defined(TOTK_VK_TESTS)
void runtime_drs_framegen_budget(double milliseconds);
uint64_t runtime_drs_framegen_level(void); /* scale float low32, signed native priority high32; zero=not sampled */
#else
static inline void runtime_drs_framegen_budget(double ms) { (void)ms; }
static inline uint64_t runtime_drs_framegen_level(void) { return 0; }
#endif
