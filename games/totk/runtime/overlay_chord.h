/* In-game shortcut Minus + R (performance overlay), as a pure state machine for frontend_android.c and the host
   test (tests/test_overlay_chord.py). Minus is held back for up to OVERLAY_CHORD_WINDOW_NS: R going down in that
   window makes a chord (neither button reaches the game); otherwise Minus is delivered late, or at once when it is
   released or another button goes down first, so a lone Minus (the map) still works, ~150 ms later at most.
   A tap released inside the window is replayed with its own length (press at the release, release as long after),
   so the game sees the same press duration it would have without the shortcut rather than a press that is already
   over by its next once-per-frame read. R first, then Minus, is not a chord: R has already reached the game by then.
   Times are CLOCK_MONOTONIC ns (Android key event times use the same clock). The caller serialises calls. */
#pragma once
#include <stdint.h>

#define OVERLAY_CHORD_WINDOW_NS UINT64_C(150000000)
enum { OVERLAY_CHORD_MINUS, OVERLAY_CHORD_R, OVERLAY_CHORD_OTHER };
/* CANCEL: Android's synthesized release (AKEY_EVENT_FLAG_CANCELED) when the window loses focus or the controller goes
   away while the key is down. It must not turn a held-back Minus into a tap the user never finished. */
enum { OVERLAY_CHORD_RELEASE, OVERLAY_CHORD_PRESS, OVERLAY_CHORD_REPEAT, OVERLAY_CHORD_CANCEL };   /* repeats: repeatCount > 0 */
/* Result bits, applied in this order: release the replayed Minus, press the held Minus, then this event as usual. */
enum { OVERLAY_CHORD_MINUS_UP = 1, OVERLAY_CHORD_MINUS_DOWN = 2, OVERLAY_CHORD_PASS = 4, OVERLAY_CHORD_CYCLE = 8 };
/* minus: IDLE, HELD_BACK (pressed, not yet delivered), DELIVERED (the game has it down), TAP (released by the user,
   still down for the game until release_ns), SWALLOWED (part of a chord). r: IDLE or SWALLOWED. */
enum { CHORD_IDLE, CHORD_HELD_BACK, CHORD_DELIVERED, CHORD_TAP, CHORD_SWALLOWED };
typedef struct {
    unsigned minus, r;
    uint64_t pressed_ns, deadline_ns, release_ns;
} overlay_chord;

/* Time-driven steps: a held-back Minus whose window has passed becomes a normal press; a replayed tap ends.
   Call on every event and every pad read. */
static inline unsigned overlay_chord_poll(overlay_chord *c, uint64_t now_ns) {
    if (c->minus == CHORD_HELD_BACK && now_ns >= c->deadline_ns) { c->minus = CHORD_DELIVERED; return OVERLAY_CHORD_MINUS_DOWN; }
    if (c->minus == CHORD_TAP && now_ns >= c->release_ns) { c->minus = CHORD_IDLE; return OVERLAY_CHORD_MINUS_UP; }
    return 0;
}
static inline unsigned overlay_chord_key(overlay_chord *c, unsigned button, unsigned action, uint64_t now_ns) {
    unsigned out = overlay_chord_poll(c, now_ns);
    if (action == OVERLAY_CHORD_CANCEL) {
        if (button == OVERLAY_CHORD_MINUS) {
            unsigned was = c->minus; c->minus = CHORD_IDLE;
            /* held back: nothing reached the game; replaying: end it; delivered: release it; chord: nothing. */
            return out | (was == CHORD_TAP ? OVERLAY_CHORD_MINUS_UP : was == CHORD_DELIVERED ? OVERLAY_CHORD_PASS : 0);
        }
        if (button == OVERLAY_CHORD_R && c->r == CHORD_SWALLOWED) { c->r = CHORD_IDLE; return out; }
        return out | OVERLAY_CHORD_PASS;
    }
    if (button == OVERLAY_CHORD_MINUS) {
        if (action == OVERLAY_CHORD_PRESS && (c->minus == CHORD_IDLE || c->minus == CHORD_TAP)) {
            if (c->minus == CHORD_TAP) out |= OVERLAY_CHORD_MINUS_UP;   /* end the replay first: a new press follows */
            c->minus = CHORD_HELD_BACK; c->pressed_ns = now_ns; c->deadline_ns = now_ns + OVERLAY_CHORD_WINDOW_NS;
            return out;
        }
        if (action != OVERLAY_CHORD_RELEASE)   /* a repeat, or a press while already down: withheld while we own it */
            return c->minus == CHORD_HELD_BACK || c->minus == CHORD_TAP || c->minus == CHORD_SWALLOWED ? out : out | OVERLAY_CHORD_PASS;
        unsigned was = c->minus;
        if (was == CHORD_HELD_BACK) {   /* a quick tap: press now, release after as long as it was held */
            c->minus = CHORD_TAP; c->release_ns = now_ns + (now_ns - c->pressed_ns);
            return out | OVERLAY_CHORD_MINUS_DOWN;
        }
        if (was == CHORD_TAP) return out;   /* a stray release; the replay ends on its own */
        c->minus = CHORD_IDLE;
        return was == CHORD_SWALLOWED ? out : out | OVERLAY_CHORD_PASS;
    }
    if (button == OVERLAY_CHORD_R) {
        if (action == OVERLAY_CHORD_RELEASE) {
            if (c->r == CHORD_SWALLOWED) { c->r = CHORD_IDLE; return out; }
            return out | OVERLAY_CHORD_PASS;
        }
        if (c->r == CHORD_SWALLOWED) return out;
        /* Minus still held back, or held from an earlier chord: each new R press advances the overlay once.
           A repeat of an R the game already has is never a chord. */
        if (action == OVERLAY_CHORD_PRESS && (c->minus == CHORD_HELD_BACK || c->minus == CHORD_SWALLOWED)) {
            c->minus = c->r = CHORD_SWALLOWED;
            return out | OVERLAY_CHORD_CYCLE;
        }
        return out | OVERLAY_CHORD_PASS;
    }
    /* Another button pressed first: this is not the shortcut, keep the game's press order. */
    if (action == OVERLAY_CHORD_PRESS && c->minus == CHORD_HELD_BACK) { c->minus = CHORD_DELIVERED; out |= OVERLAY_CHORD_MINUS_DOWN; }
    return out | OVERLAY_CHORD_PASS;
}
