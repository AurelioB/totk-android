/* Instrumented-PGO profile control; only PGO=gen builds (android/Makefile) define TOTK_PGO_GEN. */
#pragma once
#include <stdint.h>
#ifdef TOTK_PGO_GEN
/* Called once per presented frame: resets counters at TOTK_PGO_RESET_FRAME, then rewrites TOTK_PGO_FILE every
   TOTK_PGO_DUMP_EVERY frames. */
void totk_pgo_frame(uint64_t frame);
#else
static inline void totk_pgo_frame(uint64_t frame) { (void)frame; }
#endif
