/* Instrumented-PGO profile control (PGO=gen builds). The app is killed rather than exiting, so the LLVM profile
   runtime's atexit writer never runs: counters are reset once gameplay starts (TOTK_PGO_RESET_FRAME, default 1800,
   which excludes boot/loading) and the cumulative profile is rewritten to TOTK_PGO_FILE every TOTK_PGO_DUMP_EVERY
   frames after that (default 1200), so the last write before the process is killed covers nearly the whole run.
   Counter updates are racy across threads; that only perturbs counts, never the program. */
#include "pgo_dump.h"
#ifdef TOTK_PGO_GEN
#include <stdio.h>
#include <stdlib.h>

int __llvm_profile_write_file(void);
void __llvm_profile_reset_counters(void);
void __llvm_profile_set_filename(const char *name);

static uint64_t env_frame(const char *name, uint64_t fallback) {
    const char *e = getenv(name);
    return e && *e ? strtoull(e, NULL, 10) : fallback;
}

void totk_pgo_frame(uint64_t frame) {
    static uint64_t reset_at, every;
    if (!reset_at) { reset_at = env_frame("TOTK_PGO_RESET_FRAME", 1800); every = env_frame("TOTK_PGO_DUMP_EVERY", 1200); }
    if (frame == reset_at) {
        __llvm_profile_reset_counters();
        fprintf(stderr, "[pgo] counters reset at frame %llu\n", (unsigned long long)frame);
    }
    if (every && frame > reset_at && (frame - reset_at) % every == 0) {
        const char *file = getenv("TOTK_PGO_FILE");
        if (!file || !*file) { fprintf(stderr, "[pgo] TOTK_PGO_FILE unset; profile not written\n"); return; }
        __llvm_profile_set_filename(file);
        int r = __llvm_profile_write_file();
        fprintf(stderr, "[pgo] profile %s at frame %llu (counters from frame %llu): %s\n", file,
                (unsigned long long)frame, (unsigned long long)reset_at, r ? "WRITE FAILED" : "written");
    }
}
#endif
