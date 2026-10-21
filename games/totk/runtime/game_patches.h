#pragma once
#include "runtime.h"

/* TOTK version-specific game-code patches, applied to the loaded main module after relocation and before its text is sealed.
   Every site is checked against the original instruction word first; a mismatch stops the boot. */
void runtime_game_patches(Module *main_module);

/* v143 native60 only: floor physical-display pacing while a demonstrated active
 * scripted-camera action has refreshed the shared guest/presenter stamp within
 * 200 ms. Generic entry markers are diagnostic only; completion clears the
 * stamp. This covers a known camera-action subset, not every cinematic.
 * No rewritten guest time, locks or waits; other versions omit this optional
 * symbol, which shared NVN code imports weakly. */
unsigned runtime_cutscene_present_ticks(unsigned ticks, uint64_t refresh_ns);
