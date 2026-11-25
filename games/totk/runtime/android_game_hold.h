/* UI-thread-only cache lifecycle policy. The ordinary periodic saver and exit
 * flush are deliberately outside this policy. Java supplies the bounded grace
 * timer; generations reject a timer from an earlier pause or Settings binding. */
#ifndef TOTK_ANDROID_GAME_HOLD_H
#define TOTK_ANDROID_GAME_HOLD_H
#include <stdint.h>
#include <string.h>

typedef struct {
    int enabled, paused, settings_active, pending;
    uint64_t generation;
} AndroidGameHold;

typedef enum {
    ANDROID_GAME_HOLD_NONE, ANDROID_GAME_HOLD_FOREGROUND,
    ANDROID_GAME_HOLD_GRACE, ANDROID_GAME_HOLD_DEFER
} AndroidGameHoldEffect;

static inline int android_game_hold_enabled(const char *value) {
    return value && !strcmp(value, "1");
}
static inline AndroidGameHoldEffect android_game_hold_pause(AndroidGameHold *hold, int paused) {
    if (hold->paused == paused) return ANDROID_GAME_HOLD_NONE;
    hold->paused = paused;
    hold->generation++;
    hold->pending = paused;
    if (!paused) return ANDROID_GAME_HOLD_FOREGROUND;
    return hold->settings_active ? ANDROID_GAME_HOLD_DEFER : ANDROID_GAME_HOLD_GRACE;
}
static inline AndroidGameHoldEffect android_game_hold_settings(AndroidGameHold *hold, int active) {
    if (hold->settings_active == active) return ANDROID_GAME_HOLD_NONE;
    hold->settings_active = active;
    hold->generation++;
    if (!hold->paused || !hold->pending) return ANDROID_GAME_HOLD_NONE;
    return active ? ANDROID_GAME_HOLD_DEFER : ANDROID_GAME_HOLD_GRACE;
}
static inline uint64_t android_game_hold_pending(const AndroidGameHold *hold) {
    return hold->enabled && hold->paused && hold->pending && !hold->settings_active ? hold->generation : 0;
}
static inline int android_game_hold_flush(AndroidGameHold *hold, uint64_t generation) {
    if (!generation || android_game_hold_pending(hold) != generation) return 0;
    hold->pending = 0;
    return 1;
}
#endif
