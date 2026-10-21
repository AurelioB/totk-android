#pragma once
#include "runtime.h"
#include <stdatomic.h>

/* Test-profile-only control, executed on the game's frame-end thread after its GPU fence. */
extern _Atomic int runtime_survey_pending;
extern const int32_t runtime_survey_sync[2];
extern const int32_t runtime_frame_wait_request;
extern const int runtime_frame_diag_pre14;
void runtime_survey_init(Module *main_module);
int runtime_survey_command(const char *line);
void runtime_survey_tick(uint64_t frame);
void runtime_survey_cancel_user(void);
/* The dual-screen companion's item actions (Android app, companion_jni.c) run at the same frame end: hle_nvn.c calls
   runtime_companion_tick there while runtime_companion_pending is set. Defined here, for every build that has the NVN
   layer; only the app sets them. */
extern _Atomic int runtime_companion_pending;
extern void (*_Atomic runtime_companion_tick)(uint64_t frame);
